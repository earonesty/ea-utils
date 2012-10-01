/*
# Copyright (c) 2011 Erik Aronesty
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#
# ALSO, IT WOULD BE NICE IF YOU LET ME KNOW YOU USED IT.
*/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <sys/stat.h>

#include <string>
#include <google/sparse_hash_map> // or sparse_hash_set, dense_hash_map, ...
#include <google/dense_hash_map>  // or sparse_hash_set, dense_hash_map, ...

#include <samtools/sam.h>         // samtools api

#include "fastq-lib.h"

const char * VERSION = "1.32";

using namespace std;

void usage(FILE *f);

#define MAX_MAPQ 300
// this factor is based on a quick empirical look at a few bam files....
#define VFACTOR 1.5

//#define max(a,b) (a>b?a:b)
//#define min(a,b) (a<b?a:b)
#define meminit(l) (memset(&l,0,sizeof(l)))
#define debugout(s,...) if (debug) fprintf(stderr,s,##__VA_ARGS__)
#undef warn
#define warn(s,...) ((++errs), fprintf(stderr,s,##__VA_ARGS__))
#define stdev(cnt, sum, ssq) sqrt((((double)cnt)*ssq-pow((double)sum,2)) / ((double)cnt*((double)cnt-1)))

template <class vtype> 
    double quantile(const vtype &vec, double p);

template <class itype> 
    double quantile(const vector<itype> &vec, double p);

std::string string_format(const std::string &fmt, ...);

int debug=0;
int errs=0;
extern int optind;
int histnum=30;
bool isbwa=false;
int rnamode = 0;

// from http://programerror.com/2009/10/iterative-calculation-of-lies-er-stats/
class cRunningStats
{
private:
  double m_n;  // count
  double m_m1; // mean
  double m_m2; // second moment
  double m_m3; // third moment
  double m_m4; // fourth moment
public:
  cRunningStats() : m_n(0.0), m_m1(0.0), m_m2(0.0), m_m3(0.0), m_m4(0.0)
    { ; }
  void Push(double x)
  {
    m_n++;
    double d = (x - m_m1);
    double d_n = d / m_n;
    double d_n2 = d_n * d_n;
    m_m4 += d * d_n2 * d_n * ((m_n - 1) * ((m_n * m_n) - 3 * m_n + 3)) +
            6 * d_n2 * m_m2 - 4 * d_n * m_m3;
    m_m3 += d * d_n2 * ((m_n - 1) * (m_n - 2)) - 3 * d_n * m_m2;
    m_m2 += d * d_n * (m_n - 1);
    m_m1 += d_n;
  }
  double Mean() { return m_m1; }
  double StdDeviation() { return sqrt(Variance()); }
  double StdError() { return (m_n > 1.0) ? sqrt(Variance() / m_n) : 0.0; }
  double Variance() { return (m_n > 1.0) ? (m_m2 / (m_n - 1.0)) : 0.0; }
  double Skewness() { return sqrt(m_n) * m_m3 / pow(m_m2, 1.5); }
  double Kurtosis() { return m_n * m_m4 / (m_m2 * m_m2); }
};

/// if we use this a lot may want to make it variable size
class scoverage {
public:
	scoverage() {mapb=reflen=0; dist.resize(histnum+2); mapr=0;};
	long long int mapb;
	long int mapr;
    cRunningStats spos;
	int reflen;
	vector <int> dist;
};

// sorted integer bucket ... good for ram with small max size, slow to access
class ibucket {
public:
	int tot;
	vector<int> dat;
	ibucket(int max) {dat.resize(max+1);tot=0;}
	int size() const {return tot;};

	int operator[] (int n) const {
		assert(n < size());
		int i; 
		for (i=0;i<dat.size();++i) {
			if (n < dat[i]) {
				return i;
			}
			n-=dat[i];
		}
	}

	void push(int v) {
		assert(v<dat.size());
		++dat[v];
		++tot;
	}
};

class sstats {
public:
	ibucket vmapq;			// all map qualities
	sstats() : vmapq(MAX_MAPQ) {
		memset((void*)&dat,0,sizeof(dat));
		covr.set_empty_key("-");
	}
	~sstats() {
		covr.clear();
	}
	struct {
		int n, mapn;		// # of entries, # of mapped entries, 
		int lenmin, lenmax; double lensum, lenssq;	// read length stats
		double mapsum, mapssq;	// map quality sum/ssq 
		double nmnz, nmsum;	// # of mismatched reads, sum of mismatch lengths 
		long long int nbase;
		int qualmax, qualmin;	// num bases samples, min/max qual 
		double qualsum, qualssq;	// sum quals, sum-squared qual
		int nrev, nfor;		// rev reads, for reads
		double tmapb;		// number of mapped bases
		long long int basecnt[5];
		int del, ins;		// length total dels/ins found
		bool pe;		// paired-end ? 0 or 1	
		int disc;
		int disc_pos;
		int dupmax;		// max dups found
	} dat;
	vector<int> visize;		// all insert sizes
	google::dense_hash_map<std::string, scoverage> covr;	// # mapped per ref seq
	google::sparse_hash_map<std::string, int> dups;		// alignments by read-id (not necessary for some pipes)

	// file-format neutral ... called per read... warning seq/qual are not necessarily null-terminated
	void dostats(string name, int rlen, int bits, const string &ref, int pos, int mapq, const string &materef, int nmate, const string &seq, const char *qual, int nm, int del, int ins);

	// read a bam/sam file and call dostats over and over
	bool parse_bam(const char *in);
	bool parse_sam(FILE *f);
};

#define T_A 0
#define T_C 1
#define T_G 2
#define T_T 3
#define T_N 4

void build_basemap();

int dupreads = 1000000;
int max_chr = 1000;
bool trackdup=0;
int basemap[256];
int main(int argc, char **argv) {
	const char *ext = NULL;
	bool multi=0, newonly=0, inbam=0;
    const char *rnafile = NULL;
	char c;
	optind = 0;
    while ( (c = getopt (argc, argv, "?BArR:Ddx:MhS:")) != -1) {
                switch (c) {
                case 'd': ++debug; break;
                case 'D': ++trackdup; break;
                case 'B': inbam=1; break;
                case 'A': max_chr=1000000; break;
                case 'R': rnafile=optarg;
                case 'r': max_chr=1000000; rnamode=1; histnum=60; break;
                case 'S': histnum=atoi(optarg); break;
                case 'x': ext=optarg; break;
                case 'M': newonly=1; break;
                case 'h': usage(stdout); return 0;
                case '?':
                     if (!optopt) {
                        usage(stdout); return 0;
                     } else if (optopt && strchr("ox", optopt))
                       fprintf (stderr, "Option -%c requires an argument.\n", optopt);
                     else if (isprint(optopt))
                       fprintf (stderr, "Unknown option `-%c'.\n", optopt);
                     else
                       fprintf (stderr, "Unknown option character `\\x%x'.\n", optopt);
                     usage(stderr);
                     return 1;
                }
    }

	// recompute argc owing to getopt
	const char *stdv[3] = {argv[0],"-",NULL}; 
	if (!argv[optind]) {
		argc=2;
		argv = (char **) stdv;
		optind=1;
	}

	multi = (argc-optind-1) > 0;

	if (multi && !ext) 
		ext = "stats";

	
	build_basemap();

	debugout("argc:%d, argv[1]:%s, multi:%d, ext:%s\n", argc,argv[optind],multi,ext);

	const char *p;
	// for each input file
	for (;optind < argc;++optind) {
		sstats s;
		const char *in = argv[optind];
		FILE *f;
		FILE *o=NULL;
		FILE *rnao=NULL;
		bool needpclose = 0;

		// decide input format
		if (!strcmp(in,"-")) {
			// read sam/bam from stdin
			if (ext) {
				warn("Can't use file extension with stdin\n");
				continue;
			}
			f = stdin;
			o = stdout;
		} else {
			string out;
			if ((p = strrchr(in,'.')) && !strcmp(p, ".gz")) {
				// maybe this is a gzipped sam file...
				string cmd = string_format("gunzip -c '%s'", in);
				f = popen(cmd.c_str(), "r");
				needpclose=1;
				if (f) {
					char c;
					if (!inbam) {
						// guess file format with 1 char
						c=getc(f); ungetc(c,f);
						if (c==-1) {
							warn("Can't unzip %s\n", in);
							pclose(f);
							continue;
						}
						if (c==31) {
							// bam file... reopen to reset stream... can't pass directly
							string cmd = string_format("gunzip -c '%s'", in);
							f = popen(cmd.c_str(), "r");
							inbam=1;
						}
					} else 
						c = 31;	// user forced bam, no need to check/reopen

					if (inbam) {
						// why did you gzip a bam... weird? 
						if (dup2(fileno(f),0) == -1) {
						      warn("Can't dup2 STDIN\n");
						      continue;
						}
						in = "-";
					}
				} else {
					warn("Can't unzip %s: %s\n", in, strerror(errno));
					continue;
				}
				// extension mode... output to file minus .gz
				if (ext) 
					out=string(in, p-in);
			} else {
	 			f = fopen(in, "r");
				if (!f) {
					warn("Can't open %s: %s\n", in, strerror(errno));
					continue;
				}
				// extension mode... output to file
				if (ext) 
					out=in;
			}
			if (ext) {
				( out += '.') += ext;
				o=fopen(out.c_str(), "w");
				if (!o) {
					warn("Can't write %s: %s\n", out.c_str(), strerror(errno));
					continue;
				}
			} else
				o=stdout;
		}

		// more guessing
		debugout("file:%s, f: %lx\n", in, (long int) f);
		char c;
		if (!inbam) {
			// guess file format
			c=getc(f); ungetc(c,f);
			if (c==31 && !strcmp(in,"-")) {
				// if bamtools api allowed me to pass a stream, this wouldn't be an issue....
				warn("Specify -B to read a bam file from standard input\n");
				continue;
			}
		} else 
			c = 31;		// 31 == bam


		// parse sam or bam as needed
		if (c != 31) {
			// (could be an uncompressed bam... but can't magic in 1 char)
			if (!s.parse_sam(f)) {
				if (needpclose) pclose(f); else fclose(f);
				warn("Invalid sam file %s\n", in);
				continue;
			}
		} else {
			if (!s.parse_bam(in)) {
				if (needpclose) pclose(f); else fclose(f);
				warn("Invalid bam file %s\n", in);
				continue;
			}
		}
		if (needpclose) pclose(f); else fclose(f);

		// sort sstats
		sort(s.visize.begin(), s.visize.end());

		int phred = s.dat.qualmin < 64 ? 33 : 64;
		if (!s.dat.n) {
			warn("No reads in %s\n", in);
			continue;
		}
		fprintf(o, "reads\t%d\n", s.dat.n);
		fprintf(o, "version\t%s\n", VERSION);

		// mapped reads is the number of reads that mapped at least once (either mated or not)
		if (s.dat.mapn > 0) {
			if (trackdup && s.dat.dupmax > (s.dat.pe+1)) {
				google::sparse_hash_map<string,int>::iterator it = s.dups.begin();
				vector<int> vtmp;
				int amb = 0;
				int sing = 0;
				while(it!=s.dups.end()) {
					// *not* making the distinction between 2 singleton mappings and 1 paired here
					if (it->second > (s.dat.pe+1)) {
						++amb;
					}
					if (it->second == 1 && s.dat.pe) {
						++sing;	
					}
					++it;
				}
				fprintf(o,"mapped reads\t%d\n", (int) s.dups.size()*(s.dat.pe+1)-sing);
				if (amb > 0) {
					fprintf(o,"ambiguous\t%d\n", amb*(s.dat.pe+1));
					fprintf(o,"pct ambiguous\t%.6f\n", 100.0*((double)amb/(double)s.dups.size()));
					fprintf(o,"max dup align\t%.d\n", s.dat.dupmax-s.dat.pe);
				}
				if (sing)
					fprintf(o,"singleton mappings\t%.d\n", sing);
				// number of total mappings
				fprintf(o, "total mappings\t%d\n", s.dat.mapn);
			} else {
				// dup-id's not tracked
				fprintf(o, "mapped reads\t%d\n", s.dat.mapn);
				// todo: add support for bwa's multiple alignment tag
				// fprintf(o, "total mappings\t%d\n", s.dat.mapn);
			}
		} else {
			fprintf(o, "mapped reads\t%d\n", s.dat.mapn);
		}

		fprintf(o, "mapped bases\t%.0f\n", s.dat.tmapb);
		if (s.dat.pe) {
			fprintf(o, "library\tpaired-end\n");
		}
		if (s.dat.disc > 0) {
			fprintf(o, "discordant mates\t%d\n", s.dat.disc);
		}
		if (s.dat.disc_pos > 0) {
			fprintf(o, "distant mates\t%d\n", s.dat.disc_pos);
		}

		if (s.dat.mapn > 0) {
            if (rnafile) {
                rnao=fopen(rnafile,"w");
            } else {
                rnao=o;
            }

            if (s.dat.mapn > 100) {
                // at least 100 mappings to call a meaningful "percentage" 
    			fprintf(o, "pct forward\t%.3f\n", 100*(s.dat.nfor/(double)(s.dat.nfor+s.dat.nrev)));
            }

			fprintf(o, "phred\t%d\n", phred);
			fprintf(o, "forward\t%d\n", s.dat.nfor);
			fprintf(o, "reverse\t%d\n", s.dat.nrev);
			if (s.dat.lenmax != s.dat.lenmin) {
				fprintf(o, "len max\t%d\n", s.dat.lenmax);	
				fprintf(o, "len mean\t%.4f\n", s.dat.lensum/s.dat.mapn);	
				fprintf(o, "len stdev\t%.4f\n", stdev(s.dat.mapn, s.dat.lensum, s.dat.lenssq));	
			} else {
				fprintf(o, "len max\t%d\n", s.dat.lenmax);	
			}
			fprintf(o, "mapq mean\t%.4f\n", s.dat.mapsum/s.dat.mapn);
			fprintf(o, "mapq stdev\t%.4f\n", stdev(s.dat.mapn, s.dat.mapsum, s.dat.mapssq));

			fprintf(o, "mapq Q1\t%.2f\n", quantile(s.vmapq,.25));
			fprintf(o, "mapq median\t%.2f\n", quantile(s.vmapq,.50));
			fprintf(o, "mapq Q3\t%.2f\n", quantile(s.vmapq,.75));

			if (s.dat.lensum > 0) {
				fprintf(o, "snp rate\t%.6f\n", s.dat.nmsum/s.dat.lensum);
				if (s.dat.ins >0 ) fprintf(o, "ins rate\t%.6f\n", s.dat.ins/s.dat.lensum);
				if (s.dat.del >0 ) fprintf(o, "del rate\t%.6f\n", s.dat.del/s.dat.lensum);
				fprintf(o, "pct mismatch\t%.4f\n", 100.0*((double)s.dat.nmnz/s.dat.mapn));
			}

			if (s.visize.size() > 0) {
				double p10 = quantile(s.visize, .10);
				double p90 = quantile(s.visize, .90);
				double matsum=0, matssq=0;
				int matc = 0;
				int i;
				for(i=0;i<s.visize.size();++i) {
					int v = s.visize[i];
					if (v >= p10 && v <= p90) {
						++matc;
						matsum+=v;
						matssq+=v*v;
					}
				}
				fprintf(o, "insert mean\t%.4f\n", matsum/matc);
				if (matc > 1) {
					fprintf(o, "insert stdev\t%.4f\n", stdev(matc, matsum, matssq));
					fprintf(o, "insert Q1\t%.2f\n", quantile(s.visize, .25));
					fprintf(o, "insert median\t%.2f\n", quantile(s.visize, .50));
					fprintf(o, "insert Q3\t%.2f\n", quantile(s.visize, .75));
				}
			}

			if (s.dat.nbase >0) {
				fprintf(o,"base qual mean\t%.4f\n", (s.dat.qualsum/s.dat.nbase)-phred);
				fprintf(o,"base qual stdev\t%.4f\n", stdev(s.dat.nbase, s.dat.qualsum, s.dat.qualssq));
				fprintf(o,"%%A\t%.4f\n", 100.0*((double)s.dat.basecnt[T_A]/(double)s.dat.nbase));
				fprintf(o,"%%C\t%.4f\n", 100.0*((double)s.dat.basecnt[T_C]/(double)s.dat.nbase));
				fprintf(o,"%%G\t%.4f\n", 100.0*((double)s.dat.basecnt[T_G]/(double)s.dat.nbase));
				fprintf(o,"%%T\t%.4f\n", 100.0*((double)s.dat.basecnt[T_T]/(double)s.dat.nbase));
				if (s.dat.basecnt[T_N] > 0) {
					fprintf(o,"%%N\t%.4f\n", 100.0*((double)s.dat.basecnt[T_N]/(double)s.dat.nbase));
				}
			}
			// how many ref seqs have mapped bases?
			int mseq=0;
			google::dense_hash_map<string,scoverage>::iterator it = s.covr.begin();
			vector<string> vtmp;
			bool haverlen = 0;
			while (it != s.covr.end()) {
				if (it->second.mapb > 0) {
					++mseq;								// number of mapped refseqs
					if (mseq <= max_chr) vtmp.push_back(it->first);		// don't bother if too many chrs
					if (it->second.reflen > 0) haverlen = 1;
				}
				++it;
			}
			// don't print per-seq percentages if size is huge, or is 1
			if ((haverlen || mseq > 1) && mseq <= max_chr) {			// worth reporting
				// sort the id's
				sort(vtmp.begin(),vtmp.end());
				vector<string>::iterator vit=vtmp.begin();
				double logb=log(2);
                vector<double> vcovrvar;
                vector<double> vcovr;
                vector<double> vskew;
				while (vit != vtmp.end()) {
					scoverage &v = s.covr[*vit];
					if (v.reflen && histnum > 0) {
						string sig;
						int d; double logd, lsum=0, lssq=0;

						for (d=0;d<histnum;++d) {
                            logd = log(1+v.dist[d])/logb;
                            lsum+=logd;
                            lssq+=logd*logd;
							sig += ('0' + (int) logd);
						}
                        if (rnamode) {
                            // variability of coverage
                            double cv = stdev(histnum, lsum, lssq)/(lsum/histnum);
                            // percent coverage estimated using historgram... maybe track real coverage some day, for now this is fine
                            double covr = 0;
                            for (d=0;d<histnum;++d) {
                                // VFAC = % greater than 1 that a bin must be to be considered 100%
                                if (v.dist[d] > VFACTOR*v.reflen/histnum) {
                                    ++covr;     // 100% covered this bin
                                } else {
                                    // calc bases/(factor * size of bin)
                                    covr += ((double)v.dist[d] / ((double)VFACTOR*v.reflen/histnum));
                                }
                            }
                            double origcovr = covr;
                            covr /= (double) histnum;
                            covr = min(100.0*((double)v.mapb/v.reflen),100.0*covr);
                            // when dealing with "position skewness", you need to anchor things
                            v.spos.Push(v.reflen);
                            v.spos.Push(1);
                            double skew = -v.spos.Skewness();
                            // if there's some coverage
                            if (v.mapr > 0) {
                                vcovr.push_back(covr);              // look at varition
                                vcovrvar.push_back(cv);              // look at varition
                                vskew.push_back(skew);               // and skew
                                if (rnao) {
        						    fprintf(rnao,"%s\t%d\t%ld\t%.2f\t%.4f\t%.4f\t%s\n", vit->c_str(), v.reflen, v.mapr, covr, skew, cv, sig.c_str());
                                }
                            }
                        } else if (max_chr < 100) {
    						fprintf(o,"%%%s\t%.2f\t%s\n", vit->c_str(), 100.0*((double)v.mapb/s.dat.lensum), sig.c_str());
                        } else {
    						fprintf(o,"%%%s\t%.6f\t%s\n", vit->c_str(), 100.0*((double)v.mapb/s.dat.lensum), sig.c_str());
                        }
					} else {
                        if (max_chr < 100) {
						    fprintf(o,"%%%s\t%.2f\n", vit->c_str(), 100.0*((double)v.mapb/s.dat.lensum));
                        } else {
						    fprintf(o,"%%%s\t%.6f\n", vit->c_str(), 100.0*((double)v.mapb/s.dat.lensum));
                        }
					}
					++vit;
				}
                if (rnamode) {
		            sort(vcovr.begin(), vcovr.end());
		            sort(vcovrvar.begin(), vcovrvar.end());
		            sort(vskew.begin(), vskew.end());
                    double medcovrvar = quantile(vcovrvar,.5);
                    double medcovr = quantile(vcovr,.5);
                    double medskew = quantile(vskew,.5);
                    fprintf(o,"median skew\t%.2f\n", medskew);
                    fprintf(o,"median coverage cv\t%.2f\n", medcovrvar);
                    fprintf(o,"median coverage\t%.2f\n", medcovr);
                }
			}
			if (s.covr.size() > 1) {
				fprintf(o,"num ref seqs\t%d\n", (int) s.covr.size());
				fprintf(o,"num ref aligned\t%d\n", (int) mseq);
			}
		}
	}
	return errs ? 1 : 0;
}

#define S_ID 0
#define S_BITS 1
#define S_NMO 2
#define S_POS 3
#define S_MAPQ 4
#define S_CIG 5
#define S_MATEREF 6
#define S_MATE 8
#define S_READ 9
#define S_QUAL 10
#define S_TAG 11

void sstats::dostats(string name, int rlen, int bits, const string &ref, int pos, int mapq, const string &materef, int nmate, const string &seq, const char *qual, int nm, int del, int ins) {

	++dat.n;

	if (pos<=0) return;				// not mapped

	++dat.mapn;

	if (rlen > dat.lenmax) dat.lenmax = rlen;
	if ((rlen < dat.lenmin) || dat.lenmin==0) dat.lenmin = rlen;
	dat.lensum += rlen;
	dat.lenssq += rlen*rlen;

	if (bits & 16) 
	    if (bits & 0x40) 
    		++dat.nrev;
		else
            ++dat.nfor;
	else
	    if (bits & 0x40) 
		    ++dat.nfor;
        else
		    ++dat.nrev;

	dat.mapsum += mapq;
	dat.mapssq += mapq*mapq;
    vmapq.push(mapq);

	if (nm > 0) {
		dat.nmnz += 1;
		dat.nmsum += nm;
	}
	dat.del+=del;
	dat.ins+=ins;

	if (ref.length()) {
		scoverage *sc = &(covr[ref]);
		if (sc) {
			sc->mapb+=rlen;
            if (rnamode) {
                int i;
			    sc->mapr+=1;
                for (i=0;i<rlen;++i) {
                    sc->spos.Push(pos+i);       // position stats
                }
			    if (histnum > 0 && sc->reflen > 0) {
                    for (i=0;i<rlen;++i) {
				        int x = histnum * ((double)(pos+i) / sc->reflen);
                        if (x < histnum) {
                            sc->dist[x]+=1;
                        } else {
                            // out of bounds.... what to do?
                            sc->dist[histnum] += 1;
                        }
                    }
                }
            } else if (histnum > 0 && sc->reflen > 0) {
				int x = histnum * ((double)pos / sc->reflen);
				if (debug > 1) { 
					warn("chr: %s, hn: %d, pos: %d, rl: %d, x: %x\n", ref.c_str(), histnum, pos, sc->reflen, x);
				}
				if (x < histnum) {
                    sc->dist[x]+=rlen;
				} else {
					// out of bounds.... what to do?
					sc->dist[histnum] +=rlen;
				}
			}
		}
	}
	dat.tmapb+=rlen;
	if (nmate>0) {
		visize.push_back(nmate);
		dat.pe=1;
	}

	if (materef.size() && (materef != "=" && materef != "*" && materef != ref)) {
//		printf("disc:%s\t%s\n",materef.c_str(), ref.c_str());
		dat.disc++;
	} else {
		if (abs(nmate) > 50000) {
			dat.disc_pos++;
		}
	}

	int i, j;
	for (i=0;i<seq.length();++i) {
		if (qual[i]>dat.qualmax) dat.qualmax=qual[i];
		if (qual[i]<dat.qualmin) dat.qualmin=qual[i];
		dat.qualsum+=qual[i];
		dat.qualssq+=qual[i]*qual[i];
		++dat.basecnt[basemap[seq[i]]];
		++dat.nbase;
	}
	if (trackdup) {
		size_t p;
        // illumina changed things
		if ((p = name.find_first_of(' '))!=string::npos) 
				name.resize(p);
		int x=++dups[name];
		if (x>dat.dupmax) 
			dat.dupmax=x;
	}
}

bool sstats::parse_sam(FILE *f) {
	line l; meminit(l);
	while (read_line(f, l)>0)  {
		char *sp;
		if (l.s[0]=='@') {
			if (!strncmp(l.s,"@SQ\t",4)) {
				char *t=strtok_r(l.s, "\t", &sp);
				string sname; int slen=0;
				while(t) {
					if (!strncmp(t,"SN:",3)) {
						sname=&(t[3]);
						if (slen) 
							break;
					} else if (!strncmp(t,"LN:",3)) {
						slen=atoi(&t[3]);
						if (sname.length()) 
							break;
					}
					t=strtok_r(NULL, "\t", &sp);
				}
				covr[sname].reflen=slen;
			}
			continue;
		}
		char *t=strtok_r(l.s, "\t", &sp);
		char *d[100]; meminit(d);
		int n =0;
		while(t) {
			d[n++]=t;
			t=strtok_r(NULL, "\t", &sp);
		}
		int nm=0;
		int i;
		// get # mismatches
		for (i=S_TAG;i<n;++i){
			if (d[i] && !strncmp(d[i],"NM:i:",5)) {
				nm=atoi(&d[i][5]);
			}
		}

		if (!d[S_BITS] || !isdigit(d[S_BITS][0]) 
		 || !d[S_POS]  || !isdigit(d[S_POS][0])
		   ) { 
			// invalid sam
			return false;
		}

		int ins = 0, del = 0;	
		char *p=d[S_CIG];
		// sum the cig
		while (*p) {
			int n=strtod(p, &sp);
			if (sp==p) {
				break;
			}
			if (*sp == 'I') 
				ins+=n;
			else if (*sp == 'D') 
				del+=n;
			p=sp;
		}
		if (d[S_CIG][0] == '*') d[S_POS] = (char *) (char *) (char *) (char *) (char *) (char *) (char *) (char *) (char *) "-1";
		dostats(d[S_ID],strlen(d[S_READ]),atoi(d[S_BITS]),d[S_NMO],atoi(d[S_POS]),atoi(d[S_MAPQ]),d[S_MATEREF],atoi(d[S_MATE]),d[S_READ],d[S_QUAL],nm, ins, del);
	}
	return true;
}

bool sstats::parse_bam(const char *in) {
    samfile_t *fp;
    if (!(fp=samopen(in, "rb", NULL))) {
            warn("Error reading '%s': %s\n", in, strerror(errno));
            return false;
    }
    if (fp->header) {
        int i;
        for (i = 0; i < fp->header->n_targets; ++i) {
            covr[fp->header->target_name[i]].reflen=fp->header->target_len[i];
        }
    }
	bam1_t *al=bam_init1();
    while ( samread(fp, al) > 0 ) {
        uint32_t *cig = bam1_cigar(al);
        char *name = bam1_qname(al);
        int len = al->core.l_qseq;
        uint8_t *tag=bam_aux_get(al, "NM");
		int nm = tag ? bam_aux2i(tag) : 0;
		int ins=0, del=0;
		int i;
		for (i=0;i<al->core.n_cigar;++i) {
            int op = cig[i] & BAM_CIGAR_MASK;
			if (op == BAM_CINS) {
				ins+=(cig[i] >> BAM_CIGAR_SHIFT);
			} else if (op == BAM_CDEL) {
				del+=(cig[i] >> BAM_CIGAR_SHIFT);
			}
		}
		if (al->core.n_cigar == 0) 
			al->core.pos=-1;                 // not really a match if there's no cigar string... this deals with bwa's issue

        char *qual = (char *) bam1_qual(al);
        uint8_t * bamseq = bam1_seq(al);
        string seq; seq.resize(len);
        for (i=0;i<len;++i) {
            seq[i] = bam_nt16_rev_table[bam1_seqi(bamseq, i)];
            qual[i] += 33;
        }

		dostats(name,len,al->core.flag,al->core.tid>=0?fp->header->target_name[al->core.tid]:"",al->core.pos+1,al->core.qual, al->core.mtid>=0?fp->header->target_name[al->core.mtid]:"", al->core.isize, seq, qual, nm, ins, del);
	}
	return true;
}

void usage(FILE *f) {
        fprintf(f,
"Usage: sam-stats [options] [file1] [file2...filen]\n"
"Version: %s\n"
"\n"
"Produces lots of easily digested statistics for the files listed\n"
"\n"
"Options (default in parens):\n"
"\n"
"-D             Keep track of multiple alignments (slower!)\n"
"-M             Only overwrite if newer (requires -x, or multiple files)\n"
"-A             Report all chr sigs, even if there are more than 1000\n"
"-R             RNA-Seq stats output (coverage, 3' bias, etc)\n"
"-B             Input is bam, don't bother looking at magic\n"
"-x FIL         File extension for multiple files (stats)\n"
"-b INT         Number of reads to sample for per-base stats (1M)\n"
"-S INT         Size of ascii-signature (30)\n"
"\n"
"OUTPUT:\n"
"\n"
"If one file is specified, then the output is to standard out.  If\n"
"multiple files are specified, or if the -x option is supplied,\n"
"the output file is <filename>.<ext>.  Default extension is 'stats'.\n"
"\n"
"Complete Stats:\n"
"\n"
"  <STATS>           : mean, max, stdev, median, Q1 (25 percentile), Q3\n"
"  reads             : # of entries in the sam file, might not be # reads\n"
"  phred             : phred scale used\n"
"  bsize             : # reads used for qual stats\n"
"  mapped reads      : number of aligned reads (unique probe id sequences)\n"
"  mapped bases      : total of the lengths of the aligned reads\n"
"  forward           : number of forward-aligned reads\n"
"  reverse           : number of reverse-aligned reads\n"
"  snp rate          : mismatched bases / total bases\n"
"  ins rate          : insert bases / total bases\n"
"  del rate          : deleted bases / total bases\n"
"  pct mismatch      : percent of reads that have mismatches\n"
"  len <STATS>       : read length stats, ignored if fixed-length\n"
"  mapq <STATS>      : stats for mapping qualities\n"
"  insert <STATS>    : stats for insert sizes\n"
"  %%<CHR>           : percentage of mapped bases per chr, followed by a signature\n"
"\n"
"Subsampled stats (1M reads max):\n"
"  base qual <STATS> : stats for base qualities\n"
"  %%A,%%T,%%C,%%G       : base percentages\n"
"\n"
"Meaning of the per-chromosome signature:\n"
"  A ascii-histogram of mapped reads by chromosome position.\n"
"  It is only output if the original SAM/BAM has a header. The values\n"
"  are the log2 of the # of mapped reads at each position + ascii '0'.\n"
"\n"
        ,VERSION);
}

std::string string_format(const std::string &fmt, ...) {
       int n, size=100;
       std::string str;
       va_list ap;
       while (1) {
       str.resize(size);
       va_start(ap, fmt);
       int n = vsnprintf((char *)str.c_str(), size, fmt.c_str(), ap);
       va_end(ap);
       if (n > -1 && n < size)
           return str;
       if (n > -1)
           size=n+1;
       else
           size*=2;
       }
}

// R-compatible quantile code : TODO convert to template

template <class vtype>
double quantile(const vtype &vec, double p) {
        int l = vec.size();
        if (!l) return 0;
        double t = ((double)l-1)*p;
        int it = (int) t;
        int v=vec[it];
        if (t > (double)it) {
                return (v + (t-it) * (vec[it+1] - v));
        } else {
                return v;
        }
}

template <class itype>
double quantile(const vector<itype> &vec, double p) {
        int l = vec.size();
        if (!l) return 0;
        double t = ((double)l-1)*p;
        int it = (int) t;
        itype v=vec[it];
        if (t > (double)it) {
                return (v + (t-it) * (vec[it+1] - v));
        } else {
                return v;
        }
}

void build_basemap() {
	int cb,j;
	for (cb=0;cb<256;++cb) {
		switch(cb) {
			case 'A': case 'a':
				j=T_A; break;
			case 'C': case 'c':
				j=T_C; break;
			case 'G': case 'g':
				j=T_G; break;
				case 'T': case 't':
					j=T_T; break;
				default:
					j=T_N; break;
		}
		basemap[cb]=j;
	}
}	



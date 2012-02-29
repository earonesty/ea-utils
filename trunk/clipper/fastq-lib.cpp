/*
Copyright (c) 2011 Expression Analysis / Erik Aronesty

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "fastq-lib.h"

#ifdef __MAIN__
int main(int argc, char **argv) {
	// todo... put testing stuff in here, so the lib can be tested independently of the other componenets
}
#endif

int read_line(FILE *in, struct line &l) {
        return (l.n = getline(&l.s, &l.a, in));
}

int read_fq(FILE *in, int rno, struct fq *fq) {
        read_line(in, fq->id);
	if (fq->id.s && (*fq->id.s == '>')) {
		fq->id.s[0] = '@';
		// read fasta instead
		char c = fgetc(in);
		while (c != '>' && c != EOF) {
			if (fq->seq.a <= (fq->seq.n+1)) {
				fq->seq.s=(char *)realloc(fq->seq.s, fq->seq.a=(fq->seq.a+16)*2);
			}
			if (!isspace(c)) 
				fq->seq.s[fq->seq.n++]=c;
			c = fgetc(in);
		}
		if (c != EOF) {
			ungetc(c, in);
		}
		// make it look like a fastq
		fq->qual.s=(char *)realloc(fq->qual.s, fq->qual.a=(fq->seq.n+1));
		memset(fq->qual.s, 'h', fq->seq.n);
		fq->qual.s[fq->qual.n=fq->seq.n]=fq->seq.s[fq->seq.n]='\0';
		fq->com.s=(char *)malloc(fq->com.a=2);
		fq->com.n=1;
		strcpy(fq->com.s,"+");
	} else {
		read_line(in, fq->seq);
		read_line(in, fq->com);
		read_line(in, fq->qual);
	}

        if (fq->qual.n <= 0)
                return 0;
        if (fq->id.s[0] != '@' || fq->com.s[0] != '+' || fq->seq.n != fq->qual.n) {
                fprintf(stderr, "Malformed fastq record at line %d\n", rno*2+1);
                return -1;
        }
        // win32-safe chomp
        fq->seq.s[--fq->seq.n] = '\0';
        if (fq->seq.s[fq->seq.n-1] == '\r') {
                fq->seq.s[--fq->seq.n] = '\0';
        }
        fq->qual.s[--fq->qual.n] = '\0';
        if (fq->qual.s[fq->qual.n-1] == '\r') {
                fq->qual.s[--fq->qual.n] = '\0';
        }
        return 1;
}

struct qual_str {
        long long int cnt;
        long long int sum;
        long long int ssq;
        long long int ns;
} quals[MAX_FILENO_QUALS+1] = {{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0}};

int gzclose(FILE *f, bool isgz) {
	return isgz ? pclose(f) : fclose(f);
}

FILE *gzopen(const char *f, const char *m, bool*isgz) {
	// maybe use zlib some day?
        FILE *h;
        if (!strcmp(fext(f),".gz")) {
                char *tmp=(char *)malloc(strlen(f)+100);
                if (strchr(m,'w')) {
                        strcpy(tmp, "gzip --rsyncable > '");
                        strcat(tmp, f);
                        strcat(tmp, "'");
                } else {
                        strcpy(tmp, "gunzip -c '");
                        strcat(tmp, f);
                        strcat(tmp, "'");
                }
		h = popen(tmp, m);
		*isgz=1;
		free(tmp);
        } else {
                h = fopen(f, m);
                *isgz=0;
        }
        if (!h) {
                fprintf(stderr, "Error opening file '%s': %s\n",f, strerror(errno));
                exit(1);
        }
        return h;
}

const char *fext(const char *f) {
        const char *x=strrchr(f,'.');
        return x ? x : "";
}

bool poorqual(int n, int l, const char *s, const char *q) {
        int i=0, sum=0, ns=0;
        for (i=0;i<l;++i) {
                if (s[i] == 'N')
                        ++ns;
                quals[n].cnt++;
                quals[n].ssq += q[i] * q[i];
                sum+=q[i];
        }
        quals[n].sum += sum;
        quals[n].ns += ns;
        int xmean = sum/l;
        if (quals[n].cnt < 10000) {
                return (xmean > 20) && (ns == 0);
        }
        int pmean = quals[n].sum / quals[n].cnt;                                // mean q
        double pdev = stdev(quals[n].cnt, quals[n].sum, quals[n].ssq);          // dev q
        int serr = pdev/sqrt(l);                                                // stderr for length l
        if (xmean < (pmean - serr)) {                                           // off by 1 stdev?
                return 0;                                                       // ditch it
        }
        if (ns > (quals[n].ns / quals[n].cnt)) {                                // more n's than average?
                return 0;                                                       // ditch it
        }
        return 1;
}


#define comp(c) ((c)=='A'?'T':(c)=='a'?'t':(c)=='C'?'G':(c)=='c'?'g':(c)=='G'?'C':(c)=='g'?'c':(c)=='T'?'A':(c)=='t'?'a':(c))

void revcomp(struct fq *d, struct fq *s) {
        if (!d->seq.s) {
                d->seq.s=(char *) malloc(d->seq.a=s->seq.n);
                d->qual.s=(char *) malloc(d->qual.a=s->qual.n);
        } else if (d->seq.a <= s->seq.n) {
                d->seq.s=(char *) realloc(d->seq.s, d->seq.a=(s->seq.n+1));
                d->qual.s=(char *) realloc(d->qual.s, d->qual.a=(s->qual.n+1));
        }
        int i;
        for (i=0;i<s->seq.n/2;++i) {
                char b=s->seq.s[i];
                char q=s->qual.s[i];
                //printf("%d: %c, %c\n", i, comp(s->seq.s[s->seq.n-i-1]), s->qual.s[s->qual.n-i-1]);
                d->seq.s[i]=comp(s->seq.s[s->seq.n-i-1]);
                d->qual.s[i]=s->qual.s[s->qual.n-i-1];
                //printf("%d: %c, %c\n", s->seq.n-i-1, comp(b), q);
                d->seq.s[s->seq.n-i-1]=comp(b);
                d->qual.s[s->seq.n-i-1]=q;
        }
        if (s->seq.n % 2) {
                //printf("%d: %c, %c\n", 1+s->seq.n/2, comp(s->seq.s[s->seq.n/2]));
                d->seq.s[s->seq.n/2] = comp(s->seq.s[s->seq.n/2]);
                d->qual.s[s->seq.n/2] = s->qual.s[s->seq.n/2];
        }
        d->seq.n=s->seq.n;
        d->qual.n=s->qual.n;
        s->seq.s[s->seq.n]='\0';
        s->qual.s[s->seq.n]='\0';
}


/* getline.c -- Replacement for GNU C library function getline

Copyright (C) 1993 Free Software Foundation, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

/* Written by Jan Brittenson, bson@gnu.ai.mit.edu.  */

#include <sys/types.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>

/* Read up to (and including) a TERMINATOR from STREAM into *LINEPTR
   + OFFSET (and null-terminate it). *LINEPTR is a pointer returned from
   malloc (or NULL), pointing to *N characters of space.  It is realloc'd
   as necessary.  Return the number of characters read (not including the
   null terminator), or -1 on error or EOF.  */

int getstr (char ** lineptr, size_t *n, FILE * stream, char terminator, int offset)
{
  int nchars_avail;		/* Allocated but unused chars in *LINEPTR.  */
  char *read_pos;		/* Where we're reading into *LINEPTR. */
  int ret;

  if (!lineptr || !n || !stream)
    return -1;

  if (!*lineptr)
    {
      *n = 64;
      *lineptr = (char *) malloc (*n);
      if (!*lineptr)
	return -1;
    }

  nchars_avail = *n - offset;
  read_pos = *lineptr + offset;

  for (;;)
    {
      register int c = getc (stream);

      /* We always want at least one char left in the buffer, since we
	 always (unless we get an error while reading the first char)
	 NUL-terminate the line buffer.  */

      assert(*n - nchars_avail == read_pos - *lineptr);
      if (nchars_avail < 1)
	{
	  if (*n > 64)
	    *n *= 2;
	  else
	    *n += 64;

	  nchars_avail = *n + *lineptr - read_pos;
	  *lineptr = (char *) realloc (*lineptr, *n);
	  if (!*lineptr)
	    return -1;
	  read_pos = *n - nchars_avail + *lineptr;
	  assert(*n - nchars_avail == read_pos - *lineptr);
	}

      if (c == EOF || ferror (stream))
	{
	  /* Return partial line, if any.  */
	  if (read_pos == *lineptr)
	    return -1;
	  else
	    break;
	}

      *read_pos++ = c;
      nchars_avail--;

      if (c == terminator)
	/* Return the line.  */
	break;
    }

  /* Done - NUL terminate and return the number of chars read.  */
  *read_pos = '\0';

  ret = read_pos - (*lineptr + offset);
  return ret;
}

#if !defined(__GNUC__) || defined(__APPLE__) || defined(WIN32)

ssize_t getline(char **lineptr, size_t *n, FILE *stream)
{
  return getstr (lineptr, n, stream, '\n', 0);
}

#endif

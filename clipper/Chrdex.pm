package Chrdex;

# Licensed via the "Artistic License" 
# See: http://dev.perl.org/licenses/artistic.html
# Copyright 2011, Expression Analysis
# Author: Erik Aronesty <earonesty@expressionanalysis.com>
# "Let me know if it's useful"
#
# EXMAPLE:
# $x = Chrdex->new("CCDS_Exome_annot.txt", chr=>2, beg=>5, end=>6, skip=>1);
# $x->search(1, 153432255);
# TODO:
# if begin = end... just dump a regular hash

use Inline 'C';

use strict;
use warnings::register;

use Storable qw(store retrieve);
use Data::Dumper;
use locale; ##added by vjw to control case of reference bases

our $VERSION = '1.0';
my $FILEVER = 2;

sub new {
	my ($class, $path, %opts) = @_;

	if (ref($class)) {
		$class = ref($class);
	}

	$opts{delim} = "\t" if ! $opts{delim};
	$opts{skip} = 0 if ! $opts{skip};
	$opts{chr} = 0 if ! $opts{chr};
	$opts{beg} = 1 if ! $opts{beg};
	$opts{end} = 2 if ! $opts{end};
	$opts{ver} = $FILEVER;

	if (! -s $path) {		# be a little more careful about this one
		if (! -s $path || -d $path) {
			die "Can't open $path.\n";
		}
	}
	my $annob = $path;
	$annob =~ s/([^\/]+)$/\.$1/;

	# allow the user to override the index location
	$annob = $opts{index_path} if $opts{index_path};

	my $ref;
        my $mt = (stat($path))[9];
	# if index is new
	if ((stat("$annob.chrdex"))[9] > $mt) {
		$ref = retrieve "$annob.chrdex";
		# if arguments were different, then clear ref
		for (qw(delim skip chr beg end ver)) {
			last if !$ref;
			$ref = undef if !($ref->{_opts}->{$_} eq $opts{$_});
		}

		if ($ref) {
			# if begin != end, then type is range
			if ($ref->{_opts}->{beg} != $ref->{_opts}->{end}) {
				eval{chrdex_check($ref)};
				if ($@) {
					$ref = undef;
				}
			} else {
				# if begin == end, then type is plain hash
				$ref->{_type}='I';
			}
		}
	}

	if (!$ref) {
		my %locs;
		my $tmpb = "$annob.$$";
		open( IN, $path ) or die "Can't open $path: $!\n";
		my $skip = $opts{skip};
		while ($skip > 0) { scalar <IN>; --$skip };

		while(<IN>){
			my ($chr, $beg, $end);
			$_ =~ s/\s+$//;
			my @data = split /\t/;
			$chr = $data[$opts{chr}];
			$beg = $data[$opts{beg}];
			$end = $data[$opts{end}];	
			if (!(($beg+0) eq $beg)) {
				die "Invalid data in $path at line $., expected a number, got '$beg'\n";
			}
			$chr=~s/^chr//i;
			# here's where you put the annotation info
			if ($opts{beg} == $opts{end}) {
				if ($locs{"$chr:$beg"}) {
 					$locs{"$chr:$beg"} = $locs{"$chr:$beg"} . "$_\n";
				} else {
					$locs{"$chr:$beg"} = $_;
				}
			} else {
				push @{$locs{$chr}}, [$beg+0, $end+0, $_];
			}
		}
		close IN;

		if ($opts{beg} == $opts{end}) {
			goto DONE;
		}

		# sort & cache annotation, deal with overlaps nicely
		my $i;
		for my $chr (keys(%locs)) {
			my $arr = $locs{$chr};
			@{$locs{$chr}} = sort {$a->[0]-$b->[0]} @{$locs{$chr}};
			for ($i=0;$i<$#{$arr};++$i) {
				next unless $arr->[$i+1]->[0];				# empty? skip
				if ($arr->[$i]->[1] >= $arr->[$i+1]->[0]) {		# if i overlap the next one
					# warn 1, Dumper($arr->[$i], $arr->[$i+1], $arr->[$i+2]);
				
					# frag after next	
					my $new_st = $arr->[$i+1]->[1]+1;
					my $new_en = $arr->[$i]->[1];
					my $new_ro = $arr->[$i]->[2];
			
					# TODO: store as array... string folding will save lots of space when there are many overlaps
					# but hasn't been a problem so far
					if ($arr->[$i]->[1] < $arr->[$i+1]->[1]) {
						# overlap next
						$new_st = $arr->[$i]->[1] + 1;
						$new_en = $arr->[$i+1]->[1];
						$new_ro = $arr->[$i+1]->[2];
						$arr->[$i+1]->[1] = $arr->[$i]->[1];
						$arr->[$i+1]->[2] = $arr->[$i+1]->[2] . "\n" . $arr->[$i]->[2];
					} else {
						$arr->[$i+1]->[2] = $arr->[$i+1]->[2] . "\n" . $arr->[$i]->[2];
					}

					# shorten my end to less than the next's start
					$arr->[$i]->[1] = $arr->[$i+1]->[0]-1;

					#die "$new_st $new_en $new_ro", Dumper($arr->[$i])
					#	if $chr eq 'III' && $arr->[$i]->[0] == 208130;

					if ($new_en >= $new_st) {
						# warn "NEW: $new_st $new_en $new_ro\n";

						# put the fragment where it belongs
						my $j=$i+2;
						while ($j<=$#{$arr} & $new_st > $arr->[$j]->[0]) {
							++$j;
						}
						splice(@{$arr}, $j, 0, [$new_st, $new_en, $new_ro]);
					}
					
					if ($arr->[$i]->[1] < $arr->[$i]->[0]) {
						splice(@{$arr}, $i, 1);
						--$i;
					}
					# warn 2, Dumper($arr->[$i], $arr->[$i+1], $arr->[$i+2]);
				}
			}
		}
		DONE:
		$locs{_opts} = \%opts;
		store \%locs, "$tmpb.chrdex";
		$locs{_type}='I' if $ref->{_opts}->{beg} == $ref->{_opts}->{end};
		rename "$tmpb.chrdex", "$annob.chrdex";
		$ref = \%locs;
	}

	if (!($ref->{_type} eq 'I')) {
		chrdex_check($ref);
	}

	return bless $ref, $class;
}

sub search {
	my ($self, $chr, $loc) = @_;
	$chr=~s/^chr//i;
	if ($self->{_type} eq 'I') {
		return $self->{"$chr:$loc"};
	} else {
		return chrdex_search($self, $chr, $loc);	
	}
}

1;

__DATA__
__C__

bool get_sten(AV *arr, int i, int *st, int*en);
SV * av_fetch_2(AV *arr, int i, int j);

void chrdex_search(SV *self, SV *schr, SV* sloc) {
	int b=0, t, i, st, en;
	AV *arr;
	SV **pav;
	SV *roi;
	HV *map= (HV*) SvRV(self);
	char *chr = SvPV_nolen(schr);
	int loc = SvIV(sloc);

	pav = hv_fetch(map, chr, strlen(chr), 0);

	if (!pav)
		return;

	arr = (AV*) SvRV(*pav);

	b = 0;
	t = av_len(arr);

	if (t <= b) {
		get_sten(arr, i=0, &st, &en);
	} else {
            while (t > b) {
                i = (t+b)/2;
		if (!get_sten(arr, i, &st, &en))
			return;

                if ((i == b) || (i == t)) 
			break;
                if (loc > en) {
                        b = i;
                } else if (loc < st) {
                        t = i;
                } else {
                        break;
                }
            }
	}

//	printf("chr:%s loc: %d, st: %d en: %d i: %d t: %d b: %d\n", chr, loc, st, en, i, t, b);

	if (loc < st) {
		--i;
		if (i < 0 || !get_sten(arr, i, &st, &en))
			return;
	} else if (loc > en) {
		++i;
		if (!get_sten(arr, i, &st, &en))
			return;
	}

        if (loc >= st && loc <= en) {
		roi = av_fetch_2(arr, i, 2);
		if (!roi)
			return;

		Inline_Stack_Vars;
		Inline_Stack_Reset;
		Inline_Stack_Push(sv_2mortal(newSVsv(roi)));
		Inline_Stack_Done;
		Inline_Stack_Return(1);
        }

        return;
}

SV * av_fetch_2(AV *arr, int i, int j) {
        SV **pav;

        if (!(pav = av_fetch(arr,i,0)))
                return &PL_sv_undef;

        arr = (AV*) SvRV(*pav);

        if (!(pav = av_fetch(arr,j,0)))
                return &PL_sv_undef;

        return *pav;
}

bool get_sten(AV *arr, int i, int *st, int*en) {
	SV **pav;

	if (!(pav = av_fetch(arr,i,0)))
		return 0;

	arr = (AV*) SvRV(*pav);

	if (!(pav = av_fetch(arr,0,0)))
		return 0;
	*st = SvIV(*pav);

	if (!(pav = av_fetch(arr,1,0)))
		return 0;

	*en = SvIV(*pav);

	return 1;
}

void chrdex_check(SV *annoR) {
	HE *he;		// hash entry
	SV *ent;	// hash value
	HV *hv;		// annotation hash table
	AV *av;		// array in hash
	SV **v;		// entry in array

        if (!SvRV(annoR))
                croak("annotation hash must be a reference");

        annoR = SvRV(annoR);
        if (SvTYPE(annoR) != SVt_PVHV)
                croak("annotation array must be a hash ref");

	hv = (HV *) annoR;
	if (!hv_iterinit(hv)) { 
                croak("empty hash, fix that in perl");
	}

	he = hv_iternext(hv);
	ent = hv_iterval(hv, he);

	if ( SvTYPE(ent) != SVt_RV || (SvTYPE(SvRV(ent)) != SVt_PVAV) ) {
		croak("each entry in the annotation hash must be a reference to an array");
	}

	av = (AV*) SvRV(ent);
	v = av_fetch(av, 0, 0);
	if (!v) {
		croak("no empty annotation arrays, please");
	}
	
	if ( SvTYPE(*v) != SVt_RV || (SvTYPE(SvRV(*v)) != SVt_PVAV) ) {
		croak("each entry in the array should contain a start and end region");
	}

	// ok.... reference should be safe enough not to segfault later
}



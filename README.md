# ea-utils

Command-line tools for processing biological sequencing data. Barcode demultiplexing, adapter trimming, etc.

Primarily written to support an Illumina based pipeline - but should work with any FASTQs.

## Overview:

### fastq-mcf
Scans a sequence file for adapters, and, based on a log-scaled threshold, determines a set of clipping parameters and performs clipping. Also does skewing detection and quality filtering.

### fastq-multx
Demultiplexes a fastq. Capable of auto-determining barcode id's based on a master set fields. Keeps multiple reads in-sync during demultiplexing. Can verify that the reads are in-sync as well, and fail if they're not.

### fastq-join
Similar to audy's stitch program, but in C, more efficient and supports some automatic benchmarking and tuning. It uses the same "squared distance for anchored alignment" as other tools.

### varcall
Takes a pileup and calculates variants in a more easily parameterized manner than some other tools.

## Other Stuff:

* sam-stats - Basic sam/bam stats. Like other tools, but produces what I want to look at, in a format suitable for passing to other programs. (View source)
* fastq-stats - Basic fastq stats. Counts duplicates. Option for per-cycle stats, or not (irrelevant for many sequencers). (View source)
* determine-phred - Returns the phred scale of the input file. Works with sams, fastq's or pileups and gzipped files.
Chrdex.pm & Sqldex.pm - obsoleted by the cpan module Text::Tidx. Sqldex may not actually be obsolete, because Tidx uses more ram and is slower for very small jobs. But for Exome and RNA-Seq work, Text::Tidx beats both.
* qsh - Runs a bash script file like a "cluster aware makefile"...only processing newer things, die'ing if things go wrong, and sending jobs to a queue manager if they're big. That way you don't have to write makefiles, or wrap things in "qsub" calls for every little program. Not really ready yet.
* grun - Fast, lightweight grid queue software. Keeps the job queue on disk at all times. Very fast. Works well by now
* gwrap - Bash wrapper shell that downloads all dependencies that are not the local system.... good for EC2 nodes. Linux only. Will use it if we ever go to EC2.
* gtf2bed - Converter that bundles up a GFF's exons and makes a UCSC-styled bed file with thin/thick properly set from the start/stop sites. (Click for source)
* randomFQ - takes a fastq (can be gzipped or paired-end) and randomly subsets to a user defined number of reads (Click for source)

## Citing:

Erik Aronesty (2011). ea-utils : "Command-line tools for processing biological sequencing data"; http://code.google.com/p/ea-utils
Erik Aronesty (2013). TOBioiJ : "Comparison of Sequencing Utility Programs", DOI:10.2174/1875036201307010001

/*  vcfconvert.c -- convert between VCF/BCF and related formats.

    Copyright (C) 2013-2014 Genome Research Ltd.

    Author: Petr Danecek <pd3@sanger.ac.uk>

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
THE SOFTWARE.  */

#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <htslib/faidx.h>
#include <htslib/vcf.h>
#include <htslib/bgzf.h>
#include <htslib/synced_bcf_reader.h>
#include <htslib/vcfutils.h>
#include <htslib/kseq.h>
#include "bcftools.h"
#include "filter.h"
#include "convert.h"
#include "tsv2vcf.h"

// Logic of the filters: include or exclude sites which match the filters?
#define FLT_INCLUDE 1
#define FLT_EXCLUDE 2

typedef struct _args_t args_t;
struct _args_t
{
    faidx_t *ref;
    filter_t *filter;
    char *filter_str;
    int filter_logic;   // include or exclude sites which match the filters? One of FLT_INCLUDE/FLT_EXCLUDE
    convert_t *convert;
    bcf_srs_t *files;
    bcf_hdr_t *header;
    void (*convert_func)(struct _args_t *);
    struct {
        int total, skipped, hom_rr, het_ra, hom_aa, het_aa; 
    } n;
    kstring_t str;
    int32_t *gts;
    int nsamples, *samples, sample_is_file, targets_is_file, regions_is_file, output_type;
    char **argv, *sample_list, *targets_list, *regions_list, *tag, *columns;
    char *outfname, *infname, *ref_fname;
    int argc;
};

static void destroy_data(args_t *args)
{
    if ( args->convert) convert_destroy(args->convert);
    if ( args->filter ) filter_destroy(args->filter);
    free(args->samples);
    if ( args->files ) bcf_sr_destroy(args->files);
}

static void open_vcf(args_t *args, const char *format_str)
{
    args->files = bcf_sr_init();
    if ( args->regions_list )
    {
        if ( bcf_sr_set_regions(args->files, args->regions_list, args->regions_is_file)<0 )
            error("Failed to read the regions: %s\n", args->regions_list);
    }
    if ( args->targets_list )
    {
        if ( bcf_sr_set_targets(args->files, args->targets_list, args->targets_is_file, 0)<0 )
            error("Failed to read the targets: %s\n", args->targets_list);
    }
    if ( !bcf_sr_add_reader(args->files, args->infname) )
        error("Failed to open or the file not indexed: %s\n", args->infname);
    if ( args->filter_str )
        args->filter = filter_init(args->header, args->filter_str);

    args->header = args->files->readers[0].header;

    int i, nsamples = 0, *samples = NULL;
    if ( args->sample_list && strcmp("-",args->sample_list) )
    {
        for (i=0; i<args->files->nreaders; i++)
        {
            int ret = bcf_hdr_set_samples(args->files->readers[i].header,args->sample_list,args->sample_is_file);
            if ( ret<0 ) error("Error parsing the sample list\n");
            else if ( ret>0 ) error("Sample name mismatch: sample #%d not found in the header\n", ret);
        }

        if ( args->sample_list[0]!='^' )
        {
            // the sample ordering may be different if not negated
            int n;
            char **smpls = hts_readlist(args->sample_list, args->sample_is_file, &n);
            if ( !smpls ) error("Could not parse %s\n", args->sample_list);
            if ( n!=bcf_hdr_nsamples(args->files->readers[0].header) )
                error("The number of samples does not match, perhaps some are present multiple times?\n");
            nsamples = bcf_hdr_nsamples(args->files->readers[0].header);
            samples = (int*) malloc(sizeof(int)*nsamples);
            for (i=0; i<n; i++)
            {
                samples[i] = bcf_hdr_id2int(args->files->readers[0].header, BCF_DT_SAMPLE,smpls[i]);
                free(smpls[i]);
            }
            free(smpls);
        }
    }
    args->convert = convert_init(args->header, samples, nsamples, format_str);
    free(samples);

    if ( args->filter_str )
        args->filter = filter_init(args->header, args->filter_str);
}

static void vcf_to_gensample(args_t *args)
{
    kstring_t str = {0,0,0};
    kputs("%CHROM:%POS\\_%REF\\_%FIRST_ALT %_CHROM_POS_ID %POS %REF %FIRST_ALT", &str);
    if ( !args->tag || !strcmp(args->tag,"GT") ) kputs("%_GT_TO_PROB3",&str);
    else if ( !strcmp(args->tag,"PL") ) kputs("%_PL_TO_PROB3",&str);
    else error("todo: --tag %s\n", args->tag);
    kputs("\n", &str);
    open_vcf(args,str.s);

    int is_compressed = 1;
    char *gen_fname = NULL, *sample_fname = NULL;
    str.l = 0;
    kputs(args->outfname,&str);
    char *t = str.s;
    while ( *t && *t!=',' ) t++;
    if ( *t )
    {
        *t = 0;
        gen_fname = strdup(str.s);
        if ( strlen(gen_fname)<3 || strcasecmp(".gz",gen_fname+strlen(gen_fname)-3) ) is_compressed = 0;
        sample_fname = strdup(t+1);
    }
    else
    {
        int l = str.l;
        kputs(".samples",&str);
        sample_fname = strdup(str.s);
        str.l = l;
        kputs(".gen.gz",&str);
        gen_fname = strdup(str.s);
    }

    int i;
    FILE *fh = fopen(sample_fname,"w");
    if ( !fh ) error("Failed to write %s: %s\n", sample_fname,strerror(errno));
    fprintf(fh,"ID_1 ID_2 missing\n0 0 0\n");
    for (i=0; i<bcf_hdr_nsamples(args->header); i++)
        fprintf(fh,"%s %s 0\n", args->header->samples[i],args->header->samples[i]);
    fclose(fh);

    BGZF *out = bgzf_open(gen_fname, is_compressed ? "w" : "wu");
    free(sample_fname);

    while ( bcf_sr_next_line(args->files) )
    {
        bcf1_t *line = bcf_sr_get_line(args->files,0);
        if ( line->n_allele==1 ) continue;  // alternate allele is required
        if ( args->filter )
        {
            int pass = filter_test(args->filter, line, NULL);
            if ( args->filter_logic & FLT_EXCLUDE ) pass = pass ? 0 : 1;
            if ( !pass ) continue;
        }

        str.l = 0;
        convert_line(args->convert, line, &str);
        if ( str.l )
        {
            int ret = bgzf_write(out, str.s, str.l);
            if ( ret!= str.l ) error("Error writing %s: %s\n", gen_fname,strerror(errno));
        }
    }
    if ( str.m ) free(str.s);
    if ( bgzf_close(out)!=0 ) error("Error closing %s: %s\n", gen_fname,strerror(errno));
    free(gen_fname);
}

static void bcf_hdr_set_chrs(bcf_hdr_t *hdr, faidx_t *fai)
{
    int i, n = faidx_nseq(fai);
    for (i=0; i<n; i++)
    {
        const char *seq = faidx_iseq(fai,i);
        int len = faidx_seq_len(fai, seq);
        bcf_hdr_printf(hdr, "##contig=<ID=%s,length=%d>", seq,len);
    }
}
static inline int acgt_to_5(char base)
{
    if ( base=='A' ) return 0;
    if ( base=='C' ) return 1;
    if ( base=='G' ) return 2;
    if ( base=='T' ) return 3;
    return 4;
}
static inline int tsv_setter_aa1(args_t *args, char *ss, char *se, int alleles[], int *nals, int ref, int32_t *gts)
{
    if ( se - ss > 2 ) return -1;   // currently only SNPs

    if ( ss[0]=='-' ) return -2;    // skip these
    if ( ss[0]=='I' ) return -2;
    if ( ss[0]=='D' ) return -2;

    int a0 = acgt_to_5(toupper(ss[0]));
    int a1 = ss[1] ? acgt_to_5(toupper(ss[1])) : a0;
    if ( alleles[a0]<0 ) alleles[a0] = (*nals)++;
    if ( alleles[a1]<0 ) alleles[a1] = (*nals)++;

    gts[0] = bcf_gt_unphased(alleles[a0]); 
    gts[1] = ss[1] ? bcf_gt_unphased(alleles[a1]) : bcf_int32_vector_end;

    if ( ref==a0 && ref==a1  ) args->n.hom_rr++;    // hom ref: RR
    else if ( ref==a0 ) args->n.het_ra++;           // het: RA
    else if ( ref==a1 ) args->n.het_ra++;           // het: AR
    else if ( a0==a1 ) args->n.hom_aa++;            // hom-alt: AA
    else args->n.het_aa++;                          // non-ref het: AA

    return 0;
}
static int tsv_setter_aa(tsv_t *tsv, bcf1_t *rec, void *usr)
{
    args_t *args = (args_t*) usr;

    int len;
    char *ref = faidx_fetch_seq(args->ref, (char*)bcf_hdr_id2name(args->header,rec->rid), rec->pos, rec->pos, &len);
    if ( !ref ) error("faidx_fetch_seq failed at %s:%d\n", bcf_hdr_id2name(args->header,rec->rid), rec->pos+1);

    int nals = 1, alleles[5] = { -1, -1, -1, -1, -1 };    // a,c,g,t,n
    ref[0] = toupper(ref[0]);
    int iref = acgt_to_5(ref[0]);
    alleles[iref] = 0;

    rec->n_sample = bcf_hdr_nsamples(args->header);

    int i, ret;
    for (i=0; i<rec->n_sample; i++)
    {
        if ( i>0 )
        {
            ret = tsv_next(tsv);
            if ( ret==-1 ) error("Too few columns for %d samples at %s:%d\n", rec->n_sample,bcf_hdr_id2name(args->header,rec->rid), rec->pos+1);
        }
        ret = tsv_setter_aa1(args, tsv->ss, tsv->se, alleles, &nals, iref, args->gts+i*2);
        if ( ret==-1 ) error("Error parsing the site %s:%d, expected two characters\n", bcf_hdr_id2name(args->header,rec->rid), rec->pos+1);
        if ( ret==-2 ) 
        {
            // something else than a SNP
            free(ref);
            return 0;
        }
    }

    args->str.l = 0;
    kputc(ref[0], &args->str);
    for (i=0; i<5; i++) 
    {
        if ( alleles[i]>0 )
        {
            kputc(',', &args->str);
            kputc("ACGTN"[i], &args->str);
        }
    }
    bcf_update_alleles_str(args->header, rec, args->str.s);
    if ( bcf_update_genotypes(args->header,rec,args->gts,rec->n_sample*2) ) error("Could not update the GT field\n");

    free(ref);
    return 0;
}

static void tsv_to_vcf(args_t *args)
{
    if ( !args->ref_fname ) error("Missing the --ref option\n");
    if ( !args->sample_list ) error("Missing the --samples option\n");

    args->ref = fai_load(args->ref_fname);
    if ( !args->ref ) error("Could not load the reference %s\n", args->ref_fname);

    args->header = bcf_hdr_init("w");
    bcf_hdr_set_chrs(args->header, args->ref);
    bcf_hdr_append(args->header, "##FORMAT=<ID=GT,Number=1,Type=String,Description=\"Genotype\">");

    int i, n;
    char **smpls = hts_readlist(args->sample_list, args->sample_is_file, &n);
    if ( !smpls ) error("Could not parse %s\n", args->sample_list);
    for (i=0; i<n; i++)
    {
        bcf_hdr_add_sample(args->header, smpls[i]);
        free(smpls[i]);
    }
    free(smpls);
    bcf_hdr_add_sample(args->header, NULL);
    args->gts = (int32_t *) malloc(sizeof(int32_t)*n*2);

    htsFile *out_fh = hts_open(args->outfname,hts_bcf_wmode(args->output_type));
    bcf_hdr_write(out_fh,args->header);

    tsv_t *tsv = tsv_init(args->columns ? args->columns : "ID,CHROM,POS,AA");
    if ( tsv_register(tsv, "CHROM", tsv_setter_chrom, args->header) < 0 ) error("Expected CHROM column\n");
    if ( tsv_register(tsv, "POS", tsv_setter_pos, NULL) < 0 ) error("Expected POS column\n");
    if ( tsv_register(tsv, "ID", tsv_setter_id, args->header) < 0 ) error("Expected ID column\n");
    if ( tsv_register(tsv, "AA", tsv_setter_aa, args) < 0 ) error("Expected AA column\n");

    bcf1_t *rec = bcf_init();
    bcf_float_set_missing(rec->qual);

    kstring_t line = {0,0,0};
    htsFile *in_fh = hts_open(args->infname, "r");
    if ( !in_fh ) error("Could not read: %s\n", args->infname);
    while ( hts_getline(in_fh, KS_SEP_LINE, &line) > 0 )
    {
        if ( line.s[0]=='#' ) continue;     // skip comments

        args->n.total++;
        if ( !tsv_parse(tsv, rec, line.s) )
            bcf_write(out_fh, args->header, rec);
        else
            args->n.skipped++;
    }
    if ( hts_close(in_fh) ) error("Close failed: %s\n", args->infname);
    free(line.s);

    fai_destroy(args->ref);
    bcf_hdr_destroy(args->header);
    hts_close(out_fh);
    tsv_destroy(tsv);
    bcf_destroy(rec);
    free(args->str.s);
    free(args->gts);

    fprintf(stderr,"Rows total: \t%d\n", args->n.total);
    fprintf(stderr,"Rows skipped: \t%d\n", args->n.skipped);
    fprintf(stderr,"Hom RR: \t%d\n", args->n.hom_rr);
    fprintf(stderr,"Het RA: \t%d\n", args->n.het_ra);
    fprintf(stderr,"Hom AA: \t%d\n", args->n.hom_aa);
    fprintf(stderr,"Het AA: \t%d\n", args->n.het_aa);
}

static void usage(void)
{
    fprintf(stderr, "\n");
    fprintf(stderr, "About:   Converts VCF/BCF to other formats and back. See man page for file\n");
    fprintf(stderr, "         formats details\n");
    fprintf(stderr, "Usage:   bcftools convert [OPTIONS] <input_file>\n");
    fprintf(stderr, "VCF input options:\n");
    fprintf(stderr, "   -e, --exclude <expr>        exclude sites for which the expression is true\n");
    fprintf(stderr, "   -i, --include <expr>        select sites for which the expression is true\n");
    fprintf(stderr, "   -r, --regions <region>      restrict to comma-separated list of regions\n");
    fprintf(stderr, "   -R, --regions-file <file>   restrict to regions listed in a file\n");
    fprintf(stderr, "   -s, --samples <list>        list of samples to include\n");
    fprintf(stderr, "   -S, --samples-file <file>   file of samples to include\n");
    fprintf(stderr, "   -t, --targets <region>      similar to -r but streams rather than index-jumps\n");
    fprintf(stderr, "   -T, --targets-file <file>   similar to -R but streams rather than index-jumps\n");
    fprintf(stderr, "VCF output options:\n");
    fprintf(stderr, "   -o, --output <file>         write output to a file [standard output]\n");
    fprintf(stderr, "   -O, --output-type <type>    'b' compressed BCF; 'u' uncompressed BCF; 'z' compressed VCF; 'v' uncompressed VCF [v]\n");
    fprintf(stderr, "gen/sample options:\n");
//  fprintf(stderr, "   -G, --gensample2vcf     <prefix> or <gen-file>,<sample-file>\n");
    fprintf(stderr, "   -g, --gensample         <prefix> or <gen-file>,<sample-file>\n");
    fprintf(stderr, "       --tag <string>      tag to take values for .gen file: GT,PL,GL,GP [GT]\n");
    fprintf(stderr, "tsv options:\n");
    fprintf(stderr, "       --tsv2vcf <file>        \n");
    fprintf(stderr, "   -c, --columns <string>      columns of the input tsv file [CHROM,POS,ID,AA]\n");
    fprintf(stderr, "   -f, --fasta-ref <file>      reference sequence in fasta format\n");
    fprintf(stderr, "   -s, --samples <list>        list of sample names\n");
    fprintf(stderr, "   -S, --samples-file <file>   file of sample names\n");
    fprintf(stderr, "\n");

//    fprintf(stderr, "hap/legend/sample options:\n");
//    fprintf(stderr, "   -h, --haplegend     <prefix> or <hap-file>,<legend-file>,<sample-file>\n");
//    fprintf(stderr, "\n");
//    fprintf(stderr, "plink options:\n");
//    fprintf(stderr, "   -p, --plink         <prefix> or <ped>,<map>,<fam>/<bed>,<bim>,<fam>/<tped>,<tfam>\n");
//    fprintf(stderr, "   --tped              make tped file instead\n");
//    fprintf(stderr, "   --bin               make binary bed/fam/bim files\n");
//    fprintf(stderr, "\n");
//    fprintf(stderr, "pbwt options:\n");
//    fprintf(stderr, "   -b, --pbwt          <prefix> or <pbwt>,<sites>,<sample>,<missing>\n");
    exit(1);
}

int main_vcfconvert(int argc, char *argv[])
{
    int c;
    args_t *args = (args_t*) calloc(1,sizeof(args_t));
    args->argc   = argc; args->argv = argv;
    args->outfname = "-";
    args->output_type = HTS_FT_VCF;

    static struct option loptions[] =
    {
        {"help",0,0,'h'},
        {"include",1,0,'i'},
        {"exclude",1,0,'e'},
        {"output",1,0,'o'},
        {"output-type",1,0,'O'},
        {"regions",1,0,'r'},
        {"regions-file",1,0,'R'},
        {"targets",1,0,'t'},
        {"targets-file",1,0,'T'},
        {"samples",1,0,'s'},
        {"samples-file",1,0,'S'},
        {"gensample",1,0,'g'},
        {"tag",1,0,1},
        {"tsv2vcf",1,0,2},
        {"ref",1,0,'f'},
        {0,0,0,0}
    };
    while ((c = getopt_long(argc, argv, "?hr:R:s:S:t:T:i:e:g:f:o:O:",loptions,NULL)) >= 0) {
        switch (c) {
            case 'e': args->filter_str = optarg; args->filter_logic |= FLT_EXCLUDE; break;
            case 'i': args->filter_str = optarg; args->filter_logic |= FLT_INCLUDE; break;
            case 'r': args->regions_list = optarg; break;
            case 'R': args->regions_list = optarg; args->regions_is_file = 1; break;
            case 't': args->targets_list = optarg; break;
            case 'T': args->targets_list = optarg; args->targets_is_file = 1; break;
            case 's': args->sample_list = optarg; break;
            case 'S': args->sample_list = optarg; args->sample_is_file = 1; break;
            case 'g': args->convert_func = vcf_to_gensample; args->outfname = optarg; break;
            case  1 : args->tag = optarg; break;
            case  2 : args->convert_func = tsv_to_vcf; args->infname = optarg; break;
            case 'f': args->ref_fname = optarg; break;
            case 'o': args->outfname = optarg; break;
            case 'O':
                switch (optarg[0]) {
                    case 'b': args->output_type = HTS_FT_BCF|HTS_GZ; break;
                    case 'u': args->output_type = HTS_FT_BCF; break;
                    case 'z': args->output_type = HTS_FT_VCF|HTS_GZ; break;
                    case 'v': args->output_type = HTS_FT_VCF; break;
                    default: error("The output type \"%s\" not recognised\n", optarg);
                }
                break;
            case 'h':
            case '?': usage();
            default: error("Unknown argument: %s\n", optarg);
        }
    }

    if ( optind>=argc )
    {
        if ( !isatty(fileno((FILE *)stdin)) ) args->infname = "-";
    }
    else args->infname = argv[optind];
    if ( !args->infname || !args->convert_func ) usage();

    args->convert_func(args);

    destroy_data(args);
    free(args);
    return 0;
}



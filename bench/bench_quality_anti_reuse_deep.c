/*
 * UltraFastRng512 V1 benchmark / audit tooling
 * Copyright (c) 2026, Watney-0717.
 * Licensed under the Zero-Clause BSD License (0BSD).
 * See ../license/BENCHMARK-SOURCE-0BSD.txt.
 */

/*
 * UltraFastRng512 POC — Deep Optimization Quality / Anti-Reuse Audit
 *
 * Supplemental audit to bench_quality_options.c. It checks whether PaperView
 * views or FIXED-generation chunks reuse the same underlying Normal starter
 * value pool despite emitting different byte orderings.
 */
#define UFR_EXTERNAL_MAIN 1
#include "../src/UltraFastRng512_MC_NORMALTAIL_OPTIMIZED_V1.c"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#define QPER 256u

typedef struct { int32_t *buf; size_t blocks, cap_blocks; uint32_t seen_views, seen_blocks_per_view; } cap_ctx_t;
static int capq(const int32_t *q, uint64_t gen, uint32_t view, uint32_t block, void *vp) {
    (void)gen; cap_ctx_t *c=(cap_ctx_t*)vp;
    if(c->blocks>=c->cap_blocks) return -2;
    memcpy(c->buf+c->blocks*QPER,q,QPER*sizeof(int32_t)); ++c->blocks;
    if(view+1u>c->seen_views)c->seen_views=view+1u;
    if(block+1u>c->seen_blocks_per_view)c->seen_blocks_per_view=block+1u;
    return 0;
}
static int cmp_i32(const void*a,const void*b){int32_t x=*(const int32_t*)a,y=*(const int32_t*)b;return(x>y)-(x<y);}
static int multiset_equal(const int32_t*a,const int32_t*b,size_t n){
    int32_t*x=malloc(n*sizeof(*x)),*y=malloc(n*sizeof(*y)); if(!x||!y){free(x);free(y);return -1;}
    memcpy(x,a,n*sizeof(*x)); memcpy(y,b,n*sizeof(*y)); qsort(x,n,sizeof(*x),cmp_i32); qsort(y,n,sizeof(*y),cmp_i32);
    int ok=!memcmp(x,y,n*sizeof(*x)); free(x);free(y);return ok;
}
static size_t exact_qblock_overlap(const int32_t*a,const int32_t*b,size_t blocks){
    size_t same=0; for(size_t i=0;i<blocks;++i) for(size_t j=0;j<blocks;++j)
        if(!memcmp(a+i*QPER,b+j*QPER,QPER*sizeof(int32_t))){++same;break;} return same;
}
static uint64_t fnv1a64(const int32_t*p,size_t n){
    uint64_t h=UINT64_C(1469598103934665603); for(size_t i=0;i<n;++i){uint32_t x=(uint32_t)p[i];
      h^=(uint8_t)x;h*=UINT64_C(1099511628211);h^=(uint8_t)(x>>8);h*=UINT64_C(1099511628211);
      h^=(uint8_t)(x>>16);h*=UINT64_C(1099511628211);h^=(uint8_t)(x>>24);h*=UINT64_C(1099511628211);} return h;
}
static int run_one_chunk_views(void){
    ufr_mc_config_t cfg; ufr_mc_config_default(&cfg); cfg.cultivation=UFR_CULT_DIRECT; cfg.final_profile=UFR_FINAL_INT32; cfg.generation=UFR_GENERATION_CHUNK;
    ufr_mc_cultivation_engine_t e; if(ufr_mc_engine_init(&e,&cfg)!=0)return -20;
    const uint32_t views=e.cfg.views_per_chunk, bpv=UFR_PAPER_MC_BLOCKS_PER_VIEW; const size_t spv=(size_t)bpv*QPER, blocks=(size_t)views*bpv;
    cap_ctx_t c={0}; c.cap_blocks=blocks; c.buf=aligned_alloc(64,blocks*QPER*sizeof(int32_t)); if(!c.buf){ufr_mc_engine_destroy(&e);return -21;}
    if(ufr_mc_run(&e,1u,capq,&c)!=0||c.blocks!=blocks){free(c.buf);ufr_mc_engine_destroy(&e);return -22;}
    printf("VIEWS=%u blocks/view=%u scalars/view=%zu\n",views,bpv,spv);
    for(uint32_t v=1;v<views;++v){const int32_t*vv=c.buf+(size_t)v*spv;printf("VIEW0_vs_VIEW%u scalar_multiset_equal=%d exact_q256_replay=%zu/%u\n",v,multiset_equal(c.buf,vv,spv),exact_qblock_overlap(c.buf,vv,bpv),bpv);} 
    free(c.buf);ufr_mc_engine_destroy(&e);return 0;
}
static int run_two_chunk_fixed(uint32_t generation_chunks){
    ufr_mc_config_t cfg; ufr_mc_config_default(&cfg); cfg.cultivation=UFR_CULT_DIRECT; cfg.final_profile=UFR_FINAL_INT32; cfg.generation=UFR_GENERATION_FIXED;
    cfg.min_generation_mode=UFR_GENERATION_FIELD_CUSTOM; cfg.max_generation_mode=UFR_GENERATION_FIELD_CUSTOM; cfg.min_generation_chunks=generation_chunks; cfg.max_generation_chunks=generation_chunks;
    ufr_mc_cultivation_engine_t e; if(ufr_mc_engine_init(&e,&cfg)!=0)return -10;
    const size_t blocks=(size_t)e.cfg.views_per_chunk*UFR_PAPER_MC_BLOCKS_PER_VIEW, scalars=blocks*QPER; cap_ctx_t c={0}; c.cap_blocks=blocks*2u; c.buf=aligned_alloc(64,c.cap_blocks*QPER*sizeof(int32_t)); if(!c.buf){ufr_mc_engine_destroy(&e);return -11;}
    if(ufr_mc_run(&e,2u,capq,&c)!=0||c.blocks!=blocks*2u){free(c.buf);ufr_mc_engine_destroy(&e);return -12;}
    const int32_t*a=c.buf,*b=c.buf+scalars; printf("GEN=%u chunks=2 qblocks/chunk=%zu scalars/chunk=%zu raw_chunk_equal=%d exact_q256_replay=%zu/%zu scalar_multiset_equal=%d hash=(%016" PRIx64 ",%016" PRIx64 ")\n",generation_chunks,blocks,scalars,!memcmp(a,b,scalars*sizeof(int32_t)),exact_qblock_overlap(a,b,blocks),blocks,multiset_equal(a,b,scalars),fnv1a64(a,scalars),fnv1a64(b,scalars));
    free(c.buf);ufr_mc_engine_destroy(&e);return 0;
}
int main(void){pin_cpu0();
puts("UltraFastRng512 DEEP OPTIMIZATION QUALITY / ANTI-REUSE AUDIT: V1");
    puts("byte-level uniqueness and underlying Normal-value-pool reuse are tested separately");
    if(run_one_chunk_views()!=0)return 2; if(run_two_chunk_fixed(1u)!=0)return 3; if(run_two_chunk_fixed(2u)!=0)return 4; if(run_two_chunk_fixed(64u)!=0)return 5; if(run_two_chunk_fixed(1048576u)!=0)return 6;
    puts("Interpretation: raw byte inequality alone is not sufficient; FIXED generation can retain the same Normal starter and re-index it. Scalar-multiset equality and full q256 replay therefore indicate underlying sample-pool reuse even when emitted chunk bytes differ."); return 0; }

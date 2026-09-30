/*
 * UltraFastRng512 V1 benchmark / audit tooling
 * Copyright (c) 2026, Watney-0717.
 * Licensed under the Zero-Clause BSD License (0BSD).
 * See ../license/BENCHMARK-SOURCE-0BSD.txt.
 */

/*
 * UltraFastRng512 POC — Optimization Quality / Anti-Reuse Audit
 *
 * Second public quality test.
 *
 * It checks two separate invariants:
 *   A) Core cultivation/final-profile options preserve the MC-facing Normal
 *      / R2 statistics.
 *   B) Finance/Pharma acceleration options all consume the SAME q256 corpus,
 *      exactly once per logical block, and their aggregate outputs agree with
 *      the deliberate baseline. Full-block duplicate and adjacent-duplicate
 *      guards are used to catch accidental sample copying/replay.
 *
 * Reusing a common transformed value across parameters is legitimate when the
 * mathematical transform is common. The audit therefore distinguishes
 * parameter reuse from sample reuse.
 */
#define UFR_EXTERNAL_MAIN 1
#include "../src/UltraFastRng512_MC_NORMALTAIL_OPTIMIZED_V1.c"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

#define AUDIT_BLOCKS 256u
#define QWORDS_PER_BLOCK 256u
#define CORE_GROUPS_PER_FAMILY 1u

typedef struct { uint64_t key; } keybox_t;
static int cmp_key(const void *a,const void *b){
    const uint64_t x=((const keybox_t*)a)->key,y=((const keybox_t*)b)->key;
    return (x>y)-(x<y);
}

static uint64_t block_hash(const int32_t *q){
    uint64_t h=UINT64_C(0x6A09E667F3BCC909);
    for(unsigned i=0;i<QWORDS_PER_BLOCK;++i){
        uint64_t x=(uint32_t)q[i]+UINT64_C(0x9E3779B97F4A7C15)*(i+1u);
        h^=x+(h<<6)+(h>>2);
        h^=h>>30;h*=UINT64_C(0xBF58476D1CE4E5B9);h^=h>>27;
    }
    return h^(h>>31);
}

typedef struct {
    uint64_t n;
    long double sx,sy,sx2,sy2,sxy,dep2,sx4,sy4,sr2,sr22;
    uint64_t hit;
} qstats_t;

static inline void qs_add(qstats_t *s,double x,double y){
    const double x2=x*x,y2=y*y,r=x2+y2;
    ++s->n;s->sx+=x;s->sy+=y;s->sx2+=x2;s->sy2+=y2;s->sxy+=x*y;
    s->dep2+=(x2-1.0)*(y2-1.0);s->sx4+=x2*x2;s->sy4+=y2*y2;s->sr2+=r;s->sr22+=r*r;
    if(r<=1.0)++s->hit;
}

static void qstats_print(const char *name,const qstats_t *s){
    const long double N=s->n,mx=s->sx/N,my=s->sy/N,ex=s->sx2/N,ey=s->sy2/N;
    const long double vx=ex-mx*mx,vy=ey-my*my;
    const long double corr=(s->sxy/N-mx*my)/sqrtl(vx*vy);
    const long double dep2=(s->dep2/N)/sqrtl((s->sx4/N-ex*ex)*(s->sy4/N-ey*ey));
    const long double er=s->sr2/N,vr=s->sr22/N-er*er;
    printf("%-24s N=%" PRIu64 " var=(%.6Lf,%.6Lf) corr=%+.3Le dep2=%+.3Le E[R2]=%.6Lf VarR2=%.6Lf hit=%.6Lf\n",
           name,s->n,vx,vy,corr,dep2,er,vr,(long double)s->hit/N);
}

static void build_corpus(ufr_mc_cultivation_engine_t *e,int32_t **out,size_t *blocks){
    int32_t *q=(int32_t*)aligned_alloc(64,(size_t)AUDIT_BLOCKS*QWORDS_PER_BLOCK*sizeof(int32_t));
    if(!q){fprintf(stderr,"corpus allocation failed\n");exit(2);}
    size_t bi=0;
    ufrx_load(&e->fm,&e->seedx.handoff_a,UFR_DOMAIN_A);
    for(unsigned g=0;bi<AUDIT_BLOCKS;++g){
        ufr_paper_make_starter(&e->fm,&e->retained_starter);
        const uint64_t epoch=(uint64_t)g<<6;
        const ufr_paper_cultivation_ctx_t cc=ufr_paper_cultivation_ctx(epoch);
        for(unsigned v=0;v<4u && bi<AUDIT_BLOCKS;++v){
            const unsigned vid=(v+cc.phase+(cc.phase>>3))&(UFR_PAPER_MAX_VIEWS-1u);
            const unsigned base=(UFR_PAPER_A64_OFFSET[vid]+cc.shift)&(UFR_PAPER_NV-1u);
            const __m512i ci=ufr_paper_pidx(UFR_PAPER_A64_LANE[vid]);
            const __m512i ti=ufr_paper_tidx(UFR_PAPER_A64_TAIL[vid]);
            for(unsigned b=0;b<UFR_PAPER_MC_BLOCKS_PER_VIEW && bi<AUDIT_BLOCKS;++b,++bi)
                ufr_paper_materialize_block_ctx(&e->retained_starter,base,ci,ti,b,q+(size_t)bi*QWORDS_PER_BLOCK);
        }
    }
    *out=q;*blocks=bi;
}

static void anti_reuse_guard(const int32_t *q,size_t blocks){
    keybox_t *keys=(keybox_t*)malloc(blocks*sizeof(*keys));
    if(!keys)exit(3);
    size_t adjacent=0;
    for(size_t b=0;b<blocks;++b){keys[b].key=block_hash(q+b*QWORDS_PER_BLOCK);if(b&&keys[b].key==keys[b-1].key)++adjacent;}
    qsort(keys,blocks,sizeof(*keys),cmp_key);
    size_t unique=0;for(size_t i=0;i<blocks;++i)if(i==0||keys[i].key!=keys[i-1].key)++unique;
    printf("\n=== SAMPLE REUSE GUARD ===\n");
    printf("blocks presented to every optimization: %zu\n",blocks);
    printf("logical Normal samples per corpus: %zu\n",blocks*QWORDS_PER_BLOCK);
    printf("unique full q256 blocks: %zu / %zu (%.3f%%)\n",unique,blocks,100.0*(double)unique/(double)blocks);
    printf("adjacent identical full blocks: %zu\n",adjacent);
    printf("A full-block duplicate would be a strong copy/replay warning; parameter-level common-transform reuse is tested separately by baseline output agreement.\n");
    free(keys);
}

typedef struct { long double s[8]; uint64_t samples; ufr_mc_v8_profile_id_t selected; } option_result_t;

static int run_domain_option(ufr_mc_integrated_domain_t domain,const void *cfg_domain,
                             ufr_mc_v8_profile_id_t requested,const int32_t *q,size_t blocks,
                             option_result_t *out){
    memset(out,0,sizeof(*out));
    ufr_mc_int_ctx_t c;memset(&c,0,sizeof(c));c.domain=domain;
    if(domain==UFR_MC_DOMAIN_FINANCE){c.fc=*(const ufr_mc_integrated_finance_cfg_t*)cfg_domain;ufr_mc_int_prepare_finance(&c);}
    else {c.pc=*(const ufr_mc_integrated_pharma_cfg_t*)cfg_domain;ufr_mc_int_prepare_pharma(&c);}
    ufr_mc_v9_request_t req;memset(&req,0,sizeof(req));req.domain=(domain==UFR_MC_DOMAIN_FINANCE)?UFR_MC_V9_DOMAIN_FINANCE:UFR_MC_V9_DOMAIN_PHARMA;req.workload=({ufr_mc_v8_workload_t w=ufr_mc_v8_default_workload();w.caps=UFR_MC_V8_CAP_SAMPLE_INDEPENDENT|UFR_MC_V8_CAP_INDEPENDENT_PARAMS|UFR_MC_V8_CAP_COMMON_TRANSFORM|UFR_MC_V8_CAP_PRECOMPUTE_SAFE|UFR_MC_V8_CAP_PRECOMPUTE_LOCAL_FAST|UFR_MC_V8_CAP_PARAM4_PRECOMPUTE_PROVEN|UFR_MC_V8_CAP_INVARIANT_PREP_SAFE|UFR_MC_V8_CAP_INVARIANT_PREP_PROVEN;w.paths_per_batch=1u;w.params_per_batch=8u;w.samples_per_block=256u;w.generation_reuse=64u;w.expected_reuse=64u;w;});req.requested=requested;
    ufr_mc_v9_plan_t plan;if(ufr_mc_v9_embedded_select(&req,&plan)!=0)return -2;c.plan=plan.plan;out->selected=c.plan.selected;
    for(size_t b=0;b<blocks;++b)if(ufr_mc_int_consume(q+b*QWORDS_PER_BLOCK,0u,(uint32_t)(b&63u),(uint32_t)(b&63u),&c)!=0)return -3;
    for(unsigned k=0;k<8u;++k){
        if(domain==UFR_MC_DOMAIN_FINANCE &&
           (c.plan.selected==UFR_MC_V8_PARAM4_PRECOMPUTE || c.plan.selected==UFR_MC_V8_PARAM4_PRECOMPUTE_INVARIANT))
            out->s[k]=(long double)c.scalar_sum[k];
        else if(c.plan.selected==UFR_MC_V8_PARAM4_PRECOMPUTE_INVARIANT && domain==UFR_MC_DOMAIN_PHARMA)
            out->s[k]=(long double)(ufr_mc_reduce16_pd(c.pharma_common_acc4[0])+ufr_mc_reduce16_pd(c.pharma_common_acc4[1])+ufr_mc_reduce16_pd(c.pharma_common_acc4[2])+ufr_mc_reduce16_pd(c.pharma_common_acc4[3]))*(long double)c.pc.dose[k];
        else out->s[k]=(long double)ufr_mc_reduce16_pd(c.acc0[k]);
    }
    out->samples=c.samples;return 0;
}

static long double relerr(long double a,long double b){
    if(!isfinite((double)a)||!isfinite((double)b)) return INFINITY;
    return fabsl(a-b)/(fabsl(a)>1.0L?fabsl(a):1.0L);
}

static void audit_domain(const char *tag,ufr_mc_integrated_domain_t domain,const void *cfg,const int32_t *q,size_t blocks){
    const ufr_mc_v8_profile_id_t ids[]={UFR_MC_V8_AUTO,UFR_MC_V8_BASELINE,UFR_MC_V8_VEC8,UFR_MC_V8_PARAM8_SHARED,UFR_MC_V8_PARAM4_PRECOMPUTE,UFR_MC_V8_PARAM8_PRECOMPUTE,UFR_MC_V8_PARAM4_PRECOMPUTE_INVARIANT};
    option_result_t base;
    if(run_domain_option(domain,cfg,UFR_MC_V8_BASELINE,q,blocks,&base)!=0){printf("%s baseline unavailable\n",tag);return;}
    printf("\n=== %s OPTIMIZATION QUALITY ===\n",tag);
    for(size_t i=0;i<sizeof(ids)/sizeof(ids[0]);++i){
        option_result_t r;int rc=run_domain_option(domain,cfg,ids[i],q,blocks,&r);
        if(rc!=0){printf("%-30s UNAVAILABLE rc=%d\n",ufr_mc_v8_profile_name(ids[i]),rc);continue;}
        long double maxre=0;for(unsigned k=0;k<8u;++k){long double e=relerr(base.s[k],r.s[k]);if(e>maxre)maxre=e;}
        printf("%-30s selected=%-28s samples=%" PRIu64 " sample_ratio=%.9Lf max_relerr_vs_BASELINE=%.3Le\n",
               ufr_mc_v8_profile_name(ids[i]),ufr_mc_v8_profile_name(r.selected),r.samples,
               base.samples?(long double)r.samples/(long double)base.samples:0.0L,maxre);
    }
    printf("Note: parameter-level common-transform reuse is intentional for shared/precomputed options; the fixed input corpus and sample count are held identical here.\n");
}

static void core_profile_quality(ufr_mc_cultivation_engine_t *e){
    static const struct {
        const char *name;
        ufr_cultivation_mode_t cult;
        ufr_final_cultivation_profile_t prof;
    } cases[]={
        {"DIRECT/R2_FLOAT32",UFR_CULT_DIRECT,UFR_FINAL_R2_FLOAT32},
        {"DIRECT/FLOAT32",UFR_CULT_DIRECT,UFR_FINAL_FLOAT32},
        {"DIRECT/INT32",UFR_CULT_DIRECT,UFR_FINAL_INT32},
        {"DIRECT/RAW_INT16",UFR_CULT_DIRECT,UFR_FINAL_RAW_INT16},
        {"L1_CACHE/FLOAT32",UFR_CULT_L1_CACHE,UFR_FINAL_FLOAT32},
        {"L2_CACHE/FLOAT32",UFR_CULT_L2_CACHE,UFR_FINAL_FLOAT32}
    };
    printf("\n=== CORE CULTIVATION / FINAL-PROFILE QUALITY ===\n");
    for(size_t ci=0;ci<sizeof(cases)/sizeof(cases[0]);++ci){
        qstats_t st={0};
        for(unsigned fam=0;fam<8u;++fam){
            if(fam==0u) ufrx_load(&e->fm,&e->seedx.handoff_a,UFR_DOMAIN_A);
            else if(fam==1u) ufrx_load(&e->fm,&e->seedx.handoff_b,UFR_DOMAIN_B);
            else {
                UFRXWorker *w=(fam&1u)?&e->seedx.back_b:&e->seedx.back_a;
                w->pending_ready=0u;
                if(!ufrx_prepare_next(w)) { fprintf(stderr,"prepare_next failed family=%u\n",fam); return; }
                UFRXSlot tmp; memset(&tmp,0,sizeof(tmp));
                if(!ufrx_copy_set_to_slot(&tmp,&w->pending_set)) { fprintf(stderr,"copy_set failed\n"); return; }
                ufrx_load(&e->fm,&tmp,(fam&1u)?UFR_DOMAIN_B:UFR_DOMAIN_A);
            }
            for(unsigned g=0;g<CORE_GROUPS_PER_FAMILY;++g){
                ufr_paper_make_starter(&e->fm,&e->retained_starter);
                const uint64_t epoch=((uint64_t)fam<<32)|((uint64_t)g<<6);
                const ufr_paper_cultivation_ctx_t cc=ufr_paper_cultivation_ctx(epoch);
                if(cases[ci].prof==UFR_FINAL_R2_FLOAT32){
                    const float scale=UFR_R2_NORMAL_SCALE*0.7071067811865475244f;
                    for(unsigned v=0;v<4u;++v){
                        const unsigned vid=(v+cc.phase+(cc.phase>>3))&31u;
                        unsigned src0=(unsigned)(((uint64_t)UFR_R2_PAIR_STRIDE*((UFR_PAPER_A64_OFFSET[vid]+cc.shift)&1023u))&1023u);
                        const __m512i ix=ufr_paper_pidx(UFR_PAPER_A64_LANE[vid]);
                        const __m512i tx=ufr_paper_tidx(UFR_PAPER_A64_TAIL[vid]);
                        for(unsigned k=0;k<512u;++k){
                            const unsigned src1=(src0+UFR_R2_PAIR_STRIDE)&1023u;
                            const __m512i a=_mm512_permutexvar_epi32(
                                e->retained_starter.tail_mask[src0]?tx:ix,
                                _mm512_cvtepi16_epi32(_mm256_load_si256((const __m256i*)(e->retained_starter.starter+(size_t)src0*16u))));
                            const __m512i b=_mm512_permutexvar_epi32(
                                e->retained_starter.tail_mask[src1]?tx:ix,
                                _mm512_cvtepi16_epi32(_mm256_load_si256((const __m256i*)(e->retained_starter.starter+(size_t)src1*16u))));
                            int32_t ax[16],bx[16];
                            _mm512_store_si512(ax,a); _mm512_store_si512(bx,b);
                            for(unsigned lane=0;lane<16u;++lane)
                                qs_add(&st,(double)ax[lane]*scale,(double)bx[lane]*scale);
                            src0=(src0+((UFR_R2_PAIR_STRIDE<<1)&1023u))&1023u;
                        }
                    }
                } else {
                    if(ufr_paper_final_cache_build(e,4,cc.phase,epoch,cases[ci].prof)!=0) continue;
                    const size_t bpv=ufr_final_cache_bytes_per_view(cases[ci].prof,0);
                    const float scale=UF_NORMAL_SCALE*0.7071067811865475244f;
                    for(unsigned v=0;v<4u;++v){
                        const unsigned vid=(v+cc.phase+(cc.phase>>3))&31u;
                        const unsigned base=(UFR_PAPER_A64_OFFSET[vid]+cc.shift)&1023u;
                        const uint8_t *vc=(const uint8_t*)e->paper_final_cache+(size_t)v*bpv;
                        for(unsigned k=0;k<512u;++k){
                            const unsigned j0=(base+2u*k)&1023u, j1=(j0+1u)&1023u;
                            int32_t ax[16],bx[16];
                            if(cases[ci].prof==UFR_FINAL_RAW_INT16){
                                const __m256i a16=_mm256_load_si256((const __m256i*)(vc+(size_t)j0*32u));
                                const __m256i b16=_mm256_load_si256((const __m256i*)(vc+(size_t)j1*32u));
                                _mm512_store_si512(ax,_mm512_cvtepi16_epi32(a16));
                                _mm512_store_si512(bx,_mm512_cvtepi16_epi32(b16));
                                for(unsigned lane=0;lane<16u;++lane)
                                    qs_add(&st,(double)ax[lane]*scale,(double)bx[lane]*scale);
                            } else if(cases[ci].prof==UFR_FINAL_INT32){
                                _mm512_store_si512(ax,_mm512_load_si512((const void*)(vc+(size_t)j0*64u)));
                                _mm512_store_si512(bx,_mm512_load_si512((const void*)(vc+(size_t)j1*64u)));
                                for(unsigned lane=0;lane<16u;++lane)
                                    qs_add(&st,(double)ax[lane]*scale,(double)bx[lane]*scale);
                            } else {
                                const __m512 a=_mm512_load_ps((const float*)(vc+(size_t)j0*64u));
                                const __m512 b=_mm512_load_ps((const float*)(vc+(size_t)j1*64u));
                                float af[16],bf[16];
                                _mm512_store_ps(af,a); _mm512_store_ps(bf,b);
                                for(unsigned lane=0;lane<16u;++lane)
                                    qs_add(&st,(double)af[lane]*scale,(double)bf[lane]*scale);
                            }
                        }
                    }
                }
            }
        }
        qstats_print(cases[ci].name,&st);
    }
}
int main(void){
    pin_cpu0();
    ufr_mc_config_t cfg;ufr_mc_config_default(&cfg);cfg.cultivation=UFR_CULT_DIRECT;cfg.final_profile=UFR_FINAL_INT32;
    ufr_mc_cultivation_engine_t e;if(ufr_mc_engine_init(&e,&cfg)!=0)return 3;
printf("UltraFastRng512 OPTIMIZATION QUALITY / ANTI-REUSE AUDIT: V1\n");
    printf("one-core complete | core profiles + Finance/Pharma options\n");
    int32_t *q=NULL;size_t blocks=0;build_corpus(&e,&q,&blocks);anti_reuse_guard(q,blocks);
    core_profile_quality(&e);
    const ufr_mc_integrated_finance_cfg_t fc={.spot=100.0f,.rate=0.03f,.vol=0.20f,.maturity=1.0f,.strikes={80,85,90,95,100,105,110,115}};
    const ufr_mc_integrated_pharma_cfg_t pc={.clearance_lph=5.0f,.volume_l=50.0f,.obs_hour=8.0f,.dose={10,12,14,16,18,20,22,24}};
    audit_domain("Finance",UFR_MC_DOMAIN_FINANCE,&fc,q,blocks);
    audit_domain("Pharma",UFR_MC_DOMAIN_PHARMA,&pc,q,blocks);
    printf("\nThe anti-reuse test catches exact full-block replay/copying; the output-equivalence test checks estimator-preserving acceleration. Neither is a proof of statistical independence.\n");
    free(q);ufr_mc_engine_destroy(&e);return 0;
}

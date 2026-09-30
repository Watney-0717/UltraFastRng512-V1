/*
 * UltraFastRng512 V1 benchmark / audit tooling
 * Copyright (c) 2026, Watney-0717.
 * Licensed under the Zero-Clause BSD License (0BSD).
 * See ../license/BENCHMARK-SOURCE-0BSD.txt.
 */

/*
 * UltraFastRng512 POC — Primary MC-Input Quality
 *
 * This is the first of the two public quality tests.
 * It measures the actual R2 x/y values at the MC boundary, immediately before
 * the R^2 = x^2 + y^2 operation, after the exact 555 pair geometry and R2
 * normalization used by the V1 representative R2 path.
 */
#define UFR_EXTERNAL_MAIN 1
#include "../src/UltraFastRng512_MC_NORMALTAIL_OPTIMIZED_V1.c"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

typedef struct {
    uint64_t n;
    long double sx,sy,sx2,sy2,sx3,sy3,sx4,sy4,sxy,dep2,sr2,sr22;
    uint64_t hit,sign,t1,t2,t3,t4;
    double prevx,prevy;
    int have_prev;
    long double lagx,lagy;
} stats_t;

static inline void add_pair(stats_t *s,double x,double y){
    const double x2=x*x,y2=y*y,r=x2+y2;
    ++s->n; s->sx+=x; s->sy+=y; s->sx2+=x2; s->sy2+=y2;
    s->sx3+=x2*x; s->sy3+=y2*y; s->sx4+=x2*x2; s->sy4+=y2*y2;
    s->sxy+=x*y; s->dep2+=(x2-1.0)*(y2-1.0); s->sr2+=r; s->sr22+=r*r;
    if(r<=1.0)s->hit++; if((x<0)!=(y<0))s->sign++;
    if(fabs(x)>=1.0)s->t1++; if(fabs(x)>=2.0)s->t2++; if(fabs(x)>=3.0)s->t3++; if(fabs(x)>=4.0)s->t4++;
    if(s->have_prev){s->lagx+=(long double)s->prevx*x;s->lagy+=(long double)s->prevy*y;}
    s->prevx=x;s->prevy=y;s->have_prev=1;
}

static void print_stats(const char *name,const stats_t *s){
    const long double N=s->n,mx=s->sx/N,my=s->sy/N,ex=s->sx2/N,ey=s->sy2/N;
    const long double vx=ex-mx*mx,vy=ey-my*my;
    const long double corr=(s->sxy/N-mx*my)/sqrtl(vx*vy);
    const long double dep2=(s->dep2/N)/sqrtl((s->sx4/N-ex*ex)*(s->sy4/N-ey*ey));
    const long double er2=s->sr2/N,vr2=s->sr22/N-er2*er2;
    const long double sk=(s->sx3/N-3*mx*ex+2*mx*mx*mx)/powl(vx,1.5L);
    const long double kurt=(s->sx4/N-4*mx*s->sx3/N+6*mx*mx*ex-3*powl(mx,4))/powl(vx,2);
    const long double lagx=(s->lagx/(N-1.0L)-mx*mx)/vx;
    const long double lagy=(s->lagy/(N-1.0L)-my*my)/vy;
    printf("\n[%s]\n",name);
    printf("N=%" PRIu64 " pairs | mean=(%+.9Lf,%+.9Lf) | var=(%.9Lf,%.9Lf)\n",s->n,mx,my,vx,vy);
    printf("skew_x=%+.7Lf kurt_x=%.7Lf corr=%+.9Lf dep2=%+.9Lf lag1=(%+.9Lf,%+.9Lf)\n",sk,kurt,corr,dep2,lagx,lagy);
    printf("E[R2]=%.9Lf Var[R2]=%.9Lf hit=%.9Lf sign=%.9Lf tails=(%.9Lf,%.9Lf,%.9Lf,%.9Lf)\n",
           er2,vr2,(long double)s->hit/N,(long double)s->sign/N,
           (long double)s->t1/N,(long double)s->t2/N,(long double)s->t3/N,(long double)s->t4/N);
}

static void load_family(ufr_mc_cultivation_engine_t *e,unsigned family){
    if(family==0u){ufrx_load(&e->fm,&e->seedx.handoff_a,UFR_DOMAIN_A);return;}
    if(family==1u){ufrx_load(&e->fm,&e->seedx.handoff_b,UFR_DOMAIN_B);return;}
    UFRXWorker *w=(family&1u)?&e->seedx.back_b:&e->seedx.back_a;
    w->pending_ready=0u;
    if(!ufrx_prepare_next(w)){fprintf(stderr,"prepare_next failed family=%u\n",family);exit(4);}
    UFRXSlot tmp;memset(&tmp,0,sizeof(tmp));
    if(!ufrx_copy_set_to_slot(&tmp,&w->pending_set)){fprintf(stderr,"copy_set failed\n");exit(4);}
    ufrx_load(&e->fm,&tmp,(family&1u)?UFR_DOMAIN_B:UFR_DOMAIN_A);
}

static void scan_r2(stats_t *s,const ufr_paper_starter_t *st,ufr_paper_cultivation_ctx_t cc){
    const float scale=UFR_R2_NORMAL_SCALE*0.7071067811865475244f;
    for(unsigned v=0;v<4u;++v){
        const unsigned vid=(v+cc.phase+(cc.phase>>3))&31u;
        unsigned src0=(unsigned)(((uint64_t)UFR_R2_PAIR_STRIDE*((UFR_PAPER_A64_OFFSET[vid]+cc.shift)&1023u))&1023u);
        const __m512i ci=ufr_paper_pidx(UFR_PAPER_A64_LANE[vid]);
        const __m512i ti=ufr_paper_tidx(UFR_PAPER_A64_TAIL[vid]);
        for(unsigned k=0;k<512u;++k){
            const unsigned src1=(src0+UFR_R2_PAIR_STRIDE)&1023u;
            const __m256i a16=_mm256_load_si256((const __m256i*)(st->starter+(size_t)src0*16u));
            const __m256i b16=_mm256_load_si256((const __m256i*)(st->starter+(size_t)src1*16u));
            const __m512i a=_mm512_permutexvar_epi32(st->tail_mask[src0]?ti:ci,_mm512_cvtepi16_epi32(a16));
            const __m512i b=_mm512_permutexvar_epi32(st->tail_mask[src1]?ti:ci,_mm512_cvtepi16_epi32(b16));
            int32_t ax[16],bx[16];_mm512_store_si512(ax,a);_mm512_store_si512(bx,b);
            for(unsigned lane=0;lane<16u;++lane)add_pair(s,(double)ax[lane]*scale,(double)bx[lane]*scale);
            src0=(src0+((UFR_R2_PAIR_STRIDE<<1)&1023u))&1023u;
        }
    }
}

int main(int argc,char **argv){
    const unsigned groups=argc>1?(unsigned)strtoul(argv[1],0,10):8u;
    if(!groups)return 2;
    pin_cpu0();
    ufr_mc_config_t cfg;ufr_mc_config_default(&cfg);
    cfg.cultivation=UFR_CULT_DIRECT;cfg.final_profile=UFR_FINAL_R2_FLOAT32;
    ufr_mc_cultivation_engine_t e;
    if(ufr_mc_engine_init(&e,&cfg)!=0)return 3;
printf("UltraFastRng512 PRIMARY MC-INPUT QUALITY: V1\n");
    printf("one-core complete | families=8 | groups/family=%u | four views/group\n",groups);
    printf("Quality point: x/y immediately before the R2 MC operation.\n");
    stats_t r2={0};
    for(unsigned fam=0;fam<8u;++fam){
        load_family(&e,fam);
        for(unsigned g=0;g<groups;++g){
            ufr_paper_make_starter(&e.fm,&e.retained_starter);
            const uint64_t epoch=((uint64_t)fam<<32)|((uint64_t)g<<6);
            scan_r2(&r2,&e.retained_starter,ufr_paper_cultivation_ctx(epoch));
        }
    }
    print_stats("R2_FLOAT32 actual MC input",&r2);
    printf("\nIdeal N(0,1)-pair reference: var=1, kurtosis=3, corr=0, dep2=0, E[R2]=2, Var[R2]=4, hit=0.3934693403, sign=0.5\n");
    printf("This test is an MC-input statistical screen, not a named-suite certification.\n");
    ufr_mc_engine_destroy(&e);return 0;
}

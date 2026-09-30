#define _POSIX_C_SOURCE 200809L
#include "UltraFastRng512_MC_Pharma_VPop_Uncertainty_v1.h"

#include <openssl/evp.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define UFR_VPOP_MAX_RETRIES 256u
#define UFR_VPOP_INV_2P53 1.1102230246251565e-16
#define UFR_VPOP_Z95 1.959963984540054

static int sha512_seed_counter(const uint8_t seed[64], uint64_t counter, uint8_t out[64]) {
    static const uint8_t domain[] = "UFR-MC-PHARMA-VPOP-RNG-V1";
    uint8_t ctr[8];
    for (unsigned i=0;i<8u;++i) ctr[i]=(uint8_t)(counter >> (56u-8u*i));
    EVP_MD_CTX *ctx=EVP_MD_CTX_new(); unsigned got=0u;
    if(!ctx||!out){EVP_MD_CTX_free(ctx);return -1;}
    if(EVP_DigestInit_ex(ctx,EVP_sha512(),NULL)!=1||
       EVP_DigestUpdate(ctx,domain,sizeof(domain)-1u)!=1||
       EVP_DigestUpdate(ctx,seed,64u)!=1||
       EVP_DigestUpdate(ctx,ctr,8u)!=1||
       EVP_DigestFinal_ex(ctx,out,&got)!=1||got!=64u){EVP_MD_CTX_free(ctx);return -1;}
    EVP_MD_CTX_free(ctx);
    return 0;
}

static double u01_from_u64(uint64_t x) {
    /* Open interval (0,1), consuming the top 53 bits. */
    return ((double)(x >> 11) + 0.5) * UFR_VPOP_INV_2P53;
}

static double normal_from_pair(uint64_t a, uint64_t b) {
    const double u1=u01_from_u64(a);
    const double u2=u01_from_u64(b);
    return sqrt(-2.0*log(u1))*cos(6.2831853071795864769252867665590057683943388*u2);
}

static int sample_normals(const uint8_t seed[64], uint32_t pcount, uint32_t attempt,
                          double *z, uint32_t *draw_count) {
    uint8_t buf[64];
    uint64_t words[16];
    unsigned word_count=0u;
    unsigned counter=attempt*2u;
    for(unsigned chunk=0;chunk<2u; ++chunk){
        if(sha512_seed_counter(seed,(uint64_t)counter+chunk,buf)!=0) return -1;
        for(unsigned j=0;j<8u;++j){
            uint64_t x=0u;
            for(unsigned k=0;k<8u;++k) x=(x<<8)|buf[j*8u+k];
            words[word_count++]=x;
        }
    }
    unsigned n=0u;
    for(unsigned i=0;i<pcount;++i){
        z[i]=normal_from_pair(words[n],words[n+1u]); n+=2u;
    }
    if(draw_count) *draw_count=n;
    memset(buf,0,sizeof(buf));
    memset(words,0,sizeof(words));
    return 0;
}

static int validate_param(const ufr_pharma_vpop_param_t *p) {
    if(!p || !p->name[0] || !isfinite(p->mean) || !isfinite(p->cv) ||
       p->mean<=0.0 || p->cv<0.0) return 0;
    if(p->distribution!=UFR_PHARMA_PARAM_LOGNORMAL && p->distribution!=UFR_PHARMA_PARAM_NORMAL) return 0;
    if(!isfinite(p->min_value) || !isfinite(p->max_value) ||
       p->min_value<0.0 || p->max_value<=p->min_value) return 0;
    if(p->min_value>p->mean || p->max_value<p->mean) return 0;
    return 1;
}

static int validate_cholesky(const ufr_pharma_vpop_spec_t *s) {
    if(!s->use_cholesky) return 1;
    const unsigned n=s->param_count;
    for(unsigned i=0;i<n;++i){
        if(!isfinite(s->cholesky[i*UFR_PHARMA_VPOP_MAX_PARAMS+i]) || s->cholesky[i*UFR_PHARMA_VPOP_MAX_PARAMS+i]<=0.0) return 0;
        for(unsigned j=i+1u;j<n;++j) if(fabs(s->cholesky[i*UFR_PHARMA_VPOP_MAX_PARAMS+j])>1e-15) return 0;
        for(unsigned j=0;j<=i;++j) if(!isfinite(s->cholesky[i*UFR_PHARMA_VPOP_MAX_PARAMS+j])) return 0;
    }
    return 1;
}

void ufr_pharma_vpop_spec_default(ufr_pharma_vpop_spec_t *s) {
    if(!s) return;
    memset(s,0,sizeof(*s));
    s->api_version=UFR_PHARMA_VPOP_API_VERSION;
    s->param_count=5u;
    s->subjects=4096u;
    s->stream_plan.api_version=UFR_MC_STREAM_API_VERSION;
    ufr_mc_stream_plan_default(&s->stream_plan);
    static const char *names[5]={"CL","Vc","Q","Vp","Ka"};
    static const double means[5]={4.0,25.0,10.0,15.0,1.2};
    static const double cvs[5]={0.30,0.20,0.25,0.20,0.30};
    static const double mins[5]={0.25,5.0,1.0,3.0,0.05};
    static const double maxs[5]={20.0,100.0,50.0,80.0,10.0};
    for(unsigned i=0;i<5u;++i){
        snprintf(s->params[i].name,sizeof(s->params[i].name),"%s",names[i]);
        s->params[i].mean=means[i]; s->params[i].cv=cvs[i];
        s->params[i].min_value=mins[i]; s->params[i].max_value=maxs[i];
        s->params[i].distribution=UFR_PHARMA_PARAM_LOGNORMAL;
    }
}

int ufr_pharma_vpop_spec_validate(const ufr_pharma_vpop_spec_t *s) {
    if(!s || s->api_version!=UFR_PHARMA_VPOP_API_VERSION ||
       s->param_count==0u || s->param_count>UFR_PHARMA_VPOP_MAX_PARAMS ||
       s->subjects==0u || ufr_mc_stream_plan_validate(&s->stream_plan)!=0) return -1;
    if(s->stream_plan.block_count!=0u && s->stream_plan.block_count<s->subjects) return -2;
    for(unsigned i=0;i<s->param_count;++i) if(!validate_param(&s->params[i])) return -3;
    if(!validate_cholesky(s)) return -4;
    return 0;
}

static int derive_subject_normal_vector(const uint8_t seed[64], unsigned pcount, double *z) {
    uint8_t buf[64];
    uint64_t words[16];
    unsigned nw=0u;
    for(unsigned chunk=0;chunk<2u;++chunk){
        if(sha512_seed_counter(seed,chunk,buf)!=0) return -1;
        for(unsigned j=0;j<8u;++j){
            uint64_t x=0u;
            for(unsigned k=0;k<8u;++k) x=(x<<8)|buf[j*8u+k];
            words[nw++]=x;
        }
    }
    unsigned n=0u;
    for(unsigned i=0;i<pcount;++i){
        z[i]=normal_from_pair(words[n],words[n+1u]); n+=2u;
    }
    memset(buf,0,sizeof(buf)); memset(words,0,sizeof(words));
    return 0;
}

static double sample_value(const ufr_pharma_vpop_param_t *p, double z) {
    if(p->distribution==UFR_PHARMA_PARAM_LOGNORMAL){
        const double sig=sqrt(log1p(p->cv*p->cv));
        const double mu=log(p->mean)-0.5*sig*sig;
        return exp(mu+sig*z);
    }
    return p->mean*(1.0+p->cv*z);
}

int ufr_pharma_vpop_generate(const ufr_pharma_vpop_spec_t *s, ufr_pharma_vpop_t *out) {
    if(!out) return -1;
    memset(out,0,sizeof(*out));
    const int vr=ufr_pharma_vpop_spec_validate(s); if(vr!=0) return vr;
    if(s->subjects > SIZE_MAX/((size_t)s->param_count*sizeof(double))) return -5;
    const size_t total=(size_t)s->subjects*(size_t)s->param_count;
    double *vals=(double*)calloc(total,sizeof(double));
    if(!vals) return -6;
    uint8_t subj_seed[64];
    double z[UFR_PHARMA_VPOP_MAX_PARAMS];
    double cz[UFR_PHARMA_VPOP_MAX_PARAMS];
    for(uint64_t si=0;si<s->subjects;++si){
        if(ufr_mc_stream_derive_seed64(&s->stream_plan,si,subj_seed)!=0){free(vals);return -7;}
        int accepted=0;
        for(unsigned attempt=0;attempt<UFR_VPOP_MAX_RETRIES && !accepted;++attempt){
            if(attempt==0u){ if(derive_subject_normal_vector(subj_seed,s->param_count,z)!=0){free(vals);return -8;} }
            else { if(sample_normals(subj_seed,s->param_count,attempt,z,NULL)!=0){free(vals);return -8;} }
            for(unsigned i=0;i<s->param_count;++i){
                double v=0.0;
                if(s->use_cholesky){
                    double q=0.0; for(unsigned j=0;j<=i;++j) q+=s->cholesky[i*UFR_PHARMA_VPOP_MAX_PARAMS+j]*z[j]; cz[i]=q;
                } else cz[i]=z[i];
                v=sample_value(&s->params[i],cz[i]);
                vals[(size_t)i*s->subjects+(size_t)si]=v;
                if(!isfinite(v) || v<s->params[i].min_value || v>s->params[i].max_value) { accepted=0; goto retry; }
            }
            accepted=1;
retry:;
        }
        if(!accepted){
            free(vals); memset(subj_seed,0,sizeof(subj_seed)); return -9;
        }
    }
    memset(subj_seed,0,sizeof(subj_seed)); memset(z,0,sizeof(z)); memset(cz,0,sizeof(cz));
    out->subjects=s->subjects; out->param_count=s->param_count; out->values=vals;
    if(ufr_mc_stream_make_id(&s->stream_plan,out->stream_id)!=0){ufr_pharma_vpop_free(out);return -10;}
    if(ufr_pharma_vpop_hash(out,out->population_sha256)!=0){ufr_pharma_vpop_free(out);return -11;}
    return 0;
}

void ufr_pharma_vpop_free(ufr_pharma_vpop_t *p) {
    if(!p) return;
    if(p->values && p->param_count <= UFR_PHARMA_VPOP_MAX_PARAMS &&
       (size_t)p->subjects <= SIZE_MAX / ((size_t)p->param_count * sizeof(double))) {
        memset(p->values,0,(size_t)p->subjects*p->param_count*sizeof(double));
        free(p->values);
    }
    memset(p,0,sizeof(*p));
}

int ufr_pharma_vpop_hash(const ufr_pharma_vpop_t *p,uint8_t out[32]) {
    if(!p||!out||!p->values||p->param_count==0u||p->subjects==0u) return -1;
    if((size_t)p->subjects>(SIZE_MAX/((size_t)p->param_count*sizeof(double)))) return -2;
    EVP_MD_CTX *ctx=EVP_MD_CTX_new();
    unsigned got=0u;
    if(!ctx) return -3;
    if(EVP_DigestInit_ex(ctx,EVP_sha256(),NULL)!=1){EVP_MD_CTX_free(ctx);return -3;}
    for(unsigned pi=0;pi<p->param_count;++pi){
        const double *row=p->values+(size_t)pi*p->subjects;
        for(uint64_t i=0;i<p->subjects;++i){
            uint64_t bits=0u; unsigned char be[8];
            memcpy(&bits,&row[i],sizeof(bits));
            for(unsigned k=0;k<8u;++k) be[k]=(unsigned char)(bits>>(56u-8u*k));
            if(EVP_DigestUpdate(ctx,be,8u)!=1){EVP_MD_CTX_free(ctx);return -3;}
        }
    }
    if(EVP_DigestFinal_ex(ctx,out,&got)!=1||got!=32u){EVP_MD_CTX_free(ctx);return -3;}
    EVP_MD_CTX_free(ctx); return 0;
}

static int cmp_double(const void *a,const void *b){
    const double x=*(const double*)a,y=*(const double*)b;
    return (x>y)-(x<y);
}
static double quantile_sorted(const double *x,size_t n,double q){
    if(!n) return NAN;
    if(n==1u) return x[0];
    double pos=q*(double)(n-1u);
    size_t i=(size_t)floor(pos);
    size_t j=i+1u;
    if(j>=n) return x[n-1u];
    return x[i]+(x[j]-x[i])*(pos-(double)i);
}

int ufr_pharma_vpop_summary(const ufr_pharma_vpop_t *p,uint32_t pi,ufr_pharma_vpop_summary_t *out){
    if(!p||!out||pi>=p->param_count||!p->values||p->subjects==0u) return -1;
    const double *x=p->values+(size_t)pi*p->subjects;
    double mean=0.0,m2=0.0,mn=INFINITY,mx=-INFINITY;
    for(uint64_t i=0;i<p->subjects;++i){
        const double v=x[i]; if(!isfinite(v)) return -2; const uint64_t n=i+1u; const double d=v-mean; mean+=d/(double)n; m2+=d*(v-mean); if(v<mn)mn=v;if(v>mx)mx=v;
    }
    double *tmp=(double*)malloc((size_t)p->subjects*sizeof(double)); if(!tmp) return -3;
    memcpy(tmp,x,(size_t)p->subjects*sizeof(double)); qsort(tmp,(size_t)p->subjects,sizeof(double),cmp_double);
    memset(out,0,sizeof(*out)); out->count=p->subjects; out->mean=mean; out->standard_deviation=sqrt(p->subjects>1u?m2/(double)(p->subjects-1u):0.0); out->cv=mean!=0.0?out->standard_deviation/fabs(mean):NAN; out->min_value=mn; out->max_value=mx; out->p05=quantile_sorted(tmp,(size_t)p->subjects,0.05); out->p50=quantile_sorted(tmp,(size_t)p->subjects,0.50); out->p95=quantile_sorted(tmp,(size_t)p->subjects,0.95);
    free(tmp); return 0;
}

static int scenario_valid(const ufr_pharma_uncertainty_scenario_t *s,unsigned pcount){
    if(!s||s->scenario_id==0u) return 0;
    for(unsigned i=0;i<pcount;++i) if(!isfinite(s->multiplier[i])||!isfinite(s->additive[i])) return 0;
    return 1;
}

typedef struct {uint64_t n; double mean; double m2;} online_t;
static void online_add(online_t *s,double x){const uint64_t n=s->n+1u;const double d=x-s->mean;s->mean+=d/(double)n;s->m2+=d*(x-s->mean);s->n=n;}
static double online_var(const online_t *s){return s->n>1u?s->m2/(double)(s->n-1u):0.0;}

int ufr_pharma_uncertainty_run(
    const ufr_pharma_vpop_t *pop,
    const ufr_pharma_uncertainty_scenario_t *sc,
    uint32_t sc_count,
    uint32_t ref,
    ufr_pharma_subject_evaluator_fn eval,
    void *ctx,
    ufr_pharma_uncertainty_result_t *out,
    size_t cap)
{
    if(!pop||!sc||!eval||!out||sc_count==0u||sc_count>UFR_PHARMA_VPOP_MAX_SCENARIOS||cap<sc_count||ref>=sc_count||!pop->values||pop->subjects==0u) return -1;
    for(uint32_t j=0;j<sc_count;++j) {
        if(!scenario_valid(&sc[j],pop->param_count)) return -2;
        for(uint32_t k=0;k<j;++k)
            if(sc[k].scenario_id == sc[j].scenario_id) return -2;
    }
    double *base=(double*)calloc((size_t)pop->subjects,sizeof(double)); if(!base) return -3;
    for(uint64_t i=0;i<pop->subjects;++i){
        double ep[UFR_PHARMA_VPOP_MAX_PARAMS];
        for(unsigned p=0;p<pop->param_count;++p) ep[p]=pop->values[(size_t)p*pop->subjects+(size_t)i]*sc[ref].multiplier[p]+sc[ref].additive[p];
        base[i]=eval(ep,pop->param_count,i,sc[ref].scenario_id,ctx); if(!isfinite(base[i])) {free(base);return -4;}
    }
    for(uint32_t j=0;j<sc_count;++j){
        online_t raw={0},paired={0}; uint64_t invalid=0u;
        for(uint64_t i=0;i<pop->subjects;++i){
            double ep[UFR_PHARMA_VPOP_MAX_PARAMS]; for(unsigned p=0;p<pop->param_count;++p) ep[p]=pop->values[(size_t)p*pop->subjects+(size_t)i]*sc[j].multiplier[p]+sc[j].additive[p];
            const double y=eval(ep,pop->param_count,i,sc[j].scenario_id,ctx);
            if(!isfinite(y)){++invalid;continue;}
            online_add(&raw,y); online_add(&paired,y-base[i]);
        }
        const double var=online_var(&raw);
        const double sem=(raw.n>1u)?sqrt(var/(double)raw.n):NAN;
        const double hw=UFR_VPOP_Z95*sem;
        const double pvar=online_var(&paired);
        const double psem=(paired.n>1u)?sqrt(pvar/(double)paired.n):NAN;
        const double phw=UFR_VPOP_Z95*psem;
        ufr_pharma_uncertainty_result_t *r=&out[j]; memset(r,0,sizeof(*r));
        r->scenario_id=sc[j].scenario_id; r->samples=raw.n; r->invalid_values=invalid;
        r->mean=raw.mean; r->variance=var; r->sem=sem;
        r->ci95_low=isfinite(hw)?raw.mean-hw:NAN;
        r->ci95_high=isfinite(hw)?raw.mean+hw:NAN;
        r->ci95_halfwidth=hw; r->paired_delta_mean=paired.mean;
        r->paired_delta_variance=pvar; r->paired_delta_sem=psem;
        r->paired_ci95_low=isfinite(phw)?paired.mean-phw:NAN;
        r->paired_ci95_high=isfinite(phw)?paired.mean+phw:NAN;
        r->finite_ok=(uint8_t)(invalid==0u && raw.n>1u && isfinite(sem) && isfinite(hw));
        r->paired_finite_ok=(uint8_t)(paired.n>1u && isfinite(psem) && isfinite(phw));
        snprintf(r->method,sizeof(r->method),"normal-approx-95%% + CRN-paired");
    }
    free(base); return 0;
}

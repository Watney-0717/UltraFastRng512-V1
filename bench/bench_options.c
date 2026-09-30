/*
 * UltraFastRng512 V1 benchmark / audit tooling
 * Copyright (c) 2026, Watney-0717.
 * Licensed under the Zero-Clause BSD License (0BSD).
 * See ../license/BENCHMARK-SOURCE-0BSD.txt.
 */

/*
 * UltraFastRng512 POC — Broad Speed / Options Tour
 *
 * Purpose:
 *   Let an interested reader explore the implemented transport/profile and
 *   MC optimization surfaces without confusing them with the single fastest
 *   "wow" benchmark.
 *
 * The executable distinguishes:
 *   1) real integrated V1 cultivation/profile throughput;
 *   2) real integrated Finance/Pharma optimization-profile throughput;
 *   3) selector-only/manual options that the current generic domain bridge
 *      does not execute.
 */
#define UFR_EXTERNAL_MAIN 1
#include "../src/UltraFastRng512_MC_NORMALTAIL_OPTIMIZED_V1.c"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t parse_u64(const char *s, uint64_t def) {
    if (!s || !*s) return def;
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || end == s || *end != '\0') return def;
    return (uint64_t)v;
}

static double median(double *a, unsigned n) {
    double t[32];
    if (n > 32u) n = 32u;
    memcpy(t, a, n * sizeof(double));
    for (unsigned i = 0; i < n; ++i)
        for (unsigned j = i + 1u; j < n; ++j)
            if (t[j] < t[i]) { double x=t[i]; t[i]=t[j]; t[j]=x; }
    return t[n / 2u];
}

static void print_selector_matrix(void) {
    printf("\n=== MC option surface / selector coverage ===\n");
    const ufr_mc_v8_profile_id_t ids[] = {
        UFR_MC_V8_AUTO, UFR_MC_V8_BASELINE, UFR_MC_V8_VEC8,
        UFR_MC_V8_PARAM8_SHARED, UFR_MC_V8_PARAM4_PRECOMPUTE,
        UFR_MC_V8_PARAM8_PRECOMPUTE, UFR_MC_V8_PARAM4_PRECOMPUTE_INVARIANT,
        UFR_MC_V8_PATH8_BLOCK, UFR_MC_V8_PATH4X2, UFR_MC_V8_PATH4,
        UFR_MC_V8_R2_CACHE8
    };
    ufr_mc_v8_workload_t w = ufr_mc_v8_default_workload();
    w.caps = UFR_MC_V8_CAP_SAMPLE_INDEPENDENT |
             UFR_MC_V8_CAP_INDEPENDENT_PARAMS |
             UFR_MC_V8_CAP_COMMON_TRANSFORM |
             UFR_MC_V8_CAP_INDEPENDENT_PATHS |
             UFR_MC_V8_CAP_PAIRWISE |
             UFR_MC_V8_CAP_REPEATED_TRANSFORM |
             UFR_MC_V8_CAP_PRECOMPUTE_SAFE |
             UFR_MC_V8_CAP_LANE_LOCAL |
             UFR_MC_V8_CAP_PATH_BLOCK_LAYOUT_READY |
             UFR_MC_V8_CAP_PATH_BLOCK_BENCH_PROVEN |
             UFR_MC_V8_CAP_PATH4X2_BENCH_PROVEN |
             UFR_MC_V8_CAP_PURE_PATH4_BENCH_PROVEN |
             UFR_MC_V8_CAP_PRECOMPUTE_LOCAL_FAST |
             UFR_MC_V8_CAP_PARAM4_PRECOMPUTE_PROVEN |
             UFR_MC_V8_CAP_INVARIANT_PREP_SAFE |
             UFR_MC_V8_CAP_INVARIANT_PREP_PROVEN;
    w.paths_per_batch = 8u;
    w.params_per_batch = 8u;
    w.transform_cost_estimate = 100u;
    w.generation_reuse = 64u;
    w.expected_reuse = 64u;

    for (unsigned i=0; i<sizeof(ids)/sizeof(ids[0]); ++i) {
        ufr_mc_v8_plan_t p;
        int rc = ufr_mc_v8_select(&w, ids[i], &p);
        printf("%-28s selector=%s",
               ufr_mc_v8_profile_name(ids[i]), rc==0 ? "eligible" : "not-eligible");
        if (rc==0) printf(" selected=%s lanes=%u precompute=%u score=%u",
                          ufr_mc_v8_profile_name(p.selected), p.preferred_lanes,
                          p.requires_precompute, p.score);
        printf("\n");
    }
    printf("NOTE: PATH8_BLOCK/PATH4X2/PATH4 remain manual/path-specific surfaces;");
    printf(" the current generic Finance/Pharma q256 bridge does not execute them.\n");
}

static int bench_core_profile(const char *name,
                              ufr_cultivation_mode_t cultivation,
                              ufr_final_cultivation_profile_t final_profile,
                              uint64_t chunks, uint64_t generation,
                              unsigned runs) {
    if (runs == 0u || runs > 16u) return -1;
    double rates[16] = {0};
    for (unsigned rep=0; rep<runs; ++rep) {
        ufr_mc_config_t cfg;
        ufr_mc_config_default(&cfg);
        cfg.cultivation = cultivation;
        cfg.final_profile = final_profile;
        cfg.generation = UFR_GENERATION_FIXED;
        cfg.min_generation_mode = UFR_GENERATION_FIELD_CUSTOM;
        cfg.max_generation_mode = UFR_GENERATION_FIELD_CUSTOM;
        cfg.min_generation_chunks = (uint32_t)generation;
        cfg.max_generation_chunks = (uint32_t)generation;

        ufr_mc_cultivation_engine_t e;
        if (ufr_mc_engine_init(&e, &cfg) != 0 || ufr_mc_engine_start(&e) != 0) return -2;
        uint64_t ws=0, wh=0; double wz=0.0;
        if (ufr_mc_run_representative(&e, 64u, &ws, &wh, &wz) != 0) {
            ufr_mc_engine_destroy(&e); return -3;
        }
        uint64_t t0=now_ns();
        uint64_t samples=0, hits=0; double sum=0.0;
        int rc=ufr_mc_run_representative(&e, chunks, &samples, &hits, &sum);
        uint64_t t1=now_ns();
        ufr_mc_engine_destroy(&e);
        if (rc != 0 || !samples) return -4;
        rates[rep] = (double)samples / (double)(t1-t0); /* samples/ns == Gsamples/s */
    }
    double med=median(rates,runs);
    printf("%-24s %.3f Gsamples/s median", name, med);
    if (runs > 1u) printf(" | runs=%u",runs);
    printf("\n");
    return 0;
}

static ufr_mc_v8_workload_t domain_workload(void) {
    ufr_mc_v8_workload_t w=ufr_mc_v8_default_workload();
    w.caps = UFR_MC_V8_CAP_SAMPLE_INDEPENDENT |
             UFR_MC_V8_CAP_INDEPENDENT_PARAMS |
             UFR_MC_V8_CAP_COMMON_TRANSFORM |
             UFR_MC_V8_CAP_PRECOMPUTE_SAFE |
             UFR_MC_V8_CAP_PRECOMPUTE_LOCAL_FAST |
             UFR_MC_V8_CAP_PARAM4_PRECOMPUTE_PROVEN |
             UFR_MC_V8_CAP_INVARIANT_PREP_SAFE |
             UFR_MC_V8_CAP_INVARIANT_PREP_PROVEN;
    w.paths_per_batch=1u;
    w.params_per_batch=8u;
    w.samples_per_block=256u;
    w.generation_reuse=64u;
    w.transform_cost_estimate=100u;
    w.expected_reuse=64u;
    return w;
}

static int bench_domain(const char *domain_name,
                        ufr_mc_integrated_domain_t domain,
                        ufr_mc_v8_profile_id_t profile,
                        uint64_t chunks, unsigned runs) {
    if (runs == 0u || runs > 8u) return -1;
    double rates[8] = {0};
    ufr_mc_integrated_finance_cfg_t fc={
        .spot=100.0f, .rate=0.03f, .vol=0.20f, .maturity=1.0f,
        .strikes={80,85,90,95,100,105,110,115}
    };
    ufr_mc_integrated_pharma_cfg_t pc={
        .clearance_lph=5.0f, .volume_l=50.0f, .obs_hour=8.0f,
        .dose={10,12,14,16,18,20,22,24}
    };
    for (unsigned rep=0; rep<runs; ++rep) {
        ufr_paperview_mc_config_t cfg;
        memset(&cfg,0,sizeof(cfg));
        cfg.abi_version=UFR_FINAL_API_VERSION;
        cfg.cultivation=UFR_CULT_DIRECT;
        cfg.final_profile=UFR_FINAL_INT32;
        cfg.generation_chunks=64u;

        ufr_paperview_mc_engine_t *engine=NULL;
        if (ufr_paperview_mc_create(&cfg,&engine)!=0 || !engine) return -2;
        if (ufr_paperview_mc_start(engine)!=0) { ufr_paperview_mc_destroy(engine); return -3; }

        ufr_mc_v8_workload_t w=domain_workload();
        ufr_mc_integrated_result_t out;
        memset(&out,0,sizeof(out));
        uint64_t t0=now_ns();
        int rc=ufr_paperview_mc_run_domain(engine,domain,
                                            domain==UFR_MC_DOMAIN_FINANCE?&fc:NULL,
                                            domain==UFR_MC_DOMAIN_PHARMA?&pc:NULL,
                                            w,profile,chunks,&out);
        uint64_t t1=now_ns();
        ufr_paperview_mc_destroy(engine);
        if (rc!=0 || !out.samples) return rc ? rc : -4;
        rates[rep] = (double)out.samples / (double)(t1-t0); /* samples/ns == Gsamples/s */
    }
    printf("%-8s %-28s %.3f Gsamples/s median", domain_name,
           ufr_mc_v8_profile_name(profile), median(rates,runs));
    if (runs > 1u) printf(" | runs=%u",runs);
    printf("\n");
    return 0;
}

int main(int argc,char **argv) {
    const uint64_t chunks=parse_u64(argc>1?argv[1]:NULL,1024u);
    const uint64_t generation=parse_u64(argc>2?argv[2]:NULL,1048576u);
    const unsigned runs=(unsigned)parse_u64(argc>3?argv[3]:NULL,3u);
    pin_cpu0();

printf("UltraFastRng512 broad benchmark: V1\n");
    printf("policy=one-core complete; Front + Back A/B remain on the same core\n");
    printf("chunks=%" PRIu64 " generation=%" PRIu64 " runs=%u\n",chunks,generation,runs);

#ifdef __linux__
    printf("host cpu=%d; all benchmark engines pin their Front/Back to that core\n", ufrx_current_cpu());
#endif

    print_selector_matrix();

    printf("\n=== Core transport / final-profile throughput ===\n");
    bench_core_profile("AUTO -> DIRECT/R2", UFR_CULT_AUTO, UFR_FINAL_AUTO, chunks, generation, runs);
    bench_core_profile("DIRECT / R2_FLOAT32", UFR_CULT_DIRECT, UFR_FINAL_R2_FLOAT32, chunks, generation, runs);
    bench_core_profile("DIRECT / FLOAT32", UFR_CULT_DIRECT, UFR_FINAL_FLOAT32, chunks, generation, runs);
    bench_core_profile("DIRECT / INT32", UFR_CULT_DIRECT, UFR_FINAL_INT32, chunks, generation, runs);
    bench_core_profile("DIRECT / RAW_INT16", UFR_CULT_DIRECT, UFR_FINAL_RAW_INT16, chunks, generation, runs);
    bench_core_profile("L1_CACHE / FLOAT32", UFR_CULT_L1_CACHE, UFR_FINAL_FLOAT32, chunks, generation, runs);
    bench_core_profile("L2_CACHE / FLOAT32", UFR_CULT_L2_CACHE, UFR_FINAL_FLOAT32, chunks, generation, runs);

    printf("\n=== Integrated Finance optimization options ===\n");
    const ufr_mc_v8_profile_id_t finance_ids[] = {
        UFR_MC_V8_AUTO, UFR_MC_V8_BASELINE, UFR_MC_V8_VEC8,
        UFR_MC_V8_PARAM8_SHARED, UFR_MC_V8_PARAM4_PRECOMPUTE,
        UFR_MC_V8_PARAM8_PRECOMPUTE, UFR_MC_V8_PARAM4_PRECOMPUTE_INVARIANT
    };
    for (unsigned i=0;i<sizeof(finance_ids)/sizeof(finance_ids[0]);++i)
        (void)bench_domain("FIN",UFR_MC_DOMAIN_FINANCE,finance_ids[i],chunks/2u?chunks/2u:1u,runs);

    printf("\n=== Integrated Pharma optimization options ===\n");
    const ufr_mc_v8_profile_id_t pharma_ids[] = {
        UFR_MC_V8_AUTO, UFR_MC_V8_BASELINE, UFR_MC_V8_VEC8,
        UFR_MC_V8_PARAM8_SHARED, UFR_MC_V8_PARAM4_PRECOMPUTE,
        UFR_MC_V8_PARAM8_PRECOMPUTE, UFR_MC_V8_PARAM4_PRECOMPUTE_INVARIANT
    };
    for (unsigned i=0;i<sizeof(pharma_ids)/sizeof(pharma_ids[0]);++i)
        (void)bench_domain("PHARM",UFR_MC_DOMAIN_PHARMA,pharma_ids[i],chunks/2u?chunks/2u:1u,runs);

    printf("\nNotes:\n");
    printf("- DIRECT/R2_FLOAT32 is the intended first-run speed showcase.\n");
    printf("- VEC8 currently shares the deliberate baseline body in the integrated Finance/Pharma consumer.\n");
    printf("- PARAM8_PRECOMPUTE currently shares the common optimized body unless the invariant specialization is selected.\n");
    printf("- PATH8_BLOCK/PATH4X2/PATH4 are retained manual/path-specific options, not silently presented as end-to-end domain speeds.\n");
    printf("- Numbers from this tour are engineering references, not formal cross-machine claims.\n");
    return 0;
}

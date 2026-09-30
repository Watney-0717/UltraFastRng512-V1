/*
 * UltraFastRng512 V1 benchmark / audit tooling
 * Copyright (c) 2026, Watney-0717.
 * Licensed under the Zero-Clause BSD License (0BSD).
 * See ../license/BENCHMARK-SOURCE-0BSD.txt.
 */

/*
 * UltraFastRng512 POC — Speed Showcase
 *
 * Purpose:
 *   The public "wow" benchmark.  It uses the actual built-in fastest
 *   DIRECT/R2_FLOAT32 representative path, with Front + Back A/B + MC all
 *   confined to one logical CPU core.
 *
 * This is intentionally NOT a generic API microbenchmark.  Its purpose is
 * to expose the fastest practical integrated path already present in the
 * main V1 implementation.
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

static uint64_t run_once(ufr_mc_cultivation_engine_t *e, uint64_t chunks,
                         double *seconds, uint64_t *samples,
                         uint64_t *hits, double *sum) {
    uint64_t t0 = now_ns();
    int rc = ufr_mc_run_representative(e, chunks, samples, hits, sum);
    uint64_t t1 = now_ns();
    if (rc != 0) return (uint64_t)(-rc);
    *seconds = (double)(t1 - t0) * 1e-9;
    return 0;
}

int main(int argc, char **argv) {
    const uint64_t chunks = parse_u64(argc > 1 ? argv[1] : NULL, 16384u);
    const unsigned runs = (unsigned)parse_u64(argc > 2 ? argv[2] : NULL, 3u);
    const uint64_t generation = parse_u64(argc > 3 ? argv[3] : NULL, 1048576u);
    if (!chunks || !runs || generation < 8u) {
        fprintf(stderr, "usage: %s [chunks=16384] [runs=3] [generation_chunks=1048576]\n", argv[0]);
        return 2;
    }

    pin_cpu0();

    printf("UltraFastRng512 speed showcase\n");
printf("baseline=V1\n");
    printf("path=DIRECT / R2_FLOAT32 / representative specialized path\n");
    printf("policy=Front + Back A/B + MC on one logical CPU core\n");
    printf("workload=%" PRIu64 " chunks/run | generation=%" PRIu64 " chunks\n", chunks, generation);

    double cfg_zero = 0.0;
    (void)cfg_zero;
    double vals[16] = {0};
    if (runs > 16u) {
        fprintf(stderr, "runs must be <= 16\n");
        return 2;
    }

    for (unsigned rep = 0; rep < runs; ++rep) {
        ufr_mc_config_t cfg;
        ufr_mc_config_default(&cfg);
        cfg.cultivation = UFR_CULT_DIRECT;
        cfg.final_profile = UFR_FINAL_R2_FLOAT32;
        cfg.generation = UFR_GENERATION_FIXED;
        cfg.min_generation_mode = UFR_GENERATION_FIELD_CUSTOM;
        cfg.max_generation_mode = UFR_GENERATION_FIELD_CUSTOM;
        cfg.min_generation_chunks = (uint32_t)(generation > UINT32_MAX ? UINT32_MAX : generation);
        cfg.max_generation_chunks = cfg.min_generation_chunks;

        ufr_mc_cultivation_engine_t e;
        if (ufr_mc_engine_init(&e, &cfg) != 0 || ufr_mc_engine_start(&e) != 0) {
            fprintf(stderr, "engine start failed on run %u\n", rep + 1u);
            return 3;
        }

#ifdef __linux__
        printf("run %u affinity: main=%d backA=%d backB=%d%s\n",
               rep + 1u,
               ufrx_current_cpu(),
               e.seedx.back_a.bound_cpu,
               e.seedx.back_b.bound_cpu,
               (e.seedx.back_a.bound_cpu == e.seedx.back_b.bound_cpu &&
                e.seedx.back_a.bound_cpu == ufrx_current_cpu()) ? " [ONE-CORE]" : " [ERROR]");
        if (!(e.seedx.back_a.bound_cpu == e.seedx.back_b.bound_cpu &&
              e.seedx.back_a.bound_cpu == ufrx_current_cpu())) {
            ufr_mc_engine_destroy(&e);
            return 4;
        }
#endif

        /* Small untimed warm-up: establish the same R2 cache path before the
         * timed region without changing the measured path. */
        uint64_t warm_samples = 0, warm_hits = 0;
        double warm_sum = 0.0;
        if (ufr_mc_run_representative(&e, 64u, &warm_samples, &warm_hits, &warm_sum) != 0) {
            fprintf(stderr, "warm-up failed on run %u\n", rep + 1u);
            ufr_mc_engine_destroy(&e);
            return 5;
        }

        double sec = 0.0, sum = 0.0;
        uint64_t samples = 0, hits = 0;
        if (run_once(&e, chunks, &sec, &samples, &hits, &sum) != 0) {
            fprintf(stderr, "timed run failed on run %u\n", rep + 1u);
            ufr_mc_engine_destroy(&e);
            return 6;
        }
        vals[rep] = (double)samples / sec / 1e9;
        printf("run %u: %.3f Gsamples/s | %.3f ns/256 | samples=%" PRIu64 " hits=%" PRIu64 " mean=%+.9g\n",
               rep + 1u, vals[rep], sec * 1e9 / (double)(samples / 256u),
               samples, hits, samples ? sum / (double)samples : 0.0);
        printf("       switches=%" PRIu64 " ready=%" PRIu64 " missed=%" PRIu64 "\n",
               e.seedx.switches, e.seedx.ready_observations, e.seedx.missed_ready_checks);
        ufr_mc_engine_destroy(&e);
    }

    double sorted[16];
    memcpy(sorted, vals, runs * sizeof(vals[0]));
    for (unsigned i = 0; i < runs; ++i)
        for (unsigned j = i + 1u; j < runs; ++j)
            if (sorted[j] < sorted[i]) { double t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t; }
    double med = sorted[runs / 2u];

    printf("MEDIAN: %.3f Gsamples/s\n", med);
    printf("This is the integrated fastest-path showcase; it is not the broad options benchmark.\n");
    return 0;
}

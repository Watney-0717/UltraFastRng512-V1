/*
 * UltraFastRng512 V1 benchmark / audit tooling
 * Copyright (c) 2026, Watney-0717.
 * Licensed under the Zero-Clause BSD License (0BSD).
 * See ../license/BENCHMARK-SOURCE-0BSD.txt.
 */

/*
 * UltraFastRng512 POC — Fresh-sample throughput companion benchmark
 *
 * Same representative path as bench_speed_showcase.c, but forces
 * UFR_GENERATION_CHUNK so the Normal starter is regenerated at every MC chunk.
 * This is a qualification benchmark, not a replacement for the integrated
 * FIXED-generation throughput showcase.
 */
#define UFR_EXTERNAL_MAIN 1
#include "../src/UltraFastRng512_MC_NORMALTAIL_OPTIMIZED_V1.c"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>

static uint64_t parse_u64(const char *s, uint64_t def) {
    if (!s || !*s) return def;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s || *end != '\0') return def;
    return (uint64_t)v;
}

int main(int argc, char **argv) {
    const uint64_t chunks = parse_u64(argc > 1 ? argv[1] : NULL, 4096u);
    const unsigned runs = (unsigned)parse_u64(argc > 2 ? argv[2] : NULL, 3u);
    if (!chunks || !runs || runs > 16u) return 2;

    pin_cpu0();
puts("UltraFastRng512 fresh-sample throughput companion: V1");
    puts("path=DIRECT / R2_FLOAT32 / representative specialized path");
    puts("generation=CHUNK (new Normal starter at every MC chunk)");

    for (unsigned rep = 0; rep < runs; ++rep) {
        ufr_mc_config_t cfg;
        ufr_mc_config_default(&cfg);
        cfg.cultivation = UFR_CULT_DIRECT;
        cfg.final_profile = UFR_FINAL_R2_FLOAT32;
        cfg.generation = UFR_GENERATION_CHUNK;

        ufr_mc_cultivation_engine_t e;
        if (ufr_mc_engine_init(&e, &cfg) != 0 || ufr_mc_engine_start(&e) != 0) return 3;

        const uint64_t t0 = now_ns();
        uint64_t samples = 0u, hits = 0u;
        double sum = 0.0;
        const int rc = ufr_mc_run_representative(&e, chunks, &samples, &hits, &sum);
        const uint64_t t1 = now_ns();
        if (rc != 0) {
            ufr_mc_engine_destroy(&e);
            return 4;
        }

        const double sec = (double)(t1 - t0) * 1e-9;
        printf("run %u: %.6f Gsamples/s | %.3f ns/chunk | samples=%" PRIu64
               " hits=%" PRIu64 " switches=%" PRIu64 "\n",
               rep + 1u, (double)samples / sec / 1e9,
               sec * 1e9 / (double)chunks, samples, hits,
               (uint64_t)e.seedx.switches);
        ufr_mc_engine_destroy(&e);
    }
    return 0;
}

#ifndef ULTRAFAST_RNG512_MC_OPTIMIZATION_PROFILES_V8_H
#define ULTRAFAST_RNG512_MC_OPTIMIZATION_PROFILES_V8_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UFR_MC_V8_AUTO = 0,
    UFR_MC_V8_BASELINE,
    UFR_MC_V8_VEC8,
    UFR_MC_V8_PARAM8_SHARED,
    UFR_MC_V8_PARAM4_PRECOMPUTE,
    UFR_MC_V8_PARAM8_PRECOMPUTE,
    UFR_MC_V8_PARAM4_PRECOMPUTE_INVARIANT,
    UFR_MC_V8_PATH8_BLOCK,
    UFR_MC_V8_PATH4X2,
    UFR_MC_V8_PATH4,          /* manual only */
    UFR_MC_V8_R2_CACHE8,
    UFR_MC_V8__COUNT
} ufr_mc_v8_profile_id_t;

enum {
    UFR_MC_V8_CAP_SAMPLE_INDEPENDENT        = 1u << 0,
    UFR_MC_V8_CAP_REPEATED_TRANSFORM        = 1u << 1,
    UFR_MC_V8_CAP_PAIRWISE                  = 1u << 2,
    UFR_MC_V8_CAP_INDEPENDENT_PATHS         = 1u << 3,
    UFR_MC_V8_CAP_INDEPENDENT_PARAMS        = 1u << 4,
    UFR_MC_V8_CAP_GENERATION_REUSE          = 1u << 5,
    UFR_MC_V8_CAP_PRECOMPUTE_SAFE           = 1u << 6,
    UFR_MC_V8_CAP_LANE_LOCAL                = 1u << 7,
    UFR_MC_V8_CAP_FIXED_BLOCK               = 1u << 8,
    UFR_MC_V8_CAP_COMMON_TRANSFORM          = 1u << 9,
    UFR_MC_V8_CAP_PATH_BLOCK_LAYOUT_READY   = 1u << 10,
    UFR_MC_V8_CAP_PATH_BLOCK_BENCH_PROVEN   = 1u << 11,
    UFR_MC_V8_CAP_PATH4X2_BENCH_PROVEN      = 1u << 12,
    UFR_MC_V8_CAP_PURE_PATH4_BENCH_PROVEN   = 1u << 13,
    UFR_MC_V8_CAP_PRECOMPUTE_LOCAL_FAST     = 1u << 14,
    UFR_MC_V8_CAP_PARAM4_PRECOMPUTE_PROVEN  = 1u << 15,
    UFR_MC_V8_CAP_INVARIANT_PREP_SAFE       = 1u << 16,
    UFR_MC_V8_CAP_INVARIANT_PREP_PROVEN     = 1u << 17
};

typedef struct {
    uint32_t caps;
    uint32_t samples_per_block;
    uint32_t paths_per_batch;
    uint32_t params_per_batch;
    uint32_t generation_reuse;
    uint32_t hot_loop_branch_estimate;
    uint32_t transform_cost_estimate;
    uint32_t expected_reuse;
} ufr_mc_v8_workload_t;

typedef struct {
    ufr_mc_v8_profile_id_t requested;
    ufr_mc_v8_profile_id_t selected;
    uint32_t score;
    uint32_t reason_mask;
    uint32_t preferred_lanes;
    uint32_t requires_precompute;
} ufr_mc_v8_plan_t;

enum {
    UFR_MC_V8_REASON_R2             = 1u << 0,
    UFR_MC_V8_REASON_PATH           = 1u << 1,
    UFR_MC_V8_REASON_PARAM          = 1u << 2,
    UFR_MC_V8_REASON_HEAVY          = 1u << 3,
    UFR_MC_V8_REASON_REUSE          = 1u << 4,
    UFR_MC_V8_REASON_COMMON         = 1u << 5,
    UFR_MC_V8_REASON_LAYOUT_READY   = 1u << 6,
    UFR_MC_V8_REASON_BENCH_PROVEN   = 1u << 7,
    UFR_MC_V8_REASON_RESOURCE_SAFE  = 1u << 8,
    UFR_MC_V8_REASON_FAST_PRECACHE   = 1u << 9,
    UFR_MC_V8_REASON_INVARIANT_PREP = 1u << 10
};

const char *ufr_mc_v8_profile_name(ufr_mc_v8_profile_id_t id);
int ufr_mc_v8_select(const ufr_mc_v8_workload_t *w,
                     ufr_mc_v8_profile_id_t requested,
                     ufr_mc_v8_plan_t *out);

static inline ufr_mc_v8_workload_t ufr_mc_v8_default_workload(void) {
    ufr_mc_v8_workload_t w = {0};
    w.caps = UFR_MC_V8_CAP_SAMPLE_INDEPENDENT | UFR_MC_V8_CAP_LANE_LOCAL;
    w.samples_per_block = 256u;
    w.paths_per_batch = 1u;
    w.params_per_batch = 1u;
    w.generation_reuse = 1u;
    w.hot_loop_branch_estimate = 0u;
    w.transform_cost_estimate = 0u;
    w.expected_reuse = 1u;
    return w;
}
#ifdef __cplusplus
}
#endif
#endif

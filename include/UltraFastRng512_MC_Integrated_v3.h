#ifndef ULTRAFAST_RNG512_MC_INTEGRATED_V3_H
#define ULTRAFAST_RNG512_MC_INTEGRATED_V3_H
#include <stddef.h>
#include <stdint.h>
#include "ufrng_paperview_final_api_v1.h"
#include "UltraFastRng512_MC_OptimizationProfiles_v9.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UFR_MC_DOMAIN_FINANCE = 1,
    UFR_MC_DOMAIN_PHARMA = 2
} ufr_mc_integrated_domain_t;

typedef struct {
    float spot;
    float rate;
    float vol;
    float maturity;
    float strikes[8];
} ufr_mc_integrated_finance_cfg_t;

typedef struct {
    float clearance_lph;
    float volume_l;
    float obs_hour;
    float dose[8];
} ufr_mc_integrated_pharma_cfg_t;

typedef struct {
    double sum[8];
    double aux[8];
    uint64_t samples;
    uint64_t blocks;
    ufr_mc_v8_profile_id_t selected_profile;
    uint32_t preferred_lanes;
    uint32_t requires_precompute;
} ufr_mc_integrated_result_t;

/* Runs current-main Back/Front -> PaperView -> q256 -> domain consumer.
 * Profile selection happens once before the q-block loop. R2 is explicitly
 * incompatible with the current Finance/Pharma q256 domain consumers; the
 * v9 dispatcher prevents AUTO from selecting it for those domains. */
int ufr_paperview_mc_run_domain(
    ufr_paperview_mc_engine_t *engine,
    ufr_mc_integrated_domain_t domain,
    const ufr_mc_integrated_finance_cfg_t *finance,
    const ufr_mc_integrated_pharma_cfg_t *pharma,
    ufr_mc_v8_workload_t workload,
    ufr_mc_v8_profile_id_t requested_profile,
    uint64_t chunks,
    ufr_mc_integrated_result_t *out);
#ifdef __cplusplus
}
#endif
#endif

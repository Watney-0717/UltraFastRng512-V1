#ifndef ULTRAFAST_RNG512_MC_OPTIMIZATION_PROFILES_V9_H
#define ULTRAFAST_RNG512_MC_OPTIMIZATION_PROFILES_V9_H
#include <stdint.h>
#include "UltraFastRng512_MC_OptimizationProfiles_v8.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UFR_MC_V9_DOMAIN_GENERIC = 0,
    UFR_MC_V9_DOMAIN_FINANCE = 1,
    UFR_MC_V9_DOMAIN_PHARMA = 2
} ufr_mc_v9_domain_t;

typedef struct {
    ufr_mc_v9_domain_t domain;
    ufr_mc_v8_workload_t workload;
    ufr_mc_v8_profile_id_t requested;
} ufr_mc_v9_request_t;

typedef struct {
    ufr_mc_v8_plan_t plan;
    uint32_t r2_eligible;
    uint32_t param4_eligible;
    uint32_t domain_compatibility_mask;
} ufr_mc_v9_plan_t;

enum {
    UFR_MC_V9_COMPAT_R2       = 1u << 0,
    UFR_MC_V9_COMPAT_PARAM4   = 1u << 1,
    UFR_MC_V9_COMPAT_PARAM8   = 1u << 2,
    UFR_MC_V9_COMPAT_BASELINE = 1u << 3
};

/* Domain-aware one-time dispatcher. R2 is excluded from the current
 * Finance/Pharma q256 domain consumers because they do not consume R2 cache.
 */
int ufr_mc_v9_select(const ufr_mc_v9_request_t *req, ufr_mc_v9_plan_t *out);
const char *ufr_mc_v9_domain_name(ufr_mc_v9_domain_t domain);
#ifdef __cplusplus
}
#endif
#endif

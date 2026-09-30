#ifndef ULTRAFAST_RNG512_MC_FINANCE_EARLY_V1_H
#define ULTRAFAST_RNG512_MC_FINANCE_EARLY_V1_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UFR_FINANCE_EARLY_API_VERSION 1u
#define UFR_FINANCE_EARLY_MAX_STEPS 64u
#define UFR_FINANCE_EARLY_MAX_PATHS 1048576u

typedef enum {
    UFR_FINANCE_EARLY_CALL = 1u,
    UFR_FINANCE_EARLY_PUT = 2u
} ufr_finance_early_option_t;

typedef enum {
    UFR_FINANCE_EARLY_BERMUDAN = 1u,
    UFR_FINANCE_EARLY_AMERICAN = 2u
} ufr_finance_early_style_t;

/* Exercise mask is indexed by step number. Bit t means exercise is allowed
 * at t*dt. Step 0 is intentionally ignored; standard contracts normally
 * exercise only on dates 1..time_steps. Terminal exercise is required. */
typedef struct {
    uint32_t api_version;
    ufr_finance_early_option_t option;
    ufr_finance_early_style_t style;
    uint32_t time_steps;
    uint64_t exercise_mask;
    float spot;
    float strike;
    float rate;
    float vol;
    float maturity;
    double regression_ridge;
    uint32_t min_regression_paths;
    uint32_t reserved0;
} ufr_finance_early_cfg_t;

typedef struct {
    double price;
    double variance;
    double sem;
    uint64_t path_count;
    uint64_t exercise_count;
    uint64_t regression_count;
    uint64_t regression_fallback_count;
    uint64_t invalid_path_count;
    uint32_t time_steps;
    uint32_t reserved0;
} ufr_finance_early_result_t;

typedef struct {
    ufr_finance_early_result_t base;
    double price_spot_minus;
    double price_spot_plus;
    double price_vol_minus;
    double price_vol_plus;
    double price_rate_minus;
    double price_rate_plus;
    double price_maturity_minus;
    double price_maturity_plus;
    double delta;
    double gamma;
    double vega;
    double rho;
    double theta;
    double spot_bump;
    double vol_bump;
    double rate_bump;
    double maturity_bump;
    uint8_t crn_common_normals;
    uint8_t central_difference;
    uint8_t reserved[6];
} ufr_finance_early_greeks_result_t;

void ufr_finance_early_cfg_default(ufr_finance_early_cfg_t *cfg);
int ufr_finance_early_cfg_validate(const ufr_finance_early_cfg_t *cfg);

int ufr_finance_early_price(
    const float *normals,
    uint64_t path_count,
    const ufr_finance_early_cfg_t *cfg,
    ufr_finance_early_result_t *out);

/* Greeks are computed with central differences using the exact same normal
 * matrix for every bump (CRN). Theta uses maturity +/- bump while retaining
 * the same discrete exercise-step mask. */
int ufr_finance_early_greeks(
    const float *normals,
    uint64_t path_count,
    const ufr_finance_early_cfg_t *cfg,
    const double spot_bump_rel,
    const double vol_bump_abs,
    const double rate_bump_abs,
    const double maturity_bump_rel,
    ufr_finance_early_greeks_result_t *out);

/* Integration with the current PaperView -> q256 path. This bridge is explicit
 * and domain-local; AUTO selectors are not modified. q256 is interpreted as
 * the existing four-path x 64-time layout. The bridge collects a bounded set
 * of qblocks, converts them to float normals, then runs the LSM estimator. */
struct ufr_paperview_mc_engine;
int ufr_paperview_mc_run_finance_early(
    struct ufr_paperview_mc_engine *engine,
    const ufr_finance_early_cfg_t *cfg,
    uint64_t chunks,
    uint64_t max_paths,
    ufr_finance_early_result_t *out);

int ufr_paperview_mc_run_finance_early_greeks(
    struct ufr_paperview_mc_engine *engine,
    const ufr_finance_early_cfg_t *cfg,
    uint64_t chunks,
    uint64_t max_paths,
    double spot_bump_rel,
    double vol_bump_abs,
    double rate_bump_abs,
    double maturity_bump_rel,
    ufr_finance_early_greeks_result_t *out);

#ifdef __cplusplus
}
#endif

#endif

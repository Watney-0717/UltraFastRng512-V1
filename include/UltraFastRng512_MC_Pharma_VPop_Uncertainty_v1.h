#ifndef ULTRAFAST_RNG512_MC_PHARMA_VPOP_UNCERTAINTY_V1_H
#define ULTRAFAST_RNG512_MC_PHARMA_VPOP_UNCERTAINTY_V1_H

#include <stddef.h>
#include <stdint.h>
#include "UltraFastRng512_MC_DeterministicStreams_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UFR_PHARMA_VPOP_API_VERSION 1u
#define UFR_PHARMA_VPOP_MAX_PARAMS 8u
#define UFR_PHARMA_VPOP_NAME_BYTES 32u
#define UFR_PHARMA_VPOP_SCENARIO_NAME_BYTES 48u
#define UFR_PHARMA_VPOP_MAX_SCENARIOS 64u
#define UFR_PHARMA_VPOP_CI_METHOD_BYTES 32u

typedef enum {
    UFR_PHARMA_PARAM_LOGNORMAL = 1u,
    UFR_PHARMA_PARAM_NORMAL = 2u
} ufr_pharma_param_distribution_t;

typedef struct {
    char name[UFR_PHARMA_VPOP_NAME_BYTES];
    double mean;
    double cv;
    double min_value;
    double max_value;
    uint32_t distribution;
    uint32_t reserved;
} ufr_pharma_vpop_param_t;

typedef struct {
    uint32_t api_version;
    uint32_t param_count;
    uint64_t subjects;
    uint8_t use_cholesky;
    uint8_t reserved[7];
    ufr_mc_stream_plan_t stream_plan;
    ufr_pharma_vpop_param_t params[UFR_PHARMA_VPOP_MAX_PARAMS];
    /* Lower-triangular Gaussian copula factor, row-major 8x8.
     * Set use_cholesky=0 for independent parameters. */
    double cholesky[UFR_PHARMA_VPOP_MAX_PARAMS * UFR_PHARMA_VPOP_MAX_PARAMS];
} ufr_pharma_vpop_spec_t;

typedef struct {
    uint64_t subjects;
    uint32_t param_count;
    double *values; /* param-major: values[p*subjects + i] */
    uint8_t population_sha256[32];
    uint8_t stream_id[UFR_MC_STREAM_ID_BYTES];
} ufr_pharma_vpop_t;

typedef struct {
    uint64_t count;
    double mean;
    double standard_deviation;
    double cv;
    double min_value;
    double max_value;
    double p05;
    double p50;
    double p95;
} ufr_pharma_vpop_summary_t;

void ufr_pharma_vpop_spec_default(ufr_pharma_vpop_spec_t *spec);
int  ufr_pharma_vpop_spec_validate(const ufr_pharma_vpop_spec_t *spec);
int  ufr_pharma_vpop_generate(const ufr_pharma_vpop_spec_t *spec,
                              ufr_pharma_vpop_t *out);
void ufr_pharma_vpop_free(ufr_pharma_vpop_t *pop);
int  ufr_pharma_vpop_summary(const ufr_pharma_vpop_t *pop,
                             uint32_t param_index,
                             ufr_pharma_vpop_summary_t *out);
int  ufr_pharma_vpop_hash(const ufr_pharma_vpop_t *pop,
                          uint8_t out_sha256[32]);

/* Parameter-uncertainty scenarios reuse the exact same subject population.
 * This is the intended CRN boundary: all scenarios receive identical subject
 * parameter draws, while scenario transforms alter the model parameters. */
typedef struct {
    uint64_t scenario_id;
    char name[UFR_PHARMA_VPOP_SCENARIO_NAME_BYTES];
    double multiplier[UFR_PHARMA_VPOP_MAX_PARAMS];
    double additive[UFR_PHARMA_VPOP_MAX_PARAMS];
} ufr_pharma_uncertainty_scenario_t;

typedef double (*ufr_pharma_subject_evaluator_fn)(
    const double *effective_params,
    uint32_t param_count,
    uint64_t subject_index,
    uint64_t scenario_id,
    void *user_ctx);

typedef struct {
    uint64_t scenario_id;
    uint64_t samples;
    double mean;
    double variance;
    double sem; /* NAN when fewer than 2 finite observations are available. */
    double ci95_low;
    double ci95_high;
    double ci95_halfwidth;
    double paired_delta_mean;
    double paired_delta_variance;
    double paired_delta_sem;
    double paired_ci95_low;
    double paired_ci95_high;
    uint64_t invalid_values;
    uint8_t finite_ok;
    uint8_t paired_finite_ok;
    uint8_t reserved[6];
    char method[UFR_PHARMA_VPOP_CI_METHOD_BYTES];
} ufr_pharma_uncertainty_result_t;

int ufr_pharma_uncertainty_run(
    const ufr_pharma_vpop_t *population,
    const ufr_pharma_uncertainty_scenario_t *scenarios,
    uint32_t scenario_count,
    uint32_t reference_scenario,
    ufr_pharma_subject_evaluator_fn evaluator,
    void *user_ctx,
    ufr_pharma_uncertainty_result_t *results,
    size_t result_capacity);

#ifdef __cplusplus
}
#endif

#endif

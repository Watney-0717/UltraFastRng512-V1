#ifndef ULTRAFAST_RNG512_MC_INTEGRATED_V4_H
#define ULTRAFAST_RNG512_MC_INTEGRATED_V4_H

#include <stddef.h>
#include <stdint.h>
#include "ufrng_paperview_final_api_v1.h"
#include "UltraFastRng512_MC_OptimizationProfiles_v9.h"
#include "UltraFastRng512_MC_Integrated_v3.h"
#include "UltraFastRng512_MC_Pharma_LinearPiecewiseTransition_Profile_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UFR_PHARMA_3C_K_BLOCKS 5u
#define UFR_PHARMA_4C_K_BLOCKS 7u

typedef struct {
    unsigned dim;              /* 3 or 4 */
    size_t subjects;
    unsigned time_steps;
    unsigned segments;
    float dt;
    float volume;
    /* Block-major subject arrays:
     * M[(row*dim+col)*subjects + i]
     * G[row*subjects + i]
     */
    float *M;
    float *G;
} ufr_pharma_linear_piecewise_ctx_t;

/*
 * Explicit/manual Pharma domain adapter.
 *
 * k_blocks is block-major and contains:
 *   3C: k10,k12,k21,k23,k32  => 5*subjects floats
 *   4C: k10,k12,k21,k23,k32,k34,k43 => 7*subjects floats
 *
 * M/G are prepared once; run performs only repeated matrix-state
 * propagation and segment input injection.  No selector/AUTO path uses this
 * API yet because the formulation is domain-specific and changes the
 * numerical integration formulation relative to RK4.
 */
int ufr_pharma_linear_piecewise_ctx_init(
    ufr_pharma_linear_piecewise_ctx_t *ctx,
    unsigned dim,
    size_t subjects,
    unsigned time_steps,
    unsigned segments,
    float dt,
    float volume,
    const float *k_blocks);

void ufr_pharma_linear_piecewise_ctx_destroy(
    ufr_pharma_linear_piecewise_ctx_t *ctx);

/*
 * initial_blocks is block-major:
 *   state[row*subjects + i]
 * rate_by_segment[seg], bolus_by_segment[seg] are common across subjects.
 * auc/cmax are one float per subject.
 */
int ufr_pharma_linear_piecewise_run(
    const ufr_pharma_linear_piecewise_ctx_t *ctx,
    const float *initial_blocks,
    const float *rate_by_segment,
    const float *bolus_by_segment,
    float *auc,
    float *cmax);

#ifdef __cplusplus
}
#endif
#endif

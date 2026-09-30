#ifndef ULTRAFAST_RNG512_MC_PHARMA_LINEAR_PIECEWISE_TRANSITION_PROFILE_V1_H
#define ULTRAFAST_RNG512_MC_PHARMA_LINEAR_PIECEWISE_TRANSITION_PROFILE_V1_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned dim;              /* 3 or 4 compartments */
    unsigned time_steps;       /* total propagation steps */
    unsigned segments;         /* piecewise-constant input segments */
    float dt;
    int subject_specific;
    int piecewise_constant_input;
    int boundary_bolus;
    int manual_only;
} ufr_pharma_linear_piecewise_plan_t;

/* Manual/domain-specific profile validation. AUTO dispatch is intentionally not
 * provided until wider dosing/scenario coverage is benchmark-proven. */
int ufr_pharma_linear_piecewise_plan_valid(
    const ufr_pharma_linear_piecewise_plan_t *p);

#ifdef __cplusplus
}
#endif

#endif

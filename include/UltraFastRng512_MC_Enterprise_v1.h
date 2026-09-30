#ifndef ULTRAFAST_RNG512_MC_ENTERPRISE_V1_H
#define ULTRAFAST_RNG512_MC_ENTERPRISE_V1_H

#include <stddef.h>
#include <stdint.h>

#include "UltraFastRng512_MC_Finance_Early_v1.h"
#include "UltraFastRng512_MC_DeterministicStreams_v1.h"
#include "UltraFastRng512_MC_Pharma_VPop_Uncertainty_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UFR_MC_ENTERPRISE_API_VERSION 1u
#define UFR_MC_CHECKPOINT_VERSION 1u
#define UFR_MC_CHECKPOINT_FRONT_STATES 16u
#define UFR_MC_CHECKPOINT_STATE_BYTES 64u
#define UFR_MC_CHECKPOINT_NORMAL_ROWS 1024u
#define UFR_MC_CHECKPOINT_NORMAL_COLS 16u
#define UFR_MC_UNCERTAINTY_METHOD_MAX 24u

/* Bind a deterministic logical stream to the existing Back/Front engine.
 * Call after create and before start. Worker topology remains excluded from
 * seed derivation. */
struct ufr_paperview_mc_engine;
int ufr_paperview_mc_bind_stream(struct ufr_paperview_mc_engine *engine,
                                 const ufr_mc_stream_plan_t *plan);
int ufr_paperview_mc_get_stream_id(const struct ufr_paperview_mc_engine *engine,
                                   uint8_t out_id[UFR_MC_STREAM_ID_BYTES]);

/* -------------------------------------------------------------------------
 * Exact replay checkpoint.
 *
 * The checkpoint contains only deterministic logical engine state needed to
 * resume the Front/Normal path exactly: Front states, retained Normal starter,
 * stream plan, and counters. Asynchronous Back workers are deliberately not
 * serialized; restore enters replay mode and freezes seed switching.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t version;
    uint32_t flags;
    uint8_t stream_id[UFR_MC_STREAM_ID_BYTES];
    ufr_mc_stream_plan_t stream_plan;
    uint64_t logical_block;
    uint64_t generation_id;
    uint64_t chunks_in_generation;
    uint64_t chunk_id;
    uint32_t mask_phase;
    uint32_t parent_xor_omit;
    uint8_t front_state[UFR_MC_CHECKPOINT_FRONT_STATES][UFR_MC_CHECKPOINT_STATE_BYTES];
    uint16_t normal_tail_mask[UFR_MC_CHECKPOINT_NORMAL_ROWS];
    int16_t normal_starter[UFR_MC_CHECKPOINT_NORMAL_ROWS * UFR_MC_CHECKPOINT_NORMAL_COLS];
    uint8_t config_sha256[32];
    uint8_t state_sha256[32];
} ufr_mc_checkpoint_t;

int ufr_paperview_mc_checkpoint_capture(
    const struct ufr_paperview_mc_engine *engine,
    ufr_mc_checkpoint_t *out_checkpoint);

int ufr_paperview_mc_checkpoint_restore(
    struct ufr_paperview_mc_engine *engine,
    const ufr_mc_checkpoint_t *checkpoint);

int ufr_mc_checkpoint_save(const char *path,
                           const ufr_mc_checkpoint_t *checkpoint);
int ufr_mc_checkpoint_load(const char *path,
                           ufr_mc_checkpoint_t *checkpoint);

/* Exact replay verification: restore -> run -> restore -> run and compare
 * samples, hit count, result bits, and final logical position. */
typedef struct {
    uint8_t exact_match;
    uint8_t reserved[7];
    uint64_t samples_a;
    uint64_t samples_b;
    uint64_t hits_a;
    uint64_t hits_b;
    uint64_t sum_bits_a;
    uint64_t sum_bits_b;
    uint64_t next_block_a;
    uint64_t next_block_b;
} ufr_mc_exact_replay_result_t;

int ufr_paperview_mc_exact_replay_verify(
    struct ufr_paperview_mc_engine *engine,
    const ufr_mc_checkpoint_t *checkpoint,
    uint64_t chunks,
    ufr_mc_exact_replay_result_t *out_result);

/* -------------------------------------------------------------------------
 * CI / Uncertainty report.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint64_t min_samples;
    double max_relative_halfwidth; /* <=0 disables this criterion */
    double max_invalid_fraction;   /* <=0 disables this criterion */
} ufr_mc_uncertainty_config_t;

typedef struct {
    uint64_t samples;
    uint64_t invalid_values;
    double mean;
    double variance;
    double sem;
    double ci95_low;
    double ci95_high;
    double ci95_halfwidth;
    double relative_ci95_halfwidth;
    double invalid_fraction;
    uint8_t finite_ok;
    uint8_t sample_count_ok;
    uint8_t relative_precision_ok;
    uint8_t invalid_fraction_ok;
    uint8_t passed;
    uint8_t reserved[3];
    char method[UFR_MC_UNCERTAINTY_METHOD_MAX];
} ufr_mc_uncertainty_report_t;

void ufr_mc_uncertainty_config_default(ufr_mc_uncertainty_config_t *cfg);
int ufr_mc_uncertainty_report_make(uint64_t samples,
                                   double mean,
                                   double variance,
                                   uint64_t invalid_values,
                                   const ufr_mc_uncertainty_config_t *cfg,
                                   ufr_mc_uncertainty_report_t *out);
int ufr_mc_uncertainty_report_write_json(const char *path,
                                         const ufr_mc_uncertainty_report_t *report);

/* -------------------------------------------------------------------------
 * Golden Reference / Verification Mode.
 * Fixed stream vectors verify canonical deterministic derivation. This does
 * not certify statistical quality; it verifies implementation integrity.
 * ------------------------------------------------------------------------- */
int ufr_mc_golden_reference_verify(void);
int ufr_mc_enterprise_selftest(void);

#ifdef __cplusplus
}
#endif

#endif

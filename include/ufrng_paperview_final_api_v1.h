#ifndef UFRNG_PAPERVIEW_FINAL_API_V1_H
#define UFRNG_PAPERVIEW_FINAL_API_V1_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UFR_FINAL_API_VERSION 1u
#define UFR_FINAL_API_MAX_CUSTOM_PROFILES 16u
#define UFR_PAPER_FINAL_ROWS 1024u
#define UFR_PAPER_FINAL_ROW_LANES 16u

/* Built-in final-cultivation profiles. CUSTOM profiles are registered at
 * startup and are invoked only at generation/cultivation boundaries. */
typedef enum {
    UFR_FINAL_AUTO = 0,
    UFR_FINAL_RAW_INT16 = 1,
    UFR_FINAL_INT32 = 2,
    UFR_FINAL_FLOAT32 = 3,
    UFR_FINAL_R2_FLOAT32 = 4,
    UFR_FINAL_CUSTOM = 5
} ufr_final_cultivation_profile_t;

typedef enum {
    UFR_CULT_AUTO = 0,
    UFR_CULT_DIRECT = 1,
    UFR_CULT_L1_CACHE = 2,
    UFR_CULT_L2_CACHE = 3
} ufr_cultivation_mode_t;

/* Input to one CUSTOM row builder. The 16 values are already PaperView-
 * transformed and remain exact int16 representations of the current
 * Normal+4x fanout path. The epoch row rotation is applied by the engine
 * after final cultivation, so the builder stays row-local and reusable. */
typedef struct {
    const int16_t *samples;
    uint32_t sample_count;     /* always 16 for the current geometry */
    uint32_t view_id;
    uint32_t row_id;
    uint64_t epoch;
    uint32_t phase;
    uint32_t shift;
} ufr_final_row_input_t;

typedef int (*ufr_final_build_row_fn)(const ufr_final_row_input_t *input,
                                      void *dst,
                                      size_t dst_bytes,
                                      void *user_ctx);

typedef struct {
    uint32_t abi_version;
    const char *name;
    size_t bytes_per_row;
    uint32_t flags;
    ufr_final_build_row_fn build_row;
    void *user_ctx;
} ufr_final_profile_desc_t;

enum {
    UFR_FINAL_PROFILE_FLAG_ROW_LOCAL = 1u << 0,
    UFR_FINAL_PROFILE_FLAG_READONLY   = 1u << 1,
    UFR_FINAL_PROFILE_FLAG_FLOAT      = 1u << 2,
    UFR_FINAL_PROFILE_FLAG_INT32      = 1u << 3,
    UFR_FINAL_PROFILE_FLAG_INT16      = 1u << 4,
    UFR_FINAL_PROFILE_FLAG_EPOCH_SENSITIVE = 1u << 5
};

/* View handed to the user's MC. No copy is performed at this boundary.
 * `rows` points to phase-cultivated, row-major data. `row_start` is the
 * epoch-dependent cyclic starting row. The MC can walk rows modulo `row_count`
 * to obtain the same logical rotation as the built-in DIRECT path. */
typedef struct {
    const void *rows;
    size_t bytes;
    size_t bytes_per_row;
    uint32_t row_count;
    uint32_t row_start;
    uint32_t view_id;
    uint64_t epoch;
    uint64_t generation_id;
    uint32_t profile_id;       /* 1..N for custom profile; 0 for built-in */
    ufr_final_cultivation_profile_t built_in_profile;
} ufr_final_view_t;

typedef int (*ufr_final_view_consumer_fn)(const ufr_final_view_t *view,
                                          void *user_ctx);

/* Startup-only registration. Register profiles before creating the MC engine.
 * Registration is locked after the first engine is created. */
int ufr_final_profile_register(const ufr_final_profile_desc_t *desc,
                               uint32_t *out_profile_id);
int ufr_final_profile_query(uint32_t profile_id,
                            ufr_final_profile_desc_t *out_desc);

/* Opaque engine wrapper around the current PaperView/Back/Front implementation. */
typedef struct ufr_paperview_mc_engine ufr_paperview_mc_engine_t;

typedef struct {
    uint32_t abi_version;
    ufr_cultivation_mode_t cultivation;
    uint32_t chunk_bytes;       /* 0 = current default (4 x 64 KiB) */
    ufr_final_cultivation_profile_t final_profile;
    uint32_t custom_profile_id; /* required only for UFR_FINAL_CUSTOM */
    uint32_t generation_chunks; /* 0/1 = current chunk policy; >1 = fixed reuse */
} ufr_paperview_mc_config_t;

int ufr_paperview_mc_create(const ufr_paperview_mc_config_t *config,
                            ufr_paperview_mc_engine_t **out_engine);
int ufr_paperview_mc_start(ufr_paperview_mc_engine_t *engine);
int ufr_paperview_mc_stop(ufr_paperview_mc_engine_t *engine);
int ufr_paperview_mc_run_views(ufr_paperview_mc_engine_t *engine,
                               uint64_t chunks,
                               ufr_final_view_consumer_fn consumer,
                               void *user_ctx);
void ufr_paperview_mc_destroy(ufr_paperview_mc_engine_t *engine);

#ifdef __cplusplus
}
#endif

#endif

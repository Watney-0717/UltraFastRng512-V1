#ifndef ULTRAFAST_RNG512_MC_DETERMINISTIC_STREAMS_V1_H
#define ULTRAFAST_RNG512_MC_DETERMINISTIC_STREAMS_V1_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UFR_MC_STREAM_API_VERSION 1u
#define UFR_MC_STREAM_MASTER_SEED_BYTES 32u
#define UFR_MC_STREAM_SEED_BYTES 64u
#define UFR_MC_STREAM_ID_BYTES 32u

/* The worker topology is routing metadata only.  It is deliberately excluded
 * from seed derivation so changing the number of workers does not change the
 * logical random stream assigned to a block. */
enum {
    UFR_MC_STREAM_FLAG_CRN = 1u << 0,
    UFR_MC_STREAM_FLAG_WORKER_INDEPENDENT = 1u << 1,
    UFR_MC_STREAM_FLAG_BLOCK_ADDRESSABLE = 1u << 2
};

typedef struct {
    uint32_t api_version;
    uint32_t flags;
    uint8_t master_seed[UFR_MC_STREAM_MASTER_SEED_BYTES];
    uint64_t experiment_id;
    uint64_t run_id;
    uint64_t replication_id;
    uint64_t scenario_id;
    uint64_t crn_group_id;
    uint64_t logical_stream_id;
    uint64_t substream_id;
    uint64_t start_block;
    uint64_t block_count;
    uint32_t worker_id;
    uint32_t worker_count;
} ufr_mc_stream_plan_t;

typedef struct {
    uint32_t worker_id;
    uint32_t worker_count;
    uint64_t first_block;
    uint64_t block_count;
} ufr_mc_stream_partition_t;

/* Default plan is deterministic and safe for a single logical stream. */
void ufr_mc_stream_plan_default(ufr_mc_stream_plan_t *plan);
int  ufr_mc_stream_plan_validate(const ufr_mc_stream_plan_t *plan);

/* Deterministically derive the 512-bit Back/Front handoff seed for a logical
 * stream block.  CRN mode intentionally excludes scenario_id from derivation;
 * non-CRN mode includes scenario_id.  worker_id/count are always excluded. */
int ufr_mc_stream_derive_seed64(const ufr_mc_stream_plan_t *plan,
                                uint64_t relative_block,
                                uint8_t out_seed64[UFR_MC_STREAM_SEED_BYTES]);

/* One-time root seed for a logical stream.  Use this when the underlying RNG
 * runs sequentially within a fixed logical substream; block derivation is then
 * only needed for addressable restart/partition modes. */
int ufr_mc_stream_derive_root_seed64(const ufr_mc_stream_plan_t *plan,
                                     uint8_t out_seed64[UFR_MC_STREAM_SEED_BYTES]);

/* Deterministic opaque identifier for audit/replay records. */
int ufr_mc_stream_make_id(const ufr_mc_stream_plan_t *plan,
                          uint8_t out_id[UFR_MC_STREAM_ID_BYTES]);

/* Create a worker-specific routing view.  This does not alter random seed
 * derivation; only the owned block interval changes. */
int ufr_mc_stream_partition(const ufr_mc_stream_plan_t *plan,
                            uint32_t worker_id,
                            uint32_t worker_count,
                            ufr_mc_stream_plan_t *out_plan,
                            ufr_mc_stream_partition_t *out_partition);

/* Equivalent stream after seeking to an absolute logical block.  This is a
 * stream-level seek primitive, not a claim about a PRNG-specific algebraic
 * jump function. */
int ufr_mc_stream_seek(const ufr_mc_stream_plan_t *plan,
                       uint64_t absolute_block,
                       ufr_mc_stream_plan_t *out_plan);

/* CRN helper: scenarios may differ in metadata while sharing exactly the same
 * RNG stream family.  The scenario_id is retained for audit but ignored by
 * seed derivation when UFR_MC_STREAM_FLAG_CRN is set. */
int ufr_mc_stream_make_crn_plan(const ufr_mc_stream_plan_t *base,
                               uint64_t scenario_id,
                               uint64_t crn_group_id,
                               ufr_mc_stream_plan_t *out_plan);

/* True when two plans are intended to produce the same logical RNG sequence
 * under CRN semantics. */
int ufr_mc_stream_crn_compatible(const ufr_mc_stream_plan_t *a,
                                 const ufr_mc_stream_plan_t *b);

#ifdef __cplusplus
}
#endif

#endif

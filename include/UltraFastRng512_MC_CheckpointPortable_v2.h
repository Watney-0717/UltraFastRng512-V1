#ifndef ULTRAFAST_RNG512_MC_CHECKPOINT_PORTABLE_V2_H
#define ULTRAFAST_RNG512_MC_CHECKPOINT_PORTABLE_V2_H

#include "UltraFastRng512_MC_Enterprise_v1.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UFR_MC_CHECKPOINT_WIRE_VERSION 2u
#define UFR_MC_CHECKPOINT_WIRE_MAGIC_BYTES 8u
#define UFR_MC_CHECKPOINT_WIRE_PAYLOAD_BYTES 36104u

/* Canonical, endian-independent SHA-256 over the logical checkpoint fields.
 * state_sha256 itself is excluded from the hash and then resealed. */
int ufr_mc_checkpoint_canonical_hash_v2(
    const ufr_mc_checkpoint_t *checkpoint,
    uint8_t out_sha256[32]);

/* Recompute stream_id and state_sha256 from the logical checkpoint. */
int ufr_mc_checkpoint_reseal_v2(ufr_mc_checkpoint_t *checkpoint);

/* Portable on-disk representation. No compiler padding, native endianness,
 * or sizeof(struct) is used. Trailing bytes are rejected on load. */
int ufr_mc_checkpoint_save_portable_v2(
    const char *path,
    const ufr_mc_checkpoint_t *checkpoint);

int ufr_mc_checkpoint_load_portable_v2(
    const char *path,
    ufr_mc_checkpoint_t *checkpoint);

#ifdef __cplusplus
}
#endif
#endif

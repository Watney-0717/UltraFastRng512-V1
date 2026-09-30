/*
 * UltraFastRng512 V1 — Submission Release
 *
 * Purpose:
 *   Freeze the validated Front/Back architecture with three independently
 *   prepared same-PROFILE VHV lineages used as three live parents.
 *
 * Frozen V1 defining points:
 *   - seed-side AAA profile is part of the frozen V1 mainline
 *   - AAA = ChaCha 14 double-rounds / 28 rounds, Column rr=9, word=8,
 *     ADD 0x3B54CDA5 (= floor(0.375 * 0x9E3779B9))
 *   - m11 split uses ADD(ROTL64(m11, 1)) instead of ADD(m11)
 *   - m15 remains the p0-side auxiliary (B->A reassignment)
 *   - all 16 resident states remain within the existing 16-state budget
 *   - Back seed exchange remains scalar-only; Front receives completed VHV sets
 *
 * Source status: frozen V1 release baseline. Statistical certification is
 * separate from this implementation and is not claimed by the source file.
 */
#define _GNU_SOURCE
#define NORMAL_GROUP 16u /* 16 Front steps => exactly one 64 KiB Normal starter */
#define UFR_NO_MAIN
/*
 * UltraFastRng512 — UltraFastRng512_Front3Parents_VHV_NoCounter_MaskUpdate1_Nonlinear
 *
 * Submission baseline: preserve the validated fast path and frozen Back-seed lineage without introducing experimental knobs into the hot path.
 *   Back-completed Front16 SeedSet -> 16 live ZMM -> Normal v9 4WAY I32 -> exact 4x fanout packed to int16
 *
 * Data geometry: each Front micro-step advances 16 independently
 * diffused Seed states and emits 16 pre-Normal vectors. Each receives
 * POST_FACTOR=4 cheap fanout vectors, so one micro-step commits 4 q-blocks.
 * NORMAL_GROUP=16 produces one 64 KiB Normal starter. The public default MC
 * chunk is 4 PaperViews = 256 KiB; Direct/L1/L2 transport selection remains
 * explicit at the public MC boundary.
 *
 * The Back layer below uses the scalar persistent A/B seed-exchange
 * mechanism: bounded low-priority preparation,
 * scalar handoff slots, READY/ACTIVE/FREE publication, and rare switching.
 *
 * The Post v11 framework is embedded later in this same translation unit.
 * Its per-batch path is fed with one compact MC-mean observation per Result
 * group, keeping the vector MC kernel free of Post/Evaluation work.
 *
 * Build:
 *   gcc -O3 -std=c11 -march=icelake-server -pthread \
 *       UltraFastRng512_MC_NORMALTAIL_OPTIMIZED_V1.c -lcrypto -lm -o ufr_v1
 *
 * Object/library mode:
 *   gcc -O3 -std=c11 -march=icelake-server -pthread -DUFR_EXTERNAL_MAIN -c \
       UltraFastRng512_MC_NORMALTAIL_OPTIMIZED_V1.c
 */
#include <immintrin.h>
#include <stdatomic.h>
#include <stdalign.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include "UltraFastRng512_MC_Enterprise_v1.h"
#include "UltraFastRng512_MC_CheckpointPortable_v2.h"


/* -------------------------------------------------------------------------
 * Embedded VHV SeedBack API declarations.
 *
 * The implementation is already embedded in this translation unit.  These
 * declarations keep the public names/types
 * locally defined without requiring a second header file.
 * ------------------------------------------------------------------------- */
#ifndef UFRNG_SEED_BYTES
#define UFRNG_SEED_BYTES 64u
#endif
#ifndef UFRNG_SHA256_BYTES
#define UFRNG_SHA256_BYTES 32u
#endif
#ifndef UFRNG_SHA512_BYTES
#define UFRNG_SHA512_BYTES 64u
#endif
#ifndef UFRNG_NODE_ID_BYTES
#define UFRNG_NODE_ID_BYTES 16u
#endif
#ifndef UFRNG_RUN_ID_BYTES
#define UFRNG_RUN_ID_BYTES 16u
#endif
#ifndef UFRNG_FRONT_BYTES
#define UFRNG_FRONT_BYTES 64u
#endif
#ifndef UFRNG_FRONT16_COUNT
#define UFRNG_FRONT16_COUNT 16u
#endif
#ifndef UFRNG_PROFILE_ID_BYTES
#define UFRNG_PROFILE_ID_BYTES 64u
#endif
#ifndef UFRNG_CONSTRUCTION_BYTES
#define UFRNG_CONSTRUCTION_BYTES 128u
#endif

#ifndef UFRNG_PROFILE_CHACHA20
#define UFRNG_PROFILE_CHACHA20 "CHACHA20-V1"
#endif
#ifndef UFRNG_PROFILE_AES256
#define UFRNG_PROFILE_AES256 "AES256-CTR-V1"
#endif
#ifndef UFRNG_PROFILE_VARIANT_A
#define UFRNG_PROFILE_VARIANT_A "VARIANT-A-V1"
#endif
#ifndef UFRNG_PROFILE_VARIANT_B
#define UFRNG_PROFILE_VARIANT_B "VARIANT-B-V1"
#endif
#ifndef UFRNG_FRONT16_CONSTRUCTION_VERSION
#define UFRNG_FRONT16_CONSTRUCTION_VERSION "UFRNG-FRONT16-SHA512-V1"
#endif
#ifndef UFRNG_FRONT16_VHV_COMPACT_VERSION
#define UFRNG_FRONT16_VHV_COMPACT_VERSION "UFRNG-FRONT16-VHV-COMPACT-V1"
#endif

typedef struct {
    uint8_t data[UFRNG_FRONT_BYTES];
} UFRNG_FrontSeed;

typedef struct {
    uint64_t seed_id;
    uint8_t  node_id[UFRNG_NODE_ID_BYTES];
    uint8_t  run_id[UFRNG_RUN_ID_BYTES];
    uint64_t sequence;
    const char *pipeline_version;
    const char *lps_profile;
    const char *profile_id;
} UFRNG_SeedMetadata;

typedef struct {
    UFRNG_SeedMetadata meta;
    uint64_t bank_index;
    char root_fingerprint_hex[65];
    char handoff_fingerprint_hex[65];
    char reproducible_id_hex[65];
} UFRNG_SeedBackRecord;

typedef struct {
    uint64_t bank_index;
    uint64_t generation;
    char profile_id[UFRNG_PROFILE_ID_BYTES];
    char construction_version[UFRNG_CONSTRUCTION_BYTES];
    uint8_t data[UFRNG_FRONT16_COUNT][UFRNG_SEED_BYTES];
    char fingerprint_hex[65];
} UFRNG_FrontSeedSet16;

typedef struct {
    UFRNG_SeedMetadata meta;
    uint64_t bank_index;
    uint64_t generation;
    uint32_t front_state_count;
    char profile_id[UFRNG_PROFILE_ID_BYTES];
    char construction_version[UFRNG_CONSTRUCTION_BYTES];
    char root_fingerprint_hex[65];
    char front16_fingerprint_hex[65];
    char reproducible_id_hex[65];
} UFRNG_Front16BackRecord;

int ufrng_make_back_record(const UFRNG_SeedMetadata *meta,
                           uint64_t bank_index,
                           const uint8_t root_seed64[UFRNG_SEED_BYTES],
                           const uint8_t handoff64[UFRNG_SEED_BYTES],
                           UFRNG_SeedBackRecord *out_record);

int ufrng_light_cleanup(const uint8_t *raw, size_t raw_len,
                        uint8_t *cleaned, size_t cleaned_cap,
                        size_t *cleaned_len);

int ufrng_condition_entropy(const uint8_t *raw, size_t raw_len,
                            const UFRNG_SeedMetadata *meta,
                            uint8_t out32[UFRNG_SHA256_BYTES]);

int ufrng_lps_x4_extract(const uint8_t conditioned32[UFRNG_SHA256_BYTES],
                         uint8_t out64[UFRNG_SHA512_BYTES]);

int ufrng_build_root_seed(const uint8_t *raw, size_t raw_len,
                          const UFRNG_SeedMetadata *meta,
                          uint8_t root_seed64[UFRNG_SEED_BYTES]);

int ufrng_seed_bank_entry(const uint8_t root_seed64[UFRNG_SEED_BYTES],
                          uint64_t bank_index,
                          uint8_t out64[UFRNG_SEED_BYTES]);

int ufrng_profile_apply(const uint8_t seed64[UFRNG_SEED_BYTES],
                        const UFRNG_SeedMetadata *meta,
                        uint8_t out64[UFRNG_SEED_BYTES]);

int ufrng_prepare_front_seed(const uint8_t seed64[UFRNG_SEED_BYTES],
                             const UFRNG_SeedMetadata *meta,
                             UFRNG_FrontSeed *out_front);

int ufrng_seedbank_profile_to_front(
    const uint8_t root_seed64[UFRNG_SEED_BYTES],
    uint64_t bank_index,
    const UFRNG_SeedMetadata *meta,
    UFRNG_FrontSeed *out_front,
    UFRNG_SeedBackRecord *out_record);

int ufrng_front16_expand_one(const uint8_t profiled_seed64[UFRNG_SEED_BYTES],
                             uint64_t bank_index,
                             uint64_t generation,
                             uint32_t lane,
                             const UFRNG_SeedMetadata *meta,
                             uint8_t out64[UFRNG_SEED_BYTES]);

int ufrng_seedbank_profile_to_front16(
    const uint8_t root_seed64[UFRNG_SEED_BYTES],
    uint64_t bank_index,
    uint64_t generation,
    const UFRNG_SeedMetadata *meta,
    UFRNG_FrontSeedSet16 *out_set,
    UFRNG_Front16BackRecord *out_record);

int ufrng_front16_vhv_expand_one(
    const uint8_t profiled_seed64[UFRNG_SEED_BYTES],
    const uint8_t *matrix1024,
    uint64_t bank_index,
    uint64_t generation,
    uint32_t lane,
    const UFRNG_SeedMetadata *meta,
    uint8_t out64[UFRNG_SEED_BYTES]);

int ufrng_seedbank_profile_to_front16_vhv(
    const uint8_t root_seed64[UFRNG_SEED_BYTES],
    uint64_t bank_index,
    uint64_t generation,
    const UFRNG_SeedMetadata *meta,
    UFRNG_FrontSeedSet16 *out_set,
    UFRNG_Front16BackRecord *out_record);
#include "ufrng_paperview_final_api_v1.h"
#include "UltraFastRng512_MC_OptimizationProfiles_v8.h"
#include "UltraFastRng512_MC_OptimizationProfiles_v9.h"
#include "UltraFastRng512_MC_Integrated_v4.h"
#include "UltraFastRng512_MC_Pharma_LinearPiecewiseTransition_Profile_v1.h"
#include <math.h>
#include <errno.h>
#ifdef __linux__
#include <sys/resource.h>
#endif
#if !defined(__AVX512F__) || !defined(__VAES__)
#error "Compile with AVX-512F + VAES (e.g. -march=icelake-server)."
#endif
#ifndef NORMAL_GROUP
#define NORMAL_GROUP 16u /* 16 Front16 micro-steps per 64 KiB Normal starter */
#endif
#ifndef RESULT_GROUP
#define RESULT_GROUP 64u
#endif
#ifndef STAGE_SLOTS
#define STAGE_SLOTS 2u /* 2 x 256 KiB producer slots = 512 KiB total */
#endif
#ifndef BLOCKS_TOTAL
#define BLOCKS_TOTAL UINT64_C(1048576)
#endif
#define UFR_FAST_FRONT_STATES 16u
#define UFR_FAST_POST_FACTOR 4u /* actual scalar expansion after each Front16 state */
#define UFR_FAST_QBLOCKS_PER_STEP 4u /* 16 states x 4 q-vectors = 4 x 256-sample blocks */
#define UFR_BACK_BANK_LANES 8u
#define UFR_BACK_FRONT_STATES 16u
#define UFR_BACK_CHUNK_STATES 2u
#define UFR_BACK_WORK_BUDGET 2u
#define UFR_BACK_COOLDOWN_NS UINT64_C(1000000)
#define UFR_FRONT_MIN_STEPS UINT64_C(1000000)
#define UFR_FRONT_READY_POLL 256u
#define UFR_C1 UINT64_C(0x9E3779B97F4A7C15)
#define UFR_C2 UINT64_C(0xD1B54A32D192ED03)
#define UFR_C3 UINT64_C(0x94D049BB133111EB)
#define UFR_DOMAIN_A UINT64_C(0xA5A5A5A55A5A5A5A)
#define UFR_DOMAIN_B UINT64_C(0x5A5A5A5AA5A5A5A5)

/* Mainline seed-side AAA profile, frozen from the long-run mining result.
 * 14 double-rounds = 28 total rounds; Column injection after rr=9 column QR;
 * word 8 receives one ADD of 0x3B54CDA5 (= floor(0.375 * 0x9E3779B9)). */
#ifndef UFRNG_PROFILE_CHACHA20_AAA
#define UFRNG_PROFILE_CHACHA20_AAA "CHACHA20-AAA-A1"
#endif
#define UFRNG_AAA_ROUNDS       14u
#define UFRNG_AAA_INJECT_RR    9u
#define UFRNG_AAA_WORD         8u
#define UFRNG_AAA_KICK         UINT32_C(0x3B54CDA5)
enum { UFR_SLOT_FREE=0, UFR_SLOT_PREPARING=1, UFR_SLOT_READY=2, UFR_SLOT_ACTIVE=3 };

/*
 * UltraFast RNG -> Normal 4WAY -> L1 -> Customer MC -> Direct/L1/L2 -> Common Post
 *
 * Production-oriented reference main file.
 *
 * FIXED DESIGN
 *   - Customer MC algorithm is a black-box boundary.
 *   - Direct / L1 / L2 are explicit caller-selected transport paths.
 *   - No automatic size threshold or route selection.
 *   - Normal quantized q (int32) is stored to cache before MC conversion.
 *   - Optional antithetic mode is OFF by default.
 *   - When enabled, one 256-sample Normal transform emits z[256] and -z[256]
 *     into a 2 KiB Normal Box slot; MC may exploit the pair when its estimator
 *     benefits from antithetic variates.
 *
 * NOTE
 *   The small compatibility driver below is retained for regression use.
 *   The public V1 engine path is the completed Back -> Front16 -> PaperView
 *   pipeline defined later in this translation unit.
 */
#include <immintrin.h>
#include <stdatomic.h>
#include <stdalign.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <sched.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Global configuration
 * ------------------------------------------------------------------------- */
#define UF_CACHELINE_BYTES 64u
#define UF_NORMAL_BASE_SAMPLES 256u
#define UF_NORMAL_BASE_BYTES 1024u
#define UF_NORMAL_ANTI_SAMPLES 512u
#define UF_NORMAL_ANTI_BYTES 2048u
#define UF_NORMAL_LINES 16u
#define UF_L1_NORMAL_SLOTS 4u
#define UF_L2_NORMAL_SLOTS 1024u
#define UF_RESULT_BYTES 64u
#define UF_L1_RESULT_SLOTS 4u
#define UF_L2_RESULT_SLOTS 2048u
#define UF_DEFAULT_BLOCKS 200000u
#define UF_WARMUP_BLOCKS 2000u
#define UF_SPSC_BLOCKS 100000u
#define UF_NORMAL_SCALE (0.0009317057574691516f * 1.0007781f)

_Static_assert(UF_NORMAL_BASE_SAMPLES * sizeof(uint32_t) == UF_NORMAL_BASE_BYTES,
               "uniform block size");
_Static_assert(UF_NORMAL_BASE_SAMPLES * sizeof(int32_t) == UF_NORMAL_BASE_BYTES,
               "normal base block size");
_Static_assert(UF_NORMAL_ANTI_SAMPLES * sizeof(int32_t) == UF_NORMAL_ANTI_BYTES,
               "normal antithetic block size");

/* -------------------------------------------------------------------------
 * Transport Boxes: Direct / L1 / L2
 * ------------------------------------------------------------------------- */
typedef enum {
    UF_BOX_L1 = 1,
    UF_BOX_L2 = 2
} uf_box_level_t;

typedef struct {
    void *data;
    size_t bytes;
    uint64_t slot_index;
} uf_box_item_t;

typedef struct {
    void *data;
    size_t capacity;
    uint64_t slot_index;
} uf_box_write_t;

typedef struct {
    uint8_t *storage;
    uint32_t *lengths;
    size_t slot_bytes;
    size_t slot_stride;
    uint32_t slot_count;
    uint32_t slot_mask;
    uf_box_level_t level;
    struct { _Alignas(UF_CACHELINE_BYTES) _Atomic uint64_t index; } write_cursor;
    struct { _Alignas(UF_CACHELINE_BYTES) _Atomic uint64_t index; } read_cursor;
} uf_cache_box_t;

typedef int (*uf_direct_consumer_fn)(const void *data, size_t bytes, void *ctx);
typedef struct { uf_direct_consumer_fn consumer; void *ctx; } uf_direct_box_t;

static size_t uf_round_cacheline(size_t x) {
    if (!x) return 0;
    const size_t m = UF_CACHELINE_BYTES - 1u;
    if (x > SIZE_MAX - m) return 0;
    return (x + m) & ~m;
}

static int uf_pow2_u32(uint32_t x) { return x && !(x & (x - 1u)); }

static int uf_box_init(uf_cache_box_t *b, uf_box_level_t level,
                       size_t slot_bytes, uint32_t slot_count) {
    if (!b || !slot_bytes || !slot_count || slot_bytes > UINT32_MAX) return -1;
    const size_t stride = uf_round_cacheline(slot_bytes);
    if (!stride || slot_count > SIZE_MAX / stride) return -1;
    const size_t storage_bytes = stride * (size_t)slot_count;
    const size_t lengths_bytes = uf_round_cacheline((size_t)slot_count * sizeof(uint32_t));
    if (!lengths_bytes) return -1;

    memset(b, 0, sizeof(*b));
    if (posix_memalign((void **)&b->storage, UF_CACHELINE_BYTES, storage_bytes) != 0)
        return -1;
    if (posix_memalign((void **)&b->lengths, UF_CACHELINE_BYTES, lengths_bytes) != 0) {
        free(b->storage); memset(b, 0, sizeof(*b)); return -1;
    }
    memset(b->storage, 0, storage_bytes);
    memset(b->lengths, 0, lengths_bytes);
    b->slot_bytes = slot_bytes;
    b->slot_stride = stride;
    b->slot_count = slot_count;
    b->slot_mask = uf_pow2_u32(slot_count) ? slot_count - 1u : 0u;
    b->level = level;
    atomic_init(&b->write_cursor.index, 0u);
    atomic_init(&b->read_cursor.index, 0u);
    return 0;
}

static int uf_l1_box_init(uf_cache_box_t *b, size_t slot_bytes, uint32_t slots) {
    return uf_box_init(b, UF_BOX_L1, slot_bytes, slots);
}
static int uf_l2_box_init(uf_cache_box_t *b, size_t slot_bytes, uint32_t slots) {
    return uf_box_init(b, UF_BOX_L2, slot_bytes, slots);
}
static void uf_box_destroy(uf_cache_box_t *b) {
    if (!b) return;
    free(b->lengths); free(b->storage); memset(b, 0, sizeof(*b));
}
static void uf_box_reset(uf_cache_box_t *b) {
    if (!b) return;
    atomic_store_explicit(&b->write_cursor.index, 0u, memory_order_relaxed);
    atomic_store_explicit(&b->read_cursor.index, 0u, memory_order_relaxed);
}
static inline uint32_t uf_slot_at(const uf_cache_box_t *b, uint64_t idx) {
    return b->slot_mask ? (uint32_t)idx & b->slot_mask
                        : (uint32_t)(idx % b->slot_count);
}

static int uf_box_try_acquire_write(uf_cache_box_t *b, uf_box_write_t *w) {
    if (!b || !w || !b->storage) return -1;
    const uint64_t wr = atomic_load_explicit(&b->write_cursor.index, memory_order_relaxed);
    const uint64_t rd = atomic_load_explicit(&b->read_cursor.index, memory_order_acquire);
    if (wr - rd >= b->slot_count) return -1;
    const uint32_t s = uf_slot_at(b, wr);
    w->data = b->storage + (size_t)s * b->slot_stride;
    w->capacity = b->slot_bytes;
    w->slot_index = wr;
    return 0;
}

static int uf_box_commit_write(uf_cache_box_t *b, const uf_box_write_t *w, size_t bytes) {
    if (!b || !w || !w->data || !b->storage || !bytes || bytes > w->capacity || bytes > b->slot_bytes)
        return -1;
    const uint64_t wr = atomic_load_explicit(&b->write_cursor.index, memory_order_relaxed);
    const uint32_t s = uf_slot_at(b, wr);
    uint8_t *expected = b->storage + (size_t)s * b->slot_stride;
    if (w->slot_index != wr || w->data != expected) return -1;
    b->lengths[s] = (uint32_t)bytes;
    atomic_store_explicit(&b->write_cursor.index, wr + 1u, memory_order_release);
    return 0;
}

static int uf_box_try_pop(uf_cache_box_t *b, uf_box_item_t *item) {
    if (!b || !item || !b->storage) return -1;
    const uint64_t rd = atomic_load_explicit(&b->read_cursor.index, memory_order_relaxed);
    const uint64_t wr = atomic_load_explicit(&b->write_cursor.index, memory_order_acquire);
    if (rd == wr) return -1;
    const uint32_t s = uf_slot_at(b, rd);
    item->data = b->storage + (size_t)s * b->slot_stride;
    item->bytes = b->lengths[s];
    item->slot_index = rd;
    return 0;
}

static void uf_box_release(uf_cache_box_t *b, const uf_box_item_t *item) {
    if (!b || !item) return;
    const uint64_t rd = atomic_load_explicit(&b->read_cursor.index, memory_order_relaxed);
    if (item->slot_index == rd)
        atomic_store_explicit(&b->read_cursor.index, rd + 1u, memory_order_release);
}

static int uf_direct_init(uf_direct_box_t *b, uf_direct_consumer_fn fn, void *ctx) {
    if (!b || !fn) return -1;
    b->consumer = fn; b->ctx = ctx; return 0;
}
static int uf_direct_send(const uf_direct_box_t *b, const void *data, size_t bytes) {
    if (!b || !b->consumer || !data || !bytes) return -1;
    return b->consumer(data, bytes, b->ctx);
}

/* -------------------------------------------------------------------------
 * Normal 4WAY v9 body
 * ------------------------------------------------------------------------- */
static const uint8_t normal_mag128[128] __attribute__((aligned(64))) = {
    /* Frozen V1 Normal-LUT tail-shape table. */
    0,4,6,4,5,5,2,3,2,5,1,6,11,4,3,3,1,6,6,6,6,14,0,13,15,7,2,7,5,11,9,3,    10,8,4,11,2,8,2,4,5,9,9,6,10,8,10,8,11,7,11,11,10,11,3,9,2,12,11,4,7,8,5,9,    9,9,5,8,7,4,11,12,2,9,3,11,10,11,11,7,11,8,10,8,10,6,9,9,5,4,2,8,2,11,4,8,    10,3,9,11,5,7,2,7,15,13,0,14,6,6,6,6,1,3,3,4,11,6,1,5,2,3,2,5,5,4,6,4,
};

static inline __m512i normal_lookup(__m512i x) {
    const __m512i t0 = _mm512_load_si512((const void *)&normal_mag128[0]);
    const __m512i t1 = _mm512_load_si512((const void *)&normal_mag128[64]);
    return _mm512_permutex2var_epi8(t0, x, t1);
}
static inline __m512i normal_transform16_i32(const uint32_t *in) {
    const __m512i x = _mm512_loadu_si512((const void *)in);
    const __m512i a = normal_lookup(x);
    const __m512i p16 = _mm512_maddubs_epi16(a, x);
    return _mm512_madd_epi16(p16, _mm512_set1_epi16(1));
}

static inline void normal_base_4way(const uint32_t *in, int32_t *out) {
    for (int base = 0; base < 16; base += 4) {
        const __m512i q0 = normal_transform16_i32(in + (base + 0) * 16);
        const __m512i q1 = normal_transform16_i32(in + (base + 1) * 16);
        const __m512i q2 = normal_transform16_i32(in + (base + 2) * 16);
        const __m512i q3 = normal_transform16_i32(in + (base + 3) * 16);
        _mm512_storeu_si512((void *)(out + (base + 0) * 16), q0);
        _mm512_storeu_si512((void *)(out + (base + 1) * 16), q1);
        _mm512_storeu_si512((void *)(out + (base + 2) * 16), q2);
        _mm512_storeu_si512((void *)(out + (base + 3) * 16), q3);
    }
}

static inline void normal_antithetic_4way(const uint32_t *in, int32_t *out) {
    int32_t *anti = out + UF_NORMAL_BASE_SAMPLES;
    const __m512i zero = _mm512_setzero_si512();
    for (int base = 0; base < 16; base += 4) {
        const __m512i q0 = normal_transform16_i32(in + (base + 0) * 16);
        const __m512i q1 = normal_transform16_i32(in + (base + 1) * 16);
        const __m512i q2 = normal_transform16_i32(in + (base + 2) * 16);
        const __m512i q3 = normal_transform16_i32(in + (base + 3) * 16);
        _mm512_storeu_si512((void *)(out + (base + 0) * 16), q0);
        _mm512_storeu_si512((void *)(out + (base + 1) * 16), q1);
        _mm512_storeu_si512((void *)(out + (base + 2) * 16), q2);
        _mm512_storeu_si512((void *)(out + (base + 3) * 16), q3);
        _mm512_storeu_si512((void *)(anti + (base + 0) * 16), _mm512_sub_epi32(zero, q0));
        _mm512_storeu_si512((void *)(anti + (base + 1) * 16), _mm512_sub_epi32(zero, q1));
        _mm512_storeu_si512((void *)(anti + (base + 2) * 16), _mm512_sub_epi32(zero, q2));
        _mm512_storeu_si512((void *)(anti + (base + 3) * 16), _mm512_sub_epi32(zero, q3));
    }
}

static int normal_generate_to_box(uf_cache_box_t *box, const uint32_t *u, int anti) {
    if (!box || !u) return -1;
    const size_t need = anti ? UF_NORMAL_ANTI_BYTES : UF_NORMAL_BASE_BYTES;
    if (box->slot_bytes < need) return -1;
    uf_box_write_t w;
    if (uf_box_try_acquire_write(box, &w) != 0) return -1;
    if (anti) normal_antithetic_4way(u, (int32_t *)w.data);
    else normal_base_4way(u, (int32_t *)w.data);
    return uf_box_commit_write(box, &w, need);
}

/* -------------------------------------------------------------------------
 * Uniform input hook
 * ------------------------------------------------------------------------- */
static inline uint64_t splitmix64_next(uint64_t *s) {
    uint64_t z = (*s += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

/* Deterministic compatibility hook retained for the embedded reference path.
 * The public MC engine uses the completed Back -> Front16 path below. */
static void fill_uniform_block(uint32_t *u, uint64_t *seed) {
    for (unsigned i = 0; i < UF_NORMAL_BASE_SAMPLES; ++i)
        u[i] = (uint32_t)(splitmix64_next(seed) >> 32);
}

/* -------------------------------------------------------------------------
 * Customer-MC black-box example and Common Post
 * ------------------------------------------------------------------------- */
typedef struct {
    double sum_value;
    uint64_t hits;
    uint64_t samples;
    uint64_t checksum;
    uint64_t reserved[4];
} mc_result_t;
_Static_assert(sizeof(mc_result_t) == UF_RESULT_BYTES, "mc_result_t must remain 64B");

typedef struct {
    double sum_value;
    uint64_t hits;
    uint64_t samples;
    uint64_t checksum;
    uint64_t blocks;
} post_acc_t;

static inline double ufr_mc_reduce16_pd(__m512 x) {
    const __m256 lo = _mm512_castps512_ps256(x);
    const __m256 hi = _mm512_extractf32x8_ps(x, 1);
    const __m512d dlo = _mm512_cvtps_pd(lo);
    const __m512d dhi = _mm512_cvtps_pd(hi);
    return _mm512_reduce_add_pd(_mm512_add_pd(dlo, dhi));
}

static inline __m512 mc_pair_kernel(__m512 fx, __m512 fy, __mmask16 *hit_out) {
    const __m512 one = _mm512_set1_ps(1.0f);
    const __m512 r2 = _mm512_fmadd_ps(fx, fx, _mm512_mul_ps(fy, fy));
    const __mmask16 hit = _mm512_cmp_ps_mask(r2, one, _CMP_LE_OQ);
    *hit_out = hit;
    return _mm512_mask_blend_ps(hit, _mm512_setzero_ps(), _mm512_sub_ps(one, r2));
}

/* Ordinary representative customer MC: 256 samples -> 128 XY pairs. */
static void customer_mc_256(const int32_t *n, mc_result_t *out) {
    double sum = 0.0;
    uint64_t hits = 0;
    const __m512 scale = _mm512_set1_ps(UF_NORMAL_SCALE);
    for (unsigned line = 0; line < 16; line += 2) {
        const __m512i ai = _mm512_load_si512((const void *)(n + line * 16u));
        const __m512i bi = _mm512_load_si512((const void *)(n + (line + 1u) * 16u));
        const __m512 fx = _mm512_mul_ps(_mm512_cvtepi32_ps(ai), scale);
        const __m512 fy = _mm512_mul_ps(_mm512_cvtepi32_ps(bi), scale);
        __mmask16 hm;
        const __m512 v = mc_pair_kernel(fx, fy, &hm);
        sum += ufr_mc_reduce16_pd(v);
        hits += (uint64_t)_mm_popcnt_u32((unsigned)hm);
    }
    out->sum_value = sum;
    out->hits = hits;
    out->samples = UF_NORMAL_BASE_SAMPLES / 2u;
    out->checksum = ((uint64_t)hits << 32) ^ (uint64_t)(sum * 1048576.0);
    memset(out->reserved, 0, sizeof(out->reserved));
}

/* Antithetic-aware representative MC.
 * The example estimator is g(z)=z+0.1*z^2.  For each pair (z,-z), the
 * average is 0.1*z^2, so the linear term is cancelled.  It is vectorized
 * deliberately: this file must not make the antithetic path look slow merely
 * because the demonstration MC uses a scalar libm call.  Real customer MC
 * remains a black box and decides whether pairing is useful. */
static void customer_mc_antithetic_example(const int32_t *n512, mc_result_t *out) {
    const int32_t *neg = n512 + UF_NORMAL_BASE_SAMPLES;
    const __m512 scale = _mm512_set1_ps(UF_NORMAL_SCALE);
    const __m512 c01 = _mm512_set1_ps(0.1f);
    double sum = 0.0;

    for (unsigned i = 0; i < UF_NORMAL_BASE_SAMPLES; i += 16) {
        const __m512 z = _mm512_mul_ps(
            _mm512_cvtepi32_ps(_mm512_load_si512((const void *)(n512 + i))), scale);
        const __m512 az = _mm512_mul_ps(
            _mm512_cvtepi32_ps(_mm512_load_si512((const void *)(neg + i))), scale);

        const __m512 gz = _mm512_fmadd_ps(z, z, z);
        const __m512 gaz = _mm512_fmadd_ps(az, az, az);
        const __m512 pair_avg = _mm512_mul_ps(
            _mm512_add_ps(gz, gaz), _mm512_set1_ps(0.5f));
        const __m512 result = _mm512_mul_ps(pair_avg, c01);

        sum += ufr_mc_reduce16_pd(result);
    }

    out->sum_value = sum;
    out->hits = UF_NORMAL_BASE_SAMPLES;
    out->samples = UF_NORMAL_BASE_SAMPLES;
    out->checksum = (uint64_t)(sum * 1048576.0);
    memset(out->reserved, 0, sizeof(out->reserved));
}

static inline void post_reset(post_acc_t *p) { memset(p, 0, sizeof(*p)); }
static inline void common_post(const void *data, size_t bytes, post_acc_t *p) {
    if (!data || !p || bytes < sizeof(mc_result_t)) return;
    const mc_result_t *x = (const mc_result_t *)data;
    p->sum_value += x->sum_value;
    p->hits += x->hits;
    p->samples += x->samples;
    p->checksum ^= x->checksum + UINT64_C(0x9E3779B97F4A7C15) +
                    (p->blocks << 6) + (p->blocks >> 2);
    ++p->blocks;
}
static int direct_post_cb(const void *data, size_t bytes, void *ctx) {
    common_post(data, bytes, (post_acc_t *)ctx); return 0;
}

/* -------------------------------------------------------------------------
 * Front16: Back-completed seeds -> ultra-light live state */
#ifndef A_STATE_COUNT
#define A_STATE_COUNT 16u
#endif
#ifndef V_STATE_COUNT
#define V_STATE_COUNT 0u
#endif
#ifndef FRONT_STATE_COUNT
#define FRONT_STATE_COUNT 16u
#endif

/*
 * The Front owns 16 already-diffused 64-byte states.  No seed expansion,
 * hashing, permutation-fission, or heavy conditioner work is done here.
 *
 * V1 light step:
 *     state += fixed per-lane Weyl/counter vector
 *     state ^= state >> 17
 *
 * The increment vectors are initialized only when a new Back slot becomes
 * ACTIVE, so the hot loop contains only the add/shift/xor state evolution.
 */
typedef struct __attribute__((aligned(64))) {
    __m512i s[UFR_FAST_FRONT_STATES];
    uint32_t mask_phase;
    uint32_t parent_xor_omit; /* 0=p0, 1=p1, 2=p2; fixed for this handoff */
} UFRFastFrontMem;

static inline __m512i ufr_front16_light(__m512i x, __m512i inc)
{
    x = _mm512_add_epi64(x, inc);
    x = _mm512_xor_si512(x, _mm512_srli_epi64(x, 17));
    return x;
}

static inline void ufr_front16_init_phase(UFRFastFrontMem *fm)
{
    if (fm) { fm->mask_phase = 0u; fm->parent_xor_omit = 0u; }
}

/*
 * V1 fanout layout: preserve the two shuffle instructions, four stores, and
 * int16 arithmetic used by the fixed 4-way expansion path.
 */
static const _Alignas(32) uint8_t UFR_FANOUT16_SHUF0[32] = {
    6,7,8,9,10,11,12,13,14,15,0,1,2,3,4,5,
    6,7,8,9,10,11,12,13,14,15,0,1,2,3,4,5
};
static const _Alignas(32) uint8_t UFR_FANOUT16_SHUF1[32] = {
    10,11,12,13,14,15,0,1,2,3,4,5,6,7,8,9,
    10,11,12,13,14,15,0,1,2,3,4,5,6,7,8,9
};

static inline void ufr_store_post4_i16(int16_t *d,unsigned *v,__m512i q){
    const __m256i q16 = _mm512_cvtepi32_epi16(q);
    const __m256i y0 = _mm256_shuffle_epi32(q16, _MM_SHUFFLE(2,3,0,1));
    const __m256i y1 = _mm256_shuffle_epi32(q16, _MM_SHUFFLE(1,0,3,2));
    _mm256_store_si256((__m256i*)(d+(*v)*16u), _mm256_add_epi16(q16,y0)); ++*v;
    _mm256_store_si256((__m256i*)(d+(*v)*16u), _mm256_sub_epi16(q16,y0)); ++*v;
    _mm256_store_si256((__m256i*)(d+(*v)*16u), _mm256_add_epi16(q16,y1)); ++*v;
    _mm256_store_si256((__m256i*)(d+(*v)*16u), _mm256_sub_epi16(q16,y1)); ++*v;
}

static inline void ufr_store_post4(int32_t *d,unsigned *v,__m512i q){
    const __m512i y0=_mm512_shuffle_epi32(q,_MM_SHUFFLE(2,3,0,1));
    const __m512i y1=_mm512_shuffle_epi32(q,_MM_SHUFFLE(1,0,3,2));
    _mm512_store_si512((void*)(d+(*v)*16u),_mm512_add_epi32(q,y0));++*v;
    _mm512_store_si512((void*)(d+(*v)*16u),_mm512_sub_epi32(q,y0));++*v;
    _mm512_store_si512((void*)(d+(*v)*16u),_mm512_add_epi32(q,y1));++*v;
    _mm512_store_si512((void*)(d+(*v)*16u),_mm512_sub_epi32(q,y1));++*v;
}

__attribute__((noinline))
static void ufr_fast_front_normal_group(UFRFastFrontMem *fm,int32_t *dst)
{
    __m512i p0=fm->s[0], p1=fm->s[1], p2=fm->s[2];
    __m512i m3=fm->s[3],m4=fm->s[4],m5=fm->s[5],m6=fm->s[6],m7=fm->s[7],m8=fm->s[8];
    __m512i m9=fm->s[9],m10=fm->s[10],m11=fm->s[11],m12=fm->s[12],m13=fm->s[13],m14=fm->s[14],m15=fm->s[15];
    unsigned phase=fm->mask_phase;
    for(unsigned k=0;k<NORMAL_GROUP;++k){
        int32_t *d=dst+(size_t)k*UF_NORMAL_BASE_SAMPLES*UFR_FAST_QBLOCKS_PER_STEP; unsigned out=0;
        const __m512i a=p0,b=p1,c=p2;
        switch (fm->parent_xor_omit) {
            case 0:
                p0=_mm512_xor_si512(a,_mm512_rol_epi64(b,17));
                p1=_mm512_add_epi64(b,_mm512_rol_epi64(c,29));
                p2=_mm512_add_epi64(c,_mm512_rol_epi64(a,41));
                break;
            case 1:
                p0=_mm512_add_epi64(a,_mm512_rol_epi64(b,17));
                p1=_mm512_xor_si512(b,_mm512_rol_epi64(c,29));
                p2=_mm512_add_epi64(c,_mm512_rol_epi64(a,41));
                break;
            default:
                p0=_mm512_add_epi64(a,_mm512_rol_epi64(b,17));
                p1=_mm512_add_epi64(b,_mm512_rol_epi64(c,29));
                p2=_mm512_xor_si512(c,_mm512_rol_epi64(a,41));
                break;
        }
#define EMIT(X) do{const __m512i q=normal_transform16_i32((const uint32_t *)&(X));ufr_store_post4(d,&out,q);}while(0)
        __m512i x;
        EMIT(p0);EMIT(p1);EMIT(p2);
        x=_mm512_xor_si512(p0,m3);EMIT(x);
        x=_mm512_xor_si512(p1,m7);EMIT(x);
        x=_mm512_add_epi64(p2,_mm512_rol_epi64(m11,1));EMIT(x);
        x=_mm512_xor_si512(p0,m4);EMIT(x);
        x=_mm512_xor_si512(p1,m8);EMIT(x);
        x=_mm512_xor_si512(p2,m12);EMIT(x);
        x=_mm512_xor_si512(p0,m5);EMIT(x);
        x=_mm512_xor_si512(p1,m9);EMIT(x);
        x=_mm512_xor_si512(p2,m13);EMIT(x);
        x=_mm512_xor_si512(p0,m6);EMIT(x);
        x=_mm512_xor_si512(p1,m10);EMIT(x);
        x=_mm512_xor_si512(p2,m14);EMIT(x);
        x=_mm512_xor_si512(p1,m15);EMIT(x);
#undef EMIT
        switch(phase){
            case 0: m3=_mm512_xor_si512(_mm512_add_epi64(m3,_mm512_rol_epi64(p0,7)),_mm512_rol_epi64(m3,13)); break;
            case 1: m4=_mm512_xor_si512(_mm512_add_epi64(m4,_mm512_rol_epi64(p0,13)),_mm512_rol_epi64(m4,17)); break;
            case 2: m5=_mm512_xor_si512(_mm512_add_epi64(m5,_mm512_rol_epi64(p0,17)),_mm512_rol_epi64(m5,21)); break;
            case 3: m6=_mm512_xor_si512(_mm512_add_epi64(m6,_mm512_rol_epi64(p0,23)),_mm512_rol_epi64(m6,29)); break;
            case 4: m7=_mm512_xor_si512(_mm512_add_epi64(m7,_mm512_rol_epi64(p1,29)),_mm512_rol_epi64(m7,33)); break;
            case 5: m8=_mm512_xor_si512(_mm512_add_epi64(m8,_mm512_rol_epi64(p1,31)),_mm512_rol_epi64(m8,37)); break;
            case 6: m9=_mm512_xor_si512(_mm512_add_epi64(m9,_mm512_rol_epi64(p1,37)),_mm512_rol_epi64(m9,41)); break;
            case 7: m10=_mm512_xor_si512(_mm512_add_epi64(m10,_mm512_rol_epi64(p1,42)),_mm512_rol_epi64(m10,47)); break;
            case 8: m11=_mm512_xor_si512(_mm512_add_epi64(m11,_mm512_rol_epi64(p2,11)),_mm512_rol_epi64(m11,17)); break;
            case 9: m12=_mm512_xor_si512(_mm512_add_epi64(m12,_mm512_rol_epi64(p2,19)),_mm512_rol_epi64(m12,23)); break;
            case 10: m13=_mm512_xor_si512(_mm512_add_epi64(m13,_mm512_rol_epi64(p2,27)),_mm512_rol_epi64(m13,31)); break;
            case 11: m14=_mm512_xor_si512(_mm512_add_epi64(m14,_mm512_rol_epi64(p2,37)),_mm512_rol_epi64(m14,41)); break;
            default: m15=_mm512_xor_si512(_mm512_add_epi64(m15,_mm512_rol_epi64(p2,43)),_mm512_rol_epi64(m15,47)); break;
        }
        if(++phase==13u) phase=0u;
    }
    fm->s[0]=p0;fm->s[1]=p1;fm->s[2]=p2;
    fm->s[3]=m3;fm->s[4]=m4;fm->s[5]=m5;fm->s[6]=m6;fm->s[7]=m7;fm->s[8]=m8;
    fm->s[9]=m9;fm->s[10]=m10;fm->s[11]=m11;fm->s[12]=m12;fm->s[13]=m13;fm->s[14]=m14;fm->s[15]=m15;
    fm->mask_phase=phase;
}

__attribute__((noinline))
static void ufr_fast_front_normal_group16(UFRFastFrontMem *fm,int16_t *dst)
{
    __m512i p0=fm->s[0], p1=fm->s[1], p2=fm->s[2];
    __m512i m3=fm->s[3],m4=fm->s[4],m5=fm->s[5],m6=fm->s[6],m7=fm->s[7],m8=fm->s[8];
    __m512i m9=fm->s[9],m10=fm->s[10],m11=fm->s[11],m12=fm->s[12],m13=fm->s[13],m14=fm->s[14],m15=fm->s[15];
    unsigned phase=fm->mask_phase;
    for(unsigned k=0;k<NORMAL_GROUP;++k){
        int16_t *d=dst+(size_t)k*UF_NORMAL_BASE_SAMPLES*UFR_FAST_QBLOCKS_PER_STEP; unsigned out=0;
        const __m512i a=p0,b=p1,c=p2;
        switch (fm->parent_xor_omit) {
            case 0:
                p0=_mm512_xor_si512(a,_mm512_rol_epi64(b,17));
                p1=_mm512_add_epi64(b,_mm512_rol_epi64(c,29));
                p2=_mm512_add_epi64(c,_mm512_rol_epi64(a,41));
                break;
            case 1:
                p0=_mm512_add_epi64(a,_mm512_rol_epi64(b,17));
                p1=_mm512_xor_si512(b,_mm512_rol_epi64(c,29));
                p2=_mm512_add_epi64(c,_mm512_rol_epi64(a,41));
                break;
            default:
                p0=_mm512_add_epi64(a,_mm512_rol_epi64(b,17));
                p1=_mm512_add_epi64(b,_mm512_rol_epi64(c,29));
                p2=_mm512_xor_si512(c,_mm512_rol_epi64(a,41));
                break;
        }
#define EMIT16(X) do{const __m512i q=normal_transform16_i32((const uint32_t *)&(X));ufr_store_post4_i16(d,&out,q);}while(0)
        __m512i x;
        EMIT16(p0);EMIT16(p1);EMIT16(p2);
        x=_mm512_xor_si512(p0,m3);EMIT16(x);
        x=_mm512_xor_si512(p1,m7);EMIT16(x);
        x=_mm512_add_epi64(p2,_mm512_rol_epi64(m11,1));EMIT16(x);
        x=_mm512_xor_si512(p0,m4);EMIT16(x);
        x=_mm512_xor_si512(p1,m8);EMIT16(x);
        x=_mm512_xor_si512(p2,m12);EMIT16(x);
        x=_mm512_xor_si512(p0,m5);EMIT16(x);
        x=_mm512_xor_si512(p1,m9);EMIT16(x);
        x=_mm512_xor_si512(p2,m13);EMIT16(x);
        x=_mm512_xor_si512(p0,m6);EMIT16(x);
        x=_mm512_xor_si512(p1,m10);EMIT16(x);
        x=_mm512_xor_si512(p2,m14);EMIT16(x);
        x=_mm512_xor_si512(p1,m15);EMIT16(x);
#undef EMIT16
        switch(phase){
            case 0: m3=_mm512_xor_si512(_mm512_add_epi64(m3,_mm512_rol_epi64(p0,7)),_mm512_rol_epi64(m3,13)); break;
            case 1: m4=_mm512_xor_si512(_mm512_add_epi64(m4,_mm512_rol_epi64(p0,13)),_mm512_rol_epi64(m4,17)); break;
            case 2: m5=_mm512_xor_si512(_mm512_add_epi64(m5,_mm512_rol_epi64(p0,17)),_mm512_rol_epi64(m5,21)); break;
            case 3: m6=_mm512_xor_si512(_mm512_add_epi64(m6,_mm512_rol_epi64(p0,23)),_mm512_rol_epi64(m6,29)); break;
            case 4: m7=_mm512_xor_si512(_mm512_add_epi64(m7,_mm512_rol_epi64(p1,29)),_mm512_rol_epi64(m7,33)); break;
            case 5: m8=_mm512_xor_si512(_mm512_add_epi64(m8,_mm512_rol_epi64(p1,31)),_mm512_rol_epi64(m8,37)); break;
            case 6: m9=_mm512_xor_si512(_mm512_add_epi64(m9,_mm512_rol_epi64(p1,37)),_mm512_rol_epi64(m9,41)); break;
            case 7: m10=_mm512_xor_si512(_mm512_add_epi64(m10,_mm512_rol_epi64(p1,42)),_mm512_rol_epi64(m10,47)); break;
            case 8: m11=_mm512_xor_si512(_mm512_add_epi64(m11,_mm512_rol_epi64(p2,11)),_mm512_rol_epi64(m11,17)); break;
            case 9: m12=_mm512_xor_si512(_mm512_add_epi64(m12,_mm512_rol_epi64(p2,19)),_mm512_rol_epi64(m12,23)); break;
            case 10: m13=_mm512_xor_si512(_mm512_add_epi64(m13,_mm512_rol_epi64(p2,27)),_mm512_rol_epi64(m13,31)); break;
            case 11: m14=_mm512_xor_si512(_mm512_add_epi64(m14,_mm512_rol_epi64(p2,37)),_mm512_rol_epi64(m14,41)); break;
            default: m15=_mm512_xor_si512(_mm512_add_epi64(m15,_mm512_rol_epi64(p2,43)),_mm512_rol_epi64(m15,47)); break;
        }
        if(++phase==13u) phase=0u;
    }
    fm->s[0]=p0;fm->s[1]=p1;fm->s[2]=p2;
    fm->s[3]=m3;fm->s[4]=m4;fm->s[5]=m5;fm->s[6]=m6;fm->s[7]=m7;fm->s[8]=m8;
    fm->s[9]=m9;fm->s[10]=m10;fm->s[11]=m11;fm->s[12]=m12;fm->s[13]=m13;fm->s[14]=m14;fm->s[15]=m15;
    fm->mask_phase=phase;
}

static inline uint64_t mc_accumulate_q64(const int32_t *n, __m512 *acc, __m512 scale2)
{
    const __m512 one = _mm512_set1_ps(1.0f);
    uint64_t hits = 0;
    const __m512i q0=_mm512_load_si512((const void*)(n+0*16)), q1=_mm512_load_si512((const void*)(n+1*16));
    const __m512i q2=_mm512_load_si512((const void*)(n+2*16)), q3=_mm512_load_si512((const void*)(n+3*16));
    const __m512i q4=_mm512_load_si512((const void*)(n+4*16)), q5=_mm512_load_si512((const void*)(n+5*16));
    const __m512i q6=_mm512_load_si512((const void*)(n+6*16)), q7=_mm512_load_si512((const void*)(n+7*16));
    const __m512i q8=_mm512_load_si512((const void*)(n+8*16)), q9=_mm512_load_si512((const void*)(n+9*16));
    const __m512i q10=_mm512_load_si512((const void*)(n+10*16)), q11=_mm512_load_si512((const void*)(n+11*16));
    const __m512i q12=_mm512_load_si512((const void*)(n+12*16)), q13=_mm512_load_si512((const void*)(n+13*16));
    const __m512i q14=_mm512_load_si512((const void*)(n+14*16)), q15=_mm512_load_si512((const void*)(n+15*16));
#define AP(A,B) do { __m512 x=_mm512_cvtepi32_ps(A), y=_mm512_cvtepi32_ps(B); __m512 q2=_mm512_fmadd_ps(x,x,_mm512_mul_ps(y,y)); __m512 r2=_mm512_mul_ps(q2,scale2); __mmask16 h=_mm512_cmp_ps_mask(r2,one,_CMP_LE_OQ); hits += (uint64_t)_mm_popcnt_u32((unsigned)h); *acc=_mm512_add_ps(*acc,_mm512_mask_blend_ps(h,_mm512_setzero_ps(),_mm512_sub_ps(one,r2))); } while(0)
    AP(q0,q1); AP(q2,q3); AP(q4,q5); AP(q6,q7); AP(q8,q9); AP(q10,q11); AP(q12,q13); AP(q14,q15);
#undef AP
    return hits;
}

static inline void mc_finalize_acc(__m512 acc, uint64_t hits, uint64_t blocks, mc_result_t *out)
{
    double sum = ufr_mc_reduce16_pd(acc);
    out->sum_value=sum;
    out->hits=hits;
    out->samples=(UF_NORMAL_BASE_SAMPLES/2u)*blocks;
    out->checksum=((uint64_t)hits<<32) ^ (uint64_t)(sum*1048576.0);
    memset(out->reserved,0,sizeof(out->reserved));
}

__attribute__((noinline))
static uint64_t mc_process_group(const uf_box_item_t *ni, __m512 *mc_acc, __m512 scale2)
{
    uint64_t hits = 0;
    for (unsigned k = 0; k < NORMAL_GROUP; ++k) {
        const int32_t *bp = (const int32_t *)ni->data +
            (size_t)k * (UF_NORMAL_BASE_SAMPLES * UFR_FAST_QBLOCKS_PER_STEP);
        for (unsigned d = 0; d < UFR_FAST_QBLOCKS_PER_STEP; ++d)
            hits += mc_accumulate_q64(bp + (size_t)d * UF_NORMAL_BASE_SAMPLES, mc_acc, scale2);
    }
    return hits;
}

/* ---------- Back: seed source + 16-way completed handoff ---------- */
typedef struct{_Alignas(64) uint64_t state[UFR_FAST_FRONT_STATES][8];uint64_t generation;_Atomic unsigned state_flag;} UFRXSlot;

typedef struct{
    UFRXSlot *slot;
    uint64_t domain;
    _Atomic int *stop;
    pthread_t thread;
    pthread_mutex_t wait_mutex;
    pthread_cond_t wait_cond;
    UFRNG_SeedMetadata meta;
    /* Multi-Seed Back: four independently conditioned 512-bit roots.
     * Front still receives exactly 16 x 64B states. */
    uint8_t root_seed64[4][64];
    uint64_t next_bank_index;
    uint64_t next_generation;
    UFRNG_FrontSeedSet16 pending_set;
    UFRNG_Front16BackRecord pending_record;
    UFRNG_Front16BackRecord last_record;
    unsigned pending_ready;
#ifdef __linux__
    int bound_cpu;
#endif
} UFRXWorker;

typedef struct{
    UFRXWorker back_a,back_b;
    UFRXSlot handoff_a,handoff_b;
    unsigned active;
    uint64_t active_steps,switches,ready_observations,missed_ready_checks;
#ifdef __linux__
    int engine_cpu;
#endif
} UFRXSeedExchange;

static void ufrx_lowprio(void)
{
#ifdef __linux__
    struct sched_param sp;
    memset(&sp,0,sizeof(sp));
    if (sched_setscheduler(0,SCHED_IDLE,&sp)==0) return;
    (void)setpriority(PRIO_PROCESS,0,19);
#endif
}

static void ufrx_cooldown(void)
{
    struct timespec ts={(time_t)(UFR_BACK_COOLDOWN_NS/1000000000ULL),
                        (long)(UFR_BACK_COOLDOWN_NS%1000000000ULL)};
    while(nanosleep(&ts,&ts)!=0&&errno==EINTR){}
}

#if defined(__GNUC__)
#define UFR_SCALAR_ONLY __attribute__((noinline,optimize("no-tree-vectorize,no-tree-slp-vectorize"),target("no-avx,no-avx2,no-sse,no-sse2")))
#else
#define UFR_SCALAR_ONLY
#endif

static UFR_SCALAR_ONLY int ufrx_copy_set_to_slot(UFRXSlot *slot,const UFRNG_FrontSeedSet16 *set)
{
    if(!slot||!set)return 0;
    for(unsigned i=0;i<UFR_FAST_FRONT_STATES;++i)
        for(unsigned j=0;j<64;++j)
            ((volatile uint8_t *)slot->state[i])[j]=((const volatile uint8_t *)set->data[i])[j];
    slot->generation=set->generation;
    return 1;
}


/* -------------------------------------------------------------------------
 * Scalar-only ancestor-guided pairwise Seed mixer.
 *
 * The Back seed-exchange budget is scalar-only.  This implementation keeps
 * the useful structure of the validated seed conditioner while
 * removing all vector instructions from this Back-side mixer:
 *   scalar GFNI-equivalent 8x8 affine table
 *   -> scalar nibble substitution / OR equivalent of VPSHUFB+TERNLOG
 *   -> scalar ChaCha4 (2 rounds)
 *   -> scalar XOR / majority recombination
 *
 * This is the fixed byte-affine/PShuffle/
 * Ternary logic mapping, not a claim that scalar lookup has the throughput
 * of GFNI.  It is intentionally Back-only.
 * ------------------------------------------------------------------------- */
static inline uint32_t ufr_scalar_rotl32(uint32_t x, unsigned r) {
    return (x << r) | (x >> (32u-r));
}

static const uint8_t UFR_SCALAR_GFNI_LUT[256] = {
    0x00, 0x71, 0xE3, 0x92, 0xCF, 0xBE, 0x2C, 0x5D, 0x9F, 0xEE, 0x7C, 0x0D, 0x50, 0x21, 0xB3, 0xC2, 0x3F, 0x4E, 0xDC, 0xAD, 0xF0, 0x81, 0x13, 0x62, 0xA0, 0xD1, 0x43, 0x32, 0x6F, 0x1E, 0x8C, 0xFD, 0x7E, 0x0F, 0x9D, 0xEC, 0xB1, 0xC0, 0x52, 0x23, 0xE1, 0x90, 0x02, 0x73, 0x2E, 0x5F, 0xCD, 0xBC, 0x41, 0x30, 0xA2, 0xD3, 0x8E, 0xFF, 0x6D, 0x1C, 0xDE, 0xAF, 0x3D, 0x4C, 0x11, 0x60, 0xF2, 0x83, 0xFC, 0x8D, 0x1F, 0x6E, 0x33, 0x42, 0xD0, 0xA1, 0x63, 0x12, 0x80, 0xF1, 0xAC, 0xDD, 0x4F, 0x3E, 0xC3, 0xB2, 0x20, 0x51, 0x0C, 0x7D, 0xEF, 0x9E, 0x5C, 0x2D, 0xBF, 0xCE, 0x93, 0xE2, 0x70, 0x01, 0x82, 0xF3, 0x61, 0x10, 0x4D, 0x3C, 0xAE, 0xDF, 0x1D, 0x6C, 0xFE, 0x8F, 0xD2, 0xA3, 0x31, 0x40, 0xBD, 0xCC, 0x5E, 0x2F, 0x72, 0x03, 0x91, 0xE0, 0x22, 0x53, 0xC1, 0xB0, 0xED, 0x9C, 0x0E, 0x7F, 0xF8, 0x89, 0x1B, 0x6A, 0x37, 0x46, 0xD4, 0xA5, 0x67, 0x16, 0x84, 0xF5, 0xA8, 0xD9, 0x4B, 0x3A, 0xC7, 0xB6, 0x24, 0x55, 0x08, 0x79, 0xEB, 0x9A, 0x58, 0x29, 0xBB, 0xCA, 0x97, 0xE6, 0x74, 0x05, 0x86, 0xF7, 0x65, 0x14, 0x49, 0x38, 0xAA, 0xDB, 0x19, 0x68, 0xFA, 0x8B, 0xD6, 0xA7, 0x35, 0x44, 0xB9, 0xC8, 0x5A, 0x2B, 0x76, 0x07, 0x95, 0xE4, 0x26, 0x57, 0xC5, 0xB4, 0xE9, 0x98, 0x0A, 0x7B, 0x04, 0x75, 0xE7, 0x96, 0xCB, 0xBA, 0x28, 0x59, 0x9B, 0xEA, 0x78, 0x09, 0x54, 0x25, 0xB7, 0xC6, 0x3B, 0x4A, 0xD8, 0xA9, 0xF4, 0x85, 0x17, 0x66, 0xA4, 0xD5, 0x47, 0x36, 0x6B, 0x1A, 0x88, 0xF9, 0x7A, 0x0B, 0x99, 0xE8, 0xB5, 0xC4, 0x56, 0x27, 0xE5, 0x94, 0x06, 0x77, 0x2A, 0x5B, 0xC9, 0xB8, 0x45, 0x34, 0xA6, 0xD7, 0x8A, 0xFB, 0x69, 0x18, 0xDA, 0xAB, 0x39, 0x48, 0x15, 0x64, 0xF6, 0x87
};

static inline uint8_t ufr_scalar_pshuf_ternlog_byte(uint8_t x) {
    static const uint8_t lo[16] = {
        0x00,0x07,0x0D,0x0B,0x06,0x0C,0x05,0x09,
        0x0E,0x03,0x0F,0x01,0x08,0x04,0x02,0x0A
    };
    static const uint8_t hi[16] = {
        0x00,0x0E,0x0B,0x06,0x09,0x01,0x0D,0x0C,
        0x03,0x0F,0x05,0x08,0x07,0x0A,0x04,0x02
    };
    const uint8_t y0 = lo[x & 0x0Fu];
    const uint8_t y1 = (uint8_t)(hi[(x >> 4) & 0x0Fu] << 4);
    /* TERNLOG 0xFE is OR(a,b,c). */
    return (uint8_t)(y0 | y1 | x);
}

static inline void ufr_scalar_chacha_qr(uint32_t *a,uint32_t *b,uint32_t *c,uint32_t *d) {
    *a += *b; *d ^= *a; *d = ufr_scalar_rotl32(*d,16);
    *c += *d; *b ^= *c; *b = ufr_scalar_rotl32(*b,12);
    *a += *b; *d ^= *a; *d = ufr_scalar_rotl32(*d,8);
    *c += *d; *b ^= *c; *b = ufr_scalar_rotl32(*b,7);
}

static UFR_SCALAR_ONLY void ufr_scalar_chacha4(uint32_t x[16]) {
    for (unsigned r=0;r<2;++r) {
        ufr_scalar_chacha_qr(&x[0],&x[4],&x[8], &x[12]);
        ufr_scalar_chacha_qr(&x[1],&x[5],&x[9], &x[13]);
        ufr_scalar_chacha_qr(&x[2],&x[6],&x[10],&x[14]);
        ufr_scalar_chacha_qr(&x[3],&x[7],&x[11],&x[15]);
        ufr_scalar_chacha_qr(&x[0],&x[5],&x[10],&x[15]);
        ufr_scalar_chacha_qr(&x[1],&x[6],&x[11],&x[12]);
        ufr_scalar_chacha_qr(&x[2],&x[7],&x[8], &x[13]);
        ufr_scalar_chacha_qr(&x[3],&x[4],&x[9], &x[14]);
    }
}

static UFR_SCALAR_ONLY void ufr_scalar_pair_mix(const uint8_t a64[64],const uint8_t b64[64],
                                       uint8_t out_a[64],uint8_t out_b[64]) {
    uint8_t ga[64], pb[64];
    uint32_t a[16],b[16],c[16][16];
    for (unsigned i=0;i<64;++i) {
        ga[i]=UFR_SCALAR_GFNI_LUT[((volatile const uint8_t *)a64)[i]];
        pb[i]=ufr_scalar_pshuf_ternlog_byte(((volatile const uint8_t *)b64)[i]);
    }
    uint32_t ag[16], bg[16];
    for (unsigned i=0;i<16;++i) {
        uint32_t va=0,vb=0;
        for (unsigned j=0;j<4;++j) {
            va |= (uint32_t)((volatile const uint8_t *)ga)[i*4u+j] << (8u*j);
            vb |= (uint32_t)((volatile const uint8_t *)pb)[i*4u+j] << (8u*j);
        }
        a[i]=0; b[i]=0;
        for (unsigned j=0;j<4;++j) {
            a[i] |= (uint32_t)((volatile const uint8_t *)a64)[i*4u+j] << (8u*j);
            b[i] |= (uint32_t)((volatile const uint8_t *)b64)[i*4u+j] << (8u*j);
        }
        ag[i]=va; bg[i]=vb;
    }
    for (unsigned i=0;i<16;++i) {
        c[0][i]=a[i]; c[1][i]=ufr_scalar_rotl32(a[i],7);
        c[2][i]=ufr_scalar_rotl32(a[i],13); c[3][i]=ufr_scalar_rotl32(a[i],19);
        c[4][i]=b[i]; c[5][i]=ufr_scalar_rotl32(b[i],5);
        c[6][i]=ufr_scalar_rotl32(b[i],11); c[7][i]=ufr_scalar_rotl32(b[i],17);
        c[8][i]=a[i]^b[i]; c[9][i]=ufr_scalar_rotl32(c[8][i],3);
        c[10][i]=ufr_scalar_rotl32(c[8][i],9); c[11][i]=ufr_scalar_rotl32(c[8][i],15);
        c[12][i]=0x61707865u; c[13][i]=0x3320646Eu;
        c[14][i]=0x79622D32u; c[15][i]=0x6B206574u;
    }
    for (unsigned lane=0; lane<16; ++lane) {
        uint32_t q[16];
        for (unsigned k=0;k<16;++k) q[k]=c[k][lane];
        ufr_scalar_chacha4(q);
        for (unsigned k=0;k<16;++k) c[k][lane]=q[k];
        for (unsigned k=0;k<16;++k) { volatile uint32_t *pq=&q[k]; *pq=0; }
    }
    for (unsigned i=0;i<16;++i) {
        const uint32_t z0=c[0][i]^c[8][i];
        const uint32_t z1=c[3][i]^c[11][i];
        const uint32_t aa=ag[i], bb=bg[i];
        const uint32_t ab=aa+bb;
        const uint32_t bc=bb+z0;
        const uint32_t gA=aa^ab^z1;
        const uint32_t gB=(bb&bc)|(bb&z0)|(bc&z0);
        volatile uint8_t *oa=out_a; volatile uint8_t *ob=out_b;
        for (unsigned j=0;j<4;++j) {
            oa[i*4u+j]=(uint8_t)(gA>>(8u*j));
            ob[i*4u+j]=(uint8_t)(gB>>(8u*j));
        }
    }
    for (unsigned i=0;i<64;++i) {
        volatile uint8_t *pga=&ga[i]; volatile uint8_t *ppb=&pb[i];
        *pga=0; *ppb=0;
        volatile uint8_t *pa=(uint8_t*)a; volatile uint8_t *pbx=(uint8_t*)b;
        *pa=0; *pbx=0;
    }
    for (unsigned k=0;k<16;++k) for (unsigned i=0;i<16;++i) {
        volatile uint32_t *pc=&c[k][i]; *pc=0;
    }
    for (unsigned i=0;i<16;++i) { volatile uint32_t *pag=&ag[i]; volatile uint32_t *pbg=&bg[i]; *pag=0; *pbg=0; }
}

static UFR_SCALAR_ONLY void ufr_ring_condition3_phase(uint8_t s0[64],uint8_t s1[64],uint8_t s2[64],unsigned flip) {
    uint8_t a0[64],a1[64],b0[64],b1[64],c0[64],c1[64],x[64],y[64],z[64];
    if(!flip) {
        ufr_scalar_pair_mix(s0,s1,a0,a1); ufr_scalar_pair_mix(s1,s2,b0,b1); ufr_scalar_pair_mix(s2,s0,c0,c1);
    } else {
        ufr_scalar_pair_mix(s1,s0,a0,a1); ufr_scalar_pair_mix(s2,s1,b0,b1); ufr_scalar_pair_mix(s0,s2,c0,c1);
    }
    uint32_t *X=(uint32_t*)x,*Y=(uint32_t*)y,*Z=(uint32_t*)z;
    const uint32_t *A0=(const uint32_t*)a0,*A1=(const uint32_t*)a1;
    const uint32_t *B0=(const uint32_t*)b0,*B1=(const uint32_t*)b1;
    const uint32_t *C0=(const uint32_t*)c0,*C1=(const uint32_t*)c1;
    for(unsigned i=0;i<16;++i) {
        X[i]=A0[i]^ufr_scalar_rotl32(A1[i],7)^ufr_scalar_rotl32(C0[i],17);
        Y[i]=B0[i]^ufr_scalar_rotl32(A1[i],13)^ufr_scalar_rotl32(B1[i],29);
        Z[i]=C1[i]^ufr_scalar_rotl32(B1[i],19)^ufr_scalar_rotl32(C0[i],31);
    }
    for(unsigned i=0;i<64;++i){s0[i]=x[i];s1[i]=y[i];s2[i]=z[i];}
    for(unsigned i=0;i<64;++i){volatile uint8_t *p; p=&a0[i];*p=0;p=&a1[i];*p=0;p=&b0[i];*p=0;p=&b1[i];*p=0;p=&c0[i];*p=0;p=&c1[i];*p=0;p=&x[i];*p=0;p=&y[i];*p=0;p=&z[i];*p=0;}
}

static UFR_SCALAR_ONLY int ufrx_prepare_next_multiK_impl(UFRXWorker *bw);

static UFR_SCALAR_ONLY int ufrx_prepare_next(UFRXWorker *bw)
{
    return ufrx_prepare_next_multiK_impl(bw);
}

static UFR_SCALAR_ONLY void ufrx_publish(UFRXWorker *bw)
{
    ufrx_copy_set_to_slot(bw->slot,&bw->pending_set);
    bw->last_record=bw->pending_record;
    bw->pending_ready=0;
    atomic_store_explicit(&bw->slot->state_flag,UFR_SLOT_READY,memory_order_release);
}

static int ufrx_claim(UFRXSlot *s)
{
    unsigned e=UFR_SLOT_FREE;
    return atomic_compare_exchange_strong_explicit(&s->state_flag,&e,
                                                    UFR_SLOT_PREPARING,
                                                    memory_order_acquire,
                                                    memory_order_relaxed);
}

#ifdef __linux__
static int ufrx_current_cpu(void)
{
    return sched_getcpu();
}

static void ufrx_bind_current_cpu(int cpu)
{
    if (cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    (void)sched_setaffinity(0, sizeof(set), &set);
}
#endif

static void *ufrx_worker_main(void *arg)
{
    UFRXWorker *bw=(UFRXWorker*)arg;
#ifdef __linux__
    if (bw) ufrx_bind_current_cpu(bw->bound_cpu);
#endif
    ufrx_lowprio();
    for(;;){
        if(atomic_load_explicit(bw->stop,memory_order_relaxed))break;
        if(!bw->pending_ready && !ufrx_prepare_next(bw)){
            ufrx_cooldown();
            continue;
        }
        pthread_mutex_lock(&bw->wait_mutex);
        while(!atomic_load_explicit(bw->stop,memory_order_relaxed) &&
              atomic_load_explicit(&bw->slot->state_flag,memory_order_acquire)!=UFR_SLOT_FREE)
            pthread_cond_wait(&bw->wait_cond,&bw->wait_mutex);
        int st=atomic_load_explicit(bw->stop,memory_order_relaxed);
        pthread_mutex_unlock(&bw->wait_mutex);
        if(st)break;
        if(!bw->pending_ready || !ufrx_claim(bw->slot))continue;
        ufrx_publish(bw);
    }
    return NULL;
}

static int ufrx_start(UFRXSeedExchange *x,_Atomic int *stop)
{
    x->back_a.stop=stop;x->back_b.stop=stop;
#ifdef __linux__
    x->engine_cpu = ufrx_current_cpu();
    if (x->engine_cpu < 0) return -1;
    /* One-core invariant: caller and both Back workers share this CPU. */
    ufrx_bind_current_cpu(x->engine_cpu);
    x->back_a.bound_cpu = x->engine_cpu;
    x->back_b.bound_cpu = x->engine_cpu;
#endif
    if(pthread_mutex_init(&x->back_a.wait_mutex,NULL))return -1;
    if(pthread_cond_init(&x->back_a.wait_cond,NULL)){pthread_mutex_destroy(&x->back_a.wait_mutex);return -1;}
    if(pthread_mutex_init(&x->back_b.wait_mutex,NULL)){pthread_cond_destroy(&x->back_a.wait_cond);pthread_mutex_destroy(&x->back_a.wait_mutex);return -1;}
    if(pthread_cond_init(&x->back_b.wait_cond,NULL)){pthread_mutex_destroy(&x->back_b.wait_mutex);pthread_cond_destroy(&x->back_a.wait_cond);pthread_mutex_destroy(&x->back_a.wait_mutex);return -1;}
    if(pthread_create(&x->back_a.thread,NULL,ufrx_worker_main,&x->back_a)){return -1;}
    if(pthread_create(&x->back_b.thread,NULL,ufrx_worker_main,&x->back_b)){
        atomic_store_explicit(stop,1,memory_order_relaxed);
        pthread_mutex_lock(&x->back_a.wait_mutex);pthread_cond_broadcast(&x->back_a.wait_cond);pthread_mutex_unlock(&x->back_a.wait_mutex);
        pthread_join(x->back_a.thread,NULL);return -1;
    }
    return 0;
}

static void ufrx_stop(UFRXSeedExchange *x,_Atomic int *stop)
{
    atomic_store_explicit(stop,1,memory_order_relaxed);
    pthread_mutex_lock(&x->back_a.wait_mutex);pthread_cond_broadcast(&x->back_a.wait_cond);pthread_mutex_unlock(&x->back_a.wait_mutex);
    pthread_mutex_lock(&x->back_b.wait_mutex);pthread_cond_broadcast(&x->back_b.wait_cond);pthread_mutex_unlock(&x->back_b.wait_mutex);
    pthread_join(x->back_a.thread,NULL);pthread_join(x->back_b.thread,NULL);
    pthread_cond_destroy(&x->back_a.wait_cond);pthread_mutex_destroy(&x->back_a.wait_mutex);
    pthread_cond_destroy(&x->back_b.wait_cond);pthread_mutex_destroy(&x->back_b.wait_mutex);
}

static UFRNG_SeedMetadata ufrx_make_meta(unsigned stream)
{
    static const char pipeline[]="Integrated-FastMC-Front16-VHV-V4";
    static const char lps[]="PSL2(F29)-X4";
    static const char profile[]=UFRNG_PROFILE_CHACHA20_AAA;
    UFRNG_SeedMetadata m;
    memset(&m,0,sizeof(m));
    m.pipeline_version=pipeline;
    m.lps_profile=lps;
    m.profile_id=profile;
    for(unsigned i=0;i<16;++i){m.node_id[i]=(uint8_t)(0x10u+i);m.run_id[i]=(uint8_t)(0xA0u+i+(stream?0x10u:0u));}
    m.sequence=(uint64_t)stream;
    m.seed_id=UINT64_C(0x1122334455667788)+stream;
    return m;
}

static int ufrx_build_root_from_raw(const uint64_t raw64[8],const UFRNG_SeedMetadata *meta,uint8_t out64[64])
{
    if(!raw64||!meta||!out64)return 0;
    return ufrng_build_root_seed((const uint8_t *)raw64,64,meta,out64);
}

static void ufrx_build_four_roots(UFRXWorker *bw);
static int ufrx_prepare_next(UFRXWorker *bw);

static void ufrx_init(UFRXSeedExchange *x,const uint64_t raw_a[8],const uint64_t raw_b[8])
{
    memset(x,0,sizeof(*x));
    atomic_init(&x->handoff_a.state_flag,UFR_SLOT_FREE);
    atomic_init(&x->handoff_b.state_flag,UFR_SLOT_FREE);

    x->back_a.meta=ufrx_make_meta(0);
    x->back_b.meta=ufrx_make_meta(1);
    x->back_a.domain=UFR_DOMAIN_A;
    x->back_b.domain=UFR_DOMAIN_B;
    x->back_a.slot=&x->handoff_a;
    x->back_b.slot=&x->handoff_b;

    (void)raw_a; (void)raw_b;
    ufrx_build_four_roots(&x->back_a);
    ufrx_build_four_roots(&x->back_b);

    x->back_a.next_bank_index=0; x->back_a.next_generation=0;
    x->back_b.next_bank_index=1; x->back_b.next_generation=1;

    if(!ufrx_prepare_next(&x->back_a) || !ufrx_prepare_next(&x->back_b))
        abort();
    ufrx_copy_set_to_slot(&x->handoff_a,&x->back_a.pending_set);
    x->back_a.last_record=x->back_a.pending_record;
    x->back_a.pending_ready=0;
    ufrx_copy_set_to_slot(&x->handoff_b,&x->back_b.pending_set);
    x->back_b.last_record=x->back_b.pending_record;
    x->back_b.pending_ready=0;

    /* Keep the original alternating bank/generation spacing. */
    x->back_a.next_bank_index=2; x->back_a.next_generation=2;
    x->back_b.next_bank_index=3; x->back_b.next_generation=3;
    atomic_store_explicit(&x->handoff_a.state_flag,UFR_SLOT_ACTIVE,memory_order_release);
    atomic_store_explicit(&x->handoff_b.state_flag,UFR_SLOT_READY,memory_order_release);
}

static inline void ufrx_load(UFRFastFrontMem *fm,const UFRXSlot *s,uint64_t domain)
{
    for(unsigned i=0;i<UFR_FAST_FRONT_STATES;++i)
        fm->s[i]=_mm512_loadu_si512((const void*)s->state[i]);
    (void)domain;
    ufr_front16_init_phase(fm);
    if (s) fm->parent_xor_omit = (unsigned)((s->generation >> 1) % 3u);
}

static int ufrx_try_switch(UFRFastFrontMem *fm,UFRXSeedExchange *x)
{
    UFRXSlot *cur=(x->active==0)?&x->handoff_a:&x->handoff_b;
    UFRXSlot *nxt=(x->active==0)?&x->handoff_b:&x->handoff_a;
    if(atomic_load_explicit(&nxt->state_flag,memory_order_acquire)!=UFR_SLOT_READY)return 0;
    ++x->ready_observations;
    unsigned e=UFR_SLOT_READY;
    if(!atomic_compare_exchange_strong_explicit(&nxt->state_flag,&e,UFR_SLOT_ACTIVE,
                                                memory_order_acquire,memory_order_relaxed))return 0;
    const uint64_t domain=(x->active==0)?UFR_DOMAIN_B:UFR_DOMAIN_A;
    ufrx_load(fm,nxt,domain);
    UFRXWorker *owner=(x->active==0)?&x->back_a:&x->back_b;
    pthread_mutex_lock(&owner->wait_mutex);
    atomic_store_explicit(&cur->state_flag,UFR_SLOT_FREE,memory_order_release);
    pthread_cond_signal(&owner->wait_cond);
    pthread_mutex_unlock(&owner->wait_mutex);
    x->active^=1u;x->active_steps=0;++x->switches;
    return 1;
}

/* ---------- Embedded Post v11 follows; driver below it ---------- */
#ifndef UF_MC_POST_ADVANCED_V10_H
#define UF_MC_POST_ADVANCED_V10_H

/*
 * UltraFast MC Post-Processing Framework v10
 *
 * All 8 rooms are concrete in v10:
 *   - Statistics / Validity / Distribution / Convergence / Transform
 *   - Pairing / Metadata / Advanced
 *
 * Distribution v10 uses fixed-range histogram quantiles by default so its hot
 * path is O(1) per value. The legacy bounded-centroid sketch remains available
 * as an explicit compatibility mode.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UF_POST_FRAME_VERSION 10u
#define UF_POST_MAX_ROOMS 8u
#define UF_POST_MAX_HIST_BINS 256u
#define UF_POST_DEFAULT_CENTROIDS 128u
#define UF_POST_DEFAULT_HIST_BINS 64u

#define UF_POST_ROOM_BIT(id) (UINT32_C(1) << (unsigned)(id))

enum {
    UF_POST_BATCH_PLAIN = 0u,
    UF_POST_BATCH_ANTITHETIC_PAIR = 1u << 0,
    UF_POST_BATCH_GROUPED = 1u << 1
};

typedef struct {
    const double *values;
    size_t count;
    size_t group_size;
    uint32_t flags;
    uint64_t batch_id;
} uf_post_batch_t;

typedef struct {
    const char *metric_name;
    const uf_post_batch_t *batches;
    size_t batch_count;
    const void *user_meta;
    size_t user_meta_bytes;
} uf_post_input_t;

typedef struct {
    uint64_t input_batches;
    uint64_t input_values;
    uint64_t accepted_values;
    uint64_t invalid_values;
    uint64_t invalid_nan;
    uint64_t invalid_inf;
    uint32_t enabled_room_mask;
    uint32_t failed_room_mask;
    uint32_t stop_requested;
    uint32_t stop_reason_mask;
} uf_post_status_t;

typedef double (*uf_post_transform_fn)(double x, void *ctx);

typedef enum {
    UF_POST_INDICATOR_GE = 0u,
    UF_POST_INDICATOR_GT = 1u,
    UF_POST_INDICATOR_LE = 2u,
    UF_POST_INDICATOR_LT = 3u,
    UF_POST_INDICATOR_EQ = 4u,
    UF_POST_INDICATOR_NE = 5u
} uf_post_indicator_op_t;

enum {
    UF_POST_TRANSFORM_SCALE    = 1u << 0,
    UF_POST_TRANSFORM_INDICATOR= 1u << 1,
    UF_POST_TRANSFORM_DERIVED  = 1u << 2
};

typedef struct {
    uint32_t flags;
    double scale;
    double offset;
    double indicator_threshold;
    uf_post_indicator_op_t indicator_op;
    uf_post_transform_fn derived_fn;
    void *derived_ctx;
} uf_post_transform_config_t;

/* Metadata Room configuration.
 * Strings are copied into the room state; caller-owned strings do not need
 * to remain alive after init. start_*_ns == 0 means "capture automatically". */
typedef struct {
    uint64_t seed;
    uint64_t start_wall_ns;
    uint64_t start_mono_ns;
    char rng_name[64];
    char mc_name[64];
    char run_label[64];
} uf_post_metadata_config_t;

typedef struct {
    uint64_t seed;
    uint64_t start_wall_ns;
    uint64_t end_wall_ns;
    uint64_t elapsed_ns;
    uint64_t input_batches;
    uint64_t input_values;
    uint64_t first_batch_id;
    uint64_t last_batch_id;
    uint8_t have_batch_id;
    uint8_t seed_consistent;
    uint8_t reserved[6];
    double values_per_sec;
    double batches_per_sec;
    char rng_name[64];
    char mc_name[64];
    char run_label[64];
} uf_post_metadata_result_t;

typedef enum {
    UF_POST_ROOM_STATISTICS   = 0,
    UF_POST_ROOM_DISTRIBUTION = 1,
    UF_POST_ROOM_VALIDITY     = 2,
    UF_POST_ROOM_CONVERGENCE  = 3,
    UF_POST_ROOM_TRANSFORM    = 4,
    UF_POST_ROOM_ADVANCED     = 5,
    UF_POST_ROOM_METADATA     = 6,
    UF_POST_ROOM_PAIRING      = 7
} uf_post_room_id_t;

typedef struct uf_post_room uf_post_room_t;

typedef struct {
    const char *name;
    int (*init)(uf_post_room_t *room, const void *config);
    void (*reset)(uf_post_room_t *room);
    int (*consume)(uf_post_room_t *room, const uf_post_input_t *input,
                   const uf_post_batch_t *batch);
    int (*merge)(uf_post_room_t *room, const uf_post_room_t *other);
    int (*finalize)(uf_post_room_t *room, void *out, size_t out_bytes);
    void (*destroy)(uf_post_room_t *room);
} uf_post_room_vtable_t;

struct uf_post_room {
    uf_post_room_id_t id;
    uint8_t enabled;
    uint8_t initialized;
    uint16_t reserved;
    uf_post_room_vtable_t vtable;
    void *state;
};

typedef struct {
    uint32_t enabled_room_mask;
    uint32_t room_required_mask;
    uint32_t reserved[2];
} uf_post_config_t;

enum {
    UF_POST_DISTRIBUTION_QUANTILE_HISTOGRAM = 0u,
    UF_POST_DISTRIBUTION_QUANTILE_CENTROID  = 1u
};

typedef struct {
    uint32_t max_centroids;      /* legacy centroid mode budget */
    uint32_t histogram_bins;     /* 0..UF_POST_MAX_HIST_BINS; 0 disables histogram */
    double histogram_min;
    double histogram_max;
    uint32_t quantile_mode;      /* histogram=default; centroid=legacy */
} uf_post_distribution_config_t;

typedef struct {
    uint64_t max_samples;          /* 0 = disabled; checked at batch boundary */
    double target_sem;             /* <= 0 = disabled */
    uint64_t max_elapsed_ns;       /* 0 = disabled; monotonic wall-clock */
    uint64_t min_samples;          /* SEM target is ignored below this count */
} uf_post_convergence_config_t;

typedef struct {
    uint64_t sample_count;
    uint64_t pair_count;
    double mean;
    double variance;
    double sem;
    uint64_t elapsed_ns;
    uint64_t max_samples;
    double target_sem;
    uint64_t max_elapsed_ns;
    uint64_t min_samples;
    uint8_t sem_valid;
    uint8_t target_sem_reached;
    uint8_t max_samples_reached;
    uint8_t timeout_reached;
    uint8_t should_stop;
    uint8_t reserved[3];
} uf_post_convergence_result_t;

enum {
    UF_POST_STOP_TARGET_SEM = 1u << 0,
    UF_POST_STOP_MAX_SAMPLES = 1u << 1,
    UF_POST_STOP_TIMEOUT = 1u << 2
};

enum {
    UF_POST_CONSUME_ERROR = -1,
    UF_POST_CONSUME_OK = 0,
    UF_POST_CONSUME_STOP = 1
};

typedef struct {
    uf_post_room_t rooms[UF_POST_MAX_ROOMS];
    uf_post_config_t config;
    uf_post_status_t status;
    uint8_t initialized;
    uint8_t reserved[7];
} uf_post_engine_t;

typedef struct {
    uint64_t total_values;
    uint64_t finite_values;
    uint64_t invalid_values;
    uint64_t nan_values;
    uint64_t inf_values;
} uf_post_validity_result_t;

typedef struct {
    uint64_t count;
    double mean;
    double variance;
    double standard_deviation;
    double sem;
    double min_value;
    double max_value;
} uf_post_statistics_result_t;

#define UF_POST_DISTRIBUTION_MAX_OUTPUT_BINS UF_POST_MAX_HIST_BINS

typedef struct {
    uint64_t count;
    uint64_t out_of_range_low;
    uint64_t out_of_range_high;
    uint32_t histogram_bins;
    double histogram_min;
    double histogram_max;
    double p50;
    double p95;
    double p99;
    uint64_t histogram[UF_POST_DISTRIBUTION_MAX_OUTPUT_BINS];
    uint64_t cdf[UF_POST_DISTRIBUTION_MAX_OUTPUT_BINS];
} uf_post_distribution_result_t;

typedef struct {
    uint64_t input_finite_count;
    uint64_t transformed_count;
    uint64_t transformed_invalid_count;
    double transformed_sum;
    double transformed_mean;
    double transformed_min;
    double transformed_max;
    uint64_t indicator_count;
    uint64_t indicator_hits;
    double indicator_probability;
} uf_post_transform_result_t;

/* Pairing Room result.
 * For UF_POST_BATCH_ANTITHETIC_PAIR, each valid (x,y) pair contributes
 * one observation m=(x+y)/2.  The returned SEM is therefore based on the
 * pair-average observations, not on 2*N falsely-independent samples.
 * The room also tracks the within-pair covariance needed to expose the
 * antithetic variance effect explicitly.
 */
typedef struct {
    uint64_t input_values;
    uint64_t complete_pairs;
    uint64_t valid_pairs;
    uint64_t invalid_pairs;
    uint64_t standalone_values;
    double mean;
    double variance;
    double standard_deviation;
    double sem;
    double first_mean;
    double second_mean;
    double first_variance;
    double second_variance;
    double pair_covariance;
    double naive_individual_variance;
    double pair_mean_variance_formula;
} uf_post_pairing_result_t;

/* -------------------------------------------------------------------------
 * Advanced Room
 *
 * This room is intentionally multi-run / summary oriented. It does not
 * retain caller-owned sample arrays. A run is represented by a compact
 * summary (mean, variance, N) plus optional scenario/level/parameter metadata.
 *
 * Supported building blocks in v9:
 *   - Multi-run catalog / scenario comparison
 *   - Richardson extrapolation from two runs
 *   - Central finite-difference sensitivity
 *   - MLMC estimator assembly from level corrections
 *
 * These operations are explicit API calls. The generic per-batch consume path
 * remains a no-op so Advanced never changes ordinary single-run hot paths.
 */
#define UF_POST_ADVANCED_MAX_RUNS   64u
#define UF_POST_ADVANCED_MAX_LEVELS 32u

typedef struct {
    uint64_t run_id;
    uint64_t scenario_id;
    uint32_t level;
    uint32_t reserved0;
    double parameter;
    double dt;
    double mean;
    double variance;
    uint64_t samples;
    double sem;
} uf_post_advanced_run_t;

typedef struct {
    uint32_t max_runs;   /* 1..UF_POST_ADVANCED_MAX_RUNS */
    uint32_t max_levels; /* 1..UF_POST_ADVANCED_MAX_LEVELS */
    uint32_t reserved[2];
} uf_post_advanced_config_t;

typedef struct {
    uint64_t run_count;
    uint64_t scenario_count;
    uint64_t level_count;
    uint64_t mlmc_level_count;
} uf_post_advanced_result_t;

typedef struct {
    double coarse_value;
    double fine_value;
    double order;
    double extrapolated_value;
    double estimated_fine_error;
    uint8_t valid;
    uint8_t reserved[7];
} uf_post_advanced_richardson_result_t;

typedef struct {
    double theta_minus;
    double theta_plus;
    double value_minus;
    double value_plus;
    double derivative;
    double derivative_sem;
    uint8_t valid;
    uint8_t reserved[7];
} uf_post_advanced_sensitivity_result_t;

typedef struct {
    uint32_t level;
    double mean_delta;
    double variance_delta;
    uint64_t samples;
    double cost_per_sample;
} uf_post_advanced_mlmc_level_t;

typedef struct {
    uint32_t level_count;
    double estimate;
    double variance;
    double sem;
    double cost;
    uint8_t valid;
    uint8_t reserved[7];
} uf_post_advanced_mlmc_result_t;

typedef struct {
    uint64_t run_a;
    uint64_t run_b;
    double value_a;
    double value_b;
    double difference;
    double difference_sem;
    uint8_t valid;
    uint8_t reserved[7];
} uf_post_advanced_compare_result_t;

void uf_post_transform_config_default(uf_post_transform_config_t *cfg);
int  uf_post_transform_config_validate(const uf_post_transform_config_t *cfg);

void uf_post_status_reset(uf_post_status_t *status);
void uf_post_config_init(uf_post_config_t *cfg);
void uf_post_enable_room(uf_post_config_t *cfg, uf_post_room_id_t id);
void uf_post_disable_room(uf_post_config_t *cfg, uf_post_room_id_t id);
int  uf_post_room_enabled(const uf_post_config_t *cfg, uf_post_room_id_t id);
const char *uf_post_room_name(uf_post_room_id_t id);

void uf_post_distribution_config_default(uf_post_distribution_config_t *cfg);
int uf_post_distribution_config_validate(const uf_post_distribution_config_t *cfg);

void uf_post_convergence_config_default(uf_post_convergence_config_t *cfg);
int  uf_post_convergence_config_validate(const uf_post_convergence_config_t *cfg);

void uf_post_metadata_config_default(uf_post_metadata_config_t *cfg);
int  uf_post_metadata_config_validate(const uf_post_metadata_config_t *cfg);

void uf_post_advanced_config_default(uf_post_advanced_config_t *cfg);
int  uf_post_advanced_config_validate(const uf_post_advanced_config_t *cfg);

int  uf_post_engine_init(uf_post_engine_t *engine,
                         const uf_post_config_t *cfg,
                         const uf_post_distribution_config_t *distribution_cfg,
                         const uf_post_convergence_config_t *convergence_cfg,
                         const uf_post_transform_config_t *transform_cfg,
                         const uf_post_metadata_config_t *metadata_cfg,
                         const uf_post_advanced_config_t *advanced_cfg);
void uf_post_engine_reset(uf_post_engine_t *engine);
void uf_post_engine_destroy(uf_post_engine_t *engine);
int  uf_post_engine_consume_batch(uf_post_engine_t *engine,
                                  const uf_post_input_t *input,
                                  const uf_post_batch_t *batch);
int  uf_post_engine_merge(uf_post_engine_t *engine,
                          const uf_post_engine_t *other);
int  uf_post_engine_finalize(uf_post_engine_t *engine,
                             uf_post_room_id_t room_id,
                             void *out, size_t out_bytes);

int  uf_post_advanced_add_run(uf_post_engine_t *engine,
                              const uf_post_advanced_run_t *run);
int  uf_post_advanced_get_run(const uf_post_engine_t *engine,
                              uint64_t run_id,
                              uf_post_advanced_run_t *out);
int  uf_post_advanced_compare_runs(const uf_post_engine_t *engine,
                                   uint64_t run_a, uint64_t run_b,
                                   uf_post_advanced_compare_result_t *out);
int  uf_post_advanced_richardson(const uf_post_engine_t *engine,
                                 uint64_t coarse_run_id, uint64_t fine_run_id,
                                 double order,
                                 uf_post_advanced_richardson_result_t *out);
int  uf_post_advanced_sensitivity(const uf_post_engine_t *engine,
                                 uint64_t minus_run_id, uint64_t plus_run_id,
                                 uf_post_advanced_sensitivity_result_t *out);
int  uf_post_advanced_mlmc_add_level(uf_post_engine_t *engine,
                                    const uf_post_advanced_mlmc_level_t *level);
int  uf_post_advanced_mlmc_finalize(const uf_post_engine_t *engine,
                                   uf_post_advanced_mlmc_result_t *out);
const uf_post_status_t *uf_post_engine_status(const uf_post_engine_t *engine);

#ifdef __cplusplus
}
#endif

#endif


#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define UF_POST_INTERNAL_MAX_CENTROIDS UF_POST_DEFAULT_CENTROIDS

typedef struct {
    double mean;
    uint64_t weight;
} centroid_t;

typedef struct {
    uint64_t count;
    uint32_t max_centroids;
    uint32_t hist_bins;
    double hist_min;
    double hist_max;
    uint64_t underflow;
    uint64_t overflow;
    centroid_t *centroids;
    uint32_t centroid_count;
    uint32_t quantile_mode;
    double hist_scale;
    uint64_t histogram[UF_POST_MAX_HIST_BINS];
} distribution_state_t;

static int valid_room_id(uf_post_room_id_t id) {
    return (unsigned)id < UF_POST_MAX_ROOMS;
}

const char *uf_post_room_name(uf_post_room_id_t id) {
    switch (id) {
        case UF_POST_ROOM_STATISTICS:   return "statistics";
        case UF_POST_ROOM_DISTRIBUTION: return "distribution";
        case UF_POST_ROOM_VALIDITY:     return "validity";
        case UF_POST_ROOM_CONVERGENCE:  return "convergence";
        case UF_POST_ROOM_TRANSFORM:    return "transform";
        case UF_POST_ROOM_ADVANCED:     return "advanced";
        case UF_POST_ROOM_METADATA:     return "metadata";
        case UF_POST_ROOM_PAIRING:      return "pairing";
        default:                        return "unknown";
    }
}

void uf_post_status_reset(uf_post_status_t *status) {
    if (status) memset(status, 0, sizeof(*status));
}

void uf_post_config_init(uf_post_config_t *cfg) {
    if (cfg) memset(cfg, 0, sizeof(*cfg));
}

void uf_post_enable_room(uf_post_config_t *cfg, uf_post_room_id_t id) {
    if (!cfg || !valid_room_id(id)) return;
    cfg->enabled_room_mask |= UF_POST_ROOM_BIT(id);
}

void uf_post_disable_room(uf_post_config_t *cfg, uf_post_room_id_t id) {
    if (!cfg || !valid_room_id(id)) return;
    cfg->enabled_room_mask &= ~UF_POST_ROOM_BIT(id);
}

int uf_post_room_enabled(const uf_post_config_t *cfg, uf_post_room_id_t id) {
    if (!cfg || !valid_room_id(id)) return 0;
    return (cfg->enabled_room_mask & UF_POST_ROOM_BIT(id)) != 0;
}

void uf_post_distribution_config_default(uf_post_distribution_config_t *cfg) {
    if (!cfg) return;
    cfg->max_centroids = UF_POST_DEFAULT_CENTROIDS;
    cfg->histogram_bins = UF_POST_DEFAULT_HIST_BINS;
    cfg->histogram_min = -5.0;
    cfg->histogram_max = 5.0;
    cfg->quantile_mode = UF_POST_DISTRIBUTION_QUANTILE_HISTOGRAM;
}

int uf_post_distribution_config_validate(const uf_post_distribution_config_t *cfg) {
    if (!cfg) return -1;
    if (cfg->quantile_mode > UF_POST_DISTRIBUTION_QUANTILE_CENTROID) return -1;
    if (cfg->quantile_mode == UF_POST_DISTRIBUTION_QUANTILE_CENTROID &&
        (cfg->max_centroids == 0u || cfg->max_centroids > UF_POST_INTERNAL_MAX_CENTROIDS)) return -1;
    if (cfg->histogram_bins > UF_POST_MAX_HIST_BINS) return -1;
    if (cfg->histogram_bins != 0u) {
        if (!isfinite(cfg->histogram_min) || !isfinite(cfg->histogram_max)) return -1;
        if (!(cfg->histogram_min < cfg->histogram_max)) return -1;
    }
    return 0;
}

void uf_post_convergence_config_default(uf_post_convergence_config_t *cfg) {
    if (!cfg) return;
    cfg->max_samples = 0u;
    cfg->target_sem = 0.0;
    cfg->max_elapsed_ns = 0u;
    cfg->min_samples = 1024u;
}

int uf_post_convergence_config_validate(const uf_post_convergence_config_t *cfg) {
    if (!cfg) return -1;
    if (!(cfg->target_sem > 0.0) && cfg->target_sem != 0.0) return -1;
    if (!isfinite(cfg->target_sem) || cfg->target_sem < 0.0) return -1;
    return 0;
}

/* -------------------------------------------------------------------------
 * Metadata Room configuration
 * ------------------------------------------------------------------------- */
static uint64_t metadata_wall_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0u;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static uint64_t metadata_mono_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0u;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

void uf_post_metadata_config_default(uf_post_metadata_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->rng_name, "unspecified", sizeof(cfg->rng_name) - 1u);
    strncpy(cfg->mc_name, "unspecified", sizeof(cfg->mc_name) - 1u);
    strncpy(cfg->run_label, "unspecified", sizeof(cfg->run_label) - 1u);
}

int uf_post_metadata_config_validate(const uf_post_metadata_config_t *cfg) {
    if (!cfg) return -1;
    if (memchr(cfg->rng_name, '\0', sizeof(cfg->rng_name)) == NULL) return -1;
    if (memchr(cfg->mc_name, '\0', sizeof(cfg->mc_name)) == NULL) return -1;
    if (memchr(cfg->run_label, '\0', sizeof(cfg->run_label)) == NULL) return -1;
    return 0;
}

/* -------------------------------------------------------------------------
 * Statistics Room (carried forward from v3)
 * ------------------------------------------------------------------------- */
typedef struct {
    uint64_t count;
    double mean;
    double m2;
    double min_value;
    double max_value;
} statistics_state_t;

static int statistics_init(uf_post_room_t *room, const void *config) {
    (void)config;
    if (!room) return -1;
    statistics_state_t *s = (statistics_state_t *)calloc(1, sizeof(*s));
    if (!s) return -1;
    s->min_value = DBL_MAX;
    s->max_value = -DBL_MAX;
    room->state = s; room->initialized = 1u;
    return 0;
}
static void statistics_reset(uf_post_room_t *room) {
    if (!room || !room->state) return;
    statistics_state_t *s = (statistics_state_t *)room->state;
    memset(s, 0, sizeof(*s)); s->min_value = DBL_MAX; s->max_value = -DBL_MAX;
}
static inline void statistics_add(statistics_state_t *s, double x) {
    const uint64_t n1 = s->count + 1u;
    const double d = x - s->mean;
    s->mean += d / (double)n1;
    const double d2 = x - s->mean;
    s->m2 += d * d2;
    s->count = n1;
    if (x < s->min_value) s->min_value = x;
    if (x > s->max_value) s->max_value = x;
}
static int statistics_consume(uf_post_room_t *room, const uf_post_input_t *input, const uf_post_batch_t *batch) {
    (void)input;
    if (!room || !room->state || !batch) return -1;
    if (batch->count && !batch->values) return -1;
    statistics_state_t *s = (statistics_state_t *)room->state;
    for (size_t i = 0; i < batch->count; ++i) if (isfinite(batch->values[i])) statistics_add(s, batch->values[i]);
    return 0;
}
static int statistics_merge(uf_post_room_t *room, const uf_post_room_t *other) {
    if (!room || !other || !room->state || !other->state) return -1;
    statistics_state_t *a = (statistics_state_t *)room->state;
    const statistics_state_t *b = (const statistics_state_t *)other->state;
    if (b->count == 0u) return 0;
    if (a->count == 0u) { *a = *b; return 0; }
    const double na = (double)a->count, nb = (double)b->count, n = na + nb;
    const double d = b->mean - a->mean;
    a->m2 += b->m2 + d * d * (na * nb / n);
    a->mean += d * (nb / n);
    a->count += b->count;
    if (b->min_value < a->min_value) a->min_value = b->min_value;
    if (b->max_value > a->max_value) a->max_value = b->max_value;
    return 0;
}
static int statistics_finalize(uf_post_room_t *room, void *out, size_t out_bytes) {
    if (!room || !room->state || !out || out_bytes < sizeof(uf_post_statistics_result_t)) return -1;
    const statistics_state_t *s = (const statistics_state_t *)room->state;
    uf_post_statistics_result_t *r = (uf_post_statistics_result_t *)out;
    memset(r, 0, sizeof(*r)); r->count = s->count; r->mean = s->mean;
    if (s->count >= 2u) { r->variance = s->m2 / (double)(s->count - 1u); r->standard_deviation = sqrt(r->variance); r->sem = r->standard_deviation / sqrt((double)s->count); }
    if (s->count) { r->min_value = s->min_value; r->max_value = s->max_value; }
    return 0;
}
static void statistics_destroy(uf_post_room_t *room) { if (!room) return; free(room->state); room->state = NULL; room->initialized = 0u; }
static const uf_post_room_vtable_t STATISTICS_VTABLE = {
    .name="statistics", .init=statistics_init, .reset=statistics_reset, .consume=statistics_consume,
    .merge=statistics_merge, .finalize=statistics_finalize, .destroy=statistics_destroy
};

/* -------------------------------------------------------------------------
 * Validity Room (carried forward from v3)
 * ------------------------------------------------------------------------- */
typedef struct {
    uint64_t total_values, finite_values, invalid_values, nan_values, inf_values;
} validity_state_t;
static int validity_init(uf_post_room_t *room, const void *config) {
    (void)config; if (!room) return -1; validity_state_t *s=(validity_state_t*)calloc(1,sizeof(*s)); if(!s)return -1; room->state=s; room->initialized=1u; return 0;
}
static void validity_reset(uf_post_room_t *room) { if(room&&room->state) memset(room->state,0,sizeof(validity_state_t)); }
static int validity_consume(uf_post_room_t *room,const uf_post_input_t *input,const uf_post_batch_t *batch) {
    (void)input; if(!room||!room->state||!batch)return -1; if(batch->count&&!batch->values)return -1; validity_state_t*s=(validity_state_t*)room->state; s->total_values+=(uint64_t)batch->count;
    for(size_t i=0;i<batch->count;++i){double x=batch->values[i]; if(isnan(x)){++s->invalid_values;++s->nan_values;}else if(isinf(x)){++s->invalid_values;++s->inf_values;}else ++s->finite_values;} return 0;
}
static int validity_merge(uf_post_room_t *room,const uf_post_room_t *other){if(!room||!other||!room->state||!other->state)return -1;validity_state_t*a=(validity_state_t*)room->state;const validity_state_t*b=(const validity_state_t*)other->state;a->total_values+=b->total_values;a->finite_values+=b->finite_values;a->invalid_values+=b->invalid_values;a->nan_values+=b->nan_values;a->inf_values+=b->inf_values;return 0;}
static int validity_finalize(uf_post_room_t *room,void*out,size_t out_bytes){if(!room||!room->state||!out||out_bytes<sizeof(uf_post_validity_result_t))return -1;const validity_state_t*s=(const validity_state_t*)room->state;uf_post_validity_result_t*r=(uf_post_validity_result_t*)out;r->total_values=s->total_values;r->finite_values=s->finite_values;r->invalid_values=s->invalid_values;r->nan_values=s->nan_values;r->inf_values=s->inf_values;return 0;}
static void validity_destroy(uf_post_room_t *room){if(!room)return;free(room->state);room->state=NULL;room->initialized=0u;}
static const uf_post_room_vtable_t VALIDITY_VTABLE={.name="validity",.init=validity_init,.reset=validity_reset,.consume=validity_consume,.merge=validity_merge,.finalize=validity_finalize,.destroy=validity_destroy};

/* -------------------------------------------------------------------------
 * Distribution Room
 * ------------------------------------------------------------------------- */
static void dist_sort_centroids(centroid_t *c, uint32_t n) {
    for (uint32_t i=1;i<n;++i){centroid_t v=c[i];uint32_t j=i;while(j&&c[j-1].mean>v.mean){c[j]=c[j-1];--j;}c[j]=v;}
}

static void dist_compress(distribution_state_t *s) {
    if (!s || s->centroid_count <= s->max_centroids) return;
    dist_sort_centroids(s->centroids, s->centroid_count);
    while (s->centroid_count > s->max_centroids) {
        uint32_t best=0;
        double best_score=DBL_MAX;
        for(uint32_t i=0;i+1u<s->centroid_count;++i){
            double gap=fabs(s->centroids[i+1u].mean-s->centroids[i].mean);
            uint64_t w=s->centroids[i].weight+s->centroids[i+1u].weight;
            double score = gap * (double)(w ? w : 1u);
            if(score<best_score){best_score=score;best=i;}
        }
        centroid_t *a=&s->centroids[best], *b=&s->centroids[best+1u];
        uint64_t w=a->weight+b->weight;
        a->mean = (a->mean*(double)a->weight + b->mean*(double)b->weight) / (double)w;
        a->weight=w;
        memmove(b,b+1u,(size_t)(s->centroid_count-best-2u)*sizeof(*b));
        --s->centroid_count;
    }
}

static void dist_add_centroid(distribution_state_t *s, double x, uint64_t weight) {
    if(weight==0u)return;
    if(s->centroid_count < s->max_centroids){
        s->centroids[s->centroid_count++] = (centroid_t){x,weight};
        dist_sort_centroids(s->centroids,s->centroid_count);
        return;
    }
    /* Add one temporary centroid, then compress back to the configured budget. */
    s->centroids[s->centroid_count++] = (centroid_t){x,weight};
    dist_compress(s);
}

static double dist_quantile(const distribution_state_t *s,double q){
    if(!s||!s->count)return NAN;
    if(s->quantile_mode==UF_POST_DISTRIBUTION_QUANTILE_HISTOGRAM){
        if(s->hist_bins==0u)return NAN;
        const double target=q*(double)(s->count-1u);
        if(target<(double)s->underflow)return s->hist_min;
        double cumulative=(double)s->underflow;
        const double bin_width=(s->hist_max-s->hist_min)/(double)s->hist_bins;
        for(uint32_t i=0;i<s->hist_bins;++i){
            const uint64_t h=s->histogram[i];
            const double next=cumulative+(double)h;
            if(target<next){
                if(h==0u)return s->hist_min+((double)i+0.5)*bin_width;
                const double frac=(target-cumulative)/(double)h;
                return s->hist_min+((double)i+frac)*bin_width;
            }
            cumulative=next;
        }
        if(s->overflow)return s->hist_max;
        return s->hist_max;
    }
    if(s->centroid_count==0u)return NAN;
    double target=q*(double)(s->count-1u), cumulative=0.0;
    uint32_t idx=s->centroid_count-1u;
    for(uint32_t i=0;i<s->centroid_count;++i){
        double next=cumulative+(double)s->centroids[i].weight;
        if(target<next){idx=i;break;}
        cumulative=next;
    }
    double left=0.0;
    for(uint32_t i=0;i<idx;++i)left+=(double)s->centroids[i].weight;
    double right=left+(double)s->centroids[idx].weight;
    if(idx+1u<s->centroid_count && right>left){
        double t=(target-left)/(right-left);
        if(t<0.0)t=0.0;else if(t>1.0)t=1.0;
        return s->centroids[idx].mean+t*(s->centroids[idx+1u].mean-s->centroids[idx].mean);
    }
    return s->centroids[idx].mean;
}

static void dist_hist_add(distribution_state_t *s,double x){
    if(s->hist_bins==0u)return;
    if(x<s->hist_min){++s->underflow;return;}
    if(x>s->hist_max){++s->overflow;return;}
    double pos=(x-s->hist_min)*s->hist_scale;
    uint32_t bin=(x==s->hist_max) ? (s->hist_bins-1u) : (uint32_t)pos;
    if(bin>=s->hist_bins)bin=s->hist_bins-1u;
    ++s->histogram[bin];
}

static int distribution_init(uf_post_room_t *room,const void *config){
    if(!room)return -1;
    uf_post_distribution_config_t cfg;uf_post_distribution_config_default(&cfg);
    if(config){cfg=*(const uf_post_distribution_config_t*)config;}
    if(uf_post_distribution_config_validate(&cfg)!=0)return -1;
    distribution_state_t*s=(distribution_state_t*)calloc(1,sizeof(*s));if(!s)return -1;
    s->max_centroids=cfg.max_centroids;s->hist_bins=cfg.histogram_bins;s->hist_min=cfg.histogram_min;s->hist_max=cfg.histogram_max;s->quantile_mode=cfg.quantile_mode;
    s->hist_scale = cfg.histogram_bins ? (double)cfg.histogram_bins/(cfg.histogram_max-cfg.histogram_min) : 0.0;
    if(s->quantile_mode==UF_POST_DISTRIBUTION_QUANTILE_CENTROID){
        s->centroids=(centroid_t*)calloc(s->max_centroids+1u,sizeof(*s->centroids));
        if(!s->centroids){free(s);return -1;}
    }
    room->state=s;room->initialized=1u;return 0;
}
static void distribution_reset(uf_post_room_t *room){
    if(!room||!room->state)return;
    distribution_state_t*s=(distribution_state_t*)room->state;
    s->count=0;
    s->underflow=s->overflow=0;
    s->centroid_count=0;
    memset(s->histogram,0,sizeof(s->histogram));
}
static int distribution_consume(uf_post_room_t *room,const uf_post_input_t *input,const uf_post_batch_t *batch){
    (void)input;if(!room||!room->state||!batch)return -1;if(batch->count&&!batch->values)return -1;distribution_state_t*s=(distribution_state_t*)room->state;
    if(s->quantile_mode==UF_POST_DISTRIBUTION_QUANTILE_HISTOGRAM){
        for(size_t i=0;i<batch->count;++i){double x=batch->values[i];if(!isfinite(x))continue;dist_hist_add(s,x);++s->count;}
    }else{
        for(size_t i=0;i<batch->count;++i){double x=batch->values[i];if(!isfinite(x))continue;dist_hist_add(s,x);dist_add_centroid(s,x,1u);++s->count;}
    }
    return 0;
}
static int distribution_merge(uf_post_room_t *room,const uf_post_room_t *other){
    if(!room||!other||!room->state||!other->state)return -1;
    distribution_state_t*a=(distribution_state_t*)room->state;
    const distribution_state_t*b=(const distribution_state_t*)other->state;
    if(a->hist_bins!=b->hist_bins||a->hist_min!=b->hist_min||a->hist_max!=b->hist_max||a->max_centroids!=b->max_centroids||a->quantile_mode!=b->quantile_mode)return -1;
    if(b->count==0u)return 0;
    if(a->quantile_mode==UF_POST_DISTRIBUTION_QUANTILE_CENTROID){
        for(uint32_t i=0;i<b->centroid_count;++i)dist_add_centroid(a,b->centroids[i].mean,b->centroids[i].weight);
        dist_compress(a);
    }
    a->count+=b->count;a->underflow+=b->underflow;a->overflow+=b->overflow;
    for(uint32_t i=0;i<a->hist_bins;++i)a->histogram[i]+=b->histogram[i];
    return 0;
}
static int distribution_finalize(uf_post_room_t *room,void*out,size_t out_bytes){
    if(!room||!room->state||!out||out_bytes<sizeof(uf_post_distribution_result_t))return -1;
    distribution_state_t*s=(distribution_state_t*)room->state;
    uf_post_distribution_result_t*r=(uf_post_distribution_result_t*)out;
    memset(r,0,sizeof(*r));
    r->count=s->count;r->out_of_range_low=s->underflow;r->out_of_range_high=s->overflow;r->histogram_bins=s->hist_bins;r->histogram_min=s->hist_min;r->histogram_max=s->hist_max;
    r->p50=dist_quantile(s,0.50);r->p95=dist_quantile(s,0.95);r->p99=dist_quantile(s,0.99);
    for(uint32_t i=0;i<s->hist_bins;++i){r->histogram[i]=s->histogram[i];r->cdf[i]=(i==0u)?r->histogram[i]:r->cdf[i-1u]+r->histogram[i];}
    return 0;
}
static void distribution_destroy(uf_post_room_t *room){if(!room)return;if(room->state){distribution_state_t*s=(distribution_state_t*)room->state;free(s->centroids);free(s);}room->state=NULL;room->initialized=0u;}
static const uf_post_room_vtable_t DISTRIBUTION_VTABLE={.name="distribution",.init=distribution_init,.reset=distribution_reset,.consume=distribution_consume,.merge=distribution_merge,.finalize=distribution_finalize,.destroy=distribution_destroy};

/* -------------------------------------------------------------------------
 * Convergence / Control Room
 * ------------------------------------------------------------------------- */
typedef struct {
    uint64_t sample_count;
    uint64_t pair_count;
    double mean;
    double m2;
    uint64_t started_ns;
    uint64_t elapsed_ns;
    uint64_t max_samples;
    double target_sem;
    uint64_t max_elapsed_ns;
    uint64_t min_samples;
    uint8_t sem_valid;
    uint8_t target_sem_reached;
    uint8_t max_samples_reached;
    uint8_t timeout_reached;
    uint8_t should_stop;
    uint8_t reserved[3];
} convergence_state_t;

static uint64_t convergence_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0u;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static inline void convergence_add(convergence_state_t *s, double x) {
    const uint64_t n1 = s->sample_count + 1u;
    const double d = x - s->mean;
    s->mean += d / (double)n1;
    const double d2 = x - s->mean;
    s->m2 += d * d2;
    s->sample_count = n1;
}

static void convergence_eval(convergence_state_t *s) {
    if (!s) return;
    s->sem_valid = (s->sample_count >= 2u) ? 1u : 0u;

    /*
     * SEM is needed during consume() only when target-SEM stopping is active.
     * Otherwise compute it only once at finalize().  This keeps ordinary MC
     * batches from paying a needless sqrt/divide on every batch.
     */
    if (s->target_sem > 0.0 && s->sample_count >= s->min_samples && s->sem_valid) {
        const double variance = s->m2 / (double)(s->sample_count - 1u);
        if (isfinite(variance) && variance >= 0.0) {
            const double sem = sqrt(variance / (double)s->sample_count);
            if (sem <= s->target_sem) s->target_sem_reached = 1u;
        } else {
            s->sem_valid = 0u;
        }
    }
    if (s->max_samples != 0u && s->sample_count >= s->max_samples) {
        s->max_samples_reached = 1u;
    }
    if (s->max_elapsed_ns != 0u && s->elapsed_ns >= s->max_elapsed_ns) {
        s->timeout_reached = 1u;
    }
    s->should_stop = (uint8_t)(s->target_sem_reached || s->max_samples_reached || s->timeout_reached);
}

static int convergence_init(uf_post_room_t *room, const void *config) {
    if (!room || !config) return -1;
    const uf_post_convergence_config_t *cfg = (const uf_post_convergence_config_t *)config;
    if (uf_post_convergence_config_validate(cfg) != 0) return -1;
    convergence_state_t *s = (convergence_state_t *)calloc(1, sizeof(*s));
    if (!s) return -1;
    s->max_samples = cfg->max_samples;
    s->target_sem = cfg->target_sem;
    s->max_elapsed_ns = cfg->max_elapsed_ns;
    s->min_samples = cfg->min_samples;
    s->sem_valid = 1u;
    s->started_ns = convergence_now_ns();
    room->state = s;
    room->initialized = 1u;
    return 0;
}

static void convergence_reset(uf_post_room_t *room) {
    if (!room || !room->state) return;
    convergence_state_t *s = (convergence_state_t *)room->state;
    const uint64_t max_samples = s->max_samples;
    const double target_sem = s->target_sem;
    const uint64_t max_elapsed_ns = s->max_elapsed_ns;
    const uint64_t min_samples = s->min_samples;
    memset(s, 0, sizeof(*s));
    s->max_samples = max_samples;
    s->target_sem = target_sem;
    s->max_elapsed_ns = max_elapsed_ns;
    s->min_samples = min_samples;
    s->sem_valid = 1u;
    s->started_ns = convergence_now_ns();
}

static int convergence_consume(uf_post_room_t *room, const uf_post_input_t *input, const uf_post_batch_t *batch) {
    (void)input;
    if (!room || !room->state || !batch) return UF_POST_CONSUME_ERROR;
    convergence_state_t *s = (convergence_state_t *)room->state;

    /* Clock only when the timeout feature is actually enabled. */
    if (s->max_elapsed_ns != 0u) {
        const uint64_t now = convergence_now_ns();
        if (now >= s->started_ns) s->elapsed_ns = now - s->started_ns;
    }

    if ((batch->flags & UF_POST_BATCH_ANTITHETIC_PAIR) != 0u) {
        if (batch->group_size != 2u || (batch->count % 2u) != 0u) return UF_POST_CONSUME_ERROR;
        for (size_t i = 0; i < batch->count; i += 2u) {
            const double a = batch->values[i];
            const double b = batch->values[i + 1u];
            if (!isfinite(a) || !isfinite(b)) continue;
            convergence_add(s, 0.5 * (a + b));
            ++s->pair_count;
        }
    } else {
        for (size_t i = 0; i < batch->count; ++i) {
            const double x = batch->values[i];
            if (isfinite(x)) convergence_add(s, x);
        }
    }

    if (s->max_elapsed_ns != 0u) {
        const uint64_t after = convergence_now_ns();
        if (after >= s->started_ns) s->elapsed_ns = after - s->started_ns;
    }
    convergence_eval(s);
    return s->should_stop ? UF_POST_CONSUME_STOP : UF_POST_CONSUME_OK;
}

static int convergence_merge(uf_post_room_t *room, const uf_post_room_t *other) {
    if (!room || !other || !room->state || !other->state) return UF_POST_CONSUME_ERROR;
    convergence_state_t *a = (convergence_state_t *)room->state;
    const convergence_state_t *b = (const convergence_state_t *)other->state;
    if (a->max_samples != b->max_samples || a->target_sem != b->target_sem ||
        a->max_elapsed_ns != b->max_elapsed_ns || a->min_samples != b->min_samples) return UF_POST_CONSUME_ERROR;
    if (b->sample_count != 0u) {
        if (a->sample_count == 0u) {
            a->sample_count = b->sample_count;
            a->pair_count = b->pair_count;
            a->mean = b->mean;
            a->m2 = b->m2;
        } else {
            const uint64_t na = a->sample_count;
            const uint64_t nb = b->sample_count;
            const uint64_t nt = na + nb;
            const double delta = b->mean - a->mean;
            a->mean += delta * ((double)nb / (double)nt);
            a->m2 += b->m2 + delta * delta * ((double)na * (double)nb / (double)nt);
            a->sample_count = nt;
            a->pair_count += b->pair_count;
        }
    }
    if (b->elapsed_ns > a->elapsed_ns) a->elapsed_ns = b->elapsed_ns;
    a->target_sem_reached |= b->target_sem_reached;
    a->max_samples_reached |= b->max_samples_reached;
    a->timeout_reached |= b->timeout_reached;
    convergence_eval(a);
    return a->should_stop ? UF_POST_CONSUME_STOP : UF_POST_CONSUME_OK;
}

static int convergence_finalize(uf_post_room_t *room, void *out, size_t out_bytes) {
    if (!room || !room->state || !out || out_bytes < sizeof(uf_post_convergence_result_t)) return UF_POST_CONSUME_ERROR;
    convergence_state_t *s = (convergence_state_t *)room->state;
    convergence_eval(s);
    uf_post_convergence_result_t *r = (uf_post_convergence_result_t *)out;
    memset(r, 0, sizeof(*r));
    r->sample_count = s->sample_count;
    r->pair_count = s->pair_count;
    r->mean = s->mean;
    r->variance = (s->sample_count >= 2u) ? s->m2 / (double)(s->sample_count - 1u) : NAN;
    r->sem = (s->sem_valid) ? sqrt(r->variance / (double)s->sample_count) : NAN;
    r->elapsed_ns = s->elapsed_ns;
    r->max_samples = s->max_samples;
    r->target_sem = s->target_sem;
    r->max_elapsed_ns = s->max_elapsed_ns;
    r->min_samples = s->min_samples;
    r->sem_valid = s->sem_valid;
    r->target_sem_reached = s->target_sem_reached;
    r->max_samples_reached = s->max_samples_reached;
    r->timeout_reached = s->timeout_reached;
    r->should_stop = s->should_stop;
    return 0;
}

static void convergence_destroy(uf_post_room_t *room) {
    if (!room) return;
    free(room->state);
    room->state = NULL;
    room->initialized = 0u;
}

static const uf_post_room_vtable_t CONVERGENCE_VTABLE = {
    .name = "convergence",
    .init = convergence_init,
    .reset = convergence_reset,
    .consume = convergence_consume,
    .merge = convergence_merge,
    .finalize = convergence_finalize,
    .destroy = convergence_destroy
};

/* -------------------------------------------------------------------------
 * Transform / Derived Room
 * ------------------------------------------------------------------------- */
typedef struct {
    uf_post_transform_config_t cfg;
    uint64_t input_finite_count;
    uint64_t transformed_count;
    uint64_t transformed_invalid_count;
    double transformed_sum;
    double transformed_mean;
    double transformed_min;
    double transformed_max;
    uint64_t indicator_count;
    uint64_t indicator_hits;
} transform_state_t;

void uf_post_transform_config_default(uf_post_transform_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->scale = 1.0;
    cfg->offset = 0.0;
    cfg->indicator_threshold = 0.0;
    cfg->indicator_op = UF_POST_INDICATOR_GE;
}

int uf_post_transform_config_validate(const uf_post_transform_config_t *cfg) {
    if (!cfg) return -1;
    if (cfg->flags & ~(UF_POST_TRANSFORM_SCALE |
                       UF_POST_TRANSFORM_INDICATOR |
                       UF_POST_TRANSFORM_DERIVED)) return -1;
    if ((cfg->flags & UF_POST_TRANSFORM_SCALE) != 0u) {
        if (!isfinite(cfg->scale) || !isfinite(cfg->offset)) return -1;
    }
    if ((cfg->flags & UF_POST_TRANSFORM_INDICATOR) != 0u) {
        if (!isfinite(cfg->indicator_threshold)) return -1;
        if ((unsigned)cfg->indicator_op > (unsigned)UF_POST_INDICATOR_NE) return -1;
    }
    if ((cfg->flags & UF_POST_TRANSFORM_DERIVED) != 0u && !cfg->derived_fn) return -1;
    return 0;
}

static double transform_apply_indicator(uf_post_indicator_op_t op, double x, double t) {
    switch (op) {
        case UF_POST_INDICATOR_GE: return x >= t ? 1.0 : 0.0;
        case UF_POST_INDICATOR_GT: return x >  t ? 1.0 : 0.0;
        case UF_POST_INDICATOR_LE: return x <= t ? 1.0 : 0.0;
        case UF_POST_INDICATOR_LT: return x <  t ? 1.0 : 0.0;
        case UF_POST_INDICATOR_EQ: return x == t ? 1.0 : 0.0;
        case UF_POST_INDICATOR_NE: return x != t ? 1.0 : 0.0;
        default: return NAN;
    }
}

static int transform_config_same(const uf_post_transform_config_t *a,
                                  const uf_post_transform_config_t *b) {
    if (!a || !b) return 0;
    return a->flags == b->flags &&
           a->scale == b->scale &&
           a->offset == b->offset &&
           a->indicator_threshold == b->indicator_threshold &&
           a->indicator_op == b->indicator_op &&
           a->derived_fn == b->derived_fn &&
           a->derived_ctx == b->derived_ctx;
}

static int transform_init(uf_post_room_t *room, const void *config) {
    if (!room) return -1;
    uf_post_transform_config_t cfg;
    uf_post_transform_config_default(&cfg);
    if (config) cfg = *(const uf_post_transform_config_t *)config;
    if (uf_post_transform_config_validate(&cfg) != 0) return -1;
    transform_state_t *s = (transform_state_t *)calloc(1, sizeof(*s));
    if (!s) return -1;
    s->cfg = cfg;
    s->transformed_min = DBL_MAX;
    s->transformed_max = -DBL_MAX;
    room->state = s;
    room->initialized = 1u;
    return 0;
}

static void transform_reset(uf_post_room_t *room) {
    if (!room || !room->state) return;
    transform_state_t *s = (transform_state_t *)room->state;
    uf_post_transform_config_t cfg = s->cfg;
    memset(s, 0, sizeof(*s));
    s->cfg = cfg;
    s->transformed_min = DBL_MAX;
    s->transformed_max = -DBL_MAX;
}

static int transform_consume(uf_post_room_t *room,
                             const uf_post_input_t *input,
                             const uf_post_batch_t *batch) {
    (void)input;
    if (!room || !room->state || !batch) return -1;
    if (batch->count && !batch->values) return -1;
    transform_state_t *s = (transform_state_t *)room->state;
    for (size_t i = 0; i < batch->count; ++i) {
        const double in = batch->values[i];
        if (!isfinite(in)) continue;
        ++s->input_finite_count;

        double x = in;
        if (s->cfg.flags & UF_POST_TRANSFORM_SCALE) {
            x = x * s->cfg.scale + s->cfg.offset;
        }
        if ((s->cfg.flags & UF_POST_TRANSFORM_DERIVED) != 0u) {
            x = s->cfg.derived_fn(x, s->cfg.derived_ctx);
        }
        if (!isfinite(x)) {
            ++s->transformed_invalid_count;
            continue;
        }

        ++s->transformed_count;
        s->transformed_sum += x;
        if (x < s->transformed_min) s->transformed_min = x;
        if (x > s->transformed_max) s->transformed_max = x;

        if (s->cfg.flags & UF_POST_TRANSFORM_INDICATOR) {
            ++s->indicator_count;
            if (transform_apply_indicator(s->cfg.indicator_op, x,
                                          s->cfg.indicator_threshold) > 0.5) {
                ++s->indicator_hits;
            }
        }
    }
    return 0;
}

static int transform_merge(uf_post_room_t *room, const uf_post_room_t *other) {
    if (!room || !other || !room->state || !other->state) return -1;
    transform_state_t *a = (transform_state_t *)room->state;
    const transform_state_t *b = (const transform_state_t *)other->state;
    if (!transform_config_same(&a->cfg, &b->cfg)) return -1;
    a->input_finite_count += b->input_finite_count;
    a->transformed_count += b->transformed_count;
    a->transformed_invalid_count += b->transformed_invalid_count;
    a->transformed_sum += b->transformed_sum;
    a->indicator_count += b->indicator_count;
    a->indicator_hits += b->indicator_hits;
    if (b->transformed_count) {
        if (a->transformed_count == b->transformed_count) {
            a->transformed_min = b->transformed_min;
            a->transformed_max = b->transformed_max;
        } else {
            if (b->transformed_min < a->transformed_min) a->transformed_min = b->transformed_min;
            if (b->transformed_max > a->transformed_max) a->transformed_max = b->transformed_max;
        }
    }
    return 0;
}

static int transform_finalize(uf_post_room_t *room, void *out, size_t out_bytes) {
    if (!room || !room->state || !out || out_bytes < sizeof(uf_post_transform_result_t)) return -1;
    const transform_state_t *s = (const transform_state_t *)room->state;
    uf_post_transform_result_t *r = (uf_post_transform_result_t *)out;
    memset(r, 0, sizeof(*r));
    r->input_finite_count = s->input_finite_count;
    r->transformed_count = s->transformed_count;
    r->transformed_invalid_count = s->transformed_invalid_count;
    r->transformed_sum = s->transformed_sum;
    r->transformed_mean = s->transformed_count ?
        s->transformed_sum / (double)s->transformed_count : NAN;
    if (s->transformed_count) {
        r->transformed_min = s->transformed_min;
        r->transformed_max = s->transformed_max;
    }
    r->indicator_count = s->indicator_count;
    r->indicator_hits = s->indicator_hits;
    r->indicator_probability = s->indicator_count ?
        (double)s->indicator_hits / (double)s->indicator_count : NAN;
    return 0;
}

static void transform_destroy(uf_post_room_t *room) {
    if (!room) return;
    free(room->state);
    room->state = NULL;
    room->initialized = 0u;
}

static const uf_post_room_vtable_t TRANSFORM_VTABLE = {
    .name = "transform",
    .init = transform_init,
    .reset = transform_reset,
    .consume = transform_consume,
    .merge = transform_merge,
    .finalize = transform_finalize,
    .destroy = transform_destroy
};


/* -------------------------------------------------------------------------
 * Pairing Room
 * -------------------------------------------------------------------------
 *
 * The room is deliberately metadata-driven: the caller marks a batch as
 * UF_POST_BATCH_ANTITHETIC_PAIR and sets group_size=2.  No inference and no
 * value negation happen here.  A valid pair (x,y) contributes one estimator
 * observation m=(x+y)/2.  We additionally keep the first/second leg moments
 * and cross-products so the covariance is visible in the final result.
 */
typedef struct {
    uint64_t input_values;
    uint64_t complete_pairs;
    uint64_t valid_pairs;
    uint64_t invalid_pairs;
    uint64_t standalone_values;
    uint64_t standalone_valid;
    double pair_mean;
    double pair_m2;
    double first_mean;
    double first_m2;
    double second_mean;
    double second_m2;
    double co_m2;
} pairing_state_t;

static void pairing_reset_state(pairing_state_t *s) {
    memset(s, 0, sizeof(*s));
}

static inline void pairing_add_bivariate(pairing_state_t *s, double x, double y) {
    const uint64_t n1 = s->valid_pairs + 1u;
    const double old_x = s->first_mean;
    const double old_y = s->second_mean;
    const double dx = x - old_x;
    const double dy = y - old_y;
    const double new_x = old_x + dx / (double)n1;
    const double new_y = old_y + dy / (double)n1;

    s->first_m2 += dx * (x - new_x);
    s->second_m2 += dy * (y - new_y);
    /* Pairwise co-moment update: C_n = C_{n-1} + dx*(y-new_y). */
    s->co_m2 += dx * (y - new_y);
    s->first_mean = new_x;
    s->second_mean = new_y;

    const double m = 0.5 * (x + y);
    const double dm = m - s->pair_mean;
    s->pair_mean += dm / (double)n1;
    s->pair_m2 += dm * (m - s->pair_mean);
    s->valid_pairs = n1;
}

static int pairing_init(uf_post_room_t *room, const void *config) {
    (void)config;
    if (!room) return -1;
    pairing_state_t *s = (pairing_state_t *)calloc(1, sizeof(*s));
    if (!s) return -1;
    room->state = s;
    room->initialized = 1u;
    return 0;
}

static void pairing_reset(uf_post_room_t *room) {
    if (room && room->state) pairing_reset_state((pairing_state_t *)room->state);
}

static int pairing_consume(uf_post_room_t *room,
                           const uf_post_input_t *input,
                           const uf_post_batch_t *batch) {
    (void)input;
    if (!room || !room->state || !batch) return -1;
    if (batch->count && !batch->values) return -1;
    pairing_state_t *s = (pairing_state_t *)room->state;
    s->input_values += (uint64_t)batch->count;

    if ((batch->flags & UF_POST_BATCH_ANTITHETIC_PAIR) != 0u) {
        if (batch->group_size != 2u || (batch->count % 2u) != 0u) return -1;
        s->complete_pairs += (uint64_t)(batch->count / 2u);
        for (size_t i = 0; i < batch->count; i += 2u) {
            const double x = batch->values[i];
            const double y = batch->values[i + 1u];
            if (!isfinite(x) || !isfinite(y)) {
                ++s->invalid_pairs;
                continue;
            }
            pairing_add_bivariate(s, x, y);
        }
        return 0;
    }

    /* Plain batches do not invent pairs. They are recorded as standalone
     * values so the room remains informative without changing semantics. */
    for (size_t i = 0; i < batch->count; ++i) {
        if (isfinite(batch->values[i])) ++s->standalone_valid;
        ++s->standalone_values;
    }
    return 0;
}

static int pairing_merge(uf_post_room_t *room, const uf_post_room_t *other) {
    if (!room || !other || !room->state || !other->state) return -1;
    pairing_state_t *a = (pairing_state_t *)room->state;
    const pairing_state_t *b = (const pairing_state_t *)other->state;
    a->input_values += b->input_values;
    a->complete_pairs += b->complete_pairs;
    a->invalid_pairs += b->invalid_pairs;
    a->standalone_values += b->standalone_values;
    a->standalone_valid += b->standalone_valid;
    if (b->valid_pairs == 0u) return 0;
    if (a->valid_pairs == 0u) {
        a->valid_pairs = b->valid_pairs;
        a->pair_mean = b->pair_mean;
        a->pair_m2 = b->pair_m2;
        a->first_mean = b->first_mean;
        a->first_m2 = b->first_m2;
        a->second_mean = b->second_mean;
        a->second_m2 = b->second_m2;
        a->co_m2 = b->co_m2;
        return 0;
    }

    const double na = (double)a->valid_pairs;
    const double nb = (double)b->valid_pairs;
    const double nt = na + nb;

    const double dp = b->pair_mean - a->pair_mean;
    a->pair_m2 += b->pair_m2 + dp * dp * (na * nb / nt);
    a->pair_mean += dp * (nb / nt);

    const double dx = b->first_mean - a->first_mean;
    const double dy = b->second_mean - a->second_mean;
    a->first_m2 += b->first_m2 + dx * dx * (na * nb / nt);
    a->second_m2 += b->second_m2 + dy * dy * (na * nb / nt);
    /* Cross merge correction: delta_x * delta_y * na*nb/nt. */
    a->co_m2 += b->co_m2 + dx * dy * (na * nb / nt);
    a->first_mean += dx * (nb / nt);
    a->second_mean += dy * (nb / nt);
    a->valid_pairs += b->valid_pairs;
    return 0;
}

static int pairing_finalize(uf_post_room_t *room, void *out, size_t out_bytes) {
    if (!room || !room->state || !out || out_bytes < sizeof(uf_post_pairing_result_t)) return -1;
    const pairing_state_t *s = (const pairing_state_t *)room->state;
    uf_post_pairing_result_t *r = (uf_post_pairing_result_t *)out;
    memset(r, 0, sizeof(*r));
    r->input_values = s->input_values;
    r->complete_pairs = s->complete_pairs;
    r->valid_pairs = s->valid_pairs;
    r->invalid_pairs = s->invalid_pairs;
    r->standalone_values = s->standalone_values;
    if (s->valid_pairs == 0u) {
        r->mean = r->variance = r->standard_deviation = r->sem = NAN;
        r->first_mean = r->second_mean = NAN;
        r->first_variance = r->second_variance = NAN;
        r->pair_covariance = NAN;
        r->naive_individual_variance = NAN;
        r->pair_mean_variance_formula = NAN;
        return 0;
    }

    r->mean = s->pair_mean;
    r->variance = (s->valid_pairs >= 2u) ? s->pair_m2 / (double)(s->valid_pairs - 1u) : NAN;
    r->standard_deviation = isfinite(r->variance) && r->variance >= 0.0 ? sqrt(r->variance) : NAN;
    r->sem = (isfinite(r->variance) && r->variance >= 0.0)
        ? sqrt(r->variance / (double)s->valid_pairs) : NAN;
    r->first_mean = s->first_mean;
    r->second_mean = s->second_mean;
    r->first_variance = (s->valid_pairs >= 2u) ? s->first_m2 / (double)(s->valid_pairs - 1u) : NAN;
    r->second_variance = (s->valid_pairs >= 2u) ? s->second_m2 / (double)(s->valid_pairs - 1u) : NAN;
    r->pair_covariance = (s->valid_pairs >= 2u) ? s->co_m2 / (double)(s->valid_pairs - 1u) : NAN;
    r->naive_individual_variance = (isfinite(r->first_variance) && isfinite(r->second_variance))
        ? 0.5 * (r->first_variance + r->second_variance) : NAN;
    r->pair_mean_variance_formula = (isfinite(r->first_variance) &&
                                     isfinite(r->second_variance) &&
                                     isfinite(r->pair_covariance))
        ? 0.25 * (r->first_variance + r->second_variance + 2.0 * r->pair_covariance) : NAN;
    return 0;
}

static void pairing_destroy(uf_post_room_t *room) {
    if (!room) return;
    free(room->state);
    room->state = NULL;
    room->initialized = 0u;
}

static const uf_post_room_vtable_t PAIRING_VTABLE = {
    .name = "pairing",
    .init = pairing_init,
    .reset = pairing_reset,
    .consume = pairing_consume,
    .merge = pairing_merge,
    .finalize = pairing_finalize,
    .destroy = pairing_destroy
};

/* -------------------------------------------------------------------------
 * Advanced configuration
 * ------------------------------------------------------------------------- */
void uf_post_advanced_config_default(uf_post_advanced_config_t *cfg) {
    if (!cfg) return;
    cfg->max_runs = 16u;
    cfg->max_levels = 8u;
    cfg->reserved[0] = cfg->reserved[1] = 0u;
}

int uf_post_advanced_config_validate(const uf_post_advanced_config_t *cfg) {
    if (!cfg) return -1;
    if (cfg->max_runs == 0u || cfg->max_runs > UF_POST_ADVANCED_MAX_RUNS) return -1;
    if (cfg->max_levels == 0u || cfg->max_levels > UF_POST_ADVANCED_MAX_LEVELS) return -1;
    return 0;
}

/* -------------------------------------------------------------------------
 * Metadata Room
 * ------------------------------------------------------------------------- */
typedef struct {
    uint64_t seed;
    uint64_t start_wall_ns;
    uint64_t start_mono_ns;
    uint64_t end_mono_ns;
    uint64_t input_batches;
    uint64_t input_values;
    uint64_t first_batch_id;
    uint64_t last_batch_id;
    uint8_t have_batch_id;
    uint8_t seed_consistent;
    uint8_t reserved[6];
    char rng_name[64];
    char mc_name[64];
    char run_label[64];
} metadata_state_t;

static int metadata_init(uf_post_room_t *room, const void *config) {
    if (!room) return -1;
    uf_post_metadata_config_t cfg;
    uf_post_metadata_config_default(&cfg);
    if (config) cfg = *(const uf_post_metadata_config_t *)config;
    if (uf_post_metadata_config_validate(&cfg) != 0) return -1;
    metadata_state_t *s = (metadata_state_t *)calloc(1, sizeof(*s));
    if (!s) return -1;
    s->seed = cfg.seed;
    s->start_wall_ns = cfg.start_wall_ns ? cfg.start_wall_ns : metadata_wall_now_ns();
    s->start_mono_ns = cfg.start_mono_ns ? cfg.start_mono_ns : metadata_mono_now_ns();
    s->seed_consistent = 1u;
    memcpy(s->rng_name, cfg.rng_name, sizeof(s->rng_name));
    memcpy(s->mc_name, cfg.mc_name, sizeof(s->mc_name));
    memcpy(s->run_label, cfg.run_label, sizeof(s->run_label));
    room->state = s;
    room->initialized = 1u;
    return 0;
}

static void metadata_reset(uf_post_room_t *room) {
    if (!room || !room->state) return;
    metadata_state_t *s = (metadata_state_t *)room->state;
    uint64_t seed = s->seed;
    uint64_t start_wall = metadata_wall_now_ns();
    uint64_t start_mono = metadata_mono_now_ns();
    char rng[64], mc[64], label[64];
    memcpy(rng, s->rng_name, sizeof(rng));
    memcpy(mc, s->mc_name, sizeof(mc));
    memcpy(label, s->run_label, sizeof(label));
    memset(s, 0, sizeof(*s));
    s->seed = seed;
    s->start_wall_ns = start_wall;
    s->start_mono_ns = start_mono;
    s->seed_consistent = 1u;
    memcpy(s->rng_name, rng, sizeof(rng));
    memcpy(s->mc_name, mc, sizeof(mc));
    memcpy(s->run_label, label, sizeof(label));
}

static int metadata_consume(uf_post_room_t *room, const uf_post_input_t *input, const uf_post_batch_t *batch) {
    (void)input;
    if (!room || !room->state || !batch) return -1;
    metadata_state_t *s = (metadata_state_t *)room->state;
    ++s->input_batches;
    s->input_values += (uint64_t)batch->count;
    if (!s->have_batch_id) {
        s->first_batch_id = batch->batch_id;
        s->have_batch_id = 1u;
    }
    s->last_batch_id = batch->batch_id;
    return 0;
}

static int metadata_merge(uf_post_room_t *room, const uf_post_room_t *other) {
    if (!room || !other || !room->state || !other->state) return -1;
    metadata_state_t *a = (metadata_state_t *)room->state;
    const metadata_state_t *b = (const metadata_state_t *)other->state;
    if (strcmp(a->rng_name, b->rng_name) != 0 || strcmp(a->mc_name, b->mc_name) != 0 ||
        strcmp(a->run_label, b->run_label) != 0) return -1;
    if (a->input_batches == 0u) {
        a->first_batch_id = b->first_batch_id;
        a->last_batch_id = b->last_batch_id;
        a->have_batch_id = b->have_batch_id;
    } else if (b->have_batch_id) {
        if (!a->have_batch_id) a->first_batch_id = b->first_batch_id;
        if (b->first_batch_id < a->first_batch_id) a->first_batch_id = b->first_batch_id;
        if (b->last_batch_id > a->last_batch_id) a->last_batch_id = b->last_batch_id;
        a->have_batch_id = 1u;
    }
    a->input_batches += b->input_batches;
    a->input_values += b->input_values;
    if (a->seed != b->seed) { a->seed_consistent = 0u; a->seed = 0u; }
    if (b->start_mono_ns && (!a->start_mono_ns || b->start_mono_ns < a->start_mono_ns)) a->start_mono_ns = b->start_mono_ns;
    if (b->start_wall_ns && (!a->start_wall_ns || b->start_wall_ns < a->start_wall_ns)) a->start_wall_ns = b->start_wall_ns;
    return 0;
}

static int metadata_finalize(uf_post_room_t *room, void *out, size_t out_bytes) {
    if (!room || !room->state || !out || out_bytes < sizeof(uf_post_metadata_result_t)) return -1;
    metadata_state_t *s = (metadata_state_t *)room->state;
    const uint64_t end_mono = metadata_mono_now_ns();
    uint64_t elapsed = 0u;
    if (end_mono >= s->start_mono_ns) elapsed = end_mono - s->start_mono_ns;
    s->end_mono_ns = end_mono;
    uf_post_metadata_result_t *r = (uf_post_metadata_result_t *)out;
    memset(r, 0, sizeof(*r));
    r->seed = s->seed;
    r->seed_consistent = s->seed_consistent;
    r->start_wall_ns = s->start_wall_ns;
    r->end_wall_ns = metadata_wall_now_ns();
    r->elapsed_ns = elapsed;
    r->input_batches = s->input_batches;
    r->input_values = s->input_values;
    r->first_batch_id = s->first_batch_id;
    r->last_batch_id = s->last_batch_id;
    r->have_batch_id = s->have_batch_id;
    if (elapsed != 0u) {
        const double sec = (double)elapsed / 1e9;
        r->values_per_sec = (double)s->input_values / sec;
        r->batches_per_sec = (double)s->input_batches / sec;
    }
    memcpy(r->rng_name, s->rng_name, sizeof(r->rng_name));
    memcpy(r->mc_name, s->mc_name, sizeof(r->mc_name));
    memcpy(r->run_label, s->run_label, sizeof(r->run_label));
    return 0;
}

static void metadata_destroy(uf_post_room_t *room) {
    if (!room) return;
    free(room->state);
    room->state = NULL;
    room->initialized = 0u;
}

static const uf_post_room_vtable_t METADATA_VTABLE = {
    .name = "metadata",
    .init = metadata_init,
    .reset = metadata_reset,
    .consume = metadata_consume,
    .merge = metadata_merge,
    .finalize = metadata_finalize,
    .destroy = metadata_destroy
};

/* -------------------------------------------------------------------------
 * Advanced Room
 * ------------------------------------------------------------------------- */
typedef struct {
    uf_post_advanced_config_t cfg;
    uf_post_advanced_run_t runs[UF_POST_ADVANCED_MAX_RUNS];
    uint32_t run_count;
    uf_post_advanced_mlmc_level_t mlmc[UF_POST_ADVANCED_MAX_LEVELS];
    uint32_t mlmc_count;
} advanced_state_t;

static advanced_state_t *advanced_state_from_room(uf_post_room_t *room) {
    return room && room->state ? (advanced_state_t *)room->state : NULL;
}
static const advanced_state_t *advanced_const_state_from_engine(const uf_post_engine_t *engine) {
    if (!engine || !engine->initialized) return NULL;
    const uf_post_room_t *room = &engine->rooms[UF_POST_ROOM_ADVANCED];
    if (!room->enabled || !room->initialized || !room->state) return NULL;
    return (const advanced_state_t *)room->state;
}
static int advanced_find_run(const advanced_state_t *s, uint64_t run_id) {
    if (!s || run_id == 0u) return -1;
    for (uint32_t i = 0; i < s->run_count; ++i)
        if (s->runs[i].run_id == run_id) return (int)i;
    return -1;
}

static int advanced_init(uf_post_room_t *room, const void *config) {
    if (!room) return -1;
    uf_post_advanced_config_t cfg;
    uf_post_advanced_config_default(&cfg);
    if (config) cfg = *(const uf_post_advanced_config_t *)config;
    if (uf_post_advanced_config_validate(&cfg) != 0) return -1;
    advanced_state_t *s = (advanced_state_t *)calloc(1, sizeof(*s));
    if (!s) return -1;
    s->cfg = cfg;
    room->state = s;
    room->initialized = 1u;
    return 0;
}

static void advanced_reset(uf_post_room_t *room) {
    if (!room || !room->state) return;
    advanced_state_t *s = (advanced_state_t *)room->state;
    uf_post_advanced_config_t cfg = s->cfg;
    memset(s, 0, sizeof(*s));
    s->cfg = cfg;
}

static int advanced_consume(uf_post_room_t *room, const uf_post_input_t *input, const uf_post_batch_t *batch) {
    /* Deliberately a no-op: Advanced is explicit multi-run API, not a per-
       sample hot-path transform. */
    (void)room; (void)input; (void)batch;
    return 0;
}

static int advanced_merge(uf_post_room_t *room, const uf_post_room_t *other) {
    if (!room || !other || !room->state || !other->state) return -1;
    advanced_state_t *a = (advanced_state_t *)room->state;
    const advanced_state_t *b = (const advanced_state_t *)other->state;
    if (a->cfg.max_runs != b->cfg.max_runs || a->cfg.max_levels != b->cfg.max_levels) return -1;
    if (a->run_count + b->run_count > a->cfg.max_runs) return -1;
    if (a->mlmc_count + b->mlmc_count > a->cfg.max_levels) return -1;
    for (uint32_t i = 0; i < b->run_count; ++i) {
        if (advanced_find_run(a, b->runs[i].run_id) >= 0) return -1;
        a->runs[a->run_count++] = b->runs[i];
    }
    for (uint32_t i = 0; i < b->mlmc_count; ++i) {
        const uf_post_advanced_mlmc_level_t *x = &b->mlmc[i];
        for (uint32_t j = 0; j < a->mlmc_count; ++j) {
            if (a->mlmc[j].level == x->level) return -1;
        }
        a->mlmc[a->mlmc_count++] = *x;
    }
    return 0;
}

static int advanced_finalize(uf_post_room_t *room, void *out, size_t out_bytes) {
    if (!room || !room->state || !out || out_bytes < sizeof(uf_post_advanced_result_t)) return -1;
    const advanced_state_t *s = (const advanced_state_t *)room->state;
    uf_post_advanced_result_t *r = (uf_post_advanced_result_t *)out;
    memset(r, 0, sizeof(*r));
    r->run_count = s->run_count;
    r->level_count = s->run_count;
    r->mlmc_level_count = s->mlmc_count;
    uint64_t scenarios[UF_POST_ADVANCED_MAX_RUNS];
    uint64_t nsc = 0u;
    for (uint32_t i = 0; i < s->run_count; ++i) {
        uint64_t sid = s->runs[i].scenario_id;
        if (sid == 0u) continue;
        int seen = 0;
        for (uint64_t j = 0; j < nsc; ++j) if (scenarios[j] == sid) { seen = 1; break; }
        if (!seen) scenarios[nsc++] = sid;
    }
    r->scenario_count = nsc;
    return 0;
}

static void advanced_destroy(uf_post_room_t *room) {
    if (!room) return;
    free(room->state);
    room->state = NULL;
    room->initialized = 0u;
}

static const uf_post_room_vtable_t ADVANCED_VTABLE = {
    .name = "advanced",
    .init = advanced_init,
    .reset = advanced_reset,
    .consume = advanced_consume,
    .merge = advanced_merge,
    .finalize = advanced_finalize,
    .destroy = advanced_destroy
};

/* Explicit Advanced operations. */
int uf_post_advanced_add_run(uf_post_engine_t *engine, const uf_post_advanced_run_t *run) {
    if (!engine || !run) return -1;
    advanced_state_t *s = NULL;
    if (!engine->initialized || !engine->rooms[UF_POST_ROOM_ADVANCED].enabled ||
        !engine->rooms[UF_POST_ROOM_ADVANCED].initialized) return -1;
    s = advanced_state_from_room(&engine->rooms[UF_POST_ROOM_ADVANCED]);
    if (!s || run->run_id == 0u || s->run_count >= s->cfg.max_runs) return -1;
    if (advanced_find_run(s, run->run_id) >= 0) return -1;
    if (!isfinite(run->parameter) || !isfinite(run->dt) || run->dt <= 0.0 ||
        !isfinite(run->mean) || !isfinite(run->variance) || run->variance < 0.0 ||
        run->samples == 0u || !isfinite(run->sem) || run->sem < 0.0) return -1;
    s->runs[s->run_count++] = *run;
    return 0;
}

int uf_post_advanced_get_run(const uf_post_engine_t *engine, uint64_t run_id, uf_post_advanced_run_t *out) {
    if (!engine || !out) return -1;
    const advanced_state_t *s = advanced_const_state_from_engine(engine);
    if (!s) return -1;
    int idx = advanced_find_run(s, run_id);
    if (idx < 0) return -1;
    *out = s->runs[idx];
    return 0;
}

int uf_post_advanced_compare_runs(const uf_post_engine_t *engine, uint64_t run_a, uint64_t run_b,
                                  uf_post_advanced_compare_result_t *out) {
    if (!out) return -1;
    const advanced_state_t *s = advanced_const_state_from_engine(engine);
    if (!s) return -1;
    int ia = advanced_find_run(s, run_a), ib = advanced_find_run(s, run_b);
    if (ia < 0 || ib < 0) return -1;
    const uf_post_advanced_run_t *a = &s->runs[ia], *b = &s->runs[ib];
    memset(out, 0, sizeof(*out));
    out->run_a = run_a; out->run_b = run_b;
    out->value_a = a->mean; out->value_b = b->mean;
    out->difference = a->mean - b->mean;
    out->difference_sem = sqrt(a->sem * a->sem + b->sem * b->sem);
    out->valid = isfinite(out->difference_sem) ? 1u : 0u;
    return out->valid ? 0 : -1;
}

int uf_post_advanced_richardson(const uf_post_engine_t *engine, uint64_t coarse_run_id,
                                uint64_t fine_run_id, double order,
                                uf_post_advanced_richardson_result_t *out) {
    if (!out || !isfinite(order) || order <= 0.0) return -1;
    const advanced_state_t *s = advanced_const_state_from_engine(engine);
    if (!s) return -1;
    int ic = advanced_find_run(s, coarse_run_id), iff = advanced_find_run(s, fine_run_id);
    if (ic < 0 || iff < 0 || coarse_run_id == fine_run_id) return -1;
    const uf_post_advanced_run_t *c = &s->runs[ic], *f = &s->runs[iff];
    if (!(c->dt > f->dt)) return -1;
    const double ratio = c->dt / f->dt;
    const double denom = pow(ratio, order) - 1.0;
    if (!isfinite(denom) || denom <= 0.0) return -1;
    memset(out, 0, sizeof(*out));
    out->coarse_value = c->mean;
    out->fine_value = f->mean;
    out->order = order;
    out->extrapolated_value = f->mean + (f->mean - c->mean) / denom;
    out->estimated_fine_error = fabs(out->extrapolated_value - f->mean);
    out->valid = isfinite(out->extrapolated_value) && isfinite(out->estimated_fine_error);
    return out->valid ? 0 : -1;
}

int uf_post_advanced_sensitivity(const uf_post_engine_t *engine, uint64_t minus_run_id,
                                uint64_t plus_run_id,
                                uf_post_advanced_sensitivity_result_t *out) {
    if (!out) return -1;
    const advanced_state_t *s = advanced_const_state_from_engine(engine);
    if (!s) return -1;
    int im = advanced_find_run(s, minus_run_id), ip = advanced_find_run(s, plus_run_id);
    if (im < 0 || ip < 0 || im == ip) return -1;
    const uf_post_advanced_run_t *m = &s->runs[im], *p = &s->runs[ip];
    const double h = p->parameter - m->parameter;
    if (!isfinite(h) || h <= 0.0) return -1;
    memset(out, 0, sizeof(*out));
    out->theta_minus = m->parameter;
    out->theta_plus = p->parameter;
    out->value_minus = m->mean;
    out->value_plus = p->mean;
    out->derivative = (p->mean - m->mean) / h;
    out->derivative_sem = sqrt(m->sem * m->sem + p->sem * p->sem) / h;
    out->valid = isfinite(out->derivative) && isfinite(out->derivative_sem);
    return out->valid ? 0 : -1;
}

int uf_post_advanced_mlmc_add_level(uf_post_engine_t *engine, const uf_post_advanced_mlmc_level_t *level) {
    if (!engine || !level) return -1;
    if (!engine->initialized || !engine->rooms[UF_POST_ROOM_ADVANCED].enabled ||
        !engine->rooms[UF_POST_ROOM_ADVANCED].initialized) return -1;
    advanced_state_t *s = advanced_state_from_room(&engine->rooms[UF_POST_ROOM_ADVANCED]);
    if (!s || s->mlmc_count >= s->cfg.max_levels || level->level >= UF_POST_ADVANCED_MAX_LEVELS) return -1;
    if (!isfinite(level->mean_delta) || !isfinite(level->variance_delta) || level->variance_delta < 0.0 ||
        level->samples == 0u || !isfinite(level->cost_per_sample) || level->cost_per_sample < 0.0) return -1;
    for (uint32_t i = 0; i < s->mlmc_count; ++i) if (s->mlmc[i].level == level->level) return -1;
    s->mlmc[s->mlmc_count++] = *level;
    return 0;
}

int uf_post_advanced_mlmc_finalize(const uf_post_engine_t *engine, uf_post_advanced_mlmc_result_t *out) {
    if (!out) return -1;
    const advanced_state_t *s = advanced_const_state_from_engine(engine);
    if (!s || s->mlmc_count == 0u) return -1;
    double estimate = 0.0, variance = 0.0, cost = 0.0;
    uint32_t max_level_count = 0u;
    for (uint32_t i = 0; i < s->mlmc_count; ++i) {
        const uf_post_advanced_mlmc_level_t *l = &s->mlmc[i];
        estimate += l->mean_delta;
        variance += l->variance_delta / (double)l->samples;
        cost += l->cost_per_sample * (double)l->samples;
        if (l->level + 1u > max_level_count) max_level_count = l->level + 1u;
    }
    memset(out, 0, sizeof(*out));
    out->level_count = s->mlmc_count;
    out->estimate = estimate;
    out->variance = variance;
    out->sem = sqrt(variance);
    out->cost = cost;
    out->valid = isfinite(estimate) && isfinite(variance) && variance >= 0.0 && isfinite(out->sem);
    (void)max_level_count;
    return out->valid ? 0 : -1;
}

/* -------------------------------------------------------------------------
 * Optional Post rooms
 * ------------------------------------------------------------------------- */
typedef struct {uint64_t consumed_batches,consumed_values;} frame_room_state_t;
static int frame_room_init(uf_post_room_t*r,const void*c){(void)c;if(!r)return -1;frame_room_state_t*s=(frame_room_state_t*)calloc(1,sizeof(*s));if(!s)return -1;r->state=s;r->initialized=1u;return 0;}
static void frame_room_reset(uf_post_room_t*r){if(r&&r->state)memset(r->state,0,sizeof(frame_room_state_t));}
static int frame_room_consume(uf_post_room_t*r,const uf_post_input_t*i,const uf_post_batch_t*b){(void)i;if(!r||!r->state||!b)return -1;frame_room_state_t*s=(frame_room_state_t*)r->state;++s->consumed_batches;s->consumed_values+=(uint64_t)b->count;return 0;}
static int frame_room_merge(uf_post_room_t*r,const uf_post_room_t*o){if(!r||!o||!r->state||!o->state)return -1;frame_room_state_t*a=(frame_room_state_t*)r->state;const frame_room_state_t*b=(const frame_room_state_t*)o->state;a->consumed_batches+=b->consumed_batches;a->consumed_values+=b->consumed_values;return 0;}
static int frame_room_finalize(uf_post_room_t*r,void*out,size_t out_bytes){if(!r||!r->state)return -1;if(!out)return 0;if(out_bytes<sizeof(frame_room_state_t))return -1;memcpy(out,r->state,sizeof(frame_room_state_t));return 0;}
static void frame_room_destroy(uf_post_room_t*r){if(!r)return;free(r->state);r->state=NULL;r->initialized=0u;}
static const uf_post_room_vtable_t FRAME_VTABLE={.name="frame-placeholder",.init=frame_room_init,.reset=frame_room_reset,.consume=frame_room_consume,.merge=frame_room_merge,.finalize=frame_room_finalize,.destroy=frame_room_destroy};

static void install_rooms(uf_post_engine_t*e){for(unsigned i=0;i<UF_POST_MAX_ROOMS;++i){uf_post_room_t*r=&e->rooms[i];r->id=(uf_post_room_id_t)i;r->enabled=0;r->initialized=0;r->reserved=0;r->vtable=(i==UF_POST_ROOM_STATISTICS)?STATISTICS_VTABLE:(i==UF_POST_ROOM_DISTRIBUTION)?DISTRIBUTION_VTABLE:(i==UF_POST_ROOM_VALIDITY)?VALIDITY_VTABLE:(i==UF_POST_ROOM_CONVERGENCE)?CONVERGENCE_VTABLE:(i==UF_POST_ROOM_TRANSFORM)?TRANSFORM_VTABLE:(i==UF_POST_ROOM_ADVANCED)?ADVANCED_VTABLE:(i==UF_POST_ROOM_METADATA)?METADATA_VTABLE:(i==UF_POST_ROOM_PAIRING)?PAIRING_VTABLE:FRAME_VTABLE;r->state=NULL;}}

int uf_post_engine_init(uf_post_engine_t*engine,const uf_post_config_t*cfg,const uf_post_distribution_config_t*dcfg,const uf_post_convergence_config_t*ccfg,const uf_post_transform_config_t*tcfg,const uf_post_metadata_config_t*mcfg,const uf_post_advanced_config_t*acfg){
    if(!engine||!cfg)return -1;
    uf_post_distribution_config_t local;
    uf_post_distribution_config_default(&local);
    if(dcfg)local=*dcfg;
    if(uf_post_distribution_config_validate(&local)!=0)return -1;
    uf_post_convergence_config_t conv;
    uf_post_convergence_config_default(&conv);
    if(ccfg)conv=*ccfg;
    if(uf_post_convergence_config_validate(&conv)!=0)return -1;
    uf_post_transform_config_t trans;
    uf_post_transform_config_default(&trans);
    if(tcfg)trans=*tcfg;
    if(uf_post_transform_config_validate(&trans)!=0)return -1;
    uf_post_metadata_config_t meta;
    uf_post_metadata_config_default(&meta);
    if(mcfg)meta=*mcfg;
    if(uf_post_metadata_config_validate(&meta)!=0)return -1;
    uf_post_advanced_config_t adv;
    uf_post_advanced_config_default(&adv);
    if(acfg)adv=*acfg;
    if(uf_post_advanced_config_validate(&adv)!=0)return -1;
    memset(engine,0,sizeof(*engine));engine->config=*cfg;uf_post_status_reset(&engine->status);install_rooms(engine);
    for(unsigned i=0;i<UF_POST_MAX_ROOMS;++i){uf_post_room_t*r=&engine->rooms[i];r->enabled=(uint8_t)uf_post_room_enabled(cfg,r->id);if(!r->enabled)continue;const void*room_cfg=(i==UF_POST_ROOM_DISTRIBUTION)?(const void*)&local:((i==UF_POST_ROOM_CONVERGENCE)?(const void*)&conv:((i==UF_POST_ROOM_TRANSFORM)?(const void*)&trans:((i==UF_POST_ROOM_METADATA)?(const void*)&meta:((i==UF_POST_ROOM_ADVANCED)?(const void*)&adv:NULL))));if(r->vtable.init(r,room_cfg)!=0){engine->status.failed_room_mask|=UF_POST_ROOM_BIT(i);uf_post_engine_destroy(engine);return -1;}engine->status.enabled_room_mask|=UF_POST_ROOM_BIT(i);}engine->initialized=1u;return 0;
}
void uf_post_engine_reset(uf_post_engine_t*e){if(!e)return;uf_post_status_reset(&e->status);for(unsigned i=0;i<UF_POST_MAX_ROOMS;++i){uf_post_room_t*r=&e->rooms[i];if(r->enabled&&r->initialized&&r->vtable.reset)r->vtable.reset(r);}e->status.enabled_room_mask=e->config.enabled_room_mask;}
void uf_post_engine_destroy(uf_post_engine_t*e){if(!e)return;for(unsigned i=0;i<UF_POST_MAX_ROOMS;++i){uf_post_room_t*r=&e->rooms[i];if(r->initialized&&r->vtable.destroy)r->vtable.destroy(r);}e->initialized=0;e->status.enabled_room_mask=0;}
int uf_post_engine_consume_batch(uf_post_engine_t*e,const uf_post_input_t*input,const uf_post_batch_t*batch){
    if(!e||!e->initialized||!input||!batch)return UF_POST_CONSUME_ERROR;
    if(batch->count&&!batch->values)return UF_POST_CONSUME_ERROR;
    ++e->status.input_batches;
    e->status.input_values+=(uint64_t)batch->count;
    for(size_t i=0;i<batch->count;++i){double x=batch->values[i];if(isnan(x)){++e->status.invalid_values;++e->status.invalid_nan;}else if(isinf(x)){++e->status.invalid_values;++e->status.invalid_inf;}else ++e->status.accepted_values;}
    int stop_requested=0;
    for(unsigned i=0;i<UF_POST_MAX_ROOMS;++i){uf_post_room_t*r=&e->rooms[i];if(!r->enabled||!r->initialized||!r->vtable.consume)continue;int rc=r->vtable.consume(r,input,batch);if(rc<0){e->status.failed_room_mask|=UF_POST_ROOM_BIT(i);if(e->config.room_required_mask&UF_POST_ROOM_BIT(i))return UF_POST_CONSUME_ERROR;}else if(rc>0 && i==UF_POST_ROOM_CONVERGENCE){stop_requested=1;}}
    if(stop_requested){e->status.stop_requested=1u;uf_post_room_t*r=&e->rooms[UF_POST_ROOM_CONVERGENCE];if(r->initialized&&r->state){const convergence_state_t*s=(const convergence_state_t*)r->state;if(s->target_sem_reached)e->status.stop_reason_mask|=UF_POST_STOP_TARGET_SEM;if(s->max_samples_reached)e->status.stop_reason_mask|=UF_POST_STOP_MAX_SAMPLES;if(s->timeout_reached)e->status.stop_reason_mask|=UF_POST_STOP_TIMEOUT;}return UF_POST_CONSUME_STOP;}
    return UF_POST_CONSUME_OK;
}
int uf_post_engine_merge(uf_post_engine_t*e,const uf_post_engine_t*o){
    if(!e||!o||!e->initialized||!o->initialized)return -1;
    if(e->config.enabled_room_mask!=o->config.enabled_room_mask)return -1;
    for(unsigned i=0;i<UF_POST_MAX_ROOMS;++i){uf_post_room_t*d=&e->rooms[i];const uf_post_room_t*s=&o->rooms[i];if(!d->enabled||!s->enabled)continue;int rc=d->vtable.merge(d,s);if(rc<0){e->status.failed_room_mask|=UF_POST_ROOM_BIT(i);if(e->config.room_required_mask&UF_POST_ROOM_BIT(i))return -1;}else if(rc>0 && i==UF_POST_ROOM_CONVERGENCE){e->status.stop_requested=1u;}}
    e->status.input_batches+=o->status.input_batches;e->status.input_values+=o->status.input_values;e->status.accepted_values+=o->status.accepted_values;e->status.invalid_values+=o->status.invalid_values;e->status.invalid_nan+=o->status.invalid_nan;e->status.invalid_inf+=o->status.invalid_inf;e->status.failed_room_mask|=o->status.failed_room_mask;e->status.stop_requested|=o->status.stop_requested;e->status.stop_reason_mask|=o->status.stop_reason_mask;return 0;
}
int uf_post_engine_finalize(uf_post_engine_t*e,uf_post_room_id_t id,void*out,size_t out_bytes){if(!e||!e->initialized||!valid_room_id(id))return -1;uf_post_room_t*r=&e->rooms[(unsigned)id];if(!r->enabled||!r->initialized||!r->vtable.finalize)return -1;return r->vtable.finalize(r,out,out_bytes);}
const uf_post_status_t*uf_post_engine_status(const uf_post_engine_t*e){return e?&e->status:NULL;}

static uint64_t now_ns(void){struct timespec ts;if(clock_gettime(CLOCK_MONOTONIC,&ts)!=0)return 0;return (uint64_t)ts.tv_sec*UINT64_C(1000000000)+(uint64_t)ts.tv_nsec;}
static void pin_cpu0(void)
{
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0,&set);
    (void)sched_setaffinity(0,sizeof(set),&set);
#endif
}

/* ---------- Fast Result -> Post bridge + integrated driver ---------- */

typedef struct { uf_post_engine_t engine; uint64_t batch_id; uint8_t initialized; } UFRPostBridge;
static int ufr_post_init(UFRPostBridge *pb,uint64_t seed){
    memset(pb,0,sizeof(*pb));uf_post_config_t cfg;uf_post_config_init(&cfg);
    for(unsigned i=0;i<UF_POST_MAX_ROOMS;++i)uf_post_enable_room(&cfg,(uf_post_room_id_t)i);
    uf_post_distribution_config_t dc;uf_post_distribution_config_default(&dc);uf_post_convergence_config_t cc;uf_post_convergence_config_default(&cc);
    uf_post_transform_config_t tc;uf_post_transform_config_default(&tc);uf_post_metadata_config_t mc;uf_post_metadata_config_default(&mc);uf_post_advanced_config_t ac;uf_post_advanced_config_default(&ac);
    mc.seed=seed;strncpy(mc.rng_name,"UltraFastRng512-Integrated",sizeof(mc.rng_name)-1);strncpy(mc.mc_name,"MC-fast-q64",sizeof(mc.mc_name)-1);strncpy(mc.run_label,"fast-integrated",sizeof(mc.run_label)-1);
    if(uf_post_engine_init(&pb->engine,&cfg,&dc,&cc,&tc,&mc,&ac)!=0) return -1;
    pb->initialized=1;
    return 0;
}
static void ufr_post_destroy(UFRPostBridge *pb){if(pb&&pb->initialized){uf_post_engine_destroy(&pb->engine);pb->initialized=0;}}
static int ufr_post_submit(UFRPostBridge *pb,const mc_result_t *r){
    if(!pb||!pb->initialized||!r) return -1;
    const double value=r->samples?(r->sum_value/(double)r->samples):NAN;
    uf_post_batch_t b={&value,1u,1u,UF_POST_BATCH_PLAIN,++pb->batch_id};uf_post_input_t in={"mc_mean",&b,1u,NULL,0u};
    return uf_post_engine_consume_batch(&pb->engine,&in,&b)<0?-1:0;
}

typedef struct{UFRXSeedExchange seedx;UFRFastFrontMem fm;_Atomic int stop;uf_cache_box_t normal_box;UFRPostBridge post;} UFRIntegrated;
static const uint64_t UFR_DEFAULT_SEED_A[8]={UINT64_C(0x243F6A8885A308D3),UINT64_C(0x13198A2E03707344),UINT64_C(0xA4093822299F31D0),UINT64_C(0x082EFA98EC4E6C89),UINT64_C(0x452821E638D01377),UINT64_C(0xBE5466CF34E90C6C),UINT64_C(0xC0AC29B7C97C50DD),UINT64_C(0x3F84D5B5B5470917)};
static const uint64_t UFR_DEFAULT_SEED_B[8]={UINT64_C(0x9216D5D98979FB1B),UINT64_C(0xD1310BA698DFB5AC),UINT64_C(0x2FFD72DBD01ADFB7),UINT64_C(0xB8E1AFED6A267E96),UINT64_C(0xBA7C9045F12C7F99),UINT64_C(0x24A19947B3916CF7),UINT64_C(0x0801F2E2858EFC16),UINT64_C(0x636920D871574E69)};
static const uint64_t UFR_MULTI_RAW_A[4][8]={
 {UINT64_C(0x243F6A8885A308D3),UINT64_C(0x13198A2E03707344),UINT64_C(0xA4093822299F31D0),UINT64_C(0x082EFA98EC4E6C89),UINT64_C(0x452821E638D01377),UINT64_C(0xBE5466CF34E90C6C),UINT64_C(0xC0AC29B7C97C50DD),UINT64_C(0x3F84D5B5B5470917)},
 {UINT64_C(0x6A09E667F3BCC908),UINT64_C(0xBB67AE8584CAA73B),UINT64_C(0x3C6EF372FE94F82B),UINT64_C(0xA54FF53A5F1D36F1),UINT64_C(0x510E527FADE682D1),UINT64_C(0x9B05688C2B3E6C1F),UINT64_C(0x1F83D9ABFB41BD6B),UINT64_C(0x5BE0CD19137E2179)},
 {UINT64_C(0xD1310BA698DFB5A),UINT64_C(0x98DFB5AC2FFD72DB),UINT64_C(0xD01ADFB7B8E1AFED),UINT64_C(0x6A267E96BA7C9045),UINT64_C(0xF12C7F9924A19947),UINT64_C(0xB3916CF70801F2E2),UINT64_C(0x858EFC16636920D8),UINT64_C(0x74E3A4F4DDE9D9A7)},
 {UINT64_C(0x510E527FADE682D1),UINT64_C(0x9B05688C2B3E6C1F),UINT64_C(0x1F83D9ABFB41BD6B),UINT64_C(0x5BE0CD19137E2179),UINT64_C(0xC1059ED8367CD507),UINT64_C(0x367CD5073070DD17),UINT64_C(0xF70E5939FFC00B31),UINT64_C(0x68581511D3F7E0D5)}
};
static void ufrx_build_four_roots(UFRXWorker *bw)
{
    for(unsigned si=0;si<4u;++si){
        if(!ufrx_build_root_from_raw(UFR_MULTI_RAW_A[si],&bw->meta,bw->root_seed64[si])) abort();
    }
}
static int ufr_int_init(UFRIntegrated *u){memset(u,0,sizeof(*u));atomic_init(&u->stop,0);ufrx_init(&u->seedx,UFR_DEFAULT_SEED_A,UFR_DEFAULT_SEED_B);ufrx_load(&u->fm,&u->seedx.handoff_a,UFR_DOMAIN_A);if(uf_l2_box_init(&u->normal_box,(size_t)NORMAL_GROUP*UF_NORMAL_BASE_BYTES*UFR_FAST_QBLOCKS_PER_STEP,STAGE_SLOTS)!=0)return -1;if(ufr_post_init(&u->post,UINT64_C(0x0123456789ABCDEF))!=0){uf_box_destroy(&u->normal_box);return -1;}return 0;}
static int ufr_int_start(UFRIntegrated *u){return ufrx_start(&u->seedx,&u->stop);}
static void ufr_int_stop(UFRIntegrated *u){if(u)ufrx_stop(&u->seedx,&u->stop);}
static int ufr_int_run(UFRIntegrated *u,uint64_t blocks,double *ns256,mc_result_t *last){
    const uint64_t unit=(uint64_t)NORMAL_GROUP*RESULT_GROUP*STAGE_SLOTS,groups=blocks/unit;if(!groups)return -1;
    const __m512 scale=_mm512_set1_ps(UF_NORMAL_SCALE*0.7071067811865475244f),scale2=_mm512_mul_ps(scale,scale);mc_result_t agg;
    const size_t need=(size_t)NORMAL_GROUP*UF_NORMAL_BASE_BYTES*UFR_FAST_QBLOCKS_PER_STEP;
    for(unsigned warm=0;warm<2;++warm){for(unsigned rg=0;rg<RESULT_GROUP;++rg){for(unsigned ss=0;ss<STAGE_SLOTS;++ss){uf_box_write_t nw;if(uf_box_try_acquire_write(&u->normal_box,&nw))return -2;ufr_fast_front_normal_group(&u->fm,(int32_t*)nw.data);if(uf_box_commit_write(&u->normal_box,&nw,need))return -3;u->seedx.active_steps+=NORMAL_GROUP;}for(unsigned ss=0;ss<STAGE_SLOTS;++ss){uf_box_item_t ni;if(uf_box_try_pop(&u->normal_box,&ni))return -4;__m512 wa=_mm512_setzero_ps();(void)mc_process_group(&ni,&wa,scale2);uf_box_release(&u->normal_box,&ni);}}}
    const uint64_t t0=now_ns();
    for(uint64_t sg=0;sg<groups;++sg){memset(&agg,0,sizeof(agg));__m512 acc=_mm512_setzero_ps();uint64_t hits=0;
        for(unsigned rg=0;rg<RESULT_GROUP;++rg){for(unsigned ss=0;ss<STAGE_SLOTS;++ss){uf_box_write_t nw;if(uf_box_try_acquire_write(&u->normal_box,&nw))return -5;ufr_fast_front_normal_group(&u->fm,(int32_t*)nw.data);if(uf_box_commit_write(&u->normal_box,&nw,need))return -6;u->seedx.active_steps+=NORMAL_GROUP;if(u->seedx.active_steps>=UFR_FRONT_MIN_STEPS&&(u->seedx.active_steps%UFR_FRONT_READY_POLL)==0u){if(ufrx_try_switch(&u->fm,&u->seedx)){}else ++u->seedx.missed_ready_checks;}}
            for(unsigned ss=0;ss<STAGE_SLOTS;++ss){uf_box_item_t ni;if(uf_box_try_pop(&u->normal_box,&ni))return -7;hits+=mc_process_group(&ni,&acc,scale2);uf_box_release(&u->normal_box,&ni);}}
        mc_finalize_acc(acc,hits,(uint64_t)NORMAL_GROUP*RESULT_GROUP*STAGE_SLOTS*UFR_FAST_QBLOCKS_PER_STEP,&agg);if(ufr_post_submit(&u->post,&agg))return -8;if(last)*last=agg;}
    const uint64_t dt=now_ns()-t0;const double samples_per_group=(double)NORMAL_GROUP*RESULT_GROUP*STAGE_SLOTS*UFR_FAST_QBLOCKS_PER_STEP*UF_NORMAL_BASE_SAMPLES;*ns256=dt/(double)groups/(samples_per_group/256.0);return 0;
}
static void ufr_int_destroy(UFRIntegrated *u){if(!u)return;ufr_post_destroy(&u->post);uf_box_destroy(&u->normal_box);}
#ifndef UFR_NO_MAIN
static double ufr_med7(double x[7]){for(int i=0;i<7;++i)for(int j=i+1;j<7;++j)if(x[j]<x[i]){double t=x[i];x[i]=x[j];x[j]=t;}return x[3];}
int main(void){pin_cpu0();UFRIntegrated u;double r[7];mc_result_t last={0};const uint64_t blocks=BLOCKS_TOTAL-(BLOCKS_TOTAL%((uint64_t)NORMAL_GROUP*RESULT_GROUP*STAGE_SLOTS));for(int i=0;i<7;++i){if(i){ufr_int_stop(&u);ufr_int_destroy(&u);}if(ufr_int_init(&u)||ufr_int_start(&u)){fprintf(stderr,"init/start failed at run %d\n",i);return 2;}
#if defined(__linux__)
{cpu_set_t ca,cb;CPU_ZERO(&ca);CPU_SET(1,&ca);CPU_ZERO(&cb);CPU_SET(2,&cb);(void)pthread_setaffinity_np(u.seedx.back_a.thread,sizeof(ca),&ca);(void)pthread_setaffinity_np(u.seedx.back_b.thread,sizeof(cb),&cb);}
#endif
if(ufr_int_run(&u,blocks,&r[i],&last)){fprintf(stderr,"run failed at %d\n",i);ufr_int_stop(&u);ufr_int_destroy(&u);return 3;}}
ufr_int_stop(&u);double med=ufr_med7(r);uf_post_statistics_result_t st={0};uf_post_validity_result_t va={0};uf_post_distribution_result_t di={0};(void)uf_post_engine_finalize(&u.post.engine,UF_POST_ROOM_STATISTICS,&st,sizeof(st));(void)uf_post_engine_finalize(&u.post.engine,UF_POST_ROOM_VALIDITY,&va,sizeof(va));(void)uf_post_engine_finalize(&u.post.engine,UF_POST_ROOM_DISTRIBUTION,&di,sizeof(di));
printf("UltraFastRng512 V1 Front3Parents + VHV-SeedBack + NoCounter Feedback + FastMC + PostV11\n");printf("Path: Back V->H->V x3 parents -> Front16 true-3P 2ADD+XOR temporal feedback + 13 nonlinear resident states -> Normal v9 -> cheap4x -> MC\n");printf("Timing: median %.3f ns/256 scalar-normal | %.3f Gsamples/s scalar-normal | runs %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n",med,256.0/(med*1e-9)/1e9,r[0],r[1],r[2],r[3],r[4],r[5],r[6]);printf("Result: samples=%" PRIu64 " hits=%" PRIu64 " mean=%g checksum=%016" PRIx64 "\n",last.samples,last.hits,last.samples?last.sum_value/(double)last.samples:0.0,last.checksum);printf("Back: switches=%" PRIu64 " ready_obs=%" PRIu64 " missed=%" PRIu64 "\n",u.seedx.switches,u.seedx.ready_observations,u.seedx.missed_ready_checks);{const UFRXWorker *aw=(u.seedx.active==0)?&u.seedx.back_a:&u.seedx.back_b;printf("SeedAudit: active_bank=%" PRIu64 " generation=%" PRIu64 " states=%u profile=%s construction=%s\n",aw->last_record.bank_index,aw->last_record.generation,aw->last_record.front_state_count,aw->last_record.profile_id,aw->last_record.construction_version);printf("SeedAudit: root_fp=%s\n",aw->last_record.root_fingerprint_hex);printf("SeedAudit: front16_fp=%s\n",aw->last_record.front16_fingerprint_hex);printf("SeedAudit: repro_id=%s\n",aw->last_record.reproducible_id_hex);}printf("Post: count=%" PRIu64 " mean=%+.9g variance=%.9g sem=%.9g invalid=%" PRIu64 " p99=%.9g\n",st.count,st.mean,st.variance,st.sem,va.invalid_values,di.p99);ufr_int_destroy(&u);return 0;}
#endif


/* -------------------------------------------------------------------------
 * Embedded VHV SeedBack implementation (self-contained)
 * ------------------------------------------------------------------------- */
#include <openssl/evp.h>
#include <openssl/err.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================
 * Common helpers
 * ============================================================ */

static int digest_once(const EVP_MD *md,
                       const uint8_t *data, size_t len,
                       uint8_t *out, unsigned out_len)
{
    unsigned got = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return 0;

    int ok =
        EVP_DigestInit_ex(ctx, md, NULL) == 1 &&
        EVP_DigestUpdate(ctx, data, len) == 1 &&
        EVP_DigestFinal_ex(ctx, out, &got) == 1 &&
        got == out_len;

    EVP_MD_CTX_free(ctx);
    return ok;
}

static int digest_parts(const EVP_MD *md,
                        const uint8_t *p0, size_t n0,
                        const uint8_t *p1, size_t n1,
                        const uint8_t *p2, size_t n2,
                        const uint8_t *p3, size_t n3,
                        const uint8_t *p4, size_t n4,
                        uint8_t *out, unsigned out_len)
{
    unsigned got = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return 0;

    int ok = EVP_DigestInit_ex(ctx, md, NULL) == 1;

    if (ok && n0) ok = EVP_DigestUpdate(ctx, p0, n0) == 1;
    if (ok && n1) ok = EVP_DigestUpdate(ctx, p1, n1) == 1;
    if (ok && n2) ok = EVP_DigestUpdate(ctx, p2, n2) == 1;
    if (ok && n3) ok = EVP_DigestUpdate(ctx, p3, n3) == 1;
    if (ok && n4) ok = EVP_DigestUpdate(ctx, p4, n4) == 1;

    if (ok) ok = EVP_DigestFinal_ex(ctx, out, &got) == 1 && got == out_len;

    EVP_MD_CTX_free(ctx);
    return ok;
}

static void store_be32(uint8_t out[4], uint32_t x)
{
    out[0] = (uint8_t)(x >> 24);
    out[1] = (uint8_t)(x >> 16);
    out[2] = (uint8_t)(x >> 8);
    out[3] = (uint8_t)x;
}

static void store_be64(uint8_t out[8], uint64_t x)
{
    out[0] = (uint8_t)(x >> 56);
    out[1] = (uint8_t)(x >> 48);
    out[2] = (uint8_t)(x >> 40);
    out[3] = (uint8_t)(x >> 32);
    out[4] = (uint8_t)(x >> 24);
    out[5] = (uint8_t)(x >> 16);
    out[6] = (uint8_t)(x >> 8);
    out[7] = (uint8_t)x;
}

static int append_lenprefixed(uint8_t *buf, size_t cap, size_t *pos,
                              const uint8_t *data, size_t len)
{
    if (len > UINT32_MAX || *pos > cap || cap - *pos < 4 + len) return 0;
    store_be32(buf + *pos, (uint32_t)len);
    *pos += 4;
    if (len) memcpy(buf + *pos, data, len);
    *pos += len;
    return 1;
}

/* ============================================================
 * Metadata validation / canonical record ID
 * ============================================================ */

static int metadata_ok(const UFRNG_SeedMetadata *m)
{
    if (!m) return 0;
    if (!m->pipeline_version || !m->lps_profile || !m->profile_id) return 0;
    return 1;
}

static void hex64(const uint8_t in[32], char out[65])
{
    for (size_t i = 0; i < 32; ++i)
        snprintf(out + i*2, 3, "%02x", in[i]);
    out[64] = '\0';
}

int ufrng_make_back_record(const UFRNG_SeedMetadata *meta,
                           uint64_t bank_index,
                           const uint8_t root_seed64[64],
                           const uint8_t handoff64[64],
                           UFRNG_SeedBackRecord *out_record)
{
    if (!metadata_ok(meta) || !root_seed64 || !handoff64 || !out_record) return 0;

    uint8_t root_fp[32], handoff_fp[32];
    static const uint8_t root_domain[] = "UFRNG-ROOT-FP-V1";
    static const uint8_t handoff_domain[] = "UFRNG-HANDOFF-FP-V1";

    if (!digest_parts(EVP_sha256(), root_domain, sizeof(root_domain)-1,
                      root_seed64, 64, NULL, 0, NULL, 0, NULL, 0,
                      root_fp, 32)) return 0;
    if (!digest_parts(EVP_sha256(), handoff_domain, sizeof(handoff_domain)-1,
                      handoff64, 64, NULL, 0, NULL, 0, NULL, 0,
                      handoff_fp, 32)) return 0;

    /*
     * Reproducible identifier includes the exact Bank position, root
     * fingerprint, handoff fingerprint, stream IDs and pipeline metadata.
     * It is Back-only bookkeeping and contains no secret material directly.
     */
    uint8_t buf[2048];
    size_t pos = 0;
    static const uint8_t domain[] = "UFRNG-REPRO-RECORD-V2";

    if (!append_lenprefixed(buf, sizeof(buf), &pos, domain, sizeof(domain)-1))
        return 0;

    if (pos + 16 + 16 + 8 + 8 + 8 + 32 + 32 > sizeof(buf)) return 0;
    memcpy(buf + pos, meta->node_id, 16); pos += 16;
    memcpy(buf + pos, meta->run_id, 16); pos += 16;
    store_be64(buf + pos, meta->sequence); pos += 8;
    store_be64(buf + pos, meta->seed_id); pos += 8;
    store_be64(buf + pos, bank_index); pos += 8;
    memcpy(buf + pos, root_fp, 32); pos += 32;
    memcpy(buf + pos, handoff_fp, 32); pos += 32;

    if (!append_lenprefixed(buf, sizeof(buf), &pos,
                            (const uint8_t *)meta->pipeline_version,
                            strlen(meta->pipeline_version))) return 0;
    if (!append_lenprefixed(buf, sizeof(buf), &pos,
                            (const uint8_t *)meta->lps_profile,
                            strlen(meta->lps_profile))) return 0;
    if (!append_lenprefixed(buf, sizeof(buf), &pos,
                            (const uint8_t *)meta->profile_id,
                            strlen(meta->profile_id))) return 0;

    uint8_t digest[32];
    if (!digest_once(EVP_sha256(), buf, pos, digest, 32)) return 0;

    out_record->meta = *meta;
    out_record->bank_index = bank_index;
    hex64(root_fp, out_record->root_fingerprint_hex);
    hex64(handoff_fp, out_record->handoff_fingerprint_hex);
    hex64(digest, out_record->reproducible_id_hex);
    return 1;
}

/* ============================================================
 * Light cleanup
 * ============================================================ */

int ufrng_light_cleanup(const uint8_t *raw, size_t raw_len,
                        uint8_t *cleaned, size_t cleaned_cap,
                        size_t *cleaned_len)
{
    if (!raw || !cleaned || !cleaned_len) return 0;
    if (raw_len % 64 != 0) return 0;

    size_t out = 0;
    int have_prev = 0;
    uint8_t prev[64];

    for (size_t off = 0; off < raw_len; off += 64) {
        const uint8_t *sample = raw + off;

        int all_zero = 1;
        for (size_t j = 0; j < 64; ++j) {
            if (sample[j] != 0) {
                all_zero = 0;
                break;
            }
        }
        if (all_zero) continue;

        if (have_prev && memcmp(sample, prev, 64) == 0) continue;

        if (out + 64 > cleaned_cap) return 0;
        memcpy(cleaned + out, sample, 64);
        out += 64;
        memcpy(prev, sample, 64);
        have_prev = 1;
    }

    *cleaned_len = out;
    return 1;
}

/* ============================================================
 * SHA-256 conditioner
 * ============================================================ */

int ufrng_condition_entropy(const uint8_t *raw, size_t raw_len,
                            const UFRNG_SeedMetadata *meta,
                            uint8_t out32[32])
{
    if (!raw || !out32 || !metadata_ok(meta)) return 0;

    /*
     * Node ID / Run ID / Sequence are domain separation, not entropy claims.
     */
    static const uint8_t domain[] = "UFRNG-ENTROPY-CONDITIONER-V2";

    uint8_t cleaned[1 << 16];
    size_t cleaned_len = 0;

    if (raw_len > sizeof(cleaned)) {
        /*
         * Keep memory bounded for the embedded API.  The public fast path remains
         * implementation may stream the cleaner into the hash context.
         */
        return 0;
    }

    if (!ufrng_light_cleanup(raw, raw_len,
                             cleaned, sizeof(cleaned), &cleaned_len))
        return 0;

    uint8_t seq_be[8];
    store_be64(seq_be, meta->sequence);

    return digest_parts(
        EVP_sha256(),
        domain, sizeof(domain)-1,
        cleaned, cleaned_len,
        meta->node_id, 16,
        meta->run_id, 16,
        seq_be, 8,
        out32, 32
    );
}

/* ============================================================
 * LPS / PSL2(F29), degree 6
 * ============================================================ */

#define LPS_P 5u
#define LPS_Q 29u
#define LPS_LANES 4u
#define LPS_STEPS 16u
#define LPS_START_STEPS 8u

typedef struct {
    uint16_t a00, a01, a10, a11;
} lps_mat;

static const lps_mat LPS_G[6] = {
    {10,  0,  0,  3},
    { 8, 13, 16,  8},
    { 8, 11, 11,  8},
    { 8, 18, 18,  8},
    { 8, 16, 13,  8},
    { 3,  0,  0, 10},
};

static lps_mat lps_mul(lps_mat A, lps_mat B)
{
    lps_mat C;
    C.a00 = (uint16_t)((A.a00*B.a00 + A.a01*B.a10) % LPS_Q);
    C.a01 = (uint16_t)((A.a00*B.a01 + A.a01*B.a11) % LPS_Q);
    C.a10 = (uint16_t)((A.a10*B.a00 + A.a11*B.a10) % LPS_Q);
    C.a11 = (uint16_t)((A.a10*B.a01 + A.a11*B.a11) % LPS_Q);
    return C;
}

static lps_mat lps_canonical(lps_mat A)
{
    lps_mat N;
    N.a00 = (uint16_t)((LPS_Q - A.a00) % LPS_Q);
    N.a01 = (uint16_t)((LPS_Q - A.a01) % LPS_Q);
    N.a10 = (uint16_t)((LPS_Q - A.a10) % LPS_Q);
    N.a11 = (uint16_t)((LPS_Q - A.a11) % LPS_Q);

    if (A.a00 != N.a00) return
        (A.a00 < N.a00) ? A : N;
    if (A.a01 != N.a01) return
        (A.a01 < N.a01) ? A : N;
    if (A.a10 != N.a10) return
        (A.a10 < N.a10) ? A : N;
    return (A.a11 <= N.a11) ? A : N;
}

static int lps_selector6(const uint8_t *seed, size_t seed_len,
                          uint32_t needed, uint8_t *symbols)
{
    uint64_t counter = 0;
    uint32_t have = 0;
    uint8_t buf[64];
    uint8_t ctr_be[8];

    while (have < needed) {
        store_be64(ctr_be, counter++);

        static const uint8_t domain[] = "UFRNG-LPS-X4-SELECTOR-V2";
        if (!digest_parts(
                EVP_sha512(),
                domain, sizeof(domain)-1,
                seed, seed_len,
                ctr_be, 8,
                NULL, 0, NULL, 0,
                buf, 64))
            return 0;

        for (size_t i = 0; i < 64 && have < needed; ++i) {
            for (unsigned shift = 0; shift <= 6 && have < needed; shift += 3) {
                uint8_t x = (uint8_t)((buf[i] >> shift) & 7u);
                if (x < 6u) symbols[have++] = x;
            }
        }
    }

    return 1;
}

static void lps_endpoint(const uint8_t source32[32], uint32_t lane,
                          lps_mat *out)
{
    uint8_t lane_be[4];
    store_be32(lane_be, lane);

    uint8_t start_seed[64];
    uint8_t walk_seed[64];

    static const uint8_t start_domain[] = "UFRNG-LPS-X4-START-V2";
    static const uint8_t walk_domain[]  = "UFRNG-LPS-X4-WALK-V2";

    if (!digest_parts(EVP_sha512(),
                      start_domain, sizeof(start_domain)-1,
                      source32, 32,
                      lane_be, 4,
                      NULL, 0, NULL, 0,
                      start_seed, 64))
        abort();

    if (!digest_parts(EVP_sha512(),
                      walk_domain, sizeof(walk_domain)-1,
                      source32, 32,
                      lane_be, 4,
                      NULL, 0, NULL, 0,
                      walk_seed, 64))
        abort();

    uint8_t symbols[32];
    lps_mat state = {1,0,0,1};

    (void)lps_selector6(start_seed, 64, LPS_START_STEPS, symbols);
    for (unsigned i = 0; i < LPS_START_STEPS; ++i)
        state = lps_canonical(lps_mul(state, LPS_G[symbols[i]]));

    (void)lps_selector6(walk_seed, 64, LPS_STEPS, symbols);
    for (unsigned i = 0; i < LPS_STEPS; ++i)
        state = lps_canonical(lps_mul(state, LPS_G[symbols[i]]));

    *out = state;
}

int ufrng_lps_x4_extract(const uint8_t conditioned32[32],
                         uint8_t out64[64])
{
    if (!conditioned32 || !out64) return 0;

    uint8_t payload[512];
    size_t pos = 0;
    static const uint8_t domain[] = "UFRNG-LPS-X4-EXTRACT-V2";

    memcpy(payload + pos, domain, sizeof(domain)-1);
    pos += sizeof(domain)-1;

    uint8_t params[16];
    store_be32(params + 0, LPS_P);
    store_be32(params + 4, LPS_Q);
    store_be32(params + 8, LPS_STEPS);
    store_be32(params + 12, LPS_LANES);

    memcpy(payload + pos, params, sizeof(params));
    pos += sizeof(params);

    for (uint32_t lane = 0; lane < LPS_LANES; ++lane) {
        lps_mat e;
        lps_endpoint(conditioned32, lane, &e);

        uint8_t lane_be[4];
        store_be32(lane_be, lane);
        memcpy(payload + pos, lane_be, 4);
        pos += 4;

        uint8_t m[8];
        m[0] = (uint8_t)(e.a00 >> 8); m[1] = (uint8_t)e.a00;
        m[2] = (uint8_t)(e.a01 >> 8); m[3] = (uint8_t)e.a01;
        m[4] = (uint8_t)(e.a10 >> 8); m[5] = (uint8_t)e.a10;
        m[6] = (uint8_t)(e.a11 >> 8); m[7] = (uint8_t)e.a11;
        memcpy(payload + pos, m, 8);
        pos += 8;
    }

    return digest_once(EVP_sha512(), payload, pos, out64, 64);
}

/* ============================================================
 * Final root seed
 * ============================================================ */

int ufrng_build_root_seed(const uint8_t *raw, size_t raw_len,
                          const UFRNG_SeedMetadata *meta,
                          uint8_t root_seed64[64])
{
    uint8_t conditioned[32];
    if (!ufrng_condition_entropy(raw, raw_len, meta, conditioned))
        return 0;
    return ufrng_lps_x4_extract(conditioned, root_seed64);
}

/* ============================================================
 * Seed Bank
 * ============================================================ */

int ufrng_seed_bank_entry(const uint8_t root_seed64[64],
                          uint64_t bank_index,
                          uint8_t out64[64])
{
    if (!root_seed64 || !out64) return 0;

    uint8_t idx_be[8];
    store_be64(idx_be, bank_index);
    static const uint8_t domain[] = "UFRNG-SEED-BANK-V1";

    return digest_parts(
        EVP_sha512(),
        domain, sizeof(domain)-1,
        root_seed64, 64,
        idx_be, 8,
        NULL, 0, NULL, 0,
        out64, 64
    );
}

/* ============================================================
 * ChaCha20 PROFILE
 * ============================================================ */

static uint32_t rotl32(uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32u - n));
}

static uint32_t load_le32(const uint8_t *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void store_le32(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)x;
    p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16);
    p[3] = (uint8_t)(x >> 24);
}

static void chacha_qr(uint32_t x[16],
                      unsigned a, unsigned b,
                      unsigned c, unsigned d)
{
    x[a] += x[b]; x[d] ^= x[a]; x[d] = rotl32(x[d], 16);
    x[c] += x[d]; x[b] ^= x[c]; x[b] = rotl32(x[b], 12);
    x[a] += x[b]; x[d] ^= x[a]; x[d] = rotl32(x[d], 8);
    x[c] += x[d]; x[b] ^= x[c]; x[b] = rotl32(x[b], 7);
}

static void chacha20_block(const uint8_t key[32],
                           uint32_t counter,
                           const uint8_t nonce[12],
                           uint8_t out[64])
{
    static const uint8_t sigma[16] = "expand 32-byte k";

    uint32_t s[16], x[16];

    s[0] = load_le32(sigma+0);
    s[1] = load_le32(sigma+4);
    s[2] = load_le32(sigma+8);
    s[3] = load_le32(sigma+12);

    for (unsigned i = 0; i < 8; ++i)
        s[4+i] = load_le32(key + i*4);

    s[12] = counter;
    s[13] = load_le32(nonce+0);
    s[14] = load_le32(nonce+4);
    s[15] = load_le32(nonce+8);

    memcpy(x, s, sizeof(s));

    for (unsigned r = 0; r < 10; ++r) {
        chacha_qr(x, 0,4,8,12);
        chacha_qr(x, 1,5,9,13);
        chacha_qr(x, 2,6,10,14);
        chacha_qr(x, 3,7,11,15);

        chacha_qr(x, 0,5,10,15);
        chacha_qr(x, 1,6,11,12);
        chacha_qr(x, 2,7,8,13);
        chacha_qr(x, 3,4,9,14);
    }

    for (unsigned i = 0; i < 16; ++i)
        store_le32(out + 4*i, x[i] + s[i]);
}

static int profile_chacha20(const uint8_t seed[64],
                            uint64_t sequence,
                            uint8_t out[64])
{
    static const uint8_t key_domain[] = "UFRNG-PROFILE-CHACHA20-KEY-V1";
    static const uint8_t nonce_domain[] = "UFRNG-PROFILE-CHACHA20-NONCE-V1";

    uint8_t key[32], nonce_material[32], nonce[12];

    if (!digest_parts(EVP_sha256(),
                      key_domain, sizeof(key_domain)-1,
                      seed, 64,
                      NULL, 0, NULL, 0, NULL, 0,
                      key, 32))
        return 0;

    uint8_t seq_be[8];
    store_be64(seq_be, sequence);

    if (!digest_parts(EVP_sha256(),
                      nonce_domain, sizeof(nonce_domain)-1,
                      seed, 64,
                      seq_be, 8,
                      NULL, 0, NULL, 0,
                      nonce_material, 32))
        return 0;

    memcpy(nonce, nonce_material, 12);

    uint8_t block0[64], block1[64];
    chacha20_block(key, 0, nonce, block0);
    chacha20_block(key, 1, nonce, block1);
    memcpy(out + 0, block0, 32);
    memcpy(out + 32, block1, 32);
    memset(block0, 0, sizeof(block0));
    memset(block1, 0, sizeof(block1));

    memset(key, 0, sizeof(key));
    memset(nonce_material, 0, sizeof(nonce_material));
    memset(nonce, 0, sizeof(nonce));
    return 1;
}

/* Frozen AAA seed-side profile. This intentionally preserves the exact
 * key/nonce derivation and 64-byte output layout of CHACHA20-V1.
 * Only the ChaCha core is changed to the screened AAA mutation:
 *   14 double-rounds (28 rounds total),
 *   after the column half of double-round 9,
 *   x[8] += 0x3B54CDA5,
 *   then the diagonal half.
 *
 * This function is Back-side only; it is never called from the Front hot loop. */
static void chacha20_aaa_block(const uint8_t key[32],
                               uint32_t counter,
                               const uint8_t nonce[12],
                               uint8_t out[64])
{
    static const uint8_t sigma[16] = "expand 32-byte k";

    uint32_t s[16], x[16];

    s[0] = load_le32(sigma+0);
    s[1] = load_le32(sigma+4);
    s[2] = load_le32(sigma+8);
    s[3] = load_le32(sigma+12);

    for (unsigned i = 0; i < 8; ++i)
        s[4+i] = load_le32(key + i*4);

    s[12] = counter;
    s[13] = load_le32(nonce+0);
    s[14] = load_le32(nonce+4);
    s[15] = load_le32(nonce+8);

    memcpy(x, s, sizeof(s));

    for (unsigned r = 0; r < UFRNG_AAA_ROUNDS; ++r) {
        /* Column half. */
        chacha_qr(x, 0,4,8,12);
        chacha_qr(x, 1,5,9,13);
        chacha_qr(x, 2,6,10,14);
        chacha_qr(x, 3,7,11,15);

        /* AAA mutation: exactly once, in the frozen screened position. */
        if (r == UFRNG_AAA_INJECT_RR)
            x[UFRNG_AAA_WORD] += UFRNG_AAA_KICK;

        /* Diagonal half. */
        chacha_qr(x, 0,5,10,15);
        chacha_qr(x, 1,6,11,12);
        chacha_qr(x, 2,7,8,13);
        chacha_qr(x, 3,4,9,14);
    }

    for (unsigned i = 0; i < 16; ++i)
        store_le32(out + 4*i, x[i] + s[i]);
}

static int profile_chacha20_aaa(const uint8_t seed[64],
                                uint64_t sequence,
                                uint8_t out[64])
{
    static const uint8_t key_domain[] = "UFRNG-PROFILE-CHACHA20-KEY-V1";
    static const uint8_t nonce_domain[] = "UFRNG-PROFILE-CHACHA20-NONCE-V1";

    uint8_t key[32], nonce_material[32], nonce[12];

    if (!digest_parts(EVP_sha256(),
                      key_domain, sizeof(key_domain)-1,
                      seed, 64,
                      NULL, 0, NULL, 0, NULL, 0,
                      key, 32))
        return 0;

    uint8_t seq_be[8];
    store_be64(seq_be, sequence);

    if (!digest_parts(EVP_sha256(),
                      nonce_domain, sizeof(nonce_domain)-1,
                      seed, 64,
                      seq_be, 8,
                      NULL, 0, NULL, 0,
                      nonce_material, 32))
        return 0;

    memcpy(nonce, nonce_material, 12);

    uint8_t block0[64], block1[64];
    chacha20_aaa_block(key, 0, nonce, block0);
    chacha20_aaa_block(key, 1, nonce, block1);
    memcpy(out + 0, block0, 32);
    memcpy(out + 32, block1, 32);

    memset(block0, 0, sizeof(block0));
    memset(block1, 0, sizeof(block1));
    memset(key, 0, sizeof(key));
    memset(nonce_material, 0, sizeof(nonce_material));
    memset(nonce, 0, sizeof(nonce));
    return 1;
}

/* ============================================================
 * AES-256-CTR PROFILE
 * ============================================================ */

static int profile_aes256_ctr(const uint8_t seed[64],
                              uint64_t sequence,
                              uint8_t out[64])
{
    static const uint8_t key_domain[] = "UFRNG-PROFILE-AES256-KEY-V1";
    static const uint8_t iv_domain[]  = "UFRNG-PROFILE-AES256-IV-V1";

    uint8_t key[32], iv_material[32], iv[16];
    uint8_t seq_be[8];

    store_be64(seq_be, sequence);

    if (!digest_parts(EVP_sha256(),
                      key_domain, sizeof(key_domain)-1,
                      seed, 64,
                      NULL, 0, NULL, 0, NULL, 0,
                      key, 32))
        return 0;

    if (!digest_parts(EVP_sha256(),
                      iv_domain, sizeof(iv_domain)-1,
                      seed, 64,
                      seq_be, 8,
                      NULL, 0, NULL, 0,
                      iv_material, 32))
        return 0;

    /*
     * 64-bit nonce + 64-bit block counter.
     * IV block counter starts at zero.
     */
    memcpy(iv, iv_material, 8);
    memset(iv + 8, 0, 8);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return 0;

    int len1 = 0, len2 = 0;
    int ok =
        EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), NULL, key, iv) == 1 &&
        EVP_EncryptUpdate(ctx, out, &len1, seed, 64) == 1 &&
        EVP_EncryptFinal_ex(ctx, out + len1, &len2) == 1 &&
        (len1 + len2) == 64;

    EVP_CIPHER_CTX_free(ctx);
    memset(key, 0, sizeof(key));
    memset(iv_material, 0, sizeof(iv_material));
    memset(iv, 0, sizeof(iv));
    return ok;
}

/* ============================================================
 * Variant-A / Variant-B
 *
 * Architecture-validation profiles only; not cryptographic primitives.
 * ============================================================ */

static int profile_variant_a(const uint8_t seed[64],
                             uint64_t sequence,
                             uint8_t out[64])
{
    static const uint8_t d0[] = "UFRNG-VARIANT-A-LANE-0";
    static const uint8_t d1[] = "UFRNG-VARIANT-A-LANE-1";
    uint8_t seq_be[8], a[64], b[64];

    store_be64(seq_be, sequence);

    if (!digest_parts(EVP_sha512(), d0, sizeof(d0)-1, seed, 64,
                      seq_be, 8, NULL, 0, NULL, 0, a, 64))
        return 0;
    if (!digest_parts(EVP_sha512(), d1, sizeof(d1)-1, seed, 64,
                      seq_be, 8, NULL, 0, NULL, 0, b, 64))
        return 0;

    for (size_t i = 0; i < 64; ++i) out[i] = a[i] ^ b[i];

    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    return 1;
}

static int profile_variant_b(const uint8_t seed[64],
                             uint64_t sequence,
                             uint8_t out[64])
{
    static const uint8_t domain[] = "UFRNG-VARIANT-B-V1";
    uint8_t seq_be[8], lane_be[4];

    store_be64(seq_be, sequence);

    for (uint32_t lane = 0; lane < 2; ++lane) {
        store_be32(lane_be, lane);

        if (!digest_parts(EVP_sha256(),
                          domain, sizeof(domain)-1,
                          lane_be, 4,
                          seq_be, 8,
                          seed, 64,
                          NULL, 0,
                          out + lane*32, 32))
            return 0;
    }
    return 1;
}

int ufrng_profile_apply(const uint8_t seed64[64],
                        const UFRNG_SeedMetadata *meta,
                        uint8_t out64[64])
{
    if (!seed64 || !metadata_ok(meta) || !out64) return 0;

    if (strcmp(meta->profile_id, UFRNG_PROFILE_CHACHA20_AAA) == 0)
        return profile_chacha20_aaa(seed64, meta->sequence, out64);

    if (strcmp(meta->profile_id, UFRNG_PROFILE_CHACHA20) == 0)
        return profile_chacha20(seed64, meta->sequence, out64);

    if (strcmp(meta->profile_id, UFRNG_PROFILE_AES256) == 0)
        return profile_aes256_ctr(seed64, meta->sequence, out64);

    if (strcmp(meta->profile_id, UFRNG_PROFILE_VARIANT_A) == 0)
        return profile_variant_a(seed64, meta->sequence, out64);

    if (strcmp(meta->profile_id, UFRNG_PROFILE_VARIANT_B) == 0)
        return profile_variant_b(seed64, meta->sequence, out64);

    return 0;
}

/* ============================================================
 * Final clean handoff
 * ============================================================ */

int ufrng_prepare_front_seed(const uint8_t seed64[64],
                             const UFRNG_SeedMetadata *meta,
                             UFRNG_FrontSeed *out_front)
{
    if (!seed64 || !metadata_ok(meta) || !out_front) return 0;

    if (!ufrng_profile_apply(seed64, meta, out_front->data))
        return 0;

    return 1;
}

int ufrng_seedbank_profile_to_front(
    const uint8_t root_seed64[64],
    uint64_t bank_index,
    const UFRNG_SeedMetadata *meta,
    UFRNG_FrontSeed *out_front,
    UFRNG_SeedBackRecord *out_record)
{
    if (!root_seed64 || !metadata_ok(meta) || !out_front || !out_record)
        return 0;

    uint8_t seed64[64];

    if (!ufrng_seed_bank_entry(root_seed64, bank_index, seed64))
        return 0;

    if (!ufrng_prepare_front_seed(seed64, meta, out_front))
        return 0;

    if (!ufrng_make_back_record(meta, bank_index, root_seed64,
                                 out_front->data, out_record))
        return 0;

    /*
     * Explicitly dispose of the temporary Bank seed after the handoff.
     * The Front retains only out_front->data.
     */
    memset(seed64, 0, sizeof(seed64));
    return 1;
}


/* ============================================================
 * Front16 seed expansion / audit record
 *
 * This is intentionally Back-only.  No SHA work from this section is
 * intended to execute on the Front hot path.
 * ============================================================ */

static int front16_profile_string_ok(const UFRNG_SeedMetadata *meta)
{
    return meta && meta->profile_id && meta->pipeline_version && meta->lps_profile;
}

int ufrng_front16_expand_one(const uint8_t profiled_seed64[64],
                             uint64_t bank_index,
                             uint64_t generation,
                             uint32_t lane,
                             const UFRNG_SeedMetadata *meta,
                             uint8_t out64[64])
{
    if (!profiled_seed64 || !out64 || !front16_profile_string_ok(meta)) return 0;
    if (lane >= UFRNG_FRONT16_COUNT) return 0;

    static const uint8_t domain[] = "UFRNG-FRONT16-SHA512-V1";
    uint8_t lane_be[4], bank_be[8], gen_be[8];
    store_be32(lane_be, lane);
    store_be64(bank_be, bank_index);
    store_be64(gen_be, generation);

    /*
     * The lane, bank and generation are explicit domain-separated inputs.
     * This is a construction mechanism, not a claim that SHA-512 by itself
     * proves statistical independence; that property is checked separately.
     */
    return digest_parts(EVP_sha512(),
                        domain, sizeof(domain)-1,
                        profiled_seed64, 64,
                        bank_be, 8,
                        gen_be, 8,
                        lane_be, 4,
                        out64, 64);
}

static int front16_fingerprint(const UFRNG_FrontSeedSet16 *set,
                               const UFRNG_SeedMetadata *meta,
                               uint8_t out32[32])
{
    if (!set || !meta || !out32) return 0;
    static const uint8_t domain[] = "UFRNG-FRONT16-FP-V1";
    uint8_t bank_be[8], gen_be[8];
    store_be64(bank_be, set->bank_index);
    store_be64(gen_be, set->generation);

    uint8_t payload[16 * 64 + 8 + 8 + 64 + 64 + 32];
    size_t pos = 0;
    memcpy(payload + pos, domain, sizeof(domain)-1); pos += sizeof(domain)-1;
    memcpy(payload + pos, bank_be, 8); pos += 8;
    memcpy(payload + pos, gen_be, 8); pos += 8;

    size_t n = strlen(meta->pipeline_version);
    if (n > 63) n = 63;
    payload[pos++] = (uint8_t)n;
    memcpy(payload + pos, meta->pipeline_version, n); pos += n;

    n = strlen(meta->lps_profile);
    if (n > 63) n = 63;
    payload[pos++] = (uint8_t)n;
    memcpy(payload + pos, meta->lps_profile, n); pos += n;

    n = strlen(meta->profile_id);
    if (n > 63) n = 63;
    payload[pos++] = (uint8_t)n;
    memcpy(payload + pos, meta->profile_id, n); pos += n;

    memcpy(payload + pos, set->data, sizeof(set->data)); pos += sizeof(set->data);
    return digest_once(EVP_sha256(), payload, pos, out32, 32);
}

#ifndef UFR_MULTI_K
#define UFR_MULTI_K 3u
#ifndef UFR_MULTI_ROT
#define UFR_MULTI_ROT 0u
#endif
#endif

static unsigned ufr_3p_perm_index(uint64_t generation)
{
    return (unsigned)((generation >> 1) % 6u);
}

static UFR_SCALAR_ONLY int ufrx_prepare_next_multiK_impl(UFRXWorker *bw)
{
    if(!bw||bw->pending_ready)return 0;

    UFRNG_FrontSeedSet16 sets[3];
    UFRNG_FrontSeedSet16 pending;
    UFRNG_Front16BackRecord rec;
    memset(&pending,0,sizeof(pending));
    memset(&rec,0,sizeof(rec));
    pending.bank_index=bw->next_bank_index;
    pending.generation=bw->next_generation;
    strncpy(pending.profile_id,bw->meta.profile_id,sizeof(pending.profile_id)-1);
    strncpy(pending.construction_version,"VHV-MULTI3-TRUE3P-M11ADDROT1-M15A-AAA-A1-R14-RR9-W8-ADD-GB375-BACK-V1",sizeof(pending.construction_version)-1);

    if(UFR_MULTI_K != 3u) return 0;
    /* Three same-PROFILE lineages remain independent through the complete V->H->V. */
    for(unsigned si=0;si<3u;++si){
        memset(&sets[si],0,sizeof(sets[si]));
        if(!ufrng_seedbank_profile_to_front16_vhv(bw->root_seed64[si],
                                                   bw->next_bank_index,
                                                   bw->next_generation,
                                                   &bw->meta,
                                                   &sets[si],&rec)) return 0;
    }

    static const unsigned perms[6][3]={{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
    const unsigned pi=ufr_3p_perm_index(bw->next_generation);
    const unsigned A=perms[pi][0],B=perms[pi][1],C=perms[pi][2];

    /* 3 live parents + 13 auxiliary states: same 16-state resident budget. */
    memcpy(pending.data[0],sets[A].data[0],64);
    memcpy(pending.data[1],sets[B].data[0],64);
    memcpy(pending.data[2],sets[C].data[0],64);
    memcpy(pending.data[3],sets[A].data[2],64);
    memcpy(pending.data[4],sets[B].data[2],64);
    memcpy(pending.data[5],sets[C].data[2],64);
    memcpy(pending.data[6],sets[A].data[3],64);
    memcpy(pending.data[7],sets[B].data[3],64);
    memcpy(pending.data[8],sets[C].data[3],64);
    memcpy(pending.data[9],sets[A].data[4],64);
    memcpy(pending.data[10],sets[B].data[4],64);
    memcpy(pending.data[11],sets[C].data[4],64);
    memcpy(pending.data[12],sets[A].data[5],64);
    memcpy(pending.data[13],sets[B].data[5],64);
    memcpy(pending.data[14],sets[C].data[5],64);
    for(unsigned j=0;j<64u;++j)
        pending.data[15][j]=(uint8_t)(sets[A].data[1][j]^sets[B].data[1][j]^sets[C].data[1][j]);

    bw->pending_set=pending;
    memset(&bw->pending_record,0,sizeof(bw->pending_record));
    bw->pending_record.meta=bw->meta;
    bw->pending_record.bank_index=bw->next_bank_index;
    bw->pending_record.generation=bw->next_generation;
    bw->pending_record.front_state_count=UFRNG_FRONT16_COUNT;
    strncpy(bw->pending_record.profile_id,bw->meta.profile_id,sizeof(bw->pending_record.profile_id)-1);
    {
        const size_t n = strnlen(pending.construction_version, sizeof(bw->pending_record.construction_version)-1u);
        memcpy(bw->pending_record.construction_version, pending.construction_version, n);
        bw->pending_record.construction_version[n] = '\0';
    }
    uint8_t fp[32];
    if(!digest_once(EVP_sha256(),(const uint8_t *)bw->root_seed64,sizeof(bw->root_seed64),fp,32)) return 0;
    hex64(fp,bw->pending_record.root_fingerprint_hex);
    if(!front16_fingerprint(&bw->pending_set,&bw->meta,fp)) return 0;
    hex64(fp,bw->pending_set.fingerprint_hex);
    hex64(fp,bw->pending_record.front16_fingerprint_hex);
    static const uint8_t repro_domain[]="UFRNG-FRONT16-VHV-MULTI3-TRUE3P-M11ADDROT1-M15A-AAA-A1-R14-RR9-W8-ADD-GB375-BACK-V1-REPRO-V1";
    if(!digest_parts(EVP_sha256(),repro_domain,sizeof(repro_domain)-1,
                     (const uint8_t *)bw->root_seed64,sizeof(bw->root_seed64),
                     (const uint8_t *)bw->pending_set.data,sizeof(bw->pending_set.data),
                     NULL,0,NULL,0,fp,32)) return 0;
    hex64(fp,bw->pending_record.reproducible_id_hex);

    volatile uint8_t *vp=(volatile uint8_t *)sets;
    for(size_t i=0;i<sizeof(sets);++i)vp[i]=0;
    bw->pending_ready=1;
    ++bw->next_bank_index;
    bw->next_generation+=2u;
    return 1;
}

static int make_front16_record(const UFRNG_SeedMetadata *meta,
                               uint64_t bank_index,
                               uint64_t generation,
                               const uint8_t root_seed64[64],
                               const UFRNG_FrontSeedSet16 *set,
                               UFRNG_Front16BackRecord *out)
{
    if (!front16_profile_string_ok(meta) || !root_seed64 || !set || !out) return 0;

    static const uint8_t root_domain[] = "UFRNG-ROOT-FP-V1";
    static const uint8_t repro_domain[] = "UFRNG-FRONT16-REPRO-V1";
    uint8_t root_fp[32], set_fp[32];
    if (!digest_parts(EVP_sha256(), root_domain, sizeof(root_domain)-1,
                      root_seed64, 64, NULL, 0, NULL, 0, NULL, 0,
                      root_fp, 32)) return 0;
    if (!front16_fingerprint(set, meta, set_fp)) return 0;

    uint8_t payload[4096];
    size_t pos = 0;
    memcpy(payload + pos, repro_domain, sizeof(repro_domain)-1); pos += sizeof(repro_domain)-1;
    memcpy(payload + pos, root_fp, 32); pos += 32;
    memcpy(payload + pos, set_fp, 32); pos += 32;

    uint8_t bank_be[8], gen_be[8], cnt_be[4];
    store_be64(bank_be, bank_index);
    store_be64(gen_be, generation);
    store_be32(cnt_be, UFRNG_FRONT16_COUNT);
    memcpy(payload + pos, bank_be, 8); pos += 8;
    memcpy(payload + pos, gen_be, 8); pos += 8;
    memcpy(payload + pos, cnt_be, 4); pos += 4;

    const char *parts[3] = {meta->pipeline_version, meta->lps_profile, meta->profile_id};
    for (unsigned i = 0; i < 3; ++i) {
        size_t n = strlen(parts[i]);
        if (n > 255) n = 255;
        payload[pos++] = (uint8_t)n;
        memcpy(payload + pos, parts[i], n); pos += n;
    }

    uint8_t repro[32];
    if (!digest_once(EVP_sha256(), payload, pos, repro, 32)) return 0;

    memset(out, 0, sizeof(*out));
    out->meta = *meta;
    out->bank_index = bank_index;
    out->generation = generation;
    out->front_state_count = UFRNG_FRONT16_COUNT;
    strncpy(out->profile_id, meta->profile_id, sizeof(out->profile_id)-1);
    memset(out->construction_version, 0, sizeof(out->construction_version));
    size_t cn = strlen(set->construction_version);
    if (cn >= sizeof(out->construction_version)) cn = sizeof(out->construction_version) - 1u;
    memcpy(out->construction_version, set->construction_version, cn);
    hex64(root_fp, out->root_fingerprint_hex);
    hex64(set_fp, out->front16_fingerprint_hex);
    hex64(repro, out->reproducible_id_hex);
    return 1;
}

int ufrng_seedbank_profile_to_front16(
    const uint8_t root_seed64[64],
    uint64_t bank_index,
    uint64_t generation,
    const UFRNG_SeedMetadata *meta,
    UFRNG_FrontSeedSet16 *out_set,
    UFRNG_Front16BackRecord *out_record)
{
    if (!root_seed64 || !front16_profile_string_ok(meta) || !out_set || !out_record)
        return 0;

    uint8_t bank_seed[64], profiled_seed[64];
    if (!ufrng_seed_bank_entry(root_seed64, bank_index, bank_seed)) return 0;
    if (!ufrng_profile_apply(bank_seed, meta, profiled_seed)) {
        memset(bank_seed, 0, sizeof(bank_seed));
        return 0;
    }

    memset(out_set, 0, sizeof(*out_set));
    out_set->bank_index = bank_index;
    out_set->generation = generation;
    strncpy(out_set->profile_id, meta->profile_id, sizeof(out_set->profile_id)-1);
    strncpy(out_set->construction_version, UFRNG_FRONT16_CONSTRUCTION_VERSION,
            sizeof(out_set->construction_version)-1);

    for (uint32_t lane = 0; lane < UFRNG_FRONT16_COUNT; ++lane) {
        if (!ufrng_front16_expand_one(profiled_seed, bank_index, generation,
                                      lane, meta, out_set->data[lane])) {
            memset(bank_seed, 0, sizeof(bank_seed));
            memset(profiled_seed, 0, sizeof(profiled_seed));
            return 0;
        }
    }

    uint8_t fp[32];
    if (!front16_fingerprint(out_set, meta, fp)) {
        memset(bank_seed, 0, sizeof(bank_seed));
        memset(profiled_seed, 0, sizeof(profiled_seed));
        return 0;
    }
    hex64(fp, out_set->fingerprint_hex);

    const int ok = make_front16_record(meta, bank_index, generation,
                                       root_seed64, out_set, out_record);
    memset(bank_seed, 0, sizeof(bank_seed));
    memset(profiled_seed, 0, sizeof(profiled_seed));
    return ok;
}


/* ============================================================
 * Front16 V->H->V construction
 * ============================================================ */

static int front16_vhv_expand_one_impl(
    const uint8_t profiled_seed64[64],
    const uint8_t *matrix1024,
    uint64_t bank_index,
    uint64_t generation,
    uint32_t lane,
    const UFRNG_SeedMetadata *meta,
    uint8_t out64[64])
{
    if (!profiled_seed64 || !matrix1024 || !out64 || !front16_profile_string_ok(meta)) return 0;
    if (lane >= UFRNG_FRONT16_COUNT) return 0;

    static const uint8_t domain[] = "UFRNG-FRONT16-VHV-FINAL-V1";
    uint8_t lane_be[4], bank_be[8], gen_be[8], ctx[20];
    store_be32(lane_be, lane);
    store_be64(bank_be, bank_index);
    store_be64(gen_be, generation);
    memcpy(ctx + 0, bank_be, 8);
    memcpy(ctx + 8, gen_be, 8);
    memcpy(ctx + 16, lane_be, 4);

    /* Horizontal stage is intentional: every lane sees the entire 1024B set. */
    return digest_parts(EVP_sha512(),
                        domain, sizeof(domain)-1,
                        matrix1024, UFRNG_FRONT16_COUNT * UFRNG_SEED_BYTES,
                        profiled_seed64, 64,
                        ctx, sizeof(ctx),
                        NULL, 0,
                        out64, 64);
}

int ufrng_front16_vhv_expand_one(
    const uint8_t profiled_seed64[64],
    const uint8_t *matrix1024,
    uint64_t bank_index,
    uint64_t generation,
    uint32_t lane,
    const UFRNG_SeedMetadata *meta,
    uint8_t out64[64])
{
    return front16_vhv_expand_one_impl(profiled_seed64, matrix1024,
                                       bank_index, generation, lane,
                                       meta, out64);
}

int ufrng_seedbank_profile_to_front16_vhv(
    const uint8_t root_seed64[64],
    uint64_t bank_index,
    uint64_t generation,
    const UFRNG_SeedMetadata *meta,
    UFRNG_FrontSeedSet16 *out_set,
    UFRNG_Front16BackRecord *out_record)
{
    if (!root_seed64 || !front16_profile_string_ok(meta) || !out_set || !out_record)
        return 0;

    uint8_t bank_seed[64], profiled_seed[64];
    uint8_t v1[UFRNG_FRONT16_COUNT][UFRNG_SEED_BYTES];

    if (!ufrng_seed_bank_entry(root_seed64, bank_index, bank_seed)) return 0;
    if (!ufrng_profile_apply(bank_seed, meta, profiled_seed)) {
        memset(bank_seed, 0, sizeof(bank_seed));
        return 0;
    }

    /* V1: independent lane expansion. */
    for (uint32_t lane = 0; lane < UFRNG_FRONT16_COUNT; ++lane) {
        if (!ufrng_front16_expand_one(profiled_seed, bank_index, generation,
                                      lane, meta, v1[lane])) {
            memset(bank_seed, 0, sizeof(bank_seed));
            memset(profiled_seed, 0, sizeof(profiled_seed));
            memset(v1, 0, sizeof(v1));
            return 0;
        }
    }

    /* H: one global digest over the complete V1 matrix. */
    static const uint8_t h_domain[] = "UFRNG-FRONT16-VHV-HASH-V1";
    uint8_t hctx[16], h[64];
    store_be64(hctx + 0, bank_index);
    store_be64(hctx + 8, generation);
    if (!digest_parts(EVP_sha512(),
                      h_domain, sizeof(h_domain)-1,
                      (const uint8_t *)v1, sizeof(v1),
                      hctx, sizeof(hctx),
                      NULL, 0, NULL, 0,
                      h, 64)) {
        memset(bank_seed, 0, sizeof(bank_seed));
        memset(profiled_seed, 0, sizeof(profiled_seed));
        memset(v1, 0, sizeof(v1));
        return 0;
    }

    memset(out_set, 0, sizeof(*out_set));
    out_set->bank_index = bank_index;
    out_set->generation = generation;
    strncpy(out_set->profile_id, meta->profile_id, sizeof(out_set->profile_id)-1);
    strncpy(out_set->construction_version, UFRNG_FRONT16_VHV_COMPACT_VERSION,
            sizeof(out_set->construction_version)-1);

    /* V2: each lane keeps its own V1 entropy while also seeing global H. */
    for (uint32_t lane = 0; lane < UFRNG_FRONT16_COUNT; ++lane) {
        static const uint8_t v2_domain[] = "UFRNG-FRONT16-VHV-COMPACT-V1";
        uint8_t lane_be[4];
        store_be32(lane_be, lane);
        if (!digest_parts(EVP_sha512(),
                          v2_domain, sizeof(v2_domain)-1,
                          v1[lane], sizeof(v1[lane]),
                          h, sizeof(h),
                          lane_be, sizeof(lane_be),
                          NULL, 0,
                          out_set->data[lane], 64)) {
            memset(bank_seed, 0, sizeof(bank_seed));
            memset(profiled_seed, 0, sizeof(profiled_seed));
            memset(v1, 0, sizeof(v1));
            memset(h, 0, sizeof(h));
            memset(out_set, 0, sizeof(*out_set));
            return 0;
        }
    }

    uint8_t fp[32];
    if (!front16_fingerprint(out_set, meta, fp)) {
        memset(bank_seed, 0, sizeof(bank_seed));
        memset(profiled_seed, 0, sizeof(profiled_seed));
        memset(v1, 0, sizeof(v1));
        memset(out_set, 0, sizeof(*out_set));
        return 0;
    }
    hex64(fp, out_set->fingerprint_hex);

    const int ok = make_front16_record(meta, bank_index, generation,
                                       root_seed64, out_set, out_record);
    memset(bank_seed, 0, sizeof(bank_seed));
    memset(profiled_seed, 0, sizeof(profiled_seed));
    memset(v1, 0, sizeof(v1));
    memset(h, 0, sizeof(h));
    return ok;
}


/* ============================================================
 * Development timing entropy source
 * ============================================================ */

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
           (uint64_t)ts.tv_nsec;
}

/*
 * Development-only physical/timing source.
 * This does not claim a validated entropy rate.
 *
 * blocks = number of 64-byte samples.
 */
static int collect_timing_entropy(uint8_t *out, size_t blocks)
{
    if (!out) return 0;

    uint64_t prev = monotonic_ns();

    for (size_t b = 0; b < blocks; ++b) {
        for (unsigned j = 0; j < 8; ++j) {
            uint64_t now = monotonic_ns();
            uint64_t delta = now - prev;
            prev = now;

            /*
             * Add a tiny variable workload to decorrelate the timestamp
             * sampling path; still only a development entropy source.
             */
            volatile uint64_t spin = delta ^ (UINT64_C(0x9E3779B97F4A7C15) * (j + 1u));
            for (unsigned k = 0; k < 8; ++k)
                spin ^= (spin << 7) + (spin >> 3) + UINT64_C(0xA5A5A5A5A5A5A5A5);

            uint8_t *p = out + b*64 + j*8;
            store_be64(p, spin ^ delta);
        }
    }

    return 1;
}

/* Exposed only to the test translation unit below via a forward declaration. */
int ufrng_test_collect_timing_entropy(uint8_t *out, size_t blocks)
{
    return collect_timing_entropy(out, blocks);
}


/* -------------------------------------------------------------------------
 * Stable PaperView A64 descriptor family
 * ------------------------------------------------------------------------- */
#ifndef UFR_CULTIVATION_PAPERVIEW_DESCRIPTOR_A64_FINAL_H
#define UFR_CULTIVATION_PAPERVIEW_DESCRIPTOR_A64_FINAL_H
#include <stdint.h>
#define UFR_PAPER_A64_MAX_VIEWS 32u
typedef struct { uint16_t offset_qv; uint8_t lane_perm; uint8_t tail_rule; } ufr_paper_view_desc_a64_final;
static const uint16_t UFR_PAPER_A64_OFFSET[32] = {0,333,254,611,214,871,300,561,739,892,377,139,89,406,718,996,353,202,491,528,322,276,730,847,41,385,936,839,238,427,960,762};
static const uint8_t UFR_PAPER_A64_LANE[32] = {0,58,49,52,53,9,47,54,56,27,61,43,22,15,61,56,26,16,50,34,63,53,5,58,37,20,7,36,40,21,14,13};
static const uint8_t UFR_PAPER_A64_TAIL[32] = {0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3};
static const uint32_t UFR_PAPER_A64_INDEX[64][16] = {
  {9,14,3,8,13,2,7,12,1,6,11,0,5,10,15,4},
  {14,1,4,7,10,13,0,3,6,9,12,15,2,5,8,11},
  {11,8,5,2,15,12,9,6,3,0,13,10,7,4,1,14},
  {4,7,10,13,0,3,6,9,12,15,2,5,8,11,14,1},
  {9,6,3,0,13,10,7,4,1,14,11,8,5,2,15,12},
  {6,3,0,13,10,7,4,1,14,11,8,5,2,15,12,9},
  {12,15,2,5,8,11,14,1,4,7,10,13,0,3,6,9},
  {1,4,7,10,13,0,3,6,9,12,15,2,5,8,11,14},
  {1,14,11,8,5,2,15,12,9,6,3,0,13,10,7,4},
  {5,2,15,12,9,6,3,0,13,10,7,4,1,14,11,8},
  {10,7,4,1,14,11,8,5,2,15,12,9,6,3,0,13},
  {3,0,13,10,7,4,1,14,11,8,5,2,15,12,9,6},
  {5,8,11,14,1,4,7,10,13,0,3,6,9,12,15,2},
  {10,13,0,3,6,9,12,15,2,5,8,11,14,1,4,7},
  {2,5,8,11,14,1,4,7,10,13,0,3,6,9,12,15},
  {6,9,12,15,2,5,8,11,14,1,4,7,10,13,0,3},
  {9,12,15,2,5,8,11,14,1,4,7,10,13,0,3,6},
  {2,15,12,9,6,3,0,13,10,7,4,1,14,11,8,5},
  {13,0,3,6,9,12,15,2,5,8,11,14,1,4,7,10},
  {7,4,1,14,11,8,5,2,15,12,9,6,3,0,13,10},
  {0,3,6,9,12,15,2,5,8,11,14,1,4,7,10,13},
  {8,11,14,1,4,7,10,13,0,3,6,9,12,15,2,5},
  {4,15,10,5,0,11,6,1,12,7,2,13,8,3,14,9},
  {13,10,7,4,1,14,11,8,5,2,15,12,9,6,3,0},
  {7,12,1,6,11,0,5,10,15,4,9,14,3,8,13,2},
  {1,6,11,0,5,10,15,4,9,14,3,8,13,2,7,12},
  {14,11,8,5,2,15,12,9,6,3,0,13,10,7,4,1},
  {15,12,9,6,3,0,13,10,7,4,1,14,11,8,5,2},
  {12,7,2,13,8,3,14,9,4,15,10,5,0,11,6,1},
  {2,7,12,1,6,11,0,5,10,15,4,9,14,3,8,13},
  {0,11,6,1,12,7,2,13,8,3,14,9,4,15,10,5},
  {15,10,5,0,11,6,1,12,7,2,13,8,3,14,9,4},
  {10,15,4,9,14,3,8,13,2,7,12,1,6,11,0,5},
  {5,10,15,4,9,14,3,8,13,2,7,12,1,6,11,0},
  {4,9,14,3,8,13,2,7,12,1,6,11,0,5,10,15},
  {1,12,7,2,13,8,3,14,9,4,15,10,5,0,11,6},
  {8,3,14,9,4,15,10,5,0,11,6,1,12,7,2,13},
  {11,0,5,10,15,4,9,14,3,8,13,2,7,12,1,6},
  {12,1,6,11,0,5,10,15,4,9,14,3,8,13,2,7},
  {14,9,4,15,10,5,0,11,6,1,12,7,2,13,8,3},
  {0,5,10,15,4,9,14,3,8,13,2,7,12,1,6,11},
  {2,13,8,3,14,9,4,15,10,5,0,11,6,1,12,7},
  {7,2,13,8,3,14,9,4,15,10,5,0,11,6,1,12},
  {3,8,13,2,7,12,1,6,11,0,5,10,15,4,9,14},
  {11,6,1,12,7,2,13,8,3,14,9,4,15,10,5,0},
  {6,1,12,7,2,13,8,3,14,9,4,15,10,5,0,11},
  {13,8,3,14,9,4,15,10,5,0,11,6,1,12,7,2},
  {6,11,0,5,10,15,4,9,14,3,8,13,2,7,12,1},
  {9,4,15,10,5,0,11,6,1,12,7,2,13,8,3,14},
  {13,2,7,12,1,6,11,0,5,10,15,4,9,14,3,8},
  {10,5,0,11,6,1,12,7,2,13,8,3,14,9,4,15},
  {5,0,11,6,1,12,7,2,13,8,3,14,9,4,15,10},
  {15,4,9,14,3,8,13,2,7,12,1,6,11,0,5,10},
  {8,13,2,7,12,1,6,11,0,5,10,15,4,9,14,3},
  {14,3,8,13,2,7,12,1,6,11,0,5,10,15,4,9},
  {3,14,9,4,15,10,5,0,11,6,1,12,7,2,13,8},
  {15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0},
  {3,2,1,0,15,14,13,12,11,10,9,8,7,6,5,4},
  {11,10,9,8,7,6,5,4,3,2,1,0,15,14,13,12},
  {4,5,6,7,8,9,10,11,12,13,14,15,0,1,2,3},
  {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
  {8,9,10,11,12,13,14,15,0,1,2,3,4,5,6,7},
  {12,13,14,15,0,1,2,3,4,5,6,7,8,9,10,11},
  {7,6,5,4,3,2,1,0,15,14,13,12,11,10,9,8},
};
#endif



/* -------------------------------------------------------------------------
 * Public Final-Cultivation profile registry
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t registered;
    ufr_final_profile_desc_t desc;
} ufr_final_profile_slot_t;

static ufr_final_profile_slot_t g_ufr_final_profiles[UFR_FINAL_API_MAX_CUSTOM_PROFILES];
static _Atomic unsigned g_ufr_final_profile_count = 0u;
static _Atomic unsigned g_ufr_final_registry_locked = 0u;

static int ufr_final_profile_valid_desc(const ufr_final_profile_desc_t *d)
{
    if (!d || d->abi_version != UFR_FINAL_API_VERSION || !d->name || !d->build_row) return 0;
    if (d->bytes_per_row == 0u || d->bytes_per_row > (1u << 20)) return 0;
    if ((d->bytes_per_row & 63u) != 0u) return 0;
    if (!(d->flags & UFR_FINAL_PROFILE_FLAG_ROW_LOCAL)) return 0;
    return 1;
}

static const ufr_final_profile_desc_t *ufr_final_profile_lookup(uint32_t id)
{
    if (id == 0u || id > UFR_FINAL_API_MAX_CUSTOM_PROFILES) return NULL;
    const ufr_final_profile_slot_t *slot = &g_ufr_final_profiles[id - 1u];
    return slot->registered ? &slot->desc : NULL;
}

int ufr_final_profile_register(const ufr_final_profile_desc_t *desc,
                               uint32_t *out_profile_id)
{
    if (!ufr_final_profile_valid_desc(desc) || !out_profile_id) return -1;
    if (atomic_load_explicit(&g_ufr_final_registry_locked, memory_order_acquire)) return -2;
    unsigned n = atomic_load_explicit(&g_ufr_final_profile_count, memory_order_relaxed);
    if (n >= UFR_FINAL_API_MAX_CUSTOM_PROFILES) return -3;
    g_ufr_final_profiles[n].desc = *desc;
    g_ufr_final_profiles[n].registered = 1u;
    atomic_store_explicit(&g_ufr_final_profile_count, n + 1u, memory_order_release);
    *out_profile_id = n + 1u;
    return 0;
}

int ufr_final_profile_query(uint32_t profile_id,
                            ufr_final_profile_desc_t *out_desc)
{
    if (!out_desc) return -1;
    const ufr_final_profile_desc_t *d = ufr_final_profile_lookup(profile_id);
    if (!d) return -2;
    *out_desc = *d;
    return 0;
}

/* -------------------------------------------------------------------------
 * PaperView cultivation + MC 3-mode controller
 * ------------------------------------------------------------------------- */
#include <errno.h>

#ifndef UFR_PAPER_MAX_VIEWS
#define UFR_PAPER_MAX_VIEWS 32u
#endif
#ifndef UFR_PAPER_NV
#define UFR_PAPER_NV 1024u
#endif
/* Quality fix: permute the normal final-cache row source as r -> 465*r mod 1024.
 * The MC consumer still reads cache rows sequentially; only cache-build source
 * selection changes. 465 is odd, so the mapping is a bijection over 1024 rows.
 * R2 uses its separate pair stride below. */
#ifndef UFR_PAPER_CACHE_STRIDE
#define UFR_PAPER_CACHE_STRIDE 465u
#endif
#ifndef UFR_R2_PAIR_STRIDE
#define UFR_R2_PAIR_STRIDE 555u
#endif
#ifndef UFR_R2_NORMAL_SCALE
#define UFR_R2_NORMAL_SCALE (0.0009317057574691516f * 1.0007781f)
#endif
#if ((UFR_PAPER_CACHE_STRIDE & 1u) == 0u)
#error "UFR_PAPER_CACHE_STRIDE must be odd"
#endif
#ifndef UFR_PAPER_V
#define UFR_PAPER_V 16u
#endif
#ifndef UFR_PAPER_SAMPLES
#define UFR_PAPER_SAMPLES 16384u /* 64 KiB int32 starter */
#endif
#ifndef UFR_PAPER_BYTES
#define UFR_PAPER_BYTES (UFR_PAPER_SAMPLES * sizeof(int32_t))
#endif
#ifndef UFR_PAPER_MC_BLOCK_BYTES
#define UFR_PAPER_MC_BLOCK_BYTES 1024u /* 256 int32 q samples */
#endif
#ifndef UFR_PAPER_MC_BLOCKS_PER_VIEW
#define UFR_PAPER_MC_BLOCKS_PER_VIEW 64u
#endif
#define UFR_PAPER_MAX_CHUNK_BYTES (UFR_PAPER_MAX_VIEWS * UFR_PAPER_BYTES)
#define UFR_PAPER_DEFAULT_CHUNK_BYTES (4u * UFR_PAPER_BYTES)
#define UFR_PAPER_L1_MAX_CHUNK_BYTES (4u * UFR_PAPER_BYTES)
#define UFR_PAPER_L2_MAX_CHUNK_BYTES (UFR_PAPER_MAX_CHUNK_BYTES)
#define UFR_PAPER_AUTO_MIN_GENERATION 1u
#define UFR_PAPER_AUTO_MAX_GENERATION 1u

_Static_assert(UFR_PAPER_SAMPLES == UFR_PAPER_NV * UFR_PAPER_V,
               "PaperView geometry mismatch");
_Static_assert(UFR_PAPER_BYTES == 65536u, "PaperView starter must remain 64 KiB");
_Static_assert(UFR_PAPER_MC_BLOCK_BYTES == 256u*sizeof(int32_t), "MC block geometry");

typedef enum {
    UFR_MC_CHUNK_AUTO = 0,
    UFR_MC_CHUNK_CUSTOM = 1
} ufr_mc_chunk_mode_t;

typedef enum {
    UFR_GENERATION_AUTO = 0,
    UFR_GENERATION_CHUNK = 1,
    UFR_GENERATION_FIXED = 2
} ufr_generation_mode_t;

typedef enum {
    UFR_GENERATION_FIELD_AUTO = 0,
    UFR_GENERATION_FIELD_CUSTOM = 1
} ufr_generation_field_mode_t;

/* Final public MC configuration surface.
 * In FIXED generation mode, min_generation_chunks is the fixed interval.
 * max_generation_chunks remains the hard safety ceiling. */
typedef struct {
    ufr_cultivation_mode_t cultivation;
    ufr_mc_chunk_mode_t chunk_mode;
    uint32_t chunk_bytes;
    ufr_final_cultivation_profile_t final_profile;
    uint32_t custom_profile_id;
    ufr_generation_mode_t generation;
    ufr_generation_field_mode_t min_generation_mode;
    uint32_t min_generation_chunks;
    ufr_generation_field_mode_t max_generation_mode;
    uint32_t max_generation_chunks;
} ufr_mc_config_t;

typedef struct {
    ufr_cultivation_mode_t cultivation;
    uint32_t chunk_bytes;
    uint32_t views_per_chunk;
    ufr_final_cultivation_profile_t final_profile;
    uint32_t custom_profile_id;
    ufr_generation_mode_t generation;
    uint32_t min_generation_chunks;
    uint32_t max_generation_chunks;
} ufr_mc_config_resolved_t;

static const char *ufr_final_profile_name(ufr_final_cultivation_profile_t p) {
    switch (p) {
        case UFR_FINAL_AUTO: return "AUTO";
        case UFR_FINAL_RAW_INT16: return "RAW_INT16";
        case UFR_FINAL_INT32: return "INT32";
        case UFR_FINAL_FLOAT32: return "FLOAT32";
        case UFR_FINAL_R2_FLOAT32: return "R2_FLOAT32";
        case UFR_FINAL_CUSTOM: return "CUSTOM";
        default: return "UNKNOWN";
    }
}

static const char *ufr_cult_name(ufr_cultivation_mode_t m) {
    switch (m) {
        case UFR_CULT_AUTO: return "AUTO";
        case UFR_CULT_DIRECT: return "DIRECT";
        case UFR_CULT_L1_CACHE: return "L1_CACHE";
        case UFR_CULT_L2_CACHE: return "L2_CACHE";
        default: return "UNKNOWN";
    }
}
static const char *ufr_gen_name(ufr_generation_mode_t m) {
    switch (m) {
        case UFR_GENERATION_AUTO: return "AUTO";
        case UFR_GENERATION_CHUNK: return "CHUNK";
        case UFR_GENERATION_FIXED: return "FIXED";
        default: return "UNKNOWN";
    }
}

static void ufr_mc_config_default(ufr_mc_config_t *c) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->cultivation = UFR_CULT_AUTO;
    c->chunk_mode = UFR_MC_CHUNK_AUTO;
    c->chunk_bytes = 0u;
    c->final_profile = UFR_FINAL_AUTO;
    c->custom_profile_id = 0u;
    c->generation = UFR_GENERATION_AUTO;
    c->min_generation_mode = UFR_GENERATION_FIELD_AUTO;
    c->min_generation_chunks = 0u;
    c->max_generation_mode = UFR_GENERATION_FIELD_AUTO;
    c->max_generation_chunks = 0u;
}

static int ufr_mc_config_validate(const ufr_mc_config_t *c) {
    if (!c) return -1;
    if (c->cultivation > UFR_CULT_L2_CACHE) return -1;
    if (c->chunk_mode > UFR_MC_CHUNK_CUSTOM) return -1;
    if (c->generation > UFR_GENERATION_FIXED) return -1;
    if (c->final_profile > UFR_FINAL_CUSTOM) return -1;
    if (c->min_generation_mode > UFR_GENERATION_FIELD_CUSTOM ||
        c->max_generation_mode > UFR_GENERATION_FIELD_CUSTOM) return -1;
    if (c->chunk_mode == UFR_MC_CHUNK_CUSTOM) {
        if (c->chunk_bytes < UFR_PAPER_BYTES ||
            c->chunk_bytes > UFR_PAPER_MAX_CHUNK_BYTES ||
            (c->chunk_bytes % UFR_PAPER_BYTES) != 0u) return -1;
    }
    if (c->min_generation_mode == UFR_GENERATION_FIELD_CUSTOM && c->min_generation_chunks == 0u) return -1;
    if (c->max_generation_mode == UFR_GENERATION_FIELD_CUSTOM && c->max_generation_chunks == 0u) return -1;
    return 0;
}

/* Auto policy deliberately chooses one cultivated chunk per Normal starter.
 * This avoids repeated reuse of the same 64 KiB starter as the conservative
 * default. FIXED mode is available for callers that intentionally amortize
 * one starter across a longer generation. */
static int ufr_mc_config_resolve(const ufr_mc_config_t *c, ufr_mc_config_resolved_t *r) {
    if (!c || !r || ufr_mc_config_validate(c) != 0) return -1;
    memset(r, 0, sizeof(*r));

    if (c->cultivation == UFR_CULT_AUTO) {
        /* Auto is resource-conservative: DIRECT unless a cache route is
         * explicitly requested. Runtime heuristics can replace this later. */
        r->cultivation = UFR_CULT_DIRECT;
    } else r->cultivation = c->cultivation;

    /* AUTO preserves the current V1 fast path for DIRECT, while explicit
     * profiles let a customer select the representation needed by its MC.
     * Non-DIRECT customer paths remain on the established int32 qblock boundary. */
    if (c->final_profile == UFR_FINAL_AUTO)
        r->final_profile = (r->cultivation == UFR_CULT_DIRECT)
            ? UFR_FINAL_R2_FLOAT32 : UFR_FINAL_INT32;
    else
        r->final_profile = c->final_profile;
    r->custom_profile_id = (r->final_profile == UFR_FINAL_CUSTOM) ? c->custom_profile_id : 0u;
    if (r->final_profile == UFR_FINAL_CUSTOM && !ufr_final_profile_lookup(r->custom_profile_id)) return -1;

    r->chunk_bytes = c->chunk_mode == UFR_MC_CHUNK_CUSTOM ? c->chunk_bytes : UFR_PAPER_DEFAULT_CHUNK_BYTES;

    if (r->cultivation == UFR_CULT_L1_CACHE && r->chunk_bytes > UFR_PAPER_L1_MAX_CHUNK_BYTES)
        r->chunk_bytes = UFR_PAPER_L1_MAX_CHUNK_BYTES;
    if (r->cultivation == UFR_CULT_L2_CACHE && r->chunk_bytes > UFR_PAPER_L2_MAX_CHUNK_BYTES)
        r->chunk_bytes = UFR_PAPER_L2_MAX_CHUNK_BYTES;

    r->views_per_chunk = r->chunk_bytes / UFR_PAPER_BYTES;
    if (r->views_per_chunk == 0u || r->views_per_chunk > UFR_PAPER_MAX_VIEWS) return -1;

    r->generation = c->generation == UFR_GENERATION_AUTO ? UFR_GENERATION_CHUNK : c->generation;
    r->min_generation_chunks = (c->min_generation_mode == UFR_GENERATION_FIELD_CUSTOM)
        ? c->min_generation_chunks : UFR_PAPER_AUTO_MIN_GENERATION;
    r->max_generation_chunks = (c->max_generation_mode == UFR_GENERATION_FIELD_CUSTOM)
        ? c->max_generation_chunks : UFR_PAPER_AUTO_MAX_GENERATION;
    if (r->max_generation_chunks < r->min_generation_chunks) return -1;
    if (r->generation == UFR_GENERATION_FIXED && c->min_generation_mode == UFR_GENERATION_FIELD_AUTO)
        r->min_generation_chunks = UFR_PAPER_AUTO_MIN_GENERATION;
    if (r->generation == UFR_GENERATION_FIXED && r->max_generation_chunks < r->min_generation_chunks) return -1;
    return 0;
}

typedef struct {
    uint16_t tail_mask[UFR_PAPER_NV];
    /* Physical starter storage is int16_t; logical PaperView samples remain
     * identical int32 values because the fanout range is safely within int16. */
    alignas(64) int16_t starter[UFR_PAPER_SAMPLES];
} ufr_paper_starter_t;

static const uint32_t UFR_PAPER_TAIL_PERM[4][16] = {
    {8,0,9,1,10,2,11,3,12,4,13,5,14,6,15,7},
    {0,8,1,9,2,10,3,11,4,12,5,13,6,14,7,15},
    {15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0},
    {4,5,6,7,0,1,2,3,12,13,14,15,8,9,10,11}
};

/* Pre-expanded 16-bit permutation indices: eliminates per-tile cvte/add/insert.
 * The values are exactly [idx0..idx15, idx0+16..idx15+16]. */
static const _Alignas(64) uint16_t UFR_PAPER_A64_INDEX16X2[64][32] = {
    {9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 11, 0, 5, 10, 15, 4, 25, 30, 19, 24, 29, 18, 23, 28, 17, 22, 27, 16, 21, 26, 31, 20},
    {14, 1, 4, 7, 10, 13, 0, 3, 6, 9, 12, 15, 2, 5, 8, 11, 30, 17, 20, 23, 26, 29, 16, 19, 22, 25, 28, 31, 18, 21, 24, 27},
    {11, 8, 5, 2, 15, 12, 9, 6, 3, 0, 13, 10, 7, 4, 1, 14, 27, 24, 21, 18, 31, 28, 25, 22, 19, 16, 29, 26, 23, 20, 17, 30},
    {4, 7, 10, 13, 0, 3, 6, 9, 12, 15, 2, 5, 8, 11, 14, 1, 20, 23, 26, 29, 16, 19, 22, 25, 28, 31, 18, 21, 24, 27, 30, 17},
    {9, 6, 3, 0, 13, 10, 7, 4, 1, 14, 11, 8, 5, 2, 15, 12, 25, 22, 19, 16, 29, 26, 23, 20, 17, 30, 27, 24, 21, 18, 31, 28},
    {6, 3, 0, 13, 10, 7, 4, 1, 14, 11, 8, 5, 2, 15, 12, 9, 22, 19, 16, 29, 26, 23, 20, 17, 30, 27, 24, 21, 18, 31, 28, 25},
    {12, 15, 2, 5, 8, 11, 14, 1, 4, 7, 10, 13, 0, 3, 6, 9, 28, 31, 18, 21, 24, 27, 30, 17, 20, 23, 26, 29, 16, 19, 22, 25},
    {1, 4, 7, 10, 13, 0, 3, 6, 9, 12, 15, 2, 5, 8, 11, 14, 17, 20, 23, 26, 29, 16, 19, 22, 25, 28, 31, 18, 21, 24, 27, 30},
    {1, 14, 11, 8, 5, 2, 15, 12, 9, 6, 3, 0, 13, 10, 7, 4, 17, 30, 27, 24, 21, 18, 31, 28, 25, 22, 19, 16, 29, 26, 23, 20},
    {5, 2, 15, 12, 9, 6, 3, 0, 13, 10, 7, 4, 1, 14, 11, 8, 21, 18, 31, 28, 25, 22, 19, 16, 29, 26, 23, 20, 17, 30, 27, 24},
    {10, 7, 4, 1, 14, 11, 8, 5, 2, 15, 12, 9, 6, 3, 0, 13, 26, 23, 20, 17, 30, 27, 24, 21, 18, 31, 28, 25, 22, 19, 16, 29},
    {3, 0, 13, 10, 7, 4, 1, 14, 11, 8, 5, 2, 15, 12, 9, 6, 19, 16, 29, 26, 23, 20, 17, 30, 27, 24, 21, 18, 31, 28, 25, 22},
    {5, 8, 11, 14, 1, 4, 7, 10, 13, 0, 3, 6, 9, 12, 15, 2, 21, 24, 27, 30, 17, 20, 23, 26, 29, 16, 19, 22, 25, 28, 31, 18},
    {10, 13, 0, 3, 6, 9, 12, 15, 2, 5, 8, 11, 14, 1, 4, 7, 26, 29, 16, 19, 22, 25, 28, 31, 18, 21, 24, 27, 30, 17, 20, 23},
    {2, 5, 8, 11, 14, 1, 4, 7, 10, 13, 0, 3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 17, 20, 23, 26, 29, 16, 19, 22, 25, 28, 31},
    {6, 9, 12, 15, 2, 5, 8, 11, 14, 1, 4, 7, 10, 13, 0, 3, 22, 25, 28, 31, 18, 21, 24, 27, 30, 17, 20, 23, 26, 29, 16, 19},
    {9, 12, 15, 2, 5, 8, 11, 14, 1, 4, 7, 10, 13, 0, 3, 6, 25, 28, 31, 18, 21, 24, 27, 30, 17, 20, 23, 26, 29, 16, 19, 22},
    {2, 15, 12, 9, 6, 3, 0, 13, 10, 7, 4, 1, 14, 11, 8, 5, 18, 31, 28, 25, 22, 19, 16, 29, 26, 23, 20, 17, 30, 27, 24, 21},
    {13, 0, 3, 6, 9, 12, 15, 2, 5, 8, 11, 14, 1, 4, 7, 10, 29, 16, 19, 22, 25, 28, 31, 18, 21, 24, 27, 30, 17, 20, 23, 26},
    {7, 4, 1, 14, 11, 8, 5, 2, 15, 12, 9, 6, 3, 0, 13, 10, 23, 20, 17, 30, 27, 24, 21, 18, 31, 28, 25, 22, 19, 16, 29, 26},
    {0, 3, 6, 9, 12, 15, 2, 5, 8, 11, 14, 1, 4, 7, 10, 13, 16, 19, 22, 25, 28, 31, 18, 21, 24, 27, 30, 17, 20, 23, 26, 29},
    {8, 11, 14, 1, 4, 7, 10, 13, 0, 3, 6, 9, 12, 15, 2, 5, 24, 27, 30, 17, 20, 23, 26, 29, 16, 19, 22, 25, 28, 31, 18, 21},
    {4, 15, 10, 5, 0, 11, 6, 1, 12, 7, 2, 13, 8, 3, 14, 9, 20, 31, 26, 21, 16, 27, 22, 17, 28, 23, 18, 29, 24, 19, 30, 25},
    {13, 10, 7, 4, 1, 14, 11, 8, 5, 2, 15, 12, 9, 6, 3, 0, 29, 26, 23, 20, 17, 30, 27, 24, 21, 18, 31, 28, 25, 22, 19, 16},
    {7, 12, 1, 6, 11, 0, 5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 23, 28, 17, 22, 27, 16, 21, 26, 31, 20, 25, 30, 19, 24, 29, 18},
    {1, 6, 11, 0, 5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 17, 22, 27, 16, 21, 26, 31, 20, 25, 30, 19, 24, 29, 18, 23, 28},
    {14, 11, 8, 5, 2, 15, 12, 9, 6, 3, 0, 13, 10, 7, 4, 1, 30, 27, 24, 21, 18, 31, 28, 25, 22, 19, 16, 29, 26, 23, 20, 17},
    {15, 12, 9, 6, 3, 0, 13, 10, 7, 4, 1, 14, 11, 8, 5, 2, 31, 28, 25, 22, 19, 16, 29, 26, 23, 20, 17, 30, 27, 24, 21, 18},
    {12, 7, 2, 13, 8, 3, 14, 9, 4, 15, 10, 5, 0, 11, 6, 1, 28, 23, 18, 29, 24, 19, 30, 25, 20, 31, 26, 21, 16, 27, 22, 17},
    {2, 7, 12, 1, 6, 11, 0, 5, 10, 15, 4, 9, 14, 3, 8, 13, 18, 23, 28, 17, 22, 27, 16, 21, 26, 31, 20, 25, 30, 19, 24, 29},
    {0, 11, 6, 1, 12, 7, 2, 13, 8, 3, 14, 9, 4, 15, 10, 5, 16, 27, 22, 17, 28, 23, 18, 29, 24, 19, 30, 25, 20, 31, 26, 21},
    {15, 10, 5, 0, 11, 6, 1, 12, 7, 2, 13, 8, 3, 14, 9, 4, 31, 26, 21, 16, 27, 22, 17, 28, 23, 18, 29, 24, 19, 30, 25, 20},
    {10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 11, 0, 5, 26, 31, 20, 25, 30, 19, 24, 29, 18, 23, 28, 17, 22, 27, 16, 21},
    {5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 11, 0, 21, 26, 31, 20, 25, 30, 19, 24, 29, 18, 23, 28, 17, 22, 27, 16},
    {4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 11, 0, 5, 10, 15, 20, 25, 30, 19, 24, 29, 18, 23, 28, 17, 22, 27, 16, 21, 26, 31},
    {1, 12, 7, 2, 13, 8, 3, 14, 9, 4, 15, 10, 5, 0, 11, 6, 17, 28, 23, 18, 29, 24, 19, 30, 25, 20, 31, 26, 21, 16, 27, 22},
    {8, 3, 14, 9, 4, 15, 10, 5, 0, 11, 6, 1, 12, 7, 2, 13, 24, 19, 30, 25, 20, 31, 26, 21, 16, 27, 22, 17, 28, 23, 18, 29},
    {11, 0, 5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 27, 16, 21, 26, 31, 20, 25, 30, 19, 24, 29, 18, 23, 28, 17, 22},
    {12, 1, 6, 11, 0, 5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 28, 17, 22, 27, 16, 21, 26, 31, 20, 25, 30, 19, 24, 29, 18, 23},
    {14, 9, 4, 15, 10, 5, 0, 11, 6, 1, 12, 7, 2, 13, 8, 3, 30, 25, 20, 31, 26, 21, 16, 27, 22, 17, 28, 23, 18, 29, 24, 19},
    {0, 5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 11, 16, 21, 26, 31, 20, 25, 30, 19, 24, 29, 18, 23, 28, 17, 22, 27},
    {2, 13, 8, 3, 14, 9, 4, 15, 10, 5, 0, 11, 6, 1, 12, 7, 18, 29, 24, 19, 30, 25, 20, 31, 26, 21, 16, 27, 22, 17, 28, 23},
    {7, 2, 13, 8, 3, 14, 9, 4, 15, 10, 5, 0, 11, 6, 1, 12, 23, 18, 29, 24, 19, 30, 25, 20, 31, 26, 21, 16, 27, 22, 17, 28},
    {3, 8, 13, 2, 7, 12, 1, 6, 11, 0, 5, 10, 15, 4, 9, 14, 19, 24, 29, 18, 23, 28, 17, 22, 27, 16, 21, 26, 31, 20, 25, 30},
    {11, 6, 1, 12, 7, 2, 13, 8, 3, 14, 9, 4, 15, 10, 5, 0, 27, 22, 17, 28, 23, 18, 29, 24, 19, 30, 25, 20, 31, 26, 21, 16},
    {6, 1, 12, 7, 2, 13, 8, 3, 14, 9, 4, 15, 10, 5, 0, 11, 22, 17, 28, 23, 18, 29, 24, 19, 30, 25, 20, 31, 26, 21, 16, 27},
    {13, 8, 3, 14, 9, 4, 15, 10, 5, 0, 11, 6, 1, 12, 7, 2, 29, 24, 19, 30, 25, 20, 31, 26, 21, 16, 27, 22, 17, 28, 23, 18},
    {6, 11, 0, 5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 22, 27, 16, 21, 26, 31, 20, 25, 30, 19, 24, 29, 18, 23, 28, 17},
    {9, 4, 15, 10, 5, 0, 11, 6, 1, 12, 7, 2, 13, 8, 3, 14, 25, 20, 31, 26, 21, 16, 27, 22, 17, 28, 23, 18, 29, 24, 19, 30},
    {13, 2, 7, 12, 1, 6, 11, 0, 5, 10, 15, 4, 9, 14, 3, 8, 29, 18, 23, 28, 17, 22, 27, 16, 21, 26, 31, 20, 25, 30, 19, 24},
    {10, 5, 0, 11, 6, 1, 12, 7, 2, 13, 8, 3, 14, 9, 4, 15, 26, 21, 16, 27, 22, 17, 28, 23, 18, 29, 24, 19, 30, 25, 20, 31},
    {5, 0, 11, 6, 1, 12, 7, 2, 13, 8, 3, 14, 9, 4, 15, 10, 21, 16, 27, 22, 17, 28, 23, 18, 29, 24, 19, 30, 25, 20, 31, 26},
    {15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 11, 0, 5, 10, 31, 20, 25, 30, 19, 24, 29, 18, 23, 28, 17, 22, 27, 16, 21, 26},
    {8, 13, 2, 7, 12, 1, 6, 11, 0, 5, 10, 15, 4, 9, 14, 3, 24, 29, 18, 23, 28, 17, 22, 27, 16, 21, 26, 31, 20, 25, 30, 19},
    {14, 3, 8, 13, 2, 7, 12, 1, 6, 11, 0, 5, 10, 15, 4, 9, 30, 19, 24, 29, 18, 23, 28, 17, 22, 27, 16, 21, 26, 31, 20, 25},
    {3, 14, 9, 4, 15, 10, 5, 0, 11, 6, 1, 12, 7, 2, 13, 8, 19, 30, 25, 20, 31, 26, 21, 16, 27, 22, 17, 28, 23, 18, 29, 24},
    {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16},
    {3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 19, 18, 17, 16, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20},
    {11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 31, 30, 29, 28},
    {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 16, 17, 18, 19},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31},
    {8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7, 24, 25, 26, 27, 28, 29, 30, 31, 16, 17, 18, 19, 20, 21, 22, 23},
    {12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 28, 29, 30, 31, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27},
    {7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8, 23, 22, 21, 20, 19, 18, 17, 16, 31, 30, 29, 28, 27, 26, 25, 24},
};

static const _Alignas(64) uint16_t UFR_PAPER_TAIL16X2[4][32] = {
    {8, 0, 9, 1, 10, 2, 11, 3, 12, 4, 13, 5, 14, 6, 15, 7, 24, 16, 25, 17, 26, 18, 27, 19, 28, 20, 29, 21, 30, 22, 31, 23},
    {0, 8, 1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15, 16, 24, 17, 25, 18, 26, 19, 27, 20, 28, 21, 29, 22, 30, 23, 31},
    {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16},
    {4, 5, 6, 7, 0, 1, 2, 3, 12, 13, 14, 15, 8, 9, 10, 11, 20, 21, 22, 23, 16, 17, 18, 19, 28, 29, 30, 31, 24, 25, 26, 27},
};


static inline __m512i ufr_paper_pidx(unsigned id) {
    return _mm512_loadu_si512((const void *)UFR_PAPER_A64_INDEX[id & 63u]);
}
static inline __m512i ufr_paper_tidx(unsigned id) {
    return _mm512_loadu_si512((const void *)UFR_PAPER_TAIL_PERM[id & 3u]);
}
static inline uint16_t ufr_paper_tailmask_vec(__m512i x) {
    const int32_t tq = (int32_t)(3.8 / (UF_NORMAL_SCALE * 0.7071067811865475244f));
    const __m512i hi = _mm512_set1_epi32(tq);
    const __m512i lo = _mm512_set1_epi32(-tq);
    return (uint16_t)(_mm512_cmp_epi32_mask(x, hi, _MM_CMPINT_GT) |
                       _mm512_cmp_epi32_mask(x, lo, _MM_CMPINT_LT));
}

static __attribute__((noinline)) void ufr_paper_make_starter(UFRFastFrontMem *fm, ufr_paper_starter_t *s) {
    if (!fm || !s) return;
    ufr_fast_front_normal_group16(fm, s->starter);
    for (unsigned q = 0; q < UFR_PAPER_NV; ++q) {
        const __m256i q16 = _mm256_load_si256((const __m256i *)(s->starter + (size_t)q * UFR_PAPER_V));
        const __m512i x = _mm512_cvtepi16_epi32(q16);
        s->tail_mask[q] = ufr_paper_tailmask_vec(x) ? 1u : 0u;
    }
}

/*
 * PaperView V2 cultivation selector.
 *
 * The Normal starter is intentionally generated only at a generation boundary.
 * Each MC chunk then receives a cheap, deterministic, reversible re-indexing:
 *   - the low 10 epoch bits are bit-reversed to rotate the 1024 Normal rows;
 *   - the next 6 bits select a different A64 lane permutation family;
 *   - view_id is folded into that family without multiplication.
 *
 * This does NOT change the marginal Normal values: it only permutes/selects
 * already-normalized int32 samples.  It therefore preserves the Normal
 * histogram while preventing the exact same chunk byte sequence from being
 * replayed for every long-generation chunk.
 *
 * The epoch is the monotonically increasing engine chunk_id, so it does not
 * reset when a seed generation is extended.  No extra state buffer is added.
 */
static inline uint32_t ufr_paper_cultivation_shift(uint64_t epoch)
{
    /* Exact 10-bit bit-reversal. Adjacent epochs therefore jump to widely
     * separated rows instead of walking neighboring rows one-by-one. */
    uint32_t x = (uint32_t)epoch & (UFR_PAPER_NV - 1u);
    x = ((x & 0x5555u) << 1) | ((x & 0xAAAAu) >> 1);
    x = ((x & 0x0F0Fu) << 4) | ((x & 0xF0F0u) >> 4);
    x = ((x & 0x3333u) << 2) | ((x & 0xCCCCu) >> 2);
    x = ((x & 0x00FFu) << 8) | ((x & 0xFF00u) >> 8);
    return (x >> 6) & (UFR_PAPER_NV - 1u);
}

static inline unsigned ufr_paper_cultivation_profile(uint64_t epoch)
{
    return (unsigned)((epoch >> 10) & 63u);
}

typedef struct {
    unsigned shift;
    unsigned phase;
} ufr_paper_cultivation_ctx_t;

static inline ufr_paper_cultivation_ctx_t ufr_paper_cultivation_ctx(uint64_t epoch)
{
    ufr_paper_cultivation_ctx_t c;
    c.shift = ufr_paper_cultivation_shift(epoch);
    c.phase = ufr_paper_cultivation_profile(epoch);
    return c;
}

static inline void ufr_paper_materialize_block_ctx(const ufr_paper_starter_t *s,
                                                    unsigned base,
                                                    __m512i ci,
                                                    __m512i ti,
                                                    unsigned block_id,
                                                    int32_t *out256) {
    const unsigned out0 = block_id * UFR_PAPER_V;
    for (unsigned j = 0; j < UFR_PAPER_V; ++j) {
        const unsigned src = (out0 + j + base) & (UFR_PAPER_NV - 1u);
        const __m256i q16 = _mm256_load_si256((const __m256i *)(s->starter + (size_t)src * UFR_PAPER_V));
        __m512i x = _mm512_cvtepi16_epi32(q16);
        x = _mm512_permutexvar_epi32(s->tail_mask[src] ? ti : ci, x);
        _mm512_store_si512((void *)(out256 + (size_t)j * UFR_PAPER_V), x);
    }
}

static inline void ufr_paper_materialize_block(const ufr_paper_starter_t *s,
                                                unsigned view_id,
                                                unsigned block_id,
                                                uint64_t cultivation_epoch,
                                                int32_t *out256) {
    const ufr_paper_cultivation_ctx_t c = ufr_paper_cultivation_ctx(cultivation_epoch);
    const unsigned raw_view = view_id % UFR_PAPER_MAX_VIEWS;
    const unsigned vid = (raw_view + c.phase + (c.phase >> 3)) & (UFR_PAPER_MAX_VIEWS - 1u);
    const unsigned base = (UFR_PAPER_A64_OFFSET[vid] + c.shift) &
                          (UFR_PAPER_NV - 1u);
    const __m512i ci = ufr_paper_pidx(UFR_PAPER_A64_LANE[vid]);
    const __m512i ti = ufr_paper_tidx(UFR_PAPER_A64_TAIL[vid]);
    ufr_paper_materialize_block_ctx(s, base, ci, ti, block_id, out256);
}

static __attribute__((noinline)) int ufr_paper_materialize_view(const ufr_paper_starter_t *s,
                                                                  unsigned view_id,
                                                                  uint64_t cultivation_epoch,
                                                                  int32_t *dst_view) {
    if (!s || !dst_view) return -1;
    const ufr_paper_cultivation_ctx_t c = ufr_paper_cultivation_ctx(cultivation_epoch);
    const unsigned raw_view = view_id % UFR_PAPER_MAX_VIEWS;
    const unsigned vid = (raw_view + c.phase + (c.phase >> 3)) & (UFR_PAPER_MAX_VIEWS - 1u);
    const unsigned base = (UFR_PAPER_A64_OFFSET[vid] + c.shift) &
                          (UFR_PAPER_NV - 1u);
    const __m512i ci = ufr_paper_pidx(UFR_PAPER_A64_LANE[vid]);
    const __m512i ti = ufr_paper_tidx(UFR_PAPER_A64_TAIL[vid]);
    for (unsigned b = 0; b < UFR_PAPER_MC_BLOCKS_PER_VIEW; ++b)
        ufr_paper_materialize_block_ctx(s, base, ci, ti, b,
                                        dst_view + (size_t)b * 256u);
    return 0;
}

typedef struct {
    uint64_t generation_id;
    uint64_t chunk_id;
    uint32_t view_id;
    uint32_t blocks;
    uint64_t samples;
    uint64_t hits;
    double payoff_sum;
} ufr_mc_chunk_result_t;

typedef int (*ufr_mc_qblock_consumer_fn)(const int32_t *q256,
                                         uint64_t generation_id,
                                         uint32_t view_id,
                                         uint32_t block_id,
                                         void *ctx);

typedef struct {
    __m512 acc;
    uint64_t hits;
    uint64_t blocks;
    uint64_t samples;
} ufr_representative_mc_t;

static inline void ufr_representative_mc_init(ufr_representative_mc_t *m) {
    if (!m) return;
    m->acc = _mm512_setzero_ps();
    m->hits = 0; m->blocks = 0; m->samples = 0;
}

static int ufr_representative_mc_consume(const int32_t *q256,
                                         uint64_t generation_id,
                                         uint32_t view_id,
                                         uint32_t block_id,
                                         void *ctx) {
    (void)generation_id; (void)view_id; (void)block_id;
    ufr_representative_mc_t *m = (ufr_representative_mc_t *)ctx;
    if (!m || !q256) return -1;
    const __m512 scale = _mm512_set1_ps(UF_NORMAL_SCALE * 0.7071067811865475244f);
    const __m512 scale2 = _mm512_mul_ps(scale, scale);
    m->hits += mc_accumulate_q64(q256, &m->acc, scale2);
    ++m->blocks;
    m->samples += 256u;
    return 0;
}

static inline void ufr_representative_mc_finalize(ufr_representative_mc_t *m,
                                                   ufr_mc_chunk_result_t *out) {
    if (!m || !out) return;
    alignas(64) float tmp[16];
    _mm512_store_ps(tmp, m->acc);
    double sum = 0.0;
    for (unsigned i = 0; i < 16; ++i) sum += (double)tmp[i];
    memset(out, 0, sizeof(*out));
    out->blocks = (uint32_t)m->blocks;
    out->samples = m->samples;
    out->hits = m->hits;
    out->payoff_sum = sum;
}

typedef struct ufr_mc_cultivation_engine {
    UFRXSeedExchange seedx;
    UFRFastFrontMem fm;
    _Atomic int stop;
    ufr_mc_config_t requested;
    ufr_mc_config_resolved_t cfg;
    uint64_t generation_id;
    uint64_t chunks_in_generation;
    uint64_t chunk_id;
    uint8_t seedback_started;
    uint8_t initialized;
    uint8_t reserved[6];
    ufr_paper_starter_t retained_starter;
    /* Final-cultivation profile cache. RAW_INT16 / INT32 / FLOAT32 cache the
     * already-PaperView-transformed rows; R2_FLOAT32 keeps the fixed pair-parity
     * r2 fusion cache.  These caches are built outside the hot MC loop and are
     * keyed only by view-count and cultivation phase; the epoch shift remains
     * a cheap runtime row rotation. */
    void *paper_final_cache;
    size_t paper_final_cache_bytes;
    uint32_t paper_final_cache_views;
    uint32_t paper_final_cache_phase;
    uint64_t paper_final_cache_epoch;
    uint8_t paper_final_cache_profile;
    uint32_t paper_final_cache_custom_id;
    uint8_t paper_final_cache_valid;
    uint8_t paper_final_cache_reserved[2];
    /* Fixed representative-MC r2 fusion cache for the R2_FLOAT32
     * profile. Each view stores two pair-parity banks of precomputed r2 vectors. */
    float *paper_r2_cache;
    size_t paper_r2_cache_bytes;
    uint32_t paper_r2_cache_views;
    uint32_t paper_r2_cache_phase;
    uint8_t paper_r2_cache_valid;
    uint8_t paper_r2_cache_reserved[3];
    int32_t *cache_buf;
    size_t cache_bytes;
    int32_t *scratch_block;
    UFRPostBridge post;
    /* Enterprise extensions: opt-in deterministic stream binding and exact replay. */
    ufr_mc_stream_plan_t stream_plan;
    uint8_t stream_id[UFR_MC_STREAM_ID_BYTES];
    uint8_t stream_bound;
    uint8_t replay_mode;
    uint8_t enterprise_reserved[6];
} ufr_mc_cultivation_engine_t;


static size_t ufr_final_cache_bytes_per_view(ufr_final_cultivation_profile_t profile,
                                               uint32_t custom_profile_id)
{
    switch (profile) {
        case UFR_FINAL_RAW_INT16: return (size_t)UFR_PAPER_SAMPLES * sizeof(int16_t);
        case UFR_FINAL_INT32:     return (size_t)UFR_PAPER_SAMPLES * sizeof(int32_t);
        case UFR_FINAL_FLOAT32:   return (size_t)UFR_PAPER_SAMPLES * sizeof(float);
        case UFR_FINAL_CUSTOM: {
            const ufr_final_profile_desc_t *d = ufr_final_profile_lookup(custom_profile_id);
            return d ? (size_t)UFR_PAPER_NV * d->bytes_per_row : 0u;
        }
        default: return 0u;
    }
}
static void ufr_paper_final_cache_free(ufr_mc_cultivation_engine_t *e)
{
    if (!e) return;
    free(e->paper_final_cache);
    e->paper_final_cache = NULL;
    e->paper_final_cache_bytes = 0u;
    e->paper_final_cache_views = 0u;
    e->paper_final_cache_phase = 0u;
    e->paper_final_cache_epoch = 0u;
    e->paper_final_cache_profile = UFR_FINAL_AUTO;
    e->paper_final_cache_custom_id = 0u;
    e->paper_final_cache_valid = 0u;
}

static __attribute__((noinline)) void ufr_paper_final_cache_build4_raw16_tile(
    const ufr_paper_starter_t *s,
    uint8_t *cache,
    size_t bytes_per_view,
    uint32_t view_base,
    uint32_t phase)
{
    const unsigned vo = phase + (phase >> 3);
    const unsigned vid0 = ((view_base + 0u) + vo) & (UFR_PAPER_MAX_VIEWS - 1u);
    const unsigned vid1 = ((view_base + 1u) + vo) & (UFR_PAPER_MAX_VIEWS - 1u);
    const unsigned vid2 = ((view_base + 2u) + vo) & (UFR_PAPER_MAX_VIEWS - 1u);
    const unsigned vid3 = ((view_base + 3u) + vo) & (UFR_PAPER_MAX_VIEWS - 1u);
    const uint8_t i0 = UFR_PAPER_A64_LANE[vid0];
    const uint8_t i1 = UFR_PAPER_A64_LANE[vid1];
    const uint8_t i2 = UFR_PAPER_A64_LANE[vid2];
    const uint8_t i3 = UFR_PAPER_A64_LANE[vid3];
    const uint8_t t0 = UFR_PAPER_A64_TAIL[vid0];
    const uint8_t t1 = UFR_PAPER_A64_TAIL[vid1];
    const uint8_t t2 = UFR_PAPER_A64_TAIL[vid2];
    const uint8_t t3 = UFR_PAPER_A64_TAIL[vid3];
    /* Common path uses only the full 512-bit VPERMW indices. The rare tail
     * fallback derives its 256-bit indices from these already-loaded ZMMs,
     * avoiding eight redundant 256-bit index loads per four-view tile. */
    const __m512i ci02 = _mm512_load_si512((const void *)UFR_PAPER_A64_INDEX16X2[i0]);
    const __m512i ci12 = _mm512_load_si512((const void *)UFR_PAPER_A64_INDEX16X2[i1]);
    const __m512i ci22 = _mm512_load_si512((const void *)UFR_PAPER_A64_INDEX16X2[i2]);
    const __m512i ci32 = _mm512_load_si512((const void *)UFR_PAPER_A64_INDEX16X2[i3]);
    const __m512i ti02 = _mm512_load_si512((const void *)UFR_PAPER_TAIL16X2[t0]);
    const __m512i ti12 = _mm512_load_si512((const void *)UFR_PAPER_TAIL16X2[t1]);
    const __m512i ti22 = _mm512_load_si512((const void *)UFR_PAPER_TAIL16X2[t2]);
    const __m512i ti32 = _mm512_load_si512((const void *)UFR_PAPER_TAIL16X2[t3]);
    uint8_t *d0 = cache + (size_t)(view_base + 0u) * bytes_per_view;
    uint8_t *d1 = cache + (size_t)(view_base + 1u) * bytes_per_view;
    uint8_t *d2 = cache + (size_t)(view_base + 2u) * bytes_per_view;
    uint8_t *d3 = cache + (size_t)(view_base + 3u) * bytes_per_view;
    for (unsigned row = 0; row < UFR_PAPER_NV; row += 2u) {
        const unsigned src0 = (unsigned)(((uint64_t)UFR_PAPER_CACHE_STRIDE * row) & (UFR_PAPER_NV - 1u));
        const unsigned src1 = (unsigned)(((uint64_t)UFR_PAPER_CACHE_STRIDE * (row + 1u)) & (UFR_PAPER_NV - 1u));
        const __m256i q0s = _mm256_load_si256(
            (const __m256i *)(s->starter + (size_t)src0 * UFR_PAPER_V));
        const __m256i q1s = _mm256_load_si256(
            (const __m256i *)(s->starter + (size_t)src1 * UFR_PAPER_V));
        const __m512i qpair = _mm512_inserti64x4(_mm512_castsi256_si512(q0s), q1s, 1);
        const int tail0 = s->tail_mask[src0] != 0u;
        const int tail1 = s->tail_mask[src1] != 0u;
        if (!tail0 && !tail1) {
            _mm512_store_si512((void *)(d0 + (size_t)row * 32u), _mm512_permutexvar_epi16(ci02, qpair));
            _mm512_store_si512((void *)(d1 + (size_t)row * 32u), _mm512_permutexvar_epi16(ci12, qpair));
            _mm512_store_si512((void *)(d2 + (size_t)row * 32u), _mm512_permutexvar_epi16(ci22, qpair));
            _mm512_store_si512((void *)(d3 + (size_t)row * 32u), _mm512_permutexvar_epi16(ci32, qpair));
        } else {
            const __m256i q0 = _mm512_castsi512_si256(qpair);
            const __m256i q1 = _mm512_extracti64x4_epi64(qpair, 1);
            const __m256i ci0 = _mm512_castsi512_si256(ci02);
            const __m256i ci1 = _mm512_castsi512_si256(ci12);
            const __m256i ci2 = _mm512_castsi512_si256(ci22);
            const __m256i ci3 = _mm512_castsi512_si256(ci32);
            const __m256i ti0 = _mm512_castsi512_si256(ti02);
            const __m256i ti1 = _mm512_castsi512_si256(ti12);
            const __m256i ti2 = _mm512_castsi512_si256(ti22);
            const __m256i ti3 = _mm512_castsi512_si256(ti32);
#define FB(D,CI,TI) do { \
                const __m256i p0 = _mm256_permutexvar_epi16(tail0 ? (TI) : (CI), q0); \
                const __m256i p1 = _mm256_permutexvar_epi16(tail1 ? (TI) : (CI), q1); \
                _mm256_store_si256((__m256i *)((D) + (size_t)row * 32u), p0); \
                _mm256_store_si256((__m256i *)((D) + (size_t)(row + 1u) * 32u), p1); \
            } while (0)
            FB(d0,ci0,ti0); FB(d1,ci1,ti1); FB(d2,ci2,ti2); FB(d3,ci3,ti3);
#undef FB
        }
    }
}


static int ufr_paper_final_cache_build(ufr_mc_cultivation_engine_t *e,
                                       uint32_t view_count,
                                       uint32_t phase,
                                       uint64_t epoch,
                                       ufr_final_cultivation_profile_t profile)
{
    if (!e || view_count == 0u || view_count > UFR_PAPER_MAX_VIEWS) return -1;
    const size_t bytes_per_view = ufr_final_cache_bytes_per_view(profile, e->cfg.custom_profile_id);
    if (bytes_per_view == 0u) return -1;
    const ufr_final_profile_desc_t *custom_desc =
        (profile == UFR_FINAL_CUSTOM) ? ufr_final_profile_lookup(e->cfg.custom_profile_id) : NULL;
    if (profile == UFR_FINAL_CUSTOM && !custom_desc) return -1;
    const size_t bytes = (size_t)view_count * bytes_per_view;

    if (!e->paper_final_cache || e->paper_final_cache_bytes != bytes ||
        e->paper_final_cache_profile != (uint8_t)profile ||
        e->paper_final_cache_custom_id != e->cfg.custom_profile_id) {
        ufr_paper_final_cache_free(e);
        if (posix_memalign(&e->paper_final_cache, 64u, bytes) != 0)
            return -1;
        e->paper_final_cache_bytes = bytes;
    }

    /* RAW16: exactly-four-view path keeps the fully inline V1 kernel.
     * For 8/16/32 views, process four-view tiles without repeating the
     * generic 32-bit path. All other profiles retain the established baseline path. */
    if (view_count == 4u && profile == UFR_FINAL_RAW_INT16) {

        /* Two adjacent 16-lane rows form one 32-lane int16 ZMM. For the
         * overwhelmingly common non-tail pair, one 512-bit VPERMW handles
         * both rows for one view. The rare tail-containing pair falls back to
         * the exact 256-bit per-row path. */
        const unsigned vo = phase + (phase >> 3);
        const unsigned vid0 = (0u + vo) & (UFR_PAPER_MAX_VIEWS - 1u);
        const unsigned vid1 = (1u + vo) & (UFR_PAPER_MAX_VIEWS - 1u);
        const unsigned vid2 = (2u + vo) & (UFR_PAPER_MAX_VIEWS - 1u);
        const unsigned vid3 = (3u + vo) & (UFR_PAPER_MAX_VIEWS - 1u);
        const __m512i ci02 = _mm512_load_si512((const void *)UFR_PAPER_A64_INDEX16X2[UFR_PAPER_A64_LANE[vid0]]);
        const __m512i ci12 = _mm512_load_si512((const void *)UFR_PAPER_A64_INDEX16X2[UFR_PAPER_A64_LANE[vid1]]);
        const __m512i ci22 = _mm512_load_si512((const void *)UFR_PAPER_A64_INDEX16X2[UFR_PAPER_A64_LANE[vid2]]);
        const __m512i ci32 = _mm512_load_si512((const void *)UFR_PAPER_A64_INDEX16X2[UFR_PAPER_A64_LANE[vid3]]);
        const __m512i ti02 = _mm512_load_si512((const void *)UFR_PAPER_TAIL16X2[UFR_PAPER_A64_TAIL[vid0]]);
        const __m512i ti12 = _mm512_load_si512((const void *)UFR_PAPER_TAIL16X2[UFR_PAPER_A64_TAIL[vid1]]);
        const __m512i ti22 = _mm512_load_si512((const void *)UFR_PAPER_TAIL16X2[UFR_PAPER_A64_TAIL[vid2]]);
        const __m512i ti32 = _mm512_load_si512((const void *)UFR_PAPER_TAIL16X2[UFR_PAPER_A64_TAIL[vid3]]);
        uint8_t *d0 = (uint8_t *)e->paper_final_cache + 0u * bytes_per_view;
        uint8_t *d1 = (uint8_t *)e->paper_final_cache + 1u * bytes_per_view;
        uint8_t *d2 = (uint8_t *)e->paper_final_cache + 2u * bytes_per_view;
        uint8_t *d3 = (uint8_t *)e->paper_final_cache + 3u * bytes_per_view;
        for (unsigned row = 0; row < UFR_PAPER_NV; row += 2u) {
            const unsigned src0 = (unsigned)(((uint64_t)UFR_PAPER_CACHE_STRIDE * row) & (UFR_PAPER_NV - 1u));
            const unsigned src1 = (unsigned)(((uint64_t)UFR_PAPER_CACHE_STRIDE * (row + 1u)) & (UFR_PAPER_NV - 1u));
            const __m256i q0s = _mm256_load_si256(
                (const __m256i *)(e->retained_starter.starter + (size_t)src0 * UFR_PAPER_V));
            const __m256i q1s = _mm256_load_si256(
                (const __m256i *)(e->retained_starter.starter + (size_t)src1 * UFR_PAPER_V));
            const __m512i qpair = _mm512_inserti64x4(_mm512_castsi256_si512(q0s), q1s, 1);
            const int tail0 = e->retained_starter.tail_mask[src0] != 0u;
            const int tail1 = e->retained_starter.tail_mask[src1] != 0u;
            if (!tail0 && !tail1) {
                _mm512_store_si512((void *)(d0 + (size_t)row * 32u), _mm512_permutexvar_epi16(ci02, qpair));
                _mm512_store_si512((void *)(d1 + (size_t)row * 32u), _mm512_permutexvar_epi16(ci12, qpair));
                _mm512_store_si512((void *)(d2 + (size_t)row * 32u), _mm512_permutexvar_epi16(ci22, qpair));
                _mm512_store_si512((void *)(d3 + (size_t)row * 32u), _mm512_permutexvar_epi16(ci32, qpair));
            } else {
                const __m256i q0 = _mm512_castsi512_si256(qpair);
                const __m256i q1 = _mm512_extracti64x4_epi64(qpair, 1);
                const __m256i ci0 = _mm512_castsi512_si256(ci02);
                const __m256i ci1 = _mm512_castsi512_si256(ci12);
                const __m256i ci2 = _mm512_castsi512_si256(ci22);
                const __m256i ci3 = _mm512_castsi512_si256(ci32);
                const __m256i ti0 = _mm512_castsi512_si256(ti02);
                const __m256i ti1 = _mm512_castsi512_si256(ti12);
                const __m256i ti2 = _mm512_castsi512_si256(ti22);
                const __m256i ti3 = _mm512_castsi512_si256(ti32);
#define FALLBACK_VIEW(D,CI,TI) do { \
                    const __m256i p0 = _mm256_permutexvar_epi16(tail0 ? (TI) : (CI), q0); \
                    const __m256i p1 = _mm256_permutexvar_epi16(tail1 ? (TI) : (CI), q1); \
                    _mm256_store_si256((__m256i *)((D) + (size_t)row * 32u), p0); \
                    _mm256_store_si256((__m256i *)((D) + (size_t)(row + 1u) * 32u), p1); \
                } while (0)
                FALLBACK_VIEW(d0,ci0,ti0);
                FALLBACK_VIEW(d1,ci1,ti1);
                FALLBACK_VIEW(d2,ci2,ti2);
                FALLBACK_VIEW(d3,ci3,ti3);
#undef FALLBACK_VIEW
            }
        }
    
    } else if (view_count >= 8u && (view_count % 4u) == 0u && profile == UFR_FINAL_RAW_INT16) {
        for (uint32_t vb = 0; vb < view_count; vb += 4u) {
            ufr_paper_final_cache_build4_raw16_tile(
                &e->retained_starter, (uint8_t *)e->paper_final_cache,
                bytes_per_view, vb, phase);
        }
    } else if (view_count == 4u && profile != UFR_FINAL_CUSTOM) {
        /* Built-in non-RAW profiles use the same 465-row physical permutation
         * as RAW16. Cache rows remain sequential for the MC consumer, so the
         * pair geometry changes without adding work to the consumer hot loop. */

        const unsigned vid0 = (0u + phase + (phase >> 3)) & (UFR_PAPER_MAX_VIEWS - 1u);
        const unsigned vid1 = (1u + phase + (phase >> 3)) & (UFR_PAPER_MAX_VIEWS - 1u);
        const unsigned vid2 = (2u + phase + (phase >> 3)) & (UFR_PAPER_MAX_VIEWS - 1u);
        const unsigned vid3 = (3u + phase + (phase >> 3)) & (UFR_PAPER_MAX_VIEWS - 1u);
        const __m512i ci0 = ufr_paper_pidx(UFR_PAPER_A64_LANE[vid0]);
        const __m512i ci1 = ufr_paper_pidx(UFR_PAPER_A64_LANE[vid1]);
        const __m512i ci2 = ufr_paper_pidx(UFR_PAPER_A64_LANE[vid2]);
        const __m512i ci3 = ufr_paper_pidx(UFR_PAPER_A64_LANE[vid3]);
        const __m512i ti0 = ufr_paper_tidx(UFR_PAPER_A64_TAIL[vid0]);
        const __m512i ti1 = ufr_paper_tidx(UFR_PAPER_A64_TAIL[vid1]);
        const __m512i ti2 = ufr_paper_tidx(UFR_PAPER_A64_TAIL[vid2]);
        const __m512i ti3 = ufr_paper_tidx(UFR_PAPER_A64_TAIL[vid3]);
        uint8_t *d0 = (uint8_t *)e->paper_final_cache + 0u * bytes_per_view;
        uint8_t *d1 = (uint8_t *)e->paper_final_cache + 1u * bytes_per_view;
        uint8_t *d2 = (uint8_t *)e->paper_final_cache + 2u * bytes_per_view;
        uint8_t *d3 = (uint8_t *)e->paper_final_cache + 3u * bytes_per_view;
        const unsigned phase_off = phase + (phase >> 3);
        (void)phase_off;
        for (unsigned row = 0; row < UFR_PAPER_NV; ++row) {
            const unsigned src_row = (profile == UFR_FINAL_RAW_INT16 ||
                                      profile == UFR_FINAL_INT32 ||
                                      profile == UFR_FINAL_FLOAT32)
                                     ? (unsigned)(((uint64_t)UFR_PAPER_CACHE_STRIDE * row) & (UFR_PAPER_NV - 1u))
                                     : row;
            const __m256i q16 = _mm256_load_si256(
                (const __m256i *)(e->retained_starter.starter + (size_t)src_row * UFR_PAPER_V));
            const __m512i q32 = _mm512_cvtepi16_epi32(q16);
            const int tail = e->retained_starter.tail_mask[src_row] != 0u;
            const __m512i q0 = _mm512_permutexvar_epi32(tail ? ti0 : ci0, q32);
            const __m512i q1 = _mm512_permutexvar_epi32(tail ? ti1 : ci1, q32);
            const __m512i q2 = _mm512_permutexvar_epi32(tail ? ti2 : ci2, q32);
            const __m512i q3 = _mm512_permutexvar_epi32(tail ? ti3 : ci3, q32);
            if (profile == UFR_FINAL_RAW_INT16) {
                _mm256_store_si256((__m256i *)(d0 + (size_t)row * 32u), _mm512_cvtepi32_epi16(q0));
                _mm256_store_si256((__m256i *)(d1 + (size_t)row * 32u), _mm512_cvtepi32_epi16(q1));
                _mm256_store_si256((__m256i *)(d2 + (size_t)row * 32u), _mm512_cvtepi32_epi16(q2));
                _mm256_store_si256((__m256i *)(d3 + (size_t)row * 32u), _mm512_cvtepi32_epi16(q3));
            } else if (profile == UFR_FINAL_INT32) {
                _mm512_store_si512((void *)(d0 + (size_t)row * 64u), q0);
                _mm512_store_si512((void *)(d1 + (size_t)row * 64u), q1);
                _mm512_store_si512((void *)(d2 + (size_t)row * 64u), q2);
                _mm512_store_si512((void *)(d3 + (size_t)row * 64u), q3);
            } else { /* FLOAT32 */
                _mm512_store_ps((float *)(d0 + (size_t)row * 64u), _mm512_cvtepi32_ps(q0));
                _mm512_store_ps((float *)(d1 + (size_t)row * 64u), _mm512_cvtepi32_ps(q1));
                _mm512_store_ps((float *)(d2 + (size_t)row * 64u), _mm512_cvtepi32_ps(q2));
                _mm512_store_ps((float *)(d3 + (size_t)row * 64u), _mm512_cvtepi32_ps(q3));
            }
        }
    
    } else {

        for (uint32_t v = 0; v < view_count; ++v) {
            const unsigned vid = (v + phase + (phase >> 3)) & (UFR_PAPER_MAX_VIEWS - 1u);
            const __m512i ci = ufr_paper_pidx(UFR_PAPER_A64_LANE[vid]);
            const __m512i ti = ufr_paper_tidx(UFR_PAPER_A64_TAIL[vid]);
            uint8_t *dst = (uint8_t *)e->paper_final_cache +
                           (size_t)v * bytes_per_view;
            for (unsigned row = 0; row < UFR_PAPER_NV; ++row) {
                const unsigned src_row = (profile == UFR_FINAL_RAW_INT16 ||
                                          profile == UFR_FINAL_INT32 ||
                                          profile == UFR_FINAL_FLOAT32)
                                         ? (unsigned)(((uint64_t)UFR_PAPER_CACHE_STRIDE * row) & (UFR_PAPER_NV - 1u))
                                         : row;
                const __m256i q16 = _mm256_load_si256(
                    (const __m256i *)(e->retained_starter.starter + (size_t)src_row * UFR_PAPER_V));
                __m512i q32 = _mm512_cvtepi16_epi32(q16);
                q32 = _mm512_permutexvar_epi32(
                    e->retained_starter.tail_mask[src_row] ? ti : ci, q32);
                if (profile == UFR_FINAL_CUSTOM) {
                    const size_t row_bytes = custom_desc->bytes_per_row;
                    uint8_t *row_dst = dst + (size_t)row * row_bytes;
                    alignas(64) int16_t row16[UFR_PAPER_FINAL_ROW_LANES];
                    _mm256_store_si256((__m256i *)row16, _mm512_cvtepi32_epi16(q32));
                    const ufr_final_row_input_t in = {
                        .samples = row16,
                        .sample_count = UFR_PAPER_FINAL_ROW_LANES,
                        .view_id = v, .row_id = row, .epoch = epoch, .phase = phase, .shift = 0u
                    };
                    if (custom_desc->build_row(&in, row_dst, row_bytes, custom_desc->user_ctx) != 0)
                        return -1;
                } else if (profile == UFR_FINAL_RAW_INT16) {
                    const __m256i p = _mm512_cvtepi32_epi16(q32);
                    _mm256_store_si256((__m256i *)(dst + (size_t)row * 32u), p);
                } else if (profile == UFR_FINAL_INT32) {
                    _mm512_store_si512((void *)(dst + (size_t)row * 64u), q32);
                } else {
                    const __m512 x = _mm512_cvtepi32_ps(q32);
                    _mm512_store_ps((float *)(dst + (size_t)row * 64u), x);
                }
            }
        }
    
    }

    e->paper_final_cache_views = view_count;
    e->paper_final_cache_phase = phase;
    e->paper_final_cache_epoch = epoch;
    e->paper_final_cache_profile = (uint8_t)profile;
    e->paper_final_cache_custom_id = e->cfg.custom_profile_id;
    e->paper_final_cache_valid = 1u;
    return 0;
}

static int ufr_paper_final_cache_ensure(ufr_mc_cultivation_engine_t *e,
                                        uint32_t view_count,
                                        uint32_t phase,
                                        uint64_t epoch,
                                        ufr_final_cultivation_profile_t profile)
{
    if (!e || profile == UFR_FINAL_R2_FLOAT32 || profile == UFR_FINAL_AUTO) return -1;
    const ufr_final_profile_desc_t *d =
        (profile == UFR_FINAL_CUSTOM) ? ufr_final_profile_lookup(e->cfg.custom_profile_id) : NULL;
    if (profile == UFR_FINAL_CUSTOM && !d) return -1;
    const int epoch_sensitive = d && (d->flags & UFR_FINAL_PROFILE_FLAG_EPOCH_SENSITIVE);
    if (e->paper_final_cache_valid &&
        e->paper_final_cache_views == view_count &&
        e->paper_final_cache_phase == phase &&
        e->paper_final_cache_profile == (uint8_t)profile &&
        e->paper_final_cache_custom_id == e->cfg.custom_profile_id &&
        (!epoch_sensitive || e->paper_final_cache_epoch == epoch))
        return 0;
    return ufr_paper_final_cache_build(e, view_count, phase, epoch, profile);
}

static void ufr_paper_r2_cache_free(ufr_mc_cultivation_engine_t *e) {
    if (!e) return;
    free(e->paper_r2_cache);
    e->paper_r2_cache = NULL;
    e->paper_r2_cache_bytes = 0u;
    e->paper_r2_cache_views = 0u;
    e->paper_r2_cache_phase = 0u;
    e->paper_r2_cache_valid = 0u;
}

static int ufr_paper_r2_cache_build(ufr_mc_cultivation_engine_t *e,
                                    uint32_t view_count,
                                    uint32_t phase) {
    if (!e || view_count == 0u || view_count > UFR_PAPER_MAX_VIEWS) return -1;
    /* 2 parity banks x 512 row-pairs x 16 float r2 lanes = 64 KiB/view. */
    const size_t bytes_per_view = 2u * 512u * 16u * sizeof(float);
    const size_t bytes = (size_t)view_count * bytes_per_view;
    if (!e->paper_r2_cache || e->paper_r2_cache_bytes != bytes) {
        ufr_paper_r2_cache_free(e);
        if (posix_memalign((void **)&e->paper_r2_cache, 64u, bytes) != 0)
            return -1;
        e->paper_r2_cache_bytes = bytes;
    }

    const __m512 scale2 = _mm512_set1_ps(
        (UFR_R2_NORMAL_SCALE * 0.7071067811865475244f) *
        (UFR_R2_NORMAL_SCALE * 0.7071067811865475244f));

    for (uint32_t v = 0; v < view_count; ++v) {
        const unsigned vid = (v + phase + (phase >> 3)) & (UFR_PAPER_MAX_VIEWS - 1u);
        const __m512i ci = ufr_paper_pidx(UFR_PAPER_A64_LANE[vid]);
        const __m512i ti = ufr_paper_tidx(UFR_PAPER_A64_TAIL[vid]);
        float *dst = e->paper_r2_cache + (size_t)v * (bytes_per_view / sizeof(float));

        /* Two banks over the bijective R2 pair permutation P(r)=555*r mod 1024.
         * The logical pair is still adjacent in cache order, but its physical
         * starter rows are P(2p+parity), P(2p+parity+1). The runtime only
         * chooses the corresponding bank and pair offset. */
        for (unsigned parity = 0; parity < 2u; ++parity) {
            float *db = dst + (size_t)parity * 512u * 16u;
            for (unsigned p = 0; p < 512u; ++p) {
                const unsigned r0 = (unsigned)(((uint64_t)UFR_R2_PAIR_STRIDE * ((p << 1) + parity)) & (UFR_PAPER_NV - 1u));
                const unsigned r1 = (unsigned)(((uint64_t)UFR_R2_PAIR_STRIDE * ((p << 1) + parity + 1u)) & (UFR_PAPER_NV - 1u));
                __m512i q0 = _mm512_cvtepi16_epi32(_mm256_loadu_si256(
                    (const __m256i *)(e->retained_starter.starter + (size_t)r0 * UFR_PAPER_V)));
                __m512i q1 = _mm512_cvtepi16_epi32(_mm256_loadu_si256(
                    (const __m256i *)(e->retained_starter.starter + (size_t)r1 * UFR_PAPER_V)));
                q0 = _mm512_permutexvar_epi32(
                    e->retained_starter.tail_mask[r0] ? ti : ci, q0);
                q1 = _mm512_permutexvar_epi32(
                    e->retained_starter.tail_mask[r1] ? ti : ci, q1);
                const __m512 x = _mm512_cvtepi32_ps(q0);
                const __m512 y = _mm512_cvtepi32_ps(q1);
                const __m512 r2 = _mm512_mul_ps(
                    _mm512_fmadd_ps(x, x, _mm512_mul_ps(y, y)), scale2);
                _mm512_store_ps(db + (size_t)p * 16u, r2);
            }
        }
    }
    e->paper_r2_cache_views = view_count;
    e->paper_r2_cache_phase = phase;
    e->paper_r2_cache_valid = 1u;
    return 0;
}

static int ufr_paper_r2_cache_ensure(ufr_mc_cultivation_engine_t *e,
                                     uint32_t view_count,
                                     uint32_t phase) {
    if (!e) return -1;
    if (e->paper_r2_cache_valid &&
        e->paper_r2_cache_views == view_count &&
        e->paper_r2_cache_phase == phase)
        return 0;
    return ufr_paper_r2_cache_build(e, view_count, phase);
}

static void ufr_mc_free_cache(ufr_mc_cultivation_engine_t *e) {
    if (!e) return;
    free(e->cache_buf); e->cache_buf = NULL; e->cache_bytes = 0;
}

static int ufr_mc_alloc_cache(ufr_mc_cultivation_engine_t *e) {
    if (!e) return -1;
    ufr_mc_free_cache(e);
    if (e->cfg.cultivation == UFR_CULT_DIRECT) return 0;
    e->cache_bytes = e->cfg.chunk_bytes;
    if (posix_memalign((void **)&e->cache_buf, 64u, e->cache_bytes) != 0) {
        e->cache_buf = NULL; e->cache_bytes = 0; return -1;
    }
    memset(e->cache_buf, 0, e->cache_bytes);
    return 0;
}

/* Safe-point generation switch. Unlike the legacy periodic poll path, this
 * is called only after a complete MC chunk has been consumed. */
static __attribute__((noinline)) int ufr_mc_try_generation_switch(ufr_mc_cultivation_engine_t *e) {
    if (!e) return 0;
    UFRXSlot *cur = (e->seedx.active == 0u) ? &e->seedx.handoff_a : &e->seedx.handoff_b;
    UFRXSlot *nxt = (e->seedx.active == 0u) ? &e->seedx.handoff_b : &e->seedx.handoff_a;
    if (atomic_load_explicit(&nxt->state_flag, memory_order_acquire) != UFR_SLOT_READY) return 0;
    ++e->seedx.ready_observations;
    unsigned expected = UFR_SLOT_READY;
    if (!atomic_compare_exchange_strong_explicit(&nxt->state_flag, &expected, UFR_SLOT_ACTIVE,
                                                 memory_order_acquire, memory_order_relaxed)) return 0;
    const uint64_t domain = (e->seedx.active == 0u) ? UFR_DOMAIN_B : UFR_DOMAIN_A;
    ufrx_load(&e->fm, nxt, domain);
    UFRXWorker *owner = (e->seedx.active == 0u) ? &e->seedx.back_a : &e->seedx.back_b;
    pthread_mutex_lock(&owner->wait_mutex);
    atomic_store_explicit(&cur->state_flag, UFR_SLOT_FREE, memory_order_release);
    pthread_cond_signal(&owner->wait_cond);
    pthread_mutex_unlock(&owner->wait_mutex);
    e->seedx.active ^= 1u;
    e->seedx.active_steps = 0u;
    ++e->seedx.switches;
    e->generation_id = nxt->generation;
    e->chunks_in_generation = 0u;
    ufr_mc_free_cache(e);
    ufr_paper_final_cache_free(e);
    ufr_paper_r2_cache_free(e);
    return 1;
}

/* Generation policy at a safe chunk boundary. READY is necessary for an
 * immediate switch; MAX is a cap on how many chunks may be assigned to one
 * starter before we allow a switch as soon as the next seed becomes READY. */
static int ufr_mc_should_switch(ufr_mc_cultivation_engine_t *e) {
    if (!e) return 0;
    if (e->replay_mode) return 0;
    const uint64_t n = e->chunks_in_generation;
    uint32_t minc = e->cfg.min_generation_chunks;
    uint32_t maxc = e->cfg.max_generation_chunks;
    uint8_t eligible = 0u;
    if (e->cfg.generation == UFR_GENERATION_CHUNK) eligible = (n >= minc);
    else if (e->cfg.generation == UFR_GENERATION_FIXED) eligible = (n >= minc);
    else eligible = (n >= minc);
    if (!eligible) return 0;
    /* The max boundary is handled by the same safe-point switch. If READY is
     * absent we continue the current generation rather than stalling the MC. */
    (void)maxc;
    return 1;
}

static int ufr_mc_engine_init(ufr_mc_cultivation_engine_t *e, const ufr_mc_config_t *cfg) {
    if (!e || !cfg) return -1;
    memset(e, 0, sizeof(*e));
    e->requested = *cfg;
    if (ufr_mc_config_resolve(cfg, &e->cfg) != 0) return -1;
    /* Registration is startup-only. Lock only after this engine configuration
     * has been validated, so a failed create does not poison the registry. */
    atomic_store_explicit(&g_ufr_final_registry_locked, 1u, memory_order_release);
    atomic_init(&e->stop, 0);
    ufrx_init(&e->seedx, UFR_DEFAULT_SEED_A, UFR_DEFAULT_SEED_B);
    ufrx_load(&e->fm, &e->seedx.handoff_a, UFR_DOMAIN_A);
    e->generation_id = e->seedx.handoff_a.generation;
    e->chunks_in_generation = 0u;
    e->chunk_id = 0u;
    e->scratch_block = NULL;
    if (posix_memalign((void **)&e->scratch_block, 64u, UFR_PAPER_MC_BLOCK_BYTES) != 0) return -1;
    if (ufr_mc_alloc_cache(e) != 0) { free(e->scratch_block); e->scratch_block = NULL; return -1; }
    if (ufr_post_init(&e->post, UINT64_C(0x0123456789ABCDEF)) != 0) {
        ufr_mc_free_cache(e); free(e->scratch_block); e->scratch_block = NULL; return -1;
    }
    e->initialized = 1u;
    return 0;
}

static int ufr_mc_engine_start(ufr_mc_cultivation_engine_t *e) {
    if (!e || !e->initialized) return -1;
    if (ufrx_start(&e->seedx, &e->stop) != 0) return -1;
    e->seedback_started = 1u;
    return 0;
}

static void ufr_mc_engine_stop(ufr_mc_cultivation_engine_t *e) {
    if (!e) return;
    if (e->seedback_started) {
        ufrx_stop(&e->seedx, &e->stop);
        e->seedback_started = 0u;
    }
}

static void ufr_mc_engine_destroy(ufr_mc_cultivation_engine_t *e) {
    if (!e) return;
    ufr_mc_engine_stop(e);
    ufr_post_destroy(&e->post);
    ufr_mc_free_cache(e);
    ufr_paper_final_cache_free(e);
    ufr_paper_r2_cache_free(e);
    free(e->scratch_block); e->scratch_block = NULL;
    e->initialized = 0u;
}

/* DIRECT: small scratch block is built from the starter and handed directly
 * to MC. The MC callback receives ownership only for the duration of the call. */
static __attribute__((noinline)) int ufr_mc_supply_direct(const ufr_paper_starter_t *starter,
                                                           uint64_t generation_id,
                                                           uint64_t cultivation_epoch,
                                                           uint32_t view_count,
                                                           int32_t *scratch,
                                                           ufr_mc_qblock_consumer_fn consumer,
                                                           void *consumer_ctx) {
    if (!starter || !scratch || !consumer || view_count == 0u) return -1;
    const ufr_paper_cultivation_ctx_t c = ufr_paper_cultivation_ctx(cultivation_epoch);
    for (uint32_t v = 0; v < view_count; ++v) {
        const unsigned raw_view = v % UFR_PAPER_MAX_VIEWS;
        const unsigned vid = (raw_view + c.phase + (c.phase >> 3)) & (UFR_PAPER_MAX_VIEWS - 1u);
        const unsigned base = (UFR_PAPER_A64_OFFSET[vid] + c.shift) &
                              (UFR_PAPER_NV - 1u);
        const __m512i ci = ufr_paper_pidx(UFR_PAPER_A64_LANE[vid]);
        const __m512i ti = ufr_paper_tidx(UFR_PAPER_A64_TAIL[vid]);
        for (uint32_t b = 0; b < UFR_PAPER_MC_BLOCKS_PER_VIEW; ++b) {
            ufr_paper_materialize_block_ctx(starter, base, ci, ti, b, scratch);
            if (consumer(scratch, generation_id, v, b, consumer_ctx) != 0) return -1;
        }
    }
    return 0;
}

/* CACHE: cultivation completes before this function is called. This is the
 * resource-isolation boundary: the Front/permute work is no longer live while
 * the customer MC consumes the cache buffer. */
static __attribute__((noinline)) int ufr_mc_consume_cached(const int32_t *cache_buf,
                                                            size_t cache_bytes,
                                                            uint64_t generation_id,
                                                            uint32_t view_count,
                                                            ufr_mc_qblock_consumer_fn consumer,
                                                            void *consumer_ctx) {
    if (!cache_buf || !cache_bytes || !consumer || view_count == 0u) return -1;
    const size_t blocks_total = cache_bytes / UFR_PAPER_MC_BLOCK_BYTES;
    size_t block_index = 0u;
    for (uint32_t v = 0; v < view_count; ++v) {
        for (uint32_t b = 0; b < UFR_PAPER_MC_BLOCKS_PER_VIEW; ++b, ++block_index) {
            const int32_t *q = cache_buf + block_index * 256u;
            if (consumer(q, generation_id, v, b, consumer_ctx) != 0) return -1;
        }
    }
    return blocks_total == block_index ? 0 : -1;
}

static __attribute__((noinline)) int ufr_mc_cultivate_into_cache(const ufr_paper_starter_t *starter,
                                                                   uint64_t cultivation_epoch,
                                                                   uint32_t view_count,
                                                                   int32_t *cache_buf,
                                                                   size_t cache_bytes) {
    if (!starter || !cache_buf || view_count == 0u) return -1;
    if (cache_bytes != (size_t)view_count * UFR_PAPER_BYTES) return -1;
    for (uint32_t v = 0; v < view_count; ++v) {
        if (ufr_paper_materialize_view(starter, v, cultivation_epoch,
                                        cache_buf + (size_t)v * UFR_PAPER_SAMPLES) != 0) return -1;
    }
    return 0;
}

static void ufr_mc_report_resolved(const ufr_mc_config_resolved_t *r) {
    if (!r) return;
    printf("MC Configuration\n");
    printf("  Cultivation:     %s\n", ufr_cult_name(r->cultivation));
    printf("  MC Chunk:        %u KiB (%u PaperViews)\n", r->chunk_bytes / 1024u, r->views_per_chunk);
    printf("  Final profile:   %s\n", ufr_final_profile_name(r->final_profile));
    if (r->final_profile == UFR_FINAL_CUSTOM) printf("  Custom profile:  %u\n", r->custom_profile_id);
    printf("  Generation:      %s\n", ufr_gen_name(r->generation));
    printf("  Min generation:  %u chunks\n", r->min_generation_chunks);
    printf("  Max generation:  %u chunks\n", r->max_generation_chunks);
}

/* Run a finite number of chunks using the selected transport policy. The MC
 * algorithm is supplied by the caller; Common Post sees one compact chunk mean. */
static __attribute__((noinline)) int ufr_mc_run(ufr_mc_cultivation_engine_t *e,
                                                 uint64_t chunks,
                                                 ufr_mc_qblock_consumer_fn consumer,
                                                 void *consumer_ctx) {
    if (!e || !e->initialized || !consumer || chunks == 0u) return -1;
    const uint32_t views = e->cfg.views_per_chunk;
    const size_t chunk_bytes = (size_t)e->cfg.chunk_bytes;
    for (uint64_t c = 0; c < chunks; ++c) {
        /* New Normal starter at the generation boundary. In the default AUTO
         * configuration this happens once per MC chunk. FIXED mode retains the
         * same starter for the configured generation interval. */
        if (e->chunks_in_generation == 0u) {
            ufr_paper_make_starter(&e->fm, &e->retained_starter);
            e->seedx.active_steps += NORMAL_GROUP;
        }

        const uint64_t chunk_t0 = now_ns();
        int rc = 0;
        if (e->cfg.cultivation == UFR_CULT_DIRECT) {
            rc = ufr_mc_supply_direct(&e->retained_starter, e->generation_id, e->chunk_id, views, e->scratch_block, consumer, consumer_ctx);
        } else {
            if (!e->cache_buf || e->cache_bytes != chunk_bytes) {
                if (ufr_mc_alloc_cache(e) != 0) return -1;
            }
            /* Cultivation ends here before MC starts. */
            rc = ufr_mc_cultivate_into_cache(&e->retained_starter, e->chunk_id, views, e->cache_buf, e->cache_bytes);
            if (rc == 0) rc = ufr_mc_consume_cached(e->cache_buf, e->cache_bytes, e->generation_id, views, consumer, consumer_ctx);
        }
        if (rc != 0) return -1;

        ++e->chunks_in_generation;
        ++e->chunk_id;

        const uint64_t chunk_ns = now_ns() - chunk_t0;
        (void)chunk_ns;
        /* Representative callback is allowed to ignore this bridge. If the
         * supplied callback is our built-in MC, the caller can separately
         * submit its finalized result into Common Post. */

        if (e->cfg.generation == UFR_GENERATION_FIXED &&
            e->chunks_in_generation < e->cfg.min_generation_chunks) continue;

        if (ufr_mc_should_switch(e)) {
            if (ufr_mc_try_generation_switch(e)) {
                /* Cache contents belong to the old generation and are discarded. */
                ufr_mc_free_cache(e);
                if (e->cfg.cultivation != UFR_CULT_DIRECT && ufr_mc_alloc_cache(e) != 0) return -1;
            } else if (e->chunks_in_generation >= e->cfg.max_generation_chunks) {
                /* SeedBack not READY: do not stall MC. Continue current generation
                 * until the asynchronous Back handoff becomes available. */
                ++e->seedx.missed_ready_checks;
            }
        }
    }
    return 0;
}

/*
 * DIRECT fast bridge for the built-in representative MC.
 *
 * The int16 fanout starter is already the exact Normal+4x sequence.  Instead
 * of materializing each PaperView q-block into a 1 KiB int32 scratch buffer
 * and immediately reloading it, widen+permute the q vectors in ZMM registers
 * and feed the same MC arithmetic directly.  No RNG transform is changed.
 * This path is only used by the built-in representative benchmark driver;
 * the generic qblock consumer API remains unchanged for customer MCs.
 */
static __attribute__((noinline)) int ufr_mc_run_representative_direct_profile(
    ufr_mc_cultivation_engine_t *e,
    uint64_t chunks,
    uint64_t *out_samples,
    uint64_t *out_hits,
    double *out_sum)
{
    if (!e || chunks == 0u) return -1;
    const ufr_final_cultivation_profile_t profile = e->cfg.final_profile;
    if (profile == UFR_FINAL_CUSTOM) return -2;
    __m512 acc = _mm512_setzero_ps();
    uint64_t hits = 0u, samples = 0u;
    __m512i hitv = _mm512_setzero_si512();
    const __m512i hitone = _mm512_set1_epi32(1);
    const __m512 one = _mm512_set1_ps(1.0f);
    const __m512 scale2 = _mm512_set1_ps(
        (UF_NORMAL_SCALE * 0.7071067811865475244f) *
        (UF_NORMAL_SCALE * 0.7071067811865475244f));

    /* Long generations amortize final-cultivation materialization.  Short
     * generations keep the original streaming path so the option never forces
     * a large cache build when the caller changes seeds frequently. */
    const int use_profile_cache =
        (e->cfg.generation == UFR_GENERATION_FIXED &&
         e->cfg.max_generation_chunks >= 8u &&
         profile != UFR_FINAL_R2_FLOAT32 && profile != UFR_FINAL_AUTO);
    const int use_r2_cache =
        (e->cfg.generation == UFR_GENERATION_FIXED &&
         e->cfg.max_generation_chunks >= 8u &&
         profile == UFR_FINAL_R2_FLOAT32);

    for (uint64_t cno = 0; cno < chunks; ++cno) {
        if (e->chunks_in_generation == 0u) {
            ufr_paper_make_starter(&e->fm, &e->retained_starter);
            e->seedx.active_steps += NORMAL_GROUP;
        }

        const uint32_t views = e->cfg.views_per_chunk;
        const ufr_paper_cultivation_ctx_t cc = ufr_paper_cultivation_ctx(e->chunk_id);
        const unsigned phase = cc.phase;
        const unsigned shift = cc.shift;

        if (use_r2_cache) {
            if (ufr_paper_r2_cache_ensure(e, views, phase) != 0) return -1;
        } else if (use_profile_cache) {
            if (ufr_paper_final_cache_ensure(e, views, phase, e->chunk_id, profile) != 0) return -1;
        }

        for (uint32_t v = 0; v < views; ++v) {
            const unsigned vid = (v + phase + (phase >> 3)) & (UFR_PAPER_MAX_VIEWS - 1u);
            const unsigned base = (UFR_PAPER_A64_OFFSET[vid] + shift) &
                                  (UFR_PAPER_NV - 1u);

            if (use_r2_cache) {
                const size_t bytes_per_view = 2u * 512u * 16u * sizeof(float);
                const float *vc = e->paper_r2_cache +
                                  (size_t)v * (bytes_per_view / sizeof(float));
                const unsigned mapped_base = (unsigned)(((uint64_t)UFR_R2_PAIR_STRIDE * base) & (UFR_PAPER_NV - 1u));
                const unsigned parity = mapped_base & 1u;
                const float *bank = vc + (size_t)parity * 512u * 16u;
                const unsigned p0 = mapped_base >> 1;
                const unsigned n0 = 512u - p0;

                __m512 acc0 = _mm512_setzero_ps();
                __m512 acc1 = _mm512_setzero_ps();
                __m512 acc2 = _mm512_setzero_ps();
                __m512 acc3 = _mm512_setzero_ps();
                __m512 acc4 = _mm512_setzero_ps();
                __m512 acc5 = _mm512_setzero_ps();
                __m512 acc6 = _mm512_setzero_ps();
                __m512 acc7 = _mm512_setzero_ps();
                uint64_t hit0 = 0u, hit1 = 0u, hit2 = 0u, hit3 = 0u;
                uint64_t hit4 = 0u, hit5 = 0u, hit6 = 0u, hit7 = 0u;

#define UFR_R2_ACC8(DST, HITCNT, IDX) do { \
                    const __m512 r2v = _mm512_load_ps(bank + (size_t)(IDX) * 16u); \
                    const __mmask16 h = _mm512_cmp_ps_mask(r2v, one, _CMP_LE_OQ); \
                    (HITCNT) += (uint64_t)_mm_popcnt_u32((unsigned)h); \
                    (DST) = _mm512_mask_add_ps((DST), h, (DST), _mm512_sub_ps(one, r2v)); \
                } while (0)

                unsigned p = p0;
                unsigned i = 0u;
                for (; i + 7u < n0; i += 8u) {
                    UFR_R2_ACC8(acc0, hit0, p);
                    UFR_R2_ACC8(acc1, hit1, p + 1u);
                    UFR_R2_ACC8(acc2, hit2, p + 2u);
                    UFR_R2_ACC8(acc3, hit3, p + 3u);
                    UFR_R2_ACC8(acc4, hit4, p + 4u);
                    UFR_R2_ACC8(acc5, hit5, p + 5u);
                    UFR_R2_ACC8(acc6, hit6, p + 6u);
                    UFR_R2_ACC8(acc7, hit7, p + 7u);
                    p += 8u;
                }
                for (; i < n0; ++i) {
                    UFR_R2_ACC8(acc0, hit0, p++);
                }
                p = 0u;
                for (i = 0u; i + 7u < p0; i += 8u) {
                    UFR_R2_ACC8(acc0, hit0, p);
                    UFR_R2_ACC8(acc1, hit1, p + 1u);
                    UFR_R2_ACC8(acc2, hit2, p + 2u);
                    UFR_R2_ACC8(acc3, hit3, p + 3u);
                    UFR_R2_ACC8(acc4, hit4, p + 4u);
                    UFR_R2_ACC8(acc5, hit5, p + 5u);
                    UFR_R2_ACC8(acc6, hit6, p + 6u);
                    UFR_R2_ACC8(acc7, hit7, p + 7u);
                    p += 8u;
                }
                for (; i < p0; ++i) {
                    UFR_R2_ACC8(acc0, hit0, p++);
                }
#undef UFR_R2_ACC8
                acc = _mm512_add_ps(acc, acc0);
                acc = _mm512_add_ps(acc, acc1);
                acc = _mm512_add_ps(acc, acc2);
                acc = _mm512_add_ps(acc, acc3);
                acc = _mm512_add_ps(acc, acc4);
                acc = _mm512_add_ps(acc, acc5);
                acc = _mm512_add_ps(acc, acc6);
                acc = _mm512_add_ps(acc, acc7);
                hits += hit0 + hit1 + hit2 + hit3 + hit4 + hit5 + hit6 + hit7;
                samples += 16384u;
            } else if (use_profile_cache) {
                const size_t bytes_per_view = ufr_final_cache_bytes_per_view(profile, e->cfg.custom_profile_id);
                const uint8_t *vc = (const uint8_t *)e->paper_final_cache +
                                    (size_t)v * bytes_per_view;
                if (profile == UFR_FINAL_RAW_INT16) {
                    /* RAW16 has exactly 512 pair-starts. Split the odd-base
                     * wrap case so the hot linear regions need no per-pair
                     * wrap compare/cmov. The arithmetic and pair order are
                     * unchanged; only address formation is simplified. */
                    unsigned src0 = base;
                    unsigned linear_pairs = (UFR_PAPER_NV - base) / 2u;
                    unsigned pair;
                    for (pair = 0u; pair < linear_pairs; ++pair, src0 += 2u) {
                        const unsigned src1 = src0 + 1u;
                        const __m256i q0 = _mm256_load_si256(
                            (const __m256i *)(vc + (size_t)src0 * 32u));
                        const __m256i q1 = _mm256_load_si256(
                            (const __m256i *)(vc + (size_t)src1 * 32u));
                        const __m512 x = _mm512_cvtepi32_ps(_mm512_cvtepi16_epi32(q0));
                        const __m512 y = _mm512_cvtepi32_ps(_mm512_cvtepi16_epi32(q1));
                        const __m512 r2 = _mm512_mul_ps(
                            _mm512_fmadd_ps(x, x, _mm512_mul_ps(y, y)), scale2);
                        const __mmask16 h = _mm512_cmp_ps_mask(r2, one, _CMP_LE_OQ);
                        hitv = _mm512_mask_add_epi32(hitv, h, hitv, hitone);
                        acc = _mm512_add_ps(acc,
                            _mm512_mask_blend_ps(h, _mm512_setzero_ps(),
                                                  _mm512_sub_ps(one, r2)));
                        samples += 32u;
                    }
                    if (base & 1u) {
                        /* The unique odd-base wrap pair is (1023, 0). */
                        const __m256i q0 = _mm256_load_si256(
                            (const __m256i *)(vc + (size_t)1023u * 32u));
                        const __m256i q1 = _mm256_load_si256(
                            (const __m256i *)(vc + 0u));
                        const __m512 x = _mm512_cvtepi32_ps(_mm512_cvtepi16_epi32(q0));
                        const __m512 y = _mm512_cvtepi32_ps(_mm512_cvtepi16_epi32(q1));
                        const __m512 r2 = _mm512_mul_ps(
                            _mm512_fmadd_ps(x, x, _mm512_mul_ps(y, y)), scale2);
                        const __mmask16 h = _mm512_cmp_ps_mask(r2, one, _CMP_LE_OQ);
                        hitv = _mm512_mask_add_epi32(hitv, h, hitv, hitone);
                        acc = _mm512_add_ps(acc,
                            _mm512_mask_blend_ps(h, _mm512_setzero_ps(),
                                                  _mm512_sub_ps(one, r2)));
                        samples += 32u;
                        src0 = 1u;
                    } else {
                        src0 = 0u;
                    }
                    {
                        const unsigned tail_pairs = base / 2u;
                        for (pair = 0u; pair < tail_pairs; ++pair, src0 += 2u) {
                            const unsigned src1 = src0 + 1u;
                            const __m256i q0 = _mm256_load_si256(
                                (const __m256i *)(vc + (size_t)src0 * 32u));
                            const __m256i q1 = _mm256_load_si256(
                                (const __m256i *)(vc + (size_t)src1 * 32u));
                            const __m512 x = _mm512_cvtepi32_ps(_mm512_cvtepi16_epi32(q0));
                            const __m512 y = _mm512_cvtepi32_ps(_mm512_cvtepi16_epi32(q1));
                            const __m512 r2 = _mm512_mul_ps(
                                _mm512_fmadd_ps(x, x, _mm512_mul_ps(y, y)), scale2);
                            const __mmask16 h = _mm512_cmp_ps_mask(r2, one, _CMP_LE_OQ);
                            hitv = _mm512_mask_add_epi32(hitv, h, hitv, hitone);
                            acc = _mm512_add_ps(acc,
                                _mm512_mask_blend_ps(h, _mm512_setzero_ps(),
                                                      _mm512_sub_ps(one, r2)));
                            samples += 32u;
                        }
                    }
                } else {
                    /* INT32/FLOAT32 split-wrap path. The profile is invariant for
                     * the whole run, so select the concrete load path once per
                     * view rather than branching inside every pair. The two
                     * specialized loops keep the same pair order and arithmetic. */
                    const unsigned linear_pairs = (UFR_PAPER_NV - base) / 2u;
                    if (profile == UFR_FINAL_INT32) {
                        unsigned src0 = base;
                        unsigned pair;
                        for (pair = 0u; pair < linear_pairs; ++pair, src0 += 2u) {
                            const unsigned src1 = src0 + 1u;
                            const __m512i q0 = _mm512_load_si512(
                                (const void *)(vc + (size_t)src0 * 64u));
                            const __m512i q1 = _mm512_load_si512(
                                (const void *)(vc + (size_t)src1 * 64u));
                            const __m512 x = _mm512_cvtepi32_ps(q0);
                            const __m512 y = _mm512_cvtepi32_ps(q1);
                            const __m512 r2 = _mm512_mul_ps(
                                _mm512_fmadd_ps(x, x, _mm512_mul_ps(y, y)), scale2);
                            const __mmask16 h = _mm512_cmp_ps_mask(r2, one, _CMP_LE_OQ);
                            hitv = _mm512_mask_add_epi32(hitv, h, hitv, hitone);
                            acc = _mm512_add_ps(acc,
                                _mm512_mask_blend_ps(h, _mm512_setzero_ps(),
                                                      _mm512_sub_ps(one, r2)));
                            samples += 32u;
                        }
                        if (base & 1u) {
                            const __m512i q0 = _mm512_load_si512(
                                (const void *)(vc + (size_t)(UFR_PAPER_NV - 1u) * 64u));
                            const __m512i q1 = _mm512_load_si512((const void *)vc);
                            const __m512 x = _mm512_cvtepi32_ps(q0);
                            const __m512 y = _mm512_cvtepi32_ps(q1);
                            const __m512 r2 = _mm512_mul_ps(
                                _mm512_fmadd_ps(x, x, _mm512_mul_ps(y, y)), scale2);
                            const __mmask16 h = _mm512_cmp_ps_mask(r2, one, _CMP_LE_OQ);
                            hitv = _mm512_mask_add_epi32(hitv, h, hitv, hitone);
                            acc = _mm512_add_ps(acc,
                                _mm512_mask_blend_ps(h, _mm512_setzero_ps(),
                                                      _mm512_sub_ps(one, r2)));
                            samples += 32u;
                            src0 = 1u;
                        } else {
                            src0 = 0u;
                        }
                        {
                            const unsigned tail_pairs = base / 2u;
                            for (pair = 0u; pair < tail_pairs; ++pair, src0 += 2u) {
                                const unsigned src1 = src0 + 1u;
                                const __m512i q0 = _mm512_load_si512(
                                    (const void *)(vc + (size_t)src0 * 64u));
                                const __m512i q1 = _mm512_load_si512(
                                    (const void *)(vc + (size_t)src1 * 64u));
                                const __m512 x = _mm512_cvtepi32_ps(q0);
                                const __m512 y = _mm512_cvtepi32_ps(q1);
                                const __m512 r2 = _mm512_mul_ps(
                                    _mm512_fmadd_ps(x, x, _mm512_mul_ps(y, y)), scale2);
                                const __mmask16 h = _mm512_cmp_ps_mask(r2, one, _CMP_LE_OQ);
                                hitv = _mm512_mask_add_epi32(hitv, h, hitv, hitone);
                                acc = _mm512_add_ps(acc,
                                    _mm512_mask_blend_ps(h, _mm512_setzero_ps(),
                                                          _mm512_sub_ps(one, r2)));
                                samples += 32u;
                            }
                        }
                    } else { /* FLOAT32 */
                        unsigned src0 = base;
                        unsigned pair;
                        for (pair = 0u; pair < linear_pairs; ++pair, src0 += 2u) {
                            const unsigned src1 = src0 + 1u;
                            const __m512 x = _mm512_load_ps(
                                (const float *)(vc + (size_t)src0 * 64u));
                            const __m512 y = _mm512_load_ps(
                                (const float *)(vc + (size_t)src1 * 64u));
                            const __m512 r2 = _mm512_mul_ps(
                                _mm512_fmadd_ps(x, x, _mm512_mul_ps(y, y)), scale2);
                            const __mmask16 h = _mm512_cmp_ps_mask(r2, one, _CMP_LE_OQ);
                            hitv = _mm512_mask_add_epi32(hitv, h, hitv, hitone);
                            acc = _mm512_add_ps(acc,
                                _mm512_mask_blend_ps(h, _mm512_setzero_ps(),
                                                      _mm512_sub_ps(one, r2)));
                            samples += 32u;
                        }
                        if (base & 1u) {
                            const __m512 x = _mm512_load_ps(
                                (const float *)(vc + (size_t)(UFR_PAPER_NV - 1u) * 64u));
                            const __m512 y = _mm512_load_ps((const float *)vc);
                            const __m512 r2 = _mm512_mul_ps(
                                _mm512_fmadd_ps(x, x, _mm512_mul_ps(y, y)), scale2);
                            const __mmask16 h = _mm512_cmp_ps_mask(r2, one, _CMP_LE_OQ);
                            hitv = _mm512_mask_add_epi32(hitv, h, hitv, hitone);
                            acc = _mm512_add_ps(acc,
                                _mm512_mask_blend_ps(h, _mm512_setzero_ps(),
                                                      _mm512_sub_ps(one, r2)));
                            samples += 32u;
                            src0 = 1u;
                        } else {
                            src0 = 0u;
                        }
                        {
                            const unsigned tail_pairs = base / 2u;
                            for (pair = 0u; pair < tail_pairs; ++pair, src0 += 2u) {
                                const unsigned src1 = src0 + 1u;
                                const __m512 x = _mm512_load_ps(
                                    (const float *)(vc + (size_t)src0 * 64u));
                                const __m512 y = _mm512_load_ps(
                                    (const float *)(vc + (size_t)src1 * 64u));
                                const __m512 r2 = _mm512_mul_ps(
                                    _mm512_fmadd_ps(x, x, _mm512_mul_ps(y, y)), scale2);
                                const __mmask16 h = _mm512_cmp_ps_mask(r2, one, _CMP_LE_OQ);
                                hitv = _mm512_mask_add_epi32(hitv, h, hitv, hitone);
                                acc = _mm512_add_ps(acc,
                                    _mm512_mask_blend_ps(h, _mm512_setzero_ps(),
                                                          _mm512_sub_ps(one, r2)));
                                samples += 32u;
                            }
                        }
                    }
                }
            } else {
                /* Short-generation streaming fallback: preserve the established data
                 * path exactly when final-cultivation reuse cannot amortize a
                 * cache build. */
                const __m512i ci = ufr_paper_pidx(UFR_PAPER_A64_LANE[vid]);
                const __m512i ti = ufr_paper_tidx(UFR_PAPER_A64_TAIL[vid]);
                const unsigned pair_stride = (profile == UFR_FINAL_R2_FLOAT32)
                                            ? UFR_R2_PAIR_STRIDE : UFR_PAPER_CACHE_STRIDE;
                const unsigned pair_step = (pair_stride << 1) & (UFR_PAPER_NV - 1u);
                unsigned src0 = (unsigned)(((uint64_t)pair_stride * base) & (UFR_PAPER_NV - 1u));
                for (unsigned pair = 0; pair < (UFR_PAPER_NV / 2u); ++pair) {
                    unsigned src1 = (src0 + pair_stride) & (UFR_PAPER_NV - 1u);
                    const __m256i q0 = _mm256_loadu_si256(
                        (const __m256i *)(e->retained_starter.starter + (size_t)src0 * UFR_PAPER_V));
                    const __m256i q1 = _mm256_loadu_si256(
                        (const __m256i *)(e->retained_starter.starter + (size_t)src1 * UFR_PAPER_V));
                    const __m512i q0z = _mm512_permutexvar_epi32(
                        e->retained_starter.tail_mask[src0] ? ti : ci,
                        _mm512_cvtepi16_epi32(q0));
                    const __m512i q1z = _mm512_permutexvar_epi32(
                        e->retained_starter.tail_mask[src1] ? ti : ci,
                        _mm512_cvtepi16_epi32(q1));
                    const __m512 x = _mm512_cvtepi32_ps(q0z);
                    const __m512 y = _mm512_cvtepi32_ps(q1z);
                    const __m512 r2 = _mm512_mul_ps(
                        _mm512_fmadd_ps(x, x, _mm512_mul_ps(y, y)), scale2);
                    const __mmask16 h = _mm512_cmp_ps_mask(r2, one, _CMP_LE_OQ);
                    hitv = _mm512_mask_add_epi32(hitv, h, hitv, hitone);
                    acc = _mm512_add_ps(acc,
                        _mm512_mask_blend_ps(h, _mm512_setzero_ps(),
                                              _mm512_sub_ps(one, r2)));
                    src0 = (src0 + pair_step) & (UFR_PAPER_NV - 1u);
                    samples += 32u;
                }
            }
        }

        hits += (uint64_t)_mm512_reduce_add_epi32(hitv);
        hitv = _mm512_setzero_si512();

        ++e->chunks_in_generation;
        ++e->chunk_id;
        if (e->cfg.generation == UFR_GENERATION_FIXED &&
            e->chunks_in_generation < e->cfg.min_generation_chunks) continue;
        if (ufr_mc_should_switch(e)) {
            if (ufr_mc_try_generation_switch(e)) {
                ufr_mc_free_cache(e);
                ufr_paper_final_cache_free(e);
                ufr_paper_r2_cache_free(e);
                if (e->cfg.cultivation != UFR_CULT_DIRECT && ufr_mc_alloc_cache(e) != 0)
                    return -1;
            } else if (e->chunks_in_generation >= e->cfg.max_generation_chunks) {
                ++e->seedx.missed_ready_checks;
            }
        }
    }

    alignas(64) float tmp[16];
    _mm512_store_ps(tmp, acc);
    double sum = 0.0;
    for (unsigned i = 0; i < 16; ++i) sum += (double)tmp[i];
    if (out_samples) *out_samples = samples;
    if (out_hits) *out_hits = hits;
    if (out_sum) *out_sum = sum;
    return 0;
}

/* Utility that runs the built-in representative MC and returns the aggregate. */
static __attribute__((noinline)) int ufr_mc_run_representative(ufr_mc_cultivation_engine_t *e,
                                                                 uint64_t chunks,
                                                                 uint64_t *out_samples,
                                                                 uint64_t *out_hits,
                                                                 double *out_sum) {
    if (!e) return -1;
    if (e->cfg.cultivation == UFR_CULT_DIRECT)
        return ufr_mc_run_representative_direct_profile(e, chunks, out_samples, out_hits, out_sum);

    ufr_representative_mc_t rep;
    ufr_representative_mc_init(&rep);

    const int rc = ufr_mc_run(e, chunks, ufr_representative_mc_consume, &rep);
    if (rc != 0) return rc;
    if (out_samples) *out_samples = rep.samples;
    if (out_hits) *out_hits = rep.hits;
    if (out_sum) {
        alignas(64) float tmp[16];
        _mm512_store_ps(tmp, rep.acc);
        double sum = 0.0;
        for (unsigned i = 0; i < 16; ++i) sum += (double)tmp[i];
        *out_sum = sum;
    }
    return 0;
}

#ifndef UFR_PAPERVIEW_MC3_NO_MAIN
static void ufr_mc_usage(const char *argv0) {
    fprintf(stderr, "usage: %s [auto|direct|l1|l2] [chunks] [auto|raw16|int32|float|r2] [generation_chunks]\n", argv0);
    fprintf(stderr, "       generation_chunks=1 uses the per-chunk seed boundary; >1 selects FIXED generation mode.\n");
}

static int ufr_parse_final_profile(const char *s, ufr_mc_config_t *cfg) {
    if (!s || !cfg) return -1;
    if (strcmp(s, "auto") == 0) cfg->final_profile = UFR_FINAL_AUTO;
    else if (strcmp(s, "raw16") == 0) cfg->final_profile = UFR_FINAL_RAW_INT16;
    else if (strcmp(s, "int32") == 0) cfg->final_profile = UFR_FINAL_INT32;
    else if (strcmp(s, "float") == 0) cfg->final_profile = UFR_FINAL_FLOAT32;
    else if (strcmp(s, "r2") == 0) cfg->final_profile = UFR_FINAL_R2_FLOAT32;
    else return -1;
    return 0;
}

static int ufr_parse_mode(const char *s, ufr_mc_config_t *cfg) {
    if (!s || !cfg) return -1;
    if (strcmp(s, "auto") == 0) cfg->cultivation = UFR_CULT_AUTO;
    else if (strcmp(s, "direct") == 0) cfg->cultivation = UFR_CULT_DIRECT;
    else if (strcmp(s, "l1") == 0) cfg->cultivation = UFR_CULT_L1_CACHE;
    else if (strcmp(s, "l2") == 0) cfg->cultivation = UFR_CULT_L2_CACHE;
    else return -1;
    return 0;
}

#ifndef UFR_EXTERNAL_MAIN
int main(int argc, char **argv) {
    pin_cpu0();
    if (argc >= 2 && strcmp(argv[1], "verify") == 0) {
        const int vrc = ufr_mc_enterprise_selftest();
        printf("Enterprise selftest: %s\n", vrc == 0 ? "PASS" : "FAIL");
        return vrc == 0 ? 0 : 10;
    }
    ufr_mc_config_t cfg;
    ufr_mc_config_default(&cfg);
    uint64_t chunks = 4u;
    if (argc >= 2 && ufr_parse_mode(argv[1], &cfg) != 0) { ufr_mc_usage(argv[0]); return 2; }
    if (argc >= 3) chunks = strtoull(argv[2], NULL, 10);
    if (argc >= 4 && ufr_parse_final_profile(argv[3], &cfg) != 0) { ufr_mc_usage(argv[0]); return 2; }
    if (argc >= 5) {
        const uint64_t g = strtoull(argv[4], NULL, 10);
        if (g == 0u || g > UINT32_MAX) { ufr_mc_usage(argv[0]); return 2; }
        if (g > 1u) {
            cfg.generation = UFR_GENERATION_FIXED;
            cfg.min_generation_mode = UFR_GENERATION_FIELD_CUSTOM;
            cfg.max_generation_mode = UFR_GENERATION_FIELD_CUSTOM;
            cfg.min_generation_chunks = (uint32_t)g;
            cfg.max_generation_chunks = (uint32_t)g;
        }
    }
    if (chunks == 0u) chunks = 1u;

    ufr_mc_config_resolved_t resolved;
    if (ufr_mc_config_resolve(&cfg, &resolved) != 0) { fprintf(stderr, "invalid MC config\n"); return 3; }
    ufr_mc_report_resolved(&resolved);

    ufr_mc_cultivation_engine_t e;
    if (ufr_mc_engine_init(&e, &cfg) != 0) { fprintf(stderr, "engine init failed\n"); return 4; }
    if (ufr_mc_engine_start(&e) != 0) { fprintf(stderr, "engine start failed\n"); ufr_mc_engine_destroy(&e); return 5; }

    const uint64_t t0 = now_ns();
    uint64_t samples=0, hits=0; double sum=0.0;
    const int rc = ufr_mc_run_representative(&e, chunks, &samples, &hits, &sum);
    const uint64_t dt = now_ns() - t0;
    if (rc != 0) { fprintf(stderr, "MC run failed (%d)\n", rc); ufr_mc_engine_destroy(&e); return 6; }

    printf("Result: chunks=%" PRIu64 " samples=%" PRIu64 " hits=%" PRIu64 " mean=%+.9g checksum=%016" PRIx64 "\n",
           chunks, samples, hits, samples ? sum/(double)samples : 0.0,
           ((uint64_t)hits<<32) ^ (uint64_t)(sum*1048576.0));
    printf("Timing: %.3f ms | %.3f Gsamples/s logical\n",
           (double)dt/1e6, samples ? (double)samples/((double)dt/1e9)/1e9 : 0.0);
    printf("Generation: current=%" PRIu64 " chunks_in_generation=%" PRIu64 " switches=%" PRIu64 " ready=%" PRIu64 " missed=%" PRIu64 "\n",
           e.generation_id, e.chunks_in_generation, e.seedx.switches,
           e.seedx.ready_observations, e.seedx.missed_ready_checks);
    ufr_mc_engine_destroy(&e);
    return 0;
}
#endif /* UFR_EXTERNAL_MAIN */
#endif /* UFR_PAPERVIEW_MC3_NO_MAIN */


/* -------------------------------------------------------------------------
 * Public PaperView / Final-Cultivation engine wrapper
 * ------------------------------------------------------------------------- */
struct ufr_paperview_mc_engine {
    ufr_mc_cultivation_engine_t impl;
    ufr_paperview_mc_config_t public_cfg;
    uint8_t started;
    uint8_t reserved[7];
};

static int ufr_public_config_to_internal(const ufr_paperview_mc_config_t *pc,
                                         ufr_mc_config_t *ic)
{
    if (!pc || !ic || pc->abi_version != UFR_FINAL_API_VERSION) return -1;
    ufr_mc_config_default(ic);
    ic->cultivation = pc->cultivation;
    if (pc->chunk_bytes) {
        ic->chunk_mode = UFR_MC_CHUNK_CUSTOM;
        ic->chunk_bytes = pc->chunk_bytes;
    }
    ic->final_profile = pc->final_profile;
    ic->custom_profile_id = pc->custom_profile_id;
    if (pc->generation_chunks > 1u) {
        ic->generation = UFR_GENERATION_FIXED;
        ic->min_generation_mode = UFR_GENERATION_FIELD_CUSTOM;
        ic->max_generation_mode = UFR_GENERATION_FIELD_CUSTOM;
        ic->min_generation_chunks = pc->generation_chunks;
        ic->max_generation_chunks = pc->generation_chunks;
    }
    return 0;
}

int ufr_paperview_mc_create(const ufr_paperview_mc_config_t *config,
                            ufr_paperview_mc_engine_t **out_engine)
{
    if (!config || !out_engine) return -1;
    *out_engine = NULL;
    ufr_paperview_mc_engine_t *pe = NULL;
    if (posix_memalign((void **)&pe, 64u, sizeof(*pe)) != 0 || !pe) return -2;
    memset(pe, 0, sizeof(*pe));
    ufr_mc_config_t ic;
    if (ufr_public_config_to_internal(config, &ic) != 0 ||
        ufr_mc_engine_init(&pe->impl, &ic) != 0) {
        free(pe);
        return -3;
    }
    pe->public_cfg = *config;
    *out_engine = pe;
    return 0;
}

int ufr_paperview_mc_start(ufr_paperview_mc_engine_t *engine)
{
    if (!engine) return -1;
    if (engine->impl.replay_mode) return -3;
    if (engine->started) return 0;
    if (ufr_mc_engine_start(&engine->impl) != 0) return -2;
    engine->started = 1u;
    return 0;
}

int ufr_paperview_mc_stop(ufr_paperview_mc_engine_t *engine)
{
    if (!engine) return -1;
    if (engine->started) {
        ufr_mc_engine_stop(&engine->impl);
        engine->started = 0u;
    }
    return 0;
}

/* Generic view-runner. The customer MC receives a zero-copy view descriptor
 * once per PaperView. The final profile cache is built once per generation
 * phase; only the row_start (epoch rotation) changes in the hot path. */
int ufr_paperview_mc_run_views(ufr_paperview_mc_engine_t *engine,
                               uint64_t chunks,
                               ufr_final_view_consumer_fn consumer,
                               void *user_ctx)
{
    if (!engine || !engine->impl.initialized || !consumer || chunks == 0u) return -1;
    if (!engine->started) return -2;
    ufr_mc_cultivation_engine_t *e = &engine->impl;
    const uint32_t views = e->cfg.views_per_chunk;
    const ufr_final_cultivation_profile_t profile = e->cfg.final_profile;
    if (profile == UFR_FINAL_R2_FLOAT32) return -3; /* R2 uses the specialized built-in pair layout. */
    if (profile == UFR_FINAL_CUSTOM && !ufr_final_profile_lookup(e->cfg.custom_profile_id)) return -4;

    for (uint64_t c = 0; c < chunks; ++c) {
        if (e->chunks_in_generation == 0u) {
            ufr_paper_make_starter(&e->fm, &e->retained_starter);
            e->seedx.active_steps += NORMAL_GROUP;
        }
        const ufr_paper_cultivation_ctx_t cc = ufr_paper_cultivation_ctx(e->chunk_id);
        const uint32_t phase = cc.phase;
        const uint32_t shift = cc.shift;
        if (profile == UFR_FINAL_CUSTOM || profile == UFR_FINAL_RAW_INT16 ||
            profile == UFR_FINAL_INT32 || profile == UFR_FINAL_FLOAT32) {
            if (ufr_paper_final_cache_ensure(e, views, phase, e->chunk_id, profile) != 0) return -5;
        }
        const size_t bytes_per_view = ufr_final_cache_bytes_per_view(profile, e->cfg.custom_profile_id);
        if (bytes_per_view == 0u) return -6;
        for (uint32_t v = 0; v < views; ++v) {
            const unsigned vid = (v + phase + (phase >> 3)) & (UFR_PAPER_MAX_VIEWS - 1u);
            const unsigned row_start = (UFR_PAPER_A64_OFFSET[vid] + shift) &
                                        (UFR_PAPER_NV - 1u);
            const uint8_t *base_ptr = (const uint8_t *)e->paper_final_cache +
                                      (size_t)v * bytes_per_view;
            const uint32_t profile_id = (profile == UFR_FINAL_CUSTOM) ? e->cfg.custom_profile_id : 0u;
            const ufr_final_view_t view = {
                .rows = base_ptr,
                .bytes = bytes_per_view,
                .bytes_per_row = bytes_per_view / UFR_PAPER_NV,
                .row_count = UFR_PAPER_NV,
                .row_start = row_start,
                .view_id = v,
                .epoch = e->chunk_id,
                .generation_id = e->generation_id,
                .profile_id = profile_id,
                .built_in_profile = profile
            };
            if (consumer(&view, user_ctx) != 0) return -7;
        }
        ++e->chunks_in_generation;
        ++e->chunk_id;
        if (e->cfg.generation == UFR_GENERATION_FIXED &&
            e->chunks_in_generation < e->cfg.min_generation_chunks)
            continue;
        if (ufr_mc_should_switch(e)) {
            if (ufr_mc_try_generation_switch(e)) {
                ufr_mc_free_cache(e);
                ufr_paper_final_cache_free(e);
                ufr_paper_r2_cache_free(e);
                if (e->cfg.cultivation != UFR_CULT_DIRECT && ufr_mc_alloc_cache(e) != 0) return -8;
            } else if (e->chunks_in_generation >= e->cfg.max_generation_chunks) {
                ++e->seedx.missed_ready_checks;
            }
        }
    }
    return 0;
}

void ufr_paperview_mc_destroy(ufr_paperview_mc_engine_t *engine)
{
    if (!engine) return;
    ufr_mc_engine_destroy(&engine->impl);
    free(engine);
}

/* -------------------------------------------------------------------------
 * Embedded MC profile selector (single-file mainline integration)
 * ------------------------------------------------------------------------- */
static const char *ufr_mc_v8_names[UFR_MC_V8__COUNT] = {
    "AUTO","BASELINE","VEC8","PARAM8_SHARED","PARAM4_PRECOMPUTE",
    "PARAM8_PRECOMPUTE","PARAM4_PRECOMPUTE_INVARIANT","PATH8_BLOCK",
    "PATH4X2","PATH4","R2_CACHE8"
};
const char *ufr_mc_v8_profile_name(ufr_mc_v8_profile_id_t id) {
    return ((unsigned)id < UFR_MC_V8__COUNT) ? ufr_mc_v8_names[id] : "UNKNOWN";
}
static int ufr_mc_v8_ok(const ufr_mc_v8_workload_t *w, uint32_t need) {
    return w && ((w->caps & need) == need);
}
int ufr_mc_v8_select(const ufr_mc_v8_workload_t *w,
                     ufr_mc_v8_profile_id_t req,
                     ufr_mc_v8_plan_t *out) {
    if (!w || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->requested=req;
    const uint32_t heavy=w->transform_cost_estimate>=60u;
    if(req!=UFR_MC_V8_AUTO){
        uint32_t need=0u;
        switch(req){
        case UFR_MC_V8_BASELINE: break;
        case UFR_MC_V8_VEC8: need=UFR_MC_V8_CAP_SAMPLE_INDEPENDENT; break;
        case UFR_MC_V8_PARAM8_SHARED:
            need=UFR_MC_V8_CAP_SAMPLE_INDEPENDENT|UFR_MC_V8_CAP_INDEPENDENT_PARAMS|UFR_MC_V8_CAP_COMMON_TRANSFORM; break;
        case UFR_MC_V8_PARAM4_PRECOMPUTE:
            need=UFR_MC_V8_CAP_SAMPLE_INDEPENDENT|UFR_MC_V8_CAP_INDEPENDENT_PARAMS|UFR_MC_V8_CAP_COMMON_TRANSFORM|
                 UFR_MC_V8_CAP_PRECOMPUTE_SAFE|UFR_MC_V8_CAP_PARAM4_PRECOMPUTE_PROVEN; break;
        case UFR_MC_V8_PARAM8_PRECOMPUTE:
            need=UFR_MC_V8_CAP_SAMPLE_INDEPENDENT|UFR_MC_V8_CAP_INDEPENDENT_PARAMS|UFR_MC_V8_CAP_COMMON_TRANSFORM|UFR_MC_V8_CAP_PRECOMPUTE_SAFE; break;
        case UFR_MC_V8_PARAM4_PRECOMPUTE_INVARIANT:
            need=UFR_MC_V8_CAP_SAMPLE_INDEPENDENT|UFR_MC_V8_CAP_INDEPENDENT_PARAMS|UFR_MC_V8_CAP_COMMON_TRANSFORM|
                 UFR_MC_V8_CAP_PRECOMPUTE_SAFE|UFR_MC_V8_CAP_PRECOMPUTE_LOCAL_FAST|
                 UFR_MC_V8_CAP_PARAM4_PRECOMPUTE_PROVEN|UFR_MC_V8_CAP_INVARIANT_PREP_SAFE; break;
        case UFR_MC_V8_PATH8_BLOCK:
            need=UFR_MC_V8_CAP_SAMPLE_INDEPENDENT|UFR_MC_V8_CAP_INDEPENDENT_PATHS|UFR_MC_V8_CAP_PATH_BLOCK_LAYOUT_READY; break;
        case UFR_MC_V8_PATH4X2:
            need=UFR_MC_V8_CAP_SAMPLE_INDEPENDENT|UFR_MC_V8_CAP_INDEPENDENT_PATHS|UFR_MC_V8_CAP_PATH_BLOCK_LAYOUT_READY|
                 UFR_MC_V8_CAP_PATH_BLOCK_BENCH_PROVEN|UFR_MC_V8_CAP_PATH4X2_BENCH_PROVEN; break;
        case UFR_MC_V8_PATH4:
            need=UFR_MC_V8_CAP_SAMPLE_INDEPENDENT|UFR_MC_V8_CAP_INDEPENDENT_PATHS|UFR_MC_V8_CAP_PURE_PATH4_BENCH_PROVEN; break;
        case UFR_MC_V8_R2_CACHE8:
            need=UFR_MC_V8_CAP_PAIRWISE|UFR_MC_V8_CAP_REPEATED_TRANSFORM|UFR_MC_V8_CAP_PRECOMPUTE_SAFE|UFR_MC_V8_CAP_LANE_LOCAL; break;
        default: return -2;
        }
        if(!ufr_mc_v8_ok(w,need)) return -2;
        out->selected=req;
        out->preferred_lanes=(req==UFR_MC_V8_BASELINE)?1u:
            ((req==UFR_MC_V8_PARAM4_PRECOMPUTE || req==UFR_MC_V8_PARAM4_PRECOMPUTE_INVARIANT ||
              req==UFR_MC_V8_PATH4X2 || req==UFR_MC_V8_PATH4)?4u:8u);
        out->requires_precompute=(req==UFR_MC_V8_PARAM4_PRECOMPUTE || req==UFR_MC_V8_PARAM8_PRECOMPUTE ||
                                   req==UFR_MC_V8_PARAM4_PRECOMPUTE_INVARIANT || req==UFR_MC_V8_R2_CACHE8)?1u:0u;
        return 0;
    }
    if(ufr_mc_v8_ok(w,UFR_MC_V8_CAP_PAIRWISE|UFR_MC_V8_CAP_REPEATED_TRANSFORM|UFR_MC_V8_CAP_PRECOMPUTE_SAFE|UFR_MC_V8_CAP_LANE_LOCAL)){
        out->selected=UFR_MC_V8_R2_CACHE8; out->preferred_lanes=8u; out->requires_precompute=1u;
        out->reason_mask=UFR_MC_V8_REASON_R2; out->score=100u; return 0;
    }
    if(heavy && w->params_per_batch>=8u && ufr_mc_v8_ok(w,
        UFR_MC_V8_CAP_SAMPLE_INDEPENDENT|UFR_MC_V8_CAP_INDEPENDENT_PARAMS|UFR_MC_V8_CAP_COMMON_TRANSFORM|
        UFR_MC_V8_CAP_PRECOMPUTE_SAFE|UFR_MC_V8_CAP_PRECOMPUTE_LOCAL_FAST|UFR_MC_V8_CAP_PARAM4_PRECOMPUTE_PROVEN|
        UFR_MC_V8_CAP_INVARIANT_PREP_SAFE|UFR_MC_V8_CAP_INVARIANT_PREP_PROVEN)){
        out->selected=UFR_MC_V8_PARAM4_PRECOMPUTE_INVARIANT; out->preferred_lanes=4u; out->requires_precompute=1u;
        out->reason_mask=UFR_MC_V8_REASON_PARAM|UFR_MC_V8_REASON_COMMON|UFR_MC_V8_REASON_HEAVY|
                         UFR_MC_V8_REASON_FAST_PRECACHE|UFR_MC_V8_REASON_BENCH_PROVEN|UFR_MC_V8_REASON_INVARIANT_PREP;
        out->score=97u; return 0;
    }
    if(heavy && w->params_per_batch>=8u && ufr_mc_v8_ok(w,
        UFR_MC_V8_CAP_SAMPLE_INDEPENDENT|UFR_MC_V8_CAP_INDEPENDENT_PARAMS|UFR_MC_V8_CAP_COMMON_TRANSFORM|
        UFR_MC_V8_CAP_PRECOMPUTE_SAFE|UFR_MC_V8_CAP_PRECOMPUTE_LOCAL_FAST|UFR_MC_V8_CAP_PARAM4_PRECOMPUTE_PROVEN)){
        out->selected=UFR_MC_V8_PARAM4_PRECOMPUTE; out->preferred_lanes=4u; out->requires_precompute=1u;
        out->reason_mask=UFR_MC_V8_REASON_PARAM|UFR_MC_V8_REASON_COMMON|UFR_MC_V8_REASON_HEAVY|
                         UFR_MC_V8_REASON_FAST_PRECACHE|UFR_MC_V8_REASON_BENCH_PROVEN;
        out->score=95u; return 0;
    }
    if(heavy && w->params_per_batch>=8u && ufr_mc_v8_ok(w,UFR_MC_V8_CAP_SAMPLE_INDEPENDENT|UFR_MC_V8_CAP_INDEPENDENT_PARAMS|UFR_MC_V8_CAP_COMMON_TRANSFORM)){
        out->selected=UFR_MC_V8_PARAM8_SHARED; out->preferred_lanes=8u;
        out->reason_mask=UFR_MC_V8_REASON_PARAM|UFR_MC_V8_REASON_COMMON|UFR_MC_V8_REASON_HEAVY; out->score=90u; return 0;
    }
    if(heavy && w->paths_per_batch>=8u && ufr_mc_v8_ok(w,UFR_MC_V8_CAP_SAMPLE_INDEPENDENT|UFR_MC_V8_CAP_INDEPENDENT_PATHS|
        UFR_MC_V8_CAP_PATH_BLOCK_LAYOUT_READY|UFR_MC_V8_CAP_PATH_BLOCK_BENCH_PROVEN|UFR_MC_V8_CAP_PATH4X2_BENCH_PROVEN)){
        out->selected=UFR_MC_V8_PATH4X2; out->preferred_lanes=4u;
        out->reason_mask=UFR_MC_V8_REASON_PATH|UFR_MC_V8_REASON_LAYOUT_READY|UFR_MC_V8_REASON_BENCH_PROVEN|
                         UFR_MC_V8_REASON_RESOURCE_SAFE|UFR_MC_V8_REASON_HEAVY; out->score=88u; return 0;
    }
    out->selected=UFR_MC_V8_BASELINE; out->preferred_lanes=1u; out->score=10u; return 0;
}


/* -------------------------------------------------------------------------
 * Embedded V9 domain-aware profile arbitration.
 * R2_CACHE8 is a distinct cache representation and is not valid for the
 * current Finance/Pharma q256 consumers.  Strip R2 capabilities for those
 * domains before AUTO selection; generic R2 workloads retain R2 priority.
 * ------------------------------------------------------------------------- */
int ufr_mc_v9_select(const ufr_mc_v9_request_t *req,
                     ufr_mc_v9_plan_t *out);
const char *ufr_mc_v9_domain_name(ufr_mc_v9_domain_t domain);

static int ufr_mc_v9_embedded_select(const ufr_mc_v9_request_t *req,
                                     ufr_mc_v9_plan_t *out)
{
    if (!req || !out) return -1;
    memset(out, 0, sizeof(*out));
    ufr_mc_v8_workload_t w = req->workload;
    out->domain_compatibility_mask = UFR_MC_V9_COMPAT_BASELINE |
                                      UFR_MC_V9_COMPAT_PARAM4 |
                                      UFR_MC_V9_COMPAT_PARAM8 |
                                      UFR_MC_V9_COMPAT_R2;

    if (req->domain == UFR_MC_V9_DOMAIN_FINANCE ||
        req->domain == UFR_MC_V9_DOMAIN_PHARMA) {
        w.caps &= ~(UFR_MC_V8_CAP_PAIRWISE | UFR_MC_V8_CAP_REPEATED_TRANSFORM);
        out->domain_compatibility_mask &= ~UFR_MC_V9_COMPAT_R2;
    }

    const uint32_t r2need = UFR_MC_V8_CAP_PAIRWISE |
                            UFR_MC_V8_CAP_REPEATED_TRANSFORM |
                            UFR_MC_V8_CAP_PRECOMPUTE_SAFE |
                            UFR_MC_V8_CAP_LANE_LOCAL;
    const uint32_t p4need = UFR_MC_V8_CAP_SAMPLE_INDEPENDENT |
                            UFR_MC_V8_CAP_INDEPENDENT_PARAMS |
                            UFR_MC_V8_CAP_COMMON_TRANSFORM |
                            UFR_MC_V8_CAP_PRECOMPUTE_SAFE |
                            UFR_MC_V8_CAP_PRECOMPUTE_LOCAL_FAST |
                            UFR_MC_V8_CAP_PARAM4_PRECOMPUTE_PROVEN;
    out->r2_eligible = ((w.caps & r2need) == r2need);
    out->param4_eligible = ((w.caps & p4need) == p4need);

    if (req->requested == UFR_MC_V8_R2_CACHE8) {
        if (!(out->domain_compatibility_mask & UFR_MC_V9_COMPAT_R2)) return -3;
        if (!out->r2_eligible) return -2;
    }

    if (req->requested != UFR_MC_V8_AUTO)
        return ufr_mc_v8_select(&w, req->requested, &out->plan);

    if (out->r2_eligible) {
        out->plan.requested = UFR_MC_V8_AUTO;
        out->plan.selected = UFR_MC_V8_R2_CACHE8;
        out->plan.preferred_lanes = 8u;
        out->plan.requires_precompute = 1u;
        out->plan.reason_mask = UFR_MC_V8_REASON_R2;
        out->plan.score = 100u;
        return 0;
    }

    const int heavy = (req->workload.transform_cost_estimate >= 60u);
    const uint32_t invneed = p4need |
                             UFR_MC_V8_CAP_INVARIANT_PREP_SAFE |
                             UFR_MC_V8_CAP_INVARIANT_PREP_PROVEN;
    if (req->domain != UFR_MC_V9_DOMAIN_GENERIC && heavy &&
        req->workload.params_per_batch >= 8u &&
        ((w.caps & invneed) == invneed)) {
        out->plan.requested = UFR_MC_V8_AUTO;
        out->plan.selected = UFR_MC_V8_PARAM4_PRECOMPUTE_INVARIANT;
        out->plan.preferred_lanes = 4u;
        out->plan.requires_precompute = 1u;
        out->plan.reason_mask = UFR_MC_V8_REASON_PARAM |
                                UFR_MC_V8_REASON_COMMON |
                                UFR_MC_V8_REASON_HEAVY |
                                UFR_MC_V8_REASON_FAST_PRECACHE |
                                UFR_MC_V8_REASON_BENCH_PROVEN |
                                UFR_MC_V8_REASON_INVARIANT_PREP;
        out->plan.score = 97u;
        return 0;
    }

    return ufr_mc_v8_select(&w, UFR_MC_V8_AUTO, &out->plan);
}

int ufr_mc_v9_select(const ufr_mc_v9_request_t *req, ufr_mc_v9_plan_t *out)
{
    return ufr_mc_v9_embedded_select(req,out);
}

const char *ufr_mc_v9_domain_name(ufr_mc_v9_domain_t domain)
{
    switch(domain){
        case UFR_MC_V9_DOMAIN_FINANCE: return "FINANCE";
        case UFR_MC_V9_DOMAIN_PHARMA: return "PHARMA";
        default: return "GENERIC";
    }
}

/* -------------------------------------------------------------------------
 * Integrated MC optimization layer
 * ------------------------------------------------------------------------- */

static inline __m512 ufr_mc_int_exp_approx_ps(__m512 x) {
    const __m512 max12 = _mm512_set1_ps(12.0f);
    const __m512 min12 = _mm512_set1_ps(-12.0f);
    const __m512 invln2 = _mm512_set1_ps(1.4426950408889634f);
    const __m512 ln2 = _mm512_set1_ps(0.6931471805599453f);
    x = _mm512_max_ps(min12, _mm512_min_ps(max12, x));
    __m512 q = _mm512_mul_ps(x, invln2);
    __m512 n = _mm512_roundscale_ps(q, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    __m512 r = _mm512_fnmadd_ps(n, ln2, x);
    __m512 p = _mm512_set1_ps(1.0f/40320.0f);
    p = _mm512_fmadd_ps(p, r, _mm512_set1_ps(1.0f/5040.0f));
    p = _mm512_fmadd_ps(p, r, _mm512_set1_ps(1.0f/720.0f));
    p = _mm512_fmadd_ps(p, r, _mm512_set1_ps(1.0f/120.0f));
    p = _mm512_fmadd_ps(p, r, _mm512_set1_ps(1.0f/24.0f));
    p = _mm512_fmadd_ps(p, r, _mm512_set1_ps(1.0f/6.0f));
    p = _mm512_fmadd_ps(p, r, _mm512_set1_ps(0.5f));
    p = _mm512_fmadd_ps(p, r, _mm512_set1_ps(1.0f));
    p = _mm512_fmadd_ps(p, r, _mm512_set1_ps(1.0f));
    return _mm512_scalef_ps(p, n);
}

static inline void ufr_mc_int_zero_result(ufr_mc_integrated_result_t *o) {
    if (o) memset(o, 0, sizeof(*o));
}

static inline __m512 ufr_mc_int_finance_terminal(__m512 z,
                                                  const ufr_mc_integrated_finance_cfg_t *c,
                                                  __m512 spot_v,
                                                  __m512 drift_v,
                                                  __m512 vsqrt_v) {
    (void)c;
    const __m512 x = _mm512_fmadd_ps(z, vsqrt_v, drift_v);
    return _mm512_mul_ps(spot_v, ufr_mc_int_exp_approx_ps(x));
}

/* Finance-only algebraic fold for the optimized precompute path.
 * Original: float_q * UF_NORMAL_SCALE -> z; z * vsqrt -> x.
 * Folded:   float_q * (UF_NORMAL_SCALE * vsqrt) -> x.
 * The factor is prepared once. This removes one vector multiply per q-vector
 * from the hot loop. Baseline/SHARED8 retain the original operation order. */
static inline __m512 ufr_mc_int_finance_terminal_folded(
    __m512 q_float,
    __m512 spot_v,
    __m512 drift_v,
    __m512 qfactor_v)
{
    const __m512 x = _mm512_fmadd_ps(q_float, qfactor_v, drift_v);
    return _mm512_mul_ps(spot_v, ufr_mc_int_exp_approx_ps(x));
}

static inline __m512 ufr_mc_int_call_payoff(__m512 s, __m512 strike) {
    const __m512 d = _mm512_sub_ps(s, strike);
    const __m512 zero = _mm512_setzero_ps();
    return _mm512_max_ps(zero, d);
}

typedef struct {
    ufr_mc_integrated_domain_t domain;
    ufr_mc_v8_plan_t plan;
    ufr_mc_integrated_finance_cfg_t fc;
    ufr_mc_integrated_pharma_cfg_t pc;
    __m512 finance_spot_v;
    __m512 finance_drift_v;
    __m512 finance_vsqrt_v;
    __m512 finance_qfactor_v;
    __m512 finance_strike_v[8];
    __m512 pharma_cl_v;
    __m512 pharma_v_v;
    __m512 pharma_neg_t_v;
    __m512 pharma_qfactor_cl_v;
    __m512 pharma_qfactor_v_v;
    __m512 pharma_dose_v[8];
    __m512 acc0[8];
    __m512 acc1[8];
    __m512 pharma_common_acc4[4];
    alignas(64) float transform[16384]; /* 64 KiB/view reserved; Finance path uses first 32 KiB */
    double scalar_sum[8];
    uint64_t samples;
    uint64_t blocks;
    uint64_t hits;
} ufr_mc_int_ctx_t;

static inline void ufr_mc_int_prepare_finance(ufr_mc_int_ctx_t *c) {
    const float vsqrt = c->fc.vol * sqrtf(c->fc.maturity);
    const float drift = (c->fc.rate - 0.5f*c->fc.vol*c->fc.vol) * c->fc.maturity;
    c->finance_spot_v = _mm512_set1_ps(c->fc.spot);
    c->finance_drift_v = _mm512_set1_ps(drift);
    c->finance_vsqrt_v = _mm512_set1_ps(vsqrt);
    c->finance_qfactor_v = _mm512_set1_ps(UF_NORMAL_SCALE * vsqrt);
    for (int k=0;k<8;k++) c->finance_strike_v[k] = _mm512_set1_ps(c->fc.strikes[k]);
}

static inline void ufr_mc_int_prepare_pharma(ufr_mc_int_ctx_t *c) {
    c->pharma_cl_v = _mm512_set1_ps(c->pc.clearance_lph);
    c->pharma_v_v = _mm512_set1_ps(c->pc.volume_l);
    c->pharma_neg_t_v = _mm512_set1_ps(-c->pc.obs_hour);
    /* Fold the q->Normal scale into the fixed log-normal coefficients for
     * the optimized q256 path. Baseline/SHARED8 keeps the original operation
     * order; only the precompute-safe optimized path uses these factors. */
    c->pharma_qfactor_cl_v = _mm512_set1_ps(UF_NORMAL_SCALE * 0.20f);
    c->pharma_qfactor_v_v  = _mm512_set1_ps(UF_NORMAL_SCALE * 0.10f);
    for (int k=0;k<8;k++) c->pharma_dose_v[k] = _mm512_set1_ps(c->pc.dose[k]);
}

static inline __m512 ufr_mc_int_pharma_exposure(__m512 zcl, __m512 zv,
                                                 const ufr_mc_int_ctx_t *c) {
    const __m512 cl = _mm512_mul_ps(c->pharma_cl_v,
        ufr_mc_int_exp_approx_ps(_mm512_mul_ps(zcl, _mm512_set1_ps(0.20f))));
    const __m512 v = _mm512_mul_ps(c->pharma_v_v,
        ufr_mc_int_exp_approx_ps(_mm512_mul_ps(zv, _mm512_set1_ps(0.10f))));
    const __m512 k = _mm512_div_ps(cl, v);
    const __m512 e = ufr_mc_int_exp_approx_ps(_mm512_mul_ps(k, c->pharma_neg_t_v));
    return _mm512_div_ps(e, v);
}

static inline __m512 ufr_mc_int_pharma_exposure_folded(__m512 qcl, __m512 qv,
                                                        const ufr_mc_int_ctx_t *c) {
    /* Original optimized path: q->z multiply, then z*0.20 / z*0.10.
     * Folded path: q*(scale*0.20) and q*(scale*0.10).
     * This removes two vector multiplies per q-pair while retaining the same
     * exp/div arithmetic and leaving the deliberate baseline untouched. */
    const __m512 cl = _mm512_mul_ps(c->pharma_cl_v,
        ufr_mc_int_exp_approx_ps(_mm512_mul_ps(qcl, c->pharma_qfactor_cl_v)));
    const __m512 v = _mm512_mul_ps(c->pharma_v_v,
        ufr_mc_int_exp_approx_ps(_mm512_mul_ps(qv, c->pharma_qfactor_v_v)));
    const __m512 k = _mm512_div_ps(cl, v);
    const __m512 e = ufr_mc_int_exp_approx_ps(_mm512_mul_ps(k, c->pharma_neg_t_v));
    return _mm512_div_ps(e, v);
}

static int ufr_mc_int_consume(const int32_t *q256,
                              uint64_t generation_id,
                              uint32_t view_id,
                              uint32_t block_id,
                              void *vctx) {
    (void)generation_id; (void)view_id; (void)block_id;
    ufr_mc_int_ctx_t *c = (ufr_mc_int_ctx_t *)vctx;
    if (!c || !q256) return -1;
    const __m512 scale = _mm512_set1_ps(UF_NORMAL_SCALE);

    if (c->domain == UFR_MC_DOMAIN_FINANCE) {
        if (c->plan.selected == UFR_MC_V8_BASELINE || c->plan.selected == UFR_MC_V8_VEC8) {
            /* Deliberate baseline: the expensive terminal transform is
             * repeated for each independent parameter, matching the demo
             * baseline and preserving the optimization comparison. */
            for (int k=0;k<8;k++) {
                for (unsigned line=0; line<8; ++line) {
                    const __m512 z = _mm512_mul_ps(_mm512_cvtepi32_ps(
                        _mm512_load_si512((const void *)(q256 + line*16u))), scale);
                    const __m512 st = ufr_mc_int_finance_terminal(z, &c->fc,
                        c->finance_spot_v, c->finance_drift_v, c->finance_vsqrt_v);
                    c->acc0[k] = _mm512_add_ps(c->acc0[k],
                        ufr_mc_int_call_payoff(st, c->finance_strike_v[k]));
                }
            }
        } else {
            /* SHARED8: one terminal transform, eight live payoff accumulators. */
            if (c->plan.selected == UFR_MC_V8_PARAM4_PRECOMPUTE ||
                c->plan.selected == UFR_MC_V8_PARAM4_PRECOMPUTE_INVARIANT) {
                /* Resource-safe PARAM4: materialize the common terminal transform
                 * once for the current 64 KiB PaperView, then consume it in two
                 * four-accumulator passes. This is deliberately view-local: the
                 * buffer never enters Front state, Back state, or the public MC
                 * metadata. It preserves the intended PRECOMPUTE architecture
                 * while avoiding eight live ZMM accumulators. */
                const unsigned view_id = block_id;
                (void)view_id;
                for (unsigned line=0; line<8; ++line) {
                    const __m512 qf = _mm512_cvtepi32_ps(
                        _mm512_load_si512((const void *)(q256 + line*16u)));
                    const __m512 st = ufr_mc_int_finance_terminal_folded(
                        qf, c->finance_spot_v, c->finance_drift_v,
                        c->finance_qfactor_v);
                    _mm512_store_ps(c->transform + block_id*128u + line*16u, st);
                }
                /* q256 has 8 terminal vectors = 128 floats per callback.
                 * The 64 callbacks forming a view therefore occupy 8192 floats. */
                if (block_id == 63u) {
                    for (int k0=0; k0<8; k0+=4) {
                        __m512 a0=_mm512_setzero_ps(), a1=_mm512_setzero_ps();
                        __m512 a2=_mm512_setzero_ps(), a3=_mm512_setzero_ps();
                        for (unsigned off=0; off<8192u; off+=16u) {
                            const __m512 st=_mm512_load_ps(c->transform+off);
                            a0=_mm512_add_ps(a0, ufr_mc_int_call_payoff(st, c->finance_strike_v[k0+0]));
                            a1=_mm512_add_ps(a1, ufr_mc_int_call_payoff(st, c->finance_strike_v[k0+1]));
                            a2=_mm512_add_ps(a2, ufr_mc_int_call_payoff(st, c->finance_strike_v[k0+2]));
                            a3=_mm512_add_ps(a3, ufr_mc_int_call_payoff(st, c->finance_strike_v[k0+3]));
                        }
                        c->scalar_sum[k0+0] += ufr_mc_reduce16_pd(a0);
                        c->scalar_sum[k0+1] += ufr_mc_reduce16_pd(a1);
                        c->scalar_sum[k0+2] += ufr_mc_reduce16_pd(a2);
                        c->scalar_sum[k0+3] += ufr_mc_reduce16_pd(a3);
                    }
                }
            } else {
                for (unsigned line=0; line<8; ++line) {
                    const __m512 z = _mm512_mul_ps(_mm512_cvtepi32_ps(
                        _mm512_load_si512((const void *)(q256 + line*16u))), scale);
                    const __m512 st = ufr_mc_int_finance_terminal(z, &c->fc,
                        c->finance_spot_v, c->finance_drift_v, c->finance_vsqrt_v);
                    for (int k=0;k<8;k++)
                        c->acc0[k] = _mm512_add_ps(c->acc0[k],
                            ufr_mc_int_call_payoff(st, c->finance_strike_v[k]));
                }
            }
        }
        c->samples += 128u;
    } else if (c->domain == UFR_MC_DOMAIN_PHARMA) {
        if (c->plan.selected == UFR_MC_V8_BASELINE || c->plan.selected == UFR_MC_V8_VEC8) {
            /* Deliberate baseline: recompute the common PK transform for each
             * dose, matching the earlier demo's baseline definition. */
            for (int k=0;k<8;k++) {
                for (unsigned line=0; line<8; ++line) {
                    const __m512 zcl = _mm512_mul_ps(_mm512_cvtepi32_ps(
                        _mm512_load_si512((const void *)(q256 + line*16u))), scale);
                    const __m512 zv = _mm512_mul_ps(_mm512_cvtepi32_ps(
                        _mm512_load_si512((const void *)(q256 + (8u+line)*16u))), scale);
                    const __m512 e = ufr_mc_int_pharma_exposure(zcl, zv, c);
                    c->acc0[k] = _mm512_add_ps(c->acc0[k],
                        _mm512_mul_ps(c->pharma_dose_v[k], e));
                }
            }
        } else {
            /* Optimized Pharma path: operate from q directly and fold
             * UF_NORMAL_SCALE into the fixed log-normal coefficients. */
            if (c->plan.selected == UFR_MC_V8_PARAM4_PRECOMPUTE_INVARIANT) {
                /* The eight doses are sample-independent scalars applied to
                 * one common exposure. Four partial exposure sums remove the
                 * seven dose-specific multiply+add dependency chains without
                 * putting a single very long FP32 sum on one accumulator. */
                for (unsigned line=0; line<8; ++line) {
                    const __m512 qcl = _mm512_cvtepi32_ps(
                        _mm512_load_si512((const void *)(q256 + line*16u)));
                    const __m512 qv = _mm512_cvtepi32_ps(
                        _mm512_load_si512((const void *)(q256 + (8u+line)*16u)));
                    const __m512 e = ufr_mc_int_pharma_exposure_folded(qcl, qv, c);
                    c->pharma_common_acc4[line & 3u] = _mm512_add_ps(
                        c->pharma_common_acc4[line & 3u], e);
                }
            } else if (c->plan.selected == UFR_MC_V8_PARAM4_PRECOMPUTE) {
                for (unsigned line=0; line<8; ++line) {
                    const __m512 qcl = _mm512_cvtepi32_ps(
                        _mm512_load_si512((const void *)(q256 + line*16u)));
                    const __m512 qv = _mm512_cvtepi32_ps(
                        _mm512_load_si512((const void *)(q256 + (8u+line)*16u)));
                    const __m512 e = ufr_mc_int_pharma_exposure_folded(qcl, qv, c);
                    for (int k=0;k<8;k++)
                        c->acc0[k] = _mm512_add_ps(c->acc0[k],
                            _mm512_mul_ps(c->pharma_dose_v[k], e));
                }
            } else {
                for (unsigned line=0; line<8; ++line) {
                    const __m512 zcl = _mm512_mul_ps(_mm512_cvtepi32_ps(
                        _mm512_load_si512((const void *)(q256 + line*16u))), scale);
                    const __m512 zv = _mm512_mul_ps(_mm512_cvtepi32_ps(
                        _mm512_load_si512((const void *)(q256 + (8u+line)*16u))), scale);
                    const __m512 e = ufr_mc_int_pharma_exposure(zcl, zv, c);
                    for (int k=0;k<8;k++)
                        c->acc0[k] = _mm512_add_ps(c->acc0[k],
                            _mm512_mul_ps(c->pharma_dose_v[k], e));
                }
            }
        }
        c->samples += 128u;
    } else return -2;
    c->blocks++;
    return 0;
}

int ufr_paperview_mc_run_domain(
    ufr_paperview_mc_engine_t *engine,
    ufr_mc_integrated_domain_t domain,
    const ufr_mc_integrated_finance_cfg_t *finance,
    const ufr_mc_integrated_pharma_cfg_t *pharma,
    ufr_mc_v8_workload_t workload,
    ufr_mc_v8_profile_id_t requested_profile,
    uint64_t chunks,
    ufr_mc_integrated_result_t *out) {
    if (!engine || chunks==0u || !out) return -1;
    if (domain==UFR_MC_DOMAIN_FINANCE && !finance) return -2;
    if (domain==UFR_MC_DOMAIN_PHARMA && !pharma) return -3;
    if (domain!=UFR_MC_DOMAIN_FINANCE && domain!=UFR_MC_DOMAIN_PHARMA) return -4;

    ufr_mc_int_ctx_t c;
    memset(&c,0,sizeof(c));
    c.domain=domain;
    if (finance) c.fc=*finance;
    if (pharma) c.pc=*pharma;
    if (domain==UFR_MC_DOMAIN_FINANCE) ufr_mc_int_prepare_finance(&c);
    else ufr_mc_int_prepare_pharma(&c);
    ufr_mc_v9_request_t v9req;
    memset(&v9req, 0, sizeof(v9req));
    v9req.domain = (domain==UFR_MC_DOMAIN_FINANCE) ? UFR_MC_V9_DOMAIN_FINANCE : UFR_MC_V9_DOMAIN_PHARMA;
    v9req.workload = workload;
    v9req.requested = requested_profile;
    ufr_mc_v9_plan_t v9plan;
    if (ufr_mc_v9_embedded_select(&v9req, &v9plan)!=0) return -5;
    c.plan = v9plan.plan;
    if (c.plan.selected==UFR_MC_V8_R2_CACHE8 || c.plan.selected==UFR_MC_V8_PATH8_BLOCK ||
        c.plan.selected==UFR_MC_V8_PATH4X2 || c.plan.selected==UFR_MC_V8_PATH4) return -6;
    for (int k=0;k<8;k++){c.acc0[k]=_mm512_setzero_ps();c.acc1[k]=_mm512_setzero_ps();c.scalar_sum[k]=0.0;}
    for (int k=0;k<4;k++) c.pharma_common_acc4[k]=_mm512_setzero_ps();

    const uint64_t t0=now_ns();
    int rc=ufr_mc_run(&engine->impl,chunks,ufr_mc_int_consume,&c);
    const uint64_t dt=now_ns()-t0;
    if (rc!=0) return -7;
    ufr_mc_int_zero_result(out);
    out->samples=c.samples;
    out->blocks=c.blocks;
    out->selected_profile=c.plan.selected;
    out->preferred_lanes=c.plan.preferred_lanes;
    out->requires_precompute=c.plan.requires_precompute;
    out->aux[0]=(double)dt/1e6;
    for (int k=0;k<8;k++){
        if (c.plan.selected == UFR_MC_V8_PARAM4_PRECOMPUTE ||
            c.plan.selected == UFR_MC_V8_PARAM4_PRECOMPUTE_INVARIANT) {
            if (domain == UFR_MC_DOMAIN_PHARMA &&
                c.plan.selected == UFR_MC_V8_PARAM4_PRECOMPUTE_INVARIANT) {
                const double e_sum =
                    ufr_mc_reduce16_pd(c.pharma_common_acc4[0]) +
                    ufr_mc_reduce16_pd(c.pharma_common_acc4[1]) +
                    ufr_mc_reduce16_pd(c.pharma_common_acc4[2]) +
                    ufr_mc_reduce16_pd(c.pharma_common_acc4[3]);
                out->sum[k]=e_sum * (double)c.pc.dose[k];
            } else {
                out->sum[k]=c.scalar_sum[k];
            }
        } else {
            out->sum[k]=ufr_mc_reduce16_pd(c.acc0[k]);
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Integrated Pharma linear-transition / piecewise-dosing adapter.
 *
 * This is deliberately an explicit/manual domain path.  It does not enter
 * the generic MC v9 selector and does not touch Front/Back state.  The hot
 * loop consumes prepared subject-specific M/G transition data and common
 * segment inputs only.
 * ------------------------------------------------------------------------- */

int ufr_pharma_linear_piecewise_plan_valid(
    const ufr_pharma_linear_piecewise_plan_t *p)
{
    if (!p) return 0;
    if (p->dim != 3u && p->dim != 4u) return 0;
    if (p->time_steps == 0u || p->segments == 0u || p->dt <= 0.0f) return 0;
    if ((p->time_steps % p->segments) != 0u) return 0;
    if (!p->subject_specific || !p->piecewise_constant_input ||
        !p->boundary_bolus || !p->manual_only) return 0;
    return 1;
}

static int ufr_pharma_piecewise_plan_valid_internal(
    unsigned dim, size_t subjects, unsigned time_steps, unsigned segments,
    float dt, float volume)
{
    ufr_pharma_linear_piecewise_plan_t p;
    memset(&p, 0, sizeof(p));
    p.dim = dim;
    p.time_steps = time_steps;
    p.segments = segments;
    p.dt = dt;
    p.subject_specific = 1;
    p.piecewise_constant_input = 1;
    p.boundary_bolus = 1;
    p.manual_only = 1;
    if (!ufr_pharma_linear_piecewise_plan_valid(&p)) return 0;
    if (subjects == 0u || !isfinite(volume) || volume <= 0.0f) return 0;
    return 1;
}

static int ufr_pharma_pw_solve_small(double *A, double *b, int d)
{
    const double eps = 1.0e-14;
    for (int k = 0; k < d; ++k) {
        int piv = k;
        double best = fabs(A[k*d+k]);
        for (int r = k + 1; r < d; ++r) {
            double v = fabs(A[r*d+k]);
            if (v > best) { best = v; piv = r; }
        }
        if (best < eps) return -1;
        if (piv != k) {
            for (int c = k; c < d; ++c) {
                double t = A[k*d+c]; A[k*d+c] = A[piv*d+c]; A[piv*d+c] = t;
            }
            double tb = b[k]; b[k] = b[piv]; b[piv] = tb;
        }
        double diag = A[k*d+k];
        for (int r = k + 1; r < d; ++r) {
            double f = A[r*d+k] / diag;
            A[r*d+k] = 0.0;
            for (int c = k + 1; c < d; ++c) A[r*d+c] -= f * A[k*d+c];
            b[r] -= f * b[k];
        }
    }
    for (int i = d - 1; i >= 0; --i) {
        if (fabs(A[i*d+i]) < eps) return -1;
        for (int c = i + 1; c < d; ++c) b[i] -= A[i*d+c] * b[c];
        b[i] /= A[i*d+i];
    }
    return 0;
}

static void ufr_pharma_pw_mat_mul(const double *A, const double *B,
                                   double *C, int d)
{
    for (int i = 0; i < d; ++i) {
        for (int j = 0; j < d; ++j) {
            double s = 0.0;
            for (int k = 0; k < d; ++k) s += A[i*d+k] * B[k*d+j];
            C[i*d+j] = s;
        }
    }
}

static double ufr_pharma_pw_norm1(const double *A, int d)
{
    double m = 0.0;
    for (int j = 0; j < d; ++j) {
        double s = 0.0;
        for (int i = 0; i < d; ++i) s += fabs(A[i*d+j]);
        if (s > m) m = s;
    }
    return m;
}

static void ufr_pharma_pw_mat_exp_taylor(const double *A0, double *E, int d)
{
    double A[16], term[16] = {0}, sum[16] = {0}, tmp[16], next[16];
    const int nn = d * d;
    memcpy(A, A0, (size_t)nn * sizeof(double));
    double n1 = ufr_pharma_pw_norm1(A, d);
    int s = 0;
    while (n1 > 0.5 && s < 12) {
        for (int i = 0; i < nn; ++i) A[i] *= 0.5;
        n1 *= 0.5;
        ++s;
    }
    for (int i = 0; i < d; ++i) {
        term[i*d+i] = 1.0;
        sum[i*d+i] = 1.0;
    }
    double invfact = 1.0;
    for (int k = 1; k <= 22; ++k) {
        ufr_pharma_pw_mat_mul(term, A, tmp, d);
        memcpy(term, tmp, (size_t)nn * sizeof(double));
        invfact /= (double)k;
        for (int i = 0; i < nn; ++i) sum[i] += term[i] * invfact;
    }
    for (int q = 0; q < s; ++q) {
        ufr_pharma_pw_mat_mul(sum, sum, next, d);
        memcpy(sum, next, (size_t)nn * sizeof(double));
    }
    memcpy(E, sum, (size_t)nn * sizeof(double));
}

static __attribute__((unused)) void ufr_pharma_pw_build_A(const float *kb, size_t n, size_t i,
                                  unsigned dim, double *A)
{
    memset(A, 0, 16 * sizeof(double));
    if (dim == 3u) {
        const float k10 = kb[0*n+i], k12 = kb[1*n+i], k21 = kb[2*n+i];
        const float k23 = kb[3*n+i], k32 = kb[4*n+i];
        A[0] = -(double)(k10 + k12); A[1] = k21;
        A[3] = k12; A[4] = -(double)(k21 + k23); A[5] = k32;
        A[7] = k23; A[8] = -(double)k32;
    } else {
        const float k10 = kb[0*n+i], k12 = kb[1*n+i], k21 = kb[2*n+i];
        const float k23 = kb[3*n+i], k32 = kb[4*n+i];
        const float k34 = kb[5*n+i], k43 = kb[6*n+i];
        A[0] = -(double)(k10 + k12); A[1] = k21;
        A[4] = k12; A[5] = -(double)(k21 + k23); A[6] = k32;
        A[9] = k23; A[10] = -(double)(k32 + k34); A[11] = k43;
        A[14] = k34; A[15] = -(double)k43;
    }
}

static void *ufr_pharma_pw_alloc64(size_t bytes)
{
    if (bytes == 0u || bytes > SIZE_MAX - 63u) return NULL;
    const size_t rounded = (bytes + 63u) & ~(size_t)63u;
    return aligned_alloc(64u, rounded);
}


static int ufr_pharma_pw_prepare3(ufr_pharma_linear_piecewise_ctx_t *ctx,
                                  const float *kb)
{
    const size_t n = ctx->subjects;
    for (size_t i = 0; i < n; ++i) {
        const float k10=kb[0*n+i], k12=kb[1*n+i], k21=kb[2*n+i];
        const float k23=kb[3*n+i], k32=kb[4*n+i];
        if (!isfinite(k10)||!isfinite(k12)||!isfinite(k21)||!isfinite(k23)||!isfinite(k32) ||
            k10<0.0f||k12<0.0f||k21<0.0f||k23<0.0f||k32<0.0f) return -5;
        if (!(k10 > 0.0f)) return -6;
        double A[16]={0}, B[16]={0}, E[16]={0}, Ac[16]={0}, rhs[4]={0};
        A[0]=-(double)(k10+k12); A[1]=k21;
        A[3]=k12; A[4]=-(double)(k21+k23); A[5]=k32;
        A[7]=k23; A[8]=-(double)k32;
        for (unsigned j=0;j<9u;++j) B[j]=A[j]*(double)ctx->dt;
        ufr_pharma_pw_mat_exp_taylor(B,E,3);
        for (unsigned j=0;j<9u;++j) ctx->M[(size_t)j*n+i]=(float)E[j];
        rhs[0]=E[0]-1.0; rhs[1]=E[3]; rhs[2]=E[6];
        memcpy(Ac,A,9*sizeof(double));
        if (ufr_pharma_pw_solve_small(Ac,rhs,3)!=0) return -7;
        ctx->G[0*n+i]=(float)rhs[0]; ctx->G[1*n+i]=(float)rhs[1]; ctx->G[2*n+i]=(float)rhs[2];
    }
    return 0;
}

static int ufr_pharma_pw_prepare4(ufr_pharma_linear_piecewise_ctx_t *ctx,
                                  const float *kb)
{
    const size_t n = ctx->subjects;
    for (size_t i = 0; i < n; ++i) {
        const float k10=kb[0*n+i], k12=kb[1*n+i], k21=kb[2*n+i];
        const float k23=kb[3*n+i], k32=kb[4*n+i], k34=kb[5*n+i], k43=kb[6*n+i];
        if (!isfinite(k10)||!isfinite(k12)||!isfinite(k21)||!isfinite(k23)||!isfinite(k32)||
            !isfinite(k34)||!isfinite(k43) ||
            k10<0.0f||k12<0.0f||k21<0.0f||k23<0.0f||k32<0.0f||k34<0.0f||k43<0.0f) return -5;
        if (!(k10 > 0.0f)) return -6;
        double A[16]={0}, B[16]={0}, E[16]={0}, Ac[16]={0}, rhs[4]={0};
        A[0]=-(double)(k10+k12); A[1]=k21;
        A[4]=k12; A[5]=-(double)(k21+k23); A[6]=k32;
        A[9]=k23; A[10]=-(double)(k32+k34); A[11]=k43;
        A[14]=k34; A[15]=-(double)k43;
        for (unsigned j=0;j<16u;++j) B[j]=A[j]*(double)ctx->dt;
        ufr_pharma_pw_mat_exp_taylor(B,E,4);
        for (unsigned j=0;j<16u;++j) ctx->M[(size_t)j*n+i]=(float)E[j];
        rhs[0]=E[0]-1.0; rhs[1]=E[4]; rhs[2]=E[8]; rhs[3]=E[12];
        memcpy(Ac,A,16*sizeof(double));
        if (ufr_pharma_pw_solve_small(Ac,rhs,4)!=0) return -7;
        ctx->G[0*n+i]=(float)rhs[0]; ctx->G[1*n+i]=(float)rhs[1];
        ctx->G[2*n+i]=(float)rhs[2]; ctx->G[3*n+i]=(float)rhs[3];
    }
    return 0;
}

int ufr_pharma_linear_piecewise_ctx_init(
    ufr_pharma_linear_piecewise_ctx_t *ctx,
    unsigned dim,
    size_t subjects,
    unsigned time_steps,
    unsigned segments,
    float dt,
    float volume,
    const float *k_blocks)
{
    if (!ctx || !k_blocks) return -1;
    if (!ufr_pharma_piecewise_plan_valid_internal(
            dim, subjects, time_steps, segments, dt, volume)) return -2;
    if (dim == 3u && subjects > SIZE_MAX / (9u * sizeof(float))) return -3;
    if (dim == 4u && subjects > SIZE_MAX / (16u * sizeof(float))) return -3;
    if (subjects > SIZE_MAX / ((size_t)dim * sizeof(float))) return -3;

    memset(ctx, 0, sizeof(*ctx));
    ctx->dim = dim;
    ctx->subjects = subjects;
    ctx->time_steps = time_steps;
    ctx->segments = segments;
    ctx->dt = dt;
    ctx->volume = volume;

    const size_t m_elems = (size_t)dim * (size_t)dim * subjects;
    const size_t g_elems = (size_t)dim * subjects;
    ctx->M = (float *)ufr_pharma_pw_alloc64(m_elems * sizeof(float));
    ctx->G = (float *)ufr_pharma_pw_alloc64(g_elems * sizeof(float));
    if (!ctx->M || !ctx->G) {
        ufr_pharma_linear_piecewise_ctx_destroy(ctx);
        return -4;
    }

    return (dim == 3u) ? ufr_pharma_pw_prepare3(ctx, k_blocks)
                       : ufr_pharma_pw_prepare4(ctx, k_blocks);
}

void ufr_pharma_linear_piecewise_ctx_destroy(
    ufr_pharma_linear_piecewise_ctx_t *ctx)
{
    if (!ctx) return;
    free(ctx->M);
    free(ctx->G);
    memset(ctx, 0, sizeof(*ctx));
}

static int ufr_pharma_pw_run3(const ufr_pharma_linear_piecewise_ctx_t *ctx,
                              const float *initial,
                              const float *rate,
                              const float *bolus,
                              float *auc,
                              float *cmax)
{
    const size_t n = ctx->subjects;
    const unsigned nt = ctx->time_steps / ctx->segments;
    const __m512 dt = _mm512_set1_ps(ctx->dt);
    const __m512 invv = _mm512_set1_ps(1.0f / ctx->volume);
    const __m512 z = _mm512_setzero_ps();
    for (size_t i = 0; i < n; i += 16u) {
        __m512 x0 = _mm512_loadu_ps(initial + i);
        __m512 x1 = _mm512_loadu_ps(initial + n + i);
        __m512 x2 = _mm512_loadu_ps(initial + 2u*n + i);
        __m512 aa = z, mm = z;
        const __m512 m00=_mm512_load_ps(ctx->M + 0u*n+i), m01=_mm512_load_ps(ctx->M + 1u*n+i), m02=_mm512_load_ps(ctx->M + 2u*n+i);
        const __m512 m10=_mm512_load_ps(ctx->M + 3u*n+i), m11=_mm512_load_ps(ctx->M + 4u*n+i), m12=_mm512_load_ps(ctx->M + 5u*n+i);
        const __m512 m20=_mm512_load_ps(ctx->M + 6u*n+i), m21=_mm512_load_ps(ctx->M + 7u*n+i), m22=_mm512_load_ps(ctx->M + 8u*n+i);
        const __m512 g0=_mm512_load_ps(ctx->G + 0u*n+i), g1=_mm512_load_ps(ctx->G + 1u*n+i), g2=_mm512_load_ps(ctx->G + 2u*n+i);
        for (unsigned s = 0; s < ctx->segments; ++s) {
            x0 = _mm512_add_ps(x0, _mm512_set1_ps(bolus[s]));
            const __m512 r = _mm512_set1_ps(rate[s]);
            const __m512 q0 = _mm512_mul_ps(g0, r), q1 = _mm512_mul_ps(g1, r), q2 = _mm512_mul_ps(g2, r);
            for (unsigned t = 0; t < nt; ++t) {
                const __m512 y0 = _mm512_add_ps(_mm512_fmadd_ps(m02,x2,_mm512_fmadd_ps(m01,x1,_mm512_mul_ps(m00,x0))),q0);
                const __m512 y1 = _mm512_add_ps(_mm512_fmadd_ps(m12,x2,_mm512_fmadd_ps(m11,x1,_mm512_mul_ps(m10,x0))),q1);
                const __m512 y2 = _mm512_add_ps(_mm512_fmadd_ps(m22,x2,_mm512_fmadd_ps(m21,x1,_mm512_mul_ps(m20,x0))),q2);
                x0=y0; x1=y1; x2=y2;
                const __m512 c = _mm512_mul_ps(x0, invv);
                aa = _mm512_fmadd_ps(c, dt, aa);
                mm = _mm512_max_ps(mm, c);
            }
        }
        _mm512_storeu_ps(auc + i, aa);
        _mm512_storeu_ps(cmax + i, mm);
    }
    return 0;
}

static int ufr_pharma_pw_run4(const ufr_pharma_linear_piecewise_ctx_t *ctx,
                              const float *initial,
                              const float *rate,
                              const float *bolus,
                              float *auc,
                              float *cmax)
{
    const size_t n = ctx->subjects;
    const unsigned nt = ctx->time_steps / ctx->segments;
    const __m512 dt = _mm512_set1_ps(ctx->dt);
    const __m512 invv = _mm512_set1_ps(1.0f / ctx->volume);
    const __m512 z = _mm512_setzero_ps();
    for (size_t i = 0; i < n; i += 16u) {
        __m512 x0 = _mm512_loadu_ps(initial + i);
        __m512 x1 = _mm512_loadu_ps(initial + n + i);
        __m512 x2 = _mm512_loadu_ps(initial + 2u*n + i);
        __m512 x3 = _mm512_loadu_ps(initial + 3u*n + i);
        __m512 aa = z, mm = z;
        __m512 m[16], g[4];
        for (unsigned j = 0; j < 16u; ++j) m[j] = _mm512_load_ps(ctx->M + (size_t)j*n + i);
        for (unsigned j = 0; j < 4u; ++j) g[j] = _mm512_load_ps(ctx->G + (size_t)j*n + i);
        for (unsigned s = 0; s < ctx->segments; ++s) {
            x0 = _mm512_add_ps(x0, _mm512_set1_ps(bolus[s]));
            const __m512 r = _mm512_set1_ps(rate[s]);
            const __m512 q0=_mm512_mul_ps(g[0],r), q1=_mm512_mul_ps(g[1],r), q2=_mm512_mul_ps(g[2],r), q3=_mm512_mul_ps(g[3],r);
            for (unsigned t = 0; t < nt; ++t) {
                const __m512 y0 = _mm512_add_ps(_mm512_fmadd_ps(m[3],x3,_mm512_fmadd_ps(m[2],x2,_mm512_fmadd_ps(m[1],x1,_mm512_mul_ps(m[0],x0)))),q0);
                const __m512 y1 = _mm512_add_ps(_mm512_fmadd_ps(m[7],x3,_mm512_fmadd_ps(m[6],x2,_mm512_fmadd_ps(m[5],x1,_mm512_mul_ps(m[4],x0)))),q1);
                const __m512 y2 = _mm512_add_ps(_mm512_fmadd_ps(m[11],x3,_mm512_fmadd_ps(m[10],x2,_mm512_fmadd_ps(m[9],x1,_mm512_mul_ps(m[8],x0)))),q2);
                const __m512 y3 = _mm512_add_ps(_mm512_fmadd_ps(m[15],x3,_mm512_fmadd_ps(m[14],x2,_mm512_fmadd_ps(m[13],x1,_mm512_mul_ps(m[12],x0)))),q3);
                x0=y0; x1=y1; x2=y2; x3=y3;
                const __m512 c = _mm512_mul_ps(x0, invv);
                aa = _mm512_fmadd_ps(c, dt, aa);
                mm = _mm512_max_ps(mm, c);
            }
        }
        _mm512_storeu_ps(auc + i, aa);
        _mm512_storeu_ps(cmax + i, mm);
    }
    return 0;
}

int ufr_pharma_linear_piecewise_run(
    const ufr_pharma_linear_piecewise_ctx_t *ctx,
    const float *initial_blocks,
    const float *rate_by_segment,
    const float *bolus_by_segment,
    float *auc,
    float *cmax)
{
    if (!ctx || !ctx->M || !ctx->G || !initial_blocks ||
        !rate_by_segment || !bolus_by_segment || !auc || !cmax) return -1;
    if (!ufr_pharma_piecewise_plan_valid_internal(
            ctx->dim, ctx->subjects, ctx->time_steps, ctx->segments,
            ctx->dt, ctx->volume)) return -2;
    /* Hot-path rule: do not rescan subject inputs or the schedule here.
     * Parameter finiteness is validated during ctx_init; callers that require
     * runtime data validation should perform it before entering this API. */
    if ((ctx->subjects & 15u) != 0u) return -3;
    return (ctx->dim == 3u)
        ? ufr_pharma_pw_run3(ctx, initial_blocks, rate_by_segment, bolus_by_segment, auc, cmax)
        : ufr_pharma_pw_run4(ctx, initial_blocks, rate_by_segment, bolus_by_segment, auc, cmax);
}



/* -------------------------------------------------------------------------
 * Independent/manual standard 2-compartment PK adapter.
 *
 * This deliberately uses a separate context so the established 3C/4C
 * ufr_pharma_linear_piecewise_ctx_t ABI and hot path are untouched.
 * State is in amounts; concentration outputs use subject-specific 1/Vc.
 * M/G are prepared once and reused across Monte-Carlo repetitions.
 * ------------------------------------------------------------------------- */
typedef struct {
    size_t subjects;
    unsigned time_steps;
    unsigned segments;
    float dt;
    float *M;
    float *G;
    float *inv_vc;
} ufr_pharma_2c_pk_piecewise_ctx_t;

void ufr_pharma_2c_pk_piecewise_ctx_destroy(ufr_pharma_2c_pk_piecewise_ctx_t *ctx);

static inline __m512 ufr_pharma_2c_exp_approx(__m512 x)
{
    const __m512 max12=_mm512_set1_ps(12.0f), min12=_mm512_set1_ps(-12.0f);
    const __m512 invln2=_mm512_set1_ps(1.4426950408889634f), ln2=_mm512_set1_ps(0.6931471805599453f);
    x=_mm512_max_ps(min12,_mm512_min_ps(max12,x));
    __m512 q=_mm512_mul_ps(x,invln2);
    __m512 n=_mm512_roundscale_ps(q,_MM_FROUND_TO_NEAREST_INT|_MM_FROUND_NO_EXC);
    __m512 r=_mm512_fnmadd_ps(n,ln2,x);
    __m512 p=_mm512_set1_ps(1.0f/40320.0f);
    p=_mm512_fmadd_ps(p,r,_mm512_set1_ps(1.0f/5040.0f));
    p=_mm512_fmadd_ps(p,r,_mm512_set1_ps(1.0f/720.0f));
    p=_mm512_fmadd_ps(p,r,_mm512_set1_ps(1.0f/120.0f));
    p=_mm512_fmadd_ps(p,r,_mm512_set1_ps(1.0f/24.0f));
    p=_mm512_fmadd_ps(p,r,_mm512_set1_ps(1.0f/6.0f));
    p=_mm512_fmadd_ps(p,r,_mm512_set1_ps(0.5f));
    p=_mm512_fmadd_ps(p,r,_mm512_set1_ps(1.0f));
    p=_mm512_fmadd_ps(p,r,_mm512_set1_ps(1.0f));
    return _mm512_scalef_ps(p,n);
}

/* -------------------------------------------------------------------------
 * Explicit/manual Pharma 2C biexponential acceleration.
 * Fixed linear IV-bolus model with equally spaced observations.  The coupled
 * two-state system is rewritten into two independent modal decays.
 * ------------------------------------------------------------------------- */
int ufr_pharma_2c_biexp_run(
    const float *cl, const float *vc, const float *vp, const float *q,
    size_t subjects, unsigned time_steps, float dt, float dose,
    float *out_auc, float *out_cmax)
{
    if (!cl || !vc || !vp || !q || !out_auc || !out_cmax ||
        subjects == 0u || (subjects & 15u) != 0u || time_steps == 0u ||
        dt <= 0.0f || dose <= 0.0f) return -1;

    const __m512 zero = _mm512_setzero_ps();
    const __m512 one = _mm512_set1_ps(1.0f);
    const __m512 half = _mm512_set1_ps(0.5f);
    const __m512 four = _mm512_set1_ps(4.0f);
    const __m512 dtv = _mm512_set1_ps(dt);
    const __m512 dosev = _mm512_set1_ps(dose);

    for (size_t i = 0; i < subjects; i += 16u) {
        const __m512 CL = _mm512_loadu_ps(cl + i);
        const __m512 VC = _mm512_loadu_ps(vc + i);
        const __m512 VP = _mm512_loadu_ps(vp + i);
        const __m512 Q  = _mm512_loadu_ps(q + i);
        if (_mm512_cmp_ps_mask(VC, zero, _CMP_LE_OQ) != 0u ||
            _mm512_cmp_ps_mask(VP, zero, _CMP_LE_OQ) != 0u ||
            _mm512_cmp_ps_mask(CL, zero, _CMP_LT_OQ) != 0u ||
            _mm512_cmp_ps_mask(Q, zero, _CMP_LT_OQ) != 0u) return -2;

        const __m512 k10 = _mm512_div_ps(CL, VC);
        const __m512 k12 = _mm512_div_ps(Q, VC);
        const __m512 k21 = _mm512_div_ps(Q, VP);
        const __m512 sum = _mm512_add_ps(_mm512_add_ps(k10, k12), k21);
        const __m512 disc = _mm512_sqrt_ps(_mm512_max_ps(zero,
            _mm512_sub_ps(_mm512_mul_ps(sum, sum),
                          _mm512_mul_ps(four, _mm512_mul_ps(k10, k21)))));
        const __m512 alpha = _mm512_mul_ps(half, _mm512_add_ps(sum, disc));
        const __m512 beta  = _mm512_mul_ps(half, _mm512_sub_ps(sum, disc));
        const __m512 den = _mm512_sub_ps(alpha, beta);
        if (_mm512_cmp_ps_mask(den, _mm512_set1_ps(1.0e-12f), _CMP_LE_OQ) != 0u)
            return -3;

        const __m512 invvc_dose = _mm512_div_ps(dosev, VC);
        const __m512 A = _mm512_mul_ps(invvc_dose,
            _mm512_div_ps(_mm512_sub_ps(alpha, k21), den));
        const __m512 B = _mm512_mul_ps(invvc_dose,
            _mm512_div_ps(_mm512_sub_ps(k21, beta), den));
        const __m512 ra = ufr_pharma_2c_exp_approx(
            _mm512_mul_ps(_mm512_sub_ps(zero, alpha), dtv));
        const __m512 rb = ufr_pharma_2c_exp_approx(
            _mm512_mul_ps(_mm512_sub_ps(zero, beta), dtv));

        __m512 pa = one, pb = one;
        __m512 auc = zero, cmax = zero;
        for (unsigned t = 0; t < time_steps; ++t) {
            pa = _mm512_mul_ps(pa, ra);
            pb = _mm512_mul_ps(pb, rb);
            const __m512 C = _mm512_fmadd_ps(A, pa, _mm512_mul_ps(B, pb));
            auc = _mm512_fmadd_ps(C, dtv, auc);
            cmax = _mm512_max_ps(cmax, C);
        }
        _mm512_storeu_ps(out_auc + i, auc);
        _mm512_storeu_ps(out_cmax + i, cmax);
    }
    return 0;
}


/* -------------------------------------------------------------------------
 * Explicit/manual Pharma 2C biexponential closed-form output path.
 *
 * For the standard IV-bolus linear 2C model with positive macro coefficients,
 * concentration is
 *   C_n = A*ra^n + B*rb^n,  n=1..N
 * where ra=exp(-alpha*dt), rb=exp(-beta*dt).
 *
 * Therefore the sampled AUC can be evaluated with two geometric sums and the
 * sampled Cmax is C_1 (A,B >= 0, 0 < rb <= ra <= 1).  This removes the N-step
 * recurrence entirely.  It is intentionally a separate manual API because
 * it relies on the standard IV-bolus positivity/monotonicity conditions.
 * The one-step decay factors use the same exp approximation as the existing
 * 2C biexponential API.  The N-th powers are formed by exponentiation by
 * squaring, avoiding an N-step product chain while preserving the approximate
 * decay factors already used by the established path.
 * ------------------------------------------------------------------------- */
static inline __m512 ufr_pharma_2c_pow_u32(__m512 base, unsigned e)
{
    __m512 result = _mm512_set1_ps(1.0f);
    while (e != 0u) {
        if (e & 1u) result = _mm512_mul_ps(result, base);
        e >>= 1u;
        if (e != 0u) base = _mm512_mul_ps(base, base);
    }
    return result;
}

static inline __m512 ufr_pharma_2c_geom_sum_fast(__m512 r, unsigned n)
{
    /* Binary-prefix doubling for S_n = sum_{k=1}^n r^k.
     * Each processed bit doubles the represented exponent range:
     *   P' = P^2
     *   S' = S + P*S = S*(1+P)
     * and a set bit appends one more r-powered term.  This avoids the
     * cancellation/division in r*(1-r^n)/(1-r) and uses O(log2 n) multiplies. */
    __m512 p = _mm512_set1_ps(1.0f);
    __m512 s = _mm512_setzero_ps();
    unsigned highest = 0u;
    if (n != 0u) {
        unsigned x = n;
        while (x >>= 1u) ++highest;
        for (int bit = (int)highest; bit >= 0; --bit) {
            const __m512 p2 = _mm512_mul_ps(p, p);
            const __m512 s2 = _mm512_mul_ps(s, _mm512_add_ps(_mm512_set1_ps(1.0f), p));
            s = s2;
            p = p2;
            if ((n >> (unsigned)bit) & 1u) {
                const __m512 pn = _mm512_mul_ps(p, r);
                s = _mm512_add_ps(s, pn);
                p = pn;
            }
        }
    }
    return s;
}

int ufr_pharma_2c_biexp_closed_form_run(
    const float *cl, const float *vc, const float *vp, const float *q,
    size_t subjects, unsigned time_steps, float dt, float dose,
    float *out_auc, float *out_cmax)
{
    if (!cl || !vc || !vp || !q || !out_auc || !out_cmax ||
        subjects == 0u || (subjects & 15u) != 0u || time_steps == 0u ||
        dt <= 0.0f || dose <= 0.0f) return -1;

    const __m512 zero = _mm512_setzero_ps();
    const __m512 half = _mm512_set1_ps(0.5f);
    const __m512 four = _mm512_set1_ps(4.0f);
    const __m512 dtv = _mm512_set1_ps(dt);
    const __m512 dosev = _mm512_set1_ps(dose);
    for (size_t i = 0; i < subjects; i += 16u) {
        const __m512 CL = _mm512_loadu_ps(cl + i);
        const __m512 VC = _mm512_loadu_ps(vc + i);
        const __m512 VP = _mm512_loadu_ps(vp + i);
        const __m512 Q  = _mm512_loadu_ps(q + i);
        const __mmask16 bad_input =
            _mm512_cmp_ps_mask(VC, zero, _CMP_LE_OQ) |
            _mm512_cmp_ps_mask(VP, zero, _CMP_LE_OQ) |
            _mm512_cmp_ps_mask(CL, zero, _CMP_LT_OQ) |
            _mm512_cmp_ps_mask(Q,  zero, _CMP_LT_OQ) |
            _mm512_cmp_ps_mask(VC, zero, _CMP_UNORD_Q) |
            _mm512_cmp_ps_mask(VP, zero, _CMP_UNORD_Q) |
            _mm512_cmp_ps_mask(CL, zero, _CMP_UNORD_Q) |
            _mm512_cmp_ps_mask(Q,  zero, _CMP_UNORD_Q);
        if (bad_input) return -2;

        const __m512 k10 = _mm512_div_ps(CL, VC);
        const __m512 k12 = _mm512_div_ps(Q, VC);
        const __m512 k21 = _mm512_div_ps(Q, VP);
        const __m512 sum = _mm512_add_ps(_mm512_add_ps(k10, k12), k21);
        const __m512 disc = _mm512_sqrt_ps(_mm512_max_ps(zero,
            _mm512_sub_ps(_mm512_mul_ps(sum, sum),
                          _mm512_mul_ps(four, _mm512_mul_ps(k10, k21)))));
        const __m512 alpha = _mm512_mul_ps(half, _mm512_add_ps(sum, disc));
        const __m512 beta  = _mm512_mul_ps(half, _mm512_sub_ps(sum, disc));
        const __m512 den = _mm512_sub_ps(alpha, beta);
        if (_mm512_cmp_ps_mask(den, _mm512_set1_ps(1.0e-12f), _CMP_LE_OQ) != 0u)
            return -3;

        const __m512 invvc_dose = _mm512_div_ps(dosev, VC);
        const __m512 A = _mm512_mul_ps(invvc_dose,
            _mm512_div_ps(_mm512_sub_ps(alpha, k21), den));
        const __m512 B = _mm512_mul_ps(invvc_dose,
            _mm512_div_ps(_mm512_sub_ps(k21, beta), den));
        /* Standard IV-bolus 2C has A,B >= 0.  Refuse the closed-form Cmax
         * shortcut outside that domain instead of silently changing semantics. */
        if (_mm512_cmp_ps_mask(A, zero, _CMP_LT_OQ) != 0u ||
            _mm512_cmp_ps_mask(B, zero, _CMP_LT_OQ) != 0u)
            return -4;

        const __m512 ra = ufr_pharma_2c_exp_approx(
            _mm512_mul_ps(_mm512_sub_ps(zero, alpha), dtv));
        const __m512 rb = ufr_pharma_2c_exp_approx(
            _mm512_mul_ps(_mm512_sub_ps(zero, beta), dtv));
        const __m512 sumA = ufr_pharma_2c_geom_sum_fast(ra, time_steps);
        const __m512 sumB = ufr_pharma_2c_geom_sum_fast(rb, time_steps);
        const __m512 auc = _mm512_mul_ps(dtv,
            _mm512_fmadd_ps(A, sumA, _mm512_mul_ps(B, sumB)));
        const __m512 c1 = _mm512_fmadd_ps(A, ra, _mm512_mul_ps(B, rb));

        _mm512_storeu_ps(out_auc + i, auc);
        _mm512_storeu_ps(out_cmax + i, c1);
    }
    return 0;
}

static int ufr_pharma_2c_pk_prepare(ufr_pharma_2c_pk_piecewise_ctx_t *ctx,
                                    const float *cl, const float *vc,
                                    const float *vp, const float *q)
{
    const size_t n=ctx->subjects;
    const __m512 z=_mm512_setzero_ps();
    const __m512 one=_mm512_set1_ps(1.0f), four=_mm512_set1_ps(4.0f), half=_mm512_set1_ps(0.5f);
    const __m512 dt=_mm512_set1_ps(ctx->dt);
    for(size_t i=0;i<n;i+=16u){
        const __m512 CL=_mm512_loadu_ps(cl+i), VC=_mm512_loadu_ps(vc+i), VP=_mm512_loadu_ps(vp+i), Q=_mm512_loadu_ps(q+i);
        const __m512 invvc=_mm512_div_ps(one,VC);
        const __m512 k10=_mm512_div_ps(CL,VC), k12=_mm512_div_ps(Q,VC), k21=_mm512_div_ps(Q,VP);
        const __mmask16 bad = _mm512_cmp_ps_mask(k10,z,_CMP_LE_OQ) |
                              _mm512_cmp_ps_mask(k21,z,_CMP_LE_OQ) |
                              _mm512_cmp_ps_mask(k12,z,_CMP_LT_OQ) |
                              _mm512_cmp_ps_mask(CL,z,_CMP_UNORD_Q) |
                              _mm512_cmp_ps_mask(VC,z,_CMP_UNORD_Q) |
                              _mm512_cmp_ps_mask(VP,z,_CMP_UNORD_Q) |
                              _mm512_cmp_ps_mask(Q,z,_CMP_UNORD_Q);
        if (bad) return -5;
        const __m512 sum=_mm512_add_ps(_mm512_add_ps(k10,k12),k21);
        const __m512 disc=_mm512_sqrt_ps(_mm512_max_ps(z,
            _mm512_sub_ps(_mm512_mul_ps(sum,sum),_mm512_mul_ps(four,_mm512_mul_ps(k10,k21)))));
        const __m512 alpha=_mm512_mul_ps(half,_mm512_add_ps(sum,disc));
        const __m512 beta=_mm512_mul_ps(half,_mm512_sub_ps(sum,disc));
        const __m512 den=_mm512_sub_ps(alpha,beta);
        const __m512 ea=ufr_pharma_2c_exp_approx(_mm512_sub_ps(z,_mm512_mul_ps(alpha,dt)));
        const __m512 eb=ufr_pharma_2c_exp_approx(_mm512_sub_ps(z,_mm512_mul_ps(beta,dt)));
        const __m512 k00=_mm512_sub_ps(z,_mm512_add_ps(k10,k12));
        const __m512 k11=_mm512_sub_ps(z,k21);
        const __m512 m00=_mm512_div_ps(_mm512_sub_ps(_mm512_mul_ps(_mm512_add_ps(k00,alpha),eb),
                                                      _mm512_mul_ps(_mm512_add_ps(k00,beta),ea)),den);
        const __m512 m01=_mm512_div_ps(_mm512_mul_ps(k21,_mm512_sub_ps(eb,ea)),den);
        const __m512 m10=_mm512_div_ps(_mm512_mul_ps(k12,_mm512_sub_ps(eb,ea)),den);
        const __m512 m11=_mm512_div_ps(_mm512_sub_ps(_mm512_mul_ps(_mm512_add_ps(k11,alpha),eb),
                                                      _mm512_mul_ps(_mm512_add_ps(k11,beta),ea)),den);
        const __m512 a0=_mm512_sub_ps(m00,one), c0=m10;
        const __m512 invdet=_mm512_div_ps(one,_mm512_mul_ps(k10,k21));
        const __m512 g0=_mm512_mul_ps(_mm512_mul_ps(_mm512_mul_ps(_mm512_sub_ps(z,k21),_mm512_add_ps(a0,c0)),invdet),invvc);
        const __m512 g1=_mm512_mul_ps(_mm512_mul_ps(_mm512_sub_ps(_mm512_mul_ps(_mm512_sub_ps(z,k12),a0),_mm512_mul_ps(_mm512_add_ps(k10,k12),c0)),invdet),invvc);
        _mm512_store_ps(ctx->M+0u*n+i,m00); _mm512_store_ps(ctx->M+1u*n+i,m01);
        _mm512_store_ps(ctx->M+2u*n+i,m10); _mm512_store_ps(ctx->M+3u*n+i,m11);
        _mm512_store_ps(ctx->G+0u*n+i,g0);  _mm512_store_ps(ctx->G+1u*n+i,g1);
        _mm512_store_ps(ctx->inv_vc+i,invvc);
    }
    return 0;
}

int ufr_pharma_2c_pk_piecewise_ctx_init(
    ufr_pharma_2c_pk_piecewise_ctx_t *ctx,
    size_t subjects, unsigned time_steps, unsigned segments, float dt,
    const float *cl, const float *vc, const float *vp, const float *q)
{
    if(!ctx||!cl||!vc||!vp||!q||subjects==0u||time_steps==0u||segments==0u||dt<=0.0f||
       (time_steps%segments)!=0u || (subjects%16u)!=0u) return -1;
    if(subjects > SIZE_MAX/(16u*sizeof(float))) return -2;
    memset(ctx,0,sizeof(*ctx));
    ctx->subjects=subjects; ctx->time_steps=time_steps; ctx->segments=segments; ctx->dt=dt;
    ctx->M=(float*)ufr_pharma_pw_alloc64((size_t)4u*subjects*sizeof(float));
    ctx->G=(float*)ufr_pharma_pw_alloc64((size_t)2u*subjects*sizeof(float));
    ctx->inv_vc=(float*)ufr_pharma_pw_alloc64(subjects*sizeof(float));
    if(!ctx->M||!ctx->G||!ctx->inv_vc){ufr_pharma_2c_pk_piecewise_ctx_destroy(ctx);return -3;}
    return ufr_pharma_2c_pk_prepare(ctx,cl,vc,vp,q);
}

void ufr_pharma_2c_pk_piecewise_ctx_destroy(ufr_pharma_2c_pk_piecewise_ctx_t *ctx)
{
    if(!ctx) return;
    free(ctx->M);
    free(ctx->G);
    free(ctx->inv_vc);
    memset(ctx,0,sizeof(*ctx));
}

int ufr_pharma_2c_pk_piecewise_run(
    const ufr_pharma_2c_pk_piecewise_ctx_t *ctx,
    const float *initial_concentration_blocks,
    const float *rate_by_segment,
    const float *bolus_by_segment,
    float *auc, float *cmax)
{
    if(!ctx||!ctx->M||!ctx->G||!ctx->inv_vc||!initial_concentration_blocks||!rate_by_segment||!bolus_by_segment||!auc||!cmax) return -1;
    const size_t n=ctx->subjects; const unsigned nt=ctx->time_steps/ctx->segments;
    const __m512 dt=_mm512_set1_ps(ctx->dt), z=_mm512_setzero_ps();
    for(size_t i=0;i<n;i+=16u){
        /* State is concentration: Cc and Cp.  Input bolus is converted by
         * the subject-specific 1/Vc; G is likewise stored in concentration
         * units per unit infusion rate. */
        __m512 x0=_mm512_loadu_ps(initial_concentration_blocks+i), x1=_mm512_loadu_ps(initial_concentration_blocks+n+i);
        const __m512 invvc=_mm512_load_ps(ctx->inv_vc+i);
        const __m512 m00=_mm512_load_ps(ctx->M+0u*n+i),m01=_mm512_load_ps(ctx->M+1u*n+i),m10=_mm512_load_ps(ctx->M+2u*n+i),m11=_mm512_load_ps(ctx->M+3u*n+i);
        const __m512 g0=_mm512_load_ps(ctx->G+0u*n+i),g1=_mm512_load_ps(ctx->G+1u*n+i);
        __m512 aa=z,mm=z;
        for(unsigned s=0;s<ctx->segments;++s){
            x0=_mm512_add_ps(x0,_mm512_mul_ps(_mm512_set1_ps(bolus_by_segment[s]),invvc));
            const __m512 r=_mm512_set1_ps(rate_by_segment[s]);
            const __m512 q0=_mm512_mul_ps(g0,r),q1=_mm512_mul_ps(g1,r);
            for(unsigned t=0;t<nt;++t){
                const __m512 y0=_mm512_add_ps(_mm512_fmadd_ps(m01,x1,_mm512_mul_ps(m00,x0)),q0);
                const __m512 y1=_mm512_add_ps(_mm512_fmadd_ps(m11,x1,_mm512_mul_ps(m10,x0)),q1);
                x0=y0;x1=y1; const __m512 c=x0;
                aa=_mm512_fmadd_ps(c,dt,aa); mm=_mm512_max_ps(mm,c);
            }
        }
        _mm512_storeu_ps(auc+i,aa); _mm512_storeu_ps(cmax+i,mm);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Integrated Finance time-direction vectorization adapter.
 *
 * q256 is interpreted as four independent paths x 64 consecutive time steps:
 * q256[path*64 + t]
 * No repack is performed.  The adapter is explicit/manual only and does not
 * enter AUTO because the caller is opting into a specific path-major qblock
 * geometry and a path-dependent payoff formulation.
 * ------------------------------------------------------------------------- */

/* Public/manual Finance time-vector types are mirrored here because this
 * single-file integration source is intentionally self-contained. Keep these
 * definitions layout-compatible with UltraFastRng512_MC_Integrated_v5.h. */
typedef enum {
    UFR_FINANCE_TIMEVECTOR_ASIAN = 1u,
    UFR_FINANCE_TIMEVECTOR_BARRIER = 2u
} ufr_finance_timevector_mode_t;

typedef struct {
    float spot;
    float rate;
    float vol;
    float dt;
    float strike;
    float barrier;
    unsigned time_steps;
    ufr_finance_timevector_mode_t mode;
} ufr_finance_timevector_cfg_t;

typedef struct {
    double value;
    uint64_t barrier_hits;
    uint64_t path_samples;
    uint64_t qblocks;
} ufr_finance_timevector_result_t;

typedef struct {
    ufr_finance_timevector_cfg_t cfg;
    double value;
    uint64_t barrier_hits;
    uint64_t path_samples;
    uint64_t qblocks;
} ufr_finance_timevector_runner_t;

static inline __m512 ufr_fin_time_prefix16(__m512 x)
{
    const __m512i i1 = _mm512_setr_epi32(
        15,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14);
    const __m512i i2 = _mm512_setr_epi32(
        15,15,0,1,2,3,4,5,6,7,8,9,10,11,12,13);
    const __m512i i4 = _mm512_setr_epi32(
        15,15,15,15,0,1,2,3,4,5,6,7,8,9,10,11);
    const __m512i i8 = _mm512_setr_epi32(
        15,15,15,15,15,15,15,15,0,1,2,3,4,5,6,7);
    x = _mm512_add_ps(x, _mm512_maskz_permutexvar_ps(0xFFFEu, i1, x));
    x = _mm512_add_ps(x, _mm512_maskz_permutexvar_ps(0xFFFCu, i2, x));
    x = _mm512_add_ps(x, _mm512_maskz_permutexvar_ps(0xFFF0u, i4, x));
    x = _mm512_add_ps(x, _mm512_maskz_permutexvar_ps(0xFF00u, i8, x));
    return x;
}

static inline float ufr_fin_time_hsum16(__m512 x)
{
    return _mm512_reduce_add_ps(x);
}

static int ufr_finance_timevector_qblock_run(
    const int32_t *q256,
    const ufr_finance_timevector_cfg_t *cfg,
    ufr_finance_timevector_result_t *out)
{
    if (!q256 || !cfg || !out) return -1;
    if (cfg->time_steps != 64u || cfg->dt <= 0.0f ||
        !isfinite(cfg->spot) || !isfinite(cfg->rate) ||
        !isfinite(cfg->vol) || !isfinite(cfg->strike) ||
        !isfinite(cfg->barrier) || cfg->spot <= 0.0f ||
        cfg->vol < 0.0f) return -2;
    if (cfg->mode != UFR_FINANCE_TIMEVECTOR_ASIAN &&
        cfg->mode != UFR_FINANCE_TIMEVECTOR_BARRIER) return -3;

    const __m512 scale = _mm512_set1_ps(UF_NORMAL_SCALE);
    const __m512 drift = _mm512_set1_ps(
        (cfg->rate - 0.5f * cfg->vol * cfg->vol) * cfg->dt);
    const __m512 vs = _mm512_set1_ps(cfg->vol * sqrtf(cfg->dt));
    const __m512 qfactor = _mm512_mul_ps(scale, vs);
    const __m512 spot = _mm512_set1_ps(cfg->spot);
    const __m512 barrier_log = _mm512_set1_ps(logf(cfg->barrier / cfg->spot));

    double value = 0.0;
    uint64_t hits = 0u;
    for (unsigned p = 0; p < 4u; ++p) {
        float run = 0.0f;
        __m512 asian_sumv = _mm512_setzero_ps();
        unsigned hit = 0u;
        for (unsigned tb = 0; tb < 64u; tb += 16u) {
            const __m512 qv = _mm512_cvtepi32_ps(
                _mm512_loadu_si512((const void *)(q256 + p * 64u + tb)));
            __m512 logS = ufr_fin_time_prefix16(
                _mm512_add_ps(_mm512_mul_ps(qv, qfactor), drift));
            logS = _mm512_add_ps(logS, _mm512_set1_ps(run));
            if (cfg->mode == UFR_FINANCE_TIMEVECTOR_BARRIER) {
                /* Barrier event is monotone in logS.  Compare directly against
                 * log(barrier/spot), eliminating exp approximation and the
                 * subsequent spot multiply from the Barrier hot loop. */
                hit |= (_mm512_cmp_ps_mask(logS, barrier_log, _CMP_GE_OQ) != 0u);
            } else {
                const __m512 st = _mm512_mul_ps(spot, ufr_mc_int_exp_approx_ps(logS));
                /* Keep the four 16-lane blocks in-register and perform one
                 * horizontal reduction per path, instead of four.  This
                 * changes only floating-point summation order; the tested
                 * aggregate difference is ~2e-9 while reducing scalar
                 * dependency in the hot loop. */
                asian_sumv = _mm512_add_ps(asian_sumv, st);
            }
            alignas(64) float tail[16];
            _mm512_store_ps(tail, logS);
            run = tail[15];
        }
        if (cfg->mode == UFR_FINANCE_TIMEVECTOR_BARRIER)
            hits += (uint64_t)hit;
        else
            value += fmax((double)_mm512_reduce_add_ps(asian_sumv) / 64.0 - (double)cfg->strike, 0.0);
    }

    memset(out, 0, sizeof(*out));
    out->value = value;
    out->barrier_hits = hits;
    out->path_samples = 4u;
    out->qblocks = 1u;
    return 0;
}

static int ufr_finance_timevector_consume(const int32_t *q256,
                                          uint64_t generation_id,
                                          uint32_t view_id,
                                          uint32_t block_id,
                                          void *vctx)
{
    (void)generation_id; (void)view_id; (void)block_id;
    ufr_finance_timevector_runner_t *r =
        (ufr_finance_timevector_runner_t *)vctx;
    if (!r || !q256) return -1;
    ufr_finance_timevector_result_t one;
    const int rc = ufr_finance_timevector_qblock_run(q256, &r->cfg, &one);
    if (rc != 0) return rc;
    r->value += one.value;
    r->barrier_hits += one.barrier_hits;
    r->path_samples += one.path_samples;
    r->qblocks += one.qblocks;
    return 0;
}

void ufr_finance_timevector_runner_init(
    ufr_finance_timevector_runner_t *runner,
    const ufr_finance_timevector_cfg_t *cfg)
{
    if (!runner) return;
    memset(runner, 0, sizeof(*runner));
    if (cfg) runner->cfg = *cfg;
}

int ufr_finance_timevector_run_qblock(
    const int32_t *q256,
    const ufr_finance_timevector_cfg_t *cfg,
    ufr_finance_timevector_result_t *out)
{
    return ufr_finance_timevector_qblock_run(q256, cfg, out);
}

int ufr_paperview_mc_run_finance_timevector(
    ufr_paperview_mc_engine_t *engine,
    const ufr_finance_timevector_cfg_t *cfg,
    uint64_t chunks,
    ufr_finance_timevector_result_t *out)
{
    if (!engine || !cfg || !out || chunks == 0u) return -1;
    if (cfg->time_steps != 64u) return -2;
    ufr_finance_timevector_runner_t runner;
    ufr_finance_timevector_runner_init(&runner, cfg);
    const int rc = ufr_mc_run(&engine->impl, chunks,
                              ufr_finance_timevector_consume, &runner);
    if (rc != 0) return -3;
    *out = (ufr_finance_timevector_result_t){
        .value = runner.value,
        .barrier_hits = runner.barrier_hits,
        .path_samples = runner.path_samples,
        .qblocks = runner.qblocks
    };
    return 0;
}


/* -------------------------------------------------------------------------
 * Integrated Finance log-domain path adapters.
 * Local definitions mirror the public v10 header so the single-file
 * integrated source remains self-contained.
 * ------------------------------------------------------------------------- */
typedef enum {
    UFR_FINANCE_PATH_GEOMETRIC_ASIAN = 1u,
    UFR_FINANCE_PATH_LOOKBACK_CALL = 2u,
    UFR_FINANCE_PATH_LOOKBACK_PUT = 3u
} ufr_finance_path_mode_t;

typedef struct {
    float spot, rate, vol, dt, strike;
    unsigned time_steps;
    ufr_finance_path_mode_t mode;
} ufr_finance_path_cfg_t;

typedef struct {
    double value;
    uint64_t path_samples;
    uint64_t qblocks;
} ufr_finance_path_result_t;

typedef struct {
    ufr_finance_path_cfg_t cfg;
    double value;
    uint64_t path_samples;
    uint64_t qblocks;
} ufr_finance_path_runner_t;

/* q256 layout: [4 paths][64 time].
 * Geometric Asian accumulates log-price directly and exponentiates once per
 * path. Lookback keeps the running log max/min and likewise exponentiates
 * only once. This is mathematically equivalent for positive spot prices and
 * avoids 63 unnecessary exponent evaluations versus arithmetic spot paths.
 * ------------------------------------------------------------------------- */
static int ufr_finance_path_run_qblock_internal(
    const int32_t *q256,
    const ufr_finance_path_cfg_t *cfg,
    ufr_finance_path_result_t *out)
{
    if (!q256 || !cfg || !out) return -1;
    if (cfg->time_steps != 64u || cfg->dt <= 0.0f ||
        !isfinite(cfg->spot) || !isfinite(cfg->rate) ||
        !isfinite(cfg->vol) || !isfinite(cfg->strike) ||
        cfg->spot <= 0.0f || cfg->vol < 0.0f) return -2;
    if (cfg->mode != UFR_FINANCE_PATH_GEOMETRIC_ASIAN &&
        cfg->mode != UFR_FINANCE_PATH_LOOKBACK_CALL &&
        cfg->mode != UFR_FINANCE_PATH_LOOKBACK_PUT) return -3;

    const __m512 drift = _mm512_set1_ps(
        (cfg->rate - 0.5f * cfg->vol * cfg->vol) * cfg->dt);
    const __m512 qfactor = _mm512_set1_ps(UF_NORMAL_SCALE * cfg->vol * sqrtf(cfg->dt));
    const __m512 spot = _mm512_set1_ps(cfg->spot);
    const __m512 strike = _mm512_set1_ps(cfg->strike);
    const __m512 zero = _mm512_setzero_ps();

    double value = 0.0;
    for (unsigned p = 0; p < 4u; ++p) {
        float run = 0.0f;
        __m512 sum_log = _mm512_setzero_ps();
        __m512 ext = (cfg->mode == UFR_FINANCE_PATH_LOOKBACK_PUT)
                   ? _mm512_set1_ps(FLT_MAX)
                   : _mm512_set1_ps(-FLT_MAX);
        for (unsigned tb = 0; tb < 64u; tb += 16u) {
            const __m512 qv = _mm512_cvtepi32_ps(
                _mm512_loadu_si512((const void *)(q256 + p * 64u + tb)));
            __m512 logS = ufr_fin_time_prefix16(
                _mm512_add_ps(_mm512_mul_ps(qv, qfactor), drift));
            logS = _mm512_add_ps(logS, _mm512_set1_ps(run));
            if (cfg->mode == UFR_FINANCE_PATH_GEOMETRIC_ASIAN) {
                sum_log = _mm512_add_ps(sum_log, logS);
            } else if (cfg->mode == UFR_FINANCE_PATH_LOOKBACK_PUT) {
                ext = _mm512_min_ps(ext, logS);
            } else {
                ext = _mm512_max_ps(ext, logS);
            }
            alignas(64) float tail[16];
            _mm512_store_ps(tail, logS);
            run = tail[15];
        }
        if (cfg->mode == UFR_FINANCE_PATH_GEOMETRIC_ASIAN) {
            const __m512 avg_log_v = _mm512_set1_ps(
                _mm512_reduce_add_ps(sum_log) * (1.0f / 64.0f));
            const __m512 geo = _mm512_mul_ps(
                spot, ufr_mc_int_exp_approx_ps(avg_log_v));
            value += (double)_mm512_cvtss_f32(
                _mm512_max_ps(zero, _mm512_sub_ps(geo, strike)));
        } else {
            const float ext_log = (cfg->mode == UFR_FINANCE_PATH_LOOKBACK_PUT)
                                ? _mm512_reduce_min_ps(ext)
                                : _mm512_reduce_max_ps(ext);
            const __m512 ext_log_v = _mm512_set1_ps(ext_log);
            const __m512 ext_spot = _mm512_mul_ps(
                spot, ufr_mc_int_exp_approx_ps(ext_log_v));
            if (cfg->mode == UFR_FINANCE_PATH_LOOKBACK_PUT)
                value += (double)_mm512_cvtss_f32(_mm512_max_ps(
                    zero, _mm512_sub_ps(strike, ext_spot)));
            else
                value += (double)_mm512_cvtss_f32(_mm512_max_ps(
                    zero, _mm512_sub_ps(ext_spot, strike)));
        }
    }

    *out = (ufr_finance_path_result_t){
        .value = value, .path_samples = 4u, .qblocks = 1u};
    return 0;
}

int ufr_finance_path_run_qblock(
    const int32_t *q256,
    const ufr_finance_path_cfg_t *cfg,
    ufr_finance_path_result_t *out)
{
    return ufr_finance_path_run_qblock_internal(q256, cfg, out);
}

static int ufr_finance_path_consume(
    const int32_t *q256, uint64_t generation_id, uint32_t view_id,
    uint32_t block_id, void *vctx)
{
    (void)generation_id; (void)view_id; (void)block_id;
    ufr_finance_path_runner_t *r = (ufr_finance_path_runner_t *)vctx;
    if (!r || !q256) return -1;
    ufr_finance_path_result_t one;
    const int rc = ufr_finance_path_run_qblock_internal(q256, &r->cfg, &one);
    if (rc != 0) return rc;
    r->value += one.value;
    r->path_samples += one.path_samples;
    r->qblocks += one.qblocks;
    return 0;
}

int ufr_paperview_mc_run_finance_path(
    ufr_paperview_mc_engine_t *engine,
    const ufr_finance_path_cfg_t *cfg,
    uint64_t chunks,
    ufr_finance_path_result_t *out)
{
    if (!engine || !cfg || !out || chunks == 0u) return -1;
    if (cfg->time_steps != 64u) return -2;
    ufr_finance_path_runner_t runner;
    memset(&runner, 0, sizeof(runner));
    runner.cfg = *cfg;
    const int rc = ufr_mc_run(&engine->impl, chunks,
                              ufr_finance_path_consume, &runner);
    if (rc != 0) return -3;
    *out = (ufr_finance_path_result_t){
        .value = runner.value,
        .path_samples = runner.path_samples,
        .qblocks = runner.qblocks};
    return 0;
}

/* -------------------------------------------------------------------------
 * Integrated Finance two-factor correlation / Basket adapter.
 *
 * Manual-only q256 geometry:
 *   q256[0..127]   = factor 0, [4 paths][32 time]
 *   q256[128..255] = factor 1, [4 paths][32 time]
 *
 * For each path, factor 1 is correlated with factor 0 as
 *   z1c = rho*z0 + sqrt(1-rho^2)*z1.
 * The two log-price recurrences are then prefix-vectorized in 16-time
 * blocks.  No q repack/copy is performed inside the adapter.
 * ------------------------------------------------------------------------- */
typedef struct {
    float spot0, spot1;
    float weight0, weight1;
    float rate0, rate1;
    float vol0, vol1;
    float rho;
    float dt;
    float strike;
    unsigned time_steps;
} ufr_finance_multifactor2_cfg_t;

typedef struct {
    double value;
    uint64_t path_samples;
    uint64_t qblocks;
} ufr_finance_multifactor2_result_t;

typedef struct {
    ufr_finance_multifactor2_cfg_t cfg;
    double value;
    uint64_t path_samples;
    uint64_t qblocks;
} ufr_finance_multifactor2_runner_t;

static int ufr_finance_multifactor2_qblock_run(
    const int32_t *q256,
    const ufr_finance_multifactor2_cfg_t *cfg,
    ufr_finance_multifactor2_result_t *out)
{
    if (!q256 || !cfg || !out) return -1;
    if (cfg->time_steps != 32u || cfg->dt <= 0.0f ||
        !isfinite(cfg->spot0) || !isfinite(cfg->spot1) ||
        !isfinite(cfg->weight0) || !isfinite(cfg->weight1) ||
        !isfinite(cfg->rate0) || !isfinite(cfg->rate1) ||
        !isfinite(cfg->vol0) || !isfinite(cfg->vol1) ||
        !isfinite(cfg->rho) || !isfinite(cfg->strike) ||
        cfg->spot0 <= 0.0f || cfg->spot1 <= 0.0f ||
        cfg->vol0 < 0.0f || cfg->vol1 < 0.0f ||
        cfg->rho <= -0.999999f || cfg->rho >= 0.999999f) return -2;

    const float ro = sqrtf(fmaxf(0.0f, 1.0f - cfg->rho * cfg->rho));
    const float sdt = sqrtf(cfg->dt);
    /* Fold the Normal scale, volatility and time step into the q coefficients
     * before entering the time loop.  For factor 1, also fold rho and the
     * orthogonal coefficient.  This removes the intermediate z0/z1 vectors
     * and their extra scale/volatility multiplies without changing q layout. */
    const __m512 incfac0 = _mm512_set1_ps(
        UF_NORMAL_SCALE * cfg->vol0 * sdt);
    const __m512 incfac1_q0 = _mm512_set1_ps(
        UF_NORMAL_SCALE * cfg->rho * cfg->vol1 * sdt);
    const __m512 incfac1_q1 = _mm512_set1_ps(
        UF_NORMAL_SCALE * ro * cfg->vol1 * sdt);
    const __m512 drift0 = _mm512_set1_ps(
        (cfg->rate0 - 0.5f * cfg->vol0 * cfg->vol0) * cfg->dt);
    const __m512 drift1 = _mm512_set1_ps(
        (cfg->rate1 - 0.5f * cfg->vol1 * cfg->vol1) * cfg->dt);
    const __m512 weighted_spot0 = _mm512_set1_ps(cfg->spot0 * cfg->weight0);
    const __m512 weighted_spot1 = _mm512_set1_ps(cfg->spot1 * cfg->weight1);
    const __m512 strike = _mm512_set1_ps(cfg->strike);

    alignas(64) float final0[16] = {0}, final1[16] = {0};
    for (unsigned p = 0; p < 4u; ++p) {
        float run0 = 0.0f, run1 = 0.0f;
        __m512 logv0 = _mm512_setzero_ps();
        __m512 logv1 = _mm512_setzero_ps();
        for (unsigned tb = 0; tb < 32u; tb += 16u) {
            const unsigned base0 = p * 32u + tb;
            const unsigned base1 = 128u + p * 32u + tb;
            const __m512 q0 = _mm512_cvtepi32_ps(
                _mm512_loadu_si512((const void *)(q256 + base0)));
            const __m512 q1 = _mm512_cvtepi32_ps(
                _mm512_loadu_si512((const void *)(q256 + base1)));
            __m512 inc0 = _mm512_fmadd_ps(q0, incfac0, drift0);
            __m512 inc1 = _mm512_fmadd_ps(q0, incfac1_q0,
                                          _mm512_mul_ps(q1, incfac1_q1));
            inc1 = _mm512_add_ps(inc1, drift1);
            inc0 = ufr_fin_time_prefix16(inc0);
            inc1 = ufr_fin_time_prefix16(inc1);
            logv0 = _mm512_add_ps(inc0, _mm512_set1_ps(run0));
            logv1 = _mm512_add_ps(inc1, _mm512_set1_ps(run1));
            alignas(64) float t0[16], t1[16];
            _mm512_store_ps(t0, logv0);
            _mm512_store_ps(t1, logv1);
            run0 = t0[15];
            run1 = t1[15];
        }
        final0[p] = run0;
        final1[p] = run1;
    }

    const __m512 x0 = _mm512_load_ps(final0);
    const __m512 x1 = _mm512_load_ps(final1);
    /* Fold constant basket weights into spot so each terminal factor needs
     * only one vector multiply after exp instead of spot-mul + weight-mul. */
    const __m512 s0 = _mm512_mul_ps(weighted_spot0, ufr_mc_int_exp_approx_ps(x0));
    const __m512 s1 = _mm512_mul_ps(weighted_spot1, ufr_mc_int_exp_approx_ps(x1));
    const __m512 payoff = _mm512_max_ps(
        _mm512_setzero_ps(),
        _mm512_sub_ps(_mm512_add_ps(s0, s1), strike));
    double value = (double)_mm512_reduce_add_ps(payoff);
    memset(out, 0, sizeof(*out));
    out->value = value;
    out->path_samples = 4u;
    out->qblocks = 1u;
    return 0;
}

static int ufr_finance_multifactor2_consume(const int32_t *q256,
                                             uint64_t generation_id,
                                             uint32_t view_id,
                                             uint32_t block_id,
                                             void *vctx)
{
    (void)generation_id; (void)view_id; (void)block_id;
    ufr_finance_multifactor2_runner_t *r =
        (ufr_finance_multifactor2_runner_t *)vctx;
    if (!r || !q256) return -1;
    ufr_finance_multifactor2_result_t one;
    const int rc = ufr_finance_multifactor2_qblock_run(q256, &r->cfg, &one);
    if (rc != 0) return rc;
    r->value += one.value;
    r->path_samples += one.path_samples;
    r->qblocks += one.qblocks;
    return 0;
}

void ufr_finance_multifactor2_runner_init(
    ufr_finance_multifactor2_runner_t *runner,
    const ufr_finance_multifactor2_cfg_t *cfg)
{
    if (!runner) return;
    memset(runner, 0, sizeof(*runner));
    if (cfg) runner->cfg = *cfg;
}

int ufr_finance_multifactor2_run_qblock(
    const int32_t *q256,
    const ufr_finance_multifactor2_cfg_t *cfg,
    ufr_finance_multifactor2_result_t *out)
{
    return ufr_finance_multifactor2_qblock_run(q256, cfg, out);
}

int ufr_paperview_mc_run_finance_multifactor2(
    ufr_paperview_mc_engine_t *engine,
    const ufr_finance_multifactor2_cfg_t *cfg,
    uint64_t chunks,
    ufr_finance_multifactor2_result_t *out)
{
    if (!engine || !cfg || !out || chunks == 0u) return -1;
    if (cfg->time_steps != 32u) return -2;
    ufr_finance_multifactor2_runner_t runner;
    ufr_finance_multifactor2_runner_init(&runner, cfg);
    const int rc = ufr_mc_run(&engine->impl, chunks,
                              ufr_finance_multifactor2_consume, &runner);
    if (rc != 0) return -3;
    *out = (ufr_finance_multifactor2_result_t){
        .value = runner.value,
        .path_samples = runner.path_samples,
        .qblocks = runner.qblocks
    };
    return 0;
}


/* -------------------------------------------------------------------------
 * Optional/manual sparse-schedule Pharma 2C runner.
 *
 * The dense 2C piecewise runner is unchanged.  When any segment has exactly
 * zero infusion rate, this separate path removes G*0/add-zero work from the
 * corresponding segment without placing a branch inside the dense step loop.
 * ------------------------------------------------------------------------- */
static int ufr_pharma_2c_pk_piecewise_run_sparse_manual(
    const ufr_pharma_2c_pk_piecewise_ctx_t *ctx,
    const float *initial,
    const float *rate,
    const float *bolus,
    float *auc,
    float *cmax)
{
    const size_t n = ctx->subjects;
    const unsigned nt = ctx->time_steps / ctx->segments;
    const __m512 dt = _mm512_set1_ps(ctx->dt);
    const __m512 z = _mm512_setzero_ps();
    for (size_t i = 0; i < n; i += 16u) {
        __m512 x0 = _mm512_loadu_ps(initial + i);
        __m512 x1 = _mm512_loadu_ps(initial + n + i);
        const __m512 invvc = _mm512_load_ps(ctx->inv_vc + i);
        const __m512 m00 = _mm512_load_ps(ctx->M + 0u*n + i);
        const __m512 m01 = _mm512_load_ps(ctx->M + 1u*n + i);
        const __m512 m10 = _mm512_load_ps(ctx->M + 2u*n + i);
        const __m512 m11 = _mm512_load_ps(ctx->M + 3u*n + i);
        const __m512 g0 = _mm512_load_ps(ctx->G + 0u*n + i);
        const __m512 g1 = _mm512_load_ps(ctx->G + 1u*n + i);
        __m512 aa = z, mm = z;
        for (unsigned s = 0; s < ctx->segments; ++s) {
            if (bolus[s] != 0.0f)
                x0 = _mm512_add_ps(x0,
                    _mm512_mul_ps(_mm512_set1_ps(bolus[s]), invvc));
            const float rs = rate[s];
            if (rs == 0.0f) {
                for (unsigned t = 0; t < nt; ++t) {
                    const __m512 y0 = _mm512_fmadd_ps(
                        m01, x1, _mm512_mul_ps(m00, x0));
                    const __m512 y1 = _mm512_fmadd_ps(
                        m11, x1, _mm512_mul_ps(m10, x0));
                    x0 = y0; x1 = y1;
                    aa = _mm512_fmadd_ps(x0, dt, aa);
                    mm = _mm512_max_ps(mm, x0);
                }
            } else {
                const __m512 r = _mm512_set1_ps(rs);
                const __m512 q0 = _mm512_mul_ps(g0, r);
                const __m512 q1 = _mm512_mul_ps(g1, r);
                for (unsigned t = 0; t < nt; ++t) {
                    const __m512 y0 = _mm512_add_ps(
                        _mm512_fmadd_ps(m01, x1, _mm512_mul_ps(m00, x0)), q0);
                    const __m512 y1 = _mm512_add_ps(
                        _mm512_fmadd_ps(m11, x1, _mm512_mul_ps(m10, x0)), q1);
                    x0 = y0; x1 = y1;
                    aa = _mm512_fmadd_ps(x0, dt, aa);
                    mm = _mm512_max_ps(mm, x0);
                }
            }
        }
        _mm512_storeu_ps(auc + i, aa);
        _mm512_storeu_ps(cmax + i, mm);
    }
    return 0;
}

int ufr_pharma_2c_pk_piecewise_run_sparse_schedule(
    const ufr_pharma_2c_pk_piecewise_ctx_t *ctx,
    const float *initial_concentration_blocks,
    const float *rate_by_segment,
    const float *bolus_by_segment,
    float *auc, float *cmax)
{
    if (!ctx || !ctx->M || !ctx->G || !ctx->inv_vc ||
        !initial_concentration_blocks || !rate_by_segment ||
        !bolus_by_segment || !auc || !cmax) return -1;
    int any_zero = 0;
    for (unsigned s = 0; s < ctx->segments; ++s)
        if (rate_by_segment[s] == 0.0f) { any_zero = 1; break; }
    if (!any_zero)
        return ufr_pharma_2c_pk_piecewise_run(
            ctx, initial_concentration_blocks, rate_by_segment,
            bolus_by_segment, auc, cmax);
    return ufr_pharma_2c_pk_piecewise_run_sparse_manual(
        ctx, initial_concentration_blocks, rate_by_segment,
        bolus_by_segment, auc, cmax);
}

/* -------------------------------------------------------------------------
 * Optional/manual sparse-schedule Pharma runner.
 *
 * The existing ufr_pharma_linear_piecewise_run() remains byte-for-byte in
 * structure for dense schedules.  This separate API is selected explicitly
 * when at least one segment has exactly zero infusion rate.  It removes the
 * G*0/add-zero work from those segments without adding a branch to the dense
 * hot path.  If no zero-rate segment exists, it delegates to the existing
 * runner.
 * ------------------------------------------------------------------------- */
static int ufr_pharma_pw_run3_sparse_manual(const ufr_pharma_linear_piecewise_ctx_t *ctx,
                              const float *initial,
                              const float *rate,
                              const float *bolus,
                              float *auc,
                              float *cmax)
{
    const size_t n = ctx->subjects;
    const unsigned nt = ctx->time_steps / ctx->segments;
    const __m512 dt = _mm512_set1_ps(ctx->dt);
    const __m512 invv = _mm512_set1_ps(1.0f / ctx->volume);
    const __m512 z = _mm512_setzero_ps();
    for (size_t i = 0; i < n; i += 16u) {
        __m512 x0 = _mm512_loadu_ps(initial + i);
        __m512 x1 = _mm512_loadu_ps(initial + n + i);
        __m512 x2 = _mm512_loadu_ps(initial + 2u*n + i);
        __m512 aa = z, mm = z;
        const __m512 m00=_mm512_load_ps(ctx->M + 0u*n+i), m01=_mm512_load_ps(ctx->M + 1u*n+i), m02=_mm512_load_ps(ctx->M + 2u*n+i);
        const __m512 m10=_mm512_load_ps(ctx->M + 3u*n+i), m11=_mm512_load_ps(ctx->M + 4u*n+i), m12=_mm512_load_ps(ctx->M + 5u*n+i);
        const __m512 m20=_mm512_load_ps(ctx->M + 6u*n+i), m21=_mm512_load_ps(ctx->M + 7u*n+i), m22=_mm512_load_ps(ctx->M + 8u*n+i);
        const __m512 g0=_mm512_load_ps(ctx->G + 0u*n+i), g1=_mm512_load_ps(ctx->G + 1u*n+i), g2=_mm512_load_ps(ctx->G + 2u*n+i);
        for (unsigned s = 0; s < ctx->segments; ++s) {
            const float bs = bolus[s], rs = rate[s];
            if (bs != 0.0f) x0 = _mm512_add_ps(x0, _mm512_set1_ps(bs));
            if (rs == 0.0f) {
                for (unsigned t = 0; t < nt; ++t) {
                    const __m512 y0 = _mm512_fmadd_ps(m02,x2,_mm512_fmadd_ps(m01,x1,_mm512_mul_ps(m00,x0)));
                    const __m512 y1 = _mm512_fmadd_ps(m12,x2,_mm512_fmadd_ps(m11,x1,_mm512_mul_ps(m10,x0)));
                    const __m512 y2 = _mm512_fmadd_ps(m22,x2,_mm512_fmadd_ps(m21,x1,_mm512_mul_ps(m20,x0)));
                    x0=y0; x1=y1; x2=y2;
                    const __m512 c = _mm512_mul_ps(x0, invv);
                    aa = _mm512_fmadd_ps(c, dt, aa);
                    mm = _mm512_max_ps(mm, c);
                }
            } else {
                const __m512 r = _mm512_set1_ps(rs);
                const __m512 q0 = _mm512_mul_ps(g0, r), q1 = _mm512_mul_ps(g1, r), q2 = _mm512_mul_ps(g2, r);
                for (unsigned t = 0; t < nt; ++t) {
                    const __m512 y0 = _mm512_add_ps(_mm512_fmadd_ps(m02,x2,_mm512_fmadd_ps(m01,x1,_mm512_mul_ps(m00,x0))),q0);
                    const __m512 y1 = _mm512_add_ps(_mm512_fmadd_ps(m12,x2,_mm512_fmadd_ps(m11,x1,_mm512_mul_ps(m10,x0))),q1);
                    const __m512 y2 = _mm512_add_ps(_mm512_fmadd_ps(m22,x2,_mm512_fmadd_ps(m21,x1,_mm512_mul_ps(m20,x0))),q2);
                    x0=y0; x1=y1; x2=y2;
                    const __m512 c = _mm512_mul_ps(x0, invv);
                    aa = _mm512_fmadd_ps(c, dt, aa);
                    mm = _mm512_max_ps(mm, c);
                }
            }
        }
        _mm512_storeu_ps(auc + i, aa);
        _mm512_storeu_ps(cmax + i, mm);
    }
    return 0;
}


static int ufr_pharma_pw_run4_sparse_manual(const ufr_pharma_linear_piecewise_ctx_t *ctx,
                              const float *initial,
                              const float *rate,
                              const float *bolus,
                              float *auc,
                              float *cmax)
{
    const size_t n = ctx->subjects;
    const unsigned nt = ctx->time_steps / ctx->segments;
    const __m512 dt = _mm512_set1_ps(ctx->dt);
    const __m512 invv = _mm512_set1_ps(1.0f / ctx->volume);
    const __m512 z = _mm512_setzero_ps();
    for (size_t i = 0; i < n; i += 16u) {
        __m512 x0 = _mm512_loadu_ps(initial + i);
        __m512 x1 = _mm512_loadu_ps(initial + n + i);
        __m512 x2 = _mm512_loadu_ps(initial + 2u*n + i);
        __m512 x3 = _mm512_loadu_ps(initial + 3u*n + i);
        __m512 aa = z, mm = z;
        __m512 m[16], g[4];
        for (unsigned j = 0; j < 16u; ++j) m[j] = _mm512_load_ps(ctx->M + (size_t)j*n + i);
        for (unsigned j = 0; j < 4u; ++j) g[j] = _mm512_load_ps(ctx->G + (size_t)j*n + i);
        for (unsigned s = 0; s < ctx->segments; ++s) {
            const float bs = bolus[s], rs = rate[s];
            if (bs != 0.0f) x0 = _mm512_add_ps(x0, _mm512_set1_ps(bs));
            if (rs == 0.0f) {
                for (unsigned t = 0; t < nt; ++t) {
                    const __m512 y0 = _mm512_fmadd_ps(m[3],x3,_mm512_fmadd_ps(m[2],x2,_mm512_fmadd_ps(m[1],x1,_mm512_mul_ps(m[0],x0))));
                    const __m512 y1 = _mm512_fmadd_ps(m[7],x3,_mm512_fmadd_ps(m[6],x2,_mm512_fmadd_ps(m[5],x1,_mm512_mul_ps(m[4],x0))));
                    const __m512 y2 = _mm512_fmadd_ps(m[11],x3,_mm512_fmadd_ps(m[10],x2,_mm512_fmadd_ps(m[9],x1,_mm512_mul_ps(m[8],x0))));
                    const __m512 y3 = _mm512_fmadd_ps(m[15],x3,_mm512_fmadd_ps(m[14],x2,_mm512_fmadd_ps(m[13],x1,_mm512_mul_ps(m[12],x0))));
                    x0=y0; x1=y1; x2=y2; x3=y3;
                    const __m512 c = _mm512_mul_ps(x0, invv);
                    aa = _mm512_fmadd_ps(c, dt, aa);
                    mm = _mm512_max_ps(mm, c);
                }
            } else {
                const __m512 r = _mm512_set1_ps(rs);
                const __m512 q0=_mm512_mul_ps(g[0],r), q1=_mm512_mul_ps(g[1],r), q2=_mm512_mul_ps(g[2],r), q3=_mm512_mul_ps(g[3],r);
                for (unsigned t = 0; t < nt; ++t) {
                    const __m512 y0 = _mm512_add_ps(_mm512_fmadd_ps(m[3],x3,_mm512_fmadd_ps(m[2],x2,_mm512_fmadd_ps(m[1],x1,_mm512_mul_ps(m[0],x0)))),q0);
                    const __m512 y1 = _mm512_add_ps(_mm512_fmadd_ps(m[7],x3,_mm512_fmadd_ps(m[6],x2,_mm512_fmadd_ps(m[5],x1,_mm512_mul_ps(m[4],x0)))),q1);
                    const __m512 y2 = _mm512_add_ps(_mm512_fmadd_ps(m[11],x3,_mm512_fmadd_ps(m[10],x2,_mm512_fmadd_ps(m[9],x1,_mm512_mul_ps(m[8],x0)))),q2);
                    const __m512 y3 = _mm512_add_ps(_mm512_fmadd_ps(m[15],x3,_mm512_fmadd_ps(m[14],x2,_mm512_fmadd_ps(m[13],x1,_mm512_mul_ps(m[12],x0)))),q3);
                    x0=y0; x1=y1; x2=y2; x3=y3;
                    const __m512 c = _mm512_mul_ps(x0, invv);
                    aa = _mm512_fmadd_ps(c, dt, aa);
                    mm = _mm512_max_ps(mm, c);
                }
            }
        }
        _mm512_storeu_ps(auc + i, aa);
        _mm512_storeu_ps(cmax + i, mm);
    }
    return 0;
}



int ufr_pharma_linear_piecewise_run_sparse_schedule(
    const ufr_pharma_linear_piecewise_ctx_t *ctx,
    const float *initial_blocks,
    const float *rate_by_segment,
    const float *bolus_by_segment,
    float *auc,
    float *cmax)
{
    if (!ctx || !ctx->M || !ctx->G || !initial_blocks ||
        !rate_by_segment || !bolus_by_segment || !auc || !cmax) return -1;
    if (!ufr_pharma_piecewise_plan_valid_internal(
            ctx->dim, ctx->subjects, ctx->time_steps, ctx->segments,
            ctx->dt, ctx->volume)) return -2;
    if ((ctx->subjects & 15u) != 0u) return -3;
    int any_zero_rate = 0;
    for (unsigned s = 0; s < ctx->segments; ++s)
        if (rate_by_segment[s] == 0.0f) { any_zero_rate = 1; break; }
    if (!any_zero_rate)
        return ufr_pharma_linear_piecewise_run(
            ctx, initial_blocks, rate_by_segment, bolus_by_segment, auc, cmax);
    return (ctx->dim == 3u)
        ? ufr_pharma_pw_run3_sparse_manual(ctx, initial_blocks, rate_by_segment, bolus_by_segment, auc, cmax)
        : ufr_pharma_pw_run4_sparse_manual(ctx, initial_blocks, rate_by_segment, bolus_by_segment, auc, cmax);
}

/* ========================================================================
 * Enterprise Monte Carlo extensions V1
 *   (1) Deterministic Stream / CRN
 *   (2) Checkpoint / Exact Replay
 *   (3) CI / Uncertainty Report
 *   (4) Golden Reference / Verification Mode
 *
 * These facilities are deliberately outside the Front hot loop. Stream
 * binding happens once before engine start; checkpoint/replay stops the
 * asynchronous Back and freezes generation switching; reports and golden
 * tests are control-plane work only.
 * ======================================================================== */

static void ufr_mc_stream_put_be32(uint8_t *p, uint32_t x)
{
    p[0]=(uint8_t)(x>>24); p[1]=(uint8_t)(x>>16);
    p[2]=(uint8_t)(x>>8);  p[3]=(uint8_t)x;
}

static void ufr_mc_stream_put_be64(uint8_t *p, uint64_t x)
{
    p[0]=(uint8_t)(x>>56); p[1]=(uint8_t)(x>>48);
    p[2]=(uint8_t)(x>>40); p[3]=(uint8_t)(x>>32);
    p[4]=(uint8_t)(x>>24); p[5]=(uint8_t)(x>>16);
    p[6]=(uint8_t)(x>>8);  p[7]=(uint8_t)x;
}

static size_t ufr_mc_stream_encode_common(const ufr_mc_stream_plan_t *p,
                                          uint64_t absolute_block,
                                          uint8_t *buf, size_t cap)
{
    static const uint8_t domain[]="UFR-MC-STREAM-SEED-V1";
    const size_t need=sizeof(domain)-1u+4u+4u+32u+8u*8u;
    if(!p||!buf||cap<need) return 0u;
    size_t o=0u;
    memcpy(buf+o,domain,sizeof(domain)-1u); o+=sizeof(domain)-1u;
    ufr_mc_stream_put_be32(buf+o,p->api_version); o+=4u;
    ufr_mc_stream_put_be32(buf+o,p->flags & UFR_MC_STREAM_FLAG_CRN); o+=4u;
    memcpy(buf+o,p->master_seed,UFR_MC_STREAM_MASTER_SEED_BYTES); o+=UFR_MC_STREAM_MASTER_SEED_BYTES;
    ufr_mc_stream_put_be64(buf+o,p->experiment_id); o+=8u;
    ufr_mc_stream_put_be64(buf+o,p->run_id); o+=8u;
    ufr_mc_stream_put_be64(buf+o,p->replication_id); o+=8u;
    ufr_mc_stream_put_be64(buf+o,(p->flags&UFR_MC_STREAM_FLAG_CRN)?0u:p->scenario_id); o+=8u;
    ufr_mc_stream_put_be64(buf+o,p->crn_group_id); o+=8u;
    ufr_mc_stream_put_be64(buf+o,p->logical_stream_id); o+=8u;
    ufr_mc_stream_put_be64(buf+o,p->substream_id); o+=8u;
    ufr_mc_stream_put_be64(buf+o,absolute_block); o+=8u;
    return o;
}

static size_t ufr_mc_stream_encode_id_plan(const ufr_mc_stream_plan_t *p,
                                           uint8_t *buf, size_t cap)
{
    size_t o=ufr_mc_stream_encode_common(p,p?p->start_block:0u,buf,cap);
    if(!o||cap-o<8u) return 0u;
    ufr_mc_stream_put_be64(buf+o,p->block_count); o+=8u;
    return o;
}

void ufr_mc_stream_plan_default(ufr_mc_stream_plan_t *p)
{
    if(!p) return;
    memset(p,0,sizeof(*p));
    p->api_version=UFR_MC_STREAM_API_VERSION;
    p->flags=UFR_MC_STREAM_FLAG_WORKER_INDEPENDENT|
             UFR_MC_STREAM_FLAG_BLOCK_ADDRESSABLE;
    p->worker_count=1u;
}

int ufr_mc_stream_plan_validate(const ufr_mc_stream_plan_t *p)
{
    if(!p) return -1;
    if(p->api_version!=UFR_MC_STREAM_API_VERSION) return -2;
    if(p->worker_count==0u) return -3;
    if(p->worker_id>=p->worker_count) return -4;
    if(p->block_count!=0u &&
       UINT64_MAX-p->start_block<p->block_count-1u) return -5;
    if((p->flags&UFR_MC_STREAM_FLAG_CRN)&&p->crn_group_id==0u) return -6;
    return 0;
}

int ufr_mc_stream_derive_seed64(const ufr_mc_stream_plan_t *p,
                                uint64_t relative_block,
                                uint8_t out_seed64[UFR_MC_STREAM_SEED_BYTES])
{
    if(!p||!out_seed64||ufr_mc_stream_plan_validate(p)!=0) return -1;
    if(relative_block>=p->block_count&&p->block_count!=0u) return -2;
    if(UINT64_MAX-p->start_block<relative_block) return -3;

    /* Preserve the completed V1 semantics exactly, including the canonical
     * 192-byte zero-padded seed payload used by the original implementation. */
    uint8_t buf[192],digest[64];
    memset(buf,0,sizeof(buf));
    if(!ufr_mc_stream_encode_common(p,p->start_block+relative_block,
                                    buf,sizeof(buf))) return -4;
    static const uint8_t domain[]="UFR-MC-STREAM-SEED-V1-DIGEST";
    if(!digest_parts(EVP_sha512(),domain,sizeof(domain)-1,
                     buf,sizeof(buf),NULL,0,NULL,0,NULL,0,digest,64)) return -4;
    memcpy(out_seed64,digest,64u);
    memset(digest,0,sizeof(digest));
    memset(buf,0,sizeof(buf));
    return 0;
}

int ufr_mc_stream_derive_root_seed64(const ufr_mc_stream_plan_t *p,
                                     uint8_t out_seed64[UFR_MC_STREAM_SEED_BYTES])
{
    return ufr_mc_stream_derive_seed64(p,0u,out_seed64);
}

int ufr_mc_stream_make_id(const ufr_mc_stream_plan_t *p,
                          uint8_t out_id[UFR_MC_STREAM_ID_BYTES])
{
    if(!p||!out_id||ufr_mc_stream_plan_validate(p)!=0) return -1;
    uint8_t buf[160],digest[64];
    memset(buf,0,sizeof(buf));
    const size_t enc_len=ufr_mc_stream_encode_id_plan(p,buf,sizeof(buf));
    if(!enc_len) return -2;
    static const uint8_t domain[]="UFR-MC-STREAM-ID-V1";
    if(!digest_parts(EVP_sha512(),domain,sizeof(domain)-1,
                     buf,enc_len,NULL,0,NULL,0,NULL,0,digest,64)) return -2;
    memcpy(out_id,digest,UFR_MC_STREAM_ID_BYTES);
    memset(digest,0,sizeof(digest));
    memset(buf,0,sizeof(buf));
    return 0;
}

static void ufr_mc_stream_plan_copy_fields(const ufr_mc_stream_plan_t *src,
                                             ufr_mc_stream_plan_t *dst)
{
    if(!src||!dst) return;
    memset(dst,0,sizeof(*dst));
    dst->api_version=src->api_version;
    dst->flags=src->flags;
    memcpy(dst->master_seed,src->master_seed,sizeof(dst->master_seed));
    dst->experiment_id=src->experiment_id;
    dst->run_id=src->run_id;
    dst->replication_id=src->replication_id;
    dst->scenario_id=src->scenario_id;
    dst->crn_group_id=src->crn_group_id;
    dst->logical_stream_id=src->logical_stream_id;
    dst->substream_id=src->substream_id;
    dst->start_block=src->start_block;
    dst->block_count=src->block_count;
    dst->worker_id=src->worker_id;
    dst->worker_count=src->worker_count;
}

int ufr_mc_stream_partition(const ufr_mc_stream_plan_t *p,
                            uint32_t worker_id,
                            uint32_t worker_count,
                            ufr_mc_stream_plan_t *out_plan,
                            ufr_mc_stream_partition_t *out_part)
{
    if(!p||!out_plan||!out_part||ufr_mc_stream_plan_validate(p)!=0||
       worker_count==0u||worker_id>=worker_count) return -1;
    ufr_mc_stream_plan_copy_fields(p,out_plan);
    out_plan->worker_id=worker_id;
    out_plan->worker_count=worker_count;
    const uint64_t n=p->block_count;
    const uint64_t q=n/worker_count;
    const uint64_t rem=n%worker_count;
    uint64_t first_rel=(uint64_t)worker_id*q+
                       (worker_id<rem?(uint64_t)worker_id:rem);
    uint64_t cnt=q+(worker_id<rem?1u:0u);
    if(p->block_count==0u){first_rel=0u;cnt=0u;}
    uint64_t first_block=p->start_block;
    if(cnt!=0u) first_block=p->start_block+first_rel;
    else if(first_rel<=UINT64_MAX-p->start_block) first_block=p->start_block+first_rel;
    else first_block=UINT64_MAX;
    out_plan->start_block=first_block;
    out_plan->block_count=cnt;
    out_part->worker_id=worker_id;
    out_part->worker_count=worker_count;
    out_part->first_block=first_block;
    out_part->block_count=cnt;
    return 0;
}

int ufr_mc_stream_seek(const ufr_mc_stream_plan_t *p,
                       uint64_t absolute_block,
                       ufr_mc_stream_plan_t *out_plan)
{
    if(!p||!out_plan||ufr_mc_stream_plan_validate(p)!=0) return -1;
    if(p->block_count!=0u &&
       (absolute_block<p->start_block ||
        absolute_block-p->start_block>=p->block_count)) return -2;
    ufr_mc_stream_plan_copy_fields(p,out_plan);
    if(p->block_count!=0u)
        out_plan->block_count=p->block_count-(absolute_block-p->start_block);
    out_plan->start_block=absolute_block;
    return 0;
}

int ufr_mc_stream_make_crn_plan(const ufr_mc_stream_plan_t *base,
                                uint64_t scenario_id,
                                uint64_t crn_group_id,
                                ufr_mc_stream_plan_t *out_plan)
{
    if(!base||!out_plan||ufr_mc_stream_plan_validate(base)!=0||crn_group_id==0u)
        return -1;
    ufr_mc_stream_plan_copy_fields(base,out_plan);
    out_plan->flags|=UFR_MC_STREAM_FLAG_CRN|
                     UFR_MC_STREAM_FLAG_WORKER_INDEPENDENT|
                     UFR_MC_STREAM_FLAG_BLOCK_ADDRESSABLE;
    out_plan->scenario_id=scenario_id;
    out_plan->crn_group_id=crn_group_id;
    return 0;
}

int ufr_mc_stream_crn_compatible(const ufr_mc_stream_plan_t *a,
                                 const ufr_mc_stream_plan_t *b)
{
    if(!a||!b||ufr_mc_stream_plan_validate(a)!=0||ufr_mc_stream_plan_validate(b)!=0)
        return 0;
    if(!(a->flags&UFR_MC_STREAM_FLAG_CRN)||!(b->flags&UFR_MC_STREAM_FLAG_CRN)) return 0;
    return memcmp(a->master_seed,b->master_seed,UFR_MC_STREAM_MASTER_SEED_BYTES)==0 &&
           a->experiment_id==b->experiment_id &&
           a->run_id==b->run_id &&
           a->replication_id==b->replication_id &&
           a->crn_group_id==b->crn_group_id &&
           a->logical_stream_id==b->logical_stream_id &&
           a->substream_id==b->substream_id &&
           a->start_block==b->start_block &&
           a->block_count==b->block_count;
}

static void ufr_mc_stream_normalize_plan(const ufr_mc_stream_plan_t *src,
                                         ufr_mc_stream_plan_t *dst)
{
    ufr_mc_stream_plan_copy_fields(src,dst);
}

static const uint64_t UFR_MC_STREAM_SUBSTREAM_OFFSETS[8]={
    UINT64_C(0x243F6A8885A308D3), UINT64_C(0x13198A2E03707344),
    UINT64_C(0xA4093822299F31D0), UINT64_C(0x082EFA98EC4E6C89),
    UINT64_C(0x452821E638D01377), UINT64_C(0xBE5466CF34E90C6C),
    UINT64_C(0xC0AC29B7C97C50DD), UINT64_C(0x3F84D5B5B5470917)
};

static int ufr_mc_stream_rebuild_seedx(UFRXSeedExchange *x,
                                       const ufr_mc_stream_plan_t *plan)
{
    if(!x||!plan) return -1;
    uint8_t roots[8][64];
    memset(roots,0,sizeof(roots));
    for(unsigned i=0;i<8u;++i){
        ufr_mc_stream_plan_t sp;
        ufr_mc_stream_plan_copy_fields(plan,&sp);
        if(UINT64_MAX-sp.substream_id<UFR_MC_STREAM_SUBSTREAM_OFFSETS[i]) return -2;
        sp.substream_id += UFR_MC_STREAM_SUBSTREAM_OFFSETS[i];
        sp.worker_id=0u; sp.worker_count=1u;
        if(ufr_mc_stream_derive_root_seed64(&sp,roots[i])!=0){
            memset(roots,0,sizeof(roots)); return -3;
        }
    }

    atomic_store_explicit(&x->handoff_a.state_flag,UFR_SLOT_FREE,memory_order_release);
    atomic_store_explicit(&x->handoff_b.state_flag,UFR_SLOT_FREE,memory_order_release);
    x->active=0u; x->active_steps=0u; x->switches=0u;
    x->ready_observations=0u; x->missed_ready_checks=0u;

    UFRXWorker *bw[2]={&x->back_a,&x->back_b};
    UFRXSlot  *slot[2]={&x->handoff_a,&x->handoff_b};
    for(unsigned w=0;w<2u;++w){
        for(unsigned s=0;s<4u;++s)
            memcpy(bw[w]->root_seed64[s],roots[w*4u+s],64u);
        bw[w]->next_bank_index=w;
        bw[w]->next_generation=w;
        bw[w]->pending_ready=0u;
        if(!ufrx_prepare_next(bw[w])) { memset(roots,0,sizeof(roots)); return -4; }
        ufrx_copy_set_to_slot(slot[w],&bw[w]->pending_set);
        bw[w]->last_record=bw[w]->pending_record;
        bw[w]->pending_ready=0u;
        /* Preserve the original A/B spacing: generations 0/1 are live first,
         * then each worker advances through 2-bank generation steps. */
        bw[w]->next_bank_index=(uint64_t)w+2u;
        bw[w]->next_generation=(uint64_t)w+2u;
    }
    atomic_store_explicit(&x->handoff_a.state_flag,UFR_SLOT_ACTIVE,memory_order_release);
    atomic_store_explicit(&x->handoff_b.state_flag,UFR_SLOT_READY,memory_order_release);
    memset(roots,0,sizeof(roots));
    return 0;
}

int ufr_paperview_mc_bind_stream(struct ufr_paperview_mc_engine *engine,
                                 const ufr_mc_stream_plan_t *plan)
{
    if(!engine||!plan) return -1;
    if(!engine->impl.initialized) return -2;
    if(engine->started) return -3;
    if(ufr_mc_stream_plan_validate(plan)!=0) return -4;
    ufr_mc_cultivation_engine_t *e=&engine->impl;
    ufr_mc_stream_plan_t normalized;
    ufr_mc_stream_normalize_plan(plan,&normalized);
    uint8_t stream_id[32];
    if(ufr_mc_stream_make_id(&normalized,stream_id)!=0) return -5;
    if(ufr_mc_stream_rebuild_seedx(&e->seedx,&normalized)!=0) return -6;
    ufr_mc_stream_plan_copy_fields(&normalized,&e->stream_plan);
    memcpy(e->stream_id,stream_id,sizeof(e->stream_id));
    e->stream_bound=1u;
    e->replay_mode=0u;
    e->generation_id=e->seedx.handoff_a.generation;
    e->chunks_in_generation=0u;
    e->chunk_id=normalized.start_block;
    ufrx_load(&e->fm,&e->seedx.handoff_a,UFR_DOMAIN_A);
    ufr_paper_final_cache_free(e);
    ufr_paper_r2_cache_free(e);
    return 0;
}

int ufr_paperview_mc_get_stream_id(const struct ufr_paperview_mc_engine *engine,
                                   uint8_t out_id[UFR_MC_STREAM_ID_BYTES])
{
    if(!engine||!out_id||!engine->impl.stream_bound) return -1;
    memcpy(out_id,engine->impl.stream_id,UFR_MC_STREAM_ID_BYTES);
    return 0;
}

static int ufr_mc_checkpoint_hash(const ufr_mc_checkpoint_t *cp, uint8_t out[32])
{
    if(!cp||!out) return 0;
    return ufr_mc_checkpoint_canonical_hash_v2(cp,out)==0;
}
static int ufr_mc_config_hash(const ufr_mc_cultivation_engine_t *e,uint8_t out[32])
{
    if(!e||!out) return 0;
    uint8_t payload[32]; size_t o=0u;
    static const uint8_t domain[]="UFR-MC-CONFIG-CANONICAL-V2";
    ufr_mc_stream_put_be32(payload+o,e->cfg.cultivation); o+=4u;
    ufr_mc_stream_put_be32(payload+o,e->cfg.chunk_bytes); o+=4u;
    ufr_mc_stream_put_be32(payload+o,e->cfg.views_per_chunk); o+=4u;
    ufr_mc_stream_put_be32(payload+o,e->cfg.final_profile); o+=4u;
    ufr_mc_stream_put_be32(payload+o,e->cfg.custom_profile_id); o+=4u;
    ufr_mc_stream_put_be32(payload+o,e->cfg.generation); o+=4u;
    ufr_mc_stream_put_be32(payload+o,e->cfg.min_generation_chunks); o+=4u;
    ufr_mc_stream_put_be32(payload+o,e->cfg.max_generation_chunks); o+=4u;
    return digest_parts(EVP_sha256(),domain,sizeof(domain)-1,
                        payload,sizeof(payload),NULL,0,NULL,0,NULL,0,out,32);
}

int ufr_paperview_mc_checkpoint_capture(
    const struct ufr_paperview_mc_engine *engine,
    ufr_mc_checkpoint_t *out)
{
    if(!engine||!out||!engine->impl.initialized) return -1;
    /* Capture is a deterministic safe-point operation.  Without an engine
     * pause primitive, taking a snapshot while the hot loop is mutating Front
     * state would be a data race and could produce a self-inconsistent state. */
    if(engine->started) return -5;
    const ufr_mc_cultivation_engine_t *e=&engine->impl;
    if(!e->stream_bound) return -2;
    memset(out,0,sizeof(*out));
    out->version=UFR_MC_CHECKPOINT_VERSION;
    out->flags=1u; /* replay-safe */
    memcpy(out->stream_id,e->stream_id,sizeof(out->stream_id));
    ufr_mc_stream_normalize_plan(&e->stream_plan,&out->stream_plan);
    out->logical_block=e->chunk_id;
    out->generation_id=e->generation_id;
    out->chunks_in_generation=e->chunks_in_generation;
    out->chunk_id=e->chunk_id;
    out->mask_phase=e->fm.mask_phase;
    out->parent_xor_omit=e->fm.parent_xor_omit;
    for(unsigned i=0;i<UFR_MC_CHECKPOINT_FRONT_STATES;++i)
        _mm512_storeu_si512((void *)out->front_state[i],e->fm.s[i]);
    memcpy(out->normal_tail_mask,e->retained_starter.tail_mask,sizeof(out->normal_tail_mask));
    memcpy(out->normal_starter,e->retained_starter.starter,sizeof(out->normal_starter));
    if(!ufr_mc_config_hash(e,out->config_sha256)) return -3;
    if(!ufr_mc_checkpoint_hash(out,out->state_sha256)) return -4;
    return 0;
}

int ufr_paperview_mc_checkpoint_restore(
    struct ufr_paperview_mc_engine *engine,
    const ufr_mc_checkpoint_t *cp)
{
    if(!engine||!cp||!engine->impl.initialized) return -1;
    if(cp->version!=UFR_MC_CHECKPOINT_VERSION) return -2;
    if(ufr_mc_stream_plan_validate(&cp->stream_plan)!=0) return -3;
    uint8_t sid[32],cfg_hash[32],state_hash[32];
    if(ufr_mc_stream_make_id(&cp->stream_plan,sid)!=0 ||
       memcmp(sid,cp->stream_id,32u)!=0) return -4;
    if(!ufr_mc_checkpoint_hash(cp,state_hash) ||
       memcmp(state_hash,cp->state_sha256,32u)!=0) return -5;
    if(!ufr_mc_config_hash(&engine->impl,cfg_hash) ||
       memcmp(cfg_hash,cp->config_sha256,32u)!=0) return -6;

    ufr_mc_cultivation_engine_t *e=&engine->impl;
    if(engine->started){
        ufr_mc_engine_stop(e);
        engine->started=0u;
    }
    atomic_store_explicit(&e->stop,0,memory_order_relaxed);
    ufr_mc_stream_plan_copy_fields(&cp->stream_plan,&e->stream_plan);
    memcpy(e->stream_id,cp->stream_id,sizeof(e->stream_id));
    e->stream_bound=1u;
    e->replay_mode=1u;
    e->generation_id=cp->generation_id;
    e->chunks_in_generation=cp->chunks_in_generation;
    e->chunk_id=cp->chunk_id;
    e->fm.mask_phase=cp->mask_phase;
    e->fm.parent_xor_omit=cp->parent_xor_omit;
    for(unsigned i=0;i<UFR_MC_CHECKPOINT_FRONT_STATES;++i)
        e->fm.s[i]=_mm512_loadu_si512((const void *)cp->front_state[i]);
    memcpy(e->retained_starter.tail_mask,cp->normal_tail_mask,sizeof(cp->normal_tail_mask));
    memcpy(e->retained_starter.starter,cp->normal_starter,sizeof(cp->normal_starter));
    e->seedx.active_steps=0u;
    e->seedx.ready_observations=0u;
    e->seedx.missed_ready_checks=0u;
    ufr_paper_final_cache_free(e);
    ufr_paper_r2_cache_free(e);
    memset(sid,0,sizeof(sid)); memset(cfg_hash,0,sizeof(cfg_hash)); memset(state_hash,0,sizeof(state_hash));
    return 0;
}

int ufr_mc_checkpoint_save(const char *path,const ufr_mc_checkpoint_t *cp)
{
    if(!path||!cp||cp->version!=UFR_MC_CHECKPOINT_VERSION) return -1;
    uint8_t hash[32];
    if(!ufr_mc_checkpoint_hash(cp,hash)||memcmp(hash,cp->state_sha256,32u)!=0) return -2;
    FILE *fp=fopen(path,"wb");
    if(!fp) return -3;
    static const uint8_t magic[8]={'U','F','R','C','P','V','1','\0'};
    const size_t ok1=fwrite(magic,1,sizeof(magic),fp);
    const size_t ok2=fwrite(cp,1,sizeof(*cp),fp);
    const int ok3=(fclose(fp)==0);
    return (ok1==sizeof(magic)&&ok2==sizeof(*cp)&&ok3)?0:-4;
}

int ufr_mc_checkpoint_load(const char *path,ufr_mc_checkpoint_t *cp)
{
    if(!path||!cp) return -1;
    FILE *fp=fopen(path,"rb");
    if(!fp) return -2;
    uint8_t magic[8];
    const size_t ok1=fread(magic,1,sizeof(magic),fp);
    const size_t ok2=fread(cp,1,sizeof(*cp),fp);
    const int ok3=(fclose(fp)==0);
    static const uint8_t expected[8]={'U','F','R','C','P','V','1','\0'};
    if(ok1!=sizeof(magic)||memcmp(magic,expected,sizeof(expected))!=0||
       ok2!=sizeof(*cp)||!ok3) return -3;
    if(cp->version!=UFR_MC_CHECKPOINT_VERSION) return -4;
    uint8_t hash[32];
    if(!ufr_mc_checkpoint_hash(cp,hash)||memcmp(hash,cp->state_sha256,32u)!=0) return -5;
    return 0;
}

static uint64_t ufr_mc_double_bits(double x)
{
    uint64_t u=0u; memcpy(&u,&x,sizeof(u)); return u;
}

int ufr_paperview_mc_exact_replay_verify(
    struct ufr_paperview_mc_engine *engine,
    const ufr_mc_checkpoint_t *cp,
    uint64_t chunks,
    ufr_mc_exact_replay_result_t *out)
{
    if(!engine||!cp||!out||chunks==0u) return -1;
    memset(out,0,sizeof(*out));
    uint64_t sa=0,sb=0,ha=0,hb=0; double suma=0.0,sumb=0.0;
    if(ufr_paperview_mc_checkpoint_restore(engine,cp)!=0) return -2;
    if(ufr_mc_run_representative(&engine->impl,chunks,&sa,&ha,&suma)!=0) return -3;
    out->samples_a=sa; out->hits_a=ha; out->sum_bits_a=ufr_mc_double_bits(suma);
    out->next_block_a=engine->impl.chunk_id;
    if(ufr_paperview_mc_checkpoint_restore(engine,cp)!=0) return -4;
    if(ufr_mc_run_representative(&engine->impl,chunks,&sb,&hb,&sumb)!=0) return -5;
    out->samples_b=sb; out->hits_b=hb; out->sum_bits_b=ufr_mc_double_bits(sumb);
    out->next_block_b=engine->impl.chunk_id;
    out->exact_match=(uint8_t)(sa==sb&&ha==hb&&out->sum_bits_a==out->sum_bits_b&&
                               out->next_block_a==out->next_block_b);
    (void)ufr_paperview_mc_checkpoint_restore(engine,cp);
    return out->exact_match?0:-6;
}

void ufr_mc_uncertainty_config_default(ufr_mc_uncertainty_config_t *cfg)
{
    if(!cfg) return;
    cfg->min_samples=1024u;
    cfg->max_relative_halfwidth=0.0; /* reporting only by default */
    cfg->max_invalid_fraction=0.0;   /* reporting only by default */
}

static double ufr_mc_tcrit95(uint64_t df)
{
    static const double t[30]={
        12.7062047364,6.3137515147,4.3026527299,3.1824463053,2.5705818356,
        2.4469118511,2.3646242510,2.3060041350,2.2621571629,2.2281388519,
        2.2010241990,2.1788128297,2.1603686565,2.1447866879,2.1314495456,
        2.1199052992,2.1098155778,2.1009220402,2.0930240544,2.0859634473,
        2.0796138447,2.0738730689,2.0686576104,2.0638985626,2.0595385528,
        2.0555294386,2.0518305165,2.0484071418,2.0452296421,2.0422724563};
    if(df==0u) return NAN;
    return df<=30u?t[df-1u]:1.95996398454005;
}

int ufr_mc_uncertainty_report_make(uint64_t samples,double mean,double variance,
                                   uint64_t invalid_values,
                                   const ufr_mc_uncertainty_config_t *cfg,
                                   ufr_mc_uncertainty_report_t *out)
{
    if(!out) return -1;
    ufr_mc_uncertainty_config_t local;
    if(cfg) local=*cfg; else ufr_mc_uncertainty_config_default(&local);
    if(local.min_samples==0u||local.max_relative_halfwidth<0.0||local.max_invalid_fraction<0.0||
       local.max_invalid_fraction>1.0) return -2;
    memset(out,0,sizeof(*out));
    out->samples=samples; out->invalid_values=invalid_values;
    out->mean=mean; out->variance=variance;
    const uint64_t total=(UINT64_MAX-samples<invalid_values)?UINT64_MAX:samples+invalid_values;
    out->invalid_fraction=total?((double)invalid_values/(double)total):0.0;
    out->finite_ok=(uint8_t)(samples>1u&&isfinite(mean)&&isfinite(variance)&&variance>=0.0);
    out->sample_count_ok=(uint8_t)(samples>=local.min_samples);
    double tc=ufr_mc_tcrit95(samples? samples-1u:0u);
    if(out->finite_ok){
        out->sem=sqrt(variance/(double)samples);
        out->ci95_halfwidth=tc*out->sem;
        out->ci95_low=mean-out->ci95_halfwidth;
        out->ci95_high=mean+out->ci95_halfwidth;
        if(mean!=0.0) out->relative_ci95_halfwidth=fabs(out->ci95_halfwidth/mean);
        else out->relative_ci95_halfwidth=INFINITY;
        out->finite_ok=(uint8_t)(isfinite(out->sem)&&isfinite(out->ci95_halfwidth)&&
                                  isfinite(out->ci95_low)&&isfinite(out->ci95_high));
    }
    strncpy(out->method,"Student-t, 95%",sizeof(out->method)-1u);
    out->relative_precision_ok=(uint8_t)(local.max_relative_halfwidth<=0.0 ||
        (out->finite_ok&&out->relative_ci95_halfwidth<=local.max_relative_halfwidth));
    out->invalid_fraction_ok=(uint8_t)(local.max_invalid_fraction<=0.0 ||
        out->invalid_fraction<=local.max_invalid_fraction);
    out->passed=(uint8_t)(out->finite_ok&&out->sample_count_ok&&
                          out->relative_precision_ok&&out->invalid_fraction_ok);
    return 0;
}

int ufr_mc_uncertainty_report_write_json(const char *path,
                                         const ufr_mc_uncertainty_report_t *r)
{
    if(!path||!r) return -1;
    FILE *fp=fopen(path,"wb");
    if(!fp) return -2;
    const int rc=fprintf(fp,
        "{\n"
        "  \"samples\": %" PRIu64 ",\n"
        "  \"invalid_values\": %" PRIu64 ",\n"
        "  \"mean\": %.17g,\n"
        "  \"variance\": %.17g,\n"
        "  \"sem\": %.17g,\n"
        "  \"ci95_low\": %.17g,\n"
        "  \"ci95_high\": %.17g,\n"
        "  \"ci95_halfwidth\": %.17g,\n"
        "  \"relative_ci95_halfwidth\": %.17g,\n"
        "  \"invalid_fraction\": %.17g,\n"
        "  \"finite_ok\": %u,\n"
        "  \"sample_count_ok\": %u,\n"
        "  \"relative_precision_ok\": %u,\n"
        "  \"invalid_fraction_ok\": %u,\n"
        "  \"passed\": %u,\n"
        "  \"method\": \"%s\"\n"
        "}\n",
        r->samples,r->invalid_values,r->mean,r->variance,r->sem,
        r->ci95_low,r->ci95_high,r->ci95_halfwidth,
        r->relative_ci95_halfwidth,r->invalid_fraction,
        (unsigned)r->finite_ok,(unsigned)r->sample_count_ok,
        (unsigned)r->relative_precision_ok,(unsigned)r->invalid_fraction_ok,
        (unsigned)r->passed,r->method);
    const int ok=(rc>0 && fclose(fp)==0)?0:-3;
    return ok;
}

int ufr_mc_golden_reference_verify(void)
{
    ufr_mc_stream_plan_t p;
    ufr_mc_stream_plan_default(&p);
    for(unsigned i=0;i<32u;++i) p.master_seed[i]=(uint8_t)i;
    p.experiment_id=UINT64_C(0x0102030405060708);
    p.run_id=UINT64_C(0x1112131415161718);
    p.replication_id=UINT64_C(0x2122232425262728);
    p.scenario_id=123u;
    p.logical_stream_id=UINT64_C(0x3132333435363738);
    p.substream_id=UINT64_C(0x4142434445464748);
    p.start_block=7u;
    p.block_count=100u;
    if(ufr_mc_stream_plan_validate(&p)!=0) return -1;

    static const uint8_t g0[64]={0x65,0x27,0x73,0x3c,0xfa,0xbd,0xf7,0x70,0x71,0xd9,0x0f,0x33,0xfe,0xc5,0x22,0x8c,0xc5,0xdd,0x06,0xaa,0xcc,0x5c,0xe4,0x62,0xd5,0x99,0xf4,0x87,0x89,0x8b,0x6d,0x62,0x66,0x37,0x4b,0x3a,0x46,0x8f,0x8a,0x89,0x7e,0xf5,0x1c,0x83,0x73,0xda,0x6e,0x43,0x45,0x4b,0xce,0x51,0x08,0x02,0x0c,0xee,0xff,0x93,0x8c,0x4b,0xff,0xef,0x00,0xaa};
    static const uint8_t g1[64]={0x2a,0xa1,0x85,0xe2,0x56,0x8f,0x7b,0x32,0x58,0x47,0xf4,0xb0,0x01,0x84,0xcb,0xb5,0x1f,0x59,0x32,0xb2,0x79,0x6e,0x71,0x67,0x75,0x47,0xa0,0x69,0xf1,0xba,0x11,0x00,0xfd,0x43,0x36,0xaa,0x29,0x35,0x64,0xe5,0x6b,0x2b,0xa5,0x1e,0xb0,0xb3,0x3f,0x3d,0xdb,0x23,0xa2,0x05,0x5f,0x89,0x57,0xf1,0x73,0x17,0x58,0x20,0x89,0x10,0x9c,0x85};
    uint8_t got[64],id[32];
    if(ufr_mc_stream_derive_seed64(&p,0u,got)!=0||memcmp(got,g0,64u)!=0) return -2;
    if(ufr_mc_stream_derive_seed64(&p,1u,got)!=0||memcmp(got,g1,64u)!=0) return -3;
    static const uint8_t expected_id[32]={0xb2,0xc8,0x70,0xbe,0xf6,0x1b,0x74,0x80,0x04,0x67,0x34,0x72,0xc7,0x84,0x20,0xec,0xae,0x4b,0x89,0x43,0xeb,0xda,0x1c,0x13,0x2e,0x4d,0x6b,0xdd,0x78,0x30,0x7f,0x1c};
    if(ufr_mc_stream_make_id(&p,id)!=0||memcmp(id,expected_id,32u)!=0) return -4;

    ufr_mc_stream_plan_t crn_a,crn_b,non_a,non_b;
    if(ufr_mc_stream_make_crn_plan(&p,101u,9u,&crn_a)!=0||
       ufr_mc_stream_make_crn_plan(&p,202u,9u,&crn_b)!=0||
       !ufr_mc_stream_crn_compatible(&crn_a,&crn_b)) return -5;
    uint8_t ca[64],cb[64];
    if(ufr_mc_stream_derive_seed64(&crn_a,0u,ca)!=0||
       ufr_mc_stream_derive_seed64(&crn_b,0u,cb)!=0||memcmp(ca,cb,64u)!=0) return -6;
    non_a=p; non_b=p; non_a.scenario_id=101u; non_b.scenario_id=202u;
    if(ufr_mc_stream_derive_seed64(&non_a,0u,ca)!=0||
       ufr_mc_stream_derive_seed64(&non_b,0u,cb)!=0||memcmp(ca,cb,64u)==0) return -7;

    ufr_mc_stream_partition_t part; ufr_mc_stream_plan_t wp;
    if(ufr_mc_stream_partition(&p,3u,7u,&wp,&part)!=0) return -8;
    if(part.first_block!=51u||part.block_count!=14u||wp.start_block!=51u||wp.block_count!=14u) return -9;

    ufr_mc_stream_plan_t seekp;
    if(ufr_mc_stream_seek(&p,49u,&seekp)!=0) return -10;
    if(ufr_mc_stream_derive_seed64(&seekp,0u,ca)!=0||
       ufr_mc_stream_derive_seed64(&p,42u,cb)!=0||memcmp(ca,cb,64u)!=0) return -11;

    ufr_mc_uncertainty_report_t ur;
    if(ufr_mc_uncertainty_report_make(100000u,2.5,1.6666666666666667,0u,NULL,&ur)!=0||!ur.passed) return -12;
    return 0;
}

int ufr_mc_enterprise_selftest(void)
{
    const int rc=ufr_mc_golden_reference_verify();
    return rc==0?0:rc;
}



/* -------------------------------------------------------------------------
 * Enterprise Finance Early Exercise adapter V1
 * ------------------------------------------------------------------------- */
#define UFR_FINANCE_EARLY_ENGINE_INTEGRATION 1
#include "UltraFastRng512_MC_Finance_Early_v1.c"
#undef UFR_FINANCE_EARLY_ENGINE_INTEGRATION

/* Enterprise Pharma VPop + Parameter-Uncertainty adapter V1.  This remains an
 * outer-loop/domain module and does not alter the Front/Back hot path. */
#include "UltraFastRng512_MC_Pharma_VPop_Uncertainty_v1.c"


/* Portable checkpoint wire V2 implementation is part of the enterprise TU. */
#include "UltraFastRng512_MC_CheckpointPortable_v2.c"

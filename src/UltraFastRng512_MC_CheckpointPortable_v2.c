#include "UltraFastRng512_MC_CheckpointPortable_v2.h"

#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

static void be16(uint8_t *p, uint16_t x)
{
    p[0] = (uint8_t)(x >> 8);
    p[1] = (uint8_t)x;
}

static void be32(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)(x >> 24);
    p[1] = (uint8_t)(x >> 16);
    p[2] = (uint8_t)(x >> 8);
    p[3] = (uint8_t)x;
}

static void be64(uint8_t *p, uint64_t x)
{
    for (unsigned i = 0; i < 8u; ++i)
        p[i] = (uint8_t)(x >> (56u - 8u * i));
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint64_t rd64(const uint8_t *p)
{
    uint64_t x = 0u;
    for (unsigned i = 0; i < 8u; ++i)
        x = (x << 8) | p[i];
    return x;
}

static size_t encode_plan(const ufr_mc_stream_plan_t *p, uint8_t *b)
{
    size_t o = 0u;
    be32(b + o, p->api_version); o += 4u;
    be32(b + o, p->flags); o += 4u;
    memcpy(b + o, p->master_seed, 32u); o += 32u;
    be64(b + o, p->experiment_id); o += 8u;
    be64(b + o, p->run_id); o += 8u;
    be64(b + o, p->replication_id); o += 8u;
    be64(b + o, p->scenario_id); o += 8u;
    be64(b + o, p->crn_group_id); o += 8u;
    be64(b + o, p->logical_stream_id); o += 8u;
    be64(b + o, p->substream_id); o += 8u;
    be64(b + o, p->start_block); o += 8u;
    be64(b + o, p->block_count); o += 8u;
    be32(b + o, p->worker_id); o += 4u;
    be32(b + o, p->worker_count); o += 4u;
    return o;
}

static size_t decode_plan(const uint8_t *b, size_t cap, ufr_mc_stream_plan_t *p)
{
    if (!b || !p || cap < 120u)
        return 0u;

    size_t o = 0u;
    memset(p, 0, sizeof(*p));
    p->api_version = rd32(b + o); o += 4u;
    p->flags = rd32(b + o); o += 4u;
    memcpy(p->master_seed, b + o, 32u); o += 32u;
    p->experiment_id = rd64(b + o); o += 8u;
    p->run_id = rd64(b + o); o += 8u;
    p->replication_id = rd64(b + o); o += 8u;
    p->scenario_id = rd64(b + o); o += 8u;
    p->crn_group_id = rd64(b + o); o += 8u;
    p->logical_stream_id = rd64(b + o); o += 8u;
    p->substream_id = rd64(b + o); o += 8u;
    p->start_block = rd64(b + o); o += 8u;
    p->block_count = rd64(b + o); o += 8u;
    p->worker_id = rd32(b + o); o += 4u;
    p->worker_count = rd32(b + o); o += 4u;
    return o;
}

static size_t encode_payload(const ufr_mc_checkpoint_t *c,
                             uint8_t *b,
                             int include_state_hash)
{
    size_t o = 0u;
    be32(b + o, c->version); o += 4u;
    be32(b + o, c->flags); o += 4u;
    memcpy(b + o, c->stream_id, 32u); o += 32u;
    o += encode_plan(&c->stream_plan, b + o);
    be64(b + o, c->logical_block); o += 8u;
    be64(b + o, c->generation_id); o += 8u;
    be64(b + o, c->chunks_in_generation); o += 8u;
    be64(b + o, c->chunk_id); o += 8u;
    be32(b + o, c->mask_phase); o += 4u;
    be32(b + o, c->parent_xor_omit); o += 4u;
    memcpy(b + o, c->front_state, 16u * 64u); o += 16u * 64u;

    for (unsigned i = 0u; i < 1024u; ++i) {
        be16(b + o, c->normal_tail_mask[i]);
        o += 2u;
    }
    for (unsigned i = 0u; i < 16384u; ++i) {
        be16(b + o, (uint16_t)c->normal_starter[i]);
        o += 2u;
    }

    memcpy(b + o, c->config_sha256, 32u); o += 32u;
    if (include_state_hash) {
        memcpy(b + o, c->state_sha256, 32u); o += 32u;
    }
    return o;
}

int ufr_mc_checkpoint_canonical_hash_v2(
    const ufr_mc_checkpoint_t *c,
    uint8_t out[32])
{
    if (!c || !out || ufr_mc_stream_plan_validate(&c->stream_plan) != 0)
        return -1;

    uint8_t buf[UFR_MC_CHECKPOINT_WIRE_PAYLOAD_BYTES];
    memset(buf, 0, sizeof(buf));
    const size_t n = encode_payload(c, buf, 0);
    if (n != UFR_MC_CHECKPOINT_WIRE_PAYLOAD_BYTES - 32u)
        return -2;

    static const uint8_t domain[] = "UFR-MC-CHECKPOINT-CANONICAL-V2";
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx)
        return -3;

    unsigned got = 0u;
    int ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1;
    if (ok)
        ok = EVP_DigestUpdate(ctx, domain, sizeof(domain) - 1u) == 1;
    if (ok)
        ok = EVP_DigestUpdate(ctx, buf, n) == 1;
    if (ok)
        ok = EVP_DigestFinal_ex(ctx, out, &got) == 1 && got == 32u;

    EVP_MD_CTX_free(ctx);
    memset(buf, 0, sizeof(buf));
    return ok ? 0 : -4;
}

int ufr_mc_checkpoint_reseal_v2(ufr_mc_checkpoint_t *c)
{
    if (!c || ufr_mc_stream_plan_validate(&c->stream_plan) != 0)
        return -1;

    uint8_t id[32], hash[32];
    if (ufr_mc_stream_make_id(&c->stream_plan, id) != 0)
        return -2;
    memcpy(c->stream_id, id, 32u);

    if (ufr_mc_checkpoint_canonical_hash_v2(c, hash) != 0) {
        memset(id, 0, sizeof(id));
        return -3;
    }
    memcpy(c->state_sha256, hash, 32u);
    memset(id, 0, sizeof(id));
    memset(hash, 0, sizeof(hash));
    return 0;
}

int ufr_mc_checkpoint_save_portable_v2(
    const char *path,
    const ufr_mc_checkpoint_t *c)
{
    if (!path || !c || c->version != UFR_MC_CHECKPOINT_VERSION)
        return -1;

    uint8_t hash[32];
    if (ufr_mc_checkpoint_canonical_hash_v2(c, hash) != 0 ||
        memcmp(hash, c->state_sha256, 32u) != 0) {
        memset(hash, 0, sizeof(hash));
        return -2;
    }
    memset(hash, 0, sizeof(hash));

    uint8_t payload[UFR_MC_CHECKPOINT_WIRE_PAYLOAD_BYTES];
    if (encode_payload(c, payload, 1) != sizeof(payload)) {
        memset(payload, 0, sizeof(payload));
        return -3;
    }

    static const uint8_t magic[UFR_MC_CHECKPOINT_WIRE_MAGIC_BYTES] =
        {'U','F','R','C','W','2','\0','\0'};
    uint8_t hdr[8];
    be32(hdr + 0u, UFR_MC_CHECKPOINT_WIRE_VERSION);
    be32(hdr + 4u, UFR_MC_CHECKPOINT_WIRE_PAYLOAD_BYTES);

    FILE *f = fopen(path, "wb");
    if (!f) {
        memset(payload, 0, sizeof(payload));
        return -4;
    }

    int ok = 1;
    if (fwrite(magic, 1u, sizeof(magic), f) != sizeof(magic)) ok = 0;
    if (ok && fwrite(hdr, 1u, sizeof(hdr), f) != sizeof(hdr)) ok = 0;
    if (ok && fwrite(payload, 1u, sizeof(payload), f) != sizeof(payload)) ok = 0;
    if (fclose(f) != 0) ok = 0;

    memset(payload, 0, sizeof(payload));
    return ok ? 0 : -5;
}

int ufr_mc_checkpoint_load_portable_v2(
    const char *path,
    ufr_mc_checkpoint_t *c)
{
    if (!path || !c)
        return -1;

    FILE *f = fopen(path, "rb");
    if (!f)
        return -2;

    uint8_t magic[8];
    uint8_t hdr[8];
    uint8_t payload[UFR_MC_CHECKPOINT_WIRE_PAYLOAD_BYTES];
    const size_t got_magic = fread(magic, 1u, sizeof(magic), f);
    const size_t got_hdr = fread(hdr, 1u, sizeof(hdr), f);
    const size_t got_payload = fread(payload, 1u, sizeof(payload), f);
    const int tail = fgetc(f);
    const int closed = fclose(f) == 0;

    static const uint8_t expected_magic[8] =
        {'U','F','R','C','W','2','\0','\0'};
    if (got_magic != sizeof(magic) ||
        got_hdr != sizeof(hdr) ||
        memcmp(magic, expected_magic, sizeof(magic)) != 0 ||
        rd32(hdr + 0u) != UFR_MC_CHECKPOINT_WIRE_VERSION ||
        rd32(hdr + 4u) != UFR_MC_CHECKPOINT_WIRE_PAYLOAD_BYTES ||
        got_payload != sizeof(payload) ||
        tail != EOF || !closed) {
        memset(payload, 0, sizeof(payload));
        return -3;
    }

    memset(c, 0, sizeof(*c));
    size_t o = 0u;
    c->version = rd32(payload + o); o += 4u;
    c->flags = rd32(payload + o); o += 4u;
    memcpy(c->stream_id, payload + o, 32u); o += 32u;
    if (decode_plan(payload + o, sizeof(payload) - o, &c->stream_plan) != 120u) {
        memset(payload, 0, sizeof(payload));
        return -4;
    }
    o += 120u;

    c->logical_block = rd64(payload + o); o += 8u;
    c->generation_id = rd64(payload + o); o += 8u;
    c->chunks_in_generation = rd64(payload + o); o += 8u;
    c->chunk_id = rd64(payload + o); o += 8u;
    c->mask_phase = rd32(payload + o); o += 4u;
    c->parent_xor_omit = rd32(payload + o); o += 4u;
    memcpy(c->front_state, payload + o, 16u * 64u); o += 16u * 64u;

    for (unsigned i = 0u; i < 1024u; ++i) {
        c->normal_tail_mask[i] = rd16(payload + o);
        o += 2u;
    }
    for (unsigned i = 0u; i < 16384u; ++i) {
        c->normal_starter[i] = (int16_t)rd16(payload + o);
        o += 2u;
    }

    memcpy(c->config_sha256, payload + o, 32u); o += 32u;
    memcpy(c->state_sha256, payload + o, 32u); o += 32u;

    if (o != sizeof(payload) ||
        c->version != UFR_MC_CHECKPOINT_VERSION ||
        c->logical_block != c->chunk_id ||
        c->mask_phase >= 13u ||
        c->parent_xor_omit > 2u ||
        ufr_mc_stream_plan_validate(&c->stream_plan) != 0) {
        memset(payload, 0, sizeof(payload));
        return -5;
    }

    uint8_t id[32], hash[32];
    const int valid =
        ufr_mc_stream_make_id(&c->stream_plan, id) == 0 &&
        memcmp(id, c->stream_id, 32u) == 0 &&
        ufr_mc_checkpoint_canonical_hash_v2(c, hash) == 0 &&
        memcmp(hash, c->state_sha256, 32u) == 0;
    memset(id, 0, sizeof(id));
    memset(hash, 0, sizeof(hash));
    memset(payload, 0, sizeof(payload));
    return valid ? 0 : -6;
}

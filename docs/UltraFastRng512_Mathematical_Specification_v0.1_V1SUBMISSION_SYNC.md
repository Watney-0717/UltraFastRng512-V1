# UltraFastRng512 — Mathematical / Algorithm Specification v0.1

Date: 2026-09-29  
Source basis: current integrated research source snapshot materialized from Library  
Source snapshot SHA-256: `40a6b4311b4cb0e4348f08990b5380ff7607444a92534e17e7014b997af393ba`  
Specification status: V1 submission-synchronized implementation specification; not a statistical certification.

## 0. Scope

This revision is synchronized to the frozen V1 submission source. The source-release cleanup changes presentation/build metadata only; it does not introduce a new RNG/MC transform. Historical benchmark reports may therefore retain the pre-submission source hash `cd8dae...`; those measurements should be cited as provenance for the measured snapshot rather than as freshly executed results from the release-copy hash.

This document fixes the mathematical meaning of the current mainline pipeline without changing the algorithm itself. It separates the seed/back construction, the 16-state Front recurrence, Normal conversion, PaperView/final cultivation, and the MC consumer boundary.

The current implementation contains additional enterprise/control-plane options. They are intentionally not substituted into the core mathematical definition unless they alter the numeric data path.

## 1. Pipeline

```text
Raw entropy / test source
      ↓
Light cleanup
      ↓
SHA-256 conditioning
      ↓
LPS / PSL2(F29), 4 lanes
      ↓
512-bit Root Seed
      ↓
Seed Bank entry
      ↓
CHACHA20-AAA-A1 PROFILE
      ↓
V → H → V Front16 seed expansion
      ↓
3 same-PROFILE parent lineages → 16 resident states
      ↓
Front state recurrence
      ↓
Normal v9 LUT transform
      ↓
4-way fanout
      ↓
Normal Starter / PaperView
      ↓
final cache or direct transport
      ↓
MC consumer (domain-dependent)
```

## 2. Seed conditioning

Input is treated as 64-byte blocks. The current light-cleanup stage removes an all-zero block and suppresses an immediately repeated identical 64-byte block. The cleanup is preprocessing, not an entropy-rate proof.

Let `R` be the raw byte string after this preprocessing and `meta=(NodeID, RunID, Sequence, ...)`. The conditioned value is:

`C = SHA256( "UFRNG-ENTROPY-CONDITIONER-V2" || R || NodeID || RunID || BE64(Sequence) )`

`C` is 256 bits. Node ID / Run ID / Sequence are domain-separation inputs, not entropy claims.

## 3. LPS / PSL2(F29)

Parameters:

- `p = 5`, `q = 29`
- `LPS_LANES = 4`
- start walk: 8 accepted symbols
- walk: 16 accepted symbols
- degree: 6

The six fixed generators are the following matrices modulo 29:

```text
G0 = [[10,  0], [ 0,  3]]
G1 = [[ 8, 13], [16,  8]]
G2 = [[ 8, 11], [11,  8]]
G3 = [[ 8, 18], [18,  8]]
G4 = [[ 8, 16], [13,  8]]
G5 = [[ 3,  0], [ 0, 10]]
```

For lane `ℓ`, independent start/walk seeds are derived with domain-separated SHA-512. Candidate 3-bit symbols are accepted only when they are in `0..5`; values `6` and `7` are rejected and the selector continues until enough symbols are obtained.

For each walk, starting from `X0 = I`, the state update is:

`X(i+1) = canon( X(i) * G[s_i] mod 29 )`

where `canon(X)` chooses the lexicographically smaller of `X` and `-X (mod 29)` according to the current implementation.

For lane `ℓ`, let `E_ℓ` be the final canonical matrix. The 4 endpoints, lane identifiers, LPS parameters, and the extraction domain are assembled into a payload, and:

`RootSeed = SHA512(payload)`

giving 512 bits.

## 4. Seed Bank

For root seed `Root` and bank index `b`:

`Bank_b = SHA512( "UFRNG-SEED-BANK-V1" || Root || BE64(b) )`

`Bank_b` is 512 bits.

## 5. CHACHA20-AAA-A1 PROFILE

Key derivation:

`K = SHA256( "UFRNG-PROFILE-CHACHA20-KEY-V1" || Bank_b )`

`M = SHA256( "UFRNG-PROFILE-CHACHA20-NONCE-V1" || Bank_b || BE64(Sequence) )`

`Nonce = M[0:12]`

Two ChaCha blocks are generated. The AAA core uses 14 double-rounds (28 rounds total). After the column half of double-round index 9, exactly once:

`x[8] = x[8] + 0x3B54CDA5  (mod 2^32)`

The diagonal half then runs normally. The public 64-byte profile output is the first 32 bytes of block 0 followed by the first 32 bytes of block 1.

Important architectural boundary: AAA is Back/Seed-side only; it is not called from the Front hot loop.

## 6. V → H → V Front16 expansion

First, for each lane `i = 0..15`, the V1 expansion is:

`V1_i = SHA512( "UFRNG-FRONT16-SHA512-V1" || ProfiledSeed || BE64(bank) || BE64(generation) || BE32(i) )`

The complete 16×64-byte matrix is then hashed globally:

`H = SHA512( "UFRNG-FRONT16-VHV-HASH-V1" || V1_0 || ... || V1_15 || BE64(bank) || BE64(generation) )`

Finally:

`V2_i = SHA512( "UFRNG-FRONT16-VHV-COMPACT-V1" || V1_i || H || BE32(i) )`

Each `V2_i` is 64 bytes. This is the implementation-level meaning of V→H→V in the current mainline.

## 7. Three parent lineages and 16-state resident budget

The current multi-parent construction prepares three same-PROFILE VHV lineages `A,B,C`. A generation-dependent permutation is chosen from the six permutations of `(0,1,2)` with:

`π(g) = floor(g / 2) mod 6`

Three live parent states are the lane-0 states of the selected lineages. The other 13 resident states are populated from lineages A/B/C, preserving the total resident budget of 16×64-byte states. State 15 is:

`m15 = A[1] XOR B[1] XOR C[1]`

In the current implementation the Back worker stores four independently conditioned root seeds for each Back slot, while the current `UFR_MULTI_K=3` preparation path consumes three lineages for the 3-parent construction.

## 8. Front recurrence

At each micro-step:

`a=p0, b=p1, c=p2`

and the fixed generation-selected mode is `mode = floor(generation/2) mod 3`.

### Mode 0
`p0 = a XOR ROTL64(b,17)`  
`p1 = b + ROTL64(c,29)`  
`p2 = c + ROTL64(a,41)`

### Mode 1
`p0 = a + ROTL64(b,17)`  
`p1 = b XOR ROTL64(c,29)`  
`p2 = c + ROTL64(a,41)`

### Mode 2
`p0 = a + ROTL64(b,17)`  
`p1 = b + ROTL64(c,29)`  
`p2 = c XOR ROTL64(a,41)`

The 13 auxiliary states use a 13-phase schedule. The current exact update table is:

| phase | updated state | update |
|---:|---|---|
| 0 | m3 | `(m3 + ROTL64(p0,7)) XOR ROTL64(m3,13)` |
| 1 | m4 | `(m4 + ROTL64(p0,13)) XOR ROTL64(m4,17)` |
| 2 | m5 | `(m5 + ROTL64(p0,17)) XOR ROTL64(m5,21)` |
| 3 | m6 | `(m6 + ROTL64(p0,23)) XOR ROTL64(m6,29)` |
| 4 | m7 | `(m7 + ROTL64(p1,29)) XOR ROTL64(m7,33)` |
| 5 | m8 | `(m8 + ROTL64(p1,31)) XOR ROTL64(m8,37)` |
| 6 | m9 | `(m9 + ROTL64(p1,37)) XOR ROTL64(m9,41)` |
| 7 | m10 | `(m10 + ROTL64(p1,42)) XOR ROTL64(m10,47)` |
| 8 | m11 | `(m11 + ROTL64(p2,11)) XOR ROTL64(m11,17)` |
| 9 | m12 | `(m12 + ROTL64(p2,19)) XOR ROTL64(m12,23)` |
| 10 | m13 | `(m13 + ROTL64(p2,27)) XOR ROTL64(m13,31)` |
| 11 | m14 | `(m14 + ROTL64(p2,37)) XOR ROTL64(m14,41)` |
| 12 | m15 | `(m15 + ROTL64(p2,43)) XOR ROTL64(m15,47)` |

The per-step emission order is exactly:

```text
p0
p1
p2
p0 XOR m3
p1 XOR m7
p2 + ROTL64(m11,1)
p0 XOR m4
p1 XOR m8
p2 XOR m12
p0 XOR m5
p1 XOR m9
p2 XOR m13
p0 XOR m6
p1 XOR m10
p2 XOR m14
p1 XOR m15
```

One micro-step therefore emits 16 vectors before Normal conversion. With the four-way fanout this corresponds to 1024 logical Normal-domain samples per micro-step.

## 9. Normal v9

The active transform accepts 16 uint32 values in one ZMM:

`x = load512(in)`

The 128-byte table is selected through VPERMI2B. The resulting byte vector is passed to VPMADDUBSW with `x`, then VPMADDWD with an all-ones int16 vector:

`a = LUT128_VPERMI2B(x)`  
`p16 = VPMADDUBSW(a, x)`  
`q = VPMADDWD(p16, 1)`

Thus the active transform produces 16 int32 `q` values with integer SIMD only. `q` is an internal quantized Normal representation; the fixed downstream scale is required for the Normal-domain interpretation.

Current scale macro:

`UF_NORMAL_SCALE = (0.0009317057574691516f * 1.0007781f)`

## 10. Four-way fanout

The active hot path uses two fixed `_mm256_shuffle_epi32` patterns:

`y0 = SHUFFLE(q, _MM_SHUFFLE(2,3,0,1))`, which is the per-128-bit-lane index order `(1,0,3,2)`.

`y1 = SHUFFLE(q, _MM_SHUFFLE(1,0,3,2))`, which is the per-128-bit-lane index order `(2,3,0,1)`.

The four emitted vectors are:

`q + y0`  
`q - y0`  
`q + y1`  
`q - y1`

For RAW_INT16 the same arithmetic is applied after narrowing q to int16, followed by four 256-bit stores.

**Specification correction made in this revision:** the old comment saying “3/5-lane rotations” did not match the active hot-path instructions. The source comment has been rewritten to reflect the actual shuffle semantics. The legacy `UFR_FANOUT16_SHUF0/1` byte-index tables remain defined for source compatibility but are not referenced by the active store routine in the current source snapshot.

## 11. PaperView / cultivation

A standard PaperView is:

`1024 rows × 16 lanes = 16,384 logical samples = 64 KiB`.

The current cultivation context uses:

`shift = BR10(epoch)`  
`phase = (epoch >> 10) & 63`  
`vid = (view_id + phase + (phase >> 3)) & 31`  
`base = (A64_OFFSET[vid] + shift) & 1023`.

`BR10` is the exact 10-bit bit-reversal implemented by the source bit-swapping sequence.

For each selected row the A64 lane permutation is applied; a tail row uses the corresponding tail permutation instead. This layer operates on already-created Normal values and is therefore a deterministic re-indexing/materialization layer rather than a new random transform.

## 12. Final cache row mapping

For the current generic final cache:

`src_row(r) = 465 * r mod 1024`.

Because `gcd(465,1024)=1`, this is a bijection over the 1024 rows. The downstream MC consumer reads cache rows sequentially; the non-sequential row selection is paid during cache construction.

R2 has a separate dedicated pair-geometry path whose source-row stride is 555. It should not be conflated with the generic Normal final-cache stride 465.

## 13. R2 consumer

For pair `(x,y)` in the normalized domain:

`R2 = x^2 + y^2`  
`hit = 1[R2 <= 1]`.

The representative payoff used in the current R2 path is:

`f = hit ? (1 - R2) : 0`.

The dedicated R2 cache keeps two pair-parity banks corresponding to even-start and odd-start row pairings so that the runtime path selects a pair bank and offset rather than reconstructing pair geometry inside the hot loop.

## 14. Back/Front boundary and one-core rule

The Front hot path receives completed 16×64-byte state material. Seed conditioning, LPS, Seed Bank derivation, PROFILE, VHV construction, fingerprints, and record construction are Back-side work.

The current architecture is designed so that the complete Front+Back engine can execute on one CPU core. The benchmark policy treats moving Back to other cores as an experiment rather than the primary production architecture.

## 15. Exact constant manifest

```text
Front resident states              = 16
Front parent states                = 3
Front auxiliary states             = 13
Front post/fanout factor           = 4
Front q-blocks per micro-step      = 4
PaperView rows                     = 1024
PaperView lanes/row                = 16
PaperView samples                  = 16384
PaperView bytes                    = 65536
MC block bytes                     = 1024 (256 int32)
Max views                          = 32
Final-cache stride                 = 465
R2 dedicated pair stride           = 555
AAA double-rounds                  = 14
AAA total rounds                   = 28
AAA injection round index         = 9
AAA injection word                = 8
AAA injection constant             = 0x3B54CDA5
AAA profile                       = CHACHA20-AAA-A1
LPS p                             = 5
LPS q                             = 29
LPS lanes                         = 4
LPS start steps                   = 8
LPS walk steps                    = 16
Normal LUT bytes                  = 128
```

## 16. Exact LUT / permutation appendices

### 16.1 Normal 128-byte LUT
```c
static const uint8_t normal_mag128[128] __attribute__((aligned(64))) = {
    0,4,6,4,5,5,2,3,2,5,1,6,11,4,3,3,1,6,6,6,6,14,0,13,15,7,2,7,5,11,9,3,
    10,8,4,11,2,8,2,4,5,9,9,6,10,8,10,8,11,7,11,11,10,11,3,9,2,12,11,4,7,8,5,9,
    9,9,5,8,7,4,11,12,2,9,3,11,10,11,11,7,11,8,10,8,10,6,9,9,5,4,2,8,2,11,4,8,
    10,3,9,11,5,7,2,7,15,13,0,14,6,6,6,6,1,3,3,4,11,6,1,5,2,3,2,5,5,4,6,4,
}
```

### 16.2 PaperView view descriptors
```c
static const uint16_t UFR_PAPER_A64_OFFSET[32] = {0,333,254,611,214,871,300,561,739,892,377,139,89,406,718,996,353,202,491,528,322,276,730,847,41,385,936,839,238,427,960,762}

static const uint8_t UFR_PAPER_A64_LANE[32] = {0,58,49,52,53,9,47,54,56,27,61,43,22,15,61,56,26,16,50,34,63,53,5,58,37,20,7,36,40,21,14,13}

static const uint8_t UFR_PAPER_A64_TAIL[32] = {0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3}
```

### 16.3 A64 64×16 lane-index family
```c
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
}
```

### 16.4 Tail permutation family
```c
static const uint32_t UFR_PAPER_TAIL_PERM[4][16] = {
    {8,0,9,1,10,2,11,3,12,4,13,5,14,6,15,7},
    {0,8,1,9,2,10,3,11,4,12,5,13,6,14,7,15},
    {15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0},
    {4,5,6,7,0,1,2,3,12,13,14,15,8,9,10,11}
}
```

## 17. Specification audit notes

1. The active fanout description has been reconciled with the executable hot-path instructions; no numeric algorithm change was made.
2. The generic final-cache comment has been reconciled with the active `UFR_PAPER_CACHE_STRIDE=465` definition; R2 retains a separate stride-555 pairing geometry.
3. The source snapshot was checked after the comment edits to ensure the non-comment program text is identical to the pre-edit snapshot (ignoring whitespace introduced by comment removal).
4. This document does not claim cryptographic security, formal entropy extraction guarantees, or external statistical certification. Those remain separate validation questions.

## 18. Submission synchronization record

- Frozen submission source: `UltraFastRng512_MC_NORMALTAIL_OPTIMIZED_V1.c`
- Submission SHA-256: `40a6b4311b4cb0e4348f08990b5380ff7607444a92534e17e7014b997af393ba`
- Pre-submission benchmark snapshot SHA-256: `cd8dae646a9865f9867095df5da4903a6e03ce3f806a9dbbce135c8ec2a4ee8b`
- Release cleanup scope: comments/version labels, build metadata, inert fallback macro alignment, and diagnostic text only.
- Current V1 Normal scale: `0.0009317057574691516f * 1.0007781f`.
- Current V1 Normal LUT is the five symmetric tail-shape edits documented in the V1 Normal-tail benchmark report.
- Generic final-cache stride remains 465; dedicated R2 pair stride remains 555.

## 18. What remains outside v0.1

The following need separate formalization before a publication-grade specification is declared complete: exact semantics of every domain-specific MC option (Finance/Pharma/etc.), full numerical error model of the Normal LUT approximation, complete deterministic-stream / replay state semantics under all asynchronous seed-exchange cases, and a publication-defined reference implementation plus test vectors.
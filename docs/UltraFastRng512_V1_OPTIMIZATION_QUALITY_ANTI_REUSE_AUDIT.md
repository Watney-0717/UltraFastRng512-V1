# UltraFastRng512 V1 — Optimization Quality / Anti-Reuse Deep Audit v1.1

Date: 2026-09-29
Scope: frozen V1 and frozen V2 candidate, recovered POC package

## 1. Purpose

This audit answers two separate questions:

1. Do optimization options preserve the Monte Carlo-facing estimator and Normal/R² quality?
2. Does the reported logical sample throughput correspond to newly generated sample information, or does the implementation reuse an existing sample pool by copy/re-indexing?

The second question is intentionally stricter than the existing `bench_quality_options.c` guard. Exact byte-level uniqueness can pass even when the same underlying Normal values are replayed in a different order.

## 2. Recovered test assets

The following assets were recovered from the latest POC package adjacent to Deconstruction Book v2.5:

- `bench/bench_quality_options.c`
- `data/bench_quality_options_v1_20260929.out`
- `data/bench_quality_options_v2_20260929.out`
- `src/UltraFastRng512_MC_NORMALTAIL_OPTIMIZED_V1.c`
- `src/UltraFastRng512_MC_NORMALTAIL_OPTIMIZED_V2.c`
- `bench/bench_speed_showcase.c`
- `bench/bench_speed_fresh_sample.c` (added during recovery)
- `bench/bench_quality_anti_reuse_deep.c` (portable deep-audit source)

Deep-audit source added for this review:

- `bench_quality_anti_reuse_deep.c`

Frozen source identities:

```text
V1 SHA-256 40a6b4311b4cb0e4348f08990b5380ff7607444a92534e17e7014b997af393ba
V2 SHA-256 330b223cd1aa7adc9590bb993a41612da6d95b77b750036a5e4b7d657f969548
```

## 3. Existing optimization-quality audit — rerun result

Both V1 and V2 `bench_quality_options` binaries were rerun in the recovered environment and returned `rc=0`.

### Core cultivation / final-profile screen

V1:

```text
DIRECT/R2_FLOAT32 N=262144 var=(1.005379,1.005379) corr=+9.820e-04 dep2=-1.535e-03 E[R2]=2.010783 VarR2=4.035997 hit=0.394981
DIRECT/FLOAT32    N=262144 var=(0.997287,0.997287) corr=+4.255e-03 dep2=+3.202e-03 E[R2]=1.994574 VarR2=3.972522 hit=0.392143
DIRECT/INT32      N=262144 var=(1.004413,1.004413) corr=+4.060e-03 dep2=+8.260e-04 E[R2]=2.008829 VarR2=4.082587 hit=0.393253
DIRECT/RAW_INT16  N=262144 var=(1.007821,1.007821) corr=+2.620e-03 dep2=+2.704e-04 E[R2]=2.015642 VarR2=4.023270 hit=0.390392
L1_CACHE/FLOAT32  N=262144 var=(1.002832,1.002832) corr=+8.273e-04 dep2=+2.882e-03 E[R2]=2.005686 VarR2=4.030508 hit=0.391727
L2_CACHE/FLOAT32  N=262144 var=(1.011196,1.011196) corr=+6.192e-04 dep2=+6.930e-03 E[R2]=2.022515 VarR2=4.133512 hit=0.391823
```

V2:

```text
DIRECT/R2_FLOAT32 N=262144 var=(1.005502,1.005502) corr=+9.604e-04 dep2=-1.523e-03 E[R2]=2.011027 VarR2=4.035535 hit=0.395275
DIRECT/FLOAT32    N=262144 var=(0.997443,0.997443) corr=+4.232e-03 dep2=+3.230e-03 E[R2]=1.994887 VarR2=3.971129 hit=0.391983
DIRECT/INT32      N=262144 var=(1.004515,1.004515) corr=+4.023e-03 dep2=+7.356e-04 E[R2]=2.009033 VarR2=4.077967 hit=0.393154
DIRECT/RAW_INT16  N=262144 var=(1.007851,1.007851) corr=+2.610e-03 dep2=+5.927e-05 E[R2]=2.015703 VarR2=4.021305 hit=0.390110
L1_CACHE/FLOAT32  N=262144 var=(1.002456,1.002456) corr=+9.532e-04 dep2=+2.431e-03 E[R2]=2.004934 VarR2=4.024806 hit=0.391949
L2_CACHE/FLOAT32  N=262144 var=(1.010987,1.010987) corr=+6.114e-04 dep2=+6.817e-03 E[R2]=2.022095 VarR2=4.128087 hit=0.391766
```

### Finance / Pharma acceleration screen

Across all tested options and both domains:

```text
sample_ratio = 1.000000000
samples      = 32768
```

Maximum reported relative error versus the deliberate BASELINE in this rerun:

```text
V1 Finance  = 2.299e-07
V1 Pharma   = 1.851e-07
V2 Finance  = 4.381e-07
V2 Pharma   = 5.594e-07
```

Interpretation: the tested optimization profiles preserve the benchmarked sample count and estimator output to approximately sub-ppm relative differences. Shared/precomputed parameter transforms are intentional and are not themselves evidence of sample duplication.

## 4. Why the existing anti-reuse guard is not sufficient

The existing guard reports:

```text
unique full q256 blocks: 256 / 256 (100.000%)
adjacent identical full blocks: 0
```

This is a useful accidental-copy detector, but it only asks whether complete 256-sample qblocks are byte-for-byte equal inside the assembled corpus.

PaperView intentionally performs deterministic re-indexing/permutation of an already-created Normal starter. Therefore two views can be byte-different while containing exactly the same underlying scalar values.

The same issue appears across FIXED-generation chunks: chunk bytes differ because the cultivation epoch changes the indexing, while the retained starter remains unchanged.

## 5. Deep anti-reuse audit — reliable result

### 5.1 Four PaperView views in one chunk

Each view contains 64 q256 blocks = 16,384 scalar Normal values.

V1 and V2 both produced:

```text
VIEW0_vs_VIEW1 scalar_multiset_equal=1 exact_q256_replay=0/64
VIEW0_vs_VIEW2 scalar_multiset_equal=1 exact_q256_replay=0/64
VIEW0_vs_VIEW3 scalar_multiset_equal=1 exact_q256_replay=0/64
```

So the 4 views are **not** four disjoint Normal value pools. They are different deterministic layouts/permutations of the same 16,384 starter values.

This is consistent with the frozen source implementation: the starter is generated once and PaperView materialization only re-indexes/permutates its rows/lane positions.

### 5.2 Two consecutive chunks — CHUNK generation

For `generation_chunks=1`:

```text
raw_chunk_equal              = 0
exact_q256_replay             = 0/256
scalar_multiset_equal         = 0
```

This is the conservative anti-reuse behavior: a new Normal starter is made at the chunk boundary.

### 5.3 Two consecutive chunks — FIXED generation

For V1 and V2, the following all produced the same structural result:

```text
GEN=2:
  raw_chunk_equal      = 0
  exact_q256_replay    = 256/256
  scalar_multiset_equal= 1

GEN=64:
  raw_chunk_equal      = 0
  exact_q256_replay    = 256/256
  scalar_multiset_equal= 1

GEN=1048576:
  raw_chunk_equal      = 0
  exact_q256_replay    = 256/256
  scalar_multiset_equal= 1
```

Therefore a FIXED generation does **not** generate a fresh 65,536-value Normal pool for every chunk. It retains one 16,384-scalar starter and exposes that same value pool through four views and repeated chunk-level re-indexing.

The emitted chunk bytes are nevertheless different. This means the correct description is **deterministic cultivation/re-indexing with underlying sample-pool reuse**, not byte-for-byte chunk cloning. The reuse occurs at the Normal-domain value-pool level; it is not evidence that the MC result bytes themselves are copied verbatim.

## 6. Consequence for the headline throughput number

The speed showcase explicitly sets:

```text
generation = FIXED
fixed generation chunks = 1,048,576
```

and the recorded peak benchmark uses a timed region of 131,072 chunks. Because 131,072 is less than 1,048,576, the timed region remains within a single generation boundary and therefore within one retained Normal starter.

Consequently the headline number such as:

```text
46.602 Gsamples/s
```

must be described as:

> logical Normal-sample throughput under the FIXED-generation cultivation policy

It must **not** be described as:

> 46.602 billion newly generated independent Normal samples per second

The latter interpretation is not supported by the deep anti-reuse audit.

## 7. What is and is not a problem

### Preserved

- marginal Normal values are not numerically changed by PaperView permutation;
- existing core profile quality metrics remain in the expected neighborhood;
- tested Finance/Pharma acceleration options keep sample counts identical and preserve estimator outputs to sub-ppm scale;
- no accidental byte-identical q256 block duplication was observed inside the official one-corpus guard;
- CHUNK generation regenerates the Normal starter rather than replaying the prior starter.

### Qualification required

- the 4 PaperView views reuse the same underlying scalar starter pool;
- FIXED generation reuses the same starter across multiple chunks, with chunk bytes differing only by deterministic re-indexing/permutation;
- therefore logical sample count is not a synonym for statistically independent sample count;
- exact independence / effective sample size is not established by this audit.


## 8. Fresh-starter companion throughput measurement

To separate the amortized FIXED-generation showcase from the conservative fresh-starter path, a companion benchmark was added using the same `DIRECT / R2_FLOAT32 / representative specialized path` but `UFR_GENERATION_CHUNK`. The run processes 4096 chunks = 268,435,456 logical samples.

Recovered-environment 3-run medians:

```text
             CHUNK fresh starter        FIXED 1,048,576 chunks
V1           12.678 Gsamples/s         40.613 Gsamples/s
V2           12.503 Gsamples/s         44.101 Gsamples/s
```

This is not a pure measurement of the statistical value of reuse. CHUNK also disables long-generation cultivation/final-cache amortization and regenerates the Normal starter every chunk. The result is therefore best labeled **fresh-starter throughput** versus **amortized logical throughput**, not “independent-sample throughput” unless an additional independence analysis establishes that stronger property.

## 9. Documentation decision

For V1 documentation, the following terminology is now recommended:

```text
logical sample           = emitted Normal scalar according to the layer accounting
pair sample              = one (x,y) pair consumed by the representative R2 MC
independent sample       = a stronger statistical concept; not inferred from the logical count
logical throughput      = emitted logical samples per second
independent throughput   = NOT claimed unless a separate qualification establishes it
```

The existing FIXED-generation speed showcase may remain as a performance-oriented benchmark, but the generation policy must be printed next to the throughput value and the value must not be used as a proxy for independent-sample production rate.

## 10. Reproduction status

Deep audit executable builds:

```text
V1: PASS
V2: PASS
```

Official option-quality executable reruns:

```text
V1: PASS (rc=0)
V2: PASS (rc=0)
```

The audit is deterministic in the recovered environment; repeated execution reproduced the same structural findings. Both V1 and V2 deep audits were freshly rebuilt from the recovered package and returned success. The companion CHUNK/FIXED benchmark was also freshly rebuilt and executed.

## 11. Final audit statement

The acceleration options themselves do not show evidence of inflating the nominal MC sample counter: within the tested Finance/Pharma option screen, every option consumed the same 32,768 samples and reproduced the baseline estimator to sub-ppm relative error.

However, the deeper anti-reuse audit identifies a real architectural distinction that the original guard did not expose: PaperView views and FIXED-generation chunks reuse the same underlying Normal starter value pool while changing its layout/order.

Therefore the V1 quality story is best stated as:

> **Estimator-preserving acceleration with deterministic cultivation, combined with an explicitly qualified reuse policy.**

The project should retain the FIXED-generation showcase for raw integrated throughput, while treating an independently regenerated CHUNK-generation benchmark as the appropriate companion measurement when the reported number is intended to represent fresh-sample production rather than amortized logical throughput.

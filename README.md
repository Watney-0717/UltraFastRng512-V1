# UltraFastRng512 V1

**Canonical V1 public release tree — 2026-09-30**

UltraFastRng512 V1 is an AVX-512-oriented Monte Carlo execution path that separates Seed/Back preparation from a lightweight Front, then connects the Front output to Normal v9, 4-way fanout, PaperView / Final-Cultivation, and domain-specific Monte Carlo consumers.

This public tree intentionally contains **V1 only**. V2 research candidates, LEGACY material, staging artifacts, and CPU-specific prebuilt binaries are kept outside the canonical V1 release.

## Start here

### 1. Build and verify

```bash
./tools/build_and_verify.sh
```

The default build uses `-O3 -std=c11 -march=native -pthread` and links OpenSSL `libcrypto` and `libm`. Set `CC` or `CFLAGS` for a controlled environment.

The main V1 implementation is a single translation unit and embeds the selected V1 adapter `.c` components. Do not also link those embedded components as separate objects for the main executable.

### 2. Speed showcase

```bash
./build/bench_speed_v1
```

Representative path:

```text
Front16 / Back A+B
    -> Normal v9
    -> 4-way
    -> DIRECT
    -> R2_FLOAT32 specialized path
    -> 8-accumulator R2 consumer
```

Front + Back A/B + MC remain confined to one logical CPU core in this benchmark. The headline figure is a **logical MC-sample throughput** under the stated generation policy; it is not a claim that the same number of newly generated independent Normal values per second has been produced.

### 3. Fresh-starter companion benchmark

```bash
./build/bench_speed_fresh_sample_v1
```

This forces `UFR_GENERATION_CHUNK` so the Normal starter is regenerated at each MC chunk. It is a qualification companion to the fixed-generation showcase, not a replacement for it.

### 4. MC-input quality

```bash
./build/bench_quality_mc_input_v1
```

This measures the actual R2 x/y values immediately before the MC operation after the V1 555-pair geometry and R2 normalization. It reports mean, variance, skew/kurtosis, correlation, dep2, lag-1 behavior, radial moments, hit rate, sign balance, and tails.

### 5. Optimization integrity / anti-reuse

```bash
./build/bench_quality_options_v1
./build/bench_quality_anti_reuse_deep_v1
```

The deep audit distinguishes byte-level inequality from reuse of the underlying scalar Normal starter pool. In particular, the four PaperView views may expose the same scalar starter values under different deterministic layouts, and FIXED generation may retain that starter pool across chunks. Therefore logical sample count and independent sample count must not be treated as synonyms.

### Public statistical-test boundary

PractRand, TestU01 / SmallCrush, dieharder, and NIST SP800-22 are not bundled as native named-suite certification runs in this release tree. Existing MC-quality and surrogate-battery records are evidence for the stated scope only; they do not replace those named suites.

## Repository layout

```text
include/       public headers
src/           canonical V1 implementation and embedded components
bench/         V1 benchmark / audit sources
tests/         release self-verification helpers
tools/         build and verification scripts
docs/          mathematical specification, audits, full deconstruction book
results/       V1 benchmark result records
metadata/      release scope and provenance
license/       component-specific license notices
```

## License map

- **Core engine code:** AGPL-3.0-only OR separate Commercial License
- **Benchmark/source tooling:** 0BSD unless a file states otherwise
- **Benchmark result data:** CC0 1.0 intended dedication, subject to applicable law
- **Deconstruction Book:** CC BY-NC-ND 4.0
- **Mathematical specification:** CC BY 4.0
- **Third-party material:** original license remains applicable

See `LICENSE-AGPL-3.0-only.txt`, `COMMERCIAL_LICENSE.md`, `COMMERCIAL-LICENSE-NOTICE.md`, `SOURCE_LICENSE_MAP.md`, `THIRD_PARTY_LICENSES.md`, and the component-specific notices under `license/`.

## Official Communications & Commercial Inquiries

**Email:** WatneyWatneyWatney0717@proton.me

## Rights Holder

**Watney-0717** is the public project identity and rights-holder name used for this release.

## Commercial Licensing

A separate `COMMERCIAL_LICENSE.md` is provided for proprietary, closed-source, or otherwise non-AGPL use. The Commercial License includes Per-Product and Enterprise licensing options and may be supplemented by individually agreed Integration Support / advisory services.

## Release status

This tree is the **canonical V1 public release tree** assembled from the recovered 2026-09-29 POC and Anti-Reuse Recovery bundles plus the current 2026-09-30 English Deconstruction Book. V2 research candidates, LEGACY material, staging artifacts, and CPU-specific build products are excluded.

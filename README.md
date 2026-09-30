# UltraFastRng512 V1

**An ultra-fast Monte Carlo (MC) execution platform optimized for AVX-512.**

UltraFastRng512 adopts a layered architecture that completely separates **Back** (which handles heavy Seed generation and diffusion processing) from **Front** (which uses completed Seeds to generate samples at high speed).

The platform integrates not only RNG generation, but also Normal transformation, PaperView / Final-Cultivation, Fanout, and Monte Carlo computation into a single pipeline, with the design goal of achieving high-speed one-core-complete MC execution while maintaining statistical quality and numerical accuracy.

---

## Performance

### Single-core full-pipeline performance

* **Up to 46.602 Gsamples/s** *(AMD EPYC 9V74 shared-VM environment)*

> **Note:** This is not RNG-only performance. It is a long-duration measurement of the complete one-core execution system including **Front / Normal / PaperView / MC / Back A/B**.

### Domain-specific single-core effective throughput

*(Shared-VM environment)*

* **R2 Monte Carlo:** 33.065 Gsamples/s *(Peak 46.6G)*  
  — Precomputed R2 cache / 8 parallel accumulators

* **FLOAT32:** 19.129 Gsamples/s  
  — Direct Normal consumption / SIMD (standard-format output)

* **INT32:** 16.359 Gsamples/s  
  — R2-independent standard-format output

* **Finance MC:** 1.2–1.4 Gsamples/s  
  — Shared common terminal computation / 4+4 structure

* **Pharma MC:** 0.7–0.9 Gsamples/s  
  — Shared exposure calculations / invariant precomputation

* **Multi-core scalability:** A 3-core independent-worker experiment demonstrated **3.032× scaling** relative to single-core performance under the same environment.

---

## Statistical Quality & Precision

* **RNG bitstream qualification:**  
  A 512 MiB-scale surrogate statistical test battery, including chi-square, Hamming-weight, matrix-rank, linear-complexity, and related tests, was performed with no obvious anomaly observed.

* **MC-level distribution validation:**  
  After Normal transformation, the standard moments (mean ≈ 0, variance ≈ 1) and pair/radial characteristics  
  (**E[R²] ≈ 2.0, Var(R²) ≈ 4.0, P(R² ≤ 1) ≈ 0.3935**)  
  were found to be in close agreement with their theoretical values.

* **Precision preservation under optimization:**  
  With various domain optimizations enabled, including precomputation and shared computation, the relative difference of estimators from the baseline remained within the **sub-ppm range**, with a maximum of **5.594e-07 or less**, without degrading computational precision.

* **Dynamic Seed Exchange:**  
  The system observes when the next-generation Seed prepared by Back becomes **READY** and safely updates the Seed using A/B double buffering without stopping the high-speed Front execution loop.

---

## Architecture

The basic structure of UltraFastRng512 is:

```text
Physical Entropy
       │
       ▼
Entropy Conditioning
       │
       ▼
LPS / Algebraic Mixing
       │
       ▼
512-bit Seed Bank
       │
       ▼
PROFILE
       │
       ▼
Back
 Seed preparation / exchange
       │
       ▼
Fast Front
 Front16 / AVX-512
       │
       ▼
Normal v9
       │
       ▼
4-way Fanout
       │
       ▼
PaperView / Final-Cultivation
       │
       ▼
Monte Carlo
 ┌─────┼─────┐
 ▼     ▼     ▼
 R2   Finance Pharma
```

The Back side prepares strongly diffused Seed / Seed Bank material for delivery to Front.

Here, **Seed quality, independence, reproducibility, and identifiability are prioritized over speed**.

V1 adopts a simple **Back A/B Seed exchange** architecture and does not bring complex management information into the Front side.

UltraFastRng512 V1 organizes random-number generation, distribution transformation, data transport, MC statistical processing, reproducibility management, and domain-specific computation as independent layers.

Heavy processing related to Seed quality is placed in the **Back / Seed layer**; output format and data transport are placed in the **PaperView / Final-Cultivation layer**; statistical evaluation and convergence control are placed in the **Common Post layer**; Deterministic Stream, CRN, Checkpoint, Exact Replay, and related functions are placed in the **Control Plane**; and Finance / Pharma-specific computation is placed in the **Domain Adapter layer**.

This layered design allows required functionality to be added or exchanged without introducing unnecessary processing into the hot loop of the high-speed AVX-512 Front.

---

## Seed Bank and Enterprise Extensions

Seed preparation follows the basic flow:

Physical Entropy
      ↓
Conditioning
      ↓
Algebraic / LPS Mixing
      ↓
512-bit Seed Bank

This is the seed-preparation path, not a per-handoff fresh-entropy reseeding loop. The continuous Monte Carlo execution path does not reacquire physical entropy or generate a fresh physical-entropy seed for each state handoff. Instead, previously conditioned seed material is deterministically transformed into the completed Front state sets used by the high-speed execution path.

In addition, PROFILE is provided as a replaceable layer, allowing seed-quality research, comparison, and reproducibility testing to be performed independently.

The platform also provides enterprise-oriented operational functions such as reproducibility control (CRN, Checkpoint, Exact Replay) and domain adapters for financial and pharmaceutical workloads.

Importantly, these advanced functions are kept outside the core high-speed Front hot loop through a Layered Architecture, allowing required functionality to be combined and selected without placing their heavier processing into the continuous Front execution path.

---

## 133-Page Technical Monograph

A **133-page Technical Monograph** covering the design, implementation, measurements, quality evaluation, MC optimization, reproducibility, detailed mathematical derivations, and known limitations of UltraFastRng512 V1 is provided.

Please refer to the accompanying PDF documentation for details.

---

## Commercial Licensing

This platform is provided under a **dual-license model: AGPL-3.0-only and Commercial License**.

For closed-source use in commercial product integration, SaaS deployment, and similar applications, a **Commercial License** is available.

> **Note:** Please refer to the respective license documents in the repository for the scope of the licenses and details regarding commercial use.

### Commercial Inquiries

**WatneyWatneyWatney0717@proton.me**

---

## Call for Official Name

**“UltraFastRng512” is currently a development codename.**

As preparations proceed toward an official public release, we are inviting the community to propose a formal name suitable for this platform.

Suggestions for an official project name are welcome.

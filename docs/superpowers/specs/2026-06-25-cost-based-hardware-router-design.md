# Design: Cost-Based Hardware Router (MatrixDB keystone)

**Status:** approved design, pre-implementation
**Date:** 2026-06-25
**Thesis:** *The query optimizer doesn't pick an index — it picks a processor.*

A query engine where a measured cost model places each dataset on the processor that
runs it fastest (CPU for point ops, GPU for large scans), and queries execute where
their data lives. This is the first of three facets of a heterogeneous-hardware DB
(router → VRAM tiering → ownership concurrency); the router is the keystone that makes
the thesis demonstrable.

---

## 1. Goals & non-goals

**Goals**
- Both compute engines (CPU + GPU) live in **one process**, selected per dataset at runtime.
- A **CostModel**, parameterized by *measured* hardware constants, decides placement.
- **No data duplication:** each dataset has exactly one home (so no coherence protocol).
- The design is **open to unified-memory hardware** (DGX Spark, Grace-Hopper) via a
  `MemorySpace`/`MemoryModel` seam — but only the **discrete** path is implemented now.
- Demonstrable win: a mixed workload routed beats both CPU-only and GPU-only.

**Non-goals (deferred, marked `// ponytail:` at the seam)**
- Unified-memory execution path (stub only — no hardware to verify).
- Data mirroring + write-coherence (unnecessary while datasets are disjoint).
- VRAM tiering by query pressure (Seed 2 — the next facet).
- Adaptive/learned routing (route on measured feedback) — static cost model first.

---

## 2. Architecture

```
                    ┌─────────────────────────────────────────────┐
   ingestion ──────▶│  Router  (placement map + CostModel + engines)│
                    └───────────────┬─────────────────┬────────────┘
                       data in HOST │                 │ data in DEVICE
                                    ▼                 ▼
                          CPUMockEngine        CUDAGPUEngine
                          (ComputeInterface)   (ComputeInterface)
```

**Components (each single-purpose, independently testable):**

- **`MemoryModel`** — boot-time descriptor: `DISCRETE` (distinct HOST/DEVICE, the T4) or
  `UNIFIED` (one space; stub). Detected via CUDA attributes; defaults to `DISCRETE`.
- **`CostModel`** — pure functions `host_cost(op, bytes)` / `device_cost(op, bytes)` →
  predicted microseconds, from measured constants. Reads the `MemoryModel` so the
  transfer term is included (discrete) or zero (unified). No state.
- **`Router`** — holds `ComputeInterface* cpu_`, `ComputeInterface* gpu_` (gpu may be
  null), the `CostModel`, and a **placement map** (dataset id → `MemorySpace`). Policy
  only — owns no data, runs no kernels. API: `place(dataset_id, op, bytes)` (once, at
  load) and `route_scan(query)` / `route_batch(queries,n)` (per query, follows the map).
- **Engines** — unchanged; already satisfy `ComputeInterface`.

---

## 3. The cost model (the disruptive core)

Predicted microseconds; lower wins. Constants are **measured on the target machine**
(values below are first estimates from our T4 runs — flagged for one calibration pass).

```
device_cost(op, bytes):                    # GPU
    point op → INF                         # measured: PCIe < CPU cache, GPU always loses
    scan     → LAUNCH_US + transfer(bytes) + bytes / GPU_SCAN_BPus
                 LAUNCH_US      ≈ 30        # measured per-scan kernel-launch floor
                 GPU_SCAN_BPus  ≈ 240_000   # 240 GB/s, measured at 64 MB
                 transfer(bytes): one-time cost to first place data in DEVICE. Zero in
                   steady state — a column is placed once and scanned many times, so the
                   amortized per-query transfer is 0. Modeled explicitly so the UNIFIED
                   case (where it is structurally 0) is a parameter change, not a rewrite.

host_cost(op, bytes):                       # CPU
    point op → bytes / CPU_POINT_BPus       # tiny; CPU always wins points
    scan     → bytes / CPU_SCAN_BPus
                 CPU_SCAN_BPus  ≈ 10_000    # 10 GB/s, measured

MemoryModel = UNIFIED  →  transfer term = 0 AND LAUNCH amortized differently;
                          the GPU crossover drops sharply (no copy to justify).
                          [stub: documented, not implemented]
```

**Derived crossover (discrete):** solving `LAUNCH_US + b/240000 = b/10000` →
**b ≈ 313 KB**. *Calibration note:* our end-to-end sweep suggested a higher practical
crossover (~4–8 MB) because it included overheads this pure model omits. **The model
structure is the deliverable; the constants get one calibration pass against measured
numbers before we trust the boundary.** This is expected and marked, not a defect.

**Why it's disruptive:** the cost unit is a *processor*, and the boundary is *derived
from measured hardware*, not hand-tuned. Swap the constants (A100, a unified box) and
the crossover moves automatically — portable model, per-machine calibration.

---

## 4. Data placement & flow (no duplication)

**One home per dataset** — the entire coherence story is "there is only ever one copy":

```
Load time:   dataset arrives ─▶ CostModel decides cheaper home ─▶ allocate there,
                                 record dataset_id → MemorySpace in the placement map
Query time:  query arrives    ─▶ Router looks up the dataset's home ─▶ dispatch:
               point op                         → CPU engine (KV store, always HOST)
               scan, column resident in DEVICE  → GPU engine
               scan, column resident in HOST    → CPU engine
```

Point data (KV store) and scan data (columns) are **disjoint datasets** — there is no
shared dataset, hence no write-coherence problem in this keystone. The placement map is
authoritative: a query can never read a stale copy because no second copy exists.

---

## 5. Build change

Today `MATRIX_USE_CUDA` makes the engines mutually exclusive (compile-time either/or).
Change: the CUDA build compiles **both** `compute_mock.cpp` and `compute_cuda.cuh`, and
`main` instantiates both behind the `Router`.

- **CUDA build:** Router has live CPU + GPU engines; `place()` uses the full cost model.
- **No-GPU build:** Router's `gpu_` is null; `place()` always returns HOST; everything
  runs on CPU. Clean degradation, same code path. (Keeps the CPU-only build — our
  primary local verification — green.)

---

## 6. Verification

- **CostModel unit check** (`test_cost_model.cpp`, CPU-only, no GPU): assert placement
  decisions at sizes straddling the crossover; assert point ops never pick DEVICE; assert
  UNIFIED stub returns the documented placeholder. Pure functions → fully CPU-testable.
- **Routing correctness:** a mixed workload (point ops + scans of varying column sizes)
  through the Router; assert (a) each dataset placed on the predicted home, (b) results
  match the oracle regardless of home, (c) no dataset duplicated (one allocation each).
- **Thesis demo:** same mixed workload run three ways — CPU-only, GPU-only, routed —
  asserting routed ≤ both on total time. This is the headline result.
- **Discipline carried forward:** `test_scan_coverage`-style CPU simulation for any new
  kernel index math; host-syntax probe for `.cuh` changes; notebook regenerated from
  source; cross-backend oracle.

---

## 7. Open calibration items (not blockers)

- One measured pass to set `LAUNCH_US`, `GPU_SCAN_BPus`, `CPU_SCAN_BPus`,
  `CPU_POINT_BPus`, and reconcile the derived ~313 KB crossover with the practical one.
- `MemoryModel` detection: confirm the CUDA attribute(s) for unified addressing; until
  validated on real unified hardware, detection defaults to `DISCRETE`.

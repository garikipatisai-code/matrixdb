# MatrixDB — Findings & Engineering Journal

A running record of what we built, what we *measured*, what we got wrong, and the
ideas worth chasing later. The point of this file: when MatrixDB is mature, we mine
these findings for a genuinely new (possibly disruptive) thing to build.

All numbers are from a **Tesla T4** (Google Colab, 40 SMs, ~320 GB/s peak VRAM
bandwidth) and an **Apple M-series** / **Colab Xeon** CPU. Treat them as order-of-
magnitude, not benchmark-grade — they move ±15% run to run on a shared VM.

---

## 1. The one-sentence thesis (measured, not assumed)

> **A GPU cannot beat a CPU at small key-value point operations, but it wins by ~1–2
> orders of magnitude at scans/aggregations over data that lives resident in VRAM.**

MatrixDB's whole design follows from that: route point ops to the CPU, route scans to
the GPU, keep the analytical data resident on the device so it's never shipped per query.

---

## 2. What we built

A low-latency batched query engine, two interchangeable compute backends behind one
`ComputeInterface`, selected at compile time:

- **Ingestion:** lock-free SPSC ring buffer (wait-free, cache-line isolated cursors) →
  dual-trigger batcher (cut a batch when it hits N queries **or** an age in µs).
- **Point-op execution (page ownership):** the key space is split into pages; exactly
  one owner (one CUDA block / one CPU loop) touches a page's slots. Same key → same
  owner → writes serialize by ownership. **No store atomics, no OCC, no delta log** —
  the conflict those mechanisms exist to resolve is removed by construction.
- **Scan execution:** a resident `uint32` column (16M values, 64 MB) lives in VRAM,
  filled once; `OP_SCAN` queries (threshold in the payload) run a vectorized `uint4`
  filter-count kernel over it in place.
- **Two backends:** `CPUMockEngine` (runs anywhere, the no-GPU fallback and the
  correctness oracle) and `CUDAGPUEngine` (the real thing). Both assert byte-identical
  results — a cross-backend store checksum and a scan-result oracle.

---

## 3. Measured findings (the spine of the journey)

### 3.1 Ingestion is genuinely sub-microsecond
Raw SPSC handoff (ping-pong, one item in flight): **~64 ns p50, ~85 ns p99** on the T4
host. The spec's "sub-microsecond ingestion" claim holds — but only for the *handoff*.
Under burst (producer outruns a single consumer) **queue residency** is milliseconds:
that's backlog, not handoff cost. Two different numbers; report both honestly.

### 3.2 Point ops: the GPU loses, by physics
GPU point-op throughput plateaus ~20M ops/sec; CPU does ~85–100M. Root cause is not
code — it's three structural facts:
- the KV store is tiny (fits CPU L1/L2), so the GPU's bandwidth edge is neutralized;
- ~1 memory op per query, no compute to amortize a kernel launch;
- queries cross **PCIe (~12 GB/s)**, which is *slower than the CPU's own cache*.

**A GPU cannot be a faster KV cache.** This is a ceiling, not a tuning target.

### 3.3 Scans: the GPU wins big, on resident data
Filter-count over a resident column, swept 64 KB → 64 MB:
- CPU scan **degrades** as data grows (16 → 9 GB/s) — it spills cache to DRAM.
- GPU scan **climbs** as data grows (6 → ~250 GB/s) — it saturates VRAM bandwidth.
- **Crossover ≈ 4–8 MB** (where the CPU runs out of cache).
- At 64 MB: GPU **~250 GB/s (≈78% of peak)** vs CPU ~10 GB/s → **~24× standalone.**

Same silicon that *lost* 5× on point ops *wins* 24× on scans. Workload shape is
everything.

### 3.4 The scan kernel: bandwidth-bound, narrowest-type-vectorized wins
Tested four kernel variants on the T4:
- `u64` scalar — highest GB/s (it moves 8 B/load) but half the rows/byte.
- `u32` scalar — underfills the bus (memory-level parallelism too shallow).
- **`u32x4` (uint4 vectorized, 16 B/load) — the winner.** Recovered u32's lost
  bandwidth back to ~250 GB/s **and** processes 2× the rows/sec of u64 (narrower type).
- `ipt8` (register blocking, CUB's `ITEMS_PER_THREAD` lever) — close second.

Research (NVIDIA vectorized-access blog, CUDA best-practices, CUB source) pointed at
register blocking as *the* lever; the hardware said vectorization edged it out. **Theory
informs, measurement decides.** Design law confirmed: *store each column in its
narrowest type and load it vectorized.*

### 3.5 End-to-end scan through the real engine
`OP_SCAN` flowing ring → batcher → kernel → result, verified on hardware (result ==
oracle, both backends): GPU **~0.4–0.7 ms/scan** vs CPU **~8 ms/scan → ~12–19×**.

### 3.6 The "70% overhead" that wasn't (a debugging win)
We believed the integrated scan wasted ~70% on per-scan launch/copy/sync. cudaEvent
instrumentation (kernel time vs host-wall time) measured the real overhead at **4%** —
the integrated scan is ~96% efficient. The apparent gap was a **measurement artifact**:
host-wall vs cudaEvent timing, and comparing a 16M-column integrated scan against a 64M
standalone number. **There was no gap to close.** Instrumenting before fixing saved a
wasted optimization pass.

---

## 4. Hypotheses we overturned (kept on purpose — the misses are the lessons)

| Believed | Measured | Lesson |
|---|---|---|
| GPU will be faster everywhere | GPU loses 5× on point ops | Match workload to hardware, don't assume |
| "sub-microsecond" = the pipeline | only the *handoff* is; residency is ms | Name what you measure |
| Register blocking (CUB) is the top scan lever | vectorized `uint4` edged it | Measure on the actual chip |
| ~70% per-scan overhead | 4% | Instrument before optimizing |
| Spec's thread-per-query + delta log | replaced by page ownership | Ownership > conflict resolution |

---

## 5. Methodology that worked (worth reusing)

- **Measure, then cut.** Every design turn was decided by a benchmark, not a guess.
- **CPU-simulate GPU logic.** `test_scan_coverage.cpp` reproduces the scan kernel's
  *integer index math* on the CPU and asserts exact coverage. It caught a GPU-only
  striding bug **before** spending a Colab run — pure logic is hardware-independent.
- **Host-syntax probe.** A stubbed `cuda_runtime.h` lets `clang` syntax-check the `.cuh`
  on a machine with no nvcc, catching real errors (missing includes, a swallowed class
  decl) before the remote build.
- **Self-contained notebook, generated from source.** `make_notebook.py` embeds the
  real files via `%%writefile`, so the Colab notebook can never drift from the code.
- **Cross-backend oracle.** CPU and GPU must produce identical checksums/results; "it
  works" means the same thing on both.
- **Remote runs are precious** → batch many questions into one run (sweep all sizes,
  all kernel variants, all metrics at once).

---

## 6. Known limits / deferred (none are blockers)

- **Cost-based router — built.** Both engines live in one process; a measured cost model
  places each dataset (point store -> HOST, scan column -> HOST/DEVICE by size) and
  queries run where their data lives. No duplication, no coherence protocol. Unified-memory
  seam present, discrete-only implemented. Open item: one calibration pass on the cost
  constants (the derived ~313 KB crossover vs the ~4-8 MB practical one).
- **HTAP head-of-line blocking — FIXED.** Point ops and scans now run on separate
  queues + threads (GPU: separate CUDA streams). A multi-ms scan no longer stalls point
  ops. Measured on CPU: point-op queue-residency p99 ~1.8 ms with ten 8 ms scans running
  concurrently, vs ~40 ms when they shared one queue (~22×). On GPU the residency is no
  longer scan-bound; what remains (~2.6 ms p99) is the point-op path's own per-batch
  `cudaStreamSynchronize` cost (hypothesis — not yet instrumented), addressable with
  async/double-buffered batches.
- Scans return a **count, not matching rows**; no SUM/MIN/MAX/AVG yet.
- **Full OCC** (TEV lock-bit + read-set validation) — unnecessary while page ownership
  holds; needed only if a page gains multiple concurrent writers.
- Page binning runs on the **host** — folding it into the dual-trigger batcher is free
  (the batcher already touches every query).
- **Hyper-Q multi-stream**, `cudaHostRegister` pinned-DMA — throughput upgrades, not
  needed to prove anything.
- No persistence, no network ingestion — it's an engine, not yet a system.

---

## 7. Disruptive-tech seeds (for the future brainstorm)

Raw ideas the findings suggest. Not committed — just captured while fresh.

1. **HTAP on one box via workload routing.** We empirically showed point-ops (CPU) and
   scans (GPU) want *different hardware*. A DB that routes each query to its winning
   processor — same data, two engines — is the natural product. The hard part (and the
   opportunity) is keeping the CPU KV store and the GPU column in sync cheaply.
2. **"Resident-first" analytics.** The crossover is ~4–8 MB. Anything bigger than CPU
   cache and queried repeatedly *wants* to live in VRAM. A cache/tier manager that
   decides what's resident on the GPU by query pressure (hot columns pinned in VRAM)
   could beat both a pure-CPU DB and a ship-it-every-time GPU DB.
3. **Ownership-as-concurrency, generalized.** Page ownership deleted OCC entirely for
   point ops. How far does "route by owner, never lock" scale — across GPUs? across a
   cluster? A shared-nothing GPU-native store where the *routing* is the concurrency
   control.
4. **The sub-µs ring as a building block.** 64 ns handoff is fast enough to be a
   substrate for things beyond a DB: a low-latency event bus, a feature store for
   inference, a market-data tap. The ingestion layer may be more reusable than the DB.
5. **GPU column engine as a library.** The u32x4 scan + resident column + oracle harness
   is a clean, fast, verifiable columnar-scan primitive. Stripped of the DB framing it's
   a drop-in accelerator for any filter/aggregate workload.

---

*Living document — append as the engine matures. Numbers are snapshots; the lessons are
the durable part.*

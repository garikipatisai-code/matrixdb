# MatrixDB Prototype

A GPU-accelerated database engine: a lock-free ingestion pipeline that routes each
workload to the hardware where it wins, driven by measurement on a real Tesla T4.

One ingestion pipeline (lock-free SPSC ring → dual-trigger batching) feeds two
interchangeable compute backends, selected at compile time:

| Backend | When | Build |
|---|---|---|
| `CPUMockEngine` | default, no GPU | `clang++`/`g++` |
| `CUDAGPUEngine` | real GPU (Colab, etc.) | `nvcc -DMATRIX_USE_CUDA` |

Both honor the same `ComputeInterface` and the **same correctness asserts** in `main.cpp`,
so "passes" means the same thing on CPU and GPU: every query dispatched on its opcode,
every mutation committed, every scan result matching a known oracle, zero drops. A store
checksum is asserted equal across backends — the page-ownership model produces
byte-identical state on CPU and GPU.

## The one-line thesis (measured, not claimed)

**A GPU cannot beat a CPU at key-value point ops, but wins big at scans over resident data.**
The bus to the GPU (PCIe ~12 GB/s) is slower than the CPU's own cache, so tiny point ops
lose. But data living *resident* in VRAM, scanned in place, runs at the GPU's memory
bandwidth — far past what the CPU can stream. MatrixDB routes each workload to where it wins.

| Workload | CPU | GPU (Tesla T4) | Winner |
|---|---|---|---|
| Point ops (KV get/put) | ~19M ops/sec | ~20M ops/sec (plateaus) | **CPU** (GPU PCIe-bound) |
| Scan, standalone kernel | ~10 GB/s | **~240 GB/s** (75% of peak) | **GPU ~24×** |
| Scan, end-to-end through engine | ~8 ms/scan | **~0.4–0.7 ms/scan** | **GPU ~12–19×** |

(64 MB resident `uint32` column, filter-count. Standalone vs end-to-end differ because each
integrated scan still pays per-scan launch/copy/sync overhead — see *Known limits*.)

## Files
- `types.hpp` — `DatabaseQuery` (cache-aligned POD), opcodes, store + page-ownership + scan-column layout
- `ring_buffer.hpp` — lock-free SPSC ring (Component 1: the ingestion dam)
- `compute.hpp` — `ComputeInterface` contract + page-binning helper + scan-threshold codec
- `memory_model.hpp` / `cost_model.hpp` / `router.hpp` — cost-based hardware router: places each dataset on CPU or GPU by measured cost, dispatches queries to the engine that owns the data (open to unified memory via a seam, discrete-only for now)
- `compute_mock.cpp` — CPU engine (Component 5: local sandbox)
- `compute_cuda.cuh` — CUDA engine: block-per-page point ops + u32x4 resident-column scan (Component 4)
- `main.cpp` — orchestration, latency/throughput/scan benchmarks, oracle asserts
- `test_scan_coverage.cpp` — CPU simulation of the scan kernel's index math (catches GPU-only bugs)
- `make_notebook.py` — regenerates `matrixdb_colab.ipynb` from the real source (run after edits)
- `FINDINGS.md` — engineering journal: every measured result, overturned hypothesis, and idea for later
- `CMakeLists.txt` — CPU build (CUDA uses the one-liner below)

## Architecture

**Page ownership (single-owner per page, shared-nothing across pages).** The key space
is split into pages;
exactly one owner (one CUDA block) reads/writes a page's slots. The same key always routes
to the same owner, so writes serialize by ownership — **no store atomics, no OCC, no delta
log**. Point ops bin to their pages (a counting sort) and the GPU runs one block per page.

**Resident scan column.** A 16M-value `uint32` column lives in VRAM, filled once, never
shipped per query. `OP_SCAN` queries (threshold carried in the payload) flow through the
same ring + batcher and run the chosen `u32x4` vectorized kernel over it in place.

### Routing

Both engines live in one process behind a `Router`. A `CostModel` (parameterized by
measured hardware constants) decides each dataset's single home — point-op KV store on
the CPU, scan columns on CPU or GPU by size — and queries execute where their data lives.
No data is duplicated, so there is no coherence protocol. A `MemorySpace`/`MemoryModel`
seam keeps the design open to unified-memory hardware (e.g. DGX Spark, Grace-Hopper);
only the discrete-memory path is implemented today.

## Build & run locally (CPU)
```sh
clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o matrixdb_proto   # Apple Silicon
# g++ -std=c++20 -O3 -march=native main.cpp -o matrixdb_proto      # Linux x86_64
./matrixdb_proto

./run_tests.sh   # CI gate: compile + run every CPU test + the pipeline oracle (exit non-zero on any failure)
```

## Test on Google Colab (GPU)

**Easiest:** open `matrixdb_colab.ipynb` in Colab (Runtime → T4 GPU → Run all). It writes
its own source, runs the CPU coverage test, builds with `nvcc`, and runs — no uploads.

**Manual** — Runtime → T4 GPU, then:
```sh
nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread -DMATRIX_USE_CUDA main.cpp -o matrixdb_proto
./matrixdb_proto
```
Output ends with `Scan result sum: 83886070 (oracle 83886070)` and
`Engine execution loop completed successfully.` — asserts fire on any mismatch.

> `nvcc -x cu` compiles `main.cpp` as CUDA so the `.cuh` kernels link in.
> `-std=c++17` because some nvcc versions lag on C++20.
> `-D_GNU_SOURCE -Xcompiler -pthread` expose Linux thread-affinity + std::thread.

## How it was built: measure, then cut

Each step was decided by a benchmark, not a guess. Notable findings:
- **Sub-microsecond ingestion confirmed:** raw SPSC handoff ~64 ns p50 on the T4.
- **GPU point-ops lose by physics:** PCIe < CPU cache; no amount of tuning changes it.
- **Scan bandwidth lever:** narrowest column type + vectorized (`uint4`) loads won; register
  blocking (CUB's `ITEMS_PER_THREAD`) was tested and came second — hardware decided, not theory.
- **A GPU-only kernel bug** (wrong index striding) was caught *before* a Colab run by
  `test_scan_coverage.cpp`, which simulates the kernel's integer math on the CPU.

## Known limits / deferred (marked `// ponytail:` at each site)
- **Per-scan overhead is NOT a problem** (measured): cudaEvent instrumentation puts GPU
  launch/copy/sync at ~4% of scan time (kernel 0.41 ms/scan @ 163 GB/s on the 16M column).
  An earlier "70% overhead" claim was a measurement artifact (host-wall vs cudaEvent timing,
  mismatched column sizes) — the integrated scan is ~96% efficient.
- **HTAP head-of-line blocking — fixed.** Point ops and scans now run on separate queues
  and threads (GPU: separate streams), so a multi-ms scan no longer stalls point ops.
  Measured on CPU: point-op queue residency p99 ~1.8 ms with ten 8 ms scans running
  concurrently (was ~40 ms pre-split, ~22×). On GPU, point-op residency is no longer
  scan-bound — what remains (~2.6 ms p99) is the point-op path's own per-batch
  launch/sync cost (see next item), not scan interference.
- **Per-batch GPU sync (point-op path):** each `execute_batch` blocks on a
  `cudaStreamSynchronize`, so queries queue while it runs. Double-buffering / async
  batches would hide it. Hypothesis — not yet instrumented like the scan path was.
- **Scans return a count, not rows;** no SUM/MIN/MAX yet.
- **Full OCC** (TEV lock-bit + read-set validation) — unnecessary while page ownership holds.
- **Page binning on host** — folding it into the dual-trigger batcher is a future step.
- Hyper-Q multi-stream, `cudaHostRegister` pinned-DMA.

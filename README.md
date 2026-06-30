# MatrixDB Prototype

A GPU-accelerated analytical database engine: a lock-free ingestion pipeline that routes each
workload to the hardware where it wins, over a three-tier (VRAM / RAM / SSD) columnar store with a
full analytical query surface — and **every GPU result cross-checked byte-for-byte against the CPU**.

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

**A GPU cannot beat a CPU at key-value point ops, but wins big at scans/aggregations over resident data.**
The bus to the GPU (PCIe ~12 GB/s) is slower than the CPU's own cache, so tiny point ops
lose. But data living *resident* in VRAM, scanned in place, runs at the GPU's memory
bandwidth — far past what the CPU can stream. MatrixDB routes each workload to where it wins.

| Workload | CPU | GPU (Tesla T4) | Winner |
|---|---|---|---|
| Point ops (KV get/put) | ~19M ops/sec | ~20M ops/sec (plateaus) | **CPU** (GPU PCIe-bound) |
| Scan, standalone kernel | ~10 GB/s | **~240 GB/s** (75% of peak) | **GPU ~24×** |
| Scan/aggregate, end-to-end through engine | ~8–20 ms/scan | **~0.5 ms/scan (~135 GB/s)** | **GPU ~16–25×** |

(64 MB resident `uint32` column, filter-count. Standalone vs end-to-end differ because each
integrated scan still pays per-scan launch/copy/sync overhead — see *Known limits*.)

## What it does now

A real analytical engine, not just a count-scan demo — the CPU surface is feature-complete and the
**entire analytical surface is also verified on the GPU**:

- **Analytical queries** over `uint32` / `int64` / `double` columns: COUNT / SUM / MIN / MAX / AVG,
  COUNT(DISTINCT), scalar **and** `GROUP BY`, filters (GT/GE/LT/LE/EQ/NE/BETWEEN), NULL-aware
  (SQL semantics), top-N (`ORDER BY agg DESC LIMIT`), HAVING — via a struct API *and* a SQL-ish
  text parser. **Cross-column WHERE** (filter one column, aggregate another).
- **Dictionary-encoded strings** as a first-class type: `GROUP BY` a string dimension, `WHERE s = 'x'`
  and ordered/`BETWEEN` (sorted dictionary), COUNT(DISTINCT), durable — encoded to `u32` codes so they
  ride the whole engine (tiering, snapshot, GPU) for free.
- **Three-tier storage** DEVICE (VRAM) / HOST (RAM) / COLD (SSD) with heat-driven auto-tiering: hot
  analytical columns auto-promote to VRAM and are scanned there in place; cold columns spill to SSD and
  are pulled back on demand.
- **GPU analytical surface — complete & T4-verified.** Scalar + grouped reductions, all three types,
  filtered + unfiltered, run in VRAM. Each kernel is gated by a **cross-backend invariant**: its result
  must equal the CPU reducer over the *same bytes* (the merge gate for every GPU piece — see the `4b`–`4h`
  cells in the notebook).
- **Durability & transactions:** write-ahead log + checkpoint/compaction + backup/restore + selectable
  fsync policy + corrupt-WAL recovery (torn tail dropped, CRC-stopped replay); atomic group-commit
  transactions; catalog snapshot/restore (incl. string dictionaries).
- **Serving:** a serializable GET/PUT/QUERY/HEALTH/STATS request protocol + length-prefixed TCP transport
  + a client driver, with token authentication, per-principal column-level access control, and an audit log.
- **Ops:** health/readiness probe, query-latency + tiering metrics, structured leveled logging, admission
  control (group-count cap), graceful shutdown, semver build version.

Verified by **65 CPU tests + the pipeline oracle** (`run_tests.sh`), clean under ASan/UBSan and TSan; the
GPU cross-checks run on a Colab T4 via the notebook.

## Files

**Pipeline & contract** — `types.hpp` (`DatabaseQuery` POD, opcodes, page/store layout), `ring_buffer.hpp`
(lock-free SPSC ring), `compute.hpp` (`ComputeInterface`, `MatrixQuery`, the CPU reducers that are the
semantics-of-record + the scan codec).

**Routing & tiering** — `memory_model.hpp` / `cost_model.hpp` / `router.hpp` (cost-based hardware router),
`tiered_column.hpp` (one column's bytes resident in exactly one of HOST/DEVICE/COLD), `tier_manager.hpp`
(the heat-driven promote/demote brain), `migration_executor.hpp` (applies its decisions).

**Engines** — `compute_mock.cpp` (the CPU analytical engine: catalog, `execute_query`, WAL, strings, server
hooks), `compute_cuda.cuh` (the CUDA engine: page-ownership point ops, the `u32x4` resident scan, and the
analytical reduction kernels — scalar/grouped × u32/i64/f64 × filtered/unfiltered).

**Storage & I/O** — `kv_store.hpp` (point-op hash store), `cold_store.hpp` (SSD WAL), `column_io.hpp`
(binary column persistence), `csv_ingest.hpp` (CSV ingest).

**Serving & ops** — `server.hpp` (request protocol + auth/authz/audit), `server_tcp.hpp` (TCP adapter),
`client.hpp` (client driver), `version.hpp`, `logging.hpp`.

**CLI** — `matrix_cli.hpp` (the testable `matrix_repl` shell: dot-commands + SQL router + decoded output),
`matrixdb_cli.cpp` (the thin `matrixdb` main).

**Harness** — `main.cpp` (orchestration + benchmarks + oracle asserts), `run_tests.sh` (CI gate),
`test_*.cpp` (65 CPU tests, run by `run_tests.sh`), `test_gpu_*.cu` (GPU cross-check gates), `make_notebook.py` (regenerates
`matrixdb_colab.ipynb` from the real source — run after edits).

**Docs** — `FINDINGS.md` (engineering journal), `PRODUCTION_READINESS.md` (per-increment gap register),
`docs/superpowers/` (specs + plans).

## Architecture

**Page ownership (single-owner per page, shared-nothing across pages).** The key space is split into
pages; exactly one owner (one CUDA block) reads/writes a page's slots. The same key always routes to the
same owner, so writes serialize by ownership — **no store atomics, no OCC, no delta log**. Point ops bin
to their pages (a counting sort) and the GPU runs one block per page.

**Tiered analytical catalog.** Columns register into a catalog whose `TierManager` keeps the hot working
set in the fastest tier under a budget: hot columns promote HOST→DEVICE (VRAM), cold ones demote
HOST→COLD (SSD), and a scan of a non-resident column borrows it back. `execute_query` runs scalar/grouped
aggregation with predicates and NULL semantics over these columns; when a column is DEVICE-resident the
reduction runs on the GPU in place, otherwise on the CPU — same result either way.

**Cross-backend invariant.** The CPU reducers (`matrix_cpu_reduce*`) are the semantics-of-record. Every
GPU kernel is correct *iff*, over the same bytes, its result equals the CPU reducer's — the same checksum
discipline that anchors the page-ownership store. This is the merge gate for each GPU increment.

**Routing.** Both engines live in one process behind a `Router`. A `CostModel` (parameterized by measured
hardware constants) decides each dataset's single home and queries execute where their data lives. No data
is duplicated, so there is no coherence protocol. A `MemorySpace`/`MemoryModel` seam keeps the design open
to unified-memory hardware (e.g. Grace-Hopper); only the discrete-memory path is implemented today.

## Build & run locally (CPU)
```sh
clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o matrixdb_proto   # Apple Silicon
# g++ -std=c++20 -O3 -march=native main.cpp -o matrixdb_proto      # Linux x86_64
./matrixdb_proto

./run_tests.sh   # CI gate: compile + run every CPU test + the pipeline oracle (exit non-zero on any failure)
SAN=1 ./run_tests.sh   # same, under AddressSanitizer + UBSan (catches UB/OOB; slower)
```

## Use it (CLI / REPL)

`matrixdb` is an interactive shell over the engine — load CSV columns, run SQL at a prompt, see decoded
results. Build it and pipe it a session (diagnostics go to stderr, so stdout stays clean for piping):

```sh
clang++ -std=c++20 -O2 matrixdb_cli.cpp -o matrixdb     # g++ works too
printf 'amount,region\n10,books\n900,games\n20,books\n950,music\n' > demo.csv

./matrixdb                          # interactive REPL on stdin
./matrixdb -c "SELECT AVG(amount)"  # run one line, exit
./matrixdb -f session.sql           # run a file of commands/queries, exit
```

**Try the bundled tour** (from the repo root): `./matrixdb -f examples/tour.sql` — loads two CSVs and walks
every form (aggregates, `WHERE`, multi-aggregate, top-N, `HAVING`, `COUNT(DISTINCT)`, a join, `.save`).
`.timing on` prints per-query wall-time.

```text
matrixdb> .load demo.csv amount u32 col0 header
loaded 4 rows into "amount" (u32, col 0)
matrixdb> .load demo.csv region str col1 header     # strings are dictionary-encoded
loaded 4 rows into "region" (str, col 1)
matrixdb> SELECT SUM(amount) GROUP BY region        # grouped, decoded string labels
books │ 30
games │ 900
music │ 950
matrixdb> SELECT region WHERE amount > 100           # projection, decoded
games
music
```

Commands: `.load <csv> <name> <u32|i64|f64|str> [colN] [header|noheader]`, `.save <file>` / `.open <file>`
(catalog snapshot, string dictionaries included), `.timing on|off`, `.mode list|csv|table` (human ` │ ` /
machine-readable CSV / aligned columns), `.tables`, `.columns`, `.stats`, `.help`, `.quit`.
Queries: `SELECT COUNT|SUM|MIN|MAX|AVG(col) [WHERE col <op> v] [GROUP BY key] [HAVING agg <op> v | ORDER BY agg DESC LIMIT n]`,
multi-aggregate `SELECT agg(a), agg(b) …`, `SELECT COUNT(DISTINCT col)`, and projection
`SELECT col [WHERE col <op> v] [LIMIT n]`. Joins: `SELECT lcol, rcol JOIN lkey = rkey [LIMIT n]` (inner
equi-join, one column per side — left before the comma, right after; u32/i64 keys), `SELECT COUNT(*) JOIN
lkey = rkey`, and aggregate-over-join `SELECT agg(lcol) JOIN lkey = rkey [GROUP BY rcol [HAVING agg <op> v |
ORDER BY agg DESC LIMIT n]]` (sum/count/min/max a left measure by a right dimension, then filter or rank —
the star query). Malformed input prints a friendly `Error:` line — never crashes.
(`matrixdb>` is shown for clarity; the REPL reads plain lines.) A network/server mode and readline history
are the remaining deferrals.

## Test on Google Colab (GPU)

**Easiest:** open `matrixdb_colab.ipynb` in Colab (Runtime → T4 GPU → Run all). It writes its own source,
runs the CPU tests, then builds with `nvcc` and runs the GPU cross-check cells (`4b`–`4h`): migration,
aggregation, predicates, grouped, typed int64/double, and the VRAM-resident `execute_query` (scalar +
grouped) — each asserting `GPU == matrix_cpu_*`. No uploads.

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

Each step was decided by a benchmark or a cross-check, not a guess. Notable findings:
- **Sub-microsecond ingestion confirmed:** raw SPSC handoff ~64 ns p50 on the T4.
- **GPU point-ops lose by physics:** PCIe < CPU cache; no amount of tuning changes it.
- **Scan bandwidth lever:** narrowest column type + vectorized (`uint4`) loads won; register
  blocking (CUB's `ITEMS_PER_THREAD`) was tested and came second — hardware decided, not theory.
- **A GPU-only kernel bug** (wrong index striding) was caught *before* a Colab run by
  `test_scan_coverage.cpp`, which simulates the kernel's integer math on the CPU.
- **The cross-check gate earned its keep:** the Colab runs caught a C++17/`std::bit_cast` regression and
  two `nvcc` `atomicAdd(double*)` arch-guard bugs that local CPU builds had masked — each fixed before merge.

## Known limits / deferred (marked `// ponytail:` at each site)
- **Cross-column WHERE is scalar-only** for now; grouped cross-column (`GROUP BY dim WHERE other …`) and a
  non-`u32` filter column are the next SQL increments (see `docs/superpowers/specs/…-richer-sql-grammar-design.md`).
- **SQL grammar is the analytical subset people type, not full ANSI** — scalar + grouped aggregates,
  predicates (incl. **cross-column WHERE**), **multi-aggregate `SELECT`**, **projections**, and a REPL
  **inner equi-join** (`SELECT a, b JOIN lk = rk` + `COUNT(*)`, u32/i64 keys) are implemented; a cost-based
  planner, outer/multi-key joins are roadmap items.
- **Concurrent serving is reads-parallel, writes-serialized** (single-writer / many-readers via a
  `std::shared_mutex` — `ConcurrentServer`): analytical reads over HOST-resident columns run in parallel
  (verified race-free under ThreadSanitizer), writes serialize, and a read needing a tier borrow escalates
  to the exclusive lock. The lock-free single-owner *write* model (zero store atomics) is preserved.
  v1 limits (deferred): a COLD/DEVICE read escalates rather than running concurrently (wants epoch/snapshot
  reclamation); pure concurrent reads don't accrue tiering heat; the global lock can starve writers under
  sustained reads; GPU-engine concurrency and MVCC isolation are future.
- **Transport is plaintext** (TLS wants a vetted library); **no encryption-at-rest** (won't hand-roll crypto).
- **Per-batch GPU sync (point-op path):** each `execute_batch` blocks on a `cudaStreamSynchronize`;
  double-buffering / async batches would hide it. HTAP head-of-line blocking between scans and point ops is
  already fixed (separate queues/threads, GPU separate streams).
- **Page binning on host** — folding it into the dual-trigger batcher is a future step. Hyper-Q multi-stream
  and `cudaHostRegister` pinned-DMA remain open.

# Network + GPU Round-Trip Spike — Design & Scope

**Date:** 2026-06-30  **Status:** design, pending implementation
**Goal:** Answer the one open question gating the Java/Spring-Boot-on-MatrixDB roadmap (Leg 1:
package MatrixDB as a real DB; Leg 2: expose it as a dependency to a Java 26 / Spring Boot 4
backend) — does routing scan/aggregate queries over TCP + a JVM client still deliver a
multi-fold compute win once real network + serialization cost sits on top of the already-measured
in-process GPU win, or does that cost eat the win alive?

## Why this gates everything

- `FINDINGS.md` already measured, in-process, on a Tesla T4: point ops lose to CPU by ~5× (PCIe-
  bound); scans/aggregations over VRAM-resident data win ~12–25× (standalone kernel ~24×,
  end-to-end through the engine ~12–19×). Crossover is ~4–8MB.
- All of that is **in-process** — no network, no serialization, no JVM. The networked-serving
  design (`2026-06-30-matrixdb-networked-serving-design.md`) explicitly host-only-gates the very
  hop this spike needs (`bind` is blocked in the sandbox), so live client↔server behavior has
  never been measured.
- `matrixdbd.cpp` today only ever instantiates `CPUMockEngine` — the `#if defined(MATRIX_USE_CUDA)`
  engine selection that `main.cpp` (the CLI) already has was never carried into the daemon. So as
  of today, network + real GPU have literally never run together.
- **Agreed decision rule:** proceed to Leg 1 (package MatrixDB) and Leg 2 (Java/Spring Boot
  dependency) only if network + serialization overhead is small relative to the measured compute
  gap for realistic (≥8MB, VRAM-resident) scan/aggregate workloads. If it flattens the win to
  something unremarkable, stop and rethink the architecture before investing in packaging or a
  Java client.

## Correction found during planning: don't touch `matrixdbd`'s tested serve chain

The original premise here was "mirror `main.cpp`'s `#if defined(MATRIX_USE_CUDA)` engine
selection into `matrixdbd.cpp`." That doesn't fit: `matrixdbd`'s entire serve chain
(`matrix_serve`, `matrix_serve_conn`, `matrix_serve_conn_auth`, `matrix_serve_tcp`,
`matrix_serve_tcp_auth`, across `server.hpp`/`server_tcp.hpp`) is hardcoded to `CPUMockEngine&`,
not templated or written against the abstract `ComputeInterface`. Making the *production* daemon
GPU-capable means templating five functions in the most heavily-tested serving path
(`test_server.cpp`, `test_server_tcp.cpp`, `test_security.cpp`, `test_audit.cpp`,
`test_recv_timeout.cpp`, `test_client.cpp`, `test_concurrent_serving.cpp` all instantiate them
against `CPUMockEngine`). That's legitimate Leg-1-shaped work, but it's more invasive than a
throwaway spike should touch, and this spec already scopes production hardening as out.

**Revised approach:** a small, standalone `spike/spike_server.cpp` that does NOT modify
`matrixdbd.cpp`, `server.hpp`, or `server_tcp.hpp`. It reuses only the generic, engine-agnostic
framing helpers already there (`matrixsrv_detail::recv_all`/`send_all` in `server_tcp.hpp` — free
functions, not engine-typed) for wire-format fidelity (length-prefixed frames, matching
`matrixdbd`'s own transport), and drives a `ComputeInterface*` (either `CPUMockEngine` or
`CUDAGPUEngine`, selected by `#if defined(MATRIX_USE_CUDA)` — this part of the original idea does
hold, since both concrete engines share `ComputeInterface` and `main.cpp` already proves the
pattern) directly, calling `execute_scan` on the resident column each engine already builds at
construction. Zero risk to tested production code; still exercises the real thing being measured
(TCP + framing + serialization + an actual GPU kernel call).

## Spike architecture

- Runs entirely inside one self-contained Colab notebook (T4 GPU runtime) — the same
  "generate the notebook from real source via `%%writefile`" discipline `make_notebook.py`
  already uses, so the spike can't silently drift from the code being measured. A new, narrowly
  scoped generator (not an addition to the existing 580-line master notebook) that embeds the
  non-test core engine headers through `compute_mock.cpp`/`compute_cuda.cuh`, plus the new
  `spike/spike_server.cpp`.
- A minimal Java client (`spike/SpikeClient.java`) — raw `java.net.Socket`, no Spring Boot, no
  Maven/Gradle. This is a measurement throwaway, not product code. It speaks a minimal
  length-prefixed protocol matching `spike_server.cpp`: a `SCAN` request (opcode + threshold) and
  a zero-length `HEALTH`-equivalent request, timing each round trip with `System.nanoTime()`.
- Notebook flow: build `spike_server` twice — once plain (`CPUMockEngine`), once with
  `-DMATRIX_USE_CUDA` via `nvcc` (`CUDAGPUEngine`) — start each on loopback in turn, and run the
  same Java client sweep against both, for the CPU-via-network vs GPU-via-network comparison.

## Measurement plan

- Use the engine's existing fixed 64MB resident column (`MATRIX_SCAN_COLUMN_SIZE` =
  16,777,216 `uint32` values, filled `value[i]=i` at construction, in `types.hpp`) — the exact
  same column the already-measured 12–19× end-to-end number comes from (`FINDINGS.md` §3.5).
  Reusing it means the spike answers the decision rule at the single most relevant, already-
  validated data point without needing the catalog/tiered-column machinery to size a custom
  column. (A full size sweep like the in-process roofline bench is a nice-to-have, not needed to
  pass/fail the decision rule below — scope it as follow-up only if the single-point result is
  ambiguous.)
- For each engine (CPU, GPU): N repeated `SCAN` round trips from the Java client, threshold fixed
  at half the column (8,388,608) so every run does the same amount of work; record min + median
  wall time (matches the "warm, min of repeated runs" methodology used elsewhere in this repo).
- Separately measure N zero-payload round trips per engine, to isolate the fixed network +
  serialization tax from the data-dependent compute time.
- Compute: (a) via-network GPU/CPU speedup, compared against the known in-process ratio (12–25×)
  to see how much the network hop erodes it; (b) fixed tax as a fraction of the SCAN round-trip
  time.

## Decision rule

- **PASS** (proceed to Leg 1/Leg 2): via-network GPU speedup over via-network CPU, at the fixed
  64MB column, is still ≳10× (the network tax ate less than half the in-process 12–25× win),
  **and** the fixed zero-payload round-trip tax is small (<10%) relative to the SCAN round-trip
  time.
- **FAIL** (stop, rethink before building Leg 1/Leg 2): speedup collapses well below the
  in-process number (e.g. <5×), or the fixed tax dominates the SCAN round trip — that would mean
  the network hop itself, not compute, is the bottleneck, and no amount of packaging fixes that;
  it would need connection reuse/request batching/a different transport before this architecture
  is worth pursuing.
- These numbers are a decision aid, not a hard cliff — bring the actual measurements back for a
  judgment call, especially near the boundary.

## Scope & non-goals

**In:** the standalone `spike_server.cpp`, the minimal Java wire-protocol client, the spike
notebook generator, running it on Colab, recording results.

**Out, deferred pending PASS:** Spring Boot, TLS, connection pooling/multi-connection concurrency,
MatrixDB packaging (Leg 1), the Java dependency/library shape (Leg 2). None of that is built until
this spike passes.

**Out, permanently, for this spike:** production-hardening the Java client — it is a throwaway
measurement tool, not shipped code.

## Deliverables

- `spike/spike_server.cpp` — the standalone spike server (no changes to production files).
- `spike/SpikeClient.java` — one new minimal Java client source file.
- `make_spike_notebook.py` — a new notebook generator script — plus the generated `.ipynb` (Colab).
- A short results entry appended to `FINDINGS.md` once it's actually run — continuing this
  repo's "measure, then cut" journal discipline. Not written as part of this spec; written after
  the spike runs and produces numbers.

## Execution note (human-in-the-loop)

Colab requires a human to open the notebook, select the GPU runtime, and run the cells — that
part can't be automated from here. The implementation phase's deliverable is the generator script
and the notebook; running it and reporting the numbers back is a manual step.

## Success criteria for this spec

A future engineer (or a future session) can read this and (a) understand exactly what number is
being chased and why, (b) regenerate the spike notebook from source, (c) run it on Colab and get
a PASS/FAIL against the decision rule above.

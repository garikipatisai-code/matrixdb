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

## Prerequisite fix: wire `CUDAGPUEngine` into `matrixdbd`

Mirror `main.cpp`'s existing `#if defined(MATRIX_USE_CUDA)` engine selection into
`matrixdbd.cpp`'s `main()`, so a `-DMATRIX_USE_CUDA` build of the daemon actually serves off the
GPU engine instead of always defaulting to `CPUMockEngine`. Small, mechanical, no protocol change.

## Spike architecture

- Runs entirely inside one self-contained Colab notebook (T4 GPU runtime) — the same
  "generate the notebook from real source via `%%writefile`" discipline `make_notebook.py`
  already uses, so the spike can't silently drift from the code being measured. A new, narrowly
  scoped generator (not an addition to the existing 580-line master notebook) that embeds only
  what `matrixdbd` needs to build: the core engine headers, `compute_mock.cpp`, `compute_cuda.cuh`,
  and `matrixdbd.cpp`.
- A minimal Java client — raw `java.net.Socket`, no Spring Boot, no Maven/Gradle. This is a
  measurement throwaway, not product code. It implements just enough of the wire protocol
  (per the networked-serving spec) to: send the auth token frame, send a `QUERY` request (SUM
  filter-count over the resident scan column), send a zero-payload `HEALTH` request, and time
  each round trip with `System.nanoTime()`.
- Notebook flow: build + start `matrixdbd` (CUDA build) on loopback; run the Java client sweep
  against it; rebuild `matrixdbd` as `CPUMockEngine`-only and repeat, for the CPU-via-network
  baseline.

## Measurement plan

- Sweep the same column sizes as the existing GPU roofline bench (64KB → 64MB, matching
  `bench_gpu_roofline.cu` / the README sweep), same SUM-filter-count query shape.
- For each (size × engine): N repeated round trips from the Java client; record min + median wall
  time (matches the "warm, min of repeated runs" methodology used elsewhere in this repo).
- Separately measure the `HEALTH` round trip (zero data) per engine, to isolate the fixed
  network + serialization tax from the data-dependent compute time.
- Compute: (a) via-network GPU/CPU speedup per size, compared against the known in-process ratio
  (12–25×) to see how much the network hop erodes it; (b) fixed tax as a fraction of total
  round-trip time at 8MB and 64MB.

## Decision rule

- **PASS** (proceed to Leg 1/Leg 2): at ≥8MB, via-network GPU speedup over via-network CPU is
  still ≳10× (the network tax ate less than half the in-process win), **and** the fixed `HEALTH`
  tax is small (<10%) relative to the 8MB+ round-trip time.
- **FAIL** (stop, rethink before building Leg 1/Leg 2): speedup collapses well below the
  in-process number (e.g. <5×), or the fixed tax dominates even at 64MB — that would mean the
  network hop itself, not compute, is the bottleneck, and no amount of packaging fixes that;
  it would need connection reuse/request batching/a different transport before this architecture
  is worth pursuing.
- These numbers are a decision aid, not a hard cliff — bring the actual measurements back for a
  judgment call, especially near the boundary.

## Scope & non-goals

**In:** the `matrixdbd` CUDA-wiring fix, the minimal Java wire-protocol client, the spike notebook
generator, running it on Colab, recording results.

**Out, deferred pending PASS:** Spring Boot, TLS, connection pooling/multi-connection concurrency,
MatrixDB packaging (Leg 1), the Java dependency/library shape (Leg 2). None of that is built until
this spike passes.

**Out, permanently, for this spike:** production-hardening the Java client — it is a throwaway
measurement tool, not shipped code.

## Deliverables

- `matrixdbd.cpp` patch (CUDA engine wiring).
- One new minimal Java client source file, in a new `spike/` directory.
- A new notebook-generator script + the generated `.ipynb` (Colab).
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

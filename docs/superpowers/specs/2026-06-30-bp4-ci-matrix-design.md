# BP-4 — CI Build Matrix — Design & Scope

**Date:** 2026-06-30  **Status:** design, pending implementation

**Goal:** Close BP-4 in `PRODUCTION_READINESS.md`'s gap register — "no multi-platform build matrix
(Apple ARM / Linux x86 / CUDA)" — by adding a real GitHub Actions workflow that builds and tests
MatrixDB on the platforms this project claims to support, on every push to `main`. This is a
continuation of Leg 1 (packaging MatrixDB as a real, shippable app), not a step toward Leg 2 —
Leg 2 (Java/Spring Boot GPU exposure) is explicitly out of scope for this and all current work.

## Why this scope, not a bigger one

The repo has a GitHub remote (`garikipatisai-code/matrixdb`) but no `.github/workflows` at all —
every build/test run to date has been manual, on whatever machine the developer happened to be
using. `PRODUCTION_READINESS.md` already names the gap precisely (BP-4) and a closely related one
(QA-6: "CUDA path has no automated test — host-syntax probe + manual runs only ... needs GPU CI").

This repo commits directly to `main` (no PR workflow, confirmed by its entire commit history), so
the workflow triggers on `push` to `main` — post-hoc validation of what landed, not a merge gate
(there's nothing to gate). GitHub-hosted free-tier runners have no GPU, so the CUDA leg is a real
`nvcc` **compile-only** check, not a kernel-execution test — this upgrades today's clang-based
"host-syntax probe" (a stubbed `cuda_runtime.h`, syntax-only) to a genuine `nvcc` compile, which
this project's own history shows catches real CUDA-specific bugs clang's probe can't (README.md:
"two `nvcc` `atomicAdd(double*)` arch-guard bugs that local CPU builds had masked"). It does **not**
close QA-6 — that still needs an actual GPU runner to execute kernels — and this spec does not
claim otherwise.

## Scope

**In:**
1. **`.github/workflows/ci.yml`** — one workflow, four independent jobs, triggered on `push` to
   `main`:
   - **`test-linux`** (`ubuntu-latest`, x86_64): checkout, `./build_all.sh`, `./run_tests.sh`.
     Ubuntu runners ship `clang++` already — no toolchain install step needed.
   - **`test-macos`** (`macos-14`, native arm64 — GitHub's `macos-14` runners are Apple Silicon):
     same two commands. This is the real "Apple ARM" leg — no cross-compilation, no emulation.
   - **`cuda-compile-check`** (`ubuntu-latest`): `apt-get install -y nvidia-cuda-toolkit` (installs
     `nvcc`; no GPU driver needed to *compile*), then run the exact command already documented in
     `README.md`: `nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread -DMATRIX_USE_CUDA
     main.cpp -o matrixdb_proto`. Compile-only — no kernel launch, no GPU present on this runner.
   - **`docker-build`** (`ubuntu-latest`, Docker preinstalled): `docker build -t matrixdb .`, then
     `docker run -d -p 7070:7070 matrixdb`, a short sleep, `docker logs` grep for the dev-mode
     startup line (`matrixdbd: serving on port 7070 (no auth — dev mode)`), then `docker stop`.
     GitHub's runners are real hosts (`bind()` is not blocked, unlike this project's local sandbox)
     — this is the first time the Dockerfile and matrixdbd's bind/listen path are actually
     exercised end-to-end instead of reviewed by inspection.
2. **Register update.** Mark BP-4 landed in `PRODUCTION_READINESS.md`. Add a short addendum to the
   existing QA-1 note (the local CI gate now also runs in the new CI matrix, on two platforms, plus
   a compile-check job) and to the QA-6 row (the compile-check upgrade landed; execution still
   needs a GPU runner — QA-6 stays open, not overclaimed as fixed).

**Out (deferred, explicitly not part of this increment):**
- **Windows.** No existing build convention targets it (`build.sh`/`build_all.sh` are POSIX shell;
  no `.bat`/PowerShell equivalent exists), and nothing in the register asks for it — BP-4's own gap
  text names "Apple ARM / Linux x86 / CUDA," not Windows.
- **Branch protection / required status checks.** There's no PR flow to attach a gate to; the
  workflow is informational (a red run flags a bad commit fast) rather than a blocker.
- **A README status badge.** Cosmetic, trivial to add later; not requested for this increment.
- **Actual GPU kernel execution in CI (closing QA-6 fully).** Needs a paid/self-hosted GPU runner,
  out of reach here — unchanged from the register's existing "needs GPU CI" note.
- **CUDA compile of files beyond `main.cpp`.** `main.cpp` already pulls in `compute_cuda.cuh` under
  `-DMATRIX_USE_CUDA` and is the exact command this project's own README already documents as the
  GPU build; reusing it (rather than inventing a new build target) is the smaller, already-proven
  surface. `matrixdbd.cpp`/`spike_server.cpp` under CUDA are not part of this check.

## Design decisions

**Four independent jobs, not a matrix-strategy single job.** GitHub Actions' `strategy.matrix` is
built for "the same steps, varying one axis" (e.g. same steps on 3 OSes). Here the *steps* differ
per leg (macOS/Linux run `run_tests.sh`; the CUDA leg runs a single `nvcc` command; the Docker leg
runs `docker build`/`run`) — four plain jobs is simpler and clearer than forcing dissimilar steps
into one matrix with per-leg conditionals.

**No new build targets or scripts.** Every job runs a command that already exists and is already
documented (`build_all.sh`, `run_tests.sh`, the README's own `nvcc` line, `docker build`/`run` per
the Dockerfile's own usage comment). This workflow's only job is to run those existing, proven
commands automatically instead of leaving that to a human's memory.

**Docker smoke check is log-based, not wire-protocol-aware.** The protocol is a custom
length-prefixed binary format, not HTTP — writing a wire-protocol-aware CI health check would be a
new client just for this check. Confirming the container stays up and its stderr shows the expected
dev-mode startup line is a real, if shallow, verification that the binary starts and doesn't crash
immediately; a genuine `MatrixClient` round-trip test is future work if this needs more depth.

## What "done" looks like

- `.github/workflows/ci.yml` exists with the four jobs described above, triggered on `push` to
  `main`.
- Pushing this spec/plan itself (once implementation lands) triggers a real GitHub Actions run —
  since this sandbox has no GitHub Actions access, the actual green/red run is a **manual,
  host-verification step** (push and watch the Actions tab), same status as this project's other
  "HOST-ONLY" register items. Local verification is limited to: the YAML parses (`actionlint` or
  a manual read), and the exact shell commands inside each job have already been proven to work in
  this sandbox individually (`build_all.sh`, `run_tests.sh` — both green here; the `nvcc` and
  `docker` commands cannot be run in this sandbox at all, so their correctness rests on being
  copied verbatim from already-documented, already-used invocations, not re-derived).
- `PRODUCTION_READINESS.md`'s BP-4 row is marked landed; QA-1's note and QA-6's row get honest
  addenda (QA-6 NOT marked fixed).

## Success criteria for this spec

A future engineer can read this and (a) understand exactly which 4 CI jobs are being added and why
each runs what it runs, (b) build the plan from the "done" list above without further design
decisions, (c) know precisely that this closes BP-4 but only partially improves QA-6 (compile-check
only, not GPU execution) — so nobody mistakes "CI exists" for "the GPU path is automated end to
end."

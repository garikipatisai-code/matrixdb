# BP-4 CI Build Matrix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a GitHub Actions workflow (`.github/workflows/ci.yml`) that builds and tests MatrixDB
on Linux x86, macOS ARM, does an `nvcc` compile-check of the CUDA path, and builds+smoke-tests the
Docker image — on every push to `main` — closing BP-4 in `PRODUCTION_READINESS.md`.

**Architecture:** One workflow file, four independent jobs (no `strategy.matrix` — the steps differ
per leg, so four plain jobs are simpler than matrix + conditionals). Every job runs a command that
already exists and is already proven in this repo (`build_all.sh`, `run_tests.sh`, the README's own
documented `nvcc` line, the Dockerfile's own documented `docker build`/`run` usage) — this workflow
automates commands that already work, it does not invent new ones.

**Tech Stack:** GitHub Actions YAML, `actions/checkout@v4` (the standard GitHub-provided checkout
action), the repo's existing `build_all.sh`/`run_tests.sh` shell scripts, `nvcc` (via the Ubuntu
`nvidia-cuda-toolkit` package), Docker (preinstalled on GitHub's `ubuntu-latest` runners).

**Governing spec:** `docs/superpowers/specs/2026-06-30-bp4-ci-matrix-design.md` — read it for the
full rationale; this plan implements it task-by-task.

**A note on verification in this sandbox:** this dev sandbox has no `nvcc`, no `docker`, and no
access to GitHub Actions itself — so "does the workflow actually run green on GitHub" is a
**manual, host-verification step** (push and watch the Actions tab), exactly like this project's
other HOST-ONLY register items (e.g. `matrixdbd`'s `bind()`). Each task below verifies everything
that *can* be verified locally (YAML syntax, exact command parity with already-proven local
commands, the register's own markdown validity) and is explicit about what's deferred to a real
push.

---

### Task 1: Create the GitHub Actions workflow

**Files:**
- Create: `.github/workflows/ci.yml`

- [ ] **Step 1: Create the directory and the workflow file**

Create `.github/workflows/ci.yml` with this exact content:

```yaml
name: CI

on:
  push:
    branches: [main]

jobs:
  test-linux:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Build (matrixdb + matrixdbd)
        run: ./build_all.sh
      - name: Test (full suite + oracle + CLI/daemon smoke checks)
        run: ./run_tests.sh

  test-macos:
    runs-on: macos-14
    steps:
      - uses: actions/checkout@v4
      - name: Build (matrixdb + matrixdbd)
        run: ./build_all.sh
      - name: Test (full suite + oracle + CLI/daemon smoke checks)
        run: ./run_tests.sh

  cuda-compile-check:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install the CUDA toolkit (nvcc only — this runner has no GPU)
        run: sudo apt-get update && sudo apt-get install -y nvidia-cuda-toolkit
      - name: Compile the CUDA path (compile-only, no kernel execution)
        run: |
          nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread -DMATRIX_USE_CUDA \
              main.cpp -o matrixdb_proto

  docker-build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Build the image
        run: docker build -t matrixdb .
      - name: Smoke-test the container (dev mode, no auth)
        run: |
          docker run --rm -d --name mdb -p 7070:7070 matrixdb
          sleep 2
          docker logs mdb 2>&1 | tee /tmp/mdb.log
          grep -q "serving on port 7070 (no auth" /tmp/mdb.log
          docker stop mdb
```

This mirrors, line for line, the commands already proven in this repo: `build_all.sh`/`run_tests.sh`
(used identically in Tasks 2/6 of the Leg 1 packaging plan), the `nvcc` invocation already documented
in `README.md`, and `docker build`/`run` per the `Dockerfile`'s own header comment. The grep target
(`serving on port 7070 (no auth`) matches `matrixdbd.cpp`'s dev-mode startup message (`matrixdbd:
serving on port 7070 (no auth — dev mode)`, printed to stderr) — the pattern deliberately stops before
the em dash to avoid any shell/encoding mismatch on that character.

- [ ] **Step 2: Validate the YAML parses**

This sandbox has no `pyyaml` and no `actionlint`, but `ruby` (with its YAML stdlib) is present —
use it as a lazy syntax check:

Run: `ruby -ryaml -e "YAML.load_file('.github/workflows/ci.yml'); puts 'YAML OK'"`
Expected: `YAML OK` (no exception).

- [ ] **Step 3: Confirm structure by inspection**

Run: `grep -nE "^  [a-z-]+:$|runs-on:|^on:|branches:" .github/workflows/ci.yml`
(Use `-E`/`+` rather than a bracket-expression-plus-`*`: BSD `grep` on macOS doesn't expand `*` after
`[a-z-]` the way GNU `grep` does, so the non-`-E` form silently under-matches on macOS.)
Expected output shows all four job names (`test-linux:`, `test-macos:`, `cuda-compile-check:`,
`docker-build:`), the `on:` trigger, and `branches: [main]` — a quick visual confirmation nothing
was dropped or misindented.

- [ ] **Step 4: Confirm the embedded commands still match what's proven to work locally**

Run: `./build_all.sh && ./run_tests.sh`
Expected: `Built matrixdb and matrixdbd (v0.1.0, CXX=...)` followed by `ALL GREEN (66 tests + oracle)`
— the exact commands the `test-linux`/`test-macos` jobs run, confirmed still green right before this
commit (this doesn't run the workflow itself — GitHub Actions isn't reachable from here — it
confirms the commands the workflow *calls* still work, which is what's actually within this
sandbox's power to verify).

Then clean up the binaries this produced (they're build output, not source):
Run: `rm -f matrixdb matrixdbd`

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "feat(ci): add GitHub Actions build matrix — Linux/macOS/CUDA-compile/Docker-build (BP-4)"
```

---

### Task 2: Update the gap register

**Files:**
- Modify: `PRODUCTION_READINESS.md` (§9 "Build, packaging & deployment" and §8 "Correctness,
  testing & CI")

- [ ] **Step 1: Mark BP-4 landed in the §9 table**

Find this exact line (currently in §9's table):
```
| BP-4 | No multi-platform build matrix (Apple ARM / Linux x86 / CUDA) | "Works on my Mac" only | P1 | M | needs CI |
```
Replace it with:
```
| BP-4 | No multi-platform build matrix (Apple ARM / Linux x86 / CUDA) | "Works on my Mac" only **[FIXED — BP-4: .github/workflows/ci.yml]** | P1 | M | partial |
```
(The `Local?` column changes from `needs CI` to `partial`: the workflow itself is now authored and
committed — that part no longer "needs CI" to exist — but confirming a run is actually green is a
real push to GitHub, i.e. host-only, same category as this project's other partial/host-verification
items.)

- [ ] **Step 2: Add a landed-note paragraph for BP-4 in §9**

Insert this new paragraph immediately after the existing `*BP-1 & BP-2 landed (build system +
packaging, CPU): ...*` paragraph in §9 (i.e., as the next paragraph, still before the
`## 10. Reliability & HA` heading):

```markdown
*BP-4 landed (CI build matrix): `.github/workflows/ci.yml` — four jobs, triggered on every push to
`main` (this repo has no PR workflow, so there's nothing to gate; the workflow is informational,
flagging a bad commit fast). `test-linux` (`ubuntu-latest`) and `test-macos` (`macos-14`, native
Apple Silicon) each run `./build_all.sh` then `./run_tests.sh` — the exact local gate, now on two
real platforms instead of whichever machine a human happened to use. `cuda-compile-check`
(`ubuntu-latest` + the `nvidia-cuda-toolkit` apt package) runs the same `nvcc` command already
documented in `README.md` — compile-only, no GPU on this runner, but a *real* `nvcc` compile
(upgrading the old clang-only "host-syntax probe"; this project's own history shows that distinction
matters — two `nvcc`-only `atomicAdd(double*)` arch-guard bugs once slipped past a clang-only local
build). `docker-build` (`ubuntu-latest`, Docker preinstalled) runs `docker build` then a log-based
smoke test (`docker run` the image, grep its stderr for the dev-mode startup line, stop it) — the
first time the `Dockerfile` and `matrixdbd`'s bind/listen path are exercised end-to-end on a real
host instead of reviewed by inspection (this sandbox has no `docker`/`nvcc`/GitHub Actions access, so
the actual green run is a manual host-verification step, same status as `matrixdbd`'s own `bind()`).
Deferred: Windows (no existing build convention targets it), branch protection (no PR flow to attach
one to), a README status badge (cosmetic), and closing QA-6 fully (GPU kernel *execution* in CI still
needs a paid/self-hosted GPU runner — the compile-check upgrade is real progress on QA-6 but doesn't
close it; see QA-6's own note in §8).*
```

- [ ] **Step 3: Add a QA-1 addendum**

Find the existing QA-1 paragraph in §8 (starts with `*QA-1 partial (local CI gate landed): ...`, ends
with `...The CPU suite is now 34 tests (QA-2 substantially improved from "a few oracle checks").*`).

Append this sentence right before the closing `*` of that paragraph (i.e., insert it as the new last
sentence, keeping everything before it unchanged):

```
BP-4 (2026-06-30) automated this same gate in CI (`.github/workflows/ci.yml`) on Linux x86 + macOS
ARM on every push to `main`, plus a real `nvcc` compile-check and a `docker build`/`run` smoke test —
see BP-4's own note in §9 for specifics; QA-6 (GPU kernel execution in CI) remains open.
```

- [ ] **Step 4: Mark QA-6 partial (not fixed) in the §8 table**

Find this exact line (currently in §8's table):
```
| QA-6 | CUDA path has no automated test — host-syntax probe + manual runs only | GPU regressions only caught by hand | P1 | M | needs GPU CI |
```
Replace it with:
```
| QA-6 | CUDA path has no automated test — host-syntax probe + manual runs only | GPU regressions only caught by hand **[partial — BP-4 CI added a real `nvcc` compile-check; kernel execution still needs a GPU runner]** | P1 | M | needs GPU CI |
```
(`Local?` stays `needs GPU CI` — unchanged, because the part that's still missing genuinely needs a
GPU runner; only the `Why` cell gets the honest partial-progress note.)

- [ ] **Step 5: Verify**

Run: `grep -n "BP-4\|QA-6\|QA-1 partial" PRODUCTION_READINESS.md`
Expected: the BP-4 row shows the `[FIXED — BP-4: ...]` marker and `partial` in the last column; the
QA-6 row shows the new `[partial — BP-4 CI added...]` marker and unchanged `needs GPU CI`; the QA-1
paragraph's new closing sentence appears.

Run: `sed -n '262,300p' PRODUCTION_READINESS.md` (or read that line range) to visually confirm both
sections still read correctly — valid table rows (same column count as their neighbors), the new
BP-4 paragraph placed correctly, nothing malformed.

- [ ] **Step 6: Commit**

```bash
git add PRODUCTION_READINESS.md
git commit -m "docs(readiness): mark BP-4 landed, QA-6 partial — CI build matrix"
```

---

### Task 3: Final verification pass

**Files:** none (verification only)

- [ ] **Step 1: Re-run the full local test suite**

Run: `./run_tests.sh`
Expected: `ALL GREEN (66 tests + oracle)`, exit code 0 — unchanged from before this plan (no test
file or production code touched by this plan; only a new CI workflow file and two doc edits).

- [ ] **Step 2: Re-run build_all.sh and clean up**

Run: `./build_all.sh && ./matrixdb -f examples/tour.sql && rm -f matrixdb matrixdbd examples/tour.db`
Expected: both binaries build, the tour output contains `2575`, then the build artifacts are removed.

- [ ] **Step 3: Re-validate the workflow YAML one more time**

Run: `ruby -ryaml -e "YAML.load_file('.github/workflows/ci.yml'); puts 'YAML OK'"`
Expected: `YAML OK`.

- [ ] **Step 4: Confirm a clean working tree**

Run: `git status --short`
Expected: empty output — no stray build artifacts, no uncommitted changes.

- [ ] **Step 5: Confirm the commit sequence**

Run: `git log --oneline -5`
Expected: the two commits from Tasks 1-2 (`feat(ci): add GitHub Actions build matrix...` and
`docs(readiness): mark BP-4 landed, QA-6 partial...`) appear, most recent first.

No commit for this task — it's verification-only, confirming Tasks 1-2 left the repo in a clean,
consistent state. (The actual GitHub Actions run itself — confirming all four jobs go green on a
real push — remains a manual step for the user on a real host with GitHub Actions access, exactly
like this project's other HOST-ONLY items. Note this plainly rather than claiming a green CI run
that hasn't actually happened.)

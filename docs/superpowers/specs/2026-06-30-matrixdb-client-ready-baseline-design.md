# matrixdb Client-Ready Baseline (single-node + CLI) — Design

**Date:** 2026-06-30  **Status:** approved direction (target #1 of the client-model fork). The next *phase*:
hardening, not features.
**Goal:** Make the single-node engine + `matrixdb` CLI safe and pleasant for someone who isn't us to depend
on — protect their data, never crash on their input, and let them build/learn it. CPU, local; the networked
path (TLS/bind) stays gated/deferred, the research artifact comes mostly for free.

## Why now

The analytical surface is complete; more features are diminishing returns. What's missing is the
*client-facing contract*: durability across crashes, robustness to arbitrary input, a build/install story,
and honest docs + limits. The engine already has WAL/checkpoint/fault-injection tests and `test_fuzz`; this
phase extends that discipline to the **data format** and the **CLI surface** a client actually touches.

## The baseline (prioritized)

**A. Durable, fail-soft data format (data safety) — HIGHEST.**
- **Atomic `.save`:** `save_catalog` writes to `path.tmp` then `rename()`s onto `path` (POSIX-atomic on one
  filesystem). A crash mid-write leaves the original `path` intact, never a half-written file. (Backup gets
  this for free.)
- **No process abort on a bad file:** `save_catalog`/`load_catalog` return `bool` instead of `std::abort()`
  on I/O / corrupt / wrong-magic / short-read. The CLI `.save`/`.open` check the result → one `Error:` line.
  (The `'MCA2'` magic already acts as a format version — a future format bumps it and old files are *rejected
  cleanly*, no longer abort the process.)

**B. Never crash on any input (stability) — HIGH.**
- Extend fuzzing to the **CLI**: drive `matrix_repl` with random/malformed dot-commands, queries, and CSV
  paths; assert it always returns 0 and never aborts/UB. Fix anything found. (A new `test_cli_fuzz.cpp`,
  ASan/UBSan.) This is the "a client's weird input won't kill the tool" guarantee.

**C. Build + install (adoption) — MEDIUM.**
- A one-command build → a `matrixdb` binary, documented; a tiny `build.sh` (clang++ or g++ fallback, matching
  `run_tests.sh`'s compiler pick). No cmake (per project convention).

**D. User guide (adoption) — MEDIUM.**
- A focused `docs/USAGE.md`: install → load CSV → query (with the full grammar) → save/open → output modes →
  known limits. The reference a new user reads instead of the source.

**E. Honest limits + perf (expectations) — MEDIUM.**
- A "Limits & performance" section (in USAGE.md or README): single-writer, in-memory working set, no
  RFC-4180 CSV quoting, analytical-subset SQL, gated network/TLS — plus a characterization of load + query
  throughput on a realistic dataset (reuse the bench harness).

## Order & verification

A → B → C → D → E. Each lands behind `./run_tests.sh` (+ `SAN=1`) green, committed. A and B are the
substance (data + stability); C–E make it usable and honestly described. Every change is additive or
hardening — no behavior change for valid existing use (the snapshot round-trip tests must stay green).

## Non-goals (this phase)

Networked server / TLS / multi-client writes (gated or contra-thesis); a SQL planner; new query features;
RFC-4180 CSV; data-format *migration* (we reject incompatible, not migrate — a later concern); backup/restore
returning status (follow-up — `save_catalog` atomicity already makes backup crash-safe).

## Success criteria

A client can: build `matrixdb` from one command, follow USAGE.md to load + query + persist their data, trust
that a crash won't corrupt a saved DB and that bad input yields an error (never a crash), and know the
system's limits up front. Suite + oracle + demo green under ASan/UBSan throughout.

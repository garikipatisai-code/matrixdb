# Design: Per-Query Latency Metrics (OB-2)

**Status:** approved-by-standing-directive (continue all phases, don't wait). **Date:** 2026-06-27.
**Builds on:** OB-1 (`EngineStats`/`stats()`), QRY-1 (`execute_query`).
**Fully local.**

**Thesis:** *Observability (OB-1) exposes tiering counters but not query latency — the #1 operational
metric for any database. Add per-query latency to `EngineStats`: count, total nanoseconds, and max, by
timing `execute_query`. Implemented as a thin public wrapper around a renamed `execute_query_impl`, so
ALL return paths (OK and every ERR) are timed at one point with zero caller churn. Oracle-safe:
`execute_query` is the catalog query API, not the id-0 benchmark/oracle scan, so timing it touches no
measured/hot path.*

---

## 1. Scope

**IN (`compute_mock.cpp` + new `test_query_latency.cpp`):**
- `EngineStats` gains three fields (appended): `uint64_t query_count; uint64_t total_query_ns; uint64_t max_query_ns;`
- Members: `uint64_t query_count_ = 0, total_query_ns_ = 0, max_query_ns_ = 0;`
- Rename the current `execute_query(q, out)` body to a PRIVATE `MatrixQueryStatus execute_query_impl(const MatrixQuery& q, std::vector<uint64_t>& out)` (unchanged logic). The PUBLIC `execute_query(q, out)` (same signature) becomes a timing wrapper:
  ```cpp
  MatrixQueryStatus execute_query(const MatrixQuery& q, std::vector<uint64_t>& out) {
      const auto t0 = std::chrono::steady_clock::now();
      const MatrixQueryStatus st = execute_query_impl(q, out);
      const uint64_t ns = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
      ++query_count_; total_query_ns_ += ns; if (ns > max_query_ns_) max_query_ns_ = ns;
      return st;
  }
  ```
- `stats()` appends `query_count_, total_query_ns_, max_query_ns_` to the returned `EngineStats`.
- `analytical_query_demo()` in `main.cpp` prints the new metrics (e.g. `queries=… mean=…µs max=…µs`) alongside the existing stats — making query latency visible in the running binary (OB-1 pattern). [Optional/low-risk; include for visibility.]

**Invariants:** every `execute_query` call (OK or ERR) increments `query_count` and contributes its
latency. `<chrono>` is already included. Existing callers (server, tests, demo) call the public
`execute_query` unchanged. The id-0 scan / `execute_scan` / benchmark / oracle are untouched → 83886070
unchanged. `EngineStats`'s new fields are appended, so `stats()` is the only constructor and OB-1
readers (named-field access) are unaffected.

**OUT (later):** a latency histogram / percentiles (count+total+max gives count, mean, max — p50/p99
need buckets, OB-2b); per-op-kind breakdown; metrics export (Prometheus-style); leveled logging.

---

## 2. Verification (`test_query_latency.cpp`, CPU)

- Load a sizable catalog column (e.g. 200k uint32) so each query does real, > 0 ns work.
- Run a known mix of `execute_query` calls — e.g. 5 OK scalar/grouped queries + 2 that return ERR
  (unknown column, invalid group). Assert:
  - `stats().query_count == 7` (every call counted, OK and ERR) — the exact, deterministic guard.
  - `stats().total_query_ns > 0` and `stats().max_query_ns > 0` (real work took measurable time).
  - `stats().max_query_ns <= stats().total_query_ns` (max is one sample of the sum).
- **Non-vacuity**: `query_count` rises by exactly the number of calls — a no-op timer would leave it 0;
  an ERR query still counts (proving all paths are timed, not just OK).

Plus: full CPU suite (now 30 tests) + oracle `83886070`; `test_observability` (OB-1 — reads the existing
`EngineStats` fields by name, unaffected by appended fields), `test_query`, `test_server` pass; notebook
regenerated.

---

## 3. Open / deferred
Latency histogram / p50-p99 (OB-2b); per-op-kind latency; metrics export; the same treatment for the
point-op path (`kv_get`/commit) if a server loop later needs it.

# Concurrent Serving (single-writer / many-readers) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let multiple analytical reads execute concurrently on the CPU engine while writes serialize, via a `std::shared_mutex`, with every change verified under ThreadSanitizer and the existing oracle.

**Architecture:** One engine-level `std::shared_mutex`: reads take it shared, writes exclusive — so while any reader holds it, no writer runs and the catalog/tiers/store are immutable (race-free by construction). A new, **purely additive** `execute_query_shared` runs a read over HOST-resident columns with no tier side-effects (no borrow/heat/rebalance); a query needing a COLD/DEVICE borrow returns `NEEDS_EXCLUSIVE` and the dispatcher re-runs it under the exclusive lock. The read-touched latency/count stats become atomic. Existing single-threaded entry points stay byte-identical.

**Tech Stack:** C++20, `std::shared_mutex`/`std::atomic`/`std::thread`, the existing `matrix_serve` wire protocol + `socketpair` test harness, ThreadSanitizer (`SAN=1`/clang `-fsanitize=thread`).

**Spec:** `docs/superpowers/specs/2026-06-29-concurrent-serving-design.md`

---

### Task 1: Atomic read-path stats (enabling refactor, behavior-preserving)

The `execute_query` timing wrapper bumps `query_count_ / total_query_ns_ / max_query_ns_ / latency_hist_` on **every** query; once reads run concurrently these are data races. Make exactly those members atomic; everything else (incl. `scans_since_rebalance_`, `cold_borrows_`, `rebalances_`) is touched only under the exclusive path and stays plain. `EngineStats` (returned by value) and `query_latency_histogram()` (returned by value) stay plain — the getters `.load()` into them.

**Files:**
- Modify: `compute_mock.cpp` — member decls (~1819–1835 area), the `execute_query` wrapper (~995–1003), `stats()` (~435), `query_latency_histogram()` (~451), the percentile reader (~455–462).

- [ ] **Step 1: Change the four members to atomics.**

```cpp
std::atomic<uint64_t> query_count_{0};
std::atomic<uint64_t> total_query_ns_{0};
std::atomic<uint64_t> max_query_ns_{0};
std::array<std::atomic<uint64_t>, LAT_BUCKETS> latency_hist_{};   // was std::array<uint64_t, 40>
```

- [ ] **Step 2: Update the wrapper to use atomic ops (relaxed — stats are heuristic).**

```cpp
query_count_.fetch_add(1, std::memory_order_relaxed);
total_query_ns_.fetch_add(ns, std::memory_order_relaxed);
for (uint64_t cur = max_query_ns_.load(std::memory_order_relaxed); ns > cur &&
     !max_query_ns_.compare_exchange_weak(cur, ns, std::memory_order_relaxed); ) {}
{ uint64_t x = ns + 1, b = 0; while (x > 1 && b < LAT_BUCKETS - 1) { x >>= 1; ++b; } latency_hist_[b].fetch_add(1, std::memory_order_relaxed); }
```

- [ ] **Step 3:** Update `stats()` to `query_count_.load()`, `total_query_ns_.load()`, `max_query_ns_.load()`; `query_latency_histogram()` copies each bucket via `.load()` into a plain `std::array<uint64_t, LAT_BUCKETS>`; the percentile reader loads each bucket. (Search for every bare use of these four names and add `.load(...)`.)

- [ ] **Step 4: Verify behavior-preserving.** `./run_tests.sh` → `ALL GREEN (63 tests + oracle)`; `SAN=1 ./run_tests.sh` → green. Single-threaded values are identical (atomics with relaxed ops hold the same counts).

- [ ] **Step 5: Commit** — `git commit -am "refactor(engine): atomic read-path stats counters (enables concurrent reads)"`

---

### Task 2: `execute_query_shared` + `ReadOutcome` (additive, self-contained, no side effects)

A read fast path that runs only when every referenced column is HOST-resident, with **no** tier mutation (no borrow/`record_access`/`maybe_rebalance`). It reuses the existing `matrix_cpu_*` reducers (the semantics-of-record) directly over `host_ptr()`, mirroring `execute_query_impl`'s dispatch for the supported shapes (scalar + grouped, u32/i64/f64, filtered/unfiltered, scalar cross-column). A query that needs a borrow, or any shape it doesn't cover, returns `NEEDS_EXCLUSIVE` having mutated nothing.

**Files:**
- Modify: `compute_mock.cpp` — add `enum class ReadOutcome { SERVED, NEEDS_EXCLUSIVE };`, a private `bool all_referenced_host(const MatrixQuery&) const`, and the public `ReadOutcome execute_query_shared(const MatrixQuery& q, std::vector<uint64_t>& out)` (placed next to `execute_query`); reuse the atomic timing block from Task 1 (extract it into `private: void record_query_latency(uint64_t ns)`).
- Test: `test_concurrent_serving.cpp` (new).

- [ ] **Step 1: Write the failing test (single-threaded semantics).**

```cpp
// test_concurrent_serving.cpp
#include "compute_mock.cpp"
#include <cassert>
#include <cstdio>
#include <vector>
int main() {
    CPUMockEngine eng;
    const size_t N = 1u << 16;
    std::vector<uint32_t> v(N); for (size_t i=0;i<N;++i) v[i]=static_cast<uint32_t>(i%1000);
    eng.load_scan_column(1, v.data(), N);                       // HOST-resident

    // HOST column: shared path SERVED and equals execute_query
    MatrixQuery q; q.value_col=1; q.agg=AGG_SUM; q.has_filter=true; q.cmp=MatrixCmp::GT; q.threshold=500;
    std::vector<uint64_t> a, b;
    assert(eng.execute_query_shared(q, a) == CPUMockEngine::ReadOutcome::SERVED);
    assert(eng.execute_query(q, b) == MatrixQueryStatus::OK && a == b);

    // COLD column: shared path refuses without mutating (tier unchanged, no borrow)
    CPUMockEngine eng2(0, "", /*host_cap=*/N*sizeof(uint32_t)/2);   // budget < column => can be demoted
    eng2.load_scan_column(1, v.data(), N);
    eng2.load_scan_column(2, v.data(), N);                          // pressure: forces one COLD on rebalance
    for (int k=0;k<32;++k){ std::vector<uint64_t> t; eng2.execute_query(MatrixQuery{.value_col=2,.agg=AGG_COUNT}, t); }
    // whichever of 1/2 is now COLD, the shared path must refuse it:
    const uint64_t cold = (eng2.column_tier(1)!=MemorySpace::HOST)?1:2;
    if (eng2.column_tier(cold)!=MemorySpace::HOST) {
        const MemorySpace before = eng2.column_tier(cold);
        std::vector<uint64_t> t;
        assert(eng2.execute_query_shared(MatrixQuery{.value_col=cold,.agg=AGG_COUNT}, t) == CPUMockEngine::ReadOutcome::NEEDS_EXCLUSIVE);
        assert(t.empty() && eng2.column_tier(cold)==before && "no mutation / no borrow on refuse");
    }
    std::printf("[shared semantics] ok\n");
    std::printf("ALL CONCURRENT-SERVING TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 2: Run, expect FAIL** (`execute_query_shared` / `ReadOutcome` undefined): `clang++ -std=c++20 -O2 test_concurrent_serving.cpp -o /tmp/tcs && /tmp/tcs` → compile error.

- [ ] **Step 3: Implement.** `all_referenced_host(q)`: return true iff `q.value_col` is HOST and (if `q.grouped`) `q.key_col` is HOST and (if `q.has_filter && q.filter_col`) `q.filter_col` is HOST — via `catalog_.at(id)->tier() == MemorySpace::HOST` (the columns exist; validate first, else `NEEDS_EXCLUSIVE`). `execute_query_shared`: time the call (`record_query_latency`); if not `all_referenced_host` → return `NEEDS_EXCLUSIVE` (out untouched); else dispatch like `execute_query_impl` but read `host_ptr()` directly and call the matching `matrix_cpu_reduce_*` / `matrix_cpu_group_reduce_*` / `matrix_cpu_reduce_filtered_by*` reducer — **no** `record_access`, **no** `maybe_rebalance`, **no** `migrate_to`. Null-masked columns and grouped cross-column → `NEEDS_EXCLUSIVE` (let the exclusive path handle them in v1).

- [ ] **Step 4: Run, expect PASS** → `[shared semantics] ok`.

- [ ] **Step 5: Verify no regression.** `./run_tests.sh` (64 tests now) + `SAN=1 ./run_tests.sh` green.

- [ ] **Step 6: Commit** — `git commit -am "feat(engine): execute_query_shared — side-effect-free read fast path + ReadOutcome"`

---

### Task 3: Concurrent readers are race-free (TSan gate)

Proves Task 1+2: many threads running `execute_query_shared` over HOST columns concurrently produce oracle-correct results with no data races — no lock needed yet, because the path is pure.

**Files:** Modify: `test_concurrent_serving.cpp`.

- [ ] **Step 1: Add the concurrent-readers test.**

```cpp
#include <thread>
// ... after the single-threaded checks, before the final printf:
{
    CPUMockEngine eng3;
    const size_t M = 1u << 20;
    std::vector<uint32_t> w(M); for (size_t i=0;i<M;++i) w[i]=static_cast<uint32_t>(i%1000);
    eng3.load_scan_column(1, w.data(), M);
    uint64_t oracle=0; for (uint32_t e: w) if (e>500) oracle+=e;       // SUM WHERE >500
    std::atomic<int> bad{0};
    auto reader = [&]{
        for (int r=0;r<200;++r){
            MatrixQuery q; q.value_col=1; q.agg=AGG_SUM; q.has_filter=true; q.cmp=MatrixCmp::GT; q.threshold=500;
            std::vector<uint64_t> o;
            if (eng3.execute_query_shared(q,o)!=CPUMockEngine::ReadOutcome::SERVED || o.size()!=1 || o[0]!=oracle) ++bad;
        }
    };
    std::vector<std::thread> ts; for (int t=0;t<8;++t) ts.emplace_back(reader);
    for (auto& t: ts) t.join();
    assert(bad.load()==0 && "concurrent readers all oracle-correct");
    std::printf("[concurrent readers] ok (8 threads x 200 queries)\n");
}
```

- [ ] **Step 2: Run under TSan, expect PASS, zero races:**
`clang++ -std=c++20 -O1 -fsanitize=thread -pthread test_concurrent_serving.cpp -o /tmp/tcs_tsan && /tmp/tcs_tsan` → `[concurrent readers] ok`, no ThreadSanitizer report.

- [ ] **Step 3: Add `-pthread` for this test in `run_tests.sh`** (it already adds `-pthread` to FLAGS for `test_client`; confirm `test_concurrent_serving` links threads). Verify `./run_tests.sh` green.

- [ ] **Step 4: Commit** — `git commit -am "test(engine): concurrent readers race-free under TSan (8 threads)"`

---

### Task 4: `shared_mutex` dispatch + `ConcurrentServer` + escalation

Add the lock and a server that applies it at the request boundary, reusing `matrix_serve` for writes/escalation and `execute_query_shared` for the read fast path.

**Files:**
- Create: `concurrent_server.hpp` — `ConcurrentServer` (owns `std::shared_mutex mu_`, refs to engine + `AccessPolicy`); `serve(req_bytes) -> resp_bytes`.
- Modify: `server.hpp` — extract `matrix_serialize_query_response(MatrixQueryStatus, const std::vector<uint64_t>& out) -> std::vector<uint8_t>` from the `ReqKind::QUERY` case (~182) and call it from both places.
- Test: `test_concurrent_serving.cpp`.

- [ ] **Step 1: Extract the query-response serializer** in `server.hpp` (no behavior change; `matrix_serve`'s QUERY case now calls it). Verify `./run_tests.sh` (test_server) green.

- [ ] **Step 2: Write `ConcurrentServer::serve`.**

```cpp
// concurrent_server.hpp
#pragma once
#include "server.hpp"
#include <shared_mutex>
class ConcurrentServer {
public:
    ConcurrentServer(CPUMockEngine& e, const AccessPolicy& p, uint64_t principal=0)
        : eng_(e), policy_(p), principal_(principal) {}
    std::vector<uint8_t> serve(const std::vector<uint8_t>& req_bytes) {
        MatrixRequest req;
        if (!matrix_deserialize_request(req_bytes, req)) {                 // malformed: let matrix_serve answer
            std::unique_lock<std::shared_mutex> w(mu_); return matrix_serve(eng_, policy_, principal_, req_bytes);
        }
        if (req.kind == ReqKind::QUERY) {
            {   std::shared_lock<std::shared_mutex> r(mu_);
                std::vector<uint64_t> out;
                if (eng_.execute_query_shared(req.query, out) == CPUMockEngine::ReadOutcome::SERVED)
                    return matrix_serialize_query_response(MatrixQueryStatus::OK, out);
            }   // not HOST-resident -> escalate
            std::unique_lock<std::shared_mutex> w(mu_); return matrix_serve(eng_, policy_, principal_, req_bytes);
        }
        if (req.kind == ReqKind::PUT) { std::unique_lock<std::shared_mutex> w(mu_); return matrix_serve(eng_, policy_, principal_, req_bytes); }
        std::shared_lock<std::shared_mutex> r(mu_);                          // GET/HEALTH/STATS
        return matrix_serve(eng_, policy_, principal_, req_bytes);
    }
private:
    CPUMockEngine& eng_; const AccessPolicy& policy_; uint64_t principal_;
    std::shared_mutex mu_;
};
```
(Note: QUERY access-control was checked inside `matrix_serve`; for the shared fast path, also gate on `policy_.can_query(principal_, req.query.value_col)` before `execute_query_shared` — return the FORBIDDEN response via a small helper or fall through to `matrix_serve` under shared. Implement the gate to match `matrix_serve`'s `ERR_FORBIDDEN`.)

- [ ] **Step 3: Write the mixed read/write concurrency test** (readers + a writer through `ConcurrentServer`, over framed request bytes; correctness + exclusivity; COLD escalation). Use `matrix_serialize_request` to build requests; assert decoded responses match a direct engine oracle; run reader threads concurrent with a PUT-issuing thread.

- [ ] **Step 4: Run under TSan**, expect PASS + zero races: `clang++ -std=c++20 -O1 -fsanitize=thread -pthread test_concurrent_serving.cpp -o /tmp/tcs_tsan && /tmp/tcs_tsan`.

- [ ] **Step 5: Verify** `./run_tests.sh` + `SAN=1 ./run_tests.sh` green; add `concurrent_server.hpp` to `make_notebook.py` SOURCES + a notebook cell for `test_concurrent_serving.cpp`; regenerate.

- [ ] **Step 6: Commit** — `git commit -am "feat(server): ConcurrentServer — shared/exclusive dispatch + read escalation (TSan-verified)"`

---

### Task 5: Stress, throughput sanity, docs + memory

- [ ] **Step 1: Mixed read/write stress under TSan.** Add a stress block: 8 reader threads + 2 writer threads (PUT/load) through `ConcurrentServer` for a few seconds of iterations, every read result oracle-checked, writer exclusivity holds; run under TSan, zero races. Verify.

- [ ] **Step 2: Throughput sanity (informational, not a hard gate).** Time T queries on 1 thread vs N threads over HOST columns; print the ratio; assert `N-thread >= 1.5x 1-thread` on a multicore host (skip/relax the assert if `std::thread::hardware_concurrency() < 4`).

- [ ] **Step 3: Update docs** — `README.md` (Known limits: serve loop is no longer single-request; note single-writer/many-readers + the COLD-escalation / pure-read-no-rebalance / writer-starvation v1 limits); `PRODUCTION_READINESS.md` (NW-2 concurrent serving: landed for reads).

- [ ] **Step 4: Update memory** — `matrixdb-state.md`: concurrent serving (single-writer/many-readers) landed; note deferred (epoch/snapshot reclamation, striped locks, GPU concurrency, read-path heat, wire txns).

- [ ] **Step 5: Commit** — `git commit -am "feat(server): concurrent-serving stress + throughput; docs + memory"`

---

## Self-Review

**Spec coverage:** invariant + shared/exclusive (Task 4) ✓; atomic read-path counters (Task 1) ✓; rebalance off the shared path — covered by `execute_query_shared` having no `maybe_rebalance` (Task 2) ✓; borrow→escalation via `NEEDS_EXCLUSIVE` (Tasks 2, 4) ✓; `matrix_serve_pool`/threading — realized as `ConcurrentServer` + the threaded tests (Task 4; the bind() accept-loop stays compile-only as the spec notes — a thin `matrix_serve_pool` wrapper over `ConcurrentServer::serve` is optional and added in Task 4 Step 2 if trivial) ✓; tests a–e (Tasks 3,4,5) ✓; scope/non-goals honored (no epoch/striped/GPU/txn) ✓.

**Gap fixed:** spec §"heat → atomics" is **descoped** in this plan — `execute_query_shared` skips `record_access` entirely (pure reads don't accrue tiering heat in v1) rather than making the in-map `recent_bytes` atomic, which would force `TierManager::Column` to become non-copyable. This is lower-risk and additive; recorded as a deferred follow-up in Task 5 Step 4. (Single change vs the spec; intentional and documented.)

**Placeholder scan:** none — every step has concrete code/commands.

**Type consistency:** `ReadOutcome` (Task 2) used identically in Tasks 3–4; `execute_query_shared` signature stable; `matrix_serialize_query_response` defined in Task 4 Step 1 before use in Step 2.

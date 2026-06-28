# Per-Query Latency Metrics Implementation Plan (OB-2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Add per-query latency (count / total ns / max ns) to `EngineStats`, via a timing wrapper around a renamed `execute_query_impl` — all return paths timed, zero caller churn, oracle-safe.

**Spec:** `docs/superpowers/specs/2026-06-27-query-latency-metrics-design.md`

---

### Task 1: EngineStats latency fields + execute_query timing wrapper + test

**Files:** Modify `compute_mock.cpp`, `main.cpp`; Create `test_query_latency.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_query_latency.cpp`:

```cpp
// CPU test for per-query latency metrics (OB-2): execute_query records count/total_ns/max_ns in EngineStats.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

static void test_query_latency() {
    std::vector<uint32_t> v(200000);
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<uint32_t>(i);
    CPUMockEngine eng;
    eng.load_scan_column(2, v.data(), v.size());

    // 5 OK queries (real work on 200k rows) + 2 that return ERR — all must be counted/timed.
    for (int i = 0; i < 5; ++i) {
        MatrixQuery q{}; q.value_col = 2; q.agg = AGG_SUM; q.has_filter = true; q.threshold = 100;
        std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::OK);
    }
    { MatrixQuery q{}; q.value_col = 999; q.agg = AGG_COUNT; std::vector<uint64_t> o;   // unknown column
      assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_UNKNOWN_COLUMN); }
    { MatrixQuery q{}; q.value_col = 2; q.key_col = 2; q.num_groups = 4; q.grouped = true; q.agg = AGG_COUNT;
      std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_INVALID_GROUP); } // key==value

    const EngineStats s = eng.stats();
    assert(s.query_count == 7 && "every execute_query call counted (OK and ERR)");
    assert(s.total_query_ns > 0 && "real queries took measurable time");
    assert(s.max_query_ns > 0 && s.max_query_ns <= s.total_query_ns && "max is one sample of the total");
    std::cout << "[query latency] ok (count=" << s.query_count
              << " mean_ns=" << (s.total_query_ns / s.query_count) << " max_ns=" << s.max_query_ns << ")\n";
}

int main() { test_query_latency(); std::cout << "ALL QUERY-LATENCY TESTS PASSED\n"; return 0; }
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_query_latency.cpp -o /tmp/tql && /tmp/tql` → FAIL to compile (`EngineStats` has no `query_count`/`total_query_ns`/`max_query_ns`).

- [ ] **Step 3: Extend EngineStats** — In `compute_mock.cpp`, add three fields to the END of `struct EngineStats` (after `cold_resident_bytes`):
```cpp
    uint64_t query_count;         // execute_query calls served (OK and ERR)
    uint64_t total_query_ns;      // summed execute_query wall-time (ns)
    uint64_t max_query_ns;        // slowest single execute_query (ns)
```

- [ ] **Step 4: Add the members + timing wrapper** — In `CPUMockEngine`:
  - Add members beside the other counters (e.g. near `scans_since_rebalance_`): `uint64_t query_count_ = 0; uint64_t total_query_ns_ = 0; uint64_t max_query_ns_ = 0;`
  - Rename the existing `MatrixQueryStatus execute_query(const MatrixQuery& q, std::vector<uint64_t>& out) { ... }` to a PRIVATE `MatrixQueryStatus execute_query_impl(const MatrixQuery& q, std::vector<uint64_t>& out) { ... }` (move it to the private section; body unchanged).
  - Add the PUBLIC timing wrapper (same name/signature, where `execute_query` was, in the public section) from spec §1:
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
  - Update `stats()` to append the three counters to the `EngineStats{...}` initializer (order matches the struct): `…, tier_mgr_.resident_bytes(MemorySpace::COLD), query_count_, total_query_ns_, max_query_ns_ };`

- [ ] **Step 5: main.cpp demo print** — In `analytical_query_demo()`, find where `stats()` is printed and add a latency line (mirror the existing stats-print style), e.g.:
```cpp
    std::cout << "  query latency: count=" << st.query_count
              << " mean=" << (st.query_count ? st.total_query_ns / st.query_count / 1000 : 0) << "us"
              << " max=" << st.max_query_ns / 1000 << "us\n";
```
(use the existing `EngineStats` local variable name in that function; keep it after the existing stats print. If the demo doesn't keep an `EngineStats` local, call `eng.stats()` for it.)

- [ ] **Step 6: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_query_latency.cpp -o /tmp/tql && /tmp/tql` → PASS: `[query latency] ok (...)`, `ALL QUERY-LATENCY TESTS PASSED`. Zero warnings.

- [ ] **Step 7: Confirm no regression** — `EngineStats` + `execute_query` + `main.cpp` changed; these MUST still pass unmodified:
  - `for t in test_observability test_query test_query_predicates test_query_validation test_typed_columns test_typed_double test_server test_audit; do clang++ -std=c++20 -O2 $t.cpp -o /tmp/$t 2>/dev/null && /tmp/$t | tail -1; done`
  - `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`. (Also eyeball that the demo prints a `query latency:` line.)
  If any differ, STOP / report BLOCKED.

- [ ] **Step 8: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add compute_mock.cpp main.cpp test_query_latency.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: per-query latency metrics (OB-2) — EngineStats count/total_ns/max_ns via execute_query timing wrapper"
```

---

### Task 2: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (30 tests).**
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by \
         test_query test_observability test_column_io test_catalog_snapshot test_query_validation \
         test_transactions test_server test_security test_audit test_csv_ingest test_checkpoint \
         test_query_predicates test_typed_columns test_typed_predicates test_typed_grouped \
         test_typed_snapshot test_typed_double test_typed_double_grouped test_typed_csv test_query_latency; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 30× `PASS:`. If any fail, STOP / report BLOCKED.

- [ ] **Step 2: Notebook** — add `"test_query_latency.cpp"` to `make_notebook.py` SOURCES right after `"test_typed_csv.cpp"`; add a run cell after the typed-CSV run cell (`test_typed_csv.cpp` → `/tmp/ttc`):
```python
    md("### Per-query latency metrics\n"
       "EngineStats now reports query_count / total_query_ns / max_query_ns — execute_query times every "
       "call (OK and error) so query latency (count, mean, max) is observable, the #1 DB ops metric."),
    code("!clang++ -std=c++20 -O2 test_query_latency.cpp -o /tmp/tql 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_query_latency.cpp -o /tmp/tql; /tmp/tql"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 49 source files embedded`. Verify `grep -o "test_query_latency.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed query-latency test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** EngineStats fields (§1)→T1S3; members + execute_query_impl rename + timing wrapper + stats() append (§1)→T1S4; demo print (§1)→T1S5; count==N (OK+ERR) + total/max > 0 + non-vacuity (§2)→T1S1; regression of OB-1/query/server + oracle (§2)→T1S7; suite+notebook→T2. ✓
**Placeholders:** none — wrapper body verbatim; the demo-print step references the existing stats-print site (the implementer locates it). **Type consistency:** `EngineStats.{query_count,total_query_ns,max_query_ns}` (uint64), members `query_count_/total_query_ns_/max_query_ns_`, `execute_query` (public wrapper, unchanged signature) → `execute_query_impl` (private, the old body). `stats()` appends the 3 in struct order. `<chrono>` already included. Oracle/id-0/benchmark untouched; `execute_query` is the catalog API only.

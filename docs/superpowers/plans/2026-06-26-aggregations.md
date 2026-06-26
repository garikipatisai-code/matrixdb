# Analytical Aggregations Implementation Plan — AGG-1

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Generalize OP_SCAN from `count(value>threshold)` to a filtered reduction — COUNT/SUM/MIN/MAX over the matching values — over both the legacy fixed column and the tiered catalog.

**Architecture:** A new `MatrixAggOp` enum + a `matrix_cpu_reduce` helper (one tight loop per op). OP_SCAN carries the op at payload offset 16 (default `AGG_COUNT=0` → unchanged behavior, oracle preserved). `execute_scan` and `scan_tiered_column` route through the reducer.

**Tech Stack:** C++20, header-only units, clang++/g++. Tests are standalone CPU executables.

**Spec:** `docs/superpowers/specs/2026-06-26-aggregations-design.md`

---

## File Structure
- **Modify `types.hpp`** — add `enum MatrixAggOp`.
- **Modify `compute.hpp`** — add `matrix_set/get_scan_agg_op` codec + `matrix_cpu_reduce`.
- **Modify `compute_mock.cpp`** — `execute_scan` + `scan_tiered_column` route through the reducer.
- **Create `test_aggregations.cpp`** — reducer/codec tests (Task 1) + through-engine tests (Task 2).
- **Modify `make_notebook.py` / regenerate `matrixdb_colab.ipynb`** — embed the new test.

---

### Task 1: MatrixAggOp enum + reducer + codec

**Files:** Modify `types.hpp`, `compute.hpp`; Create `test_aggregations.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_aggregations.cpp`:

```cpp
// CPU test for analytical aggregations. Grows across tasks; one main() runs all.
#include "compute_mock.cpp"   // CPUMockEngine + compute.hpp (codec + reducer)
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

static void test_reduce_closed_form() {
    const size_t N = 1000;
    std::vector<uint32_t> v(N);
    for (size_t i = 0; i < N; ++i) v[i] = static_cast<uint32_t>(i); // value[i]=i
    const uint32_t T = 600;
    const uint64_t count = N - 1 - T;            // # of i>T in [0,N)
    const uint64_t mn = T + 1;
    const uint64_t mx = N - 1;
    uint64_t sum = 0; for (uint64_t i = T + 1; i <= N - 1; ++i) sum += i;
    assert(matrix_cpu_reduce(v.data(), N, T, AGG_COUNT) == count);
    assert(matrix_cpu_reduce(v.data(), N, T, AGG_SUM)   == sum);
    assert(matrix_cpu_reduce(v.data(), N, T, AGG_MIN)   == mn);
    assert(matrix_cpu_reduce(v.data(), N, T, AGG_MAX)   == mx);
    assert(sum != count); // non-vacuity: a stub returning count for every op would fail here
    std::cout << "[reduce closed-form] ok\n";
}

static void test_reduce_empty() {
    const size_t N = 1000;
    std::vector<uint32_t> v(N);
    for (size_t i = 0; i < N; ++i) v[i] = static_cast<uint32_t>(i);
    const uint32_t T = N - 1; // nothing is > N-1
    assert(matrix_cpu_reduce(v.data(), N, T, AGG_COUNT) == 0);
    assert(matrix_cpu_reduce(v.data(), N, T, AGG_SUM)   == 0);
    assert(matrix_cpu_reduce(v.data(), N, T, AGG_MIN)   == UINT64_MAX);
    assert(matrix_cpu_reduce(v.data(), N, T, AGG_MAX)   == 0);
    std::cout << "[reduce empty-set] ok\n";
}

static void test_agg_codec() {
    DatabaseQuery q{};
    matrix_set_scan_target(q, 50u, 9ull);
    assert(matrix_get_scan_agg_op(q) == AGG_COUNT);  // default 0 when not set
    matrix_set_scan_agg_op(q, AGG_SUM);
    assert(matrix_get_scan_agg_op(q) == AGG_SUM);
    assert(matrix_get_scan_threshold(q) == 50u);     // not disturbed
    assert(matrix_get_scan_column_id(q) == 9ull);    // not disturbed
    std::cout << "[agg codec] ok\n";
}

int main() {
    test_reduce_closed_form();
    test_reduce_empty();
    test_agg_codec();
    std::cout << "ALL AGGREGATION TESTS PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_aggregations.cpp -o /tmp/tagg && /tmp/tagg` → FAIL to compile (`matrix_cpu_reduce`, `AGG_COUNT`, `matrix_set_scan_agg_op` undeclared).

- [ ] **Step 3: Add the MatrixAggOp enum** — In `types.hpp`, add right after the `MatrixOpcode` enum (after its closing `};`, ~line 47):

```cpp
// Aggregate reduction carried by OP_SCAN (payload offset 16). AGG_COUNT==0 is the default, so a
// scan with no agg op set counts matches (the original behavior). SUM/MIN/MAX reduce the values
// matching the predicate (value > threshold).
enum MatrixAggOp : uint32_t {
    AGG_COUNT = 0,
    AGG_SUM   = 1,
    AGG_MIN   = 2,
    AGG_MAX   = 3,
};
```

- [ ] **Step 4: Add the codec + reducer** — In `compute.hpp`, add right after `matrix_get_scan_threshold` (the end of the scan codec block, before `matrix_bin_by_page`):

```cpp
// OP_SCAN's aggregate op lives at payload offset 16 (u32). Default 0 == AGG_COUNT (the original
// count semantics), so a query built with only set_scan_target/set_scan_threshold reduces by COUNT.
inline void matrix_set_scan_agg_op(DatabaseQuery& q, MatrixAggOp op) {
    *reinterpret_cast<uint32_t*>(q.payload + 16) = static_cast<uint32_t>(op);
}
inline MatrixAggOp matrix_get_scan_agg_op(const DatabaseQuery& q) {
    return static_cast<MatrixAggOp>(*reinterpret_cast<const uint32_t*>(q.payload + 16));
}

// Filtered reduction over a uint32 column: reduce the values matching the predicate
// (value > threshold) by `op`. One tight loop per op (dispatch OUTSIDE the loop, so the COUNT
// loop stays exactly as fast as the original scan). Empty match-set sentinels: COUNT/SUM -> 0,
// MIN -> UINT64_MAX, MAX -> 0 (matched values are > threshold >= 0, i.e. >= 1, so 0 / UINT64_MAX
// unambiguously signal "no match"). SUM accumulates in u64 (no overflow for the engine's columns).
inline uint64_t matrix_cpu_reduce(const uint32_t* v, size_t n, uint32_t threshold, MatrixAggOp op) {
    switch (op) {
        case AGG_SUM: { uint64_t s = 0; for (size_t i = 0; i < n; ++i) if (v[i] > threshold) s += v[i]; return s; }
        case AGG_MIN: { uint64_t m = UINT64_MAX; for (size_t i = 0; i < n; ++i) if (v[i] > threshold && v[i] < m) m = v[i]; return m; }
        case AGG_MAX: { uint64_t m = 0; for (size_t i = 0; i < n; ++i) if (v[i] > threshold && v[i] > m) m = v[i]; return m; }
        case AGG_COUNT:
        default:      { uint64_t c = 0; for (size_t i = 0; i < n; ++i) c += (v[i] > threshold); return c; }
    }
}
```

(`compute.hpp` already includes `<cstdint>` for `UINT64_MAX` and `types.hpp` for `MatrixAggOp`/`DatabaseQuery`.)

- [ ] **Step 5: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_aggregations.cpp -o /tmp/tagg && /tmp/tagg` → PASS, prints the three `ok` lines + `ALL AGGREGATION TESTS PASSED`. No warnings.

- [ ] **Step 6: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add types.hpp compute.hpp test_aggregations.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: MatrixAggOp + matrix_cpu_reduce (COUNT/SUM/MIN/MAX) + agg-op codec"
```

---

### Task 2: Route the engine scan paths through the reducer

**Files:** Modify `compute_mock.cpp`; Modify `test_aggregations.cpp`.

- [ ] **Step 1: Write the failing test** — In `test_aggregations.cpp`, add above `main`:

```cpp
static void test_engine_agg_legacy() {
    CPUMockEngine eng(0);                          // legacy fixed column (id 0)
    const uint64_t SIZE = MATRIX_SCAN_COLUMN_SIZE;
    const uint32_t T = 8000000;                    // < SIZE
    auto run = [&](MatrixAggOp op) {
        DatabaseQuery q{}; matrix_set_scan_target(q, T, 0); matrix_set_scan_agg_op(q, op);
        eng.execute_scan(q); return q.transaction_id;
    };
    const uint64_t cnt = SIZE - 1 - T;
    assert(run(AGG_COUNT) == cnt);
    assert(run(AGG_MIN)   == static_cast<uint64_t>(T) + 1);
    assert(run(AGG_MAX)   == SIZE - 1);
    const uint64_t sum = (static_cast<uint64_t>(SIZE - 1) + (static_cast<uint64_t>(T) + 1)) * cnt / 2;
    assert(run(AGG_SUM)   == sum);
    std::cout << "[engine agg legacy] ok\n";
}

static void test_engine_agg_tiered() {
    const size_t N = 1000;
    std::vector<uint32_t> col(N);
    for (size_t i = 0; i < N; ++i) col[i] = static_cast<uint32_t>(i);
    CPUMockEngine eng(0, "", /*host_cap=*/SIZE_MAX);
    eng.load_scan_column(1, col.data(), N);
    const uint32_t T = 600;
    auto run = [&](MatrixAggOp op) {
        DatabaseQuery q{}; matrix_set_scan_target(q, T, 1); matrix_set_scan_agg_op(q, op);
        eng.execute_scan(q); return q.transaction_id;
    };
    const uint64_t cnt = N - 1 - T;
    assert(run(AGG_COUNT) == cnt);
    assert(run(AGG_MIN)   == static_cast<uint64_t>(T) + 1);
    assert(run(AGG_MAX)   == N - 1);
    uint64_t sum = 0; for (uint64_t i = T + 1; i <= N - 1; ++i) sum += i;
    assert(run(AGG_SUM)   == sum);
    std::cout << "[engine agg tiered] ok\n";
}
```

Add `test_engine_agg_legacy();` and `test_engine_agg_tiered();` to `main()` after `test_agg_codec();`.

- [ ] **Step 2: Run to verify it fails** — `clang++ -std=c++20 -O2 test_aggregations.cpp -o /tmp/tagg && /tmp/tagg` → FAIL: `execute_scan` ignores the agg op (always counts), so `run(AGG_SUM)`/`MIN`/`MAX` return the count, tripping the SUM/MIN/MAX asserts.

- [ ] **Step 3: Route execute_scan through the reducer** — In `compute_mock.cpp`, replace the body of `execute_scan` (the current id==0 count loop + the `scan_tiered_column(col_id, threshold)` call) with:

```cpp
    void execute_scan(DatabaseQuery& q) override {
        // id==0 -> the legacy fixed resident column; id>0 -> a tiered catalog column. The agg op
        // (default AGG_COUNT) selects the reduction; AGG_COUNT preserves the original count result.
        const uint32_t threshold = matrix_get_scan_threshold(q);
        const uint64_t col_id = matrix_get_scan_column_id(q);
        const MatrixAggOp op = matrix_get_scan_agg_op(q);
        const auto st0 = std::chrono::steady_clock::now();
        uint64_t c = 0;
        if (col_id == 0) {
            c = matrix_cpu_reduce(scan_column_.data(), MATRIX_SCAN_COLUMN_SIZE, threshold, op);
        } else {
            c = scan_tiered_column(col_id, threshold, op);
        }
        scan_time_s_ += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - st0).count();
        q.transaction_id = c;
        ++scans_;
        scan_result_sum_ += c;
    }
```

- [ ] **Step 4: Add the op parameter to scan_tiered_column** — In `compute_mock.cpp`, change `scan_tiered_column`'s signature and replace its inline count loop with a reducer call. The full updated method:

```cpp
    uint64_t scan_tiered_column(uint64_t col_id, uint32_t threshold, MatrixAggOp op) {
        auto it = catalog_.find(col_id);
        if (it == catalog_.end()) {
            assert(false && "scan of unregistered column id"); // debug: catch the caller bug
            std::cerr << "CPUMockEngine: scan of unregistered column id " << col_id
                      << " — empty result\n";                 // release: diagnosable, no null-deref
            return 0;
        }
        TieredColumn& col = *it->second;
        tier_mgr_.record_access(col_id, col.size_bytes());          // heat signal

        const MemorySpace home = col.tier();
        if (home != MemorySpace::HOST) col.migrate_to(MemorySpace::HOST); // pull SSD->RAM to scan
        const uint32_t* vals = reinterpret_cast<const uint32_t*>(col.host_ptr());
        const size_t nvals = col.size_bytes() / sizeof(uint32_t);
        const uint64_t result = matrix_cpu_reduce(vals, nvals, threshold, op);
        // ponytail: returning the borrow rewrites the COLD file each cold scan; skip-if-unchanged
        // (or a TierManager note_residency) is the upgrade path if cold-scan churn ever matters.
        if (home != MemorySpace::HOST) col.migrate_to(home);        // return the borrow

        if (++scans_since_rebalance_ >= REBALANCE_EVERY) {
            std::unordered_map<uint64_t, TieredColumn*> ptrs;
            for (auto& kv : catalog_) ptrs[kv.first] = kv.second.get();
            executor_.apply(tier_mgr_.rebalance(), ptrs);
            scans_since_rebalance_ = 0;
        }
        return result;
    }
```

- [ ] **Step 5: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_aggregations.cpp -o /tmp/tagg && /tmp/tagg` → PASS, prints `[engine agg legacy] ok` + `[engine agg tiered] ok` + `ALL AGGREGATION TESTS PASSED`. No warnings.

- [ ] **Step 6: Confirm the pipeline oracle + existing tiering tests are unchanged**

Run:
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"
clang++ -std=c++20 -O2 test_live_tiering.cpp -o /tmp/tlt && /tmp/tlt | tail -1
```
Expected: `Scan result sum: 83886070 (oracle 83886070)` (COUNT path unchanged) and `ALL LIVE-TIERING TESTS PASSED` (the borrow/rebalance logic is intact — only the inner loop changed to the reducer).

- [ ] **Step 7: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add compute_mock.cpp test_aggregations.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: OP_SCAN computes COUNT/SUM/MIN/MAX over legacy + tiered columns via the reducer"
```

---

### Task 3: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (regression gate)**

Run:
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store \
         test_engine_restart test_migration test_scan_coverage test_live_tiering test_aggregations; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t ($(tail -1 /tmp/out_$t))" || echo "FAIL: $t"
done
```
Expected: every line `PASS:`. If any `FAIL:`, STOP and report BLOCKED with `cat /tmp/out_<test>`.

- [ ] **Step 2: Add the new test to make_notebook.py SOURCES**

In `make_notebook.py`, add `"test_aggregations.cpp"` to the `SOURCES` list (after `"test_live_tiering.cpp"`).

- [ ] **Step 3: Add a run cell**

In `make_notebook.py`, immediately after the live-tiering run cell (the `code(... test_live_tiering.cpp ... /tmp/tlt)` block), add a matching markdown + code cell pair (mirror the surrounding indentation/comma style):

```python
    md("### Analytical aggregations\n"
       "OP_SCAN computes COUNT / SUM / MIN / MAX over the values matching the predicate, on both "
       "the legacy column and a tiered catalog column — verified against closed-form oracles."),
    code("!clang++ -std=c++20 -O2 test_aggregations.cpp -o /tmp/tagg 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_aggregations.cpp -o /tmp/tagg; /tmp/tagg"),
```

- [ ] **Step 4: Regenerate the notebook**

Run: `python3 make_notebook.py`
Expected: prints `wrote matrixdb_colab.ipynb: <N> cells, 25 source files embedded` (24 + the new test).

- [ ] **Step 5: Verify the embed**

Run: `grep -o "test_aggregations.cpp" matrixdb_colab.ipynb | wc -l`
Expected: `>= 2` (the `%%writefile` cell + the run cell).

- [ ] **Step 6: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed aggregations test in Colab notebook; suite + oracle verified"
```

---

## Self-Review

**1. Spec coverage:** enum (§1)→T1S3; codec (§2)→T1S4; reducer + edge cases (§3)→T1S4 + T1 tests; dispatch (§2)→T2S3/S4; verification incl. legacy+tiered+empty+non-vacuity (§4)→T1/T2 tests; oracle/suite/notebook→T2S6 + T3. ✓
**2. Placeholder scan:** none — complete code + exact commands + expected output throughout.
**3. Type consistency:** `MatrixAggOp`/`AGG_COUNT..AGG_MAX`, `matrix_cpu_reduce(const uint32_t*, size_t, uint32_t, MatrixAggOp)`, `matrix_set/get_scan_agg_op`, `scan_tiered_column(uint64_t, uint32_t, MatrixAggOp)` — names/signatures identical across T1/T2/T3. `matrix_set_scan_target`/`matrix_get_scan_*` from INT-1 reused unchanged.

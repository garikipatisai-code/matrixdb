# Query Input Validation Implementation Plan — VAL-1

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** `execute_query` returns a `MatrixQueryStatus` and rejects malformed queries gracefully (no assert/throw/crash) instead of aborting.

**Spec:** `docs/superpowers/specs/2026-06-26-query-validation-design.md`

---

### Task 1: MatrixQueryStatus + validated execute_query

**Files:** Modify `compute.hpp`, `compute_mock.cpp`; Create `test_query_validation.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_query_validation.cpp`:

```cpp
// CPU test for query input validation (execute_query returns MatrixQueryStatus, never crashes).
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

static void test_validation() {
    const size_t N = 1000;
    std::vector<uint32_t> a(N), b(500), keys(500);
    for (size_t i = 0; i < N; ++i) a[i] = static_cast<uint32_t>(i);
    for (size_t i = 0; i < 500; ++i) { b[i] = static_cast<uint32_t>(i); keys[i] = static_cast<uint32_t>(i % 4); }
    CPUMockEngine eng(0, "", SIZE_MAX);
    eng.load_scan_column(5, a.data(), N);        // len 1000
    eng.load_scan_column(6, b.data(), 500);      // len 500
    eng.load_scan_column(7, keys.data(), 500);   // len 500, keys i%4
    std::vector<uint64_t> out;
    using S = MatrixQueryStatus;

    assert(eng.execute_query(MatrixQuery{.value_col = 5, .agg = AGG_SUM}, out) == S::OK && out.size() == 1);
    assert(eng.execute_query(MatrixQuery{.value_col = 0, .agg = AGG_SUM}, out) == S::ERR_UNKNOWN_COLUMN && out.empty());
    assert(eng.execute_query(MatrixQuery{.value_col = 999, .agg = AGG_SUM}, out) == S::ERR_UNKNOWN_COLUMN && out.empty());
    assert(eng.execute_query(MatrixQuery{.value_col = 5, .agg = AGG_SUM, .grouped = true, .key_col = 5, .num_groups = 4}, out) == S::ERR_INVALID_GROUP);
    assert(eng.execute_query(MatrixQuery{.value_col = 5, .agg = AGG_SUM, .grouped = true, .key_col = 999, .num_groups = 4}, out) == S::ERR_INVALID_GROUP);
    assert(eng.execute_query(MatrixQuery{.value_col = 6, .agg = AGG_SUM, .grouped = true, .key_col = 7, .num_groups = 0}, out) == S::ERR_INVALID_GROUP);
    assert(eng.execute_query(MatrixQuery{.value_col = 5, .agg = AGG_SUM, .grouped = true, .key_col = 7, .num_groups = 4}, out) == S::ERR_INVALID_GROUP); // len 1000 vs 500
    assert(eng.execute_query(MatrixQuery{.value_col = 6, .agg = AGG_SUM, .grouped = true, .key_col = 7, .num_groups = (1u << 28) + 1}, out) == S::ERR_TOO_MANY_GROUPS);
    // valid grouped (key 7 len500, value 6 len500, distinct) -> OK + per-group result
    assert(eng.execute_query(MatrixQuery{.value_col = 6, .agg = AGG_SUM, .grouped = true, .key_col = 7, .num_groups = 4}, out) == S::OK && out.size() == 4);
    std::cout << "[query validation] ok\n";
}

int main() {
    test_validation();
    std::cout << "ALL QUERY-VALIDATION TESTS PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_query_validation.cpp -o /tmp/tqv && /tmp/tqv` → FAIL to compile (`MatrixQueryStatus` undeclared; `execute_query` returns void).

- [ ] **Step 3: Add the enum** — In `compute.hpp`, immediately AFTER the `struct MatrixQuery { ... };`, add:

```cpp
// Result of CPUMockEngine::execute_query — OK or a specific input-validation rejection (the query
// boundary never crashes on caller input; on any ERR the out vector is left empty).
enum class MatrixQueryStatus { OK, ERR_UNKNOWN_COLUMN, ERR_INVALID_GROUP, ERR_TOO_MANY_GROUPS };
```

- [ ] **Step 4: Add the MAX_QUERY_GROUPS constant + catalog_has helper** — In `compute_mock.cpp`: add `static constexpr uint32_t MAX_QUERY_GROUPS = 1u << 28;` near the other static constants (e.g. by `REBALANCE_EVERY`/`MATRIX_CATALOG_MAGIC`). Add a private helper:

```cpp
    bool catalog_has(uint64_t id) const { return id != 0 && catalog_.count(id) != 0; }
```

- [ ] **Step 5: Replace execute_query with the validated version** — In `compute_mock.cpp`, replace the entire current `execute_query` (the `void` version with the two `assert`s) with:

```cpp
    // Unified analytical query over catalog columns. Validates input at the boundary and returns a
    // status (never crashes on caller input); on any ERR, out is cleared. Scalar -> out[0];
    // grouped -> out[0..num_groups). Catalog columns only (the legacy id-0 fixed column is the
    // benchmark fixture, not a query target).
    MatrixQueryStatus execute_query(const MatrixQuery& q, std::vector<uint64_t>& out) {
        out.clear();
        if (!catalog_has(q.value_col)) return MatrixQueryStatus::ERR_UNKNOWN_COLUMN;
        if (q.grouped) {
            if (!catalog_has(q.key_col) || q.key_col == q.value_col || q.num_groups == 0
                || catalog_.at(q.key_col)->size_bytes() != catalog_.at(q.value_col)->size_bytes())
                return MatrixQueryStatus::ERR_INVALID_GROUP;
            if (q.num_groups > MAX_QUERY_GROUPS) return MatrixQueryStatus::ERR_TOO_MANY_GROUPS;
            if (q.has_filter) grouped_aggregate_where(q.key_col, q.value_col, q.num_groups, q.agg, q.threshold, out);
            else              grouped_aggregate(q.key_col, q.value_col, q.num_groups, q.agg, out);
        } else {
            out.assign(1, scan_tiered_column(q.value_col, q.threshold, q.agg, q.has_filter));
        }
        return MatrixQueryStatus::OK;
    }
```

- [ ] **Step 6: Run to verify it passes (debug + release)** — Both must pass:
  - `clang++ -std=c++20 -O2 -Wall -Wextra test_query_validation.cpp -o /tmp/tqv && /tmp/tqv` → `[query validation] ok` + `ALL QUERY-VALIDATION TESTS PASSED`, no warnings.
  - `clang++ -std=c++20 -O2 -DNDEBUG test_query_validation.cpp -o /tmp/tqv_r && /tmp/tqv_r` → same PASS (proves the error paths are release-safe: no assert/`.at()`-throw/abort — validation returns before reaching them).

- [ ] **Step 7: Confirm no regression** — `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep -E "Scan result sum|Demo OK"` → oracle `83886070` + `Demo OK.` (the demo ignores the new return value and still works); `clang++ -std=c++20 -O2 test_query.cpp -o /tmp/tq && /tmp/tq | tail -1` → `ALL QUERY TESTS PASSED`.

- [ ] **Step 8: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add compute.hpp compute_mock.cpp test_query_validation.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: query input validation — execute_query returns MatrixQueryStatus, rejects bad queries gracefully (no crash)"
```

---

### Task 2: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (15 tests).**
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by \
         test_query test_observability test_column_io test_catalog_snapshot test_query_validation; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 15× `PASS:`. If any fail, STOP / report BLOCKED.

- [ ] **Step 2: Notebook** — add `"test_query_validation.cpp"` to `make_notebook.py` SOURCES (after `"test_catalog_snapshot.cpp"`); add a run cell after the catalog-snapshot run cell:
```python
    md("### Query input validation\n"
       "execute_query rejects malformed queries (unknown/zero column, self-group, length mismatch, "
       "absurd group count) with a status code — gracefully, never crashing (verified under -DNDEBUG too)."),
    code("!clang++ -std=c++20 -O2 test_query_validation.cpp -o /tmp/tqv 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_query_validation.cpp -o /tmp/tqv; /tmp/tqv"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 32 source files embedded`. Verify `grep -o "test_query_validation.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed query-validation test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** enum (§1)→T1S3; MAX_QUERY_GROUPS + catalog_has + validated execute_query (§2)→T1S4/S5; every error path + valid + release-safety (§3)→T1S1/S6; suite+notebook→T2. ✓
**Placeholders:** none. **Type consistency:** `MatrixQueryStatus{OK,ERR_UNKNOWN_COLUMN,ERR_INVALID_GROUP,ERR_TOO_MANY_GROUPS}`, `execute_query(...) -> MatrixQueryStatus`, `catalog_has`, `MAX_QUERY_GROUPS` consistent T1/T2. Existing callers (demo, test_query) ignore the return — still compile. Reuses grouped_aggregate(_where)/scan_tiered_column unchanged.

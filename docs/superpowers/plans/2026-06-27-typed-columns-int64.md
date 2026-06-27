# Typed Columns — int64 Implementation Plan (DM-3a)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Add signed `int64` analytical columns with scalar (unfiltered) COUNT/SUM/MIN/MAX. Additive + oracle-safe via a per-column type tag defaulting to `U32`.

**Spec:** `docs/superpowers/specs/2026-06-27-typed-columns-int64-design.md`

---

### Task 1: type tag + int64 reducer + int64 column + query dispatch + test

**Files:** Modify `compute.hpp`, `compute_mock.cpp`; Create `test_typed_columns.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_typed_columns.cpp`:

```cpp
// CPU test for int64 typed columns (DM-3a): MatrixType, matrix_cpu_reduce_all_i64,
// load_scan_column_i64, and execute_query int64 scalar dispatch (incl. graceful ERR_UNSUPPORTED_TYPE).
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

// Hand oracle over the int64 data (independent of the SUT reducer).
static int64_t oracle(const std::vector<int64_t>& v, MatrixAggOp op) {
    int64_t cnt = static_cast<int64_t>(v.size()), sum = 0, mn = INT64_MAX, mx = INT64_MIN;
    for (int64_t x : v) { sum += x; if (x < mn) mn = x; if (x > mx) mx = x; }
    switch (op) { case AGG_SUM: return sum; case AGG_MIN: return mn; case AGG_MAX: return mx;
                  case AGG_COUNT: default: return cnt; }
}

static void test_reduce_i64() {
    const std::vector<int64_t> v = {-7, 0, 5, 5000000000LL, -3, 2147483648LL};   // negatives + > UINT32_MAX
    for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX})
        assert(matrix_cpu_reduce_all_i64(v.data(), v.size(), op) == oracle(v, op));
    // Empty-set sentinels.
    assert(matrix_cpu_reduce_all_i64(nullptr, 0, AGG_COUNT) == 0);
    assert(matrix_cpu_reduce_all_i64(nullptr, 0, AGG_SUM) == 0);
    assert(matrix_cpu_reduce_all_i64(nullptr, 0, AGG_MIN) == INT64_MAX);
    assert(matrix_cpu_reduce_all_i64(nullptr, 0, AGG_MAX) == INT64_MIN);
    std::cout << "[reduce i64] ok\n";
}

static void test_engine_i64() {
    const std::vector<int64_t> v = {-7, 0, 5, 5000000000LL, -3, 2147483648LL, 100, -100};
    CPUMockEngine eng;
    eng.load_scan_column_i64(7, v.data(), v.size());
    assert(eng.column_type(7) == MatrixType::I64);
    for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX}) {
        MatrixQuery q{}; q.value_col = 7; q.agg = op;
        std::vector<uint64_t> out;
        assert(eng.execute_query(q, out) == MatrixQueryStatus::OK && out.size() == 1);
        assert(static_cast<int64_t>(out[0]) == oracle(v, op) && "int64 scalar aggregate matches oracle");
    }
    // The > UINT32_MAX value proves genuine 64-bit: a uint32 read of 5000000000 would be 705032704.
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_MAX; std::vector<uint64_t> o; eng.execute_query(q, o);
      assert(static_cast<int64_t>(o[0]) == 5000000000LL && static_cast<int64_t>(o[0]) != 705032704); }
    // Graceful: filtered / grouped int64 are not yet supported.
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_SUM; q.has_filter = true; q.threshold = 0;
      std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_UNSUPPORTED_TYPE && o.empty()); }
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_COUNT; q.grouped = true; q.key_col = 7; q.num_groups = 2;
      std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_UNSUPPORTED_TYPE); }
    std::cout << "[engine i64] ok\n";
}

static void test_u32_untouched() {
    std::vector<uint32_t> v(100);
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<uint32_t>(i);
    CPUMockEngine eng;
    eng.load_scan_column(3, v.data(), v.size());
    assert(eng.column_type(3) == MatrixType::U32 && "untagged column defaults to U32");
    MatrixQuery q{}; q.value_col = 3; q.agg = AGG_SUM; std::vector<uint64_t> out;
    assert(eng.execute_query(q, out) == MatrixQueryStatus::OK && out[0] == 4950);   // 0+...+99
    MatrixQuery qf{}; qf.value_col = 3; qf.agg = AGG_COUNT; qf.has_filter = true; qf.threshold = 50;
    std::vector<uint64_t> of; assert(eng.execute_query(qf, of) == MatrixQueryStatus::OK && of[0] == 49); // 51..99
    std::cout << "[u32 untouched] ok\n";
}

int main() {
    test_reduce_i64();
    test_engine_i64();
    test_u32_untouched();
    std::cout << "ALL TYPED-COLUMN TESTS PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_typed_columns.cpp -o /tmp/ttyp && /tmp/ttyp` → FAIL to compile (`MatrixType`, `matrix_cpu_reduce_all_i64`, `load_scan_column_i64`, `column_type`, `ERR_UNSUPPORTED_TYPE` undeclared).

- [ ] **Step 3: Add MatrixType + the int64 reducer + the status value to compute.hpp** — Insert `enum class MatrixType` and `matrix_cpu_reduce_all_i64` (spec §2, verbatim) immediately after `matrix_cpu_reduce_all` (after line 142, before `MatrixQuery`). Add `ERR_UNSUPPORTED_TYPE` as the last value of `enum class MatrixQueryStatus` (line 158).

- [ ] **Step 4: Wire the engine (compute_mock.cpp)** — per spec §3:
  - Add the member `std::unordered_map<uint64_t, MatrixType> col_types_;` beside the `catalog_` member.
  - Add `load_scan_column_i64` immediately after `load_scan_column` (line 75) — spec §3 verbatim.
  - Add `MatrixType column_type(uint64_t id) const { ... }` near the other inspection accessors (~line 79-82) — spec §3 verbatim.
  - Add the private `scan_tiered_column_i64` beside `scan_tiered_column` (read `scan_tiered_column` at line ~425 and mirror its exact `record_access`/borrow/`host_ptr`/return-borrow steps) — spec §3 verbatim.
  - In `execute_query` (line 290), after `if (!catalog_has(q.value_col)) return MatrixQueryStatus::ERR_UNKNOWN_COLUMN;` (line 292), insert the int64 dispatch block (spec §3 verbatim). Leave the existing U32 body unchanged.
  - In `save_catalog`'s per-column loop AND `save_column`, add `assert(column_type(id) == MatrixType::U32 && "typed-column persistence is DM-3c");` before the column's bytes are written. (In `save_catalog` the per-column id is the loop variable; in `save_column` it is the `id` parameter.)

- [ ] **Step 5: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_typed_columns.cpp -o /tmp/ttyp && /tmp/ttyp` → PASS: `[reduce i64] ok`, `[engine i64] ok`, `[u32 untouched] ok`, `ALL TYPED-COLUMN TESTS PASSED`. Zero warnings.

- [ ] **Step 6: Confirm no regression** — `execute_query` and `save_catalog` changed, so these MUST still pass unmodified:
  - `for t in test_query test_group_by test_aggregations test_query_validation test_query_predicates test_live_tiering test_catalog_snapshot test_observability; do clang++ -std=c++20 -O2 $t.cpp -o /tmp/$t 2>/dev/null && /tmp/$t | tail -1; done`
  - `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`.
  Also build the test with `-DNDEBUG` once (`clang++ -std=c++20 -O2 -DNDEBUG test_typed_columns.cpp -o /tmp/ttypr && /tmp/ttypr`) to confirm it still passes with the asserts compiled out (the `save_catalog` guard is debug-only by design; behavior must not depend on it). If any differ, STOP / report BLOCKED.

- [ ] **Step 7: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add compute.hpp compute_mock.cpp test_typed_columns.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: int64 typed columns (DM-3a) — load_scan_column_i64 + signed scalar aggregation, additive/oracle-safe"
```

---

### Task 2: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (23 tests).**
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by \
         test_query test_observability test_column_io test_catalog_snapshot test_query_validation \
         test_transactions test_server test_security test_audit test_csv_ingest test_checkpoint \
         test_query_predicates test_typed_columns; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 23× `PASS:`. If any fail, STOP / report BLOCKED with `cat /tmp/out_<test>`.

- [ ] **Step 2: Notebook** — add `"test_typed_columns.cpp"` to `make_notebook.py` SOURCES right after `"test_query_predicates.cpp"`; add a run cell after the richer-predicates run cell (the one compiling `test_query_predicates.cpp` to `/tmp/tqp`):
```python
    md("### Typed columns — int64\n"
       "load_scan_column_i64 registers a signed 64-bit column (values beyond uint32 range, and "
       "negatives); execute_query aggregates it (COUNT/SUM/MIN/MAX) — the first slice of real types."),
    code("!clang++ -std=c++20 -O2 test_typed_columns.cpp -o /tmp/ttyp 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_typed_columns.cpp -o /tmp/ttyp; /tmp/ttyp"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 42 source files embedded`. Verify `grep -o "test_typed_columns.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed int64 typed-columns test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** MatrixType + matrix_cpu_reduce_all_i64 + ERR_UNSUPPORTED_TYPE (§2)→T1S3; col_types_ + load_scan_column_i64 + column_type + scan_tiered_column_i64 + execute_query dispatch + save guards (§3)→T1S4; reduce correctness (negatives + >UINT32_MAX) + engine scalar + graceful unsupported + U32 untouched + empty sentinels (§4)→T1S1; regression of the 8 catalog/query tests + oracle + NDEBUG (§4)→T1S6; suite+notebook→T2. ✓
**Placeholders:** `scan_tiered_column_i64` in T1S4 says "mirror scan_tiered_column's borrow steps" — spec §3 gives its full body verbatim, and the implementer reads `scan_tiered_column` (line 425) to match the borrow idiom; not a vague placeholder. **Type consistency:** `MatrixType{U32=0,I64}`, `matrix_cpu_reduce_all_i64(const int64_t*,n,op)->int64_t`, `load_scan_column_i64(id,const int64_t*,n)`, `column_type(id)->MatrixType`, `scan_tiered_column_i64(id,op)->int64_t`, `MatrixQueryStatus::ERR_UNSUPPORTED_TYPE`, `col_types_` — consistent across spec §2/§3 and plan T1S3/S4 and the test. Result delivered as `static_cast<uint64_t>(int64 result)`; test reads `static_cast<int64_t>(out[0])`. Oracle path (`matrix_cpu_reduce*`, id-0) untouched; every existing column is U32 so the new branch is dormant for them.

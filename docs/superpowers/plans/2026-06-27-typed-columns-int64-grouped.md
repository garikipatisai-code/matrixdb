# Grouped int64 Aggregation Implementation Plan (DM-3c)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** `GROUP BY uint32_key, AGG(int64_value)` (filtered + unfiltered) — completing int64 query parity. int64 MAX inits INT64_MIN; row-count (not byte-length) length guard; int64 key still rejected.

**Spec:** `docs/superpowers/specs/2026-06-27-typed-columns-int64-grouped-design.md`

---

### Task 1: int64 group reducer + column_rows + grouped_aggregate_i64 + dispatch + test

**Files:** Modify `compute.hpp`, `compute_mock.cpp`; Create `test_typed_grouped.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_typed_grouped.cpp`:

```cpp
// CPU test for grouped int64 aggregation (DM-3c): matrix_group_reduce_i64[_pred], grouped_aggregate_i64,
// execute_query int64 GROUP BY (u32 key, int64 value), incl. mixed-width + int64-key-rejection guards.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

static bool refm(int64_t v, MatrixCmp c, int64_t a, int64_t b) {
    switch (c) { case MatrixCmp::GT: return v>a; case MatrixCmp::GE: return v>=a; case MatrixCmp::LT: return v<a;
                 case MatrixCmp::LE: return v<=a; case MatrixCmp::EQ: return v==a; case MatrixCmp::NE: return v!=a;
                 case MatrixCmp::BETWEEN: return v>=a && v<=b; } return false; }
// Brute grouped oracle: out[g] for the given op over rows matching (filtered ? pred : all).
static void ref_group(const std::vector<uint32_t>& keys, const std::vector<int64_t>& vals, uint32_t ng,
                      MatrixAggOp op, bool filt, MatrixCmp c, int64_t a, int64_t b, std::vector<int64_t>& out) {
    out.assign(ng, op == AGG_MIN ? INT64_MAX : (op == AGG_MAX ? INT64_MIN : 0));
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] >= ng) continue;
        if (filt && !refm(vals[i], c, a, b)) continue;
        int64_t& o = out[keys[i]];
        switch (op) { case AGG_SUM: o += vals[i]; break; case AGG_MIN: if (vals[i]<o) o=vals[i]; break;
                      case AGG_MAX: if (vals[i]>o) o=vals[i]; break; case AGG_COUNT: default: o += 1; break; } }
}

static void test_group_reduce_i64() {
    // keys 0..2; group 1 is ALL NEGATIVE (the MAX-init guard); group 2 has a > UINT32_MAX value.
    std::vector<uint32_t> keys = {0, 1, 1, 2, 0, 1, 2};
    std::vector<int64_t>  vals = {5, -3, -100, 5000000000LL, 7, -1, -2};
    for (bool filt : {false, true})
        for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX}) {
            std::vector<int64_t> got(3), exp;
            MatrixPredicateI64 p{MatrixCmp::GE, -3, 0};   // filt: value >= -3
            if (filt) matrix_cpu_group_reduce_i64_pred(keys.data(), vals.data(), keys.size(), 3, op, p, got.data());
            else      matrix_cpu_group_reduce_i64(keys.data(), vals.data(), keys.size(), 3, op, got.data());
            ref_group(keys, vals, 3, op, filt, MatrixCmp::GE, -3, 0, exp);
            assert(got == exp && "grouped int64 reduce matches oracle");
        }
    // Explicit MAX-init guard: group 1 (all negative) MAX must be -3, NOT 0.
    std::vector<int64_t> mx(3);
    matrix_cpu_group_reduce_i64(keys.data(), vals.data(), keys.size(), 3, AGG_MAX, mx.data());
    assert(mx[1] == -1 && "MAX of a negative group is the max negative (-1), not 0");   // group1 vals {-3,-100,-1}
    std::cout << "[group reduce i64] ok\n";
}

static void test_engine_grouped_i64() {
    std::vector<uint32_t> keys = {0, 1, 1, 2, 0, 1, 2};
    std::vector<int64_t>  vals = {5, -3, -100, 5000000000LL, 7, -1, -2};
    CPUMockEngine eng;
    eng.load_scan_column(1, keys.data(), keys.size());        // u32 key
    eng.load_scan_column_i64(7, vals.data(), vals.size());    // int64 value (equal ROW count, 4N vs 8N bytes)
    for (bool filt : {false, true})
        for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX}) {
            MatrixQuery q{}; q.value_col = 7; q.key_col = 1; q.num_groups = 3; q.agg = op; q.grouped = true;
            q.has_filter = filt; q.cmp = MatrixCmp::GE; q.lo_i64 = -3;
            std::vector<uint64_t> out;
            assert(eng.execute_query(q, out) == MatrixQueryStatus::OK && out.size() == 3);
            std::vector<int64_t> exp; ref_group(keys, vals, 3, op, filt, MatrixCmp::GE, -3, 0, exp);
            for (uint32_t g = 0; g < 3; ++g) assert(static_cast<int64_t>(out[g]) == exp[g] && "engine grouped int64 == oracle");
        }
    // Non-vacuity: group 2's SUM includes the > UINT32_MAX value (genuine int64).
    { MatrixQuery q{}; q.value_col = 7; q.key_col = 1; q.num_groups = 3; q.agg = AGG_SUM; q.grouped = true;
      std::vector<uint64_t> o; eng.execute_query(q, o); assert(static_cast<int64_t>(o[2]) == 5000000000LL - 2); }
    std::cout << "[engine grouped i64] ok\n";
}

static void test_grouped_i64_guards() {
    std::vector<uint32_t> keys = {0, 1, 0, 1};
    std::vector<int64_t>  vals = {1, 2, 3, 4};
    CPUMockEngine eng;
    eng.load_scan_column(1, keys.data(), keys.size());        // u32 key, 4 rows
    eng.load_scan_column_i64(7, vals.data(), vals.size());    // i64 value, 4 rows (equal ROW count)
    // int64 KEY rejected: i64 value + i64 key.
    std::vector<int64_t> keys64 = {0, 1, 0, 1};
    eng.load_scan_column_i64(8, keys64.data(), keys64.size());
    { MatrixQuery q{}; q.value_col = 7; q.key_col = 8; q.num_groups = 2; q.agg = AGG_COUNT; q.grouped = true;
      std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_UNSUPPORTED_TYPE && "int64 key rejected"); }
    // Row-count mismatch -> ERR_INVALID_GROUP (i64 value 4 rows, u32 key 5 rows).
    std::vector<uint32_t> keys5 = {0, 1, 0, 1, 0};
    eng.load_scan_column(2, keys5.data(), keys5.size());
    { MatrixQuery q{}; q.value_col = 7; q.key_col = 2; q.num_groups = 2; q.agg = AGG_COUNT; q.grouped = true;
      std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_INVALID_GROUP && "row-count mismatch rejected"); }
    std::cout << "[grouped i64 guards] ok\n";
}

int main() {
    test_group_reduce_i64();
    test_engine_grouped_i64();
    test_grouped_i64_guards();
    std::cout << "ALL TYPED-GROUPED TESTS PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_typed_grouped.cpp -o /tmp/ttg && /tmp/ttg` → FAIL to compile (`matrix_cpu_group_reduce_i64[_pred]` undeclared; the engine grouped-int64 path returns `ERR_UNSUPPORTED_TYPE` so the engine tests would also fail).

- [ ] **Step 3: Add the int64 group reducer to compute.hpp** — Insert `matrix_group_reduce_i64_impl` + `matrix_cpu_group_reduce_i64` + `matrix_cpu_group_reduce_i64_pred` (spec §2, verbatim) immediately AFTER `matrix_cpu_group_reduce_pred` (the last u32 grouped wrapper, ~line 281).

- [ ] **Step 4: Wire the engine (compute_mock.cpp)** — per spec §3:
  - Add `size_t column_rows(uint64_t id) const { ... }` near `column_type` (~line 94).
  - Add `grouped_aggregate_i64(...)` (spec §3 verbatim) beside `grouped_aggregate_pred` (~line 276-291).
  - In `execute_query`, REPLACE the I64-value branch's `if (q.grouped) return MatrixQueryStatus::ERR_UNSUPPORTED_TYPE;` (~line 310) with the grouped-int64 block from spec §3 (validate → `grouped_aggregate_i64`); the scalar `out.assign(1, ...)` else-path stays.

- [ ] **Step 5: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_typed_grouped.cpp -o /tmp/ttg && /tmp/ttg` → PASS: `[group reduce i64] ok`, `[engine grouped i64] ok`, `[grouped i64 guards] ok`, `ALL TYPED-GROUPED TESTS PASSED`. Zero warnings.

- [ ] **Step 6: Confirm no regression** — `execute_query` changed, so these MUST still pass unmodified:
  - `for t in test_typed_columns test_typed_predicates test_group_by test_query test_query_predicates test_query_validation test_aggregations test_live_tiering test_observability test_server; do clang++ -std=c++20 -O2 $t.cpp -o /tmp/$t 2>/dev/null && /tmp/$t | tail -1; done`
  - `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`.
  Also build `test_typed_grouped` with `-DNDEBUG -Wall -Wextra` and run (confirm warning-free + passes). If any differ, STOP / report BLOCKED.

- [ ] **Step 7: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add compute.hpp compute_mock.cpp test_typed_grouped.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: grouped int64 aggregation (DM-3c) — GROUP BY u32 key over int64 value; completes int64 query parity"
```

---

### Task 2: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (25 tests).**
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by \
         test_query test_observability test_column_io test_catalog_snapshot test_query_validation \
         test_transactions test_server test_security test_audit test_csv_ingest test_checkpoint \
         test_query_predicates test_typed_columns test_typed_predicates test_typed_grouped; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 25× `PASS:`. If any fail, STOP / report BLOCKED with `cat /tmp/out_<test>`.

- [ ] **Step 2: Notebook** — add `"test_typed_grouped.cpp"` to `make_notebook.py` SOURCES right after `"test_typed_predicates.cpp"`; add a run cell after the int64-filtered run cell (the one compiling `test_typed_predicates.cpp` to `/tmp/ttp`):
```python
    md("### Grouped int64 aggregation\n"
       "GROUP BY a uint32 key over an int64 value column (filtered + unfiltered) — completing int64 "
       "query parity; verified incl. negative-group MAX and mixed-width row-count guards."),
    code("!clang++ -std=c++20 -O2 test_typed_grouped.cpp -o /tmp/ttg 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_typed_grouped.cpp -o /tmp/ttg; /tmp/ttg"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 44 source files embedded`. Verify `grep -o "test_typed_grouped.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed grouped-int64 test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** group reducer + wrappers (§2)→T1S3; column_rows + grouped_aggregate_i64 + execute_query dispatch (§3)→T1S4; reduce + MAX-init guard + engine grouped + mixed-width/int64-key/row-count guards + non-vacuity (§4)→T1S1; regression of 10 tests + oracle + NDEBUG (§4)→T1S6; suite+notebook→T2. ✓
**Placeholders:** none — all bodies verbatim. **Type consistency:** `matrix_group_reduce_i64_impl<Filtered>(uint32* keys, int64* values, …, MatrixPredicateI64, int64* out)`, wrappers `matrix_cpu_group_reduce_i64`/`_pred`, `column_rows(id)->size_t`, `grouped_aggregate_i64(key,value,ng,op,MatrixPredicateI64,has_filter,out)`, `execute_query` i64-grouped dispatch — consistent across spec §2/§3, plan T1S3/S4, and the test. int64 MAX inits INT64_MIN (negative-group correctness). u32 grouped path + byte-length guard untouched; only the i64-value branch uses `column_rows`. int64 key still rejected.

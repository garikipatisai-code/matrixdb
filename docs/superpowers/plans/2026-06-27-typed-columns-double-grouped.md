# Grouped double Aggregation Implementation Plan (DM-3f)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** `GROUP BY uint32_key, AGG(double_value)` (filtered + unfiltered) — completing double query parity. Direct mirror of DM-3c (grouped int64): MAX inits `-inf`, row-count guard, double key rejected, bit_cast result.

**Spec:** `docs/superpowers/specs/2026-06-27-typed-columns-double-grouped-design.md`

---

### Task 1: double group reducer + grouped_aggregate_f64 + dispatch + test

**Files:** Modify `compute.hpp`, `compute_mock.cpp`; Create `test_typed_double_grouped.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_typed_double_grouped.cpp`:

```cpp
// CPU test for grouped double aggregation (DM-3f): matrix_group_reduce_f64[_pred], grouped_aggregate_f64,
// execute_query double GROUP BY (u32 key, double value), incl. negative-group MAX + guards. Mirrors DM-3c.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <bit>
#include <limits>
#include <vector>
#include <iostream>

static bool refm(double v, MatrixCmp c, double a, double b) {
    switch (c) { case MatrixCmp::GT: return v>a; case MatrixCmp::GE: return v>=a; case MatrixCmp::LT: return v<a;
                 case MatrixCmp::LE: return v<=a; case MatrixCmp::EQ: return v==a; case MatrixCmp::NE: return v!=a;
                 case MatrixCmp::BETWEEN: return v>=a && v<=b; } return false; }
static void ref_group(const std::vector<uint32_t>& keys, const std::vector<double>& vals, uint32_t ng,
                      MatrixAggOp op, bool filt, MatrixCmp c, double a, double b, std::vector<double>& out) {
    const double inf = std::numeric_limits<double>::infinity();
    out.assign(ng, op == AGG_MIN ? inf : (op == AGG_MAX ? -inf : 0.0));
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] >= ng) continue;
        if (filt && !refm(vals[i], c, a, b)) continue;
        double& o = out[keys[i]];
        switch (op) { case AGG_SUM: o += vals[i]; break; case AGG_MIN: if (vals[i]<o) o=vals[i]; break;
                      case AGG_MAX: if (vals[i]>o) o=vals[i]; break; case AGG_COUNT: default: o += 1.0; break; } } }

// keys 0..2; group 1 all-negative (MAX-init guard); group 2 has a large + fractional value.
static const std::vector<uint32_t> K = {0, 1, 1, 2, 0, 1, 2};
static const std::vector<double>   V = {1.5, -3.0, -100.0, 5000000000.0, 0.5, -0.25, 2.25};

static void test_group_reduce_f64() {
    for (bool filt : {false, true})
        for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX}) {
            std::vector<double> got(3), exp;
            MatrixPredicateF64 p{MatrixCmp::GE, -3.0, 0};   // value >= -3.0
            if (filt) matrix_cpu_group_reduce_f64_pred(K.data(), V.data(), K.size(), 3, op, p, got.data());
            else      matrix_cpu_group_reduce_f64(K.data(), V.data(), K.size(), 3, op, got.data());
            ref_group(K, V, 3, op, filt, MatrixCmp::GE, -3.0, 0, exp);
            assert(got == exp && "grouped double reduce matches oracle"); }
    // MAX-init guard: group 1 (all negative {-3,-100,-0.25}) MAX must be -0.25, NOT 0.
    std::vector<double> mx(3); matrix_cpu_group_reduce_f64(K.data(), V.data(), K.size(), 3, AGG_MAX, mx.data());
    assert(mx[1] == -0.25 && "MAX of a negative double group is the max negative, not 0");
    std::cout << "[group reduce f64] ok\n";
}

static void test_engine_grouped_f64() {
    CPUMockEngine eng;
    eng.load_scan_column(1, K.data(), K.size());          // u32 key
    eng.load_scan_column_f64(7, V.data(), V.size());      // double value (same ROW count, 4N vs 8N bytes)
    for (bool filt : {false, true})
        for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX}) {
            MatrixQuery q{}; q.value_col = 7; q.key_col = 1; q.num_groups = 3; q.agg = op; q.grouped = true;
            q.has_filter = filt; q.cmp = MatrixCmp::GE; q.lo_f64 = -3.0;
            std::vector<uint64_t> out;
            assert(eng.execute_query(q, out) == MatrixQueryStatus::OK && out.size() == 3);
            std::vector<double> exp; ref_group(K, V, 3, op, filt, MatrixCmp::GE, -3.0, 0, exp);
            for (uint32_t g = 0; g < 3; ++g) assert(std::bit_cast<double>(out[g]) == exp[g] && "engine grouped double == oracle"); }
    // Non-vacuity: group 2 SUM includes the large + fractional values (5e9 + 2.25).
    { MatrixQuery q{}; q.value_col = 7; q.key_col = 1; q.num_groups = 3; q.agg = AGG_SUM; q.grouped = true;
      std::vector<uint64_t> o; eng.execute_query(q, o); assert(std::bit_cast<double>(o[2]) == 5000000000.0 + 2.25); }
    std::cout << "[engine grouped f64] ok\n";
}

static void test_grouped_f64_guards() {
    CPUMockEngine eng;
    eng.load_scan_column(1, K.data(), K.size());
    eng.load_scan_column_f64(7, V.data(), V.size());
    std::vector<double> dkeys = {0, 1, 0, 1, 0, 1, 0};
    eng.load_scan_column_f64(8, dkeys.data(), dkeys.size());     // double key
    { MatrixQuery q{}; q.value_col = 7; q.key_col = 8; q.num_groups = 2; q.agg = AGG_COUNT; q.grouped = true;
      std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_UNSUPPORTED_TYPE && "double key rejected"); }
    std::vector<uint32_t> k8 = {0, 1, 0, 1, 0, 1, 0, 1};         // 8 rows vs V's 7 -> mismatch
    eng.load_scan_column(2, k8.data(), k8.size());
    { MatrixQuery q{}; q.value_col = 7; q.key_col = 2; q.num_groups = 2; q.agg = AGG_COUNT; q.grouped = true;
      std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_INVALID_GROUP && "row-count mismatch rejected"); }
    std::cout << "[grouped f64 guards] ok\n";
}

int main() { test_group_reduce_f64(); test_engine_grouped_f64(); test_grouped_f64_guards();
    std::cout << "ALL TYPED-DOUBLE-GROUPED TESTS PASSED\n"; return 0; }
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_typed_double_grouped.cpp -o /tmp/tdg && /tmp/tdg` → FAIL to compile (`matrix_cpu_group_reduce_f64[_pred]` undeclared; grouped double still `ERR_UNSUPPORTED_TYPE`).

- [ ] **Step 3: Add the double group reducer to compute.hpp** — Insert `matrix_group_reduce_f64_impl` + `matrix_cpu_group_reduce_f64` + `matrix_cpu_group_reduce_f64_pred` (spec §2 verbatim) immediately after `matrix_cpu_group_reduce_i64_pred` (the int64 grouped wrappers).

- [ ] **Step 4: Wire the engine (compute_mock.cpp)** — add `grouped_aggregate_f64` (spec §3 verbatim) beside `grouped_aggregate_i64`; in `execute_query` replace the F64-value branch's `if (q.grouped) return MatrixQueryStatus::ERR_UNSUPPORTED_TYPE;` with the grouped block from spec §3 (key-type check first, guarded by catalog_has → `grouped_aggregate_f64`). The scalar else-path stays.

- [ ] **Step 5: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_typed_double_grouped.cpp -o /tmp/tdg && /tmp/tdg` → PASS: `[group reduce f64] ok`, `[engine grouped f64] ok`, `[grouped f64 guards] ok`, `ALL TYPED-DOUBLE-GROUPED TESTS PASSED`. Zero warnings.

- [ ] **Step 6: Confirm no regression** — `execute_query` changed; these MUST still pass unmodified:
  - `for t in test_typed_double test_typed_grouped test_typed_columns test_typed_predicates test_typed_snapshot test_group_by test_query test_query_predicates test_query_validation test_live_tiering; do clang++ -std=c++20 -O2 $t.cpp -o /tmp/$t 2>/dev/null && /tmp/$t | tail -1; done`
  - `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`.
  Also build `test_typed_double_grouped` with `-DNDEBUG -Wall -Wextra` (warning-free + passes). If any differ, STOP / report BLOCKED.

- [ ] **Step 7: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add compute.hpp compute_mock.cpp test_typed_double_grouped.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: grouped double aggregation (DM-3f) — GROUP BY u32 key over double value; double query parity complete"
```

---

### Task 2: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (28 tests).**
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by \
         test_query test_observability test_column_io test_catalog_snapshot test_query_validation \
         test_transactions test_server test_security test_audit test_csv_ingest test_checkpoint \
         test_query_predicates test_typed_columns test_typed_predicates test_typed_grouped test_typed_snapshot test_typed_double test_typed_double_grouped; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 28× `PASS:`. If any fail, STOP / report BLOCKED.

- [ ] **Step 2: Notebook** — add `"test_typed_double_grouped.cpp"` to `make_notebook.py` SOURCES right after `"test_typed_double.cpp"`; add a run cell after the double-columns run cell (`test_typed_double.cpp` → `/tmp/ttd`):
```python
    md("### Grouped double aggregation\n"
       "GROUP BY a uint32 key over a double value column (filtered + unfiltered) — completing double "
       "query parity; verified incl. negative-group MAX and mixed-width row-count guards."),
    code("!clang++ -std=c++20 -O2 test_typed_double_grouped.cpp -o /tmp/tdg 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_typed_double_grouped.cpp -o /tmp/tdg; /tmp/tdg"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 47 source files embedded`. Verify `grep -o "test_typed_double_grouped.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed grouped-double test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** group reducer + wrappers (§2)→T1S3; grouped_aggregate_f64 + execute_query dispatch (§3)→T1S4; reduce + MAX-init + engine grouped + double-key/row-count guards + non-vacuity (§4)→T1S1; regression + oracle + NDEBUG→T1S6; suite+notebook→T2. ✓
**Placeholders:** none — verbatim mirror of DM-3c. **Type consistency:** `matrix_group_reduce_f64_impl<Filtered>(uint32* keys, double* values, …, MatrixPredicateF64, double* out)`, wrappers `_f64`/`_f64_pred`, `grouped_aggregate_f64(...,MatrixPredicateF64,has_filter,out)`, `std::bit_cast<uint64_t>` out, execute_query F64-grouped dispatch (key-type check first, guarded by catalog_has). MAX init -inf. `column_rows` already F64-aware (DM-3e). u32/int64 grouped paths untouched.

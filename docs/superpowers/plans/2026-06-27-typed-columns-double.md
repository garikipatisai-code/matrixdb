# double (float64) Columns Implementation Plan (DM-3e)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Add `double` columns with scalar aggregation (unfiltered + filtered) and durability via the typed catalog snapshot — mirroring int64. Additive + oracle-safe. Grouped double = DM-3f.

**Spec:** `docs/superpowers/specs/2026-06-27-typed-columns-double-design.md`

---

### Task 1: F64 type + double reducers + engine + dispatch + durability + test

**Files:** Modify `compute.hpp`, `compute_mock.cpp`; Create `test_typed_double.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_typed_double.cpp`:

```cpp
// CPU test for double (float64) columns (DM-3e): MatrixType::F64, matrix_cpu_reduce_all_f64 / _pred,
// matrix_pred_match_f64 (incl. NaN), load_scan_column_f64, execute_query scalar dispatch, durability.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <bit>
#include <cmath>
#include <limits>
#include <vector>
#include <iostream>

static bool refm(double v, MatrixCmp c, double a, double b) {
    switch (c) { case MatrixCmp::GT: return v>a; case MatrixCmp::GE: return v>=a; case MatrixCmp::LT: return v<a;
                 case MatrixCmp::LE: return v<=a; case MatrixCmp::EQ: return v==a; case MatrixCmp::NE: return v!=a;
                 case MatrixCmp::BETWEEN: return v>=a && v<=b; } return false; }
static double ref_reduce(const std::vector<double>& v, bool filt, MatrixCmp c, double a, double b, MatrixAggOp op) {
    double cnt=0, sum=0, mn=std::numeric_limits<double>::infinity(), mx=-std::numeric_limits<double>::infinity();
    for (double x : v) if (!filt || refm(x,c,a,b)) { cnt+=1; sum+=x; if (x<mn) mn=x; if (x>mx) mx=x; }
    switch (op) { case AGG_SUM: return sum; case AGG_MIN: return mn; case AGG_MAX: return mx; case AGG_COUNT: default: return cnt; } }

// Exactly-representable doubles + matching order -> == is exact.
static const std::vector<double> V = {1.5, -3.0, 0.5, 2.25, 5000000000.0, -0.25, 100.0, -100.0};

static void test_reduce_f64() {
    for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX})
        assert(matrix_cpu_reduce_all_f64(V.data(), V.size(), op) == ref_reduce(V, false, MatrixCmp::GT, 0, 0, op));
    const std::pair<MatrixCmp, std::pair<double,double>> cs[] = {
        {MatrixCmp::LT,{0.0,0}}, {MatrixCmp::GE,{0.5,0}}, {MatrixCmp::EQ,{1.5,0}}, {MatrixCmp::BETWEEN,{-3.0,2.25}} };
    for (auto& c : cs) for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX}) {
        MatrixPredicateF64 p{c.first, c.second.first, c.second.second};
        assert(matrix_cpu_reduce_pred_f64(V.data(), V.size(), p, op) == ref_reduce(V, true, c.first, c.second.first, c.second.second, op)); }
    // empty sentinels
    assert(matrix_cpu_reduce_all_f64(nullptr, 0, AGG_MIN) == std::numeric_limits<double>::infinity());
    assert(matrix_cpu_reduce_all_f64(nullptr, 0, AGG_MAX) == -std::numeric_limits<double>::infinity());
    std::cout << "[reduce f64] ok\n";
}

static void test_pred_match_f64() {
    assert(matrix_pred_match_f64(1.6, {MatrixCmp::GT, 1.5, 0}) && !matrix_pred_match_f64(1.5, {MatrixCmp::GT, 1.5, 0}));
    assert(matrix_pred_match_f64(-0.25, {MatrixCmp::BETWEEN, -3.0, 0.0}) && !matrix_pred_match_f64(0.5, {MatrixCmp::BETWEEN, -3.0, 0.0}));
    const double nan = std::numeric_limits<double>::quiet_NaN();
    assert(!matrix_pred_match_f64(nan, {MatrixCmp::GT, 0, 0}) && !matrix_pred_match_f64(nan, {MatrixCmp::LE, 0, 0})
           && !matrix_pred_match_f64(nan, {MatrixCmp::EQ, nan, 0}) && matrix_pred_match_f64(nan, {MatrixCmp::NE, 0, 0}));
    std::cout << "[pred match f64] ok\n";
}

static void test_engine_f64() {
    CPUMockEngine eng;
    eng.load_scan_column_f64(7, V.data(), V.size());
    assert(eng.column_type(7) == MatrixType::F64);
    for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX}) {
        MatrixQuery q{}; q.value_col = 7; q.agg = op; std::vector<uint64_t> o;
        assert(eng.execute_query(q, o) == MatrixQueryStatus::OK && o.size() == 1);
        assert(std::bit_cast<double>(o[0]) == ref_reduce(V, false, MatrixCmp::GT, 0, 0, op) && "f64 scalar == oracle"); }
    // filtered (value > 0)
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_SUM; q.has_filter = true; q.cmp = MatrixCmp::GT; q.lo_f64 = 0.0;
      std::vector<uint64_t> o; eng.execute_query(q, o);
      assert(std::bit_cast<double>(o[0]) == ref_reduce(V, true, MatrixCmp::GT, 0, 0, AGG_SUM)); }
    // fractional value survives (non-vacuity: a uint32/int64 path would truncate 1.5)
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_MIN; q.has_filter = true; q.cmp = MatrixCmp::GT; q.lo_f64 = 0.0;
      std::vector<uint64_t> o; eng.execute_query(q, o); assert(std::bit_cast<double>(o[0]) == 0.5); }
    // grouped double still rejected (DM-3f)
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_COUNT; q.grouped = true; q.key_col = 7; q.num_groups = 2;
      std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_UNSUPPORTED_TYPE); }
    std::cout << "[engine f64] ok\n";
}

static void test_f64_durability() {
    const std::string path = "/tmp/mdb_f64_catalog.bin"; std::remove(path.c_str());
    std::vector<uint32_t> u = {1, 2, 3};
    { CPUMockEngine eng; eng.load_scan_column(3, u.data(), u.size()); eng.load_scan_column_f64(7, V.data(), V.size()); eng.save_catalog(path); }
    { CPUMockEngine eng; eng.load_catalog(path);
      assert(eng.column_type(7) == MatrixType::F64 && eng.column_type(3) == MatrixType::U32 && "types restored");
      MatrixQuery q{}; q.value_col = 7; q.agg = AGG_SUM; std::vector<uint64_t> o; eng.execute_query(q, o);
      assert(std::bit_cast<double>(o[0]) == ref_reduce(V, false, MatrixCmp::GT, 0, 0, AGG_SUM) && "f64 column survived restart"); }
    std::remove(path.c_str());
    std::cout << "[f64 durability] ok\n";
}

int main() { test_reduce_f64(); test_pred_match_f64(); test_engine_f64(); test_f64_durability();
    std::cout << "ALL TYPED-DOUBLE TESTS PASSED\n"; return 0; }
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_typed_double.cpp -o /tmp/ttd && /tmp/ttd` → FAIL to compile (`MatrixType::F64`, `matrix_cpu_reduce_all_f64`, `MatrixPredicateF64`, `load_scan_column_f64`, `MatrixQuery::lo_f64` undeclared).

- [ ] **Step 3: compute.hpp** — Add `#include <limits>` (after `<cstdint>`). Change `MatrixType` to the 3-value enum `{ U32 = 0, I64, F64 }`. Insert `matrix_cpu_reduce_all_f64`, `MatrixPredicateF64`, `matrix_pred_match_f64`, `matrix_cpu_reduce_pred_f64` (spec §2 verbatim) after the int64 reducers (`matrix_cpu_reduce_pred_i64`). Add `double lo_f64 = 0.0; double hi_f64 = 0.0;` to `MatrixQuery` after `hi_i64`.

- [ ] **Step 4: compute_mock.cpp** — Add `#include <bit>` (near the other includes, top of file). Add `load_scan_column_f64` (mirror `load_scan_column_i64`); `scan_tiered_column_f64` (mirror `scan_tiered_column_i64`, double reduce); update `column_rows`'s width to treat I64 **and** F64 as 8 bytes; add the F64 branch in `execute_query` (spec §3, `std::bit_cast<uint64_t>` result); add the F64 branch in `load_catalog` (spec §3). (`save_catalog` unchanged.)

- [ ] **Step 5: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_typed_double.cpp -o /tmp/ttd && /tmp/ttd` → PASS: `[reduce f64] ok`, `[pred match f64] ok`, `[engine f64] ok`, `[f64 durability] ok`, `ALL TYPED-DOUBLE TESTS PASSED`. Zero warnings.

- [ ] **Step 6: Confirm no regression** — `execute_query`, `load_catalog`, `column_rows`, `MatrixType` changed, so these MUST still pass unmodified:
  - `for t in test_typed_columns test_typed_predicates test_typed_grouped test_typed_snapshot test_catalog_snapshot test_query test_query_predicates test_query_validation test_group_by test_live_tiering; do clang++ -std=c++20 -O2 $t.cpp -o /tmp/$t 2>/dev/null && /tmp/$t | tail -1; done`
  - `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`.
  If any differ, STOP / report BLOCKED.

- [ ] **Step 7: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add compute.hpp compute_mock.cpp test_typed_double.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: double (float64) columns (DM-3e) — scalar aggregation (filtered+unfiltered) + durability"
```

---

### Task 2: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (27 tests).**
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by \
         test_query test_observability test_column_io test_catalog_snapshot test_query_validation \
         test_transactions test_server test_security test_audit test_csv_ingest test_checkpoint \
         test_query_predicates test_typed_columns test_typed_predicates test_typed_grouped test_typed_snapshot test_typed_double; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 27× `PASS:`. If any fail, STOP / report BLOCKED with `cat /tmp/out_<test>`.

- [ ] **Step 2: Notebook** — add `"test_typed_double.cpp"` to `make_notebook.py` SOURCES right after `"test_typed_snapshot.cpp"`; add a run cell after the typed-catalog-snapshot run cell (the one compiling `test_typed_snapshot.cpp` to `/tmp/tts`):
```python
    md("### double (float64) columns\n"
       "load_scan_column_f64 registers a 64-bit float column; execute_query aggregates it "
       "(COUNT/SUM/MIN/MAX, filtered + unfiltered) and it survives a restart — fractional real data."),
    code("!clang++ -std=c++20 -O2 test_typed_double.cpp -o /tmp/ttd 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_typed_double.cpp -o /tmp/ttd; /tmp/ttd"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 46 source files embedded`. Verify `grep -o "test_typed_double.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed double-columns test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** F64 enum + double reducers + predicate + MatrixQuery fields (§2)→T1S3; load_scan_column_f64 + scan_tiered_column_f64 + column_rows + execute_query + load_catalog F64 (§3)→T1S4; reduce + NaN + engine scalar/filtered + durability + grouped-rejected + non-vacuity (§4)→T1S1; regression + oracle (§4)→T1S6; suite+notebook→T2. ✓
**Placeholders:** none. **Type consistency:** `MatrixType::F64`, `matrix_cpu_reduce_all_f64`/`_pred_f64`, `MatrixPredicateF64{cmp,a,b}` (double), `matrix_pred_match_f64`, `load_scan_column_f64`, `scan_tiered_column_f64(id,MatrixPredicateF64,op,has_filter)`, `MatrixQuery.lo_f64/.hi_f64`, result via `std::bit_cast<uint64_t>`/`<double>` — consistent. Empty sentinels MIN +inf / MAX -inf. `column_rows` width 8 for I64 AND F64. `save_catalog` unchanged (type-generic); `load_catalog` gains F64 branch. Oracle path + u32/int64 reducers untouched. Includes: `<limits>` in compute.hpp, `<bit>` in compute_mock.cpp.

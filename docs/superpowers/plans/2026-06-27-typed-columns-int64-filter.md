# int64 Filtered Aggregation Implementation Plan (DM-3b)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** int64 columns support filtered scalar aggregation (`WHERE value <cmp> bound`, signed bounds via new `lo_i64`/`hi_i64`); int64 scans drive the rebalance cadence (shared `maybe_rebalance()`). Grouped int64 stays deferred (DM-3c).

**Spec:** `docs/superpowers/specs/2026-06-27-typed-columns-int64-filter-design.md`

---

### Task 1: int64 predicate + reducer + maybe_rebalance + filtered dispatch + test

**Files:** Modify `compute.hpp`, `compute_mock.cpp`; Create `test_typed_predicates.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_typed_predicates.cpp`:

```cpp
// CPU test for int64 filtered aggregation (DM-3b): MatrixPredicateI64, matrix_pred_match_i64,
// matrix_cpu_reduce_pred_i64, execute_query int64 filtered dispatch, and int64 scans driving rebalance.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

// Independent signed predicate (explicit ops; does NOT call matrix_pred_match_i64).
static bool ref_match(int64_t v, MatrixCmp c, int64_t a, int64_t b) {
    switch (c) {
        case MatrixCmp::GT: return v > a;   case MatrixCmp::GE: return v >= a;
        case MatrixCmp::LT: return v < a;   case MatrixCmp::LE: return v <= a;
        case MatrixCmp::EQ: return v == a;  case MatrixCmp::NE: return v != a;
        case MatrixCmp::BETWEEN: return v >= a && v <= b;
    }
    return false;
}
static int64_t ref_reduce(const std::vector<int64_t>& v, MatrixCmp c, int64_t a, int64_t b, MatrixAggOp op) {
    int64_t cnt = 0, sum = 0, mn = INT64_MAX, mx = INT64_MIN;
    for (int64_t x : v) if (ref_match(x, c, a, b)) { ++cnt; sum += x; if (x < mn) mn = x; if (x > mx) mx = x; }
    switch (op) { case AGG_SUM: return sum; case AGG_MIN: return mn; case AGG_MAX: return mx;
                  case AGG_COUNT: default: return cnt; }
}

static void test_pred_match_i64() {
    assert(matrix_pred_match_i64(-4, {MatrixCmp::GT, -5, 0}) && !matrix_pred_match_i64(-5, {MatrixCmp::GT, -5, 0}));
    assert(matrix_pred_match_i64(-1, {MatrixCmp::LT, 0, 0}) && !matrix_pred_match_i64(0, {MatrixCmp::LT, 0, 0}));
    assert(matrix_pred_match_i64(-10, {MatrixCmp::BETWEEN, -10, 10}) && matrix_pred_match_i64(10, {MatrixCmp::BETWEEN, -10, 10}));
    assert(!matrix_pred_match_i64(-11, {MatrixCmp::BETWEEN, -10, 10}) && !matrix_pred_match_i64(11, {MatrixCmp::BETWEEN, -10, 10}));
    assert(matrix_pred_match_i64(5000000000LL, {MatrixCmp::GT, 4000000000LL, 0})
           && !matrix_pred_match_i64(3000000000LL, {MatrixCmp::GT, 4000000000LL, 0}));   // > UINT32_MAX bound
    assert(matrix_pred_match_i64(7, {MatrixCmp::GE, 7, 0}) && matrix_pred_match_i64(7, {MatrixCmp::LE, 7, 0})
           && matrix_pred_match_i64(7, {MatrixCmp::EQ, 7, 0}) && !matrix_pred_match_i64(7, {MatrixCmp::NE, 7, 0}));
    std::cout << "[pred match i64] ok\n";
}

static void test_reduce_pred_i64() {
    const std::vector<int64_t> v = {-7, 0, 5, 5000000000LL, -3, 2147483648LL, 100, -100, 5};
    const std::pair<MatrixCmp, std::pair<int64_t,int64_t>> cases[] = {
        {MatrixCmp::GT,{0,0}}, {MatrixCmp::LT,{0,0}}, {MatrixCmp::GE,{5,0}}, {MatrixCmp::LE,{-3,0}},
        {MatrixCmp::EQ,{5,0}}, {MatrixCmp::NE,{5,0}}, {MatrixCmp::BETWEEN,{-7,100}}, {MatrixCmp::GT,{4000000000LL,0}} };
    for (auto& cs : cases)
        for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX}) {
            MatrixPredicateI64 p{cs.first, cs.second.first, cs.second.second};
            assert(matrix_cpu_reduce_pred_i64(v.data(), v.size(), p, op)
                   == ref_reduce(v, cs.first, cs.second.first, cs.second.second, op));
        }
    std::cout << "[reduce pred i64] ok\n";
}

static void test_engine_i64_filtered() {
    const std::vector<int64_t> v = {-7, 0, 5, 5000000000LL, -3, 2147483648LL, 100, -100, 5};
    CPUMockEngine eng;
    eng.load_scan_column_i64(7, v.data(), v.size());
    struct Case { MatrixCmp c; int64_t a, b; MatrixAggOp op; };
    const Case cases[] = {
        {MatrixCmp::LT, 0, 0, AGG_COUNT}, {MatrixCmp::EQ, 5, 0, AGG_COUNT}, {MatrixCmp::GE, 100, 0, AGG_SUM},
        {MatrixCmp::BETWEEN, -7, 100, AGG_SUM}, {MatrixCmp::GT, 4000000000LL, 0, AGG_MAX} };
    for (auto& cs : cases) {
        MatrixQuery q{}; q.value_col = 7; q.agg = cs.op; q.has_filter = true;
        q.cmp = cs.c; q.lo_i64 = cs.a; q.hi_i64 = cs.b;
        std::vector<uint64_t> out;
        assert(eng.execute_query(q, out) == MatrixQueryStatus::OK && out.size() == 1);
        assert(static_cast<int64_t>(out[0]) == ref_reduce(v, cs.c, cs.a, cs.b, cs.op) && "int64 filtered matches oracle");
    }
    // Non-vacuity: a > UINT32_MAX bound is honored as int64 (only 5000000000 exceeds it -> MAX is it).
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_MAX; q.has_filter = true; q.cmp = MatrixCmp::GT; q.lo_i64 = 4000000000LL;
      std::vector<uint64_t> o; eng.execute_query(q, o); assert(static_cast<int64_t>(o[0]) == 5000000000LL); }
    // EQ differs from GT (predicate actually applied).
    { MatrixQuery eq{}; eq.value_col = 7; eq.agg = AGG_COUNT; eq.has_filter = true; eq.cmp = MatrixCmp::EQ; eq.lo_i64 = 5;
      MatrixQuery gt{}; gt.value_col = 7; gt.agg = AGG_COUNT; gt.has_filter = true; gt.cmp = MatrixCmp::GT; gt.lo_i64 = 5;
      std::vector<uint64_t> a, b; eng.execute_query(eq, a); eng.execute_query(gt, b); assert(a[0] != b[0]); }
    // Grouped int64 still rejected (DM-3c).
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_COUNT; q.grouped = true; q.key_col = 7; q.num_groups = 2;
      std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_UNSUPPORTED_TYPE); }
    std::cout << "[engine i64 filtered] ok\n";
}

static void test_i64_drives_rebalance() {
    std::vector<int64_t> v(1000, 1);
    CPUMockEngine eng;
    eng.load_scan_column_i64(7, v.data(), v.size());
    for (int i = 0; i < 5; ++i) {   // > REBALANCE_EVERY (4) int64 scalar queries
        MatrixQuery q{}; q.value_col = 7; q.agg = AGG_SUM; std::vector<uint64_t> o; eng.execute_query(q, o);
    }
    assert(eng.stats().rebalances >= 1 && "int64 scans drive the rebalance cadence (DM-3a follow-up)");
    std::cout << "[i64 drives rebalance] ok\n";
}

int main() {
    test_pred_match_i64();
    test_reduce_pred_i64();
    test_engine_i64_filtered();
    test_i64_drives_rebalance();
    std::cout << "ALL TYPED-PREDICATE TESTS PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_typed_predicates.cpp -o /tmp/ttp && /tmp/ttp` → FAIL to compile (`MatrixPredicateI64`, `matrix_pred_match_i64`, `matrix_cpu_reduce_pred_i64`, `MatrixQuery::lo_i64/hi_i64` undeclared). (It will also fail the rebalance assert until the int64 path calls `maybe_rebalance()`.)

- [ ] **Step 3: Add the int64 predicate + reducer to compute.hpp** — Insert `MatrixPredicateI64`, `matrix_pred_match_i64`, `matrix_cpu_reduce_pred_i64` (spec §2, verbatim) immediately after `matrix_cpu_reduce_all_i64` (the DM-3a reducer). Add `lo_i64`/`hi_i64` to `MatrixQuery` after the `upper` field (spec §2 verbatim).

- [ ] **Step 4: Wire the engine (compute_mock.cpp)** — per spec §3:
  - Extract the rebalance trigger from `scan_tiered_column` (its `if (++scans_since_rebalance_ >= REBALANCE_EVERY) {...}` block) into a private `void maybe_rebalance() {...}` (spec §3 verbatim); replace the inline block in `scan_tiered_column` with `maybe_rebalance();`.
  - Generalize `scan_tiered_column_i64` signature to `(uint64_t col_id, MatrixPredicateI64 pred, MatrixAggOp op, bool has_filter = false)`; change its reduce line to `const int64_t result = has_filter ? matrix_cpu_reduce_pred_i64(vals, nvals, pred, op) : matrix_cpu_reduce_all_i64(vals, nvals, op);`; add `maybe_rebalance();` immediately before `return result;`.
  - Update the `execute_query` I64 branch (spec §3 verbatim): reject only `q.grouped`; scalar (filtered or not) routes to `scan_tiered_column_i64(q.value_col, MatrixPredicateI64{q.cmp, q.lo_i64, q.hi_i64}, q.agg, q.has_filter)`.

- [ ] **Step 5: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_typed_predicates.cpp -o /tmp/ttp && /tmp/ttp` → PASS: `[pred match i64] ok`, `[reduce pred i64] ok`, `[engine i64 filtered] ok`, `[i64 drives rebalance] ok`, `ALL TYPED-PREDICATE TESTS PASSED`. Zero warnings.

- [ ] **Step 6: Confirm no regression** — `scan_tiered_column` (rebalance extraction), `scan_tiered_column_i64` (signature), and `execute_query` changed. NOTE: `test_typed_columns.cpp` (DM-3a) asserts filtered int64 → `ERR_UNSUPPORTED_TYPE` at its line 44-45 — DM-3b makes that work, so that ONE assertion must be updated (replace it with a filtered-works assert using `lo_i64`; keep the grouped-rejection case). The spec's backward-compat note anticipated this ("the test's filter rejection updates"). After that edit, these MUST pass:
  - `for t in test_typed_columns test_live_tiering test_aggregations test_group_by test_query test_query_predicates test_query_validation test_observability test_server test_catalog_snapshot; do clang++ -std=c++20 -O2 $t.cpp -o /tmp/$t 2>/dev/null && /tmp/$t | tail -1; done`
  - `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`.
  If any differ, STOP / report BLOCKED (esp. `test_typed_columns` — its unfiltered int64 path now goes through the generalized signature with `has_filter=false`; and `test_live_tiering` — the rebalance extraction must be byte-identical).

- [ ] **Step 7: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add compute.hpp compute_mock.cpp test_typed_predicates.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: int64 filtered aggregation (DM-3b) — signed predicates over int64 columns + shared maybe_rebalance"
```

---

### Task 2: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (24 tests).**
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by \
         test_query test_observability test_column_io test_catalog_snapshot test_query_validation \
         test_transactions test_server test_security test_audit test_csv_ingest test_checkpoint \
         test_query_predicates test_typed_columns test_typed_predicates; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 24× `PASS:`. If any fail, STOP / report BLOCKED with `cat /tmp/out_<test>`.

- [ ] **Step 2: Notebook** — add `"test_typed_predicates.cpp"` to `make_notebook.py` SOURCES right after `"test_typed_columns.cpp"`; add a run cell after the int64 typed-columns run cell (the one compiling `test_typed_columns.cpp` to `/tmp/ttyp`):
```python
    md("### int64 filtered aggregation\n"
       "int64 columns now support WHERE GT/GE/LT/LE/EQ/NE/BETWEEN with signed 64-bit bounds "
       "(negatives and values beyond uint32) — verified per-operator against brute-force oracles."),
    code("!clang++ -std=c++20 -O2 test_typed_predicates.cpp -o /tmp/ttp 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_typed_predicates.cpp -o /tmp/ttp; /tmp/ttp"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 43 source files embedded`. Verify `grep -o "test_typed_predicates.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed int64 filtered-aggregation test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** MatrixPredicateI64 + matrix_pred_match_i64 + matrix_cpu_reduce_pred_i64 (§2)→T1S3; MatrixQuery lo_i64/hi_i64 (§2)→T1S3; maybe_rebalance extraction + scan_tiered_column_i64 generalization + execute_query dispatch (§3)→T1S4; pred match + reduce + engine filtered (incl. >UINT32_MAX + negative bounds) + grouped-rejected + rebalance-fix (§4)→T1S1; regression of the 10 affected tests + oracle (§4)→T1S6; suite+notebook→T2. ✓
**Placeholders:** none — `maybe_rebalance` body is given verbatim and is the exact extracted block. **Type consistency:** `MatrixPredicateI64{cmp,a,b}` (int64 a/b), `matrix_pred_match_i64(v,pred)`, `matrix_cpu_reduce_pred_i64(v,n,pred,op)->int64_t`, `scan_tiered_column_i64(id, MatrixPredicateI64, op, has_filter)`, `MatrixQuery.lo_i64/.hi_i64`, `maybe_rebalance()` — consistent across spec §2/§3 and plan T1S3/S4 and the test. `server.hpp` serialization untouched (new fields not serialized). Oracle path + `matrix_cpu_reduce*`/`matrix_cpu_reduce_all_i64` unchanged.

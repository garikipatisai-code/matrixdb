# Richer Scan Predicates Implementation Plan — QRY-3

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Generalize the query WHERE clause from `value > threshold` to GT/GE/LT/LE/EQ/NE/BETWEEN, threaded through the catalog query paths only — additive and oracle-safe (default `GT`).

**Spec:** `docs/superpowers/specs/2026-06-27-richer-predicates-design.md`

---

### Task 1: predicate type + reducers + query wiring + test

**Files:** Modify `compute.hpp`, `compute_mock.cpp`; Create `test_query_predicates.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_query_predicates.cpp`:

```cpp
// CPU test for richer scan predicates (QRY-3): MatrixCmp / MatrixPredicate, matrix_cpu_reduce_pred,
// and execute_query with GE/LT/LE/EQ/NE/BETWEEN. Oracles compute the comparison EXPLICITLY (not via
// matrix_pred_match) so a bug in the predicate dispatch is caught independently.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

// Independent predicate evaluation (explicit ops; does NOT call matrix_pred_match).
static bool ref_match(uint32_t v, MatrixCmp c, uint32_t a, uint32_t b) {
    switch (c) {
        case MatrixCmp::GT: return v > a;   case MatrixCmp::GE: return v >= a;
        case MatrixCmp::LT: return v < a;   case MatrixCmp::LE: return v <= a;
        case MatrixCmp::EQ: return v == a;  case MatrixCmp::NE: return v != a;
        case MatrixCmp::BETWEEN: return v >= a && v <= b;
    }
    return false;
}
static uint64_t ref_reduce(const std::vector<uint32_t>& v, MatrixCmp c, uint32_t a, uint32_t b, MatrixAggOp op) {
    uint64_t cnt = 0, sum = 0, mn = UINT64_MAX, mx = 0;
    for (uint32_t x : v) if (ref_match(x, c, a, b)) { ++cnt; sum += x; if (x < mn) mn = x; if (x > mx) mx = x; }
    switch (op) { case AGG_SUM: return sum; case AGG_MIN: return mn; case AGG_MAX: return mx;
                  case AGG_COUNT: default: return cnt; }
}

static void test_pred_match() {
    assert(matrix_pred_match(6, {MatrixCmp::GT, 5, 0}) && !matrix_pred_match(5, {MatrixCmp::GT, 5, 0}));
    assert(matrix_pred_match(5, {MatrixCmp::GE, 5, 0}) && !matrix_pred_match(4, {MatrixCmp::GE, 5, 0}));
    assert(matrix_pred_match(4, {MatrixCmp::LT, 5, 0}) && !matrix_pred_match(5, {MatrixCmp::LT, 5, 0}));
    assert(matrix_pred_match(5, {MatrixCmp::LE, 5, 0}) && !matrix_pred_match(6, {MatrixCmp::LE, 5, 0}));
    assert(matrix_pred_match(5, {MatrixCmp::EQ, 5, 0}) && !matrix_pred_match(6, {MatrixCmp::EQ, 5, 0}));
    assert(matrix_pred_match(6, {MatrixCmp::NE, 5, 0}) && !matrix_pred_match(5, {MatrixCmp::NE, 5, 0}));
    assert(matrix_pred_match(3, {MatrixCmp::BETWEEN, 3, 7}) && matrix_pred_match(7, {MatrixCmp::BETWEEN, 3, 7}));
    assert(!matrix_pred_match(2, {MatrixCmp::BETWEEN, 3, 7}) && !matrix_pred_match(8, {MatrixCmp::BETWEEN, 3, 7}));
    std::cout << "[pred match] ok\n";
}

static void test_reduce_pred() {
    const std::vector<uint32_t> v = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 5, 5};
    const std::pair<MatrixCmp, std::pair<uint32_t,uint32_t>> cases[] = {
        {MatrixCmp::GT,{5,0}}, {MatrixCmp::GE,{5,0}}, {MatrixCmp::LT,{5,0}}, {MatrixCmp::LE,{5,0}},
        {MatrixCmp::EQ,{5,0}}, {MatrixCmp::NE,{5,0}}, {MatrixCmp::BETWEEN,{3,7}}, {MatrixCmp::LT,{1,0}} };
    for (auto& cs : cases)
        for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX}) {
            MatrixPredicate p{cs.first, cs.second.first, cs.second.second};
            assert(matrix_cpu_reduce_pred(v.data(), v.size(), p, op)
                   == ref_reduce(v, cs.first, cs.second.first, cs.second.second, op));
        }
    // MAX-0 caveat: LT 1 matches only the value 0 -> MAX returns 0; COUNT distinguishes (==1, not empty).
    MatrixPredicate lt1{MatrixCmp::LT, 1, 0};
    assert(matrix_cpu_reduce_pred(v.data(), v.size(), lt1, AGG_MAX) == 0);
    assert(matrix_cpu_reduce_pred(v.data(), v.size(), lt1, AGG_COUNT) == 1);
    std::cout << "[reduce pred] ok\n";
}

static void test_execute_query_scalar() {
    std::vector<uint32_t> v(200);
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<uint32_t>(i % 50);   // 0..49 repeating
    CPUMockEngine eng;
    eng.load_scan_column(2, v.data(), v.size());
    struct Case { MatrixCmp c; uint32_t a, b; MatrixAggOp op; };
    const Case cases[] = {
        {MatrixCmp::LT, 10, 0, AGG_COUNT}, {MatrixCmp::EQ, 7, 0, AGG_COUNT}, {MatrixCmp::NE, 7, 0, AGG_SUM},
        {MatrixCmp::BETWEEN, 20, 30, AGG_SUM}, {MatrixCmp::GE, 45, 0, AGG_MIN}, {MatrixCmp::LE, 5, 0, AGG_MAX} };
    for (auto& cs : cases) {
        MatrixQuery q{}; q.value_col = 2; q.agg = cs.op; q.has_filter = true;
        q.cmp = cs.c; q.threshold = cs.a; q.upper = cs.b;
        std::vector<uint64_t> out;
        assert(eng.execute_query(q, out) == MatrixQueryStatus::OK && out.size() == 1);
        assert(out[0] == ref_reduce(v, cs.c, cs.a, cs.b, cs.op) && "scalar predicate query matches oracle");
    }
    // Backward-compat: default cmp (GT) == old value>threshold.
    MatrixQuery old_style{}; old_style.value_col = 2; old_style.agg = AGG_COUNT; old_style.has_filter = true; old_style.threshold = 25;
    std::vector<uint64_t> o1; eng.execute_query(old_style, o1);
    assert(o1[0] == ref_reduce(v, MatrixCmp::GT, 25, 0, AGG_COUNT) && "default cmp is GT");
    // Non-vacuity: EQ 25 differs from GT 25.
    MatrixQuery eq{}; eq.value_col = 2; eq.agg = AGG_COUNT; eq.has_filter = true; eq.cmp = MatrixCmp::EQ; eq.threshold = 25;
    std::vector<uint64_t> o2; eng.execute_query(eq, o2);
    assert(o2[0] != o1[0] && "EQ is actually applied, not treated as GT");
    std::cout << "[execute_query scalar] ok\n";
}

static void test_execute_query_grouped() {
    std::vector<uint32_t> keys(300), vals(300);
    for (size_t i = 0; i < 300; ++i) { keys[i] = static_cast<uint32_t>(i % 4); vals[i] = static_cast<uint32_t>(i % 60); }
    CPUMockEngine eng;
    eng.load_scan_column(1, keys.data(), keys.size());
    eng.load_scan_column(2, vals.data(), vals.size());
    MatrixQuery q{}; q.value_col = 2; q.key_col = 1; q.num_groups = 4; q.agg = AGG_SUM;
    q.grouped = true; q.has_filter = true; q.cmp = MatrixCmp::BETWEEN; q.threshold = 20; q.upper = 40;
    std::vector<uint64_t> out;
    assert(eng.execute_query(q, out) == MatrixQueryStatus::OK && out.size() == 4);
    uint64_t ref[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < 300; ++i) if (vals[i] >= 20 && vals[i] <= 40) ref[keys[i]] += vals[i];
    for (int g = 0; g < 4; ++g) assert(out[g] == ref[g] && "grouped BETWEEN matches oracle");
    std::cout << "[execute_query grouped] ok\n";
}

int main() {
    test_pred_match();
    test_reduce_pred();
    test_execute_query_scalar();
    test_execute_query_grouped();
    std::cout << "ALL PREDICATE TESTS PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_query_predicates.cpp -o /tmp/tqp && /tmp/tqp` → FAIL to compile (`MatrixCmp`, `MatrixPredicate`, `matrix_pred_match`, `matrix_cpu_reduce_pred`, `MatrixQuery::cmp/upper` undeclared).

- [ ] **Step 3: Add the predicate type + reducer to compute.hpp** — Insert `MatrixCmp`, `MatrixPredicate`, `matrix_pred_match`, `matrix_cpu_reduce_pred` (spec §2, verbatim) immediately AFTER `matrix_cpu_reduce_all` (after line 142) and BEFORE the `MatrixQuery` struct.

- [ ] **Step 4: Extend MatrixQuery + generalize the group reducer** — In `compute.hpp`:
  - In `struct MatrixQuery`, after the `uint32_t threshold = 0;` line, add:
    ```cpp
    MatrixCmp   cmp        = MatrixCmp::GT;   // comparison op for the filter (default keeps value>threshold)
    uint32_t    upper      = 0;               // BETWEEN's inclusive upper bound (ignored for other ops)
    ```
  - Change `matrix_group_reduce_impl`'s parameter `uint32_t threshold` → `const MatrixPredicate& pred`, and its filter line `if constexpr (Filtered) { if (v <= threshold) continue; }` → `if constexpr (Filtered) { if (!matrix_pred_match(v, pred)) continue; }`.
  - Update the two existing wrappers and add the new one (spec §2):
    ```cpp
    inline void matrix_cpu_group_reduce(const uint32_t* keys, const uint32_t* values, size_t n,
                                        uint32_t num_groups, MatrixAggOp op, uint64_t* out) {
        matrix_group_reduce_impl<false>(keys, values, n, num_groups, op, MatrixPredicate{}, out);
    }
    inline void matrix_cpu_group_reduce_where(const uint32_t* keys, const uint32_t* values, size_t n,
                                              uint32_t num_groups, MatrixAggOp op, uint32_t threshold, uint64_t* out) {
        matrix_group_reduce_impl<true>(keys, values, n, num_groups, op, MatrixPredicate{MatrixCmp::GT, threshold, 0}, out);
    }
    inline void matrix_cpu_group_reduce_pred(const uint32_t* keys, const uint32_t* values, size_t n,
                                             uint32_t num_groups, MatrixAggOp op, const MatrixPredicate& pred, uint64_t* out) {
        matrix_group_reduce_impl<true>(keys, values, n, num_groups, op, pred, out);
    }
    ```

- [ ] **Step 5: Wire the engine (compute_mock.cpp)** — 
  - `scan_tiered_column` (line 425): change the signature from `(uint64_t col_id, uint32_t threshold, MatrixAggOp op, bool has_filter = true)` to `(uint64_t col_id, MatrixPredicate pred, MatrixAggOp op, bool has_filter = true)`, and the reduce line (440-441) from `matrix_cpu_reduce(vals, nvals, threshold, op)` to `matrix_cpu_reduce_pred(vals, nvals, pred, op)` (the `matrix_cpu_reduce_all` else-branch is unchanged).
  - `execute_scan` id>0 caller (line 359): `c = scan_tiered_column(col_id, MatrixPredicate{MatrixCmp::GT, threshold, 0}, op);`
  - `execute_query` scalar (line 295): `out.assign(1, scan_tiered_column(q.value_col, MatrixPredicate{q.cmp, q.threshold, q.upper}, q.agg, q.has_filter));`
  - Add `grouped_aggregate_pred` beside `grouped_aggregate_where` (full body verbatim — it is `grouped_aggregate_where`'s body with the final call swapped to `matrix_cpu_group_reduce_pred`):
    ```cpp
    // GROUP BY key WHERE <predicate> — same double borrow-and-return as grouped_aggregate_where.
    void grouped_aggregate_pred(uint64_t key_id, uint64_t value_id, uint32_t num_groups,
                                MatrixAggOp op, const MatrixPredicate& pred, std::vector<uint64_t>& out) {
        assert(key_id != value_id && "group-by key and value must be distinct columns");
        TieredColumn& kc = *catalog_.at(key_id);
        TieredColumn& vc = *catalog_.at(value_id);
        assert(kc.size_bytes() == vc.size_bytes() && "key and value columns must be the same length");
        tier_mgr_.record_access(key_id, kc.size_bytes());
        tier_mgr_.record_access(value_id, vc.size_bytes());
        const MemorySpace kh = kc.tier(); if (kh != MemorySpace::HOST) { ++cold_borrows_; kc.migrate_to(MemorySpace::HOST); }
        const MemorySpace vh = vc.tier(); if (vh != MemorySpace::HOST) { ++cold_borrows_; vc.migrate_to(MemorySpace::HOST); }
        const uint32_t* keys = reinterpret_cast<const uint32_t*>(kc.host_ptr());
        const uint32_t* vals = reinterpret_cast<const uint32_t*>(vc.host_ptr());
        const size_t n = kc.size_bytes() / sizeof(uint32_t);
        out.resize(num_groups);
        matrix_cpu_group_reduce_pred(keys, vals, n, num_groups, op, pred, out.data());
        if (vh != MemorySpace::HOST) vc.migrate_to(vh);
        if (kh != MemorySpace::HOST) kc.migrate_to(kh);
    }
    ```
  - `grouped_aggregate_where` (line 261): keep its signature, REPLACE its whole body (lines 263-277, the asserts/borrow/reduce/return) with a single delegation so the borrow logic lives once:
    ```cpp
    void grouped_aggregate_where(uint64_t key_id, uint64_t value_id, uint32_t num_groups,
                                 MatrixAggOp op, uint32_t threshold, std::vector<uint64_t>& out) {
        grouped_aggregate_pred(key_id, value_id, num_groups, op, MatrixPredicate{MatrixCmp::GT, threshold, 0}, out);
    }
    ```
  - `execute_query` grouped+filtered (line 292): `if (q.has_filter) grouped_aggregate_pred(q.key_col, q.value_col, q.num_groups, q.agg, MatrixPredicate{q.cmp, q.threshold, q.upper}, out);`

- [ ] **Step 6: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_query_predicates.cpp -o /tmp/tqp && /tmp/tqp` → PASS: `[pred match] ok`, `[reduce pred] ok`, `[execute_query scalar] ok`, `[execute_query grouped] ok`, `ALL PREDICATE TESTS PASSED`. Zero warnings.

- [ ] **Step 7: Confirm no regression** — the catalog reduce + group paths changed, so these MUST still pass unmodified:
  - `for t in test_query test_group_by test_aggregations test_query_validation test_live_tiering; do clang++ -std=c++20 -O2 $t.cpp -o /tmp/$t 2>/dev/null && /tmp/$t | tail -1; done`
  - `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`.
  If any differ, STOP / report BLOCKED.

- [ ] **Step 8: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add compute.hpp compute_mock.cpp test_query_predicates.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: richer scan predicates (QRY-3) — WHERE supports GT/GE/LT/LE/EQ/NE/BETWEEN over catalog columns"
```

---

### Task 2: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (22 tests).**
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by \
         test_query test_observability test_column_io test_catalog_snapshot test_query_validation \
         test_transactions test_server test_security test_audit test_csv_ingest test_checkpoint \
         test_query_predicates; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 22× `PASS:`. If any fail, STOP / report BLOCKED with `cat /tmp/out_<test>`.

- [ ] **Step 2: Notebook** — add `"test_query_predicates.cpp"` to `make_notebook.py` SOURCES right after `"test_checkpoint.cpp"`; add a run cell after the WAL-checkpoint run cell (the one compiling `test_checkpoint.cpp` to `/tmp/tckpt`):
```python
    md("### Richer scan predicates\n"
       "execute_query's WHERE clause supports GT / GE / LT / LE / EQ / NE / BETWEEN over catalog "
       "columns (not just value>threshold) — verified per-operator against brute-force oracles."),
    code("!clang++ -std=c++20 -O2 test_query_predicates.cpp -o /tmp/tqp 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_query_predicates.cpp -o /tmp/tqp; /tmp/tqp"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 41 source files embedded`. Verify `grep -o "test_query_predicates.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed richer-predicates test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** MatrixCmp/MatrixPredicate/matrix_pred_match/matrix_cpu_reduce_pred (§2)→T1S3; MatrixQuery cmp+upper + group reducer generalization (§2)→T1S4; engine wiring (§3)→T1S5; per-op + scalar + grouped + backward-compat + non-vacuity (§4)→T1S1; regression of the 5 catalog-path tests + oracle (§4)→T1S7; suite+notebook→T2. ✓
**Placeholders:** none — `grouped_aggregate_pred`'s full body is given verbatim in T1S5 (it is `grouped_aggregate_where`'s exact borrow code with the final call swapped), and `grouped_aggregate_where` becomes a one-line delegate. **Type consistency:** `MatrixPredicate{cmp,a,b}`, `matrix_pred_match(v,pred)`, `matrix_cpu_reduce_pred(v,n,pred,op)`, `matrix_cpu_group_reduce_pred(...,pred,...)`, `scan_tiered_column(col_id, MatrixPredicate, op, has_filter)`, `grouped_aggregate_pred(...,pred,out)`, `MatrixQuery.cmp/.upper` — consistent across spec §2/§3 and plan T1S3-S5 and the test. Oracle path (`matrix_cpu_reduce`, line 357 id-0) untouched.

# Design: Unified Query API — QRY-1 (DM-4 lite)

**Status:** approved-by-standing-directive, pre-implementation. **Date:** 2026-06-26.
**Builds on:** AGG-1 (`matrix_cpu_reduce`), GBY-1/2 (`grouped_aggregate`, `grouped_aggregate_where`), INT-1 (tiered catalog, `scan_tiered_column`).

**Thesis:** *Five increments built scalar/grouped × filtered/unfiltered aggregation as four
separate entry points. A real client wants one: `execute_query(MatrixQuery)`. Unify them behind a
single composable descriptor — the API a query planner targets — resolving the historical
filter/aggregate conflation cleanly and without disturbing the oracle.*

---

## 1. Scope

**IN:**
- `struct MatrixQuery { uint64_t value_col; MatrixAggOp agg; bool has_filter; uint32_t threshold;
  bool grouped; uint64_t key_col; uint32_t num_groups; }` (compute.hpp) — a structured analytical
  query over catalog columns. `has_filter` ⇒ `WHERE value > threshold`; `grouped` ⇒ `GROUP BY key_col`.
- `matrix_cpu_reduce_all(const uint32_t* v, size_t n, MatrixAggOp op) -> uint64_t` (compute.hpp) —
  the **unfiltered** scalar reduction (aggregate ALL values), the one primitive the four existing
  paths lack (`matrix_cpu_reduce` always applies `value > threshold`). COUNT→n, SUM→Σv, MIN/MAX→over
  all (empty→sentinel only when n==0).
- `scan_tiered_column` gains a trailing `bool has_filter = true` param: routes to `matrix_cpu_reduce`
  (filtered) or `matrix_cpu_reduce_all` (unfiltered). The default preserves `execute_scan`'s call
  byte-for-byte (oracle untouched — and the oracle uses the *legacy id-0* path, which doesn't even
  go through `scan_tiered_column`).
- `CPUMockEngine::execute_query(const MatrixQuery& q, std::vector<uint64_t>& out)` — a thin
  dispatcher routing the 4 cases to existing primitives:
  | grouped | has_filter | routes to | out |
  |---|---|---|---|
  | no | no | `scan_tiered_column(value_col, 0, agg, /*has_filter=*/false)` | 1 value |
  | no | yes | `scan_tiered_column(value_col, threshold, agg, /*has_filter=*/true)` | 1 value |
  | yes | no | `grouped_aggregate(key_col, value_col, num_groups, agg, out)` | num_groups |
  | yes | yes | `grouped_aggregate_where(key_col, value_col, num_groups, agg, threshold, out)` | num_groups |
- `test_query.cpp` — all 4 routes over loaded catalog columns vs oracle; non-vacuity; over a
  COLD-demoted column.

**OUT (deferred):** SQL text parsing; multi-aggregate / multi-key; richer predicates (range,
equality); the legacy fixed column as a query target (it stays the benchmark fixture — `execute_query`
operates on catalog columns, `value_col`/`key_col` > 0); a query planner / cost-based routing; GPU.

---

## 2. The unfiltered scalar reducer

```cpp
// Unfiltered scalar reduction (aggregate ALL values; the no-WHERE companion to matrix_cpu_reduce).
// COUNT -> n; SUM -> Σv (u64); MIN/MAX over all values (empty sentinels — MIN UINT64_MAX, MAX 0 —
// only reachable when n==0). Kept separate from matrix_cpu_reduce (which always filters value>threshold)
// rather than templated, because matrix_cpu_reduce dispatches one tight loop per op; a second tiny
// function is clearer than threading `if constexpr` through four loops.
inline uint64_t matrix_cpu_reduce_all(const uint32_t* v, size_t n, MatrixAggOp op) {
    switch (op) {
        case AGG_SUM: { uint64_t s = 0; for (size_t i = 0; i < n; ++i) s += v[i]; return s; }
        case AGG_MIN: { uint64_t m = UINT64_MAX; for (size_t i = 0; i < n; ++i) if (v[i] < m) m = v[i]; return m; }
        case AGG_MAX: { uint64_t m = 0; for (size_t i = 0; i < n; ++i) if (v[i] > m) m = v[i]; return m; }
        case AGG_COUNT:
        default:      return n;
    }
}
```

## 3. Dispatch & the oracle-safety argument

`scan_tiered_column(col_id, threshold, op, bool has_filter = true)` selects the reducer:
`has_filter ? matrix_cpu_reduce(vals, n, threshold, op) : matrix_cpu_reduce_all(vals, n, op)`. The
borrow-and-return, `record_access`, and rebalance-trigger are unchanged. `execute_scan` calls it
without the new arg ⇒ `has_filter` defaults true ⇒ identical behavior. The pipeline oracle
(`83886070`) is the legacy id-0 path in `execute_scan` (uses `matrix_cpu_reduce` directly, untouched)
— doubly safe.

`execute_query` asserts `value_col != 0` (and `key_col != 0` when grouped) — catalog columns only.
For scalar results, `out` holds a single value; for grouped, `num_groups` values.

---

## 4. Verification (`test_query.cpp`, CPU)

Load a value column `value[i]=i` (N=1000) and a key column `key[i]=i%G` (G=8) into the catalog.
- **Scalar, no filter:** `execute_query({value_col, AGG_SUM, has_filter=false, ...})` → `out[0] ==
  Σ_{0..N-1} i` (closed form). COUNT→N, MIN→0, MAX→N-1.
- **Scalar, filter (value > T):** matches `matrix_cpu_reduce` oracle (e.g. SUM of i>T). And the
  filtered SUM differs from the unfiltered SUM (non-vacuity: proves `has_filter` is honored).
- **Grouped, no filter:** equals a brute-force per-group reference.
- **Grouped, filter:** equals a brute-force per-group reference with the `value > T` predicate.
- **Over a COLD column:** demote the value column (scan a dummy hot), run a scalar query → correct
  (the borrow path through `scan_tiered_column` works for the unfiltered route too).
- **Oracle/regression:** `83886070` unchanged; all existing tests green; notebook regenerated.

---

## 5. Open / deferred
- A demo in `main` exercising `execute_query` end-to-end (load → auto-tier → query → print) so the
  analytical stack is visible in the live product — the natural QRY-2 follow-up.
- SQL parser, multi-agg/multi-key, range predicates, planner, GPU query execution (Colab).

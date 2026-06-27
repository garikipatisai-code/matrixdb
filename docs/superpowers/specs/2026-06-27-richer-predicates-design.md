# Design: Richer Scan Predicates — QRY-3

**Status:** approved-by-standing-directive (goal: continue all phases), pre-implementation. **Date:** 2026-06-27.
**Builds on:** AGG-1 (`matrix_cpu_reduce`), GBY-2 (`matrix_group_reduce_impl<Filtered>`), QRY-1 (`MatrixQuery`/`execute_query`), VAL-1.
**Fully local: pure compute, no transport.**

**Thesis:** *The query layer's WHERE clause supports exactly one comparison — `value > threshold`. Real
analytics needs `<`, `>=`, `<=`, `==`, `!=`, and range (`BETWEEN`). Generalize the filter to a small
comparison predicate, threaded only through the catalog query paths (`execute_query`). Additive and
oracle-safe: `MatrixQuery` gains a comparison op defaulting to `GT`, so every existing query — and the
id-0 benchmark/oracle path, which never touches the new code — is byte-identical.*

---

## 1. Scope

**IN (`compute.hpp` + `compute_mock.cpp` + new `test_query_predicates.cpp`):**
- `enum class MatrixCmp : uint32_t { GT = 0, GE, LT, LE, EQ, NE, BETWEEN };` — `GT == 0` so it is the
  default (a query that only sets `threshold` keeps `value > threshold`).
- `struct MatrixPredicate { MatrixCmp cmp = MatrixCmp::GT; uint32_t a = 0; uint32_t b = 0; };` — `a` is
  the primary bound (the threshold for the binary ops; the **inclusive lower** for BETWEEN); `b` is the
  **inclusive upper** for BETWEEN (unused otherwise).
- `bool matrix_pred_match(uint32_t v, const MatrixPredicate&)` — the comparison dispatch.
- `uint64_t matrix_cpu_reduce_pred(const uint32_t* v, size_t n, const MatrixPredicate&, MatrixAggOp)` —
  general filtered scalar reduce (the predicate-aware sibling of `matrix_cpu_reduce`; same empty-set
  sentinels). `matrix_cpu_reduce` (the GT fast path used by the id-0 oracle scan) is **left untouched**.
- Generalize `matrix_group_reduce_impl<Filtered>` to take a `MatrixPredicate` instead of a bare
  `threshold` (the `if constexpr (Filtered)` branch calls `matrix_pred_match`). The unfiltered path
  (`Filtered == false`) is byte-identical (predicate unused). Wrappers: `matrix_cpu_group_reduce`
  (unchanged sig) and `matrix_cpu_group_reduce_where(…, threshold, …)` (unchanged sig — maps to
  `{GT, threshold}`, so its behavior is byte-identical) stay; add
  `matrix_cpu_group_reduce_pred(…, const MatrixPredicate&, …)`.
- `MatrixQuery` gains `MatrixCmp cmp = MatrixCmp::GT;` and `uint32_t upper = 0;` (BETWEEN's inclusive
  upper). `threshold` remains the primary bound. `has_filter` still gates whether the predicate applies.
- `CPUMockEngine`: `scan_tiered_column` takes a `MatrixPredicate` in place of `threshold` (PRIVATE —
  only two internal callers: `execute_scan` id>0 passes `{GT, threshold}`, `execute_query` passes the
  query's predicate). Add `grouped_aggregate_pred(key, value, num_groups, op, MatrixPredicate, out)`;
  `grouped_aggregate_where(…, threshold, …)` keeps its signature and delegates to it with `{GT, threshold}`.
  `execute_query` builds `MatrixPredicate{q.cmp, q.threshold, q.upper}` and routes the filtered scalar /
  filtered grouped cases to the predicate paths.

**Oracle / backward-compat invariants:**
- The id-0 scan (`execute_scan` → `matrix_cpu_reduce(scan_column_, …)`, the 83886070 oracle and the
  benchmark) is on a code path QRY-3 does not modify. Byte-identical.
- Every existing `MatrixQuery` (no `cmp` set → `GT`) and every existing public method signature
  (`grouped_aggregate_where`, `matrix_cpu_group_reduce*`, `matrix_cpu_reduce`) is unchanged, so
  `test_query`, `test_query_validation`, `test_group_by`, `test_aggregations`, `test_live_tiering`
  pass as-is. (id>0 scans now flow through `matrix_cpu_reduce_pred({GT,…})`, whose GT result is
  identical to `matrix_cpu_reduce`.)

**MAX empty-sentinel caveat (inherited):** the empty-match sentinels stay as today — COUNT/SUM → 0,
MIN → `UINT64_MAX`, MAX → 0. Under `GT` (matches ≥ 1) MAX=0 unambiguously meant "no match". With
predicates that can match the value 0 (e.g. `EQ 0`, `LT 1`, `LE 0`, `BETWEEN 0..k`), a MAX result of 0
means "empty OR the max matched value is 0"; callers needing to distinguish read COUNT. This is the
pre-existing sentinel convention extended, documented here and in code — not a new behavior.

**OUT (deferred):** predicates on the GROUP-BY key (only the value is filtered); IN-list / set
membership; multiple ANDed/ORed predicates (one comparison per query); predicates on the legacy id-0
scan (it stays GT-only — it is the benchmark fixture); float/signed comparison (rides DM-3 typed columns).

---

## 2. compute.hpp additions

```cpp
enum class MatrixCmp : uint32_t { GT = 0, GE, LT, LE, EQ, NE, BETWEEN }; // GT == 0 == the default

// A value predicate for WHERE. `a` = primary bound (threshold; inclusive lower for BETWEEN);
// `b` = inclusive upper for BETWEEN (ignored otherwise). uint32 to match the column element type.
struct MatrixPredicate { MatrixCmp cmp = MatrixCmp::GT; uint32_t a = 0; uint32_t b = 0; };

inline bool matrix_pred_match(uint32_t v, const MatrixPredicate& p) {
    switch (p.cmp) {
        case MatrixCmp::GE:      return v >= p.a;
        case MatrixCmp::LT:      return v <  p.a;
        case MatrixCmp::LE:      return v <= p.a;
        case MatrixCmp::EQ:      return v == p.a;
        case MatrixCmp::NE:      return v != p.a;
        case MatrixCmp::BETWEEN: return v >= p.a && v <= p.b;   // inclusive [a, b]
        case MatrixCmp::GT:
        default:                 return v >  p.a;
    }
}

// Predicate-aware filtered scalar reduce — the general sibling of matrix_cpu_reduce. Same empty-set
// sentinels (COUNT/SUM 0, MIN UINT64_MAX, MAX 0; see the MAX caveat in the design). matrix_cpu_reduce
// (the GT fast path) is intentionally left untouched so the id-0 oracle scan is byte-identical.
inline uint64_t matrix_cpu_reduce_pred(const uint32_t* v, size_t n, const MatrixPredicate& p, MatrixAggOp op) {
    switch (op) {
        case AGG_SUM: { uint64_t s = 0; for (size_t i = 0; i < n; ++i) if (matrix_pred_match(v[i], p)) s += v[i]; return s; }
        case AGG_MIN: { uint64_t m = UINT64_MAX; for (size_t i = 0; i < n; ++i) if (matrix_pred_match(v[i], p) && v[i] < m) m = v[i]; return m; }
        case AGG_MAX: { uint64_t m = 0; for (size_t i = 0; i < n; ++i) if (matrix_pred_match(v[i], p) && v[i] > m) m = v[i]; return m; }
        case AGG_COUNT:
        default:      { uint64_t c = 0; for (size_t i = 0; i < n; ++i) c += matrix_pred_match(v[i], p); return c; }
    }
}
```

`matrix_group_reduce_impl` changes its `uint32_t threshold` parameter to `const MatrixPredicate& pred`,
and the filter line `if (v <= threshold) continue;` becomes `if (!matrix_pred_match(v, pred)) continue;`.
Wrappers:

```cpp
inline void matrix_cpu_group_reduce(const uint32_t* keys, const uint32_t* values, size_t n,
                                    uint32_t num_groups, MatrixAggOp op, uint64_t* out) {
    matrix_group_reduce_impl<false>(keys, values, n, num_groups, op, MatrixPredicate{}, out);  // pred unused
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

`MatrixQuery` gains two fields (after `threshold`):
```cpp
    uint32_t    threshold  = 0;          // primary filter bound (BETWEEN's inclusive lower)
    MatrixCmp   cmp        = MatrixCmp::GT;   // comparison op for the filter (default keeps value>threshold)
    uint32_t    upper      = 0;          // BETWEEN's inclusive upper bound (ignored for other ops)
```

---

## 3. compute_mock.cpp wiring

- `scan_tiered_column(uint64_t col_id, MatrixPredicate pred, MatrixAggOp op, bool has_filter = true)` —
  the reduce line becomes `has_filter ? matrix_cpu_reduce_pred(vals, nvals, pred, op) : matrix_cpu_reduce_all(vals, nvals, op)`.
  - `execute_scan` id>0 caller: `scan_tiered_column(col_id, MatrixPredicate{MatrixCmp::GT, threshold, 0}, op)`.
  - `execute_query` scalar caller: `scan_tiered_column(q.value_col, MatrixPredicate{q.cmp, q.threshold, q.upper}, q.agg, q.has_filter)`.
- `grouped_aggregate_pred(key_id, value_id, num_groups, op, const MatrixPredicate& pred, out)` — same
  double borrow-and-return as `grouped_aggregate_where`, calling `matrix_cpu_group_reduce_pred`.
  `grouped_aggregate_where(…, threshold, …)` delegates to it with `{GT, threshold, 0}` (DRY; signature kept).
- `execute_query` grouped+filtered branch: `grouped_aggregate_pred(q.key_col, q.value_col, q.num_groups, q.agg, MatrixPredicate{q.cmp, q.threshold, q.upper}, out)`.

No validation change: an out-of-range `cmp` falls through `matrix_pred_match`'s `default` to GT
(safe); BETWEEN with `a > b` simply matches nothing (a valid empty result, not an error).

---

## 4. Verification (`test_query_predicates.cpp`, CPU)

- **`matrix_pred_match`**: hand cases for every op — GT(5): 6→T,5→F; GE(5):5→T,4→F; LT(5):4→T,5→F;
  LE(5):5→T,6→F; EQ(5):5→T,6→F; NE(5):6→T,5→F; BETWEEN(3,7): 3→T,7→T,2→F,8→F.
- **`matrix_cpu_reduce_pred`**: on a small known array, every `cmp` × {COUNT,SUM,MIN,MAX} against a
  brute-force loop oracle, including a 0-matching predicate (`LT 1`) to exercise the MAX-0 caveat
  (assert COUNT distinguishes empty from match-is-0).
- **`execute_query` scalar**: catalog column; `cmp = LT`, `EQ`, `NE`, `BETWEEN` each vs a brute oracle
  over the same data; `agg = SUM` and `COUNT`.
- **`execute_query` grouped**: `cmp = BETWEEN` over two aligned columns vs a brute grouped oracle.
- **Backward-compat**: a query with `cmp` defaulted (GT) equals the same query built the old way
  (`has_filter=true, threshold=X`) — both give `value > X`.
- **Non-vacuity**: an `EQ X` query and a `GT X` query over the same column return DIFFERENT results
  (proves the predicate is actually applied, not ignored / always-GT). A `BETWEEN a,b` with `a>b`
  returns the empty-set aggregate (COUNT 0).

Plus: full CPU suite (now 22 tests) + oracle `83886070` unchanged; `test_query`, `test_group_by`,
`test_aggregations`, `test_query_validation`, `test_live_tiering` pass unmodified; notebook regenerated.

---

## 5. Open / deferred
Key-side predicates; IN-lists; multi-predicate AND/OR; predicate pushdown to the GPU scan kernel
(rides the GPU batch); float/signed comparisons (DM-3); a proper MAX "no rows" sentinel that survives
0-matching predicates (today: read COUNT).

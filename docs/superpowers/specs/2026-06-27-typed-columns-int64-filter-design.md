# Design: int64 Filtered Aggregation (DM-3b, second slice of the DM-3 epic)

**Status:** approved-by-standing-directive (goal: continue all phases), pre-implementation. **Date:** 2026-06-27.
**Builds on:** DM-3a (int64 columns + `MatrixType`/`scan_tiered_column_i64`), QRY-3 (`MatrixCmp`/predicate model).
**Fully local: pure compute + a side-map already in place, no transport.**

**Thesis:** *DM-3a gave int64 columns scalar UNFILTERED aggregation; a filtered query (`WHERE value <cmp>
bound`) on an int64 column returns `ERR_UNSUPPORTED_TYPE`. Add signed int64 predicates so int64 columns
support the same GT/GE/LT/LE/EQ/NE/BETWEEN WHERE as uint32. The filter bounds are int64, carried in NEW
parallel `MatrixQuery` fields (`lo_i64`/`hi_i64`) — widening the existing `threshold` would break
`server.hpp`'s field-by-field wire format. Also fix the DM-3a follow-up: extract a shared
`maybe_rebalance()` so int64 scans drive the heat-rebalance cadence, not just uint32 scans. Grouped int64
(mixed key/value widths) stays `ERR_UNSUPPORTED_TYPE` — that is DM-3c.*

---

## 1. Scope (DM-3b)

**IN (`compute.hpp` + `compute_mock.cpp` + new `test_typed_predicates.cpp`):**
- `struct MatrixPredicateI64 { MatrixCmp cmp = MatrixCmp::GT; int64_t a = 0; int64_t b = 0; };`
- `bool matrix_pred_match_i64(int64_t v, const MatrixPredicateI64&)` — signed comparison dispatch
  (mirrors `matrix_pred_match`; `>` etc. are signed so negatives compare correctly).
- `int64_t matrix_cpu_reduce_pred_i64(const int64_t* v, size_t n, const MatrixPredicateI64&, MatrixAggOp)`
  — filtered signed reduce (same empty sentinels as `matrix_cpu_reduce_all_i64`: SUM/COUNT 0, MIN
  INT64_MAX, MAX INT64_MIN).
- `MatrixQuery` gains `int64_t lo_i64 = 0;` (primary bound / BETWEEN lower) and `int64_t hi_i64 = 0;`
  (BETWEEN upper) — used only for I64-column filtering. **Not serialized by `server.hpp`** (which keeps
  its existing `threshold`/etc. fields); over-the-wire int64 filtering is future (deferred), the
  in-process `execute_query` path supports it now.
- `CPUMockEngine`:
  - `void maybe_rebalance()` — extract the existing rebalance trigger (`++scans_since_rebalance_ >=
    REBALANCE_EVERY → executor_.apply(...)`) verbatim into a private helper.
  - `scan_tiered_column` calls `maybe_rebalance()` in place of the inline block (byte-identical behavior).
  - `scan_tiered_column_i64` generalized to `(col_id, MatrixPredicateI64 pred, MatrixAggOp op, bool has_filter)`:
    `has_filter ? matrix_cpu_reduce_pred_i64(...) : matrix_cpu_reduce_all_i64(...)`, and now also calls
    `maybe_rebalance()` before returning (the DM-3a follow-up — int64 scans drive the cadence too).
  - `execute_query` I64 branch: reject only `q.grouped` (→ `ERR_UNSUPPORTED_TYPE`, "grouped int64 = DM-3c");
    scalar (filtered or not) → `scan_tiered_column_i64(q.value_col, MatrixPredicateI64{q.cmp, q.lo_i64, q.hi_i64}, q.agg, q.has_filter)`.

**Oracle / backward-compat invariants:**
- `matrix_cpu_reduce_all_i64` (DM-3a), `matrix_cpu_reduce*` (u32), and the id-0 oracle path are untouched.
- `maybe_rebalance()` is the prior inline code verbatim, so the u32 scan path (`test_live_tiering`,
  `test_aggregations`, the demo) is byte-identical and the oracle stays 83886070.
- `MatrixQuery`'s new fields default 0 and are appended; `server.hpp` serialization is unchanged
  (it never reads `lo_i64`/`hi_i64`), so `test_server`/`test_security`/`test_audit` are unaffected.
- DM-3a's `test_typed_columns` still passes: its unfiltered int64 queries now go through the generalized
  `scan_tiered_column_i64(..., has_filter=false)` (identical result) and its int64-grouped/filter
  rejection updates — see Verification.

**MAX sentinel caveat (signed):** empty-match MAX returns INT64_MIN; ambiguous only if the column
actually contains INT64_MIN (read COUNT to disambiguate). Documented, consistent with the u32 MAX=0 caveat.

**OUT (later slices):** grouped int64 (DM-3c — needs a row-count guard for mixed key/value widths + a
u32-key/int64-value grouped reducer); typed persistence + CSV (DM-3c/d); double (DM-3d); over-the-wire
int64 filtering in `server.hpp`; GPU (DM-3e).

---

## 2. compute.hpp additions

```cpp
// int64 value predicate for WHERE (the signed sibling of MatrixPredicate). a = primary bound / BETWEEN
// lower (inclusive); b = BETWEEN upper (inclusive). Signed comparisons so negatives order correctly.
struct MatrixPredicateI64 { MatrixCmp cmp = MatrixCmp::GT; int64_t a = 0; int64_t b = 0; };

inline bool matrix_pred_match_i64(int64_t v, const MatrixPredicateI64& p) {
    switch (p.cmp) {
        case MatrixCmp::GE:      return v >= p.a;
        case MatrixCmp::LT:      return v <  p.a;
        case MatrixCmp::LE:      return v <= p.a;
        case MatrixCmp::EQ:      return v == p.a;
        case MatrixCmp::NE:      return v != p.a;
        case MatrixCmp::BETWEEN: return v >= p.a && v <= p.b;
        case MatrixCmp::GT:
        default:                 return v >  p.a;
    }
}

// Filtered signed scalar reduce — the int64 sibling of matrix_cpu_reduce_pred. Same empty sentinels as
// matrix_cpu_reduce_all_i64 (SUM/COUNT 0, MIN INT64_MAX, MAX INT64_MIN; the MAX sentinel is ambiguous
// only for a column literally containing INT64_MIN — read COUNT).
inline int64_t matrix_cpu_reduce_pred_i64(const int64_t* v, size_t n, const MatrixPredicateI64& p, MatrixAggOp op) {
    switch (op) {
        case AGG_SUM: { int64_t s = 0; for (size_t i = 0; i < n; ++i) if (matrix_pred_match_i64(v[i], p)) s += v[i]; return s; }
        case AGG_MIN: { int64_t m = INT64_MAX; for (size_t i = 0; i < n; ++i) if (matrix_pred_match_i64(v[i], p) && v[i] < m) m = v[i]; return m; }
        case AGG_MAX: { int64_t m = INT64_MIN; for (size_t i = 0; i < n; ++i) if (matrix_pred_match_i64(v[i], p) && v[i] > m) m = v[i]; return m; }
        case AGG_COUNT:
        default:      { int64_t c = 0; for (size_t i = 0; i < n; ++i) c += matrix_pred_match_i64(v[i], p); return c; }
    }
}
```

`MatrixQuery` gains (after `upper`):
```cpp
    int64_t     lo_i64     = 0;   // int64 filter primary bound / BETWEEN lower (I64 columns; not wire-serialized)
    int64_t     hi_i64     = 0;   // int64 filter BETWEEN upper (I64 columns)
```

---

## 3. compute_mock.cpp wiring

- Extract the rebalance trigger (current `scan_tiered_column` lines 477-483) into:
  ```cpp
  // Every REBALANCE_EVERY scans, run the brain + executor: promote hot (DEVICE inert here), demote the
  // coldest HOST columns to SSD under the budget. Shared by the u32 and int64 scan paths.
  void maybe_rebalance() {
      if (++scans_since_rebalance_ >= REBALANCE_EVERY) {
          std::unordered_map<uint64_t, TieredColumn*> ptrs;
          for (auto& kv : catalog_) ptrs[kv.first] = kv.second.get();
          migrations_ += executor_.apply(tier_mgr_.rebalance(), ptrs);
          ++rebalances_;
          scans_since_rebalance_ = 0;
      }
  }
  ```
  In `scan_tiered_column`, replace the inline block with `maybe_rebalance();`.
- Generalize `scan_tiered_column_i64(uint64_t col_id, MatrixPredicateI64 pred, MatrixAggOp op, bool has_filter = false)`:
  the reduce line becomes `has_filter ? matrix_cpu_reduce_pred_i64(vals, nvals, pred, op) : matrix_cpu_reduce_all_i64(vals, nvals, op)`,
  and add `maybe_rebalance();` before `return result;`.
- `execute_query` I64 branch (currently rejects `grouped || has_filter`):
  ```cpp
  if (column_type(q.value_col) == MatrixType::I64) {
      if (q.grouped) return MatrixQueryStatus::ERR_UNSUPPORTED_TYPE;   // grouped int64 = DM-3c
      out.assign(1, static_cast<uint64_t>(
          scan_tiered_column_i64(q.value_col, MatrixPredicateI64{q.cmp, q.lo_i64, q.hi_i64}, q.agg, q.has_filter)));
      return MatrixQueryStatus::OK;
  }
  ```

---

## 4. Verification (`test_typed_predicates.cpp`, CPU)

- **`matrix_pred_match_i64`**: all 7 ops with negatives and a value > UINT32_MAX — GT(-5): -4→T,-5→F;
  LT(0): -1→T,0→F; BETWEEN(-10,10): -10→T,10→T,-11→F,11→F; EQ/NE/GE/LE endpoints; GT(4000000000): 5e9→T,3e9→F.
- **`matrix_cpu_reduce_pred_i64`**: a known int64 array (negatives + >UINT32_MAX) × every cmp ×
  {COUNT,SUM,MIN,MAX} vs an explicit-comparison hand oracle (independent of `matrix_pred_match_i64`).
- **`execute_query` I64 filtered**: load an int64 column; `cmp = LT/EQ/BETWEEN/GE` with int64 bounds
  (incl. negative and >UINT32_MAX, set via `lo_i64`/`hi_i64`); assert `static_cast<int64_t>(out[0])`
  equals the oracle for SUM and COUNT. **Non-vacuity**: a `GT 4000000000` (int64) filter gives a
  different (correct) result than if the bound were truncated to uint32 — proves int64 bounds flow
  through; an `EQ` differs from a `GT`.
- **Grouped int64 still rejected**: `execute_query` on an int64 column with `grouped = true` →
  `ERR_UNSUPPORTED_TYPE`.
- **Rebalance fix (the DM-3a follow-up)**: with a small `host_cap`, run a hot int64-column workload (>
  `REBALANCE_EVERY` int64 queries) and assert `stats().rebalances > 0` — i.e. int64 scans now drive the
  cadence (non-vacuous: without `maybe_rebalance()` in the int64 path this stays 0). Optionally assert a
  cold int64 column demotes under the budget.

Plus: full CPU suite (now 24 tests) + oracle `83886070` unchanged; `test_typed_columns` (DM-3a),
`test_live_tiering`, `test_aggregations`, `test_query`, `test_query_predicates`, `test_server` pass
unmodified; notebook regenerated.

---

## 5. Open / deferred
DM-3c grouped int64 (row-count guard + u32-key/int64-value grouped reducer) + typed persistence/CSV;
DM-3d double; over-the-wire int64 filtering in `server.hpp`; GPU typed kernels; a signed MAX "no rows"
sentinel robust to INT64_MIN data.

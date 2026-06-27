# Design: Grouped int64 Aggregation (DM-3c, third slice of the DM-3 epic)

**Status:** approved-by-standing-directive. **Date:** 2026-06-27.
**Builds on:** DM-3a/b (int64 columns, `MatrixPredicateI64`, `column_type`), GBY-2 (group reducer), QRY-3.
**Fully local.**

**Thesis:** *int64 columns do scalar + filtered aggregation (DM-3a/b) but `GROUP BY` over an int64 value
returns `ERR_UNSUPPORTED_TYPE`. Add grouped int64: `SELECT key, AGG(int64_value) [WHERE ...] GROUP BY key`
with a **uint32 key** (group ids are dense small ints) and an **int64 value**. This completes int64 query
parity (scalar + filtered + grouped). Two correctness points the uint32 path doesn't have: the int64 MAX
accumulator must init to `INT64_MIN` (values can be negative — `0` would be wrong), and the key/value
length check must compare ROW COUNTS, not byte lengths (an int64 value is 8N bytes vs a uint32 key's 4N
for the same N rows). The int64 key stays rejected (DM-3d).*

---

## 1. Scope

**IN (`compute.hpp` + `compute_mock.cpp` + new `test_typed_grouped.cpp`):**
- `matrix_group_reduce_i64_impl<Filtered>(const uint32_t* keys, const int64_t* values, size_t n, uint32_t num_groups, MatrixAggOp op, const MatrixPredicateI64& pred, int64_t* out)` — dense-group reduction with int64 values/accumulators. Empty-group sentinels: COUNT/SUM 0, MIN `INT64_MAX`, MAX `INT64_MIN`. Wrappers `matrix_cpu_group_reduce_i64` (unfiltered) + `matrix_cpu_group_reduce_i64_pred` (filtered).
- `CPUMockEngine::column_rows(uint64_t id)` — `size_bytes() / element_width(type)` (4 for U32, 8 for I64). The type-aware row count.
- `CPUMockEngine::grouped_aggregate_i64(key_id, value_id, num_groups, op, pred, has_filter, out)` — double borrow-and-return (uint32 key + int64 value), reduce into an `int64_t` temp, copy to `out` as uint64 bit-patterns. Does NOT trigger rebalance (matches `grouped_aggregate` — GROUP BY is not scan-driven).
- `execute_query` I64-value branch: when `q.grouped`, validate (`catalog_has(key)`, `key != value`, `num_groups in [1, MAX]`, key is **U32** → else `ERR_UNSUPPORTED_TYPE`, `column_rows(key) == column_rows(value)` → else `ERR_INVALID_GROUP`) then `grouped_aggregate_i64`; else the existing scalar path.

**Correctness invariants:**
- int64 MAX init `INT64_MIN` (a group of all-negative values yields the correct negative max, not 0).
- Row-count guard (not byte-length) for the int64-value grouped path; the existing **uint32**-value grouped path keeps its byte-length guard unchanged (u32==u32 ⇒ byte length ≡ row count).
- All key/value type combos safe (no reinterpretation): u32val+u32key → u32 path (works); u32val+i64key → u32 path rejects (byte mismatch + key-type check, DM-3a fix); i64val+u32key → i64 grouped (works, NEW); i64val+i64key → i64 branch rejects (key-type check). Result `out` holds int64 group aggregates as uint64 bit-patterns (caller `static_cast<int64_t>`).

**OUT (later):** int64 (or other-typed) GROUP-BY **key** (DM-3d); typed persistence/CSV; double; GPU; over-the-wire grouped int64.

---

## 2. compute.hpp additions

```cpp
// Grouped reduction over an int64 value column keyed by a uint32 key column (dense group ids). Same
// semantics as matrix_group_reduce_impl but values + accumulators are signed int64. Empty-group
// sentinels: COUNT/SUM 0, MIN INT64_MAX, MAX INT64_MIN. NOTE: MAX inits to INT64_MIN (not 0) because
// int64 values may be negative. Filtered applies the int64 predicate.
template <bool Filtered>
inline void matrix_group_reduce_i64_impl(const uint32_t* keys, const int64_t* values, size_t n,
                                         uint32_t num_groups, MatrixAggOp op, const MatrixPredicateI64& pred, int64_t* out) {
    for (uint32_t g = 0; g < num_groups; ++g)
        out[g] = (op == AGG_MIN) ? INT64_MAX : (op == AGG_MAX ? INT64_MIN : 0);
    for (size_t i = 0; i < n; ++i) {
        const uint32_t k = keys[i];
        if (k >= num_groups) continue;
        const int64_t v = values[i];
        if constexpr (Filtered) { if (!matrix_pred_match_i64(v, pred)) continue; }
        switch (op) {
            case AGG_SUM:   out[k] += v; break;
            case AGG_MIN:   if (v < out[k]) out[k] = v; break;
            case AGG_MAX:   if (v > out[k]) out[k] = v; break;
            case AGG_COUNT:
            default:        out[k] += 1; break;
        }
    }
}
inline void matrix_cpu_group_reduce_i64(const uint32_t* keys, const int64_t* values, size_t n,
                                        uint32_t num_groups, MatrixAggOp op, int64_t* out) {
    matrix_group_reduce_i64_impl<false>(keys, values, n, num_groups, op, MatrixPredicateI64{}, out);
}
inline void matrix_cpu_group_reduce_i64_pred(const uint32_t* keys, const int64_t* values, size_t n,
                                             uint32_t num_groups, MatrixAggOp op, const MatrixPredicateI64& pred, int64_t* out) {
    matrix_group_reduce_i64_impl<true>(keys, values, n, num_groups, op, pred, out);
}
```
Place after `matrix_cpu_group_reduce_pred` (the u32 grouped wrappers), so `MatrixPredicateI64`/`matrix_pred_match_i64` are in scope.

---

## 3. compute_mock.cpp wiring

```cpp
// Type-aware row count of a catalog column (U32 = 4 bytes/row, I64 = 8).
size_t column_rows(uint64_t id) const {
    const size_t w = (column_type(id) == MatrixType::I64) ? sizeof(int64_t) : sizeof(uint32_t);
    return catalog_.at(id)->size_bytes() / w;
}

// GROUP BY a uint32 key over an int64 value column (DM-3c). Double borrow-and-return like
// grouped_aggregate; out holds int64 group aggregates as uint64 bit-patterns. No rebalance (GROUP BY
// is not scan-driven, matching grouped_aggregate).
void grouped_aggregate_i64(uint64_t key_id, uint64_t value_id, uint32_t num_groups, MatrixAggOp op,
                           const MatrixPredicateI64& pred, bool has_filter, std::vector<uint64_t>& out) {
    TieredColumn& kc = *catalog_.at(key_id);
    TieredColumn& vc = *catalog_.at(value_id);
    tier_mgr_.record_access(key_id, kc.size_bytes());
    tier_mgr_.record_access(value_id, vc.size_bytes());
    const MemorySpace kh = kc.tier(); if (kh != MemorySpace::HOST) { ++cold_borrows_; kc.migrate_to(MemorySpace::HOST); }
    const MemorySpace vh = vc.tier(); if (vh != MemorySpace::HOST) { ++cold_borrows_; vc.migrate_to(MemorySpace::HOST); }
    const uint32_t* keys = reinterpret_cast<const uint32_t*>(kc.host_ptr());
    const int64_t*  vals = reinterpret_cast<const int64_t*>(vc.host_ptr());
    const size_t n = vc.size_bytes() / sizeof(int64_t);
    std::vector<int64_t> tmp(num_groups);
    if (has_filter) matrix_cpu_group_reduce_i64_pred(keys, vals, n, num_groups, op, pred, tmp.data());
    else            matrix_cpu_group_reduce_i64(keys, vals, n, num_groups, op, tmp.data());
    out.resize(num_groups);
    for (uint32_t g = 0; g < num_groups; ++g) out[g] = static_cast<uint64_t>(tmp[g]);
    if (vh != MemorySpace::HOST) vc.migrate_to(vh);
    if (kh != MemorySpace::HOST) kc.migrate_to(kh);
}
```

`execute_query` I64-value branch — replace the current
`if (q.grouped) return MatrixQueryStatus::ERR_UNSUPPORTED_TYPE;` with:
```cpp
        if (q.grouped) {
            if (!catalog_has(q.key_col) || q.key_col == q.value_col || q.num_groups == 0
                || column_rows(q.key_col) != column_rows(q.value_col))
                return MatrixQueryStatus::ERR_INVALID_GROUP;
            if (column_type(q.key_col) != MatrixType::U32) return MatrixQueryStatus::ERR_UNSUPPORTED_TYPE; // int64 key = DM-3d
            if (q.num_groups > MAX_QUERY_GROUPS) return MatrixQueryStatus::ERR_TOO_MANY_GROUPS;
            grouped_aggregate_i64(q.key_col, q.value_col, q.num_groups, q.agg,
                                  MatrixPredicateI64{q.cmp, q.lo_i64, q.hi_i64}, q.has_filter, out);
            return MatrixQueryStatus::OK;
        }
```
(The scalar `out.assign(1, ...scan_tiered_column_i64...)` stays as the else.)

---

## 4. Verification (`test_typed_grouped.cpp`, CPU)

- **`matrix_group_reduce_i64` / `_pred`**: u32 keys + int64 values incl. **negatives** and **> UINT32_MAX**, every op vs a brute grouped oracle. **MAX-init guard**: a group whose values are ALL NEGATIVE must yield the correct negative max (NOT 0) — the assertion that fails if MAX inits to 0. Empty group → MIN INT64_MAX, MAX INT64_MIN, SUM/COUNT 0. Filtered (BETWEEN with negative bound) vs oracle.
- **Engine grouped int64** via `execute_query`: i64 value (id 7) + u32 key (id 1), `grouped` SUM/MIN/MAX/COUNT, filtered + unfiltered, `static_cast<int64_t>(out[g])` vs a brute oracle (data with negatives + large).
- **Mixed-width adversarial (the DM-3a-class risk)**: i64 value (N rows = 8N bytes) + u32 key (N rows = 4N bytes) — **row counts equal** → grouped works (proves the row-count guard, not byte-length). A row-count MISMATCH (e.g. i64 value of N + u32 key of N+1) → `ERR_INVALID_GROUP`. An **int64 KEY** (i64 value + i64 key) → `ERR_UNSUPPORTED_TYPE` (still rejected). A `u32` value + `i64` key (the DM-3a case) → still rejected.
- **Non-vacuity**: filtered grouped (BETWEEN) ≠ unfiltered grouped; grouped MAX over a negative-only group ≠ 0.

Plus: full CPU suite (now 25 tests) + oracle `83886070`; the u32 group tests (`test_group_by`, `test_query`, `test_query_predicates`), `test_typed_columns`, `test_typed_predicates` pass unmodified; notebook regenerated.

---

## 5. Open / deferred
int64/typed GROUP-BY key (DM-3d); typed persistence + CSV; double; GPU; a MAX "no rows" sentinel robust to INT64_MIN data.

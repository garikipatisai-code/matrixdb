# Design: Grouped double Aggregation (DM-3f, sixth slice of the DM-3 epic)

**Status:** approved-by-standing-directive (continue all phases, don't wait). **Date:** 2026-06-27.
**Builds on:** DM-3c (grouped int64 — the exact template), DM-3e (double columns/`MatrixPredicateF64`/`column_rows` F64).
**Fully local.** Direct mirror of DM-3c with `double` in place of `int64`.

**Thesis:** *double columns do scalar + filtered aggregation (DM-3e) but `GROUP BY` over a double value
returns `ERR_UNSUPPORTED_TYPE`. Add grouped double (`GROUP BY uint32_key, AGG(double_value)`, filtered +
unfiltered), completing double query parity — exactly mirroring DM-3c (grouped int64). MAX inits `-inf`,
the length guard uses `column_rows` (already F64-aware), the double key stays rejected, and the result is
the double bit pattern.*

---

## 1. Scope

**IN (`compute.hpp` + `compute_mock.cpp` + new `test_typed_double_grouped.cpp`):**
- `matrix_group_reduce_f64_impl<Filtered>(const uint32_t* keys, const double* values, size_t n, uint32_t num_groups, MatrixAggOp op, const MatrixPredicateF64& pred, double* out)` — dense-group reduction, double accumulators. Empty-group sentinels: COUNT/SUM 0.0, MIN `+inf`, MAX `-inf`. Wrappers `matrix_cpu_group_reduce_f64` (unfiltered) + `matrix_cpu_group_reduce_f64_pred` (filtered).
- `CPUMockEngine::grouped_aggregate_f64(key_id, value_id, num_groups, op, pred, has_filter, out)` — double borrow-and-return (uint32 key + double value), reduce into a `double` temp, copy to `out` as `std::bit_cast<uint64_t>` per group. (Mirror `grouped_aggregate_i64`.) No rebalance (matches the other grouped paths).
- `execute_query` F64-value branch: when `q.grouped`, validate exactly like the int64-grouped path (`catalog_has(key)` → else fall through; key is **U32** else `ERR_UNSUPPORTED_TYPE`; `key != value`, `num_groups in [1, MAX]`, `column_rows(key) == column_rows(value)` else `ERR_INVALID_GROUP`) then `grouped_aggregate_f64`; else the existing scalar path.

**Invariants (mirror DM-3c):** MAX init `-inf` (a negative-only double group yields the right max, not 0); row-count guard via `column_rows` (F64 value 8N bytes vs u32 key 4N for same N rows — the working grouped test is the proof); double key rejected; key-type check first (guarded by `catalog_has` so an unknown key → `ERR_INVALID_GROUP`). uint32/int64 grouped paths untouched. Result `out[g]` = double group aggregate as a uint64 bit pattern.

**OUT (later):** single-column typed file I/O + typed CSV; GPU; typed GROUP-BY key.

---

## 2. compute.hpp additions

```cpp
// Grouped reduction over a double value column keyed by a uint32 key column (dense group ids). Double
// accumulators; empty-group sentinels COUNT/SUM 0.0, MIN +inf, MAX -inf (MAX inits -inf so a negative
// group yields the right max). Filtered applies the double predicate.
template <bool Filtered>
inline void matrix_group_reduce_f64_impl(const uint32_t* keys, const double* values, size_t n,
                                         uint32_t num_groups, MatrixAggOp op, const MatrixPredicateF64& pred, double* out) {
    for (uint32_t g = 0; g < num_groups; ++g)
        out[g] = (op == AGG_MIN) ? std::numeric_limits<double>::infinity()
               : (op == AGG_MAX ? -std::numeric_limits<double>::infinity() : 0.0);
    for (size_t i = 0; i < n; ++i) {
        const uint32_t k = keys[i];
        if (k >= num_groups) continue;
        const double v = values[i];
        if constexpr (Filtered) { if (!matrix_pred_match_f64(v, pred)) continue; }
        switch (op) {
            case AGG_SUM:   out[k] += v; break;
            case AGG_MIN:   if (v < out[k]) out[k] = v; break;
            case AGG_MAX:   if (v > out[k]) out[k] = v; break;
            case AGG_COUNT:
            default:        out[k] += 1.0; break;
        }
    }
}
inline void matrix_cpu_group_reduce_f64(const uint32_t* keys, const double* values, size_t n,
                                        uint32_t num_groups, MatrixAggOp op, double* out) {
    matrix_group_reduce_f64_impl<false>(keys, values, n, num_groups, op, MatrixPredicateF64{}, out);
}
inline void matrix_cpu_group_reduce_f64_pred(const uint32_t* keys, const double* values, size_t n,
                                             uint32_t num_groups, MatrixAggOp op, const MatrixPredicateF64& pred, double* out) {
    matrix_group_reduce_f64_impl<true>(keys, values, n, num_groups, op, pred, out);
}
```
Place after `matrix_cpu_group_reduce_i64_pred` (the int64 grouped wrappers).

---

## 3. compute_mock.cpp wiring

`grouped_aggregate_f64` — mirror `grouped_aggregate_i64` exactly, with `const double*` value pointer,
`n = vc.size_bytes() / sizeof(double)`, a `std::vector<double> tmp(num_groups)`, the
`matrix_cpu_group_reduce_f64`/`_pred` calls, and `out[g] = std::bit_cast<uint64_t>(tmp[g])`:
```cpp
void grouped_aggregate_f64(uint64_t key_id, uint64_t value_id, uint32_t num_groups, MatrixAggOp op,
                           const MatrixPredicateF64& pred, bool has_filter, std::vector<uint64_t>& out) {
    TieredColumn& kc = *catalog_.at(key_id);
    TieredColumn& vc = *catalog_.at(value_id);
    tier_mgr_.record_access(key_id, kc.size_bytes());
    tier_mgr_.record_access(value_id, vc.size_bytes());
    const MemorySpace kh = kc.tier(); if (kh != MemorySpace::HOST) { ++cold_borrows_; kc.migrate_to(MemorySpace::HOST); }
    const MemorySpace vh = vc.tier(); if (vh != MemorySpace::HOST) { ++cold_borrows_; vc.migrate_to(MemorySpace::HOST); }
    const uint32_t* keys = reinterpret_cast<const uint32_t*>(kc.host_ptr());
    const double*   vals = reinterpret_cast<const double*>(vc.host_ptr());
    const size_t n = vc.size_bytes() / sizeof(double);
    std::vector<double> tmp(num_groups);
    if (has_filter) matrix_cpu_group_reduce_f64_pred(keys, vals, n, num_groups, op, pred, tmp.data());
    else            matrix_cpu_group_reduce_f64(keys, vals, n, num_groups, op, tmp.data());
    out.resize(num_groups);
    for (uint32_t g = 0; g < num_groups; ++g) out[g] = std::bit_cast<uint64_t>(tmp[g]);
    if (vh != MemorySpace::HOST) vc.migrate_to(vh);
    if (kh != MemorySpace::HOST) kc.migrate_to(kh);
}
```

`execute_query` F64-value branch — replace `if (q.grouped) return MatrixQueryStatus::ERR_UNSUPPORTED_TYPE;` with the grouped block (mirror the int64-grouped branch from DM-3c):
```cpp
        if (q.grouped) {
            if (catalog_has(q.key_col) && column_type(q.key_col) != MatrixType::U32)
                return MatrixQueryStatus::ERR_UNSUPPORTED_TYPE;                  // double key = later
            if (!catalog_has(q.key_col) || q.key_col == q.value_col || q.num_groups == 0
                || column_rows(q.key_col) != column_rows(q.value_col))
                return MatrixQueryStatus::ERR_INVALID_GROUP;
            if (q.num_groups > MAX_QUERY_GROUPS) return MatrixQueryStatus::ERR_TOO_MANY_GROUPS;
            grouped_aggregate_f64(q.key_col, q.value_col, q.num_groups, q.agg,
                                  MatrixPredicateF64{q.cmp, q.lo_f64, q.hi_f64}, q.has_filter, out);
            return MatrixQueryStatus::OK;
        }
```
(Note: key-type check FIRST, guarded by `catalog_has` — the DM-3c ordering lesson, so `key==value==f64col` → `ERR_UNSUPPORTED_TYPE` and an unknown key → `ERR_INVALID_GROUP`.)

---

## 4. Verification (`test_typed_double_grouped.cpp`, CPU)

Exactly-representable doubles + matching order (`==` exact). Mirror DM-3c's test:
- **`matrix_cpu_group_reduce_f64` / `_pred`**: u32 keys + double values (incl. a **negative-only group** and a fractional value), every op vs a brute oracle. **MAX-init guard**: the negative-only group's MAX is its (negative) max, NOT 0. Empty group → MIN +inf, MAX -inf.
- **Engine grouped double** via `execute_query`: double value (id 7) + u32 key (id 1), grouped SUM/MIN/MAX/COUNT, filtered + unfiltered, `std::bit_cast<double>(out[g])` vs oracle.
- **Guards**: double KEY (double value + double key) → `ERR_UNSUPPORTED_TYPE`; row-count mismatch → `ERR_INVALID_GROUP`. The working case (double value 8N bytes + u32 key 4N) succeeds (row-count guard, not byte-length).
- **Non-vacuity**: a fractional value contributes correctly (a truncating path would differ); filtered ≠ unfiltered.

Plus: full CPU suite (now 28 tests) + oracle `83886070`; `test_typed_double`, `test_typed_grouped` (int64), `test_group_by`, `test_query*` pass unmodified; notebook regenerated.

---

## 5. Open / deferred
Single-column typed file I/O + typed CSV; GPU; typed GROUP-BY key; double query parity is COMPLETE after this slice (scalar + filtered + grouped + durable).

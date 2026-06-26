# Design: Filtered Grouped Aggregation (WHERE on GROUP BY) — GBY-2

**Status:** approved-by-standing-directive, pre-implementation. **Date:** 2026-06-26.
**Builds on:** GBY-1 (`matrix_cpu_group_reduce`, `grouped_aggregate`), AGG-1 (`MatrixAggOp`).

**Thesis:** *`SELECT key, AGG(value) GROUP BY key` is useful; `… WHERE value > T GROUP BY key` is
the canonical analytical query. Add a predicate to grouped aggregation, consistent with the scalar
scan's `value > threshold` filter — without churning GBY-1 (templated impl + thin wrappers).*

---

## 1. Scope

**IN:**
- A `template <bool Filtered>` internal `group_reduce_impl` in compute.hpp; the existing
  `matrix_cpu_group_reduce` becomes a thin `<false>` wrapper (unchanged signature → zero churn to
  GBY-1 callers/tests), and a new `matrix_cpu_group_reduce_where(keys, values, n, num_groups, op,
  uint32_t threshold, out)` is the `<true>` wrapper (folds only rows with `value > threshold`).
- `CPUMockEngine::grouped_aggregate_where(key_id, value_id, num_groups, op, uint32_t threshold,
  std::vector<uint64_t>& out)` — mirrors `grouped_aggregate` (double borrow-and-return), calling
  the filtered reducer. Rows whose value ≤ threshold do not contribute to any group.
- `test_group_by.cpp` extended: filtered hand-worked oracle + through-engine (incl. a group that
  becomes empty *because* all its rows were filtered out → sentinel).

**OUT (deferred):** range/equality predicates, filter on the key, a unified query struct
(resolving `matrix_cpu_reduce`'s filter+aggregate conflation) — the query layer (DM-4); GPU.

---

## 2. The filtered reducer (no duplication, no runtime branch)

```cpp
template <bool Filtered>
inline void matrix_group_reduce_impl(const uint32_t* keys, const uint32_t* values, size_t n,
                                     uint32_t num_groups, MatrixAggOp op, uint32_t threshold, uint64_t* out) {
    const uint64_t init = (op == AGG_MIN) ? UINT64_MAX : 0;
    for (uint32_t g = 0; g < num_groups; ++g) out[g] = init;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t k = keys[i];
        if (k >= num_groups) continue;                       // out-of-range key
        const uint32_t v = values[i];
        if constexpr (Filtered) { if (v <= threshold) continue; }   // WHERE value > threshold (no branch when !Filtered)
        switch (op) {
            case AGG_SUM:   out[k] += v; break;
            case AGG_MIN:   if (v < out[k]) out[k] = v; break;
            case AGG_MAX:   if (v > out[k]) out[k] = v; break;
            case AGG_COUNT:
            default:        out[k] += 1; break;
        }
    }
}
// Unfiltered (GBY-1's signature, unchanged — now a wrapper):
inline void matrix_cpu_group_reduce(const uint32_t* keys, const uint32_t* values, size_t n,
                                    uint32_t num_groups, MatrixAggOp op, uint64_t* out) {
    matrix_group_reduce_impl<false>(keys, values, n, num_groups, op, /*threshold*/0, out);
}
// Filtered (WHERE value > threshold):
inline void matrix_cpu_group_reduce_where(const uint32_t* keys, const uint32_t* values, size_t n,
                                          uint32_t num_groups, MatrixAggOp op, uint32_t threshold, uint64_t* out) {
    matrix_group_reduce_impl<true>(keys, values, n, num_groups, op, threshold, out);
}
```

`if constexpr (Filtered)` compiles the predicate out entirely for the unfiltered path, so
`matrix_cpu_group_reduce` is byte-identical in behavior to GBY-1 (no perf or result change). The
filtered path's sentinels are identical, so a group whose rows are all filtered out reads as empty
(COUNT/SUM/MAX 0, MIN UINT64_MAX) — consistent with the scalar reducer and unfiltered group-by.

---

## 3. Engine method

`grouped_aggregate_where` is `grouped_aggregate` with a `uint32_t threshold` param and the filtered
reducer call — same double borrow-and-return (capture each home tier, migrate both to HOST, reduce,
return both), same distinct-id + equal-length asserts, same heat recording / no GBY-driven rebalance.

---

## 4. Verification (`test_group_by.cpp`, CPU)

Reuse GBY-1's hand-worked arrays `keys={0,1,0,2,1,0}`, `vals={5,7,9,11,13,15}`, G=3, threshold T=8:
- Rows with value > 8: vals {9,15} (g0), {13} (g1), {11} (g2). So filtered:
  COUNT {2,1,1}, SUM {24,13,11}, MIN {9,13,11}, MAX {15,13,11}. Assert exact (hand-verifiable).
- **Group emptied by the filter:** with T=12, g2's only row (11) is filtered out → COUNT[2]=0,
  SUM[2]=0, MIN[2]=UINT64_MAX, MAX[2]=0 (group becomes empty *via the predicate*, sentinels hold).
- **Unfiltered unchanged:** `matrix_cpu_group_reduce` still gives GBY-1's results (the `<false>`
  wrapper) — re-assert one op to prove no regression.
- **Through the engine:** `grouped_aggregate_where(1, 2, 3, op, T, out)` over loaded columns equals
  the hand-worked filtered oracle; repeat with the value column COLD-demoted (filtered double-borrow).
- **Non-vacuity:** the filtered result differs from the unfiltered (e.g. SUM {24,13,11} ≠ {29,20,11}),
  so a stub ignoring the predicate fails.

Plus: oracle `83886070` unchanged; 10-test suite green; notebook regenerated.

---

## 5. Open / deferred
- Range / equality / key predicates; a unified query struct (DM-4); GPU filtered group-by (Colab).

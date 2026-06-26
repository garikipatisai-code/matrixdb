# Design: Grouped Aggregation (GROUP BY) — GBY-1

**Status:** approved-by-standing-directive (session goal: proceed with ideal choice, no pause), pre-implementation
**Date:** 2026-06-26
**Builds on:** AGG-1 (`MatrixAggOp`, the reducer), INT-1/1b (tiered catalog + borrow-and-return).

**Thesis:** *Ungrouped aggregates are half an analytical engine; the other half is `GROUP BY key →
aggregate(value)` — the dominant analytical operation. Compute per-group COUNT/SUM/MIN/MAX over two
aligned tiered columns (a key and a value), the canonical bandwidth-bound grouped reduction and the
CPU semantics-of-record for the eventual GPU version.*

---

## 1. Scope

**IN:**
- `matrix_cpu_group_reduce(const uint32_t* keys, const uint32_t* values, size_t n, uint32_t num_groups, MatrixAggOp op, uint64_t* out)`
  (compute.hpp): array-of-accumulators grouped reduction. For each row `i`, if `keys[i] < num_groups`,
  fold `values[i]` into `out[keys[i]]` per `op`. `out` (length `num_groups`) is initialized per op by
  the function. Keys ≥ `num_groups` are ignored (out-of-range — documented, not an error).
- Engine method `grouped_aggregate(uint64_t key_id, uint64_t value_id, uint32_t num_groups, MatrixAggOp op, std::vector<uint64_t>& out)`
  on `CPUMockEngine`: borrows both catalog columns to HOST (reusing the borrow-and-return pattern),
  validates, runs `matrix_cpu_group_reduce`, returns the borrows. Records access heat on both
  columns. Result is `num_groups` values in `out`.
- `test_group_by.cpp` — CPU, computable oracles: per-group reductions over known key/value columns
  (incl. empty groups + out-of-range keys), and through the engine (incl. a COLD-demoted column).

**OUT (deferred):**
- A WHERE filter on grouped aggregation (predicate on key/value before grouping) — v1 aggregates
  ALL rows per group; filtered GROUP BY is a later refinement.
- High-cardinality / hash grouping — v1 uses a dense `[0, num_groups)` key space (array of
  accumulators), the common analytical bucket/category case and the natural GPU layout. Hash
  aggregation for sparse keys is deferred.
- GPU grouped-reduction kernel (atomics or sort-based) — Colab follow-up, alongside AGG-2.
- A typed/named schema (DM-2) — GBY-1 designates two existing catalog columns as key/value by id;
  a real `SELECT k, SUM(v) FROM t GROUP BY k` parser is the query layer (DM-4).
- Multiple aggregates per query, multiple group keys — v1 is one key, one value, one op.

---

## 2. The grouped reducer & edge cases

```cpp
inline void matrix_cpu_group_reduce(const uint32_t* keys, const uint32_t* values, size_t n,
                                    uint32_t num_groups, MatrixAggOp op, uint64_t* out) {
    // init per op (empty-group sentinels match the scalar reducer)
    const uint64_t init = (op == AGG_MIN) ? UINT64_MAX : 0; // COUNT/SUM/MAX -> 0, MIN -> UINT64_MAX
    for (uint32_t g = 0; g < num_groups; ++g) out[g] = init;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t k = keys[i];
        if (k >= num_groups) continue;          // out-of-range key: ignored
        const uint32_t v = values[i];
        switch (op) {
            case AGG_SUM:   out[k] += v; break;
            case AGG_MIN:   if (v < out[k]) out[k] = v; break;
            case AGG_MAX:   if (v > out[k]) out[k] = v; break;
            case AGG_COUNT:
            default:        out[k] += 1; break;
        }
    }
}
```

- **Empty group** (no rows): COUNT/SUM/MAX → 0, MIN → `UINT64_MAX` — same sentinels as the scalar
  reducer (`matrix_cpu_reduce`), so the two are consistent.
- **Out-of-range key** (`>= num_groups`): the row is ignored (no bucket exists). Documented.
- **SUM** accumulates in u64 (no overflow for the engine's column sizes).
- No predicate: every in-range row contributes (this is GROUP BY without WHERE).
- One pass, branch-per-op-inside-the-loop here (unlike the scalar reducer's loop-per-op) — grouped
  reduction is scatter-bound (random `out[k]` writes), so the op branch is not the bottleneck; one
  clean loop is the right tradeoff. `ponytail:` if profiling ever shows the branch matters, split
  per op then.

---

## 3. Engine method (borrow both columns)

```cpp
void grouped_aggregate(uint64_t key_id, uint64_t value_id, uint32_t num_groups,
                       MatrixAggOp op, std::vector<uint64_t>& out) {
    assert(key_id != value_id && "group-by key and value must be distinct columns");
    TieredColumn& kc = *catalog_.at(key_id);     // .at throws if absent (caller error)
    TieredColumn& vc = *catalog_.at(value_id);
    assert(kc.size_bytes() == vc.size_bytes() && "key and value columns must be the same length");
    tier_mgr_.record_access(key_id, kc.size_bytes());     // heat on both
    tier_mgr_.record_access(value_id, vc.size_bytes());
    const MemorySpace kh = kc.tier(); if (kh != HOST) kc.migrate_to(HOST);  // borrow both to HOST
    const MemorySpace vh = vc.tier(); if (vh != HOST) vc.migrate_to(HOST);
    const uint32_t* keys = reinterpret_cast<const uint32_t*>(kc.host_ptr());
    const uint32_t* vals = reinterpret_cast<const uint32_t*>(vc.host_ptr());
    const size_t n = kc.size_bytes() / sizeof(uint32_t);
    out.assign(num_groups, 0);
    matrix_cpu_group_reduce(keys, vals, n, num_groups, op, out.data());
    if (vh != HOST) vc.migrate_to(vh);            // return borrows (reverse order; independent files)
    if (kh != HOST) kc.migrate_to(kh);
}
```

- Borrow-and-return on **both** columns keeps residency in lockstep with the brain (same invariant
  as scan). Both are transiently HOST during the reduction; returned to their home tiers after.
- `key_id != value_id` required (a self-group-by is degenerate; avoids double-borrow bookkeeping).
- GROUP BY records heat on both columns but does **not** itself trigger `rebalance()` — rebalancing
  stays scan-driven (v1 simplification; GROUP BY participates in heat, scans drive migration). The
  per-column COLD files are distinct (INT-1b's pid+serial), so both borrowing simultaneously is safe.
- Not on `ComputeInterface` (a CPU-engine capability, like the CPU agg path); the GPU grouped
  kernel is the deferred follow-up.

---

## 4. Verification (`test_group_by.cpp`, CPU)

Use a deterministic key/value pattern with closed-form per-group oracles, e.g. `key[i] = i % G`,
`value[i] = i`, over `n` rows with `G` groups:
- **COUNT[g]** = #{ i<n : i%G==g } = `n/G` (+1 for `g < n%G`).
- **SUM[g]** = Σ{ i<n : i%G==g } = g + (g+G) + (g+2G) + … (closed form, or computed by a reference loop).
- **MIN[g]** = the smallest i with i%G==g = `g` (if g<n); **MAX[g]** = the largest i with i%G==g.

Tests:
- **`matrix_cpu_group_reduce` directly:** COUNT/SUM/MIN/MAX vs a reference brute-force grouping over
  the same arrays (independent of the impl). An **empty group** (a g that no key maps to) → COUNT 0,
  SUM 0, MIN UINT64_MAX, MAX 0. An **out-of-range key** (some key set to `num_groups+5`) is ignored
  (doesn't change any bucket vs a run without it).
- **Non-vacuity:** assert at least two groups have different SUMs (a stub returning a constant, or
  ignoring the key, fails).
- **Through the engine:** `load_scan_column` a key column and a value column; `grouped_aggregate`
  each op; assert `out` equals the brute-force oracle. Repeat after the value column has been
  demoted to COLD (grouped aggregate is correct over a borrowed column too).
- **Validation:** distinct-ids assert and equal-length assert are exercised (debug builds).

Plus: the pipeline oracle stays `83886070`; all existing CPU tests stay green; notebook regenerated
with `test_group_by.cpp` embedded + a run cell.

---

## 5. Open / deferred (not blockers)
- Filtered GROUP BY (WHERE on key/value), multi-key / multi-aggregate — the query layer (DM-4).
- Hash aggregation for high-cardinality / sparse keys (v1 is dense `[0, num_groups)`).
- GPU grouped-reduction kernel (atomics or sort-reduce) — Colab, with AGG-2.
- Typed/named schema + a real GROUP BY parser (DM-2/DM-4).
- GROUP BY-driven rebalancing (v1: heat recorded, migration stays scan-driven).

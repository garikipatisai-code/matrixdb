# Design: Analytical Aggregations over scan columns — AGG-1

**Status:** approved-by-standing-directive (session goal: proceed with ideal choice, no pause), pre-implementation
**Date:** 2026-06-26
**Builds on:** INT-1/INT-1b (live tiered scan-column catalog), the OP_SCAN codec, the u32 scan loop.

**Thesis:** *A scan that can only count is half a query engine. Generalize OP_SCAN from
`count(value > threshold)` to a filtered reduction — `COUNT | SUM | MIN | MAX` over the values
matching the predicate — so the engine computes real analytical aggregates over tiered columns.
Reductions are bandwidth-bound: the GPU's home turf and the next step toward a query layer.*

---

## 1. Scope

**IN:**
- `enum MatrixAggOp { AGG_COUNT=0, AGG_SUM=1, AGG_MIN=2, AGG_MAX=3 }` (types.hpp). `AGG_COUNT=0`
  is the existing behavior, so every current scan is an aggregate with op 0 — backward compatible.
- Codec: `matrix_set_scan_agg_op(q, op)` / `matrix_get_scan_agg_op(q)` — agg op at payload offset
  16 (u32). The existing `matrix_set_scan_target(q, threshold, column_id)` leaves op 0 (COUNT).
- `matrix_cpu_reduce(const uint32_t* v, size_t n, uint32_t threshold, MatrixAggOp op) -> uint64_t`
  (compute.hpp): one tight inner loop per op over the values matching `value > threshold`.
- `execute_scan` routes BOTH the legacy fixed column (id 0) and the tiered catalog (id>0) through
  `matrix_cpu_reduce`. The result lands in `q.transaction_id` (u64), as today.
- `test_aggregations.cpp` — CPU, oracle-computable: COUNT/SUM/MIN/MAX over `value[i]=i` with a
  threshold, on both the legacy column and a tiered catalog column; empty-match-set edge cases.

**OUT (deferred):**
- GPU reduction kernel (parallel block-reduce over device bytes) — the perf path; CPU establishes
  the semantics + oracle first, exactly as the scan did (scalar → u32x4). Colab follow-up.
- GROUP BY, multi-column aggregates, DISTINCT — that's the query layer (DM-4).
- Predicates other than `value > threshold` (>=, <, ==, ranges) — the scan's single predicate is
  unchanged; richer predicates come with the query layer.
- Floating-point / typed values — values stay u32; SUM accumulates in u64 (no overflow for the
  engine's column sizes; see §3).

---

## 2. Codec & dispatch

The payload (32 bytes) currently holds threshold@0 (u32) and column_id@8 (u64). Add agg_op@16
(u32). Offsets are disjoint and aligned. `matrix_set_scan_agg_op(q, op)` writes @16;
`matrix_get_scan_agg_op(q)` reads @16; a query built only with `matrix_set_scan_target` /
`matrix_set_scan_threshold` reads op 0 (COUNT) — the legacy/oracle path is unchanged.

```
execute_scan(q):
  threshold = matrix_get_scan_threshold(q)
  col_id    = matrix_get_scan_column_id(q)
  op        = matrix_get_scan_agg_op(q)          // 0 == COUNT (legacy)
  if col_id == 0:  result = matrix_cpu_reduce(scan_column_.data(), SIZE, threshold, op)
  else:            result = scan_tiered_column(col_id, threshold, op)   // borrows/rebalances, then reduces
  q.transaction_id = result; ++scans_; scan_result_sum_ += result; (time it)
```

`scan_tiered_column` gains an `op` parameter and calls `matrix_cpu_reduce` over the resident
HOST bytes (after the borrow), replacing its inline count loop. Everything else (record_access,
borrow-and-return, rebalance trigger) is unchanged.

---

## 3. The reducer & its edge cases

```cpp
inline uint64_t matrix_cpu_reduce(const uint32_t* v, size_t n, uint32_t threshold, MatrixAggOp op) {
    switch (op) {
        case AGG_SUM: { uint64_t s = 0; for (size_t i=0;i<n;++i) if (v[i] > threshold) s += v[i]; return s; }
        case AGG_MIN: { uint64_t m = UINT64_MAX; for (size_t i=0;i<n;++i) if (v[i] > threshold && v[i] < m) m = v[i]; return m; }
        case AGG_MAX: { uint64_t m = 0; for (size_t i=0;i<n;++i) if (v[i] > threshold && v[i] > m) m = v[i]; return m; }
        case AGG_COUNT:
        default:      { uint64_t c = 0; for (size_t i=0;i<n;++i) c += (v[i] > threshold); return c; }
    }
}
```

- **COUNT** is byte-identical arithmetic to the current loop → the pipeline oracle (`83886070`)
  is preserved (it's COUNT over the fixed column at the midpoint threshold).
- **SUM** accumulates in `uint64_t`. Worst case for the engine's columns: the 16M fixed column,
  `value[i]=i`, sum ≈ 2^24·2^24/2 ≈ 1.4e14 ≪ 2^64. No overflow.
- **Empty match set** (threshold ≥ max value) sentinels — documented, and asserted in tests:
  COUNT→0, SUM→0, MIN→`UINT64_MAX` (no u32 can equal it, so it unambiguously means "no match"),
  MAX→0 (matched values are `> threshold ≥ 0`, i.e. ≥1, so 0 unambiguously means "no match").
- One tight branchy loop per op (dispatch *outside* the loop) — the COUNT loop stays exactly as
  fast as today; no per-element switch.

---

## 4. Verification (`test_aggregations.cpp`, CPU)

`value[i]=i` over `[0,N)` gives closed-form oracles for a threshold T (0 ≤ T < N):
- COUNT = N-1-T;  SUM = Σ_{i=T+1}^{N-1} i = (N-1+T+1)(N-1-T)/2;  MIN = T+1;  MAX = N-1.

Tests:
- **`matrix_cpu_reduce` directly:** all four ops over a known array vs the closed-form oracle.
- **Empty match set:** threshold = N-1 (nothing `> N-1`) → COUNT 0, SUM 0, MIN UINT64_MAX, MAX 0.
- **Through the engine, legacy column (id 0):** build a query with each op, assert the closed-form
  oracle over `MATRIX_SCAN_COLUMN_SIZE`. Confirms the dispatch + codec.
- **Through the engine, tiered column (id>0):** `load_scan_column` a known column, run each op via
  `matrix_set_scan_target` + `matrix_set_scan_agg_op`, assert the oracle — incl. after the column
  has been demoted+borrowed (aggregate is correct regardless of tier).
- **Non-vacuity:** a control that fails without the feature — e.g. assert `SUM != COUNT` for a
  threshold where they'd differ, so a stub returning count for every op is caught.

Plus: the pipeline oracle stays `83886070` (COUNT path unchanged); all 8 existing CPU tests stay
green; notebook regenerated with `test_aggregations.cpp` embedded + a run cell.

---

## 5. Open / deferred (not blockers)

- GPU parallel-reduction kernel over device bytes (block-reduce; the perf path) — Colab follow-up,
  asserting the device result equals `matrix_cpu_reduce` over the same bytes (the cross-backend
  invariant that anchored the scan work).
- GROUP BY / multi-column / DISTINCT — the query layer (DM-4).
- Richer predicates (ranges, equality) — with the query layer.
- AVG — derivable as SUM/COUNT by the caller; not a distinct op (YAGNI).

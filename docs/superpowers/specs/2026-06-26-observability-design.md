# Design: Engine Observability — OB-1

**Status:** approved-by-standing-directive, pre-implementation. **Date:** 2026-06-26.
**Builds on:** INT-1/1b (tiering), QRY-1/2 (queries + live demo).

**Thesis:** *The auto-tiering machinery runs invisibly — you can't operate what you can't see.
Instrument the engine with cheap counters (cold borrows, rebalances, migrations) + resident-bytes
gauges, expose them via `stats()`, and surface them in the live demo. Turns the tiering from
"happening" into "observed and measurable."*

---

## 1. Scope

**IN:**
- `struct EngineStats { uint64_t cold_borrows; uint64_t rebalances; uint64_t migrations;
  size_t catalog_columns; size_t host_resident_bytes; size_t cold_resident_bytes; }` (compute_mock.cpp
  or a small header).
- `CPUMockEngine::stats() const -> EngineStats` — counters + gauges (resident bytes from the
  TierManager, catalog size from the catalog map).
- Counters incremented in the existing tiering paths (additive, behavior-unchanged):
  - `cold_borrows_` — each COLD→HOST borrow (in `scan_tiered_column`, `grouped_aggregate`,
    `grouped_aggregate_where`; grouped borrows two columns → up to 2 per call).
  - `rebalances_` — each `rebalance()` trigger (in `scan_tiered_column`).
  - `migrations_` — `+= executor_.apply(...)` return (decisions actually moved) at each rebalance.
- `analytical_query_demo()` (main.cpp) prints `engine stats` after its workload, making the
  tiering observable (e.g. `cold_borrows=… rebalances=… migrations=… resident HOST/COLD=…`).
- `test_observability.cpp` — drive a known workload, assert the counters/gauges match expectations.

**OUT (deferred):** latency histograms / timing metrics (the pipeline already reports scan timing;
per-query latency is a follow-up); a metrics export format (Prometheus/JSON); logging levels;
counters on the GPU engine (Colab). `const`-correctness note: `stats()` is `const`; the counters
are plain members mutated in the (non-const) op methods.

---

## 2. Counters & gauges

Members on `CPUMockEngine` (init 0): `uint64_t cold_borrows_ = 0, rebalances_ = 0, migrations_ = 0;`

- **cold_borrows_**: in each op, the borrow is `if (home != HOST) { col.migrate_to(HOST); }` — add
  `++cold_borrows_;` inside that branch. (scan_tiered_column: 1 site; grouped_aggregate &
  grouped_aggregate_where: 2 sites each, one per column.)
- **rebalances_ / migrations_**: in `scan_tiered_column`'s rebalance trigger:
  ```
  if (++scans_since_rebalance_ >= REBALANCE_EVERY) {
      auto ptrs = …;
      migrations_ += executor_.apply(tier_mgr_.rebalance(), ptrs);   // was: executor_.apply(...);
      ++rebalances_;
      scans_since_rebalance_ = 0;
  }
  ```
- **gauges** (computed in `stats()`, not stored): `host_resident_bytes = tier_mgr_.resident_bytes(HOST)`,
  `cold_resident_bytes = tier_mgr_.resident_bytes(COLD)`, `catalog_columns = catalog_.size()`.

```cpp
EngineStats stats() const {
    return EngineStats{ cold_borrows_, rebalances_, migrations_, catalog_.size(),
                        tier_mgr_.resident_bytes(MemorySpace::HOST),
                        tier_mgr_.resident_bytes(MemorySpace::COLD) };
}
```

These are O(1) increments on existing paths — zero behavior change, oracle-safe (the legacy id-0
scan path and point-op path don't touch these counters).

---

## 3. Verification (`test_observability.cpp`, CPU)

Reuse the eviction scenario (3 columns, 2-column budget):
- Load 3 columns; initially `stats().catalog_columns == 3`, `cold_resident_bytes == 0`,
  `rebalances == 0`, `migrations == 0`.
- Scan cols 1 & 2 in a loop (never col 3) to drive rebalances + demote col 3. After the loop:
  `rebalances == (#scans / REBALANCE_EVERY)`, `migrations >= 1` (col 3 demoted),
  `cold_resident_bytes == one column's bytes`, `host_resident_bytes == 2 columns' bytes`.
- Scan the now-COLD col 3 once → `cold_borrows` increments by 1 (the borrow).
- A grouped query over two columns where one is COLD → `cold_borrows` increments (the cold one).
- **Non-vacuity:** assert `migrations >= 1` and `cold_borrows >= 1` actually occurred (a no-op
  engine would leave them 0); assert exact `rebalances` count from the known scan count.

Plus: the live demo prints stats (eyeball + the demo's existing asserts unaffected); oracle
`83886070` unchanged; all 11 existing tests green; notebook regenerated.

---

## 4. Open / deferred
- Per-query latency histograms; metrics export (JSON/Prometheus); structured leveled logging;
  GPU-engine counters (Colab). These are OB-2+.

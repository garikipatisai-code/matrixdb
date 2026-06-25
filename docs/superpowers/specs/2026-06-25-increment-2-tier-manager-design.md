# Design: Increment 2 — TierManager (the auto-tiering brain)

**Status:** approved design (brainstorm sections walked + approved), pre-implementation
**Date:** 2026-06-25
**Parent spec:** `2026-06-25-three-tier-storage-engine-design.md` (Increment 2 row of §7; policy detailed in §4)

**Thesis:** *The database decides where each column should live — promoting hot data toward
VRAM and evicting cold data toward SSD — using measured per-tier cost, not guesswork.*

---

## 1. Scope

Build the **decision brain** of the three-tier engine: a `TierManager` that tracks per-
column access heat and, on demand, computes the set of migrations that would lower total
scan cost. It **decides only** — it moves no bytes (Increment 4 executes), does no I/O
(Increment 3 adds the SSD substrate), and is not yet wired into the live query path.

**IN:** `tier_manager.hpp` (heat tracking, cost-benefit promotion, capacity eviction,
anti-thrash) + `test_tier_manager.cpp`. Pure logic, fully CPU-unit-testable.

**OUT (later increments):** byte movement / `cudaMemcpy` (Inc 4); SSD read/write/WAL
(Inc 3); wiring into router/engine query path (after the executor exists); real typed
values; GPU store changes.

---

## 2. Component & interface

One new header-only unit, `tier_manager.hpp`. Pure policy: it owns column metadata and the
placement map, consults the Increment-1 `CostModel`/`tier_model`, and returns decisions.

```cpp
struct MigrationDecision {
    uint64_t column_id;
    MemorySpace from;
    MemorySpace to;
};

class TierManager {
public:
    // capacities: usable bytes per tier. Defaults pulled from tier_physics(); overridable
    // (tests use tiny tiers). SSD/COLD is treated as unbounded (capacity 0 == unbounded).
    TierManager(CostModel cm,
                size_t device_capacity_bytes,
                size_t host_capacity_bytes);

    // Register a column the manager will track, with its size and starting tier.
    void register_column(uint64_t id, size_t bytes, MemorySpace initial_tier);

    // Record that `bytes` of column `id` were scanned. O(1); bumps the column's recent
    // access accumulator. Cheap enough to call on every scan.
    void record_access(uint64_t id, size_t bytes);

    // Global pass: age heat, choose promotions (cost-benefit), evict over-capacity tiers,
    // APPLY each move to the internal placement map, and RETURN the decision list (for the
    // future executor). No bytes move here.
    std::vector<MigrationDecision> rebalance();

    // Current placement of a column (reflects applied rebalance decisions).
    MemorySpace tier_of(uint64_t id) const;

    // Introspection for tests/telemetry.
    double heat_of(uint64_t id) const;
    size_t resident_bytes(MemorySpace tier) const;
};
```

Dependencies: `cost_model.hpp` (Inc 1) and `tier_model.hpp` (Inc 1) only. No engine, no
GPU, no I/O. This is why it is fully unit-testable on the CPU.

---

## 3. The rebalance algorithm (one pass)

All thresholds are documented named constants (calibration targets, like the cost model's).

1. **Age heat.** For every registered column:
   `heat = HEAT_ALPHA * recent_bytes + (1 - HEAT_ALPHA) * heat`, then `recent_bytes = 0`.
   `HEAT_ALPHA = 0.5`. Recency-weighted and bytes-weighted (a large hot column outranks a
   tiny one touched equally often). Advance the global tick counter.

2. **Promotion candidates.** For each column not already on the fastest justified tier,
   consider promoting it **one tier** toward VRAM (COLD→HOST, HOST→DEVICE). Single-tier
   steps are deliberate: a very hot COLD column climbs to DEVICE over successive rebalances
   (COLD→HOST, then HOST→DEVICE), which keeps each pass's moves small and lets capacity
   pressure re-evaluate between steps rather than committing a two-tier jump at once.
   Compute:
   - `est_future_scans` = a heat-derived estimate of upcoming scans (heat normalized by the
     column's byte size → ~scans-per-tick; clamped to a small horizon `SCAN_HORIZON = 8`).
   - `benefit_us = (scan_us(current_tier, bytes) - scan_us(faster_tier, bytes)) * est_future_scans`
   - `cost_us = migration_us(current_tier, faster_tier, bytes)`
   - Promote iff `benefit_us > HYSTERESIS * cost_us` (`HYSTERESIS = 1.5`).
   Order candidates by `benefit_us - cost_us` descending (best first).

3. **Capacity & eviction.** Applying promotions may overfill a bounded tier (DEVICE, HOST).
   For each over-capacity tier, evict the **lowest cost-benefit** resident (not merely the
   coldest) — demote it one tier down (DEVICE→HOST, HOST→COLD) — until the tier fits. A
   column may not be evicted within `MIN_RESIDENCY_TICKS = 2` of arriving on its tier
   (anti-thrash). COLD is unbounded, so demotion to COLD always succeeds.

4. **Emit + apply.** Each promotion/demotion becomes a `MigrationDecision{id, from, to}`;
   apply it to the placement map and the per-tier resident-bytes totals, then return the
   list. Order: process promotions first, then evictions they triggered, so the returned
   list is a valid sequential plan.

Anti-thrash summary: hysteresis margin on promotion + min-residency on eviction prevents a
column from oscillating between tiers across consecutive rebalances.

---

## 4. Verification (`test_tier_manager.cpp`, CPU-only)

Asserts the behaviors that define "a correct brain":

- **Promote hot:** a large, frequently-accessed column starting on COLD/HOST is promoted
  toward DEVICE after enough `record_access` + `rebalance`.
- **Don't promote cold:** a rarely-accessed column stays put (`benefit < HYSTERESIS*cost`).
- **Cost-benefit eviction:** with DEVICE capacity for one column and two competing, the
  higher cost-benefit column wins residency; the other is evicted/demoted — verifying it's
  not pure LRU.
- **Anti-thrash:** a column promoted then left idle is not immediately evicted on the next
  rebalance (min-residency); and a marginal column near the threshold doesn't oscillate.
- **Determinism:** identical access sequence → identical decision list (so the brain is
  reproducible and testable).
- **Placement-map integrity:** `tier_of()` after `rebalance()` matches the applied
  decisions; `resident_bytes(tier)` never exceeds a bounded tier's capacity post-rebalance.

Test prints `PASS: tier manager decisions correct`. Wired into the notebook as its own
CPU test cell (like the KVStore + cost-model tests).

---

## 5. Open calibration items (not blockers)

- `HEAT_ALPHA`, `HYSTERESIS`, `SCAN_HORIZON`, `MIN_RESIDENCY_TICKS` are first-estimate
  tunables; calibrate against a real mixed workload once the executor (Inc 4) lets us
  measure end-to-end migration churn vs. hit rate.
- `est_future_scans` from heat is a heuristic; revisit when real access traces exist.
- Capacity defaults come from `tier_physics()` (also calibration targets).

# Design: Live Tiering Integration — tiered scan-column catalog (INT-1)

**Status:** approved-by-standing-directive (session goal: proceed with ideal choice, no pause), pre-implementation
**Date:** 2026-06-26
**Builds on:** tier_manager.hpp (Inc 2), tiered_column.hpp + migration_executor.hpp (Inc 4), kv_store.hpp (Inc 1)

**Thesis:** *The dormant tiering machinery becomes a live capability: the engine holds more
analytical columns than fit in RAM, auto-demoting cold ones to SSD and pulling them back on
access. Resident-first analytics, actually running in the engine.*

This closes the integration debt the honest review found — TierManager + MigrationExecutor +
TieredColumn were tested-but-unused; now the live OP_SCAN path drives them.

---

## 1. Scope

Wire the tiering machinery into the CPU engine so analytical scan columns auto-tier between
RAM (HOST) and SSD (COLD) by access heat, under a RAM budget — letting the engine hold a
working set larger than RAM.

**IN:**
- CPUMockEngine gains: a `TierManager`, a `MigrationExecutor`, a catalog of `TieredColumn`s
  (`std::unordered_map<uint64_t, std::unique_ptr<TieredColumn>>`), and a scan counter.
- `load_scan_column(uint64_t id, const uint32_t* data, size_t n)` — register a tiered
  analytical column (born HOST, registered with the TierManager, added to the catalog).
- `matrix_set_scan_target(q, threshold, column_id)` / `matrix_get_scan_column_id(q)` codec —
  OP_SCAN carries a column id (payload offset 8, u64) alongside the threshold (offset 0, u32).
  `matrix_set_scan_threshold(q, t)` is kept and **delegates to `matrix_set_scan_target(q, t, 0)`**
  so the legacy path always stamps column_id=0 explicitly (unambiguous routing, no reliance on
  zero-initialized payload).
- Tiered `execute_scan`: when the query targets a catalog column (id > 0) — record access,
  ensure resident in HOST (migrate COLD→HOST if needed: a scan pulls cold data up), scan,
  and every `REBALANCE_EVERY` scans run `rebalance()` + `executor.apply()` to demote the
  coldest columns to SSD under the HOST byte budget.
- `test_live_tiering.cpp` — the headline: 3 columns into a 2-column RAM budget, hot/cold scan
  pattern, assert holds-more-than-RAM + cold demoted to SSD + scanning-cold pulls-back-correct.

**OUT (deferred, next increments):**
- DEVICE/VRAM promotion of catalog columns (the executor already moves bytes to VRAM — Inc 4
  proved it; wiring the GPU engine's catalog is the follow-up). This increment is CPU/HOST↔COLD.
- WAL activation in the live pipeline (INT-2) — the immediate next increment.
- Unifying the legacy fixed `scan_column_` into the catalog (it stays as the perf-benchmark
  fixture; see §3).
- Real query language / column-id assignment by a planner — ids are caller-assigned here.

---

## 2. Backward compatibility (the oracle must not move)

The existing pipeline builds scan queries with `matrix_set_scan_threshold(q, T)`, which writes
only the threshold at payload offset 0; the rest of the zero-initialized payload (incl. the
column-id slot at offset 8) stays 0. So:

- **`column_id == 0` → legacy path:** scan the existing fixed `scan_column_` exactly as today.
  The pipeline oracle (`Scan result sum: 83886070`) is byte-for-byte unchanged.
- **`column_id > 0` → tiered catalog path:** the new behavior.

`matrix_set_scan_threshold` is untouched (legacy); `matrix_set_scan_target(q, threshold, id)`
is the new codec that also writes the id. This is a strict superset — no existing call changes.

---

## 3. Why the legacy fixed column stays separate

The fixed 16M `scan_column_` is the perf-benchmark fixture (the `sweep`/`scan_benchmark` and the
oracle depend on its exact content and unconditional residency). Making it a tiered catalog
entry would force an ensure-resident check into the perf hot path and risk the oracle. So this
increment keeps the fixed column as-is and adds the tiered catalog alongside it. A future
increment may unify them once the perf-fixture role is no longer needed. Honest, low-risk.

---

## 4. Components & data flow

The engine constructs (CPU build): `MemoryModel::detect(false)` → `CostModel(mm, false)` →
`TierManager(cost, /*device_cap=*/1, /*host_cap=*/budget)` + `MigrationExecutor` + the catalog.

**Why `device_cap = 1` (not 0):** `scan_us(DEVICE,…)` ignores `gpu_available` (cost_model.hpp:42),
so the brain always sees VRAM as faster and would emit a HOST→DEVICE promotion — which the CPU
executor's `migrate_to(DEVICE)` aborts on. The promotion capacity-gate
(`resident_bytes(DEVICE)+bytes > cap`, tier_manager.hpp:75) rejects every real column when
`cap == 1` (none is ≤1 byte), so no DEVICE decision is ever emitted. `cap == 0` means *unbounded*
(the opposite). DEVICE is thus inert on the CPU build — matching this increment's HOST↔COLD scope.
The CUDA build passes the real VRAM budget and DEVICE promotion becomes valid + executable.

```
load_scan_column(id, data, n) ─▶ catalog[id] = TieredColumn(id, bytes, n*4);  tm_.register_column(id, n*4, HOST)

execute_scan(q):
  id = matrix_get_scan_column_id(q)
  if id == 0:  scan fixed scan_column_   (legacy, unchanged)
  else:
    TieredColumn& col = *catalog[id]
    tm_.record_access(id, col.size_bytes())              // heat signal
    bool borrowed = (col.tier() == COLD)
    if borrowed: col.migrate_to(HOST)                    // can't scan SSD in place; pull it up
    count = scan col's HOST bytes for value > threshold   // same filter-count as the fixed path
    if borrowed: col.migrate_to(COLD)                    // RETURN the borrow: resting tier == TierManager's belief
    if (++scans_since_rebalance_ >= REBALANCE_EVERY):
        auto ptrs = raw_catalog_ptrs()                   // unordered_map<id, TieredColumn*> from the unique_ptr catalog
        executor_.apply(tm_.rebalance(), ptrs)           // promote hot (none → DEVICE inert), demote coldest HOST→COLD under budget
        scans_since_rebalance_ = 0
    return count
```

`REBALANCE_EVERY` is a documented constant (default 4). The TierManager's HOST capacity is the
engine's RAM budget for the catalog (a ctor parameter; tests use a tiny budget to force eviction).

**Borrow-and-return** keeps the engine's actual residency in lockstep with the TierManager's
accounting: a cold column is in HOST only transiently during its own scan, then rests back on
COLD until a `rebalance()` *promotes* it (evicting a colder resident) once its heat earns a HOST
slot. No side-channel migration is invisible to the brain, so its byte budget stays honest.
`ponytail:` returning the borrow rewrites the COLD file each cold scan; a skip-if-unchanged (or a
TierManager `note_residency`) optimization is the upgrade path if cold-scan churn ever matters.

---

## 5. Verification (`test_live_tiering.cpp`, CPU, real temp files)

The headline capability — holds more than fits in RAM, correct results regardless of tier:

- **Load 3 columns** of S=N*4 bytes each, each `value[i]=i` (so a threshold T yields a known
  count N-1-T). TierManager HOST budget = 2*S (room for two of three), device_cap = 1.
- **Hot/cold pattern (deterministic by construction):** scan columns 1 and 2 several times each
  per interval; **never scan column 3 after load**. Column 3's heat stays 0 → lowest `keep_score`
  → it is the eviction victim, unambiguously. Columns 1 and 2 are scanned enough that their heat
  gives `est_future_scans ≥ 1` (heat ≥ 0.5·bytes), so their `keep_score > 0` and they're retained.
- **Anti-thrash timing:** `MIN_RESIDENCY_TICKS = 2`, so the *first* `rebalance()` evicts nothing
  (all columns arrived this tick); column 3 demotes on the *second* rebalance. The test runs
  ≥ 2·REBALANCE_EVERY scans so at least two rebalances fire.
- **Assert:** total catalog bytes (3*S) > HOST budget (2*S) — the engine holds more than RAM fits;
  after the rebalances `tm_.tier_of(3) == COLD` **and** `catalog[3]->tier() == COLD` (brain and
  bytes agree — borrow-and-return); columns 1,2 rest in HOST.
- **Correctness across tiers:** a scan of column 3 (cold → borrowed to HOST → scanned → returned)
  returns the exact oracle count (N-1-threshold), byte-identical to a scan of a never-demoted
  column; checksum of column 3 is invariant across the demote/borrow cycle.
- **Resident-bytes invariant:** after each rebalance, `tm_.resident_bytes(HOST) ≤ budget`.
- **Legacy path untouched:** a `column_id == 0` scan still hits the fixed column and matches the
  oracle (the engine's existing OP_SCAN still works).

Plus: the pipeline oracle (`main`) stays `83886070` (legacy path unchanged); all existing CPU
tests stay green; notebook regenerated with a live-tiering test cell.

---

## 6. Open / deferred (not blockers)

- GPU catalog promotion (DEVICE tier) — wire the CUDA engine's catalog + promote hot columns to
  VRAM; the migration mechanics are proven (Inc 4). Next GPU increment.
- WAL activation in the live pipeline (INT-2) — make the running engine durable by default/flag.
- Unify the legacy fixed column into the catalog once it's no longer the perf fixture.
- Eviction of the column mid-scan / concurrency — single-threaded scan consumer today, so no race;
  revisit with multi-client.
- Column-id assignment + a real load/query API — comes with the query layer (DM-4/DM-5).

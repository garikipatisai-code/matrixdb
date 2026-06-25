# Design: Three-Tier Auto-Tiering Storage Engine (VRAM / RAM / SSD)

**Status:** approved design (sections walked + approved in brainstorm), pre-implementation
**Date:** 2026-06-25
**Supersedes scope of:** gap-register items DM-1 (KV store), DU-1/2/3 (durability), DM-9
(growth/spill), and the deferred KD-4 unified seam — these merge into one coherent engine.

**Thesis:** *Each memory tier has distinct physics; the database respects each tier's
strength and concern, and automatically moves data to the tier that runs its workload
fastest — VRAM for hot scans, RAM for point ops, SSD for cold data and durability.*

---

## 1. Why this exists

The prototype keeps everything in RAM/VRAM and loses it on restart, and its "KV store"
silently overwrites colliding keys. Real data is bigger than VRAM, mostly cold, and must
survive a crash. A single fixed tier can't serve that. The opportunity (and the reason
this is distinctive, not just "a GPU database"): treat VRAM, RAM, and SSD as a **managed
hierarchy** where placement follows measured per-tier physics and live access patterns.

This is the **full auto-tiering** design (the DB decides and migrates), built in
increments. Increment 1 is shippable and fixes the P0 data-loss bug; later increments add
the brain, the SSD substrate (which delivers durability), and GPU/unified execution.

---

## 2. The tier model and its central asymmetry

The principled core: **point-op data and scan data tier differently because their access
patterns differ.**

```
                 ┌─────────── MatrixDB data ───────────┐
        Point-op KV (random access)            Scan columns (sequential bandwidth)
                 │                                       │
        RAM-primary ⇄ SSD (cold spill)        VRAM ⇄ RAM ⇄ SSD (by access heat)
        NEVER VRAM (measured: GPU loses        full 3-tier migration
        point ops, PCIe < CPU cache)
```

- **Point-op KV store:** lives in **RAM** (its strength: low-latency random access). When
  RAM capacity is the concern, **cold entries spill to SSD**. Never VRAM — measured.
- **Scan columns:** full **3-tier** citizens. The migration unit is a **column / column-
  chunk** (matches our columnar+GPU design; scans already run on resident columns).

### Tier physics (first-class, each with strength + concern)

| Tier | `MemorySpace` | Strength | Concern | Bandwidth (measured/est.) |
|------|---------------|----------|---------|---------------------------|
| VRAM | `DEVICE` | massive scan bandwidth, parallel | scarce capacity; PCIe to reach | ~240 GB/s |
| RAM  | `HOST`  | low-latency random access | medium capacity | ~10 GB/s scan / fast random |
| SSD  | `COLD`  | huge, cheap, sequential append | high latency; write wear | ~1–5 GB/s seq, slow random |
| (unified) | `UNIFIED` | zero-copy CPU+GPU pool | (future HW) | seam only |

`MemorySpace` grows from 3 → 4 members by adding `COLD`. `UNIFIED` remains the existing
seam (when present, VRAM/RAM collapse to one space and migration between them is a no-op).

---

## 3. The exploitable merge: SSD cold tier == durability substrate

SSD's defining strength is **sequential append**; a write-ahead log's defining access
pattern is **sequential append**. They are the same thing. So MatrixDB does **not** build
"SSD storage" and "a WAL" separately:

- **One append-only log on SSD** is simultaneously: (a) the WAL (durability — every commit
  appends here before ack), (b) cold-column storage (demoted columns append here), and
  (c) the persistent base for crash recovery (replay/load on restart).
- Respect the wear concern: **append-only**, never random rewrite. Compaction is periodic
  and sequential.
- Respect the latency concern: **reads are async/prefetched**, never synchronous on a
  query hot path. A cold-column scan triggers a promotion (to RAM/VRAM) rather than
  scanning from SSD directly.

This merges durability (DU-1/2/3) into the tiering engine: durability is a *property of
the cold tier*, not a separate subsystem.

---

## 4. The migration brain (auto-tiering policy)

A **cost-aware multi-level cache** — not plain LRU. Four parts:

1. **Heat tracking.** Per-column access signal: an EWMA (recency-weighted) bumped on each
   scan/access. Cheap, O(1) per access. Recency matters more than raw count.
2. **Tier-aware cost.** The existing `CostModel` extends to: scan cost of a column resident
   in each tier, plus a one-time **migration cost** (bytes / source-tier-bandwidth +
   dest write). Promote when *projected savings over a horizon* exceed migration cost.
3. **Capacity & eviction.** VRAM is scarce. When a tier is full, evict the resident item
   with the **lowest projected cost-benefit** (cost-aware victim selection, not merely the
   coldest). Evicted columns demote one tier down (VRAM→RAM→SSD).
4. **Migration executor.** Moves bytes **off the query hot path** (async): `cudaMemcpy`
   for RAM↔VRAM, file I/O for RAM↔SSD. A query never blocks on a migration; it runs on the
   data's current tier and the migration changes *future* queries.

Anti-thrash: hysteresis (promote only when savings clear migration cost by a margin), and
a minimum residency time before re-eviction.

---

## 5. Components (each single-purpose, independently testable)

- **`tier_model.hpp`** — `MemorySpace{DEVICE, HOST, COLD, UNIFIED}` + a `TierPhysics`
  table (bandwidth, latency, capacity, concern) per tier. Pure data. (extends
  `memory_model.hpp`; `MemoryModel`/unified seam folds in here.)
- **`cost_model.hpp`** (extend) — per-tier scan cost + migration cost + promote/demote/
  evict math. Pure functions, CPU-unit-testable.
- **`kv_store.hpp`** — the point-op store (DM-1): open-addressing hash table, key→value,
  RAM-primary, fixed capacity now, with an explicit **SSD-spill seam** for cold entries.
  Distinct keys never overwrite (fixes the P0 bug); a full table without spill is an
  explicit error, never silent corruption.
- **`tier_manager.hpp`** — the brain: owns the placement map (column → tier), heat tracking,
  eviction + migration *decisions*. Pure policy; no I/O itself.
- **`cold_store.hpp`** — the SSD append-only substrate: WAL append, cold-column append,
  recovery replay. On the dev box "SSD" is just files; the interface is what matters.
- **`router.hpp`** (extend) — consults `tier_manager` for a query's data's current tier,
  dispatches to that tier's engine; reports access back to the brain to update heat.

Boundaries: the **brain decides** (tier_manager + cost_model, pure logic), the
**substrates execute** (kv_store, cold_store, GPU engine), the **router connects**. Each
testable without the others.

---

## 6. Data flow

```
WRITE (point op):  router → kv_store.put(k,v) in RAM
                   → append to cold_store WAL (durability) → ack
                   → if RAM full: kv_store spills coldest entries → cold_store

SCAN (column):     router asks tier_manager for column's tier
                   → dispatch scan to that tier's engine (VRAM→GPU, RAM→CPU)
                   → if column is COLD: trigger async promotion, scan current copy
                   → report access → tier_manager bumps heat → maybe schedules promote

MIGRATION (bg):    tier_manager: for each tier over capacity, pick lowest-benefit victim,
                   demote it one tier; for each hot column below its ideal tier, if
                   savings > migration cost, promote. Executor moves bytes async.

RECOVERY (boot):   cold_store replays WAL → rebuilds kv_store + column base in RAM
                   → tier_manager re-warms (mechanism: promote columns the log's heat
                     metadata marks hottest; exact re-warm policy is an Inc 3/4 tunable)
```

---

## 7. Build increments (nothing given up; each has a verifiable path)

| Inc | Scope | Delivers | Verifiable where |
|-----|-------|----------|------------------|
| **1** | tier_model + tier-aware cost_model + kv_store (DM-1) | Fixes P0 data-loss bug; captures tier vision; real KV store | **Local (CPU)** |
| **2** | tier_manager brain — heat, eviction, migration *decisions* (pure logic) | The auto-tiering policy, unit-tested without real I/O | **Local (CPU)** |
| **3** | cold_store SSD substrate = append log = **durability (DU-1/2/3)** | Crash recovery, persistence, cold spill | **Local (disk = files)** |
| **4** | VRAM promote/demote execution (cudaMemcpy migration) | Real 3-tier movement on GPU | **Colab T4** |
| **5** | unified-memory tier collapse | VRAM/RAM as one space on DGX-Spark-class HW | Needs unified HW |

Increments 1–3 are fully buildable and verifiable on the current machine and deliver: the
P0 fix, the tiering brain, **and durability**. Increment 1 alone is shippable.

---

## 8. Scope discipline (what each increment does NOT do)

- Inc 1: KV store is fixed-capacity; spill is a **seam**, not implemented (that's Inc 3).
  Migration logic absent (Inc 2). No SSD code.
- Inc 2: decisions only — the brain returns "promote column X to VRAM"; nothing moves yet.
- The migration executor for VRAM (Inc 4) is GPU-only; on the CPU build it's a no-op stub.
- `value = key` mock projection stays until real typed values (gap DM-3) — this engine is
  about *placement and durability*, not value semantics.
- Real query language, networking, transactions-with-rollback remain separate gaps; the
  cold_store WAL is the substrate a future transaction manager will build commit on.

---

## 9. Open calibration / research items (not blockers)

- Per-tier bandwidth/latency constants need one measured pass each (VRAM & RAM measured;
  SSD seq/random and migration costs to be measured on the target box).
- EWMA half-life, promotion hysteresis margin, min-residency — tunables; start with
  documented defaults, calibrate against a mixed workload.
- WAL format, fsync discipline / durability levels, compaction cadence — detailed in the
  Inc 3 sub-spec when we get there.
- Recovery re-warm policy (how much to promote on boot) — Inc 3/4.

# Design: Increment 4 — Migration Executor (cross-tier byte movement)

**Status:** approved design (component split approved; remaining sections decided per session goal), pre-implementation
**Date:** 2026-06-26
**Parent spec:** `2026-06-25-three-tier-storage-engine-design.md` (Increment 4 row of §7)

**Thesis:** *The TierManager decides; the executor moves the bytes. A column promoted to VRAM
is physically copied there, byte-identical, and is then GPU-scannable — closing the
auto-tiering loop (heat → decision → migration → faster scans).*

---

## 1. Scope

Build the migration **machinery** that turns TierManager `MigrationDecision`s into actual
cross-tier byte movement, with a checksum-verified integrity guarantee, and prove a
VRAM-promoted column is genuinely GPU-scannable.

**IN:**
- `tiered_column.hpp` — a movable column whose bytes reside in exactly one tier
  (HOST / DEVICE / COLD); `migrate_to()` moves them; `checksum()` reads from wherever it lives.
- `migration_executor.hpp` — applies a `vector<MigrationDecision>` to a registry of columns.
- `test_migration.cpp` — CPU tests: HOST↔COLD round-trip integrity + the TierManager→executor
  loop (decisions actually move columns).
- GPU proof (Colab): HOST→DEVICE migration, scan the device bytes with the existing u32x4
  kernel, assert the filter-count equals a CPU scan of the same bytes; DEVICE→HOST round-trip
  checksum. Wired into the notebook.

**OUT (deferred):**
- Replacing the engine's live single scan column with a managed multi-column tiered store
  (the executor operates on `TieredColumn`s here, not the live OP_SCAN path — that rewire is
  a later increment, high blast radius).
- Async/background migration off the query thread, pinned-memory DMA, multi-stream — the
  executor is synchronous here; async is a future optimization.
- Real typed values / variable-length columns (unchanged: bytes are opaque).

---

## 2. Components

(Approved in brainstorm.) Two header-only units; the column owns storage + movement, the
executor maps decisions onto columns. Depends on `memory_model.hpp` (MemorySpace),
`tier_manager.hpp` (MigrationDecision), and — only under `MATRIX_USE_CUDA` — `<cuda_runtime.h>`.

```cpp
// tiered_column.hpp
class TieredColumn {
public:
    TieredColumn(uint64_t id, const unsigned char* bytes, size_t n); // born resident in HOST
    ~TieredColumn();                       // frees whichever tier holds it (cudaFree / file / vector)
    void migrate_to(MemorySpace dest);     // see §3 for the transition matrix
    MemorySpace tier() const;
    size_t size_bytes() const;
    uint64_t checksum() const;             // byte checksum wherever it lives (DEVICE copies back)
#if defined(MATRIX_USE_CUDA)
    const void* device_ptr() const;        // valid only while tier()==DEVICE; for in-place scan
#endif
    // non-copyable (owns a unique resident buffer)
};

// migration_executor.hpp
class MigrationExecutor {
public:
    // For each decision, look up the column and migrate_to(decision.to). A decision whose
    // column_id is absent is skipped (logged). Returns count of migrations applied.
    size_t apply(const std::vector<MigrationDecision>& plan,
                 std::unordered_map<uint64_t, TieredColumn*>& columns);
};
```

---

## 3. Migration paths & the integrity invariant

`migrate_to(dest)` handles the full 3×3 transition matrix, freeing the source tier after a
successful move:

| from \ to | HOST | DEVICE | COLD |
|---|---|---|---|
| **HOST** | no-op | `cudaMemcpy` H→D | write file |
| **DEVICE** | `cudaMemcpy` D→H | no-op | D→H then write file |
| **COLD** | read file | read file then H→D | no-op |

- **HOST↔DEVICE:** `cudaMemcpy` host↔device (only compiled under `MATRIX_USE_CUDA`).
- **HOST↔COLD:** write/read the raw bytes to/from a per-column file (`<wal-dir>/col_<id>.bin`
  or a temp path; the column owns the path).
- **DEVICE↔COLD:** routed through HOST (two hops) — there is no fast commodity direct path;
  via-RAM needs zero exotic deps.
- **Non-CUDA build:** any transition touching DEVICE is impossible; `migrate_to(DEVICE)` (or
  from DEVICE) **aborts with a clear message** ("DEVICE tier requires a CUDA build"). Honest,
  fail-loud — tests on the CPU build exercise only HOST↔COLD.

**Integrity invariant (the headline correctness guarantee):** for any sequence of migrations,
`checksum()` is invariant — the bytes are identical regardless of which tier holds them. This
is the cross-*tier* analog of the cross-*backend* checksum that anchored page-ownership. Every
test asserts checksum-before == checksum-after.

---

## 4. The auto-tiering loop (decision → migration)

This increment finally connects Inc 2's brain to real movement:

```
TierManager.rebalance() ─▶ vector<MigrationDecision>{id, from, to}
                              │
                   MigrationExecutor.apply(plan, columns)
                              │
                  per decision: columns[id]->migrate_to(to)
                              │
              bytes physically move; column.tier() updated; checksum preserved
```

The test registers `TieredColumn`s with a `TierManager`, drives access heat, calls
`rebalance()`, feeds the resulting plan to the executor, and asserts each column ended on its
decided tier with its checksum intact — the heat→decision→migration loop, end to end (HOST/COLD
locally; DEVICE on Colab).

---

## 5. Verification

**`test_migration.cpp`** (CPU, real temp files):
- **HOST↔COLD round-trip:** a column HOST→COLD→HOST has identical checksum and tier()==HOST at
  the end; the COLD file exists between the hops.
- **Integrity across a chain:** HOST→COLD→HOST→COLD, checksum invariant at every step.
- **Decision-driven loop:** register 2 columns, run a TierManager rebalance that demotes a cold
  column toward COLD, executor.apply, assert the column moved and checksum held; absent
  column_id in a decision is skipped, not a crash.
- **DEVICE-on-CPU-build guard:** `migrate_to(DEVICE)` on a non-CUDA build aborts fail-loud (a
  DEVICE tier cannot exist without CUDA). The CPU unit test does NOT invoke it (an inline abort
  would kill the test process); the guard is documented and the DEVICE paths are exercised only
  on the CUDA build (GPU proof below). The CPU test stays entirely within HOST↔COLD.

**GPU proof (Colab, in the notebook):**
- **Scannable-after-promote:** build a uint32 column in HOST, migrate_to(DEVICE), run the
  existing `matrix_scan_kernel_u32x4` over `device_ptr()`, assert the filter-count equals a CPU
  scan of the same bytes (count of value>threshold). Proves the cudaMemcpy preserved the data
  AND the column is genuinely GPU-scannable in place.
- **DEVICE round-trip:** HOST→DEVICE→HOST checksum invariant.

Discipline carried forward: a CPU-simulatable check for any index/offset math; host-syntax
probe for the `.hpp` CUDA paths before a Colab run; notebook regenerated with a migration test
cell; all existing CPU tests + the pipeline oracle stay green (this increment adds files, does
not touch the live engine path).

---

## 6. Open / deferred items (not blockers)

- Async/background migration (off the query thread) + pinned-host-memory DMA + multi-stream —
  throughput optimizations; the executor is synchronous now.
- Live-engine integration: making the engine's OP_SCAN column a managed `TieredColumn` the
  TierManager+executor drive automatically — the next increment after this.
- DEVICE↔COLD direct path (GPUDirect Storage) — exotic; via-RAM is the portable choice.
- Per-column file lifecycle/cleanup policy (COLD files) — basic create/unlink here; a real
  column catalog comes with live-engine integration.
- Migration failure handling (cudaMemcpy / file I/O errors) — fail-loud (abort) this increment,
  consistent with ColdStore's fopen handling; graceful retry is later hardening.

# Design: Catalog Snapshot Durability — CKPT-1

**Status:** approved-by-standing-directive, pre-implementation. **Date:** 2026-06-26.
**Builds on:** DM-5 (`column_io`, save/load a single column), INT-1 (the tiered catalog, borrow-and-return).

**Thesis:** *The tiered analytical catalog is in-memory and ephemeral — its COLD-tier files are
deleted on destruction, so a restart loses every loaded/computed column. Add a single-file catalog
snapshot (`save_catalog` / `load_catalog`) so the whole analytical store survives a restart —
the analytical-side analog of the point-op WAL.*

---

## 1. Scope

**IN:**
- `CPUMockEngine::save_catalog(const std::string& path)` — write every catalog column to one file:
  `[u32 magic][u64 num_cols]` then, per column, `[u64 id][u64 count][count × u32 data]`. COLD
  columns are borrowed to HOST to read (reusing borrow-and-return + the OB cold-borrow counter),
  then returned. Fail-loud (abort) on open / short write — consistent with ColdStore/TieredColumn.
- `CPUMockEngine::load_catalog(const std::string& path)` — read the file, verify magic, and
  `load_scan_column(id, data, count)` each column into the (fresh) catalog. Fail-loud on open /
  bad magic / short read.
- `test_catalog_snapshot.cpp` — build a multi-column catalog (some columns demoted to COLD at save
  time), `save_catalog`, `load_catalog` into a **fresh** engine, and verify every column reloads
  with identical query results + the right column count.

**OUT (deferred):**
- Persisting tier *placement* — tiers are a cache the TierManager recomputes from access heat, not
  data, so a restore lands every column in HOST and re-tiers under load. (Persisting placement is
  a future optimization, not correctness.)
- The point-op store (already covered by the Inc-3 WAL); a combined whole-engine snapshot.
- Incremental / concurrent-safe / crash-atomic snapshots (single-shot, single-threaded here;
  `.tmp`+rename atomicity is a follow-up); endianness portability (host-native, documented — same
  as `column_io`); GPU.

---

## 2. Format & methods (inline I/O in compute_mock.cpp)

A distinct catalog magic (so a column file and a catalog file can't be confused), then a count, then
each column self-describing (id + count + data) so load is order-independent and a column lands back
under its original id.

```cpp
static constexpr uint32_t MATRIX_CATALOG_MAGIC = 0x4D434154u; // 'MCAT' — MatrixDB catalog snapshot v0

// Snapshot every catalog column to `path`. Borrows COLD columns to HOST to read their bytes,
// returns them. Fail-loud on I/O error (never leave a half-written snapshot mistaken for valid).
void save_catalog(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "save_catalog: open failed %s\n", path.c_str()); std::abort(); }
    const uint32_t magic = MATRIX_CATALOG_MAGIC;
    const uint64_t ncols = catalog_.size();
    bool ok = std::fwrite(&magic, sizeof magic, 1, f) == 1
           && std::fwrite(&ncols, sizeof ncols, 1, f) == 1;
    for (auto& kv : catalog_) {
        if (!ok) break;
        TieredColumn& col = *kv.second;
        const MemorySpace home = col.tier();
        if (home != MemorySpace::HOST) { ++cold_borrows_; col.migrate_to(MemorySpace::HOST); }
        const uint64_t id = kv.first;
        const uint64_t count = col.size_bytes() / sizeof(uint32_t);
        const uint32_t* data = reinterpret_cast<const uint32_t*>(col.host_ptr());
        ok = std::fwrite(&id, sizeof id, 1, f) == 1
          && std::fwrite(&count, sizeof count, 1, f) == 1
          && (count == 0 || std::fwrite(data, sizeof(uint32_t), count, f) == count);
        if (home != MemorySpace::HOST) col.migrate_to(home);   // return the borrow regardless
    }
    std::fclose(f);
    if (!ok) { std::fprintf(stderr, "save_catalog: short write %s\n", path.c_str()); std::abort(); }
}

// Restore a snapshot into the catalog (columns land in HOST; the TierManager re-tiers under load).
void load_catalog(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "load_catalog: open failed %s\n", path.c_str()); std::abort(); }
    uint32_t magic = 0; uint64_t ncols = 0;
    bool ok = std::fread(&magic, sizeof magic, 1, f) == 1 && magic == MATRIX_CATALOG_MAGIC
           && std::fread(&ncols, sizeof ncols, 1, f) == 1;
    std::vector<uint32_t> data;
    for (uint64_t c = 0; ok && c < ncols; ++c) {
        uint64_t id = 0, count = 0;
        ok = std::fread(&id, sizeof id, 1, f) == 1 && std::fread(&count, sizeof count, 1, f) == 1;
        if (!ok) break;
        data.resize(static_cast<size_t>(count));
        ok = (count == 0 || std::fread(data.data(), sizeof(uint32_t), data.size(), f) == data.size());
        if (ok) load_scan_column(id, data.data(), data.size());
    }
    std::fclose(f);
    if (!ok) { std::fprintf(stderr, "load_catalog: bad/short snapshot %s\n", path.c_str()); std::abort(); }
}
```

- `load_scan_column` keeps its `id != 0` + not-already-registered asserts, so loading into a
  non-empty catalog with a colliding id fails loud (load into a fresh engine, as the WAL replay does).
- The borrow-return inside the save loop runs per column, so HOST never holds more than one extra
  COLD column transiently during the snapshot.

---

## 3. Verification (`test_catalog_snapshot.cpp`, CPU, real temp file)

- **Multi-column round-trip:** load 3 columns with distinct contents (e.g. `value=i`, `i+1000`,
  `i*2`); `save_catalog("/tmp/...")`; `load_catalog` into a **fresh** engine B; assert
  `stats().catalog_columns == 3` and each column's `execute_query` SUM equals the source's (proves
  every column reloaded under its own id with correct bytes).
- **Snapshot with a COLD column:** demote one column (scan a dummy hot under a tight budget),
  `save_catalog` (exercises the COLD→HOST borrow inside save), restore into a fresh engine, query →
  correct. Assert the saved column returned to COLD in the source engine after the snapshot.
- **Empty catalog:** `save_catalog` then `load_catalog` → `catalog_columns == 0` (num_cols 0, no
  records).
- **Non-vacuity:** the three restored columns have *different* SUMs (a stub that loaded one column
  for all ids, or dropped columns, fails); restoring into a fresh engine (not reusing the source)
  proves real persistence.

Plus: oracle `83886070` unchanged (additive); all 13 existing tests green; notebook regenerated.

---

## 4. Open / deferred
- Crash-atomic snapshot (write `.tmp` + rename); persisting tier placement; incremental/concurrent
  snapshots; a combined engine snapshot (catalog + point-op WAL checkpoint); endianness-portable
  encoding; GPU.

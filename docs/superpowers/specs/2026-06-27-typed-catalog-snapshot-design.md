# Design: Typed Catalog Snapshot (DM-3d, fourth slice of the DM-3 epic)

**Status:** approved-by-standing-directive. **Date:** 2026-06-27.
**Builds on:** CKPT-1 (catalog snapshot), DM-3a/b/c (int64 columns, `column_type`, `column_rows`).
**Fully local.**

**Thesis:** *int64 columns have full query parity (DM-3a/b/c) but are RAM-only — `save_catalog` fail-loud
aborts on them, so an int64 analytical store is lost on restart. Make the catalog snapshot type-aware:
write the element type per column and dispatch the load by type. After this, an int64 (or mixed u32+int64)
catalog survives a restart. Scoped to the catalog snapshot only; single-column `column_io.hpp` stays
u32-only (its `save_column` guard remains) — single-column typed file I/O is a later slice.*

---

## 1. Scope

**IN (`compute_mock.cpp` only + new `test_typed_snapshot.cpp`):**
- Bump the catalog-snapshot magic to a v1 value (so a v0 snapshot is rejected fail-loud, not misparsed).
- `save_catalog` per-column record: `[u64 id][u32 type][u64 count][count × width bytes]` (width = 4 for U32, 8 for I64; `type` = `column_type(id)`; `count` = `column_rows(id)`). The raw column bytes are written directly (`host_ptr()`, `size_bytes()`), type-agnostic. **Remove** the int64-abort guard (int64 is now persistable here).
- `load_catalog`: read `[id][type][count]`, validate `type ∈ {U32, I64}` (else fail-loud), read `count × width` bytes, and register via `load_scan_column` (U32) or `load_scan_column_i64` (I64). Restored columns land in HOST with their type tag set (the load functions set `col_types_`).

**Oracle / backward-compat invariants:**
- `save_column` / `load_column_from_file` / `column_io.hpp` (single-column binary I/O) are UNCHANGED — `save_column` keeps its int64 fail-loud guard (single-column typed file I/O = later). The WAL/checkpoint paths are untouched.
- The id-0 oracle path is untouched (83886070).
- A u32-only catalog round-trips identically in value (the new format adds a per-column `type` field = U32; `test_catalog_snapshot` writes+reads with the new code, so it still round-trips — it must not assert raw file bytes/size).

**OUT (later):** single-column typed file I/O (`save_column`/`load_column_from_file` + `column_io.hpp`) and typed CSV; double (DM-3e); GPU; crash-atomic (.tmp+rename) catalog snapshot (CKPT-1 already deferred that).

---

## 2. compute_mock.cpp changes

Bump the magic (find `MATRIX_CATALOG_MAGIC` — a `static constexpr uint32_t` member):
```cpp
static constexpr uint32_t MATRIX_CATALOG_MAGIC = 0x4D434131u; // 'MCA1' — typed catalog snapshot v1 (DM-3d)
```

`save_catalog` per-column loop body (replace the int64-guard + u32 write with a type-aware write):
```cpp
        const uint64_t id = kv.first;
        const uint32_t type = static_cast<uint32_t>(column_type(id));   // 0=U32, 1=I64
        const uint64_t count = column_rows(id);                          // type-aware row count
        ok = std::fwrite(&id,    sizeof id,    1, f) == 1
          && std::fwrite(&type,  sizeof type,  1, f) == 1
          && std::fwrite(&count, sizeof count, 1, f) == 1
          && (col.size_bytes() == 0
              || std::fwrite(col.host_ptr(), 1, col.size_bytes(), f) == col.size_bytes());  // raw bytes
        if (home != MemorySpace::HOST) col.migrate_to(home);
```
(Keep the surrounding borrow/return + the `if (!ok) break;` + final fclose/abort exactly as is. The
`const uint64_t count = col.size_bytes() / sizeof(uint32_t);` and `const uint32_t* data = ...` lines and
the int64-abort guard line are removed/replaced by the above.)

`load_catalog` per-column loop body (replace the u32-only read):
```cpp
        uint64_t id = 0, count = 0; uint32_t type = 0;
        ok = std::fread(&id, sizeof id, 1, f) == 1 && std::fread(&type, sizeof type, 1, f) == 1
          && std::fread(&count, sizeof count, 1, f) == 1;
        if (!ok) break;
        if (type == static_cast<uint32_t>(MatrixType::I64)) {
            std::vector<int64_t> d(static_cast<size_t>(count));
            ok = (count == 0 || std::fread(d.data(), sizeof(int64_t), d.size(), f) == d.size());
            if (ok) load_scan_column_i64(id, d.data(), d.size());
        } else if (type == static_cast<uint32_t>(MatrixType::U32)) {
            std::vector<uint32_t> d(static_cast<size_t>(count));
            ok = (count == 0 || std::fread(d.data(), sizeof(uint32_t), d.size(), f) == d.size());
            if (ok) load_scan_column(id, d.data(), d.size());
        } else {
            ok = false;   // unknown element type -> bad/corrupt snapshot, fail loud below
        }
```
(The outer `for (uint64_t c = 0; ok && c < ncols; ++c)` loop, the magic/ncols header read, and the
fclose/abort tail are unchanged. The old `std::vector<uint32_t> data;` hoisted before the loop is removed —
each branch declares its own typed buffer.)

---

## 3. Verification (`test_typed_snapshot.cpp`, CPU)

- **Mixed round-trip**: an engine with a u32 column (id 3, e.g. `{0..99}`) AND an int64 column (id 7,
  with **negatives** and a **> UINT32_MAX** value) → `save_catalog(path)`. Construct a FRESH engine →
  `load_catalog(path)`. Assert: `column_type(3) == U32`, `column_type(7) == I64`; a scalar query on each
  matches the original (u32 SUM; int64 SUM via `static_cast<int64_t>`). The int64 negatives/large value
  surviving is the non-vacuity guard (a u32 round-trip would corrupt them).
- **int64 MAX/MIN** on the reloaded int64 column equals the original (signed).
- **Empty catalog** round-trips (ncols 0, no crash).
- **COLD int64 column** (optional): drive the int64 column to COLD, snapshot, reload → values intact
  (borrow-to-HOST-to-read path).
- (Regression) `test_catalog_snapshot` (u32-only) still round-trips under the new format.

Plus: full CPU suite (now 26 tests) + oracle `83886070`; `test_catalog_snapshot`, `test_column_io`
(single-column, unchanged), `test_typed_columns/predicates/grouped` pass; notebook regenerated.

---

## 4. Open / deferred
Single-column typed file I/O (`save_column`/`load_column_from_file`/`column_io.hpp`) + typed CSV;
double (DM-3e); GPU; crash-atomic catalog snapshot; a snapshot format version field (the bumped magic
suffices for now).

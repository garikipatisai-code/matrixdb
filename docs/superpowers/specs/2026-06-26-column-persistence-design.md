# Design: Binary Column Persistence — DM-5

**Status:** approved-by-standing-directive, pre-implementation. **Date:** 2026-06-26.
**Builds on:** INT-1 (tiered catalog, `load_scan_column`, borrow-and-return), QRY-1 (`execute_query`).

**Thesis:** *The engine only operates on in-memory columns the caller hands it — it can neither
ingest a column from disk nor persist one. Add a robust binary column format + `save_column` /
`load_column_from_file`, closing the "data in/out + durable" gap and completing the end-to-end
story: load → auto-tier → query → observe → save.*

---

## 1. Scope

**IN:**
- `column_io.hpp` (new, header-only) — the on-disk column format + two free functions:
  - `matrix_write_column(const std::string& path, const uint32_t* data, size_t n)` — writes
    `[u32 magic][u64 count][count × u32 data]`. fopen-fail / short write → abort (fail-loud,
    consistent with `ColdStore`/`TieredColumn`).
  - `matrix_read_column(const std::string& path, std::vector<uint32_t>& out)` — verifies magic,
    reads count, fills `out`. fopen-fail / bad magic / short read → abort.
- `CPUMockEngine::save_column(uint64_t id, const std::string& path)` — borrow the catalog column
  to HOST, write its uint32 bytes, return the borrow (counts as a cold borrow if it was COLD).
- `CPUMockEngine::load_column_from_file(uint64_t id, const std::string& path)` — read a file into
  a vector, register it as a catalog column (delegates to `load_scan_column`).
- `test_column_io.hpp` direct round-trip + an engine save→load→query round-trip.

**OUT (deferred):** CSV / typed / variable-length / compressed formats; whole-catalog snapshot
(save/restore all columns + the TierManager state); endianness portability (assumes same-endian
read/write — the format is a same-machine persistence/cache, documented); GPU.

---

## 2. The format & functions (column_io.hpp)

```cpp
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

inline constexpr uint32_t MATRIX_COLUMN_MAGIC = 0x4D43'4F4Cu; // 'MCOL' — MatrixDB column file v0

// Write a uint32 column to `path` as [magic][u64 count][count×u32]. Fail-loud (abort) on
// open/short-write — never leave a partially/wrongly written file mistaken for valid.
inline void matrix_write_column(const std::string& path, const uint32_t* data, size_t n) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "matrix_write_column: open failed %s\n", path.c_str()); std::abort(); }
    const uint32_t magic = MATRIX_COLUMN_MAGIC;
    const uint64_t count = n;
    const bool ok = std::fwrite(&magic, sizeof magic, 1, f) == 1
                 && std::fwrite(&count, sizeof count, 1, f) == 1
                 && (n == 0 || std::fwrite(data, sizeof(uint32_t), n, f) == n);
    std::fclose(f);
    if (!ok) { std::fprintf(stderr, "matrix_write_column: short write %s\n", path.c_str()); std::abort(); }
}

// Read a column written by matrix_write_column. Fail-loud on open / bad magic / short read.
inline void matrix_read_column(const std::string& path, std::vector<uint32_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "matrix_read_column: open failed %s\n", path.c_str()); std::abort(); }
    uint32_t magic = 0; uint64_t count = 0;
    bool ok = std::fread(&magic, sizeof magic, 1, f) == 1 && magic == MATRIX_COLUMN_MAGIC
           && std::fread(&count, sizeof count, 1, f) == 1;
    if (ok) { out.resize(static_cast<size_t>(count));
              ok = (count == 0 || std::fread(out.data(), sizeof(uint32_t), out.size(), f) == out.size()); }
    std::fclose(f);
    if (!ok) { std::fprintf(stderr, "matrix_read_column: bad/short file %s\n", path.c_str()); std::abort(); }
}
```

(Magic detects "wrong file" — a real footgun for a load path. Count lets read allocate exactly.
Same-endian assumption documented; portable encoding is a follow-up if cross-machine files matter.)

---

## 3. Engine methods (compute_mock.cpp, after the inspection accessors)

```cpp
    // Persist a catalog column's bytes to `path` (borrows to HOST to read, returns the borrow).
    void save_column(uint64_t id, const std::string& path) {
        TieredColumn& col = *catalog_.at(id);
        const MemorySpace home = col.tier();
        if (home != MemorySpace::HOST) { ++cold_borrows_; col.migrate_to(MemorySpace::HOST); }
        matrix_write_column(path, reinterpret_cast<const uint32_t*>(col.host_ptr()),
                            col.size_bytes() / sizeof(uint32_t));
        if (home != MemorySpace::HOST) col.migrate_to(home);
    }
    // Load a column file into the catalog under `id` (born resident in HOST, like load_scan_column).
    void load_column_from_file(uint64_t id, const std::string& path) {
        std::vector<uint32_t> data;
        matrix_read_column(path, data);
        load_scan_column(id, data.data(), data.size());
    }
```

`compute_mock.cpp` adds `#include "column_io.hpp"`. `save_column` reuses the borrow-and-return (so
a COLD column can be persisted) and the OB cold-borrow counter; `load_column_from_file` reuses
`load_scan_column` (id assert, TierManager registration).

---

## 4. Verification (`test_column_io.cpp`, CPU, real temp files)

- **Direct round-trip:** `matrix_write_column("/tmp/...", data, n)` then `matrix_read_column` →
  the vector equals the original; the first 4 bytes of the file are `MATRIX_COLUMN_MAGIC`.
- **Empty column:** n==0 round-trips (count 0, no data).
- **Engine save→load→query:** build a column in engine A (`value[i]=i`), `save_column`; in a fresh
  engine B `load_column_from_file`; `execute_query` SUM on B == the closed-form oracle (== a SUM on
  A). Proves a persisted column reloads and queries identically.
- **Save a COLD column:** demote a column (scan a dummy hot), `save_column` it (borrows from SSD),
  load into a new engine, query → correct (persistence works regardless of tier).
- **Non-vacuity:** the loaded column's SUM differs from a different column's, and the magic check
  is real (the file starts with the magic). (The abort-on-bad-magic path is fail-loud, documented,
  not unit-tested — consistent with the codebase's other abort guards.)

Plus: oracle `83886070` unchanged (additive — no existing path touched); all 12 tests green;
notebook regenerated with the new test.

---

## 5. Open / deferred
- CSV / typed / compressed formats; whole-catalog snapshot (+ TierManager state) for full restart
  durability of the analytical store; endianness-portable encoding; GPU.

# Design: Append / Dynamic Column Growth (DM-9)

**Status:** approved-by-standing-directive (continue all phases, don't wait). **Date:** 2026-06-27.
**Builds on:** Inc 4 (`TieredColumn`), INT-1 (`TierManager`/catalog), DM-3 (`column_type`/`column_rows`).
**Fully local.**

**Thesis:** *Analytical columns are load-once — there's no way to add rows after creation, so a dataset
that grows over time must be fully reloaded. Add `append_to_column[_i64/_f64]` to grow an existing column
with new rows. `TieredColumn.host_` is already a growable buffer; appending borrows a COLD column to HOST,
grows it, returns the borrow, and updates the TierManager's byte accounting (a new `update_bytes`).
Additive — load-once columns and queries are unchanged.*

---

## 1. Scope

**IN (`tiered_column.hpp` + `tier_manager.hpp` + `compute_mock.cpp` + new `test_append.cpp`):**
- `TieredColumn::append_bytes(const unsigned char* b, size_t n)` — requires `tier()==HOST`
  (`assert`); `host_.insert(host_.end(), b, b+n); size_ += n;`.
- `TierManager::update_bytes(uint64_t id, size_t bytes)` — set `cols_[id].bytes = bytes` in place,
  preserving tier/heat/recent (NOT `register_column`, which would reset them). No-op if `id` absent.
- `CPUMockEngine`: a private `append_raw(id, const unsigned char* bytes, size_t byte_count)` doing the
  borrow-to-HOST → `append_bytes` → return-borrow → `tier_mgr_.update_bytes(id, col.size_bytes())`; and
  three public typed wrappers `append_to_column` (u32) / `append_to_column_i64` / `append_to_column_f64`,
  each asserting `catalog_has(id)` and the matching `column_type(id)`, then calling `append_raw` with
  `n * sizeof(elem)` bytes.

**Invariants:** the appended rows become visible to subsequent queries (`column_rows` grows;
SUM/COUNT/etc. include them). Works across tiers — appending to a COLD column borrows it to HOST
(`++cold_borrows_`), grows it, and writes the grown buffer back to its home tier (the COLD file is
rewritten via `migrate_to`). The TierManager's `resident_bytes`/capacity accounting reflects the new
size. Append does NOT itself trigger a rebalance (like `grouped_aggregate`); residency rebalances on the
next scan — documented. Type-mismatched append is a debug-asserted caller error. Oracle path (id-0)
untouched.

**OUT:** auto-rebalance on append (next scan handles it); shrinking/deleting rows; concurrent append
(single-owner per the page-ownership model); growing the id-0 fixed column (it's the benchmark fixture).

---

## 2. tiered_column.hpp + tier_manager.hpp

```cpp
// TieredColumn (public): append bytes to a HOST-resident column (grow in place). Caller ensures the
// column is HOST (the engine borrows COLD->HOST first). size_ grows so checksum()/host_ptr() span it.
void append_bytes(const unsigned char* b, size_t n) {
    if (tier_ != MemorySpace::HOST) {
        std::fprintf(stderr, "TieredColumn::append_bytes requires HOST tier\n"); std::abort();
    }
    host_.insert(host_.end(), b, b + n);
    size_ += n;
}
```
(Fail-loud rather than `assert` so a misuse can't silently corrupt under `-DNDEBUG`, matching the file's other guards.)

```cpp
// TierManager (public): update a column's byte size in place after an append. Preserves tier/heat/
// recent_bytes (unlike register_column, which resets them). No-op if id is unknown.
void update_bytes(uint64_t id, size_t bytes) {
    auto it = cols_.find(id);
    if (it != cols_.end()) it->second.bytes = bytes;
}
```

---

## 3. compute_mock.cpp

```cpp
// Append `n` elements of raw bytes to catalog column `id`, growing it (borrow COLD->HOST, append,
// return the borrow, update the brain's accounting). Private — typed wrappers enforce the element type.
void append_raw(uint64_t id, const unsigned char* bytes, size_t byte_count) {
    TieredColumn& col = *catalog_.at(id);
    const MemorySpace home = col.tier();
    if (home != MemorySpace::HOST) { ++cold_borrows_; col.migrate_to(MemorySpace::HOST); }
    col.append_bytes(bytes, byte_count);
    if (home != MemorySpace::HOST) col.migrate_to(home);     // return the borrow (grown buffer)
    tier_mgr_.update_bytes(id, col.size_bytes());
}
```
Public (near `load_scan_column*`):
```cpp
void append_to_column(uint64_t id, const uint32_t* data, size_t n) {
    assert(catalog_has(id) && column_type(id) == MatrixType::U32 && "append type must match column (u32)");
    append_raw(id, reinterpret_cast<const unsigned char*>(data), n * sizeof(uint32_t));
}
void append_to_column_i64(uint64_t id, const int64_t* data, size_t n) {
    assert(catalog_has(id) && column_type(id) == MatrixType::I64 && "append type must match column (int64)");
    append_raw(id, reinterpret_cast<const unsigned char*>(data), n * sizeof(int64_t));
}
void append_to_column_f64(uint64_t id, const double* data, size_t n) {
    assert(catalog_has(id) && column_type(id) == MatrixType::F64 && "append type must match column (double)");
    append_raw(id, reinterpret_cast<const unsigned char*>(data), n * sizeof(double));
}
```

---

## 4. Verification (`test_append.cpp`, CPU)

- **u32 append**: load `{0..9}` (id 3); `append_to_column(3, {10,11,12}, 3)`; assert `column_rows(3) == 13`
  and `execute_query(SUM)` == 0+…+12 = 78. Append again; SUM grows accordingly.
- **int64 / double append**: load a small int64 (id 7) and double (id 9) column; append rows (incl. a
  negative / fractional); assert SUM reflects all rows (`static_cast<int64_t>` / `std::bit_cast<double>`).
- **Append to a COLD column**: load a column, drive it COLD (small `host_cap` + a hot workload on other
  columns, OR `column_tier`-checked), `append_to_column`, assert the query result includes the appended
  rows and the checksum is consistent (borrow-append-return across the tier).
- **Accounting**: after appending, `stats().host_resident_bytes` (for a HOST column) / `column_rows`
  reflect the grown size.
- **Non-vacuity**: the appended rows actually change the aggregate (SUM before ≠ SUM after); appending
  to a COLD column still surfaces the new rows (proves the borrow path grows the persisted bytes).

Plus: full CPU suite (now 33 tests) + oracle `83886070`; `test_live_tiering`, `test_migration`,
`test_typed_*`, `test_query` pass unmodified; notebook regenerated.

---

## 5. Open / deferred
Auto-rebalance on append; row delete/shrink; bulk/streaming append; append over the wire; growing the
id-0 fixed column.

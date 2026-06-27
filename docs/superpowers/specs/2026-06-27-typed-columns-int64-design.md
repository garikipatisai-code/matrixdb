# Design: Typed Columns — int64 (DM-3a, first slice of the DM-3 epic)

**Status:** approved-by-standing-directive (goal: continue all phases), pre-implementation. **Date:** 2026-06-27.
**Builds on:** INT-1 (tiered catalog), AGG-1 (`matrix_cpu_reduce_all`), QRY-1/VAL-1 (`execute_query`/status).
**Fully local: pure compute + an in-memory type tag, no transport.**

**Thesis:** *Every analytical column is `uint32` — values are capped at ~4.29 B and cannot be negative,
so timestamps-in-ms, money-in-cents, large IDs, and signed deltas are unrepresentable. Introduce a
per-column element-type tag and add signed 64-bit (`int64`) columns. Additive and oracle-safe: the tag
defaults to `U32`, so every existing column, query, and the id-0 benchmark/oracle scan is byte-identical;
the new code runs only for an `I64` column. This first slice delivers `int64` columns with scalar
aggregation (COUNT/SUM/MIN/MAX over all rows); filtered/grouped/persisted `int64` are later slices.*

---

## 0. The DM-3 epic — decomposition (why this is a slice)

DM-3 (real types) is cross-cutting: the `uint32` element width is implicit at ~75 sites (`/sizeof(uint32_t)`,
`reinterpret_cast<uint32_t*>`, the `uint32` reducers, `column_io`/catalog-snapshot's `[count×u32]` format,
CSV). `TieredColumn` itself is already type-agnostic (raw bytes). Rather than one risky mega-change, DM-3
lands as additive slices, each oracle-safe:
- **DM-3a (this slice):** type tag + `int64` columns + scalar **unfiltered** aggregation. ← here
- **DM-3b:** `int64` predicates (generalize `MatrixPredicate` to 64-bit/signed) → filtered + grouped `int64`.
- **DM-3c:** typed persistence — encode the element type in `column_io`/catalog-snapshot/CSV so `int64`
  columns survive restart and ingest from CSV.
- **DM-3d:** `double` (float) columns (float comparison/NaN handling) and any further types.
- **DM-3e (GPU, Colab):** typed scan kernels.

## 1. Scope (DM-3a)

**IN (`compute.hpp` + `compute_mock.cpp` + new `test_typed_columns.cpp`):**
- `enum class MatrixType : uint32_t { U32 = 0, I64 };` — `U32 == 0` is the default (absent tag ⇒ U32).
- `int64_t matrix_cpu_reduce_all_i64(const int64_t* v, size_t n, MatrixAggOp op)` — signed unfiltered
  reduce: COUNT→`n`, SUM→`Σv` (int64, see overflow note), MIN→smallest (init `INT64_MAX`), MAX→largest
  (init `INT64_MIN`); empty (`n==0`) → SUM/COUNT 0, MIN `INT64_MAX`, MAX `INT64_MIN`.
- `MatrixQueryStatus::ERR_UNSUPPORTED_TYPE` — returned when an `I64` column hits a path this slice does
  not implement (filtered or grouped), gracefully (out cleared, no crash — the VAL-1 discipline).
- `CPUMockEngine`:
  - `std::unordered_map<uint64_t, MatrixType> col_types_` — parallel to `catalog_`; absent ⇒ `U32`.
    (ponytail: a side map keeps `tiered_column.hpp` untouched; fold the tag into `TieredColumn` if types
    proliferate.)
  - `load_scan_column_i64(uint64_t id, const int64_t* data, size_t n)` — mirrors `load_scan_column`
    (TieredColumn stores the `n*8` raw bytes — already type-agnostic) and sets `col_types_[id] = I64`.
  - `MatrixType column_type(uint64_t id) const` — `col_types_` lookup, default `U32` (inspection/dispatch).
  - `int64_t scan_tiered_column_i64(uint64_t id, MatrixAggOp op)` — same borrow-to-HOST-and-return as
    `scan_tiered_column`, reinterpret as `const int64_t*`, `n = size_bytes()/sizeof(int64_t)`,
    `matrix_cpu_reduce_all_i64`.
  - `execute_query`: after the `catalog_has` check, branch on `column_type(q.value_col)`. `I64` +
    (`grouped` or `has_filter`) → `ERR_UNSUPPORTED_TYPE`; `I64` scalar → `out.assign(1, static_cast<uint64_t>(scan_tiered_column_i64(...)))`. `U32` → the existing path, **unchanged**.
  - Guard `save_catalog`/`save_column` against an `I64` column (debug `assert(column_type(id)==U32 && "typed-column persistence is DM-3c")`) — slice-1 does not persist typed columns; this prevents silently writing int64 bytes through the `u32` snapshot format.

**Result convention (the one wart, documented):** `execute_query`'s `out` is `vector<uint64_t>`. For an
`I64` column the result is delivered as the **int64 two's-complement bit pattern** in `out[0]`; the
caller reads `static_cast<int64_t>(out[0])`. COUNT is a non-negative count (fits directly). U32 columns
are unaffected.

**SUM overflow:** `int64` SUM accumulates in `int64` and can overflow for pathological data (e.g. 10^11
rows near `INT64_MAX`). Documented; saturating/`__int128` accumulation is deferred (matches the existing
`uint32` SUM-in-`u64` "no overflow for our columns" stance).

**Oracle / backward-compat invariants:** the id-0 scan and `matrix_cpu_reduce*`/`matrix_cpu_reduce_all`
are untouched; every existing catalog column is `U32` (tag absent), so `execute_query`'s new branch is
never taken for them. `test_query`, `test_group_by`, `test_aggregations`, `test_query_validation`,
`test_query_predicates`, `test_live_tiering`, `test_catalog_snapshot`, `test_observability` pass as-is.

**OUT (later slices):** filtered/grouped `int64` (DM-3b); `int64` persistence + CSV (DM-3c); `double`
(DM-3d); GPU (DM-3e); a typed result vector (the bit-pattern convention suffices here).

---

## 2. compute.hpp additions

```cpp
// Per-column element type. U32 == 0 is the default (an untagged column is uint32). int64 columns
// (DM-3a) hold signed 64-bit values — negatives and values beyond UINT32_MAX, which uint32 cannot.
enum class MatrixType : uint32_t { U32 = 0, I64 };

// Signed unfiltered scalar reduce over an int64 column (the int64 sibling of matrix_cpu_reduce_all).
// COUNT -> n; SUM -> Σv (int64; see the overflow note in the design); MIN/MAX over all (empty
// sentinels MIN INT64_MAX / MAX INT64_MIN, reachable only when n == 0).
inline int64_t matrix_cpu_reduce_all_i64(const int64_t* v, size_t n, MatrixAggOp op) {
    switch (op) {
        case AGG_SUM: { int64_t s = 0; for (size_t i = 0; i < n; ++i) s += v[i]; return s; }
        case AGG_MIN: { int64_t m = INT64_MAX; for (size_t i = 0; i < n; ++i) if (v[i] < m) m = v[i]; return m; }
        case AGG_MAX: { int64_t m = INT64_MIN; for (size_t i = 0; i < n; ++i) if (v[i] > m) m = v[i]; return m; }
        case AGG_COUNT:
        default:      return static_cast<int64_t>(n);
    }
}
```
(`<cstdint>` — already included — provides `INT64_MAX`/`INT64_MIN`/`int64_t`.)

`MatrixQueryStatus` gains one value:
```cpp
enum class MatrixQueryStatus { OK, ERR_UNKNOWN_COLUMN, ERR_INVALID_GROUP, ERR_TOO_MANY_GROUPS, ERR_UNSUPPORTED_TYPE };
```

---

## 3. compute_mock.cpp wiring

- Member (beside `catalog_`): `std::unordered_map<uint64_t, MatrixType> col_types_;`
- `load_scan_column_i64` (beside `load_scan_column`):
  ```cpp
  // Register a signed int64 analytical column (born HOST-resident, like load_scan_column). DM-3a.
  void load_scan_column_i64(uint64_t id, const int64_t* data, size_t n) {
      assert(id != 0 && "column id 0 is reserved for the legacy fixed scan column");
      assert(catalog_.find(id) == catalog_.end() && "column id already registered");
      const size_t bytes = n * sizeof(int64_t);
      catalog_[id] = std::make_unique<TieredColumn>(id, reinterpret_cast<const unsigned char*>(data), bytes);
      tier_mgr_.register_column(id, bytes, MemorySpace::HOST);
      col_types_[id] = MatrixType::I64;
  }
  ```
- `MatrixType column_type(uint64_t id) const { auto it = col_types_.find(id); return it == col_types_.end() ? MatrixType::U32 : it->second; }`
- `scan_tiered_column_i64` (private, beside `scan_tiered_column`) — same borrow-and-return, int64 reduce:
  ```cpp
  int64_t scan_tiered_column_i64(uint64_t col_id, MatrixAggOp op) {
      TieredColumn& col = *catalog_.at(col_id);
      tier_mgr_.record_access(col_id, col.size_bytes());
      const MemorySpace home = col.tier();
      if (home != MemorySpace::HOST) { ++cold_borrows_; col.migrate_to(MemorySpace::HOST); }
      const int64_t* vals = reinterpret_cast<const int64_t*>(col.host_ptr());
      const size_t nvals = col.size_bytes() / sizeof(int64_t);
      const int64_t result = matrix_cpu_reduce_all_i64(vals, nvals, op);
      if (home != MemorySpace::HOST) col.migrate_to(home);
      return result;
  }
  ```
  (Mirror the exact `record_access` / borrow / `host_ptr` / return-borrow steps of `scan_tiered_column` — read it to match.)
- `execute_query` — after `if (!catalog_has(q.value_col)) return ERR_UNKNOWN_COLUMN;`, insert:
  ```cpp
  if (column_type(q.value_col) == MatrixType::I64) {
      if (q.grouped || q.has_filter) return MatrixQueryStatus::ERR_UNSUPPORTED_TYPE; // DM-3b
      out.assign(1, static_cast<uint64_t>(scan_tiered_column_i64(q.value_col, q.agg)));
      return MatrixQueryStatus::OK;
  }
  ```
  The existing `U32` body below is unchanged.
- `save_catalog` per-column loop and `save_column`: add `assert(column_type(id) == MatrixType::U32 && "typed-column persistence is DM-3c");` before writing the column's bytes (debug guard against the u32-format mishandling an int64 column).

---

## 4. Verification (`test_typed_columns.cpp`, CPU)

- **`matrix_cpu_reduce_all_i64`**: an int64 array with negatives and a value > `UINT32_MAX`
  (e.g. `{-7, 0, 5, 5000000000, -3, 2147483648}`): COUNT/SUM/MIN/MAX vs a hand-computed oracle;
  empty array → COUNT 0, SUM 0, MIN `INT64_MAX`, MAX `INT64_MIN`.
- **Engine int64 column**: `load_scan_column_i64(7, data, n)` with the same kind of data; `execute_query`
  scalar for each op; assert `static_cast<int64_t>(out[0])` equals the oracle. The **> UINT32_MAX** and
  **negative** values are the non-vacuity guard — a uint32 column (or a uint32 reducer) would truncate
  `5000000000` to `705032704` and read `-7` as `4294967289`, so the asserts fail unless the column is
  genuinely 64-bit signed. Assert `column_type(7) == MatrixType::I64`.
- **Graceful unsupported**: `execute_query` on the int64 column with `has_filter = true` → `ERR_UNSUPPORTED_TYPE`,
  out empty; with `grouped = true` → `ERR_UNSUPPORTED_TYPE`.
- **U32 untouched**: a `load_scan_column` (uint32) column still answers scalar + filtered + grouped
  queries exactly as before (`column_type` is `U32`); run one of each in the test.
- **COLD borrow**: optionally drive an int64 column to COLD and query it (pulled back, correct) — reuses
  the borrow path; assert the result survives a tier round-trip.

Plus: full CPU suite (now 23 tests) + oracle `83886070` unchanged; the 8 catalog/query tests listed in
§1 pass unmodified; notebook regenerated.

---

## 5. Open / deferred
DM-3b (int64 predicates → filtered/grouped int64); DM-3c (typed persistence + CSV); DM-3d (double);
DM-3e (GPU typed kernels); a typed result vector; `__int128`/saturating SUM; folding the type tag into
`TieredColumn`.

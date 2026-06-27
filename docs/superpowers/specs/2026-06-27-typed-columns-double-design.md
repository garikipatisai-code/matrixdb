# Design: double (float64) Columns (DM-3e, fifth slice of the DM-3 epic)

**Status:** approved-by-standing-directive (goal: continue all phases, don't wait). **Date:** 2026-06-27.
**Builds on:** DM-3a/b/d (int64 columns + `MatrixType`/`column_rows`/typed catalog snapshot), QRY-3.
**Fully local.** Mirrors the proven int64 pattern.

**Thesis:** *Real measurements (prices, rates, sensor readings) are floating-point. Add `double` (float64)
columns with scalar aggregation — unfiltered + filtered — and durability via the typed catalog snapshot,
mirroring int64 (DM-3a/b/d). Additive and oracle-safe: a third `MatrixType` value, new `double` reducers,
and a `column_type`-gated dispatch; every existing column (U32/I64) and the oracle are byte-identical.
Grouped double is a small follow-up (DM-3f). IEEE semantics: NaN doesn't match ordered predicates and is
skipped by MIN/MAX; SUM propagates NaN — documented, not special-cased.*

---

## 1. Scope

**IN (`compute.hpp` + `compute_mock.cpp` + new `test_typed_double.cpp`):**
- `MatrixType` gains `F64` (= 2): `enum class MatrixType : uint32_t { U32 = 0, I64, F64 };`
- `matrix_cpu_reduce_all_f64(const double* v, size_t n, MatrixAggOp op) -> double` — unfiltered: COUNT
  `(double)n`, SUM `Σv`, MIN/MAX over all (empty sentinels MIN `+inf`, MAX `-inf`).
- `struct MatrixPredicateF64 { MatrixCmp cmp = MatrixCmp::GT; double a = 0.0; double b = 0.0; };`,
  `matrix_pred_match_f64` (IEEE compares — NaN makes GT/GE/LT/LE/EQ false, NE true, BETWEEN false),
  `matrix_cpu_reduce_pred_f64` (filtered; same empty sentinels).
- `MatrixQuery` gains `double lo_f64 = 0.0; double hi_f64 = 0.0;` (double filter bounds; not wire-serialized — parallel to `lo_i64`/`hi_i64`).
- `CPUMockEngine`: `load_scan_column_f64(id, const double*, n)` (sets `col_types_[id]=F64`); `scan_tiered_column_f64(col_id, MatrixPredicateF64 pred, op, bool has_filter=false)` (borrow-and-return, mirrors `scan_tiered_column_i64`, calls `maybe_rebalance()`); `column_rows` width for F64 = 8; `execute_query` F64-value branch: grouped → `ERR_UNSUPPORTED_TYPE` (DM-3f); scalar → `scan_tiered_column_f64` with `MatrixPredicateF64{q.cmp, q.lo_f64, q.hi_f64}`.
- **Durability:** `load_catalog` gains an F64 branch (read `count` doubles → `load_scan_column_f64`). `save_catalog` needs NO change — its DM-3d write is type-generic (`type=column_type(id)`, `count=column_rows(id)`, raw bytes); once `column_rows` knows F64=8, an F64 column snapshots correctly. So double columns are durable in this slice too.

**Result convention:** an F64-column query result is delivered as the double's BIT PATTERN via `std::bit_cast<uint64_t>(double)` in `out[0]` (NOT a numeric cast — that would truncate); caller reads `std::bit_cast<double>(out[0])`. COUNT is `(double)n` (a count expressed as a double, exact for counts < 2^53). `<bit>` provides `std::bit_cast` (C++20).

**Oracle / backward-compat:** U32/I64 reducers, the id-0 path, and `matrix_cpu_reduce*` are untouched; existing columns are U32/I64 so the F64 branch is dormant. The catalog snapshot's u32/int64 round-trips are unchanged (F64 is a new `type` value the loader now also handles).

**OUT (later):** grouped double (DM-3f); single-column typed file I/O + typed CSV; GPU; over-the-wire double filtering.

---

## 2. compute.hpp additions

```cpp
enum class MatrixType : uint32_t { U32 = 0, I64, F64 };   // F64 == 2 (DM-3e)
```
(replace the existing 2-value enum)

```cpp
#include <bit>     // std::bit_cast (add near the existing includes if not present)

// Unfiltered scalar reduce over a double column. COUNT (double)n; SUM Σv; MIN/MAX over all (empty
// sentinels MIN +inf, MAX -inf). IEEE: NaN values are skipped by MIN/MAX (NaN compares false) and
// poison SUM (NaN propagates) — documented, not special-cased.
inline double matrix_cpu_reduce_all_f64(const double* v, size_t n, MatrixAggOp op) {
    switch (op) {
        case AGG_SUM: { double s = 0.0; for (size_t i = 0; i < n; ++i) s += v[i]; return s; }
        case AGG_MIN: { double m = std::numeric_limits<double>::infinity();  for (size_t i = 0; i < n; ++i) if (v[i] < m) m = v[i]; return m; }
        case AGG_MAX: { double m = -std::numeric_limits<double>::infinity(); for (size_t i = 0; i < n; ++i) if (v[i] > m) m = v[i]; return m; }
        case AGG_COUNT:
        default:      return static_cast<double>(n);
    }
}

struct MatrixPredicateF64 { MatrixCmp cmp = MatrixCmp::GT; double a = 0.0; double b = 0.0; };
inline bool matrix_pred_match_f64(double v, const MatrixPredicateF64& p) {
    switch (p.cmp) {
        case MatrixCmp::GE:      return v >= p.a;
        case MatrixCmp::LT:      return v <  p.a;
        case MatrixCmp::LE:      return v <= p.a;
        case MatrixCmp::EQ:      return v == p.a;
        case MatrixCmp::NE:      return v != p.a;
        case MatrixCmp::BETWEEN: return v >= p.a && v <= p.b;
        case MatrixCmp::GT:
        default:                 return v >  p.a;
    }
}
inline double matrix_cpu_reduce_pred_f64(const double* v, size_t n, const MatrixPredicateF64& p, MatrixAggOp op) {
    switch (op) {
        case AGG_SUM: { double s = 0.0; for (size_t i = 0; i < n; ++i) if (matrix_pred_match_f64(v[i], p)) s += v[i]; return s; }
        case AGG_MIN: { double m = std::numeric_limits<double>::infinity();  for (size_t i = 0; i < n; ++i) if (matrix_pred_match_f64(v[i], p) && v[i] < m) m = v[i]; return m; }
        case AGG_MAX: { double m = -std::numeric_limits<double>::infinity(); for (size_t i = 0; i < n; ++i) if (matrix_pred_match_f64(v[i], p) && v[i] > m) m = v[i]; return m; }
        case AGG_COUNT:
        default:      { double c = 0.0; for (size_t i = 0; i < n; ++i) c += matrix_pred_match_f64(v[i], p) ? 1.0 : 0.0; return c; }
    }
}
```
Place the F64 reducers near the int64 ones (after `matrix_cpu_reduce_pred_i64` / `MatrixPredicateI64`, post-`MatrixCmp`). Add `#include <limits>` if not already present (for `infinity()`).

`MatrixQuery` gains (after `hi_i64`): `double lo_f64 = 0.0;` and `double hi_f64 = 0.0;`

---

## 3. compute_mock.cpp wiring

- `load_scan_column_f64(uint64_t id, const double* data, size_t n)` — mirror `load_scan_column_i64` (bytes `n*sizeof(double)`, `col_types_[id] = MatrixType::F64`).
- `scan_tiered_column_f64(uint64_t col_id, MatrixPredicateF64 pred, MatrixAggOp op, bool has_filter = false)` — mirror `scan_tiered_column_i64`: record_access, borrow-to-HOST, `reinterpret_cast<const double*>(host_ptr())`, `n = size_bytes()/sizeof(double)`, `has_filter ? matrix_cpu_reduce_pred_f64 : matrix_cpu_reduce_all_f64`, return borrow, `maybe_rebalance()`.
- `column_rows`: `const size_t w = (column_type(id) == MatrixType::I64 || column_type(id) == MatrixType::F64) ? 8 : sizeof(uint32_t);` (both I64 and F64 are 8 bytes/row).
- `execute_query`: add an F64-value branch alongside the I64 one:
  ```cpp
  if (column_type(q.value_col) == MatrixType::F64) {
      if (q.grouped) return MatrixQueryStatus::ERR_UNSUPPORTED_TYPE;   // grouped double = DM-3f
      out.assign(1, std::bit_cast<uint64_t>(
          scan_tiered_column_f64(q.value_col, MatrixPredicateF64{q.cmp, q.lo_f64, q.hi_f64}, q.agg, q.has_filter)));
      return MatrixQueryStatus::OK;
  }
  ```
- `load_catalog`: add an F64 read branch alongside U32/I64:
  ```cpp
  } else if (type == static_cast<uint32_t>(MatrixType::F64)) {
      std::vector<double> d(static_cast<size_t>(count));
      ok = (count == 0 || std::fread(d.data(), sizeof(double), d.size(), f) == d.size());
      if (ok) load_scan_column_f64(id, d.data(), d.size());
  }
  ```
  `save_catalog` is unchanged (type-generic since DM-3d; `column_rows` now returns the F64 row count).

---

## 4. Verification (`test_typed_double.cpp`, CPU)

Use **exactly-representable** doubles (e.g. `1.5, -3.0, 0.5, 2.25, 5000000000.0, -0.25`) and matching
summation order so `==` is exact (no epsilon needed).
- **`matrix_cpu_reduce_all_f64` / `_pred`**: per op vs a hand oracle; filtered (LT/EQ/BETWEEN with double bounds incl. negative + fractional). Empty → MIN +inf, MAX -inf.
- **`matrix_pred_match_f64`**: ordered ops at fractional boundaries; a **NaN** value → GT/GE/LT/LE/EQ false, NE true; BETWEEN false.
- **Engine scalar** via `execute_query`: `load_scan_column_f64`, scalar SUM/MIN/MAX/COUNT (unfiltered + filtered), `std::bit_cast<double>(out[0])` == oracle. `column_type(id) == F64`.
- **Durability**: `save_catalog` an F64 (+ a U32) column → fresh engine `load_catalog` → `column_type` F64 restored, scalar SUM matches (the fractional/large values survive — non-vacuity).
- **Grouped double rejected**: F64 + `grouped` → `ERR_UNSUPPORTED_TYPE`.
- **Non-vacuity**: a fractional value (1.5) survives (an int64/uint32 path would truncate it); EQ ≠ GT.

Plus: full CPU suite (now 27 tests) + oracle `83886070`; `test_typed_columns/predicates/grouped/snapshot`, `test_catalog_snapshot`, `test_query*` pass unmodified; notebook regenerated.

---

## 5. Open / deferred
Grouped double (DM-3f); single-column typed file I/O + typed CSV; GPU; over-the-wire double filtering;
a SUM that's NaN/precision-aware (Kahan); typed result vector instead of bit_cast convention.

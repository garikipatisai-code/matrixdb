# Design: Typed CSV Ingest (DM-3g, seventh slice of the DM-3 epic)

**Status:** approved-by-standing-directive (continue all phases, don't wait). **Date:** 2026-06-27.
**Builds on:** DM-5b (`matrix_read_csv_column` u32), DM-3a/e (int64/double columns), VAL-1 (graceful untrusted input).
**Fully local.**

**Thesis:** *CSV ingest is uint32-only — real datasets have signed integers (deltas, money) and
floating-point (prices, rates), which can't be loaded today. Add `int64` and `double` CSV column readers
mirroring the u32 one (DM-5b), plus the engine `load_column_from_csv_i64`/`_f64` methods. Same graceful
contract: a malformed field returns `false` (no crash), never `abort` — CSV is untrusted input (the
VAL-1 lesson). Additive; the u32 CSV path is untouched.*

(Single-column typed BINARY file I/O — `save_column`/`load_column_from_file`/`column_io.hpp` — is left
deferred-low-value: the typed catalog snapshot (DM-3d/e) already gives typed columns durability, so a
per-column typed binary file is redundant. `save_column` keeps its int64/double fail-loud guard.)

---

## 1. Scope

**IN (`csv_ingest.hpp` + `compute_mock.cpp` + new `test_typed_csv.cpp`):**
- `bool matrix_read_csv_column_i64(const std::string& path, size_t col_index, bool has_header, char delim, std::vector<int64_t>& out)` — mirrors `matrix_read_csv_column`; parses the field via `std::from_chars` into `int64_t` (handles leading `-`; rejects trailing junk / overflow via `ec`+`ptr==end`). Graceful `false` on any error.
- `bool matrix_read_csv_column_f64(const std::string& path, size_t col_index, bool has_header, char delim, std::vector<double>& out)` — same field-walk; parses via `std::strtod` (portable — Apple libc++ historically lacks `std::from_chars` for floating point), requiring the parse to consume exactly the field (`endptr == field_end`) and `errno != ERANGE`. Graceful `false` on any error. (`strtod` skips leading whitespace — accepted, documented.)
- `CPUMockEngine::load_column_from_csv_i64(id, path, col_index, has_header=false, delim=',')` → `matrix_read_csv_column_i64` → `load_scan_column_i64`; returns `false` (no catalog entry) on malformed CSV. Same for `_f64` → `load_scan_column_f64`.

**Invariants:** the u32 `matrix_read_csv_column` / `load_column_from_csv` are UNCHANGED. Graceful failure
(open fail / short row / non-numeric / overflow / trailing junk) → `false`, `out` cleared, no catalog
entry, no crash. The oracle path is untouched (CSV is not on it).

**OUT (later/deferred):** single-column typed binary file I/O (covered by the catalog snapshot); RFC-4180
quoting (simple unquoted CSV only — inherited from DM-5b); type/schema inference (caller picks the reader);
GPU; CSV export.

---

## 2. csv_ingest.hpp additions

Both mirror `matrix_read_csv_column`'s structure (CRLF strip, header skip, field-walk to `col_index`,
graceful `out.clear(); return false` on any error). Only the per-field PARSE differs.

```cpp
#include <cstdlib>   // std::strtod  (add if not present)
#include <cerrno>    // errno, ERANGE (add if not present)

// int64 sibling of matrix_read_csv_column (DM-3g). Parses the col_index-th field as a signed 64-bit
// integer via std::from_chars (rejects trailing junk / overflow). Graceful false on any error.
inline bool matrix_read_csv_column_i64(const std::string& path, size_t col_index, bool has_header,
                                       char delim, std::vector<int64_t>& out) {
    out.clear();
    std::ifstream in(path);
    if (!in) return false;
    std::string line; bool first = true;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (has_header && first) { first = false; continue; }
        first = false;
        size_t start = 0, field = 0;
        while (field < col_index) {
            size_t comma = line.find(delim, start);
            if (comma == std::string::npos) { out.clear(); return false; }
            start = comma + 1; ++field;
        }
        size_t end = line.find(delim, start);
        if (end == std::string::npos) end = line.size();
        int64_t value = 0;
        auto [ptr, ec] = std::from_chars(line.data() + start, line.data() + end, value);
        if (ec != std::errc{} || ptr != line.data() + end) { out.clear(); return false; }
        out.push_back(value);
    }
    return true;
}

// double sibling (DM-3g). Parses via std::strtod (portable across libc++ versions), requiring the parse
// to consume exactly the field (no trailing junk) and not overflow. Graceful false on any error.
// ponytail: strtod skips leading whitespace and is C-locale '.'-decimal — fine for simple numeric CSV.
inline bool matrix_read_csv_column_f64(const std::string& path, size_t col_index, bool has_header,
                                       char delim, std::vector<double>& out) {
    out.clear();
    std::ifstream in(path);
    if (!in) return false;
    std::string line; bool first = true;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (has_header && first) { first = false; continue; }
        first = false;
        size_t start = 0, field = 0;
        while (field < col_index) {
            size_t comma = line.find(delim, start);
            if (comma == std::string::npos) { out.clear(); return false; }
            start = comma + 1; ++field;
        }
        size_t end = line.find(delim, start);
        if (end == std::string::npos) end = line.size();
        if (start == end) { out.clear(); return false; }     // empty field
        errno = 0;
        char* endptr = nullptr;
        const double value = std::strtod(line.c_str() + start, &endptr);
        if (errno == ERANGE || endptr != line.c_str() + end) { out.clear(); return false; }  // overflow / trailing junk / not a number
        out.push_back(value);
    }
    return true;
}
```
(`matrix_read_csv_column` (u32) and the `<charconv>`/`<fstream>`/`<string>`/`<vector>` includes already exist; add `<cstdlib>`+`<cerrno>` if missing.)

---

## 3. compute_mock.cpp additions (beside `load_column_from_csv`)

```cpp
bool load_column_from_csv_i64(uint64_t id, const std::string& path, size_t col_index,
                              bool has_header = false, char delim = ',') {
    std::vector<int64_t> data;
    if (!matrix_read_csv_column_i64(path, col_index, has_header, delim, data)) return false;
    load_scan_column_i64(id, data.data(), data.size());
    return true;
}
bool load_column_from_csv_f64(uint64_t id, const std::string& path, size_t col_index,
                              bool has_header = false, char delim = ',') {
    std::vector<double> data;
    if (!matrix_read_csv_column_f64(path, col_index, has_header, delim, data)) return false;
    load_scan_column_f64(id, data.data(), data.size());
    return true;
}
```

---

## 4. Verification (`test_typed_csv.cpp`, CPU)

- **`matrix_read_csv_column_i64`**: a CSV with negatives and a > UINT32_MAX value (`"k,v\n-7,-7\n10,5000000000\n"`, col 1, header) → `{-7, 5000000000}`. Graceful false: missing file, short row, non-integer (`"1,x"`), overflow (`"99999999999999999999"`), trailing junk (`"12x"`).
- **`matrix_read_csv_column_f64`**: fractional + negative + exponent (`"1.5\n-3.25\n5e9\n"`, col 0) → `{1.5, -3.25, 5e9}` (exactly representable → `==`). Graceful false: non-numeric (`"x"`), empty field, trailing junk (`"1.5x"`).
- **Engine**: `load_column_from_csv_i64(7, file, col, has_header=true)` then `execute_query(SUM)` == `static_cast<int64_t>` oracle; `load_column_from_csv_f64(8, file, col)` then `execute_query(SUM)` == `std::bit_cast<double>` oracle; `column_type(7)==I64`, `column_type(8)==F64`. A malformed CSV → `false`, no catalog entry (subsequent query → `ERR_UNKNOWN_COLUMN`).
- **Non-vacuity**: the > UINT32_MAX int64 value and the fractional double survive (a u32 reader would truncate / reject); a parser ignoring bad fields would make the failure asserts fail.

Plus: full CPU suite (now 29 tests) + oracle `83886070`; `test_csv_ingest` (u32, unchanged), `test_typed_columns`/`double`/`snapshot` pass; notebook regenerated.

---

## 5. Open / deferred
Single-column typed binary file I/O (covered by the catalog snapshot); RFC-4180 quoting; schema/type
inference; CSV export; GPU.

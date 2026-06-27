# Design: CSV Ingest — DM-5b

**Status:** approved-by-standing-directive (goal: continue all phases), pre-implementation. **Date:** 2026-06-27.
**Builds on:** DM-5 (`load_column_from_file`, binary column I/O), VAL-1 (graceful validation of untrusted input).
**Fully local: text-file parsing, no transport, no new dependency.**

**Thesis:** *Today data only enters the engine as our own binary column format (DM-5) or an in-memory
`uint32_t*`. Real datasets live in CSV. Add a minimal CSV→column loader so an external integer
dataset can be ingested straight into the tiered catalog and queried. CSV is **untrusted input**, so —
unlike the binary reader's `abort()` on corruption — a malformed CSV is reported gracefully (returns
false), never a crash (the VAL-1 lesson).*

---

## 1. Scope

**IN (new `csv_ingest.hpp` + new `test_csv_ingest.cpp`; one method on `CPUMockEngine`):**
- `bool matrix_read_csv_column(const std::string& path, size_t col_index, bool has_header, char delim, std::vector<uint32_t>& out)`
  — read `path` line by line, take the field at 0-based `col_index` of each row, parse it as `uint32_t`,
  append to `out`. Optional `has_header` skips the first line. Returns `true` + fills `out` on success;
  on ANY error returns `false` and leaves `out` empty (cleared up front).
- `CPUMockEngine::load_column_from_csv(uint64_t id, const std::string& path, size_t col_index, bool has_header = false, char delim = ',')`
  — calls `matrix_read_csv_column`; on success forwards to `load_scan_column(id, …)` (born HOST-resident,
  exactly like `load_column_from_file`); returns the parser's bool. Bad CSV → no catalog entry created.

**Graceful failure (returns false), each tested:**
- file cannot be opened;
- a row has fewer than `col_index + 1` fields (short row);
- the target field is not a non-negative integer (empty, letters, sign, trailing junk);
- the value exceeds `UINT32_MAX` (the column element type).

A successful parse over an empty file (or header-only with `has_header`) yields `out.empty()` and returns
**true** (zero rows is valid, not an error).

**OUT (deferred — `ponytail:` ceilings in code):**
- Quoted fields / embedded delimiters / escapes / RFC-4180 quoting — assumes simple unquoted integer CSV.
  Upgrade path: a quote-aware field splitter if real files need it.
- Type inference / float / string / signed columns — `uint32_t` only (the engine's column type). Other
  types ride DM-3 (typed columns).
- Multi-column ingest in one pass — one column per call (call N times). YAGNI.
- Streaming/`mmap` of huge files — `std::getline` line-at-a-time is enough; whole result held in RAM.

---

## 2. `csv_ingest.hpp`

```cpp
#pragma once
#include <cstdint>
#include <charconv>      // std::from_chars — locale-free, no-throw integer parse
#include <fstream>
#include <string>
#include <vector>

// Parse one uint32 column (0-based col_index) out of a simple CSV file. has_header skips line 1.
// Returns true + fills `out` on success; false + empty `out` on any error (open fail / short row /
// non-integer / overflow). CSV is untrusted input, so malformed data is a graceful false, NOT abort
// (cf. column_io.hpp, which aborts on corruption of our OWN binary format). See VAL-1.
// ponytail: no quoted-field / escape handling — simple unquoted integer CSV only; quote-aware split is
// the upgrade path if real files need it.
inline bool matrix_read_csv_column(const std::string& path, size_t col_index, bool has_header,
                                   char delim, std::vector<uint32_t>& out) {
    out.clear();
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();   // tolerate CRLF
        if (has_header && first) { first = false; continue; }
        first = false;
        // Walk to the col_index-th field without allocating a vector of all fields.
        size_t start = 0, field = 0;
        while (field < col_index) {
            size_t comma = line.find(delim, start);
            if (comma == std::string::npos) { out.clear(); return false; }   // short row (clear: false ⇒ empty out)
            start = comma + 1;
            ++field;
        }
        size_t end = line.find(delim, start);
        if (end == std::string::npos) end = line.size();
        const char* b = line.data() + start;
        const char* e = line.data() + end;
        uint32_t value = 0;
        auto [ptr, ec] = std::from_chars(b, e, value);
        if (ec != std::errc{} || ptr != e) { out.clear(); return false; }    // non-integer, overflow, or trailing junk
        out.push_back(value);
    }
    return true;
}
```

`std::from_chars` is chosen over `strtoul`/`stoul` precisely because it (a) never throws, (b) is
locale-independent, (c) reports overflow via `errc::result_out_of_range`, and (d) requires
`ptr == e` to prove the WHOLE field was an integer (rejects `12x`, `1.5`, `+1`, leading space).

---

## 3. Engine method (`compute_mock.cpp`, beside `load_column_from_file`)

```cpp
// Ingest one uint32 column from a CSV file into the catalog under `id` (born HOST-resident, like
// load_column_from_file). Returns false (no catalog entry created) if the CSV is malformed — CSV is
// untrusted input, so a bad file is reported, never a crash. See DM-5b / VAL-1.
bool load_column_from_csv(uint64_t id, const std::string& path, size_t col_index,
                          bool has_header = false, char delim = ',') {
    std::vector<uint32_t> data;
    if (!matrix_read_csv_column(path, col_index, has_header, delim, data)) return false;
    load_scan_column(id, data.data(), data.size());
    return true;
}
```

Add `#include "csv_ingest.hpp"` beside the existing `#include "column_io.hpp"`.

---

## 4. Verification (`test_csv_ingest.cpp`, CPU)

Write small CSVs to `/tmp`, then assert:
- **Basic:** `1,10\n2,20\n3,30` — column 1 → `{10,20,30}`; column 0 → `{1,2,3}`; returns true.
- **Header skip:** `key,val\n5,50\n6,60` with `has_header=true`, col 1 → `{50,60}`.
- **Delimiter:** `7;70\n8;80` with `delim=';'`, col 1 → `{70,80}`.
- **Empty / header-only:** empty file → true, `out.empty()`; `"k,v\n"` with `has_header=true` → true, empty.
- **CRLF tolerance:** `"1,2\r\n3,4\r\n"` col 0 → `{1,3}`.
- **Graceful failures (return false, out empty):** missing file; short row (`col_index` past the
  last field); non-integer field (`"1,x\n"`, col 1); overflow (`"5000000000\n"` > UINT32_MAX); trailing
  junk (`"12x\n"`).
- **Engine end-to-end:** `load_column_from_csv(id, file, col)` then `execute_query(SUM)` equals the
  hand-summed oracle; a malformed CSV returns false and creates **no** catalog entry
  (`column_tier(id)` style check / a subsequent query on `id` is ERR_UNKNOWN_COLUMN).
- **Non-vacuity:** the failure asserts are real — a parser that ignored bad fields (e.g. pushed 0)
  would make column 1 of `"1,x\n"` succeed; we assert it returns false.

Plus: full CPU suite (now 20 tests) + oracle `83886070` unchanged; notebook regenerated (38 sources).

---

## 5. Open / deferred
RFC-4180 quoting; typed/float/signed columns (DM-3); multi-column single-pass ingest; streaming huge
files; type/schema inference; a CSV *export* (the inverse) if round-tripping to other tools matters.

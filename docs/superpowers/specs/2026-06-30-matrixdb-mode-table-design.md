# matrixdb `.mode table` — aligned output (Design + Plan)

**Date:** 2026-06-30  **Status:** approved (standing directive). Small feature → combined design+plan.
**Goal:** `.mode table` — aligned, padded columns, so grouped results and multi-aggregate tables are easy to
read. Completes the `.mode` family (list | csv | table). CPU, local.

## Why

`list` (` │ `) and `csv` (`,`) stream row-by-row. Alignment needs all rows of a result first (to size each
column), so it can't stream. Rather than refactor the ~10 emit sites in `matrix_cli_run_sql` to collect rows,
**capture its LIST output and post-process it** — a pure string transform. `matrix_cli_run_sql` stays
untouched; LIST/CSV are byte-identical (existing tests stay green); table is purely additive.

## Design

- `OutMode` gains `TABLE`.
- In `matrix_repl`, a SQL line under `TABLE` mode runs `matrix_cli_run_sql(line, buf, eng, OutMode::LIST)`
  into an `ostringstream`, then `align_table(buf.str(), out)` re-emits it aligned. (LIST/CSV run straight to
  `out` as today.)
- **`align_table(text, out)`** — splits `text` into lines; a line containing the ` │ ` separator is a table
  row (split into fields on ` │ `), any other line (a single scalar value, a `… (N rows)` note, an `Error:`)
  passes through literally. Column width = max field byte-length over all rows; re-emit each row with every
  field except the last padded to its column width, joined by ` │ `. No header rule (a captured result
  doesn't mark which row is a header — uniform columns are the win).
  - ponytail: width = byte length, exact for ASCII (our data); a multi-byte UTF-8 field value would over-pad.
    Noted, not handled in v1.
- `.mode list|csv|table`; unknown arg → usage error.

## Scope & non-goals

**In:** `.mode table` aligned columns via post-processing; `.help`/`.mode`/README reflect it.
**Deferred (named):** a header underline rule (needs to know the header row); true Unicode display-width;
right-aligning numeric columns (everything left-aligns in v1).

## Implementation outline

- [ ] `matrix_cli.hpp`: `enum class OutMode { LIST, CSV, TABLE };` (add `TABLE`).
- [ ] add `align_table(const std::string& text, std::ostream& out)` in `matrixcli_detail`:
```cpp
inline void align_table(const std::string& text, std::ostream& out) {
    const std::string SEP = " │ ";
    std::vector<std::string> lines; { std::istringstream is(text); std::string ln; while (std::getline(is, ln)) lines.push_back(ln); }
    std::vector<std::vector<std::string>> cells(lines.size());   // empty = literal pass-through line
    std::vector<size_t> width;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find(SEP) == std::string::npos) continue;   // scalar / note / error -> literal
        std::vector<std::string> f; size_t p = 0, q;
        while ((q = lines[i].find(SEP, p)) != std::string::npos) { f.push_back(lines[i].substr(p, q - p)); p = q + SEP.size(); }
        f.push_back(lines[i].substr(p));
        for (size_t c = 0; c < f.size(); ++c) { if (c >= width.size()) width.push_back(0); width[c] = std::max(width[c], f[c].size()); }
        cells[i] = std::move(f);
    }
    for (size_t i = 0; i < lines.size(); ++i) {
        if (cells[i].empty()) { out << lines[i] << "\n"; continue; }
        const auto& f = cells[i];
        for (size_t c = 0; c < f.size(); ++c) {
            if (c) out << SEP;
            out << f[c];
            if (c + 1 < f.size()) for (size_t pad = f[c].size(); pad < width[c]; ++pad) out << ' ';   // pad all but last col
        }
        out << "\n";
    }
}
```
- [ ] `matrix_repl` SQL dispatch — unify timing + add table capture:
```cpp
        if (line[0] != '.') {
            const auto t0 = std::chrono::steady_clock::now();
            if (mode == OutMode::TABLE) { std::ostringstream buf; matrix_cli_run_sql(line, buf, eng, OutMode::LIST); align_table(buf.str(), out); }
            else matrix_cli_run_sql(line, out, eng, mode);
            if (timing) { const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count(); out << "(" << us << " µs)\n"; }
            continue;
        }
```
- [ ] `.mode` command: accept `table` too (`mode = OutMode::TABLE`).
- [ ] `.help`: `.mode list|csv|table`.
- [ ] `test_cli.cpp`: `.mode table` + a multi-agg grouped query vs the same in list → assert outputs differ,
      data intact, and table output contains a 2-space run (padding) that list lacks.
- [ ] Verify: `test_cli` ASan/UBSan; `./run_tests.sh` + `SAN=1` green; README/memory/notebook.
- [ ] Commits: `feat(cli): aligned .mode table (post-process LIST output)` then `docs(mode): table`.

## Success criteria

`.mode table` then a grouped/multi-agg query prints aligned padded columns; list/csv unchanged; suite +
oracle + demo green under ASan/UBSan.

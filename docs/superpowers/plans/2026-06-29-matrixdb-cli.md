# matrixdb CLI / REPL (Core, v1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A runnable `matrixdb` CLI/REPL — load CSV, run SQL at a prompt, see decoded results + catalog/stats — over the existing CPU engine.

**Architecture:** A testable shell over streams: `matrix_repl(std::istream&, std::ostream&, CPUMockEngine&)` in `matrix_cli.hpp` (line loop + dot-commands + SQL router + typed/string-decoded output), a thin `matrixdb_cli.cpp` main, and `test_cli.cpp` driving it with string streams. One small additive engine/IO piece enables `.load … str`.

**Tech Stack:** C++20, the existing `parse_query`/`avg_query`/`project_query`/`execute_query`, `csv_ingest.hpp`, dictionary strings, `catalog_columns()`/`stats()`.

**Spec:** `docs/superpowers/specs/2026-06-29-matrixdb-cli-design.md`

---

### Task 1: `str` CSV ingest (additive IO + engine method)

The numeric CSV readers exist; strings don't. Add the string sibling + an engine wrapper that dict-encodes.

**Files:**
- Modify: `csv_ingest.hpp` (after `matrix_read_csv_column`, ~line 46) — add `matrix_read_csv_column_str`.
- Modify: `compute_mock.cpp` (after `load_column_from_csv_f64`, ~line 512) — add `load_string_column_from_csv`.
- Test: `test_typed_csv.cpp` (extend) — a string column round-trips through CSV.

- [ ] **Step 1: Add the string reader** (mirrors `matrix_read_csv_column` but keeps the field verbatim — no `from_chars`):

```cpp
// String sibling of matrix_read_csv_column: keeps the col_index-th field verbatim. Graceful false on open
// failure / short row. (Used by load_string_column_from_csv -> dictionary encoding.)
inline bool matrix_read_csv_column_str(const std::string& path, size_t col_index, bool has_header,
                                       char delim, std::vector<std::string>& out) {
    out.clear();
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    bool first = true;
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
        out.emplace_back(line.substr(start, end - start));
    }
    return true;
}
```

- [ ] **Step 2: Add the engine wrapper** (after `load_column_from_csv_f64`):

```cpp
// Ingest a string column straight from CSV (dictionary-encoded — see load_string_column_dict). Same
// graceful contract: malformed CSV / open failure → false, no catalog entry, no crash.
bool load_string_column_from_csv(uint64_t id, const std::string& path, size_t col_index,
                                 bool has_header = false, char delim = ',') {
    std::vector<std::string> data;
    if (!matrix_read_csv_column_str(path, col_index, has_header, delim, data)) return false;
    load_string_column_dict(id, data);
    return true;
}
```

- [ ] **Step 3: Extend `test_typed_csv.cpp`** — after its existing checks, add: write a 1-column CSV of
  categories to a temp path, `load_string_column_from_csv(7, path, 0, /*has_header=*/false)`, assert
  `string_dict_size(7)` == distinct count and a `GROUP BY`/`count_distinct` over it matches a brute oracle.

```cpp
    {   // string column from CSV -> dict-encoded, queryable
        const char* p = "/tmp/matrixdb_str_csv_test.csv";
        { std::ofstream f(p); f << "books\ngames\nbooks\nmusic\ngames\nbooks\n"; }
        CPUMockEngine e; e.load_string_column_from_csv(7, p, 0, /*has_header=*/false);
        assert(e.string_dict_size(7) == 3 && e.count_distinct(7) == 3);
        std::remove(p);
        std::printf("[csv str -> dict] ok\n");
    }
```
(Needs `#include <fstream>` / `<cstdio>` in the test if absent.)

- [ ] **Step 4: Run** `./run_tests.sh` → `ALL GREEN` (test_typed_csv now covers the str path); `SAN=1 ./run_tests.sh` green.

- [ ] **Step 5: Commit** — `git commit -am "feat(csv): string CSV ingest — matrix_read_csv_column_str + load_string_column_from_csv"`

---

### Task 2: `matrix_cli.hpp` — REPL loop + dot-commands

**Files:**
- Create: `matrix_cli.hpp` — `matrix_repl(in, out, eng)` + the dot-command handlers + small output helpers.
- Test: `test_cli.cpp` (new).

- [ ] **Step 1: Write `test_cli.cpp` (dot-commands first).** Drives a scripted session; asserts substrings.

```cpp
#include "matrix_cli.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
static bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
int main() {
    const char* csv = "/tmp/matrixdb_cli_test.csv";
    { std::ofstream f(csv); f << "amount,region\n10,books\n900,games\n20,books\n950,music\n"; }
    CPUMockEngine eng;
    std::istringstream in(
        ".load " + std::string(csv) + " amount u32 header\n"
        ".load " + std::string(csv) + " region str header\n"   // note: region is col 1 -> see Task 2 Step 3 (col index)
        ".columns\n"
        ".stats\n"
        ".help\n"
        ".quit\n");
    std::ostringstream out;
    const int rc = matrix_repl(in, out, eng);
    const std::string s = out.str();
    assert(rc == 0);
    assert(has(s, "loaded 4 rows into \"amount\""));
    assert(has(s, "amount") && has(s, "u32"));
    assert(eng.column_id("amount") != 0);
    std::printf("[cli dot-commands] ok\n");
    std::remove(csv);
    std::printf("ALL CLI TESTS PASSED\n");
    return 0;
}
```
(The `region` load uses column index 1 — `.load` must accept an optional column-index; default 0. Decision: `.load <path> <name> <type> [colN] [header|noheader]`. For this test, load `region` with `col1`. Adjust the test's `.load region` line to `… region str col1 header`.)

- [ ] **Step 2: Run, expect FAIL** (`matrix_cli.hpp` missing): `clang++ -std=c++20 -O2 test_cli.cpp -o /tmp/tcli` → error.

- [ ] **Step 3: Implement `matrix_cli.hpp`** — the loop + dot-commands. `matrix_repl` reads lines; a CLI-local `next_id_` (starts at 1) assigns column ids; a `name→id` is via `eng.column_id`. Tokenise a dot-command on spaces.

```cpp
#pragma once
#include "compute_mock.cpp"
#include <istream>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

namespace matrixcli_detail {
inline std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n"); return s.substr(a, b - a + 1);
}
inline std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> t; std::istringstream is(s); std::string w; while (is >> w) t.push_back(w); return t;
}
inline const char* type_name(MatrixType t) { return t == MatrixType::I64 ? "i64" : t == MatrixType::F64 ? "f64" : "u32"; }
inline const char* tier_name(MemorySpace m) { return m == MemorySpace::DEVICE ? "DEVICE" : m == MemorySpace::COLD ? "COLD" : "HOST"; }
}

// Forward decl: the SQL router (Task 3). Returns false only on a fatal stream error (never for a query error,
// which it formats to `out`).
inline void matrix_cli_run_sql(const std::string& line, std::ostream& out, CPUMockEngine& eng);

// Read commands/queries from `in`, write results to `out`. Returns 0 on clean exit (EOF or .quit).
inline int matrix_repl(std::istream& in, std::ostream& out, CPUMockEngine& eng) {
    using namespace matrixcli_detail;
    uint64_t next_id = 1;
    std::string raw;
    while (std::getline(in, raw)) {
        const std::string line = trim(raw);
        if (line.empty()) continue;
        if (line[0] == '.') {
            const std::vector<std::string> tk = split_ws(line);
            const std::string& cmd = tk[0];
            if (cmd == ".quit" || cmd == ".exit") break;
            else if (cmd == ".help") {
                out << "commands: .load <csv> <name> <u32|i64|f64|str> [colN] [header|noheader] | "
                       ".tables | .columns | .stats | .help | .quit\n"
                       "queries:  SELECT COUNT|SUM|MIN|MAX|AVG(col) [WHERE col <op> v] [GROUP BY key] | "
                       "SELECT col [WHERE ...]\n";
            }
            else if (cmd == ".tables") {
                for (const std::string& t : eng.tables()) out << t << "\n";
            }
            else if (cmd == ".columns") {
                out << "id\tname\ttype\trows\ttier\n";
                for (const ColumnInfo& c : eng.catalog_columns())
                    out << c.id << "\t" << c.name << "\t" << type_name(c.type) << "\t" << c.rows << "\t" << tier_name(c.tier) << "\n";
            }
            else if (cmd == ".stats") {
                const EngineStats st = eng.stats();
                out << "catalog_columns=" << st.catalog_columns << " host_mb=" << st.host_resident_bytes / (1024*1024)
                    << " cold_mb=" << st.cold_resident_bytes / (1024*1024) << " cold_borrows=" << st.cold_borrows
                    << " rebalances=" << st.rebalances << " migrations=" << st.migrations
                    << " queries=" << st.query_count << " p50_ns=" << eng.query_latency_percentile_ns(0.50)
                    << " p99_ns=" << eng.query_latency_percentile_ns(0.99) << "\n";
            }
            else if (cmd == ".load") {
                // .load <path> <name> <type> [colN] [header|noheader]
                if (tk.size() < 4) { out << "Error: usage: .load <csv> <name> <u32|i64|f64|str> [colN] [header|noheader]\n"; continue; }
                const std::string& path = tk[1]; const std::string& name = tk[2]; const std::string& type = tk[3];
                size_t col = 0; bool header = true;
                for (size_t i = 4; i < tk.size(); ++i) {
                    if (tk[i] == "noheader") header = false;
                    else if (tk[i] == "header") header = true;
                    else if (tk[i].rfind("col", 0) == 0) { col = static_cast<size_t>(std::stoul(tk[i].substr(3))); }
                }
                const uint64_t id = next_id++;
                bool okl = false;
                if      (type == "u32") okl = eng.load_column_from_csv(id, path, col, header);
                else if (type == "i64") okl = eng.load_column_from_csv_i64(id, path, col, header);
                else if (type == "f64") okl = eng.load_column_from_csv_f64(id, path, col, header);
                else if (type == "str") okl = eng.load_string_column_from_csv(id, path, col, header);
                else { out << "Error: unknown type '" << type << "' (use u32|i64|f64|str)\n"; --next_id; continue; }
                if (!okl) { out << "Error: could not load " << path << " (col " << col << ")\n"; --next_id; continue; }
                eng.name_column(id, name);
                out << "loaded " << eng.catalog_columns_rows(id) << " rows into \"" << name << "\" (" << type << ", col " << col << ")\n";
            }
            else out << "Error: unknown command '" << cmd << "' (try .help)\n";
        } else {
            matrix_cli_run_sql(line, out, eng);   // Task 3
        }
    }
    return 0;
}
```
(`eng.catalog_columns_rows(id)` is a tiny helper to fetch a column's row count for the message — add it as
`size_t catalog_columns_rows(uint64_t id) const { return column_rows(id); }` next to `column_rows`, OR just
call the existing `column_rows(id)` if it is public. Verify `column_rows` visibility; if private, add the
public one-liner. Use whichever resolves.)

- [ ] **Step 4: Implement a placeholder `matrix_cli_run_sql`** that prints `Error: queries not yet wired\n`
  so Task 2 compiles/links; Task 3 replaces it.

- [ ] **Step 5: Run** `clang++ -std=c++20 -O2 test_cli.cpp -o /tmp/tcli && /tmp/tcli` → `[cli dot-commands] ok`. Fix the test's `region` load to `… region str col1 header`.

- [ ] **Step 6: Commit** — `git commit -am "feat(cli): matrix_repl loop + dot-commands (.load/.tables/.columns/.stats/.help/.quit)"`

---

### Task 3: SQL router + typed/string-decoded output

**Files:**
- Modify: `matrix_cli.hpp` — replace the placeholder `matrix_cli_run_sql` with the real router + formatter.
- Test: `test_cli.cpp` — add query lines + assert decoded output.

- [ ] **Step 1: Add query assertions to `test_cli.cpp`** (after the dot-command session; reuse `eng`):

```cpp
    std::istringstream q(
        "SELECT SUM(amount)\n"                          // scalar
        "SELECT SUM(amount) GROUP BY region\n"          // grouped over a string key -> decoded labels
        "SELECT AVG(amount)\n"                          // avg
        "SELECT region\n"                               // projection (decoded strings)
        "SELECT FROM nonsense\n"                        // bad -> Error, session continues
        ".quit\n");
    std::ostringstream qo; matrix_repl(q, qo, eng);
    const std::string r = qo.str();
    assert(has(r, "1880"));                             // SUM(amount) = 10+900+20+950
    assert(has(r, "books") && has(r, "games") && has(r, "music"));   // grouped labels decoded
    assert(has(r, "470"));                              // AVG = 1880/4
    assert(has(r, "Error:"));                           // bad query reported, not crashed
    std::printf("[cli queries] ok\n");
```

- [ ] **Step 2: Run, expect FAIL** (placeholder prints "not yet wired").

- [ ] **Step 3: Implement the router + formatter** (replace the placeholder):

```cpp
namespace matrixcli_detail {
inline std::string up(std::string s) { for (char& c : s) c = static_cast<char>(std::toupper((unsigned char)c)); return s; }
// Decode one engine-encoded uint64 result by its column type, to text.
inline std::string decode_val(CPUMockEngine& eng, uint64_t col, uint64_t v) {
    if (eng.string_dict_size(col) > 0) return eng.string_decode(col, static_cast<uint32_t>(v));
    switch (eng.column_type(col)) {
        case MatrixType::I64: return std::to_string(static_cast<int64_t>(v));
        case MatrixType::F64: { std::ostringstream o; o << matrix_bit_cast<double>(v); return o.str(); }
        default:              return std::to_string(v);
    }
}
}
inline void matrix_cli_run_sql(const std::string& line, std::ostream& out, CPUMockEngine& eng) {
    using namespace matrixcli_detail;
    const std::string U = up(line);
    if (U.find("AVG(") != std::string::npos) {                         // AVG path
        const std::vector<double> a = eng.avg_query(line);
        if (a.empty()) { out << "Error: could not run AVG query\n"; return; }
        if (a.size() == 1) out << a[0] << "\n";
        else for (size_t g = 0; g < a.size(); ++g) out << "group " << g << " │ " << a[g] << "\n";
        return;
    }
    if (line.find('(') != std::string::npos) {                         // COUNT/SUM/MIN/MAX (scalar or grouped)
        MatrixQuery q;
        if (eng.parse_query(line, q) != MatrixQueryStatus::OK) { out << "Error: could not parse query\n"; return; }
        std::vector<uint64_t> o;
        if (eng.execute_query(q, o) != MatrixQueryStatus::OK) { out << "Error: query rejected\n"; return; }
        if (!q.grouped) { out << (o.empty() ? "" : decode_val(eng, q.value_col, o[0])) << "\n"; return; }
        for (uint32_t g = 0; g < o.size(); ++g) {
            const std::string label = eng.string_dict_size(q.key_col) > 0 ? eng.string_decode(q.key_col, g) : std::to_string(g);
            out << label << " │ " << decode_val(eng, q.value_col, o[g]) << "\n";
        }
        return;
    }
    // projection: SELECT col [WHERE ...] [LIMIT n]
    const std::vector<uint64_t> o = eng.project_query(line);
    // resolve the projected column to decode by type: the token after SELECT
    const std::vector<std::string> tk = split_ws(line);
    uint64_t pcol = (tk.size() >= 2) ? eng.column_id(tk[1]) : 0;
    if (pcol == 0) { out << "Error: could not run query (unknown column?)\n"; return; }
    const size_t cap = 100;
    for (size_t i = 0; i < o.size() && i < cap; ++i) out << decode_val(eng, pcol, o[i]) << "\n";
    if (o.size() > cap) out << "… (" << o.size() << " rows, showing " << cap << ")\n";
}
```
(`project_query` returning `{}` for an unknown/aggregate line and an empty-but-valid projection are
indistinguishable here; the `pcol == 0` check catches the unknown-column case, and an empty projection prints
nothing — acceptable for v1. The bad line `SELECT FROM nonsense` → no `(`, projection path, `tk[1]`=="FROM" →
`column_id("FROM")==0` → `Error:`.)

- [ ] **Step 4: Run** `clang++ -std=c++20 -O1 -fsanitize=address,undefined test_cli.cpp -o /tmp/tcli && /tmp/tcli` → `[cli queries] ok` + `ALL CLI TESTS PASSED`.

- [ ] **Step 5: Run** `./run_tests.sh` (test_cli auto-discovered) + `SAN=1 ./run_tests.sh` green.

- [ ] **Step 6: Commit** — `git commit -am "feat(cli): SQL router + typed/string-decoded output (scalar/grouped/AVG/projection)"`

---

### Task 4: `matrixdb_cli.cpp` main + wiring + docs

**Files:**
- Create: `matrixdb_cli.cpp` — thin main.
- Modify: `make_notebook.py` (SOURCES + a build cell), `README.md`, memory.

- [ ] **Step 1: Write the main.**

```cpp
// matrixdb — interactive CLI/REPL over the CPU engine. Build:
//   clang++ -std=c++20 -O2 matrixdb_cli.cpp -o matrixdb
//   ./matrixdb                 # interactive (reads SQL/.commands from stdin)
//   ./matrixdb -c "SELECT ..." # one-shot
//   ./matrixdb -f script.sql   # run a file
#include "matrix_cli.hpp"
#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
int main(int argc, char** argv) {
    CPUMockEngine eng;
    if (argc >= 3 && std::string(argv[1]) == "-c") { std::istringstream in(std::string(argv[2]) + "\n"); return matrix_repl(in, std::cout, eng); }
    if (argc >= 3 && std::string(argv[1]) == "-f") { std::ifstream f(argv[2]); if (!f) { std::cerr << "cannot open " << argv[2] << "\n"; return 1; } return matrix_repl(f, std::cout, eng); }
    return matrix_repl(std::cin, std::cout, eng);
}
```

- [ ] **Step 2: Verify the binary** builds + runs a one-shot:
`clang++ -std=c++20 -O2 matrixdb_cli.cpp -o /tmp/matrixdb && printf '.help\n.quit\n' | /tmp/matrixdb` → prints the help, exits 0. And `/tmp/matrixdb -c "SELECT 1"` style smoke (will print an Error for an unknown column, not crash).

- [ ] **Step 3: Wire into `make_notebook.py`** — add `"matrix_cli.hpp"`, `"matrixdb_cli.cpp"`, `"test_cli.cpp"` to SOURCES; add a md+code cell building/running `test_cli.cpp`; regenerate (`python3 make_notebook.py`). Note: `matrixdb_cli.cpp` has its own `main`, so it must NOT be compiled into other test binaries — it's only built standalone (it's a source artifact in the notebook, not part of `run_tests` which globs `test_*.cpp`).

- [ ] **Step 4: Update `README.md`** — add a "Use it (CLI)" section: build `matrixdb_cli.cpp`, `.load` a CSV, run a query; note `-c`/`-f`. Update memory `matrixdb-state.md` (CLI/REPL landed; Core scope; deferred forms).

- [ ] **Step 5: Final verify** — `./run_tests.sh` + `SAN=1 ./run_tests.sh` green (incl. test_cli); the `matrixdb` binary runs an end-to-end `.load`+query session.

- [ ] **Step 6: Commit** — `git commit -am "feat(cli): matrixdb_cli main (-c/-f/REPL) + notebook + docs; the engine is now a usable tool"`

---

## Self-Review

**Spec coverage:** shape/files (Tasks 2,4) ✓; loop+commands (Task 2) ✓; SQL router (Task 3) ✓; typed/string-decoded formatting incl. grouped string labels + projection cap (Task 3) ✓; `.load` incl. `str` (Tasks 1,2) ✓; introspection/stats (Task 2) ✓; testing (Tasks 1–3 in `test_cli.cpp`/`test_typed_csv.cpp`) ✓; `-c`/`-f`/interactive (Task 4) ✓; scope/non-goals honored (no HAVING/top-N/multi-agg/.save/network) ✓.

**Placeholder scan:** none — Task 2's `matrix_cli_run_sql` placeholder is explicitly a one-step scaffold replaced in Task 3 (not a vague TODO). The `column_rows` visibility note is a concrete "verify + add a one-liner if private" instruction.

**Type consistency:** `matrix_repl(std::istream&, std::ostream&, CPUMockEngine&)` and `matrix_cli_run_sql(const std::string&, std::ostream&, CPUMockEngine&)` are consistent across tasks; `decode_val` / `up` / `trim` / `split_ws` live in `matrixcli_detail`; `load_string_column_from_csv` (Task 1) is called in Task 2's `.load`. The `.load region` test line is corrected to `col1` in Task 2 Step 5 / Task 3 (the CSV has `amount` in col 0, `region` in col 1).

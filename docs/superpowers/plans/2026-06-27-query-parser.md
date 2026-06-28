# Text Query Parser Implementation Plan — scalar subset (DM-4)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** `parse_query(sql, out)` parses `SELECT <agg>(<col>) [WHERE <col> <op> <val> [AND <val>]]` into a `MatrixQuery` — resolving names (DM-2) and placing the bound by column type (DM-3). Graceful `ERR_PARSE` on malformed input.

**Spec:** `docs/superpowers/specs/2026-06-27-query-parser-design.md` (contains the full tokenizer + parser + `set_bound` code).

---

### Task 1: ERR_PARSE + tokenizer + parse_query + set_bound + test

**Files:** Modify `compute.hpp`, `compute_mock.cpp`; Create `test_query_parser.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_query_parser.cpp`:

```cpp
// CPU test for the text query parser (DM-4): parse_query parses scalar SELECT/WHERE into a MatrixQuery
// (name resolution + type-aware bound placement); graceful ERR_PARSE / ERR_UNKNOWN_COLUMN on bad input.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <bit>
#include <string>
#include <vector>
#include <iostream>

static void test_happy() {
    std::vector<uint32_t> q = {1, 2, 3, 4, 5};                 // qty, sum 15
    std::vector<int64_t>  r = {-100, 5000000000LL, 50, 2000000000LL};  // revenue
    std::vector<double>   t = {1.5, 3.5, 4.0, 2.0};            // rate
    CPUMockEngine eng;
    eng.load_scan_column(3, q.data(), q.size());     eng.name_column(3, "qty");
    eng.load_scan_column_i64(7, r.data(), r.size()); eng.name_column(7, "revenue");
    eng.load_scan_column_f64(9, t.data(), t.size()); eng.name_column(9, "rate");

    MatrixQuery m; std::vector<uint64_t> o;
    // unfiltered scalar
    assert(eng.parse_query("SELECT SUM(qty)", m) == MatrixQueryStatus::OK);
    assert(m.value_col == 3 && m.agg == AGG_SUM && !m.has_filter);
    eng.execute_query(m, o); assert(o[0] == 15);
    // int64 GT bound -> lo_i64 (NOT threshold)
    assert(eng.parse_query("SELECT COUNT(revenue) WHERE revenue > 1000000000", m) == MatrixQueryStatus::OK);
    assert(m.value_col == 7 && m.agg == AGG_COUNT && m.has_filter && m.cmp == MatrixCmp::GT
           && m.lo_i64 == 1000000000LL && m.threshold == 0 && "int64 bound in lo_i64");
    eng.execute_query(m, o); assert(o[0] == 2 && "revenue>1e9 -> {5e9, 2e9}");
    // double LE bound -> lo_f64
    assert(eng.parse_query("SELECT MAX(rate) WHERE rate <= 3.5", m) == MatrixQueryStatus::OK);
    assert(m.cmp == MatrixCmp::LE && m.lo_f64 == 3.5 && "double bound in lo_f64");
    eng.execute_query(m, o); assert(std::bit_cast<double>(o[0]) == 3.5);
    // BETWEEN (int64)
    assert(eng.parse_query("SELECT SUM(revenue) WHERE revenue BETWEEN -100 AND 5000000000", m) == MatrixQueryStatus::OK);
    assert(m.cmp == MatrixCmp::BETWEEN && m.lo_i64 == -100 && m.hi_i64 == 5000000000LL);
    eng.execute_query(m, o); assert(static_cast<int64_t>(o[0]) == -100 + 5000000000LL + 50 + 2000000000LL);
    // case-insensitive + spacing
    assert(eng.parse_query("select sum ( qty )", m) == MatrixQueryStatus::OK && m.value_col == 3);
    std::cout << "[parser happy] ok\n";
}

static void test_errors() {
    std::vector<uint32_t> q = {1, 2, 3};
    CPUMockEngine eng; eng.load_scan_column(3, q.data(), q.size()); eng.name_column(3, "qty");
    MatrixQuery m;
    assert(eng.parse_query("", m) == MatrixQueryStatus::ERR_PARSE);
    assert(eng.parse_query("SELECT FOO(qty)", m) == MatrixQueryStatus::ERR_PARSE);
    assert(eng.parse_query("SELECT SUM(nosuchcol)", m) == MatrixQueryStatus::ERR_UNKNOWN_COLUMN);
    assert(eng.parse_query("SELECT SUM(qty", m) == MatrixQueryStatus::ERR_PARSE);          // missing )
    assert(eng.parse_query("SELECT SUM(qty) WHERE qty >", m) == MatrixQueryStatus::ERR_PARSE);   // missing value
    assert(eng.parse_query("SELECT SUM(qty) WHERE qty > x", m) == MatrixQueryStatus::ERR_PARSE); // non-numeric
    assert(eng.parse_query("SELECT SUM(qty) WHERE qty BETWEEN 1 5", m) == MatrixQueryStatus::ERR_PARSE); // missing AND
    assert(eng.parse_query("SELECT SUM(qty) GROUP BY qty", m) == MatrixQueryStatus::ERR_PARSE);  // unsupported tail
    assert(eng.parse_query("SELECT SUM(qty) extra", m) == MatrixQueryStatus::ERR_PARSE);    // trailing junk
    std::cout << "[parser errors] ok\n";
}

int main() { test_happy(); test_errors();
    std::cout << "ALL QUERY-PARSER TESTS PASSED\n"; return 0; }
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_query_parser.cpp -o /tmp/tqparse && /tmp/tqparse` → FAIL to compile (`parse_query` / `MatrixQueryStatus::ERR_PARSE` undeclared).

- [ ] **Step 3: Add ERR_PARSE** — In `compute.hpp`, append `, ERR_PARSE` to `enum class MatrixQueryStatus` (after `ERR_UNSUPPORTED_TYPE`).

- [ ] **Step 4: Add the parser to compute_mock.cpp** — Add `#include <cctype>` to the includes (for `toupper`/`isspace`; `<charconv>`/`<cstdlib>`/`<cerrno>` arrive via `csv_ingest.hpp`, already included — but add them explicitly too if any symbol is unresolved). Add the free `matrixparse_tokenize` (spec §3) above the class (or in an anonymous detail spot before `CPUMockEngine`). Add `parse_query` as a PUBLIC method (near `execute_query`) and `set_bound` as a PRIVATE method (spec §3, both verbatim).

- [ ] **Step 5: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_query_parser.cpp -o /tmp/tqparse && /tmp/tqparse` → PASS: `[parser happy] ok`, `[parser errors] ok`, `ALL QUERY-PARSER TESTS PASSED`. Zero warnings.

- [ ] **Step 6: Confirm no regression** — `compute.hpp` (enum) + `compute_mock.cpp` changed; these MUST still pass unmodified:
  - `for t in test_query test_query_validation test_query_predicates test_schema test_typed_columns test_typed_double test_server test_audit; do clang++ -std=c++20 -O2 $t.cpp -o /tmp/$t 2>/dev/null && /tmp/$t | tail -1; done`
  - `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`.
  - Build the parser test with `-DNDEBUG` and run (confirm malformed input returns a status, no abort).
  If any differ, STOP / report BLOCKED.

- [ ] **Step 7: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add compute.hpp compute_mock.cpp test_query_parser.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: text query parser (DM-4) — parse_query parses scalar SELECT/WHERE into a MatrixQuery (name + type aware)"
```

---

### Task 2: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (35 tests).** Run `./run_tests.sh` (the CI gate) — expect `ALL GREEN (35 tests + oracle)`, exit 0. (It auto-discovers `test_query_parser.cpp`.) If any fail, STOP / report BLOCKED.

- [ ] **Step 2: Notebook** — add `"test_query_parser.cpp"` to `make_notebook.py` SOURCES right after `"test_kv_range.cpp"`; add a run cell after the kv-range run cell (`test_kv_range.cpp` → `/tmp/tkr`):
```python
    md("### Text query parser\n"
       "parse_query turns a SQL-ish string (SELECT AGG(col) [WHERE col <op> val [AND val]]) into a "
       "MatrixQuery — resolving column names and placing the bound by column type; malformed input is a "
       "graceful ERR_PARSE, never a crash."),
    code("!clang++ -std=c++20 -O2 test_query_parser.cpp -o /tmp/tqparse 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_query_parser.cpp -o /tmp/tqparse; /tmp/tqparse"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 54 source files embedded`. Verify `grep -o "test_query_parser.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed query-parser test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** ERR_PARSE (§2)→T1S3; tokenizer + parse_query + set_bound (§3)→T1S4; happy paths (each type + BETWEEN + case-insensitive) + type-aware bound placement + all graceful errors + NDEBUG release-safety (§4)→T1S1/S6; regression + oracle (§4)→T1S6; suite (run_tests.sh)+notebook→T2. ✓
**Placeholders:** none — tokenizer/parser/set_bound verbatim in spec §3. **Type consistency:** `parse_query(sql, MatrixQuery&)→MatrixQueryStatus`, `set_bound(MatrixType, MatrixQuery&, bool lo, string)→bool`, `matrixparse_tokenize(string)→vector<string>`; bound fields by type (`threshold`/`upper`, `lo_i64`/`hi_i64`, `lo_f64`/`hi_f64`) consistent with QRY-3/DM-3b/e. `<cctype>` added; `<charconv>`/`<cstdlib>`/`<cerrno>` via csv_ingest.hpp. No exhaustive `MatrixQueryStatus` switch exists, so appending `ERR_PARSE` is safe. Oracle/execute path untouched (parse is read-only).

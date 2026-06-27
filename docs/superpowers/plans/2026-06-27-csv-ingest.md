# CSV Ingest Implementation Plan — DM-5b

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Ingest one uint32 column from a CSV file straight into the tiered catalog, with graceful (non-crashing) handling of malformed input.

**Spec:** `docs/superpowers/specs/2026-06-27-csv-ingest-design.md`

**Architecture:** New header-only `matrix_read_csv_column` (line-at-a-time `std::getline` + `std::from_chars`, returns bool) + a thin `CPUMockEngine::load_column_from_csv` forwarding to `load_scan_column`. CSV is untrusted → bad data returns false, never aborts (cf. binary `column_io.hpp` which aborts on corruption of our own format).

---

### Task 1: csv_ingest.hpp + engine method + test

**Files:** Create `csv_ingest.hpp`, `test_csv_ingest.cpp`; Modify `compute_mock.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_csv_ingest.cpp`:

```cpp
// CPU test for CSV ingest (matrix_read_csv_column + CPUMockEngine::load_column_from_csv).
#include "server.hpp"        // pulls in compute_mock.cpp (engine) — execute_query, MatrixQueryStatus
#include "csv_ingest.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <iostream>

static std::string write_tmp(const std::string& name, const std::string& body) {
    const std::string path = "/tmp/" + name;
    std::ofstream(path) << body;
    return path;
}

static void test_parser() {
    std::vector<uint32_t> out;

    // Basic: two columns, pick each.
    std::string p = write_tmp("mdb_csv_basic.csv", "1,10\n2,20\n3,30\n");
    assert(matrix_read_csv_column(p, 1, false, ',', out) && out == (std::vector<uint32_t>{10, 20, 30}));
    assert(matrix_read_csv_column(p, 0, false, ',', out) && out == (std::vector<uint32_t>{1, 2, 3}));

    // Header skip.
    p = write_tmp("mdb_csv_hdr.csv", "key,val\n5,50\n6,60\n");
    assert(matrix_read_csv_column(p, 1, true, ',', out) && out == (std::vector<uint32_t>{50, 60}));

    // Custom delimiter.
    p = write_tmp("mdb_csv_semi.csv", "7;70\n8;80\n");
    assert(matrix_read_csv_column(p, 1, false, ';', out) && out == (std::vector<uint32_t>{70, 80}));

    // Empty file -> true, empty. Header-only -> true, empty.
    p = write_tmp("mdb_csv_empty.csv", "");
    assert(matrix_read_csv_column(p, 0, false, ',', out) && out.empty());
    p = write_tmp("mdb_csv_hdronly.csv", "k,v\n");
    assert(matrix_read_csv_column(p, 0, true, ',', out) && out.empty());

    // CRLF tolerance.
    p = write_tmp("mdb_csv_crlf.csv", "1,2\r\n3,4\r\n");
    assert(matrix_read_csv_column(p, 0, false, ',', out) && out == (std::vector<uint32_t>{1, 3}));

    // Graceful failures: each returns false and clears out.
    assert(!matrix_read_csv_column("/tmp/mdb_csv_does_not_exist.csv", 0, false, ',', out));
    p = write_tmp("mdb_csv_short.csv", "1,2\n3\n");          // row 2 has no field index 1
    assert(!matrix_read_csv_column(p, 1, false, ',', out) && out.empty());
    p = write_tmp("mdb_csv_nonint.csv", "1,x\n");
    assert(!matrix_read_csv_column(p, 1, false, ',', out));
    p = write_tmp("mdb_csv_junk.csv", "12x\n");              // trailing junk -> not a full integer
    assert(!matrix_read_csv_column(p, 0, false, ',', out));
    p = write_tmp("mdb_csv_over.csv", "5000000000\n");       // > UINT32_MAX
    assert(!matrix_read_csv_column(p, 0, false, ',', out));
    std::cout << "[csv parser] ok\n";
}

static void test_engine_ingest() {
    const std::string wal = "/tmp/mdb_csv_eng.bin"; std::remove(wal.c_str());
    CPUMockEngine eng(0, wal);
    const std::string p = write_tmp("mdb_csv_eng.csv", "k,v\n10,100\n20,200\n30,300\n");

    assert(eng.load_column_from_csv(7, p, 1, /*has_header=*/true));      // values {100,200,300}
    MatrixQuery q{}; q.value_col = 7; q.agg = AGG_SUM;
    std::vector<uint64_t> r;
    assert(eng.execute_query(q, r) == MatrixQueryStatus::OK);
    assert(r.size() == 1 && r[0] == 600 && "SUM of ingested column");   // 100+200+300

    // Malformed CSV -> false, and NO catalog entry created (query on id 8 is unknown).
    const std::string bad = write_tmp("mdb_csv_bad.csv", "1,x\n");
    assert(!eng.load_column_from_csv(8, bad, 1));
    MatrixQuery q8{}; q8.value_col = 8; q8.agg = AGG_COUNT;
    std::vector<uint64_t> r8;
    assert(eng.execute_query(q8, r8) == MatrixQueryStatus::ERR_UNKNOWN_COLUMN && "no entry from bad CSV");
    std::remove(wal.c_str());
    std::cout << "[csv engine ingest] ok\n";
}

int main() {
    test_parser();
    test_engine_ingest();
    std::cout << "ALL CSV INGEST TESTS PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_csv_ingest.cpp -o /tmp/tcsv && /tmp/tcsv` → FAIL to compile (`csv_ingest.hpp` missing, `matrix_read_csv_column` / `load_column_from_csv` undeclared).

- [ ] **Step 3: Create `csv_ingest.hpp`** — exactly the contents of spec §2 (the `#pragma once`, includes `<cstdint> <charconv> <fstream> <sstream> <string> <vector>`, and `matrix_read_csv_column` as written there). `<sstream>` may be unused — drop it if `-Wall -Wextra` is clean either way; keep includes minimal.

- [ ] **Step 4: Add the engine method** — In `compute_mock.cpp`, add `#include "csv_ingest.hpp"` right after the `#include "column_io.hpp"` line (line 6), and add `load_column_from_csv` (spec §3) immediately after `load_column_from_file` (after line 122, the closing brace of that method):

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

- [ ] **Step 5: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_csv_ingest.cpp -o /tmp/tcsv && /tmp/tcsv` → PASS, prints `[csv parser] ok`, `[csv engine ingest] ok`, `ALL CSV INGEST TESTS PASSED`. No warnings. (If `<sstream>` triggers no warning either way, fine; the goal is zero warnings.)

- [ ] **Step 6: Confirm no regression** — `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`; `clang++ -std=c++20 -O2 test_column_io.cpp -o /tmp/tcio && /tmp/tcio | tail -1` → `ALL ... PASSED`; `clang++ -std=c++20 -O2 test_query.cpp -o /tmp/tq && /tmp/tq | tail -1` → passes. If any differ, STOP / report BLOCKED.

- [ ] **Step 7: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add csv_ingest.hpp test_csv_ingest.cpp compute_mock.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: CSV ingest — load a uint32 column from a CSV file into the catalog (graceful on malformed input)"
```

---

### Task 2: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (20 tests).**
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by \
         test_query test_observability test_column_io test_catalog_snapshot test_query_validation \
         test_transactions test_server test_security test_audit test_csv_ingest; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 20× `PASS:`. If any fail, STOP / report BLOCKED with `cat /tmp/out_<test>`.

- [ ] **Step 2: Notebook** — add `"csv_ingest.hpp"` to `make_notebook.py` SOURCES right after `"column_io.hpp"`, and `"test_csv_ingest.cpp"` right after `"test_audit.cpp"`; add a run cell after the audit-logging run cell (the one compiling `test_audit.cpp` to `/tmp/taud`):
```python
    md("### CSV ingest\n"
       "load_column_from_csv reads a uint32 column straight out of a CSV file into the tiered "
       "catalog — and reports malformed input gracefully (false, no crash) rather than aborting."),
    code("!clang++ -std=c++20 -O2 test_csv_ingest.cpp -o /tmp/tcsv 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_csv_ingest.cpp -o /tmp/tcsv; /tmp/tcsv"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 39 source files embedded` (37 + csv_ingest.hpp + test_csv_ingest.cpp). Verify `grep -o "test_csv_ingest.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2` and `grep -c "csv_ingest.hpp" matrixdb_colab.ipynb` → `>= 1`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed CSV-ingest test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** `matrix_read_csv_column` (§2)→T1S3; engine `load_column_from_csv` (§3)→T1S4; all parse cases + graceful failures + non-vacuity (§4)→T1S1; regression+notebook (§4 tail)→T2. ✓
**Placeholders:** none. **Type consistency:** `matrix_read_csv_column(path, col_index, has_header, delim, out)→bool` and `load_column_from_csv(id, path, col_index, has_header=false, delim=',')→bool` are identical across spec §2/§3 and plan T1S3/S4/test. Test uses `MatrixQuery{}; .value_col/.agg` + `execute_query→MatrixQueryStatus::{OK,ERR_UNKNOWN_COLUMN}` (matches VAL-1). `test_csv_ingest.cpp` includes `server.hpp` (engine) + `csv_ingest.hpp`, like `test_audit.cpp`'s include of `server.hpp`.

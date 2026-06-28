# Typed CSV Ingest Implementation Plan (DM-3g)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** int64 + double CSV column readers + `load_column_from_csv_i64`/`_f64`, mirroring the u32 DM-5b path. Graceful on malformed input. Additive — u32 CSV untouched.

**Spec:** `docs/superpowers/specs/2026-06-27-typed-csv-ingest-design.md`

---

### Task 1: int64 + double CSV readers + engine methods + test

**Files:** Modify `csv_ingest.hpp`, `compute_mock.cpp`; Create `test_typed_csv.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_typed_csv.cpp`:

```cpp
// CPU test for typed CSV ingest (DM-3g): matrix_read_csv_column_i64/_f64 + load_column_from_csv_i64/_f64.
#include "compute_mock.cpp"
#include "csv_ingest.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <bit>
#include <fstream>
#include <string>
#include <vector>
#include <iostream>

static std::string wr(const std::string& name, const std::string& body) {
    const std::string p = "/tmp/" + name; std::ofstream(p) << body; return p; }

static void test_csv_i64() {
    std::vector<int64_t> out;
    std::string p = wr("mdb_csv_i64.csv", "k,v\n-7,-7\n10,5000000000\n3,3\n");   // negatives + > UINT32_MAX
    assert(matrix_read_csv_column_i64(p, 1, true, ',', out) && out == (std::vector<int64_t>{-7, 5000000000LL, 3}));
    // graceful failures
    assert(!matrix_read_csv_column_i64("/tmp/mdb_csv_nope.csv", 0, false, ',', out));
    p = wr("mdb_csv_i64_bad.csv", "1,x\n");      assert(!matrix_read_csv_column_i64(p, 1, false, ',', out));
    p = wr("mdb_csv_i64_over.csv", "99999999999999999999\n"); assert(!matrix_read_csv_column_i64(p, 0, false, ',', out));
    p = wr("mdb_csv_i64_junk.csv", "12x\n");     assert(!matrix_read_csv_column_i64(p, 0, false, ',', out));
    p = wr("mdb_csv_i64_short.csv", "1,2\n3\n"); assert(!matrix_read_csv_column_i64(p, 1, false, ',', out) && out.empty());
    std::cout << "[csv i64] ok\n";
}

static void test_csv_f64() {
    std::vector<double> out;
    std::string p = wr("mdb_csv_f64.csv", "1.5\n-3.25\n5e9\n0.5\n");   // fractional + negative + exponent
    assert(matrix_read_csv_column_f64(p, 0, false, ',', out) && out == (std::vector<double>{1.5, -3.25, 5e9, 0.5}));
    assert(!matrix_read_csv_column_f64("/tmp/mdb_csv_nope2.csv", 0, false, ',', out));
    p = wr("mdb_csv_f64_bad.csv", "x\n");     assert(!matrix_read_csv_column_f64(p, 0, false, ',', out));
    p = wr("mdb_csv_f64_junk.csv", "1.5x\n"); assert(!matrix_read_csv_column_f64(p, 0, false, ',', out));
    p = wr("mdb_csv_f64_empty.csv", "1,\n");  assert(!matrix_read_csv_column_f64(p, 1, false, ',', out));  // empty field
    std::cout << "[csv f64] ok\n";
}

static void test_engine_typed_csv() {
    CPUMockEngine eng;
    std::string pi = wr("mdb_eng_i64.csv", "k,v\n-7,-7\n10,5000000000\n3,3\n");
    assert(eng.load_column_from_csv_i64(7, pi, 1, true));
    assert(eng.column_type(7) == MatrixType::I64);
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_SUM; std::vector<uint64_t> o;
      assert(eng.execute_query(q, o) == MatrixQueryStatus::OK && static_cast<int64_t>(o[0]) == -7 + 5000000000LL + 3); }
    std::string pf = wr("mdb_eng_f64.csv", "1.5\n-3.25\n0.25\n");
    assert(eng.load_column_from_csv_f64(8, pf, 0));
    assert(eng.column_type(8) == MatrixType::F64);
    { MatrixQuery q{}; q.value_col = 8; q.agg = AGG_SUM; std::vector<uint64_t> o;
      eng.execute_query(q, o); assert(std::bit_cast<double>(o[0]) == 1.5 - 3.25 + 0.25); }
    // malformed -> false, no catalog entry
    std::string pb = wr("mdb_eng_bad.csv", "1,x\n");
    assert(!eng.load_column_from_csv_i64(9, pb, 1));
    { MatrixQuery q{}; q.value_col = 9; q.agg = AGG_COUNT; std::vector<uint64_t> o;
      assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_UNKNOWN_COLUMN && "no entry from bad CSV"); }
    std::cout << "[engine typed csv] ok\n";
}

int main() { test_csv_i64(); test_csv_f64(); test_engine_typed_csv();
    std::cout << "ALL TYPED-CSV TESTS PASSED\n"; return 0; }
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_typed_csv.cpp -o /tmp/ttc && /tmp/ttc` → FAIL to compile (`matrix_read_csv_column_i64`/`_f64`, `load_column_from_csv_i64`/`_f64` undeclared).

- [ ] **Step 3: Add the typed CSV readers to csv_ingest.hpp** — Add `#include <cstdlib>` and `#include <cerrno>` (after the existing includes). Add `matrix_read_csv_column_i64` and `matrix_read_csv_column_f64` (spec §2 verbatim) after the existing `matrix_read_csv_column`.

- [ ] **Step 4: Add the engine methods (compute_mock.cpp)** — Add `load_column_from_csv_i64` and `load_column_from_csv_f64` (spec §3 verbatim) immediately after `load_column_from_csv`.

- [ ] **Step 5: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_typed_csv.cpp -o /tmp/ttc && /tmp/ttc` → PASS: `[csv i64] ok`, `[csv f64] ok`, `[engine typed csv] ok`, `ALL TYPED-CSV TESTS PASSED`. Zero warnings.

- [ ] **Step 6: Confirm no regression** — `csv_ingest.hpp` + `compute_mock.cpp` changed (additive), so these MUST still pass unmodified:
  - `for t in test_csv_ingest test_typed_columns test_typed_double test_typed_snapshot test_query test_column_io; do clang++ -std=c++20 -O2 $t.cpp -o /tmp/$t 2>/dev/null && /tmp/$t | tail -1; done`
  - `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`.
  If any differ, STOP / report BLOCKED.

- [ ] **Step 7: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add csv_ingest.hpp compute_mock.cpp test_typed_csv.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: typed CSV ingest (DM-3g) — int64 + double CSV column readers (graceful on malformed input)"
```

---

### Task 2: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (29 tests).**
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by \
         test_query test_observability test_column_io test_catalog_snapshot test_query_validation \
         test_transactions test_server test_security test_audit test_csv_ingest test_checkpoint \
         test_query_predicates test_typed_columns test_typed_predicates test_typed_grouped \
         test_typed_snapshot test_typed_double test_typed_double_grouped test_typed_csv; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 29× `PASS:`. If any fail, STOP / report BLOCKED.

- [ ] **Step 2: Notebook** — add `"test_typed_csv.cpp"` to `make_notebook.py` SOURCES right after `"test_typed_double_grouped.cpp"`; add a run cell after the grouped-double run cell (`test_typed_double_grouped.cpp` → `/tmp/tdg`):
```python
    md("### Typed CSV ingest (int64 + double)\n"
       "load_column_from_csv_i64 / _f64 ingest signed-64-bit and floating-point columns straight from "
       "CSV (negatives, values beyond uint32, fractions) — gracefully rejecting malformed input."),
    code("!clang++ -std=c++20 -O2 test_typed_csv.cpp -o /tmp/ttc 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_typed_csv.cpp -o /tmp/ttc; /tmp/ttc"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 48 source files embedded`. Verify `grep -o "test_typed_csv.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed typed-CSV test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** matrix_read_csv_column_i64/_f64 (§2)→T1S3; load_column_from_csv_i64/_f64 (§3)→T1S4; i64 (negatives/large/graceful) + f64 (fractional/exponent/graceful) + engine + non-vacuity (§4)→T1S1; regression + oracle (§4)→T1S6; suite+notebook→T2. ✓
**Placeholders:** none. **Type consistency:** `matrix_read_csv_column_i64(path,col,hdr,delim,vector<int64_t>&)→bool`, `_f64(...,vector<double>&)→bool`, `load_column_from_csv_i64/_f64(id,path,col,hdr=false,delim=',')→bool`. i64 via `from_chars` (ec+ptr==end), f64 via `strtod` (errno+endptr==field_end, empty-field guarded). u32 `matrix_read_csv_column`/`load_column_from_csv` untouched. `<cstdlib>`+`<cerrno>` added to csv_ingest.hpp. Oracle path untouched.

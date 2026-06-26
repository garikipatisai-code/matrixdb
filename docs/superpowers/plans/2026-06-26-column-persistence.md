# Binary Column Persistence Implementation Plan — DM-5

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** `save_column` / `load_column_from_file` + a robust binary column format, so the engine can ingest columns from disk and persist them.

**Spec:** `docs/superpowers/specs/2026-06-26-column-persistence-design.md`

---

### Task 1: column_io.hpp + engine save/load

**Files:** Create `column_io.hpp`, `test_column_io.cpp`; Modify `compute_mock.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_column_io.cpp`:

```cpp
// CPU test for binary column persistence (column_io.hpp + engine save/load).
#include "compute_mock.cpp"     // CPUMockEngine (+ includes column_io.hpp after Task changes)
#include "column_io.hpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <cstdio>
#include <iostream>

static void test_direct_roundtrip() {
    std::vector<uint32_t> in(500);
    for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<uint32_t>(i * 3 + 1);
    const std::string path = "/tmp/matrixdb_coltest_500.bin";
    matrix_write_column(path, in.data(), in.size());
    std::vector<uint32_t> out;
    matrix_read_column(path, out);
    assert(out == in && "round-trip preserves the column");
    // file starts with the magic
    FILE* f = std::fopen(path.c_str(), "rb"); assert(f);
    uint32_t magic = 0; assert(std::fread(&magic, sizeof magic, 1, f) == 1); std::fclose(f);
    assert(magic == MATRIX_COLUMN_MAGIC && "file carries the column magic");
    std::remove(path.c_str());
    // empty column round-trips
    std::vector<uint32_t> empty, eout;
    matrix_write_column("/tmp/matrixdb_coltest_0.bin", empty.data(), 0);
    matrix_read_column("/tmp/matrixdb_coltest_0.bin", eout);
    assert(eout.empty());
    std::remove("/tmp/matrixdb_coltest_0.bin");
    std::cout << "[column_io direct round-trip] ok\n";
}

static uint64_t sum_to(uint64_t hi) { uint64_t s = 0; for (uint64_t i = 0; i <= hi; ++i) s += i; return s; }

static void test_engine_save_load_query() {
    const size_t N = 1000;
    std::vector<uint32_t> col(N);
    for (size_t i = 0; i < N; ++i) col[i] = static_cast<uint32_t>(i);
    const std::string path = "/tmp/matrixdb_coltest_eng.bin";

    CPUMockEngine a(0, "", SIZE_MAX);
    a.load_scan_column(7, col.data(), N);
    a.save_column(7, path);

    CPUMockEngine b(0, "", SIZE_MAX);
    b.load_column_from_file(7, path);
    std::vector<uint64_t> out;
    b.execute_query(MatrixQuery{.value_col = 7, .agg = AGG_SUM}, out);
    assert(out.size() == 1 && out[0] == sum_to(N - 1) && "persisted column reloads + queries identically");
    std::remove(path.c_str());
    std::cout << "[engine save->load->query] ok\n";
}

static void test_engine_save_cold_column() {
    const size_t N = 1000;
    const size_t S = N * sizeof(uint32_t);
    std::vector<uint32_t> col(N), dummy(N, 0);
    for (size_t i = 0; i < N; ++i) col[i] = static_cast<uint32_t>(i);
    const std::string path = "/tmp/matrixdb_coltest_cold.bin";

    CPUMockEngine a(0, "", /*host_cap=*/S);   // one-column budget
    a.load_scan_column(7, col.data(), N);
    a.load_scan_column(8, dummy.data(), N);
    for (int r = 0; r < 12; ++r) { DatabaseQuery q{}; matrix_set_scan_target(q, 0u, 8); a.execute_scan(q); }
    assert(a.column_tier(7) == MemorySpace::COLD && "col 7 demoted to SSD");
    a.save_column(7, path);                    // persist a COLD column (borrows from SSD)
    assert(a.column_tier(7) == MemorySpace::COLD && "save returned the borrow");

    CPUMockEngine b(0, "", SIZE_MAX);
    b.load_column_from_file(7, path);
    std::vector<uint64_t> out;
    b.execute_query(MatrixQuery{.value_col = 7, .agg = AGG_SUM}, out);
    assert(out.size() == 1 && out[0] == sum_to(N - 1) && "COLD column persisted + reloaded correctly");
    std::remove(path.c_str());
    std::cout << "[engine save COLD column] ok\n";
}

int main() {
    test_direct_roundtrip();
    test_engine_save_load_query();
    test_engine_save_cold_column();
    std::cout << "ALL COLUMN-IO TESTS PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_column_io.cpp -o /tmp/tcio && /tmp/tcio` → FAIL to compile (`column_io.hpp` missing; `matrix_write_column`/`save_column`/`load_column_from_file` undeclared).

- [ ] **Step 3: Create column_io.hpp** — exactly the contents from the spec §2 (the `#pragma once`, includes, `MATRIX_COLUMN_MAGIC`, `matrix_write_column`, `matrix_read_column`).

- [ ] **Step 4: Wire it into the engine** — In `compute_mock.cpp`, add `#include "column_io.hpp"` with the other includes (after `#include "memory_model.hpp"`). Add the two public methods (from spec §3) after the `stats()` accessor:

```cpp
    void save_column(uint64_t id, const std::string& path) {
        TieredColumn& col = *catalog_.at(id);
        const MemorySpace home = col.tier();
        if (home != MemorySpace::HOST) { ++cold_borrows_; col.migrate_to(MemorySpace::HOST); }
        matrix_write_column(path, reinterpret_cast<const uint32_t*>(col.host_ptr()),
                            col.size_bytes() / sizeof(uint32_t));
        if (home != MemorySpace::HOST) col.migrate_to(home);
    }
    void load_column_from_file(uint64_t id, const std::string& path) {
        std::vector<uint32_t> data;
        matrix_read_column(path, data);
        load_scan_column(id, data.data(), data.size());
    }
```

- [ ] **Step 5: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_column_io.cpp -o /tmp/tcio && /tmp/tcio` → PASS, prints the three `ok` lines + `ALL COLUMN-IO TESTS PASSED`. No warnings. (Reads/writes /tmp files — expected; the test removes them.)

- [ ] **Step 6: Confirm no regression** — `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`; `clang++ -std=c++20 -O2 test_live_tiering.cpp -o /tmp/tlt && /tmp/tlt | tail -1` → `ALL LIVE-TIERING TESTS PASSED`.

- [ ] **Step 7: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add column_io.hpp compute_mock.cpp test_column_io.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: binary column persistence — column_io.hpp + save_column/load_column_from_file"
```

---

### Task 2: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (13 tests).**
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by \
         test_query test_observability test_column_io; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 13× `PASS:`. If any fail, STOP / report BLOCKED.

- [ ] **Step 2: Notebook** — In `make_notebook.py`: add `"column_io.hpp"` to `SOURCES` (with the other headers, e.g. after `"tiered_column.hpp"`/`"migration_executor.hpp"`) AND `"test_column_io.cpp"` (after `"test_observability.cpp"`). Add a run cell after the observability run cell:
```python
    md("### Binary column persistence\n"
       "save_column / load_column_from_file round-trip a column through a binary file (incl. a "
       "COLD-tier column); a reloaded column queries identically."),
    code("!clang++ -std=c++20 -O2 test_column_io.cpp -o /tmp/tcio 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_column_io.cpp -o /tmp/tcio; /tmp/tcio"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 30 source files embedded` (28 + column_io.hpp + test_column_io.cpp). Verify `grep -o "test_column_io.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed column-persistence test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** column_io.hpp format+functions (§2)→T1S3; engine save/load (§3)→T1S4; direct + engine + COLD + empty + non-vacuity (§4)→T1 test; suite+notebook→T2. ✓
**Placeholders:** none. **Type consistency:** `matrix_write_column(const std::string&, const uint32_t*, size_t)`, `matrix_read_column(const std::string&, std::vector<uint32_t>&)`, `MATRIX_COLUMN_MAGIC`, `save_column(uint64_t, const std::string&)`, `load_column_from_file(uint64_t, const std::string&)` consistent T1/T2. Reuses `load_scan_column`, `column_tier`, `execute_query`, `cold_borrows_`, borrow-and-return.

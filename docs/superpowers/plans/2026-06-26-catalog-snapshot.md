# Catalog Snapshot Durability Implementation Plan — CKPT-1

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** `save_catalog(path)` / `load_catalog(path)` — snapshot the whole tiered catalog to one file and restore it into a fresh engine, so the analytical store survives a restart.

**Spec:** `docs/superpowers/specs/2026-06-26-catalog-snapshot-design.md`

---

### Task 1: save_catalog / load_catalog + test

**Files:** Modify `compute_mock.cpp`; Create `test_catalog_snapshot.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_catalog_snapshot.cpp`:

```cpp
// CPU test for catalog snapshot durability (save_catalog / load_catalog).
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <cstdio>
#include <iostream>

static uint64_t sum_of(const std::vector<uint32_t>& v) { uint64_t s = 0; for (uint32_t x : v) s += x; return s; }
static uint64_t engine_sum(CPUMockEngine& e, uint64_t id) {
    std::vector<uint64_t> out; e.execute_query(MatrixQuery{.value_col = id, .agg = AGG_SUM}, out); return out[0];
}

static void test_multicolumn_roundtrip() {
    const size_t N = 1000;
    std::vector<uint32_t> a(N), b(N), c(N);
    for (size_t i = 0; i < N; ++i) { a[i] = i; b[i] = i + 1000; c[i] = static_cast<uint32_t>(i * 2); }
    const std::string path = "/tmp/matrixdb_catsnap.bin";
    CPUMockEngine src(0, "", SIZE_MAX);
    src.load_scan_column(1, a.data(), N);
    src.load_scan_column(2, b.data(), N);
    src.load_scan_column(3, c.data(), N);
    src.save_catalog(path);

    CPUMockEngine dst(0, "", SIZE_MAX);
    dst.load_catalog(path);
    assert(dst.stats().catalog_columns == 3);
    assert(engine_sum(dst, 1) == sum_of(a));
    assert(engine_sum(dst, 2) == sum_of(b));
    assert(engine_sum(dst, 3) == sum_of(c));
    // non-vacuity: the three columns are genuinely distinct
    assert(sum_of(a) != sum_of(b) && sum_of(b) != sum_of(c));
    std::remove(path.c_str());
    std::cout << "[catalog multi-column round-trip] ok\n";
}

static void test_snapshot_with_cold_column() {
    const size_t N = 1000;
    const size_t S = N * sizeof(uint32_t);
    std::vector<uint32_t> a(N), dummy(N, 0);
    for (size_t i = 0; i < N; ++i) a[i] = i;
    const std::string path = "/tmp/matrixdb_catsnap_cold.bin";
    CPUMockEngine src(0, "", /*host_cap=*/S);   // one-column budget
    src.load_scan_column(1, a.data(), N);
    src.load_scan_column(9, dummy.data(), N);
    for (int r = 0; r < 12; ++r) { DatabaseQuery q{}; matrix_set_scan_target(q, 0u, 9); src.execute_scan(q); }
    assert(src.column_tier(1) == MemorySpace::COLD && "col 1 demoted before snapshot");
    src.save_catalog(path);                      // borrows the COLD column to read it
    assert(src.column_tier(1) == MemorySpace::COLD && "save returned the borrow");

    CPUMockEngine dst(0, "", SIZE_MAX);
    dst.load_catalog(path);
    assert(engine_sum(dst, 1) == sum_of(a) && "COLD column restored correctly");
    assert(engine_sum(dst, 9) == 0 && "dummy column restored");
    std::remove(path.c_str());
    std::cout << "[catalog snapshot with COLD column] ok\n";
}

static void test_empty_catalog() {
    const std::string path = "/tmp/matrixdb_catsnap_empty.bin";
    CPUMockEngine src(0, "", SIZE_MAX);
    src.save_catalog(path);
    CPUMockEngine dst(0, "", SIZE_MAX);
    dst.load_catalog(path);
    assert(dst.stats().catalog_columns == 0);
    std::remove(path.c_str());
    std::cout << "[empty catalog snapshot] ok\n";
}

int main() {
    test_multicolumn_roundtrip();
    test_snapshot_with_cold_column();
    test_empty_catalog();
    std::cout << "ALL CATALOG-SNAPSHOT TESTS PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_catalog_snapshot.cpp -o /tmp/tcs2 && /tmp/tcs2` → FAIL to compile (`save_catalog`/`load_catalog` undeclared).

- [ ] **Step 3: Add the catalog magic + methods** — In `compute_mock.cpp`, add a `static constexpr uint32_t MATRIX_CATALOG_MAGIC = 0x4D434154u;` member (with the other constants, e.g. near `REBALANCE_EVERY`), and add the public `save_catalog` and `load_catalog` methods (verbatim from spec §2) after `load_column_from_file`.

- [ ] **Step 4: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_catalog_snapshot.cpp -o /tmp/tcs2 && /tmp/tcs2` → PASS, prints the three `ok` lines + `ALL CATALOG-SNAPSHOT TESTS PASSED`. No warnings. (Reads/writes /tmp — expected; the test removes its files.)

- [ ] **Step 5: Confirm no regression** — `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`; `clang++ -std=c++20 -O2 test_column_io.cpp -o /tmp/tcio && /tmp/tcio | tail -1` → `ALL COLUMN-IO TESTS PASSED`.

- [ ] **Step 6: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add compute_mock.cpp test_catalog_snapshot.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: catalog snapshot durability — save_catalog/load_catalog (whole analytical store survives restart)"
```

---

### Task 2: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (14 tests).**
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by \
         test_query test_observability test_column_io test_catalog_snapshot; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 14× `PASS:`. If any fail, STOP / report BLOCKED.

- [ ] **Step 2: Notebook** — add `"test_catalog_snapshot.cpp"` to `make_notebook.py` SOURCES (after `"test_column_io.cpp"`); add a run cell after the column-persistence run cell:
```python
    md("### Catalog snapshot durability\n"
       "save_catalog / load_catalog snapshot the whole tiered catalog to one file and restore it "
       "into a fresh engine (incl. COLD columns) — the analytical store survives a restart."),
    code("!clang++ -std=c++20 -O2 test_catalog_snapshot.cpp -o /tmp/tcs2 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_catalog_snapshot.cpp -o /tmp/tcs2; /tmp/tcs2"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 31 source files embedded`. Verify `grep -o "test_catalog_snapshot.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed catalog-snapshot test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** magic + save_catalog/load_catalog (§2)→T1S3; multi-column + COLD + empty + non-vacuity (§3)→T1 test; suite+notebook→T2. ✓
**Placeholders:** none. **Type consistency:** `MATRIX_CATALOG_MAGIC`, `save_catalog(const std::string&)`, `load_catalog(const std::string&)`; reuses `load_scan_column`, `column_tier`, `execute_query`, `stats().catalog_columns`, `cold_borrows_`, borrow-and-return. The test's `engine_sum`/`sum_of` helpers are self-contained.

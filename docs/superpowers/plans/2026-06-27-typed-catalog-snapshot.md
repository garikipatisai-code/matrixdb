# Typed Catalog Snapshot Implementation Plan (DM-3d)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Make `save_catalog`/`load_catalog` type-aware so an int64 (or mixed u32+int64) catalog survives a restart. Catalog-snapshot only; single-column `column_io` stays u32-guarded.

**Spec:** `docs/superpowers/specs/2026-06-27-typed-catalog-snapshot-design.md`

---

### Task 1: typed catalog snapshot + test

**Files:** Modify `compute_mock.cpp`; Create `test_typed_snapshot.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_typed_snapshot.cpp`:

```cpp
// CPU test for typed catalog snapshot (DM-3d): save_catalog/load_catalog round-trip a mixed
// u32 + int64 catalog (incl. negatives and > UINT32_MAX), with types and values preserved.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <iostream>

static int64_t i64_sum(const std::vector<int64_t>& v) { int64_t s = 0; for (int64_t x : v) s += x; return s; }
static uint64_t u32_sum(const std::vector<uint32_t>& v) { uint64_t s = 0; for (uint32_t x : v) s += x; return s; }

static void test_mixed_roundtrip() {
    const std::string path = "/tmp/mdb_typed_catalog.bin"; std::remove(path.c_str());
    std::vector<uint32_t> u(100); for (size_t i = 0; i < u.size(); ++i) u[i] = static_cast<uint32_t>(i);
    std::vector<int64_t>  s = {-7, 0, 5, 5000000000LL, -3, 2147483648LL, 100, -100};
    {
        CPUMockEngine eng;
        eng.load_scan_column(3, u.data(), u.size());          // u32
        eng.load_scan_column_i64(7, s.data(), s.size());      // int64 (negatives + > UINT32_MAX)
        eng.save_catalog(path);
    }
    {
        CPUMockEngine eng;                                    // fresh engine
        eng.load_catalog(path);
        assert(eng.column_type(3) == MatrixType::U32 && eng.column_type(7) == MatrixType::I64 && "types restored");
        // u32 column value check
        MatrixQuery qu{}; qu.value_col = 3; qu.agg = AGG_SUM; std::vector<uint64_t> ou;
        assert(eng.execute_query(qu, ou) == MatrixQueryStatus::OK && ou[0] == u32_sum(u) && "u32 column restored");
        // int64 column value checks (SUM/MIN/MAX) — the negatives + large value must survive
        MatrixQuery qs{}; qs.value_col = 7; qs.agg = AGG_SUM; std::vector<uint64_t> os;
        assert(eng.execute_query(qs, os) == MatrixQueryStatus::OK && static_cast<int64_t>(os[0]) == i64_sum(s) && "int64 SUM restored");
        MatrixQuery qmax{}; qmax.value_col = 7; qmax.agg = AGG_MAX; std::vector<uint64_t> omax;
        eng.execute_query(qmax, omax); assert(static_cast<int64_t>(omax[0]) == 5000000000LL && "int64 MAX (>UINT32_MAX) restored");
        MatrixQuery qmin{}; qmin.value_col = 7; qmin.agg = AGG_MIN; std::vector<uint64_t> omin;
        eng.execute_query(qmin, omin); assert(static_cast<int64_t>(omin[0]) == -100 && "int64 MIN (negative) restored");
    }
    std::remove(path.c_str());
    std::cout << "[mixed catalog roundtrip] ok\n";
}

static void test_empty_catalog() {
    const std::string path = "/tmp/mdb_empty_catalog.bin"; std::remove(path.c_str());
    { CPUMockEngine eng; eng.save_catalog(path); }
    { CPUMockEngine eng; eng.load_catalog(path); }   // must not crash
    std::remove(path.c_str());
    std::cout << "[empty catalog] ok\n";
}

int main() {
    test_mixed_roundtrip();
    test_empty_catalog();
    std::cout << "ALL TYPED-SNAPSHOT TESTS PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_typed_snapshot.cpp -o /tmp/tts && /tmp/tts` → FAIL: `save_catalog` aborts on the int64 column (current guard `save_catalog: typed-column persistence is DM-3c`), so the test dies. (Confirm it's the abort, proving the pre-DM-3d behavior.)

- [ ] **Step 3: Bump the magic + make save_catalog type-aware** — In `compute_mock.cpp`:
  - Change the `MATRIX_CATALOG_MAGIC` member value to `0x4D434131u` and its comment to `'MCA1' — typed catalog snapshot v1 (DM-3d)`.
  - In `save_catalog`'s per-column loop, replace the int64 fail-loud guard + the `count`/`data` u32 lines + the u32 `fwrite`s with the type-aware write from spec §2 (writes `[id][type][count][raw bytes]`). Keep the borrow/return and the `if (!ok) break;`/fclose/abort exactly as is.

- [ ] **Step 4: Make load_catalog type-aware** — In `compute_mock.cpp`'s `load_catalog`, remove the hoisted `std::vector<uint32_t> data;` and replace the per-column read body with the type-dispatching read from spec §2 (read `[id][type][count]`, then by type into a `uint32_t`/`int64_t` buffer → `load_scan_column`/`load_scan_column_i64`; unknown type → `ok = false`). The header read + `for` loop + fclose/abort tail stay.

- [ ] **Step 5: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_typed_snapshot.cpp -o /tmp/tts && /tmp/tts` → PASS: `[mixed catalog roundtrip] ok`, `[empty catalog] ok`, `ALL TYPED-SNAPSHOT TESTS PASSED`. Zero warnings.

- [ ] **Step 6: Confirm no regression** — `save_catalog`/`load_catalog` changed (format + magic), so these MUST still pass unmodified:
  - `for t in test_catalog_snapshot test_column_io test_typed_columns test_typed_predicates test_typed_grouped test_query test_engine_restart test_transactions; do clang++ -std=c++20 -O2 $t.cpp -o /tmp/$t 2>/dev/null && /tmp/$t | tail -1; done`
  - `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`.
  (`test_catalog_snapshot` round-trips u32 columns through the new format; if it asserts raw file bytes/size it may need a look — report if so. `test_column_io`/`save_column` are unchanged.) If any differ, STOP / report BLOCKED.

- [ ] **Step 7: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add compute_mock.cpp test_typed_snapshot.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: typed catalog snapshot (DM-3d) — int64 columns survive restart via save_catalog/load_catalog"
```

---

### Task 2: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (26 tests).**
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by \
         test_query test_observability test_column_io test_catalog_snapshot test_query_validation \
         test_transactions test_server test_security test_audit test_csv_ingest test_checkpoint \
         test_query_predicates test_typed_columns test_typed_predicates test_typed_grouped test_typed_snapshot; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 26× `PASS:`. If any fail, STOP / report BLOCKED with `cat /tmp/out_<test>`.

- [ ] **Step 2: Notebook** — add `"test_typed_snapshot.cpp"` to `make_notebook.py` SOURCES right after `"test_typed_grouped.cpp"`; add a run cell after the grouped-int64 run cell (the one compiling `test_typed_grouped.cpp` to `/tmp/ttg`):
```python
    md("### Typed catalog snapshot (int64 durability)\n"
       "save_catalog / load_catalog round-trip a mixed uint32 + int64 catalog (types + values), so an "
       "int64 analytical store survives a restart — not just RAM-resident."),
    code("!clang++ -std=c++20 -O2 test_typed_snapshot.cpp -o /tmp/tts 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_typed_snapshot.cpp -o /tmp/tts; /tmp/tts"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 45 source files embedded`. Verify `grep -o "test_typed_snapshot.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed typed-catalog-snapshot test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** magic bump + save_catalog type-aware write (§2)→T1S3; load_catalog type dispatch (§2)→T1S4; mixed round-trip (negatives + >UINT32_MAX) + empty + types preserved (§3)→T1S1; regression of catalog/column/typed tests + oracle (§3)→T1S6; suite+notebook→T2. ✓
**Placeholders:** none — write/read bodies verbatim. **Type consistency:** record `[u64 id][u32 type][u64 count][count×width]`; `type = static_cast<uint32_t>(column_type(id))`, `count = column_rows(id)`; load dispatches `load_scan_column`(U32)/`load_scan_column_i64`(I64); unknown type → fail-loud. `save_column`/`column_io.hpp` unchanged (single-column still u32-guarded). Magic bumped so a v0 snapshot is rejected, not misparsed. Oracle path untouched.

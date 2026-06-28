# Named Columns + Catalog Introspection Implementation Plan (DM-2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Optional column names (`name_column`/`column_id`/`column_name`) + a `catalog_columns()` introspection (id, name, type, rows, tier). Additive; assembles existing accessors + a name↔id map.

**Spec:** `docs/superpowers/specs/2026-06-27-named-columns-catalog-design.md`

---

### Task 1: names + ColumnInfo + catalog_columns + test

**Files:** Modify `compute_mock.cpp`; Create `test_schema.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_schema.cpp`:

```cpp
// CPU test for named columns + catalog introspection (DM-2): name_column / column_id / column_name /
// catalog_columns over u32 + int64 + double columns.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <algorithm>
#include <string>
#include <vector>
#include <iostream>

static void test_naming() {
    std::vector<uint32_t> u = {1, 2, 3};
    std::vector<int64_t>  s = {-7, 5000000000LL, 3, -100};
    std::vector<double>   d = {1.5, -3.25};
    CPUMockEngine eng;
    eng.load_scan_column(3, u.data(), u.size());
    eng.load_scan_column_i64(7, s.data(), s.size());
    eng.load_scan_column_f64(9, d.data(), d.size());
    eng.name_column(3, "qty"); eng.name_column(7, "revenue"); eng.name_column(9, "rate");
    assert(eng.column_id("qty") == 3 && eng.column_id("revenue") == 7 && eng.column_id("rate") == 9);
    assert(eng.column_id("nonexistent") == 0 && "unknown name -> 0");
    assert(eng.column_name(7) == "revenue");
    // load a 4th unnamed column; its name is ""
    std::vector<uint32_t> u2 = {9, 9};
    eng.load_scan_column(4, u2.data(), u2.size());
    assert(eng.column_name(4).empty() && "unnamed column -> empty name");
    std::cout << "[naming] ok\n";
}

static void test_catalog_columns() {
    std::vector<uint32_t> u = {1, 2, 3, 4, 5};       // 5 rows
    std::vector<int64_t>  s = {-7, 5000000000LL, 3}; // 3 rows
    std::vector<double>   d = {1.5, -3.25};          // 2 rows
    CPUMockEngine eng;
    eng.load_scan_column(3, u.data(), u.size());     eng.name_column(3, "qty");
    eng.load_scan_column_i64(7, s.data(), s.size()); eng.name_column(7, "revenue");
    eng.load_scan_column_f64(9, d.data(), d.size()); // unnamed
    std::vector<ColumnInfo> info = eng.catalog_columns();
    assert(info.size() == 3);
    std::sort(info.begin(), info.end(), [](const ColumnInfo& a, const ColumnInfo& b){ return a.id < b.id; });
    assert(info[0].id == 3 && info[0].name == "qty"     && info[0].type == MatrixType::U32 && info[0].rows == 5);
    assert(info[1].id == 7 && info[1].name == "revenue" && info[1].type == MatrixType::I64 && info[1].rows == 3);
    assert(info[2].id == 9 && info[2].name == ""        && info[2].type == MatrixType::F64 && info[2].rows == 2);
    for (const auto& ci : info) assert(ci.tier == MemorySpace::HOST && "freshly loaded -> HOST");
    std::cout << "[catalog columns] ok\n";
}

static void test_resolve_then_query() {
    std::vector<int64_t> s = {-7, 5000000000LL, 3, -100};
    CPUMockEngine eng;
    eng.load_scan_column_i64(7, s.data(), s.size());
    eng.name_column(7, "revenue");
    MatrixQuery q{}; q.value_col = eng.column_id("revenue"); q.agg = AGG_SUM;   // resolve name -> id
    std::vector<uint64_t> o;
    assert(eng.execute_query(q, o) == MatrixQueryStatus::OK);
    assert(static_cast<int64_t>(o[0]) == -7 + 5000000000LL + 3 - 100 && "query by resolved name");
    std::cout << "[resolve then query] ok\n";
}

int main() { test_naming(); test_catalog_columns(); test_resolve_then_query();
    std::cout << "ALL SCHEMA TESTS PASSED\n"; return 0; }
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_schema.cpp -o /tmp/tsch && /tmp/tsch` → FAIL to compile (`name_column`/`column_id`/`column_name`/`ColumnInfo`/`catalog_columns` undeclared).

- [ ] **Step 3: Add ColumnInfo + the name maps + methods** — In `compute_mock.cpp`:
  - Add `struct ColumnInfo { uint64_t id; std::string name; MatrixType type; size_t rows; MemorySpace tier; };` near `struct EngineStats` (spec §2).
  - Add the two members `column_names_` / `name_to_id_` beside `col_types_` (spec §2).
  - Add `name_column`, `column_id`, `column_name`, `catalog_columns` (spec §2 verbatim) near the other inspection accessors (`column_type`/`column_tier`).

- [ ] **Step 4: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_schema.cpp -o /tmp/tsch && /tmp/tsch` → PASS: `[naming] ok`, `[catalog columns] ok`, `[resolve then query] ok`, `ALL SCHEMA TESTS PASSED`. Zero warnings.

- [ ] **Step 5: Confirm no regression** — additive, but `compute_mock.cpp` changed; these MUST still pass unmodified:
  - `for t in test_observability test_query test_typed_columns test_typed_double test_live_tiering test_catalog_snapshot test_backup; do clang++ -std=c++20 -O2 $t.cpp -o /tmp/$t 2>/dev/null && /tmp/$t | tail -1; done`
  - `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`.
  If any differ, STOP / report BLOCKED.

- [ ] **Step 6: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add compute_mock.cpp test_schema.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: named columns + catalog introspection (DM-2) — name_column/column_id/column_name + catalog_columns()"
```

---

### Task 2: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (32 tests).**
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by \
         test_query test_observability test_column_io test_catalog_snapshot test_query_validation \
         test_transactions test_server test_security test_audit test_csv_ingest test_checkpoint \
         test_query_predicates test_typed_columns test_typed_predicates test_typed_grouped \
         test_typed_snapshot test_typed_double test_typed_double_grouped test_typed_csv \
         test_query_latency test_backup test_schema; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 32× `PASS:`. If any fail, STOP / report BLOCKED.

- [ ] **Step 2: Notebook** — add `"test_schema.cpp"` to `make_notebook.py` SOURCES right after `"test_backup.cpp"`; add a run cell after the backup/restore run cell (`test_backup.cpp` → `/tmp/tbk`):
```python
    md("### Named columns + catalog introspection\n"
       "name_column / column_id / column_name attach names to columns, and catalog_columns() lists every "
       "column with its id, name, type, row count, and tier — a discoverable schema, not just numeric ids."),
    code("!clang++ -std=c++20 -O2 test_schema.cpp -o /tmp/tsch 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_schema.cpp -o /tmp/tsch; /tmp/tsch"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 51 source files embedded`. Verify `grep -o "test_schema.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed schema/catalog-introspection test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** ColumnInfo + name maps + name_column/column_id/column_name/catalog_columns (§2)→T1S3; naming resolve + catalog listing (id/name/type/rows/tier) + resolve-then-query + non-vacuity (§3)→T1S1; regression + oracle (§3)→T1S5; suite+notebook→T2. ✓
**Placeholders:** none — all methods verbatim. **Type consistency:** `name_column(id,name)`, `column_id(name)→uint64_t` (0 if absent), `column_name(id)→string` ("" if unnamed), `ColumnInfo{id,name,type,rows,tier}`, `catalog_columns()→vector<ColumnInfo>` assembled from `column_name`/`column_type`/`column_rows`/`tier()`. Additive; load/query/oracle untouched. `<string>`/`<vector>`/`<unordered_map>` already included.

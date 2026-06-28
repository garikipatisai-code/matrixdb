# Append / Dynamic Column Growth Implementation Plan (DM-9)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** `append_to_column[_i64/_f64]` grow an existing catalog column with new rows (borrow COLD→HOST, append, return, update the brain's byte accounting). Additive; load-once columns + queries unchanged.

**Spec:** `docs/superpowers/specs/2026-06-27-append-column-growth-design.md`

---

### Task 1: append_bytes + update_bytes + append_to_column[_i64/_f64] + test

**Files:** Modify `tiered_column.hpp`, `tier_manager.hpp`, `compute_mock.cpp`; Create `test_append.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_append.cpp`:

```cpp
// CPU test for append / dynamic column growth (DM-9): append_to_column[_i64/_f64] grow a column;
// rows become queryable; works across the COLD tier (borrow-append-return).
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <bit>
#include <vector>
#include <iostream>

static void test_append_u32() {
    std::vector<uint32_t> v(10); for (uint32_t i = 0; i < 10; ++i) v[i] = i;   // 0..9, sum 45
    CPUMockEngine eng;
    eng.load_scan_column(3, v.data(), v.size());
    MatrixQuery q{}; q.value_col = 3; q.agg = AGG_SUM; std::vector<uint64_t> o;
    eng.execute_query(q, o); assert(o[0] == 45);
    uint32_t more[] = {10, 11, 12};
    eng.append_to_column(3, more, 3);                       // 0..12, sum 78, 13 rows
    assert(eng.column_rows(3) == 13);
    eng.execute_query(q, o); assert(o[0] == 78 && "appended rows included in SUM");
    std::cout << "[append u32] ok\n";
}

static void test_append_typed() {
    CPUMockEngine eng;
    std::vector<int64_t> s = {-7, 5};               eng.load_scan_column_i64(7, s.data(), s.size());
    std::vector<double>  d = {1.5, -0.5};           eng.load_scan_column_f64(9, d.data(), d.size());
    int64_t mi[] = {5000000000LL, -100};            eng.append_to_column_i64(7, mi, 2);   // sum -7+5+5e9-100
    double  md[] = {2.25, -3.0};                    eng.append_to_column_f64(9, md, 2);   // sum 1.5-0.5+2.25-3.0
    assert(eng.column_rows(7) == 4 && eng.column_rows(9) == 4);
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_SUM; std::vector<uint64_t> o; eng.execute_query(q, o);
      assert(static_cast<int64_t>(o[0]) == -7 + 5 + 5000000000LL - 100); }
    { MatrixQuery q{}; q.value_col = 9; q.agg = AGG_SUM; std::vector<uint64_t> o; eng.execute_query(q, o);
      assert(std::bit_cast<double>(o[0]) == 1.5 - 0.5 + 2.25 - 3.0); }
    std::cout << "[append typed] ok\n";
}

static void test_append_cold() {
    // Small host budget: holds ~one 400KB column, not two -> the idle one demotes to COLD.
    CPUMockEngine eng(0, "", 600 * 1024);
    std::vector<uint32_t> a(100000, 1), b(100000, 1);       // 400KB each
    eng.load_scan_column(1, a.data(), a.size());
    eng.load_scan_column(2, b.data(), b.size());
    for (int i = 0; i < 8; ++i) {                           // scan col1 hard -> rebalance demotes idle col2
        MatrixQuery q{}; q.value_col = 1; q.agg = AGG_COUNT; std::vector<uint64_t> o; eng.execute_query(q, o);
    }
    assert(eng.column_tier(2) == MemorySpace::COLD && "idle column demoted to SSD");
    uint32_t more[] = {7, 7, 7};
    eng.append_to_column(2, more, 3);                       // append to the COLD column
    MatrixQuery q{}; q.value_col = 2; q.agg = AGG_COUNT; std::vector<uint64_t> o;
    assert(eng.execute_query(q, o) == MatrixQueryStatus::OK && o[0] == 100003 && "COLD column grew + queryable");
    std::cout << "[append cold] ok\n";
}

int main() { test_append_u32(); test_append_typed(); test_append_cold();
    std::cout << "ALL APPEND TESTS PASSED\n"; return 0; }
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_append.cpp -o /tmp/tap && /tmp/tap` → FAIL to compile (`append_to_column*` undeclared).

- [ ] **Step 3: Add `TieredColumn::append_bytes`** — In `tiered_column.hpp`, add the public `append_bytes` (spec §2 verbatim) after `migrate_to` / before `checksum` (any public spot).

- [ ] **Step 4: Add `TierManager::update_bytes`** — In `tier_manager.hpp`, add the public `update_bytes` (spec §2 verbatim) near `register_column` / `record_access`.

- [ ] **Step 5: Add the engine methods** — In `compute_mock.cpp`, add the private `append_raw` (spec §3) and the three public `append_to_column`/`_i64`/`_f64` wrappers (spec §3 verbatim) near `load_scan_column*`.

- [ ] **Step 6: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_append.cpp -o /tmp/tap && /tmp/tap` → PASS: `[append u32] ok`, `[append typed] ok`, `[append cold] ok`, `ALL APPEND TESTS PASSED`. Zero warnings.

- [ ] **Step 7: Confirm no regression** — `tiered_column.hpp`/`tier_manager.hpp`/`compute_mock.cpp` changed (additive); these MUST still pass unmodified:
  - `for t in test_migration test_tier_manager test_live_tiering test_typed_columns test_typed_double test_query test_observability test_backup test_schema; do clang++ -std=c++20 -O2 $t.cpp -o /tmp/$t 2>/dev/null && /tmp/$t | tail -1; done`
  - `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`.
  If any differ, STOP / report BLOCKED.

- [ ] **Step 8: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add tiered_column.hpp tier_manager.hpp compute_mock.cpp test_append.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: append / dynamic column growth (DM-9) — append_to_column[_i64/_f64] grow columns across tiers"
```

---

### Task 2: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (33 tests).**
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by \
         test_query test_observability test_column_io test_catalog_snapshot test_query_validation \
         test_transactions test_server test_security test_audit test_csv_ingest test_checkpoint \
         test_query_predicates test_typed_columns test_typed_predicates test_typed_grouped \
         test_typed_snapshot test_typed_double test_typed_double_grouped test_typed_csv \
         test_query_latency test_backup test_schema test_append; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 33× `PASS:`. If any fail, STOP / report BLOCKED.

- [ ] **Step 2: Notebook** — add `"test_append.cpp"` to `make_notebook.py` SOURCES right after `"test_schema.cpp"`; add a run cell after the schema run cell (`test_schema.cpp` → `/tmp/tsch`):
```python
    md("### Append / dynamic column growth\n"
       "append_to_column[_i64/_f64] add rows to an existing column (growing it, even across the COLD "
       "tier) — the store is no longer load-once; appended rows are immediately queryable."),
    code("!clang++ -std=c++20 -O2 test_append.cpp -o /tmp/tap 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_append.cpp -o /tmp/tap; /tmp/tap"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 52 source files embedded`. Verify `grep -o "test_append.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed append/column-growth test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** append_bytes (§2)→T1S3; update_bytes (§2)→T1S4; append_raw + 3 typed wrappers (§3)→T1S5; u32/typed append + COLD-tier append + accounting + non-vacuity (§4)→T1S1; regression of tier/migration/typed/query + oracle (§4)→T1S7; suite+notebook→T2. ✓
**Placeholders:** none — all bodies verbatim. **Type consistency:** `TieredColumn::append_bytes(const unsigned char*, size_t)` (fail-loud if not HOST), `TierManager::update_bytes(id, bytes)`, `append_raw(id, bytes, byte_count)` (borrow/append/return/update), `append_to_column`(u32)/`_i64`/`_f64` (assert catalog_has + matching column_type, `n*sizeof(elem)` bytes). COLD-append test mirrors `test_live_tiering`'s demote mechanics (small host_cap + hot col1 → col2 COLD). Append doesn't trigger rebalance (next scan does). Oracle/id-0 untouched.

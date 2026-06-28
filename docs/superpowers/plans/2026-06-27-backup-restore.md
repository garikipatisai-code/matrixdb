# Backup / Restore Implementation Plan (DU-6)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** `backup(prefix)` + `restore(prefix)` — one-call backup/restore of the whole durable state (analytical catalog + point-op store), composing the existing fail-loud writers/readers.

**Spec:** `docs/superpowers/specs/2026-06-27-backup-restore-design.md`

---

### Task 1: backup/restore + test

**Files:** Modify `compute_mock.cpp`; Create `test_backup.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_backup.cpp`:

```cpp
// CPU test for backup/restore (DU-6): backup() writes <prefix>.catalog + <prefix>.kv; restore() loads
// both into a fresh engine — analytical columns (u32 + int64) and point-op writes all survive.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <iostream>

static void test_backup_roundtrip() {
    const std::string pfx = "/tmp/mdb_backup_rt";
    std::remove((pfx + ".catalog").c_str()); std::remove((pfx + ".kv").c_str());
    std::vector<uint32_t> u(50); for (size_t i = 0; i < u.size(); ++i) u[i] = static_cast<uint32_t>(i);
    std::vector<int64_t>  s = {-7, 5000000000LL, 3, -100};
    {
        CPUMockEngine eng;                                   // no WAL — backup must still capture kv_
        eng.load_scan_column(3, u.data(), u.size());
        eng.load_scan_column_i64(7, s.data(), s.size());
        eng.begin(); eng.txn_put(11, 111); eng.txn_put(22, 222); eng.commit();
        eng.begin(); eng.txn_put(33, 333); eng.commit();
        eng.backup(pfx);
    }
    {
        CPUMockEngine eng;                                   // fresh engine
        eng.restore(pfx);
        // analytical columns restored (type + values)
        assert(eng.column_type(3) == MatrixType::U32 && eng.column_type(7) == MatrixType::I64);
        MatrixQuery qu{}; qu.value_col = 3; qu.agg = AGG_SUM; std::vector<uint64_t> ou;
        assert(eng.execute_query(qu, ou) == MatrixQueryStatus::OK && ou[0] == (49u * 50u / 2u)); // 0..49 = 1225
        MatrixQuery qs{}; qs.value_col = 7; qs.agg = AGG_SUM; std::vector<uint64_t> os;
        eng.execute_query(qs, os); assert(static_cast<int64_t>(os[0]) == -7 + 5000000000LL + 3 - 100);
        // point-op writes restored
        uint64_t v = 0;
        assert(eng.kv_get(11, v) && v == 111);
        assert(eng.kv_get(22, v) && v == 222);
        assert(eng.kv_get(33, v) && v == 333);
        assert(!eng.kv_get(99, v) && "absent key still absent");
    }
    std::remove((pfx + ".catalog").c_str()); std::remove((pfx + ".kv").c_str());
    std::cout << "[backup roundtrip] ok\n";
}

static void test_backup_empty() {
    const std::string pfx = "/tmp/mdb_backup_empty";
    std::remove((pfx + ".catalog").c_str()); std::remove((pfx + ".kv").c_str());
    { CPUMockEngine eng; eng.backup(pfx); }
    { CPUMockEngine eng; eng.restore(pfx); }   // empty catalog + empty kv -> no crash
    std::remove((pfx + ".catalog").c_str()); std::remove((pfx + ".kv").c_str());
    std::cout << "[backup empty] ok\n";
}

int main() { test_backup_roundtrip(); test_backup_empty();
    std::cout << "ALL BACKUP TESTS PASSED\n"; return 0; }
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_backup.cpp -o /tmp/tbk && /tmp/tbk` → FAIL to compile (`CPUMockEngine` has no `backup`/`restore`).

- [ ] **Step 3: Add backup/restore** — In `compute_mock.cpp`, add the `backup` and `restore` methods (spec §2 verbatim) immediately after `load_catalog` (~line 231, the public section near the other persistence methods).

- [ ] **Step 4: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_backup.cpp -o /tmp/tbk && /tmp/tbk` → PASS: `[backup roundtrip] ok`, `[backup empty] ok`, `ALL BACKUP TESTS PASSED`. Zero warnings.

- [ ] **Step 5: Confirm no regression** — additive (two new methods), but the underlying writers/readers are exercised, so:
  - `for t in test_catalog_snapshot test_checkpoint test_typed_snapshot test_transactions test_engine_restart test_query; do clang++ -std=c++20 -O2 $t.cpp -o /tmp/$t 2>/dev/null && /tmp/$t | tail -1; done`
  - `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`.
  If any differ, STOP / report BLOCKED.

- [ ] **Step 6: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add compute_mock.cpp test_backup.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: backup/restore (DU-6) — one-call backup(prefix)/restore(prefix) of catalog + point-op store"
```

---

### Task 2: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (31 tests).**
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by \
         test_query test_observability test_column_io test_catalog_snapshot test_query_validation \
         test_transactions test_server test_security test_audit test_csv_ingest test_checkpoint \
         test_query_predicates test_typed_columns test_typed_predicates test_typed_grouped \
         test_typed_snapshot test_typed_double test_typed_double_grouped test_typed_csv \
         test_query_latency test_backup; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 31× `PASS:`. If any fail, STOP / report BLOCKED.

- [ ] **Step 2: Notebook** — add `"test_backup.cpp"` to `make_notebook.py` SOURCES right after `"test_query_latency.cpp"`; add a run cell after the query-latency run cell (`test_query_latency.cpp` → `/tmp/tql`):
```python
    md("### Backup / restore\n"
       "backup(prefix) snapshots the whole durable state (analytical catalog + point-op store) under one "
       "path prefix; restore(prefix) brings it all back into a fresh engine — a basic ops capability."),
    code("!clang++ -std=c++20 -O2 test_backup.cpp -o /tmp/tbk 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_backup.cpp -o /tmp/tbk; /tmp/tbk"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 50 source files embedded`. Verify `grep -o "test_backup.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed backup/restore test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** backup/restore methods (§2)→T1S3; full round-trip (u32+int64+kv, no-WAL) + empty (§3)→T1S1; regression of the underlying writers + oracle (§3)→T1S5; suite+notebook→T2. ✓
**Placeholders:** none — both methods verbatim, composing existing `save_catalog`/`save_checkpoint`/`load_catalog`/`load_checkpoint`. **Type consistency:** `backup(prefix)`/`restore(prefix)` use `prefix+".catalog"` and `prefix+".kv"`; the test reads back via `execute_query` (typed) + `kv_get`. No format changes; oracle untouched.

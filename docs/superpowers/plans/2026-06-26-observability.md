# Engine Observability Implementation Plan — OB-1

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Instrument the engine with tiering metrics (cold borrows, rebalances, migrations) + resident-bytes gauges, exposed via `stats()` and surfaced in the live demo.

**Spec:** `docs/superpowers/specs/2026-06-26-observability-design.md`

---

### Task 1: EngineStats + counters + stats()

**Files:** Modify `compute_mock.cpp`; Create `test_observability.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_observability.cpp`:

```cpp
// CPU test for engine observability (EngineStats / stats()).
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

static void test_stats() {
    const size_t N = 1000;
    const size_t S = N * sizeof(uint32_t);
    std::vector<uint32_t> col(N);
    for (size_t i = 0; i < N; ++i) col[i] = static_cast<uint32_t>(i);
    CPUMockEngine eng(0, "", /*host_cap=*/2 * S);   // 2-column budget
    eng.load_scan_column(1, col.data(), N);
    eng.load_scan_column(2, col.data(), N);
    eng.load_scan_column(3, col.data(), N);
    {
        EngineStats s = eng.stats();
        assert(s.catalog_columns == 3);
        assert(s.cold_resident_bytes == 0);
        assert(s.rebalances == 0 && s.migrations == 0 && s.cold_borrows == 0);
    }
    // Scan cols 1 & 2 (never 3) -> rebalances demote col 3 to SSD.
    const uint32_t T = 250;
    for (int r = 0; r < 8; ++r)
        for (uint64_t id : {1ull, 2ull}) {
            DatabaseQuery q{}; matrix_set_scan_target(q, T, id); eng.execute_scan(q);
        }
    {
        EngineStats s = eng.stats();
        assert(s.rebalances == 16 / 4);                 // 16 tiered scans / REBALANCE_EVERY(4)
        assert(s.migrations >= 1);                       // col 3 demoted (non-vacuous)
        assert(s.cold_resident_bytes == S);              // one column on SSD
        assert(s.host_resident_bytes == 2 * S);          // two columns in RAM
        assert(s.cold_borrows == 0);                     // cols 1,2 stayed HOST -> no borrow
    }
    // Scan the now-COLD col 3 -> exactly one cold borrow.
    { DatabaseQuery q{}; matrix_set_scan_target(q, T, 3); eng.execute_scan(q); }
    assert(eng.stats().cold_borrows == 1);
    std::cout << "[engine stats] ok\n";
}

int main() {
    test_stats();
    std::cout << "ALL OBSERVABILITY TESTS PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_observability.cpp -o /tmp/tobs && /tmp/tobs` → FAIL to compile (`EngineStats` / `stats()` undeclared).

- [ ] **Step 3: Add the EngineStats struct** — In `compute_mock.cpp`, add immediately BEFORE the `class CPUMockEngine` definition (after the includes / the file's doc comment):

```cpp
// Engine observability snapshot: tiering activity counters + current resident-bytes gauges.
struct EngineStats {
    uint64_t cold_borrows;        // COLD->HOST borrows performed for a scan/aggregate
    uint64_t rebalances;          // rebalance() passes run (scan-driven)
    uint64_t migrations;          // migration decisions actually executed
    size_t   catalog_columns;     // # columns in the tiered catalog
    size_t   host_resident_bytes; // catalog bytes currently in RAM (TierManager's view)
    size_t   cold_resident_bytes; // catalog bytes currently on SSD
};
```

- [ ] **Step 4: Add the counters + stats()** — In `compute_mock.cpp`, add the counter members next to `scans_since_rebalance_` (the live-tiering members block):

```cpp
    uint64_t cold_borrows_ = 0;    // observability: COLD->HOST borrows
    uint64_t rebalances_ = 0;      // observability: rebalance passes
    uint64_t migrations_ = 0;      // observability: migration decisions executed
```

Add the public accessor next to the other inspection accessors (e.g. after `column_checksum`):

```cpp
    // Observability snapshot (counters since construction + current resident-bytes gauges).
    EngineStats stats() const {
        return EngineStats{ cold_borrows_, rebalances_, migrations_, catalog_.size(),
                            tier_mgr_.resident_bytes(MemorySpace::HOST),
                            tier_mgr_.resident_bytes(MemorySpace::COLD) };
    }
```

- [ ] **Step 5: Increment the counters at the existing sites** — three edits in `compute_mock.cpp`:

(a) `scan_tiered_column` borrow (the `if (home != MemorySpace::HOST) col.migrate_to(MemorySpace::HOST);` line):
```cpp
        if (home != MemorySpace::HOST) { ++cold_borrows_; col.migrate_to(MemorySpace::HOST); } // pull SSD->RAM to scan
```

(b) `scan_tiered_column` rebalance trigger — replace `executor_.apply(tier_mgr_.rebalance(), ptrs);` with:
```cpp
            migrations_ += executor_.apply(tier_mgr_.rebalance(), ptrs);
            ++rebalances_;
```

(c) Both borrow lines in `grouped_aggregate` AND both in `grouped_aggregate_where` (4 lines total of the form `const MemorySpace Xh = Xc.tier(); if (Xh != MemorySpace::HOST) Xc.migrate_to(MemorySpace::HOST);`) — add `++cold_borrows_;` inside each branch:
```cpp
        const MemorySpace kh = kc.tier(); if (kh != MemorySpace::HOST) { ++cold_borrows_; kc.migrate_to(MemorySpace::HOST); }
        const MemorySpace vh = vc.tier(); if (vh != MemorySpace::HOST) { ++cold_borrows_; vc.migrate_to(MemorySpace::HOST); }
```
(apply to both methods — `grouped_aggregate` and `grouped_aggregate_where`).

- [ ] **Step 6: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_observability.cpp -o /tmp/tobs && /tmp/tobs` → PASS, prints `[engine stats] ok` + `ALL OBSERVABILITY TESTS PASSED`. No warnings.

- [ ] **Step 7: Confirm no regression** — `clang++ -std=c++20 -O2 test_live_tiering.cpp -o /tmp/tlt && /tmp/tlt | tail -1` → `ALL LIVE-TIERING TESTS PASSED`; `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`.

- [ ] **Step 8: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add compute_mock.cpp test_observability.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: engine observability — EngineStats + stats() (cold borrows / rebalances / migrations / resident bytes)"
```

---

### Task 2: Surface stats in the demo + regression + notebook

**Files:** Modify `main.cpp`; Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Print stats in the demo** — In `main.cpp`'s `analytical_query_demo()`, immediately AFTER the final `... "Demo OK." << std::endl;` line, add:

```cpp
    const EngineStats st = demo.stats();
    std::cout << "engine stats: cold_borrows=" << st.cold_borrows
              << " rebalances=" << st.rebalances << " migrations=" << st.migrations
              << " catalog_cols=" << st.catalog_columns
              << " | resident HOST=" << st.host_resident_bytes / (1024 * 1024) << "MB"
              << " COLD=" << st.cold_resident_bytes / (1024 * 1024) << "MB" << std::endl;
```

- [ ] **Step 2: Build & run main; verify** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep -E "Scan result sum|engine stats|Demo OK"` → contains `83886070 (oracle 83886070)`, `Demo OK.`, and an `engine stats: cold_borrows=… rebalances=… migrations=… …` line with migrations>=1 and a nonzero COLD resident (col 3 demoted). Binary exits 0.

- [ ] **Step 3: Full CPU suite (12 tests now)** —
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by test_query test_observability; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 12× `PASS:`. If any fail, STOP / report BLOCKED.

- [ ] **Step 4: Notebook** — add `"test_observability.cpp"` to `make_notebook.py` SOURCES (after `"test_query.cpp"`); add a run cell after the query-API run cell:
```python
    md("### Engine observability\n"
       "`stats()` exposes tiering activity (cold borrows, rebalances, migrations) + resident-bytes "
       "gauges — verified against a known eviction workload."),
    code("!clang++ -std=c++20 -O2 test_observability.cpp -o /tmp/tobs 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_observability.cpp -o /tmp/tobs; /tmp/tobs"),
```
Then `python3 make_notebook.py` → `wrote matrixdb_colab.ipynb: <N> cells, 28 source files embedded`. Verify: `grep -o "test_observability.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 5: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add main.cpp make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: print engine stats in the live demo; embed observability test in notebook"
```

---

## Self-Review
**Spec coverage:** EngineStats (§1)→T1S3; counters+stats (§2)→T1S4/S5; verification (§3)→T1 test; demo surfacing→T2S1; suite+notebook→T2. ✓
**Placeholders:** none. **Type consistency:** `EngineStats{cold_borrows,rebalances,migrations,catalog_columns,host_resident_bytes,cold_resident_bytes}`, `stats() const`, counters `cold_borrows_/rebalances_/migrations_` consistent T1/T2. Counter increments are additive on existing sites — oracle/point-op paths untouched.

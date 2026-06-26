# Live Tiering Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the dormant TierManager + MigrationExecutor + TieredColumn into the live CPU engine so analytical scan columns auto-tier between RAM (HOST) and SSD (COLD) by access heat, letting the engine hold a working set larger than RAM.

**Architecture:** OP_SCAN gains an optional column id (payload offset 8). `id==0` keeps the existing fixed-column path (oracle unchanged); `id>0` targets a catalog of `TieredColumn`s the engine drives through a `TierManager` + `MigrationExecutor`. Cold columns are borrowed to HOST for a scan then returned, so the brain's byte accounting stays honest. DEVICE is inert on the CPU build (`device_cap=1` blocks every promotion).

**Tech Stack:** C++20, header-only units, clang++/g++. Tests are standalone CPU executables that `#include "compute_mock.cpp"`.

**Spec:** `docs/superpowers/specs/2026-06-26-live-tiering-integration-design.md`

---

## File Structure

- **Modify `compute.hpp`** — add the `matrix_set_scan_target` / `matrix_get_scan_column_id` codec; make `matrix_set_scan_threshold` delegate.
- **Modify `tiered_column.hpp`** — add `host_ptr()` (zero-copy read of the resident HOST bytes for scanning).
- **Modify `compute_mock.cpp`** — add the TierManager/executor/catalog members, `load_scan_column`, the tiered scan path in `execute_scan`, and test-inspection accessors.
- **Create `test_live_tiering.cpp`** — one test binary that grows across tasks: codec → host_ptr → single-column tiered scan → eviction headline.
- **Modify `make_notebook.py`** — embed the new test + add a run cell; regenerate the notebook.

All point-op / batching / WAL code is untouched. The legacy fixed `scan_column_` and its `id==0` path are unchanged.

---

### Task 1: Scan-target codec (column id in the payload)

**Files:**
- Modify: `compute.hpp:86-94`
- Create: `test_live_tiering.cpp`

- [ ] **Step 1: Write the failing test**

Create `test_live_tiering.cpp`:

```cpp
// CPU test for the live tiering integration. Grows across tasks; one main() runs all.
#include "compute_mock.cpp"   // CPUMockEngine + compute.hpp (codec) + tiering headers
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

static void test_codec() {
    DatabaseQuery q{};
    matrix_set_scan_target(q, 42u, 7ull);
    assert(q.opcode == OP_SCAN);
    assert(matrix_get_scan_threshold(q) == 42u);
    assert(matrix_get_scan_column_id(q) == 7ull);

    DatabaseQuery q2{};
    matrix_set_scan_threshold(q2, 99u);              // legacy delegates to target(...,0)
    assert(matrix_get_scan_threshold(q2) == 99u);
    assert(matrix_get_scan_column_id(q2) == 0ull);   // legacy id is explicitly 0
    std::cout << "[codec] ok\n";
}

int main() {
    test_codec();
    std::cout << "ALL LIVE-TIERING TESTS PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `clang++ -std=c++20 -O2 test_live_tiering.cpp -o /tmp/tlt && /tmp/tlt`
Expected: FAIL to compile — `matrix_set_scan_target` / `matrix_get_scan_column_id` not declared.

- [ ] **Step 3: Implement the codec**

In `compute.hpp`, replace the current threshold codec (lines 86-94):

```cpp
// OP_SCAN carries its filter threshold in the query payload, and may target a specific
// catalog column: threshold at payload[0] (u32), column id at payload[8] (u64). Column
// id 0 == the legacy fixed scan column. payload begins 8-byte aligned (DatabaseQuery
// starts with u64 fields, payload is at offset 32), so the u64 at payload+8 is aligned.
// One codec used by both engines so they decode identically.
inline void matrix_set_scan_target(DatabaseQuery& q, uint32_t threshold, uint64_t column_id) {
    q.opcode = OP_SCAN;
    *reinterpret_cast<uint32_t*>(q.payload) = threshold;
    *reinterpret_cast<uint64_t*>(q.payload + 8) = column_id;
}
inline uint64_t matrix_get_scan_column_id(const DatabaseQuery& q) {
    return *reinterpret_cast<const uint64_t*>(q.payload + 8);
}
inline void matrix_set_scan_threshold(DatabaseQuery& q, uint32_t threshold) {
    matrix_set_scan_target(q, threshold, 0); // legacy: target the fixed column (id 0)
}
inline uint32_t matrix_get_scan_threshold(const DatabaseQuery& q) {
    return *reinterpret_cast<const uint32_t*>(q.payload);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `clang++ -std=c++20 -O2 test_live_tiering.cpp -o /tmp/tlt && /tmp/tlt`
Expected: PASS — prints `[codec] ok` then `ALL LIVE-TIERING TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
git add compute.hpp test_live_tiering.cpp
git commit -m "feat: OP_SCAN column-id codec; legacy threshold delegates to target(...,0)"
```

---

### Task 2: TieredColumn::host_ptr() — zero-copy read of resident bytes

**Files:**
- Modify: `tiered_column.hpp:29-31` (public accessors block)
- Modify: `test_live_tiering.cpp`

- [ ] **Step 1: Write the failing test**

Add to `test_live_tiering.cpp` (above `main`), and add `#include "tiered_column.hpp"` after the existing includes:

```cpp
static void test_host_ptr() {
    std::vector<uint32_t> data(4);
    for (uint32_t i = 0; i < 4; ++i) data[i] = i * 10;          // 0,10,20,30
    TieredColumn col(101, reinterpret_cast<const unsigned char*>(data.data()),
                     data.size() * sizeof(uint32_t));
    assert(col.tier() == MemorySpace::HOST);
    const uint32_t* v = reinterpret_cast<const uint32_t*>(col.host_ptr());
    assert(v != nullptr && v[0] == 0 && v[1] == 10 && v[2] == 20 && v[3] == 30);

    const uint64_t cks = col.checksum();
    col.migrate_to(MemorySpace::COLD);
    assert(col.host_ptr() == nullptr);                          // bytes on SSD now
    col.migrate_to(MemorySpace::HOST);
    const uint32_t* v2 = reinterpret_cast<const uint32_t*>(col.host_ptr());
    assert(v2 != nullptr && v2[0] == 0 && v2[3] == 30);
    assert(col.checksum() == cks);                              // round-trip integrity
    std::cout << "[host_ptr] ok\n";
}
```

Add `test_host_ptr();` as the second line of `main()` (after `test_codec();`).

- [ ] **Step 2: Run test to verify it fails**

Run: `clang++ -std=c++20 -O2 test_live_tiering.cpp -o /tmp/tlt && /tmp/tlt`
Expected: FAIL to compile — `TieredColumn` has no member `host_ptr`.

- [ ] **Step 3: Implement host_ptr()**

In `tiered_column.hpp`, add to the public accessors (right after `uint64_t id() const { return id_; }` on line 31):

```cpp
    // Pointer to the resident HOST bytes for in-place reads (e.g. a scan). Valid only while
    // tier()==HOST; nullptr otherwise (the bytes live on SSD/VRAM — migrate to HOST first).
    // Zero-copy: returns the live buffer, no allocation.
    const unsigned char* host_ptr() const {
        return tier_ == MemorySpace::HOST ? host_.data() : nullptr;
    }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `clang++ -std=c++20 -O2 test_live_tiering.cpp -o /tmp/tlt && /tmp/tlt`
Expected: PASS — prints `[host_ptr] ok`.

- [ ] **Step 5: Commit**

```bash
git add tiered_column.hpp test_live_tiering.cpp
git commit -m "feat: TieredColumn::host_ptr() for zero-copy scan of resident HOST bytes"
```

---

### Task 3: Engine tiered scan path (catalog + load + dispatch + borrow-and-return)

**Files:**
- Modify: `compute_mock.cpp` (includes, ctor, new methods, `execute_scan`, members)
- Modify: `test_live_tiering.cpp`

- [ ] **Step 1: Write the failing test**

Add to `test_live_tiering.cpp` (above `main`):

```cpp
static void test_tiered_single_column() {
    const size_t N = 1000;
    std::vector<uint32_t> col(N);
    for (size_t i = 0; i < N; ++i) col[i] = static_cast<uint32_t>(i); // value[i]=i
    CPUMockEngine eng(0, "", /*host_cap=*/SIZE_MAX);                   // generous: no eviction
    eng.load_scan_column(1, col.data(), N);

    DatabaseQuery q{};
    const uint32_t T = 500;
    matrix_set_scan_target(q, T, 1);
    eng.execute_scan(q);
    assert(q.transaction_id == N - 1 - T);                            // count of i>T == 499
    assert(eng.column_tier(1) == MemorySpace::HOST);                  // fits budget, stays resident

    // legacy id==0 path still scans the fixed column correctly
    DatabaseQuery ql{};
    const uint32_t TL = 1000;
    matrix_set_scan_threshold(ql, TL);
    eng.execute_scan(ql);
    assert(ql.transaction_id == MATRIX_SCAN_COLUMN_SIZE - 1 - TL);
    std::cout << "[tiered single-column + legacy] ok\n";
}
```

Add `test_tiered_single_column();` to `main()` after `test_host_ptr();`.

- [ ] **Step 2: Run test to verify it fails**

Run: `clang++ -std=c++20 -O2 test_live_tiering.cpp -o /tmp/tlt && /tmp/tlt`
Expected: FAIL to compile — `CPUMockEngine` has no `load_scan_column` / `column_tier`, and the ctor takes no third arg.

- [ ] **Step 3: Add the tiering includes and members**

In `compute_mock.cpp`, add after the existing `#include "cold_store.hpp"` (line 3):

```cpp
#include "migration_executor.hpp"  // MigrationExecutor + TierManager + TieredColumn + CostModel
#include "memory_model.hpp"        // MemorySpace, MemoryModel
#include <unordered_map>
```

In the private members block (after `std::unique_ptr<ColdStore> cold_store_;` on line 166), add:

```cpp
    // --- live tiering (INT-1): a catalog of analytical columns the brain auto-tiers ---
    static constexpr uint64_t REBALANCE_EVERY = 4;     // rebalance every N tiered scans
    TierManager tier_mgr_;                              // decides migrations (heat-driven)
    MigrationExecutor executor_;                        // moves the bytes per decision
    std::unordered_map<uint64_t, std::unique_ptr<TieredColumn>> catalog_; // id -> column
    uint64_t scans_since_rebalance_ = 0;
```

- [ ] **Step 4: Extend the constructor**

Replace the constructor signature + init list (lines 23-25) with:

```cpp
    // host_cap is the RAM budget (bytes) for the tiered catalog; default unbounded so the
    // existing pipeline (empty catalog) is unaffected. device_cap=1 makes the DEVICE tier
    // inert on the CPU build: scan_us() ignores gpu_available, so the brain would otherwise
    // emit HOST->DEVICE promotions the CPU executor's migrate_to(DEVICE) aborts on; a 1-byte
    // cap means no real column ever fits, so no DEVICE decision is emitted (cap==0 == unbounded).
    explicit CPUMockEngine(size_t /*worker_count*/ = 0, std::string wal_path = "",
                           size_t host_cap = SIZE_MAX)
        : tier_mgr_(CostModel(MemoryModel::detect(false), false), /*device_cap=*/1, host_cap)
        , binned_(MATRIX_BATCH_MAX)
        , scan_column_(MATRIX_SCAN_COLUMN_SIZE) {
```

(The init list must list `tier_mgr_` first to match declaration order — `tier_mgr_` is declared before `binned_`. Leave the rest of the constructor body unchanged.)

- [ ] **Step 5: Add load_scan_column + inspection accessors**

In `compute_mock.cpp`, add as public methods (after the constructor, before `~CPUMockEngine`):

```cpp
    // Register a uint32 analytical column into the tiered catalog (born resident in HOST).
    // id must be > 0 (0 is reserved for the legacy fixed scan column).
    void load_scan_column(uint64_t id, const uint32_t* data, size_t n) {
        assert(id != 0 && "column id 0 is reserved for the legacy fixed scan column");
        const size_t bytes = n * sizeof(uint32_t);
        catalog_[id] = std::make_unique<TieredColumn>(
            id, reinterpret_cast<const unsigned char*>(data), bytes);
        tier_mgr_.register_column(id, bytes, MemorySpace::HOST);
    }

    // Inspection (tests): where the bytes actually live vs where the brain believes, the
    // HOST bytes the brain is accounting for, and a column's integrity checksum.
    MemorySpace column_tier(uint64_t id) const { return catalog_.at(id)->tier(); }
    MemorySpace manager_tier(uint64_t id) const { return tier_mgr_.tier_of(id); }
    size_t host_resident_bytes() const { return tier_mgr_.resident_bytes(MemorySpace::HOST); }
    uint64_t column_checksum(uint64_t id) const { return catalog_.at(id)->checksum(); }
```

- [ ] **Step 6: Add the tiered scan helper**

In `compute_mock.cpp`, add as a private method (in the private section, after the members or before them — anywhere in `private:`):

```cpp
    // Scan one catalog column for value>threshold. A cold column is borrowed to HOST for the
    // scan then returned to its home tier, so the engine's residency always matches the brain's
    // accounting (no side-channel migration the budget can't see). Every REBALANCE_EVERY scans,
    // run the brain + executor: promote hot columns (DEVICE inert here), demote the coldest
    // HOST columns to SSD under the budget.
    uint64_t scan_tiered_column(uint64_t col_id, uint32_t threshold) {
        auto it = catalog_.find(col_id);
        assert(it != catalog_.end() && "scan of unregistered column id");
        TieredColumn& col = *it->second;
        tier_mgr_.record_access(col_id, col.size_bytes());          // heat signal

        const MemorySpace home = col.tier();
        if (home != MemorySpace::HOST) col.migrate_to(MemorySpace::HOST); // pull SSD->RAM to scan
        const uint32_t* vals = reinterpret_cast<const uint32_t*>(col.host_ptr());
        const size_t nvals = col.size_bytes() / sizeof(uint32_t);
        uint64_t c = 0;
        for (size_t i = 0; i < nvals; ++i) c += (vals[i] > threshold);
        // ponytail: returning the borrow rewrites the COLD file each cold scan; skip-if-unchanged
        // (or a TierManager note_residency) is the upgrade path if cold-scan churn ever matters.
        if (home != MemorySpace::HOST) col.migrate_to(home);        // return the borrow

        if (++scans_since_rebalance_ >= REBALANCE_EVERY) {
            std::unordered_map<uint64_t, TieredColumn*> ptrs;
            for (auto& kv : catalog_) ptrs[kv.first] = kv.second.get();
            executor_.apply(tier_mgr_.rebalance(), ptrs);
            scans_since_rebalance_ = 0;
        }
        return c;
    }
```

- [ ] **Step 7: Dispatch in execute_scan**

Replace the body of `execute_scan` (lines 98-111) with:

```cpp
    void execute_scan(DatabaseQuery& q) override {
        // id==0 -> the legacy fixed resident column (unchanged); id>0 -> a tiered catalog column.
        const uint32_t threshold = matrix_get_scan_threshold(q);
        const uint64_t col_id = matrix_get_scan_column_id(q);
        const auto st0 = std::chrono::steady_clock::now();
        uint64_t c = 0;
        if (col_id == 0) {
            for (size_t s2 = 0; s2 < MATRIX_SCAN_COLUMN_SIZE; ++s2)
                c += (scan_column_[s2] > threshold);
        } else {
            c = scan_tiered_column(col_id, threshold);
        }
        scan_time_s_ += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - st0).count();
        q.transaction_id = c;
        ++scans_;
        scan_result_sum_ += c;
    }
```

- [ ] **Step 8: Run test to verify it passes**

Run: `clang++ -std=c++20 -O2 test_live_tiering.cpp -o /tmp/tlt && /tmp/tlt`
Expected: PASS — prints `[tiered single-column + legacy] ok`. (Watch for `-Wreorder`: if it warns, the init list order doesn't match declaration order — fix the init-list order, don't suppress.)

- [ ] **Step 9: Commit**

```bash
git add compute_mock.cpp test_live_tiering.cpp
git commit -m "feat: live tiered scan path — catalog, load_scan_column, borrow-and-return, rebalance trigger"
```

---

### Task 4: Eviction headline — holds more than RAM, cold demoted, pull-back correct

**Files:**
- Modify: `test_live_tiering.cpp`

This task is a test against Task 3's code. It is the non-vacuous proof of the whole increment: it FAILS if eviction, borrow-and-return, or the device-cap guard is wrong (a no-op implementation cannot make `column_tier(3)==COLD` while results stay correct).

- [ ] **Step 1: Write the test**

Add to `test_live_tiering.cpp` (above `main`):

```cpp
static void test_eviction_holds_more_than_ram() {
    const size_t N = 1000;
    const size_t S = N * sizeof(uint32_t);            // 4000 bytes per column
    std::vector<uint32_t> col(N);
    for (size_t i = 0; i < N; ++i) col[i] = static_cast<uint32_t>(i);

    CPUMockEngine eng(0, "", /*host_cap=*/2 * S);      // room for 2 of 3 columns
    eng.load_scan_column(1, col.data(), N);
    eng.load_scan_column(2, col.data(), N);
    eng.load_scan_column(3, col.data(), N);            // 3*S > 2*S: more than fits in RAM

    // Hot/cold: scan cols 1 & 2 each round, NEVER col 3 -> col 3 heat stays 0 (the victim).
    const uint32_t T = 250;
    for (int round = 0; round < 8; ++round) {
        for (uint64_t id : {1ull, 2ull}) {
            DatabaseQuery q{};
            matrix_set_scan_target(q, T, id);
            eng.execute_scan(q);
            assert(q.transaction_id == N - 1 - T);     // hot scans stay correct (749)
        }
    }
    // 16 tiered scans -> 4 rebalances (REBALANCE_EVERY=4). MIN_RESIDENCY_TICKS=2 means the
    // 1st rebalance evicts nothing; col 3 demotes on the 2nd. Final state is deterministic:
    assert(eng.manager_tier(3) == MemorySpace::COLD);  // brain demoted the cold column
    assert(eng.column_tier(3)  == MemorySpace::COLD);  // and the bytes are actually on SSD
    assert(eng.manager_tier(1) == MemorySpace::HOST);  // hot columns retained
    assert(eng.manager_tier(2) == MemorySpace::HOST);
    assert(eng.host_resident_bytes() <= 2 * S);        // budget respected
    static_assert(3 * (1000 * sizeof(uint32_t)) > 2 * (1000 * sizeof(uint32_t)),
                  "catalog holds more bytes than the RAM budget");

    // Scan the COLD column: borrowed to HOST, scanned, returned to COLD; result still correct.
    const uint64_t cks_before = eng.column_checksum(3);
    DatabaseQuery q3{};
    matrix_set_scan_target(q3, T, 3);
    eng.execute_scan(q3);
    assert(q3.transaction_id == N - 1 - T);            // pull-back-correct (749)
    assert(eng.column_tier(3) == MemorySpace::COLD);   // borrow returned: rest tier == brain
    assert(eng.column_checksum(3) == cks_before);      // demote+borrow preserved the bytes
    std::cout << "[eviction holds-more-than-RAM + borrow] ok\n";
}
```

Add `test_eviction_holds_more_than_ram();` to `main()` after `test_tiered_single_column();`.

- [ ] **Step 2: Run test to verify it passes**

Run: `clang++ -std=c++20 -O2 test_live_tiering.cpp -o /tmp/tlt && /tmp/tlt`
Expected: PASS — prints `[eviction holds-more-than-RAM + borrow] ok` then `ALL LIVE-TIERING TESTS PASSED`.

- [ ] **Step 3: Prove the test is non-vacuous (temporary sanity check)**

Temporarily change `REBALANCE_EVERY` in `compute_mock.cpp` from `4` to a huge value (e.g. `100000`) so no rebalance ever fires, rebuild and run.
Expected: FAIL — `manager_tier(3) == COLD` assertion trips (no rebalance → col 3 never demoted). This confirms the test genuinely exercises eviction. **Revert `REBALANCE_EVERY` back to `4`** and rerun to confirm PASS before committing.

- [ ] **Step 4: Commit**

```bash
git add test_live_tiering.cpp
git commit -m "test: live tiering headline — engine holds >RAM, demotes cold to SSD, scans pull back correct"
```

---

### Task 5: Regression (oracle + all tests) and notebook regeneration

**Files:**
- Modify: `make_notebook.py:6-13` (SOURCES), and the test-cells section (~line 81)
- Regenerate: `matrixdb_colab.ipynb`

- [ ] **Step 1: Verify the pipeline oracle is unchanged**

Run: `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/matrixdb_proto && /tmp/matrixdb_proto 2>&1 | grep -E "Scan result sum|reads="`
Expected: contains `Scan result sum: 83886070 (oracle 83886070)` and the `reads=4990 writes=5000 commits=5000 scans=10` line — the legacy path is byte-for-byte unchanged.

- [ ] **Step 2: Run the full CPU test suite**

Run:
```bash
for t in test_kv_store test_cost_model test_tier_manager test_cold_store \
         test_engine_restart test_migration test_scan_coverage test_live_tiering; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" && "/tmp/$t" | tail -1 || echo "FAIL: $t"
done
```
Expected: every test prints its pass line; no `FAIL:` lines.

- [ ] **Step 3: Add the new test to make_notebook.py SOURCES**

In `make_notebook.py`, add `"test_live_tiering.cpp"` to the `SOURCES` list (lines 6-13), e.g. on the line with the other tests:

```python
           "test_migration.cpp", "test_migration_gpu.cpp", "test_live_tiering.cpp"]
```

- [ ] **Step 4: Add a run cell for the new test**

In `make_notebook.py`, immediately after the `test_migration` run cell (the `code(... test_migration.cpp ... /tmp/tmig)` block near line 81-82), add a matching markdown + code cell pair, following the existing pattern:

```python
    md("### Live tiering integration\n"
       "The engine holds a catalog of analytical columns larger than its RAM budget; the "
       "TierManager demotes the coldest to SSD and a scan pulls a cold column back, results intact."),
    code("!clang++ -std=c++20 -O2 test_live_tiering.cpp -o /tmp/tlt 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_live_tiering.cpp -o /tmp/tlt; /tmp/tlt"),
```

- [ ] **Step 5: Regenerate the notebook**

Run: `python3 make_notebook.py`
Expected: prints `wrote matrixdb_colab.ipynb: <N> cells, 24 source files embedded` (24 = 23 + the new test).

- [ ] **Step 6: Verify the notebook embeds the new test**

Run: `grep -c "test_live_tiering.cpp" matrixdb_colab.ipynb`
Expected: `>= 2` (the `%%writefile` cell + the run cell).

- [ ] **Step 7: Commit**

```bash
git add make_notebook.py matrixdb_colab.ipynb
git commit -m "chore: embed live-tiering test in Colab notebook; verify oracle + suite unchanged"
```

---

## Self-Review

**1. Spec coverage:**
- §1 catalog + TierManager/executor/scan counter → Task 3 members. ✓
- §1 `load_scan_column` → Task 3. ✓
- §1 codec + `matrix_set_scan_threshold` delegation → Task 1. ✓
- §1/§4 tiered `execute_scan` with record_access / borrow-and-return / periodic rebalance → Task 3 (`scan_tiered_column` + dispatch). ✓
- §4 `device_cap=1` DEVICE-inert lever → Task 3 ctor. ✓
- §4 `host_ptr()` (implied: scanning needs to read the bytes) → Task 2. ✓
- §5 headline (holds >RAM, cold→COLD in brain+bytes, pull-back correct, checksum invariant, budget respected) → Task 4. ✓
- §5 legacy path untouched + oracle 83886070 + suite green + notebook → Tasks 3 (legacy assert) & 5. ✓

**2. Placeholder scan:** none — every code step carries complete code; every command has expected output.

**3. Type consistency:** `matrix_set_scan_target(q, threshold, column_id)`, `matrix_get_scan_column_id(q)`, `host_ptr()`, `load_scan_column(id, data, n)`, `column_tier/manager_tier/host_resident_bytes/column_checksum`, `scan_tiered_column(col_id, threshold)`, member `tier_mgr_/executor_/catalog_/scans_since_rebalance_/REBALANCE_EVERY` — names identical across Tasks 1–5. Constructor third param `host_cap` used consistently (`SIZE_MAX` default in Task 3; `2*S` in Task 4). `MemorySpace::HOST/COLD` from memory_model.hpp used throughout.

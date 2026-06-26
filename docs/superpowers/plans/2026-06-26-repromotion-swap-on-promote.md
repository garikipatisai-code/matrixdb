# Heat-Driven Re-Promotion (swap-on-promote) Implementation Plan — INT-1b

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the TierManager re-promote a re-hot lower-tier column into a full faster tier by displacing a colder resident (swap-on-promote), so hot data climbs back to RAM instead of being re-read from SSD every scan. Plus two hardening fixes INT-1's review flagged.

**Architecture:** In `TierManager::rebalance()`'s promotion loop, when a candidate doesn't fit its target tier, evict the lowest-`keep_score` resident (single victim) to make room — gated by a `SWAP_MARGIN` value-hysteresis and `MIN_RESIDENCY_TICKS`. The engine already drives `rebalance()` + executor each `REBALANCE_EVERY` scans (INT-1), so the swap decisions flow to real byte movement with no engine change.

**Tech Stack:** C++20, header-only units, clang++/g++. Tests are standalone CPU executables.

**Spec:** `docs/superpowers/specs/2026-06-26-repromotion-swap-on-promote-design.md`

---

## File Structure

- **Modify `tier_manager.hpp`** — add `SWAP_MARGIN`; add private `try_swap_promote(...)`; integrate it into the promotion loop (the `continue` on no-fit becomes a swap attempt).
- **Modify `test_tier_manager.cpp`** — add a positive-swap block and a margin-control block.
- **Modify `test_live_tiering.cpp`** — add `test_repromotion_under_pressure()` (the headline) and `test_cold_path_uniqueness()`.
- **Modify `tiered_column.hpp`** — make the COLD file path per-process/instance unique.
- **Modify `compute_mock.cpp`** — release-safe unregistered-id guard in `scan_tiered_column`.
- **Modify `make_notebook.py` / regenerate `matrixdb_colab.ipynb`** — keep embedded copies current (no new source file).

The engine's `rebalance()` call site, the executor, and the legacy scan path are untouched.

---

### Task 1: Swap-on-promote in TierManager

**Files:**
- Modify: `tier_manager.hpp` (tunables ~line 36; promotion loop ~lines 70-80; add a private helper)
- Modify: `test_tier_manager.cpp`

- [ ] **Step 1: Write the failing tests**

In `test_tier_manager.cpp`, add these two blocks immediately BEFORE the `// --- Task 5: determinism` block:

```cpp
    // --- Swap-on-promote: a re-hot lower-tier column displaces a COLDER resident of a full tier ---
    {
        const size_t COL = 64u*1024*1024;
        // HOST holds exactly 2 columns; DEVICE inert (cap 1 => nothing fits, like the CPU engine).
        TierManager tm(make_cm(), /*device_cap*/ 1, /*host_cap*/ 2*COL);
        tm.register_column(1, COL, MemorySpace::HOST); // kept hot -> stays
        tm.register_column(2, COL, MemorySpace::HOST); // goes idle -> the victim
        tm.register_column(3, COL, MemorySpace::COLD); // re-hot candidate -> must swap in
        for (int r = 0; r < 4; ++r) {
            for (int i = 0; i < 50; ++i) { tm.record_access(1, COL); tm.record_access(3, COL); }
            // col 2 idle: heat decays to ~0 -> lowest keep_score -> the displaced victim
            tm.rebalance();
        }
        assert(tm.tier_of(3) == MemorySpace::HOST && "hot COLD column swapped into the full HOST tier");
        assert(tm.tier_of(2) == MemorySpace::COLD && "idle HOST resident displaced to COLD");
        assert(tm.tier_of(1) == MemorySpace::HOST && "hot resident kept (cost-benefit, not LRU)");
        assert(tm.resident_bytes(MemorySpace::HOST) <= 2*COL && "HOST within budget after the swap");
    }
    // --- Swap margin (anti-thrash): an equally-hot candidate canNOT displace a resident ---
    {
        const size_t COL = 64u*1024*1024;
        TierManager tm(make_cm(), /*device_cap*/ 1, /*host_cap*/ 2*COL);
        tm.register_column(1, COL, MemorySpace::HOST);
        tm.register_column(2, COL, MemorySpace::HOST);
        tm.register_column(3, COL, MemorySpace::COLD);
        for (int r = 0; r < 4; ++r) {
            // all three equally hot -> value(col3) == value(col2) -> not > 1.5x -> no swap
            for (int i = 0; i < 50; ++i) { tm.record_access(1, COL); tm.record_access(2, COL); tm.record_access(3, COL); }
            tm.rebalance();
        }
        assert(tm.tier_of(3) == MemorySpace::COLD && "equally-hot candidate cannot clear the SWAP_MARGIN");
        assert(tm.resident_bytes(MemorySpace::HOST) <= 2*COL && "HOST within budget");
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_tier_manager.cpp -o /tmp/ttm && /tmp/ttm`
Expected: FAIL — the first new block's `assert(tm.tier_of(3) == MemorySpace::HOST ...)` trips (today promotion only fills free space; with HOST full, col 3 is never promoted → stays COLD).

- [ ] **Step 3: Add the SWAP_MARGIN tunable**

In `tier_manager.hpp`, in the tunables block, add after the `MIN_RESIDENCY_TICKS` line (line 36):

```cpp
    static constexpr double SWAP_MARGIN = 1.5;         // swap-on-promote: candidate must be > 1.5x
                                                       // the victim's keep-value (anti-thrash band)
```

- [ ] **Step 4: Add the try_swap_promote helper**

In `tier_manager.hpp`, add this private method right AFTER the `keep_score` method (after its closing brace, ~line 195):

```cpp
    // Swap-on-promote: `cand` wants to move up into tier `to` but `to` is full. If the lowest-
    // keep_score resident of `to` (a) is past MIN_RESIDENCY_TICKS and (b) is decisively colder
    // than cand (promote_value(cand) > SWAP_MARGIN * keep_score(victim)), and evicting that one
    // victim makes room, demote the victim one tier down and promote cand. Emits both decisions,
    // updates tiers/arrival ticks, returns true. Single victim by design (see spec §2).
    bool try_swap_promote(Column& cand, MemorySpace to, size_t cap,
                          std::vector<MigrationDecision>& decisions) {
        if (cap == 0) return false; // unbounded tier never needs to evict to fit
        Column* victim = nullptr;
        double worst = 1e301;
        for (auto& kv : cols_) {
            Column& v = kv.second;
            if (v.tier != to) continue;
            if (tick_ - v.arrived_tick < MIN_RESIDENCY_TICKS) continue; // don't evict a fresh arrival
            const double s = keep_score(v);
            if (s < worst) { worst = s; victim = &v; }
        }
        if (!victim) return false;                                   // nothing evictable
        if (promote_eval(cand).benefit <= SWAP_MARGIN * keep_score(*victim)) return false; // not worth it
        if (resident_bytes(to) - victim->bytes + cand.bytes > cap) return false; // one eviction won't fit cand
        const MemorySpace v_to = slower_tier(to);
        decisions.push_back(MigrationDecision{victim->id, victim->tier, v_to});
        victim->tier = v_to;
        victim->arrived_tick = tick_;
        decisions.push_back(MigrationDecision{cand.id, cand.tier, to});
        cand.tier = to;
        cand.arrived_tick = tick_;
        return true;
    }
```

- [ ] **Step 5: Integrate the swap into the promotion loop**

In `tier_manager.hpp`, replace the promotion `for` loop (currently lines ~70-80):

```cpp
        for (uint64_t id : candidates) {
            Column& c = cols_.at(id);
            const MemorySpace to = faster_tier(c.tier);
            const size_t cap = capacity_of(to);
            // cap == 0 means unbounded; otherwise the column must fit after current residents.
            if (cap != 0 && resident_bytes(to) + c.bytes > cap) continue; // doesn't fit: skip
            const MemorySpace from = c.tier;
            decisions.push_back(MigrationDecision{c.id, from, to});
            c.tier = to;
            c.arrived_tick = tick_;
        }
```

with:

```cpp
        for (uint64_t id : candidates) {
            Column& c = cols_.at(id);
            const MemorySpace to = faster_tier(c.tier);
            const size_t cap = capacity_of(to);
            const bool fits = (cap == 0) || (resident_bytes(to) + c.bytes <= cap);
            if (!fits) {
                // `to` is full. Swap-on-promote: displace a colder resident if cand is worth it.
                // (Free-space promotion is the common path above; this is the contended path.)
                try_swap_promote(c, to, cap, decisions); // does nothing if no worthwhile victim
                continue;
            }
            const MemorySpace from = c.tier;
            decisions.push_back(MigrationDecision{c.id, from, to});
            c.tier = to;
            c.arrived_tick = tick_;
        }
```

- [ ] **Step 6: Run the TierManager tests**

Run: `clang++ -std=c++20 -O2 -Wall -Wextra test_tier_manager.cpp -o /tmp/ttm && /tmp/ttm`
Expected: PASS — `PASS: tier manager decisions correct`. No new warnings. (All pre-existing blocks, incl. the capacity-gated and anti-thrash ones, still pass — the swap only triggers on the new no-fit-but-worthwhile path.)

- [ ] **Step 7: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add tier_manager.hpp test_tier_manager.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: swap-on-promote in TierManager — re-hot column displaces a colder resident (margin-gated)"
```

---

### Task 2: Live re-promotion headline test

**Files:**
- Modify: `test_live_tiering.cpp`

This proves the engine re-promotes a re-hot column to *resident* RAM (not just borrows it). It is the non-vacuous proof of the increment. No engine code change — Task 1's TierManager swap flows through the executor the engine already calls.

- [ ] **Step 1: Write the test**

In `test_live_tiering.cpp`, add above `main`:

```cpp
static void test_repromotion_under_pressure() {
    const size_t N = 1000;
    const size_t S = N * sizeof(uint32_t);
    std::vector<uint32_t> col(N);
    for (size_t i = 0; i < N; ++i) col[i] = static_cast<uint32_t>(i);

    CPUMockEngine eng(0, "", /*host_cap=*/2 * S);   // room for 2 of 3 columns
    eng.load_scan_column(1, col.data(), N);
    eng.load_scan_column(2, col.data(), N);
    eng.load_scan_column(3, col.data(), N);

    const uint32_t T = 250;
    auto scan = [&](uint64_t id) {
        DatabaseQuery q{};
        matrix_set_scan_target(q, T, id);
        eng.execute_scan(q);
        assert(q.transaction_id == N - 1 - T);       // 749, regardless of tier
    };

    // Phase 1: cols 1 & 2 hot, col 3 never -> col 3 demoted to COLD (the INT-1 baseline).
    for (int r = 0; r < 8; ++r) { scan(1); scan(2); }
    assert(eng.column_tier(3) == MemorySpace::COLD && "phase 1: col 3 demoted to SSD");

    // Phase 2: FLIP the heat — cols 1 & 3 hot, col 2 NEVER. Col 2 cools; col 3 re-heats and must
    // be RE-PROMOTED to resident HOST (swap-on-promote displaces the now-cold col 2).
    for (int r = 0; r < 16; ++r) { scan(1); scan(3); }
    assert(eng.column_tier(3)  == MemorySpace::HOST && "col 3 re-promoted to RESIDENT RAM (not just borrowed)");
    assert(eng.manager_tier(3) == MemorySpace::HOST && "brain agrees col 3 is resident");
    assert(eng.column_tier(2)  == MemorySpace::COLD && "col 2 displaced to SSD");
    assert(eng.host_resident_bytes() <= 2 * S       && "HOST within budget");
    std::cout << "[re-promotion under pressure] ok\n";
}
```

Add `test_repromotion_under_pressure();` to `main()` after `test_eviction_holds_more_than_ram();`.

- [ ] **Step 2: Run to verify it passes**

Run: `clang++ -std=c++20 -O2 test_live_tiering.cpp -o /tmp/tlt && /tmp/tlt`
Expected: PASS — prints `[re-promotion under pressure] ok` then `ALL LIVE-TIERING TESTS PASSED`.

- [ ] **Step 3: Prove non-vacuity (temporary)**

This test must fail if swap-on-promote is removed. Temporarily edit `tier_manager.hpp`: change `static constexpr double SWAP_MARGIN = 1.5;` to `= 1e9;` (so no candidate can ever clear the margin — swap disabled). Rebuild and run:
`clang++ -std=c++20 -O2 test_live_tiering.cpp -o /tmp/tlt_vac && /tmp/tlt_vac; echo exit=$?`
Expected: ABORTS on `assert(eng.column_tier(3) == MemorySpace::HOST ...)` (col 3 stays COLD without swap), nonzero exit. Paste the output. Then **revert `SWAP_MARGIN` to `1.5`**, rebuild, confirm PASS, and confirm `git diff tier_manager.hpp` is EMPTY.

- [ ] **Step 4: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add test_live_tiering.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "test: live re-promotion — re-hot column climbs back to resident RAM under pressure"
```

---

### Task 3: Hardening — COLD-path uniqueness + unregistered-id guard

**Files:**
- Modify: `tiered_column.hpp` (includes; add a serial member; rewrite `cold_path()`)
- Modify: `compute_mock.cpp` (`scan_tiered_column` guard)
- Modify: `test_live_tiering.cpp` (uniqueness test)

- [ ] **Step 1: Write the failing test**

In `test_live_tiering.cpp`, add above `main`:

```cpp
static void test_cold_path_uniqueness() {
    // Two columns with the SAME id must NOT share a COLD file (else one's demote silently
    // clobbers the other's bytes). Distinct values; round-trip both through COLD independently.
    std::vector<uint32_t> a(4), b(4);
    for (uint32_t i = 0; i < 4; ++i) { a[i] = i; b[i] = 100 + i; }
    TieredColumn ca(7, reinterpret_cast<const unsigned char*>(a.data()), a.size() * sizeof(uint32_t));
    TieredColumn cb(7, reinterpret_cast<const unsigned char*>(b.data()), b.size() * sizeof(uint32_t)); // same id 7
    const uint64_t cka = ca.checksum(), ckb = cb.checksum();
    assert(cka != ckb);
    ca.migrate_to(MemorySpace::COLD);
    cb.migrate_to(MemorySpace::COLD);             // shared path would overwrite ca's file here
    ca.migrate_to(MemorySpace::HOST);
    cb.migrate_to(MemorySpace::HOST);
    assert(ca.checksum() == cka && "column A intact — not clobbered by same-id column B");
    assert(cb.checksum() == ckb && "column B intact");
    std::cout << "[cold-path uniqueness] ok\n";
}
```

Add `test_cold_path_uniqueness();` to `main()` after `test_host_ptr();`.

- [ ] **Step 2: Run to verify it fails**

Run: `clang++ -std=c++20 -O2 test_live_tiering.cpp -o /tmp/tlt && /tmp/tlt`
Expected: FAIL — `assert(ca.checksum() == cka ...)` trips: today both columns use `/tmp/matrixdb_tcol_7.bin`, so `cb`'s COLD write clobbers `ca`'s file and `ca` reads back `cb`'s bytes.

- [ ] **Step 3: Make the COLD path unique (tiered_column.hpp)**

In `tiered_column.hpp`, add to the includes (after `#include <vector>`, ~line 9):

```cpp
#include <atomic>
#include <unistd.h>   // getpid — per-process COLD-file namespace
```

Add a private member + serial source. Place the member with the other private members (after `MemorySpace tier_;`, ~line 132) and the helper in the private section:

```cpp
    // Process-unique serial so two columns (even with the same logical id, even in two engine
    // instances) never share a COLD file. Assigned once at construction, stable for the object's
    // life (so HOST<->COLD round-trips hit the same path).
    static uint64_t next_serial() {
        static std::atomic<uint64_t> counter{0};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }
    const uint64_t serial_ = next_serial();
```

Replace `cold_path()` (currently lines ~75-77):

```cpp
    std::string cold_path() const {
        return std::string("/tmp/matrixdb_tcol_") + std::to_string(id_) + ".bin";
    }
```

with:

```cpp
    std::string cold_path() const {
        // pid + per-object serial: unique within and across engine instances/processes.
        return std::string("/tmp/matrixdb_tcol_")
             + std::to_string(static_cast<long long>(getpid())) + "_"
             + std::to_string(serial_) + ".bin";
    }
```

- [ ] **Step 4: Add the release-safe unregistered-id guard (compute_mock.cpp)**

In `compute_mock.cpp`, in `scan_tiered_column`, replace:

```cpp
        auto it = catalog_.find(col_id);
        assert(it != catalog_.end() && "scan of unregistered column id");
        TieredColumn& col = *it->second;
```

with:

```cpp
        auto it = catalog_.find(col_id);
        if (it == catalog_.end()) {
            assert(false && "scan of unregistered column id"); // debug: catch the caller bug
            return 0;                                          // release: defined empty result, no null-deref
        }
        TieredColumn& col = *it->second;
```

- [ ] **Step 5: Run to verify it passes**

Run: `clang++ -std=c++20 -O2 -Wall -Wextra test_live_tiering.cpp -o /tmp/tlt && /tmp/tlt`
Expected: PASS — prints `[cold-path uniqueness] ok` among the others, then `ALL LIVE-TIERING TESTS PASSED`. No new warnings.

- [ ] **Step 6: Confirm the migration test still passes (tiered_column.hpp is shared)**

Run: `clang++ -std=c++20 -O2 test_migration.cpp -o /tmp/tmig && /tmp/tmig | tail -1`
Expected: `PASS: migration correct` (HOST↔COLD round-trips still work with the new path — the serial is stable per object).

- [ ] **Step 7: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add tiered_column.hpp compute_mock.cpp test_live_tiering.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "harden: per-process/instance unique COLD path; release-safe unregistered-id scan guard"
```

---

### Task 4: Regression + notebook regeneration

**Files:**
- Regenerate: `matrixdb_colab.ipynb` (via `make_notebook.py`)

- [ ] **Step 1: Verify the pipeline oracle is unchanged**

Run: `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/matrixdb_proto && /tmp/matrixdb_proto 2>&1 | grep -E "Scan result sum"`
Expected: `Scan result sum: 83886070 (oracle 83886070)`. If different, STOP and report BLOCKED.

- [ ] **Step 2: Run the full CPU test suite**

Run:
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store \
         test_engine_restart test_migration test_scan_coverage test_live_tiering; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t ($(tail -1 /tmp/out_$t))" || echo "FAIL: $t"
done
```
Expected: every line `PASS:`. If any `FAIL:`, STOP and report BLOCKED with `cat /tmp/out_<test>`.

- [ ] **Step 3: Regenerate the notebook (no SOURCES change — files only grew)**

The source file list is unchanged (test_live_tiering.cpp is already embedded from INT-1); regenerate so the embedded copies pick up the new test code.
Run: `python3 make_notebook.py`
Expected: prints `wrote matrixdb_colab.ipynb: <N> cells, 24 source files embedded`.

- [ ] **Step 4: Confirm the regenerated notebook is committed-clean and embeds the new tests**

Run: `grep -c "test_repromotion_under_pressure" matrixdb_colab.ipynb`
Expected: `>= 1` (the new test is embedded in the test_live_tiering.cpp writefile cell).

- [ ] **Step 5: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: regenerate notebook with re-promotion + hardening tests; oracle + suite verified"
```

---

## Self-Review

**1. Spec coverage:**
- §1/§2 swap-on-promote (SWAP_MARGIN, try_swap_promote, promotion-loop integration) → Task 1. ✓
- §4 test_tier_manager positive + margin control → Task 1 Step 1. ✓
- §4 test_live_tiering re-promotion headline + non-vacuity → Task 2. ✓
- §1/§3 COLD-path uniqueness → Task 3 Steps 3,5; uniqueness test Step 1. ✓
- §1/§3 unregistered-id guard → Task 3 Step 4. ✓
- §4 oracle + suite + notebook → Task 4. ✓

**2. Placeholder scan:** none — every code step has complete code; every command has expected output; the non-vacuity step has concrete edit + revert.

**3. Type consistency:** `try_swap_promote(Column&, MemorySpace, size_t, std::vector<MigrationDecision>&)` uses existing `Column`, `keep_score`, `promote_eval(...).benefit`, `slower_tier`, `resident_bytes`, `capacity_of`, `MIN_RESIDENCY_TICKS` — all defined in tier_manager.hpp. `SWAP_MARGIN` defined in Task 1 Step 3, used in Step 4. `next_serial()`/`serial_`/`cold_path()` consistent in Task 3. `column_tier`/`manager_tier`/`host_resident_bytes` (from INT-1) used in Task 2. `matrix_set_scan_target` (from INT-1) used in Task 2.

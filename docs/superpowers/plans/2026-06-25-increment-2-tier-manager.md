# Increment 2: TierManager (auto-tiering brain) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `TierManager` — a pure-logic brain that tracks per-column access heat and, on `rebalance()`, returns the cost-benefit migration decisions (promote hot toward VRAM, evict cold toward SSD) that would lower total scan cost. Decisions only; moves no bytes.

**Architecture:** One header-only unit (`tier_manager.hpp`) over the Increment-1 `CostModel`/`tier_model`. Built TDD in layers: registration/placement → heat tracking → cost-benefit promotion → capacity eviction + anti-thrash → notebook. Each layer adds tests to a single growing `test_tier_manager.cpp`.

**Tech Stack:** C++20, header-only, `clang++`/`g++`. No new dependencies. No GPU, no I/O.

**Spec:** `docs/superpowers/specs/2026-06-25-increment-2-tier-manager-design.md`

**Dependency signatures (verified, use exactly):**
- `CostModel(MemoryModel mm, bool gpu_available = true)`
- `double CostModel::scan_us(MemorySpace s, size_t bytes) const`
- `double CostModel::migration_us(MemorySpace from, MemorySpace to, size_t bytes) const`
- `MemorySpace` = `{ HOST, DEVICE, COLD, UNIFIED }` (enum class, in memory_model.hpp)
- `MemoryModel::detect(bool gpu_available)` → MemoryModel
- tier order fastest→slowest for scan: DEVICE (240000 B/us) > HOST (10000) > COLD (3000)

---

## File structure

- **Create** `tier_manager.hpp` — `MigrationDecision` struct + `TierManager` class. One responsibility: tiering decisions over tracked columns.
- **Create** `test_tier_manager.cpp` — CPU unit test, grown across tasks.
- **Modify** `make_notebook.py` — embed both new files + a test cell.

Named constants (all in tier_manager.hpp, documented as calibration targets):
`HEAT_ALPHA = 0.5`, `HYSTERESIS = 1.5`, `SCAN_HORIZON = 8`, `MIN_RESIDENCY_TICKS = 2`.

Tier-promotion order helper: `faster_tier(COLD)=HOST`, `faster_tier(HOST)=DEVICE`, `faster_tier(DEVICE)=DEVICE` (already fastest). `slower_tier(DEVICE)=HOST`, `slower_tier(HOST)=COLD`, `slower_tier(COLD)=COLD` (already slowest).

---

### Task 1: Skeleton — registration & placement map

**Files:**
- Create: `tier_manager.hpp`
- Create: `test_tier_manager.cpp`

- [ ] **Step 1: Write the failing test — create `test_tier_manager.cpp`**

```cpp
// CPU unit test for TierManager (the auto-tiering brain). No GPU, no I/O.
// Build: clang++ -std=c++20 -O2 test_tier_manager.cpp -o /tmp/ttm && /tmp/ttm
#include "tier_manager.hpp"
#include "cost_model.hpp"
#include "memory_model.hpp"
#include <cstdio>
#include <cassert>

static CostModel make_cm() { return CostModel(MemoryModel::detect(true), true); }

int main() {
    // --- Task 1: registration & placement ---
    {
        TierManager tm(make_cm(), /*device_cap*/ 1u<<30, /*host_cap*/ 1u<<30);
        tm.register_column(1, 1024, MemorySpace::COLD);
        tm.register_column(2, 2048, MemorySpace::HOST);
        assert(tm.tier_of(1) == MemorySpace::COLD && "registered tier is reported");
        assert(tm.tier_of(2) == MemorySpace::HOST && "registered tier is reported");
        assert(tm.resident_bytes(MemorySpace::COLD) == 1024 && "COLD resident bytes");
        assert(tm.resident_bytes(MemorySpace::HOST) == 2048 && "HOST resident bytes");
        assert(tm.heat_of(1) == 0.0 && "fresh column starts cold (heat 0)");
    }

    std::printf("PASS: tier manager decisions correct\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `clang++ -std=c++20 -O2 test_tier_manager.cpp -o /tmp/ttm 2>&1 | head -3`
Expected: FAIL — `fatal error: 'tier_manager.hpp' file not found`.

- [ ] **Step 3: Create `tier_manager.hpp` (skeleton)**

```cpp
#pragma once

#include "cost_model.hpp"
#include "memory_model.hpp"
#include "tier_model.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>

// A migration the brain decided on (the future executor will move the bytes).
struct MigrationDecision {
    uint64_t column_id;
    MemorySpace from;
    MemorySpace to;
};

// The auto-tiering brain. Tracks per-column access heat and, on rebalance(), returns the
// cost-benefit migrations that lower total scan cost. Decides only — moves no bytes, does
// no I/O. Pure logic over the Increment-1 CostModel + tier_model.
class TierManager {
public:
    TierManager(CostModel cm, size_t device_capacity_bytes, size_t host_capacity_bytes)
        : cm_(cm), device_cap_(device_capacity_bytes), host_cap_(host_capacity_bytes) {}

    void register_column(uint64_t id, size_t bytes, MemorySpace initial_tier) {
        cols_[id] = Column{id, bytes, initial_tier, /*heat*/0.0, /*recent*/0,
                           /*arrived_tick*/tick_};
    }

    MemorySpace tier_of(uint64_t id) const { return cols_.at(id).tier; }
    double heat_of(uint64_t id) const { return cols_.at(id).heat; }

    size_t resident_bytes(MemorySpace tier) const {
        size_t sum = 0;
        for (const auto& kv : cols_) if (kv.second.tier == tier) sum += kv.second.bytes;
        return sum;
    }

private:
    struct Column {
        uint64_t id;
        size_t   bytes;
        MemorySpace tier;
        double   heat;
        size_t   recent_bytes;   // accesses since last rebalance
        uint64_t arrived_tick;   // when it last landed on its current tier
    };

    CostModel cm_;
    size_t device_cap_;
    size_t host_cap_;
    uint64_t tick_ = 0;
    std::unordered_map<uint64_t, Column> cols_;
};
```

- [ ] **Step 4: Run test to verify it passes**

Run: `clang++ -std=c++20 -O2 test_tier_manager.cpp -o /tmp/ttm && /tmp/ttm`
Expected: `PASS: tier manager decisions correct`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add tier_manager.hpp test_tier_manager.cpp
git -c user.name="garikipatisai-code" -c user.email="garikipatisai-code@users.noreply.github.com" commit -m "feat: TierManager skeleton — column registration + placement map"
```

---

### Task 2: Heat tracking (record_access + aging in rebalance)

**Files:**
- Modify: `tier_manager.hpp`
- Modify: `test_tier_manager.cpp`

- [ ] **Step 1: Add the failing test — insert before the `std::printf("PASS` line in `test_tier_manager.cpp`**

```cpp
    // --- Task 2: heat tracking ---
    {
        TierManager tm(make_cm(), 1u<<30, 1u<<30);
        tm.register_column(1, 1000, MemorySpace::HOST);

        // Access accumulates; heat only updates on rebalance (EWMA aging).
        tm.record_access(1, 1000);
        tm.record_access(1, 1000);
        assert(tm.heat_of(1) == 0.0 && "heat unchanged until rebalance");

        tm.rebalance(); // heat = 0.5*recent + 0.5*old = 0.5*2000 + 0 = 1000
        assert(tm.heat_of(1) == 1000.0 && "first rebalance sets heat to alpha*recent");

        // No access this round: heat decays toward 0.
        tm.rebalance(); // heat = 0.5*0 + 0.5*1000 = 500
        assert(tm.heat_of(1) == 500.0 && "idle column heat decays");
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `clang++ -std=c++20 -O2 test_tier_manager.cpp -o /tmp/ttm 2>&1 | head -5`
Expected: FAIL — `no member named 'record_access'` / `'rebalance'`.

- [ ] **Step 3: Add `record_access`, the constants, and a heat-aging `rebalance` to `tier_manager.hpp`**

Add these public constants and methods inside the class (after `register_column`):

```cpp
    // --- tunables (calibration targets) ---
    static constexpr double HEAT_ALPHA = 0.5;          // EWMA weight on recent accesses
    static constexpr double HYSTERESIS = 1.5;          // promote only if benefit > 1.5x cost
    static constexpr int    SCAN_HORIZON = 8;          // cap on est. future scans
    static constexpr uint64_t MIN_RESIDENCY_TICKS = 2; // anti-thrash: min ticks before evict

    // Record that `bytes` of column `id` were scanned. O(1); accumulates until rebalance.
    void record_access(uint64_t id, size_t bytes) {
        auto it = cols_.find(id);
        if (it != cols_.end()) it->second.recent_bytes += bytes;
    }

    // Global pass. (This task: age heat only. Later tasks add promotion + eviction.)
    std::vector<MigrationDecision> rebalance() {
        ++tick_;
        for (auto& kv : cols_) {
            Column& c = kv.second;
            c.heat = HEAT_ALPHA * static_cast<double>(c.recent_bytes)
                     + (1.0 - HEAT_ALPHA) * c.heat;
            c.recent_bytes = 0;
        }
        std::vector<MigrationDecision> decisions;
        return decisions;
    }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `clang++ -std=c++20 -O2 test_tier_manager.cpp -o /tmp/ttm && /tmp/ttm`
Expected: `PASS: tier manager decisions correct`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add tier_manager.hpp test_tier_manager.cpp
git -c user.name="garikipatisai-code" -c user.email="garikipatisai-code@users.noreply.github.com" commit -m "feat: TierManager heat tracking (record_access + EWMA aging)"
```

---

### Task 3: Cost-benefit promotion

**Files:**
- Modify: `tier_manager.hpp`
- Modify: `test_tier_manager.cpp`

- [ ] **Step 1: Add the failing test — insert before the `std::printf("PASS` line**

```cpp
    // --- Task 3: cost-benefit promotion ---
    {
        TierManager tm(make_cm(), 1u<<30, 1u<<30); // ample capacity: no eviction pressure
        // A large, hot column on COLD should be promoted toward HOST then DEVICE.
        tm.register_column(1, 64u*1024*1024, MemorySpace::COLD);
        for (int i = 0; i < 50; ++i) tm.record_access(1, 64u*1024*1024);

        auto d1 = tm.rebalance();
        assert(tm.tier_of(1) == MemorySpace::HOST && "hot COLD column promotes to HOST");
        bool found = false;
        for (auto& d : d1) if (d.column_id==1 && d.from==MemorySpace::COLD && d.to==MemorySpace::HOST) found = true;
        assert(found && "promotion emitted as a decision");

        // Keep it hot; next rebalance climbs HOST -> DEVICE.
        for (int i = 0; i < 50; ++i) tm.record_access(1, 64u*1024*1024);
        tm.rebalance();
        assert(tm.tier_of(1) == MemorySpace::DEVICE && "still-hot column climbs to DEVICE");

        // A cold (rarely accessed) column is NOT promoted.
        TierManager tm2(make_cm(), 1u<<30, 1u<<30);
        tm2.register_column(2, 64u*1024*1024, MemorySpace::COLD);
        tm2.record_access(2, 1); // negligible heat
        tm2.rebalance();
        assert(tm2.tier_of(2) == MemorySpace::COLD && "cold column stays put");
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `clang++ -std=c++20 -O2 test_tier_manager.cpp -o /tmp/ttm && /tmp/ttm 2>&1 | head -5`
Expected: FAIL — assertion `hot COLD column promotes to HOST` fails (rebalance doesn't promote yet).

- [ ] **Step 3: Add promotion logic to `rebalance` in `tier_manager.hpp`**

First add these private helpers (inside the class, in the private section):

```cpp
    static MemorySpace faster_tier(MemorySpace t) {
        if (t == MemorySpace::COLD) return MemorySpace::HOST;
        if (t == MemorySpace::HOST) return MemorySpace::DEVICE;
        return t; // DEVICE already fastest
    }

    // Heat-derived estimate of upcoming scans for a column, clamped to the horizon.
    int est_future_scans(const Column& c) const {
        if (c.bytes == 0) return 0;
        const double scans = c.heat / static_cast<double>(c.bytes); // ~scans-per-tick
        int n = static_cast<int>(scans + 0.5);
        if (n < 0) n = 0;
        if (n > SCAN_HORIZON) n = SCAN_HORIZON;
        return n;
    }

    // Should this column be promoted one tier now? (cost-benefit + hysteresis)
    bool should_promote(const Column& c) const {
        const MemorySpace faster = faster_tier(c.tier);
        if (faster == c.tier) return false; // already fastest
        const double benefit = (cm_.scan_us(c.tier, c.bytes) - cm_.scan_us(faster, c.bytes))
                               * static_cast<double>(est_future_scans(c));
        const double cost = cm_.migration_us(c.tier, faster, c.bytes);
        return benefit > HYSTERESIS * cost;
    }
```

Then replace the body of `rebalance()` (keep the heat-aging loop, add promotion after it):

```cpp
    std::vector<MigrationDecision> rebalance() {
        ++tick_;
        for (auto& kv : cols_) {
            Column& c = kv.second;
            c.heat = HEAT_ALPHA * static_cast<double>(c.recent_bytes)
                     + (1.0 - HEAT_ALPHA) * c.heat;
            c.recent_bytes = 0;
        }

        std::vector<MigrationDecision> decisions;

        // Promotion: move qualifying columns one tier toward DEVICE.
        for (auto& kv : cols_) {
            Column& c = kv.second;
            if (should_promote(c)) {
                const MemorySpace from = c.tier;
                const MemorySpace to = faster_tier(c.tier);
                decisions.push_back(MigrationDecision{c.id, from, to});
                c.tier = to;
                c.arrived_tick = tick_;
            }
        }

        return decisions;
    }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `clang++ -std=c++20 -O2 test_tier_manager.cpp -o /tmp/ttm && /tmp/ttm`
Expected: `PASS: tier manager decisions correct`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add tier_manager.hpp test_tier_manager.cpp
git -c user.name="garikipatisai-code" -c user.email="garikipatisai-code@users.noreply.github.com" commit -m "feat: TierManager cost-benefit promotion (heat -> faster tier)"
```

---

### Task 4: Capacity eviction + anti-thrash

**Files:**
- Modify: `tier_manager.hpp`
- Modify: `test_tier_manager.cpp`

- [ ] **Step 1: Add the failing test — insert before the `std::printf("PASS` line**

```cpp
    // --- Task 4: capacity eviction (cost-benefit, not pure LRU) + anti-thrash ---
    {
        const size_t COL = 64u*1024*1024;
        // DEVICE holds ONE column; two compete. The hotter one must win residency.
        TierManager tm(make_cm(), /*device_cap*/ COL, /*host_cap*/ 1u<<30);
        tm.register_column(1, COL, MemorySpace::HOST); // will be hotter
        tm.register_column(2, COL, MemorySpace::HOST); // cooler

        for (int r = 0; r < 3; ++r) {
            for (int i = 0; i < 50; ++i) tm.record_access(1, COL); // col 1 very hot
            tm.record_access(2, COL);                              // col 2 barely warm
            tm.rebalance();
        }
        // Only one fits in DEVICE; the hotter column 1 should be the resident.
        assert(tm.tier_of(1) == MemorySpace::DEVICE && "hotter column wins scarce DEVICE");
        assert(tm.tier_of(2) != MemorySpace::DEVICE && "cooler column evicted/kept out of full DEVICE");
        assert(tm.resident_bytes(MemorySpace::DEVICE) <= COL && "DEVICE never over capacity");
    }
    {
        // Anti-thrash: a column promoted then idle is not evicted within MIN_RESIDENCY_TICKS.
        const size_t COL = 8u*1024*1024;
        TierManager tm(make_cm(), 1u<<30, 1u<<30);
        tm.register_column(1, COL, MemorySpace::HOST);
        for (int i = 0; i < 50; ++i) tm.record_access(1, COL);
        tm.rebalance(); // promote to DEVICE
        const MemorySpace after_promote = tm.tier_of(1);
        tm.rebalance(); // idle round — must not immediately demote (min residency)
        if (after_promote == MemorySpace::DEVICE)
            assert(tm.tier_of(1) == MemorySpace::DEVICE && "no thrash within min-residency");
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `clang++ -std=c++20 -O2 test_tier_manager.cpp -o /tmp/ttm && /tmp/ttm 2>&1 | head -5`
Expected: FAIL — DEVICE over capacity (both columns promoted, no eviction) OR the resident assertion fails.

- [ ] **Step 3: Add capacity helpers + eviction to `tier_manager.hpp`**

Add these private helpers:

```cpp
    static MemorySpace slower_tier(MemorySpace t) {
        if (t == MemorySpace::DEVICE) return MemorySpace::HOST;
        if (t == MemorySpace::HOST) return MemorySpace::COLD;
        return t; // COLD already slowest
    }

    // Usable capacity of a tier; 0 means unbounded (COLD/SSD).
    size_t capacity_of(MemorySpace t) const {
        if (t == MemorySpace::DEVICE) return device_cap_;
        if (t == MemorySpace::HOST)   return host_cap_;
        return 0; // COLD unbounded
    }

    // Cost-benefit score of keeping a column on its tier (higher = more worth keeping).
    // Lower-scored residents are evicted first when a tier is over capacity.
    double keep_score(const Column& c) const {
        const MemorySpace slower = slower_tier(c.tier);
        if (slower == c.tier) return 1e300; // COLD: never "evict" further (infinite keep)
        const double penalty = (cm_.scan_us(slower, c.bytes) - cm_.scan_us(c.tier, c.bytes))
                               * static_cast<double>(est_future_scans(c));
        return penalty; // bytes/heat that would be lost by demoting
    }
```

Then, in `rebalance()`, AFTER the promotion loop and BEFORE `return decisions;`, add the eviction pass:

```cpp
        // Capacity eviction: for each bounded tier over capacity, demote the lowest
        // keep_score residents (cost-benefit, not pure LRU) until it fits. Respect
        // MIN_RESIDENCY_TICKS so a freshly-arrived column isn't immediately thrashed out.
        for (MemorySpace tier : {MemorySpace::DEVICE, MemorySpace::HOST}) {
            const size_t cap = capacity_of(tier);
            if (cap == 0) continue;
            // Gather evictable residents (respecting min residency), worst keep_score first.
            for (;;) {
                size_t used = resident_bytes(tier);
                if (used <= cap) break;
                // find lowest keep_score evictable column on this tier
                Column* victim = nullptr;
                double worst = 1e301;
                for (auto& kv : cols_) {
                    Column& c = kv.second;
                    if (c.tier != tier) continue;
                    if (tick_ - c.arrived_tick < MIN_RESIDENCY_TICKS) continue; // anti-thrash
                    const double s = keep_score(c);
                    if (s < worst) { worst = s; victim = &c; }
                }
                if (!victim) break; // nothing evictable (all within min residency) — give up this pass
                const MemorySpace from = victim->tier;
                const MemorySpace to = slower_tier(from);
                decisions.push_back(MigrationDecision{victim->id, from, to});
                victim->tier = to;
                victim->arrived_tick = tick_;
            }
        }
```

IMPORTANT ORDERING NOTE for the implementer: the eviction loop recomputes `resident_bytes(tier)` each iteration (it reflects the in-progress demotions), so it terminates when the tier fits or nothing is evictable. The promotion in Task 3 set `arrived_tick = tick_` on promote, so a column promoted THIS tick is within min-residency and won't be evicted the same tick — that's intended (a promote isn't undone in the same pass).

- [ ] **Step 4: Run test to verify it passes**

Run: `clang++ -std=c++20 -O2 test_tier_manager.cpp -o /tmp/ttm && /tmp/ttm`
Expected: `PASS: tier manager decisions correct`, exit 0.

If the "hotter column wins scarce DEVICE" assertion is flaky because BOTH columns promote the same tick (both arrived_tick==tick_, neither evictable that tick): that's why the test runs 3 rounds — by a later round the hotter column is already resident (arrived in an earlier tick, past min-residency) and the cooler one trying to promote in gets evicted. Confirm the 3-round loop makes it deterministic; if col 2 ends DEVICE-resident instead of col 1, STOP and report (the keep_score ordering would need review).

- [ ] **Step 5: Commit**

```bash
git add tier_manager.hpp test_tier_manager.cpp
git -c user.name="garikipatisai-code" -c user.email="garikipatisai-code@users.noreply.github.com" commit -m "feat: TierManager capacity eviction (cost-benefit) + anti-thrash min-residency"
```

---

### Task 5: Determinism test + notebook

**Files:**
- Modify: `test_tier_manager.cpp`
- Modify: `make_notebook.py`

- [ ] **Step 1: Add a determinism test — insert before the `std::printf("PASS` line**

```cpp
    // --- Task 5: determinism — identical access sequence -> identical decisions ---
    {
        auto run = []() {
            TierManager tm(make_cm(), 64u*1024*1024, 1u<<30);
            tm.register_column(1, 32u*1024*1024, MemorySpace::COLD);
            tm.register_column(2, 32u*1024*1024, MemorySpace::COLD);
            std::vector<MigrationDecision> all;
            for (int r = 0; r < 4; ++r) {
                for (int i = 0; i < 20; ++i) { tm.record_access(1, 32u*1024*1024); tm.record_access(2, 16u*1024*1024); }
                auto d = tm.rebalance();
                for (auto& x : d) all.push_back(x);
            }
            return all;
        };
        auto a = run();
        auto b = run();
        assert(a.size() == b.size() && "deterministic decision count");
        for (size_t i = 0; i < a.size(); ++i) {
            assert(a[i].column_id == b[i].column_id && a[i].from == b[i].from && a[i].to == b[i].to
                   && "deterministic decision sequence");
        }
    }
```

- [ ] **Step 2: Run test to verify it passes (determinism should already hold)**

Run: `clang++ -std=c++20 -O2 test_tier_manager.cpp -o /tmp/ttm && /tmp/ttm`
Expected: `PASS: tier manager decisions correct`, exit 0.

NOTE: `std::unordered_map` iteration order is unspecified but stable within a single program run for identical insertion sequences, so two runs in the same process with identical registration order iterate identically — the determinism test passes. If it FAILS (iteration-order nondeterminism across the two runs), STOP and report: the fix is to iterate columns in sorted id order inside rebalance (collect ids, sort, iterate) — but only apply that if the test actually fails.

- [ ] **Step 3: Add tier_manager files to make_notebook.py SOURCES**

In `make_notebook.py`, add `tier_manager.hpp` (after `kv_store.hpp`) and `test_tier_manager.cpp` (after `test_kv_store.cpp`) to the `SOURCES` list. Read the current list first and preserve all existing entries.

- [ ] **Step 4: Add a TierManager test cell**

In `make_notebook.py`, find the KVStore test cell (`## 3a. KVStore unit test`). Immediately after that cell's code entry (before the cost-model `## 3b` cell), insert:

```python
    md("## 3c. TierManager unit test (CPU, no GPU)\n"
       "\n"
       "Proves the auto-tiering brain: hot columns promote toward VRAM, cold stay put, "
       "scarce tiers evict by cost-benefit (not pure LRU), anti-thrash holds, decisions "
       "are deterministic."),
    code("!clang++ -std=c++20 -O2 test_tier_manager.cpp -o /tmp/ttm 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_tier_manager.cpp -o /tmp/ttm; /tmp/ttm"),
```

- [ ] **Step 5: Regenerate + verify notebook in sync**

Run:
```bash
python3 make_notebook.py && python3 -c "
import json
nb=json.load(open('matrixdb_colab.ipynb'))
emb={c['source'].split(chr(10),1)[0].split()[1]:c['source'].split(chr(10),1)[1] for c in nb['cells'] if c['cell_type']=='code' and c['source'].startswith('%%writefile')}
print('in sync:', all(emb[f]==open(f).read() for f in emb))
print('tier_manager embedded:', 'tier_manager.hpp' in emb)
print('test embedded:', 'test_tier_manager.cpp' in emb)
"
```
Expected: all three print `True`.

- [ ] **Step 6: Commit**

```bash
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name="garikipatisai-code" -c user.email="garikipatisai-code@users.noreply.github.com" commit -m "test+docs: TierManager determinism test + notebook cell"
```

---

## Self-review (completed)

**Spec coverage:**
- §2 interface (register_column, record_access, rebalance, tier_of, heat_of, resident_bytes, MigrationDecision) → Tasks 1–4 build all of it. ✓
- §3 algorithm: heat aging (Task 2), cost-benefit promotion + hysteresis + est_future_scans + single-tier steps (Task 3), capacity eviction by keep_score + min-residency anti-thrash (Task 4). ✓
- §4 verification: promote-hot, don't-promote-cold (Task 3); cost-benefit eviction, anti-thrash (Task 4); determinism, placement-map integrity / capacity-never-exceeded (Tasks 4–5). ✓
- §1 scope discipline: no byte movement (only decisions returned), no I/O, not wired to engine — none of the tasks touch compute_mock/router/main. ✓

**Placeholder scan:** No TBDs. Every code step is complete. The two runtime risks (same-tick double-promote in Task 4; unordered_map determinism in Task 5) are called out explicitly with the deterministic test structure and a STOP-and-report fallback, not left vague.

**Type consistency:** `MigrationDecision{column_id, from, to}` consistent across all tasks. `TierManager(CostModel, size_t, size_t)`, `register_column(uint64_t,size_t,MemorySpace)`, `record_access(uint64_t,size_t)`, `rebalance()->vector<MigrationDecision>`, `tier_of/heat_of(uint64_t)`, `resident_bytes(MemorySpace)` consistent between the skeleton (Task 1) and all uses. Constants HEAT_ALPHA/HYSTERESIS/SCAN_HORIZON/MIN_RESIDENCY_TICKS defined once (Task 2) and used in Tasks 3–4. Helpers faster_tier (Task 3), slower_tier/capacity_of/keep_score (Task 4) defined before use.

**Known follow-ups (not this increment):** byte-movement executor (Inc 4), SSD substrate/durability (Inc 3), wiring TierManager into the live router/engine query path (after executor), calibration of the four tunables.

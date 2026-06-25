// CPU unit test for TierManager (the auto-tiering brain). No GPU, no I/O.
// Build: clang++ -std=c++20 -O2 test_tier_manager.cpp -o /tmp/ttm && /tmp/ttm
#include "tier_manager.hpp"
#include "cost_model.hpp"
#include "memory_model.hpp"
#include <cstdio>
#include <cassert>
#include <vector>

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

    // --- Task 4: capacity eviction (cost-benefit, not pure LRU) + anti-thrash ---
    {
        const size_t COL = 64u*1024*1024;
        // DEVICE capacity holds ONE column, but BOTH start resident on DEVICE (e.g. loaded
        // hot). Col 1 is kept hot; col 2 goes cold. Eviction must demote the cold one so
        // DEVICE fits its capacity — and keep the hot one (cost-benefit, not pure LRU).
        TierManager tm(make_cm(), /*device_cap*/ COL, /*host_cap*/ 1u<<30);
        tm.register_column(1, COL, MemorySpace::DEVICE); // hot — must stay
        tm.register_column(2, COL, MemorySpace::DEVICE); // cold — must be evicted

        for (int r = 0; r < 3; ++r) {
            for (int i = 0; i < 50; ++i) tm.record_access(1, COL); // col 1 very hot
            // col 2 gets no accesses → cold → lowest keep_score → the eviction victim
            tm.rebalance();
        }
        assert(tm.tier_of(1) == MemorySpace::DEVICE && "hot column kept on scarce DEVICE");
        assert(tm.tier_of(2) != MemorySpace::DEVICE && "cold column evicted from full DEVICE");
        assert(tm.resident_bytes(MemorySpace::DEVICE) <= COL && "DEVICE never over capacity");
    }
    {
        // Anti-thrash: when a tier is OVER capacity but the only over-capacity resident is
        // within MIN_RESIDENCY_TICKS of arriving, eviction is SUPPRESSED this pass (it is
        // not thrashed straight back out). This test genuinely exercises the min-residency
        // guard: DEVICE holds one column's worth, both columns are freshly registered on
        // DEVICE, so the first rebalance (tick 1, arrived_tick 0 → age 1 < 2) must NOT evict
        // — DEVICE stays over capacity for one tick. (It would evict if MIN_RESIDENCY were 0.)
        const size_t COL = 8u*1024*1024;
        TierManager tm(make_cm(), /*device_cap*/ COL, /*host_cap*/ 1u<<30);
        tm.register_column(1, COL, MemorySpace::DEVICE);
        tm.register_column(2, COL, MemorySpace::DEVICE);
        auto d = tm.rebalance(); // tick 1: both age 1 < MIN_RESIDENCY_TICKS(2) → no eviction
        assert(d.empty() && "no eviction within min-residency, even when over capacity");
        assert(tm.resident_bytes(MemorySpace::DEVICE) == 2*COL && "still over cap this tick");
        tm.rebalance(); // tick 2: now age 2 >= 2 → eviction permitted, DEVICE fits
        assert(tm.resident_bytes(MemorySpace::DEVICE) <= COL && "evicted once past min-residency");
    }

    // --- Capacity-gated promotion: the plan must never over-subscribe a bounded tier ---
    {
        // DEVICE holds ONE column; THREE equally-hot columns all qualify to promote.
        // Promotion must be capacity-gated so the brain never emits an infeasible plan
        // (more bytes on DEVICE than it holds). Without gating, all three promote and
        // DEVICE sits multi-x over capacity in steady state.
        const size_t COL = 64u*1024*1024;
        TierManager tm(make_cm(), /*device_cap*/ COL, /*host_cap*/ 1u<<30);
        tm.register_column(1, COL, MemorySpace::COLD);
        tm.register_column(2, COL, MemorySpace::COLD);
        tm.register_column(3, COL, MemorySpace::COLD);
        for (int r = 0; r < 8; ++r) {
            for (int i = 0; i < 50; ++i) {
                tm.record_access(1, COL); tm.record_access(2, COL); tm.record_access(3, COL);
            }
            tm.rebalance();
            // The hard invariant the eviction/gating logic exists to guarantee:
            assert(tm.resident_bytes(MemorySpace::DEVICE) <= COL
                   && "DEVICE plan must never exceed capacity (promotion is capacity-gated)");
            assert(tm.resident_bytes(MemorySpace::HOST) <= (1u<<30)
                   && "HOST plan must never exceed capacity");
        }
        // Exactly one of the three should occupy DEVICE; the rest sit on HOST.
        int on_device = 0;
        for (uint64_t id : {1u, 2u, 3u}) if (tm.tier_of(id) == MemorySpace::DEVICE) ++on_device;
        assert(on_device == 1 && "exactly one column fits the 1-column DEVICE");
    }

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

    std::printf("PASS: tier manager decisions correct\n");
    return 0;
}

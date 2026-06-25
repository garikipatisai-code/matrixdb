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

    std::printf("PASS: tier manager decisions correct\n");
    return 0;
}

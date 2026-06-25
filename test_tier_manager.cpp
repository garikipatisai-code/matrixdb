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

    std::printf("PASS: tier manager decisions correct\n");
    return 0;
}

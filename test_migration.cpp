// CPU unit test for cross-tier migration (HOST<->COLD; DEVICE is Colab-verified).
// Build: clang++ -std=c++20 -O2 test_migration.cpp -o /tmp/tmig && /tmp/tmig
#include "tiered_column.hpp"
#include "migration_executor.hpp"
#include "tier_manager.hpp"
#include "cost_model.hpp"
#include <unordered_map>
#include <cstdio>
#include <cassert>
#include <vector>
#include <numeric>

int main() {
    // --- Task 1: HOST<->COLD round-trip + integrity ---
    {
        std::vector<unsigned char> data(4096);
        for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<unsigned char>(i * 7 + 1);
        TieredColumn col(1, data.data(), data.size());
        const uint64_t want = col.checksum();
        assert(col.tier() == MemorySpace::HOST && "born in HOST");
        assert(col.size_bytes() == 4096);

        col.migrate_to(MemorySpace::COLD);
        assert(col.tier() == MemorySpace::COLD && "moved to COLD");
        assert(col.checksum() == want && "checksum invariant HOST->COLD");

        col.migrate_to(MemorySpace::HOST);
        assert(col.tier() == MemorySpace::HOST && "moved back to HOST");
        assert(col.checksum() == want && "checksum invariant COLD->HOST");

        // Integrity across a chain.
        col.migrate_to(MemorySpace::COLD);
        col.migrate_to(MemorySpace::HOST);
        col.migrate_to(MemorySpace::COLD);
        assert(col.checksum() == want && "checksum invariant across a HOST/COLD chain");
        col.migrate_to(MemorySpace::HOST); // leave on HOST so dtor frees the vector (no temp file left)
    }

    // --- Task 2: TierManager decisions actually move columns (the auto-tiering loop) ---
    {
        // Hand-built plan (decision-driven loop; TierManager produces exactly this shape).
        const size_t N = 4096;
        std::vector<unsigned char> bytes(N, 0xAB);
        TieredColumn hot(1, bytes.data(), N);   // stays
        TieredColumn cold(2, bytes.data(), N);  // will be demoted to COLD
        const uint64_t hot_sum = hot.checksum(), cold_sum = cold.checksum();

        std::unordered_map<uint64_t, TieredColumn*> columns{{1, &hot}, {2, &cold}};

        std::vector<MigrationDecision> plan{
            MigrationDecision{2, MemorySpace::HOST, MemorySpace::COLD},
            MigrationDecision{99, MemorySpace::HOST, MemorySpace::COLD}, // absent id -> skipped
        };

        MigrationExecutor exec;
        const size_t applied = exec.apply(plan, columns);
        assert(applied == 1 && "one valid decision applied, absent id skipped");
        assert(cold.tier() == MemorySpace::COLD && "cold column physically demoted");
        assert(hot.tier() == MemorySpace::HOST && "untouched column stays");
        assert(cold.checksum() == cold_sum && "demoted column bytes intact");
        assert(hot.checksum() == hot_sum && "untouched column bytes intact");
        cold.migrate_to(MemorySpace::HOST); // cleanup: leave on HOST so no temp file remains
    }

    // --- Task 2b: a real TierManager rebalance feeds the executor ---
    {
        const size_t N = 4096;
        std::vector<unsigned char> bytes(N, 0x5C);
        TieredColumn a(10, bytes.data(), N);
        TieredColumn b(11, bytes.data(), N);
        std::unordered_map<uint64_t, TieredColumn*> columns{{10, &a}, {11, &b}};

        // Brain: both registered on HOST; HOST cap holds ONE column, so the colder is evicted.
        TierManager tm(CostModel(MemoryModel::detect(true), true),
                       /*device_cap*/ 1u<<30, /*host_cap*/ N);
        tm.register_column(10, N, MemorySpace::HOST);
        tm.register_column(11, N, MemorySpace::HOST);
        for (int r = 0; r < 3; ++r) {
            for (int i = 0; i < 50; ++i) tm.record_access(10, N); // col 10 hot
            auto plan = tm.rebalance();
            MigrationExecutor exec;
            exec.apply(plan, columns);
        }
        assert(a.tier() == tm.tier_of(10) && "executor synced column 10 to brain's tier");
        assert(b.tier() == tm.tier_of(11) && "executor synced column 11 to brain's tier");
        assert(a.checksum() == b.checksum() && "both columns' bytes intact (same fill)");
        a.migrate_to(MemorySpace::HOST); b.migrate_to(MemorySpace::HOST); // cleanup temp files
    }

    std::printf("PASS: migration correct\n");
    return 0;
}

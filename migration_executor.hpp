#pragma once

#include "tier_manager.hpp"   // MigrationDecision
#include "tiered_column.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include <cstdio>

// Turns a TierManager migration plan into physical byte movement. The brain decides
// (which column goes where); the executor actuates (calls migrate_to on the real bytes).
class MigrationExecutor {
public:
    // Apply each decision by migrating the named column to its target tier. A decision whose
    // column_id is not in the registry is skipped (logged). Returns the number applied.
    size_t apply(const std::vector<MigrationDecision>& plan,
                 std::unordered_map<uint64_t, TieredColumn*>& columns) {
        size_t applied = 0;
        for (const MigrationDecision& d : plan) {
            auto it = columns.find(d.column_id);
            if (it == columns.end()) {
                std::fprintf(stderr, "MigrationExecutor: no column %llu — skipped\n",
                             static_cast<unsigned long long>(d.column_id));
                continue;
            }
            it->second->migrate_to(d.to);
            ++applied;
        }
        return applied;
    }
};

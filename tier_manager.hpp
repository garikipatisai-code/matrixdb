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

        // Capacity eviction: for each bounded tier over capacity, demote the lowest
        // keep_score residents (cost-benefit, not pure LRU) until it fits. Respect
        // MIN_RESIDENCY_TICKS so a freshly-arrived column isn't immediately thrashed out.
        for (MemorySpace tier : {MemorySpace::DEVICE, MemorySpace::HOST}) {
            const size_t cap = capacity_of(tier);
            if (cap == 0) continue;
            for (;;) {
                if (resident_bytes(tier) <= cap) break;
                Column* victim = nullptr;
                double worst = 1e301;
                for (auto& kv : cols_) {
                    Column& c = kv.second;
                    if (c.tier != tier) continue;
                    if (tick_ - c.arrived_tick < MIN_RESIDENCY_TICKS) continue; // anti-thrash
                    const double s = keep_score(c);
                    if (s < worst) { worst = s; victim = &c; }
                }
                if (!victim) break; // nothing evictable (all within min residency) this pass
                const MemorySpace from = victim->tier;
                const MemorySpace to = slower_tier(from);
                decisions.push_back(MigrationDecision{victim->id, from, to});
                victim->tier = to;
                victim->arrived_tick = tick_;
            }
        }

        return decisions;
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
        return penalty; // time/heat that would be lost by demoting
    }

    CostModel cm_;
    size_t device_cap_;
    size_t host_cap_;
    uint64_t tick_ = 0;
    std::unordered_map<uint64_t, Column> cols_;
};

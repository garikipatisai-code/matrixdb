#pragma once

#include "cost_model.hpp"
#include "memory_model.hpp"
#include "tier_model.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include <algorithm>

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

        // Promotion: move qualifying columns one tier toward DEVICE — but capacity-gated,
        // so the emitted plan never over-subscribes a bounded tier (the executor in Inc 4
        // could not honor an over-capacity plan). Approve best-first (highest net benefit)
        // and only if the column fits its target tier given current + already-approved
        // same-tick promotions (resident_bytes reflects approved moves as we apply them).
        std::vector<uint64_t> candidates;
        for (auto& kv : cols_) if (should_promote(kv.second)) candidates.push_back(kv.first);
        std::sort(candidates.begin(), candidates.end(),
                  [this](uint64_t a, uint64_t b) {
                      const double na = promote_net_benefit(cols_.at(a));
                      const double nb = promote_net_benefit(cols_.at(b));
                      if (na != nb) return na > nb;     // best benefit first
                      return a < b;                      // tie-break by id (determinism)
                  });
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

    // Benefit (scan-time saved over the horizon) and cost (one-time migration) of promoting
    // a column one tier toward DEVICE. Single source of the promotion arithmetic so
    // should_promote and promote_net_benefit can never drift. benefit==cost==0 if already
    // on the fastest tier.
    struct PromoteEval { double benefit; double cost; };
    PromoteEval promote_eval(const Column& c) const {
        const MemorySpace faster = faster_tier(c.tier);
        if (faster == c.tier) return PromoteEval{0.0, 0.0};
        const double benefit = (cm_.scan_us(c.tier, c.bytes) - cm_.scan_us(faster, c.bytes))
                               * static_cast<double>(est_future_scans(c));
        const double cost = cm_.migration_us(c.tier, faster, c.bytes);
        return PromoteEval{benefit, cost};
    }

    // Should this column be promoted one tier now? (cost-benefit + hysteresis)
    bool should_promote(const Column& c) const {
        if (faster_tier(c.tier) == c.tier) return false; // already fastest
        const PromoteEval e = promote_eval(c);
        return e.benefit > HYSTERESIS * e.cost;
    }

    // Net benefit (benefit - cost) of promoting one tier — used to rank candidates so the
    // best columns win scarce capacity first. Only meaningful when should_promote is true.
    double promote_net_benefit(const Column& c) const {
        const PromoteEval e = promote_eval(c);
        return e.benefit - e.cost;
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

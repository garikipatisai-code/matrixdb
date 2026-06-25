#pragma once

#include "compute.hpp"
#include "cost_model.hpp"
#include "memory_model.hpp"
#include <cassert>
#include <cstdint>
#include <vector>

// Routing policy over two live engines. Holds no data and runs no kernels: it decides
// (via CostModel) where each dataset lives, records it in a placement map, and dispatches
// each query to the engine that owns its data. gpu may be null (no-GPU build) — then the
// CostModel places everything HOST and all routing goes to the CPU engine.
class Router {
public:
    Router(ComputeInterface* cpu, ComputeInterface* gpu, CostModel cm)
        : cpu_(cpu), gpu_(gpu), cm_(cm) {}

    // Point ops always run on the CPU engine (page-ownership KV store lives in HOST RAM).
    void route_batch(DatabaseQuery* batch, size_t count) {
        cpu_->execute_batch(batch, count);
    }

    // Register a scan column of `bytes`, deciding its home now. Returns a dataset id used
    // by route_scan. One home per dataset — recorded once, never duplicated.
    uint64_t place_scan_column(size_t bytes) {
        const MemorySpace home =
            (gpu_ != nullptr) ? cm_.place_scan(bytes) : MemorySpace::HOST;
        placement_.push_back(home);
        return static_cast<uint64_t>(placement_.size() - 1);
    }

    // Dispatch a scan to the engine that owns the column's data.
    void route_scan(DatabaseQuery& q, uint64_t dataset_id) {
        assert(dataset_id < placement_.size() && "route_scan: unknown dataset_id");
        const MemorySpace home = placement_[dataset_id];
        ComputeInterface* eng = (home == MemorySpace::DEVICE && gpu_) ? gpu_ : cpu_;
        eng->execute_scan(q);
    }

    MemorySpace home_of(uint64_t dataset_id) const {
        assert(dataset_id < placement_.size() && "home_of: unknown dataset_id");
        return placement_[dataset_id];
    }

private:
    ComputeInterface* cpu_;
    ComputeInterface* gpu_;             // may be null
    CostModel cm_;
    std::vector<MemorySpace> placement_; // dataset id -> home (the entire coherence story)
};

#pragma once

#include "memory_model.hpp"
#include <cstddef>

// Cost-based placement: predict microseconds for a workload on each processor, choose
// the cheaper home. Constants are MEASURED on the target machine (values below are first
// estimates from Tesla T4 runs — see the spec's calibration note). The structure is the
// deliverable; the constants get one calibration pass before the boundary is trusted.
class CostModel {
public:
    explicit CostModel(MemoryModel mm, bool gpu_available = true)
        : mm_(mm), gpu_available_(gpu_available) {}

    // Point ops: CPU always wins (PCIe slower than CPU cache, ~1 memory op/query).
    MemorySpace place_point() const { return MemorySpace::HOST; }

    // Scan of `bytes`: place where the predicted scan time is lower.
    MemorySpace place_scan(size_t bytes) const {
        if (!gpu_available_) return MemorySpace::HOST;
        return device_scan_us(bytes) < host_scan_us(bytes)
                   ? MemorySpace::DEVICE
                   : MemorySpace::HOST;
    }

    // Predicted microseconds (exposed for tests / future tuning).
    double host_scan_us(size_t bytes) const {
        return static_cast<double>(bytes) / CPU_SCAN_BPus;
    }
    double device_scan_us(size_t bytes) const {
        // Transfer is amortized to 0 today (a column is placed once and scanned many).
        // Both branches are 0.0 ON PURPOSE: this is the seam where the discrete one-time
        // transfer term will go; UNIFIED stays 0. Deliberately inert until calibrated.
        const double transfer = mm_.is_unified() ? 0.0 : 0.0;
        return LAUNCH_US + transfer + static_cast<double>(bytes) / GPU_SCAN_BPus;
    }

private:
    // --- measured constants (bytes per microsecond) — CALIBRATION TARGETS ---
    static constexpr double CPU_SCAN_BPus = 10'000.0;   // ~10 GB/s  (measured)
    static constexpr double GPU_SCAN_BPus = 240'000.0;  // ~240 GB/s (measured at 64 MB)
    static constexpr double LAUNCH_US     = 30.0;       // per-scan GPU launch floor (measured)

    MemoryModel mm_;
    bool gpu_available_;
};

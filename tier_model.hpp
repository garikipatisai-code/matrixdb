#pragma once

#include "memory_model.hpp"
#include <cstddef>

// The storage tiers, ordered fastest-scan to coldest. DEVICE=VRAM, HOST=RAM, COLD=SSD.
// UNIFIED is the existing seam from memory_model.hpp (CPU+GPU share one pool); when the
// machine is unified, DEVICE and HOST collapse and migration between them is a no-op.
//
// NOTE: MemorySpace is DEFINED in memory_model.hpp as { HOST, DEVICE, COLD, UNIFIED };
// this header layers the per-tier physics on top of that enum.

// Physics of one tier: what it's good at and what it costs. bandwidth in bytes/microsecond
// (so it composes with CostModel's existing *_BPus units); latency in microseconds to first
// byte; capacity_bytes is the usable budget (0 == "treat as unbounded for now").
struct TierPhysics {
    double   scan_bytes_per_us;  // sequential scan bandwidth
    double   access_latency_us;  // latency to first byte (random-access cost proxy)
    size_t   capacity_bytes;     // usable capacity budget; 0 = unbounded (not yet enforced)
    const char* concern;         // the tier's defining risk, for docs/telemetry
};

// First-estimate physics per tier (CALIBRATION TARGETS, like CostModel's constants).
// VRAM/RAM scan numbers are measured; SSD and latencies are estimates to refine on the box.
inline TierPhysics tier_physics(MemorySpace s) {
    switch (s) {
        case MemorySpace::DEVICE: // VRAM
            return TierPhysics{240'000.0, 5.0, 16ull*1024*1024*1024, "scarce capacity; PCIe to reach"};
        case MemorySpace::HOST:   // RAM
            return TierPhysics{10'000.0, 0.1, 256ull*1024*1024*1024, "medium capacity"};
        case MemorySpace::COLD:   // SSD
            return TierPhysics{3'000.0, 100.0, 0 /*unbounded*/, "high latency; write wear; append-only"};
        case MemorySpace::UNIFIED:
            return TierPhysics{240'000.0, 0.1, 0, "unified pool (future hardware)"};
    }
    return TierPhysics{1.0, 1.0, 0, "unknown"};
}

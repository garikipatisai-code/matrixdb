// CPU-only unit test for the cost model + placement. No GPU, no engines.
// Build: clang++ -std=c++20 -O2 test_cost_model.cpp -o /tmp/tcm && /tmp/tcm
#include "cost_model.hpp"
#include "memory_model.hpp"
#include <cstdio>
#include <cassert>

int main() {
    const MemoryModel discrete = MemoryModel::detect(/*gpu_available=*/true);
    CostModel cm(discrete);

    // 1. Point ops NEVER go to DEVICE (measured: PCIe < CPU cache).
    assert(cm.place_point() == MemorySpace::HOST && "point op must place HOST");

    // 2. A tiny scan stays HOST (GPU launch floor dominates below the crossover).
    assert(cm.place_scan(1024) == MemorySpace::HOST && "1KB scan must place HOST");

    // 3. A large scan goes DEVICE (GPU bandwidth wins far above the crossover).
    assert(cm.place_scan(64u * 1024 * 1024) == MemorySpace::DEVICE && "64MB scan must place DEVICE");

    // 4. The crossover is monotonic: there is a single size boundary, HOST below it,
    //    DEVICE at/above it (no oscillation).
    bool seen_device = false;
    MemorySpace prev = MemorySpace::HOST;
    for (size_t b = 256; b <= (1u << 27); b *= 2) {
        MemorySpace s = cm.place_scan(b);
        if (s == MemorySpace::DEVICE) seen_device = true;
        // once DEVICE, never back to HOST as size grows
        assert(!(prev == MemorySpace::DEVICE && s == MemorySpace::HOST) && "crossover not monotonic");
        prev = s;
    }
    assert(seen_device && "some large size must reach DEVICE");

    // 5. No-GPU machine: everything is HOST.
    CostModel cm_nogpu(discrete, /*gpu_available=*/false);
    assert(cm_nogpu.place_scan(64u * 1024 * 1024) == MemorySpace::HOST && "no-GPU must place HOST");

    std::printf("PASS: cost model placement decisions correct\n");
    return 0;
}

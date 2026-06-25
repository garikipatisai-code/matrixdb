// CPU-only unit test for the cost model + placement. No GPU, no engines.
// Build: clang++ -std=c++20 -O2 test_cost_model.cpp -o /tmp/tcm && /tmp/tcm
#include "cost_model.hpp"
#include "memory_model.hpp"
#include <cstdio>
#include <cassert>
#include "router.hpp"
#include "compute.hpp"
#include <vector>
#include "tier_model.hpp"

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

    // --- Router routing test with a fake engine that records what it received ---
    struct FakeEngine : ComputeInterface {
        uint64_t batch_calls = 0, scan_calls = 0, last_batch_n = 0;
        void execute_batch(DatabaseQuery*, size_t n) override { ++batch_calls; last_batch_n = n; }
        void execute_scan(DatabaseQuery& q) override { ++scan_calls; q.transaction_id = 42; }
        uint64_t reads() const override { return 0; }
        uint64_t writes() const override { return 0; }
        uint64_t commits() const override { return 0; }
        uint64_t scans() const override { return scan_calls; }
        uint64_t scan_result_sum() const override { return 0; }
        double scan_time_s() const override { return 0; }
        double scan_kernel_time_s() const override { return 0; }
        uint64_t store_checksum() const override { return 0; }
        double benchmark_scan(size_t, uint64_t, uint64_t&) override { return 0; }
        double benchmark_scan_u32(size_t, uint32_t, uint64_t&) override { return 0; }
        double benchmark_scan_u32x4(size_t, uint32_t, uint64_t&) override { return 0; }
        double benchmark_scan_ipt(size_t, uint32_t, uint64_t&) override { return 0; }
    };

    FakeEngine cpu_eng, gpu_eng;
    Router router(&cpu_eng, &gpu_eng, CostModel(discrete));

    // Point-op batch always routes to CPU.
    std::vector<DatabaseQuery> pts(4);
    for (auto& q : pts) q.opcode = OP_READ;
    router.route_batch(pts.data(), pts.size());
    assert(cpu_eng.batch_calls == 1 && gpu_eng.batch_calls == 0 && "point batch must hit CPU");

    // A scan over a DEVICE-placed column routes to GPU.
    const uint64_t big_id = router.place_scan_column(64u * 1024 * 1024); // returns a dataset id
    DatabaseQuery sbig{}; matrix_set_scan_threshold(sbig, 1);
    router.route_scan(sbig, big_id);
    assert(gpu_eng.scan_calls == 1 && cpu_eng.scan_calls == 0 && "big scan must hit GPU");

    // A scan over a HOST-placed column routes to CPU.
    const uint64_t small_id = router.place_scan_column(1024);
    DatabaseQuery ssmall{}; matrix_set_scan_threshold(ssmall, 1);
    router.route_scan(ssmall, small_id);
    assert(cpu_eng.scan_calls == 1 && "small scan must hit CPU");

    // --- Tier-aware cost: scan time per tier, and migration cost ---
    {
        CostModel tcm(discrete);
        const size_t bytes = 64u * 1024 * 1024; // 64 MB

        // Per-tier scan time ranks by bandwidth: DEVICE (VRAM) fastest, COLD (SSD) slowest.
        const double dev = tcm.scan_us(MemorySpace::DEVICE, bytes);
        const double host = tcm.scan_us(MemorySpace::HOST, bytes);
        const double cold = tcm.scan_us(MemorySpace::COLD, bytes);
        assert(dev < host && host < cold && "scan time must rank DEVICE < HOST < COLD");

        // Migration cost is non-negative and zero when source==dest (no move).
        assert(tcm.migration_us(MemorySpace::HOST, MemorySpace::HOST, bytes) == 0.0
               && "no-op migration costs 0");
        assert(tcm.migration_us(MemorySpace::HOST, MemorySpace::DEVICE, bytes) > 0.0
               && "real migration costs > 0");
    }

    std::printf("PASS: cost model placement decisions correct\n");
    return 0;
}

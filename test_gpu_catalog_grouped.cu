// GPU-3g cross-check (Colab/nvcc only): a GROUP BY over DEVICE/VRAM-resident key+value columns must equal
// matrix_cpu_group_reduce(_pred) over the same bytes — for {COUNT,SUM,MIN,MAX} x {filtered,unfiltered}.
// Proves the grouped analytical path (not just scalar) runs on the GPU: pin key+value to VRAM
// (pin_device), run execute_query GROUP BY (which dispatches matrix_gpu_group_reduce_dev when both columns
// are tier()==DEVICE) and compare per-group to the CPU oracle. Merge gate for grouped-on-device. Build:
//   nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread -DMATRIX_USE_CUDA test_gpu_catalog_grouped.cu -o test_gpu_catalog_grouped && ./test_gpu_catalog_grouped
#include "compute_mock.cpp"   // CPUMockEngine (execute_query, pin_device) + matrix_cpu_group_reduce(_pred) oracle
#include "compute_cuda.cuh"   // defines matrix_gpu_group_reduce_dev + the grouped kernels
#include <cstdio>
#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    const size_t N = 1u << 20;
    const uint32_t G = 8;
    std::vector<uint32_t> keys(N), vals(N);
    for (size_t i = 0; i < N; ++i) { keys[i] = static_cast<uint32_t>(i % G); vals[i] = static_cast<uint32_t>(i % 1000); }

    CPUMockEngine eng;
    eng.load_scan_column(1, keys.data(), N);   // group key
    eng.load_scan_column(2, vals.data(), N);   // value
    assert(eng.pin_device(1) && eng.pin_device(2) && "key+value pinned to VRAM");   // both DEVICE-resident

    const MatrixAggOp ops[] = {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX};
    const char* on[] = {"COUNT", "SUM", "MIN", "MAX"};
    std::printf("GPU-3g GROUP BY (DEVICE-resident key+value, G=%u) vs matrix_cpu_group_reduce(_pred):\n", G);

    for (int filt = 0; filt < 2; ++filt) for (int k = 0; k < 4; ++k) {
        MatrixQuery q; q.value_col = 2; q.agg = ops[k]; q.grouped = true; q.key_col = 1; q.num_groups = G;
        q.has_filter = (filt == 1); q.cmp = MatrixCmp::GT; q.threshold = 500;
        std::vector<uint64_t> gpu;
        const MatrixQueryStatus st = eng.execute_query(q, gpu);
        assert(st == MatrixQueryStatus::OK && gpu.size() == G);

        std::vector<uint64_t> cpu(G);
        if (q.has_filter) matrix_cpu_group_reduce_pred(keys.data(), vals.data(), N, G, ops[k], MatrixPredicate{MatrixCmp::GT, 500, 0}, cpu.data());
        else              matrix_cpu_group_reduce(keys.data(), vals.data(), N, G, ops[k], cpu.data());

        const bool ok = (gpu == cpu);
        std::printf("  %-9s %-5s match=%s  (g0 GPU=%llu CPU=%llu)\n", filt ? "GT500" : "all", on[k],
                    ok ? "OK" : "*** MISMATCH ***", (unsigned long long)gpu[0], (unsigned long long)cpu[0]);
        assert(ok && "GPU GROUP BY DEVICE path == matrix_cpu_group_reduce(_pred) for every group");
    }

    std::printf("ALL GPU-CATALOG-GROUPED TESTS PASSED\n");
    return 0;
}

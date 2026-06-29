// GPU-3g cross-check (Colab/nvcc only): a GROUP BY over DEVICE/VRAM-resident key+value columns must equal
// matrix_cpu_group_reduce[_i64/_f64](_pred) over the same bytes — for u32/i64/f64 values x {COUNT,SUM,MIN,MAX}
// x {filtered,unfiltered}. Proves the grouped analytical path (all value types) runs on the GPU: pin
// key+value to VRAM (pin_device), run execute_query GROUP BY (which dispatches matrix_gpu_group_reduce_dev*
// when both columns are tier()==DEVICE) and compare per-group to the CPU oracle. Merge gate. Build:
//   nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread -DMATRIX_USE_CUDA test_gpu_catalog_grouped.cu -o test_gpu_catalog_grouped && ./test_gpu_catalog_grouped
#include "compute_mock.cpp"   // CPUMockEngine (execute_query, pin_device) + matrix_cpu_group_reduce* oracle
#include "compute_cuda.cuh"   // matrix_gpu_group_reduce_dev{,_i64,_f64} + the grouped kernels
#include <cstdio>
#include <cassert>
#include <cstdint>
#include <vector>

static const MatrixAggOp OPS[] = {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX};
static const char* ON[] = {"COUNT", "SUM", "MIN", "MAX"};

int main() {
    const size_t N = 1u << 20;
    const uint32_t G = 8;
    std::vector<uint32_t> keys(N);
    for (size_t i = 0; i < N; ++i) keys[i] = static_cast<uint32_t>(i % G);

    // ---- u32 value, filter GT 500 + unfiltered ----
    {
        std::vector<uint32_t> vals(N);
        for (size_t i = 0; i < N; ++i) vals[i] = static_cast<uint32_t>(i % 1000);
        CPUMockEngine eng;
        eng.load_scan_column(1, keys.data(), N);
        eng.load_scan_column(2, vals.data(), N);
        assert(eng.pin_device(1) && eng.pin_device(2));
        std::printf("GPU-3g u32 GROUP BY (DEVICE-resident key+value, G=%u) vs matrix_cpu_group_reduce:\n", G);
        for (int filt = 0; filt < 2; ++filt) for (int k = 0; k < 4; ++k) {
            MatrixQuery q; q.value_col = 2; q.agg = OPS[k]; q.grouped = true; q.key_col = 1; q.num_groups = G;
            q.has_filter = (filt == 1); q.cmp = MatrixCmp::GT; q.threshold = 500;
            std::vector<uint64_t> gpu; assert(eng.execute_query(q, gpu) == MatrixQueryStatus::OK && gpu.size() == G);
            std::vector<uint64_t> cpu(G);
            if (q.has_filter) matrix_cpu_group_reduce_pred(keys.data(), vals.data(), N, G, OPS[k], MatrixPredicate{MatrixCmp::GT, 500, 0}, cpu.data());
            else              matrix_cpu_group_reduce(keys.data(), vals.data(), N, G, OPS[k], cpu.data());
            std::printf("  %-6s %-5s match=%s (g0 GPU=%llu CPU=%llu)\n", filt ? "GT500" : "all", ON[k],
                        gpu == cpu ? "OK" : "*** MISMATCH ***", (unsigned long long)gpu[0], (unsigned long long)cpu[0]);
            assert(gpu == cpu && "u32 GROUP BY DEVICE path == matrix_cpu_group_reduce");
        }
    }

    // ---- i64 value (negatives + > 2^32), filter GT 0 + unfiltered ----
    {
        std::vector<int64_t> vals(N);
        for (size_t i = 0; i < N; ++i) vals[i] = ((int64_t)(i % 4000) - 2000) * 3000000LL;
        CPUMockEngine eng;
        eng.load_scan_column(1, keys.data(), N);
        eng.load_scan_column_i64(2, vals.data(), N);
        assert(eng.pin_device(1) && eng.pin_device(2));
        std::printf("GPU-3g i64 GROUP BY (DEVICE-resident) vs matrix_cpu_group_reduce_i64:\n");
        for (int filt = 0; filt < 2; ++filt) for (int k = 0; k < 4; ++k) {
            MatrixQuery q; q.value_col = 2; q.agg = OPS[k]; q.grouped = true; q.key_col = 1; q.num_groups = G;
            q.has_filter = (filt == 1); q.cmp = MatrixCmp::GT; q.lo_i64 = 0; q.hi_i64 = 0;
            std::vector<uint64_t> gpu; assert(eng.execute_query(q, gpu) == MatrixQueryStatus::OK && gpu.size() == G);
            std::vector<int64_t> tmp(G);
            if (q.has_filter) matrix_cpu_group_reduce_i64_pred(keys.data(), vals.data(), N, G, OPS[k], MatrixPredicateI64{MatrixCmp::GT, 0, 0}, tmp.data());
            else              matrix_cpu_group_reduce_i64(keys.data(), vals.data(), N, G, OPS[k], tmp.data());
            std::vector<uint64_t> cpu(G); for (uint32_t g = 0; g < G; ++g) cpu[g] = static_cast<uint64_t>(tmp[g]);
            std::printf("  %-6s %-5s match=%s (g0 GPU=%lld CPU=%lld)\n", filt ? "GT0" : "all", ON[k],
                        gpu == cpu ? "OK" : "*** MISMATCH ***", (long long)gpu[0], (long long)tmp[0]);
            assert(gpu == cpu && "i64 GROUP BY DEVICE path == matrix_cpu_group_reduce_i64");
        }
    }

    // ---- f64 value (half-integers -> exact order-independent SUM), filter GT 0 + unfiltered ----
    {
        std::vector<double> vals(N);
        for (size_t i = 0; i < N; ++i) vals[i] = ((double)((int64_t)(i % 2000) - 1000)) * 0.5;
        CPUMockEngine eng;
        eng.load_scan_column(1, keys.data(), N);
        eng.load_scan_column_f64(2, vals.data(), N);
        assert(eng.pin_device(1) && eng.pin_device(2));
        std::printf("GPU-3g f64 GROUP BY (DEVICE-resident, half-integers) vs matrix_cpu_group_reduce_f64:\n");
        for (int filt = 0; filt < 2; ++filt) for (int k = 0; k < 4; ++k) {
            MatrixQuery q; q.value_col = 2; q.agg = OPS[k]; q.grouped = true; q.key_col = 1; q.num_groups = G;
            q.has_filter = (filt == 1); q.cmp = MatrixCmp::GT; q.lo_f64 = 0.0; q.hi_f64 = 0.0;
            std::vector<uint64_t> gpu; assert(eng.execute_query(q, gpu) == MatrixQueryStatus::OK && gpu.size() == G);
            std::vector<double> tmp(G);
            if (q.has_filter) matrix_cpu_group_reduce_f64_pred(keys.data(), vals.data(), N, G, OPS[k], MatrixPredicateF64{MatrixCmp::GT, 0.0, 0.0}, tmp.data());
            else              matrix_cpu_group_reduce_f64(keys.data(), vals.data(), N, G, OPS[k], tmp.data());
            std::vector<uint64_t> cpu(G); for (uint32_t g = 0; g < G; ++g) cpu[g] = matrix_bit_cast<uint64_t>(tmp[g]);
            std::printf("  %-6s %-5s match=%s (g0 GPU=%g CPU=%g)\n", filt ? "GT0" : "all", ON[k],
                        gpu == cpu ? "OK" : "*** MISMATCH ***", matrix_bit_cast<double>(gpu[0]), tmp[0]);
            assert(gpu == cpu && "f64 GROUP BY DEVICE path == matrix_cpu_group_reduce_f64");
        }
    }

    std::printf("ALL GPU-CATALOG-GROUPED TESTS PASSED\n");
    return 0;
}

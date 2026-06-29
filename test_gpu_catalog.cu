// GPU-3 cross-check (Colab/nvcc only): execute_query over a DEVICE/VRAM-resident catalog column must
// equal the CPU reducer over the same bytes — for u32/i64/f64 x {COUNT,SUM,MIN,MAX} x {filtered,unfiltered}.
// This proves the *analytical* path (not just the legacy scan column) runs on the GPU and matches the
// oracle: pin a column to VRAM (CPUMockEngine::pin_device), run execute_query (which now dispatches the
// verified matrix_gpu_reduce_dev_* kernels when tier()==DEVICE), and compare to matrix_cpu_reduce_*.
// Merge gate for GPU-3. Build (Colab T4):
//   nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread -DMATRIX_USE_CUDA test_gpu_catalog.cu -o test_gpu_catalog && ./test_gpu_catalog
#include "compute_mock.cpp"   // CPUMockEngine (execute_query, pin_device) + matrix_cpu_reduce_* oracle
#include "compute_cuda.cuh"   // defines matrix_gpu_reduce_dev_* (forward-declared in compute_mock.cpp)
#include <cstdio>
#include <cassert>
#include <cstdint>
#include <vector>

static const MatrixAggOp OPS[] = {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX};
static const char* ON[] = {"COUNT", "SUM", "MIN", "MAX"};

int main() {
    const size_t N = 1u << 20;

    // ---- u32: values 0..999, filter GT 500 (a known subset) + unfiltered ----
    {
        std::vector<uint32_t> v(N);
        for (size_t i = 0; i < N; ++i) v[i] = static_cast<uint32_t>(i % 1000);
        CPUMockEngine eng;
        eng.load_scan_column(1, v.data(), N);
        assert(eng.pin_device(1) && "u32 column must pin to VRAM");   // now DEVICE-resident
        std::printf("GPU-3 u32 execute_query (DEVICE-resident) vs matrix_cpu_reduce*:\n");
        for (int filt = 0; filt < 2; ++filt) for (int k = 0; k < 4; ++k) {
            MatrixQuery q; q.value_col = 1; q.agg = OPS[k];
            q.has_filter = (filt == 1); q.cmp = MatrixCmp::GT; q.threshold = 500;
            std::vector<uint64_t> out;
            const MatrixQueryStatus st = eng.execute_query(q, out);
            assert(st == MatrixQueryStatus::OK && out.size() == 1);
            const uint64_t gpu = out[0];
            const uint64_t cpu = q.has_filter
                ? matrix_cpu_reduce_pred(v.data(), N, MatrixPredicate{MatrixCmp::GT, 500, 0}, OPS[k])
                : matrix_cpu_reduce_all(v.data(), N, OPS[k]);
            std::printf("  %-9s %-5s GPU=%llu CPU=%llu  %s\n", filt ? "GT500" : "all", ON[k],
                        (unsigned long long)gpu, (unsigned long long)cpu, gpu == cpu ? "OK" : "*** MISMATCH ***");
            assert(gpu == cpu && "u32 execute_query DEVICE path == matrix_cpu_reduce*");
        }
    }

    // ---- i64: negatives + values > 2^32, filter GT 0 + unfiltered ----
    {
        std::vector<int64_t> v(N);
        for (size_t i = 0; i < N; ++i) v[i] = ((int64_t)(i % 4000) - 2000) * 3000000LL;
        CPUMockEngine eng;
        eng.load_scan_column_i64(1, v.data(), N);
        assert(eng.pin_device(1) && "i64 column must pin to VRAM");
        std::printf("GPU-3 i64 execute_query (DEVICE-resident) vs matrix_cpu_reduce*_i64:\n");
        for (int filt = 0; filt < 2; ++filt) for (int k = 0; k < 4; ++k) {
            MatrixQuery q; q.value_col = 1; q.agg = OPS[k];
            q.has_filter = (filt == 1); q.cmp = MatrixCmp::GT; q.lo_i64 = 0; q.hi_i64 = 0;
            std::vector<uint64_t> out;
            const MatrixQueryStatus st = eng.execute_query(q, out);
            assert(st == MatrixQueryStatus::OK && out.size() == 1);
            const int64_t gpu = static_cast<int64_t>(out[0]);
            const int64_t cpu = q.has_filter
                ? matrix_cpu_reduce_pred_i64(v.data(), N, MatrixPredicateI64{MatrixCmp::GT, 0, 0}, OPS[k])
                : matrix_cpu_reduce_all_i64(v.data(), N, OPS[k]);
            std::printf("  %-9s %-5s GPU=%lld CPU=%lld  %s\n", filt ? "GT0" : "all", ON[k],
                        (long long)gpu, (long long)cpu, gpu == cpu ? "OK" : "*** MISMATCH ***");
            assert(gpu == cpu && "i64 execute_query DEVICE path == matrix_cpu_reduce*_i64");
        }
    }

    // ---- f64: half-integers (exact order-independent SUM), filter GT 0 + unfiltered ----
    {
        std::vector<double> v(N);
        for (size_t i = 0; i < N; ++i) v[i] = ((double)((int64_t)(i % 2000) - 1000)) * 0.5;
        CPUMockEngine eng;
        eng.load_scan_column_f64(1, v.data(), N);
        assert(eng.pin_device(1) && "f64 column must pin to VRAM");
        std::printf("GPU-3 f64 execute_query (DEVICE-resident, half-integers) vs matrix_cpu_reduce*_f64:\n");
        for (int filt = 0; filt < 2; ++filt) for (int k = 0; k < 4; ++k) {
            MatrixQuery q; q.value_col = 1; q.agg = OPS[k];
            q.has_filter = (filt == 1); q.cmp = MatrixCmp::GT; q.lo_f64 = 0.0; q.hi_f64 = 0.0;
            std::vector<uint64_t> out;
            const MatrixQueryStatus st = eng.execute_query(q, out);
            assert(st == MatrixQueryStatus::OK && out.size() == 1);
            const double gpu = matrix_bit_cast<double>(out[0]);
            const double cpu = q.has_filter
                ? matrix_cpu_reduce_pred_f64(v.data(), N, MatrixPredicateF64{MatrixCmp::GT, 0.0, 0.0}, OPS[k])
                : matrix_cpu_reduce_all_f64(v.data(), N, OPS[k]);
            std::printf("  %-9s %-5s GPU=%g CPU=%g  %s\n", filt ? "GT0" : "all", ON[k],
                        gpu, cpu, gpu == cpu ? "OK" : "*** MISMATCH ***");
            assert(gpu == cpu && "f64 execute_query DEVICE path == matrix_cpu_reduce*_f64");
        }
    }

    std::printf("ALL GPU-CATALOG TESTS PASSED\n");
    return 0;
}

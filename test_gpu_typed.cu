// GPU-5 typed cross-check (Colab/nvcc only): the GPU int64 + double predicate reductions must equal
// matrix_cpu_reduce_pred_i64 / _f64 over the SAME bytes, for several predicates x {COUNT,SUM,MIN,MAX}.
// Merge gate for GPU-5. Build (Colab T4):
//   nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread -DMATRIX_USE_CUDA test_gpu_typed.cu -o test_gpu_typed && ./test_gpu_typed
//
// NOTE on double SUM: floating-point addition is order-dependent and GPU atomics accumulate in
// nondeterministic order, so GPU==CPU is only bit-exact when every partial sum is exact. The dataset uses
// HALF-INTEGER values (k*0.5) with a bounded integer running sum (< 2^53), so all partial sums are exact
// and the cross-check holds regardless of accumulation order. int64 is exact integer arithmetic (no issue).
#include "compute_cuda.cuh"   // matrix_{count,sum,min,max}_kernel_pred_{i64,f64} + matrix_cpu_reduce_pred_{i64,f64} + CUDA_CHECK
#include <cstdio>
#include <cassert>
#include <cstdint>
#include <limits>
#include <vector>

static long long gpu_pred_i64(const long long* d, size_t n, MatrixCmp c, long long a, long long b, MatrixAggOp op) {
    long long* d_out = nullptr; CUDA_CHECK(cudaMalloc(&d_out, sizeof(long long)));
    long long init = (op == AGG_MIN) ? INT64_MAX : (op == AGG_MAX ? INT64_MIN : 0);
    CUDA_CHECK(cudaMemcpy(d_out, &init, sizeof(long long), cudaMemcpyHostToDevice));   // sentinels aren't memset-able
    constexpr int TPB = 256, BLOCKS = 1024;
    if (op == AGG_SUM)      matrix_sum_kernel_pred_i64<<<BLOCKS, TPB>>>(d, n, c, a, b, d_out);
    else if (op == AGG_MIN) matrix_min_kernel_pred_i64<<<BLOCKS, TPB>>>(d, n, c, a, b, d_out);
    else if (op == AGG_MAX) matrix_max_kernel_pred_i64<<<BLOCKS, TPB>>>(d, n, c, a, b, d_out);
    else                    matrix_count_kernel_pred_i64<<<BLOCKS, TPB>>>(d, n, c, a, b, d_out);
    CUDA_CHECK(cudaGetLastError());
    long long h = 0; CUDA_CHECK(cudaMemcpy(&h, d_out, sizeof(long long), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaFree(d_out));
    return h;
}
static double gpu_pred_f64(const double* d, size_t n, MatrixCmp c, double a, double b, MatrixAggOp op) {
    double* d_out = nullptr; CUDA_CHECK(cudaMalloc(&d_out, sizeof(double)));
    const double inf = std::numeric_limits<double>::infinity();
    double init = (op == AGG_MIN) ? inf : (op == AGG_MAX ? -inf : 0.0);
    CUDA_CHECK(cudaMemcpy(d_out, &init, sizeof(double), cudaMemcpyHostToDevice));
    constexpr int TPB = 256, BLOCKS = 1024;
    if (op == AGG_SUM)      matrix_sum_kernel_pred_f64<<<BLOCKS, TPB>>>(d, n, c, a, b, d_out);
    else if (op == AGG_MIN) matrix_min_kernel_pred_f64<<<BLOCKS, TPB>>>(d, n, c, a, b, d_out);
    else if (op == AGG_MAX) matrix_max_kernel_pred_f64<<<BLOCKS, TPB>>>(d, n, c, a, b, d_out);
    else                    matrix_count_kernel_pred_f64<<<BLOCKS, TPB>>>(d, n, c, a, b, d_out);
    CUDA_CHECK(cudaGetLastError());
    double h = 0; CUDA_CHECK(cudaMemcpy(&h, d_out, sizeof(double), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaFree(d_out));
    return h;
}

int main() {
    const size_t N = 1u << 20;
    const MatrixAggOp ops[] = {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX};
    const char* on[] = {"COUNT", "SUM", "MIN", "MAX"};

    // --- int64: negatives + values beyond UINT32_MAX (exact integer arithmetic) ---
    std::vector<int64_t> vi(N);
    for (size_t i = 0; i < N; ++i) vi[i] = ((int64_t)(i % 4000) - 2000) * 3000000LL;   // range ~[-6e9, 6e9], incl. >2^32
    int64_t* di = nullptr; CUDA_CHECK(cudaMalloc(&di, N * sizeof(int64_t)));
    CUDA_CHECK(cudaMemcpy(di, vi.data(), N * sizeof(int64_t), cudaMemcpyHostToDevice));
    struct Ci { MatrixCmp c; long long a, b; const char* nm; };
    const Ci ci[] = { {MatrixCmp::GT, 0, 0, "GT0"}, {MatrixCmp::LT, 0, 0, "LT0"},
                      {MatrixCmp::GE, 3000000000LL, 0, "GE3e9"}, {MatrixCmp::EQ, 0, 0, "EQ0"},
                      {MatrixCmp::BETWEEN, -1000000000LL, 1000000000LL, "BETW"}, {MatrixCmp::GT, 999999999999LL, 0, "empty"} };
    std::printf("GPU-5 int64 cross-check (N=%zu) vs matrix_cpu_reduce_pred_i64:\n", N);
    for (const auto& cs : ci) for (int k = 0; k < 4; ++k) {
        const long long g = gpu_pred_i64((const long long*)di, N, cs.c, cs.a, cs.b, ops[k]);
        const long long cpu = matrix_cpu_reduce_pred_i64(vi.data(), N, MatrixPredicateI64{cs.c, cs.a, cs.b}, ops[k]);
        std::printf("  %-6s %-5s GPU=%lld CPU=%lld  %s\n", cs.nm, on[k], g, cpu, g == cpu ? "OK" : "*** MISMATCH ***");
        assert(g == cpu && "GPU int64 predicate aggregate == matrix_cpu_reduce_pred_i64");
    }
    CUDA_CHECK(cudaFree(di));

    // --- double: half-integers (exact partial sums -> order-independent SUM) incl. negatives + fractions ---
    std::vector<double> vf(N);
    for (size_t i = 0; i < N; ++i) vf[i] = ((double)((int64_t)(i % 2000) - 1000)) * 0.5;   // [-500, 499.5] in 0.5 steps
    double* df = nullptr; CUDA_CHECK(cudaMalloc(&df, N * sizeof(double)));
    CUDA_CHECK(cudaMemcpy(df, vf.data(), N * sizeof(double), cudaMemcpyHostToDevice));
    struct Cf { MatrixCmp c; double a, b; const char* nm; };
    const Cf cf[] = { {MatrixCmp::GT, 0.0, 0, "GT0"}, {MatrixCmp::LT, 0.0, 0, "LT0"},
                      {MatrixCmp::LE, -100.0, 0, "LE-100"}, {MatrixCmp::EQ, 0.0, 0, "EQ0"},
                      {MatrixCmp::BETWEEN, -50.0, 50.0, "BETW"}, {MatrixCmp::GT, 1e9, 0, "empty"} };
    std::printf("GPU-5 double cross-check (N=%zu, half-integers) vs matrix_cpu_reduce_pred_f64:\n", N);
    for (const auto& cs : cf) for (int k = 0; k < 4; ++k) {
        const double g = gpu_pred_f64(df, N, cs.c, cs.a, cs.b, ops[k]);
        const double cpu = matrix_cpu_reduce_pred_f64(vf.data(), N, MatrixPredicateF64{cs.c, cs.a, cs.b}, ops[k]);
        std::printf("  %-6s %-5s GPU=%g CPU=%g  %s\n", cs.nm, on[k], g, cpu, g == cpu ? "OK" : "*** MISMATCH ***");
        assert(g == cpu && "GPU double predicate aggregate == matrix_cpu_reduce_pred_f64");
    }
    CUDA_CHECK(cudaFree(df));

    std::printf("ALL GPU-TYPED TESTS PASSED\n");
    return 0;
}

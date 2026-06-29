// GPU-4 predicate cross-check (Colab/nvcc only): the GPU predicate-filtered reductions must equal
// matrix_cpu_reduce_pred over the SAME bytes, for every MatrixCmp op x {COUNT,SUM,MIN,MAX} + an empty match.
// Merge gate for GPU-4. Build (Colab T4):
//   nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread -DMATRIX_USE_CUDA test_gpu_pred.cu -o test_gpu_pred && ./test_gpu_pred
#include "compute_cuda.cuh"   // matrix_{count,sum,min,max}_kernel_pred_u32 + matrix_cpu_reduce_pred + CUDA_CHECK
#include <cstdio>
#include <cassert>
#include <cstdint>
#include <vector>

static unsigned long long gpu_pred(const uint32_t* d, size_t n, MatrixCmp c, uint32_t a, uint32_t b, MatrixAggOp op) {
    unsigned long long* d_out = nullptr;
    CUDA_CHECK(cudaMalloc(&d_out, sizeof(unsigned long long)));
    constexpr int TPB = 256, BLOCKS = 1024;
    if (op == AGG_SUM) {
        CUDA_CHECK(cudaMemset(d_out, 0x00, sizeof(unsigned long long)));
        matrix_sum_kernel_pred_u32<<<BLOCKS, TPB>>>(d, n, c, a, b, d_out);
    } else if (op == AGG_MIN) {
        CUDA_CHECK(cudaMemset(d_out, 0xFF, sizeof(unsigned long long)));   // UINT64_MAX
        matrix_min_kernel_pred_u32<<<BLOCKS, TPB>>>(d, n, c, a, b, d_out);
    } else if (op == AGG_MAX) {
        CUDA_CHECK(cudaMemset(d_out, 0x00, sizeof(unsigned long long)));
        matrix_max_kernel_pred_u32<<<BLOCKS, TPB>>>(d, n, c, a, b, d_out);
    } else {
        CUDA_CHECK(cudaMemset(d_out, 0x00, sizeof(unsigned long long)));
        matrix_count_kernel_pred_u32<<<BLOCKS, TPB>>>(d, n, c, a, b, d_out);
    }
    CUDA_CHECK(cudaGetLastError());
    unsigned long long h = 0;
    CUDA_CHECK(cudaMemcpy(&h, d_out, sizeof(h), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaFree(d_out));
    return h;
}

int main() {
    const size_t N = 1u << 20;
    std::vector<uint32_t> v(N);
    for (size_t i = 0; i < N; ++i) v[i] = static_cast<uint32_t>(i % 1000);   // values 0..999, predicates select known subsets
    uint32_t* d = nullptr;
    CUDA_CHECK(cudaMalloc(&d, N * sizeof(uint32_t)));
    CUDA_CHECK(cudaMemcpy(d, v.data(), N * sizeof(uint32_t), cudaMemcpyHostToDevice));

    struct Case { MatrixCmp c; uint32_t a, b; const char* nm; };
    const Case cases[] = {
        {MatrixCmp::GT, 500, 0, "GT500"}, {MatrixCmp::GE, 500, 0, "GE500"},
        {MatrixCmp::LT, 500, 0, "LT500"}, {MatrixCmp::LE, 500, 0, "LE500"},
        {MatrixCmp::EQ, 42, 0, "EQ42"},   {MatrixCmp::NE, 42, 0, "NE42"},
        {MatrixCmp::BETWEEN, 200, 800, "BETW200_800"}, {MatrixCmp::GT, 99999, 0, "empty"} };
    const MatrixAggOp ops[] = {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX};
    const char* on[] = {"COUNT", "SUM", "MIN", "MAX"};

    std::printf("GPU-4 predicate cross-check (N=%zu) — GPU kernels vs matrix_cpu_reduce_pred:\n", N);
    for (const auto& cs : cases)
        for (int i = 0; i < 4; ++i) {
            const unsigned long long g = gpu_pred(d, N, cs.c, cs.a, cs.b, ops[i]);
            const unsigned long long cpu = static_cast<unsigned long long>(
                matrix_cpu_reduce_pred(v.data(), N, MatrixPredicate{cs.c, cs.a, cs.b}, ops[i]));
            std::printf("  %-11s %-5s GPU=%llu CPU=%llu  %s\n", cs.nm, on[i], g, cpu, g == cpu ? "OK" : "*** MISMATCH ***");
            assert(g == cpu && "GPU predicate aggregate must equal matrix_cpu_reduce_pred");
        }
    CUDA_CHECK(cudaFree(d));
    std::printf("ALL GPU-PRED TESTS PASSED\n");
    return 0;
}

// GPU AGG-2 cross-backend merge gate (Colab/nvcc only): the GPU SUM/MIN/MAX/COUNT reduction kernels must
// equal matrix_cpu_reduce over the SAME bytes — the correctness anchor for the whole GPU phase. Tests a
// matching predicate and an empty-match predicate (the sentinels). Merge GPU-1 to main only when green.
// Build (Colab T4):
//   nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread -DMATRIX_USE_CUDA test_gpu_agg.cu -o test_gpu_agg && ./test_gpu_agg
#include "compute_cuda.cuh"   // matrix_{sum,min,max}_kernel_u32 + matrix_scan_kernel_u32x4 + matrix_cpu_reduce + CUDA_CHECK
#include <cstdio>
#include <cassert>
#include <cstdint>
#include <vector>

// Run one aggregate on the GPU over the device column `d` (n values), return the uint64 result. Inits the
// device accumulator to the same empty-sentinel the CPU reducer uses (SUM/COUNT/MAX -> 0; MIN -> all-ones).
static unsigned long long gpu_agg(const uint32_t* d, size_t n, uint32_t threshold, MatrixAggOp op) {
    unsigned long long* d_out = nullptr;
    CUDA_CHECK(cudaMalloc(&d_out, sizeof(unsigned long long)));
    constexpr int TPB = 256, BLOCKS = 1024;
    if (op == AGG_SUM) {
        CUDA_CHECK(cudaMemset(d_out, 0x00, sizeof(unsigned long long)));
        matrix_sum_kernel_u32<<<BLOCKS, TPB>>>(d, n, threshold, d_out);
    } else if (op == AGG_MIN) {
        CUDA_CHECK(cudaMemset(d_out, 0xFF, sizeof(unsigned long long)));   // UINT64_MAX
        matrix_min_kernel_u32<<<BLOCKS, TPB>>>(d, n, threshold, d_out);
    } else if (op == AGG_MAX) {
        CUDA_CHECK(cudaMemset(d_out, 0x00, sizeof(unsigned long long)));
        matrix_max_kernel_u32<<<BLOCKS, TPB>>>(d, n, threshold, d_out);
    } else {                                                              // AGG_COUNT (u32x4, the oracle path)
        CUDA_CHECK(cudaMemset(d_out, 0x00, sizeof(unsigned long long)));
        const uint4* d4 = reinterpret_cast<const uint4*>(d);
        matrix_scan_kernel_u32x4<<<BLOCKS, TPB>>>(d4, n / 4, threshold, d_out);
    }
    CUDA_CHECK(cudaGetLastError());
    unsigned long long h = 0;
    CUDA_CHECK(cudaMemcpy(&h, d_out, sizeof(h), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaFree(d_out));
    return h;
}

static void check_all_ops(const uint32_t* host, const uint32_t* d, size_t n, uint32_t threshold) {
    const MatrixAggOp ops[] = {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX};
    const char* names[]     = {"COUNT", "SUM", "MIN", "MAX"};
    for (int i = 0; i < 4; ++i) {
        const unsigned long long gpu = gpu_agg(d, n, threshold, ops[i]);
        const unsigned long long cpu = static_cast<unsigned long long>(matrix_cpu_reduce(host, n, threshold, ops[i]));
        std::printf("  thr=%-10u %-5s GPU=%llu CPU=%llu  %s\n", threshold, names[i], gpu, cpu,
                    gpu == cpu ? "OK" : "*** MISMATCH ***");
        assert(gpu == cpu && "GPU aggregate must equal matrix_cpu_reduce over the same bytes");
    }
}

int main() {
    const size_t N = 1u << 20;                       // 1,048,576 values (divisible by 4 for the u32x4 COUNT)
    std::vector<uint32_t> vals(N);
    for (size_t i = 0; i < N; ++i) vals[i] = static_cast<uint32_t>(i);
    uint32_t* d = nullptr;
    CUDA_CHECK(cudaMalloc(&d, N * sizeof(uint32_t)));
    CUDA_CHECK(cudaMemcpy(d, vals.data(), N * sizeof(uint32_t), cudaMemcpyHostToDevice));

    std::printf("GPU AGG-2 cross-backend check (N=%zu) — GPU kernels vs matrix_cpu_reduce:\n", N);
    check_all_ops(vals.data(), d, N, N / 2);          // ~half the rows match value > threshold
    check_all_ops(vals.data(), d, N, 0u);             // all but value 0 match
    check_all_ops(vals.data(), d, N, 0xFFFFFFFFu);    // empty match -> exercises the empty sentinels

    CUDA_CHECK(cudaFree(d));
    std::printf("ALL GPU-AGG TESTS PASSED\n");
    return 0;
}

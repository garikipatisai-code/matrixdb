// GPU-2 grouped cross-check (Colab/nvcc only): the GPU per-group reduction must equal
// matrix_cpu_group_reduce over the SAME keys+values, for {COUNT,SUM,MIN,MAX}. The dataset includes
// out-of-range keys (>= num_groups) to verify both backends ignore them. Merge gate for GPU-2. Build:
//   nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread -DMATRIX_USE_CUDA test_gpu_grouped.cu -o test_gpu_grouped && ./test_gpu_grouped
#include "compute_cuda.cuh"   // matrix_group_{count,sum,min,max}_kernel + matrix_cpu_group_reduce + CUDA_CHECK
#include <cstdio>
#include <cassert>
#include <cstdint>
#include <vector>

static std::vector<unsigned long long> gpu_group(const uint32_t* dk, const uint32_t* dv, size_t n,
                                                 uint32_t G, MatrixAggOp op) {
    unsigned long long* d_out = nullptr;
    CUDA_CHECK(cudaMalloc(&d_out, G * sizeof(unsigned long long)));
    // Per-op sentinel init (matches matrix_cpu_group_reduce): MIN -> all-ones (UINT64_MAX), else 0.
    CUDA_CHECK(cudaMemset(d_out, (op == AGG_MIN) ? 0xFF : 0x00, G * sizeof(unsigned long long)));
    constexpr int TPB = 256, BLOCKS = 1024;
    if (op == AGG_SUM)      matrix_group_sum_kernel<<<BLOCKS, TPB>>>(dk, dv, n, G, d_out);
    else if (op == AGG_MIN) matrix_group_min_kernel<<<BLOCKS, TPB>>>(dk, dv, n, G, d_out);
    else if (op == AGG_MAX) matrix_group_max_kernel<<<BLOCKS, TPB>>>(dk, dv, n, G, d_out);
    else                    matrix_group_count_kernel<<<BLOCKS, TPB>>>(dk, n, G, d_out);
    CUDA_CHECK(cudaGetLastError());
    std::vector<unsigned long long> h(G);
    CUDA_CHECK(cudaMemcpy(h.data(), d_out, G * sizeof(unsigned long long), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaFree(d_out));
    return h;
}

int main() {
    const size_t N = 1u << 20;
    const uint32_t G = 8;
    std::vector<uint32_t> keys(N), vals(N);
    for (size_t i = 0; i < N; ++i) {
        keys[i] = static_cast<uint32_t>(i % (G + 2));   // 0..G+1 -> keys G and G+1 are out-of-range (must be ignored)
        vals[i] = static_cast<uint32_t>(i % 1000);
    }
    uint32_t *dk = nullptr, *dv = nullptr;
    CUDA_CHECK(cudaMalloc(&dk, N * sizeof(uint32_t)));
    CUDA_CHECK(cudaMalloc(&dv, N * sizeof(uint32_t)));
    CUDA_CHECK(cudaMemcpy(dk, keys.data(), N * sizeof(uint32_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(dv, vals.data(), N * sizeof(uint32_t), cudaMemcpyHostToDevice));

    const MatrixAggOp ops[] = {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX};
    const char* on[] = {"COUNT", "SUM", "MIN", "MAX"};
    std::printf("GPU-2 grouped cross-check (N=%zu, G=%u, keys incl. out-of-range) — GPU vs matrix_cpu_group_reduce:\n", N, G);
    for (int i = 0; i < 4; ++i) {
        const std::vector<unsigned long long> g = gpu_group(dk, dv, N, G, ops[i]);
        std::vector<uint64_t> cpu(G);
        matrix_cpu_group_reduce(keys.data(), vals.data(), N, G, ops[i], cpu.data());
        bool ok = true;
        for (uint32_t k = 0; k < G; ++k) if (g[k] != static_cast<unsigned long long>(cpu[k])) ok = false;
        std::printf("  %-5s match=%s  (g0 GPU=%llu CPU=%llu)\n", on[i], ok ? "OK" : "*** MISMATCH ***",
                    g[0], static_cast<unsigned long long>(cpu[0]));
        for (uint32_t k = 0; k < G; ++k)
            assert(g[k] == static_cast<unsigned long long>(cpu[k]) && "GPU grouped == matrix_cpu_group_reduce");
    }
    CUDA_CHECK(cudaFree(dk)); CUDA_CHECK(cudaFree(dv));
    std::printf("ALL GPU-GROUPED TESTS PASSED\n");
    return 0;
}

// bench_gpu_roofline.cu — does the GPU engine actually earn its keep? Measures, on THIS card:
//   (1) achieved DRAM bandwidth of a resident scan vs the card's theoretical peak (% of roofline),
//   (2) the branch cost of a filtered scan (a low-selectivity predicate),
//   (3) the KERNEL-FUSION lever: sum+count as two kernels (2 column reads) vs one fused kernel (1 read).
// Standalone (no engine deps) to keep it robust. Colab (T4):
//   nvcc -std=c++17 -O3 bench_gpu_roofline.cu -o roof && ./roof
#include <cstdio>
#include <cstdint>
#include <cuda_runtime.h>

#define CK(x) do { cudaError_t e_ = (x); if (e_) { std::printf("CUDA error at %d: %s\n", __LINE__, cudaGetErrorString(e_)); return 1; } } while (0)

__global__ void k_fill(uint32_t* d, size_t n) {
    for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x)
        d[i] = (uint32_t)(i * 2654435761u);                 // hashed -> ~uniform, ~50% pass a mid threshold
}
__global__ void k_sum(const uint32_t* __restrict__ d, size_t n, unsigned long long* out) {
    unsigned long long s = 0;
    for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x) s += d[i];
    atomicAdd(out, s);                                       // bandwidth-bound: atomic contention negligible vs DRAM traffic
}
__global__ void k_count(const uint32_t* __restrict__ d, size_t n, uint32_t thr, unsigned long long* out) {
    unsigned long long c = 0;
    for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x)
        if (d[i] > thr) c++;                                 // data-dependent branch (predicate)
    atomicAdd(out, c);
}
__global__ void k_sum_count(const uint32_t* __restrict__ d, size_t n, uint32_t thr,
                            unsigned long long* s_out, unsigned long long* c_out) {
    unsigned long long s = 0, c = 0;
    for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x) {
        uint32_t v = d[i]; s += v; if (v > thr) c++;         // BOTH aggregates from ONE read (fusion)
    }
    atomicAdd(s_out, s); atomicAdd(c_out, c);
}

template <class F> static float time_ms(F run, int iters) {
    cudaEvent_t a, b; cudaEventCreate(&a); cudaEventCreate(&b);
    run(); cudaDeviceSynchronize();                          // warm up
    cudaEventRecord(a); for (int i = 0; i < iters; ++i) run(); cudaEventRecord(b);
    cudaEventSynchronize(b); float ms = 0; cudaEventElapsedTime(&ms, a, b);
    cudaEventDestroy(a); cudaEventDestroy(b); return ms / iters;
}

int main() {
    cudaDeviceProp p; CK(cudaGetDeviceProperties(&p, 0));
    const double peak = 2.0 * (double)p.memoryClockRate * 1e3 * (p.memoryBusWidth / 8) / 1e9;   // GB/s (DDR = x2)
    std::printf("GPU: %s | theoretical peak DRAM ~ %.0f GB/s\n\n", p.name, peak);

    const size_t N = (size_t)256 * 1024 * 1024 / 4;         // 256 MB of u32 = 64M elements (well past cache)
    const double bytes = (double)N * 4;
    uint32_t* d; CK(cudaMalloc(&d, N * 4));
    unsigned long long *s, *c; CK(cudaMalloc(&s, 8)); CK(cudaMalloc(&c, 8));
    const int TPB = 256, BLK = 4096;
    k_fill<<<BLK, TPB>>>(d, N); CK(cudaGetLastError()); CK(cudaDeviceSynchronize());
    const uint32_t thr = 0x80000000u;                        // ~50% selectivity (worst case for branch prediction)
    const int IT = 50;

    const float t_sum = time_ms([&]{ cudaMemsetAsync(s, 0, 8); k_sum<<<BLK, TPB>>>(d, N, s); }, IT);
    const double gbps = bytes / 1e9 / (t_sum / 1e3);
    std::printf("scan SUM (1 read)          : %.3f ms  ->  %6.0f GB/s   (%.0f%% of peak)\n", t_sum, gbps, 100 * gbps / peak);

    const float t_cnt = time_ms([&]{ cudaMemsetAsync(c, 0, 8); k_count<<<BLK, TPB>>>(d, N, thr, c); }, IT);
    std::printf("filtered COUNT (branchy)   : %.3f ms  ->  %6.0f GB/s   (predicate branch cost vs pure SUM)\n",
                t_cnt, bytes / 1e9 / (t_cnt / 1e3));

    const float t_two   = time_ms([&]{ cudaMemsetAsync(s,0,8); cudaMemsetAsync(c,0,8);
                                       k_sum<<<BLK,TPB>>>(d,N,s); k_count<<<BLK,TPB>>>(d,N,thr,c); }, IT);
    const float t_fused = time_ms([&]{ cudaMemsetAsync(s,0,8); cudaMemsetAsync(c,0,8);
                                       k_sum_count<<<BLK,TPB>>>(d,N,thr,s,c); }, IT);
    std::printf("\nFUSION LEVER (sum + count):\n");
    std::printf("  2 kernels, 2 column reads: %.3f ms\n", t_two);
    std::printf("  1 fused kernel, 1 read   : %.3f ms   ->  %.2fx faster (bandwidth halved)\n", t_fused, t_two / t_fused);
    std::printf("\nReading: SUM near 100%% of peak = the kernel is optimal; the fusion speedup is the perf left on the\n");
    std::printf("table by running separate filter/aggregate kernels (MatrixDB's current shape). End-to-end through the\n");
    std::printf("engine will be lower than this standalone ceiling — that gap is launch/copy/sync overhead.\n");

    cudaFree(d); cudaFree(s); cudaFree(c);
    return 0;
}

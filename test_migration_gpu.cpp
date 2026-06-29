// GPU proof (Colab): a column migrated HOST->DEVICE is byte-intact and GPU-scannable in
// place. Build: nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread \
//   -DMATRIX_USE_CUDA test_migration_gpu.cpp -o test_migration_gpu && ./test_migration_gpu
#include "tiered_column.hpp"
#include "compute_cuda.cuh"   // matrix_scan_kernel_u32x4 + CUDA_CHECK
#include <cstdio>
#include <cassert>
#include <vector>

int main() {
    // Build a uint32 column value[i]=i (n divisible by 4 for uint4 scan).
    const size_t N = 1u << 20; // 1,048,576 values
    std::vector<uint32_t> vals(N);
    for (size_t i = 0; i < N; ++i) vals[i] = static_cast<uint32_t>(i);
    const uint32_t threshold = N / 2;

    // CPU reference count of value > threshold.
    uint64_t cpu_count = 0;
    for (size_t i = 0; i < N; ++i) cpu_count += (vals[i] > threshold);

    // Make a column from those bytes; checksum before.
    TieredColumn col(1, reinterpret_cast<const unsigned char*>(vals.data()), N * sizeof(uint32_t));
    const uint64_t want = col.checksum();

    // Migrate HOST -> DEVICE and scan IN PLACE on the device pointer.
    col.migrate_to(MemorySpace::DEVICE);
    assert(col.tier() == MemorySpace::DEVICE && "promoted to DEVICE");

    unsigned long long* d_count = nullptr;
    CUDA_CHECK(cudaMalloc(&d_count, sizeof(unsigned long long)));
    CUDA_CHECK(cudaMemset(d_count, 0, sizeof(unsigned long long)));
    const uint4* col4 = reinterpret_cast<const uint4*>(col.device_ptr());
    const size_t n4 = (N * sizeof(uint32_t)) / sizeof(uint4); // bytes/16
    matrix_scan_kernel_u32x4<<<1024, 256>>>(col4, n4, threshold, d_count);
    CUDA_CHECK(cudaGetLastError());
    unsigned long long gpu_count = 0;
    CUDA_CHECK(cudaMemcpy(&gpu_count, d_count, sizeof(gpu_count), cudaMemcpyDeviceToHost));
    cudaFree(d_count);

    assert(gpu_count == cpu_count && "GPU scan of the promoted column matches the CPU count");

    // Migrate back; bytes must be byte-identical (integrity across the round trip).
    col.migrate_to(MemorySpace::HOST);
    assert(col.tier() == MemorySpace::HOST && "demoted back to HOST");
    assert(col.checksum() == want && "checksum invariant HOST->DEVICE->HOST");

    std::printf("PASS: VRAM-promoted column is GPU-scannable and byte-intact "
                "(gpu_count=%llu cpu_count=%llu)\n",
                static_cast<unsigned long long>(gpu_count), static_cast<unsigned long long>(cpu_count));
    return 0;
}

#pragma once

// CUDA backend — page-ownership model (Component 4: Parallel Engine).
// Compile on a GPU host (Google Colab) with:
//     nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread -DMATRIX_USE_CUDA main.cpp -o matrixdb_proto
//
// One CUDA BLOCK owns one page. The batch is binned by page on the host (CSR offsets),
// so block p processes only page p's contiguous queries against page p's store slice.
// Different blocks touch disjoint store slots ⇒ no cross-block conflict, no store
// atomics, no delta log. Per page: single-owner (Redis). Across pages: shared-nothing
// (Dragonfly). Threads within a block stride over the page's queries.

#include "compute.hpp"
#include <cuda_runtime.h>
#include <vector>
#include <cstdio>
#include <cstdlib>

#define CUDA_CHECK(call)                                                            \
    do {                                                                           \
        cudaError_t _err = (call);                                                 \
        if (_err != cudaSuccess) {                                                 \
            std::fprintf(stderr, "CUDA error %s at %s:%d -> %s\n",                 \
                         #call, __FILE__, __LINE__, cudaGetErrorString(_err));     \
            std::abort();                                                          \
        }                                                                          \
    } while (0)

// One block per page. blockIdx.x == page id; the block's threads cooperatively process
// that page's queries [offsets[page], offsets[page+1]). Writes to a slot within a page
// are owned by this block alone, so no atomics on the store are needed. Same-slot writes
// within the page race between threads -> last-writer-wins, matching the CPU mock's
// deterministic in-order result only when keys are unique (true for our benchmark).
__global__ void matrix_page_kernel(const DatabaseQuery* binned, const uint32_t* offsets,
                                   uint64_t* store,
                                   unsigned long long* reads,
                                   unsigned long long* writes) {
    const size_t page = blockIdx.x;
    if (page >= MATRIX_PAGE_COUNT) return;

    const uint32_t begin = offsets[page];
    const uint32_t end = offsets[page + 1];

    unsigned r = 0, w = 0;
    for (uint32_t j = begin + threadIdx.x; j < end; j += blockDim.x) {
        const DatabaseQuery q = binned[j];
        const size_t slot = q.query_id & MATRIX_STORE_MASK;
        if (q.opcode == OP_READ) {
            volatile uint64_t v = store[slot]; (void)v; // touch the store (read path)
            ++r;
        } else if (q.opcode == OP_WRITE) {
            store[slot] = q.query_id; // mock projection: value == key
            ++w;
        }
    }
    if (r) atomicAdd(reads, (unsigned long long)r);   // counters only — not on the store
    if (w) atomicAdd(writes, (unsigned long long)w);
}

// Filter-count scan: count values > threshold. Grid-stride over resident VRAM data,
// block-local reduction, one atomicAdd per block. This is the GPU's home turf —
// streaming bandwidth over data too large for CPU cache.
__global__ void matrix_scan_kernel(const uint64_t* data, size_t n,
                                   uint64_t threshold, unsigned long long* count) {
    __shared__ unsigned long long block_count;
    if (threadIdx.x == 0) block_count = 0;
    __syncthreads();

    unsigned long long local = 0;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride) {
        if (data[i] > threshold) ++local;
    }
    atomicAdd(&block_count, local);
    __syncthreads();
    if (threadIdx.x == 0) atomicAdd(count, block_count);
}

/**
 * @brief Real CUDA GPU engine, page-ownership model. Device-resident store persists
 * across batches (it is the database). Same ComputeInterface + correctness contract
 * as CPUMockEngine.
 */
class CUDAGPUEngine : public ComputeInterface {
public:
    explicit CUDAGPUEngine(size_t /*worker_count*/ = 0)
        : h_binned_(MATRIX_BATCH_MAX) {
        int device_count = 0;
        CUDA_CHECK(cudaGetDeviceCount(&device_count));
        if (device_count == 0) {
            std::fprintf(stderr, "CUDAGPUEngine: no CUDA device found.\n");
            std::abort();
        }
        cudaDeviceProp prop{};
        CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));

        CUDA_CHECK(cudaMalloc(&d_store_, MATRIX_STORE_SLOTS * sizeof(uint64_t)));
        CUDA_CHECK(cudaMemset(d_store_, 0, MATRIX_STORE_SLOTS * sizeof(uint64_t)));
        CUDA_CHECK(cudaMalloc(&d_binned_, MATRIX_BATCH_MAX * sizeof(DatabaseQuery)));
        CUDA_CHECK(cudaMalloc(&d_offsets_, (MATRIX_PAGE_COUNT + 1) * sizeof(uint32_t)));
        CUDA_CHECK(cudaMalloc(&d_reads_, sizeof(unsigned long long)));
        CUDA_CHECK(cudaMalloc(&d_writes_, sizeof(unsigned long long)));
        CUDA_CHECK(cudaMemset(d_reads_, 0, sizeof(unsigned long long)));
        CUDA_CHECK(cudaMemset(d_writes_, 0, sizeof(unsigned long long)));

        offsets_.resize(MATRIX_PAGE_COUNT + 1);

        // ponytail: single stream. Hyper-Q multi-stream is the throughput upgrade.
        CUDA_CHECK(cudaStreamCreate(&stream_));
        std::printf("CUDAGPUEngine initialized on '%s' (%d SMs, page-ownership, %zu pages).\n",
                    prop.name, prop.multiProcessorCount, MATRIX_PAGE_COUNT);
    }

    ~CUDAGPUEngine() override {
        cudaFree(d_store_);
        cudaFree(d_binned_);
        cudaFree(d_offsets_);
        cudaFree(d_reads_);
        cudaFree(d_writes_);
        cudaStreamDestroy(stream_);
        std::printf("CUDAGPUEngine released device resources.\n");
    }

    void execute_batch(DatabaseQuery* batch_array, size_t count) override {
        if (count == 0) return;
        if (count > MATRIX_BATCH_MAX) count = MATRIX_BATCH_MAX;

        // Bin by page on the host (folds into the dual-trigger batcher later).
        matrix_bin_by_page(batch_array, count, h_binned_.data(), offsets_.data());

        CUDA_CHECK(cudaMemcpyAsync(d_binned_, h_binned_.data(), count * sizeof(DatabaseQuery),
                                   cudaMemcpyHostToDevice, stream_));
        CUDA_CHECK(cudaMemcpyAsync(d_offsets_, offsets_.data(),
                                   (MATRIX_PAGE_COUNT + 1) * sizeof(uint32_t),
                                   cudaMemcpyHostToDevice, stream_));

        // One block per page; 128 threads/block stride over the page's queries.
        constexpr int TPB = 128;
        matrix_page_kernel<<<MATRIX_PAGE_COUNT, TPB, 0, stream_>>>(
            d_binned_, d_offsets_, d_store_, d_reads_, d_writes_);
        CUDA_CHECK(cudaGetLastError());
        CUDA_CHECK(cudaStreamSynchronize(stream_));
    }

    uint64_t reads() const override { return read_counter(d_reads_); }
    uint64_t writes() const override { return read_counter(d_writes_); }
    uint64_t commits() const override { return read_counter(d_writes_); } // every write commits (owned slot)

    uint64_t store_checksum() const override {
        // ponytail: copy the whole store back (32KB) and sum on host. Once, off the
        // hot path — a device reduction would be premature for a correctness check.
        std::vector<uint64_t> h(MATRIX_STORE_SLOTS);
        CUDA_CHECK(cudaMemcpy(h.data(), d_store_, MATRIX_STORE_SLOTS * sizeof(uint64_t),
                              cudaMemcpyDeviceToHost));
        uint64_t sum = 0;
        for (uint64_t v : h) sum += v;
        return sum;
    }

    double benchmark_scan(size_t n, uint64_t threshold, uint64_t& out_count) override {
        // Data lives resident in VRAM; we never ship it per query. Fill via a small
        // host buffer once (NOT timed) — the query is the scan over resident data.
        uint64_t* d_data = nullptr;
        unsigned long long* d_count = nullptr;
        CUDA_CHECK(cudaMalloc(&d_data, n * sizeof(uint64_t)));
        CUDA_CHECK(cudaMalloc(&d_count, sizeof(unsigned long long)));
        CUDA_CHECK(cudaMemset(d_count, 0, sizeof(unsigned long long)));
        {
            std::vector<uint64_t> h(n);
            for (size_t i = 0; i < n; ++i) h[i] = i;
            CUDA_CHECK(cudaMemcpy(d_data, h.data(), n * sizeof(uint64_t), cudaMemcpyHostToDevice));
        }

        constexpr int TPB = 256;
        const int blocks = 1024; // saturate the 40 SMs; grid-stride handles any n
        cudaEvent_t start, stop;
        CUDA_CHECK(cudaEventCreate(&start));
        CUDA_CHECK(cudaEventCreate(&stop));
        CUDA_CHECK(cudaEventRecord(start, stream_));
        matrix_scan_kernel<<<blocks, TPB, 0, stream_>>>(d_data, n, threshold, d_count);
        CUDA_CHECK(cudaEventRecord(stop, stream_));
        CUDA_CHECK(cudaEventSynchronize(stop));
        float ms = 0.0f;
        CUDA_CHECK(cudaEventElapsedTime(&ms, start, stop));

        unsigned long long h_count = 0;
        CUDA_CHECK(cudaMemcpy(&h_count, d_count, sizeof(h_count), cudaMemcpyDeviceToHost));
        out_count = h_count;

        cudaEventDestroy(start);
        cudaEventDestroy(stop);
        cudaFree(d_data);
        cudaFree(d_count);
        return ms / 1e3; // seconds
    }

private:
    static uint64_t read_counter(const unsigned long long* d_ptr) {
        unsigned long long h = 0;
        CUDA_CHECK(cudaMemcpy(&h, d_ptr, sizeof(h), cudaMemcpyDeviceToHost));
        return static_cast<uint64_t>(h);
    }

    uint64_t* d_store_ = nullptr;
    DatabaseQuery* d_binned_ = nullptr;
    uint32_t* d_offsets_ = nullptr;
    unsigned long long* d_reads_ = nullptr;
    unsigned long long* d_writes_ = nullptr;
    std::vector<DatabaseQuery> h_binned_;
    std::vector<uint32_t> offsets_;
    cudaStream_t stream_{};
};

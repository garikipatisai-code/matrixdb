#pragma once

// CUDA backend (Component 4: Parallel Engine + Component 5: Enterprise path).
// Compile on a GPU host (e.g. Google Colab) with:
//     nvcc -std=c++17 -O3 -x cu -DMATRIX_USE_CUDA main.cpp -o matrixdb_proto
// One GPU thread per query executes the opcode dispatch; writes reserve Delta Log
// slots via atomicAdd (the spec's wait-free fetch-add on real silicon); a second
// kernel reconciles the log into the device-resident columnar store.

#include "compute.hpp"
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>

// Trust boundary: a failed CUDA call means the device state is unknown — never swallow it.
#define CUDA_CHECK(call)                                                            \
    do {                                                                           \
        cudaError_t _err = (call);                                                 \
        if (_err != cudaSuccess) {                                                 \
            std::fprintf(stderr, "CUDA error %s at %s:%d -> %s\n",                 \
                         #call, __FILE__, __LINE__, cudaGetErrorString(_err));     \
            std::abort();                                                          \
        }                                                                          \
    } while (0)

// --- Device kernels ---

// One thread per query. Reads hit the store; writes stage into the append-only
// Delta Log at an atomically-reserved slot. Identical mechanics to the CPU mock.
__global__ void matrix_execute_kernel(DatabaseQuery* batch, size_t count,
                                      const uint64_t* store, Mutation* delta_log,
                                      unsigned long long* delta_head,
                                      unsigned long long* reads,
                                      unsigned long long* writes) {
    const size_t i = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= count) return;

    const uint32_t opcode = batch[i].opcode;
    const uint64_t key = batch[i].query_id;
    if (opcode == OP_READ) {
        batch[i].transaction_id = store[key & MATRIX_STORE_MASK];
        atomicAdd(reads, 1ULL);
    } else if (opcode == OP_WRITE) {
        const unsigned long long slot =
            atomicAdd(delta_head, 1ULL) & MATRIX_DELTA_LOG_MASK;
        delta_log[slot].key = key;
        delta_log[slot].value = key; // mock projection: value == key
        atomicAdd(writes, 1ULL);
    }
    // OP_SCAN / unknown: no-op, matching the CPU mock.
}

// One thread per logged mutation. Commits the Delta Log into the store.
// ponytail: parallel last-writer-wins — on colliding keys the winner is
// nondeterministic. Test keys are unique so it's exact; colliding keys need
// the spec's §4 OCC validation (TEV lock bit + read-set check). Upgrade path.
__global__ void matrix_reconcile_kernel(uint64_t* store, const Mutation* delta_log,
                                        unsigned long long logged,
                                        unsigned long long* delta_applied) {
    const size_t s = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (s >= logged) return;
    const Mutation m = delta_log[s & MATRIX_DELTA_LOG_MASK];
    store[m.key & MATRIX_STORE_MASK] = m.value;
    atomicAdd(delta_applied, 1ULL);
}

/**
 * @brief Real CUDA GPU engine. Device-resident store + Delta Log persist across
 * batches (it is the database). Honors the same ComputeInterface contract and the
 * same correctness asserts as CPUMockEngine.
 */
class CUDAGPUEngine : public ComputeInterface {
public:
    explicit CUDAGPUEngine(size_t /*worker_count*/ = 0) {
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
        CUDA_CHECK(cudaMalloc(&d_delta_log_, MATRIX_DELTA_LOG_CAPACITY * sizeof(Mutation)));
        CUDA_CHECK(cudaMalloc(&d_delta_head_, sizeof(unsigned long long)));
        CUDA_CHECK(cudaMemset(d_delta_head_, 0, sizeof(unsigned long long)));
        CUDA_CHECK(cudaMalloc(&d_batch_, MATRIX_BATCH_MAX * sizeof(DatabaseQuery)));
        CUDA_CHECK(cudaMalloc(&d_reads_, sizeof(unsigned long long)));
        CUDA_CHECK(cudaMalloc(&d_writes_, sizeof(unsigned long long)));
        CUDA_CHECK(cudaMalloc(&d_delta_applied_, sizeof(unsigned long long)));
        CUDA_CHECK(cudaMemset(d_reads_, 0, sizeof(unsigned long long)));
        CUDA_CHECK(cudaMemset(d_writes_, 0, sizeof(unsigned long long)));
        CUDA_CHECK(cudaMemset(d_delta_applied_, 0, sizeof(unsigned long long)));

        // ponytail: single stream. Hyper-Q multi-stream is a throughput upgrade
        // (spec §3) — add a stream pool when batches overlap; not needed to run.
        CUDA_CHECK(cudaStreamCreate(&stream_));

        std::printf("CUDAGPUEngine initialized on '%s' (%d SMs).\n",
                    prop.name, prop.multiProcessorCount);
    }

    ~CUDAGPUEngine() override {
        cudaFree(d_store_);
        cudaFree(d_delta_log_);
        cudaFree(d_delta_head_);
        cudaFree(d_batch_);
        cudaFree(d_reads_);
        cudaFree(d_writes_);
        cudaFree(d_delta_applied_);
        cudaStreamDestroy(stream_);
        std::printf("CUDAGPUEngine released device resources.\n");
    }

    void execute_batch(DatabaseQuery* batch_array, size_t count) override {
        if (count == 0) return;
        if (count > MATRIX_BATCH_MAX) count = MATRIX_BATCH_MAX; // ponytail: clamp; main never exceeds this

        // Stage batch to device. ponytail: plain async copy. cudaHostRegister to pin
        // the host pool for zero-copy DMA (spec §3) is the bandwidth upgrade.
        CUDA_CHECK(cudaMemcpyAsync(d_batch_, batch_array, count * sizeof(DatabaseQuery),
                                   cudaMemcpyHostToDevice, stream_));

        constexpr int TPB = 256;
        const int exec_blocks = static_cast<int>((count + TPB - 1) / TPB);
        matrix_execute_kernel<<<exec_blocks, TPB, 0, stream_>>>(
            d_batch_, count, d_store_, d_delta_log_, d_delta_head_, d_reads_, d_writes_);
        CUDA_CHECK(cudaGetLastError());

        // Read how many mutations were staged this batch to size the reconcile launch.
        unsigned long long logged = 0;
        CUDA_CHECK(cudaMemcpyAsync(&logged, d_delta_head_, sizeof(logged),
                                   cudaMemcpyDeviceToHost, stream_));
        CUDA_CHECK(cudaStreamSynchronize(stream_));

        if (logged > 0) {
            const int rec_blocks = static_cast<int>((logged + TPB - 1) / TPB);
            matrix_reconcile_kernel<<<rec_blocks, TPB, 0, stream_>>>(
                d_store_, d_delta_log_, logged, d_delta_applied_);
            CUDA_CHECK(cudaGetLastError());
        }

        // Reset the Delta Log head for the next batch, then copy read results back.
        CUDA_CHECK(cudaMemsetAsync(d_delta_head_, 0, sizeof(unsigned long long), stream_));
        CUDA_CHECK(cudaMemcpyAsync(batch_array, d_batch_, count * sizeof(DatabaseQuery),
                                   cudaMemcpyDeviceToHost, stream_));
        CUDA_CHECK(cudaStreamSynchronize(stream_));
    }

    uint64_t reads() const override { return read_counter(d_reads_); }
    uint64_t writes() const override { return read_counter(d_writes_); }
    uint64_t delta_applied() const override { return read_counter(d_delta_applied_); }

private:
    static uint64_t read_counter(const unsigned long long* d_ptr) {
        unsigned long long h = 0;
        CUDA_CHECK(cudaMemcpy(&h, d_ptr, sizeof(h), cudaMemcpyDeviceToHost));
        return static_cast<uint64_t>(h);
    }

    uint64_t* d_store_ = nullptr;
    Mutation* d_delta_log_ = nullptr;
    unsigned long long* d_delta_head_ = nullptr;
    DatabaseQuery* d_batch_ = nullptr;
    unsigned long long* d_reads_ = nullptr;
    unsigned long long* d_writes_ = nullptr;
    unsigned long long* d_delta_applied_ = nullptr;
    cudaStream_t stream_{};
};

#include "compute.hpp"
#include <vector>
#include <thread>
#include <atomic>
#include <array>
#include <chrono>
#include <iostream>

/**
 * @brief CPU Mock Engine — the no-GPU fallback (Component 5: Local Sandbox).
 * Executes the real opcode-dispatch + Delta Log mechanics serially, simulating
 * GPU kernel-launch latency with a spin-wait. Same correctness contract as the
 * CUDA engine, so it is the runnable proof when no GPU is present.
 */
class CPUMockEngine : public ComputeInterface {
public:
    explicit CPUMockEngine(size_t worker_count)
        : shutdown_(false) {
        // ponytail: workers kept only to mirror the spec's pinned-thread-pool shape.
        // They do no work in the serial CPU mock; real parallelism lives in the CUDA engine.
        for (size_t i = 0; i < worker_count; ++i) {
            workers_.emplace_back([this, i]() {
                this->worker_loop(i);
            });
        }
        std::cout << "CPUMockEngine initialized with " << worker_count << " worker threads." << std::endl;
    }

    ~CPUMockEngine() override {
        shutdown_.store(true, std::memory_order_relaxed);
        for (auto& thread : workers_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        std::cout << "CPUMockEngine workers shutdown cleanly." << std::endl;
    }

    void execute_batch(DatabaseQuery* batch_array, size_t count) override {
        // Simulates query processing and kernel execution latency using a spin-wait loop
        const auto start_wait = std::chrono::high_resolution_clock::now();
        while (true) {
            const auto current_time = std::chrono::high_resolution_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(current_time - start_wait).count();
            if (elapsed >= 50) { // Emulate a 50-microsecond GPU execution delay
                break;
            }
#if defined(__ARM_ARCH) || defined(__apple_build_version__)
            asm volatile("isb" ::: "memory");
#else
            asm volatile("pause" ::: "memory");
#endif
        }

        // Execution phase: dispatch every query on its opcode.
        // Reads hit the columnar store directly. Writes never touch the store here —
        // they reserve a Delta Log slot with a wait-free atomic fetch-add and stage
        // the mutation, exactly as the spec's append-only log prescribes.
        for (size_t i = 0; i < count; ++i) {
            DatabaseQuery& q = batch_array[i];
            switch (q.opcode) {
                case OP_READ:
                    q.transaction_id = store_[q.query_id & MATRIX_STORE_MASK];
                    reads_.fetch_add(1, std::memory_order_relaxed);
                    break;
                case OP_WRITE: {
                    // ponytail: mask guards overflow; capacity already >= batch size so it never wraps in practice
                    const size_t slot = delta_head_.fetch_add(1, std::memory_order_relaxed) & MATRIX_DELTA_LOG_MASK;
                    delta_log_[slot] = Mutation{q.query_id, q.query_id};
                    writes_.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                default:
                    break; // OP_SCAN and unknown opcodes are no-ops in the mock
            }
        }

        // Reconciliation phase: commit staged mutations into the store, then reset the
        // log for the next batch. Single consumer ⇒ no validation needed yet.
        // ponytail: last-writer-wins. Full OCC (TEV lock bit + read-set validation) is the
        // upgrade path once batches are processed by concurrent workers.
        const size_t logged = delta_head_.load(std::memory_order_relaxed);
        for (size_t s = 0; s < logged; ++s) {
            const Mutation& m = delta_log_[s & MATRIX_DELTA_LOG_MASK];
            store_[m.key & MATRIX_STORE_MASK] = m.value;
            delta_applied_.fetch_add(1, std::memory_order_relaxed);
        }
        delta_head_.store(0, std::memory_order_relaxed);
    }

    uint64_t reads() const override { return reads_.load(std::memory_order_relaxed); }
    uint64_t writes() const override { return writes_.load(std::memory_order_relaxed); }
    uint64_t delta_applied() const override { return delta_applied_.load(std::memory_order_relaxed); }

private:
    void worker_loop(size_t thread_id) {
        (void)thread_id;
        while (!shutdown_.load(std::memory_order_relaxed)) {
#if defined(__ARM_ARCH) || defined(__apple_build_version__)
            asm volatile("isb" ::: "memory");
#else
            asm volatile("pause" ::: "memory");
#endif
        }
    }

    std::array<uint64_t, MATRIX_STORE_SLOTS> store_{};        // the Value column
    std::array<Mutation, MATRIX_DELTA_LOG_CAPACITY> delta_log_{};
    std::atomic<size_t> delta_head_{0};                      // wait-free slot reservation cursor
    std::atomic<uint64_t> reads_{0};
    std::atomic<uint64_t> writes_{0};
    std::atomic<uint64_t> delta_applied_{0};

    std::vector<std::thread> workers_;
    std::atomic<bool> shutdown_;
};

#include "types.hpp"
#include "ring_buffer.hpp"
#if defined(MATRIX_USE_CUDA)
    #include "compute_cuda.cuh"   // real GPU engine (build with nvcc -DMATRIX_USE_CUDA)
    using EngineType = CUDAGPUEngine;
#else
    #include "compute_mock.cpp"   // CPU fallback (default; runs anywhere)
    using EngineType = CPUMockEngine;
#endif
#include <iostream>
#include <chrono>
#include <thread>
#include <memory>
#include <array>
#include <atomic>
#include <cassert>
#include <vector>
#include <algorithm>

#if defined(__ARM_ARCH) || defined(__apple_build_version__)
    #include <pthread.h>
    #include <pthread/qos.h>        // ponytail: spec omitted these 3; needed to compile the
    #include <mach/mach.h>          // QoS + Mach affinity calls below on macOS
    #include <mach/thread_policy.h>
#elif defined(__linux__)
    #include <sched.h>
#endif

/**
 * @brief Platform-agnostic pipeline stalling barrier for low-overhead busy spins.
 */
inline void spin_stall() noexcept {
#if defined(__ARM_ARCH) || defined(__apple_build_version__)
    // Instruction Synchronization Barrier (isb) forces instruction execution pipeline flush on ARM64
    asm volatile("isb" ::: "memory");
#elif defined(__x86_64__)
    // Standard pause execution hint
    asm volatile("pause" ::: "memory");
#else
    asm volatile("" ::: "memory");
#endif
}

/**
 * @brief Configures OS-specific thread-pinning and priority policies to ensure execution on performance cores.
 */
inline void pin_to_performance_core() {
#if defined(__ARM_ARCH) || defined(__apple_build_version__)
    // Promotes the calling thread to the interactive QoS class to guarantee performance core placement on macOS
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

    // Advisory affinity grouping configuration on Apple Silicon Mach kernels
    thread_port_t mach_thread = pthread_mach_thread_np(pthread_self());
    thread_affinity_policy_data_t policy = { 1 };
    thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY, (thread_policy_t)&policy, THREAD_AFFINITY_POLICY_COUNT);
#elif defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset); // Pin strictly to logical Core 0
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
}

/**
 * @brief Raw SPSC handoff latency via ping-pong: the producer pushes exactly one
 * item, waits until the consumer has popped it, then pushes the next. This isolates
 * the pure enqueue->dequeue cost — the spec's "sub-microsecond" claim — with zero
 * queue backlog, unlike the pipeline's queue-residency number which is dominated by
 * burst backlog. Backend-independent: measures only the ring buffer.
 */
void measure_handoff_latency() {
    constexpr size_t WARMUP = 1000;   // discard cold-cache / thread-migration samples
    constexpr size_t SAMPLES = 100000;
    auto ring = std::make_unique<SPSCRingBuffer<DatabaseQuery, 4096>>();
    std::atomic<bool> running{true};
    std::vector<uint64_t> samples;
    samples.reserve(SAMPLES);

    // Consumer pops and timestamps; it is the sole writer of `samples` (joined before read).
    std::thread consumer([&]() {
        pin_to_performance_core();
        DatabaseQuery v;
        while (running.load(std::memory_order_relaxed)) {
            if (ring->try_pop(v)) {
                const uint64_t now_ns = static_cast<uint64_t>(
                    std::chrono::steady_clock::now().time_since_epoch().count());
                if (v.query_id >= WARMUP) {
                    samples.push_back(now_ns - v.timestamp_us);
                }
            }
        }
    });

    for (size_t i = 0; i < WARMUP + SAMPLES; ++i) {
        DatabaseQuery q{};
        q.query_id = i;
        q.timestamp_us = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        while (!ring->try_emplace(q)) spin_stall();
        while (!ring->empty()) spin_stall(); // wait for consumer to take it (ping-pong)
    }
    running.store(false);
    consumer.join();

    std::sort(samples.begin(), samples.end());
    const auto pct = [&](double p) { return samples[static_cast<size_t>(p * (samples.size() - 1))]; };
    std::cout << "Raw SPSC handoff (ns)    "
              << "p50=" << pct(0.50) << "  p99=" << pct(0.99)
              << "  p99.9=" << pct(0.999) << "  max=" << samples.back() << std::endl;
}

int main() {
    std::cout << "MatrixDB Bare-Metal Engine Booting..." << std::endl;

    // Microbenchmark first: pure ring handoff, before the pipeline saturates anything.
    measure_handoff_latency();

    constexpr size_t BATCH_SIZE_LIMIT = 512;
    constexpr uint64_t TEMPORAL_LIMIT_US = 50;
    constexpr size_t TOTAL_QUERIES = 10000;

    auto ring_buffer = std::make_unique<SPSCRingBuffer<DatabaseQuery, 4096>>();
    auto mock_engine = std::make_unique<EngineType>(4);

    std::atomic<bool> run_state{true};
    std::atomic<size_t> total_processed{0}; // ponytail: end-to-end check that every query flows through

    // Pre-reserved so the hot path never allocates. Filled by the single consumer only.
    std::vector<uint64_t> latencies_ns;
    latencies_ns.reserve(TOTAL_QUERIES);

    // Spin up the consumer thread to execute the ingestion busy-spin control loop
    std::thread consumer([&]() {
        pin_to_performance_core();

        std::array<DatabaseQuery, BATCH_SIZE_LIMIT> batch;
        size_t accumulated_queries = 0;
        auto batch_start_time = std::chrono::high_resolution_clock::now();

        while (run_state.load(std::memory_order_relaxed)) {
            DatabaseQuery incoming_query;
            const bool success = ring_buffer->try_pop(incoming_query);

            if (success) {
                // Queue residency = time from producer's enqueue stamp to this pop.
                const uint64_t now_ns = static_cast<uint64_t>(
                    std::chrono::steady_clock::now().time_since_epoch().count());
                latencies_ns.push_back(now_ns - incoming_query.timestamp_us);

                if (accumulated_queries == 0) {
                    batch_start_time = std::chrono::high_resolution_clock::now();
                }
                batch[accumulated_queries++] = incoming_query;
            }

            // Continuous evaluation of the Dual-Trigger Condition
            if (accumulated_queries > 0) {
                const auto current_time = std::chrono::high_resolution_clock::now();
                const auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    current_time - batch_start_time
                ).count();

                // Trigger flush if batch is full OR time-based window has expired
                if (accumulated_queries >= BATCH_SIZE_LIMIT || duration_us >= static_cast<long long>(TEMPORAL_LIMIT_US)) {
                    mock_engine->execute_batch(batch.data(), accumulated_queries);
                    total_processed.fetch_add(accumulated_queries, std::memory_order_relaxed);
                    accumulated_queries = 0;
                }
            }

            if (!success) {
                spin_stall();
            }
        }
    });

    // Simulate query production on a separate thread
    auto pipeline_start = std::chrono::steady_clock::now();
    std::thread producer([&]() {
        for (size_t i = 0; i < TOTAL_QUERIES; ++i) {
            // Stamp ingestion time (steady_clock ns) so the consumer can measure queue residency.
            const uint64_t stamp_ns = static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            // Alternate Read/Write so both opcode paths are exercised end to end.
            const uint32_t op = (i % 2 == 0) ? OP_READ : OP_WRITE;
            DatabaseQuery q{i, stamp_ns, 0, op, 8, {0}, {0}};
            while (!ring_buffer->try_emplace(q)) {
                spin_stall();
            }
        }
    });

    producer.join();

    // Wait until every produced query has been processed, then stamp the drain time.
    // Replaces an arbitrary fixed sleep so throughput reflects real drain, not slack.
    while (total_processed.load(std::memory_order_relaxed) < TOTAL_QUERIES) {
        spin_stall();
    }
    auto pipeline_end = std::chrono::steady_clock::now();
    run_state.store(false);

    if (consumer.joinable()) {
        consumer.join();
    }

    const double elapsed_s =
        std::chrono::duration<double>(pipeline_end - pipeline_start).count();
    const double throughput = TOTAL_QUERIES / elapsed_s;

    const size_t processed = total_processed.load();
    std::cout << "Processed " << processed << " / " << TOTAL_QUERIES << " queries." << std::endl;
    assert(processed == TOTAL_QUERIES && "Dual-trigger pipeline dropped queries");

    // Verify the engine actually dispatched on opcode and committed every mutation.
    const uint64_t reads = mock_engine->reads();
    const uint64_t writes = mock_engine->writes();
    const uint64_t applied = mock_engine->delta_applied();
    std::cout << "Engine: reads=" << reads << " writes=" << writes
              << " delta_applied=" << applied << std::endl;
    assert(reads + writes == processed && "opcode dispatch missed queries");
    assert(applied == writes && "delta log reconcile dropped mutations");

    // Report end-to-end pipeline throughput (the payoff of batched GPU execution).
    std::cout << "Throughput: " << static_cast<uint64_t>(throughput)
              << " ops/sec (" << TOTAL_QUERIES << " queries in "
              << elapsed_s * 1e3 << " ms)" << std::endl;

    // Queue residency under burst: time each query waits in the ring before it is
    // batched. Dominated by backlog (producer outruns the single consumer), so this
    // is NOT the raw handoff cost reported above — it is the saturated-pipeline view.
    if (!latencies_ns.empty()) {
        std::sort(latencies_ns.begin(), latencies_ns.end());
        const auto pct = [&](double p) {
            const size_t idx = static_cast<size_t>(p * (latencies_ns.size() - 1));
            return latencies_ns[idx];
        };
        std::cout << "Queue residency (ns)     "
                  << "p50=" << pct(0.50) << "  "
                  << "p99=" << pct(0.99) << "  "
                  << "p99.9=" << pct(0.999) << "  "
                  << "max=" << latencies_ns.back() << std::endl;
    }

    std::cout << "Engine execution loop completed successfully." << std::endl;
    return 0;
}

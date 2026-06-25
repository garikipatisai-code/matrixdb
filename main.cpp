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

/**
 * @brief Throughput vs. batch size sweep — the key question for GPU viability.
 * Calls execute_batch() directly (no ring) so it measures pure engine cost per query
 * across batch sizes. Reveals fixed per-batch overhead and where (if ever) larger
 * batches amortize it. One run gives the whole curve — precious when GPU runs are remote.
 */
void sweep_batch_sizes(ComputeInterface& engine) {
    constexpr size_t QUERIES_PER_POINT = 400000; // enough work to dwarf timer noise
    const size_t sizes[] = {64, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};

    std::cout << "--- Batch-size sweep (execute_batch only, " << QUERIES_PER_POINT
              << " queries/point) ---" << std::endl;

    std::vector<DatabaseQuery> batch(MATRIX_BATCH_MAX);
    for (size_t i = 0; i < MATRIX_BATCH_MAX; ++i) {
        batch[i] = DatabaseQuery{};
        batch[i].query_id = i;
        batch[i].opcode = (i % 2 == 0) ? OP_READ : OP_WRITE;
    }

    // Warm up the engine (first GPU call pays one-time CUDA init) so it doesn't
    // pollute the smallest batch point.
    engine.execute_batch(batch.data(), 256);

    for (size_t bs : sizes) {
        const size_t iters = QUERIES_PER_POINT / bs;
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t it = 0; it < iters; ++it) {
            engine.execute_batch(batch.data(), bs);
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(t1 - t0).count();
        const double ops = static_cast<double>(iters * bs) / secs;
        const double us_per_batch = (secs / iters) * 1e6;
        std::cout << "  batch=" << bs
                  << "\t" << static_cast<uint64_t>(ops) << " ops/sec"
                  << "\t" << us_per_batch << " us/batch" << std::endl;
    }
}

/**
 * @brief Scan benchmark — the GPU's home turf. Filter-count over RESIDENT data of
 * growing size: small (fits CPU cache) to large (far exceeds it). The crossover where
 * GPU overtakes CPU is the whole point — exploiting bandwidth over big data, not
 * shipping tiny point-ops over PCIe. Verifies the count against a known oracle.
 */
void scan_benchmark(ComputeInterface& engine) {
    const size_t sizes[] = {1u<<16, 1u<<18, 1u<<20, 1u<<22, 1u<<24, 1u<<26}; // 64K .. 64M values
    std::cout << "--- Scan benchmark (filter-count over resident data) ---" << std::endl;
    for (size_t n : sizes) {
        const uint64_t threshold = n / 2;
        uint64_t c64 = 0, c32 = 0, c32x4 = 0;
        const double s64 = engine.benchmark_scan(n, threshold, c64);
        const double s32 = engine.benchmark_scan_u32(n, static_cast<uint32_t>(threshold), c32);
        const double s32x4 = engine.benchmark_scan_u32x4(n, static_cast<uint32_t>(threshold), c32x4);
        // value[i]=i, so count of i>threshold == n-1-threshold (oracle).
        assert(c64 == n - 1 - threshold && "u64 scan produced wrong count");
        assert(c32 == n - 1 - threshold && "u32 scan produced wrong count");
        assert(c32x4 == n - 1 - threshold && "u32x4 vectorized scan produced wrong count");
        std::cout << "  n=" << n
                  << "\tu64 " << (n * sizeof(uint64_t)) / 1e9 / s64 << " GB/s"
                  << "\tu32 " << (n * sizeof(uint32_t)) / 1e9 / s32 << " GB/s"
                  << "\tu32x4 " << (n * sizeof(uint32_t)) / 1e9 / s32x4 << " GB/s"
                  << " (" << static_cast<uint64_t>(n / s32x4 / 1e6) << "M vals/s)"
                  << std::endl;
    }
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

    // Sweep batch sizes before the pipeline run so one execution yields the whole
    // throughput-vs-batch curve (decisive for GPU viability; remote runs are costly).
    sweep_batch_sizes(*mock_engine);
    scan_benchmark(*mock_engine);

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

    // Snapshot engine counters after the sweep so the pipeline asserts below measure
    // only the pipeline's 10k queries, not the sweep's traffic.
    const uint64_t reads_base = mock_engine->reads();
    const uint64_t writes_base = mock_engine->writes();
    const uint64_t applied_base = mock_engine->commits();

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
    // Deltas since the pre-pipeline snapshot isolate the pipeline from the sweep.
    const uint64_t reads = mock_engine->reads() - reads_base;
    const uint64_t writes = mock_engine->writes() - writes_base;
    const uint64_t applied = mock_engine->commits() - applied_base;
    std::cout << "Engine: reads=" << reads << " writes=" << writes
              << " commits=" << applied << std::endl;
    std::cout << "Store checksum: " << mock_engine->store_checksum()
              << " (must match across CPU and GPU backends)" << std::endl;
    assert(reads + writes == processed && "opcode dispatch missed queries");
    assert(applied == writes && "page-ownership commit dropped mutations");

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

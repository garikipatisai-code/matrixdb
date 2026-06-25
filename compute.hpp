#pragma once

#include "types.hpp"
#include <cstddef>
#include <cstdint>

/**
 * @brief Pure virtual interface defining the compute engine's entry point.
 * Enables zero-copy, raw pointer processing over batch slices.
 *
 * Two implementations satisfy this contract and are selected at compile time:
 *   - CPUMockEngine (compute_mock.cpp)  — default, runs anywhere, no GPU needed
 *   - CUDAGPUEngine (compute_cuda.cuh)  — built with nvcc when MATRIX_USE_CUDA is set
 *
 * Both use the page-ownership model: a key always routes to one owning page, so the
 * two backends produce byte-identical store contents (verifiable in main.cpp).
 */
class ComputeInterface {
public:
    virtual ~ComputeInterface() = default;
    virtual void execute_batch(DatabaseQuery* batch_array, size_t count) = 0;

    virtual uint64_t reads() const = 0;
    virtual uint64_t writes() const = 0;
    virtual uint64_t commits() const = 0; // writes actually applied to the store

    // Order-independent fingerprint of the whole store (sum of slot values). Lets
    // main.cpp assert CPU and GPU reach byte-identical state — the real test of the
    // ownership model, not just matching counts.
    virtual uint64_t store_checksum() const = 0;

    // Scan benchmark: allocate n resident values (value[i]=i), then time a single
    // filter-count scan (count of value[i] > threshold) over that resident data.
    // alloc+fill are NOT timed — the point is "data lives here, query scans it".
    // This is the GPU's home turf (bandwidth over data too big for CPU cache),
    // the opposite of the point-op path. Returns seconds; sets out_count.
    // out_count is deterministic (value[i]=i) so it must match across CPU and GPU.
    virtual double benchmark_scan(size_t n, uint64_t threshold, uint64_t& out_count) = 0;

    // Same scan over a uint32 column. Scan is bandwidth-bound, so halving bytes/value
    // should ~double values/sec at the same GB/s — the columnar "narrowest type" win.
    // If GB/s holds vs the uint64 scan, we are at the bandwidth wall (vectorized loads
    // won't help); if it drops, narrower loads underfill the bus and vectorizing is next.
    virtual double benchmark_scan_u32(size_t n, uint32_t threshold, uint64_t& out_count) = 0;

    // Vectorized uint32 scan (4 values per load via uint4). Tests the memory-level
    // parallelism theory: if this beats the scalar u32 GB/s, narrow scalar loads were
    // underfilling the bus (MLP-bound) and wider loads are the fix. If it doesn't, the
    // kernel is ALU-bound and we stop optimizing. CPU may treat this same as scalar.
    virtual double benchmark_scan_u32x4(size_t n, uint32_t threshold, uint64_t& out_count) = 0;
};

// Which page owns a query's key. Single source of truth for both engines and the
// kernel: slot = key & STORE_MASK, page = slot / PAGE_SIZE.
inline size_t matrix_page_of(uint64_t key) {
    return (key & MATRIX_STORE_MASK) / MATRIX_PAGE_SIZE;
}

/**
 * @brief Stable counting-sort of a batch by owning page (the "bin in the batcher" step).
 * Fills `binned` with the queries grouped by page — order within a page preserved, so
 * same-slot writes keep batch order and last-writer-wins is deterministic. `offsets`
 * is a CSR index: page p's queries are binned[offsets[p] .. offsets[p+1]).
 * Caller owns the buffers (binned >= count, offsets >= PAGE_COUNT+1) — zero alloc here.
 */
inline void matrix_bin_by_page(const DatabaseQuery* batch, size_t count,
                               DatabaseQuery* binned, uint32_t* offsets) {
    uint32_t counts[MATRIX_PAGE_COUNT] = {0};
    for (size_t i = 0; i < count; ++i) {
        counts[matrix_page_of(batch[i].query_id)]++;
    }
    offsets[0] = 0;
    for (size_t p = 0; p < MATRIX_PAGE_COUNT; ++p) {
        offsets[p + 1] = offsets[p] + counts[p];
    }
    uint32_t cursor[MATRIX_PAGE_COUNT];
    for (size_t p = 0; p < MATRIX_PAGE_COUNT; ++p) cursor[p] = offsets[p];
    for (size_t i = 0; i < count; ++i) {
        const size_t p = matrix_page_of(batch[i].query_id);
        binned[cursor[p]++] = batch[i];
    }
}

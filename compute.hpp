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

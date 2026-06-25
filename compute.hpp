#pragma once

#include "types.hpp"
#include <cstddef>

/**
 * @brief Pure virtual interface defining the compute engine's entry point.
 * Enables zero-copy, raw pointer processing over batch slices.
 *
 * Two implementations satisfy this contract and are selected at compile time:
 *   - CPUMockEngine (compute_mock.cpp)  — default, runs anywhere, no GPU needed
 *   - CUDAGPUEngine (compute_cuda.cuh)  — built with nvcc when MATRIX_USE_CUDA is set
 *
 * The counters below let the caller verify dispatch/commit correctness identically
 * on both backends (same asserts in main.cpp prove the same thing on CPU and GPU).
 */
class ComputeInterface {
public:
    virtual ~ComputeInterface() = default;
    virtual void execute_batch(DatabaseQuery* batch_array, size_t count) = 0;

    virtual uint64_t reads() const = 0;
    virtual uint64_t writes() const = 0;
    virtual uint64_t delta_applied() const = 0;
};

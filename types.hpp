#pragma once

#include <cstdint>
#include <cstddef> // ponytail: spec wrote <csize_t> (does not exist); size_t lives in <cstddef>

#if defined(__ARM_ARCH) || defined(__apple_build_version__)
    // Apple Silicon L1 cache lines are typically 128 bytes
    constexpr size_t MATRIX_CACHE_LINE = 128;
#else
    // Standard Intel/AMD x86_64 cache lines are 64 bytes
    constexpr size_t MATRIX_CACHE_LINE = 64;
#endif

/**
 * @brief Represents a single transaction query record.
 * Designed with descending structural member alignment to prevent internal compiler padding.
 * Aligned to target CPU cache line boundaries to eliminate false sharing.
 */
struct alignas(MATRIX_CACHE_LINE) DatabaseQuery {
    uint64_t query_id;
    uint64_t timestamp_us;
    uint64_t transaction_id;
    uint32_t opcode;
    uint32_t payload_size;
    uint8_t payload[32];

    static constexpr size_t data_footprint =
        sizeof(uint64_t) * 3 +
        sizeof(uint32_t) * 2 +
        sizeof(uint8_t) * 32;

    static constexpr size_t padding_needed =
        (MATRIX_CACHE_LINE > data_footprint) ? (MATRIX_CACHE_LINE - data_footprint) : 0;

    // Explicit structural padding to align with target L1 cache line size
    uint8_t padding[padding_needed > 0 ? padding_needed : 1];
};

static_assert((sizeof(DatabaseQuery) % MATRIX_CACHE_LINE) == 0,
    "DatabaseQuery structure violates cache-line alignment constraints.");

// Operational instruction carried in DatabaseQuery::opcode (spec: Read, Write, Scan).
enum MatrixOpcode : uint32_t {
    OP_READ  = 1,
    OP_WRITE = 2,
    OP_SCAN  = 3,
};

// Shared store + append-only Delta Log layout (spec §2.3 store, §4 mutation path).
// Defined here so the CPU engine and the CUDA kernel agree byte-for-byte on the layout.
struct Mutation { uint64_t key; uint64_t value; };

constexpr size_t MATRIX_STORE_SLOTS       = 4096;                        // power of two
constexpr size_t MATRIX_STORE_MASK        = MATRIX_STORE_SLOTS - 1;
constexpr size_t MATRIX_DELTA_LOG_CAPACITY = 8192;                       // >= max queries per batch
constexpr size_t MATRIX_DELTA_LOG_MASK    = MATRIX_DELTA_LOG_CAPACITY - 1;
constexpr size_t MATRIX_BATCH_MAX         = 512;                         // host-side max batch (matches main)

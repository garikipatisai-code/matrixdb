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
    OP_TXN_WRITE = 4,   // a transactional put: buffered on WAL replay until a commit marker
};

// Aggregate reduction carried by OP_SCAN (payload offset 16). AGG_COUNT==0 is the default, so a
// scan with no agg op set counts matches (the original behavior). SUM/MIN/MAX reduce the values
// matching the predicate (value > threshold).
enum MatrixAggOp : uint32_t {
    AGG_COUNT = 0,
    AGG_SUM   = 1,
    AGG_MIN   = 2,
    AGG_MAX   = 3,
};

// Columnar store + page-ownership layout (shard-per-thread).
// The keyspace (STORE_SLOTS) is split into PAGE_COUNT contiguous pages. Exactly one
// owner (one GPU thread) reads/writes a page's slots, so the same key always routes
// to the same owner: writes to a key serialize by ownership. No cross-thread conflict
// => no store atomics, no OCC, no delta log. Each page has a single owner; pages are
// independent of one another (shared-nothing).
//
// DM-1b fix (2026-07-01): this used to be 4096 while KVStore's own point-op capacity
// (compute_mock.cpp's `KVStore kv_{...}`) was independently hardcoded to 65536 (== BATCH_MAX) --
// two different capacities for what's supposed to be "the same" point-op store across backends.
// GPU's flat, direct-mapped store (compute_cuda.cuh's matrix_page_kernel: `store[key & MASK]`, no
// probing) silently overwrote on any two keys sharing a slot -- guaranteed for the standard
// benchmark workload (BATCH_MAX=65536 unique keys into only 4096 slots). Matching this to BATCH_MAX
// (and KVStore's capacity, which now reads MATRIX_STORE_SLOTS instead of its own literal) makes the
// two backends' capacities the same symbol, permanently, and gives GPU enough slots that the
// standard sequential-key workload (query_id 0..BATCH_MAX-1) is a perfect bijection -- zero
// collisions by construction, not by luck. See PRODUCTION_READINESS.md DM-1b and
// test_gpu_pointop_collision.cu for the hardware-verified proof. This does NOT make the GPU store a
// general-purpose hash table: a batch with more than STORE_SLOTS truly-distinct keys, or an
// adversarial (non-sequential) key distribution, can still collide -- narrower and far less likely
// than the guaranteed-collision case this fix closes, and unlike a hash table's graceful probing,
// still resolves via silent last-writer-wins if it happens. Not solved further: this kernel is
// confirmed unreachable from anything exercised in this repo (see the DM-1b note), so going further
// than "make the actual, demonstrated case correct" was judged disproportionate.
constexpr size_t MATRIX_STORE_SLOTS = 65536;                      // power of two; == BATCH_MAX, == KVStore's capacity
constexpr size_t MATRIX_STORE_MASK  = MATRIX_STORE_SLOTS - 1;
constexpr size_t MATRIX_PAGE_COUNT  = 1024;                       // owner threads; the tuning knob
constexpr size_t MATRIX_PAGE_SIZE   = MATRIX_STORE_SLOTS / MATRIX_PAGE_COUNT; // slots per page
constexpr size_t MATRIX_BATCH_MAX   = 65536;                      // sweep ceiling / scratch buffer size

// Resident analytical column (the GPU-DB's actual data): a uint32 column that lives
// in VRAM/RAM and is scanned in place by OP_SCAN — never shipped per query. 16M values
// = 64MB, well past CPU cache so the GPU bandwidth advantage is real. Filled value[i]=i
// so a threshold T yields a known count (SCAN_SIZE-1-T) for oracle verification.
constexpr size_t MATRIX_SCAN_COLUMN_SIZE = 1u << 24;             // 16,777,216 values (divisible by 4 for uint4)

static_assert(MATRIX_STORE_SLOTS % MATRIX_PAGE_COUNT == 0,
    "store slots must divide evenly into pages so each page owns a contiguous slice");

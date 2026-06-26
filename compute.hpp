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

    // Run a single OP_SCAN over the resident column (sets q.transaction_id to the count).
    // Separate from execute_batch so scans run on their own thread/queue and stream and
    // don't head-of-line-block point ops. Touches only scan-path state (disjoint from
    // the point-op store/counters), so the two may run concurrently.
    virtual void execute_scan(DatabaseQuery& q) = 0;

    virtual uint64_t reads() const = 0;
    virtual uint64_t writes() const = 0;
    virtual uint64_t commits() const = 0; // writes actually applied to the store

    // OP_SCAN result accounting: how many scan queries ran, and the running sum of their
    // filter-count results. main asserts the sum against a known oracle to prove scans
    // flowed ring -> batcher -> execute_batch -> (GPU) scan kernel correctly.
    virtual uint64_t scans() const = 0;
    virtual uint64_t scan_result_sum() const = 0;

    // Wall time the engine spent inside OP_SCAN work. Lets the pipeline report point-op
    // and scan throughput separately — a single 64MB scan costs ~1000x a point op, so
    // one combined "ops/sec" number is meaningless once scans are mixed in.
    virtual double scan_time_s() const = 0;

    // Pure kernel time for OP_SCAN work (GPU: cudaEvent-measured, excludes launch/sync/
    // copy and host jitter; CPU: same as the compute loop). scan_time_s() - this =
    // per-scan overhead. The split attributes the integrated-vs-standalone gap.
    virtual double scan_kernel_time_s() const = 0;

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

    // Items-per-thread scan (register blocking, ITEMS independent loads/thread before
    // comparing). CUB's BlockReduce lever. Tests whether deeper memory-level parallelism
    // per thread beats both scalar and uint4. CPU delegates (compiler auto-vectorizes).
    virtual double benchmark_scan_ipt(size_t n, uint32_t threshold, uint64_t& out_count) = 0;
};

// Which page owns a query's key. Single source of truth for both engines and the
// kernel: slot = key & STORE_MASK, page = slot / PAGE_SIZE.
inline size_t matrix_page_of(uint64_t key) {
    return (key & MATRIX_STORE_MASK) / MATRIX_PAGE_SIZE;
}

// OP_SCAN carries its filter threshold in the query payload, and may target a specific
// catalog column: threshold at payload[0] (u32), column id at payload[8] (u64). Column
// id 0 == the legacy fixed scan column. payload begins 8-byte aligned (DatabaseQuery
// starts with u64 fields, payload is at offset 32), so the u64 at payload+8 is aligned.
// One codec used by both engines so they decode identically.
inline void matrix_set_scan_target(DatabaseQuery& q, uint32_t threshold, uint64_t column_id) {
    q.opcode = OP_SCAN;
    *reinterpret_cast<uint32_t*>(q.payload) = threshold;
    *reinterpret_cast<uint64_t*>(q.payload + 8) = column_id;
}
inline uint64_t matrix_get_scan_column_id(const DatabaseQuery& q) {
    return *reinterpret_cast<const uint64_t*>(q.payload + 8);
}
inline void matrix_set_scan_threshold(DatabaseQuery& q, uint32_t threshold) {
    matrix_set_scan_target(q, threshold, 0); // legacy: target the fixed column (id 0)
}
inline uint32_t matrix_get_scan_threshold(const DatabaseQuery& q) {
    return *reinterpret_cast<const uint32_t*>(q.payload);
}

// OP_SCAN's aggregate op lives at payload offset 16 (u32). Default 0 == AGG_COUNT (the original
// count semantics), so a query built with only set_scan_target/set_scan_threshold reduces by COUNT.
inline void matrix_set_scan_agg_op(DatabaseQuery& q, MatrixAggOp op) {
    *reinterpret_cast<uint32_t*>(q.payload + 16) = static_cast<uint32_t>(op);
}
inline MatrixAggOp matrix_get_scan_agg_op(const DatabaseQuery& q) {
    return static_cast<MatrixAggOp>(*reinterpret_cast<const uint32_t*>(q.payload + 16));
}

// Filtered reduction over a uint32 column: reduce the values matching the predicate
// (value > threshold) by `op`. One tight loop per op (dispatch OUTSIDE the loop, so the COUNT
// loop stays exactly as fast as the original scan). Empty match-set sentinels: COUNT/SUM -> 0,
// MIN -> UINT64_MAX, MAX -> 0 (matched values are > threshold >= 0, i.e. >= 1, so 0 / UINT64_MAX
// unambiguously signal "no match"). SUM accumulates in u64 (no overflow for the engine's columns).
inline uint64_t matrix_cpu_reduce(const uint32_t* v, size_t n, uint32_t threshold, MatrixAggOp op) {
    switch (op) {
        case AGG_SUM: { uint64_t s = 0; for (size_t i = 0; i < n; ++i) if (v[i] > threshold) s += v[i]; return s; }
        case AGG_MIN: { uint64_t m = UINT64_MAX; for (size_t i = 0; i < n; ++i) if (v[i] > threshold && v[i] < m) m = v[i]; return m; }
        case AGG_MAX: { uint64_t m = 0; for (size_t i = 0; i < n; ++i) if (v[i] > threshold && v[i] > m) m = v[i]; return m; }
        case AGG_COUNT:
        default:      { uint64_t c = 0; for (size_t i = 0; i < n; ++i) c += (v[i] > threshold); return c; }
    }
}

// Unfiltered scalar reduction (aggregate ALL values; the no-WHERE companion to matrix_cpu_reduce).
// COUNT -> n; SUM -> Σv (u64); MIN/MAX over all (empty sentinels MIN UINT64_MAX / MAX 0 reachable
// only when n==0). Separate from matrix_cpu_reduce (which always filters value>threshold) — clearer
// than threading if constexpr through its four per-op loops.
inline uint64_t matrix_cpu_reduce_all(const uint32_t* v, size_t n, MatrixAggOp op) {
    switch (op) {
        case AGG_SUM: { uint64_t s = 0; for (size_t i = 0; i < n; ++i) s += v[i]; return s; }
        case AGG_MIN: { uint64_t m = UINT64_MAX; for (size_t i = 0; i < n; ++i) if (v[i] < m) m = v[i]; return m; }
        case AGG_MAX: { uint64_t m = 0; for (size_t i = 0; i < n; ++i) if (v[i] > m) m = v[i]; return m; }
        case AGG_COUNT:
        default:      return n;
    }
}

// A structured analytical query over catalog columns (value_col / key_col > 0). has_filter applies
// WHERE value > threshold; grouped applies GROUP BY key_col into num_groups dense buckets.
struct MatrixQuery {
    uint64_t    value_col  = 0;
    MatrixAggOp agg        = AGG_COUNT;
    bool        has_filter = false;
    uint32_t    threshold  = 0;
    bool        grouped    = false;
    uint64_t    key_col    = 0;
    uint32_t    num_groups = 0;
};

// Grouped reduction core (GROUP BY key). Filtered==true applies WHERE value > threshold (compiled
// out when false via if constexpr, so the unfiltered path is byte-identical to the original). Dense
// groups [0, num_groups); keys >= num_groups ignored; out initialized per op (empty-group sentinels
// match matrix_cpu_reduce: COUNT/SUM/MAX -> 0, MIN -> UINT64_MAX). SUM accumulates in u64. One pass;
// the op branch is inside the loop because grouped reduction is scatter-bound (random out[k] writes),
// not branch-bound.
template <bool Filtered>
inline void matrix_group_reduce_impl(const uint32_t* keys, const uint32_t* values, size_t n,
                                     uint32_t num_groups, MatrixAggOp op, uint32_t threshold, uint64_t* out) {
    const uint64_t init = (op == AGG_MIN) ? UINT64_MAX : 0;
    for (uint32_t g = 0; g < num_groups; ++g) out[g] = init;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t k = keys[i];
        if (k >= num_groups) continue;                       // out-of-range key: ignored
        const uint32_t v = values[i];
        if constexpr (Filtered) { if (v <= threshold) continue; }  // WHERE value > threshold
        switch (op) {
            case AGG_SUM:   out[k] += v; break;
            case AGG_MIN:   if (v < out[k]) out[k] = v; break;
            case AGG_MAX:   if (v > out[k]) out[k] = v; break;
            case AGG_COUNT:
            default:        out[k] += 1; break;
        }
    }
}
// GROUP BY key (all rows). Unchanged signature from GBY-1 — now a thin wrapper.
inline void matrix_cpu_group_reduce(const uint32_t* keys, const uint32_t* values, size_t n,
                                    uint32_t num_groups, MatrixAggOp op, uint64_t* out) {
    matrix_group_reduce_impl<false>(keys, values, n, num_groups, op, /*threshold*/0, out);
}
// GROUP BY key WHERE value > threshold.
inline void matrix_cpu_group_reduce_where(const uint32_t* keys, const uint32_t* values, size_t n,
                                          uint32_t num_groups, MatrixAggOp op, uint32_t threshold, uint64_t* out) {
    matrix_group_reduce_impl<true>(keys, values, n, num_groups, op, threshold, out);
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

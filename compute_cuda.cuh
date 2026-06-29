#pragma once

// CUDA backend — page-ownership model (Component 4: Parallel Engine).
// Compile on a GPU host (Google Colab) with:
//     nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread -DMATRIX_USE_CUDA main.cpp -o matrixdb_proto
//
// One CUDA BLOCK owns one page. The batch is binned by page on the host (CSR offsets),
// so block p processes only page p's contiguous queries against page p's store slice.
// Different blocks touch disjoint store slots ⇒ no cross-block conflict, no store
// atomics, no delta log. Each page has a single owner; pages are independent
// (shared-nothing). Threads within a block stride over the page's queries.

#include "compute.hpp"
#include <cuda_runtime.h>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#define CUDA_CHECK(call)                                                            \
    do {                                                                           \
        cudaError_t _err = (call);                                                 \
        if (_err != cudaSuccess) {                                                 \
            std::fprintf(stderr, "CUDA error %s at %s:%d -> %s\n",                 \
                         #call, __FILE__, __LINE__, cudaGetErrorString(_err));     \
            std::abort();                                                          \
        }                                                                          \
    } while (0)

// One block per page. blockIdx.x == page id; the block's threads cooperatively process
// that page's queries [offsets[page], offsets[page+1]). Writes to a slot within a page
// are owned by this block alone, so no atomics on the store are needed. Same-slot writes
// within the page race between threads -> last-writer-wins, matching the CPU mock's
// deterministic in-order result only when keys are unique (true for our benchmark).
__global__ void matrix_page_kernel(const DatabaseQuery* binned, const uint32_t* offsets,
                                   uint64_t* store,
                                   unsigned long long* reads,
                                   unsigned long long* writes) {
    const size_t page = blockIdx.x;
    if (page >= MATRIX_PAGE_COUNT) return;

    const uint32_t begin = offsets[page];
    const uint32_t end = offsets[page + 1];

    unsigned r = 0, w = 0;
    for (uint32_t j = begin + threadIdx.x; j < end; j += blockDim.x) {
        const DatabaseQuery q = binned[j];
        const size_t slot = q.query_id & MATRIX_STORE_MASK;
        if (q.opcode == OP_READ) {
            volatile uint64_t v = store[slot]; (void)v; // touch the store (read path)
            ++r;
        } else if (q.opcode == OP_WRITE) {
            store[slot] = q.query_id; // mock projection: value == key
            ++w;
        }
    }
    if (r) atomicAdd(reads, (unsigned long long)r);   // counters only — not on the store
    if (w) atomicAdd(writes, (unsigned long long)w);
}

// Filter-count scan: count values > threshold. Grid-stride over resident VRAM data,
// block-local reduction, one atomicAdd per block. This is the GPU's home turf —
// streaming bandwidth over data too large for CPU cache.
__global__ void matrix_scan_kernel(const uint64_t* data, size_t n,
                                   uint64_t threshold, unsigned long long* count) {
    __shared__ unsigned long long block_count;
    if (threadIdx.x == 0) block_count = 0;
    __syncthreads();

    unsigned long long local = 0;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride) {
        if (data[i] > threshold) ++local;
    }
    atomicAdd(&block_count, local);
    __syncthreads();
    if (threadIdx.x == 0) atomicAdd(count, block_count);
}

// uint32 column variant — half the bytes/value. Same grid-stride filter-count.
__global__ void matrix_scan_kernel_u32(const uint32_t* data, size_t n,
                                       uint32_t threshold, unsigned long long* count) {
    __shared__ unsigned long long block_count;
    if (threadIdx.x == 0) block_count = 0;
    __syncthreads();

    unsigned long long local = 0;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride) {
        if (data[i] > threshold) ++local;
    }
    atomicAdd(&block_count, local);
    __syncthreads();
    if (threadIdx.x == 0) atomicAdd(count, block_count);
}

// AGG-2 (GPU-1): SUM / MIN / MAX of values matching `value > threshold`, same grid-stride +
// block-local + one-atomic-per-block shape as the COUNT kernel, differing only in accumulator + atomic.
// Each is correct iff its result equals matrix_cpu_reduce(host, n, threshold, op) over the same bytes
// (the cross-backend invariant — test_gpu_agg.cu is the merge gate). Empty-match sentinels match the CPU:
// SUM 0, MIN UINT64_MAX, MAX 0. `out` must be pre-initialized to the sentinel before launch.
__global__ void matrix_sum_kernel_u32(const uint32_t* data, size_t n,
                                      uint32_t threshold, unsigned long long* out) {
    __shared__ unsigned long long blk;
    if (threadIdx.x == 0) blk = 0;
    __syncthreads();
    unsigned long long local = 0;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride)
        if (data[i] > threshold) local += data[i];
    atomicAdd(&blk, local);
    __syncthreads();
    if (threadIdx.x == 0) atomicAdd(out, blk);
}
__global__ void matrix_min_kernel_u32(const uint32_t* data, size_t n,
                                      uint32_t threshold, unsigned long long* out) {
    __shared__ unsigned long long blk;
    if (threadIdx.x == 0) blk = 0xFFFFFFFFFFFFFFFFull;   // UINT64_MAX (CPU empty-MIN sentinel)
    __syncthreads();
    unsigned long long local = 0xFFFFFFFFFFFFFFFFull;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride)
        if (data[i] > threshold && data[i] < local) local = data[i];
    atomicMin(&blk, local);
    __syncthreads();
    if (threadIdx.x == 0) atomicMin(out, blk);
}
__global__ void matrix_max_kernel_u32(const uint32_t* data, size_t n,
                                      uint32_t threshold, unsigned long long* out) {
    __shared__ unsigned long long blk;
    if (threadIdx.x == 0) blk = 0;                       // 0 (CPU empty-MAX sentinel)
    __syncthreads();
    unsigned long long local = 0;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride)
        if (data[i] > threshold && data[i] > local) local = data[i];
    atomicMax(&blk, local);
    __syncthreads();
    if (threadIdx.x == 0) atomicMax(out, blk);
}

// === GPU-4 (predicates) + GPU-2 (grouped) — ADDITIVE; the GPU-1 threshold kernels above are untouched. ===
// Device predicate mirroring matrix_pred_match (compute.hpp) so a GPU WHERE matches the CPU's exactly.
__device__ __forceinline__ bool matrix_pred_match_dev(uint32_t v, MatrixCmp c, uint32_t a, uint32_t b) {
    switch (c) {
        case MatrixCmp::GE:      return v >= a;
        case MatrixCmp::LT:      return v <  a;
        case MatrixCmp::LE:      return v <= a;
        case MatrixCmp::EQ:      return v == a;
        case MatrixCmp::NE:      return v != a;
        case MatrixCmp::BETWEEN: return v >= a && v <= b;
        case MatrixCmp::GT:
        default:                 return v >  a;
    }
}
// GPU-4: predicate-filtered scalar reductions — the general sibling of the GPU-1 threshold kernels.
// Each == matrix_cpu_reduce_pred(host, n, {cmp,a,b}, op). Sentinels: COUNT/SUM 0, MIN UINT64_MAX, MAX 0
// (caller pre-inits `out` to the same: cudaMemset 0x00 for COUNT/SUM/MAX, 0xFF for MIN).
__global__ void matrix_count_kernel_pred_u32(const uint32_t* data, size_t n, MatrixCmp c, uint32_t a, uint32_t b, unsigned long long* out) {
    __shared__ unsigned long long blk; if (threadIdx.x == 0) blk = 0; __syncthreads();
    unsigned long long local = 0;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride)
        if (matrix_pred_match_dev(data[i], c, a, b)) ++local;
    atomicAdd(&blk, local); __syncthreads();
    if (threadIdx.x == 0) atomicAdd(out, blk);
}
__global__ void matrix_sum_kernel_pred_u32(const uint32_t* data, size_t n, MatrixCmp c, uint32_t a, uint32_t b, unsigned long long* out) {
    __shared__ unsigned long long blk; if (threadIdx.x == 0) blk = 0; __syncthreads();
    unsigned long long local = 0;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride)
        if (matrix_pred_match_dev(data[i], c, a, b)) local += data[i];
    atomicAdd(&blk, local); __syncthreads();
    if (threadIdx.x == 0) atomicAdd(out, blk);
}
__global__ void matrix_min_kernel_pred_u32(const uint32_t* data, size_t n, MatrixCmp c, uint32_t a, uint32_t b, unsigned long long* out) {
    __shared__ unsigned long long blk; if (threadIdx.x == 0) blk = 0xFFFFFFFFFFFFFFFFull; __syncthreads();
    unsigned long long local = 0xFFFFFFFFFFFFFFFFull;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride)
        if (matrix_pred_match_dev(data[i], c, a, b) && data[i] < local) local = data[i];
    atomicMin(&blk, local); __syncthreads();
    if (threadIdx.x == 0) atomicMin(out, blk);
}
__global__ void matrix_max_kernel_pred_u32(const uint32_t* data, size_t n, MatrixCmp c, uint32_t a, uint32_t b, unsigned long long* out) {
    __shared__ unsigned long long blk; if (threadIdx.x == 0) blk = 0; __syncthreads();
    unsigned long long local = 0;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride)
        if (matrix_pred_match_dev(data[i], c, a, b) && data[i] > local) local = data[i];
    atomicMax(&blk, local); __syncthreads();
    if (threadIdx.x == 0) atomicMax(out, blk);
}
// GPU-2: grouped reduction — one atomic per row into its dense group slot out[k] (k = keys[i], k < num_groups;
// out-of-range keys ignored, matching the CPU). Each == matrix_cpu_group_reduce(keys, vals, n, num_groups, op).
// Caller pre-inits out[num_groups] per op via cudaMemset: COUNT/SUM/MAX -> 0x00, MIN -> 0xFF (UINT64_MAX).
// ponytail: simple global-atomic per row (correct first); a per-block shared-memory privatized histogram is
// the contention-reduction follow-up for high group counts.
__global__ void matrix_group_count_kernel(const uint32_t* keys, size_t n, uint32_t num_groups, unsigned long long* out) {
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride) {
        const uint32_t k = keys[i]; if (k < num_groups) atomicAdd(&out[k], 1ull);
    }
}
__global__ void matrix_group_sum_kernel(const uint32_t* keys, const uint32_t* vals, size_t n, uint32_t num_groups, unsigned long long* out) {
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride) {
        const uint32_t k = keys[i]; if (k < num_groups) atomicAdd(&out[k], (unsigned long long)vals[i]);
    }
}
__global__ void matrix_group_min_kernel(const uint32_t* keys, const uint32_t* vals, size_t n, uint32_t num_groups, unsigned long long* out) {
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride) {
        const uint32_t k = keys[i]; if (k < num_groups) atomicMin(&out[k], (unsigned long long)vals[i]);
    }
}
__global__ void matrix_group_max_kernel(const uint32_t* keys, const uint32_t* vals, size_t n, uint32_t num_groups, unsigned long long* out) {
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride) {
        const uint32_t k = keys[i]; if (k < num_groups) atomicMax(&out[k], (unsigned long long)vals[i]);
    }
}

// === GPU-5 (typed int64 + double) — ADDITIVE; the u32 kernels above are untouched. ===
// Device predicates mirroring matrix_pred_match_i64 / matrix_pred_match_f64 (compute.hpp).
__device__ __forceinline__ bool matrix_pred_match_i64_dev(long long v, MatrixCmp c, long long a, long long b) {
    switch (c) {
        case MatrixCmp::GE:      return v >= a;
        case MatrixCmp::LT:      return v <  a;
        case MatrixCmp::LE:      return v <= a;
        case MatrixCmp::EQ:      return v == a;
        case MatrixCmp::NE:      return v != a;
        case MatrixCmp::BETWEEN: return v >= a && v <= b;
        case MatrixCmp::GT:
        default:                 return v >  a;
    }
}
__device__ __forceinline__ bool matrix_pred_match_f64_dev(double v, MatrixCmp c, double a, double b) {
    switch (c) {
        case MatrixCmp::GE:      return v >= a;
        case MatrixCmp::LT:      return v <  a;
        case MatrixCmp::LE:      return v <= a;
        case MatrixCmp::EQ:      return v == a;
        case MatrixCmp::NE:      return v != a;
        case MatrixCmp::BETWEEN: return v >= a && v <= b;
        case MatrixCmp::GT:
        default:                 return v >  a;
    }
}
// double has no native atomicMin/Max — CAS loop on the bit pattern (the standard idiom). Works on
// shared or global. NaN: matches the CPU's IEEE behavior (a NaN val never updates, since cur<=NaN is false).
__device__ __forceinline__ double atomicMinDouble(double* addr, double val) {
    unsigned long long* p = (unsigned long long*)addr;
    unsigned long long old = *p, assumed;
    do { assumed = old;
         if (__longlong_as_double(assumed) <= val) break;
         old = atomicCAS(p, assumed, __double_as_longlong(val));
    } while (assumed != old);
    return __longlong_as_double(old);
}
__device__ __forceinline__ double atomicMaxDouble(double* addr, double val) {
    unsigned long long* p = (unsigned long long*)addr;
    unsigned long long old = *p, assumed;
    do { assumed = old;
         if (__longlong_as_double(assumed) >= val) break;
         old = atomicCAS(p, assumed, __double_as_longlong(val));
    } while (assumed != old);
    return __longlong_as_double(old);
}
// Native atomicAdd(double*) exists on arch >= 6.0 and in the host pass; nvcc without an explicit -arch
// can default the device pass below 6.0, where the overload is absent. Provide the CAS fallback ONLY in
// that device pass (defined(__CUDA_ARCH__) && < 600) — NOT the host pass, where the native one is already
// declared (defining it there is the redefinition Colab caught).
#if defined(__CUDA_ARCH__) && __CUDA_ARCH__ < 600
__device__ __forceinline__ double atomicAdd(double* address, double val) {
    unsigned long long* p = (unsigned long long*)address;
    unsigned long long old = *p, assumed;
    do { assumed = old;
         old = atomicCAS(p, assumed, __double_as_longlong(val + __longlong_as_double(assumed)));
    } while (assumed != old);
    return __longlong_as_double(old);
}
#endif
#define MATRIX_DEV_POSINF __longlong_as_double((long long)0x7FF0000000000000ULL)
#define MATRIX_DEV_NEGINF __longlong_as_double((long long)0xFFF0000000000000ULL)

// int64 predicate-filtered reductions. out is `long long*`; sentinels match matrix_cpu_reduce_pred_i64
// (COUNT/SUM 0, MIN INT64_MAX, MAX INT64_MIN — caller cudaMemcpy's the init). SUM accumulates as unsigned
// (two's-complement add is bit-identical to signed; same overflow-wrap as the CPU).
__global__ void matrix_count_kernel_pred_i64(const long long* d, size_t n, MatrixCmp c, long long a, long long b, long long* out) {
    __shared__ unsigned long long blk; if (threadIdx.x == 0) blk = 0; __syncthreads();
    unsigned long long local = 0;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride)
        if (matrix_pred_match_i64_dev(d[i], c, a, b)) ++local;
    atomicAdd(&blk, local); __syncthreads();
    if (threadIdx.x == 0) atomicAdd((unsigned long long*)out, blk);
}
__global__ void matrix_sum_kernel_pred_i64(const long long* d, size_t n, MatrixCmp c, long long a, long long b, long long* out) {
    __shared__ unsigned long long blk; if (threadIdx.x == 0) blk = 0; __syncthreads();
    long long local = 0;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride)
        if (matrix_pred_match_i64_dev(d[i], c, a, b)) local += d[i];
    atomicAdd(&blk, (unsigned long long)local); __syncthreads();
    if (threadIdx.x == 0) atomicAdd((unsigned long long*)out, blk);
}
__global__ void matrix_min_kernel_pred_i64(const long long* d, size_t n, MatrixCmp c, long long a, long long b, long long* out) {
    __shared__ long long blk; if (threadIdx.x == 0) blk = INT64_MAX; __syncthreads();
    long long local = INT64_MAX;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride)
        if (matrix_pred_match_i64_dev(d[i], c, a, b) && d[i] < local) local = d[i];
    atomicMin(&blk, local); __syncthreads();
    if (threadIdx.x == 0) atomicMin(out, blk);
}
__global__ void matrix_max_kernel_pred_i64(const long long* d, size_t n, MatrixCmp c, long long a, long long b, long long* out) {
    __shared__ long long blk; if (threadIdx.x == 0) blk = INT64_MIN; __syncthreads();
    long long local = INT64_MIN;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride)
        if (matrix_pred_match_i64_dev(d[i], c, a, b) && d[i] > local) local = d[i];
    atomicMax(&blk, local); __syncthreads();
    if (threadIdx.x == 0) atomicMax(out, blk);
}
// double predicate-filtered reductions. out is `double*`; sentinels match matrix_cpu_reduce_pred_f64
// (COUNT/SUM 0.0, MIN +inf, MAX -inf — caller cudaMemcpy's the init). atomicAdd(double*) needs arch>=6.0
// (T4 is 7.5); MIN/MAX use the CAS-loop helpers above.
__global__ void matrix_count_kernel_pred_f64(const double* d, size_t n, MatrixCmp c, double a, double b, double* out) {
    __shared__ double blk; if (threadIdx.x == 0) blk = 0.0; __syncthreads();
    double local = 0.0;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride)
        if (matrix_pred_match_f64_dev(d[i], c, a, b)) local += 1.0;
    atomicAdd(&blk, local); __syncthreads();
    if (threadIdx.x == 0) atomicAdd(out, blk);
}
__global__ void matrix_sum_kernel_pred_f64(const double* d, size_t n, MatrixCmp c, double a, double b, double* out) {
    __shared__ double blk; if (threadIdx.x == 0) blk = 0.0; __syncthreads();
    double local = 0.0;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride)
        if (matrix_pred_match_f64_dev(d[i], c, a, b)) local += d[i];
    atomicAdd(&blk, local); __syncthreads();
    if (threadIdx.x == 0) atomicAdd(out, blk);
}
__global__ void matrix_min_kernel_pred_f64(const double* d, size_t n, MatrixCmp c, double a, double b, double* out) {
    __shared__ double blk; if (threadIdx.x == 0) blk = MATRIX_DEV_POSINF; __syncthreads();
    double local = MATRIX_DEV_POSINF;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride)
        if (matrix_pred_match_f64_dev(d[i], c, a, b) && d[i] < local) local = d[i];
    atomicMinDouble(&blk, local); __syncthreads();
    if (threadIdx.x == 0) atomicMinDouble(out, blk);
}
__global__ void matrix_max_kernel_pred_f64(const double* d, size_t n, MatrixCmp c, double a, double b, double* out) {
    __shared__ double blk; if (threadIdx.x == 0) blk = MATRIX_DEV_NEGINF; __syncthreads();
    double local = MATRIX_DEV_NEGINF;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride)
        if (matrix_pred_match_f64_dev(d[i], c, a, b) && d[i] > local) local = d[i];
    atomicMaxDouble(&blk, local); __syncthreads();
    if (threadIdx.x == 0) atomicMaxDouble(out, blk);
}
// Vectorized uint32 scan: each thread loads 4 values per instruction via uint4
// (LDG.128 = 16 bytes/load). This is the standard memory-bound fix across DL/DB/HPC
// kernels — more bytes in flight per thread => deeper memory-level parallelism =>
// closer to peak DRAM bandwidth than the scalar 4-byte load. Requires n % 4 == 0 and
// 16-byte-aligned data (cudaMalloc guarantees alignment; n is a power of two here).
__global__ void matrix_scan_kernel_u32x4(const uint4* data4, size_t n4,
                                         uint32_t threshold, unsigned long long* count) {
    __shared__ unsigned long long block_count;
    if (threadIdx.x == 0) block_count = 0;
    __syncthreads();

    unsigned long long local = 0;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n4; i += stride) {
        const uint4 v = data4[i]; // one 16-byte load -> 4 comparisons
        local += (v.x > threshold) + (v.y > threshold)
               + (v.z > threshold) + (v.w > threshold);
    }
    atomicAdd(&block_count, local);
    __syncthreads();
    if (threadIdx.x == 0) atomicAdd(count, block_count);
}

// Items-per-thread uint32 scan (register blocking) — the lever CUB's BlockReduce uses
// (128 threads x 4 items each). Each thread issues ITEMS independent loads into a
// register array BEFORE comparing any, so multiple loads are in flight per thread =>
// DRAM latency is hidden => bandwidth saturates. Unlike uint4 this needs no special
// type/alignment and doesn't inflate per-load register width. Strided access keeps the
// warp's ITEMS loads coalesced (lane L reads base+L, base+L+blockDim, ...).
template <int ITEMS>
__global__ void matrix_scan_kernel_ipt(const uint32_t* data, size_t n,
                                       uint32_t threshold, unsigned long long* count) {
    __shared__ unsigned long long block_count;
    if (threadIdx.x == 0) block_count = 0;
    __syncthreads();

    unsigned long long local = 0;
    const size_t base_stride = (size_t)gridDim.x * blockDim.x;
    const size_t chunk = base_stride * ITEMS;
    // Striped layout: thread's global id is the base; item k is base + k*base_stride.
    // (base init must NOT include *ITEMS — that mixes the blocked convention with the
    // striped strides below and leaves most indices unvisited. Verified by
    // test_scan_coverage.cpp.)
    for (size_t base = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
         base < n; base += chunk) {
        uint32_t reg[ITEMS];
        #pragma unroll
        for (int k = 0; k < ITEMS; ++k) {
            const size_t idx = base + (size_t)k * base_stride;
            reg[k] = (idx < n) ? data[idx] : 0u; // load all ITEMS first (in flight)
        }
        #pragma unroll
        for (int k = 0; k < ITEMS; ++k) {
            const size_t idx = base + (size_t)k * base_stride;
            if (idx < n) local += (reg[k] > threshold); // then compare
        }
    }
    atomicAdd(&block_count, local);
    __syncthreads();
    if (threadIdx.x == 0) atomicAdd(count, block_count);
}

/**
 * @brief Real CUDA GPU engine, page-ownership model. Device-resident store persists
 * across batches (it is the database). Same ComputeInterface + correctness contract
 * as CPUMockEngine.
 */
class CUDAGPUEngine : public ComputeInterface {
public:
    explicit CUDAGPUEngine(size_t /*worker_count*/ = 0)
        : h_binned_(MATRIX_BATCH_MAX) {
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
        CUDA_CHECK(cudaMalloc(&d_binned_, MATRIX_BATCH_MAX * sizeof(DatabaseQuery)));
        CUDA_CHECK(cudaMalloc(&d_offsets_, (MATRIX_PAGE_COUNT + 1) * sizeof(uint32_t)));
        CUDA_CHECK(cudaMalloc(&d_reads_, sizeof(unsigned long long)));
        CUDA_CHECK(cudaMalloc(&d_writes_, sizeof(unsigned long long)));
        CUDA_CHECK(cudaMemset(d_reads_, 0, sizeof(unsigned long long)));
        CUDA_CHECK(cudaMemset(d_writes_, 0, sizeof(unsigned long long)));

        // Resident analytical column (value[i]=i), filled once, scanned in place by
        // OP_SCAN. This is the GPU-DB's actual data — never shipped per query.
        CUDA_CHECK(cudaMalloc(&d_scan_col_, MATRIX_SCAN_COLUMN_SIZE * sizeof(uint32_t)));
        {
            std::vector<uint32_t> h(MATRIX_SCAN_COLUMN_SIZE);
            for (size_t i = 0; i < MATRIX_SCAN_COLUMN_SIZE; ++i) h[i] = static_cast<uint32_t>(i);
            CUDA_CHECK(cudaMemcpy(d_scan_col_, h.data(),
                                  MATRIX_SCAN_COLUMN_SIZE * sizeof(uint32_t),
                                  cudaMemcpyHostToDevice));
        }
        CUDA_CHECK(cudaMalloc(&d_scan_count_, sizeof(unsigned long long)));
        CUDA_CHECK(cudaMemset(d_scan_count_, 0, sizeof(unsigned long long)));

        offsets_.resize(MATRIX_PAGE_COUNT + 1);

        // ponytail: single stream. Hyper-Q multi-stream is the throughput upgrade.
        CUDA_CHECK(cudaStreamCreate(&stream_));
        // Dedicated stream for scans so they overlap point-op work and don't serialize
        // behind it (the HTAP separation, GPU side).
        CUDA_CHECK(cudaStreamCreate(&scan_stream_));
        // Reused events to time just the scan kernel (per-scan create/destroy would
        // itself add overhead and pollute the measurement).
        CUDA_CHECK(cudaEventCreate(&scan_k0_));
        CUDA_CHECK(cudaEventCreate(&scan_k1_));
        std::printf("CUDAGPUEngine initialized on '%s' (%d SMs, page-ownership, %zu pages).\n",
                    prop.name, prop.multiProcessorCount, MATRIX_PAGE_COUNT);
    }

    ~CUDAGPUEngine() override {
        cudaFree(d_store_);
        cudaFree(d_binned_);
        cudaFree(d_offsets_);
        cudaFree(d_reads_);
        cudaFree(d_writes_);
        cudaFree(d_scan_col_);
        cudaFree(d_scan_count_);
        cudaEventDestroy(scan_k0_);
        cudaEventDestroy(scan_k1_);
        cudaStreamDestroy(stream_);
        cudaStreamDestroy(scan_stream_);
        std::printf("CUDAGPUEngine released device resources.\n");
    }

    void execute_batch(DatabaseQuery* batch_array, size_t count) override {
        if (count == 0) return;
        if (count > MATRIX_BATCH_MAX) count = MATRIX_BATCH_MAX;

        // Bin by page on the host (folds into the dual-trigger batcher later).
        matrix_bin_by_page(batch_array, count, h_binned_.data(), offsets_.data());

        CUDA_CHECK(cudaMemcpyAsync(d_binned_, h_binned_.data(), count * sizeof(DatabaseQuery),
                                   cudaMemcpyHostToDevice, stream_));
        CUDA_CHECK(cudaMemcpyAsync(d_offsets_, offsets_.data(),
                                   (MATRIX_PAGE_COUNT + 1) * sizeof(uint32_t),
                                   cudaMemcpyHostToDevice, stream_));

        // One block per page; 128 threads/block stride over the page's queries.
        // Point ops only — scans arrive via execute_scan on their own stream.
        constexpr int TPB = 128;
        matrix_page_kernel<<<MATRIX_PAGE_COUNT, TPB, 0, stream_>>>(
            d_binned_, d_offsets_, d_store_, d_reads_, d_writes_);
        CUDA_CHECK(cudaGetLastError());
        CUDA_CHECK(cudaStreamSynchronize(stream_));
    }

    void execute_scan(DatabaseQuery& q) override {
        // u32x4 filter-count over the resident column, on the dedicated scan stream so
        // it overlaps point-op submission on stream_ and never head-of-line-blocks them.
        // AGG-2 (GPU-1): dispatch on matrix_get_scan_agg_op(q). COUNT keeps the byte-identical u32x4
        // path (the 83886070 oracle); SUM/MIN/MAX init `out` to the matching CPU sentinel then launch the
        // scalar reduction kernel. Each is verified GPU==matrix_cpu_reduce by test_gpu_agg.cu.
        const auto st0 = std::chrono::steady_clock::now();
        const uint32_t threshold = matrix_get_scan_threshold(q);
        const MatrixAggOp op = matrix_get_scan_agg_op(q);
        constexpr int SCAN_TPB = 256;
        const int blocks = 1024;
        CUDA_CHECK(cudaEventRecord(scan_k0_, scan_stream_));
        if (op == AGG_SUM) {
            CUDA_CHECK(cudaMemsetAsync(d_scan_count_, 0, sizeof(unsigned long long), scan_stream_));  // SUM sentinel 0
            matrix_sum_kernel_u32<<<blocks, SCAN_TPB, 0, scan_stream_>>>(d_scan_col_, MATRIX_SCAN_COLUMN_SIZE, threshold, d_scan_count_);
        } else if (op == AGG_MIN) {
            CUDA_CHECK(cudaMemsetAsync(d_scan_count_, 0xFF, sizeof(unsigned long long), scan_stream_)); // MIN sentinel UINT64_MAX
            matrix_min_kernel_u32<<<blocks, SCAN_TPB, 0, scan_stream_>>>(d_scan_col_, MATRIX_SCAN_COLUMN_SIZE, threshold, d_scan_count_);
        } else if (op == AGG_MAX) {
            CUDA_CHECK(cudaMemsetAsync(d_scan_count_, 0, sizeof(unsigned long long), scan_stream_));    // MAX sentinel 0
            matrix_max_kernel_u32<<<blocks, SCAN_TPB, 0, scan_stream_>>>(d_scan_col_, MATRIX_SCAN_COLUMN_SIZE, threshold, d_scan_count_);
        } else {                                                                                       // AGG_COUNT (default) — unchanged
            CUDA_CHECK(cudaMemsetAsync(d_scan_count_, 0, sizeof(unsigned long long), scan_stream_));
            const uint4* col4 = reinterpret_cast<const uint4*>(d_scan_col_);
            matrix_scan_kernel_u32x4<<<blocks, SCAN_TPB, 0, scan_stream_>>>(
                col4, MATRIX_SCAN_COLUMN_SIZE / 4, threshold, d_scan_count_);
        }
        CUDA_CHECK(cudaEventRecord(scan_k1_, scan_stream_));
        CUDA_CHECK(cudaGetLastError());
        unsigned long long c = 0;
        CUDA_CHECK(cudaMemcpyAsync(&c, d_scan_count_, sizeof(c),
                                   cudaMemcpyDeviceToHost, scan_stream_));
        CUDA_CHECK(cudaStreamSynchronize(scan_stream_));
        scan_time_s_ += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - st0).count();
        float k_ms = 0.0f;
        CUDA_CHECK(cudaEventElapsedTime(&k_ms, scan_k0_, scan_k1_));
        scan_kernel_time_s_ += k_ms / 1e3;
        q.transaction_id = c;
        ++scans_;
        scan_result_sum_ += c;
    }

    uint64_t reads() const override { return read_counter(d_reads_); }
    uint64_t writes() const override { return read_counter(d_writes_); }
    uint64_t commits() const override { return read_counter(d_writes_); } // every write commits (owned slot)
    uint64_t scans() const override { return scans_; }
    uint64_t scan_result_sum() const override { return scan_result_sum_; }
    double scan_time_s() const override { return scan_time_s_; }
    double scan_kernel_time_s() const override { return scan_kernel_time_s_; }

    uint64_t store_checksum() const override {
        // ponytail: copy the whole store back (32KB) and sum on host. Once, off the
        // hot path — a device reduction would be premature for a correctness check.
        std::vector<uint64_t> h(MATRIX_STORE_SLOTS);
        CUDA_CHECK(cudaMemcpy(h.data(), d_store_, MATRIX_STORE_SLOTS * sizeof(uint64_t),
                              cudaMemcpyDeviceToHost));
        uint64_t sum = 0;
        for (uint64_t v : h) sum += v;
        return sum;
    }

    double benchmark_scan(size_t n, uint64_t threshold, uint64_t& out_count) override {
        return timed_scan<uint64_t>(n, threshold, out_count,
            [](const uint64_t* d, size_t nn, uint64_t thr, unsigned long long* c,
               int blocks, int tpb, cudaStream_t s) {
                matrix_scan_kernel<<<blocks, tpb, 0, s>>>(d, nn, thr, c);
            });
    }

    double benchmark_scan_u32(size_t n, uint32_t threshold, uint64_t& out_count) override {
        return timed_scan<uint32_t>(n, threshold, out_count,
            [](const uint32_t* d, size_t nn, uint32_t thr, unsigned long long* c,
               int blocks, int tpb, cudaStream_t s) {
                matrix_scan_kernel_u32<<<blocks, tpb, 0, s>>>(d, nn, thr, c);
            });
    }

    double benchmark_scan_u32x4(size_t n, uint32_t threshold, uint64_t& out_count) override {
        // Same resident-column setup, but the kernel reads 4 u32 per uint4 load.
        // n is a power of two (>= 65536) so n % 4 == 0; cudaMalloc is 256-byte aligned.
        return timed_scan<uint32_t>(n, threshold, out_count,
            [](const uint32_t* d, size_t nn, uint32_t thr, unsigned long long* c,
               int blocks, int tpb, cudaStream_t s) {
                const uint4* d4 = reinterpret_cast<const uint4*>(d);
                matrix_scan_kernel_u32x4<<<blocks, tpb, 0, s>>>(d4, nn / 4, thr, c);
            });
    }

    double benchmark_scan_ipt(size_t n, uint32_t threshold, uint64_t& out_count) override {
        return timed_scan<uint32_t>(n, threshold, out_count,
            [](const uint32_t* d, size_t nn, uint32_t thr, unsigned long long* c,
               int blocks, int tpb, cudaStream_t s) {
                matrix_scan_kernel_ipt<8><<<blocks, tpb, 0, s>>>(d, nn, thr, c);
            });
    }

private:
    // Shared scan harness: alloc resident column, fill (untimed), time kernel via CUDA
    // events, return seconds. Templated on column type so u32/u64 share one code path.
    template <typename T, typename Launch>
    double timed_scan(size_t n, T threshold, uint64_t& out_count, Launch launch) {
        T* d_data = nullptr;
        unsigned long long* d_count = nullptr;
        CUDA_CHECK(cudaMalloc(&d_data, n * sizeof(T)));
        CUDA_CHECK(cudaMalloc(&d_count, sizeof(unsigned long long)));
        CUDA_CHECK(cudaMemset(d_count, 0, sizeof(unsigned long long)));
        {
            std::vector<T> h(n);
            for (size_t i = 0; i < n; ++i) h[i] = static_cast<T>(i);
            CUDA_CHECK(cudaMemcpy(d_data, h.data(), n * sizeof(T), cudaMemcpyHostToDevice));
        }

        constexpr int TPB = 256;
        const int blocks = 1024; // saturate the 40 SMs; grid-stride handles any n
        cudaEvent_t start, stop;
        CUDA_CHECK(cudaEventCreate(&start));
        CUDA_CHECK(cudaEventCreate(&stop));
        CUDA_CHECK(cudaEventRecord(start, stream_));
        launch(d_data, n, threshold, d_count, blocks, TPB, stream_);
        CUDA_CHECK(cudaEventRecord(stop, stream_));
        CUDA_CHECK(cudaEventSynchronize(stop));
        float ms = 0.0f;
        CUDA_CHECK(cudaEventElapsedTime(&ms, start, stop));

        unsigned long long h_count = 0;
        CUDA_CHECK(cudaMemcpy(&h_count, d_count, sizeof(h_count), cudaMemcpyDeviceToHost));
        out_count = h_count;

        cudaEventDestroy(start);
        cudaEventDestroy(stop);
        cudaFree(d_data);
        cudaFree(d_count);
        return ms / 1e3; // seconds
    }

    static uint64_t read_counter(const unsigned long long* d_ptr) {
        unsigned long long h = 0;
        CUDA_CHECK(cudaMemcpy(&h, d_ptr, sizeof(h), cudaMemcpyDeviceToHost));
        return static_cast<uint64_t>(h);
    }

    uint64_t* d_store_ = nullptr;
    DatabaseQuery* d_binned_ = nullptr;
    uint32_t* d_offsets_ = nullptr;
    unsigned long long* d_reads_ = nullptr;
    unsigned long long* d_writes_ = nullptr;
    uint32_t* d_scan_col_ = nullptr;            // resident analytical column
    unsigned long long* d_scan_count_ = nullptr; // scratch for one scan's count
    uint64_t scans_ = 0;                          // host-side scan accounting
    uint64_t scan_result_sum_ = 0;
    double scan_time_s_ = 0.0;
    double scan_kernel_time_s_ = 0.0;            // cudaEvent-measured pure kernel time
    cudaEvent_t scan_k0_{}, scan_k1_{};          // reused scan-kernel timing events
    std::vector<DatabaseQuery> h_binned_;
    std::vector<uint32_t> offsets_;
    cudaStream_t stream_{};
    cudaStream_t scan_stream_{};                  // dedicated scan path (HTAP)
};

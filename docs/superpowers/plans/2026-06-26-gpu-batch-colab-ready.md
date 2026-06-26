# GPU Batch — Colab-Ready Plan (AGG-2 + GPU GROUP BY + VRAM Promotion)

**Status:** research + plan complete; **implementation + verification are Colab/nvcc-only** and
must NOT be merged to `main` until the cross-backend invariant passes on real hardware.
**Date:** 2026-06-26.

**Why this is a plan, not a merged increment:** CUDA kernel-launch syntax (`kernel<<<...>>>(...)`)
is not compilable by clang/g++, and there is no `nvcc` in the autonomous environment — so GPU code
**cannot be syntax-checked, run, or verified locally.** Landing unverified GPU code on `main` would
violate the project's verification discipline. This doc makes the GPU work **turnkey**: copy the
kernels in, run the cross-backend cell on Colab (Tesla T4), and merge only when `GPU == matrix_cpu_*`.

**The correctness anchor (every item below uses it):** the CPU reducers (`matrix_cpu_reduce`,
`matrix_cpu_group_reduce`, `matrix_cpu_reduce_all`) are the semantics-of-record. Each GPU kernel is
correct iff, over the *same bytes*, its result equals the CPU reducer's — the same cross-backend
checksum invariant that anchored the page-ownership and u32x4-scan work.

---

## GPU-1 (AGG-2): SUM / MIN / MAX reduction kernels

The existing `matrix_scan_kernel_u32` (compute_cuda.cuh) is COUNT. SUM/MIN/MAX are the same
grid-stride + block-local + one-atomic-per-block shape, differing only in the accumulator and the
atomic. Add three kernels + dispatch `execute_scan` on `matrix_get_scan_agg_op(q)`.

```cuda
// SUM of values > threshold. atomicAdd into a u64 (matches matrix_cpu_reduce AGG_SUM).
__global__ void matrix_sum_kernel_u32(const uint32_t* data, size_t n,
                                      uint32_t threshold, unsigned long long* out) {
    __shared__ unsigned long long blk; if (threadIdx.x == 0) blk = 0; __syncthreads();
    unsigned long long local = 0;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x*blockDim.x+threadIdx.x; i < n; i += stride)
        if (data[i] > threshold) local += data[i];
    atomicAdd(&blk, local); __syncthreads();
    if (threadIdx.x == 0) atomicAdd(out, blk);
}
// MIN of values > threshold. Init out = UINT64_MAX (matches the CPU empty sentinel).
__global__ void matrix_min_kernel_u32(const uint32_t* data, size_t n,
                                      uint32_t threshold, unsigned long long* out) {
    __shared__ unsigned long long blk; if (threadIdx.x == 0) blk = ULLONG_MAX; __syncthreads();
    unsigned long long local = ULLONG_MAX;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x*blockDim.x+threadIdx.x; i < n; i += stride)
        if (data[i] > threshold && data[i] < local) local = data[i];
    atomicMin(&blk, local); __syncthreads();
    if (threadIdx.x == 0) atomicMin(out, blk);
}
// MAX of values > threshold. Init out = 0 (matches the CPU empty sentinel).
__global__ void matrix_max_kernel_u32(const uint32_t* data, size_t n,
                                      uint32_t threshold, unsigned long long* out) {
    __shared__ unsigned long long blk; if (threadIdx.x == 0) blk = 0; __syncthreads();
    unsigned long long local = 0;
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x*blockDim.x+threadIdx.x; i < n; i += stride)
        if (data[i] > threshold && data[i] > local) local = data[i];
    atomicMax(&blk, local); __syncthreads();
    if (threadIdx.x == 0) atomicMax(out, blk);
}
```
- `atomicMin`/`atomicMax` on `unsigned long long` require `__CUDA_ARCH__ >= 350` (T4 is 7.5 — fine).
- `ULLONG_MAX` needs `<climits>` (or use `0xFFFFFFFFFFFFFFFFull`). Init the device `out` to the same
  sentinel as the block-local before launch (`cudaMemset` works for 0/MAX: MAX = memset 0xFF).
- **execute_scan dispatch:** read `matrix_get_scan_agg_op(q)`; for AGG_COUNT keep the existing
  `matrix_scan_kernel_u32x4`; for SUM/MIN/MAX init `out` to the sentinel and launch the matching
  kernel. Remove the `NOTE(AGG-2)` comment once done.
- **Verify (Colab cell):** build a uint32 column on host, run each op on the GPU over the device
  copy, assert `gpu_result == matrix_cpu_reduce(host, n, threshold, op)` for COUNT/SUM/MIN/MAX +
  an empty-match threshold (verifies the sentinels). This is the merge gate.

## GPU-2: grouped reduction (GROUP BY on the GPU)

Per-group accumulators in VRAM (dense `[0, num_groups)`), one atomic per row into its group's slot.

```cuda
// out[num_groups] pre-initialized per op (COUNT/SUM/MAX -> 0, MIN -> ULLONG_MAX) via cudaMemset.
__global__ void matrix_group_sum_kernel(const uint32_t* keys, const uint32_t* vals, size_t n,
                                        uint32_t num_groups, unsigned long long* out) {
    const size_t stride = (size_t)gridDim.x * blockDim.x;
    for (size_t i = (size_t)blockIdx.x*blockDim.x+threadIdx.x; i < n; i += stride) {
        const uint32_t k = keys[i]; if (k < num_groups) atomicAdd(&out[k], (unsigned long long)vals[i]);
    }
}
// COUNT: atomicAdd(&out[k], 1ull). MIN/MAX: atomicMin/atomicMax(&out[k], vals[i]).
```
- Matches `matrix_cpu_group_reduce`. Filtered variant adds `if (vals[i] <= threshold) continue;`
  (matches `matrix_cpu_group_reduce_where`). For high group counts, a shared-memory privatized
  histogram per block (then one atomic per group to global) reduces global-atomic contention — a
  perf follow-up; the simple global-atomic version is correct first.
- **Verify:** `gpu_group_result == matrix_cpu_group_reduce(keys, vals, n, num_groups, op)` (and the
  `_where` variant). Merge gate.

## GPU-3: DEVICE/VRAM catalog promotion (the 24× scan payoff)

Wire the tiered catalog into the CUDA engine so hot columns auto-promote to VRAM and are scanned
there at ~240 GB/s. The mechanics are **already proven** (Inc 4: a VRAM-promoted `TieredColumn` is
GPU-scannable via `device_ptr()` + `matrix_scan_kernel_u32x4`). The remaining work:
- The CUDA `CPUMockEngine`-equivalent (or the GPU engine) constructs its `TierManager` with the
  **real VRAM budget** as `device_capacity_bytes` (instead of the CPU build's `device_cap=1` that
  makes DEVICE inert). Now `should_promote` HOST→DEVICE fires for hot columns and the executor's
  `migrate_to(DEVICE)` (cudaMemcpy H→D) runs — no abort, because this is the CUDA build.
- Scans of a DEVICE-resident column run the GPU kernel over `device_ptr()`; HOST/COLD columns use
  the CPU path (or borrow up). The 3-tier ladder (COLD→HOST→DEVICE) is now fully live.
- **Verify:** a hot column promotes to DEVICE (tier()==DEVICE), a GPU scan/agg over it equals the
  CPU reducer over the same bytes, and the heat→promote→VRAM-scan loop measurably beats the CPU
  scan at ≥ the crossover size (~4–8 MB). This is the thesis result — capture the GB/s.

---

## Execution checklist (on Colab, in order)
1. Open `matrixdb_colab.ipynb` (it embeds all sources incl. the CPU reducers as the oracle).
2. Add the GPU-1 kernels to `compute_cuda.cuh`; build `-DMATRIX_USE_CUDA`; run the cross-backend
   cell (GPU agg == CPU agg for all 4 ops + empty). Green → the SUM/MIN/MAX kernels are correct.
3. GPU-2 grouped kernels; cross-backend cell vs `matrix_cpu_group_reduce(_where)`. Green.
4. GPU-3 VRAM promotion; assert promote + GPU==CPU + capture the GB/s vs CPU (the 24× claim).
5. Only after each cell is green, commit that piece to `main` (now hardware-verified).

Each piece is independently verifiable and independently mergeable once green — no big-bang.

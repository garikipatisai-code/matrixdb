# GPU Batch — Colab-Ready Plan (AGG-2 + GPU GROUP BY + VRAM Promotion + Predicates + Typed Columns)

**Status:** research + plan complete; **implementation + verification are Colab/nvcc-only** and
must NOT be merged to `main` until the cross-backend invariant passes on real hardware.
**Date:** 2026-06-26; **extended 2026-06-27** with GPU-4 (richer predicates, QRY-3) + GPU-5 (typed
int64/double columns, DM-3) so the GPU phase covers the full current CPU engine.

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

## GPU-4: richer scan predicates (QRY-3 parity)

Since this plan was written, the CPU gained `MatrixCmp` (GT/GE/LT/LE/EQ/NE/BETWEEN) +
`matrix_pred_match` + `matrix_cpu_reduce_pred` (QRY-3). The GPU filter is currently hardcoded
`data[i] > threshold`. Replace it with a `__device__` predicate so GPU scans match the CPU's WHERE.

```cuda
enum class MatrixCmp : uint32_t { GT=0, GE, LT, LE, EQ, NE, BETWEEN };   // mirror compute.hpp
__device__ __forceinline__ bool matrix_pred_match_dev(uint32_t v, MatrixCmp c, uint32_t a, uint32_t b) {
    switch (c) { case MatrixCmp::GE: return v>=a; case MatrixCmp::LT: return v<a; case MatrixCmp::LE: return v<=a;
                 case MatrixCmp::EQ: return v==a; case MatrixCmp::NE: return v!=a;
                 case MatrixCmp::BETWEEN: return v>=a && v<=b; default: return v>a; }   // GT
}
// Each GPU-1 kernel takes (MatrixCmp cmp, uint32_t a, uint32_t b) instead of (threshold) and replaces
// `data[i] > threshold` with `matrix_pred_match_dev(data[i], cmp, a, b)`. (Pass the predicate by value.)
```
- **execute_query/scan dispatch** builds `{cmp, a, b}` from the query (the CPU already does this) and
  launches the predicate kernel. COUNT/SUM/MIN/MAX share the one predicate.
- **Verify:** for every `cmp` × {COUNT,SUM,MIN,MAX}, `gpu == matrix_cpu_reduce_pred(host, n, {cmp,a,b}, op)`
  — including a 0-matching predicate (sentinel check). Merge gate. (MAX empty-sentinel `0` is ambiguous
  for 0-matching predicates exactly as on the CPU — same documented behavior, so the invariant still holds.)

## GPU-5: typed columns — int64 + double (DM-3 parity)

The CPU now has `int64`/`double` columns with scalar (filtered/unfiltered) + grouped reducers
(`matrix_cpu_reduce_all_i64`/`_pred_i64`, `matrix_cpu_reduce_all_f64`/`_pred_f64`,
`matrix_group_reduce_i64`/`_f64`). The GPU kernels are the same grid-stride shape over the typed pointer;
the column's `MatrixType` (carried in the catalog) selects which kernel to launch.

```cuda
// int64: signed atomics. atomicMin/Max exist for long long on arch>=3.5 (T4 7.5 ok). SUM via atomicAdd
// on unsigned long long reinterpreted (two's-complement add is bit-identical) or use long long atomicAdd.
__global__ void matrix_sum_kernel_i64(const long long* d, size_t n, MatrixCmp c, long long a, long long b,
                                      long long* out) { /* local += d[i] if pred; atomicAdd(out, local) */ }
// MIN init LLONG_MAX, MAX init LLONG_MIN (match matrix_cpu_reduce_all_i64 sentinels).

// double: atomicAdd(double*) needs arch>=6.0 (T4 7.5 ok). MIN/MAX have no native double atomic — use a
// CAS loop (atomicCAS on the unsigned long long bit-pattern) or a two-pass reduction. Empty sentinels
// MIN +inf, MAX -inf (match matrix_cpu_reduce_all_f64). NaN: IEEE compares false — same as CPU.
__global__ void matrix_sum_kernel_f64(const double* d, size_t n, MatrixCmp c, double a, double b, double* out);
```
- The predicate (GPU-4) is templated/duplicated per element type (`matrix_pred_match_dev` for int64/double
  with signed/float compares — the CPU's `matrix_pred_match_i64`/`_f64`).
- **Verify:** `gpu_i64 == matrix_cpu_reduce_all_i64 / _pred_i64` and `gpu_f64 == matrix_cpu_reduce_*_f64`
  over the same bytes, with negatives + `> UINT32_MAX` + fractional data (the CPU tests' datasets). For
  double, compare bit-patterns of exactly-representable inputs so `==` is exact. Grouped: vs
  `matrix_group_reduce_i64`/`_f64`. Each is an independent merge gate.
- **VRAM promotion (GPU-3) is type-agnostic** — `TieredColumn` moves raw bytes, so a promoted int64/double
  column is already device-resident; only the scan kernel selection (by `MatrixType`) is new here.

---

## Execution checklist (on Colab, in order)
1. Open `matrixdb_colab.ipynb` (it embeds all sources incl. the CPU reducers as the oracle).
2. **✅ DONE (Colab T4, hardware-verified):** GPU-1 SUM/MIN/MAX (u32, GT); cross-backend cell `test_gpu_agg.cu` vs `matrix_cpu_reduce` — GREEN (incl. >2^32 SUM + empty sentinels). Merged to `main`. The same run exposed + fixed a C++17/`std::bit_cast` regression in the pipeline build (now `matrix_bit_cast`, memcpy-based).
3. **✅ DONE (Colab T4):** GPU-4 predicates — `test_gpu_pred.cu` vs `matrix_cpu_reduce_pred`, every op × {COUNT,SUM,MIN,MAX} + empty-match — GREEN. Merged. Additive `matrix_{count,sum,min,max}_kernel_pred_u32` + `matrix_pred_match_dev`.
4. **✅ DONE (Colab T4):** GPU-2 grouped — `test_gpu_grouped.cu` vs `matrix_cpu_group_reduce` (incl. out-of-range keys) — GREEN. Merged. Additive `matrix_group_{count,sum,min,max}_kernel`.
5. **✅ DONE (Colab T4):** GPU-5 typed int64+double — `test_gpu_typed.cu` vs `matrix_cpu_reduce_pred_i64`/`_f64`, every op × {COUNT,SUM,MIN,MAX} incl. negatives, >2^32, fractions, half-integer exact SUM, inf/INT64 sentinels — GREEN. Merged. Additive `matrix_{count,sum,min,max}_kernel_pred_{i64,f64}` + `atomicMinDouble`/`atomicMaxDouble` CAS helpers + a `sm_60`-scoped CAS-fallback `atomicAdd(double*)`. (Two build fixes this run: native double `atomicAdd` absent under the default arch, then a host-pass redefinition — final guard `defined(__CUDA_ARCH__) && __CUDA_ARCH__ < 600`.)
6. **✅ DONE (Colab T4):** GPU-3 VRAM *catalog* promotion — `test_gpu_catalog.cu` (cell 4g) GREEN: `execute_query` over a DEVICE-pinned catalog column == `matrix_cpu_reduce_*` for u32/i64/f64 × {COUNT,SUM,MIN,MAX} × {filtered,unfiltered}. cell 4 live engine: hot analytical cols auto-promote to VRAM (`col1=DEVICE col2=DEVICE col3=HOST`, migrations=2, cold_borrows=0), all sums correct, oracle 83886070, legacy resident scan 142.6 GB/s. Merged. Additive `matrix_gpu_reduce_dev_{u32,i64,f64}` + a real device budget/`pin_device` in `CPUMockEngine` (all behind `#if MATRIX_USE_CUDA`). **Deferred (YAGNI):** a dedicated device-GB/s capture for the analytical path (the legacy resident scan's 142.6 GB/s already shows the bandwidth).
7. Commit each piece to `main` only after its cell is green (now hardware-verified). No big-bang.
8. **✅ DONE (Colab T4):** grouped-on-device — `test_gpu_catalog_grouped.cu` (cell 4h) GREEN: GPU `GROUP BY` over DEVICE-resident key+value == `matrix_cpu_group_reduce[_i64/_f64](_pred)` for u32/i64/f64 × {COUNT,SUM,MIN,MAX} × {filtered,unfiltered} — 24 per-group cross-checks. Additive predicate-gated grouped kernels (u32 + typed) reusing the GPU-5 atomics + `matrix_pred_match_*_dev`; `matrix_gpu_group_reduce_dev{,_i64,_f64}` wrappers; DEVICE branch in the u32/i64/f64 grouped paths. Merged.

**STATUS: GPU phase COMPLETE.** All of GPU-1/2/3/4/5 + grouped-on-device are hardware-verified on a Colab T4 and merged to `main`. Point-ops stay on CPU (thesis); the full **scalar AND grouped** analytical surface (COUNT/SUM/MIN/MAX, u32/i64/f64, filtered + unfiltered, GROUP BY) runs in VRAM end-to-end, cross-checked against the CPU oracle. No bounded GPU increment remains.

Each piece is independently verifiable and independently mergeable once green.

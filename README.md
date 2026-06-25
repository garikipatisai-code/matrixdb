# MatrixDB Prototype

Low-latency batched query engine from `MatrixDB Engine Architectural Specification.md`.
One ingestion pipeline (lock-free SPSC ring → dual-trigger batching), two interchangeable
compute backends selected at compile time:

| Backend | When | Build |
|---|---|---|
| `CPUMockEngine` | default, no GPU | `clang++`/`g++` |
| `CUDAGPUEngine` | real GPU (Colab, etc.) | `nvcc -DMATRIX_USE_CUDA` |

Both honor the same `ComputeInterface` and the **same correctness asserts** in `main.cpp`,
so "passes" means the same thing on CPU and GPU: every query dispatched on its opcode,
every Delta Log mutation committed, zero drops.

## Files
- `types.hpp` — `DatabaseQuery` (cache-aligned POD), opcodes, shared store/Delta-Log layout
- `ring_buffer.hpp` — lock-free SPSC ring (Component 1)
- `compute.hpp` — `ComputeInterface` contract
- `compute_mock.cpp` — CPU engine (Component 5: local sandbox)
- `compute_cuda.cuh` — CUDA engine: 1 thread/query dispatch, `atomicAdd` Delta Log, reconcile kernel (Component 4)
- `main.cpp` — orchestration + latency benchmark
- `CMakeLists.txt` — CPU build (CUDA uses the one-liner below)

## Build & run locally (CPU)
```sh
clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o matrixdb_proto   # Apple Silicon
# g++ -std=c++20 -O3 -march=native main.cpp -o matrixdb_proto      # Linux x86_64
./matrixdb_proto
```

## Test on Google Colab (GPU)

**Easiest:** upload `matrixdb_colab.ipynb` to Colab (or open from your repo). It writes
its own source, builds, and runs — no manual file uploads. Set Runtime → T4 GPU first.

**Manual** — Runtime → Change runtime type → **T4 GPU**, then:

```python
# Cell 1 — confirm a GPU is attached
!nvcc --version && nvidia-smi --query-gpu=name --format=csv,noheader
```

```python
# Cell 2 — upload all source files via the Files pane (or clone your repo), then:
!nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread -DMATRIX_USE_CUDA main.cpp -o matrixdb_proto
!./matrixdb_proto
```

Expected output ends with `reads=5000 writes=5000 delta_applied=5000` and
`Engine execution loop completed successfully.` (the asserts fire on any drop).

> `nvcc -x cu` compiles `main.cpp` as CUDA so the `.cuh` kernels link in.
> `-std=c++17` because some nvcc versions lag on C++20.
> `-D_GNU_SOURCE -Xcompiler -pthread` expose Linux thread-affinity + std::thread.

The notebook is generated from the real source by `make_notebook.py` — re-run it after
editing any source file so the notebook stays in sync.

## Built vs. deferred
Built: lock-free ingestion, dual-trigger batching, opcode dispatch, append-only Delta Log,
reconcile/commit, real CUDA parallel engine (verified on a Tesla T4), and three honest
metrics — raw SPSC handoff latency (sub-microsecond: ~167ns p50 / 250ns p99 on Apple M-series),
end-to-end throughput (ops/sec), and queue residency under burst.

Deferred (marked `// ponytail:` at the upgrade site): full OCC (TEV lock-bit + read-set
validation — only needed once keys collide within a concurrent batch), Hyper-Q multi-stream,
`cudaHostRegister` pinned-DMA, true columnar split (Key/Op/Value arrays).

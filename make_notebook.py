#!/usr/bin/env python3
# ponytail: regenerates matrixdb_colab.ipynb from the real source files so the
# notebook can never drift from the code. Re-run after editing any embedded source.
import json

SOURCES = ["types.hpp", "ring_buffer.hpp", "compute.hpp",
           "memory_model.hpp", "tier_model.hpp", "cost_model.hpp", "router.hpp",
           "kv_store.hpp", "cold_store.hpp", "tier_manager.hpp",
           "compute_mock.cpp", "compute_cuda.cuh", "main.cpp",
           "test_scan_coverage.cpp", "test_cost_model.cpp", "test_kv_store.cpp",
           "test_tier_manager.cpp", "test_cold_store.cpp", "test_engine_restart.cpp"]

def code(src):
    return {"cell_type": "code", "metadata": {}, "execution_count": None,
            "outputs": [], "source": src}

def md(src):
    return {"cell_type": "markdown", "metadata": {}, "source": src}

cells = [
    md("# MatrixDB on Google Colab\n"
       "\n"
       "GPU-accelerated database engine: page-ownership point ops + a resident-column "
       "`u32x4` scan kernel, behind a lock-free SPSC ring + dual-trigger batcher.\n"
       "\n"
       "**Before running:** Runtime -> Change runtime type -> **T4 GPU**.\n"
       "\n"
       "This notebook writes its own source files (cells below), runs the CPU coverage "
       "test, builds with `nvcc`, and runs. No uploads needed. Success ends with "
       "`Scan result sum: 83886070 (oracle 83886070)` — asserts fire on any mismatch."),
    md("## 1. Confirm a GPU is attached"),
    code("!nvcc --version\n"
         "!nvidia-smi --query-gpu=name,memory.total --format=csv,noheader"),
    md("## 2. Write the source files"),
]

for f in SOURCES:
    with open(f, "r") as fh:
        body = fh.read()
    # %%writefile magic must be the first line of the cell.
    cells.append(code("%%writefile " + f + "\n" + body))

cells += [
    md("## 3. Kernel index-coverage test (CPU, no GPU needed)\n"
       "\n"
       "Simulates the items-per-thread scan kernel's index arithmetic and asserts "
       "every index is visited exactly once. This is pure integer logic, so it runs "
       "on the CPU and catches GPU-only coverage bugs before spending a GPU build."),
    code("!g++ -std=c++17 -O2 test_scan_coverage.cpp -o /tmp/tcov && /tmp/tcov"),
    md("## 3a. KVStore unit test (CPU, no GPU)\n"
       "\n"
       "Proves the DM-1 fix: distinct colliding keys never overwrite each other, and a "
       "full table is an explicit error, not silent data loss."),
    code("!clang++ -std=c++20 -O2 test_kv_store.cpp -o /tmp/tkv 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_kv_store.cpp -o /tmp/tkv; /tmp/tkv"),
    md("## 3c. TierManager unit test (CPU, no GPU)\n"
       "\n"
       "Proves the auto-tiering brain: hot columns promote toward VRAM, cold stay put, "
       "scarce tiers evict by cost-benefit (not pure LRU), anti-thrash holds, decisions "
       "are deterministic."),
    code("!clang++ -std=c++20 -O2 test_tier_manager.cpp -o /tmp/ttm 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_tier_manager.cpp -o /tmp/ttm; /tmp/ttm"),
    md("## 3d. ColdStore WAL test (CPU, no GPU)\n"
       "\n"
       "Proves durability: append+replay round-trip, data survives a fresh ColdStore "
       "instance (restart), torn tail dropped, CRC corruption stops replay."),
    code("!clang++ -std=c++20 -O2 test_cold_store.cpp -o /tmp/tcs 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_cold_store.cpp -o /tmp/tcs; /tmp/tcs"),
    md("## 3e. Engine restart test (CPU, no GPU)\n"
       "\n"
       "End-to-end: point-op writes through one engine survive into a fresh engine on the "
       "same WAL path — MatrixDB no longer loses data on restart."),
    code("!clang++ -std=c++20 -O2 test_engine_restart.cpp -o /tmp/ter 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_engine_restart.cpp -o /tmp/ter; /tmp/ter"),
    md("## 3b. Cost-model unit test (CPU, no GPU)\n"
       "\n"
       "Pure-function check of the router's placement decisions — point ops -> HOST, "
       "small scans -> HOST, large scans -> DEVICE, monotonic crossover."),
    code("!clang++ -std=c++20 -O2 test_cost_model.cpp -o /tmp/tcm 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_cost_model.cpp -o /tmp/tcm; /tmp/tcm"),
    md("## 4. Build & run on the GPU\n"
       "\n"
       "`-x cu` compiles `main.cpp` as CUDA so the `.cuh` kernels link in. "
       "`-D_GNU_SOURCE` exposes Linux thread-affinity APIs; "
       "`-Xcompiler -pthread` links std::thread."),
    code("!nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread "
         "-DMATRIX_USE_CUDA main.cpp -o matrixdb_proto"),
    code("!./matrixdb_proto"),
    md("## 5. CPU fallback (run this if no GPU runtime is selected)\n"
       "\n"
       "Same code, same asserts, no CUDA — proves the logic without a GPU."),
    code("!g++ -std=c++17 -O3 -D_GNU_SOURCE -pthread main.cpp -o matrixdb_cpu "
         "&& ./matrixdb_cpu"),
]

nb = {
    "nbformat": 4,
    "nbformat_minor": 5,
    "metadata": {
        "accelerator": "GPU",
        "colab": {"provenance": [], "gpuType": "T4"},
        "kernelspec": {"name": "python3", "display_name": "Python 3"},
        "language_info": {"name": "python"},
    },
    "cells": cells,
}

with open("matrixdb_colab.ipynb", "w") as fh:
    json.dump(nb, fh, indent=1)
print("wrote matrixdb_colab.ipynb:", len(cells), "cells,", len(SOURCES), "source files embedded")

#!/usr/bin/env python3
# ponytail: regenerates matrixdb_colab.ipynb from the real source files so the
# notebook can never drift from the code. Re-run after editing any embedded source.
import json

SOURCES = ["types.hpp", "ring_buffer.hpp", "compute.hpp",
           "compute_mock.cpp", "compute_cuda.cuh", "main.cpp"]

def code(src):
    return {"cell_type": "code", "metadata": {}, "execution_count": None,
            "outputs": [], "source": src}

def md(src):
    return {"cell_type": "markdown", "metadata": {}, "source": src}

cells = [
    md("# MatrixDB on Google Colab\n"
       "\n"
       "Real CUDA backend of the MatrixDB engine: one GPU thread per query, "
       "`atomicAdd` Delta Log, reconcile kernel.\n"
       "\n"
       "**Before running:** Runtime -> Change runtime type -> **T4 GPU**.\n"
       "\n"
       "This notebook writes its own source files (cells below), builds with `nvcc`, "
       "and runs. No uploads needed. Success ends with "
       "`reads=5000 writes=5000 delta_applied=5000` — asserts fire on any drop."),
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
    md("## 3. Build & run on the GPU\n"
       "\n"
       "`-x cu` compiles `main.cpp` as CUDA so the `.cuh` kernels link in. "
       "`-D_GNU_SOURCE` exposes Linux thread-affinity APIs; "
       "`-Xcompiler -pthread` links std::thread."),
    code("!nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread "
         "-DMATRIX_USE_CUDA main.cpp -o matrixdb_proto"),
    code("!./matrixdb_proto"),
    md("## 4. CPU fallback (run this if no GPU runtime is selected)\n"
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

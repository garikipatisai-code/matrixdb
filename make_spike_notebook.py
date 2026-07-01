#!/usr/bin/env python3
# ponytail: generates network_spike_colab.ipynb from the real source files, mirroring
# make_notebook.py's discipline (the notebook can never drift from the code it measures).
# Re-run after editing spike/spike_server.cpp, spike/SpikeClient.java, or any CORE_SOURCES file.
import json

# The non-test engine files spike_server.cpp needs to build — the same core subset
# make_notebook.py already proves compiles together for main.cpp/matrixdbd.cpp/the tests.
CORE_SOURCES = ["types.hpp", "ring_buffer.hpp", "compute.hpp",
                "memory_model.hpp", "tier_model.hpp", "cost_model.hpp", "router.hpp",
                "kv_store.hpp", "cold_store.hpp", "tier_manager.hpp",
                "tiered_column.hpp", "migration_executor.hpp", "column_io.hpp",
                "csv_ingest.hpp",
                "server.hpp",
                "server_tcp.hpp",
                "client.hpp",
                "concurrent_server.hpp",
                "version.hpp",
                "logging.hpp",
                "compute_mock.cpp", "compute_cuda.cuh"]

SPIKE_SOURCES = ["spike/spike_server.cpp", "spike/SpikeClient.java"]


def code(src):
    return {"cell_type": "code", "metadata": {}, "execution_count": None, "outputs": [], "source": src}


def md(src):
    return {"cell_type": "markdown", "metadata": {}, "source": src}


cells = [
    md("# Network + GPU round-trip spike\n"
       "\n"
       "Answers one question: once a Java client talks to MatrixDB's GPU engine over a real TCP "
       "socket, does the ~12-19x in-process GPU scan win (see FINDINGS.md 3.5) survive the network "
       "+ serialization hop, or does that hop eat it alive? See "
       "docs/superpowers/specs/2026-06-30-network-gpu-spike-design.md for the full design and the "
       "PASS/FAIL decision rule.\n"
       "\n"
       "**Before running:** Runtime -> Change runtime type -> **T4 GPU**."),
    md("## 1. Confirm a GPU is attached"),
    code("!nvcc --version\n"
         "!nvidia-smi --query-gpu=name,memory.total --format=csv,noheader"),
    md("## 2. Write the source files"),
]

for f in CORE_SOURCES:
    with open(f, "r") as fh:
        body = fh.read()
    cells.append(code("%%writefile " + f + "\n" + body))

for f in SPIKE_SOURCES:
    with open(f, "r") as fh:
        body = fh.read()
    target = f.split("/")[-1]   # flatten spike/ — the notebook's cwd is flat, like every other file
    cells.append(code("%%writefile " + target + "\n" + body))

cells += [
    md("## 3. Install a JDK for the spike client"),
    code("!apt-get -qq update && apt-get -qq install -y openjdk-17-jdk-headless"),
    md("## 4. Build spike_server (CPU) and compile the Java client"),
    code("!clang++ -std=c++20 -O2 spike_server.cpp -o spike_server_cpu 2>/dev/null "
         "|| g++ -std=c++20 -O2 spike_server.cpp -o spike_server_cpu"),
    code("!javac SpikeClient.java"),
    md("## 5. CPU-via-network run\n"
       "\n"
       "Starts spike_server_cpu in the background, runs the Java client sweep against it, then "
       "stops it. Output is CSV: `kind,iteration,round_trip_ns,server_seconds,count`."),
    code("%%bash\n"
         "./spike_server_cpu 7070 &\n"
         "SERVER_PID=$!\n"
         "sleep 1\n"
         "java SpikeClient 127.0.0.1 7070 50 > cpu_results.csv\n"
         "kill $SERVER_PID\n"
         "wait 2>/dev/null\n"
         "cat cpu_results.csv"),
    md("## 6. Build spike_server (GPU, real CUDAGPUEngine)"),
    code("!nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread -DMATRIX_USE_CUDA "
         "spike_server.cpp -o spike_server_gpu"),
    md("## 7. GPU-via-network run"),
    code("%%bash\n"
         "./spike_server_gpu 7071 &\n"
         "SERVER_PID=$!\n"
         "sleep 1\n"
         "java SpikeClient 127.0.0.1 7071 50 > gpu_results.csv\n"
         "kill $SERVER_PID\n"
         "wait 2>/dev/null\n"
         "cat gpu_results.csv"),
    md("## 8. Compare against the decision rule\n"
       "\n"
       "Medians of `round_trip_ns` per `kind`. `scan` = the actual query cost over the network; "
       "`health` = the fixed network+serialization tax (zero payload). Compare against "
       "`docs/superpowers/specs/2026-06-30-network-gpu-spike-design.md`'s PASS/FAIL rule: PASS "
       "needs via-network GPU speedup >= ~10x AND health/scan tax fraction < 10%.\n"
       "\n"
       "Also reports `server_seconds` — the compute-only time each server timed around its own "
       "`execute_scan()` call, embedded in the response. It's immune to client-side JVM/OS "
       "scheduling noise, so comparing it to `round_trip_ns` tells apart two very different things: "
       "compute that's genuinely slower on this host, vs. round-trip inflation from something *around* "
       "the compute (client scheduling contention, GC, etc). Since `CPUMockEngine::execute_scan` is a "
       "single-core hot loop while `CUDAGPUEngine::execute_scan` mostly just waits on the GPU chip, a "
       "CPU-hungry JVM client sharing this box's vCPUs would contend directly with the CPU engine's "
       "scan but barely touch the GPU engine's — watch for that asymmetry here before trusting the "
       "speedup number."),
    code("import csv, statistics\n"
         "\n"
         "def medians(path):\n"
         "    scan, health, scan_server = [], [], []\n"
         "    with open(path) as fh:\n"
         "        for row in csv.DictReader(fh):\n"
         "            if row['kind'] == 'scan':\n"
         "                scan.append(int(row['round_trip_ns']))\n"
         "                scan_server.append(float(row['server_seconds']))\n"
         "            elif row['kind'] == 'health':\n"
         "                health.append(int(row['round_trip_ns']))\n"
         "    return statistics.median(scan), statistics.median(health), statistics.median(scan_server)\n"
         "\n"
         "cpu_scan_ns, cpu_health_ns, cpu_server_s = medians('cpu_results.csv')\n"
         "gpu_scan_ns, gpu_health_ns, gpu_server_s = medians('gpu_results.csv')\n"
         "cpu_server_ns, gpu_server_ns = cpu_server_s * 1e9, gpu_server_s * 1e9\n"
         "print(f'CPU: scan round_trip median={cpu_scan_ns:.0f}ns  server_seconds median={cpu_server_ns:.0f}ns  "
         "health(fixed tax) median={cpu_health_ns:.0f}ns  tax_fraction={cpu_health_ns/cpu_scan_ns:.1%}')\n"
         "print(f'GPU: scan round_trip median={gpu_scan_ns:.0f}ns  server_seconds median={gpu_server_ns:.0f}ns  "
         "health(fixed tax) median={gpu_health_ns:.0f}ns  tax_fraction={gpu_health_ns/gpu_scan_ns:.1%}')\n"
         "print(f'via-network GPU speedup over CPU (round_trip_ns): {cpu_scan_ns / gpu_scan_ns:.1f}x')\n"
         "print(f'compute-only GPU speedup over CPU (server_seconds): {cpu_server_ns / gpu_server_ns:.1f}x')\n"
         "print(f'CPU round_trip - server_seconds gap (client/scheduling overhead, not network tax): "
         "{cpu_scan_ns - cpu_server_ns:.0f}ns')\n"
         "print(f'GPU round_trip - server_seconds gap (client/scheduling overhead, not network tax): "
         "{gpu_scan_ns - gpu_server_ns:.0f}ns')"),
]

nb = {"cells": cells,
      "metadata": {"colab": {"name": "network_spike_colab.ipynb", "provenance": []},
                   "kernelspec": {"name": "python3", "display_name": "Python 3"}},
      "nbformat": 4, "nbformat_minor": 0}

with open("network_spike_colab.ipynb", "w") as fh:
    json.dump(nb, fh, indent=1)
print("wrote network_spike_colab.ipynb")

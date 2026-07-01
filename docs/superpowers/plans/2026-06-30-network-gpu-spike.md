# Network + GPU Round-Trip Spike Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a throwaway Colab-runnable spike that measures whether MatrixDB's already-proven
~12–19× in-process GPU scan win (`FINDINGS.md` §3.5) survives being wrapped in a real TCP +
serialization hop to a Java client — the one open number gating the Leg 1 (package MatrixDB)
and Leg 2 (Java/Spring Boot dependency) roadmap.

**Architecture:** A standalone `spike/spike_server.cpp` (not `matrixdbd` — see the spec's
"Correction found during planning" section for why) serves a minimal length-prefixed protocol
over loopback TCP, driving either `CPUMockEngine` or `CUDAGPUEngine` directly via
`ComputeInterface::execute_scan` over each engine's existing fixed 64MB resident column. A
throwaway `spike/SpikeClient.java` (raw sockets, no framework) round-trips SCAN and zero-payload
requests against it and times them. A new `make_spike_notebook.py` generates a self-contained
Colab notebook (T4 GPU) that builds both engine variants and runs the client against each.

**Tech Stack:** C++20 (CPU build: clang++/g++) / C++17+CUDA (GPU build: nvcc), Java (any JDK —
`java.net.Socket`, no dependencies), Python 3 stdlib (notebook generator).

**Spec:** `docs/superpowers/specs/2026-06-30-network-gpu-spike-design.md`

---

### Task 1: `spike/spike_server.cpp`

**Files:**
- Create: `spike/spike_server.cpp`

- [ ] **Step 1: Write the file**

```cpp
// spike/spike_server.cpp — throwaway network+GPU round-trip spike server (NOT production code).
// Answers one question: does a real TCP+serialization hop still leave the GPU's measured
// in-process scan win (FINDINGS.md 3.5, ~12-19x) multi-fold once a client talks to it over the
// network? Serves ONE connection at a time, reusing matrixdbd's own framing helpers
// (matrixsrv_detail::recv_all/send_all) but a minimal opcode protocol instead of the full
// MatrixRequest wire format — this file is never wired into matrixdbd or any tested path.
//
// Wire format:
//   request:  [u32 len][payload]   len==0 -> HEALTH (server echoes an empty response)
//                                  len==5 -> SCAN: payload[0]=opcode(1), payload[1..5)=u32 threshold (LE)
//   response: [u32 len][payload]   HEALTH -> len==0
//                                  SCAN    -> len==16: [0..8)=double seconds (LE), [8..16)=uint64 count (LE)
//
// Build (CPU):  clang++ -std=c++20 -O2 spike_server.cpp -o spike_server_cpu
// Build (GPU):  nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread -DMATRIX_USE_CUDA \
//               spike_server.cpp -o spike_server_gpu
#include "server_tcp.hpp"      // matrixsrv_detail::recv_all/send_all, matrix_set_recv_timeout/send_timeout
#if defined(MATRIX_USE_CUDA)
    #include "compute_cuda.cuh"
#endif
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: spike_server <port>\n"; return 2; }
    const uint16_t port = static_cast<uint16_t>(std::strtoul(argv[1], nullptr, 10));
    std::signal(SIGPIPE, SIG_IGN);   // a peer hanging up mid-send must not kill the server

#if defined(MATRIX_USE_CUDA)
    std::unique_ptr<ComputeInterface> engine = std::make_unique<CUDAGPUEngine>(4);
    std::cerr << "spike_server: CUDA engine, port " << port << "\n";
#else
    std::unique_ptr<ComputeInterface> engine = std::make_unique<CPUMockEngine>(4);
    std::cerr << "spike_server: CPU engine, port " << port << "\n";
#endif

    const int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { std::cerr << "spike_server: socket() failed\n"; return 1; }
    int yes = 1; ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        std::cerr << "spike_server: bind() failed (note: blocked in a sandboxed build env — run on a real host)\n";
        return 1;
    }
    if (::listen(srv, 16) != 0) { std::cerr << "spike_server: listen() failed\n"; return 1; }
    std::cerr << "spike_server: listening\n";

    for (;;) {
        const int fd = ::accept(srv, nullptr, nullptr);
        if (fd < 0) continue;
        matrix_set_recv_timeout(fd, 30000);
        matrix_set_send_timeout(fd, 30000);
        for (;;) {
            uint32_t len = 0;
            if (!matrixsrv_detail::recv_all(fd, &len, sizeof len)) break;
            if (len == 0) {
                const uint32_t rlen = 0;
                if (!matrixsrv_detail::send_all(fd, &rlen, sizeof rlen)) break;
                continue;
            }
            if (len != 5) break;   // only a 5-byte SCAN request is supported
            uint8_t req[5];
            if (!matrixsrv_detail::recv_all(fd, req, sizeof req)) break;
            if (req[0] != 1) break;   // opcode 1 == SCAN
            uint32_t threshold;
            std::memcpy(&threshold, req + 1, sizeof threshold);

            DatabaseQuery q{};
            matrix_set_scan_threshold(q, threshold);
            const auto t0 = std::chrono::steady_clock::now();
            engine->execute_scan(q);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const uint64_t count = q.transaction_id;

            uint8_t resp[16];
            std::memcpy(resp, &seconds, sizeof seconds);
            std::memcpy(resp + 8, &count, sizeof count);
            const uint32_t rlen = sizeof resp;
            if (!matrixsrv_detail::send_all(fd, &rlen, sizeof rlen)) break;
            if (!matrixsrv_detail::send_all(fd, resp, sizeof resp)) break;
        }
        ::close(fd);
    }
}
```

- [ ] **Step 2: Compile-check the CPU build (bind() is sandbox-blocked, so this only proves it compiles — actual runtime verification happens on Colab in Task 4)**

Run: `cd MatrixDB && clang++ -std=c++20 -O2 -I. -c spike/spike_server.cpp -o /tmp/spike_server_cpu.o`
Expected: no output, exit code 0. `-I.` is required so headers at the repo root resolve from the
`spike/` subdirectory. If `clang++` is unavailable, fall back to `g++ -std=c++20 -O2 -I. -c spike/spike_server.cpp -o /tmp/spike_server_cpu.o`.

- [ ] **Step 3: Commit**

```bash
cd MatrixDB
git add spike/spike_server.cpp
git commit -m "feat(spike): standalone network+GPU spike server (throwaway, not production)"
```

---

### Task 2: `spike/SpikeClient.java`

**Files:**
- Create: `spike/SpikeClient.java`

- [ ] **Step 1: Write the file**

```java
// spike/SpikeClient.java — throwaway Java client for the network+GPU round-trip spike.
// Speaks the minimal wire protocol implemented by spike_server.cpp: length-prefixed frames,
// a zero-length HEALTH-equivalent request, and a 5-byte SCAN request (opcode=1, u32 threshold).
// NOT production code — no Spring Boot, no dependencies, just java.net.Socket.
//
// Usage: java SpikeClient <host> <port> <iterations>
// Prints CSV to stdout: kind,iteration,round_trip_ns,server_seconds,count
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class SpikeClient {
    private static final int WARMUP = 5;
    private static final int SCAN_THRESHOLD = 8_388_608; // half of MATRIX_SCAN_COLUMN_SIZE (16,777,216)

    public static void main(String[] args) throws IOException {
        final String host = args.length > 0 ? args[0] : "127.0.0.1";
        final int port = args.length > 1 ? Integer.parseInt(args[1]) : 7070;
        final int iterations = args.length > 2 ? Integer.parseInt(args[2]) : 50;

        try (Socket sock = new Socket(host, port)) {
            sock.setTcpNoDelay(true);
            final DataOutputStream out = new DataOutputStream(sock.getOutputStream());
            final DataInputStream in = new DataInputStream(sock.getInputStream());

            System.out.println("kind,iteration,round_trip_ns,server_seconds,count");

            for (int i = -WARMUP; i < iterations; i++) {
                final long t0 = System.nanoTime();
                writeFrame(out, new byte[0]);
                readFrame(in);
                final long t1 = System.nanoTime();
                if (i >= 0) System.out.println("health," + i + "," + (t1 - t0) + ",,");
            }

            final ByteBuffer req = ByteBuffer.allocate(5).order(ByteOrder.LITTLE_ENDIAN);
            req.put(0, (byte) 1);
            req.putInt(1, SCAN_THRESHOLD);
            for (int i = -WARMUP; i < iterations; i++) {
                final long t0 = System.nanoTime();
                writeFrame(out, req.array());
                final byte[] resp = readFrame(in);
                final long t1 = System.nanoTime();
                if (i >= 0) {
                    final ByteBuffer rb = ByteBuffer.wrap(resp).order(ByteOrder.LITTLE_ENDIAN);
                    final double serverSeconds = Double.longBitsToDouble(rb.getLong(0));
                    final long count = rb.getLong(8);
                    System.out.println("scan," + i + "," + (t1 - t0) + "," + serverSeconds + "," + count);
                }
            }
        }
    }

    private static void writeFrame(DataOutputStream out, byte[] payload) throws IOException {
        final ByteBuffer lenBuf = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(0, payload.length);
        out.write(lenBuf.array());
        out.write(payload);
        out.flush();
    }

    private static byte[] readFrame(DataInputStream in) throws IOException {
        final byte[] lenBytes = new byte[4];
        in.readFully(lenBytes);
        final int len = ByteBuffer.wrap(lenBytes).order(ByteOrder.LITTLE_ENDIAN).getInt();
        final byte[] payload = new byte[len];
        if (len > 0) in.readFully(payload);
        return payload;
    }
}
```

- [ ] **Step 2: Compile-check it**

Run: `cd MatrixDB && javac -d /tmp spike/SpikeClient.java`
Expected: no output, exit code 0. (Running it needs a live server to connect to — that only
happens on a real host, in Task 4's Colab notebook; there is nothing to runtime-test here.)

- [ ] **Step 3: Commit**

```bash
cd MatrixDB
git add spike/SpikeClient.java
git commit -m "feat(spike): throwaway Java client for the network+GPU round-trip spike"
```

---

### Task 3: `make_spike_notebook.py` + generated notebook

**Files:**
- Create: `make_spike_notebook.py`
- Create: `network_spike_colab.ipynb` (generated, not hand-edited)

- [ ] **Step 1: Write the generator**

```python
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
       "needs via-network GPU speedup >= ~10x AND health/scan tax fraction < 10%."),
    code("import csv, statistics\n"
         "\n"
         "def medians(path):\n"
         "    scan, health = [], []\n"
         "    with open(path) as fh:\n"
         "        for row in csv.DictReader(fh):\n"
         "            if row['kind'] == 'scan':\n"
         "                scan.append(int(row['round_trip_ns']))\n"
         "            elif row['kind'] == 'health':\n"
         "                health.append(int(row['round_trip_ns']))\n"
         "    return statistics.median(scan), statistics.median(health)\n"
         "\n"
         "cpu_scan_ns, cpu_health_ns = medians('cpu_results.csv')\n"
         "gpu_scan_ns, gpu_health_ns = medians('gpu_results.csv')\n"
         "print(f'CPU: scan median={cpu_scan_ns:.0f}ns  health(fixed tax) median={cpu_health_ns:.0f}ns  "
         "tax_fraction={cpu_health_ns/cpu_scan_ns:.1%}')\n"
         "print(f'GPU: scan median={gpu_scan_ns:.0f}ns  health(fixed tax) median={gpu_health_ns:.0f}ns  "
         "tax_fraction={gpu_health_ns/gpu_scan_ns:.1%}')\n"
         "print(f'via-network GPU speedup over CPU: {cpu_scan_ns / gpu_scan_ns:.1f}x')"),
]

nb = {"cells": cells,
      "metadata": {"colab": {"name": "network_spike_colab.ipynb", "provenance": []},
                   "kernelspec": {"name": "python3", "display_name": "Python 3"}},
      "nbformat": 4, "nbformat_minor": 0}

with open("network_spike_colab.ipynb", "w") as fh:
    json.dump(nb, fh, indent=1)
print("wrote network_spike_colab.ipynb")
```

- [ ] **Step 2: Generate the notebook**

Run: `cd MatrixDB && python3 make_spike_notebook.py`
Expected: `wrote network_spike_colab.ipynb`

- [ ] **Step 3: Verify the notebook is valid JSON and has the expected cell count**

Run: `cd MatrixDB && python3 -c "import json; nb=json.load(open('network_spike_colab.ipynb')); print(len(nb['cells']), 'cells')"`
Expected: a cell count printed with no traceback (22 `CORE_SOURCES` + 2 `SPIKE_SOURCES` write
cells, plus the markdown/setup/build/run cells — no fixed number to assert, just confirm it
parses and is non-trivial, e.g. `> 20 cells`).

- [ ] **Step 4: Commit**

```bash
cd MatrixDB
git add make_spike_notebook.py network_spike_colab.ipynb
git commit -m "feat(spike): notebook generator + generated Colab notebook for the network+GPU spike"
```

---

### Task 4: Run on Colab and record the result (manual, human-in-the-loop)

This cannot be automated from an agent session — Colab needs a human to open the notebook,
attach a T4 GPU runtime, and run the cells.

- [ ] **Step 1: Open `network_spike_colab.ipynb` in Google Colab** (upload the file, or open it from
  Google Drive/GitHub if you push this branch there)

- [ ] **Step 2: Runtime -> Change runtime type -> T4 GPU, then Runtime -> Run all**

- [ ] **Step 3: Read the final cell's output** — three lines: CPU scan/tax, GPU scan/tax, and the
  via-network GPU speedup

- [ ] **Step 4: Report the numbers back** (paste the final cell's output, or `cpu_results.csv` /
  `gpu_results.csv`) for the PASS/FAIL call against
  `docs/superpowers/specs/2026-06-30-network-gpu-spike-design.md`'s decision rule — a follow-up
  `FINDINGS.md` entry gets written once there's a real result, continuing the existing "measure,
  then cut" journal discipline. That entry is explicitly not part of this plan.

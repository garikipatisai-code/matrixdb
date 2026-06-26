# Increment 4: Migration Executor (cross-tier byte movement) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn TierManager `MigrationDecision`s into actual cross-tier byte movement (HOST/RAM ↔ DEVICE/VRAM ↔ COLD/SSD), with a checksum-verified integrity invariant, and prove a VRAM-promoted column is genuinely GPU-scannable.

**Architecture:** `TieredColumn` holds a column's bytes resident in exactly one tier and `migrate_to()` moves them by always staging through HOST (read-to-host, free source, push-to-dest) — collapsing the 3×3 transition matrix into two simple halves and routing DEVICE↔COLD via HOST for free. `MigrationExecutor` applies a decision list to a column registry. DEVICE paths are guarded by `MATRIX_USE_CUDA` (cudaMemcpy); on a CPU build, DEVICE is unreachable and aborts fail-loud. RAM/SSD is fully CPU-tested; the VRAM path is host-syntax-probed locally and Colab-verified.

**Tech Stack:** C++20, header-only, `clang++`/`g++` (CPU), `nvcc` (GPU). POSIX file I/O. `<cuda_runtime.h>` only under `MATRIX_USE_CUDA`.

**Spec:** `docs/superpowers/specs/2026-06-26-increment-4-migration-executor-design.md`

**Verified current state (use exactly):**
- `MigrationDecision { uint64_t column_id; MemorySpace from; MemorySpace to; }` (tier_manager.hpp).
- `TierManager`: `register_column(uint64_t,size_t,MemorySpace)`, `record_access(uint64_t,size_t)`, `rebalance()->std::vector<MigrationDecision>`, `tier_of(uint64_t)->MemorySpace`.
- `MemorySpace { HOST, DEVICE, COLD, UNIFIED }` (memory_model.hpp).
- GPU scan kernel: `__global__ void matrix_scan_kernel_u32x4(const uint4* data4, size_t n4, uint32_t threshold, unsigned long long* count)` (compute_cuda.cuh) — counts values > threshold over n4 uint4s.

---

## File structure

- **Create** `tiered_column.hpp` — `TieredColumn` (movable bytes; HOST/COLD always, DEVICE under CUDA guard).
- **Create** `migration_executor.hpp` — `MigrationExecutor::apply(plan, columns)`.
- **Create** `test_migration.cpp` — CPU tests: HOST↔COLD round-trip, integrity chain, TierManager→executor loop.
- **Create** `test_migration_gpu.cpp` — GPU proof: HOST→DEVICE migrate, scan device bytes with u32x4, compare to CPU scan; DEVICE round-trip. Built with `nvcc -DMATRIX_USE_CUDA` on Colab.
- **Modify** `make_notebook.py` — embed new files + a CPU migration test cell + the GPU proof cell.
- **Modify** `PRODUCTION_READINESS.md` — note Inc 4 landed.

Per-column COLD file path: `"/tmp/matrixdb_tcol_<id>.bin"` (deterministic from id; a real column catalog comes with live-engine integration). The destructor frees whatever tier the column holds (DEVICE→cudaFree, COLD→remove file, HOST→vector auto) — the column owns its bytes; persistent cold-data-outliving-the-object semantics belong to the later catalog increment.

---

### Task 1: TieredColumn (movable column, HOST/COLD + DEVICE guarded)

**Files:**
- Create: `tiered_column.hpp`
- Create: `test_migration.cpp`

- [ ] **Step 1: Write the failing test — create `test_migration.cpp`**

```cpp
// CPU unit test for cross-tier migration (HOST<->COLD; DEVICE is Colab-verified).
// Build: clang++ -std=c++20 -O2 test_migration.cpp -o /tmp/tmig && /tmp/tmig
#include "tiered_column.hpp"
#include <cstdio>
#include <cassert>
#include <vector>
#include <numeric>

int main() {
    // --- Task 1: HOST<->COLD round-trip + integrity ---
    {
        std::vector<unsigned char> data(4096);
        for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<unsigned char>(i * 7 + 1);
        TieredColumn col(1, data.data(), data.size());
        const uint64_t want = col.checksum();
        assert(col.tier() == MemorySpace::HOST && "born in HOST");
        assert(col.size_bytes() == 4096);

        col.migrate_to(MemorySpace::COLD);
        assert(col.tier() == MemorySpace::COLD && "moved to COLD");
        assert(col.checksum() == want && "checksum invariant HOST->COLD");

        col.migrate_to(MemorySpace::HOST);
        assert(col.tier() == MemorySpace::HOST && "moved back to HOST");
        assert(col.checksum() == want && "checksum invariant COLD->HOST");

        // Integrity across a chain.
        col.migrate_to(MemorySpace::COLD);
        col.migrate_to(MemorySpace::HOST);
        col.migrate_to(MemorySpace::COLD);
        assert(col.checksum() == want && "checksum invariant across a HOST/COLD chain");
        col.migrate_to(MemorySpace::HOST); // leave on HOST so dtor frees the vector (no temp file left)
    }

    std::printf("PASS: migration correct\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `clang++ -std=c++20 -O2 test_migration.cpp -o /tmp/tmig 2>&1 | head -3`
Expected: FAIL — `fatal error: 'tiered_column.hpp' file not found`.

- [ ] **Step 3: Create `tiered_column.hpp`**

```cpp
#pragma once

#include "memory_model.hpp"
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#if defined(MATRIX_USE_CUDA)
#include <cuda_runtime.h>
#endif

// A column's bytes resident in exactly ONE tier (HOST/DEVICE/COLD). migrate_to() moves
// them by always staging through HOST: read the bytes to a host buffer, free the source
// tier, push to the destination. This collapses the 3x3 transition matrix into two halves
// and routes DEVICE<->COLD via HOST for free. The integrity invariant: checksum() is the
// same regardless of which tier holds the bytes. DEVICE requires a CUDA build.
class TieredColumn {
public:
    TieredColumn(uint64_t id, const unsigned char* bytes, size_t n)
        : id_(id), size_(n), tier_(MemorySpace::HOST), host_(bytes, bytes + n) {}

    ~TieredColumn() { free_current(); }

    TieredColumn(const TieredColumn&) = delete;
    TieredColumn& operator=(const TieredColumn&) = delete;

    MemorySpace tier() const { return tier_; }
    size_t size_bytes() const { return size_; }
    uint64_t id() const { return id_; }

    // Move the column's bytes to `dest` (no-op if already there). Always stages through HOST.
    void migrate_to(MemorySpace dest) {
        if (dest == tier_) return;
        std::vector<unsigned char> staged = read_to_host(); // pull bytes from wherever we are
        free_current();                                     // release the source tier's resource
        tier_ = MemorySpace::HOST;                          // logically in HOST now (staged holds bytes)
        host_ = std::move(staged);
        if (dest == MemorySpace::HOST) return;
        if (dest == MemorySpace::COLD) {
            write_cold(host_);
            host_.clear(); host_.shrink_to_fit();
            tier_ = MemorySpace::COLD;
            return;
        }
        if (dest == MemorySpace::DEVICE) {
#if defined(MATRIX_USE_CUDA)
            push_device(host_);
            host_.clear(); host_.shrink_to_fit();
            tier_ = MemorySpace::DEVICE;
            return;
#else
            std::fprintf(stderr, "TieredColumn: DEVICE tier requires a CUDA build\n");
            std::abort();
#endif
        }
        std::fprintf(stderr, "TieredColumn: unsupported destination tier\n");
        std::abort();
    }

    // Byte checksum wherever the column lives (DEVICE copies back to host first).
    uint64_t checksum() const {
        const std::vector<unsigned char> b = read_to_host();
        uint64_t sum = 0;
        for (unsigned char c : b) sum += c;
        return sum;
    }

#if defined(MATRIX_USE_CUDA)
    const void* device_ptr() const { return device_; } // valid only while tier()==DEVICE
#endif

private:
    std::string cold_path() const {
        return std::string("/tmp/matrixdb_tcol_") + std::to_string(id_) + ".bin";
    }

    std::vector<unsigned char> read_to_host() const {
        if (tier_ == MemorySpace::HOST) return host_;
        if (tier_ == MemorySpace::COLD) {
            std::vector<unsigned char> b(size_);
            FILE* f = std::fopen(cold_path().c_str(), "rb");
            if (!f) { std::fprintf(stderr, "TieredColumn: cold read failed %s\n", cold_path().c_str()); std::abort(); }
            const size_t got = std::fread(b.data(), 1, size_, f);
            std::fclose(f);
            if (got != size_) { std::fprintf(stderr, "TieredColumn: short cold read\n"); std::abort(); }
            return b;
        }
#if defined(MATRIX_USE_CUDA)
        if (tier_ == MemorySpace::DEVICE) {
            std::vector<unsigned char> b(size_);
            cudaMemcpy(b.data(), device_, size_, cudaMemcpyDeviceToHost);
            return b;
        }
#endif
        std::fprintf(stderr, "TieredColumn: read from unsupported tier\n");
        std::abort();
    }

    void write_cold(const std::vector<unsigned char>& b) {
        FILE* f = std::fopen(cold_path().c_str(), "wb");
        if (!f) { std::fprintf(stderr, "TieredColumn: cold write failed %s\n", cold_path().c_str()); std::abort(); }
        std::fwrite(b.data(), 1, b.size(), f);
        std::fclose(f);
    }

    void free_current() {
        if (tier_ == MemorySpace::COLD) std::remove(cold_path().c_str());
#if defined(MATRIX_USE_CUDA)
        if (tier_ == MemorySpace::DEVICE && device_) { cudaFree(device_); device_ = nullptr; }
#endif
        // HOST: host_ vector frees itself / is overwritten by the caller.
    }

#if defined(MATRIX_USE_CUDA)
    void push_device(const std::vector<unsigned char>& b) {
        cudaMalloc(&device_, size_);
        cudaMemcpy(device_, b.data(), size_, cudaMemcpyHostToDevice);
    }
    void* device_ = nullptr;
#endif

    uint64_t id_;
    size_t size_;
    MemorySpace tier_;
    std::vector<unsigned char> host_;
};
```

- [ ] **Step 4: Run test to verify it passes**

Run: `clang++ -std=c++20 -O2 test_migration.cpp -o /tmp/tmig && /tmp/tmig`
Expected: `PASS: migration correct`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add tiered_column.hpp test_migration.cpp
git -c user.name="garikipatisai-code" -c user.email="garikipatisai-code@users.noreply.github.com" commit -m "feat: TieredColumn — movable column with checksum-invariant HOST<->COLD migration (DEVICE guarded)"
```

---

### Task 2: MigrationExecutor + the TierManager→executor loop

**Files:**
- Create: `migration_executor.hpp`
- Modify: `test_migration.cpp`

- [ ] **Step 1: Add the failing test — insert before the `std::printf("PASS` line in `test_migration.cpp`**

Add these includes at the top of `test_migration.cpp` (after the existing includes):
```cpp
#include "migration_executor.hpp"
#include "tier_manager.hpp"
#include "cost_model.hpp"
#include <unordered_map>
```

Insert this block before the `PASS` printf:
```cpp
    // --- Task 2: TierManager decisions actually move columns (the auto-tiering loop) ---
    {
        // Two columns start on DEVICE in the brain's view; one is hot, one cold. With a
        // 1-column DEVICE capacity, rebalance() demotes the cold one. The executor must
        // physically move it. (DEVICE isn't reachable on a CPU build, so here we model the
        // movable tiers as HOST<->COLD: brain says COLD, executor moves the real bytes.)
        const size_t N = 4096;
        std::vector<unsigned char> bytes(N, 0xAB);
        TieredColumn hot(1, bytes.data(), N);   // stays
        TieredColumn cold(2, bytes.data(), N);  // will be demoted to COLD
        const uint64_t hot_sum = hot.checksum(), cold_sum = cold.checksum();

        std::unordered_map<uint64_t, TieredColumn*> columns{{1, &hot}, {2, &cold}};

        // Hand-built plan (decision-driven loop; TierManager produces exactly this shape).
        std::vector<MigrationDecision> plan{
            MigrationDecision{2, MemorySpace::HOST, MemorySpace::COLD},
            MigrationDecision{99, MemorySpace::HOST, MemorySpace::COLD}, // absent id → skipped
        };

        MigrationExecutor exec;
        const size_t applied = exec.apply(plan, columns);
        assert(applied == 1 && "one valid decision applied, absent id skipped");
        assert(cold.tier() == MemorySpace::COLD && "cold column physically demoted");
        assert(hot.tier() == MemorySpace::HOST && "untouched column stays");
        assert(cold.checksum() == cold_sum && "demoted column bytes intact");
        assert(hot.checksum() == hot_sum && "untouched column bytes intact");
        cold.migrate_to(MemorySpace::HOST); // cleanup: leave on HOST so no temp file remains
    }

    // --- Task 2b: a real TierManager rebalance feeds the executor ---
    {
        const size_t N = 4096;
        std::vector<unsigned char> bytes(N, 0x5C);
        TieredColumn a(10, bytes.data(), N);
        TieredColumn b(11, bytes.data(), N);
        std::unordered_map<uint64_t, TieredColumn*> columns{{10, &a}, {11, &b}};

        // Brain: both registered on HOST; demotion happens when HOST is over capacity. Use a
        // tiny HOST cap so the colder column is evicted toward COLD.
        TierManager tm(CostModel(MemoryModel::detect(true), true),
                       /*device_cap*/ 1u<<30, /*host_cap*/ N); // room for ONE column
        tm.register_column(10, N, MemorySpace::HOST);
        tm.register_column(11, N, MemorySpace::HOST);
        for (int r = 0; r < 3; ++r) {
            for (int i = 0; i < 50; ++i) tm.record_access(10, N); // col 10 hot
            auto plan = tm.rebalance();
            MigrationExecutor exec;
            exec.apply(plan, columns);
        }
        // Whatever the brain decided, the executor kept the columns' physical tier in sync
        // with the brain's view, and bytes are intact.
        assert(a.tier() == tm.tier_of(10) && "executor synced column 10 to brain's tier");
        assert(b.tier() == tm.tier_of(11) && "executor synced column 11 to brain's tier");
        assert(a.checksum() == b.checksum() && "both columns' bytes intact (same fill)");
        a.migrate_to(MemorySpace::HOST); b.migrate_to(MemorySpace::HOST); // cleanup temp files
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `clang++ -std=c++20 -O2 test_migration.cpp -o /tmp/tmig 2>&1 | head -3`
Expected: FAIL — `fatal error: 'migration_executor.hpp' file not found`.

- [ ] **Step 3: Create `migration_executor.hpp`**

```cpp
#pragma once

#include "tier_manager.hpp"   // MigrationDecision
#include "tiered_column.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include <cstdio>

// Turns a TierManager migration plan into physical byte movement. The brain decides
// (which column goes where); the executor actuates (calls migrate_to on the real bytes).
class MigrationExecutor {
public:
    // Apply each decision by migrating the named column to its target tier. A decision whose
    // column_id is not in the registry is skipped (logged). Returns the number applied.
    size_t apply(const std::vector<MigrationDecision>& plan,
                 std::unordered_map<uint64_t, TieredColumn*>& columns) {
        size_t applied = 0;
        for (const MigrationDecision& d : plan) {
            auto it = columns.find(d.column_id);
            if (it == columns.end()) {
                std::fprintf(stderr, "MigrationExecutor: no column %llu — skipped\n",
                             static_cast<unsigned long long>(d.column_id));
                continue;
            }
            it->second->migrate_to(d.to);
            ++applied;
        }
        return applied;
    }
};
```

- [ ] **Step 4: Run test to verify it passes**

Run: `clang++ -std=c++20 -O2 test_migration.cpp -o /tmp/tmig && /tmp/tmig`
Expected: `PASS: migration correct`, exit 0.

If Task 2b's `a.tier() == tm.tier_of(10)` fails, the executor and brain disagree on placement — STOP and report both tiers. (They should agree: the executor applies exactly the brain's plan, so the physical tier follows tier_of after each rebalance.) Do not weaken the assert.

- [ ] **Step 5: Commit**

```bash
git add migration_executor.hpp test_migration.cpp
git -c user.name="garikipatisai-code" -c user.email="garikipatisai-code@users.noreply.github.com" commit -m "feat: MigrationExecutor applies TierManager decisions to move column bytes"
```

---

### Task 3: GPU proof — scannable-after-promote (+ CUDA host-syntax probe)

**Files:**
- Create: `test_migration_gpu.cpp`

**Context:** This file is built with `nvcc -DMATRIX_USE_CUDA` on Colab. It proves a HOST column migrated to DEVICE is byte-intact AND genuinely GPU-scannable in place via the existing `matrix_scan_kernel_u32x4`. Locally we can only host-syntax-probe it (no nvcc).

- [ ] **Step 1: Create `test_migration_gpu.cpp`**

```cpp
// GPU proof (Colab): a column migrated HOST->DEVICE is byte-intact and GPU-scannable in
// place. Build: nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread \
//   -DMATRIX_USE_CUDA test_migration_gpu.cpp -o test_migration_gpu && ./test_migration_gpu
#include "tiered_column.hpp"
#include "compute_cuda.cuh"   // matrix_scan_kernel_u32x4 + CUDA_CHECK
#include <cstdio>
#include <cassert>
#include <vector>

int main() {
    // Build a uint32 column value[i]=i (n divisible by 4 for uint4 scan).
    const size_t N = 1u << 20; // 1,048,576 values
    std::vector<uint32_t> vals(N);
    for (size_t i = 0; i < N; ++i) vals[i] = static_cast<uint32_t>(i);
    const uint32_t threshold = N / 2;

    // CPU reference count of value > threshold.
    uint64_t cpu_count = 0;
    for (size_t i = 0; i < N; ++i) cpu_count += (vals[i] > threshold);

    // Make a column from those bytes; checksum before.
    TieredColumn col(1, reinterpret_cast<const unsigned char*>(vals.data()), N * sizeof(uint32_t));
    const uint64_t want = col.checksum();

    // Migrate HOST -> DEVICE and scan IN PLACE on the device pointer.
    col.migrate_to(MemorySpace::DEVICE);
    assert(col.tier() == MemorySpace::DEVICE && "promoted to DEVICE");

    unsigned long long* d_count = nullptr;
    CUDA_CHECK(cudaMalloc(&d_count, sizeof(unsigned long long)));
    CUDA_CHECK(cudaMemset(d_count, 0, sizeof(unsigned long long)));
    const uint4* col4 = reinterpret_cast<const uint4*>(col.device_ptr());
    const size_t n4 = (N * sizeof(uint32_t)) / sizeof(uint4); // bytes/16
    matrix_scan_kernel_u32x4<<<1024, 256>>>(col4, n4, threshold, d_count);
    CUDA_CHECK(cudaGetLastError());
    unsigned long long gpu_count = 0;
    CUDA_CHECK(cudaMemcpy(&gpu_count, d_count, sizeof(gpu_count), cudaMemcpyDeviceToHost));
    cudaFree(d_count);

    assert(gpu_count == cpu_count && "GPU scan of the promoted column matches the CPU count");

    // Migrate back; bytes must be byte-identical (integrity across the round trip).
    col.migrate_to(MemorySpace::HOST);
    assert(col.tier() == MemorySpace::HOST && "demoted back to HOST");
    assert(col.checksum() == want && "checksum invariant HOST->DEVICE->HOST");

    std::printf("PASS: VRAM-promoted column is GPU-scannable and byte-intact "
                "(gpu_count=%llu cpu_count=%llu)\n", gpu_count, cpu_count);
    return 0;
}
```

- [ ] **Step 2: Host-syntax probe the CUDA paths in tiered_column.hpp (no nvcc locally)**

The DEVICE branches in `tiered_column.hpp` only compile under `MATRIX_USE_CUDA`. Probe them with a stubbed `cuda_runtime.h` (reuse the prior pattern). Run:

```bash
cat > /tmp/cuda_stub_mig.h <<'EOF'
#pragma once
#include <cstddef>
enum{cudaMemcpyHostToDevice=1,cudaMemcpyDeviceToHost=2};
inline int cudaMalloc(void**p,size_t){*p=(void*)0;return 0;}
inline int cudaFree(void*){return 0;}
inline int cudaMemcpy(void*,const void*,size_t,int){return 0;}
EOF
sed -E 's@#include <cuda_runtime.h>@#include "/tmp/cuda_stub_mig.h"@' tiered_column.hpp > /tmp/tc_probe.hpp
printf '#define MATRIX_USE_CUDA 1\n#include "/tmp/tc_probe.hpp"\nint main(){ return 0; }\n' > /tmp/tc_probe.cpp
clang++ -std=c++20 -fsyntax-only -I. /tmp/tc_probe.cpp 2>&1 | grep -iE "error:" | head
echo "=== genuine errors above (empty = CUDA paths parse) ==="
```
Expected: empty (the DEVICE branches — push_device, read_to_host DEVICE arm, free_current cudaFree, device_ptr — parse cleanly with the stub).

Also confirm test_migration_gpu.cpp's NON-CUDA parts are sane by checking the CPU test still builds (it doesn't include the gpu file): `clang++ -std=c++20 -O2 test_migration.cpp -o /tmp/tmig && /tmp/tmig` → PASS.

NOTE: the real `nvcc` compile of test_migration_gpu.cpp happens on Colab (it includes compute_cuda.cuh's kernels which only nvcc compiles). Do NOT attempt to fully compile it locally. The host-probe of tiered_column.hpp's CUDA arms is the local guarantee; the kernel-launch + scan is Colab-verified.

- [ ] **Step 3: Commit**

```bash
git add test_migration_gpu.cpp
git -c user.name="garikipatisai-code" -c user.email="garikipatisai-code@users.noreply.github.com" commit -m "test: GPU proof — VRAM-promoted column is scannable in place + byte-intact (Colab)"
```

---

### Task 4: Notebook + register

**Files:**
- Modify: `make_notebook.py`
- Modify: `PRODUCTION_READINESS.md`

- [ ] **Step 1: Add new files to make_notebook.py SOURCES**

Read `make_notebook.py`. Add to `SOURCES`: `tiered_column.hpp`, `migration_executor.hpp` (near the other engine headers), and `test_migration.cpp`, `test_migration_gpu.cpp` (near the other tests). Preserve all existing entries.

- [ ] **Step 2: Add test cells**

Find the engine-restart test cell (`## 3e. Engine restart test`). After its code entry, insert:
```python
    md("## 3f. Migration test (CPU, no GPU)\n"
       "\n"
       "Cross-tier byte movement: HOST<->COLD round-trip is checksum-invariant, and "
       "TierManager decisions drive the executor to physically move columns."),
    code("!clang++ -std=c++20 -O2 test_migration.cpp -o /tmp/tmig 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_migration.cpp -o /tmp/tmig; /tmp/tmig"),
    md("## 4b. Migration GPU proof (needs T4 GPU)\n"
       "\n"
       "A column migrated HOST->VRAM is byte-intact AND GPU-scannable in place: the u32x4 "
       "kernel run over the promoted column's device pointer matches a CPU scan of the same "
       "bytes. Closes the heat->decision->migration->faster-scan loop on real hardware."),
    code("!nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread "
         "-DMATRIX_USE_CUDA test_migration_gpu.cpp -o test_migration_gpu && ./test_migration_gpu"),
```

- [ ] **Step 3: Regenerate + verify in sync**

Run:
```bash
python3 make_notebook.py && python3 -c "
import json
nb=json.load(open('matrixdb_colab.ipynb'))
emb={c['source'].split(chr(10),1)[0].split()[1]:c['source'].split(chr(10),1)[1] for c in nb['cells'] if c['cell_type']=='code' and c['source'].startswith('%%writefile')}
print('in sync:', all(emb[f]==open(f).read() for f in emb))
print('tiered_column embedded:', 'tiered_column.hpp' in emb)
print('migration test embedded:', 'test_migration.cpp' in emb)
print('gpu proof embedded:', 'test_migration_gpu.cpp' in emb)
"
```
Expected: all four print `True`.

- [ ] **Step 4: Note Inc 4 in PRODUCTION_READINESS.md**

In `PRODUCTION_READINESS.md`, find the Inc-3 landing note (after the section-2 durability table). Add this line after it:
```markdown

*Inc 4 landed: `tiered_column.hpp` + `migration_executor.hpp` — cross-tier byte movement (HOST/RAM ↔ DEVICE/VRAM ↔ COLD/SSD via HOST), checksum-invariant, driven by TierManager decisions. A VRAM-promoted column is proven GPU-scannable in place. The heat→decision→migration loop is closed on the TieredColumn primitive. Live-engine integration (the OP_SCAN column becoming a managed TieredColumn) is the next step. See spec 2026-06-26-increment-4-migration-executor-design.md.*
```

- [ ] **Step 5: Commit**

```bash
git add make_notebook.py matrixdb_colab.ipynb PRODUCTION_READINESS.md
git -c user.name="garikipatisai-code" -c user.email="garikipatisai-code@users.noreply.github.com" commit -m "docs: notebook migration test cells (CPU + GPU proof); note Inc 4 landed"
```

---

## Self-review (completed)

**Spec coverage:**
- §2 components (TieredColumn ctor/migrate_to/tier/size_bytes/checksum/device_ptr; MigrationExecutor.apply) → Tasks 1–2. ✓
- §3 migration paths (3×3 via HOST staging; HOST↔DEVICE cudaMemcpy; HOST↔COLD file; DEVICE↔COLD via HOST; DEVICE-on-CPU abort) → Task 1 (`migrate_to`/`read_to_host`/`push_device`/`free_current`). ✓
- §3 integrity invariant (checksum-before==after) → Task 1 round-trip + chain, Task 2 loop, Task 3 round-trip. ✓
- §4 the loop (decision → migration) → Task 2 (hand plan + real TierManager rebalance). ✓
- §5 verification: HOST↔COLD + chain + decision-loop + skip-absent (Tasks 1–2); GPU scannable-after-promote + DEVICE round-trip (Task 3); host-syntax probe (Task 3 Step 2); DEVICE-on-CPU abort documented (Task 1, not invoked in CPU test per spec). ✓
- §1 scope: no live-engine rewire (no compute_mock/router/main change); value=key untouched. ✓

**Placeholder scan:** No TBDs. Every code step complete. The local-vs-Colab split is explicit (Task 3: host-probe local, kernel Colab). The one runtime caveat (Task 2b tier-agreement) has a STOP-and-report fallback.

**Type consistency:** `TieredColumn(uint64_t,const unsigned char*,size_t)`, `migrate_to(MemorySpace)`, `tier()->MemorySpace`, `size_bytes()->size_t`, `checksum()->uint64_t`, `device_ptr()->const void*` consistent across Tasks 1/3. `MigrationExecutor::apply(const vector<MigrationDecision>&, unordered_map<uint64_t,TieredColumn*>&)->size_t` consistent Task 2/2. `MigrationDecision{column_id,from,to}` matches tier_manager.hpp. `matrix_scan_kernel_u32x4(const uint4*,size_t,uint32_t,unsigned long long*)` matches compute_cuda.cuh (Task 3 launch). TierManager API (register_column/record_access/rebalance/tier_of) matches.

**Known follow-ups (not this increment):** async/background migration + pinned DMA + multi-stream; live-engine integration (OP_SCAN column as a managed TieredColumn); DEVICE↔COLD direct (GPUDirect); migration I/O-error graceful handling; column catalog / cold-file lifecycle.

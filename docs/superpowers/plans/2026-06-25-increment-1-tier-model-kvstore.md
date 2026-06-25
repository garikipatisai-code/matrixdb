# Increment 1: Tier Model + Tier-Aware Cost + KVStore (DM-1) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the first increment of the three-tier storage engine: a 4-tier `MemorySpace` with per-tier physics, a tier-aware cost model, and a real open-addressing `KVStore` that replaces the `key & MASK` flat array — fixing the P0 silent-data-loss bug (DM-1) where colliding keys overwrite each other.

**Architecture:** Three header-only units. `tier_model.hpp` adds the `COLD` tier and a `TierPhysics` table (extends the existing `memory_model.hpp` concepts). `cost_model.hpp` extends with per-tier scan cost + migration cost. `kv_store.hpp` is a standalone open-addressing hash table (linear probing, key→value, fixed capacity, no silent overwrite). The CPU engine's flat `store_` array is replaced by a `KVStore` member; the GPU point-op store and the cross-backend store checksum are NOT touched in this increment (that's the per-engine work — here we only fix the CPU store and prove the oracle still holds).

**Tech Stack:** C++20, header-only, `clang++`/`g++` for CPU build (no nvcc locally). No new dependencies.

**Spec:** `docs/superpowers/specs/2026-06-25-three-tier-storage-engine-design.md` (Increment 1 row in §7)

---

## Scope of THIS increment (from the spec §7/§8)

- IN: `tier_model.hpp` (MemorySpace+COLD, TierPhysics), `cost_model.hpp` per-tier extension, `kv_store.hpp` (DM-1 fix), wire KVStore into the CPU engine.
- OUT (later increments, do NOT build): the tier_manager brain (Inc 2), the SSD cold_store/WAL (Inc 3), GPU migration (Inc 4), unified collapse (Inc 5). KVStore SSD-spill is a **seam** (an explicit error when full), not implemented. `value = key` mock projection stays.

## File structure

- **Create** `tier_model.hpp` — `MemorySpace{DEVICE,HOST,COLD,UNIFIED}` + `TierPhysics` table + `tier_physics(MemorySpace)` accessor. Folds in the existing `MemoryModel`/unified seam by including `memory_model.hpp`.
- **Create** `kv_store.hpp` — `KVStore` open-addressing hash table. One responsibility: a correct fixed-capacity key→value map.
- **Create** `test_kv_store.cpp` — CPU unit test for KVStore (no-overwrite, get/put, full-table error, checksum).
- **Modify** `cost_model.hpp` — add per-tier `scan_us(MemorySpace, bytes)` and `migration_us(from,to,bytes)`; keep existing API working.
- **Modify** `compute_mock.cpp` — replace `std::array store_` + `key&MASK` access with a `KVStore` member; keep `store_checksum()` semantics.
- **Modify** `test_cost_model.cpp` — add assertions for the new per-tier cost methods.
- **Modify** `make_notebook.py` — add `tier_model.hpp`, `kv_store.hpp`, `test_kv_store.cpp` to SOURCES + a KVStore test cell.

---

### Task 1: Tier model (add COLD tier + physics table)

**Files:**
- Create: `tier_model.hpp`
- Test: exercised by Task 3's cost-model test (data-only header)

- [ ] **Step 1: Create `tier_model.hpp`**

```cpp
#pragma once

#include "memory_model.hpp"
#include <cstddef>

// The storage tiers, ordered fastest-scan to coldest. DEVICE=VRAM, HOST=RAM, COLD=SSD.
// UNIFIED is the existing seam from memory_model.hpp (CPU+GPU share one pool); when the
// machine is unified, DEVICE and HOST collapse and migration between them is a no-op.
//
// NOTE: MemorySpace is DEFINED in memory_model.hpp as { HOST, DEVICE, UNIFIED }. This
// increment adds COLD there (see Task 1 Step 2) and this header layers the physics on top.

// Physics of one tier: what it's good at and what it costs. bandwidth in bytes/microsecond
// (so it composes with CostModel's existing *_BPus units); latency in microseconds to first
// byte; capacity_bytes is the usable budget (0 == "treat as unbounded for now").
struct TierPhysics {
    double   scan_bytes_per_us;  // sequential scan bandwidth
    double   access_latency_us;  // latency to first byte (random-access cost proxy)
    size_t   capacity_bytes;     // usable capacity budget; 0 = unbounded (not yet enforced)
    const char* concern;         // the tier's defining risk, for docs/telemetry
};

// First-estimate physics per tier (CALIBRATION TARGETS, like CostModel's constants).
// VRAM/RAM scan numbers are measured; SSD and latencies are estimates to refine on the box.
inline TierPhysics tier_physics(MemorySpace s) {
    switch (s) {
        case MemorySpace::DEVICE: // VRAM
            return TierPhysics{240'000.0, 5.0, 16ull*1024*1024*1024, "scarce capacity; PCIe to reach"};
        case MemorySpace::HOST:   // RAM
            return TierPhysics{10'000.0, 0.1, 256ull*1024*1024*1024, "medium capacity"};
        case MemorySpace::COLD:   // SSD
            return TierPhysics{3'000.0, 100.0, 0 /*unbounded*/, "high latency; write wear; append-only"};
        case MemorySpace::UNIFIED:
            return TierPhysics{240'000.0, 0.1, 0, "unified pool (future hardware)"};
    }
    return TierPhysics{1.0, 1.0, 0, "unknown"};
}
```

- [ ] **Step 2: Add `COLD` to the `MemorySpace` enum in `memory_model.hpp`**

Modify `memory_model.hpp` — change:
```cpp
enum class MemorySpace {
    HOST,    // CPU RAM
    DEVICE,  // GPU VRAM (discrete)
    UNIFIED, // shared CPU+GPU pool (DGX Spark / Grace-Hopper) — placement is zero-copy
};
```
to:
```cpp
enum class MemorySpace {
    HOST,    // CPU RAM
    DEVICE,  // GPU VRAM (discrete)
    COLD,    // SSD — cold columns + durability log (append-only, high latency)
    UNIFIED, // shared CPU+GPU pool (DGX Spark / Grace-Hopper) — placement is zero-copy
};
```
NOTE: this inserts COLD before UNIFIED. Two existing files index a `{"HOST","DEVICE","UNIFIED"}` array by `(int)MemorySpace` — `main.cpp` (routing_demo) and `test_cost_model.cpp` are NOT affected because they only ever produce HOST/DEVICE today, but the demo's `sp[]` array must gain a "COLD" entry so the index stays valid. Fix it now: in `main.cpp` find `const char* sp[] = {"HOST", "DEVICE", "UNIFIED"};` and change to `const char* sp[] = {"HOST", "DEVICE", "COLD", "UNIFIED"};`.

- [ ] **Step 3: Verify both headers compile standalone**

Run: `clang++ -std=c++20 -fsyntax-only tier_model.hpp && clang++ -std=c++20 -fsyntax-only memory_model.hpp`
Expected: no output, exit 0.

- [ ] **Step 4: Verify the CPU build still compiles (the enum change + sp[] fix)**

Run: `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o matrixdb_proto 2>&1 | head -10`
Expected: exit 0, clean.

- [ ] **Step 5: Commit**

```bash
git add tier_model.hpp memory_model.hpp main.cpp
git -c user.name="garikipatisai-code" -c user.email="garikipatisai-code@users.noreply.github.com" commit -m "feat: add COLD (SSD) tier + per-tier physics table"
```

---

### Task 2: KVStore — the DM-1 fix (open-addressing hash table)

**Files:**
- Create: `kv_store.hpp`
- Test: `test_kv_store.cpp`

- [ ] **Step 1: Write the failing test — create `test_kv_store.cpp`**

```cpp
// CPU unit test for KVStore: the DM-1 fix. Proves distinct keys never overwrite each
// other (the silent-data-loss bug), get/put round-trips, and a full table is an explicit
// error, not corruption.
// Build: clang++ -std=c++20 -O2 test_kv_store.cpp -o /tmp/tkv && /tmp/tkv
#include "kv_store.hpp"
#include <cstdio>
#include <cassert>

int main() {
    KVStore kv(1024); // capacity 1024 slots

    // 1. put/get round-trip.
    kv.put(42, 100);
    uint64_t v = 0;
    assert(kv.get(42, v) && v == 100 && "get must return the put value");

    // 2. Missing key returns false.
    assert(!kv.get(999, v) && "absent key must report miss");

    // 3. Overwrite same key updates value (not a collision — same key).
    kv.put(42, 200);
    assert(kv.get(42, v) && v == 200 && "same-key put updates value");
    assert(kv.size() == 1 && "same-key put does not grow size");

    // 4. THE DM-1 BUG: two DISTINCT keys that collide under masking must BOTH survive.
    //    With a 1024 table, keys 7 and 7+1024 collide on the initial slot. Old code
    //    (key & MASK) silently overwrote; the hash table must probe and keep both.
    kv.put(7, 70);
    kv.put(7 + 1024, 71);
    uint64_t a = 0, b = 0;
    assert(kv.get(7, a) && a == 70 && "colliding key 7 must survive");
    assert(kv.get(7 + 1024, b) && b == 71 && "colliding key 7+1024 must survive");
    assert(kv.size() == 3 && "two distinct colliding keys are two entries");

    // 5. Full table is an explicit error (return false), never silent loss.
    KVStore small(2); // 2 slots
    assert(small.put(1, 1) && "first put fits");
    assert(small.put(2, 2) && "second put fits");
    assert(!small.put(3, 3) && "third put must FAIL (full), not overwrite");
    uint64_t x = 0;
    assert(small.get(1, x) && x == 1 && "existing entries intact after full");
    assert(small.get(2, x) && x == 2 && "existing entries intact after full");

    // 6. checksum is order-independent sum of stored values (used by the engine).
    KVStore c(16);
    c.put(1, 10); c.put(2, 20); c.put(3, 30);
    assert(c.checksum() == 60 && "checksum is sum of values");

    std::printf("PASS: KVStore correctness (no silent overwrite, full=error)\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `clang++ -std=c++20 -O2 test_kv_store.cpp -o /tmp/tkv 2>&1 | head -3`
Expected: FAIL — `fatal error: 'kv_store.hpp' file not found`.

- [ ] **Step 3: Create `kv_store.hpp`**

```cpp
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

// Open-addressing hash table (linear probing), key -> value, fixed capacity.
// This is the point-op store (gap DM-1). The prototype used store[key & MASK], which
// SILENTLY OVERWROTE colliding keys — a data-loss bug. Here, distinct keys probe to
// distinct slots and never overwrite each other; a full table is an explicit error
// (put returns false), never silent corruption.
//
// Single-owner: the engine's point-op path is single-threaded per the page-ownership
// model, so no internal locking. capacity is rounded up to a power of two so the slot
// index is a mask, not a modulo. SSD-spill (gap DM-9 / Inc 3) is a future seam: when
// full we return false today; later that path demotes cold entries to the cold tier.
class KVStore {
public:
    explicit KVStore(size_t capacity)
        : mask_(round_up_pow2(capacity) - 1),
          slots_(round_up_pow2(capacity)) {}

    // Insert or update. Returns false ONLY if the table is full and the key is new
    // (explicit failure, never overwrite of a different key).
    bool put(uint64_t key, uint64_t value) {
        size_t i = key & mask_;
        for (size_t probe = 0; probe <= mask_; ++probe) {
            Slot& s = slots_[i];
            if (!s.occupied) {            // empty slot: new insert
                s.key = key; s.value = value; s.occupied = true;
                ++size_;
                return true;
            }
            if (s.key == key) {           // same key: update in place
                s.value = value;
                return true;
            }
            i = (i + 1) & mask_;          // collision with a DIFFERENT key: probe on
        }
        return false;                     // table full, key not present: explicit error
    }

    // Look up. Returns true and sets out if present; false if absent.
    bool get(uint64_t key, uint64_t& out) const {
        size_t i = key & mask_;
        for (size_t probe = 0; probe <= mask_; ++probe) {
            const Slot& s = slots_[i];
            if (!s.occupied) return false;     // empty slot ends the probe chain: miss
            if (s.key == key) { out = s.value; return true; }
            i = (i + 1) & mask_;
        }
        return false;
    }

    size_t size() const { return size_; }
    size_t capacity() const { return mask_ + 1; }

    // Order-independent fingerprint: sum of stored values (matches the engine's old
    // store_checksum semantics so cross-checks stay meaningful).
    uint64_t checksum() const {
        uint64_t sum = 0;
        for (const Slot& s : slots_) if (s.occupied) sum += s.value;
        return sum;
    }

private:
    struct Slot { uint64_t key = 0; uint64_t value = 0; bool occupied = false; };

    static size_t round_up_pow2(size_t n) {
        size_t p = 1;
        while (p < n) p <<= 1;
        return p < 2 ? 2 : p; // minimum 2 slots
    }

    size_t mask_;
    size_t size_ = 0;
    std::vector<Slot> slots_;
};
```

- [ ] **Step 4: Run test to verify it passes**

Run: `clang++ -std=c++20 -O2 test_kv_store.cpp -o /tmp/tkv && /tmp/tkv`
Expected: `PASS: KVStore correctness (no silent overwrite, full=error)`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add kv_store.hpp test_kv_store.cpp
git -c user.name="garikipatisai-code" -c user.email="garikipatisai-code@users.noreply.github.com" commit -m "feat: KVStore open-addressing hash table — fixes DM-1 silent key overwrite"
```

---

### Task 3: Tier-aware cost model

**Files:**
- Modify: `cost_model.hpp`
- Test: `test_cost_model.cpp` (extend)

- [ ] **Step 1: Write the failing test — add to `test_cost_model.cpp`**

Add `#include "tier_model.hpp"` near the top includes (after the existing includes). Then insert before the existing `std::printf("PASS: cost model placement decisions correct\n");` line:

```cpp
    // --- Tier-aware cost: scan time per tier, and migration cost ---
    {
        CostModel tcm(discrete);
        const size_t bytes = 64u * 1024 * 1024; // 64 MB

        // Per-tier scan time ranks by bandwidth: DEVICE (VRAM) fastest, COLD (SSD) slowest.
        const double dev = tcm.scan_us(MemorySpace::DEVICE, bytes);
        const double host = tcm.scan_us(MemorySpace::HOST, bytes);
        const double cold = tcm.scan_us(MemorySpace::COLD, bytes);
        assert(dev < host && host < cold && "scan time must rank DEVICE < HOST < COLD");

        // Migration cost is non-negative and zero when source==dest (no move).
        assert(tcm.migration_us(MemorySpace::HOST, MemorySpace::HOST, bytes) == 0.0
               && "no-op migration costs 0");
        assert(tcm.migration_us(MemorySpace::HOST, MemorySpace::DEVICE, bytes) > 0.0
               && "real migration costs > 0");
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `clang++ -std=c++20 -O2 test_cost_model.cpp -o /tmp/tcm 2>&1 | head -5`
Expected: FAIL — `no member named 'scan_us'` (and/or 'migration_us') in 'CostModel', or 'tier_model.hpp' not found if include missing.

- [ ] **Step 3: Extend `cost_model.hpp`**

Add `#include "tier_model.hpp"` after the existing `#include "memory_model.hpp"` line. Then add these two public methods to the `CostModel` class (place them after the existing `device_scan_us` method, before the `private:` label):

```cpp
    // Predicted scan time (us) for `bytes` resident in tier `s`. Uses each tier's measured
    // bandwidth from tier_model. This generalizes host_scan_us/device_scan_us to all tiers
    // (the existing two stay for the router's HOST/DEVICE fast path and back-compat).
    double scan_us(MemorySpace s, size_t bytes) const {
        const TierPhysics p = tier_physics(s);
        const double launch = (s == MemorySpace::DEVICE) ? LAUNCH_US : 0.0;
        return launch + static_cast<double>(bytes) / p.scan_bytes_per_us;
    }

    // Predicted one-time cost (us) to move `bytes` from tier `from` to tier `to`. Zero if
    // same tier (no move) or if memory is unified (zero-copy). Otherwise bounded by the
    // slower of read-from-source and write-to-dest bandwidth.
    double migration_us(MemorySpace from, MemorySpace to, size_t bytes) const {
        if (from == to) return 0.0;
        if (mm_.is_unified()) return 0.0; // unified pool: placement is zero-copy
        const double read_bw  = tier_physics(from).scan_bytes_per_us;
        const double write_bw = tier_physics(to).scan_bytes_per_us;
        const double slower = (read_bw < write_bw) ? read_bw : write_bw;
        return static_cast<double>(bytes) / slower;
    }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `clang++ -std=c++20 -O2 test_cost_model.cpp -o /tmp/tcm && /tmp/tcm`
Expected: `PASS: cost model placement decisions correct`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add cost_model.hpp test_cost_model.cpp
git -c user.name="garikipatisai-code" -c user.email="garikipatisai-code@users.noreply.github.com" commit -m "feat: tier-aware cost model — per-tier scan_us + migration_us"
```

---

### Task 4: Wire KVStore into the CPU engine (replace the flat store)

**Context:** `compute_mock.cpp` has `std::array<uint64_t, MATRIX_STORE_SLOTS> store_{};` (the flat array). Point ops use `store_[key & MATRIX_STORE_MASK]`. We replace it with a `KVStore`. The mock value stays `value = key`. After this, the pipeline oracle (`reads=4990 writes=5000 commits=5000`, scan sum `83886070`) must still pass — distinct test keys never collide in the old code either, so behavior is preserved, but now collisions would be handled correctly if they occurred.

**Files:**
- Modify: `compute_mock.cpp`

- [ ] **Step 1: Add the include**

At the top of `compute_mock.cpp`, after the existing `#include "compute.hpp"` line, add:
```cpp
#include "kv_store.hpp"
```

- [ ] **Step 2: Replace the store member declaration**

Find:
```cpp
    std::array<uint64_t, MATRIX_STORE_SLOTS> store_{}; // the Value column
```
Replace with:
```cpp
    // Point-op store: a real hash table (gap DM-1). Sized to the page-store capacity.
    // Distinct keys never overwrite; full table is an explicit error, not silent loss.
    KVStore kv_{MATRIX_STORE_SLOTS};
```

- [ ] **Step 3: Replace the point-op access in `execute_batch`**

Find:
```cpp
                const size_t slot = q.query_id & MATRIX_STORE_MASK;
                if (q.opcode == OP_READ) {
                    q.transaction_id = store_[slot];
                    ++reads_;
                } else if (q.opcode == OP_WRITE) {
                    store_[slot] = q.query_id; // mock projection: value == key
                    ++writes_;
                    ++commits_;
                }
```
Replace with:
```cpp
                if (q.opcode == OP_READ) {
                    uint64_t v = 0;
                    kv_.get(q.query_id, v); // miss leaves v=0 (matches old zero-init store)
                    q.transaction_id = v;
                    ++reads_;
                } else if (q.opcode == OP_WRITE) {
                    // mock projection: value == key. A full table is a hard error here
                    // (Inc 3 adds SSD spill); assert so it can never silently drop a write.
                    const bool ok = kv_.put(q.query_id, q.query_id);
                    assert(ok && "KVStore full — point-op store capacity exceeded (Inc 3 adds spill)");
                    (void)ok;
                    ++writes_;
                    ++commits_;
                }
```
NOTE: this needs `#include <cassert>` — add it to the top includes of `compute_mock.cpp` if not already present (check first).

- [ ] **Step 4: Replace `store_checksum()`**

Find:
```cpp
    uint64_t store_checksum() const override {
        uint64_t sum = 0;
        for (uint64_t v : store_) sum += v;
        return sum;
    }
```
Replace with:
```cpp
    uint64_t store_checksum() const override {
        return kv_.checksum();
    }
```

- [ ] **Step 5: Build and run the full pipeline — oracle must still pass**

Run: `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o matrixdb_proto && ./matrixdb_proto 2>&1 | grep -E "Engine:|Scan result|Processed|completed"`
Expected:
```
Processed 10000 / 10000 queries.
Engine: reads=4990 writes=5000 commits=5000 scans=10
Scan result sum: 83886070 (oracle 83886070)
Engine execution loop completed successfully.
```
IMPORTANT: the `store_checksum` VALUE may differ from before (the old flat array summed slot contents including the `key&MASK` overwrites; the new KVStore sums distinct stored values). That is expected and correct — the checksum is only asserted equal ACROSS backends in the CUDA build, and that comparison is unaffected because this increment doesn't change the GPU store. If the process aborts on the new `assert(ok)`, the test keys exceeded MATRIX_STORE_SLOTS capacity — STOP and report (would require raising capacity), but with 10000 queries writing keys 1,3,5… mod-free into a 4096-slot table this assert WILL trip because >4096 distinct keys are written. See Step 6.

- [ ] **Step 6: Fix capacity — the pipeline writes more distinct keys than 4096 slots hold**

The pipeline writes ~5000 distinct WRITE keys, exceeding the 4096-slot table → the `assert(ok)` in Step 3 would fire (correctly — that's the table being full). The old code didn't notice because it silently overwrote. Raise the point-op store capacity so the increment's demo fits. In `compute_mock.cpp`, change the KVStore construction from:
```cpp
    KVStore kv_{MATRIX_STORE_SLOTS};
```
to:
```cpp
    // ponytail: sized to comfortably hold the demo's distinct write-keys with headroom.
    // Real capacity / SSD-spill is gap DM-9 / Inc 3; this is the fixed-capacity seam.
    KVStore kv_{1u << 16}; // 65536 slots
```
Then re-run Step 5's command. Expected: the oracle output above, no abort.

- [ ] **Step 7: Run the KVStore unit test + cost test once more (regression)**

Run: `clang++ -std=c++20 -O2 test_kv_store.cpp -o /tmp/tkv && /tmp/tkv && clang++ -std=c++20 -O2 test_cost_model.cpp -o /tmp/tcm && /tmp/tcm`
Expected: both print PASS.

- [ ] **Step 8: Commit**

```bash
git add compute_mock.cpp
git -c user.name="garikipatisai-code" -c user.email="garikipatisai-code@users.noreply.github.com" commit -m "feat: CPU engine point-op store uses KVStore (DM-1 fix wired in)"
```

---

### Task 5: Notebook + register update

**Files:**
- Modify: `make_notebook.py`
- Modify: `PRODUCTION_READINESS.md`

- [ ] **Step 1: Add new files to make_notebook.py SOURCES**

In `make_notebook.py`, find the `SOURCES = [...]` list and add `tier_model.hpp`, `kv_store.hpp` (header order: after `memory_model.hpp`/`cost_model.hpp`/`router.hpp` grouping — put `tier_model.hpp` next to `memory_model.hpp` and `kv_store.hpp` after `router.hpp`) and `test_kv_store.cpp` (next to the other test files). The exact resulting list:
```python
SOURCES = ["types.hpp", "ring_buffer.hpp", "compute.hpp",
           "memory_model.hpp", "tier_model.hpp", "cost_model.hpp", "router.hpp",
           "kv_store.hpp",
           "compute_mock.cpp", "compute_cuda.cuh", "main.cpp",
           "test_scan_coverage.cpp", "test_cost_model.cpp", "test_kv_store.cpp"]
```

- [ ] **Step 2: Add a KVStore test cell before the GPU build cell**

In `make_notebook.py`, find the `## 3b. Cost-model unit test` markdown cell. Immediately before it, insert:
```python
    md("## 3a. KVStore unit test (CPU, no GPU)\n"
       "\n"
       "Proves the DM-1 fix: distinct colliding keys never overwrite each other, and a "
       "full table is an explicit error, not silent data loss."),
    code("!clang++ -std=c++20 -O2 test_kv_store.cpp -o /tmp/tkv 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_kv_store.cpp -o /tmp/tkv; /tmp/tkv"),
```

- [ ] **Step 3: Regenerate + verify notebook in sync**

Run:
```bash
python3 make_notebook.py && python3 -c "
import json
nb=json.load(open('matrixdb_colab.ipynb'))
emb={c['source'].split(chr(10),1)[0].split()[1]:c['source'].split(chr(10),1)[1] for c in nb['cells'] if c['cell_type']=='code' and c['source'].startswith('%%writefile')}
print('in sync:', all(emb[f]==open(f).read() for f in emb))
print('kv_store embedded:', 'kv_store.hpp' in emb)
print('tier_model embedded:', 'tier_model.hpp' in emb)
"
```
Expected: all three print `True`.

- [ ] **Step 4: Mark DM-1 done in the gap register**

In `PRODUCTION_READINESS.md`, find the DM-1 row in the section-1 table and change its row to note it's fixed. The row currently starts `| DM-1 | **Store is 4096 slots...`. Append ` **[FIXED — Inc 1: KVStore]**` to the "Gap" cell text so the register reflects reality. Also add a one-line note under the table:
```markdown
*Inc 1 of the three-tier engine landed: `tier_model.hpp`, tier-aware `cost_model.hpp`, and `kv_store.hpp` (DM-1 fixed — open-addressing hash table, no silent overwrite). See spec 2026-06-25-three-tier-storage-engine-design.md.*
```

- [ ] **Step 5: Commit**

```bash
git add make_notebook.py matrixdb_colab.ipynb PRODUCTION_READINESS.md
git -c user.name="garikipatisai-code" -c user.email="garikipatisai-code@users.noreply.github.com" commit -m "docs: notebook KVStore test cell + mark DM-1 fixed in gap register"
```

---

## Self-review (completed)

**Spec coverage (Increment 1 row of §7):**
- tier_model (MemorySpace+COLD, TierPhysics) → Task 1. ✓
- tier-aware cost_model (per-tier scan + migration cost) → Task 3. ✓
- kv_store / DM-1 fix → Task 2 (the store) + Task 4 (wired into engine). ✓
- §8 scope discipline: SSD-spill is a seam (full=error, Task 2 Step 3 + Task 4 assert); migration logic absent (only cost math added, no executor); value=key kept (Task 4). ✓

**Placeholder scan:** No TBDs. Every code step has complete code. The one runtime risk (table-full on the pipeline) is explicitly handled in Task 4 Steps 5–6 with the actual fix, not deferred.

**Type consistency:** `KVStore(size_t)`, `put(uint64_t,uint64_t)->bool`, `get(uint64_t,uint64_t&)->bool`, `size()`, `capacity()`, `checksum()->uint64_t` consistent across Task 2 (def), Task 2 test, and Task 4 (use). `MemorySpace::COLD` added in Task 1, used in Task 3's `scan_us`/`migration_us` and tests. `tier_physics(MemorySpace)->TierPhysics` with `scan_bytes_per_us` field used consistently in Task 1 (def) and Task 3 (use). `scan_us(MemorySpace,size_t)` / `migration_us(MemorySpace,MemorySpace,size_t)` consistent between Task 3 def and test.

**Known follow-ups (not in this increment, by design):** tier_manager brain (Inc 2), SSD cold_store/WAL/durability (Inc 3), GPU migration executor (Inc 4), unified collapse (Inc 5), real typed values (DM-3), KVStore auto-grow/real spill (DM-9).

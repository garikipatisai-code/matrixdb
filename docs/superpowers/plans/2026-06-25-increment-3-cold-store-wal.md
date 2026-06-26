# Increment 3: ColdStore (SSD WAL + durability) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a synchronous, CRC-framed append-only WAL (`ColdStore`) and wire it into the CPU engine so committed point-op writes survive a process restart — MatrixDB stops losing data on crash.

**Architecture:** A header-only `ColdStore` appends length+CRC32-framed records to a file and fsyncs per policy; `replay()` reconstructs state front-to-back, stopping safely at a torn/corrupt tail. `CPUMockEngine` gains an optional WAL path: on construct it replays the log into its KVStore (recovery), on OP_WRITE it appends-before-commit (durability). Durability is opt-in via the path, so the existing in-memory pipeline + oracle are unchanged.

**Tech Stack:** C++20, header-only, `clang++`/`g++`. Standard library + POSIX `fsync` (`<unistd.h>`). CRC32 implemented inline (no dependency).

**Spec:** `docs/superpowers/specs/2026-06-25-increment-3-cold-store-wal-design.md`

**Verified current state (use exactly):**
- `CPUMockEngine` ctor: `explicit CPUMockEngine(size_t /*worker_count*/ = 0)` in `compute_mock.cpp:20`; members include `KVStore kv_{1u << 16}`. OP_WRITE block at `compute_mock.cpp:62-74`. Includes already present: `compute.hpp`, `kv_store.hpp`, `<vector>`, `<array>`, `<cassert>`, `<chrono>`, `<iostream>`.
- `KVStore`: `bool put(uint64_t,uint64_t)`, `bool get(uint64_t,uint64_t&) const`, `uint64_t checksum() const`.
- `main.cpp:209` constructs `std::make_unique<CPUMockEngine>(4)` — adding a DEFAULTED 2nd ctor param keeps this valid (no main.cpp change).
- `OP_WRITE` is a `MatrixOpcode` enum value in `types.hpp`.

---

## File structure

- **Create** `cold_store.hpp` — `matrix_crc32`, `SyncPolicy`, `WalRecord`, `ColdStore` (append + replay). One responsibility: durable append-log + recovery.
- **Create** `test_cold_store.cpp` — CPU unit tests over real temp files (round-trip, restart, torn tail, CRC corruption, empty file).
- **Create** `test_engine_restart.cpp` — end-to-end: writes via one engine survive into a fresh engine on the same WAL path.
- **Modify** `compute_mock.cpp` — optional WAL path: replay on construct, append-before-commit on OP_WRITE.
- **Modify** `make_notebook.py` — embed new files + test cells.

Constants (in cold_store.hpp): `MATRIX_WAL_MAX_RECORD = 4096` (sane upper bound for the length field), payload is 20 bytes (key 8 + value 8 + opcode 4).

---

### Task 1: ColdStore core — CRC32, append, replay

**Files:**
- Create: `cold_store.hpp`
- Create: `test_cold_store.cpp`

- [ ] **Step 1: Write the failing test — create `test_cold_store.cpp`**

```cpp
// CPU unit test for ColdStore (the SSD WAL). Uses real temp files.
// Build: clang++ -std=c++20 -O2 test_cold_store.cpp -o /tmp/tcs && /tmp/tcs
#include "cold_store.hpp"
#include <cstdio>
#include <cassert>
#include <vector>
#include <string>

static const char* PATH = "/tmp/matrixdb_wal_test.log";

int main() {
    // --- Task 1: append + replay round-trip, last-writer-wins ---
    {
        std::remove(PATH); // fresh
        {
            ColdStore cs(PATH, SyncPolicy::SYNC_EACH);
            cs.append_put(10, 100);
            cs.append_put(20, 200);
            cs.append_put(10, 101); // overwrite key 10 (later record wins on replay)
            assert(cs.records_written() == 3 && "three appends counted");
        }
        // Replay (a fresh ColdStore on the same path).
        ColdStore cs(PATH);
        std::vector<std::pair<uint64_t,uint64_t>> got;
        cs.replay([&](uint64_t k, uint64_t v){ got.push_back({k, v}); });
        assert(got.size() == 3 && "replay yields all three records in order");
        assert(got[0].first == 10 && got[0].second == 100);
        assert(got[1].first == 20 && got[1].second == 200);
        assert(got[2].first == 10 && got[2].second == 101 && "last write for key 10 replays last");
        std::remove(PATH);
    }

    std::printf("PASS: cold store WAL correct\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `clang++ -std=c++20 -O2 test_cold_store.cpp -o /tmp/tcs 2>&1 | head -3`
Expected: FAIL — `fatal error: 'cold_store.hpp' file not found`.

- [ ] **Step 3: Create `cold_store.hpp`**

```cpp
#pragma once

#include "types.hpp"   // OP_WRITE
#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <unistd.h>    // fsync, fileno

// SSD-backed write-ahead log (gap DU-1/2/3 / three-tier cold tier). Append-only: a record
// is [u32 length][u32 crc32(payload)][payload]. Synchronous (fsync per policy) so a
// returned append_put is durable. replay() rebuilds state front-to-back and stops at the
// first torn or corrupt record — never replays corruption. "SSD" is a file here; the
// interface is identical on real flash.

enum class SyncPolicy {
    SYNC_EACH,  // fsync after every append — committed write survives a crash (default)
    SYNC_OFF,   // no fsync — tests / throughput; crash loses unflushed tail
};

// One logged mutation. Fixed 20-byte serialized payload (key 8 + value 8 + opcode 4).
struct WalRecord {
    uint64_t key;
    uint64_t value;
    uint32_t opcode;
};

constexpr size_t MATRIX_WAL_PAYLOAD_BYTES = 20;     // 8 + 8 + 4, serialized explicitly
constexpr uint32_t MATRIX_WAL_MAX_RECORD = 4096;    // sane upper bound for the length field

// Standard CRC32 (reflected, poly 0xEDB88320). Inline, no dependency.
inline uint32_t matrix_crc32(const unsigned char* data, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFFu;
}

class ColdStore {
public:
    explicit ColdStore(std::string path, SyncPolicy policy = SyncPolicy::SYNC_EACH)
        : path_(std::move(path)), policy_(policy) {
        // "ab" creates if missing and positions writes at end (append).
        fp_ = std::fopen(path_.c_str(), "ab");
    }

    ~ColdStore() {
        if (fp_) std::fclose(fp_);
    }

    ColdStore(const ColdStore&) = delete;
    ColdStore& operator=(const ColdStore&) = delete;

    // Append one put durably. Returns after the durable point (fsync) per policy.
    void append_put(uint64_t key, uint64_t value) {
        unsigned char payload[MATRIX_WAL_PAYLOAD_BYTES];
        const uint32_t opcode = OP_WRITE;
        std::memcpy(payload + 0,  &key,    8);
        std::memcpy(payload + 8,  &value,  8);
        std::memcpy(payload + 16, &opcode, 4);

        const uint32_t length = MATRIX_WAL_PAYLOAD_BYTES;
        const uint32_t crc = matrix_crc32(payload, MATRIX_WAL_PAYLOAD_BYTES);

        std::fwrite(&length, sizeof(length), 1, fp_);
        std::fwrite(&crc,    sizeof(crc),    1, fp_);
        std::fwrite(payload, 1, MATRIX_WAL_PAYLOAD_BYTES, fp_);
        std::fflush(fp_);
        if (policy_ == SyncPolicy::SYNC_EACH) ::fsync(::fileno(fp_));
        ++records_written_;
    }

    // Replay every intact record in append order, calling apply(key, value). Stops at the
    // first short read or CRC mismatch (torn/corrupt tail). Missing/empty file → nothing.
    template <typename Apply>
    void replay(Apply&& apply) const {
        FILE* r = std::fopen(path_.c_str(), "rb");
        if (!r) return;
        for (;;) {
            uint32_t length = 0;
            if (std::fread(&length, sizeof(length), 1, r) != 1) break;     // clean EOF
            if (length == 0 || length > MATRIX_WAL_MAX_RECORD) break;      // torn tail
            uint32_t crc = 0;
            if (std::fread(&crc, sizeof(crc), 1, r) != 1) break;           // torn tail
            unsigned char buf[MATRIX_WAL_MAX_RECORD];
            if (std::fread(buf, 1, length, r) != length) break;            // torn tail
            if (matrix_crc32(buf, length) != crc) break;                   // corruption
            if (length == MATRIX_WAL_PAYLOAD_BYTES) {
                uint64_t key = 0, value = 0;
                std::memcpy(&key,   buf + 0, 8);
                std::memcpy(&value, buf + 8, 8);
                apply(key, value);
            }
        }
        std::fclose(r);
    }

    uint64_t records_written() const { return records_written_; }

private:
    std::string path_;
    SyncPolicy policy_;
    FILE* fp_ = nullptr;
    uint64_t records_written_ = 0;
};
```

- [ ] **Step 4: Run test to verify it passes**

Run: `clang++ -std=c++20 -O2 test_cold_store.cpp -o /tmp/tcs && /tmp/tcs`
Expected: `PASS: cold store WAL correct`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add cold_store.hpp test_cold_store.cpp
git -c user.name="garikipatisai-code" -c user.email="garikipatisai-code@users.noreply.github.com" commit -m "feat: ColdStore append-only WAL (length+CRC32 framed) + replay"
```

---

### Task 2: Durability & robustness tests (restart, torn tail, CRC corruption, empty)

**Files:**
- Modify: `test_cold_store.cpp`

- [ ] **Step 1: Add the failing tests — insert before the `std::printf("PASS` line in `test_cold_store.cpp`**

```cpp
    // --- Task 2a: survives restart (write with one ColdStore, read with a fresh one) ---
    {
        std::remove(PATH);
        { ColdStore w(PATH); w.append_put(7, 70); w.append_put(8, 80); } // closed = "restart"
        ColdStore r(PATH);
        uint64_t sum = 0; int n = 0;
        r.replay([&](uint64_t, uint64_t v){ sum += v; ++n; });
        assert(n == 2 && sum == 150 && "data persists across ColdStore instances (restart)");
        std::remove(PATH);
    }

    // --- Task 2b: torn tail — truncating mid-last-record drops only that record ---
    {
        std::remove(PATH);
        { ColdStore w(PATH); w.append_put(1, 11); w.append_put(2, 22); w.append_put(3, 33); }
        // Each record on disk = 4 (len) + 4 (crc) + 20 (payload) = 28 bytes. Truncate the
        // file to 2 full records + a partial third (cut the third's payload).
        FILE* f = std::fopen(PATH, "rb");
        std::fseek(f, 0, SEEK_END); long size = std::ftell(f); std::fclose(f);
        assert(size == 3*28 && "three 28-byte records");
        ::truncate(PATH, 2*28 + 10); // 2 whole records + 10 bytes of the third (torn)
        ColdStore r(PATH);
        int n = 0; uint64_t last = 0;
        r.replay([&](uint64_t k, uint64_t){ ++n; last = k; });
        assert(n == 2 && last == 2 && "torn third record dropped; first two recovered");
        std::remove(PATH);
    }

    // --- Task 2c: CRC corruption — a flipped payload byte stops replay at that record ---
    {
        std::remove(PATH);
        { ColdStore w(PATH); w.append_put(5, 55); w.append_put(6, 66); }
        // Flip a byte inside the FIRST record's payload (offset 8 = start of payload).
        FILE* f = std::fopen(PATH, "rb+");
        std::fseek(f, 8, SEEK_SET);            // first record's payload byte 0
        unsigned char b; std::fread(&b, 1, 1, f);
        b ^= 0xFF;
        std::fseek(f, 8, SEEK_SET); std::fwrite(&b, 1, 1, f);
        std::fclose(f);
        ColdStore r(PATH);
        int n = 0;
        r.replay([&](uint64_t, uint64_t){ ++n; });
        assert(n == 0 && "corruption in the first record stops replay immediately");
        std::remove(PATH);
    }

    // --- Task 2d: empty / missing file replays nothing, no error ---
    {
        std::remove(PATH); // ensure missing
        ColdStore r(PATH); // "ab" creates an empty file
        int n = 0;
        r.replay([&](uint64_t, uint64_t){ ++n; });
        assert(n == 0 && "empty/missing log replays nothing");
        std::remove(PATH);
    }
```

This uses `::truncate` — add `#include <unistd.h>` to `test_cold_store.cpp` if not present (cold_store.hpp includes it, which is transitively visible, but include it explicitly in the test for clarity).

- [ ] **Step 2: Run the tests**

Run: `clang++ -std=c++20 -O2 test_cold_store.cpp -o /tmp/tcs && /tmp/tcs`
Expected: `PASS: cold store WAL correct`, exit 0.

If 2b fails on the `size == 3*28` assert, the on-disk record size differs from the assumed 28 bytes — STOP and report the actual `size` value (the framing is 4+4+20=28; a mismatch means a serialization bug). Do NOT change the assert to match a wrong size; investigate the framing.

- [ ] **Step 3: Commit**

```bash
git add test_cold_store.cpp
git -c user.name="garikipatisai-code" -c user.email="garikipatisai-code@users.noreply.github.com" commit -m "test: ColdStore durability — restart, torn tail, CRC corruption, empty file"
```

---

### Task 3: Wire ColdStore into the CPU engine (durability opt-in)

**Files:**
- Modify: `compute_mock.cpp`

- [ ] **Step 1: Add includes**

At the top of `compute_mock.cpp`, after `#include "kv_store.hpp"`, add:
```cpp
#include "cold_store.hpp"
#include <memory>
#include <string>
```

- [ ] **Step 2: Change the constructor to take an optional WAL path + replay on construct**

Find:
```cpp
    explicit CPUMockEngine(size_t /*worker_count*/ = 0)
        : binned_(MATRIX_BATCH_MAX)
        , scan_column_(MATRIX_SCAN_COLUMN_SIZE) {
        for (size_t i = 0; i < MATRIX_SCAN_COLUMN_SIZE; ++i)
            scan_column_[i] = static_cast<uint32_t>(i); // resident analytical column
        std::cout << "CPUMockEngine initialized (page-ownership, "
                  << MATRIX_PAGE_COUNT << " pages, "
                  << MATRIX_SCAN_COLUMN_SIZE << "-value scan column)." << std::endl;
    }
```
Replace with:
```cpp
    explicit CPUMockEngine(size_t /*worker_count*/ = 0, std::string wal_path = "")
        : binned_(MATRIX_BATCH_MAX)
        , scan_column_(MATRIX_SCAN_COLUMN_SIZE) {
        for (size_t i = 0; i < MATRIX_SCAN_COLUMN_SIZE; ++i)
            scan_column_[i] = static_cast<uint32_t>(i); // resident analytical column
        // Durability is opt-in: with a WAL path, recover the point-op store by replaying
        // the log into kv_ (a write that was committed before a crash is restored here).
        if (!wal_path.empty()) {
            cold_store_ = std::make_unique<ColdStore>(wal_path);
            cold_store_->replay([this](uint64_t k, uint64_t v){ kv_.put(k, v); });
        }
        std::cout << "CPUMockEngine initialized (page-ownership, "
                  << MATRIX_PAGE_COUNT << " pages, "
                  << MATRIX_SCAN_COLUMN_SIZE << "-value scan column"
                  << (cold_store_ ? ", WAL durability ON" : "") << ")." << std::endl;
    }
```

- [ ] **Step 3: Append-before-commit on OP_WRITE**

Find:
```cpp
                } else if (q.opcode == OP_WRITE) {
                    // mock projection: value == key. Fixed-capacity seam: a full table is
                    // counted as an overflow (always live, even under NDEBUG) so a dropped
                    // write is never silent. Inc 3 replaces this with SSD spill. The assert
                    // makes it fail loud in debug builds too.
                    ++writes_;
                    if (kv_.put(q.query_id, q.query_id)) {
                        ++commits_;
                    } else {
                        ++store_overflows_;
                        assert(false && "KVStore full — point-op store capacity exceeded (Inc 3 adds spill)");
                    }
                }
```
Replace with:
```cpp
                } else if (q.opcode == OP_WRITE) {
                    // Durability invariant: append to the WAL FIRST (fsync per policy) so a
                    // write is only counted committed once it is durable. The in-memory kv_
                    // is volatile and rebuilt from the WAL on recovery.
                    if (cold_store_) cold_store_->append_put(q.query_id, q.query_id);
                    // mock projection: value == key. Fixed-capacity seam: a full table is
                    // counted as an overflow (always live, even under NDEBUG) so a dropped
                    // write is never silent. Inc 3's SSD spill is a future seam; the assert
                    // makes it fail loud in debug builds too.
                    ++writes_;
                    if (kv_.put(q.query_id, q.query_id)) {
                        ++commits_;
                    } else {
                        ++store_overflows_;
                        assert(false && "KVStore full — point-op store capacity exceeded (Inc 3 adds spill)");
                    }
                }
```

- [ ] **Step 4: Add the `cold_store_` member**

Find the private member declaration `KVStore kv_{1u << 16}; // 65536 slots` and add immediately after it:
```cpp
    std::unique_ptr<ColdStore> cold_store_; // null = durability off (default); set via WAL path
```

- [ ] **Step 5: Verify the existing pipeline + oracle are unchanged (durability defaults OFF)**

Run: `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o matrixdb_proto && ./matrixdb_proto 2>&1 | grep -E "Engine:|Scan result|completed"`
Expected (identical to before — main constructs `CPUMockEngine(4)`, no WAL path):
```
Engine: reads=4990 writes=5000 commits=5000 scans=10
Scan result sum: 83886070 (oracle 83886070)
Engine execution loop completed successfully.
```

- [ ] **Step 6: Verify the other CPU tests still pass (regression)**

Run: `for t in test_kv_store test_cost_model test_tier_manager test_cold_store; do clang++ -std=c++20 -O2 $t.cpp -o /tmp/$t && /tmp/$t; done`
Expected: four PASS lines.

- [ ] **Step 7: Commit**

```bash
git add compute_mock.cpp
git -c user.name="garikipatisai-code" -c user.email="garikipatisai-code@users.noreply.github.com" commit -m "feat: CPUMockEngine optional WAL — replay on construct, append-before-commit"
```

---

### Task 4: End-to-end restart test (data survives a fresh engine)

**Files:**
- Create: `test_engine_restart.cpp`

- [ ] **Step 1: Write the failing test — create `test_engine_restart.cpp`**

```cpp
// End-to-end durability: point-op writes through one CPUMockEngine survive into a fresh
// engine constructed on the same WAL path (simulates process restart).
// Build: clang++ -std=c++20 -O2 test_engine_restart.cpp -o /tmp/ter && /tmp/ter
#include "compute_mock.cpp"   // pulls in CPUMockEngine (used header-style, as in main.cpp)
#include <cstdio>
#include <cassert>
#include <vector>

static const char* WAL = "/tmp/matrixdb_engine_restart.log";

// Build a batch of OP_WRITE queries for keys [1..n].
static std::vector<DatabaseQuery> write_batch(uint64_t n) {
    std::vector<DatabaseQuery> b(n);
    for (uint64_t i = 0; i < n; ++i) {
        b[i] = DatabaseQuery{};
        b[i].query_id = i + 1;       // keys 1..n; value == key (mock projection)
        b[i].opcode = OP_WRITE;
    }
    return b;
}

int main() {
    std::remove(WAL);

    uint64_t checksum_before = 0;
    {
        CPUMockEngine eng(4, WAL);          // durability ON
        auto batch = write_batch(100);
        eng.execute_batch(batch.data(), batch.size());
        checksum_before = eng.store_checksum();
        assert(checksum_before == 5050 && "sum of values 1..100 (value==key)");
    } // engine destroyed = "process exit"

    {
        CPUMockEngine eng2(4, WAL);         // fresh engine, same WAL → recovery on construct
        const uint64_t checksum_after = eng2.store_checksum();
        assert(checksum_after == checksum_before
               && "writes survived restart: recovered store matches pre-restart");
    }

    std::remove(WAL);
    std::printf("PASS: engine survives restart (WAL recovery)\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails meaningfully first**

Run: `clang++ -std=c++20 -O2 test_engine_restart.cpp -o /tmp/ter && /tmp/ter`
Expected: it should PASS if Task 3 is correct. To confirm the test is non-vacuous (genuinely exercises recovery), temporarily build a control: copy the test, change `CPUMockEngine eng2(4, WAL)` to `CPUMockEngine eng2(4)` (no WAL path → no recovery) in the copy, build+run it, and confirm it ABORTS on the checksum_after assert (recovered store is empty → checksum 0 ≠ 5050). Report what you observed, then discard the copy. (Do not modify the real test.)

- [ ] **Step 3: Commit**

```bash
git add test_engine_restart.cpp
git -c user.name="garikipatisai-code" -c user.email="garikipatisai-code@users.noreply.github.com" commit -m "test: end-to-end engine restart — point-op writes survive via WAL recovery"
```

---

### Task 5: Notebook + register update

**Files:**
- Modify: `make_notebook.py`
- Modify: `PRODUCTION_READINESS.md`

- [ ] **Step 1: Add new files to make_notebook.py SOURCES**

Read `make_notebook.py`. Add to the `SOURCES` list: `cold_store.hpp` (after `kv_store.hpp`), `test_cold_store.cpp` and `test_engine_restart.cpp` (after `test_tier_manager.cpp`). Preserve all existing entries.

- [ ] **Step 2: Add test cells**

In `make_notebook.py`, find the TierManager test cell (`## 3c. TierManager unit test`). After its code entry, insert:
```python
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
```

- [ ] **Step 3: Regenerate + verify notebook in sync**

Run:
```bash
python3 make_notebook.py && python3 -c "
import json
nb=json.load(open('matrixdb_colab.ipynb'))
emb={c['source'].split(chr(10),1)[0].split()[1]:c['source'].split(chr(10),1)[1] for c in nb['cells'] if c['cell_type']=='code' and c['source'].startswith('%%writefile')}
print('in sync:', all(emb[f]==open(f).read() for f in emb))
print('cold_store embedded:', 'cold_store.hpp' in emb)
print('restart test embedded:', 'test_engine_restart.cpp' in emb)
"
```
Expected: all three print `True`.

- [ ] **Step 4: Mark DU-1/2/3 fixed in PRODUCTION_READINESS.md**

In `PRODUCTION_READINESS.md`, in the section-2 (Durability) table, append ` **[FIXED — Inc 3: ColdStore WAL]**` to the DU-1, DU-2, and DU-3 row "Why"/gap cells (keep the rows valid). Then add this line after that table:
```markdown

*Inc 3 landed: `cold_store.hpp` — synchronous CRC-framed append-only WAL, wired into CPUMockEngine (append-before-commit + replay-on-construct). Committed point-op writes survive restart (DU-1/2/3 fixed). Checkpoint/compaction (DU-4) and cold-column spill (Inc 4) still pending. See spec 2026-06-25-increment-3-cold-store-wal-design.md.*
```

- [ ] **Step 5: Commit**

```bash
git add make_notebook.py matrixdb_colab.ipynb PRODUCTION_READINESS.md
git -c user.name="garikipatisai-code" -c user.email="garikipatisai-code@users.noreply.github.com" commit -m "docs: notebook WAL + restart test cells; mark DU-1/2/3 fixed"
```

---

## Self-review (completed)

**Spec coverage:**
- §2 interface (ColdStore ctor, append_put, replay, records_written; SyncPolicy; WalRecord) → Task 1. ✓
- §3 framing (length+CRC32, fsync, inline CRC32) → Task 1 (`cold_store.hpp`). ✓
- §4 recovery (front-to-back, stop at short/CRC-bad tail, missing→empty) → Task 1 replay + Task 2 torn/CRC/empty tests. ✓
- §5 wire-in (optional WAL path, replay on construct, append-before-commit invariant) → Task 3. ✓
- §6 verification (round-trip/restart/torn/CRC/empty + engine restart) → Tasks 1, 2, 4. ✓
- §1/§7 scope: cold-column spill NOT built (no column code touched); checkpoint NOT built; durability opt-in (default path empty → existing oracle unchanged, Task 3 Step 5). ✓

**Placeholder scan:** No TBDs. Every code step is complete. The two runtime-verification points (record size 28 in Task 2; non-vacuous control in Task 4) are explicit with STOP-and-report fallbacks, not vague.

**Type consistency:** `ColdStore(std::string, SyncPolicy=SYNC_EACH)`, `append_put(uint64_t,uint64_t)`, `replay(Apply&&)` calling `apply(key,value)`, `records_written()->uint64_t` consistent across Tasks 1–4. `MATRIX_WAL_PAYLOAD_BYTES=20` ↔ on-disk record 28 bytes (4+4+20) used consistently in Task 2's truncation math. `CPUMockEngine(size_t, std::string="")` (Task 3) ↔ `CPUMockEngine(4, WAL)` (Task 4). `cold_store_` member + `append_put` call consistent.

**Known follow-ups (not this increment):** checkpoint/compaction (DU-4), cold-column spill on the same substrate (Inc 4), `F_FULLFSYNC` for true power-loss durability on macOS (production hardening), multi-writer log locking.

# WAL Checkpoint / Compaction Implementation Plan — DU-4

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Bound WAL size and recovery time: snapshot the point-op store to a checkpoint file, truncate the log, and recover from checkpoint-then-replay. Crash-safe via idempotent replay.

**Spec:** `docs/superpowers/specs/2026-06-27-wal-checkpoint-design.md`

---

### Task 1: for_each + truncate + checkpoint + recovery + test

**Files:** Modify `kv_store.hpp`, `cold_store.hpp`, `compute_mock.cpp`; Create `test_checkpoint.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_checkpoint.cpp`:

```cpp
// CPU test for WAL checkpoint / compaction (DU-4): snapshot kv_ + truncate WAL; recover from both.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <iostream>

// Remove a WAL path and its derived checkpoint/temp files so each sub-test starts clean.
static void clean(const std::string& wal) {
    std::remove(wal.c_str());
    std::remove((wal + ".ckpt").c_str());
    std::remove((wal + ".ckpt.tmp").c_str());
}

// Write keys [lo, hi) as value=key*10+1, each in its own committed transaction.
static void write_range(CPUMockEngine& e, uint64_t lo, uint64_t hi) {
    for (uint64_t k = lo; k < hi; ++k) { e.begin(); e.txn_put(k, k * 10 + 1); e.commit(); }
}

static void test_wal_shrinks_and_recovers() {
    const std::string wal = "/tmp/mdb_ckpt_a.wal"; clean(wal);
    {
        CPUMockEngine e(0, wal);
        e.begin(); for (uint64_t k = 0; k < 100; ++k) e.txn_put(k, k * 10 + 1); e.commit();
        assert(e.wal_records() == 101 && "100 txn-puts + 1 commit marker");
        e.checkpoint();
        assert(e.wal_records() == 0 && e.checkpoints() == 1 && "WAL truncated after checkpoint");
        write_range(e, 100, 110);                       // 10 more txns post-checkpoint
        assert(e.wal_records() == 20 && "10 txn-puts + 10 commit markers since checkpoint");
    }
    {   // restart on the same WAL: checkpoint (0..99) + replayed WAL (100..109)
        CPUMockEngine e(0, wal);
        for (uint64_t k = 0; k < 110; ++k) {
            uint64_t v = 0; assert(e.kv_get(k, v) && v == k * 10 + 1 && "all 110 keys survive restart");
        }
    }
    clean(wal);
    std::cout << "[checkpoint shrink+recover] ok\n";
}

static void test_checkpoint_alone_carries_data() {   // NON-VACUITY: data must come from the checkpoint file
    const std::string wal = "/tmp/mdb_ckpt_b.wal"; clean(wal);
    {
        CPUMockEngine e(0, wal);
        e.begin(); for (uint64_t k = 0; k < 100; ++k) e.txn_put(k, k * 10 + 1); e.commit();
        e.checkpoint();                                 // WAL now empty; data only in the .ckpt file
        assert(e.wal_records() == 0);
    }
    {
        CPUMockEngine e(0, wal);                        // replay sees an EMPTY WAL — only the checkpoint can restore
        size_t found = 0;
        for (uint64_t k = 0; k < 100; ++k) { uint64_t v = 0; if (e.kv_get(k, v) && v == k * 10 + 1) ++found; }
        assert(found == 100 && "checkpoint file alone restored every key");
    }
    clean(wal);
    std::cout << "[checkpoint alone] ok\n";
}

static void test_idempotent_overlap() {                 // a key written before AND after checkpoint
    const std::string wal = "/tmp/mdb_ckpt_c.wal"; clean(wal);
    {
        CPUMockEngine e(0, wal);
        e.begin(); e.txn_put(7, 700); e.commit();
        e.checkpoint();
        e.begin(); e.txn_put(7, 777); e.commit();       // post-checkpoint overwrite
    }
    {
        CPUMockEngine e(0, wal);
        uint64_t v = 0; assert(e.kv_get(7, v) && v == 777 && "last write wins; checkpoint value not stale/doubled");
    }
    clean(wal);
    std::cout << "[idempotent overlap] ok\n";
}

static void test_no_wal_noop() {
    CPUMockEngine e(0, "");                              // durability off
    e.checkpoint();                                     // must not crash
    assert(e.checkpoints() == 0 && e.wal_records() == 0 && "checkpoint is a no-op without a WAL");
    std::cout << "[no-wal no-op] ok\n";
}

int main() {
    test_wal_shrinks_and_recovers();
    test_checkpoint_alone_carries_data();
    test_idempotent_overlap();
    test_no_wal_noop();
    std::cout << "ALL CHECKPOINT TESTS PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_checkpoint.cpp -o /tmp/tckpt && /tmp/tckpt` → FAIL to compile (`checkpoint`, `wal_records`, `checkpoints` undeclared).

- [ ] **Step 3: Add `KVStore::for_each`** — In `kv_store.hpp`, after `checksum()` (after line 64, before the `private:`), add:

```cpp
    // Visit every live entry (snapshot / iteration). Allocation-free; same walk as checksum().
    template <typename F>
    void for_each(F&& f) const { for (const Slot& s : slots_) if (s.occupied) f(s.key, s.value); }
```

- [ ] **Step 4: Add `ColdStore::truncate`** — In `cold_store.hpp`, after `append_commit()` (after its closing brace, line 82) and before the `replay` template, add:

```cpp
    // Empty the log after its contents are captured in a checkpoint. Durable per SyncPolicy.
    void truncate() {
        std::fclose(fp_);
        fp_ = std::fopen(path_.c_str(), "wb");   // "wb" truncates to zero length
        if (!fp_) { std::fprintf(stderr, "ColdStore::truncate: reopen failed %s\n", path_.c_str()); std::abort(); }
        if (policy_ == SyncPolicy::SYNC_EACH) ::fsync(::fileno(fp_));
        std::fclose(fp_);
        fp_ = std::fopen(path_.c_str(), "ab");   // back to append mode
        if (!fp_) { std::fprintf(stderr, "ColdStore::truncate: reopen-append failed %s\n", path_.c_str()); std::abort(); }
        records_written_ = 0;
    }
```

- [ ] **Step 5: Add the checkpoint file I/O + checkpoint() + getters** — In `compute_mock.cpp`: add the magic constant as a `static constexpr` member right beside `MATRIX_CATALOG_MAGIC` (line 411, in the private members block):

```cpp
    static constexpr uint32_t MATRIX_CKPT_MAGIC = 0x4D434B50u; // 'MCKP' — point-op checkpoint file
```

Then add `save_checkpoint`, `load_checkpoint`, `checkpoint`, `checkpoints()`, `wal_records()` as PUBLIC methods (place `save_checkpoint`/`load_checkpoint` next to `save_catalog`/`load_catalog`; `checkpoint()` + getters next to the txn methods `begin`/`commit`). Use spec §3 verbatim (the method bodies reference `MATRIX_CKPT_MAGIC`, which resolves to the member just added):

```cpp
    // Snapshot kv_ atomically to `path`: write temp -> fsync -> rename (POSIX-atomic replace). Fail-loud.
    // ponytail: file data is fsync'd; the rename's own power-loss durability would need a directory fsync
    // (a pre-existing gap shared with the WAL) — the upgrade path if rename-durability matters.
    void save_checkpoint(const std::string& path) {
        const std::string tmp = path + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "save_checkpoint: open failed %s\n", tmp.c_str()); std::abort(); }
        const uint32_t magic = MATRIX_CKPT_MAGIC;
        const uint64_t count = kv_.size();
        bool ok = std::fwrite(&magic, sizeof magic, 1, f) == 1
               && std::fwrite(&count, sizeof count, 1, f) == 1;
        kv_.for_each([&](uint64_t k, uint64_t v) {
            ok = ok && std::fwrite(&k, sizeof k, 1, f) == 1 && std::fwrite(&v, sizeof v, 1, f) == 1;
        });
        std::fflush(f);
        ::fsync(::fileno(f));                       // checkpoint durable BEFORE it replaces the old one
        std::fclose(f);
        if (!ok) { std::fprintf(stderr, "save_checkpoint: short write %s\n", tmp.c_str()); std::abort(); }
        if (std::rename(tmp.c_str(), path.c_str()) != 0) { std::fprintf(stderr, "save_checkpoint: rename failed\n"); std::abort(); }
    }

    // Load a checkpoint into kv_. Missing file -> false (none taken yet). Bad/short -> abort (our format).
    bool load_checkpoint(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        uint32_t magic = 0; uint64_t count = 0;
        bool ok = std::fread(&magic, sizeof magic, 1, f) == 1 && magic == MATRIX_CKPT_MAGIC
               && std::fread(&count, sizeof count, 1, f) == 1;
        for (uint64_t i = 0; ok && i < count; ++i) {
            uint64_t k = 0, v = 0;
            ok = std::fread(&k, sizeof k, 1, f) == 1 && std::fread(&v, sizeof v, 1, f) == 1;
            if (ok) kv_.put(k, v);
        }
        std::fclose(f);
        if (!ok) { std::fprintf(stderr, "load_checkpoint: bad/short %s\n", path.c_str()); std::abort(); }
        return true;
    }

    // Compact the WAL: snapshot the point-op store, then truncate the log (DU-4). No-op if durability off.
    void checkpoint() {
        assert(!in_txn_ && "checkpoint inside a transaction");
        if (!cold_store_) return;
        save_checkpoint(checkpoint_path_);
        cold_store_->truncate();
        ++checkpoints_;
    }

    uint64_t checkpoints() const { return checkpoints_; }
    uint64_t wal_records() const { return cold_store_ ? cold_store_->records_written() : 0; }
```

Note: `MATRIX_CKPT_MAGIC` is the `static constexpr` member added above (same form as `MATRIX_CATALOG_MAGIC`). `std::rename` is in `<cstdio>` (already included).

- [ ] **Step 6: Wire recovery in the ctor** — In `compute_mock.cpp`, REPLACE the recovery block (currently lines 52-55):

```cpp
        if (!wal_path.empty()) {
            cold_store_ = std::make_unique<ColdStore>(wal_path);
            cold_store_->replay([this](uint64_t k, uint64_t v){ kv_.put(k, v); });
        }
```

with:

```cpp
        if (!wal_path.empty()) {
            checkpoint_path_ = wal_path + ".ckpt";
            load_checkpoint(checkpoint_path_);                                    // restore the last compaction (no-op if none)
            cold_store_ = std::make_unique<ColdStore>(wal_path);
            cold_store_->replay([this](uint64_t k, uint64_t v){ kv_.put(k, v); }); // post-checkpoint records on top
        }
```

And add the two members beside the existing `cold_store_` member (grep `cold_store_` declaration, ~line 408):

```cpp
    std::string checkpoint_path_;     // <wal_path>.ckpt — last point-op compaction snapshot
    uint64_t checkpoints_ = 0;        // DU-4: number of WAL compactions performed
```

- [ ] **Step 7: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_checkpoint.cpp -o /tmp/tckpt && /tmp/tckpt` → PASS: `[checkpoint shrink+recover] ok`, `[checkpoint alone] ok`, `[idempotent overlap] ok`, `[no-wal no-op] ok`, `ALL CHECKPOINT TESTS PASSED`. Zero warnings.

- [ ] **Step 8: Confirm no regression** — the recovery path changed, so these MUST still pass:
  - `clang++ -std=c++20 -O2 test_engine_restart.cpp -o /tmp/ter && /tmp/ter | tail -1`
  - `clang++ -std=c++20 -O2 test_transactions.cpp -o /tmp/ttx && /tmp/ttx | tail -1`
  - `clang++ -std=c++20 -O2 test_cold_store.cpp -o /tmp/tcs && /tmp/tcs | tail -1`
  - `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`.
  If any differ, STOP / report BLOCKED (the ctor change likely broke replay).

- [ ] **Step 9: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add kv_store.hpp cold_store.hpp compute_mock.cpp test_checkpoint.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: WAL checkpoint/compaction (DU-4) — snapshot kv_ + truncate the log; recover from checkpoint then replay"
```

---

### Task 2: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (21 tests).**
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by \
         test_query test_observability test_column_io test_catalog_snapshot test_query_validation \
         test_transactions test_server test_security test_audit test_csv_ingest test_checkpoint; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 21× `PASS:`. If any fail, STOP / report BLOCKED with `cat /tmp/out_<test>`.

- [ ] **Step 2: Notebook** — add `"test_checkpoint.cpp"` to `make_notebook.py` SOURCES right after `"test_csv_ingest.cpp"`; add a run cell after the CSV-ingest run cell (the one compiling `test_csv_ingest.cpp` to `/tmp/tcsv`):
```python
    md("### WAL checkpoint / compaction\n"
       "checkpoint() snapshots the point-op store and truncates the write-ahead log, so recovery "
       "loads the snapshot then replays only newer records — WAL size and restart time stay bounded."),
    code("!clang++ -std=c++20 -O2 test_checkpoint.cpp -o /tmp/tckpt 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_checkpoint.cpp -o /tmp/tckpt; /tmp/tckpt"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 40 source files embedded`. Verify `grep -o "test_checkpoint.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed WAL-checkpoint test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** `for_each` (§2)→T1S3; `truncate` (§2)→T1S4; `save/load_checkpoint`+`checkpoint`+getters (§3)→T1S5; ctor recovery (§3)→T1S6; WAL-shrink + restart + non-vacuity + idempotent-overlap + no-wal (§4)→T1S1; regression of the 3 recovery-touching tests (§4)→T1S8; suite+notebook→T2. ✓
**Placeholders:** none. **Type consistency:** `for_each(F&&) const`, `truncate()`, `save_checkpoint(path)`/`load_checkpoint(path)→bool`, `checkpoint()`, `checkpoints()→uint64_t`, `wal_records()→uint64_t`, members `checkpoint_path_`/`checkpoints_` — consistent across spec §2/§3 and plan T1S3-S6 and the test. Test writes via `begin`/`txn_put`/`commit` and reads via `kv_get` (existing public API, confirmed in compute_mock.cpp). `MATRIX_CKPT_MAGIC` placement matched to `MATRIX_CATALOG_MAGIC` (grep in T1S5).

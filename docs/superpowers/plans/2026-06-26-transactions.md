# Atomic Transactions Implementation Plan — TX-1

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** WAL-backed atomic transactions: `begin / txn_put / commit / rollback`, all-or-nothing across a crash, additive to the existing auto-commit WAL path (which stays byte-identical).

**Spec:** `docs/superpowers/specs/2026-06-26-transactions-design.md`

---

### Task 1: WAL transaction support (cold_store.hpp + types.hpp)

**Files:** Modify `types.hpp`, `cold_store.hpp`; Create `test_transactions.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_transactions.cpp` (ColdStore-level crash-atomicity first; engine tests added in Task 2):

```cpp
// CPU test for atomic transactions (WAL group commit). Grows across tasks; one main().
#include "compute_mock.cpp"   // ColdStore + (Task 2) CPUMockEngine txn API
#include <cassert>
#include <cstdint>
#include <vector>
#include <cstdio>
#include <iostream>

// Replay a WAL into a map (key->value) using the transaction-aware replay.
static std::vector<std::pair<uint64_t,uint64_t>> replay_all(const std::string& path) {
    std::vector<std::pair<uint64_t,uint64_t>> applied;
    ColdStore cs(path, SyncPolicy::SYNC_OFF);
    cs.replay([&](uint64_t k, uint64_t v){ applied.emplace_back(k, v); });
    return applied;
}

static void test_wal_commit_atomicity() {
    const std::string path = "/tmp/matrixdb_txn_wal.bin";
    std::remove(path.c_str());
    {
        ColdStore cs(path, SyncPolicy::SYNC_OFF);
        cs.append_txn_put(10, 100);
        cs.append_txn_put(11, 110);
        cs.append_commit();              // committed group {10,11}
        cs.append_txn_put(12, 120);      // a "crash": txn-put with no following commit marker
    }
    auto applied = replay_all(path);
    assert(applied.size() == 2 && "only the committed group replays");
    assert(applied[0].first == 10 && applied[1].first == 11);
    bool saw12 = false; for (auto& kv : applied) if (kv.first == 12) saw12 = true;
    assert(!saw12 && "uncommitted txn-put is discarded (crash-atomic)");
    std::remove(path.c_str());
    std::cout << "[wal commit atomicity] ok\n";
}

static void test_wal_autocommit_unchanged() {
    const std::string path = "/tmp/matrixdb_txn_auto.bin";
    std::remove(path.c_str());
    {
        ColdStore cs(path, SyncPolicy::SYNC_OFF);
        cs.append_put(1, 11);            // auto-commit (OP_WRITE) — applies immediately on replay
        cs.append_put(2, 22);
    }
    auto applied = replay_all(path);
    assert(applied.size() == 2 && applied[0].first == 1 && applied[1].first == 2
           && "auto-commit puts still apply immediately (backward compat)");
    std::remove(path.c_str());
    std::cout << "[wal auto-commit unchanged] ok\n";
}

int main() {
    test_wal_commit_atomicity();
    test_wal_autocommit_unchanged();
    std::cout << "ALL TRANSACTION TESTS PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_transactions.cpp -o /tmp/ttx && /tmp/ttx` → FAIL to compile (`append_txn_put`/`append_commit` undeclared).

- [ ] **Step 3: Add OP_TXN_WRITE** — In `types.hpp`, in the `MatrixOpcode` enum, add after `OP_SCAN = 3,`:

```cpp
    OP_TXN_WRITE = 4,   // a transactional put: buffered on WAL replay until a commit marker
```

- [ ] **Step 4: Add commit-marker constants** — In `cold_store.hpp`, after `constexpr uint32_t MATRIX_WAL_MAX_RECORD = 4096;`, add:

```cpp
constexpr size_t   MATRIX_WAL_COMMIT_BYTES = 4;          // commit-marker record length
constexpr uint32_t MATRIX_WAL_COMMIT       = 0x434F4D4Du; // 'COMM' — commit-marker payload
```

Also add `#include <vector>` and `#include <utility>` to the includes (after `#include <string>`).

- [ ] **Step 5: Refactor append_put + add append_txn_put / append_commit** — In `cold_store.hpp`, REPLACE the current `append_put` method (the whole `void append_put(...) { ... }`) with these three public methods:

```cpp
    // Append one auto-commit put durably (applied immediately on replay). Behavior unchanged.
    void append_put(uint64_t key, uint64_t value) { append_record(OP_WRITE, key, value); }

    // Append a transactional put — buffered on replay until a commit marker; discarded if a crash
    // leaves it without one. Written as part of the engine's commit() group.
    void append_txn_put(uint64_t key, uint64_t value) { append_record(OP_TXN_WRITE, key, value); }

    // Append a commit marker durably — replay applies all txn-puts buffered since the last commit.
    void append_commit() {
        const uint32_t magic = MATRIX_WAL_COMMIT;
        const uint32_t length = static_cast<uint32_t>(MATRIX_WAL_COMMIT_BYTES);
        const uint32_t crc = matrix_crc32(reinterpret_cast<const unsigned char*>(&magic), MATRIX_WAL_COMMIT_BYTES);
        std::fwrite(&length, sizeof(length), 1, fp_);
        std::fwrite(&crc,    sizeof(crc),    1, fp_);
        std::fwrite(&magic,  1, MATRIX_WAL_COMMIT_BYTES, fp_);
        std::fflush(fp_);
        if (policy_ == SyncPolicy::SYNC_EACH) ::fsync(::fileno(fp_));
        ++records_written_;
    }
```

And add the shared private writer in the `private:` section (above the data members):

```cpp
    // Write one length-20 [key8][value8][opcode4] record durably (the put/txn-put wire form).
    void append_record(uint32_t opcode, uint64_t key, uint64_t value) {
        unsigned char payload[MATRIX_WAL_PAYLOAD_BYTES];
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
```

- [ ] **Step 6: Make replay transaction-aware** — In `cold_store.hpp`, REPLACE the body of `replay` with:

```cpp
    template <typename Apply>
    void replay(Apply&& apply) const {
        FILE* r = std::fopen(path_.c_str(), "rb");
        if (!r) return;
        std::vector<std::pair<uint64_t, uint64_t>> txn; // txn-puts buffered since the last commit
        for (;;) {
            uint32_t length = 0;
            if (std::fread(&length, sizeof(length), 1, r) != 1) break;     // clean EOF
            if (length == 0 || length > MATRIX_WAL_MAX_RECORD) break;      // torn tail
            uint32_t crc = 0;
            if (std::fread(&crc, sizeof(crc), 1, r) != 1) break;           // torn tail
            unsigned char buf[MATRIX_WAL_MAX_RECORD];
            if (std::fread(buf, 1, length, r) != length) break;            // torn tail
            if (matrix_crc32(buf, length) != crc) break;                   // corruption
            if (length == MATRIX_WAL_PAYLOAD_BYTES) {                      // 20-byte put record
                uint32_t opcode = 0; std::memcpy(&opcode, buf + 16, 4);
                uint64_t key = 0, value = 0;
                std::memcpy(&key,   buf + 0, 8);
                std::memcpy(&value, buf + 8, 8);
                if (opcode == OP_WRITE)          apply(key, value);        // auto-commit (unchanged)
                else if (opcode == OP_TXN_WRITE) txn.emplace_back(key, value); // buffer until commit
                // other opcode at length 20: skip (forward-compat)
            } else if (length == MATRIX_WAL_COMMIT_BYTES) {               // 4-byte marker
                uint32_t magic = 0; std::memcpy(&magic, buf, 4);
                if (magic == MATRIX_WAL_COMMIT) { for (auto& kv : txn) apply(kv.first, kv.second); txn.clear(); }
                // other 4-byte record: skip (forward-compat)
            }
            // other lengths: skip (forward-compat)
        }
        std::fclose(r);   // EOF: any still-buffered txn was uncommitted -> discarded
    }
```

- [ ] **Step 7: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_transactions.cpp -o /tmp/ttx && /tmp/ttx` → PASS, prints `[wal commit atomicity] ok` + `[wal auto-commit unchanged] ok` + `ALL TRANSACTION TESTS PASSED`. No warnings.

- [ ] **Step 8: Confirm the existing WAL tests are unchanged** — `clang++ -std=c++20 -O2 test_cold_store.cpp -o /tmp/tcs && /tmp/tcs | tail -1` → `PASS: cold store WAL correct`; `clang++ -std=c++20 -O2 test_engine_restart.cpp -o /tmp/ter && /tmp/ter | tail -1` → `PASS: engine survives restart (WAL recovery)`. (Both use `append_put`/OP_WRITE → still apply immediately → unchanged.) If either fails, STOP / report BLOCKED.

- [ ] **Step 9: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add types.hpp cold_store.hpp test_transactions.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: WAL transaction support — append_txn_put/append_commit + commit-aware replay (auto-commit path unchanged)"
```

---

### Task 2: Engine transaction API (begin/commit/rollback)

**Files:** Modify `compute_mock.cpp`; Modify `test_transactions.cpp`.

- [ ] **Step 1: Write the failing test** — In `test_transactions.cpp`, add above `main`:

```cpp
static void test_engine_commit_durable() {
    const std::string wal = "/tmp/matrixdb_txn_eng.bin";
    std::remove(wal.c_str());
    {
        CPUMockEngine eng(0, wal);            // durability ON
        eng.begin();
        eng.txn_put(100, 1000);
        eng.txn_put(101, 1010);
        eng.txn_put(102, 1020);
        eng.commit();
        uint64_t v = 0;
        assert(eng.kv_get(100, v) && v == 1000);
        assert(eng.kv_get(102, v) && v == 1020);
    }
    {
        CPUMockEngine eng2(0, wal);           // replay the committed txn on restart
        uint64_t v = 0;
        assert(eng2.kv_get(100, v) && v == 1000 && "committed txn survives restart");
        assert(eng2.kv_get(101, v) && v == 1010);
        assert(eng2.kv_get(102, v) && v == 1020);
    }
    std::remove(wal.c_str());
    std::cout << "[engine commit durable] ok\n";
}

static void test_engine_rollback() {
    const std::string wal = "/tmp/matrixdb_txn_rb.bin";
    std::remove(wal.c_str());
    {
        CPUMockEngine eng(0, wal);
        eng.begin();
        eng.txn_put(200, 2000);
        eng.rollback();
        uint64_t v = 0;
        assert(!eng.kv_get(200, v) && "rolled-back write is not visible");
    }
    {
        CPUMockEngine eng2(0, wal);
        uint64_t v = 0;
        assert(!eng2.kv_get(200, v) && "rolled-back write was never persisted");
    }
    std::remove(wal.c_str());
    std::cout << "[engine rollback] ok\n";
}
```

Add `test_engine_commit_durable();` and `test_engine_rollback();` to `main()` after the Task-1 tests.

- [ ] **Step 2: Run to verify it fails** — `clang++ -std=c++20 -O2 test_transactions.cpp -o /tmp/ttx && /tmp/ttx` → FAIL to compile (`begin`/`txn_put`/`commit`/`rollback`/`kv_get` undeclared).

- [ ] **Step 3: Add a kv_get accessor + apply_committed_write helper** — In `compute_mock.cpp`, add a public test accessor near the other accessors (after `column_checksum`):

```cpp
    // Point-op read accessor (tests): true + sets out if present. Mirrors execute_batch's OP_READ.
    bool kv_get(uint64_t key, uint64_t& out) const { return kv_.get(key, out); }
```

Add a private helper (near the members / private section):

```cpp
    // Apply one durable write to the point-op store with the standard overflow accounting.
    void apply_committed_write(uint64_t key, uint64_t value) {
        ++writes_;
        if (kv_.put(key, value)) ++commits_;
        else { ++store_overflows_; assert(false && "KVStore full — point-op store capacity exceeded (Inc 3 adds spill)"); }
    }
```

- [ ] **Step 4: Route OP_WRITE through the helper** — In `compute_mock.cpp`'s `execute_batch`, replace the OP_WRITE accounting block (`++writes_;` through the closing brace of the `if (kv_.put(...))`/`else` — lines around 257-263) with a single call, keeping the WAL append above it:

```cpp
                    if (cold_store_) cold_store_->append_put(q.query_id, q.query_id);
                    apply_committed_write(q.query_id, q.query_id);
```

(This is behavior-preserving — `apply_committed_write` does exactly what the inline block did.)

- [ ] **Step 5: Add the transaction API + members** — In `compute_mock.cpp`, add public methods (e.g. after `kv_get`):

```cpp
    // --- Atomic transactions (WAL group commit) ---
    void begin() { assert(!in_txn_ && "transaction already open"); txn_buf_.clear(); in_txn_ = true; }
    void txn_put(uint64_t key, uint64_t value) { assert(in_txn_ && "txn_put outside a transaction"); txn_buf_.emplace_back(key, value); }
    // Durably commit the buffered writes as one all-or-nothing group, then apply them.
    void commit() {
        assert(in_txn_ && "commit without begin");
        if (cold_store_) {
            for (auto& kv : txn_buf_) cold_store_->append_txn_put(kv.first, kv.second);
            cold_store_->append_commit();   // the durable, atomic commit point
        }
        for (auto& kv : txn_buf_) apply_committed_write(kv.first, kv.second);
        in_txn_ = false; ++transactions_committed_; txn_buf_.clear();
    }
    void rollback() { assert(in_txn_ && "rollback without begin"); in_txn_ = false; ++transactions_rolled_back_; txn_buf_.clear(); }
    uint64_t transactions_committed() const { return transactions_committed_; }
    uint64_t transactions_rolled_back() const { return transactions_rolled_back_; }
```

Add the members (near `scans_since_rebalance_` / the counters; needs `<utility>` for pair — already pulled in via cold_store.hpp):

```cpp
    std::vector<std::pair<uint64_t, uint64_t>> txn_buf_; // pending writes in the open transaction
    bool in_txn_ = false;
    uint64_t transactions_committed_ = 0;
    uint64_t transactions_rolled_back_ = 0;
```

- [ ] **Step 6: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_transactions.cpp -o /tmp/ttx && /tmp/ttx` → PASS, prints all four `ok` lines + `ALL TRANSACTION TESTS PASSED`. No warnings.

- [ ] **Step 7: Confirm no regression** — `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`; `clang++ -std=c++20 -O2 test_engine_restart.cpp -o /tmp/ter && /tmp/ter | tail -1` → `PASS: engine survives restart (WAL recovery)`. If either differs, STOP / report BLOCKED.

- [ ] **Step 8: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add compute_mock.cpp test_transactions.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: engine transactions — begin/txn_put/commit/rollback (WAL group commit, crash-atomic)"
```

---

### Task 3: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (16 tests).**
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by \
         test_query test_observability test_column_io test_catalog_snapshot test_query_validation test_transactions; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 16× `PASS:`. If any fail, STOP / report BLOCKED with `cat /tmp/out_<test>` (esp. test_cold_store / test_engine_restart — they must be unchanged-green).

- [ ] **Step 2: Notebook** — add `"test_transactions.cpp"` to `make_notebook.py` SOURCES (after `"test_query_validation.cpp"`); add a run cell after the query-validation run cell:
```python
    md("### Atomic transactions (WAL group commit)\n"
       "begin/txn_put/commit/rollback — a committed transaction is all-or-nothing across a crash "
       "(uncommitted txn-puts are discarded on replay); the auto-commit path is unchanged."),
    code("!clang++ -std=c++20 -O2 test_transactions.cpp -o /tmp/ttx 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_transactions.cpp -o /tmp/ttx; /tmp/ttx"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 33 source files embedded`. Verify `grep -o "test_transactions.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed transactions test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** OP_TXN_WRITE + commit constants + append_txn_put/append_commit + commit-aware replay (§2/§3)→T1S3-6; engine begin/commit/rollback + apply_committed_write + kv_get (§4)→T2S3-5; crash-atomicity + commit-durable + rollback + auto-commit-unchanged (§5)→T1/T2 tests; backward-compat (test_cold_store/test_engine_restart unchanged)→T1S8/T2S7; suite+notebook→T3. ✓
**Placeholders:** none. **Type consistency:** `OP_TXN_WRITE`, `MATRIX_WAL_COMMIT(_BYTES)`, `append_txn_put`/`append_commit`/`append_record`, `begin`/`txn_put`/`commit`/`rollback`/`kv_get`/`apply_committed_write`, `txn_buf_`/`in_txn_`/`transactions_committed_` consistent T1/T2/T3. OP_WRITE path behavior preserved (apply_committed_write == old inline block).

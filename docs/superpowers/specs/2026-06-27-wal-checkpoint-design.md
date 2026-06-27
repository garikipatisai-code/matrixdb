# Design: WAL Checkpoint / Compaction — DU-4

**Status:** approved-by-standing-directive (goal: continue all phases), pre-implementation. **Date:** 2026-06-27.
**Builds on:** Inc 3 (`ColdStore` WAL), TX-1 (txn commit path), CKPT-1 (the snapshot-file pattern).
**Fully local: file I/O only, no transport.**

**Thesis:** *The point-op WAL grows without bound — `ColdStore::replay` re-applies every write ever made
on every restart, so recovery time and disk both grow forever. Add a checkpoint: snapshot the point-op
store (`kv_`) to a file, then truncate the WAL. Recovery loads the checkpoint and replays only the
records written since it, bounding both. Crash-safe by construction: replayed puts are idempotent
(last-writer-wins per key), so "load checkpoint, then replay whatever WAL survived" yields the same
state no matter where a crash lands.*

---

## 1. Scope

**IN:**
- `KVStore::for_each(F&&) const` — visit every occupied `(key, value)` (allocation-free, mirrors `checksum()`'s slot walk). Needed to snapshot the store.
- `ColdStore::truncate()` — reset the WAL file to empty (reopen `"wb"` → fsync → reopen `"ab"`), `records_written_ = 0`. Respects `SyncPolicy`.
- `CPUMockEngine::save_checkpoint(path)` / `load_checkpoint(path)` — a kv snapshot file `[u32 magic 'MCKP'][u64 count]{[u64 key][u64 value]}`. Write is atomic-replace: temp file → fsync → `rename` (POSIX-atomic). Read: missing file → `false` (no checkpoint taken yet, not an error); bad magic / short read → fail-loud `abort` (corruption of our OWN format, like `column_io.hpp`).
- `CPUMockEngine::checkpoint()` — `save_checkpoint(checkpoint_path_)` then `cold_store_->truncate()`; `++checkpoints_`. No-op if durability is off (`!cold_store_`). Asserts `!in_txn_` (checkpoint at a clean boundary).
- Ctor recovery becomes: derive `checkpoint_path_ = wal_path + ".ckpt"`; `load_checkpoint` (populate `kv_` from the last snapshot) **then** open the `ColdStore` and `replay` the WAL (post-checkpoint records). Order matters — checkpoint first, then the log on top.
- Observability getters: `checkpoints()`, `wal_records()` (→ `cold_store_->records_written()`, or 0 if off) — for the test to prove the WAL shrank.

**Crash-safety (why it's correct):**
- `checkpoint()` does `save_checkpoint` (captures **all** writes ≤ T) **before** `truncate` (which removes only records ≤ T — at that instant the WAL holds nothing newer, since new writes append only after `checkpoint()` returns). So truncation can never drop a write absent from the checkpoint.
- Crash **between** save and truncate → recovery = checkpoint (≤ T) + full WAL (≤ T and > T). The overlap re-applies idempotently (kv `put` overwrites), so the result is identical. No double-count, no loss.
- Crash **during** truncate → the WAL is either its old self or empty; both, combined with the durable checkpoint, recover the same ≤ T state. Writes > T do not exist yet.

**OUT (deferred — noted in code):**
- Directory `fsync` after the temp-rename (and after WAL create) — the file-data `fsync` is here; making the *rename itself* power-loss-durable needs a dir `fsync`. This is a pre-existing gap across the whole persistence layer (the WAL never dir-fsync'd either), not introduced here; one `ponytail:` note, upgrade path flagged.
- Automatic checkpoint triggers (every N records / size threshold) — `checkpoint()` is explicit/caller-driven for now; an auto-policy is a thin add on top.
- Checkpointing the analytical catalog WAL — the catalog has no WAL (it's snapshotted whole by CKPT-1); DU-4 is the point-op log.
- WAL segment rotation / multiple segments — single-file truncate is enough at this scale.

---

## 2. KVStore::for_each + ColdStore::truncate

```cpp
// KVStore — visit every live entry (snapshot/iteration). Allocation-free; same walk as checksum().
template <typename F>
void for_each(F&& f) const { for (const Slot& s : slots_) if (s.occupied) f(s.key, s.value); }
```

```cpp
// ColdStore — empty the log (after its contents are captured in a checkpoint). Durable per policy.
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

---

## 3. Engine: checkpoint file I/O + checkpoint() + recovery

```cpp
inline constexpr uint32_t MATRIX_CKPT_MAGIC = 0x4D434B50u; // 'MCKP' — point-op checkpoint file

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

Ctor recovery (replaces the current `if (!wal_path.empty()) { … }` block):

```cpp
if (!wal_path.empty()) {
    checkpoint_path_ = wal_path + ".ckpt";
    load_checkpoint(checkpoint_path_);                       // restore the last compaction (no-op if none)
    cold_store_ = std::make_unique<ColdStore>(wal_path);
    cold_store_->replay([this](uint64_t k, uint64_t v){ kv_.put(k, v); });   // post-checkpoint records on top
}
```

New members: `std::string checkpoint_path_;` and `uint64_t checkpoints_ = 0;`.

---

## 4. Verification (`test_checkpoint.cpp`, CPU)

All via the public durable-write path (`begin`/`txn_put`/`commit`) on an engine with a WAL:

1. **WAL shrinks.** Write 100 keys (one txn) → `wal_records() == 101` (100 txn-puts + 1 commit). `checkpoint()` → `wal_records() == 0`, `checkpoints() == 1`, the `.ckpt` file exists.
2. **Survives restart via checkpoint + WAL.** After (1), write keys 100..109 (another txn). Destroy the engine; construct a fresh one on the same WAL path. Assert all 110 keys read back with correct values.
3. **Non-vacuity — checkpoint alone carries the data.** Fresh WAL: write 100 keys, `checkpoint()` (WAL now empty), **no** further writes. Restart. Assert all 100 present. They can ONLY come from the checkpoint file (the WAL is empty) — if `save_checkpoint` didn't persist `kv_`, this fails (empty store). This is the guard that proves the feature real.
4. **Idempotent overlap.** Write key 7 = 700; `checkpoint()`; write key 7 = 777 (post-checkpoint). Restart. Assert `kv_get(7) == 777` (last write wins; the checkpoint's 700 is correctly overwritten by the replayed 777, not double-applied or stale).
5. **No-WAL no-op.** An engine with no WAL: `checkpoint()` does nothing and does not crash; `checkpoints() == 0`, `wal_records() == 0`.

Plus: full CPU suite (now 21 tests) + oracle `83886070` unchanged; `test_engine_restart` + `test_transactions` + `test_cold_store` still green (recovery path changed — they must still pass); notebook regenerated.

---

## 5. Open / deferred
Directory `fsync` for rename/create durability; automatic checkpoint triggers (size/record thresholds);
WAL segment rotation; checkpoint of the analytical catalog's (absent) WAL; incremental/delta checkpoints.

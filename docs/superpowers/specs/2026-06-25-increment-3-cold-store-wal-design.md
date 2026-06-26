# Design: Increment 3 — ColdStore (SSD WAL + durability)

**Status:** approved design (brainstorm sections walked + approved), pre-implementation
**Date:** 2026-06-25
**Parent spec:** `2026-06-25-three-tier-storage-engine-design.md` (Increment 3 row of §7; substrate in §3)

**Thesis:** *The SSD tier's defining strength — sequential append — is exactly a write-ahead
log's access pattern, so the cold tier IS the durability substrate. One append-only log makes
committed writes survive a crash.*

This is the increment where **MatrixDB stops losing data on restart** — the single biggest
step toward being a real database.

---

## 1. Scope

Build a durable append-only log (`ColdStore`) and wire it into the CPU engine's point-op
write path so committed writes survive process restart / crash.

**IN:**
- `cold_store.hpp` — append-only WAL on disk (synchronous, length+CRC32 framed) + recovery replay.
- `test_cold_store.cpp` — durability + torn-tail + corruption tests over real temp files.
- Wire into `CPUMockEngine`: every OP_WRITE appends to the WAL before ack; on construction,
  replay the WAL to rebuild the KVStore. Delivers "survives restart."
- A restart test in the engine/pipeline proving data persists across a fresh engine instance.

**OUT (deferred, marked at the seam):**
- Cold-*column* spill (demoting analytical columns to SSD) — needs the Inc-4 byte-movement
  executor; the append-log substrate is *designed* to also hold columns later, but that's
  not built here.
- Checkpoint/compaction (DU-4) — the log grows unbounded for now; `checkpoint()` is a
  documented stretch, deferred unless trivial.
- GPU engine durability — point-ops are CPU-only (proven thesis); the GPU engine has no
  point-op store to persist.
- Async/batched fsync level — a future `SyncPolicy` value; the seam is present.

---

## 2. Component & interface

One new header-only unit, `cold_store.hpp`. Single responsibility: durable append + replay.
"SSD" is a file on the dev box — the interface is what matters; it works identically on a
real SSD.

```cpp
enum class SyncPolicy {
    SYNC_EACH,  // fsync after every append — a committed write survives a crash (default)
    SYNC_OFF,   // no fsync — tests / throughput; crash loses unflushed tail
};

// One logical mutation in the log. POD, fixed-size payload (matches the KVStore put).
struct WalRecord {
    uint64_t key;
    uint64_t value;
    uint32_t opcode;   // OP_WRITE today; room for delete/other later
};

class ColdStore {
public:
    explicit ColdStore(std::string path, SyncPolicy policy = SyncPolicy::SYNC_EACH);
    ~ColdStore();                         // flush + close the file handle

    // Append a put durably. Frame: [u32 length][u32 crc32(payload)][payload]. Writes the
    // frame, then fsync per policy. Returns only after the durable point is reached.
    void append_put(uint64_t key, uint64_t value);

    // Replay every intact record in append order, calling apply(key, value) for each.
    // Stops at the first short read or CRC mismatch (the torn/corrupt tail) — never
    // replays corruption. A missing/empty file replays nothing (clean first boot).
    template <typename Apply>
    void replay(Apply&& apply) const;

    uint64_t records_written() const;     // appends this session (telemetry/tests)

private:
    // length+crc framing, fsync, file handle, path, policy
};
```

Dependencies: standard library + `types.hpp` (for `OP_WRITE`). No new third-party deps;
CRC32 implemented inline (small standard table). No GPU, no engine coupling in the unit
itself — the wire-in lives in `compute_mock.cpp`.

---

## 3. Record framing, sync, and CRC

- **Frame:** `[uint32 length][uint32 crc32][payload(length) bytes]`, where payload is a
  serialized `WalRecord` (key, value, opcode). `length` is the payload byte count;
  `crc32` is computed over the payload.
- **Sync:** `SYNC_EACH` → after writing the frame, `fsync(fd)` (or `fflush`+`fsync`) before
  returning, so a returned `append_put` is durable. `SYNC_OFF` skips fsync (tests). The
  async/batched policy from brainstorming is a future enum value — seam only.
- **CRC32:** inline implementation (standard polynomial table, ~15 lines), no dependency.

---

## 4. Recovery

`replay()` reads front-to-back:
1. Read 4 bytes `length`. If fewer than 4 bytes remain → end (clean EOF).
2. If `length` is out of sane range (0 or > max record) → torn tail, stop.
3. Read 4 bytes `crc` + `length` payload bytes. If short → torn tail, stop.
4. Compute crc32(payload); if ≠ stored `crc` → corruption, stop.
5. Deserialize `WalRecord`, call `apply(key, value)`, continue.

Replaying in append order reconstructs last-writer-wins for free (later puts overwrite
earlier in the KVStore). Stopping at the first bad record means a crash mid-append loses
only the in-flight write, never corrupts recovery. Missing file → zero records.

---

## 5. Engine wire-in (delivers "survives restart")

`CPUMockEngine` gains an optional WAL path (a ctor parameter; default empty = no durability,
preserving existing in-memory tests):
- **On construct (if a WAL path is given):** open the `ColdStore` and `replay()` it into
  `kv_` — rebuilding the point-op store from disk. This is recovery.
- **On OP_WRITE:** append to the WAL FIRST (`cold_store_->append_put(key, value)`, which
  fsyncs per policy), THEN `kv_.put(...)` into the in-memory store, THEN `++commits_`.
  **Invariant: a write is counted as committed only after its WAL append has returned
  durable.** The in-memory KVStore is volatile and is rebuilt from the WAL on recovery, so
  the WAL is the source of truth; ordering append-before-commit guarantees that anything
  acked survives a crash, and a crash between append and the in-memory put is harmless
  (recovery replays the appended record).
- Reads/scans unchanged.

The existing default-constructed engine (no WAL path) behaves exactly as before — all
current tests stay green. Durability is opt-in via the path, which is how the restart test
and a future server config turn it on.

---

## 6. Verification

**`test_cold_store.cpp`** (CPU, real temp files via a unique path, cleaned up):
- **Append + replay:** N puts → replay yields all, correct values, last-writer-wins on a
  repeated key.
- **Survives restart:** append with one `ColdStore`, destroy it, open a NEW `ColdStore` on
  the same path, replay → data present (simulates process restart).
- **Torn tail:** append several records, truncate the file mid-last-record → replay returns
  exactly the records before the tear, no crash.
- **CRC corruption:** flip a byte inside a complete record's payload → replay stops there
  (bit-rot detected), earlier records intact.
- **Empty/missing file:** replay yields nothing, no error.

**Engine restart test** (in the test suite / a small harness): create a `CPUMockEngine` with
a WAL path, write known keys, destroy it; create a second engine on the same WAL path; assert
its `store_checksum()` / reads reflect the persisted writes. Proves end-to-end durability.

Discipline carried forward: notebook regenerated with the new files + a `ColdStore` test cell;
all existing CPU tests + the pipeline oracle stay green.

---

## 7. Open / deferred items (not blockers)

- Checkpoint/compaction (DU-4): log grows unbounded; add `checkpoint()` to compact to current
  state later. `fsync` cost vs. checkpoint cadence is a calibration item.
- Real fsync durability nuance: `fsync` on macOS may need `F_FULLFSYNC` for true power-loss
  durability; note it, use `fsync` for the increment (sufficient for crash-consistency in
  the dev/test environment), flag `F_FULLFSYNC` as the production hardening.
- Cold-column storage on the same substrate — Inc 4.
- Concurrency: single-owner append (the point-op consumer is single-threaded per the
  page-ownership model), so no log locking yet; revisit if multiple writers append.

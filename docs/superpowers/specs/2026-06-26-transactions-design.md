# Design: Atomic Transactions (WAL group commit) — TX-1

**Status:** approved-by-standing-directive, pre-implementation. **Date:** 2026-06-26.
**Builds on:** Inc-3 ColdStore WAL (`append_put`/`replay`, CRC-framed, fsync), CPUMockEngine point-op store.
**Fully local + crash-simulatable — no infra (verified: this axis needs no sockets).**

**Thesis:** *The WAL gives durability (D) but each write auto-commits independently — there is no
multi-write atomicity (A). Add WAL-backed transactions: `begin / txn_put / commit / rollback`,
where a commit is all-or-nothing across a crash. The existing auto-commit `OP_WRITE` path stays
byte-identical (transactions are an additive new record type), so no existing WAL test changes.*

---

## 1. Scope

**IN:**
- `OP_TXN_WRITE` opcode (types.hpp) + a `MATRIX_WAL_COMMIT` marker magic (cold_store.hpp).
- ColdStore gains `append_txn_put(key, value)` (a length-20 record with opcode `OP_TXN_WRITE`) and
  `append_commit()` (a length-4 marker record). `replay` becomes transaction-aware:
  - length-20 `OP_WRITE` → apply immediately (**existing auto-commit — unchanged**).
  - length-20 `OP_TXN_WRITE` → buffer (don't apply yet).
  - length-4 `MATRIX_WAL_COMMIT` → apply the buffered txn-puts, clear the buffer.
  - clean EOF / torn or corrupt tail → **discard** the buffer (an uncommitted transaction).
  - any other CRC-valid length → skip (existing forward-compat).
- CPUMockEngine transaction API (direct methods, like `load_scan_column`):
  `begin()` (start an in-engine buffer), `txn_put(key, value)` (buffer), `commit()` (flush the whole
  group to the WAL as `append_txn_put × N` + `append_commit`, then apply all to `kv_`), `rollback()`
  (discard the buffer — nothing written). Works with or without a WAL (durable + crash-atomic only
  with a WAL; in-memory atomic-rollback either way). A `transactions_committed_` / `_rolled_back_`
  counter for observability.
- `test_transactions.cpp` — commit visibility + durability (replay), rollback discards, and the
  **crash-atomicity** proof at the ColdStore level (txn-puts without a commit marker → replay
  applies nothing) + mixed committed-then-crashed.

**OUT (deferred):** isolation / concurrent transactions (MVCC, locking) — single-writer-per-page
already serializes; full isolation is a bigger increment; savepoints / nested txns; transactional
*reads* (snapshot isolation); transactions over the analytical catalog (this is the point-op store).

---

## 2. Why it's additive + crash-atomic (the key reasoning)

- **Backward compatibility:** `OP_WRITE` length-20 records still apply immediately on replay, so
  `append_put` (the engine's auto-commit path, `test_cold_store`, `test_engine_restart`) is
  unchanged — those tests pass as-is. Transactions use a *distinct* opcode (`OP_TXN_WRITE`) that
  replay buffers instead of applying, plus the commit marker.
- **Crash-atomicity (single-threaded, crash-truncates-tail):** `commit()` writes the group
  `[txn_put×N][commit]` sequentially. A crash truncates the file's tail, so a crashed transaction is
  always the *trailing* group — buffered txn-puts with no following commit marker → replay's EOF
  path discards them. A committed group ends with its commit marker (which clears the buffer), so a
  later crashed group never merges with an earlier committed one. No BEGIN marker is needed for this
  single-threaded, append-only, tail-truncating model (documented assumption; a BEGIN delimiter is
  the upgrade if concurrent/interleaved transactions are added later).
- **fsync:** `append_commit` fsyncs per the WAL's `SyncPolicy` — `commit()` returns only once the
  commit marker is durable, so a returned commit survives a crash (all-or-nothing).

---

## 3. Record formats (extend the existing `[u32 len][u32 crc][payload]` frame)

- **Auto-commit put (existing, unchanged):** len 20, payload `[key8][value8][opcode4=OP_WRITE]`.
- **Txn put (new):** len 20, payload `[key8][value8][opcode4=OP_TXN_WRITE]`. `append_txn_put` is
  `append_put` with the opcode field set to `OP_TXN_WRITE`.
- **Commit marker (new):** len 4, payload `[u32 = MATRIX_WAL_COMMIT]` (a distinct magic). A len-4
  record whose payload isn't the magic is skipped (forward-compat).

Replay reads the opcode at `payload+16` for len-20 records (it currently ignores it); this is a
small, behavior-preserving change (OP_WRITE still applies). The buffer is a
`std::vector<std::pair<uint64_t,uint64_t>>` local to `replay`.

---

## 4. Engine API & flow

```
begin():     txn_buf_.clear(); in_txn_ = true;
txn_put(k,v): assert(in_txn_); txn_buf_.emplace_back(k, v);
commit():    assert(in_txn_);
             if (cold_store_) { for (kv) cold_store_->append_txn_put(kv.k, kv.v); cold_store_->append_commit(); }
             for (kv) { ++writes_; if (kv_.put(kv.k, kv.v)) ++commits_; else { ++store_overflows_; } }
             in_txn_ = false; ++transactions_committed_; txn_buf_.clear();
rollback():  assert(in_txn_); in_txn_ = false; ++transactions_rolled_back_; txn_buf_.clear();   // nothing written
```
- Recovery: the engine's existing `replay(...)` on construction now applies committed txn groups too
  (the transaction-aware replay), in addition to auto-commit puts. So a committed transaction
  survives restart automatically.
- Applying to `kv_` reuses the existing put-with-overflow accounting, so transactional writes show
  in the same `writes_`/`commits_`/`store_overflows_` counters (consistent with auto-commit writes).

---

## 5. Verification (`test_transactions.cpp`, CPU, real WAL temp files)

- **Commit visibility + durability:** engine with a WAL; `begin()`, `txn_put` keys 100/101/102,
  `commit()`; `get` returns the values. A **fresh** engine on the same WAL path (replay) has all
  three (committed txn survives restart).
- **Rollback discards:** `begin()`, `txn_put` key 200, `rollback()`; `get(200)` misses; a fresh
  engine's replay has no 200 (nothing was written).
- **Crash-atomicity (ColdStore level, deterministic):** a ColdStore: `append_txn_put`×2 +
  `append_commit` (committed), then `append_txn_put`×1 (a "crash" — no commit). A fresh ColdStore
  `replay` applies exactly the 2 committed, **discards** the 1 uncommitted. Proves all-or-nothing.
- **Backward-compat (regression):** an `append_put` (OP_WRITE) record still applies immediately on
  replay (interleaved with txn groups) — and `test_cold_store` / `test_engine_restart` pass unchanged.
- **Non-vacuity:** the rollback test fails if rollback wrongly persisted; the crash-atomicity test
  fails if the uncommitted put were applied (a no-op "transaction" that just auto-commits each write
  would fail both).

Plus: pipeline oracle `83886070` unchanged (it has no WAL / no transactions); all existing tests
green; notebook regenerated.

---

## 6. Open / deferred
- Isolation (MVCC / lock-based) for concurrent transactions; transactional snapshot reads;
  savepoints/nested; a BEGIN delimiter for interleaved transactions; transactions over the
  analytical catalog; group-commit batching across transactions for throughput.

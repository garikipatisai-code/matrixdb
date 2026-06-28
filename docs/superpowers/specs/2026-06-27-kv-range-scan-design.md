# Design: Key Range Scan over the Point-Op Store (DM-7)

**Status:** approved-by-standing-directive (continue all phases, don't wait). **Date:** 2026-06-27.
**Builds on:** DM-1 (`KVStore`), DU-4 (`KVStore::for_each`).
**Fully local. Small increment — implemented directly (one method + test).**

**Thesis:** *The point-op store has only exact-key `kv_get`; there's no way to retrieve all records whose
key falls in a range — a fundamental query access path. Add `kv_range(lo, hi)` returning every `(key,
value)` with `lo ≤ key ≤ hi`, reusing `KVStore::for_each`. Correctness-complete; an O(n) full scan (the
store is an unordered open-addressing hash) — a sorted secondary index for log-time selective ranges is
the documented upgrade (the "index" half of DM-7). Additive; nothing else changes.*

---

## 1. Scope & API

`CPUMockEngine` (near `kv_get`):
```cpp
// Collect every (key, value) with lo <= key <= hi (inclusive). Order unspecified (hash order — sort if
// needed). ponytail: O(capacity) full scan of the point-op store; a sorted secondary index is the
// upgrade for selective ranges over large stores (the "index" half of DM-7, deferred).
std::vector<std::pair<uint64_t, uint64_t>> kv_range(uint64_t lo, uint64_t hi) const {
    std::vector<std::pair<uint64_t, uint64_t>> out;
    kv_.for_each([&](uint64_t k, uint64_t v) { if (k >= lo && k <= hi) out.emplace_back(k, v); });
    return out;
}
```

**Invariants:** read-only; inclusive `[lo, hi]`; reuses the existing `for_each` (DU-4). The point-op
store and all else unchanged. Oracle untouched.

**OUT:** a sorted secondary index (log-time ranges); range over analytical catalog columns (those are
scanned/aggregated, not key-addressed); `<` / open-ended ranges (caller passes `0`/`UINT64_MAX`).

---

## 2. Verification (`test_kv_range.cpp`, CPU)

- Put keys {5, 10, 15, 20, 100} (via `begin`/`txn_put`/`commit`). `kv_range(10, 20)` → exactly
  {(10,…),(15,…),(20,…)} (sort the result; values correct). `kv_range(0, UINT64_MAX)` → all 5.
  `kv_range(11, 14)` → empty. Boundary inclusivity: `kv_range(5, 5)` → {(5,…)}.
- **Non-vacuity**: a key just outside the range (e.g. 100 for `[10,20]`) is excluded; a key on each
  boundary is included.

Plus: full CPU suite (now 34 tests) + oracle `83886070`; `test_kv_store`, `test_transactions` pass.

---

## 3. Open / deferred
Sorted secondary index (log-time selective ranges); range over catalog columns; predicate-rich key scans.

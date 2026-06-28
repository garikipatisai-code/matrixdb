# Design: Equi-Join Primitive (DM-8)

**Status:** approved-by-standing-directive (continue all phases, don't wait). **Date:** 2026-06-27.
**Builds on:** INT-1 (catalog/`TieredColumn`), DM-2 (`column_id`), DM-3 (`column_type`), the borrow-and-return idiom.
**Fully local.**

**Thesis:** *The engine has no join — multi-column/table correlation is impossible (DM-8). Add a hash
equi-join PRIMITIVE: `hash_join(left_key_id, right_key_id)` returns every `(left_row, right_row)` index
pair whose key values match, by building a hash on the left column's values and probing with the right.
This is the core join operation, fitting the column model (a "table" is a set of equal-length columns
indexed by row); the matching index pairs let a caller correlate any other columns of those tables.
Borrow-and-return across tiers like the scan/group paths. uint32 keys (typed-key join deferred).*

---

## 1. Scope

**IN (`compute_mock.cpp` + new `test_join.cpp`):**
- `std::vector<std::pair<uint64_t, uint64_t>> hash_join(uint64_t left_key_id, uint64_t right_key_id)` —
  both must be `U32` catalog columns. Returns `(left_row, right_row)` for every pair with
  `left_key[left_row] == right_key[right_row]` (an inner equi-join on the key). Build phase: a hash
  `value → [left rows]` over the left column; probe phase: each right row looks up its value and emits a
  pair per matching left row. Records heat + borrows both columns to HOST and returns them (like
  `grouped_aggregate`). Result order unspecified (hash/probe order).

**Invariants:** asserts both ids are catalog columns and both `column_type == U32` (typed-key join is a
later slice — debug assert, the caller controls the columns). Correct under duplicate keys (Cartesian of
matching rows on each side) and across tiers (a COLD column is borrowed). No catalog mutation; oracle
path untouched. The result is the standard join primitive — matching row-index pairs (cardinality =
`.size()`); a higher-level "join + project/aggregate" builds on it.

**OUT (deferred):** typed (int64/double/string) join keys; join-then-project (gather other columns by
the matched rows); non-equi / range / outer joins; multi-key joins; a join planner / cost-based side
selection (always builds on the left — a real planner builds on the smaller side); spill for huge builds.

---

## 2. compute_mock.cpp

```cpp
// Inner hash equi-join on two uint32 key columns: every (left_row, right_row) with left_key[left_row]
// == right_key[right_row]. Build a value->left-rows hash, probe with the right. Borrow-and-return both
// columns (like grouped_aggregate). Result order unspecified; cardinality = result.size(). DM-8.
// ponytail: builds on the LEFT side unconditionally + materializes all pairs in RAM — a planner would
// build on the smaller side, and a huge result would need spilling; both are deferred.
std::vector<std::pair<uint64_t, uint64_t>> hash_join(uint64_t left_key_id, uint64_t right_key_id) {
    assert(catalog_has(left_key_id) && catalog_has(right_key_id) && "hash_join: unknown column id");
    assert(column_type(left_key_id) == MatrixType::U32 && column_type(right_key_id) == MatrixType::U32
           && "hash_join: keys must be uint32 (typed-key join is deferred)");
    TieredColumn& lc = *catalog_.at(left_key_id);
    TieredColumn& rc = *catalog_.at(right_key_id);
    tier_mgr_.record_access(left_key_id, lc.size_bytes());
    tier_mgr_.record_access(right_key_id, rc.size_bytes());
    const MemorySpace lh = lc.tier(); if (lh != MemorySpace::HOST) { ++cold_borrows_; lc.migrate_to(MemorySpace::HOST); }
    const MemorySpace rh = rc.tier(); if (rh != MemorySpace::HOST) { ++cold_borrows_; rc.migrate_to(MemorySpace::HOST); }
    const uint32_t* lk = reinterpret_cast<const uint32_t*>(lc.host_ptr());
    const uint32_t* rk = reinterpret_cast<const uint32_t*>(rc.host_ptr());
    const size_t ln = lc.size_bytes() / sizeof(uint32_t);
    const size_t rn = rc.size_bytes() / sizeof(uint32_t);
    std::unordered_map<uint32_t, std::vector<uint64_t>> build;     // left value -> left rows
    for (size_t i = 0; i < ln; ++i) build[lk[i]].push_back(static_cast<uint64_t>(i));
    std::vector<std::pair<uint64_t, uint64_t>> out;
    for (size_t j = 0; j < rn; ++j) {
        auto it = build.find(rk[j]);
        if (it != build.end()) for (uint64_t i : it->second) out.emplace_back(i, static_cast<uint64_t>(j));
    }
    if (rh != MemorySpace::HOST) rc.migrate_to(rh);                // return borrows
    if (lh != MemorySpace::HOST) lc.migrate_to(lh);
    return out;
}
```
(`<unordered_map>`/`<vector>`/`<utility>` already included.)

---

## 3. Verification (`test_join.cpp`, CPU)

Brute O(n·m) oracle (nested loops) is the reference — the hash join must produce the SAME set of pairs.
- **Match + duplicates**: left `{10,20,30,20}`, right `{20,40,10,20}`. Expected pairs (sorted):
  `(0,2)` [10=10], `(1,0),(1,3)` [left-row1 20 = right 20s], `(3,0),(3,3)` [left-row3 20]. Assert
  `sorted(hash_join) == sorted(brute_oracle)` and the exact set.
- **No matches**: left `{1,2,3}`, right `{4,5,6}` → empty.
- **Self-ish / all-match**: left `{7,7}`, right `{7}` → `(0,0),(1,0)` (Cartesian of the dup).
- **COLD**: drive one join column to COLD (small `host_cap` + a hot workload on a third column, mirroring
  `test_live_tiering`), join, assert results match the brute oracle (borrow path).
- **Non-vacuity**: the brute nested-loop oracle is computed independently; assert equality. Cardinality
  (`.size()`) equals the oracle's count.

Plus: full CPU suite (now 36 tests) + oracle `83886070` via `./run_tests.sh`; `test_live_tiering`,
`test_schema`, `test_query` pass; notebook regenerated.

---

## 4. Open / deferred
Typed/string join keys; join-then-project (gather columns by matched rows); outer/non-equi/range joins;
multi-key joins; build-side selection (smaller side) + a cost planner; spill for huge results.

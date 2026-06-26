# Design: Query Input Validation — VAL-1

**Status:** approved-by-standing-directive, pre-implementation. **Date:** 2026-06-26.
**Builds on:** QRY-1 (`execute_query` / `MatrixQuery`), GBY-1/2, INT-1 (catalog).

**Thesis:** *`execute_query` is the engine's public query boundary, but it `assert`s on bad input
(debug-abort) and the methods it calls use `catalog_.at()` (release-throws `std::out_of_range`) —
so a malformed query crashes the engine. A database must reject bad queries gracefully. Validate
at the boundary and return a defined status; never crash on caller input.*

---

## 1. Scope

**IN:**
- `enum class MatrixQueryStatus { OK, ERR_UNKNOWN_COLUMN, ERR_INVALID_GROUP, ERR_TOO_MANY_GROUPS };`
  (compute.hpp, near `MatrixQuery`).
- `CPUMockEngine::execute_query` returns `MatrixQueryStatus` (was `void`) and **validates up front**,
  clearing `out` and returning the error WITHOUT calling the underlying (asserting) methods on any
  invalid query:
  - `value_col` must be a live catalog column (id > 0 AND present) → else `ERR_UNKNOWN_COLUMN`.
  - if `grouped`: `key_col` live, `key_col != value_col`, key and value columns equal length,
    `num_groups >= 1` → else `ERR_INVALID_GROUP`; and `num_groups <= MAX_QUERY_GROUPS` (a generous
    guard against an absurd/hostile allocation) → else `ERR_TOO_MANY_GROUPS`.
  - valid → dispatch as before, return `OK`.
- A private helper `bool catalog_has(uint64_t id) const { return id != 0 && catalog_.count(id) != 0; }`.
- Return value is **not** `[[nodiscard]]` (existing call sites — the live demo, QRY-1 tests — ignore
  it and must keep compiling); callers SHOULD check it (documented).
- `test_query_validation.cpp` — every error path returns the right status + empty `out` + **no crash**,
  built and run BOTH normally AND with `-DNDEBUG` (proves release-safety: no assert/throw/abort).

**OUT (deferred):** validating the admin/persistence entry points (`save_column`/`load_*` still use
`.at()` fail-fast on a bad id — operator ops, hardened later); a richer error type with messages;
exceptions; per-error metrics; GPU.

---

## 2. Behavior

```cpp
MatrixQueryStatus execute_query(const MatrixQuery& q, std::vector<uint64_t>& out) {
    out.clear();
    if (!catalog_has(q.value_col)) return MatrixQueryStatus::ERR_UNKNOWN_COLUMN;
    if (q.grouped) {
        if (!catalog_has(q.key_col) || q.key_col == q.value_col || q.num_groups == 0
            || catalog_.at(q.key_col)->size_bytes() != catalog_.at(q.value_col)->size_bytes())
            return MatrixQueryStatus::ERR_INVALID_GROUP;
        if (q.num_groups > MAX_QUERY_GROUPS) return MatrixQueryStatus::ERR_TOO_MANY_GROUPS;
        if (q.has_filter) grouped_aggregate_where(q.key_col, q.value_col, q.num_groups, q.agg, q.threshold, out);
        else              grouped_aggregate(q.key_col, q.value_col, q.num_groups, q.agg, out);
    } else {
        out.assign(1, scan_tiered_column(q.value_col, q.threshold, q.agg, q.has_filter));
    }
    return MatrixQueryStatus::OK;
}
```

- `MAX_QUERY_GROUPS = 1u << 28` (256M groups ≈ 2 GB of u64 accumulators) — a generous ceiling that
  rejects only absurd/hostile values, never a realistic cardinality. A static constexpr member.
- After validation, the underlying methods' own asserts (`key!=value`, equal length, `value_col` in
  catalog) are guaranteed satisfied — so they never fire on a query that passed validation. The
  asserts stay as internal-invariant documentation; the validation is the external guarantee.
- The previous `assert(value_col != 0)` / `assert(key_col != 0)` in `execute_query` are replaced by
  the graceful checks (no behavior change for valid queries; bad queries now return instead of abort).

---

## 3. Verification (`test_query_validation.cpp`, CPU)

Load one column (id 5) into an engine; build a second engine/column of a different length for the
length-mismatch case. Assert each returns the right status, leaves `out` empty, and does **not** crash:
- valid scalar query → `OK`, `out.size()==1`, correct value.
- `value_col = 0` → `ERR_UNKNOWN_COLUMN`, `out` empty.
- `value_col = 999` (never loaded) → `ERR_UNKNOWN_COLUMN`.
- grouped, `key_col = value_col` (5,5) → `ERR_INVALID_GROUP`.
- grouped, `key_col = 999` (unknown) → `ERR_INVALID_GROUP`.
- grouped, `num_groups = 0` → `ERR_INVALID_GROUP`.
- grouped, key & value of different lengths → `ERR_INVALID_GROUP`.
- grouped, `num_groups = (1u<<28)+1` → `ERR_TOO_MANY_GROUPS`.
- a valid grouped query → `OK` + correct per-group result (regression: validation didn't break the
  happy path).
- **Release-safety:** the test is ALSO built with `-DNDEBUG` and run — it must pass identically (no
  assert/`.at()`-throw/abort on any error path; the validation returns before reaching them).

Plus: oracle `83886070` unchanged; all 14 existing tests green (they ignore the new return value);
notebook regenerated.

---

## 4. Open / deferred
- Validate `save_column`/`load_column_from_file`/`save_catalog`/`load_catalog` ids gracefully
  (currently `.at()` fail-fast); a string-message error type; per-error-code metrics in `EngineStats`;
  exceptions vs status (kept status — matches the codebase's non-throwing style); GPU.

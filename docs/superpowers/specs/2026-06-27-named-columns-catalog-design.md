# Design: Named Columns + Catalog Introspection (DM-2)

**Status:** approved-by-standing-directive (continue all phases, don't wait). **Date:** 2026-06-27.
**Builds on:** INT-1 (catalog), DM-3 (`column_type`/`column_rows`), OB-1 (`column_tier`).
**Fully local.**

**Thesis:** *The catalog is addressed only by numeric id, and there's no way to discover what's in the
engine — no names, no "SHOW COLUMNS". Add optional column names (`name_column` / `column_id` /
`column_name`) and a `catalog_columns()` introspection listing each column's id, name, type, row count,
and tier. This is the schema/catalog gap (DM-2): a client/tool/operator can now reference columns by
name and enumerate the store. Thin and additive — it assembles existing accessors plus a name↔id map;
naming is optional (unnamed columns keep working).*

---

## 1. Scope

**IN (`compute_mock.cpp` + new `test_schema.cpp`):**
- Members: `std::unordered_map<uint64_t, std::string> column_names_;` (id → name) and
  `std::unordered_map<std::string, uint64_t> name_to_id_;` (name → id, O(1) resolve).
- `void name_column(uint64_t id, const std::string& name)` — attach/overwrite a name for an existing
  catalog column (`assert(catalog_has(id))`); updates both maps. (Duplicate names: last-writer-wins for
  `column_id`; documented — a schema layer would enforce uniqueness, out of scope.)
- `uint64_t column_id(const std::string& name) const` — resolve name → id; returns `0` if absent (0 is
  the reserved legacy id, a safe "not found" sentinel).
- `std::string column_name(uint64_t id) const` — reverse; `""` if the column is unnamed.
- `struct ColumnInfo { uint64_t id; std::string name; MatrixType type; size_t rows; MemorySpace tier; };`
- `std::vector<ColumnInfo> catalog_columns() const` — one `ColumnInfo` per catalog column, assembled
  from `column_name(id)` / `column_type(id)` / `column_rows(id)` / `column_tier(id)`. (Order is
  unspecified — `unordered_map` iteration; callers sort if they need a stable order.)

**Invariants:** purely additive — `load_scan_column*` and queries are unchanged; naming is optional.
`catalog_columns()` is read-only (const). The id-0 legacy column is not a catalog entry (so not listed).
Oracle untouched.

**OUT:** named TABLES (grouping columns into tables) — columns are the catalog unit here; unique-name
enforcement; querying MatrixQuery directly by name (callers resolve name→id then use the existing API);
renaming side effects (drop is not supported — columns are load-once).

---

## 2. compute_mock.cpp additions

Near the catalog members (beside `col_types_`):
```cpp
    std::unordered_map<uint64_t, std::string> column_names_;   // id -> optional name
    std::unordered_map<std::string, uint64_t> name_to_id_;     // name -> id (resolve)
```

`ColumnInfo` near `struct EngineStats`:
```cpp
// One catalog column's metadata (DM-2 introspection): id, optional name (""), element type, row count, tier.
struct ColumnInfo { uint64_t id; std::string name; MatrixType type; size_t rows; MemorySpace tier; };
```

Public methods (near `column_type`/`column_tier`):
```cpp
// Attach (or overwrite) a name for an existing catalog column. Duplicate names: last wins for column_id.
void name_column(uint64_t id, const std::string& name) {
    assert(catalog_has(id) && "name_column: unknown column id");
    column_names_[id] = name;
    name_to_id_[name] = id;
}
// Resolve a column name to its id; 0 (the reserved legacy id) if no such name.
uint64_t column_id(const std::string& name) const {
    auto it = name_to_id_.find(name);
    return it == name_to_id_.end() ? 0 : it->second;
}
// A column's name, or "" if unnamed.
std::string column_name(uint64_t id) const {
    auto it = column_names_.find(id);
    return it == column_names_.end() ? std::string{} : it->second;
}
// List every catalog column with its metadata (id, name, type, row count, tier). Order unspecified.
std::vector<ColumnInfo> catalog_columns() const {
    std::vector<ColumnInfo> out;
    out.reserve(catalog_.size());
    for (const auto& kv : catalog_)
        out.push_back(ColumnInfo{ kv.first, column_name(kv.first), column_type(kv.first),
                                  column_rows(kv.first), kv.second->tier() });
    return out;
}
```
(`column_rows` and `column_type` are `const` already; `catalog_`/`kv.second->tier()` are accessible. `<string>`/`<vector>`/`<unordered_map>` already included.)

---

## 3. Verification (`test_schema.cpp`, CPU)

- Register a u32 column (id 3), an int64 column (id 7), a double column (id 9). `name_column` them
  (`"qty"`, `"revenue"`, `"rate"`). Assert:
  - `column_id("qty") == 3`, `column_id("revenue") == 7`, `column_id("rate") == 9`;
  - `column_id("nonexistent") == 0`; `column_name(3) == "qty"`; an unnamed column's name is `""`.
- `catalog_columns()`: size 3; sort by id; assert each entry's `{id, name, type, rows}` is correct
  (u32/I64/F64, the right row counts), and `tier == HOST` for freshly-loaded columns.
- **Resolve-then-query**: `execute_query` on `column_id("revenue")` (int64 SUM) returns the int64 oracle —
  proving name resolution feeds the existing query path.
- **Non-vacuity**: an unnamed column appears in `catalog_columns()` with `name == ""` but is still listed
  (introspection covers unnamed columns); `column_id` of a never-registered name is 0, not a stale id.

Plus: full CPU suite (now 32 tests) + oracle `83886070`; `test_observability`, `test_typed_*`,
`test_query` pass unmodified; notebook regenerated.

---

## 4. Open / deferred
Named tables (column grouping); unique-name enforcement; query-by-name in `MatrixQuery` directly; column
drop/rename; a richer system-catalog (created-at, stats).

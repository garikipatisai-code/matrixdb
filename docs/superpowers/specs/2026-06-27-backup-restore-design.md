# Design: Backup / Restore (DU-6)

**Status:** approved-by-standing-directive (continue all phases, don't wait). **Date:** 2026-06-27.
**Builds on:** CKPT-1 (`save_catalog`/`load_catalog`), DU-4 (`save_checkpoint`/`load_checkpoint`).
**Fully local.**

**Thesis:** *MatrixDB can snapshot the analytical catalog (CKPT-1) and checkpoint the point-op store
(DU-4) separately, but there's no single "back up everything / restore everything" operation — a basic
ops capability. Add `backup(prefix)` (writes both the catalog snapshot and the point-op checkpoint under
one path prefix) and `restore(prefix)` (loads both into a fresh engine). Thin — it composes the existing
durable-state writers/readers; `save_checkpoint` already captures the in-memory `kv_` (which reflects all
committed writes), so backup works whether or not a WAL is attached.*

---

## 1. Scope

**IN (`compute_mock.cpp` + new `test_backup.cpp`):**
- `void backup(const std::string& prefix)` — `save_catalog(prefix + ".catalog")` (the tiered analytical
  store, all column types) + `save_checkpoint(prefix + ".kv")` (the point-op `kv_`). Two files at the
  prefix; no directory creation needed.
- `void restore(const std::string& prefix)` — `load_catalog(prefix + ".catalog")` +
  `load_checkpoint(prefix + ".kv")`. Restores both into THIS engine. Intended for a **fresh** engine
  (loading a column id that already exists asserts — re-registration; documented).

**Invariants:**
- Reuses the existing fail-loud writers/readers verbatim (no format changes) — so a backup is just
  `{<prefix>.catalog, <prefix>.kv}`, each independently valid.
- `backup` works with durability off (no WAL): `save_checkpoint` snapshots the in-memory `kv_` directly
  (it doesn't require `cold_store_`); committed point-op writes are already applied to `kv_`.
- `restore`'s `load_checkpoint` returns `false` (not abort) if `<prefix>.kv` is missing — a catalog-only
  backup still restores the analytical store. `load_catalog` aborts on a missing/corrupt `.catalog`
  (our format, fail-loud).
- Oracle untouched (backup/restore are new methods, off the id-0 path).

**OUT:** incremental/differential backup; compression; the WAL tail (backup snapshots `kv_` at backup
time — equivalent to a checkpoint, so post-backup writes aren't in the backup, which is the expected
point-in-time semantics); remote/object-store targets; restore into a non-empty engine.

---

## 2. compute_mock.cpp additions (beside `save_catalog`/`load_catalog`)

```cpp
// Back up the whole durable state under one path prefix: <prefix>.catalog (tiered analytical columns,
// all types) + <prefix>.kv (the point-op store). A point-in-time snapshot; reuses the existing fail-loud
// writers. Works with or without an attached WAL (save_checkpoint snapshots the in-memory kv_).
void backup(const std::string& prefix) {
    save_catalog(prefix + ".catalog");
    save_checkpoint(prefix + ".kv");
}

// Restore a backup written by backup() into THIS engine (intended for a fresh engine — loading an
// already-registered column id asserts). Catalog-only backups restore the analytical store; a missing
// <prefix>.kv leaves the point-op store empty (load_checkpoint returns false).
void restore(const std::string& prefix) {
    load_catalog(prefix + ".catalog");
    load_checkpoint(prefix + ".kv");
}
```

---

## 3. Verification (`test_backup.cpp`, CPU)

- **Full round-trip**: an engine with a u32 column (id 3), an int64 column (id 7, negatives + >UINT32_MAX),
  and committed point-op writes (`begin`/`txn_put`/`commit` for several keys). `backup("/tmp/mdb_bk")`.
  A FRESH engine → `restore("/tmp/mdb_bk")`. Assert: both columns restored with correct type + values
  (query SUM each, int64 via `static_cast<int64_t>`); `kv_get` returns the committed point-op values.
- **No-WAL backup works**: the source engine has no WAL (durability off) — `backup` still captures `kv_`
  (the committed writes are in memory), and `restore` brings them back. (Non-vacuity: the restored kv
  values can only come from `<prefix>.kv`.)
- **Empty engine** round-trips (empty catalog + empty kv → no crash).

Plus: full CPU suite (now 31 tests) + oracle `83886070`; `test_catalog_snapshot`, `test_checkpoint`,
`test_typed_snapshot` (the underlying writers/readers) pass unmodified; notebook regenerated.

---

## 4. Open / deferred
Incremental/differential backup; compression; remote targets; restore-into-populated-engine (needs a
catalog clear); backing up the live WAL tail (point-in-time semantics make this unnecessary).

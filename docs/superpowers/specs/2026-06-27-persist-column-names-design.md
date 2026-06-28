# Design: Persist Column Names in the Catalog Snapshot (DM-2b)

**Status:** approved-by-standing-directive (continue all phases, don't wait). **Date:** 2026-06-27.
**Builds on:** DM-2 (`name_column`/`column_name`/`column_id`), DM-3d (typed catalog snapshot), DU-6 (backup).
**Fully local.** Closes a finding the QA-2 integration test surfaced (names were RAM-only — lost on backup/restore).

**Thesis:** *DM-2 added column names, but `save_catalog`/`load_catalog` (and therefore `backup`/`restore`)
didn't carry them — a restored engine lost all names (`column_id("revenue") == 0`). A complete backup
must preserve names. Add the name to each column's snapshot record and restore it on load.*

---

## 1. Change

- Catalog-snapshot per-column record gains a name: `[u64 id][u32 type][u64 count][u32 namelen][name bytes][data]`
  (was `[id][type][count][data]`). `save_catalog` writes `column_name(id)` (empty for unnamed columns);
  `load_catalog` reads it (guarded `namelen ≤ 4096` — corrupt-snapshot bound, fail-loud) and calls
  `name_column(id, name)` after the column is loaded (skipped if `namelen == 0`).
- Magic bumped `0x4D434131` ('MCA1', v1) → `0x4D434132` ('MCA2', v2) so a v1 snapshot is rejected
  fail-loud rather than misparsed.

**Invariants:** unnamed columns (`namelen == 0`) round-trip exactly as before (so `test_catalog_snapshot`,
`test_typed_snapshot`, `test_backup` — which don't name columns — pass unchanged). `backup`/`restore`
use `save_catalog`/`load_catalog`, so names now survive a full backup. Oracle untouched.

**OUT:** unique-name enforcement; named tables.

---

## 2. Verification

The QA-2 integration test (`test_integration.cpp`) now asserts the FIXED behavior: after
`backup → fresh engine → restore`, `column_id("revenue") == 7` and `column_id("region") == 2`, and the
revenue SUM is queried via the **restored name** (`fresh.column_id("revenue")`). 37-test suite + oracle
green (the catalog/backup/typed-snapshot tests round-trip through the v2 format).

---

## 3. Open / deferred
Unique-name enforcement; named tables; a system catalog with more metadata.

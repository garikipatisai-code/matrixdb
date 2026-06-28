# Design: End-to-End Integration Test (QA-2)

**Status:** approved-by-standing-directive (continue all phases, don't wait). **Date:** 2026-06-27.
**Builds on:** ~the whole session — DM-2/3/4/5b/7/8/9, DU-6. **Test-only increment** (no engine code).

**Thesis:** *The suite has 36 UNIT tests but nothing exercises the features COMPOSING end-to-end — QA-2's
"no integration coverage". Add `test_integration.cpp`: one realistic flow that wires the major
capabilities together and asserts at each stage, catching cross-feature integration bugs unit tests
miss. It also documents one genuine cross-feature finding (column names are RAM-only — not carried by
backup/restore).*

---

## 1. The flow (one test, asserted at each stage)

1. **Typed CSV ingest + naming (DM-5b/3g/2):** write a `region,revenue` CSV; `load_column_from_csv` the
   u32 `region` (col 0) and `load_column_from_csv_i64` the int64 `revenue` (col 1, header skipped);
   `name_column` both. Assert the loads succeed and types are right.
2. **Catalog introspection (DM-2):** `catalog_columns()` lists both with correct id/name/type/rows/tier.
3. **Parse + execute (DM-4/QRY):** `parse_query("SELECT SUM(revenue) WHERE revenue > 1000000000")` →
   `execute_query` → assert == the hand oracle (sum of revenue rows > 1e9).
4. **Append + re-query (DM-9):** `append_to_column_i64(revenue, {4000000000})`; re-run the same parsed
   query → assert the sum grew by 4e9 (the appended row passes the filter).
5. **Equi-join (DM-8):** load a u32 `valid_region` lookup column; `hash_join(region, valid_region)` →
   assert the matching `(region_row, lookup_row)` pairs vs a brute oracle.
6. **Backup → restore (DU-6):** `backup(prefix)`; a FRESH engine `restore(prefix)`; re-query `revenue`
   **by id** (SUM all) → assert it survived WITH the appended row (== 7e9 + 4e9 = 11e9), and
   `column_type` is restored. **Finding (documented in-test):** `column_id("revenue")` is `0` in the
   restored engine — DM-2 names are RAM-only, not carried by the catalog snapshot. The test queries by
   id and notes this; persisting names in the snapshot is a deferred enhancement.

**Verification:** every stage asserts against an in-test oracle; the test is added to the suite (auto-
discovered by `run_tests.sh`). It's non-vacuous (each stage's assert fails if that feature regresses or
fails to compose with the prior stage's state).

---

## 2. Open / deferred
Persist column names in the catalog snapshot (so backup/restore round-trips names — the finding above);
a tiering-under-pressure stage; concurrency-mixed integration (gated).

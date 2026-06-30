# matrixdb SQL joins in the REPL (Design)

**Date:** 2026-06-30  **Status:** design approved (standing directive); ready for plan.
**Goal:** Expose the engine's `hash_join` + `gather` primitives as a SQL form in the REPL — combine two
row-aligned datasets on an equi-key and see the joined, decoded rows. The one analytical capability the
engine has at the primitive level but never at the query level. CPU, fully local + verifiable.

## Why

`compute_mock.cpp` has `hash_join(lkey, rkey) -> vector<(lrow, rrow)>` (inner equi-join, Cartesian on
duplicate keys), `hash_join_i64` (typed sibling), `hash_join_count` (cardinality, resource-safe), and
`gather(col, rows) -> values`. v1/v2 never wired them. A join form turns "two independent columns" into
"matched rows across two datasets" — the missing piece for real analytics.

## The catalog is flat — what a "join" means here

There are no SQL tables; the catalog is columns by id/name. A dataset is a set of **row-aligned** columns
(same length, row *i* is the same record): e.g. `orders` = `{ord_region, ord_amount}`, `regions` =
`{reg_key, reg_name}`. A join relates two **key columns** and projects one column **from each side**:

```
SELECT <lcol>, <rcol> JOIN <lkey> = <rkey> [LIMIT n]
```

- `hash_join(lkey, rkey)` → matched `(lrow, rrow)` pairs.
- `<lcol>` is gathered at the **left** rows, `<rcol>` at the **right** rows — **positional rule: the column
  before the comma is left (aligned with `lkey`), the column after is right (aligned with `rkey`).** This is
  the only unambiguous rule in a flat catalog; documented in `.help` and the README.
- Output: one decoded row per matched pair, `lval │ rval`, capped at `LIMIT n` (or 100 default, like
  projection) with a `… (<total> matches, showing N)` note when longer.

Plus the cardinality form:

```
SELECT COUNT(*) JOIN <lkey> = <rkey>      ->  hash_join_count(lkey, rkey)   (one integer)
```

## Key-type rules (correctness)

- `hash_join` asserts **u32** keys; `hash_join_i64` for **i64**. Both keys must be the **same** type.
  - `column_type(lkey) == column_type(rkey)`; u32 → `hash_join`, i64 → `hash_join_i64`, else (f64/mismatch)
    → `Error: join keys must be matching u32 or i64 columns`.
- **Reject dictionary-string keys.** Two independently-encoded dict columns have **independent code spaces**
  — the same string gets different codes, so joining their codes is silently wrong. If
  `string_dict_size(lkey) > 0 || string_dict_size(rkey) > 0` → `Error: cannot join on string-dictionary keys
  (independent code spaces)`. (A shared-dictionary string join is a future item.)
- Unknown column names (`column_id == 0`) → `Error: unknown column in join`.

## Router placement

A new branch in `matrix_cli_run_sql`, checked **first** (a join line is unambiguous — it contains the
`JOIN` keyword, which no other form uses):

```
if (U.find(" JOIN ") != npos) { ... join path ... return; }
```

(Leading space avoids matching a column literally named `join…`; the keyword is space-delimited.)

## Parsing (tokenize; no grammar changes to the engine)

Tokenize on whitespace (reuse `split_ws`); also split a glued `lkey=rkey` is **not** supported — require
spaces around `=` (documented). Expected token shapes:
- `SELECT <lcol> , <rcol> JOIN <lkey> = <rkey> [LIMIT n]`
- `SELECT COUNT ( * ) JOIN <lkey> = <rkey>`  (detect `COUNT(*)` before the comma split)

Find the `JOIN` token; the SELECT list is the tokens between `SELECT` and `JOIN`; after `JOIN` expect
`<lkey> = <rkey> [LIMIT n]`. Any shape mismatch → `Error: could not parse join (SELECT a, b JOIN lk = rk
[LIMIT n])`. **Never crashes** — every branch validates before calling the engine.

## Execution & output

1. Resolve `lkey`/`rkey` ids; apply the key-type rules above.
2. `COUNT(*)` form → print `hash_join_count(lkey, rkey)`.
3. Projection form: resolve `lcol`/`rcol` ids (unknown → Error). `pairs = hash_join* (lkey, rkey)`; cap to
   the limit. `gather(lcol, lrows)` + `gather(rcol, rrows)` (two batched calls). Print
   `decode_proj(lcol, lv[i]) │ decode_proj(rcol, rv[i])` per row (reuses v1's `decode_proj`, so dict-string
   *value* columns still decode to text — only *key* columns are restricted).

## Testing (extend `test_cli.cpp`)

Two row-aligned datasets in one engine (load via `.load` or directly): left `ord_region = {0,1,0,2}` (u32),
`ord_amt = {10,900,20,950}`; right `reg_key = {0,1,2}`, `reg_name` (dict str `{north,south,east}` or u32
labels). Over string streams:
- `SELECT ord_amt, reg_name JOIN ord_region = reg_key` → 4 matched rows pairing each order's amount with its
  region's name (e.g. `10 │ north`, `900 │ south`, `20 │ north`, `950 │ east`); assert the decoded pairs and
  the row count.
- `SELECT COUNT(*) JOIN ord_region = reg_key` → `4`.
- `... JOIN ord_region = <f64 col>` → `Error:`; join on a dict-string key → `Error:`; unknown column →
  `Error:`; malformed (`SELECT a JOIN`) → `Error:`. Session continues after each.
ASan/UBSan-clean; `run_tests.sh` stays green.

## Scope & non-goals (v1)

**In:** inner equi-join projection (one column per side, positional), `COUNT(*)` cardinality, u32 + i64
keys, decoded output, full error handling.
**Deferred (named):** aggregates over a join (`SELECT SUM(lcol) JOIN …` — wants a join→column materialization
or a semi-join filter); more than one column per side; left/right/outer joins; multi-key joins; string-key
joins (needs a shared dictionary); a `WHERE` on the join result; `f64` keys.

## Success criteria

The REPL can join two datasets on a u32/i64 key and print decoded `left │ right` rows + a `COUNT(*)`
cardinality; bad keys/types/columns/syntax produce friendly errors, never a crash or a silently-wrong
string-key join. `test_cli.cpp` green under ASan/UBSan; full suite + oracle green.

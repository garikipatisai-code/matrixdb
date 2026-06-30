# matrixdb CLI v2 — persistence + full query surface (Design)

**Date:** 2026-06-30  **Status:** design approved (standing directive); ready for plan.
**Goal:** Close the gap between what the engine can do and what the REPL can type. v1 exposed a subset; v2
adds session persistence (`.save`/`.open`) and routes the analytical forms the engine already supports but
the shell didn't reach: multi-aggregate `SELECT a, b`, top-N (`ORDER BY agg DESC LIMIT`), `HAVING`, and
`COUNT(DISTINCT)`. All executors exist; this is router + formatter + two dot-commands. CPU, fully local.

## Why

`compute_mock.cpp` has public string-form helpers the v1 router never called: `query_multi`, `top_query`,
`having_query`, `distinct_query(sql, &out)`, and the durable `save_catalog`/`load_catalog` (which persist the
string dictionaries too). Wiring them makes the CLI expose the engine's real power and turns it from a
scratch calculator into a tool you can build a dataset in and reopen later.

## Persistence — `.save` / `.open`

- `.save <file>` → `save_catalog(file)`; `.open <file>` → `load_catalog(file)` (restores every column incl.
  names + string dictionaries).
- **Footgun:** both `abort()` on `fopen` failure — unacceptable in a REPL. Guard with a pre-check:
  - `.open`: if `!std::ifstream(file).good()` → `Error: cannot open <file>` and return (don't call the engine).
  - `.save`: if `!std::ofstream(file).good()` → `Error: cannot write <file>` and return.
  - (The pre-open for `.save` creates/truncates the target; `save_catalog` then writes it — harmless.)
  - A *corrupt* snapshot passed to `.open` still aborts inside `load_catalog` (CRC/short-read). Pre-existing
    engine behavior; out of scope — note with a `// ponytail:` at the call site.
- After `.open`, the CLI's `next_id` counter must not collide with restored ids. Set
  `next_id = max(existing column id) + 1` after a successful `.open` (scan `catalog_columns()`).

## Query surface — router additions

The router (`matrix_cli_run_sql`) gains four branches. Detection is by cheap uppercased-substring tests; the
new branches are checked **before** the existing single-aggregate `(` branch. Final order:

1. `HAVING` present → **HAVING path**: split the line at `HAVING` → head; `parse_query(head)`→`q` (for
   decode; `Error` if it fails); `pairs = having_query(line)`; print each `label │ value` (decoded). Empty
   pairs → print nothing (no group passed — valid).
2. `DISTINCT` present → **distinct path**: `distinct_query(line, out)`; true → print `out` (integer);
   false → `Error: could not run COUNT(DISTINCT) query`.
3. `AVG(` present → existing AVG path (unchanged).
4. comma in the SELECT list **and** `(` present → **multi-aggregate path** (see below).
5. `(` present → existing single-aggregate path, **extended**: after `parse_query`→`q`+`execute_query`, if
   `q.limit > 0` (top-N: `ORDER BY agg DESC LIMIT n`) → instead format `top_query(line)`'s `(group,value)`
   pairs (already sorted desc), decoded via `q`. Otherwise the existing scalar/grouped formatting.
6. else → existing projection path (unchanged).

### Multi-aggregate path

A multi-agg line is N independent single-aggregate queries sharing a tail. Rather than call `query_multi`
(which returns values but not the per-term parsed query needed to decode by type), the router runs the same
split itself so it keeps each term's `MatrixQuery` for decoding and labeling:

- Split the SELECT list (between `SELECT` and the first `WHERE`/`GROUP`/`ORDER`) on commas → terms; the tail
  is the shared remainder. For each term: `parse_query("SELECT " + term + tail)` → `q_i`; `execute_query` →
  `r_i`. Any failure → `Error: could not run multi-aggregate query`.
- **Scalar** (every `r_i` size 1): one line `term0 │ term1 │ …` (the labels) then `v0 │ v1 │ …` (decoded via
  each `q_i` with `decode_agg`). 
- **Grouped** (all `q_i.grouped`, shared `key_col`, equal sizes): header `key │ term0 │ term1 │ …`; then per
  group `g`: `label(g) │ decode_agg(q_0, r_0[g]) │ …` where `label(g)` decodes the string key if
  `string_dict_size(key_col) > 0`.
- `AVG` in a multi-agg list is unsupported (parser-side, like `query_multi`); such a term fails parse →
  `Error`. Documented, not silently wrong.

## Output formatting

Reuses v1's `decode_agg` (COUNT→integer; SUM/MIN/MAX→by value type) and the grouped-key string decode. Top-N
and HAVING reuse the grouped row format `label │ value`. Multi-agg uses ` │ `-separated columns with a label
header row. No raw `uint64_t` ever printed.

## Testing (extend `test_cli.cpp`)

Same `/tmp` CSV (`amount` u32 + `region` str). New asserts, all over string streams:
- `.save /tmp/mdb_cli.db` then a fresh engine `.open /tmp/mdb_cli.db` → `.columns` shows `amount`+`region`
  with the right rows; a query over the reopened engine returns the same answer (round-trip).
- `.open /tmp/does_not_exist.db` → `Error:` (no abort, session continues).
- `SELECT COUNT(amount), SUM(amount)` → both values on a labeled row (4 and 1880).
- `SELECT SUM(amount) GROUP BY region ORDER BY SUM DESC LIMIT 2` → top 2 groups by value desc
  (`games 900`, `music 950` → desc: `music 950`, `games 900`), decoded labels.
- `SELECT SUM(amount) GROUP BY region HAVING SUM > 100` → only `games`/`music` (books=30 excluded).
- `SELECT COUNT(DISTINCT region)` → `3`.
ASan/UBSan-clean; `run_tests.sh` stays green.

## Scope & non-goals (v2)

**In:** `.save`/`.open` (guarded), multi-agg (scalar+grouped), top-N, HAVING, COUNT(DISTINCT).
**Deferred:** `.save`/`.open` of WAL/durability state (catalog snapshot only); AVG inside multi-agg; HAVING
with BETWEEN (single comparison only — engine limit); readline/history; joins (Sub-project B).

## Success criteria

The REPL can express every analytical form the engine supports (scalar/grouped/filtered/cross-column +
multi-agg + top-N + HAVING + DISTINCT + projection), persist a catalog and reopen it, and never aborts or
crashes on bad input. `test_cli.cpp` green under ASan/UBSan; full suite + oracle green.

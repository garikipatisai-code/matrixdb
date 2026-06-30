# matrixdb output modes — `.mode list|csv` (Design + Plan)

**Date:** 2026-06-30  **Status:** approved (standing directive). Small feature → combined design+plan.
**Goal:** Machine-readable output so REPL results pipe into other tools — `matrixdb -c "SELECT …" | ...`.
The canonical DB-CLI affordance (sqlite's `.mode`). CPU, local.

## Why

Every result prints with a ` │ ` field separator — readable, but not parseable. `.mode csv` swaps the
separator to `,` (and drops the decorative `… (N rows)` trailer) so output is consumable downstream. Default
stays `list` (today's exact output), so it's purely additive.

## Design

- A per-session `OutMode { LIST, CSV }` in `matrix_repl` (default `LIST`); `.mode list|csv` sets it,
  `.mode` with no/!arg → `Error: usage: .mode list|csv`.
- `matrix_cli_run_sql` gains a trailing `OutMode mode = OutMode::LIST` param (only caller is `matrix_repl`).
  Inside: `const std::string sep = (mode == OutMode::CSV) ? "," : " │ ";` — every ` │ ` field/header join
  uses `sep`. The decorative trailer notes (`… (<n> rows, showing <cap>)` / `… (<n> matches …)`) print only
  in `LIST` mode (they'd corrupt CSV).
- Single-value rows (scalar agg, COUNT, AVG scalar, distinct) are one value per line in both modes — already
  separator-free, unchanged.
- LIST mode output is **byte-identical to today** → the existing 9 `test_cli` groups stay green untouched.

## Scope & non-goals

**In:** `.mode list|csv` separator swap + trailer-note suppression in CSV; `.help` + `.mode` reflect it.
**Deferred (named):** aligned `.mode table` (needs buffering rows to compute column widths — a bigger change);
RFC-4180 quoting of values containing `,`/`"`/newlines (v1 assumes simple values — note it); a header row in
CSV (`.headers on`). These are real but separable.

## Implementation outline

- [ ] `matrix_cli.hpp`: add `enum class OutMode { LIST, CSV };` in `matrixcli_detail`.
- [ ] `matrix_cli_run_sql(..., OutMode mode = OutMode::LIST)`: `const std::string sep = mode==OutMode::CSV ? "," : " │ ";`
      replace every ` │ ` emit with `sep`; gate the two `… (…)` trailer lines on `mode == OutMode::LIST`.
- [ ] `matrix_repl`: `OutMode mode = OutMode::LIST;`; pass `mode` at both `matrix_cli_run_sql` call sites
      (timed + untimed); add `.mode` dot-command (`list`/`csv`, else usage error) → `out << "mode " << ...`.
- [ ] `.help`: add `.mode list|csv` to the commands line.
- [ ] `test_cli.cpp`: new block — `.mode csv` then `SELECT SUM(amount) GROUP BY region` → assert `has("books,30")`
      and `!has("books │ 30")`; then `.mode list` + same query → `has("books │ 30")`. (Reuses `eng`.)
- [ ] `examples/tour.sql`: (optional) leave as-is (list mode reads best for a human tour).
- [ ] Verify: `test_cli` under ASan/UBSan; `./run_tests.sh` + `SAN=1` green; README CLI commands list + memory.
- [ ] Commits: `feat(cli): .mode list|csv output separator` then `docs(mode): README + notebook + .help`.

## Success criteria

`matrixdb -c "SELECT SUM(amount) GROUP BY region"` under `.mode csv` emits comma-separated rows pipeable to
another tool; `list` is unchanged and the suite stays green under ASan/UBSan.

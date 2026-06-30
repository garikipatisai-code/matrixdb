# matrixdb Harden & Polish for release (Design)

**Date:** 2026-06-30  **Status:** design approved (standing directive); ready for plan. **Final sub-project**
of the CLI program (after v1, v2, joins).
**Goal:** Make what exists *feel finished* rather than wider — a one-command end-to-end demo a newcomer can
run, per-query timing in the REPL, and a docs pass so README/FINDINGS match the shipped tool. No new query
capability. CPU, local.

## Why

The engine + CLI are feature-complete for the analytical subset, but a newcomer still has to assemble their
own CSV and guess the grammar. A shipped demo (`matrixdb -f examples/tour.sql`) that loads real data and runs
every form turns "powerful internals" into "I ran it and saw it work." A `.timing` toggle is the one missing
table-stakes REPL affordance (sqlite's `.timer`). FINDINGS/README should record the CLI arc.

## 1. End-to-end demo (`examples/`)

A tiny, realistic two-dataset example that exercises the whole surface and doubles as a smoke test:
- `examples/orders.csv` — header `region,amount,qty` (u32 region-index, u32 amount, u32 qty); ~12 rows
  across 3 regions so GROUP BY / HAVING / top-N are non-trivial.
- `examples/regions.csv` — header `key,name` (u32 key 0..2, string name) for the join.
- `examples/tour.sql` — a commented REPL script run with `./matrixdb -f examples/tour.sql` (from the repo
  root, so the relative `.load examples/orders.csv …` paths resolve). It walks: `.load` (typed + str),
  `.columns`, scalar + grouped aggregates, `WHERE`, multi-aggregate, `HAVING`, top-N, `COUNT(DISTINCT)`,
  a join, `.save`/`.open`, `.stats`. Lines starting with `#` are comments (the REPL must skip them — see
  §change). Ends with `.quit`.

**One REPL change for scripts:** comment lines. `matrix_repl` currently treats a non-dot, non-empty line as
SQL. Add: a line whose first non-space char is `#` is skipped (so `tour.sql` can be self-documenting). One
line in the loop, next to the empty-line skip.

## 2. `.timing on|off` (per-query elapsed)

- New dot-command `.timing on` / `.timing off` (default off). A `bool timing` local in `matrix_repl`.
- When on, after running a **SQL** line (not dot-commands), print `(<n> µs)\n` measured with
  `std::chrono::steady_clock` around the `matrix_cli_run_sql` call. (Dot-commands aren't timed — they're not
  queries.)
- `std::chrono` is fine here (this is the CLI, not a workflow script); no engine change. Reuses nothing from
  the engine's latency histogram (that measures `execute_query` internals; this measures end-to-end REPL
  query wall-time, which is what a user wants to see).

## 3. Docs pass

- **README:** add a short "Try the demo" line under the CLI section: `./matrixdb -f examples/tour.sql`.
  Verify the command/query/join lists match the shipped `.help` (they were updated per sub-project; this is a
  final consistency check, fix any drift).
- **FINDINGS.md:** one concise journal entry for the CLI arc (v1 shell-over-streams, v2 full surface +
  persistence, joins, polish) — what was built and the one non-obvious decision per piece (testable
  `matrix_repl` over streams; COUNT-as-integer decode; abort-guarded `.save`/`.open`; dict-string-key join
  rejection). Matches the existing FINDINGS voice (decision + why).
- **Notebook:** the demo cell already builds `matrixdb` and pipes a session; extend its piped script to hit a
  multi-agg + join line so the notebook showcases the full surface. Regenerate.

## Testing

- The demo is itself a smoke test: a new `test_cli` block (or a shell check in `run_tests.sh`) runs
  `matrixdb -f examples/tour.sql` and asserts the output contains expected decoded values (e.g. a known SUM,
  a joined `amount │ name` row) and exits 0. Simplest: a `test_cli.cpp` block that opens the two example CSVs
  via `matrix_repl` over an ifstream of `tour.sql`... but `tour.sql` paths are repo-relative. Instead: a
  `test_cli.cpp` block that drives `matrix_repl` with the **same sequence** inline (not the file), asserting
  key outputs — keeping the test hermetic (no CWD dependency). The `examples/` files remain the
  human-runnable artifact; `run_tests.sh` optionally also runs `matrixdb -f examples/tour.sql` if built.
- `.timing`: a `test_cli` block sends `.timing on` then a query, asserts the output contains `µs)`.
- ASan/UBSan-clean; full suite + oracle green.

## Scope & non-goals

**In:** `examples/` demo (2 CSVs + tour.sql), `#` comment lines, `.timing on|off`, README "Try the demo" +
FINDINGS entry + notebook demo extension, hermetic tests.
**Deferred:** `EXPLAIN`/`.explain` (routing is documented in `.help`; marginal value — YAGNI); readline /
history; a man page; packaging/install. These are named, not hidden.

## Success criteria

`./matrixdb -f examples/tour.sql` runs start-to-finish showing decoded results for every form; `.timing on`
prints per-query µs; README points at the demo; FINDINGS records the arc; `test_cli.cpp` covers the tour
sequence + `.timing`; full suite + oracle green under ASan/UBSan.

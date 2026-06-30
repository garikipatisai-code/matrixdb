# matrixdb CLI / REPL (Core, v1) — Design

**Date:** 2026-06-29  **Status:** design approved; ready for an implementation plan.
**Goal:** Turn the verified engine into something a person can actually *use* — a runnable `matrixdb` that
loads CSV data, runs SQL at a prompt, and shows decoded results + catalog/stats — closing the gap between
"powerful engine" and "product you pick up." CPU engine; fully local + verifiable (no network/bind).

## Why

Today the only way to exercise MatrixDB is to write C++ tests; `main.cpp` is a benchmark/oracle harness, not
a tool. The engine already has the pieces a shell needs — a SQL-ish parser (`parse_query`, `avg_query`,
`project_query`), `execute_query`, CSV ingest, dictionary strings, catalog introspection (`catalog_columns`,
`column_name`), and `stats()`. The CLI is mostly a thin, testable driver over those verified methods.

## Shape & files (testable shell over streams)

- `matrix_cli.hpp` — `int matrix_repl(std::istream& in, std::ostream& out, CPUMockEngine& eng)`: the line
  loop, dot-command dispatch, SQL router, and output formatter. All logic lives here, pure over the two
  streams (so it is unit-testable with string streams).
- `matrixdb_cli.cpp` — a ~12-line `main`: construct a `CPUMockEngine`, then `matrix_repl(std::cin, std::cout,
  eng)`. Args: `-c "SELECT …"` (feed one line then exit) and `-f path.sql` (feed a file then exit); no args =
  interactive REPL on stdin.
- `test_cli.cpp` — drives `matrix_repl` with a scripted `std::istringstream`, asserts on the captured
  `std::ostringstream`. Runs in `run_tests.sh` under ASan/UBSan.

## Loop & commands

Read a line; trim leading/trailing whitespace. Blank → continue. A line beginning with `.` is a
dot-command; anything else is SQL. EOF or `.quit` exits the loop (returns 0). **No input ever throws or
crashes** — every engine call's status is checked and failures print a single `Error: <message>` line.

Dot-commands:
- `.load <path> <name> <u32|i64|f64|str> [header|noheader]` — ingest CSV column 0 into a new named column
  (default `header`, comma-delimited).
- `.tables` — list named tables (`tables()`).
- `.columns` — `catalog_columns()` as a table: `id  name  type  rows  tier`.
- `.stats` — `EngineStats` gauges + p50/p99 query latency.
- `.help` — the command + query summary.
- `.quit` — exit.

## SQL router

Given a non-dot line, dispatch by cheap inspection (uppercased copy):
1. contains `AVG(` → `avg_query(line)` → format double(s).
2. else contains `(` → `parse_query(line, q)`; on OK → `execute_query(q, out)` → format scalar/grouped using
   `q` (value/key columns + types); on parse error → `Error: could not parse query`.
3. else (no parens) → `project_query(line)` → format the value column.

(COUNT/SUM/MIN/MAX live in path 2; AVG in path 1; bare-column projection in path 3. HAVING / top-N /
multi-aggregate are deferred — see non-goals.)

## Output formatting

The product polish — results are decoded, never raw `uint64_t`:
- **scalar** (1 value): decode by `column_type(q.value_col)` — u32 as unsigned, i64 as signed
  (`static_cast<int64_t>`), f64 via `matrix_bit_cast<double>`. Print the bare value.
- **grouped** (`q.num_groups` values): aligned `group │ value` rows. The group key is the **decoded string**
  when the key column is dictionary-encoded (`string_dict_size(q.key_col) > 0` → `string_decode(q.key_col, g)`),
  otherwise the numeric group id. Values decoded by value type. Skip empty MIN/MAX sentinel groups? No — print
  every group `[0, num_groups)` as `execute_query` returns them (matches engine semantics).
- **AVG**: the double(s) — scalar one value; grouped `group │ avg` (numeric group id; AVG path lacks the
  parsed query, so string-key decoding is a follow-up).
- **projection**: a single column of decoded values, capped at the first 100 with a
  `… (<total> rows, showing 100)` note when longer (so it never floods a terminal). The projected column's
  type drives decoding; if it is a dictionary-encoded string column, decode each code to its string.

## `.load` data path

Assign the next free column id (a CLI-local counter starting at 1; id 0 is reserved). Dispatch by type:
- `u32`/`i64`/`f64` → `load_column_from_csv` / `_i64` / `_f64`.
- `str` → `load_string_column_from_csv` (new) → dictionary-encode.
Then `name_column(id, name)`. Print `loaded <rows> rows into "<name>" (<type>, col 0)`. A missing file or an
unparseable field → `Error: could not load <path>` (the readers already return false; no abort).

**New additive engine/IO pieces (the only non-shell code):**
- `csv_ingest.hpp`: `matrix_read_csv_column_str(path, col_index, has_header, delim, std::vector<std::string>& out) -> bool`
  — the string sibling of `matrix_read_csv_column` (keeps the field verbatim; graceful false on open failure).
- `compute_mock.cpp`: `bool load_string_column_from_csv(uint64_t id, const std::string& path, size_t col_index,
  bool has_header = true, char delim = ',')` — reads strings via the above, then `load_string_column_dict`.
  Completes the "strings are first-class, ingestable from CSV" story.

## Testing

`test_cli.cpp`: write a small CSV to `/tmp` (a numeric `amount` column + a categorical `region` column),
then script a `matrix_repl` session via `std::istringstream` and assert on the `std::ostringstream`:
- `.load /tmp/…csv amount u32` and `.load … region str` → `loaded` lines with the right row counts.
- a scalar `SELECT SUM(amount)` → the closed-form value.
- a `GROUP BY` over `region` (string key) → rows whose group labels are the **decoded strings**.
- `SELECT AVG(amount)` → the average.
- a projection `SELECT region` → decoded string values (capped).
- `.columns` shows the two columns with names/types/rows; `.stats` shows a nonzero `query_count`.
- a deliberate bad line (`SELECT FROM nonsense`) → an `Error:` line and the session continues.
- `.quit` returns cleanly.
ASan/UBSan-clean; added to `run_tests.sh` (auto-discovered) and the notebook.

## Scope & non-goals (v1)

**In:** the REPL over streams, the dot-commands above, the SQL router (scalar/grouped/AVG/projection), typed +
string-decoded output, CSV load (incl. `str`), introspection, stats, `-c`/`-f`/interactive entry, and the test.

**Deferred (named, not dropped):** HAVING / top-N (`ORDER BY … LIMIT`) / multi-aggregate (`SELECT a, b`) in the
REPL (executors exist — small router+formatter adds); `.save`/`.open` catalog snapshot; readline/history/line
editing (plain `getline`); a network/server mode (separate, `bind`-gated); string-key decoding on the AVG
path; configurable CSV column index / delimiter beyond the defaults.

## Success criteria

- `test_cli.cpp` passes in `run_tests.sh` under ASan/UBSan; the existing suite + oracle stay green (the CLI is
  additive; the new `csv_ingest`/engine methods are behavior-preserving for existing callers).
- A user can: build `matrixdb_cli.cpp`, run it, `.load` a CSV, run a scalar/grouped/AVG query and a
  projection with readable decoded output, inspect `.columns`/`.stats`, and `.quit` — with malformed input
  producing a friendly error, never a crash.

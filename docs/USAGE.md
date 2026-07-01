# MatrixDB — User Guide

`matrixdb` is a single-node analytical database you drive from a prompt or a script: load CSV columns, run
SQL-ish analytical queries, and save/reopen your data. This guide is the reference for using it. (For the
internals and the GPU story, see `README.md`.)

## Install / build

No cmake — one command produces both binaries:

```sh
./build_all.sh              # uses clang++, falls back to g++ — builds ./matrixdb and ./matrixdbd
# CXX=g++ ./build_all.sh    # force a compiler
```

You need a C++20 compiler (clang++ or g++). This is the same script CI and the Docker image use.

## Run

```sh
./matrixdb                          # interactive REPL on stdin
./matrixdb -c "SELECT SUM(amount)"  # run one line, exit
./matrixdb -f examples/tour.sql     # run a script of commands/queries, exit
```

Diagnostics (the engine's startup/shutdown banner) go to **stderr**, so `stdout` stays clean for piping.
Lines beginning with `#` are comments. Malformed input prints a single `Error: …` line and the session
continues — the REPL never crashes on bad input.

Try the bundled tour to see every feature end-to-end:

```sh
./matrixdb -f examples/tour.sql
```

## Quick start

```
matrixdb> .load examples/orders.csv  region u32 col0 header
loaded 12 rows into "region" (u32, col 0)
matrixdb> .load examples/orders.csv  amount u32 col1 header
loaded 12 rows into "amount" (u32, col 1)
matrixdb> .load examples/regions.csv reg_key  u32 col0 header
matrixdb> .load examples/regions.csv reg_name str col1 header
matrixdb> SELECT SUM(amount) GROUP BY region
0 │ 700
1 │ 1240
2 │ 635
matrixdb> SELECT SUM(amount) JOIN region = reg_key GROUP BY reg_name   # total per region NAME
east  │ 635
north │ 700
south │ 1240
matrixdb> .save mydata.db
matrixdb> .quit
```

## Commands (dot-commands)

| Command | Effect |
|---|---|
| `.load <csv> <name> <u32\|i64\|f64\|str> [colN] [header\|noheader]` | Ingest CSV column `colN` (default 0) as a typed, named column. `str` is dictionary-encoded. Default assumes a header row. |
| `.save <file>` | Snapshot the catalog (columns + names + string dictionaries) to `<file>`. Atomic — a crash mid-save can't corrupt an existing file. |
| `.open <file>` | Load a snapshot into a **fresh** session (start a new `matrixdb`). A bad/incompatible file errors, never crashes. |
| `.tables` / `.columns` | List table names / all columns (`id  name  type  rows  tier`). |
| `.stats` | Engine gauges + query-latency percentiles. |
| `.timing on\|off` | Print per-query wall-time. |
| `.mode list\|csv\|table` | Output format: human (` │ `), machine-readable (CSV), or aligned columns. |
| `.help` / `.quit` | Command summary / exit. |

## Queries

Columns are referenced by the name you gave them in `.load`. Supported forms:

```sql
-- scalar + grouped aggregates, with predicates (cross-column WHERE allowed)
SELECT COUNT|SUM|MIN|MAX|AVG(col) [WHERE fcol <op> v] [GROUP BY key]
   -- <op>: >  >=  <  <=  =  !=  ;  also  WHERE col BETWEEN lo AND hi

-- rank / filter a grouped result
SELECT SUM(col) GROUP BY key ORDER BY SUM DESC LIMIT n     -- top-N groups
SELECT SUM(col) GROUP BY key HAVING SUM <op> v             -- filter groups

-- multiple aggregates at once (a table)
SELECT COUNT(a), SUM(b), MAX(c) [WHERE …] [GROUP BY key]

-- distinct count, and a bare-column projection
SELECT COUNT(DISTINCT col)
SELECT col [WHERE fcol <op> v] [LIMIT n]

-- inner equi-join of two row-aligned datasets on a u32/i64 key
SELECT lcol, rcol JOIN lkey = rkey [LIMIT n]   -- lcol gathered left, rcol gathered right
SELECT COUNT(*)   JOIN lkey = rkey             -- join cardinality

-- aggregate a left measure over the join, by a right dimension (the star query)
SELECT agg(lcol) JOIN lkey = rkey [GROUP BY rcol [HAVING agg <op> v | ORDER BY agg DESC LIMIT n]]
```

Results are decoded to their real types — `u32`/`i64`/`f64` numerics, and dictionary columns print as their
strings (in `GROUP BY` labels and projections). `COUNT` always prints as an integer.

## Persistence

`.save <file>` writes a portable snapshot of every column (including string dictionaries). Reopen it in a
**new** session with `.open <file>`. Saves are atomic (temp-file + rename), so an interrupted save leaves the
previous snapshot intact. `.open` into a session that already has columns is refused (start fresh) — and a
corrupt or incompatible file produces an `Error:`, never a crash.

## Limits & performance

**Known limits (by design or v1 scope):**
- **Single node, in-memory working set.** Columns live in RAM, with cold columns spilling to SSD under a
  tier budget; a dataset must fit the configured tiers. Not distributed.
- **Single-writer.** Writes serialize by design (lock-free page ownership); analytical *reads* run in
  parallel (`ConcurrentServer`), concurrent *writes* are not a goal.
- **Analytical SQL subset**, not full ANSI — the grammar above. No subqueries, no SQL-level DDL beyond
  `.load`, no planner/optimizer. Joins are inner equi-joins on `u32`/`i64` keys, one column per side;
  string-dictionary keys are rejected (each column's dictionary is independent).
- **CSV is simple-split**, not RFC-4180 — values containing the delimiter, quotes, or newlines aren't
  supported, and `.mode csv` output is likewise unquoted.
- **No network/TLS in the CLI.** It's a local/single-node tool. (The engine has a GET/PUT/QUERY/STATS server
  protocol + TCP transport + auth, but TLS and a hardened public listener are out of scope here.)

**Performance (measured CLI path — reproduce with `./bench_cli.py`).** 8,000,000 rows × 2 `u32` columns
(71 MB CSV), single thread, warm (min of repeated runs). Absolute numbers are hardware-dependent (measured on
an Apple-Silicon dev machine); the *shape* is the point, and all of it scales linearly with row count
(verified 2M ↔ 8M):

| Operation | Time | Notes |
|---|---|---|
| Load (CSV → 2 columns) | ~640 ms | ~225 MB/s CSV parse, ~25M values/s; one full file pass per column |
| `SELECT SUM(amount)` — full scan | ~0.6 ms | memory-bandwidth-bound (~50 GB/s here) |
| `SELECT SUM(amount) GROUP BY region` — 10 groups | ~7.6 ms | scatter into group buckets (~1B rows/s) |
| `SELECT COUNT(DISTINCT region)` | ~25 ms | hash set over all rows |
| `SELECT SUM(amount) WHERE amount > x` — 50/50 predicate | ~40 ms | **branch-misprediction-bound** on a random predicate; a selective or clustered predicate is far cheaper |

Takeaways: ingestion is CSV-parse-bound (~225 MB/s); unfiltered scans/aggregates are memory-bound and very
fast; a low-selectivity *random* filter pays a branch-prediction tax. Use `.timing on` to measure your own
queries. (For the standalone CPU-vs-GPU scan-bandwidth thesis — a different, kernel-level benchmark — see
`README.md`.)

## Networked server (preview)

Besides the local CLI, MatrixDB has a network daemon, **`matrixdbd`**, speaking a length-prefixed
GET/PUT/QUERY/HEALTH/STATS protocol with token auth:

```sh
./build_all.sh
./matrixdbd 7070 --open mydata.db --token s3cret      # token auth, serving a saved catalog
./matrixdbd 7070                                       # dev mode: no auth, empty catalog
```

Clients talk to it with `MatrixClient` (`client.hpp`): `authenticate(token)` then `get`/`put`/`query`.

**Preview status:** the daemon compiles in CI and its per-connection auth + serve path is socketpair-tested,
but it's **host-only to run** (loopback `bind` is blocked in the build sandbox) and serves **one connection at
a time**. TLS and concurrent connections are *designed but not built* — for encryption today, terminate TLS at
a reverse proxy (nginx/Caddy/stunnel) in front of `matrixdbd` on loopback. Full plan + wire-protocol spec:
`docs/superpowers/specs/2026-06-30-matrixdb-networked-serving-design.md`.

## Docker

A multi-stage `Dockerfile` at the repo root builds `matrixdbd` (and `matrixdb`, for admin access) into a
minimal runtime image with no compiler/toolchain — the standard way to ship a single-box database today.

```sh
docker build -t matrixdb .
docker run -p 7070:7070 matrixdb                                   # dev mode: no auth, empty catalog

# authenticated, serving a snapshot mounted from the host:
docker run -p 7070:7070 \
    -e MATRIXDB_OPEN=/data/mydata.db -e MATRIXDB_TOKEN=s3cret \
    -v ./mydata.db:/data/mydata.db \
    matrixdb

# administer the running container's data directory with the bundled CLI:
docker exec -it <container> matrixdb -c "SELECT COUNT(*)"
```

`MATRIXDB_OPEN` and `MATRIXDB_TOKEN` are optional environment variables the entrypoint translates into
`matrixdbd`'s `--open`/`--token` flags; unset, the container starts in dev mode. `PORT` (default `7070`)
overrides the listen port without needing to rewrite the entrypoint.

**Current limitations (read before relying on this in anything but a dev/demo setting):**
- **Token visibility.** `MATRIXDB_TOKEN` is a plain environment variable — visible to anyone who can run
  `docker inspect` on the container, same caveat as `POSTGRES_PASSWORD` and similar containerized-DB
  conventions. There's no secrets-manager integration yet; treat container-level access as equivalent to
  token-level access.
- **No durable-volume story beyond a snapshot file.** `--open` loads one snapshot file at startup;
  `matrixdbd` does not write back to it automatically (persistence is point-op WAL, which isn't wired into
  `matrixdbd` yet — a pre-existing gap, not introduced by this image). Mounting `/data` gives you a place to
  put a snapshot to load, not a live-durable database directory.
- **CPU-only image.** The build stage has no CUDA toolchain, so the image always runs the CPU engine
  (`CPUMockEngine`) — there is no GPU-accelerated container yet.
- **No TLS.** Same as the bare `matrixdbd` — terminate TLS at a reverse proxy in front of the container if
  you need encryption.

## Verifying a build

```sh
./run_tests.sh           # compile + run every CPU test + the pipeline oracle + the CLI demo smoke-check
SAN=1 ./run_tests.sh     # the same under AddressSanitizer + UBSan
```
A green run prints `ALL GREEN (N tests + oracle)` with `demo: OK`.

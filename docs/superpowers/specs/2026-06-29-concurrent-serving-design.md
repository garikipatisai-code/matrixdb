# Concurrent Serving — Single-Writer / Many-Readers (CPU engine, v1)

**Date:** 2026-06-29  **Status:** design approved; ready for an implementation plan.
**Goal:** Let multiple analytical reads execute concurrently on a worker pool while writes serialize, taking
MatrixDB's serve layer from one-request-at-a-time toward a deployable system — **without breaking the
lock-free single-owner model and with every change verified under ThreadSanitizer + the oracle.**

## Motivation

The serve loop today is single-threaded: `matrix_serve_tcp` accepts a connection, serves one framed request
via `matrix_serve_conn` → `matrix_serve`, then moves on. One slow or compute-heavy query blocks every other
client, and a multicore host runs analytical scans on a single core. The analytical workload is read-heavy
(the project's whole thesis), so the highest-value concurrency is **many readers in parallel, writes
serialized** — the single-writer / many-readers model used by LMDB and DuckDB. This *strengthens* the
single-owner thesis (writes still serialize through one owner) rather than contradicting it.

## The core invariant

One `std::shared_mutex` guards the engine. Every served request takes it before touching the engine:

| Request kind | Lock | Requests |
|---|---|---|
| read  | `std::shared_lock` (shared) | `QUERY`, `GET`, `HEALTH`, `STATS` |
| write | `std::unique_lock` (exclusive) | `PUT` (and, in-process: `load_*`, `append_*`, `checkpoint`, `shutdown`, `set_*`) |

**Invariant:** *while any shared lock is held, no writer runs, so the catalog, tier placements, point-op
store, and WAL are immutable — therefore concurrent readers over those bytes cannot race.* Correctness is by
construction from this single fact; the rest of the design is making the read path actually honor it (it
mutates shared state today) and choosing where the lock is taken.

The lock is taken at the **serve-dispatch boundary** (one acquire per request). In-process multi-step
transactions (`begin`/`txn_put`/`commit`) stay single-threaded in v1 — they are not a wire request, so the
concurrent server never issues them; in-process callers that use them hold the exclusive lock themselves or
run single-threaded.

## Taming the read path (the actual work)

A shared-lock read must perform **no non-atomic shared mutation**. Today a `QUERY` mutates three things; each
is addressed:

1. **Heat + counters → atomics.** The counters touched on the read path become `std::atomic<uint64_t>` with
   relaxed ordering (heat and latency are heuristics; exact-once is not required):
   - `TierManager` per-column heat (`record_access`),
   - `scans_since_rebalance_`,
   - the read-touched `EngineStats` fields: `query_count`, `total_query_ns`, `max_query_ns` (atomic
     compare-exchange max), `reads`, `scans`, and the latency-histogram buckets.

   Counters mutated **only** under the exclusive lock (`writes`, `commits`, `cold_borrows`, `rebalances`,
   `migrations`, resident-bytes gauges) stay plain — a single writer touches them, and `STATS` reads them
   under a shared lock with no concurrent writer.

2. **Rebalance leaves the *shared* read path.** The new shared fast path (`execute_query_shared`) never
   migrates: when `scans_since_rebalance_` (now atomic) crosses the threshold it only *sets a "rebalance
   due" flag* and continues. The pending rebalance runs under the exclusive lock — performed by the next
   write, or an explicit `maintain()` entry. The existing `execute_query` (the exclusive / escalated path,
   and the one all current tests call directly) is **behaviorally unchanged, including its in-line
   `maybe_rebalance()`** — so single-threaded tiering behavior, the demo, and every existing test are
   identical. Only requests served concurrently through the new pool take the flag-and-defer path.

3. **Borrow becomes a write action, via escalation.** A read whose referenced columns
   (`value_col`, and when present `key_col` / `filter_col`) are all **HOST-resident** runs the pure reduction
   concurrently. A read that touches a COLD/DEVICE column needs a borrow (= migration = mutation), which is
   not allowed under a shared lock, so it **escalates** to the exclusive lock.

### The read fast-path + escalation mechanism

Add an engine method `execute_query_shared(const MatrixQuery& q, std::vector<uint64_t>& out) -> ReadOutcome`
where `ReadOutcome ∈ { SERVED, NEEDS_EXCLUSIVE }`:

- It first checks (cheap `tier()` lookups, no work) that every referenced column is HOST-resident **and** the
  query needs no write-path feature (no tiering borrow). If a column is COLD/DEVICE it returns
  `NEEDS_EXCLUSIVE` immediately, having mutated nothing and produced no output.
- Otherwise it runs the existing reduction over the (immutable-under-shared-lock) HOST bytes, bumping only the
  atomic counters, and returns `SERVED`.

`ReadOutcome` is internal to the serve layer; it never reaches the client. The existing `execute_query` is
unchanged and remains the **exclusive-path** implementation (it borrows/restores any tier as today).

Dispatcher logic per request (in the concurrent server):

```
on QUERY:
    take shared lock
    r = engine.execute_query_shared(q, out)
    if r == NEEDS_EXCLUSIVE:
        release shared lock
        take exclusive lock
        engine.execute_query(q, out)     // full path: borrow → scan → restore
on GET / HEALTH / STATS:  take shared lock; call the engine read method
on PUT (and in-process writes): take exclusive lock; call the engine method
```

Escalation is safe: under the shared lock tiers cannot change, so the HOST check is stable for the query's
duration; after releasing and re-acquiring exclusively, `execute_query` re-validates the column and tier from
scratch, so an interleaved writer between the two locks is handled (re-check, not assume).

## Serve / threading layer

Add `matrix_serve_pool(engine, ...)` — a fixed worker pool (or thread-per-connection) over the existing
`matrix_serve_conn` framing — that owns the `std::shared_mutex` and applies the shared/exclusive + escalation
rule above, reusing the existing `matrix_serve` request logic unchanged. The single-threaded
`matrix_serve_tcp` stays as the simple path.

`bind()` to a real port is blocked in the sandbox, so verification uses N threads communicating over
`socketpair`s (exactly as `test_client.cpp` / `test_server_tcp.cpp` already do) and direct multi-threaded
engine calls — not a live TCP port. `matrix_serve_pool`'s accept loop is compile-verified, like
`matrix_serve_tcp`'s is.

## Components & touchpoints

- `compute_mock.cpp` — make the read-path counters atomic (heat in `TierManager`, `scans_since_rebalance_`,
  the read-touched `EngineStats` fields); move `maybe_rebalance` to fire only under the exclusive path; add
  `execute_query_shared` (HOST-check + pure reduction) and a `maintain()` exclusive-path entry.
- `server.hpp` / `server_tcp.hpp` — add the `matrix_serve_pool` worker-pool entry + the shared/exclusive
  dispatch + escalation; reuse `matrix_serve` / `matrix_serve_conn`.
- New test `test_concurrent_serving.cpp` — the cases below (run by `run_tests.sh`, and under `SAN=1`/TSan).

## Testing (ThreadSanitizer is the gate)

`run_tests.sh` already passes clean under TSan; concurrency rides that gate. New cases:

1. **Concurrent readers, correct.** N threads issue overlapping `QUERY`s over HOST columns; every result
   equals the closed-form/brute-force oracle; zero TSan reports.
2. **Readers + a writer.** Reader threads run while one thread does `PUT`/`load`/`append`; results stay
   correct, the writer's exclusivity holds (no torn reads), zero races.
3. **COLD-column escalation.** A query over a COLD column runs under reader load; it escalates to exclusive,
   returns the correct result, no race.
4. **Mixed read/write stress** under TSan (many threads, churn) — every result oracle-checked.
5. **Throughput sanity.** N reader threads exceed single-thread throughput on a multicore host (a
   ratio check, not an absolute number — informational, not a hard gate).

## Scope & non-goals (v1)

**In:** CPU-engine concurrent reads; one global `std::shared_mutex`; the side-effect-free read path
(atomic counters, rebalance off the read path, COLD/DEVICE escalation); the `matrix_serve_pool` entry; the
test suite above.

**Deferred — named, not silently dropped:**
- **Epoch / snapshot reclamation** so reads run *concurrently with* column growth/migration instead of
  escalating to exclusive — the path to truly lock-free cold reads. v1 escalates instead.
- **Per-column / striped locking** — the single global lock is coarse but correct; finer granularity is a
  later increment.
- **GPU-engine concurrency** (CUDA streams) — v1 is the CPU engine only.
- **Concurrent multi-request transactions** over the wire.

**Known limit:** `std::shared_mutex` can starve writers / escalating reads under a sustained reader stream
(it is not guaranteed writer-preferring). Acceptable for a read-heavy analytical workload in v1; a
writer-preferring lock is the follow-up. A `// ponytail:` comment at the global-lock site names this and the
striped-lock / snapshot upgrade paths.

## Success criteria

- All existing tests + the new concurrency tests pass, clean under ASan/UBSan and **TSan**.
- The `83886070` scan oracle and the cross-backend store checksum are unchanged: concurrency is purely
  additive, and behavior through the existing (single-threaded) entry points — `execute_query`,
  `matrix_serve`, `matrix_serve_tcp` — is byte-identical (atomic counters hold the same values; the existing
  rebalance cadence is untouched).
- N concurrent readers measurably exceed one reader on a multicore host.

# MatrixDB — Production Readiness Gap Register

Status: **research prototype → production** roadmap. This is the inventory of everything
between what we have (a measured, correct GPU/CPU engine core) and a database someone
trusts with real data. Each gap is something we will run through brainstorm → spec →
plan → implement, the same loop that built the cost-based router.

**Honest scope note:** "implement all of this" is quarters-to-years, not a session. Several
items (networking with real clients, HA across nodes, multi-platform CI) can't even be
meaningfully built or verified in the current dev environment (single box, no nvcc locally,
sandboxed network). This register exists so we sequence deliberately and never confuse
"prototype works" with "production ready."

---

## Prioritization lens: who we're building for

From the client analysis, the wedge is the **single-box, unified-memory, HTAP developer/
small team** (DGX-Spark-class workstation, 50–300 GB working set, mixed point lookups +
analytical scans). That reorders priorities:

- **Up-weighted:** durability, a real data model + query API, multi-client access, single-
  node reliability, observability. These are what that buyer needs day one.
- **Down-weighted (for the wedge):** distributed consensus, cross-node sharding, multi-
  region. A single box doesn't need them. Build only when we move up-market to the
  datacenter/HBM tier — explicitly deferred, not forgotten.

---

## Severity

- **P0** — blocks any real use / silent data loss / correctness. Must fix to call it a DB.
- **P1** — required for production trust (durability, security, ops) but not a correctness bug.
- **P2** — robustness, performance, scale; matters as adoption grows.
- **P3** — up-market / future-tier; out of scope for the wedge.

Effort: S (days) · M (1–2 wk) · L (weeks) · XL (months). "Local?" = buildable + verifiable
in the current env (CPU, no network) vs needs real infra.

---

## 1. Data model & query layer

| ID | Gap | Why it matters | Sev | Effort | Local? |
|----|-----|----------------|-----|--------|--------|
| DM-1 | **Store is 4096 slots, `key & MASK` → silent overwrite on collision** **[FIXED — Inc 1: KVStore]** | Real keys collide → **silent data loss**. This is a correctness bug, not a size limit. | P0 | M | yes |
| DM-2 | No schema/catalog (tables, columns, types) | Can't model real data; everything is one fixed record shape | P0 | L | partial — DM-2 (named columns + introspection) + DM-3 (types) landed; named tables deferred |
| DM-3 | Fixed 64-byte record / 32-byte payload; no variable-length or real types | No strings, no nullable, no real columns | P0 | L | partial — DM-3a (int64 columns) landed; epic decomposed |
| DM-4 | No query language — opcodes only (`OP_READ/WRITE/SCAN`) | No way for a client to express a query; the "router" routes opcodes, there's no parser/planner | P0 | XL | partial — QRY-1 (`MatrixQuery`) + DM-4 (text parser, scalar) landed; full SQL/planner deferred |
| DM-5 | No data loading path — scan column is synthetic `value[i]=i` | Can't get real data in; no INSERT/bulk-load/import | P0 | M | yes — DM-5 (binary) + DM-5b (CSV) landed |
| DM-6 | Scans return a count only — no GROUP BY / SUM / MIN / MAX / projection / rows | Not real analytical queries | P1 | L | yes |
| DM-7 | No secondary indexes; point access is a single masked slot | Only one access path; no range scans on keys **[range scans FIXED — DM-7: kv_range; sorted index deferred]** | P2 | L | yes |
| DM-8 | No joins | Multi-table analytics impossible **[equi-join primitive landed — DM-8: hash_join]** | P2 | XL | partial |
| DM-9 | Dynamic data growth — store/column are fixed pre-allocations | Can't grow past boot-time size; no realloc/spill **[FIXED for catalog columns — DM-9: append_to_column]** | P1 | L | partial |

*Inc 1 of the three-tier engine landed: `tier_model.hpp`, tier-aware `cost_model.hpp`, and `kv_store.hpp` (DM-1 fixed — open-addressing hash table, no silent overwrite). See spec 2026-06-25-three-tier-storage-engine-design.md.*

*Inc 2 landed: `tier_manager.hpp` — the auto-tiering brain (per-column heat EWMA, capacity-gated cost-benefit promotion, cost-benefit eviction, anti-thrash; decisions-only, returns a feasible migration plan). Pure logic, fully CPU-tested. See spec 2026-06-25-increment-2-tier-manager-design.md. Remaining three-tier increments: Inc 3 = SSD cold-store/WAL (delivers durability DU-1/2/3), Inc 4 = GPU migration executor, Inc 5 = unified-memory collapse.*

## 2. Durability & persistence (the "D" in ACID)

| ID | Gap | Why | Sev | Effort | Local? |
|----|-----|-----|-----|--------|--------|
| DU-1 | **Everything in-memory — total loss on restart/crash** | A database that loses all data on exit is not a database **[FIXED — Inc 3: ColdStore WAL]** | P0 | L | yes |
| DU-2 | No write-ahead log (WAL) | No way to recover committed writes after crash **[FIXED — Inc 3: ColdStore WAL]** | P0 | L | yes |
| DU-3 | No crash recovery / replay | Can't reconstruct state from log **[FIXED — Inc 3: ColdStore WAL]** | P0 | L | yes |
| DU-4 | No checkpoint/snapshot | Recovery would replay the entire log forever **[FIXED — DU-4: WAL checkpoint/compaction]** | P1 | M | yes |
| DU-5 | No `fsync` discipline / durability guarantee levels | Can't promise "committed = survives power loss" | P1 | M | yes |
| DU-6 | No backup / restore | Ops can't protect data **[FIXED — DU-6: backup/restore]** | P1 | M | yes |

*Inc 3 landed: `cold_store.hpp` — synchronous CRC-framed append-only WAL, wired into CPUMockEngine (append-before-commit + replay-on-construct). Committed point-op writes survive restart (DU-1/2/3 fixed). Checkpoint/compaction (DU-4) and cold-column spill (Inc 4) still pending. See spec 2026-06-25-increment-3-cold-store-wal-design.md.*

*DU-4 landed (WAL checkpoint/compaction, CPU): the point-op WAL no longer grows without bound. `CPUMockEngine::checkpoint()` snapshots `kv_` to a `<wal>.ckpt` file (atomic temp→fsync→`rename`) then `ColdStore::truncate()`s the log; recovery now `load_checkpoint` THEN replays only post-checkpoint records — bounding both restart time and disk. **Crash-safe by construction:** `kv_.put` is idempotent (last-writer-wins), the snapshot is durable before the truncate, and new writes append only after `checkpoint()` returns, so a crash at any point recovers checkpoint + surviving-WAL to the same state (no loss, no double-apply). A reviewer empirically attacked every crash window (post-rename/pre-truncate, mid-truncate, stale `.tmp`, malicious `count=2^60`→aborts in 0s, ENOSPC→atomic) and proved non-vacuity (a no-op `save_checkpoint` fails the checkpoint-alone test). Added `KVStore::for_each`, `ColdStore::truncate`, `checkpoints()`/`wal_records()` observability. 21-test suite + oracle green; the 3 recovery-path tests (engine_restart/transactions/cold_store) still pass. See spec/plan 2026-06-27-wal-checkpoint. Deferred: directory-fsync for rename durability (pre-existing layer-wide gap), auto-checkpoint triggers, WAL segment rotation.*

*DU-6 landed (backup/restore, CPU): one-call `backup(prefix)` / `restore(prefix)` for the whole durable state — composes the existing fail-loud writers. `backup` = `save_catalog(prefix+".catalog")` (tiered analytical store, all column types) + `save_checkpoint(prefix+".kv")` (point-op store); `restore` = `load_catalog` + `load_checkpoint` into a fresh engine. A point-in-time snapshot that works with OR without an attached WAL (`save_checkpoint` snapshots the in-memory `kv_`, which reflects all committed writes). Verified by a full round-trip (u32 + int64 columns with negatives/>UINT32_MAX + committed point-op writes → fresh engine restore → all values + types intact, incl. a no-WAL source) + empty round-trip. 31-test suite + oracle green; the underlying writers (`test_catalog_snapshot`/`test_checkpoint`/`test_typed_snapshot`) pass. See spec/plan 2026-06-27-backup-restore. Deferred: incremental/differential backup, compression, remote targets.*

*Inc 4 landed: `tiered_column.hpp` + `migration_executor.hpp` — cross-tier byte movement (HOST/RAM ↔ DEVICE/VRAM ↔ COLD/SSD via HOST), checksum-invariant, driven by TierManager decisions. A VRAM-promoted column is proven GPU-scannable in place. The heat→decision→migration loop is closed on the TieredColumn primitive. Live-engine integration (the OP_SCAN column becoming a managed TieredColumn) is the next step. See spec 2026-06-26-increment-4-migration-executor-design.md.*

*INT-1 landed (integration debt closed): the previously tested-but-dormant TierManager + MigrationExecutor + TieredColumn are now WIRED INTO the live `CPUMockEngine`. OP_SCAN carries a column id (id 0 = legacy fixed column, oracle unchanged); id>0 targets a tiered catalog the engine auto-tiers by access heat — demoting the coldest columns to SSD under a RAM budget so the engine holds a working set larger than RAM, and borrowing a cold column back to RAM for a scan (results correct regardless of tier). Proven non-vacuous (the test fails if rebalancing is disabled). CPU/HOST↔COLD only; DEVICE inert on the CPU build via device_cap=1. See spec/plan 2026-06-26-live-tiering-integration. **Honest scope:** delivers borrow-on-access, NOT heat-driven resident re-promotion under a full budget (TierManager promotion only fills free HOST space — no swap-on-promote yet). That + per-column COLD-file uniqueness + a release-safe unregistered-id guard = INT-1b, the immediate next increment.*

*INT-1b landed (auto-tiering loop COMPLETE): `tier_manager.hpp` gained swap-on-promote — when a re-hot lower-tier column wants a full faster tier, it displaces the lowest-keep_score resident if it's >1.5x more valuable (SWAP_MARGIN) and the victim is past MIN_RESIDENCY_TICKS. So a column scanned heavily again climbs back to RESIDENT RAM (not just borrowed per-scan); proven end-to-end (test_repromotion_under_pressure, non-vacuous via disabling the mechanism). The full RAM↔SSD auto-tiering loop (demote cold + re-promote hot) now runs live in the CPU engine. Hardening: per-process/instance-unique TieredColumn COLD path (kills silent same-id cross-corruption); release-safe guard for scanning an unregistered column id (was a null-deref under NDEBUG). See spec/plan 2026-06-26-repromotion-swap-on-promote. Remaining tiering: DEVICE/VRAM catalog promotion in the CUDA engine (the 24x perf win — migration mechanics proven by Inc 4, needs Colab to run); swap-on-promote multi-victim + COLD-write skip-if-unchanged are deferred perf niceties.*

*AGG-1 landed (DM-6 analytical aggregations, CPU): OP_SCAN generalized from `count(value>threshold)` to a filtered reduction — COUNT/SUM/MIN/MAX (`MatrixAggOp` @payload[16], default AGG_COUNT) via `matrix_cpu_reduce`, over both the legacy fixed column and the tiered catalog (correct even over a COLD-demoted-then-borrowed column). COUNT is byte-identical arithmetic to the old loop, so the oracle (83886070) and the benchmark hot loop are unchanged (measured: no regression). Empty-set sentinels: MIN=UINT64_MAX, MAX=0. Backward compatible (existing scans read op 0=COUNT). See spec/plan 2026-06-26-aggregations. **AGG-2 next:** the GPU reduction kernel — the CUDA `execute_scan` currently honors only AGG_COUNT (flagged loudly in-code); SUM/MIN/MAX on the GPU need a parallel-reduction kernel (Colab). Also deferred: GROUP BY / multi-column / richer predicates (the query layer DM-4); AVG is caller-derivable (SUM/COUNT).*

*GBY-1 landed (GROUP BY, CPU): `matrix_cpu_group_reduce` (dense `[0,num_groups)` key reduction, COUNT/SUM/MIN/MAX, out-of-range keys ignored, empty-group sentinels matching the scalar reducer) + `CPUMockEngine::grouped_aggregate(key_id, value_id, num_groups, op, out)` — per-group aggregate over two aligned tiered columns, double borrow-and-return (both columns pulled to RAM for the reduction, returned to home tier; correct even when both are COLD-demoted). Records heat on both; migration stays scan-driven. Not on ComputeInterface (CPU-engine method, GPU unaffected). Oracle + 10-test suite green. See spec/plan 2026-06-26-group-by.*

*GBY-2 landed (filtered GROUP BY, CPU): `WHERE value > threshold` added to grouped aggregation via a templated `matrix_group_reduce_impl<bool Filtered>` (`if constexpr` compiles the filter out → unfiltered path byte-identical, zero churn to GBY-1) + `matrix_cpu_group_reduce_where` + `CPUMockEngine::grouped_aggregate_where`. Completes the canonical `SELECT key, AGG(value) WHERE value>T GROUP BY key`. Oracle + 10-test suite green. See spec/plan 2026-06-26-filtered-group-by. The 4 analytical entry points (scalar scan/agg, grouped, filtered-grouped) are candidates to unify behind one `MatrixQuery` (DM-4 query layer) next.*

*QRY-1 landed (DM-4 lite, unified query API, CPU): `struct MatrixQuery {value_col, agg, has_filter, threshold, grouped, key_col, num_groups}` + `CPUMockEngine::execute_query(q, out)` — one entry point routing the 4 cases (scalar/grouped × filtered/unfiltered) to the existing primitives. Added `matrix_cpu_reduce_all` (the missing unfiltered scalar reduce) + a defaulted `has_filter=true` param on `scan_tiered_column` (oracle-safe: execute_scan's call + matrix_cpu_reduce unchanged; the 83886070 oracle is the legacy id-0 path, untouched). Catalog columns only (id>0; legacy id-0 is the benchmark fixture). Designated-initializer ergonomics read like SQL. 11-test suite + oracle green. See spec/plan 2026-06-26-query-api. **QRY-2 next:** demonstrate the stack live in `main` (load → auto-tier → execute_query → print) — the capabilities currently run only in tests; the live pipeline still only does point-ops + the legacy scan.*

*QRY-2 landed (live analytical demo in `main`): `analytical_query_demo()` runs in the binary after the existing pipeline — loads 3 catalog columns (12 MB) into an 8 MB RAM budget, runs `SELECT key, SUM(va) WHERE va>500 GROUP BY key` + scalar queries via `execute_query`, drives a hot workload so the idle 3rd column auto-demotes to SSD (prints `col3=COLD` — a 3-column working set in a 2-column RAM budget), then queries the demoted column (pulled back, correct). **Self-verifying:** every result is asserted against an in-function brute-force oracle (a reviewer proved the asserts are real guards by forcing a SIGABRT). Oracle-safe: separate engine + catalog ids>0, runs after the 83886070 assert. The analytical stack (tiering + aggregation + group-by + query API) is now visible AND verified in the running product, not just in tests. See spec/plan 2026-06-26-live-query-demo. The CPU analytical engine arc is complete; remaining axes: GPU batch (Colab), DM-5 data loading from file, OB observability, NW/TX/SE.*

*OB-1 landed (observability, CPU): `struct EngineStats` + `CPUMockEngine::stats()` expose tiering activity (cold_borrows, rebalances, migrations) + resident-bytes gauges (HOST/COLD) + catalog size. Counters incremented additively at all 5 COLD→HOST borrow sites + the rebalance trigger (`migrations_ += executor.apply(...)`), verified by a reviewer to cover every borrow path with no miss/double-count — oracle-safe. The live demo prints them (`cold_borrows=1 rebalances=8 migrations=1 … resident HOST=8MB COLD=4MB`), so the auto-tiering is now observable in the running binary. 12-test suite + oracle green. See spec/plan 2026-06-26-observability. OB-2 (latency histograms / metrics export / leveled logging) deferred.*

*OB-2 landed (per-query latency metrics, CPU): `EngineStats` gains `query_count` / `total_query_ns` / `max_query_ns` — the #1 DB ops metric. `execute_query` is now a thin timing wrapper (`steady_clock`) around a renamed private `execute_query_impl`, so EVERY call (OK and every ERR path) is timed at one point with zero caller churn. `stats()` exposes them (count → mean via total/count, + max); the live `analytical_query_demo()` prints `query latency: count=… mean=…us max=…us`. Oracle-safe — `execute_query` is the catalog query API, NOT the id-0 benchmark/oracle scan, so no measured/hot path is touched (83886070 unchanged). Verified: 5 OK + 2 ERR queries → `query_count == 7`, `total_query_ns > 0`, `max_query_ns ∈ (0, total]` (non-vacuous on the exact count; ERR queries counted too). 30-test suite + oracle green; `test_observability` (OB-1, named-field access) survives the appended fields. See spec/plan 2026-06-27-query-latency-metrics. Deferred: latency histogram / p50-p99 (OB-2b), per-op-kind breakdown, metrics export.*

*DM-5 landed (binary column persistence, CPU): `column_io.hpp` (`matrix_write_column`/`matrix_read_column` — `[u32 magic][u64 count][count×u32]`, fail-loud abort on open/short/bad-magic, host-endian/same-machine documented) + `CPUMockEngine::save_column` (borrows to HOST, reuses the OB cold-borrow counter) / `load_column_from_file` (→ `load_scan_column`). Closes the data in/out + durability gap: the engine can now ingest a column from disk and persist one (incl. a COLD-tier column). End-to-end lifecycle complete: load → auto-tier → query → observe → save. 13-test suite + oracle green. See spec/plan 2026-06-26-column-persistence. CSV ingest landed as DM-5b (below); typed/float/signed ingest rides DM-3.*

*CKPT-1 landed (catalog snapshot durability, CPU): `CPUMockEngine::save_catalog(path)` / `load_catalog(path)` — single-file snapshot `[u32 magic 'MCAT'][u64 num_cols]{[u64 id][u64 count][count×u32]}` of the whole tiered catalog (borrows COLD columns to HOST to read, returns them), restored into a fresh engine via `load_scan_column` (columns land in HOST; the TierManager re-tiers under load — tiers are a heat cache, not data, so placement need not be persisted). The analytical store now survives a restart, complementing the point-op WAL. Fail-loud, host-endian. 14-test suite + oracle green. See spec/plan 2026-06-26-catalog-snapshot. Crash-atomic (.tmp+rename) snapshot deferred.*

*DM-5b landed (CSV ingest, CPU): `csv_ingest.hpp` (`matrix_read_csv_column(path, col_index, has_header, delim, out)` — line-at-a-time `std::getline` + `std::from_chars`, parses one uint32 column out of a simple unquoted CSV; CRLF-tolerant) + `CPUMockEngine::load_column_from_csv(id, path, col_index, has_header=false, delim=',')` (→ `load_scan_column`). Closes the "can't get REAL data in" half of DM-5 P0: an external integer dataset now ingests straight into the tiered catalog and is queryable. **CSV is untrusted input**, so — unlike the binary reader's `abort()` on corruption of our OWN format — malformed CSV (open fail / short row / non-integer / overflow / trailing junk) returns `false` gracefully with no catalog entry created, never a crash (the VAL-1 lesson). A reviewer proved non-vacuity by building a mutant parser that ignores bad fields — it aborts on the test's failure asserts. 20-test suite + oracle green. See spec/plan 2026-06-27-csv-ingest. Deferred: RFC-4180 quoting, typed/float/signed columns (DM-3), multi-column single-pass, CSV export.*

*VAL-1 landed (query input validation, CPU): `execute_query` now returns `MatrixQueryStatus` (OK / ERR_UNKNOWN_COLUMN / ERR_INVALID_GROUP / ERR_TOO_MANY_GROUPS) and validates at the boundary — unknown/zero column id, self-group (key==value), key/value length mismatch, num_groups==0, and num_groups>MAX_QUERY_GROUPS (1<<28) are all rejected gracefully (out cleared) with NO assert/throw/crash. The `||` short-circuit (catalog_has before `.at()`) makes the error paths release-safe — proven under `-DNDEBUG` (a reviewer wrapped a bad query in try/catch: no throw). The query trust boundary no longer crashes on malformed input. Existing callers (demo, test_query) ignore the new return and still compile. 15-test suite + oracle green. See spec/plan 2026-06-26-query-validation. Validating the admin/persistence entry points is the follow-up.*

*QRY-3 landed (richer scan predicates, CPU): the analytical WHERE clause was `value > threshold` only; it now supports GT/GE/LT/LE/EQ/NE/BETWEEN over catalog columns. `compute.hpp`: `enum class MatrixCmp`, `struct MatrixPredicate{cmp,a,b}`, `matrix_pred_match`, `matrix_cpu_reduce_pred` (the predicate-aware sibling of `matrix_cpu_reduce`); `matrix_group_reduce_impl<Filtered>` generalized to take a predicate (unfiltered path byte-identical via `if constexpr`); `MatrixQuery` gained `cmp` (default GT) + `upper` (BETWEEN's inclusive upper). Threaded only through `execute_query`/`scan_tiered_column`/`grouped_aggregate_pred` — **oracle-safe**: the id-0 benchmark scan still uses the untouched `matrix_cpu_reduce`, and every existing query (cmp defaults to GT) is byte-identical. A reviewer proved the GT-equivalence of the rerouted id>0 path by 96k randomized+boundary checks AND an assembly diff of the unfiltered group loop, and proved non-vacuity by two separate mutations (scalar + grouped dispatch each independently load-bearing). MAX empty-sentinel stays 0 — now ambiguous only for 0-matching predicates (read COUNT to disambiguate; documented). 22-test suite + oracle green; the 5 catalog-path tests pass unmodified. See spec/plan 2026-06-27-richer-predicates. Deferred: key-side predicates, IN-lists, multi-predicate AND/OR, GPU pushdown, float/signed (DM-3).*

*DM-3a landed (int64 typed columns — first slice of the DM-3 typed-columns epic, CPU): columns were all uint32 (values ≤ ~4.29 B, no negatives). Added a per-column element-type tag + signed `int64` columns with scalar (unfiltered) COUNT/SUM/MIN/MAX. `compute.hpp`: `enum class MatrixType{U32=0,I64}`, `matrix_cpu_reduce_all_i64` (signed; empty sentinels MIN=INT64_MAX/MAX=INT64_MIN), `MatrixQueryStatus::ERR_UNSUPPORTED_TYPE`. `compute_mock.cpp`: `col_types_` side-map (absent⇒U32, so `TieredColumn` — already byte-oriented — is untouched), `load_scan_column_i64`, `column_type`, `scan_tiered_column_i64` (same borrow-and-return), `execute_query` int64 dispatch. **Additive + oracle-safe**: every existing column is U32 so the new branch is dormant; the id-0 oracle path + `matrix_cpu_reduce*` are untouched (83886070 byte-identical). Result delivered as the int64 two's-complement bit pattern in the `uint64` out vector (caller `static_cast<int64_t>`). A reviewer found + I fixed two real corruption paths before merge: (1) an int64 GROUP-BY **key** (8N bytes == a 2N-row u32 value, so it passed the length guard) was reinterpreted as u32 → now rejected (`ERR_UNSUPPORTED_TYPE`, pinned by `test_typed_key_rejected`); (2) the persistence guard was assert-only → silently corrupted in release → now unconditional fail-loud (verified: release `save_column` on an int64 column aborts, writes nothing). 23-test suite + oracle green; 8 catalog/query tests + `-DNDEBUG` pass. See spec/plan 2026-06-27-typed-columns-int64 (incl. the DM-3 epic decomposition: 3b int64 predicates→filtered/grouped, 3c typed persistence+CSV, 3d double, 3e GPU). KNOWN follow-up: `scan_tiered_column_i64` omits the rebalance trigger — heat is still recorded so int64 columns tier under mixed workloads, but a pure-int64 workload under a bounded `host_cap` won't auto-demote (fold into a shared helper in 3b).*

*DM-3b landed (int64 filtered aggregation — second slice, CPU): int64 columns now support `WHERE value <cmp> bound` for GT/GE/LT/LE/EQ/NE/BETWEEN (DM-3a was unfiltered-only). `compute.hpp`: `struct MatrixPredicateI64{cmp,a,b}` (signed bounds), `matrix_pred_match_i64`, `matrix_cpu_reduce_pred_i64` (empty sentinels MIN=INT64_MAX/MAX=INT64_MIN). The int64 filter bounds ride NEW parallel `MatrixQuery` fields `lo_i64`/`hi_i64` — NOT a widening of `threshold`, which would break `server.hpp`'s field-by-field wire format (server int64-query support stays future). `execute_query` now rejects only grouped int64 (DM-3c); scalar filtered/unfiltered routes through the generalized `scan_tiered_column_i64`. **Also closed the DM-3a follow-up**: extracted the rebalance trigger into a shared `maybe_rebalance()` (verified byte-identical to the inline block — `test_live_tiering` green) now called by BOTH the u32 and int64 scan paths, so int64 scans drive the heat-rebalance cadence (`test_typed_predicates` asserts `stats().rebalances >= 1` after int64 scans). Updated DM-3a's now-stale "filtered int64 unsupported" assertion. The DM-3a type-confusion fixes (int64 GROUP-BY-key rejection, fail-loud save guards) remain intact. 24-test suite + oracle green; the 10 affected catalog/query/server tests pass. See spec/plan 2026-06-27-typed-columns-int64-filter. Deferred: grouped int64 (DM-3c — row-count guard + u32-key/int64-value reducer), typed persistence+CSV, double (DM-3d), over-the-wire int64 filtering, GPU.*

*DM-3c landed (grouped int64 aggregation — third slice, CPU): completes int64 query parity (scalar + filtered + **grouped**). `SELECT key, AGG(int64_value) [WHERE ...] GROUP BY key` over a **uint32 key** + **int64 value**. `compute.hpp`: `matrix_group_reduce_i64_impl<Filtered>` + `matrix_cpu_group_reduce_i64`/`_pred` (signed values/accumulators). `compute_mock.cpp`: `column_rows` (type-aware row count), `grouped_aggregate_i64` (double borrow-and-return, out = int64 bit-patterns), `execute_query` int64-grouped dispatch. Two correctness points the uint32 path lacks, both non-vacuously tested: (1) int64 MAX inits **INT64_MIN** (a negative-only group yields the right negative max, not 0 — `mx[1]==-1` asserts it); (2) the length guard compares **ROW COUNTS** not byte lengths (an int64 value is 8N bytes vs a uint32 key's 4N — the working grouped test, 56-byte value + 28-byte key, passes only with the row-count guard). All key/value type combos safe: u32val+u32key works (u32 path), i64val+u32key works (new), i64 KEY rejected `ERR_UNSUPPORTED_TYPE` (key-type check runs first, guarded by catalog_has so an unknown key still → `ERR_INVALID_GROUP`), row-count mismatch → `ERR_INVALID_GROUP`. The uint32 grouped path + byte-length guard untouched. 25-test suite + oracle green. See spec/plan 2026-06-27-typed-columns-int64-grouped. Deferred: int64/typed GROUP-BY **key** (DM-3d), typed persistence+CSV (int64 still RAM-only — save_* fail-loud-aborts on it), double, GPU.*

*DM-3d landed (typed catalog snapshot — fourth slice, CPU): int64 columns are no longer RAM-only — an int64 (or mixed u32+int64) analytical store now survives a restart. `save_catalog`/`load_catalog` are type-aware: per-column record is `[u64 id][u32 type][u64 count][count×width]` (width 4=U32, 8=I64), the snapshot magic bumped to v1 (`'MCA1'`, so a v0 snapshot fails loud rather than misparsing), and `load_catalog` dispatches by type to `load_scan_column`/`load_scan_column_i64` (restoring the type tag); the `save_catalog` int64-abort guard is removed. Scoped to the catalog snapshot — single-column `column_io.hpp`/`save_column` stay u32-only (their int64 guard remains; single-column typed file I/O is a later slice). Verified by a mixed round-trip (an int64 column with negatives + a >UINT32_MAX value survives save→fresh-engine→load with type and SUM/MIN/MAX intact — non-vacuous, a u32 round-trip would corrupt it). 26-test suite + oracle green; `test_catalog_snapshot` (u32) round-trips through the new format. See spec/plan 2026-06-27-typed-catalog-snapshot. Deferred: single-column typed file I/O + typed CSV, double (DM-3e), GPU, typed GROUP-BY key.*

*DM-3e landed (double/float64 columns — fifth slice, CPU): real floating-point data now has a column type. `compute.hpp`: `MatrixType::F64` (=2), `matrix_cpu_reduce_all_f64`/`matrix_cpu_reduce_pred_f64` + `MatrixPredicateF64` + `matrix_pred_match_f64` (empty sentinels MIN +inf / MAX -inf; IEEE NaN — ordered preds false, NE true, skipped by MIN/MAX, poisons SUM — documented not special-cased). `MatrixQuery` += `lo_f64`/`hi_f64` (double filter bounds, parallel to the int64 ones, not wire-serialized). `compute_mock.cpp`: `load_scan_column_f64`, `scan_tiered_column_f64`, `column_rows` width 8 for I64 **and** F64, `execute_query` F64 scalar dispatch (grouped → ERR_UNSUPPORTED_TYPE, DM-3f), `load_catalog` F64 branch (`save_catalog` unchanged — type-generic since DM-3d, so double columns are durable too). Result delivered as the double's bit pattern via `std::bit_cast<uint64_t>` (caller `std::bit_cast<double>`). Scalar (unfiltered + filtered) + durable in this slice. Verified with exactly-representable doubles (`==` exact) incl. fractional 0.5 surviving (non-vacuity — a uint32/int64 path would truncate) + a NaN-predicate case + a save→restart round-trip. 27-test suite + oracle green. See spec/plan 2026-06-27-typed-columns-double. Deferred: grouped double (DM-3f), single-column typed file I/O + typed CSV, GPU.*

*DM-3f landed (grouped double — sixth slice, CPU): **double query parity complete** (scalar + filtered + grouped, + durable). `GROUP BY uint32_key, AGG(double_value)`, filtered + unfiltered — a direct mirror of DM-3c (grouped int64). `compute.hpp`: `matrix_group_reduce_f64_impl<Filtered>` + `matrix_cpu_group_reduce_f64`/`_pred` (double accumulators; MAX inits `-inf` so a negative-only group yields the right max, non-vacuously tested via `mx[1]==-0.25`). `compute_mock.cpp`: `grouped_aggregate_f64` (double borrow-and-return, `std::bit_cast<uint64_t>` per group), `execute_query` F64-grouped dispatch (row-count guard via the F64-aware `column_rows`; double key rejected `ERR_UNSUPPORTED_TYPE`, key-type check first guarded by `catalog_has`). 28-test suite + oracle green. See spec/plan 2026-06-27-typed-columns-double-grouped. **All three column types (uint32, int64, double) now have full query parity (scalar/filtered/grouped) + catalog-snapshot durability.** Deferred: single-column typed file I/O + typed CSV (int64/double), GPU typed kernels, typed GROUP-BY key.*

*DM-3g landed (typed CSV ingest — seventh slice, CPU): real signed/floating data can now be ingested from CSV (was uint32-only). `csv_ingest.hpp`: `matrix_read_csv_column_i64` (parses via `std::from_chars`<int64> — negatives + >UINT32_MAX, rejects junk/overflow) + `matrix_read_csv_column_f64` (parses via `std::strtod` — `from_chars`<double> is absent on Apple libc++; requires full-field consumption `endptr==field_end` + `errno!=ERANGE` + empty-field guard). `compute_mock.cpp`: `load_column_from_csv_i64`/`_f64` → `load_scan_column_i64`/`_f64`. Same VAL-1 graceful contract: malformed → `false`, no catalog entry, never abort. Additive — the u32 CSV path untouched. test_typed_csv.cpp (i64 negatives/large + f64 fractional/exponent + graceful failures + engine + non-vacuity). 29-test suite + oracle green. See spec/plan 2026-06-27-typed-csv-ingest. **DM-3 typed-columns epic substantially complete: uint32/int64/double load (in-mem + CSV) + query (scalar/filtered/grouped) + durable (catalog snapshot).** Deferred-low-value: single-column typed BINARY file I/O (the catalog snapshot already gives typed durability — `save_column` keeps its u32 guard); GPU typed kernels; typed GROUP-BY key.*

*DM-2 landed (named columns + catalog introspection, CPU): the catalog was numeric-id-only with no way to enumerate it. Added optional names — `name_column(id, name)` / `column_id(name)` (→ 0 if absent) / `column_name(id)` (→ "" if unnamed) — and `catalog_columns()` returning `ColumnInfo{id, name, type, rows, tier}` per column (a "SHOW COLUMNS"), assembled from the existing `column_type`/`column_rows`/`column_tier` accessors + a name↔id map. A client/operator can now reference columns by name (resolve → id → the existing query path) and discover the whole store with its types and tiers. Thin + additive — naming optional, load/query/oracle unchanged. With DM-3 (types) this closes the bulk of the DM-2 schema/catalog gap (named, typed, introspectable columns; named TABLES — grouping columns — deferred). test_schema.cpp. 32-test suite + oracle green. See spec/plan 2026-06-27-named-columns-catalog.*

*DM-2b landed (persist column names, CPU): closes a finding the QA-2 integration test surfaced — DM-2 names were RAM-only, lost on backup/restore. The catalog-snapshot per-column record now carries the name (`[id][type][count][u32 namelen][name][data]`; magic bumped to v2 `'MCA2'` so a v1 snapshot fails loud; `namelen ≤ 4096` guard); `load_catalog` restores it via `name_column`. So `backup`/`restore` now round-trip names — the integration test asserts `column_id("revenue") == 7` post-restore and queries by the restored name. Unnamed columns (namelen 0) round-trip exactly as before. 37-test suite + oracle green. See spec 2026-06-27-persist-column-names.*

*DM-9 landed (append / dynamic column growth, CPU): catalog columns are no longer load-once — `append_to_column` / `append_to_column_i64` / `append_to_column_f64` grow an existing column with new rows. `TieredColumn::append_bytes` grows the HOST buffer (fail-loud if not HOST); a private `append_raw` borrows a COLD column to HOST, appends, returns the borrow (rewriting the grown buffer to its home tier), and updates the brain's per-column byte accounting via a new `TierManager::update_bytes` (preserves tier/heat). Appended rows are immediately queryable (`column_rows` grows; aggregates include them). Verified across the COLD tier (a demoted column grows and stays queryable — 100003 rows, the borrow-append-return path) + u32/int64/double + non-vacuity (SUM before ≠ after). Append doesn't itself rebalance (next scan does). 33-test suite + oracle green. See spec/plan 2026-06-27-append-column-growth. Deferred: row delete/shrink, streaming append, growing the id-0 fixed column.*

*DM-7 landed (key range scan, CPU): the point-op store had only exact-key `kv_get`; `kv_range(lo, hi)` now returns every `(key, value)` with `lo ≤ key ≤ hi` (inclusive), reusing `KVStore::for_each` (DU-4). Adds a range access path the engine lacked. ponytail: O(capacity) full scan (the store is an unordered open-addressing hash) — a sorted secondary index for log-time selective ranges is the deferred "index" half of DM-7. test_kv_range.cpp (inclusive bounds, empty/full ranges, non-vacuity — out-of-range excluded, both boundaries included). 34-test suite + oracle green. See spec 2026-06-27-kv-range-scan. Deferred: sorted secondary index, range over catalog columns.*

*DM-4 landed (text query parser, scalar subset, CPU): the engine now has a query LANGUAGE, not just opcodes/structs. `CPUMockEngine::parse_query(sql, out)` parses `SELECT <agg>(<col>) [WHERE <col> <op> <val> [AND <val>]]` (agg COUNT/SUM/MIN/MAX; op > >= < <= = != BETWEEN; case-insensitive) into a `MatrixQuery` ready for `execute_query` — resolving the column NAME via DM-2 `column_id` and placing the bound in the field matching the column TYPE (DM-3: u32→threshold/upper, int64→lo_i64/hi_i64, double→lo_f64/hi_f64, parsed via from_chars/strtod with overflow rejected). A small char tokenizer + recursive parse; `MatrixQueryStatus::ERR_PARSE` added. **Untrusted input → graceful ERR_PARSE/ERR_UNKNOWN_COLUMN, never a crash** (the public wrapper resets `out` on any error so no partial state leaks). An adversarial reviewer threw 90 pathological inputs under ASan/UBSan — zero findings; overflow rejected for all 3 types; type-aware placement proven non-vacuous (a mutant set_bound aborts the test). test_query_parser.cpp. 35-test suite + oracle green; release-safe (-DNDEBUG). See spec/plan 2026-06-27-query-parser. Completes the DM-4 P0 for scalar queries (with QRY-1's struct); GROUP BY parsing (num_groups), projections, multi-predicate, joins, full SQL = deferred.*

*DM-8 landed (equi-join primitive, CPU): the engine had no join (multi-table correlation impossible). `CPUMockEngine::hash_join(left_key_id, right_key_id)` does an inner hash equi-join over two uint32 key columns — builds a `value→[left rows]` hash on the left, probes with the right, returns every `(left_row, right_row)` pair whose keys match (cardinality = `.size()`). Borrow-and-return both columns across tiers like `grouped_aggregate`. Fits the column model (a "table" = equal-length columns indexed by row); the matched index pairs let a caller correlate any other columns. Verified against a brute O(n·m) oracle incl. duplicate-key Cartesian, no-match, and a COLD-tier borrow (idle column demoted under a 16-byte host_cap, asserted COLD, joined correctly). test_join.cpp. 36-test suite + oracle green. See spec/plan 2026-06-27-equi-join. ponytail: builds on the left unconditionally + materializes all pairs in RAM. Deferred: typed/string join keys, join-then-project, outer/non-equi/range joins, build-side selection + planner, spill.*

*DM-8b (§6 resource-safe join, CPU): `hash_join_count(left, right)` returns an equi-join's cardinality WITHOUT materializing the pairs — builds a value→count map (O(distinct) memory, not O(left rows)) and sums match counts on probe, so a huge join's count is computable without the O(pairs) RAM blow-up (addresses DM-8's materialize-all ceiling). Cross-checked in test_join.cpp against `hash_join(...).size()`. 37-test suite + oracle green.*

**GPU batch (queued for the user's next Colab run — host `<<<>>>` syntax is not clang-compilable, so these can't be verified autonomously; each carries the cross-backend invariant GPU==`matrix_cpu_*` as its correctness anchor):** AGG-2 GPU SUM/MIN/MAX reduction (atomicAdd/atomicMin/atomicMax variants of the u32x4 scan kernel, dispatched on the agg op); GPU grouped-reduction (atomics into per-group accumulators); DEVICE/VRAM catalog promotion (wire the tiered catalog into the CUDA engine with a real VRAM budget — the 24x scan win; migration mechanics proven by Inc 4). Fully-local CPU increments are preferred while autonomous. **→ Turnkey plan with exact kernels + the cross-backend verification harness: `docs/superpowers/plans/2026-06-26-gpu-batch-colab-ready.md` (research+plan done; implement+verify on Colab, merge per-piece when green).**

## 3. Transactions & concurrency correctness

| ID | Gap | Why | Sev | Effort | Local? |
|----|-----|-----|-----|--------|--------|
| TX-1 | No multi-key/multi-statement transactions (no atomicity, no rollback) | Page-ownership serializes one key; real txns span many | P0 | L | yes |
| TX-2 | No isolation levels / MVCC | Concurrent readers/writers have no defined semantics | P1 | XL | yes |
| TX-3 | Full OCC (TEV lock-bit + read-set validation) never built (spec'd, deferred) | The conflict path the spec designed is absent | P1 | L | yes |
| TX-4 | Commit/visibility semantics undefined | When is a write visible to a reader? No answer today | P1 | M | yes |

*TX-1 landed (atomic transactions, CPU — fully local, no infra): `CPUMockEngine::begin/txn_put/commit/rollback` over a WAL-backed group commit. A transaction buffers writes; `commit()` durably appends them as one group (`append_txn_put × N` + a CRC'd commit marker, fsync) then applies; `rollback()` discards (writes nothing). **Crash-atomic:** a transaction is all-or-nothing across a crash — replay buffers `OP_TXN_WRITE` records and applies them only on a commit marker, discarding an uncommitted (trailing) group. Additive to the WAL: the `OP_WRITE` auto-commit path is byte-identical, so `test_cold_store`/`test_engine_restart` pass unchanged (a reviewer proved crash-atomicity with a 6-case adversarial harness; the OP_WRITE refactor is byte-for-byte behavior-preserving). Delivers ACID **A**(tomicity) on top of the existing **D**(urability). 16-test suite + oracle green. See spec/plan 2026-06-26-transactions. **Remaining TX (all local): TX-2 isolation/MVCC, TX-4 reader-visibility/snapshot semantics — bigger, deferred.***

## 4. Networking, API & multi-client

| ID | Gap | Why | Sev | Effort | Local? |
|----|-----|-----|-----|--------|--------|
| NW-1 | **No network layer — in-process hardcoded producer loop** | Nobody can connect to it | P0 | L | partial |
| NW-2 | **SPSC ring is single-producer** — one client by construction | Real DB needs many concurrent clients → MPSC or per-connection queues feeding the consumer | P0 | L | yes |
| NW-3 | No wire protocol | No defined client/server contract | P1 | L | partial |
| NW-4 | No client driver/libraries | Nothing for an app to import | P1 | L | partial |
| NW-5 | No connection management (pooling, limits, timeouts, backpressure) | Can't handle real connection churn | P1 | M | partial |

*NW-1/NW-3 substantially landed (server core, CPU — the logic, not the socket): `server.hpp` defines a serializable request/response wire protocol (`MatrixRequest` GET/PUT/QUERY + `MatrixResponse` status+results, host-endian, fail-loud, OOB/huge-alloc-guarded — fuzzed clean under ASan/UBSan) + a stateless `matrix_serve(engine, bytes) → bytes` dispatcher (GET→kv_get, PUT→durable txn commit, QUERY→execute_query; bad request → ERR_BADREQUEST, never crashes). The engine is now request-serveable; serialize→serve→deserialize round-trips equal direct engine calls, and a PUT through the wire is durable (replays into a fresh engine). 17-test suite + oracle green. See spec/plan 2026-06-26-server-core. **Only the literal transport remains (NW-1 socket layer):** the TCP/Unix-socket `accept`/`recv`/`send` loop is a thin adapter that shuttles these byte buffers — sandbox-blocked here (proven: loopback `bind` denied), runs on a non-sandboxed machine. NW-2 (multi-producer/concurrent serving) + NW-4 (client driver) + NW-5 still open.*

## 5. Security

| ID | Gap | Why | Sev | Effort | Local? |
|----|-----|-----|-----|--------|--------|
| SE-1 | No authentication | Anyone who reaches it owns it | P1 | M | partial |
| SE-2 | No authorization / access control | No per-user/role permissions | P1 | L | yes |
| SE-3 | No TLS / encryption in transit | Wire sniffable | P1 | M | partial |
| SE-4 | No encryption at rest | Disk/backup readable if stolen | P2 | M | yes |
| SE-5 | No input validation at trust boundaries | The payload `reinterpret_cast` etc. is safe in-process but not from a wire boundary that doesn't exist yet | P1 | M | yes |
| SE-6 | No audit logging | No record of who did what | P2 | S | yes |

*SE-2 landed (authorization / access control, CPU): `AccessPolicy` (per-principal grants — `allow_column` for QUERY column-level access, `allow_read`/`allow_write` for point GET/PUT, `permissive()` default) + an authorizing `matrix_serve(eng, policy, principal, bytes)` that checks the principal BEFORE any engine call — a denied request returns `ERR_FORBIDDEN` with zero side effects (a reviewer's runtime probe confirmed a denied PUT leaves the WAL at 0 bytes) and no existence leak (authz precedes existence). Principal is supplied by the authenticated caller, never the payload (no spoofing). The 2-arg `matrix_serve` delegates to a permissive policy (NW-1 unchanged). 18-test suite + oracle green. See spec/plan 2026-06-27-access-control. Remaining SE (local): SE-6 audit log, SE-4 encryption-at-rest; SE-1 authn + SE-3 TLS ride the deferred transport.*

*SE-6 landed (audit logging, CPU): `AuditLog` (append-only) + `AuditEntry{principal, kind, status, key, column}` + a `matrix_serve(…, AuditLog&)` overload recording EVERY served request — allowed, **denied** (the forbidden attempt + its principal — the forensic point), and malformed (kind 0). Refactored the serve pipeline into a shared `serve_core` so the existing overloads stay byte-identical (verified by source diff + green regression). 19-test suite + oracle green. See spec/plan 2026-06-27-audit-logging. Remaining SE (local): SE-4 encryption-at-rest; SE-1 authn + SE-3 TLS need the transport.*

## 6. Resource management & failure handling

| ID | Gap | Why | Sev | Effort | Local? |
|----|-----|-----|-----|--------|--------|
| RM-1 | `CUDA_CHECK` aborts the process on any GPU error | A transient GPU error kills the whole DB; needs graceful degradation/retry | P1 | M | partial |
| RM-2 | No memory limits / quotas / admission control | One big query can OOM the box | P1 | M | yes |
| RM-3 | No eviction / spill when VRAM or RAM is full | Working set > capacity → crash, not graceful | P2 | L | partial |
| RM-4 | No graceful shutdown / drain beyond the demo harness | Can't stop cleanly under load | P1 | S | yes |

## 7. Observability & operability

| ID | Gap | Why | Sev | Effort | Local? |
|----|-----|-----|-----|--------|--------|
| OB-1 | No structured logging (just `cout`) | Can't diagnose production issues | P1 | S | yes |
| OB-2 | No metrics/telemetry export (latency, throughput, queue depth, GPU util) | Operators are blind | P1 | M | yes |
| OB-3 | No health checks / readiness probes | Can't run under an orchestrator | P2 | S | partial |
| OB-4 | No runtime config (constants are compile-time `constexpr`) | Can't tune without recompiling | P1 | M | yes |
| OB-5 | No admin/management interface | No way to inspect/operate a running instance | P2 | M | partial |

## 8. Correctness, testing & CI

| ID | Gap | Why | Sev | Effort | Local? |
|----|-----|-----|-----|--------|--------|
| QA-1 | No CI/CD pipeline | Every change verified by hand; CUDA only via manual Colab **[local CI gate landed — `run_tests.sh`]** | P1 | M | partial |
| QA-2 | Thin test suite (a few oracle checks) — no unit/integration coverage | Regressions slip through **[grown to 37 CPU tests; `test_integration.cpp` covers the full stack end-to-end]** | P1 | L | yes |
| QA-3 | No sanitizers (ASan/UBSan/TSan) on the concurrent code | Data races / UB in lock-free + multithread paths undetected | P1 | S | yes |
| QA-4 | No fuzzing / property-based testing | Edge cases unexplored | P2 | M | yes |
| QA-5 | No stress / chaos / failure-injection testing | Behavior under load & failure unknown | P2 | L | partial |
| QA-6 | CUDA path has no automated test — host-syntax probe + manual runs only | GPU regressions only caught by hand | P1 | M | needs GPU CI |

*QA-1 partial (local CI gate landed): `run_tests.sh` — one command that auto-discovers every `test_*.cpp` (skips the nvcc-only `test_migration_gpu.cpp`), compiles each under `-std=c++20 -O2 -Wall -Wextra`, runs it, then builds `main.cpp` and asserts the pipeline oracle (`83886070`); exits 0 only if ALL pass, non-zero (= failure count) otherwise. Portable (plain `-O3`, no `-mcpu`; `CXX` override; clang++→g++ fallback). Codifies the per-increment gate I'd been running by hand into a repeatable build verification. Verified ALL GREEN (34 tests + oracle, exit 0) and non-vacuous (a deliberately-failing throwaway test → `FAIL` + exit 1). This is the LOCAL CI gate; full CI/CD with the CUDA path on GPU runners remains QA-6 (needs GPU CI). The CPU suite is now 34 tests (QA-2 substantially improved from "a few oracle checks").*

## 9. Build, packaging & deployment

| ID | Gap | Why | Sev | Effort | Local? |
|----|-----|-----|-----|--------|--------|
| BP-1 | Build is manual `clang++`/`nvcc` one-liners; cmake not even installed | Not reproducible; no real build system in use | P1 | S | yes |
| BP-2 | No packaging (container/binary/release artifact) | Nothing to deploy | P1 | M | partial |
| BP-3 | No versioning / release process | Can't ship or roll back | P2 | S | yes |
| BP-4 | No multi-platform build matrix (Apple ARM / Linux x86 / CUDA) | "Works on my Mac" only | P1 | M | needs CI |

## 10. Reliability & HA (mostly P3 for the single-box wedge)

| ID | Gap | Why | Sev | Effort | Local? |
|----|-----|-----|-----|--------|--------|
| HA-1 | No replication | No redundancy | P3 | XL | no |
| HA-2 | No failover / HA | Single point of failure | P3 | XL | no |
| HA-3 | No clustering / sharding across nodes | Bounded to one box | P3 | XL | no |
| HA-4 | No consensus (Raft/Paxos) | Needed only if distributed | P3 | XL | no |

*HA-1..4 are deliberately P3: the wedge client runs on one box. Promote to P1 only when we
move up-market to the datacenter tier.*

## 11. Known deferred items (from our own FINDINGS)

| ID | Gap | Sev | Effort | Local? |
|----|-----|-----|--------|--------|
| KD-1 | Cost-model constants uncalibrated (~313 KB derived vs ~4–8 MB practical crossover) | P1 | S | needs GPU |
| KD-2 | Per-batch GPU sync — double-buffer/async to hide it | P2 | M | needs GPU |
| KD-3 | Page-binning runs on host — fold into the dual-trigger batcher | P2 | S | yes |
| KD-4 | Unified-memory path is a stub — implement when hardware is available | P2 | L | needs HW |
| KD-5 | Hyper-Q multi-stream, `cudaHostRegister` pinned DMA | P2 | M | needs GPU |

---

## Critical path — recommended phasing

Each phase produces something demonstrably more "real." Sequenced by dependency and by the
wedge client's needs. Phases are roughly quarter-sized.

**Phase A — "It's actually a database" (correctness + durability core)**
The non-negotiables. Without these it's a benchmark, not a DB.
- DM-1 (fix key collisions — silent data loss) ← *do first, it's a P0 correctness bug*
- DU-1/DU-2/DU-3 (persistence + WAL + recovery)
- DM-5 (a real data-loading path)
- TX-1 (basic atomic multi-key transactions) + TX-3 (the OCC we deferred)
- QA-3 (sanitizers on the concurrent code — cheap, high value, find races now)

**Phase B — "Someone other than us can use it" (access + model)**
- NW-2 (multi-producer ingestion — unblocks multiple clients)
- DM-2/DM-3 (schema + real types)
- DM-4 (a minimal query interface — even a tiny one, not full SQL)
- NW-1/NW-3/NW-4 (network layer + wire protocol + a client)

**Phase C — "Operable & trustworthy" (production hygiene)**
- OB-1/OB-2/OB-4 (logging, metrics, runtime config)
- DU-4/DU-5/DU-6 (checkpoints, fsync levels, backup)
- SE-1/SE-2/SE-3/SE-5 (authn, authz, TLS, boundary validation)
- RM-1/RM-4 (graceful GPU-error handling, clean shutdown)
- BP-1/BP-2/QA-1/QA-2 (real build system, packaging, CI, test suite)

**Phase D — "Faster & richer" (perf + analytics depth)**
- DM-6/DM-7/DM-8 (aggregations, indexes, joins)
- KD-1..KD-5 (the deferred GPU perf work) — needs the GPU calibration loop
- RM-2/RM-3 (quotas, spill)

**Phase E — up-market only (when leaving the single-box wedge)**
- HA-1..HA-4 (replication, failover, clustering, consensus)

---

## How we execute each gap

Same proven loop, one gap (or tight cluster) at a time:
1. **Brainstorm** the gap into a design (the hard decisions surface here).
2. **Spec** → self-review → your approval.
3. **Plan** → bite-sized TDD tasks.
4. **Implement** via subagent-driven development, two-stage review per task.
5. Merge. Update this register (check the item off, note follow-ups).

Locally-buildable items (most of Phases A–C minus networking/CUDA-CI) we can fully build
and verify here. GPU-dependent items (KD-*) batch into Colab runs as before. Infra-dependent
items (network, CI, HA) need a real target environment and are flagged "Local? = no/partial."

---

*Living document. As each gap is designed/built, link its spec + plan and mark status.*

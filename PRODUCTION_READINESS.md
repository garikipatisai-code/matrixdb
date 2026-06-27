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
| DM-2 | No schema/catalog (tables, columns, types) | Can't model real data; everything is one fixed record shape | P0 | L | yes |
| DM-3 | Fixed 64-byte record / 32-byte payload; no variable-length or real types | No strings, no nullable, no real columns | P0 | L | yes |
| DM-4 | No query language — opcodes only (`OP_READ/WRITE/SCAN`) | No way for a client to express a query; the "router" routes opcodes, there's no parser/planner | P0 | XL | yes |
| DM-5 | No data loading path — scan column is synthetic `value[i]=i` | Can't get real data in; no INSERT/bulk-load/import | P0 | M | yes — DM-5 (binary) + DM-5b (CSV) landed |
| DM-6 | Scans return a count only — no GROUP BY / SUM / MIN / MAX / projection / rows | Not real analytical queries | P1 | L | yes |
| DM-7 | No secondary indexes; point access is a single masked slot | Only one access path; no range scans on keys | P2 | L | yes |
| DM-8 | No joins | Multi-table analytics impossible | P2 | XL | yes |
| DM-9 | Dynamic data growth — store/column are fixed pre-allocations | Can't grow past boot-time size; no realloc/spill | P1 | L | partial |

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
| DU-6 | No backup / restore | Ops can't protect data | P1 | M | yes |

*Inc 3 landed: `cold_store.hpp` — synchronous CRC-framed append-only WAL, wired into CPUMockEngine (append-before-commit + replay-on-construct). Committed point-op writes survive restart (DU-1/2/3 fixed). Checkpoint/compaction (DU-4) and cold-column spill (Inc 4) still pending. See spec 2026-06-25-increment-3-cold-store-wal-design.md.*

*DU-4 landed (WAL checkpoint/compaction, CPU): the point-op WAL no longer grows without bound. `CPUMockEngine::checkpoint()` snapshots `kv_` to a `<wal>.ckpt` file (atomic temp→fsync→`rename`) then `ColdStore::truncate()`s the log; recovery now `load_checkpoint` THEN replays only post-checkpoint records — bounding both restart time and disk. **Crash-safe by construction:** `kv_.put` is idempotent (last-writer-wins), the snapshot is durable before the truncate, and new writes append only after `checkpoint()` returns, so a crash at any point recovers checkpoint + surviving-WAL to the same state (no loss, no double-apply). A reviewer empirically attacked every crash window (post-rename/pre-truncate, mid-truncate, stale `.tmp`, malicious `count=2^60`→aborts in 0s, ENOSPC→atomic) and proved non-vacuity (a no-op `save_checkpoint` fails the checkpoint-alone test). Added `KVStore::for_each`, `ColdStore::truncate`, `checkpoints()`/`wal_records()` observability. 21-test suite + oracle green; the 3 recovery-path tests (engine_restart/transactions/cold_store) still pass. See spec/plan 2026-06-27-wal-checkpoint. Deferred: directory-fsync for rename durability (pre-existing layer-wide gap), auto-checkpoint triggers, WAL segment rotation.*

*Inc 4 landed: `tiered_column.hpp` + `migration_executor.hpp` — cross-tier byte movement (HOST/RAM ↔ DEVICE/VRAM ↔ COLD/SSD via HOST), checksum-invariant, driven by TierManager decisions. A VRAM-promoted column is proven GPU-scannable in place. The heat→decision→migration loop is closed on the TieredColumn primitive. Live-engine integration (the OP_SCAN column becoming a managed TieredColumn) is the next step. See spec 2026-06-26-increment-4-migration-executor-design.md.*

*INT-1 landed (integration debt closed): the previously tested-but-dormant TierManager + MigrationExecutor + TieredColumn are now WIRED INTO the live `CPUMockEngine`. OP_SCAN carries a column id (id 0 = legacy fixed column, oracle unchanged); id>0 targets a tiered catalog the engine auto-tiers by access heat — demoting the coldest columns to SSD under a RAM budget so the engine holds a working set larger than RAM, and borrowing a cold column back to RAM for a scan (results correct regardless of tier). Proven non-vacuous (the test fails if rebalancing is disabled). CPU/HOST↔COLD only; DEVICE inert on the CPU build via device_cap=1. See spec/plan 2026-06-26-live-tiering-integration. **Honest scope:** delivers borrow-on-access, NOT heat-driven resident re-promotion under a full budget (TierManager promotion only fills free HOST space — no swap-on-promote yet). That + per-column COLD-file uniqueness + a release-safe unregistered-id guard = INT-1b, the immediate next increment.*

*INT-1b landed (auto-tiering loop COMPLETE): `tier_manager.hpp` gained swap-on-promote — when a re-hot lower-tier column wants a full faster tier, it displaces the lowest-keep_score resident if it's >1.5x more valuable (SWAP_MARGIN) and the victim is past MIN_RESIDENCY_TICKS. So a column scanned heavily again climbs back to RESIDENT RAM (not just borrowed per-scan); proven end-to-end (test_repromotion_under_pressure, non-vacuous via disabling the mechanism). The full RAM↔SSD auto-tiering loop (demote cold + re-promote hot) now runs live in the CPU engine. Hardening: per-process/instance-unique TieredColumn COLD path (kills silent same-id cross-corruption); release-safe guard for scanning an unregistered column id (was a null-deref under NDEBUG). See spec/plan 2026-06-26-repromotion-swap-on-promote. Remaining tiering: DEVICE/VRAM catalog promotion in the CUDA engine (the 24x perf win — migration mechanics proven by Inc 4, needs Colab to run); swap-on-promote multi-victim + COLD-write skip-if-unchanged are deferred perf niceties.*

*AGG-1 landed (DM-6 analytical aggregations, CPU): OP_SCAN generalized from `count(value>threshold)` to a filtered reduction — COUNT/SUM/MIN/MAX (`MatrixAggOp` @payload[16], default AGG_COUNT) via `matrix_cpu_reduce`, over both the legacy fixed column and the tiered catalog (correct even over a COLD-demoted-then-borrowed column). COUNT is byte-identical arithmetic to the old loop, so the oracle (83886070) and the benchmark hot loop are unchanged (measured: no regression). Empty-set sentinels: MIN=UINT64_MAX, MAX=0. Backward compatible (existing scans read op 0=COUNT). See spec/plan 2026-06-26-aggregations. **AGG-2 next:** the GPU reduction kernel — the CUDA `execute_scan` currently honors only AGG_COUNT (flagged loudly in-code); SUM/MIN/MAX on the GPU need a parallel-reduction kernel (Colab). Also deferred: GROUP BY / multi-column / richer predicates (the query layer DM-4); AVG is caller-derivable (SUM/COUNT).*

*GBY-1 landed (GROUP BY, CPU): `matrix_cpu_group_reduce` (dense `[0,num_groups)` key reduction, COUNT/SUM/MIN/MAX, out-of-range keys ignored, empty-group sentinels matching the scalar reducer) + `CPUMockEngine::grouped_aggregate(key_id, value_id, num_groups, op, out)` — per-group aggregate over two aligned tiered columns, double borrow-and-return (both columns pulled to RAM for the reduction, returned to home tier; correct even when both are COLD-demoted). Records heat on both; migration stays scan-driven. Not on ComputeInterface (CPU-engine method, GPU unaffected). Oracle + 10-test suite green. See spec/plan 2026-06-26-group-by.*

*GBY-2 landed (filtered GROUP BY, CPU): `WHERE value > threshold` added to grouped aggregation via a templated `matrix_group_reduce_impl<bool Filtered>` (`if constexpr` compiles the filter out → unfiltered path byte-identical, zero churn to GBY-1) + `matrix_cpu_group_reduce_where` + `CPUMockEngine::grouped_aggregate_where`. Completes the canonical `SELECT key, AGG(value) WHERE value>T GROUP BY key`. Oracle + 10-test suite green. See spec/plan 2026-06-26-filtered-group-by. The 4 analytical entry points (scalar scan/agg, grouped, filtered-grouped) are candidates to unify behind one `MatrixQuery` (DM-4 query layer) next.*

*QRY-1 landed (DM-4 lite, unified query API, CPU): `struct MatrixQuery {value_col, agg, has_filter, threshold, grouped, key_col, num_groups}` + `CPUMockEngine::execute_query(q, out)` — one entry point routing the 4 cases (scalar/grouped × filtered/unfiltered) to the existing primitives. Added `matrix_cpu_reduce_all` (the missing unfiltered scalar reduce) + a defaulted `has_filter=true` param on `scan_tiered_column` (oracle-safe: execute_scan's call + matrix_cpu_reduce unchanged; the 83886070 oracle is the legacy id-0 path, untouched). Catalog columns only (id>0; legacy id-0 is the benchmark fixture). Designated-initializer ergonomics read like SQL. 11-test suite + oracle green. See spec/plan 2026-06-26-query-api. **QRY-2 next:** demonstrate the stack live in `main` (load → auto-tier → execute_query → print) — the capabilities currently run only in tests; the live pipeline still only does point-ops + the legacy scan.*

*QRY-2 landed (live analytical demo in `main`): `analytical_query_demo()` runs in the binary after the existing pipeline — loads 3 catalog columns (12 MB) into an 8 MB RAM budget, runs `SELECT key, SUM(va) WHERE va>500 GROUP BY key` + scalar queries via `execute_query`, drives a hot workload so the idle 3rd column auto-demotes to SSD (prints `col3=COLD` — a 3-column working set in a 2-column RAM budget), then queries the demoted column (pulled back, correct). **Self-verifying:** every result is asserted against an in-function brute-force oracle (a reviewer proved the asserts are real guards by forcing a SIGABRT). Oracle-safe: separate engine + catalog ids>0, runs after the 83886070 assert. The analytical stack (tiering + aggregation + group-by + query API) is now visible AND verified in the running product, not just in tests. See spec/plan 2026-06-26-live-query-demo. The CPU analytical engine arc is complete; remaining axes: GPU batch (Colab), DM-5 data loading from file, OB observability, NW/TX/SE.*

*OB-1 landed (observability, CPU): `struct EngineStats` + `CPUMockEngine::stats()` expose tiering activity (cold_borrows, rebalances, migrations) + resident-bytes gauges (HOST/COLD) + catalog size. Counters incremented additively at all 5 COLD→HOST borrow sites + the rebalance trigger (`migrations_ += executor.apply(...)`), verified by a reviewer to cover every borrow path with no miss/double-count — oracle-safe. The live demo prints them (`cold_borrows=1 rebalances=8 migrations=1 … resident HOST=8MB COLD=4MB`), so the auto-tiering is now observable in the running binary. 12-test suite + oracle green. See spec/plan 2026-06-26-observability. OB-2 (latency histograms / metrics export / leveled logging) deferred.*

*DM-5 landed (binary column persistence, CPU): `column_io.hpp` (`matrix_write_column`/`matrix_read_column` — `[u32 magic][u64 count][count×u32]`, fail-loud abort on open/short/bad-magic, host-endian/same-machine documented) + `CPUMockEngine::save_column` (borrows to HOST, reuses the OB cold-borrow counter) / `load_column_from_file` (→ `load_scan_column`). Closes the data in/out + durability gap: the engine can now ingest a column from disk and persist one (incl. a COLD-tier column). End-to-end lifecycle complete: load → auto-tier → query → observe → save. 13-test suite + oracle green. See spec/plan 2026-06-26-column-persistence. CSV ingest landed as DM-5b (below); typed/float/signed ingest rides DM-3.*

*CKPT-1 landed (catalog snapshot durability, CPU): `CPUMockEngine::save_catalog(path)` / `load_catalog(path)` — single-file snapshot `[u32 magic 'MCAT'][u64 num_cols]{[u64 id][u64 count][count×u32]}` of the whole tiered catalog (borrows COLD columns to HOST to read, returns them), restored into a fresh engine via `load_scan_column` (columns land in HOST; the TierManager re-tiers under load — tiers are a heat cache, not data, so placement need not be persisted). The analytical store now survives a restart, complementing the point-op WAL. Fail-loud, host-endian. 14-test suite + oracle green. See spec/plan 2026-06-26-catalog-snapshot. Crash-atomic (.tmp+rename) snapshot deferred.*

*DM-5b landed (CSV ingest, CPU): `csv_ingest.hpp` (`matrix_read_csv_column(path, col_index, has_header, delim, out)` — line-at-a-time `std::getline` + `std::from_chars`, parses one uint32 column out of a simple unquoted CSV; CRLF-tolerant) + `CPUMockEngine::load_column_from_csv(id, path, col_index, has_header=false, delim=',')` (→ `load_scan_column`). Closes the "can't get REAL data in" half of DM-5 P0: an external integer dataset now ingests straight into the tiered catalog and is queryable. **CSV is untrusted input**, so — unlike the binary reader's `abort()` on corruption of our OWN format — malformed CSV (open fail / short row / non-integer / overflow / trailing junk) returns `false` gracefully with no catalog entry created, never a crash (the VAL-1 lesson). A reviewer proved non-vacuity by building a mutant parser that ignores bad fields — it aborts on the test's failure asserts. 20-test suite + oracle green. See spec/plan 2026-06-27-csv-ingest. Deferred: RFC-4180 quoting, typed/float/signed columns (DM-3), multi-column single-pass, CSV export.*

*VAL-1 landed (query input validation, CPU): `execute_query` now returns `MatrixQueryStatus` (OK / ERR_UNKNOWN_COLUMN / ERR_INVALID_GROUP / ERR_TOO_MANY_GROUPS) and validates at the boundary — unknown/zero column id, self-group (key==value), key/value length mismatch, num_groups==0, and num_groups>MAX_QUERY_GROUPS (1<<28) are all rejected gracefully (out cleared) with NO assert/throw/crash. The `||` short-circuit (catalog_has before `.at()`) makes the error paths release-safe — proven under `-DNDEBUG` (a reviewer wrapped a bad query in try/catch: no throw). The query trust boundary no longer crashes on malformed input. Existing callers (demo, test_query) ignore the new return and still compile. 15-test suite + oracle green. See spec/plan 2026-06-26-query-validation. Validating the admin/persistence entry points is the follow-up.*

*QRY-3 landed (richer scan predicates, CPU): the analytical WHERE clause was `value > threshold` only; it now supports GT/GE/LT/LE/EQ/NE/BETWEEN over catalog columns. `compute.hpp`: `enum class MatrixCmp`, `struct MatrixPredicate{cmp,a,b}`, `matrix_pred_match`, `matrix_cpu_reduce_pred` (the predicate-aware sibling of `matrix_cpu_reduce`); `matrix_group_reduce_impl<Filtered>` generalized to take a predicate (unfiltered path byte-identical via `if constexpr`); `MatrixQuery` gained `cmp` (default GT) + `upper` (BETWEEN's inclusive upper). Threaded only through `execute_query`/`scan_tiered_column`/`grouped_aggregate_pred` — **oracle-safe**: the id-0 benchmark scan still uses the untouched `matrix_cpu_reduce`, and every existing query (cmp defaults to GT) is byte-identical. A reviewer proved the GT-equivalence of the rerouted id>0 path by 96k randomized+boundary checks AND an assembly diff of the unfiltered group loop, and proved non-vacuity by two separate mutations (scalar + grouped dispatch each independently load-bearing). MAX empty-sentinel stays 0 — now ambiguous only for 0-matching predicates (read COUNT to disambiguate; documented). 22-test suite + oracle green; the 5 catalog-path tests pass unmodified. See spec/plan 2026-06-27-richer-predicates. Deferred: key-side predicates, IN-lists, multi-predicate AND/OR, GPU pushdown, float/signed (DM-3).*

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
| QA-1 | No CI/CD pipeline | Every change verified by hand; CUDA only via manual Colab | P1 | M | partial |
| QA-2 | Thin test suite (a few oracle checks) — no unit/integration coverage | Regressions slip through | P1 | L | yes |
| QA-3 | No sanitizers (ASan/UBSan/TSan) on the concurrent code | Data races / UB in lock-free + multithread paths undetected | P1 | S | yes |
| QA-4 | No fuzzing / property-based testing | Edge cases unexplored | P2 | M | yes |
| QA-5 | No stress / chaos / failure-injection testing | Behavior under load & failure unknown | P2 | L | partial |
| QA-6 | CUDA path has no automated test — host-syntax probe + manual runs only | GPU regressions only caught by hand | P1 | M | needs GPU CI |

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

#!/usr/bin/env python3
# ponytail: regenerates matrixdb_colab.ipynb from the real source files so the
# notebook can never drift from the code. Re-run after editing any embedded source.
import json

SOURCES = ["types.hpp", "ring_buffer.hpp", "compute.hpp",
           "memory_model.hpp", "tier_model.hpp", "cost_model.hpp", "router.hpp",
           "kv_store.hpp", "cold_store.hpp", "tier_manager.hpp",
           "tiered_column.hpp", "migration_executor.hpp", "column_io.hpp",
           "csv_ingest.hpp",
           "server.hpp",
           "server_tcp.hpp",
           "client.hpp",
           "concurrent_server.hpp",
           "version.hpp",
           "logging.hpp",
           "compute_mock.cpp", "compute_cuda.cuh", "main.cpp",
           "test_scan_coverage.cpp", "test_cost_model.cpp", "test_kv_store.cpp",
           "test_tier_manager.cpp", "test_cold_store.cpp", "test_engine_restart.cpp",
           "test_migration.cpp", "test_live_tiering.cpp", "test_aggregations.cpp",
           "test_group_by.cpp",
           "test_query.cpp",
           "test_observability.cpp",
           "test_column_io.cpp",
           "test_catalog_snapshot.cpp",
           "test_query_validation.cpp",
           "test_transactions.cpp",
           "test_server.cpp",
           "test_security.cpp",
           "test_audit.cpp",
           "test_csv_ingest.cpp",
           "test_checkpoint.cpp",
           "test_query_predicates.cpp",
           "test_typed_columns.cpp",
           "test_typed_predicates.cpp",
           "test_typed_grouped.cpp",
           "test_typed_snapshot.cpp",
           "test_typed_double.cpp",
           "test_typed_double_grouped.cpp",
           "test_typed_csv.cpp",
           "test_query_latency.cpp",
           "test_backup.cpp",
           "test_schema.cpp",
           "test_append.cpp",
           "test_kv_range.cpp",
           "test_query_parser.cpp",
           "test_join.cpp",
           "test_integration.cpp",
           "test_typed_column_io.cpp",
           "test_kv_index.cpp",
           "test_gather.cpp",
           "test_tables.cpp",
           "test_string_columns.cpp",
           "test_string_dict.cpp",
           "test_multi_agg.cpp",
           "test_projection.cpp",
           "test_nullable.cpp",
           "test_top_groups.cpp",
           "test_having.cpp",
           "test_average.cpp",
           "test_count_distinct.cpp",
           "test_admission_control.cpp",
           "test_rebalance_config.cpp",
           "test_shutdown.cpp",
           "test_durability_levels.cpp",
           "test_fault_injection.cpp",
           "test_health.cpp",
           "test_fuzz.cpp",
           "test_stress.cpp",
           "test_server_tcp.cpp",
           "test_recv_timeout.cpp",
           "test_client.cpp",
           "test_concurrent_serving.cpp",
           "test_version.cpp",
           "test_logging.cpp",
           "test_migration_gpu.cpp",
           "test_gpu_agg.cu",
           "test_gpu_pred.cu",
           "test_gpu_grouped.cu",
           "test_gpu_typed.cu",
           "test_gpu_catalog.cu",
           "test_gpu_catalog_grouped.cu"]

def code(src):
    return {"cell_type": "code", "metadata": {}, "execution_count": None,
            "outputs": [], "source": src}

def md(src):
    return {"cell_type": "markdown", "metadata": {}, "source": src}

cells = [
    md("# MatrixDB on Google Colab\n"
       "\n"
       "GPU-accelerated database engine: page-ownership point ops + a resident-column "
       "`u32x4` scan kernel, behind a lock-free SPSC ring + dual-trigger batcher.\n"
       "\n"
       "**Before running:** Runtime -> Change runtime type -> **T4 GPU**.\n"
       "\n"
       "This notebook writes its own source files (cells below), runs the CPU coverage "
       "test, builds with `nvcc`, and runs. No uploads needed. Success ends with "
       "`Scan result sum: 83886070 (oracle 83886070)` — asserts fire on any mismatch."),
    md("## 1. Confirm a GPU is attached"),
    code("!nvcc --version\n"
         "!nvidia-smi --query-gpu=name,memory.total --format=csv,noheader"),
    md("## 2. Write the source files"),
]

for f in SOURCES:
    with open(f, "r") as fh:
        body = fh.read()
    # %%writefile magic must be the first line of the cell.
    cells.append(code("%%writefile " + f + "\n" + body))

cells += [
    md("## 3. Kernel index-coverage test (CPU, no GPU needed)\n"
       "\n"
       "Simulates the items-per-thread scan kernel's index arithmetic and asserts "
       "every index is visited exactly once. This is pure integer logic, so it runs "
       "on the CPU and catches GPU-only coverage bugs before spending a GPU build."),
    code("!g++ -std=c++17 -O2 test_scan_coverage.cpp -o /tmp/tcov && /tmp/tcov"),
    md("## 3a. KVStore unit test (CPU, no GPU)\n"
       "\n"
       "Proves the DM-1 fix: distinct colliding keys never overwrite each other, and a "
       "full table is an explicit error, not silent data loss."),
    code("!clang++ -std=c++20 -O2 test_kv_store.cpp -o /tmp/tkv 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_kv_store.cpp -o /tmp/tkv; /tmp/tkv"),
    md("## 3c. TierManager unit test (CPU, no GPU)\n"
       "\n"
       "Proves the auto-tiering brain: hot columns promote toward VRAM, cold stay put, "
       "scarce tiers evict by cost-benefit (not pure LRU), anti-thrash holds, decisions "
       "are deterministic."),
    code("!clang++ -std=c++20 -O2 test_tier_manager.cpp -o /tmp/ttm 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_tier_manager.cpp -o /tmp/ttm; /tmp/ttm"),
    md("## 3d. ColdStore WAL test (CPU, no GPU)\n"
       "\n"
       "Proves durability: append+replay round-trip, data survives a fresh ColdStore "
       "instance (restart), torn tail dropped, CRC corruption stops replay."),
    code("!clang++ -std=c++20 -O2 test_cold_store.cpp -o /tmp/tcs 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_cold_store.cpp -o /tmp/tcs; /tmp/tcs"),
    md("## 3e. Engine restart test (CPU, no GPU)\n"
       "\n"
       "End-to-end: point-op writes through one engine survive into a fresh engine on the "
       "same WAL path — MatrixDB no longer loses data on restart."),
    code("!clang++ -std=c++20 -O2 test_engine_restart.cpp -o /tmp/ter 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_engine_restart.cpp -o /tmp/ter; /tmp/ter"),
    md("## 3f. Migration test (CPU, no GPU)\n"
       "\n"
       "Cross-tier byte movement: HOST<->COLD round-trip is checksum-invariant, and "
       "TierManager decisions drive the executor to physically move columns."),
    code("!clang++ -std=c++20 -O2 test_migration.cpp -o /tmp/tmig 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_migration.cpp -o /tmp/tmig; /tmp/tmig"),
    md("### Live tiering integration\n"
       "The engine holds a catalog of analytical columns larger than its RAM budget; the "
       "TierManager demotes the coldest to SSD and a scan pulls a cold column back, results intact."),
    code("!clang++ -std=c++20 -O2 test_live_tiering.cpp -o /tmp/tlt 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_live_tiering.cpp -o /tmp/tlt; /tmp/tlt"),
    md("### Analytical aggregations\n"
       "OP_SCAN computes COUNT / SUM / MIN / MAX over the values matching the predicate, on both "
       "the legacy column and a tiered catalog column — verified against closed-form oracles."),
    code("!clang++ -std=c++20 -O2 test_aggregations.cpp -o /tmp/tagg 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_aggregations.cpp -o /tmp/tagg; /tmp/tagg"),
    md("### Grouped aggregation (GROUP BY)\n"
       "Per-group COUNT/SUM/MIN/MAX over two aligned tiered columns (a key and a value), "
       "verified against hand-worked and brute-force oracles — including over COLD-demoted columns."),
    code("!clang++ -std=c++20 -O2 test_group_by.cpp -o /tmp/tgby 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_group_by.cpp -o /tmp/tgby; /tmp/tgby"),
    md("### Unified query API\n"
       "One `execute_query(MatrixQuery)` composes scalar/grouped × filtered/unfiltered aggregation "
       "over tiered catalog columns — verified against closed-form and brute-force oracles."),
    code("!clang++ -std=c++20 -O2 test_query.cpp -o /tmp/tq 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_query.cpp -o /tmp/tq; /tmp/tq"),
    md("### Engine observability\n"
       "`stats()` exposes tiering activity (cold borrows, rebalances, migrations) + resident-bytes "
       "gauges — verified against a known eviction workload."),
    code("!clang++ -std=c++20 -O2 test_observability.cpp -o /tmp/tobs 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_observability.cpp -o /tmp/tobs; /tmp/tobs"),
    md("### Binary column persistence\n"
       "save_column / load_column_from_file round-trip a column through a binary file (incl. a "
       "COLD-tier column); a reloaded column queries identically."),
    code("!clang++ -std=c++20 -O2 test_column_io.cpp -o /tmp/tcio 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_column_io.cpp -o /tmp/tcio; /tmp/tcio"),
    md("### Catalog snapshot durability\n"
       "save_catalog / load_catalog snapshot the whole tiered catalog to one file and restore it "
       "into a fresh engine (incl. COLD columns) — the analytical store survives a restart."),
    code("!clang++ -std=c++20 -O2 test_catalog_snapshot.cpp -o /tmp/tcs2 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_catalog_snapshot.cpp -o /tmp/tcs2; /tmp/tcs2"),
    md("### Query input validation\n"
       "execute_query rejects malformed queries (unknown/zero column, self-group, length mismatch, "
       "absurd group count) with a status code — gracefully, never crashing (verified under -DNDEBUG too)."),
    code("!clang++ -std=c++20 -O2 test_query_validation.cpp -o /tmp/tqv 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_query_validation.cpp -o /tmp/tqv; /tmp/tqv"),
    md("### Atomic transactions (WAL group commit)\n"
       "begin/txn_put/commit/rollback — a committed transaction is all-or-nothing across a crash "
       "(uncommitted txn-puts are discarded on replay); the auto-commit path is unchanged."),
    code("!clang++ -std=c++20 -O2 test_transactions.cpp -o /tmp/ttx 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_transactions.cpp -o /tmp/ttx; /tmp/ttx"),
    md("### Server core (request/response protocol)\n"
       "A serializable GET/PUT/QUERY request + matrix_serve dispatcher make the engine "
       "request-serveable; serialize->serve->deserialize round-trips equal direct engine calls. "
       "(The TCP accept-loop adapter runs on a non-sandboxed machine.)"),
    code("!clang++ -std=c++20 -O2 test_server.cpp -o /tmp/tsv 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_server.cpp -o /tmp/tsv; /tmp/tsv"),
    md("### Authorization / access control\n"
       "AccessPolicy enforces per-principal column-level query access + read/write grants at the "
       "matrix_serve boundary; unpermitted requests are ERR_FORBIDDEN with no engine side-effects."),
    code("!clang++ -std=c++20 -O2 test_security.cpp -o /tmp/tsec 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_security.cpp -o /tmp/tsec; /tmp/tsec"),
    md("### Audit logging\n"
       "AuditLog records every served request — allowed, denied (with the principal), and "
       "malformed — at the matrix_serve boundary; the forensic trail of who did what."),
    code("!clang++ -std=c++20 -O2 test_audit.cpp -o /tmp/taud 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_audit.cpp -o /tmp/taud; /tmp/taud"),
    md("### CSV ingest\n"
       "load_column_from_csv reads a uint32 column straight out of a CSV file into the tiered "
       "catalog — and reports malformed input gracefully (false, no crash) rather than aborting."),
    code("!clang++ -std=c++20 -O2 test_csv_ingest.cpp -o /tmp/tcsv 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_csv_ingest.cpp -o /tmp/tcsv; /tmp/tcsv"),
    md("### WAL checkpoint / compaction\n"
       "checkpoint() snapshots the point-op store and truncates the write-ahead log, so recovery "
       "loads the snapshot then replays only newer records — WAL size and restart time stay bounded."),
    code("!clang++ -std=c++20 -O2 test_checkpoint.cpp -o /tmp/tckpt 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_checkpoint.cpp -o /tmp/tckpt; /tmp/tckpt"),
    md("### Richer scan predicates\n"
       "execute_query's WHERE clause supports GT / GE / LT / LE / EQ / NE / BETWEEN over catalog "
       "columns (not just value>threshold) — verified per-operator against brute-force oracles."),
    code("!clang++ -std=c++20 -O2 test_query_predicates.cpp -o /tmp/tqp 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_query_predicates.cpp -o /tmp/tqp; /tmp/tqp"),
    md("### Typed columns — int64\n"
       "load_scan_column_i64 registers a signed 64-bit column (values beyond uint32 range, and "
       "negatives); execute_query aggregates it (COUNT/SUM/MIN/MAX) — the first slice of real types."),
    code("!clang++ -std=c++20 -O2 test_typed_columns.cpp -o /tmp/ttyp 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_typed_columns.cpp -o /tmp/ttyp; /tmp/ttyp"),
    md("### int64 filtered aggregation\n"
       "int64 columns now support WHERE GT/GE/LT/LE/EQ/NE/BETWEEN with signed 64-bit bounds "
       "(negatives and values beyond uint32) — verified per-operator against brute-force oracles."),
    code("!clang++ -std=c++20 -O2 test_typed_predicates.cpp -o /tmp/ttp 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_typed_predicates.cpp -o /tmp/ttp; /tmp/ttp"),
    md("### Grouped int64 aggregation\n"
       "GROUP BY a uint32 key over an int64 value column (filtered + unfiltered) — completing int64 "
       "query parity; verified incl. negative-group MAX and mixed-width row-count guards."),
    code("!clang++ -std=c++20 -O2 test_typed_grouped.cpp -o /tmp/ttg 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_typed_grouped.cpp -o /tmp/ttg; /tmp/ttg"),
    md("### Typed catalog snapshot (int64 durability)\n"
       "save_catalog / load_catalog round-trip a mixed uint32 + int64 catalog (types + values), so an "
       "int64 analytical store survives a restart — not just RAM-resident."),
    code("!clang++ -std=c++20 -O2 test_typed_snapshot.cpp -o /tmp/tts 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_typed_snapshot.cpp -o /tmp/tts; /tmp/tts"),
    md("### double (float64) columns\n"
       "load_scan_column_f64 registers a 64-bit float column; execute_query aggregates it "
       "(COUNT/SUM/MIN/MAX, filtered + unfiltered) and it survives a restart — fractional real data."),
    code("!clang++ -std=c++20 -O2 test_typed_double.cpp -o /tmp/ttd 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_typed_double.cpp -o /tmp/ttd; /tmp/ttd"),
    md("### Grouped double aggregation\n"
       "GROUP BY a uint32 key over a double value column (filtered + unfiltered) — completing double "
       "query parity; verified incl. negative-group MAX and mixed-width row-count guards."),
    code("!clang++ -std=c++20 -O2 test_typed_double_grouped.cpp -o /tmp/tdg 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_typed_double_grouped.cpp -o /tmp/tdg; /tmp/tdg"),
    md("### Typed CSV ingest (int64 + double)\n"
       "load_column_from_csv_i64 / _f64 ingest signed-64-bit and floating-point columns straight from "
       "CSV (negatives, values beyond uint32, fractions) — gracefully rejecting malformed input."),
    code("!clang++ -std=c++20 -O2 test_typed_csv.cpp -o /tmp/ttc 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_typed_csv.cpp -o /tmp/ttc; /tmp/ttc"),
    md("### Per-query latency metrics\n"
       "EngineStats now reports query_count / total_query_ns / max_query_ns — execute_query times every "
       "call (OK and error) so query latency (count, mean, max) is observable, the #1 DB ops metric."),
    code("!clang++ -std=c++20 -O2 test_query_latency.cpp -o /tmp/tql 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_query_latency.cpp -o /tmp/tql; /tmp/tql"),
    md("### Backup / restore\n"
       "backup(prefix) snapshots the whole durable state (analytical catalog + point-op store) under one "
       "path prefix; restore(prefix) brings it all back into a fresh engine — a basic ops capability."),
    code("!clang++ -std=c++20 -O2 test_backup.cpp -o /tmp/tbk 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_backup.cpp -o /tmp/tbk; /tmp/tbk"),
    md("### Named columns + catalog introspection\n"
       "name_column / column_id / column_name attach names to columns, and catalog_columns() lists every "
       "column with its id, name, type, row count, and tier — a discoverable schema, not just numeric ids."),
    code("!clang++ -std=c++20 -O2 test_schema.cpp -o /tmp/tsch 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_schema.cpp -o /tmp/tsch; /tmp/tsch"),
    md("### Append / dynamic column growth\n"
       "append_to_column[_i64/_f64] add rows to an existing column (growing it, even across the COLD "
       "tier) — the store is no longer load-once; appended rows are immediately queryable."),
    code("!clang++ -std=c++20 -O2 test_append.cpp -o /tmp/tap 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_append.cpp -o /tmp/tap; /tmp/tap"),
    md("### Key range scan (point-op store)\n"
       "kv_range(lo, hi) returns every (key, value) with lo <= key <= hi — a range access path over the "
       "point-op store, beyond exact-key get. (O(n) scan; a sorted index is the deferred upgrade.)"),
    code("!clang++ -std=c++20 -O2 test_kv_range.cpp -o /tmp/tkr 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_kv_range.cpp -o /tmp/tkr; /tmp/tkr"),
    md("### Text query parser\n"
       "parse_query turns a SQL-ish string (SELECT AGG(col) [WHERE col <op> val [AND val]]) into a "
       "MatrixQuery — resolving column names and placing the bound by column type; malformed input is a "
       "graceful ERR_PARSE, never a crash."),
    code("!clang++ -std=c++20 -O2 test_query_parser.cpp -o /tmp/tqparse 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_query_parser.cpp -o /tmp/tqparse; /tmp/tqparse"),
    md("### Equi-join primitive\n"
       "hash_join(left, right) inner-joins two uint32 key columns into matching (left_row, right_row) "
       "pairs (build-hash + probe) — multi-table correlation; verified against a brute-force oracle."),
    code("!clang++ -std=c++20 -O2 test_join.cpp -o /tmp/tj 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_join.cpp -o /tmp/tj; /tmp/tj"),
    md("### End-to-end integration\n"
       "One realistic flow exercising the features composing: typed CSV ingest -> name -> catalog "
       "introspection -> parse_query -> execute -> append -> equi-join -> backup/restore -> re-query."),
    code("!clang++ -std=c++20 -O2 test_integration.cpp -o /tmp/tint 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_integration.cpp -o /tmp/tint; /tmp/tint"),
    md("### Typed single-column files\n"
       "save_column / load_column_from_file round-trip int64 and double columns (not just uint32) via a "
       "typed single-file format — the int64-abort guard is gone; the element type is carried in the file."),
    code("!clang++ -std=c++20 -O2 test_typed_column_io.cpp -o /tmp/ttci 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_typed_column_io.cpp -o /tmp/ttci; /tmp/ttci"),
    md("### Sorted secondary index\n"
       "kv_range_sorted returns a key range in ascending order via an ordered index (O(log n) locate + "
       "O(result) walk, vs kv_range's O(n) scan); maintained on commit, rebuilt on recovery."),
    code("!clang++ -std=c++20 -O2 test_kv_index.cpp -o /tmp/tki 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_kv_index.cpp -o /tmp/tki; /tmp/tki"),
    md("### Gather / join-then-project\n"
       "gather(col, rows) projects a column's values at given row indices (typed) — composes with "
       "hash_join so a join yields joined data, not just index pairs."),
    code("!clang++ -std=c++20 -O2 test_gather.cpp -o /tmp/tg 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_gather.cpp -o /tmp/tg; /tmp/tg"),
    md("### Named tables\n"
       "create_table groups equal-length columns into a named, validated, introspectable unit "
       "(table_columns / tables) — organizational schema over the named columns."),
    code("!clang++ -std=c++20 -O2 test_tables.cpp -o /tmp/tt 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_tables.cpp -o /tmp/tt; /tmp/tt"),
    md("### String columns\n"
       "load_string_column + string_count_where_eq give the engine variable-length string data it had "
       "no support for — a minimal self-contained store (load, row count, equality-filter count)."),
    code("!clang++ -std=c++20 -O2 test_string_columns.cpp -o /tmp/tsc 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_string_columns.cpp -o /tmp/tsc; /tmp/tsc"),
    md("### Dictionary-encoded string columns\n"
       "load_string_column_dict encodes strings -> u32 codes and registers the codes as an ordinary u32 "
       "catalog column, so a string column becomes first-class: GROUP BY a string dimension, WHERE s=='x', "
       "and COUNT(DISTINCT) all run through the existing engine (and ride tiering/snapshot/GPU for free)."),
    code("!clang++ -std=c++20 -O2 test_string_dict.cpp -o /tmp/tsdict 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_string_dict.cpp -o /tmp/tsdict; /tmp/tsdict"),
    md("### Multi-aggregate SELECT\n"
       "query_multi runs a comma-separated aggregate list in one call (SELECT COUNT(a), SUM(b), MIN(c) "
       "[WHERE ...] [GROUP BY k]) — one result column per aggregate — by splitting the list and delegating "
       "each term to the full parser+executor, so it inherits WHERE (incl. cross-column), GROUP BY, and "
       "per-type handling."),
    code("!clang++ -std=c++20 -O2 test_multi_agg.cpp -o /tmp/tma 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_multi_agg.cpp -o /tmp/tma; /tmp/tma"),
    md("### Projection (row retrieval)\n"
       "project_query returns the matching rows' values, not an aggregate (SELECT col [WHERE fcol op val] "
       "[LIMIT n]) — composing a filter (matching_rows) with gather, and reusing parse_query for the WHERE "
       "predicate (numeric + string-dict / ordered filters). The non-aggregate query shape."),
    code("!clang++ -std=c++20 -O2 test_projection.cpp -o /tmp/tproj 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_projection.cpp -o /tmp/tproj; /tmp/tproj"),
    md("### Nullable columns\n"
       "set_column_nulls marks NULL rows; unfiltered scalar aggregates skip them (SQL NULL semantics) — "
       "COUNT counts non-null, SUM/MIN/MAX ignore nulls. A maskless column is unchanged."),
    code("!clang++ -std=c++20 -O2 test_nullable.cpp -o /tmp/tn 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_nullable.cpp -o /tmp/tn; /tmp/tn"),
    md("### Top-N groups (ORDER BY agg DESC LIMIT k)\n"
       "top_groups runs a grouped query, then returns the k (group, value) pairs with the largest "
       "aggregate, descending — the \"top 10 regions by revenue\" staple. U32/COUNT grouping."),
    code("!clang++ -std=c++20 -O2 test_top_groups.cpp -o /tmp/ttg 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_top_groups.cpp -o /tmp/ttg; /tmp/ttg"),
    md("### HAVING (filter groups by aggregate)\n"
       "having() runs a grouped query and returns the (group, value) pairs whose aggregate satisfies a "
       "comparison (GT/GE/LT/LE/EQ/NE/BETWEEN) — the SQL HAVING clause. Group-id order preserved. U32/COUNT."),
    code("!clang++ -std=c++20 -O2 test_having.cpp -o /tmp/th 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_having.cpp -o /tmp/th; /tmp/th"),
    md("### AVG aggregate\n"
       "average() derives AVG = SUM/COUNT as double(s) from the two existing aggregates, so it inherits "
       "per-type handling (U32/I64/F64) and scalar NULL-skipping for free. Scalar -> one value; grouped -> "
       "one per group; zero-count -> NaN."),
    code("!clang++ -std=c++20 -O2 test_average.cpp -o /tmp/tavg 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_average.cpp -o /tmp/tavg; /tmp/tavg"),
    md("### COUNT(DISTINCT)\n"
       "count_distinct returns the number of distinct non-NULL values in a column — exact (hash set), "
       "typed (U32/I64/F64), null-aware. A HyperLogLog sketch is the upgrade path for huge columns."),
    code("!clang++ -std=c++20 -O2 test_count_distinct.cpp -o /tmp/tcd 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_count_distinct.cpp -o /tmp/tcd; /tmp/tcd"),
    md("### Admission control (RM-2)\n"
       "set_max_query_groups caps a single grouped query's group count — bounding its result memory "
       "(num_groups x 8 bytes) so one runaway GROUP BY can't OOM the box. Over the cap -> ERR_TOO_MANY_GROUPS, "
       "no allocation. Runtime-settable (toward OB-4)."),
    code("!clang++ -std=c++20 -O2 test_admission_control.cpp -o /tmp/tac 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_admission_control.cpp -o /tmp/tac; /tmp/tac"),
    md("### Runtime config (OB-4)\n"
       "set_rebalance_interval tunes the heat-rebalance cadence (run the brain every N tiered scans) at "
       "runtime — no recompile. Smaller N re-tiers more aggressively; larger N relaxes it. With the query "
       "group cap, the compile-time tiering knobs are becoming operator-tunable."),
    code("!clang++ -std=c++20 -O2 test_rebalance_config.cpp -o /tmp/trc 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_rebalance_config.cpp -o /tmp/trc; /tmp/trc"),
    md("### Graceful shutdown (RM-4)\n"
       "shutdown() rolls back any open transaction, then checkpoints the WAL (snapshot + truncate) so a "
       "restart replays an ~empty log — bounded recovery. Committed writes survive; uncommitted are discarded. "
       "Idempotent; no-op without a WAL."),
    code("!clang++ -std=c++20 -O2 test_shutdown.cpp -o /tmp/tsd 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_shutdown.cpp -o /tmp/tsd; /tmp/tsd"),
    md("### Durability levels (DU-5)\n"
       "The engine constructor selects the WAL fsync policy: SYNC_EACH (default) fsyncs each commit so a "
       "committed write survives power loss; SYNC_OFF buffers for throughput (a crash may lose the tail). "
       "durability_level() reports it. Both recover committed writes on a clean restart."),
    code("!clang++ -std=c++20 -O2 test_durability_levels.cpp -o /tmp/tdl 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_durability_levels.cpp -o /tmp/tdl; /tmp/tdl"),
    md("### Fault injection — corrupt-WAL recovery (QA-5)\n"
       "A fresh engine built on a CORRUPT WAL must recover the intact prefix, discard the corrupt tail, and "
       "stay usable — never crash, never apply garbage. A torn tail -> all committed writes recover; an "
       "early flipped byte -> replay stops there (CRC), recovering nothing, engine still usable."),
    code("!clang++ -std=c++20 -O2 test_fault_injection.cpp -o /tmp/tfi 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_fault_injection.cpp -o /tmp/tfi; /tmp/tfi"),
    md("### Health / readiness probe (OB-3)\n"
       "health() returns a ready verdict + the gauges behind it (catalog size, durable flag, pending-WAL "
       "records, resident bytes, dropped writes). ready is false when point-op writes have been dropped "
       "(store full) — a real degradation signal an orchestrator can act on."),
    code("!clang++ -std=c++20 -O2 test_health.cpp -o /tmp/thl 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_health.cpp -o /tmp/thl; /tmp/thl"),
    md("### Fuzz harness (untrusted-input crash-safety)\n"
       "Seeded pseudo-random + mutated inputs hammer the untrusted paths (parse_query, "
       "deserialize_request, CSV) — they must never crash. Run under ASan/UBSan for memory safety."),
    code("!clang++ -std=c++20 -O2 test_fuzz.cpp -o /tmp/tf 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_fuzz.cpp -o /tmp/tf; /tmp/tf"),
    md("### Stress / load test\n"
       "Sustained query load + heavy tiering churn + append + join at scale (50k-row columns in a tight "
       "RAM budget), every result checked against a closed-form oracle — behavior under load."),
    code("!clang++ -std=c++20 -O2 test_stress.cpp -o /tmp/tstr 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_stress.cpp -o /tmp/tstr; /tmp/tstr"),
    md("### TCP transport adapter\n"
       "The length-prefixed wire protocol + matrix_serve_conn are verified over a socketpair (a framed "
       "request served over a real socket == a direct matrix_serve); matrix_serve_tcp is the host-runnable "
       "accept loop (bind is sandbox-blocked, so its loop is compile-verified here)."),
    code("!clang++ -std=c++20 -O2 test_server_tcp.cpp -o /tmp/ttcp 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_server_tcp.cpp -o /tmp/ttcp; /tmp/ttcp"),
    md("### Connection read timeout (NW-5)\n"
       "matrix_set_recv_timeout bounds how long a recv blocks, so a client that connects but never sends "
       "(slowloris) can't hang the single-owner serve loop — recv times out, serve_conn returns false, the "
       "loop drops the stuck connection. matrix_serve_tcp sets it on every accepted connection."),
    code("!clang++ -std=c++20 -O2 test_recv_timeout.cpp -o /tmp/trt 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_recv_timeout.cpp -o /tmp/trt; /tmp/trt"),
    md("### Client driver (NW-4)\n"
       "MatrixClient is the app-side of the wire protocol — wraps a connected socket with typed "
       "get/put/query/health/stats calls (frame -> send -> recv -> deframe), the inverse of "
       "matrix_serve_conn. Driven end-to-end over a socketpair (server in a thread); client results == "
       "direct engine calls. (-pthread)"),
    code("!clang++ -std=c++20 -O2 -pthread test_client.cpp -o /tmp/tcl 2>/dev/null "
         "|| g++ -std=c++20 -O2 -pthread test_client.cpp -o /tmp/tcl; /tmp/tcl"),
    md("### Concurrent serving (single-writer / many-readers)\n"
       "ConcurrentServer takes a shared_mutex shared for reads / exclusive for writes; QUERY runs the "
       "lock-free execute_query_shared fast path (no tier side effects) and escalates to the exclusive "
       "matrix_serve path only when it needs a borrow. Verified under ThreadSanitizer: concurrent readers + "
       "mixed read/write are race-free and oracle-correct. (Built with -pthread; also run under TSan.)"),
    code("!clang++ -std=c++20 -O2 -pthread test_concurrent_serving.cpp -o /tmp/tconc 2>/dev/null "
         "|| g++ -std=c++20 -O2 -pthread test_concurrent_serving.cpp -o /tmp/tconc; /tmp/tconc"),
    md("### Build version (BP-3)\n"
       "version.hpp carries the semver build version; the engine reports it (string + a packed "
       "major<<32|minor<<16|patch numeric form), and STATS exposes the packed version over the wire so a "
       "client can read/compare which build it's talking to."),
    code("!clang++ -std=c++20 -O2 test_version.cpp -o /tmp/tver 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_version.cpp -o /tmp/tver; /tmp/tver"),
    md("### Structured logging (OB-1)\n"
       "A tiny leveled logger (DEBUG<INFO<WARN<ERROR) with a runtime-settable threshold, so operators can "
       "filter/route diagnostics instead of grepping unconditional cout. Default WARN; writes to stderr. The "
       "engine's data-loss (dropped-write) path now logs at ERROR; set_log_level tunes it."),
    code("!clang++ -std=c++20 -O2 test_logging.cpp -o /tmp/tlog 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_logging.cpp -o /tmp/tlog; /tmp/tlog"),
    md("## 4b. Migration GPU proof (needs T4 GPU)\n"
       "\n"
       "A column migrated HOST->VRAM is byte-intact AND GPU-scannable in place: the u32x4 "
       "kernel run over the promoted column's device pointer matches a CPU scan of the same "
       "bytes. Closes the heat->decision->migration->faster-scan loop on real hardware."),
    code("!nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread "
         "-DMATRIX_USE_CUDA test_migration_gpu.cpp -o test_migration_gpu && ./test_migration_gpu"),
    md("## 4c. GPU AGG-2 cross-backend proof (needs T4 GPU)\n"
       "\n"
       "The GPU SUM/MIN/MAX/COUNT reduction kernels must equal `matrix_cpu_reduce` over the SAME "
       "bytes — the correctness anchor for the GPU aggregate phase. Checks a matching predicate and an "
       "empty-match predicate (the empty sentinels: SUM 0, MIN UINT64_MAX, MAX 0). This is the merge "
       "gate for GPU-1: green here means the GPU SUM/MIN/MAX can land on `main`."),
    code("!nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread "
         "-DMATRIX_USE_CUDA test_gpu_agg.cu -o test_gpu_agg && ./test_gpu_agg"),
    md("## 4d. GPU-4 predicate cross-backend proof (needs T4 GPU)\n"
       "\n"
       "The GPU predicate-filtered reductions (GE/LT/LE/EQ/NE/BETWEEN, not just `value > threshold`) must "
       "equal `matrix_cpu_reduce_pred` over the SAME bytes — for every MatrixCmp op x {COUNT,SUM,MIN,MAX} "
       "plus an empty-match case. Merge gate for GPU-4 (GPU WHERE matches the CPU's exactly)."),
    code("!nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread "
         "-DMATRIX_USE_CUDA test_gpu_pred.cu -o test_gpu_pred && ./test_gpu_pred"),
    md("## 4e. GPU-2 grouped cross-backend proof (needs T4 GPU)\n"
       "\n"
       "The GPU per-group reduction (one atomic per row into its dense group slot) must equal "
       "`matrix_cpu_group_reduce` for {COUNT,SUM,MIN,MAX}; the dataset includes out-of-range keys to verify "
       "both backends ignore them. Merge gate for GPU-2 (GROUP BY on the GPU)."),
    code("!nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread "
         "-DMATRIX_USE_CUDA test_gpu_grouped.cu -o test_gpu_grouped && ./test_gpu_grouped"),
    md("## 4f. GPU-5 typed (int64 + double) cross-backend proof (needs T4 GPU)\n"
       "\n"
       "The GPU int64 + double predicate reductions must equal `matrix_cpu_reduce_pred_i64`/`_f64` over the "
       "SAME bytes (incl. negatives, values > 2^32, and fractions), for several predicates x "
       "{COUNT,SUM,MIN,MAX}. int64 uses native signed atomics; double MIN/MAX use a CAS loop on the bit "
       "pattern (no native double atomicMin/Max). Merge gate for GPU-5. (double data is half-integers so "
       "the order-dependent float SUM is bit-exact across the GPU's nondeterministic accumulation order.)"),
    code("!nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread "
         "-DMATRIX_USE_CUDA test_gpu_typed.cu -o test_gpu_typed && ./test_gpu_typed"),
    md("## 4g. GPU-3 VRAM catalog proof (needs T4 GPU)\n"
       "\n"
       "The payoff: the *analytical* `execute_query` path runs on the GPU. A catalog column is pinned to "
       "DEVICE/VRAM (`pin_device`), then `execute_query` reduces it **in place on the GPU** (the verified "
       "`matrix_gpu_reduce_dev_*` kernels) instead of borrowing it down to HOST. Must equal "
       "`matrix_cpu_reduce_*` over the same bytes for u32/i64/f64 x {COUNT,SUM,MIN,MAX} x {filtered,unfiltered}. "
       "Merge gate for GPU-3 — the live-engine version of the 24x scan thesis."),
    code("!nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread "
         "-DMATRIX_USE_CUDA test_gpu_catalog.cu -o test_gpu_catalog && ./test_gpu_catalog"),
    md("## 4h. GPU-3g grouped VRAM proof (needs T4 GPU)\n"
       "\n"
       "Completes the analytical-on-GPU surface: a `GROUP BY` over DEVICE/VRAM-resident key+value columns "
       "runs **in place on the GPU** (the `matrix_group_*_kernel[_pred]` kernels) instead of borrowing both "
       "down to HOST. Must equal `matrix_cpu_group_reduce(_pred)` per group for {COUNT,SUM,MIN,MAX} x "
       "{filtered,unfiltered}. Merge gate for grouped-on-device (the filtered grouped kernel is new this "
       "round — a predicate-gated variant of the 4e-verified unfiltered kernel)."),
    code("!nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread "
         "-DMATRIX_USE_CUDA test_gpu_catalog_grouped.cu -o test_gpu_catalog_grouped && ./test_gpu_catalog_grouped"),
    md("## 3b. Cost-model unit test (CPU, no GPU)\n"
       "\n"
       "Pure-function check of the router's placement decisions — point ops -> HOST, "
       "small scans -> HOST, large scans -> DEVICE, monotonic crossover."),
    code("!clang++ -std=c++20 -O2 test_cost_model.cpp -o /tmp/tcm 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_cost_model.cpp -o /tmp/tcm; /tmp/tcm"),
    md("## 4. Build & run on the GPU\n"
       "\n"
       "`-x cu` compiles `main.cpp` as CUDA so the `.cuh` kernels link in. "
       "`-D_GNU_SOURCE` exposes Linux thread-affinity APIs; "
       "`-Xcompiler -pthread` links std::thread."),
    code("!nvcc -std=c++17 -O3 -x cu -D_GNU_SOURCE -Xcompiler -pthread "
         "-DMATRIX_USE_CUDA main.cpp -o matrixdb_proto"),
    code("!./matrixdb_proto"),
    md("## 5. CPU fallback (run this if no GPU runtime is selected)\n"
       "\n"
       "Same code, same asserts, no CUDA — proves the logic without a GPU."),
    code("!g++ -std=c++17 -O3 -D_GNU_SOURCE -pthread main.cpp -o matrixdb_cpu "
         "&& ./matrixdb_cpu"),
]

nb = {
    "nbformat": 4,
    "nbformat_minor": 5,
    "metadata": {
        "accelerator": "GPU",
        "colab": {"provenance": [], "gpuType": "T4"},
        "kernelspec": {"name": "python3", "display_name": "Python 3"},
        "language_info": {"name": "python"},
    },
    "cells": cells,
}

with open("matrixdb_colab.ipynb", "w") as fh:
    json.dump(nb, fh, indent=1)
print("wrote matrixdb_colab.ipynb:", len(cells), "cells,", len(SOURCES), "source files embedded")

#include "compute.hpp"
#include "kv_store.hpp"
#include "cold_store.hpp"
#include "migration_executor.hpp"  // MigrationExecutor + TierManager + TieredColumn + CostModel
#include "memory_model.hpp"        // MemorySpace, MemoryModel
#include "column_io.hpp"           // matrix_write_column / matrix_read_column (binary column persistence)
#include "csv_ingest.hpp"          // matrix_read_csv_column (CSV column ingest, graceful on bad input)
#include "version.hpp"             // MATRIXDB_VERSION (BP-3)
#include "logging.hpp"             // Log / LogLevel (OB-1 structured logging)
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cctype>
#include <chrono>
#include <iostream>

/**
 * @brief CPU Mock Engine — the no-GPU fallback (Component 5: Local Sandbox).
 * Page-ownership model: bins the batch by owning page, then processes each page's
 * queries against its own slice of the store. One owner per page ⇒ no atomics, no
 * delta log, deterministic last-writer-wins. The point-op store is a real KVStore
 * (DM-1 fix: distinct colliding keys never overwrite). The CUDA engine's point-op
 * store is still the flat key&MASK array (replaced in a later increment), so CPU/GPU
 * store parity holds only while keys don't collide; CPU is the DM-1-correct path.
 */
// Engine observability snapshot: tiering activity counters + current resident-bytes gauges.
struct EngineStats {
    uint64_t cold_borrows;        // COLD->HOST borrows performed for a scan/aggregate
    uint64_t rebalances;          // rebalance() passes run (scan-driven)
    uint64_t migrations;          // migration decisions actually executed
    size_t   catalog_columns;     // # columns in the tiered catalog
    size_t   host_resident_bytes; // catalog bytes currently in RAM (TierManager's view)
    size_t   cold_resident_bytes; // catalog bytes currently on SSD
    uint64_t query_count;         // execute_query calls served (OK and ERR)
    uint64_t total_query_ns;      // summed execute_query wall-time (ns)
    uint64_t max_query_ns;        // slowest single execute_query (ns)
};

// One catalog column's metadata (DM-2 introspection): id, optional name (""), element type, row count, tier.
struct ColumnInfo { uint64_t id; std::string name; MatrixType type; size_t rows; MemorySpace tier; };

// OB-3 health/readiness snapshot for an orchestrator probe: a ready verdict + the gauges that justify it.
// `ready` is false when the engine is degraded (point-op writes dropped — the KVStore filled), so a
// liveness/readiness probe can pull the instance out of rotation and page someone.
struct HealthStatus {
    bool     ready;                 // false if degraded (dropped_writes > 0)
    bool     durable;               // a WAL is attached (committed point-ops survive restart)
    size_t   catalog_columns;       // analytical columns registered
    size_t   host_resident_bytes;   // catalog bytes currently in RAM
    uint64_t wal_records_pending;   // un-checkpointed WAL records (a restart-recovery-cost proxy)
    uint64_t dropped_writes;        // point-op writes lost to a full KVStore (the degradation signal)
};

// Tokenize a query string: identifiers/keywords/numbers, the comparison operators (> >= < <= = !=),
// and parens. Whitespace-separated; operators and parens need no surrounding spaces. (free helper)
inline std::vector<std::string> matrixparse_tokenize(const std::string& s) {
    std::vector<std::string> t;
    const size_t n = s.size();
    for (size_t i = 0; i < n; ) {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (c == '(' || c == ')') { t.emplace_back(1, c); ++i; continue; }
        if (c == '>' || c == '<' || c == '=' || c == '!') {
            if (i + 1 < n && s[i + 1] == '=') { t.push_back(s.substr(i, 2)); i += 2; }
            else { t.emplace_back(1, c); ++i; }
            continue;
        }
        size_t j = i;   // a run up to the next space / paren / operator char (covers names and signed numbers)
        while (j < n && !std::isspace(static_cast<unsigned char>(s[j])) && s[j] != '(' && s[j] != ')'
               && s[j] != '>' && s[j] != '<' && s[j] != '=' && s[j] != '!') ++j;
        t.push_back(s.substr(i, j - i));
        i = j;
    }
    return t;
}

#if defined(MATRIX_USE_CUDA)
// GPU-3: defined in compute_cuda.cuh (included after this file in the nvcc TU). Reduce a DEVICE/VRAM-
// resident column's bytes with the cross-checked GPU kernels; the return matches the CPU reducers'
// encoding so execute_query's DEVICE path is bit-identical to its CPU path.
inline uint64_t matrix_gpu_reduce_dev_u32(const void* d, size_t n, MatrixPredicate pred, MatrixAggOp op, bool has_filter);
inline int64_t  matrix_gpu_reduce_dev_i64(const void* d, size_t n, MatrixPredicateI64 pred, MatrixAggOp op, bool has_filter);
inline double   matrix_gpu_reduce_dev_f64(const void* d, size_t n, MatrixPredicateF64 pred, MatrixAggOp op, bool has_filter);
inline void     matrix_gpu_group_reduce_dev(const void* keys, const void* vals, size_t n, uint32_t num_groups, MatrixAggOp op, MatrixPredicate pred, bool has_filter, uint64_t* out_host);
inline void     matrix_gpu_group_reduce_dev_i64(const void* keys, const void* vals, size_t n, uint32_t num_groups, MatrixAggOp op, MatrixPredicateI64 pred, bool has_filter, uint64_t* out_host);
inline void     matrix_gpu_group_reduce_dev_f64(const void* keys, const void* vals, size_t n, uint32_t num_groups, MatrixAggOp op, MatrixPredicateF64 pred, bool has_filter, uint64_t* out_host);
#endif

class CPUMockEngine : public ComputeInterface {
public:
    // host_cap is the RAM budget (bytes) for the tiered catalog; default unbounded so the
    // existing pipeline (empty catalog) is unaffected. device_cap=1 makes the DEVICE tier
    // inert on the CPU build: scan_us() ignores gpu_available, so the brain would otherwise
    // emit HOST->DEVICE promotions the CPU executor's migrate_to(DEVICE) aborts on; a 1-byte
    // cap means no real column ever fits, so no DEVICE decision is emitted (cap==0 == unbounded).
    explicit CPUMockEngine(size_t /*worker_count*/ = 0, std::string wal_path = "",
                           size_t host_cap = SIZE_MAX, SyncPolicy sync = SyncPolicy::SYNC_EACH)
#if defined(MATRIX_USE_CUDA)
        // GPU-3: a real device budget + gpu=true cost model makes the DEVICE tier live, so hot analytical
        // columns promote to VRAM and scan_tiered_column* reduces them in place on the GPU (1 GiB << T4's 16 GB).
        : tier_mgr_(CostModel(MemoryModel::detect(true), true), /*device_cap=*/(size_t)1 << 30, host_cap)
#else
        : tier_mgr_(CostModel(MemoryModel::detect(false), false), /*device_cap=*/1, host_cap)
#endif
        , binned_(MATRIX_BATCH_MAX)
        , scan_column_(MATRIX_SCAN_COLUMN_SIZE) {
        for (size_t i = 0; i < MATRIX_SCAN_COLUMN_SIZE; ++i)
            scan_column_[i] = static_cast<uint32_t>(i); // resident analytical column
        // Durability is opt-in: with a WAL path, recover the point-op store by replaying
        // the log into kv_ (a write committed before a crash is restored here). DU-5: `sync` picks the
        // durability level — SYNC_EACH (default) fsyncs each append so a committed write survives power
        // loss; SYNC_OFF trades that for throughput (a crash may lose the unflushed tail).
        if (!wal_path.empty()) {
            checkpoint_path_ = wal_path + ".ckpt";
            load_checkpoint(checkpoint_path_);                                    // restore the last compaction (no-op if none)
            cold_store_ = std::make_unique<ColdStore>(wal_path, sync);
            cold_store_->replay([this](uint64_t k, uint64_t v){ kv_.put(k, v); }); // post-checkpoint records on top
            kv_.for_each([this](uint64_t k, uint64_t v){ key_index_[k] = v; });     // rebuild the ordered index from recovered state
        }
        std::cerr << "CPUMockEngine initialized (page-ownership, "
                  << MATRIX_PAGE_COUNT << " pages, "
                  << MATRIX_SCAN_COLUMN_SIZE << "-value scan column"
                  << (cold_store_ ? ", WAL durability ON" : "") << ")." << std::endl;
    }

    // RM-2 admission control: cap the groups a single grouped query may allocate (the out vector is
    // num_groups * 8 bytes, so this bounds one query's result memory). Default is MAX_QUERY_GROUPS (2^28,
    // ~2 GB); an operator can tighten it so one runaway GROUP BY can't OOM the box. A query above the cap
    // returns ERR_TOO_MANY_GROUPS (no allocation). Runtime-settable (a step toward OB-4 runtime config).
    void set_max_query_groups(uint32_t n) { max_query_groups_ = n; }
    uint32_t max_query_groups() const { return max_query_groups_; }

    // OB-4 runtime config: tune the heat-rebalance cadence (run the brain + executor every N tiered scans)
    // without recompiling. Default REBALANCE_EVERY (4). A smaller N re-tiers more aggressively (more
    // responsive, more migration work); a large N relaxes it. Clamped to ≥ 1 (0 → 1, rebalance every scan).
    void set_rebalance_interval(uint64_t n) { rebalance_every_ = n ? n : 1; }
    uint64_t rebalance_interval() const { return rebalance_every_; }

    // BP-3: the build version this instance is running (semver string + packed numeric form for the wire).
    const char* version() const { return matrixdb_version(); }
    uint64_t version_u64() const { return matrixdb_version_u64(); }

    // OB-1/OB-4: set the diagnostic log threshold at runtime (DEBUG<INFO<WARN<ERROR; default WARN). Global
    // (the logger is process-wide); exposed here for API discoverability alongside the other tuning knobs.
    void set_log_level(LogLevel l) { Log::set_level(l); }
    LogLevel log_level() const { return Log::get_level(); }

    // Register a uint32 analytical column into the tiered catalog (born resident in HOST).
    // id must be > 0 (0 is reserved for the legacy fixed scan column).
    void load_scan_column(uint64_t id, const uint32_t* data, size_t n) {
        assert(id != 0 && "column id 0 is reserved for the legacy fixed scan column");
        // Register once: re-registering an id would desync the catalog from the brain (and
        // could orphan a demoted column's COLD file). Callers assign distinct ids.
        assert(catalog_.find(id) == catalog_.end() && "column id already registered");
        const size_t bytes = n * sizeof(uint32_t);
        catalog_[id] = std::make_unique<TieredColumn>(
            id, reinterpret_cast<const unsigned char*>(data), bytes);
        tier_mgr_.register_column(id, bytes, MemorySpace::HOST);
    }

    // Register a signed int64 analytical column (born HOST-resident, like load_scan_column). DM-3a.
    void load_scan_column_i64(uint64_t id, const int64_t* data, size_t n) {
        assert(id != 0 && "column id 0 is reserved for the legacy fixed scan column");
        assert(catalog_.find(id) == catalog_.end() && "column id already registered");
        const size_t bytes = n * sizeof(int64_t);
        catalog_[id] = std::make_unique<TieredColumn>(id, reinterpret_cast<const unsigned char*>(data), bytes);
        tier_mgr_.register_column(id, bytes, MemorySpace::HOST);
        col_types_[id] = MatrixType::I64;
    }

    // Register a double (float64) analytical column (born HOST-resident, like load_scan_column). DM-3e.
    void load_scan_column_f64(uint64_t id, const double* data, size_t n) {
        assert(id != 0 && "column id 0 is reserved for the legacy fixed scan column");
        assert(catalog_.find(id) == catalog_.end() && "column id already registered");
        const size_t bytes = n * sizeof(double);
        catalog_[id] = std::make_unique<TieredColumn>(id, reinterpret_cast<const unsigned char*>(data), bytes);
        tier_mgr_.register_column(id, bytes, MemorySpace::HOST);
        col_types_[id] = MatrixType::F64;
    }

    // --- Minimal variable-length string columns (DM-3i) ---
    // A SELF-CONTAINED store, separate from the byte catalog_ (whose columns are fixed-width TieredColumns).
    // Supports load + row count + equality-filter count + element access — the meaningful string ops
    // (SUM/MIN/MAX don't apply numerically). ponytail: a plain id->vector<string> map — not tiered, and
    // NOT in catalog_columns()/execute_query (those stay fixed-width-typed); full integration needs the
    // catalog generalized beyond TieredColumn (XL — the upgrade path).
    void load_string_column(uint64_t id, const std::vector<std::string>& data) {
        assert(id != 0 && "column id 0 is reserved");
        string_columns_[id] = data;
    }
    size_t string_column_rows(uint64_t id) const {
        auto it = string_columns_.find(id);
        return it == string_columns_.end() ? 0 : it->second.size();
    }
    // COUNT of rows whose string equals `value` (a string WHERE col = 'literal' count).
    uint64_t string_count_where_eq(uint64_t id, const std::string& value) const {
        auto it = string_columns_.find(id);
        if (it == string_columns_.end()) return 0;
        uint64_t c = 0;
        for (const std::string& s : it->second) if (s == value) ++c;
        return c;
    }
    std::string string_column_at(uint64_t id, size_t row) const {
        auto it = string_columns_.find(id);
        assert(it != string_columns_.end() && row < it->second.size() && "string_column_at: bad id/row");
        return it->second[row];
    }

    // --- Dictionary-encoded string columns: strings as a first-class queryable type ---
    // load_string_column_dict encodes the strings to u32 codes (distinct value -> code in SORTED /
    // lexicographic order, so a code's order == its string's order) and registers the code vector as an
    // ordinary u32 catalog column (via load_scan_column) — so a string column instantly inherits the whole
    // analytical engine: tiering, snapshot, scalar+grouped aggregation, and the GPU path. The id->string
    // dictionary (code -> string, sorted) stays here for decode + range translation. "Top categories":
    // GROUP BY the code column; "WHERE s == 'x'": EQ string_encode(id,"x"); "WHERE s > 'x'": the parser maps
    // the literal to a code rank via the sorted dict; COUNT(DISTINCT) on the codes == string_dict_size.
    void load_string_column_dict(uint64_t id, const std::vector<std::string>& data) {
        assert(id != 0 && "column id 0 is reserved");
        std::vector<std::string> dict(data.begin(), data.end());   // distinct strings in sorted order ...
        std::sort(dict.begin(), dict.end());
        dict.erase(std::unique(dict.begin(), dict.end()), dict.end());
        std::unordered_map<std::string, uint32_t> enc;             // ... so code == lexicographic rank
        enc.reserve(dict.size());
        for (uint32_t c = 0; c < dict.size(); ++c) enc.emplace(dict[c], c);
        std::vector<uint32_t> codes; codes.reserve(data.size());
        for (const std::string& s : data) codes.push_back(enc[s]);
        load_scan_column(id, codes.data(), codes.size());   // codes become a first-class u32 catalog column
        string_dicts_[id] = std::move(dict);
        string_encoders_[id] = std::move(enc);
    }
    // Distinct-string count of a dict-encoded column (== its GROUP BY group count, == COUNT(DISTINCT)).
    uint32_t string_dict_size(uint64_t id) const {
        auto it = string_dicts_.find(id);
        return it == string_dicts_.end() ? 0u : static_cast<uint32_t>(it->second.size());
    }
    // Decode a code back to its string (empty if id isn't dict-encoded or code is out of range).
    std::string string_decode(uint64_t id, uint32_t code) const {
        auto it = string_dicts_.find(id);
        if (it == string_dicts_.end() || code >= it->second.size()) return std::string{};
        return it->second[code];
    }
    // Encode a literal to its code for building a filter (WHERE s == value). Returns string_dict_size(id) —
    // a code no row holds — when the value is absent, so an EQ filter on it correctly matches nothing.
    uint32_t string_encode(uint64_t id, const std::string& value) const {
        auto it = string_encoders_.find(id);
        if (it == string_encoders_.end()) return 0u;
        auto e = it->second.find(value);
        return e == it->second.end() ? static_cast<uint32_t>(it->second.size()) : e->second;
    }

    // --- Nullable columns (DM-3j) ---
    // Mark which rows of a U32 catalog column are NULL (1=null), so scalar aggregates skip them (SQL NULL
    // semantics). is_null must have one byte per row. ponytail: U32 unfiltered scalar only for this slice —
    // int64/double/filtered/grouped null-awareness is the follow-up (the maskless path is byte-identical).
    void set_column_nulls(uint64_t id, const std::vector<uint8_t>& is_null) {
        assert(catalog_has(id) && "set_column_nulls: unknown catalog column");   // any byte-catalog column (U32/I64/F64)
        assert(is_null.size() == column_rows(id) && "null mask must have one entry per row");
        null_masks_[id] = is_null;
    }

    // Append `n` more rows to an existing catalog column, growing it (DM-9). The store is no longer
    // load-once. Asserts the column exists and the element type matches; works across the COLD tier
    // (append_raw borrows COLD->HOST, grows, returns the borrow). Appended rows are immediately queryable.
    void append_to_column(uint64_t id, const uint32_t* data, size_t n) {
        assert(catalog_has(id) && column_type(id) == MatrixType::U32 && "append type must match column (u32)");
        append_raw(id, reinterpret_cast<const unsigned char*>(data), n * sizeof(uint32_t));
    }
    void append_to_column_i64(uint64_t id, const int64_t* data, size_t n) {
        assert(catalog_has(id) && column_type(id) == MatrixType::I64 && "append type must match column (int64)");
        append_raw(id, reinterpret_cast<const unsigned char*>(data), n * sizeof(int64_t));
    }
    void append_to_column_f64(uint64_t id, const double* data, size_t n) {
        assert(catalog_has(id) && column_type(id) == MatrixType::F64 && "append type must match column (double)");
        append_raw(id, reinterpret_cast<const unsigned char*>(data), n * sizeof(double));
    }

    // Inspection (tests): where the bytes actually live vs where the brain believes, the
    // HOST bytes the brain is accounting for, and a column's integrity checksum.
    MemorySpace column_tier(uint64_t id) const { return catalog_.at(id)->tier(); }
    MemorySpace manager_tier(uint64_t id) const { return tier_mgr_.tier_of(id); }
    size_t host_resident_bytes() const { return tier_mgr_.resident_bytes(MemorySpace::HOST); }
    uint64_t column_checksum(uint64_t id) const { return catalog_.at(id)->checksum(); }
    // A column's element type (DM-3a). Absent from col_types_ ⇒ U32 (every legacy/untagged column).
    MatrixType column_type(uint64_t id) const { auto it = col_types_.find(id); return it == col_types_.end() ? MatrixType::U32 : it->second; }

    // Type-aware row count of a catalog column (U32 = 4 bytes/row, I64 and F64 = 8).
    size_t column_rows(uint64_t id) const {
        const size_t w = (column_type(id) == MatrixType::I64 || column_type(id) == MatrixType::F64) ? 8 : sizeof(uint32_t);
        return catalog_.at(id)->size_bytes() / w;
    }

    // Attach (or overwrite) a name for an existing catalog column. Duplicate names: last wins for column_id.
#if defined(MATRIX_USE_CUDA)
    // GPU-3: pin a catalog column to DEVICE/VRAM now (deterministic promotion for tests/ops; the heat
    // brain also promotes hot columns under the device budget). Returns false if the id is unknown.
    bool pin_device(uint64_t col_id) {
        auto it = catalog_.find(col_id);
        if (it == catalog_.end()) return false;
        it->second->migrate_to(MemorySpace::DEVICE);
        return true;
    }
#endif

    void name_column(uint64_t id, const std::string& name) {
        assert(catalog_has(id) && "name_column: unknown column id");
        column_names_[id] = name;
        name_to_id_[name] = id;
    }
    // Resolve a column name to its id; 0 (the reserved legacy id) if no such name.
    uint64_t column_id(const std::string& name) const {
        auto it = name_to_id_.find(name);
        return it == name_to_id_.end() ? 0 : it->second;
    }
    // A column's name, or "" if unnamed.
    std::string column_name(uint64_t id) const {
        auto it = column_names_.find(id);
        return it == column_names_.end() ? std::string{} : it->second;
    }
    // List every catalog column with its metadata (id, name, type, row count, tier). Order unspecified.
    std::vector<ColumnInfo> catalog_columns() const {
        std::vector<ColumnInfo> out;
        out.reserve(catalog_.size());
        for (const auto& kv : catalog_)
            out.push_back(ColumnInfo{ kv.first, column_name(kv.first), column_type(kv.first),
                                      column_rows(kv.first), kv.second->tier() });
        return out;
    }

    // Group existing, equal-length columns into a named table (a row-aligned unit) — DM-2c. Returns false
    // (no table created) if a column id is unknown or the columns differ in row count (the table invariant).
    // Organizational schema over the named columns; queries still run per-column (a table-scoped query
    // planner is the upgrade). ponytail: stores column ids; a dropped column would dangle (no drop exists).
    bool create_table(const std::string& name, const std::vector<uint64_t>& col_ids) {
        if (col_ids.empty()) return false;
        size_t rows = 0; bool first = true;
        for (uint64_t id : col_ids) {
            if (!catalog_has(id)) return false;
            const size_t r = column_rows(id);
            if (first) { rows = r; first = false; } else if (r != rows) return false;   // row-aligned invariant
        }
        tables_[name] = col_ids;
        return true;
    }
    // The columns of a named table (in declared order), as ColumnInfo; empty if no such table.
    std::vector<ColumnInfo> table_columns(const std::string& name) const {
        std::vector<ColumnInfo> out;
        auto it = tables_.find(name);
        if (it == tables_.end()) return out;
        for (uint64_t id : it->second)
            if (catalog_has(id))
                out.push_back(ColumnInfo{ id, column_name(id), column_type(id), column_rows(id), catalog_.at(id)->tier() });
        return out;
    }
    // Names of all registered tables (order unspecified).
    std::vector<std::string> tables() const {
        std::vector<std::string> out; out.reserve(tables_.size());
        for (const auto& kv : tables_) out.push_back(kv.first);
        return out;
    }

    // Point-op read accessor (tests): true + sets out if present. Mirrors execute_batch's OP_READ.
    bool kv_get(uint64_t key, uint64_t& out) const { return kv_.get(key, out); }

    // Range scan over the point-op store: every (key, value) with lo <= key <= hi (inclusive). Order
    // unspecified (hash order — sort if needed). ponytail: O(capacity) full scan via KVStore::for_each;
    // a sorted secondary index for log-time selective ranges is the deferred upgrade (the "index" half
    // of DM-7).
    std::vector<std::pair<uint64_t, uint64_t>> kv_range(uint64_t lo, uint64_t hi) const {
        std::vector<std::pair<uint64_t, uint64_t>> out;
        kv_.for_each([&](uint64_t k, uint64_t v) { if (k >= lo && k <= hi) out.emplace_back(k, v); });
        return out;
    }

    // Sorted secondary index range scan (DM-7): every (key, value) with lo <= key <= hi, in ASCENDING
    // key order, via the ordered index — O(log n) to locate `lo` + O(result) to walk the range (not the
    // O(n) full scan of kv_range). The result is sorted by key (kv_range's is unordered).
    std::vector<std::pair<uint64_t, uint64_t>> kv_range_sorted(uint64_t lo, uint64_t hi) const {
        std::vector<std::pair<uint64_t, uint64_t>> out;
        for (auto it = key_index_.lower_bound(lo); it != key_index_.end() && it->first <= hi; ++it)
            out.emplace_back(it->first, it->second);
        return out;
    }

    // --- Atomic transactions (WAL group commit) ---
    void begin() { assert(!in_txn_ && "transaction already open"); txn_buf_.clear(); in_txn_ = true; }
    void txn_put(uint64_t key, uint64_t value) { assert(in_txn_ && "txn_put outside a transaction"); txn_buf_.emplace_back(key, value); }
    // Durably commit the buffered writes as one all-or-nothing group, then apply them.
    void commit() {
        assert(in_txn_ && "commit without begin");
        if (cold_store_) {
            for (auto& kv : txn_buf_) cold_store_->append_txn_put(kv.first, kv.second);
            cold_store_->append_commit();   // the durable, atomic commit point
        }
        for (auto& kv : txn_buf_) apply_committed_write(kv.first, kv.second);
        in_txn_ = false; ++transactions_committed_; txn_buf_.clear();
    }
    void rollback() { assert(in_txn_ && "rollback without begin"); in_txn_ = false; ++transactions_rolled_back_; txn_buf_.clear(); }
    uint64_t transactions_committed() const { return transactions_committed_; }
    uint64_t transactions_rolled_back() const { return transactions_rolled_back_; }

    // Compact the WAL: snapshot the point-op store, then truncate the log (DU-4). No-op if durability off.
    void checkpoint() {
        assert(!in_txn_ && "checkpoint inside a transaction");
        if (!cold_store_) return;
        save_checkpoint(checkpoint_path_);
        cold_store_->truncate();
        ++checkpoints_;
    }

    uint64_t checkpoints() const { return checkpoints_; }
    uint64_t wal_records() const { return cold_store_ ? cold_store_->records_written() : 0; }
    // DU-5: the durability level in force. SYNC_EACH = a committed write is fsync'd (survives power loss);
    // SYNC_OFF = buffered (faster, a crash may lose the tail). SYNC_OFF when no WAL is attached (nothing to sync).
    SyncPolicy durability_level() const { return cold_store_ ? cold_store_->policy() : SyncPolicy::SYNC_OFF; }

    // RM-4 graceful shutdown: stop cleanly and bound restart-recovery time. Rolls back any open
    // (uncommitted) transaction — its writes are correctly discarded on a clean stop — then, if a WAL is
    // attached, checkpoints (snapshot kv_ + truncate the log) so a restart replays an ~empty WAL.
    // Idempotent and safe to call before destruction; a no-op without a WAL.
    void shutdown() {
        if (in_txn_) rollback();
        if (cold_store_) checkpoint();
    }

    // Observability snapshot (counters since construction + current resident-bytes gauges).
    EngineStats stats() const {
        return EngineStats{ cold_borrows_, rebalances_, migrations_, catalog_.size(),
                            tier_mgr_.resident_bytes(MemorySpace::HOST),
                            tier_mgr_.resident_bytes(MemorySpace::COLD),
                            query_count_.load(), total_query_ns_.load(), max_query_ns_.load() };
    }

    // OB-3 health/readiness probe: a ready verdict + the gauges behind it. `ready` is false when any
    // point-op write has been dropped (KVStore full = data loss in progress) — a real degradation signal
    // an orchestrator can act on. Cheap + const, so it's safe to poll on a liveness interval.
    HealthStatus health() const {
        return HealthStatus{ store_overflows_ == 0, cold_store_ != nullptr, catalog_.size(),
                             tier_mgr_.resident_bytes(MemorySpace::HOST), wal_records(), store_overflows_ };
    }

    // OB-2b: the per-query latency histogram — log2 buckets, bucket b counts queries with latency in
    // ~[2^(b-1), 2^b) ns. Sums to stats().query_count.
    std::array<uint64_t, 40> query_latency_histogram() const {   // 40 == LAT_BUCKETS (not visible in return-type position)
        std::array<uint64_t, LAT_BUCKETS> h{};
        for (int b = 0; b < LAT_BUCKETS; ++b) h[static_cast<size_t>(b)] = latency_hist_[static_cast<size_t>(b)].load(std::memory_order_relaxed);
        return h;
    }
    // Estimate the p-th percentile (0..1) query latency in ns from the histogram (bucket-granular —
    // returns ~the bucket's upper bound). p50/p99 are the real ops latency metrics (vs mean/max).
    uint64_t query_latency_percentile_ns(double p) const {
        const uint64_t qc = query_count_.load(std::memory_order_relaxed);
        if (qc == 0) return 0;
        const uint64_t target = static_cast<uint64_t>(p * static_cast<double>(qc));
        uint64_t cum = 0;
        for (int b = 0; b < LAT_BUCKETS; ++b) {
            cum += latency_hist_[static_cast<size_t>(b)].load(std::memory_order_relaxed);
            if (cum >= target) return (b == 0) ? 0ull : (1ull << b);
        }
        return max_query_ns_.load(std::memory_order_relaxed);
    }

    // Persist a catalog column (any type) to `path` via the typed file format (borrows to HOST, returns it).
    void save_column(uint64_t id, const std::string& path) {
        TieredColumn& col = *catalog_.at(id);
        const MemorySpace home = col.tier();
        if (home != MemorySpace::HOST) { ++cold_borrows_; col.migrate_to(MemorySpace::HOST); }
        matrix_write_column_typed(path, col.host_ptr(), col.size_bytes(), static_cast<uint32_t>(column_type(id)));
        if (home != MemorySpace::HOST) col.migrate_to(home);
    }
    // Load a typed column file into the catalog under `id` (born HOST-resident; dispatched by element type).
    void load_column_from_file(uint64_t id, const std::string& path) {
        std::vector<unsigned char> bytes; uint32_t type = 0;
        matrix_read_column_typed(path, bytes, type);
        if (type == static_cast<uint32_t>(MatrixType::I64))
            load_scan_column_i64(id, reinterpret_cast<const int64_t*>(bytes.data()), bytes.size() / sizeof(int64_t));
        else if (type == static_cast<uint32_t>(MatrixType::F64))
            load_scan_column_f64(id, reinterpret_cast<const double*>(bytes.data()), bytes.size() / sizeof(double));
        else
            load_scan_column(id, reinterpret_cast<const uint32_t*>(bytes.data()), bytes.size() / sizeof(uint32_t));
    }

    // Ingest one uint32 column from a CSV file into the catalog under `id` (born HOST-resident, like
    // load_column_from_file). Returns false (no catalog entry created) if the CSV is malformed — CSV is
    // untrusted input, so a bad file is reported, never a crash. See DM-5b / VAL-1.
    bool load_column_from_csv(uint64_t id, const std::string& path, size_t col_index,
                              bool has_header = false, char delim = ',') {
        std::vector<uint32_t> data;
        if (!matrix_read_csv_column(path, col_index, has_header, delim, data)) return false;
        load_scan_column(id, data.data(), data.size());
        return true;
    }

    // int64 / double siblings of load_column_from_csv (DM-3g). Ingest a signed-64-bit or floating-point
    // column straight from CSV. Same graceful contract: malformed CSV → false, no catalog entry, no crash.
    bool load_column_from_csv_i64(uint64_t id, const std::string& path, size_t col_index,
                                  bool has_header = false, char delim = ',') {
        std::vector<int64_t> data;
        if (!matrix_read_csv_column_i64(path, col_index, has_header, delim, data)) return false;
        load_scan_column_i64(id, data.data(), data.size());
        return true;
    }
    bool load_column_from_csv_f64(uint64_t id, const std::string& path, size_t col_index,
                                  bool has_header = false, char delim = ',') {
        std::vector<double> data;
        if (!matrix_read_csv_column_f64(path, col_index, has_header, delim, data)) return false;
        load_scan_column_f64(id, data.data(), data.size());
        return true;
    }

    // String sibling: ingest a string column straight from CSV, dictionary-encoded (see
    // load_string_column_dict). Same graceful contract: malformed CSV / open failure → false, no catalog
    // entry, no crash. Completes "strings are first-class, ingestable from CSV".
    bool load_string_column_from_csv(uint64_t id, const std::string& path, size_t col_index,
                                     bool has_header = false, char delim = ',') {
        std::vector<std::string> data;
        if (!matrix_read_csv_column_str(path, col_index, has_header, delim, data)) return false;
        load_string_column_dict(id, data);
        return true;
    }

    // Snapshot every catalog column to `path`: [u32 magic][u64 num_cols] then per column
    // [u64 id][u64 count][count×u32]. Borrows COLD columns to HOST to read, returns them.
    // Fail-loud on I/O error (never leave a half-written snapshot mistaken for valid).
    void save_catalog(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "save_catalog: open failed %s\n", path.c_str()); std::abort(); }
        const uint32_t magic = MATRIX_CATALOG_MAGIC;
        const uint64_t ncols = catalog_.size();
        bool ok = std::fwrite(&magic, sizeof magic, 1, f) == 1
               && std::fwrite(&ncols, sizeof ncols, 1, f) == 1;
        for (auto& kv : catalog_) {
            if (!ok) break;
            TieredColumn& col = *kv.second;
            const MemorySpace home = col.tier();
            if (home != MemorySpace::HOST) { ++cold_borrows_; col.migrate_to(MemorySpace::HOST); }
            const uint64_t id = kv.first;
            const uint32_t type = static_cast<uint32_t>(column_type(id));   // 0=U32, 1=I64, 2=F64
            const uint64_t count = column_rows(id);                          // type-aware row count
            const std::string nm = column_name(id);                          // optional name ("" if unnamed)
            const uint32_t namelen = static_cast<uint32_t>(nm.size());
            ok = std::fwrite(&id,      sizeof id,      1, f) == 1
              && std::fwrite(&type,    sizeof type,    1, f) == 1
              && std::fwrite(&count,   sizeof count,   1, f) == 1
              && std::fwrite(&namelen, sizeof namelen, 1, f) == 1
              && (namelen == 0 || std::fwrite(nm.data(), 1, namelen, f) == namelen)   // the name
              && (col.size_bytes() == 0
                  || std::fwrite(col.host_ptr(), 1, col.size_bytes(), f) == col.size_bytes());  // raw bytes
            if (home != MemorySpace::HOST) col.migrate_to(home);
        }
        // Trailing section: the string dictionaries (code -> string) for dict-encoded columns, so a restored
        // string column decodes (its codes ride the normal column path above). Backward-compatible — old
        // snapshots simply lack this section and load_catalog's read is EOF-tolerant.
        if (ok) {
            const uint64_t ndicts = string_dicts_.size();
            ok = std::fwrite(&ndicts, sizeof ndicts, 1, f) == 1;
            for (auto& kv : string_dicts_) {
                if (!ok) break;
                const uint64_t did = kv.first, nstr = kv.second.size();
                ok = std::fwrite(&did, sizeof did, 1, f) == 1 && std::fwrite(&nstr, sizeof nstr, 1, f) == 1;
                for (const std::string& s : kv.second) {
                    if (!ok) break;
                    const uint32_t len = static_cast<uint32_t>(s.size());
                    ok = std::fwrite(&len, sizeof len, 1, f) == 1 && (len == 0 || std::fwrite(s.data(), 1, len, f) == len);
                }
            }
        }
        std::fclose(f);
        if (!ok) { std::fprintf(stderr, "save_catalog: short write %s\n", path.c_str()); std::abort(); }
    }
    // Restore a snapshot into the catalog (columns land in HOST; the TierManager re-tiers under load).
    void load_catalog(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) { std::fprintf(stderr, "load_catalog: open failed %s\n", path.c_str()); std::abort(); }
        uint32_t magic = 0; uint64_t ncols = 0;
        bool ok = std::fread(&magic, sizeof magic, 1, f) == 1 && magic == MATRIX_CATALOG_MAGIC
               && std::fread(&ncols, sizeof ncols, 1, f) == 1;
        for (uint64_t c = 0; ok && c < ncols; ++c) {
            uint64_t id = 0, count = 0; uint32_t type = 0, namelen = 0;
            ok = std::fread(&id, sizeof id, 1, f) == 1 && std::fread(&type, sizeof type, 1, f) == 1
              && std::fread(&count, sizeof count, 1, f) == 1 && std::fread(&namelen, sizeof namelen, 1, f) == 1;
            if (!ok) break;
            if (namelen > 4096) { ok = false; break; }   // sane bound — corrupt snapshot guard
            std::string nm(static_cast<size_t>(namelen), '\0');
            if (namelen > 0) { ok = std::fread(nm.data(), 1, namelen, f) == namelen; if (!ok) break; }
            if (type == static_cast<uint32_t>(MatrixType::I64)) {
                std::vector<int64_t> d(static_cast<size_t>(count));
                ok = (count == 0 || std::fread(d.data(), sizeof(int64_t), d.size(), f) == d.size());
                if (ok) load_scan_column_i64(id, d.data(), d.size());
            } else if (type == static_cast<uint32_t>(MatrixType::F64)) {
                std::vector<double> d(static_cast<size_t>(count));
                ok = (count == 0 || std::fread(d.data(), sizeof(double), d.size(), f) == d.size());
                if (ok) load_scan_column_f64(id, d.data(), d.size());
            } else if (type == static_cast<uint32_t>(MatrixType::U32)) {
                std::vector<uint32_t> d(static_cast<size_t>(count));
                ok = (count == 0 || std::fread(d.data(), sizeof(uint32_t), d.size(), f) == d.size());
                if (ok) load_scan_column(id, d.data(), d.size());
            } else {
                ok = false;   // unknown element type -> bad/corrupt snapshot, fail loud below
            }
            if (ok && namelen > 0) name_column(id, nm);   // restore the column's name (DM-2b)
        }
        // Trailing string-dictionaries section (EOF-tolerant: a missing section = an old snapshot, fine).
        if (ok) {
            uint64_t ndicts = 0;
            if (std::fread(&ndicts, sizeof ndicts, 1, f) == 1) {   // present -> new format
                for (uint64_t di = 0; ok && di < ndicts; ++di) {
                    uint64_t did = 0, nstr = 0;
                    ok = std::fread(&did, sizeof did, 1, f) == 1 && std::fread(&nstr, sizeof nstr, 1, f) == 1;
                    if (!ok || nstr > (1ull << 32)) { ok = false; break; }   // sane bound — corrupt guard
                    std::vector<std::string> dict; dict.reserve(static_cast<size_t>(nstr));
                    std::unordered_map<std::string, uint32_t> enc;
                    for (uint64_t si = 0; ok && si < nstr; ++si) {
                        uint32_t len = 0;
                        ok = std::fread(&len, sizeof len, 1, f) == 1;
                        if (!ok || len > (1u << 20)) { ok = false; break; }
                        std::string s(static_cast<size_t>(len), '\0');
                        if (len > 0) { ok = std::fread(s.data(), 1, len, f) == len; if (!ok) break; }
                        enc.emplace(s, static_cast<uint32_t>(dict.size()));
                        dict.push_back(std::move(s));
                    }
                    if (ok) { string_dicts_[did] = std::move(dict); string_encoders_[did] = std::move(enc); }
                }
            }
        }
        std::fclose(f);
        if (!ok) { std::fprintf(stderr, "load_catalog: bad/short snapshot %s\n", path.c_str()); std::abort(); }
    }

    // Back up the whole durable state under one path prefix: <prefix>.catalog (tiered analytical columns,
    // all types) + <prefix>.kv (the point-op store). A point-in-time snapshot; reuses the existing fail-loud
    // writers. Works with or without an attached WAL (save_checkpoint snapshots the in-memory kv_).
    void backup(const std::string& prefix) {
        save_catalog(prefix + ".catalog");
        save_checkpoint(prefix + ".kv");
    }

    // Restore a backup written by backup() into THIS engine (intended for a fresh engine — loading an
    // already-registered column id asserts). Catalog-only backups restore the analytical store; a missing
    // <prefix>.kv leaves the point-op store empty (load_checkpoint returns false).
    void restore(const std::string& prefix) {
        load_catalog(prefix + ".catalog");
        load_checkpoint(prefix + ".kv");
    }

    // Snapshot kv_ atomically to `path`: write temp -> fsync -> rename (POSIX-atomic replace). Fail-loud.
    // ponytail: file data is fsync'd; the rename's own power-loss durability would need a directory fsync
    // (a pre-existing gap shared with the WAL) — the upgrade path if rename-durability matters.
    void save_checkpoint(const std::string& path) {
        const std::string tmp = path + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "save_checkpoint: open failed %s\n", tmp.c_str()); std::abort(); }
        const uint32_t magic = MATRIX_CKPT_MAGIC;
        const uint64_t count = kv_.size();
        bool ok = std::fwrite(&magic, sizeof magic, 1, f) == 1
               && std::fwrite(&count, sizeof count, 1, f) == 1;
        kv_.for_each([&](uint64_t k, uint64_t v) {
            ok = ok && std::fwrite(&k, sizeof k, 1, f) == 1 && std::fwrite(&v, sizeof v, 1, f) == 1;
        });
        std::fflush(f);
        ::fsync(::fileno(f));                       // checkpoint durable BEFORE it replaces the old one
        std::fclose(f);
        if (!ok) { std::fprintf(stderr, "save_checkpoint: short write %s\n", tmp.c_str()); std::abort(); }
        if (std::rename(tmp.c_str(), path.c_str()) != 0) { std::fprintf(stderr, "save_checkpoint: rename failed\n"); std::abort(); }
    }

    // Load a checkpoint into kv_. Missing file -> false (none taken yet). Bad/short -> abort (our format).
    bool load_checkpoint(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        uint32_t magic = 0; uint64_t count = 0;
        bool ok = std::fread(&magic, sizeof magic, 1, f) == 1 && magic == MATRIX_CKPT_MAGIC
               && std::fread(&count, sizeof count, 1, f) == 1;
        for (uint64_t i = 0; ok && i < count; ++i) {
            uint64_t k = 0, v = 0;
            ok = std::fread(&k, sizeof k, 1, f) == 1 && std::fread(&v, sizeof v, 1, f) == 1;
            if (ok) kv_.put(k, v);
        }
        std::fclose(f);
        if (!ok) { std::fprintf(stderr, "load_checkpoint: bad/short %s\n", path.c_str()); std::abort(); }
        return true;
    }

    // Null-mask pointer for a value column, but only if it has one AND it covers all n rows; else nullptr
    // (no masking). ponytail: a mask whose length != n is treated as absent (fail-safe to "no nulls")
    // rather than risk an out-of-bounds read in the reducer.
    const uint8_t* value_nulls(uint64_t value_id, size_t n) const {
        auto it = null_masks_.find(value_id);
        return (it != null_masks_.end() && it->second.size() == n) ? it->second.data() : nullptr;
    }

    // Distinct non-NULL value count over a typed array (helper for count_distinct). Exact hash set.
    template <typename T>
    static uint64_t distinct_count(const T* v, size_t n, const uint8_t* nulls) {
        std::unordered_set<T> seen;
        for (size_t i = 0; i < n; ++i) if (!nulls || !nulls[i]) seen.insert(v[i]);
        return seen.size();
    }

    // uint64 comparison for HAVING (a grouped aggregate can exceed u32, so this is the wide sibling of
    // matrix_pred_match). BETWEEN is inclusive [a, b].
    static bool cmp_u64(MatrixCmp c, uint64_t v, uint64_t a, uint64_t b) {
        switch (c) {
            case MatrixCmp::GE:      return v >= a;
            case MatrixCmp::LT:      return v <  a;
            case MatrixCmp::LE:      return v <= a;
            case MatrixCmp::EQ:      return v == a;
            case MatrixCmp::NE:      return v != a;
            case MatrixCmp::BETWEEN: return v >= a && v <= b;
            case MatrixCmp::GT:
            default:                 return v >  a;
        }
    }

    // GROUP BY: out[g] = aggregate (by op) of value-column rows whose key-column value == g, for
    // g in [0, num_groups). key_id and value_id are distinct catalog columns of equal length
    // (uint32). Borrows both to HOST for the reduction, then returns each to its home tier (so
    // residency stays in lockstep with the TierManager). Records access heat on both columns;
    // migration stays scan-driven (GROUP BY does not itself rebalance — see spec).
    void grouped_aggregate(uint64_t key_id, uint64_t value_id, uint32_t num_groups,
                           MatrixAggOp op, std::vector<uint64_t>& out) {
        assert(key_id != value_id && "group-by key and value must be distinct columns");
        TieredColumn& kc = *catalog_.at(key_id);
        TieredColumn& vc = *catalog_.at(value_id);
        assert(kc.size_bytes() == vc.size_bytes() && "key and value columns must be the same length");
        tier_mgr_.record_access(key_id, kc.size_bytes());
        tier_mgr_.record_access(value_id, vc.size_bytes());

#if defined(MATRIX_USE_CUDA)
        {   // GPU-3g: both columns VRAM-resident + no null mask -> GROUP BY in VRAM, no borrow-down
            const size_t gn = kc.size_bytes() / sizeof(uint32_t);
            if (kc.tier() == MemorySpace::DEVICE && vc.tier() == MemorySpace::DEVICE && value_nulls(value_id, gn) == nullptr) {
                out.resize(num_groups);
                matrix_gpu_group_reduce_dev(kc.device_ptr(), vc.device_ptr(), gn, num_groups, op, MatrixPredicate{}, false, out.data());
                return;
            }
        }
#endif
        const MemorySpace kh = kc.tier(); if (kh != MemorySpace::HOST) { ++cold_borrows_; kc.migrate_to(MemorySpace::HOST); }
        const MemorySpace vh = vc.tier(); if (vh != MemorySpace::HOST) { ++cold_borrows_; vc.migrate_to(MemorySpace::HOST); }
        const uint32_t* keys = reinterpret_cast<const uint32_t*>(kc.host_ptr());
        const uint32_t* vals = reinterpret_cast<const uint32_t*>(vc.host_ptr());
        const size_t n = kc.size_bytes() / sizeof(uint32_t);
        out.resize(num_groups);   // matrix_cpu_group_reduce initializes every slot per op (MIN sentinel ≠ 0)
        matrix_cpu_group_reduce(keys, vals, n, num_groups, op, out.data(), value_nulls(value_id, n));
        if (vh != MemorySpace::HOST) vc.migrate_to(vh);       // return borrows
        if (kh != MemorySpace::HOST) kc.migrate_to(kh);
    }

    // Inner hash equi-join on two uint32 key columns: every (left_row, right_row) with left_key[left_row]
    // == right_key[right_row]. Build a value->left-rows hash, probe with the right. Borrow-and-return both
    // columns (like grouped_aggregate). Result order unspecified; cardinality = result.size(). DM-8.
    // ponytail: builds on the LEFT side unconditionally + materializes all pairs in RAM — a planner would
    // build on the smaller side, and a huge result would need spilling; both are deferred.
    std::vector<std::pair<uint64_t, uint64_t>> hash_join(uint64_t left_key_id, uint64_t right_key_id) {
        assert(catalog_has(left_key_id) && catalog_has(right_key_id) && "hash_join: unknown column id");
        assert(column_type(left_key_id) == MatrixType::U32 && column_type(right_key_id) == MatrixType::U32
               && "hash_join: keys must be uint32 (typed-key join is deferred)");
        TieredColumn& lc = *catalog_.at(left_key_id);
        TieredColumn& rc = *catalog_.at(right_key_id);
        tier_mgr_.record_access(left_key_id, lc.size_bytes());
        tier_mgr_.record_access(right_key_id, rc.size_bytes());
        const MemorySpace lh = lc.tier(); if (lh != MemorySpace::HOST) { ++cold_borrows_; lc.migrate_to(MemorySpace::HOST); }
        const MemorySpace rh = rc.tier(); if (rh != MemorySpace::HOST) { ++cold_borrows_; rc.migrate_to(MemorySpace::HOST); }
        const uint32_t* lk = reinterpret_cast<const uint32_t*>(lc.host_ptr());
        const uint32_t* rk = reinterpret_cast<const uint32_t*>(rc.host_ptr());
        const size_t ln = lc.size_bytes() / sizeof(uint32_t);
        const size_t rn = rc.size_bytes() / sizeof(uint32_t);
        std::unordered_map<uint32_t, std::vector<uint64_t>> build;     // left value -> left rows
        for (size_t i = 0; i < ln; ++i) build[lk[i]].push_back(static_cast<uint64_t>(i));
        std::vector<std::pair<uint64_t, uint64_t>> out;
        for (size_t j = 0; j < rn; ++j) {
            auto it = build.find(rk[j]);
            if (it != build.end()) for (uint64_t i : it->second) out.emplace_back(i, static_cast<uint64_t>(j));
        }
        if (rh != MemorySpace::HOST) rc.migrate_to(rh);                // return borrows
        if (lh != MemorySpace::HOST) lc.migrate_to(lh);
        return out;
    }

    // Count an equi-join's matching pairs WITHOUT materializing them — resource-safe for huge joins
    // (addresses hash_join's materialize-all ceiling). Builds a value->count map (O(distinct) memory,
    // not O(left rows)) and sums match counts on probe. Equals hash_join(left,right).size(). DM-8/§6.
    uint64_t hash_join_count(uint64_t left_key_id, uint64_t right_key_id) {
        assert(catalog_has(left_key_id) && catalog_has(right_key_id) && "hash_join_count: unknown column id");
        assert(column_type(left_key_id) == MatrixType::U32 && column_type(right_key_id) == MatrixType::U32
               && "hash_join_count: keys must be uint32");
        TieredColumn& lc = *catalog_.at(left_key_id);
        TieredColumn& rc = *catalog_.at(right_key_id);
        tier_mgr_.record_access(left_key_id, lc.size_bytes());
        tier_mgr_.record_access(right_key_id, rc.size_bytes());
        const MemorySpace lh = lc.tier(); if (lh != MemorySpace::HOST) { ++cold_borrows_; lc.migrate_to(MemorySpace::HOST); }
        const MemorySpace rh = rc.tier(); if (rh != MemorySpace::HOST) { ++cold_borrows_; rc.migrate_to(MemorySpace::HOST); }
        const uint32_t* lk = reinterpret_cast<const uint32_t*>(lc.host_ptr());
        const uint32_t* rk = reinterpret_cast<const uint32_t*>(rc.host_ptr());
        const size_t ln = lc.size_bytes() / sizeof(uint32_t);
        const size_t rn = rc.size_bytes() / sizeof(uint32_t);
        std::unordered_map<uint32_t, uint64_t> build;                 // left value -> # of left rows
        for (size_t i = 0; i < ln; ++i) ++build[lk[i]];
        uint64_t pairs = 0;
        for (size_t j = 0; j < rn; ++j) { auto it = build.find(rk[j]); if (it != build.end()) pairs += it->second; }
        if (rh != MemorySpace::HOST) rc.migrate_to(rh);
        if (lh != MemorySpace::HOST) lc.migrate_to(lh);
        return pairs;
    }

    // Inner hash equi-join on two int64 key columns (the typed sibling of hash_join — DM-8c). Same
    // build-left / probe-right shape over int64 values; returns matching (left_row, right_row) pairs.
    // (double-key joins are intentionally unsupported — exact float equality is semantically fraught.)
    std::vector<std::pair<uint64_t, uint64_t>> hash_join_i64(uint64_t left_key_id, uint64_t right_key_id) {
        assert(catalog_has(left_key_id) && catalog_has(right_key_id) && "hash_join_i64: unknown column id");
        assert(column_type(left_key_id) == MatrixType::I64 && column_type(right_key_id) == MatrixType::I64
               && "hash_join_i64: keys must be int64");
        TieredColumn& lc = *catalog_.at(left_key_id);
        TieredColumn& rc = *catalog_.at(right_key_id);
        tier_mgr_.record_access(left_key_id, lc.size_bytes());
        tier_mgr_.record_access(right_key_id, rc.size_bytes());
        const MemorySpace lh = lc.tier(); if (lh != MemorySpace::HOST) { ++cold_borrows_; lc.migrate_to(MemorySpace::HOST); }
        const MemorySpace rh = rc.tier(); if (rh != MemorySpace::HOST) { ++cold_borrows_; rc.migrate_to(MemorySpace::HOST); }
        const int64_t* lk = reinterpret_cast<const int64_t*>(lc.host_ptr());
        const int64_t* rk = reinterpret_cast<const int64_t*>(rc.host_ptr());
        const size_t ln = lc.size_bytes() / sizeof(int64_t);
        const size_t rn = rc.size_bytes() / sizeof(int64_t);
        std::unordered_map<int64_t, std::vector<uint64_t>> build;     // left value -> left rows
        for (size_t i = 0; i < ln; ++i) build[lk[i]].push_back(static_cast<uint64_t>(i));
        std::vector<std::pair<uint64_t, uint64_t>> out;
        for (size_t j = 0; j < rn; ++j) {
            auto it = build.find(rk[j]);
            if (it != build.end()) for (uint64_t i : it->second) out.emplace_back(i, static_cast<uint64_t>(j));
        }
        if (rh != MemorySpace::HOST) rc.migrate_to(rh);
        if (lh != MemorySpace::HOST) lc.migrate_to(lh);
        return out;
    }

    // Gather a column's values at the given row indices (the "project" step — e.g. over a join's matched
    // rows). Returns one value per index, in index order, as a uint64: a U32 value zero-extended; an
    // I64/F64 value as its bit pattern (caller reads via static_cast<int64_t> / std::bit_cast<double>,
    // per column_type(col_id) — same convention as execute_query). Borrows COLD->HOST and returns it.
    std::vector<uint64_t> gather(uint64_t col_id, const std::vector<uint64_t>& rows) {
        assert(catalog_has(col_id) && "gather: unknown column id");
        TieredColumn& col = *catalog_.at(col_id);
        const MemorySpace home = col.tier();
        if (home != MemorySpace::HOST) { ++cold_borrows_; col.migrate_to(MemorySpace::HOST); }
        const bool wide = (column_type(col_id) != MatrixType::U32);   // I64/F64 are 8 bytes, U32 is 4
        const size_t width = wide ? 8 : 4;
        const size_t n = col.size_bytes() / width;
        const unsigned char* base = col.host_ptr();
        std::vector<uint64_t> out; out.reserve(rows.size());
        for (uint64_t r : rows) {
            assert(r < n && "gather: row index out of range");
            uint64_t v = 0;
            if (wide) std::memcpy(&v, base + r * 8, 8);
            else { uint32_t u = 0; std::memcpy(&u, base + r * 4, 4); v = u; }
            out.push_back(v);
        }
        if (home != MemorySpace::HOST) col.migrate_to(home);
        return out;
    }

    // Row indices where the u32 filter column satisfies the predicate (capped at `limit`, 0 = no cap) —
    // the filter primitive behind projection. Borrow-and-return like the scans.
    std::vector<uint64_t> matching_rows(uint64_t filter_col, const MatrixPredicate& pred, uint64_t limit) {
        TieredColumn& fc = *catalog_.at(filter_col);
        tier_mgr_.record_access(filter_col, fc.size_bytes());
        const MemorySpace home = fc.tier();
        if (home != MemorySpace::HOST) { ++cold_borrows_; fc.migrate_to(MemorySpace::HOST); }
        const uint32_t* f = reinterpret_cast<const uint32_t*>(fc.host_ptr());
        const size_t n = fc.size_bytes() / sizeof(uint32_t);
        std::vector<uint64_t> rows;
        for (size_t i = 0; i < n; ++i)
            if (matrix_pred_match(f[i], pred)) { rows.push_back(i); if (limit && rows.size() >= limit) break; }
        if (home != MemorySpace::HOST) fc.migrate_to(home);
        return rows;
    }
    // Projection: the values of `col_id` for the rows matching an optional u32 filter (else all rows), capped
    // at `limit` (0 = no cap) — the data behind SELECT col [WHERE fcol <pred>] [LIMIT n]. Values are encoded
    // like gather (u32 zero-extended; i64/f64 as their 8-byte bit pattern). Composes matching_rows + gather.
    std::vector<uint64_t> project(uint64_t col_id, bool has_filter, uint64_t filter_col, const MatrixPredicate& pred, uint64_t limit) {
        std::vector<uint64_t> rows;
        if (has_filter) {
            rows = matching_rows(filter_col, pred, limit);
        } else {
            const size_t n = column_rows(col_id);
            const size_t cap = (limit && static_cast<size_t>(limit) < n) ? static_cast<size_t>(limit) : n;
            rows.reserve(cap);
            for (size_t i = 0; i < cap; ++i) rows.push_back(i);
        }
        return gather(col_id, rows);
    }
    // GROUP BY key WHERE <predicate> — same double borrow-and-return as grouped_aggregate_where.
    void grouped_aggregate_pred(uint64_t key_id, uint64_t value_id, uint32_t num_groups,
                                MatrixAggOp op, const MatrixPredicate& pred, std::vector<uint64_t>& out) {
        assert(key_id != value_id && "group-by key and value must be distinct columns");
        TieredColumn& kc = *catalog_.at(key_id);
        TieredColumn& vc = *catalog_.at(value_id);
        assert(kc.size_bytes() == vc.size_bytes() && "key and value columns must be the same length");
        tier_mgr_.record_access(key_id, kc.size_bytes());
        tier_mgr_.record_access(value_id, vc.size_bytes());
#if defined(MATRIX_USE_CUDA)
        {   // GPU-3g: both columns VRAM-resident + no null mask -> filtered GROUP BY in VRAM, no borrow-down
            const size_t gn = kc.size_bytes() / sizeof(uint32_t);
            if (kc.tier() == MemorySpace::DEVICE && vc.tier() == MemorySpace::DEVICE && value_nulls(value_id, gn) == nullptr) {
                out.resize(num_groups);
                matrix_gpu_group_reduce_dev(kc.device_ptr(), vc.device_ptr(), gn, num_groups, op, pred, true, out.data());
                return;
            }
        }
#endif
        const MemorySpace kh = kc.tier(); if (kh != MemorySpace::HOST) { ++cold_borrows_; kc.migrate_to(MemorySpace::HOST); }
        const MemorySpace vh = vc.tier(); if (vh != MemorySpace::HOST) { ++cold_borrows_; vc.migrate_to(MemorySpace::HOST); }
        const uint32_t* keys = reinterpret_cast<const uint32_t*>(kc.host_ptr());
        const uint32_t* vals = reinterpret_cast<const uint32_t*>(vc.host_ptr());
        const size_t n = kc.size_bytes() / sizeof(uint32_t);
        out.resize(num_groups);
        matrix_cpu_group_reduce_pred(keys, vals, n, num_groups, op, pred, out.data(), value_nulls(value_id, n));
        if (vh != MemorySpace::HOST) vc.migrate_to(vh);
        if (kh != MemorySpace::HOST) kc.migrate_to(kh);
    }

    // GROUP BY key WHERE value > threshold (filtered grouped aggregate). Same contract and double
    // borrow-and-return as grouped_aggregate; only rows with value > threshold contribute.
    void grouped_aggregate_where(uint64_t key_id, uint64_t value_id, uint32_t num_groups,
                                 MatrixAggOp op, uint32_t threshold, std::vector<uint64_t>& out) {
        grouped_aggregate_pred(key_id, value_id, num_groups, op, MatrixPredicate{MatrixCmp::GT, threshold, 0}, out);
    }

    // GROUP BY a uint32 key over an int64 value column (DM-3c). Double borrow-and-return like
    // grouped_aggregate; out holds int64 group aggregates as uint64 bit-patterns. No rebalance (GROUP BY
    // is not scan-driven, matching grouped_aggregate).
    void grouped_aggregate_i64(uint64_t key_id, uint64_t value_id, uint32_t num_groups, MatrixAggOp op,
                               const MatrixPredicateI64& pred, bool has_filter, std::vector<uint64_t>& out) {
        TieredColumn& kc = *catalog_.at(key_id);
        TieredColumn& vc = *catalog_.at(value_id);
        tier_mgr_.record_access(key_id, kc.size_bytes());
        tier_mgr_.record_access(value_id, vc.size_bytes());
#if defined(MATRIX_USE_CUDA)
        {   // GPU-3g: u32 key + i64 value both VRAM-resident + no null mask -> GROUP BY in VRAM
            const size_t gn = vc.size_bytes() / sizeof(int64_t);
            if (kc.tier() == MemorySpace::DEVICE && vc.tier() == MemorySpace::DEVICE && value_nulls(value_id, gn) == nullptr) {
                out.resize(num_groups);
                matrix_gpu_group_reduce_dev_i64(kc.device_ptr(), vc.device_ptr(), gn, num_groups, op, pred, has_filter, out.data());
                return;
            }
        }
#endif
        const MemorySpace kh = kc.tier(); if (kh != MemorySpace::HOST) { ++cold_borrows_; kc.migrate_to(MemorySpace::HOST); }
        const MemorySpace vh = vc.tier(); if (vh != MemorySpace::HOST) { ++cold_borrows_; vc.migrate_to(MemorySpace::HOST); }
        const uint32_t* keys = reinterpret_cast<const uint32_t*>(kc.host_ptr());
        const int64_t*  vals = reinterpret_cast<const int64_t*>(vc.host_ptr());
        const size_t n = vc.size_bytes() / sizeof(int64_t);
        std::vector<int64_t> tmp(num_groups);
        const uint8_t* nulls = value_nulls(value_id, n);
        if (has_filter) matrix_cpu_group_reduce_i64_pred(keys, vals, n, num_groups, op, pred, tmp.data(), nulls);
        else            matrix_cpu_group_reduce_i64(keys, vals, n, num_groups, op, tmp.data(), nulls);
        out.resize(num_groups);
        for (uint32_t g = 0; g < num_groups; ++g) out[g] = static_cast<uint64_t>(tmp[g]);
        if (vh != MemorySpace::HOST) vc.migrate_to(vh);
        if (kh != MemorySpace::HOST) kc.migrate_to(kh);
    }

    // GROUP BY a uint32 key over a double value column (DM-3f). Double borrow-and-return like
    // grouped_aggregate; out holds double group aggregates as uint64 bit-patterns. No rebalance (GROUP BY
    // is not scan-driven, matching grouped_aggregate).
    void grouped_aggregate_f64(uint64_t key_id, uint64_t value_id, uint32_t num_groups, MatrixAggOp op,
                               const MatrixPredicateF64& pred, bool has_filter, std::vector<uint64_t>& out) {
        TieredColumn& kc = *catalog_.at(key_id);
        TieredColumn& vc = *catalog_.at(value_id);
        tier_mgr_.record_access(key_id, kc.size_bytes());
        tier_mgr_.record_access(value_id, vc.size_bytes());
        tier_mgr_.record_access(key_id, kc.size_bytes());
        tier_mgr_.record_access(value_id, vc.size_bytes());
#if defined(MATRIX_USE_CUDA)
        {   // GPU-3g: u32 key + f64 value both VRAM-resident + no null mask -> GROUP BY in VRAM
            const size_t gn = vc.size_bytes() / sizeof(double);
            if (kc.tier() == MemorySpace::DEVICE && vc.tier() == MemorySpace::DEVICE && value_nulls(value_id, gn) == nullptr) {
                out.resize(num_groups);
                matrix_gpu_group_reduce_dev_f64(kc.device_ptr(), vc.device_ptr(), gn, num_groups, op, pred, has_filter, out.data());
                return;
            }
        }
#endif
        const MemorySpace kh = kc.tier(); if (kh != MemorySpace::HOST) { ++cold_borrows_; kc.migrate_to(MemorySpace::HOST); }
        const MemorySpace vh = vc.tier(); if (vh != MemorySpace::HOST) { ++cold_borrows_; vc.migrate_to(MemorySpace::HOST); }
        const uint32_t* keys = reinterpret_cast<const uint32_t*>(kc.host_ptr());
        const double*   vals = reinterpret_cast<const double*>(vc.host_ptr());
        const size_t n = vc.size_bytes() / sizeof(double);
        std::vector<double> tmp(num_groups);
        const uint8_t* nulls = value_nulls(value_id, n);
        if (has_filter) matrix_cpu_group_reduce_f64_pred(keys, vals, n, num_groups, op, pred, tmp.data(), nulls);
        else            matrix_cpu_group_reduce_f64(keys, vals, n, num_groups, op, tmp.data(), nulls);
        out.resize(num_groups);
        for (uint32_t g = 0; g < num_groups; ++g) out[g] = matrix_bit_cast<uint64_t>(tmp[g]);
        if (vh != MemorySpace::HOST) vc.migrate_to(vh);
        if (kh != MemorySpace::HOST) kc.migrate_to(kh);
    }

    // Unified analytical query over catalog columns. Validates input at the boundary and returns a
    // status (never crashes on caller input); on any ERR, out is cleared. Scalar -> out[0];
    // grouped -> out[0..num_groups). Catalog columns only (the legacy id-0 fixed column is the
    // benchmark fixture, not a query target). Public API: times every call (OB-2) — see the
    // execute_query_impl below for the body; this thin wrapper records latency on all return paths.
    MatrixQueryStatus execute_query(const MatrixQuery& q, std::vector<uint64_t>& out) {
        const auto t0 = std::chrono::steady_clock::now();
        const MatrixQueryStatus st = execute_query_impl(q, out);
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        record_query_latency(ns);   // atomic read-path stats; shared with execute_query_shared
        return st;
    }

    // NW-2 concurrency: outcome of the lock-free read fast path (below).
    enum class ReadOutcome { SERVED, NEEDS_EXCLUSIVE };

    // Concurrent read fast-path: serve a QUERY with NO tier side effects (no borrow / heat / rebalance), so
    // it is safe to run under a shared lock alongside other readers. Serves only fully-valid, all-HOST-
    // resident, non-null-masked queries it can reduce directly over host_ptr() via the same matrix_cpu_*
    // reducers as the exclusive path; anything else returns NEEDS_EXCLUSIVE and the caller re-runs it under
    // the exclusive lock via execute_query (which borrows, validates, and reports proper error statuses).
    // The SERVED==execute_query equivalence is asserted in test_concurrent_serving.cpp.
    ReadOutcome execute_query_shared(const MatrixQuery& q, std::vector<uint64_t>& out) {
        const auto t0 = std::chrono::steady_clock::now();
        const ReadOutcome r = execute_query_shared_impl(q, out);
        if (r == ReadOutcome::SERVED)
            record_query_latency(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count()));   // NEEDS_EXCLUSIVE: the exclusive re-run records its own latency
        return r;
    }
    // True iff `id` is a registered, HOST-resident column (so a shared-lock read touches only immutable bytes).
    bool host_resident(uint64_t id) const {
        auto it = catalog_.find(id);
        return it != catalog_.end() && it->second->tier() == MemorySpace::HOST;
    }
    ReadOutcome execute_query_shared_impl(const MatrixQuery& q, std::vector<uint64_t>& out) {
        out.clear();
        if (!host_resident(q.value_col) || null_masks_.count(q.value_col)) return ReadOutcome::NEEDS_EXCLUSIVE;
        const MatrixType vty = column_type(q.value_col);
        const void* vp = catalog_.at(q.value_col)->host_ptr();
        const size_t n = column_rows(q.value_col);

        // scalar cross-column WHERE: filter on a different u32 column
        if (!q.grouped && q.has_filter && q.filter_col != 0 && q.filter_col != q.value_col) {
            if (!host_resident(q.filter_col) || column_type(q.filter_col) != MatrixType::U32) return ReadOutcome::NEEDS_EXCLUSIVE;
            if (column_rows(q.filter_col) != n) return ReadOutcome::NEEDS_EXCLUSIVE;
            const uint32_t* f = reinterpret_cast<const uint32_t*>(catalog_.at(q.filter_col)->host_ptr());
            const MatrixPredicate p{q.cmp, q.threshold, q.upper};
            if (vty == MatrixType::I64)      out.assign(1, static_cast<uint64_t>(matrix_cpu_reduce_filtered_by_i64(f, p, reinterpret_cast<const int64_t*>(vp), n, q.agg)));
            else if (vty == MatrixType::F64) out.assign(1, matrix_bit_cast<uint64_t>(matrix_cpu_reduce_filtered_by_f64(f, p, reinterpret_cast<const double*>(vp), n, q.agg)));
            else                             out.assign(1, matrix_cpu_reduce_filtered_by(f, p, reinterpret_cast<const uint32_t*>(vp), n, q.agg));
            return ReadOutcome::SERVED;
        }
        if (q.grouped && q.has_filter && q.filter_col != 0 && q.filter_col != q.value_col)
            return ReadOutcome::NEEDS_EXCLUSIVE;   // grouped cross-column: defer to the exclusive path

        // grouped (key must be a registered, HOST, u32 column distinct from value, valid group count, aligned)
        if (q.grouped) {
            if (!host_resident(q.key_col) || q.key_col == q.value_col || q.num_groups == 0
                || q.num_groups > max_query_groups_ || column_type(q.key_col) != MatrixType::U32
                || column_rows(q.key_col) != n) return ReadOutcome::NEEDS_EXCLUSIVE;
            const uint32_t* keys = reinterpret_cast<const uint32_t*>(catalog_.at(q.key_col)->host_ptr());
            const uint32_t G = q.num_groups;
            out.assign(G, 0);
            if (vty == MatrixType::I64) {
                std::vector<int64_t> tmp(G);
                if (q.has_filter) matrix_cpu_group_reduce_i64_pred(keys, reinterpret_cast<const int64_t*>(vp), n, G, q.agg, MatrixPredicateI64{q.cmp, q.lo_i64, q.hi_i64}, tmp.data());
                else              matrix_cpu_group_reduce_i64(keys, reinterpret_cast<const int64_t*>(vp), n, G, q.agg, tmp.data());
                for (uint32_t g = 0; g < G; ++g) out[g] = static_cast<uint64_t>(tmp[g]);
            } else if (vty == MatrixType::F64) {
                std::vector<double> tmp(G);
                if (q.has_filter) matrix_cpu_group_reduce_f64_pred(keys, reinterpret_cast<const double*>(vp), n, G, q.agg, MatrixPredicateF64{q.cmp, q.lo_f64, q.hi_f64}, tmp.data());
                else              matrix_cpu_group_reduce_f64(keys, reinterpret_cast<const double*>(vp), n, G, q.agg, tmp.data());
                for (uint32_t g = 0; g < G; ++g) out[g] = matrix_bit_cast<uint64_t>(tmp[g]);
            } else {
                if (q.has_filter) matrix_cpu_group_reduce_pred(keys, reinterpret_cast<const uint32_t*>(vp), n, G, q.agg, MatrixPredicate{q.cmp, q.threshold, q.upper}, out.data());
                else              matrix_cpu_group_reduce(keys, reinterpret_cast<const uint32_t*>(vp), n, G, q.agg, out.data());
            }
            return ReadOutcome::SERVED;
        }

        // scalar (same-column or unfiltered)
        if (vty == MatrixType::I64) {
            const MatrixPredicateI64 p{q.cmp, q.lo_i64, q.hi_i64}; const int64_t* v = reinterpret_cast<const int64_t*>(vp);
            out.assign(1, static_cast<uint64_t>(q.has_filter ? matrix_cpu_reduce_pred_i64(v, n, p, q.agg) : matrix_cpu_reduce_all_i64(v, n, q.agg)));
        } else if (vty == MatrixType::F64) {
            const MatrixPredicateF64 p{q.cmp, q.lo_f64, q.hi_f64}; const double* v = reinterpret_cast<const double*>(vp);
            out.assign(1, matrix_bit_cast<uint64_t>(q.has_filter ? matrix_cpu_reduce_pred_f64(v, n, p, q.agg) : matrix_cpu_reduce_all_f64(v, n, q.agg)));
        } else {
            const MatrixPredicate p{q.cmp, q.threshold, q.upper}; const uint32_t* v = reinterpret_cast<const uint32_t*>(vp);
            out.assign(1, q.has_filter ? matrix_cpu_reduce_pred(v, n, p, q.agg) : matrix_cpu_reduce_all(v, n, q.agg));
        }
        return ReadOutcome::SERVED;
    }

    // Top-N groups by aggregate value (ORDER BY agg DESC LIMIT k): run a grouped query, then return the k
    // (group_id, value) pairs with the largest aggregate. The staple "top 10 X by Y" analytical query.
    // ponytail: sorts by the RAW uint64 aggregate — exact for U32-valued groups and COUNT (non-negative);
    // for int64/double SUM/MIN/MAX the result bits aren't value-order, so use this for U32/COUNT grouping.
    std::vector<std::pair<uint64_t, uint64_t>> top_groups(const MatrixQuery& q, size_t k) {
        std::vector<uint64_t> out;
        if (!q.grouped || execute_query(q, out) != MatrixQueryStatus::OK) return {};
        std::vector<std::pair<uint64_t, uint64_t>> pairs;
        pairs.reserve(out.size());
        for (uint64_t g = 0; g < out.size(); ++g) pairs.emplace_back(g, out[g]);
        const size_t kk = std::min(k, pairs.size());
        std::partial_sort(pairs.begin(), pairs.begin() + static_cast<std::ptrdiff_t>(kk), pairs.end(),
                          [](const auto& a, const auto& b) { return a.second > b.second; });   // value desc
        pairs.resize(kk);
        return pairs;
    }

    // HAVING: the groups whose aggregate satisfies `cmp` against threshold (BETWEEN uses [threshold, upper]) —
    // e.g. "regions where SUM(amount) > 1000". Runs the grouped query, then filters the (group, value) pairs.
    // ponytail: compares the RAW uint64 aggregate (a uint64 comparator, since a grouped SUM can exceed u32),
    // so it's exact for U32-valued groups and COUNT; I64/F64 bit-patterns aren't value-order (use for U32/COUNT),
    // matching top_groups.
    std::vector<std::pair<uint64_t, uint64_t>> having(const MatrixQuery& q, MatrixCmp cmp,
                                                      uint64_t threshold, uint64_t upper = 0) {
        std::vector<uint64_t> out;
        if (!q.grouped || execute_query(q, out) != MatrixQueryStatus::OK) return {};
        std::vector<std::pair<uint64_t, uint64_t>> pairs;
        for (uint64_t g = 0; g < out.size(); ++g)
            if (cmp_u64(cmp, out[g], threshold, upper)) pairs.emplace_back(g, out[g]);
        return pairs;
    }

    // AVG(value_col) = SUM/COUNT as double(s), derived from the two existing aggregates — so it inherits
    // their per-type handling (U32/I64/F64) and NULL-skipping for free, with no new reducer op threaded
    // through every typed/grouped/filtered/nullable path. A scalar query yields one element; a grouped
    // query yields one per group. A zero-count set/group yields NaN (the average of nothing). NULL-aware on
    // both paths: SUM and COUNT both skip NULLs (scalar always; grouped since the grouped reducers take the mask).
    std::vector<double> average(const MatrixQuery& q) {
        MatrixQuery sq = q; sq.agg = AGG_SUM;   std::vector<uint64_t> sv;
        MatrixQuery cq = q; cq.agg = AGG_COUNT; std::vector<uint64_t> cv;
        if (execute_query(sq, sv) != MatrixQueryStatus::OK || execute_query(cq, cv) != MatrixQueryStatus::OK
            || sv.size() != cv.size())
            return {};
        const MatrixType ty = column_type(q.value_col);
        std::vector<double> out(sv.size());
        for (size_t i = 0; i < sv.size(); ++i) {
            const double sum = (ty == MatrixType::F64) ? matrix_bit_cast<double>(sv[i])
                             : (ty == MatrixType::I64) ? static_cast<double>(static_cast<int64_t>(sv[i]))
                                                       : static_cast<double>(sv[i]);            // U32
            // COUNT is encoded like the column: F64 columns return it as double-bits (execute_query bit_casts
            // the whole F64 result), U32/I64 return a plain integer count.
            const double count = (ty == MatrixType::F64) ? matrix_bit_cast<double>(cv[i])
                                                         : static_cast<double>(cv[i]);
            out[i] = (count != 0.0) ? sum / count : std::numeric_limits<double>::quiet_NaN();
        }
        return out;
    }

    // COUNT(DISTINCT col): number of distinct non-NULL values in a column. Borrow-and-return like the
    // scalar aggregates; null-aware (skips masked rows, via value_nulls). Typed over U32/I64/F64.
    // ponytail: an exact hash set over every value — O(distinct) memory. A HyperLogLog sketch is the
    // upgrade path when the column is huge and an estimate is acceptable. F64 NaN edge: each NaN counts as
    // distinct (NaN != NaN), since the set is over double values — documented, not special-cased.
    uint64_t count_distinct(uint64_t col_id) {
        TieredColumn& col = *catalog_.at(col_id);
        tier_mgr_.record_access(col_id, col.size_bytes());
        const MemorySpace home = col.tier();
        if (home != MemorySpace::HOST) { ++cold_borrows_; col.migrate_to(MemorySpace::HOST); }
        const MatrixType ty = column_type(col_id);
        const size_t elem = (ty == MatrixType::U32) ? sizeof(uint32_t) : sizeof(uint64_t);
        const size_t n = col.size_bytes() / elem;
        const uint8_t* nulls = value_nulls(col_id, n);
        uint64_t result;
        if (ty == MatrixType::I64)
            result = distinct_count(reinterpret_cast<const int64_t*>(col.host_ptr()), n, nulls);
        else if (ty == MatrixType::F64)
            result = distinct_count(reinterpret_cast<const double*>(col.host_ptr()), n, nulls);
        else
            result = distinct_count(reinterpret_cast<const uint32_t*>(col.host_ptr()), n, nulls);
        if (home != MemorySpace::HOST) col.migrate_to(home);
        maybe_rebalance();
        return result;
    }

    // Parse + run an AVG query string ("SELECT AVG(col) [WHERE ...] [GROUP BY k]") -> the average(s) as
    // double(s). AVG isn't a reducer op (see average()), so rather than teach the parser a phantom agg we
    // rewrite the AVG token to SUM, reuse the full parser (WHERE/GROUP BY/etc.), then derive SUM/COUNT.
    // The tokenizer round-trips (space-join re-tokenizes identically), so this needs no parser change.
    // Empty on parse error or a non-AVG query.
    std::vector<double> avg_query(const std::string& sql) {
        std::vector<std::string> tk = matrixparse_tokenize(sql);
        if (tk.size() < 2) return {};
        std::string a = tk[1];
        for (char& c : a) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (a != "AVG") return {};                                   // only the AVG string form lives here
        tk[1] = "SUM";                                               // rewrite the aggregate keyword
        std::string rewritten;
        for (size_t i = 0; i < tk.size(); ++i) { if (i) rewritten += ' '; rewritten += tk[i]; }
        MatrixQuery q;
        if (parse_query(rewritten, q) != MatrixQueryStatus::OK) return {};
        return average(q);
    }

    // Parse + run a MULTI-aggregate query ("SELECT COUNT(a), SUM(b), MIN(c) [WHERE ...] [GROUP BY k]") ->
    // one result column per aggregate (scalar: size 1; grouped: size num_groups), each encoded like
    // execute_query for that aggregate's value type. Splits the comma-separated SELECT list and delegates
    // each "SELECT agg(col) <shared tail>" to the full parser+executor, so it inherits WHERE (incl.
    // cross-column / string filters), GROUP BY, and per-type handling for free. Empty {} on any parse/exec
    // error. ponytail: COUNT/SUM/MIN/MAX only (AVG via avg_query); the SELECT list is literal-free, so the
    // first WHERE/GROUP/ORDER keyword is the tail boundary and the list splits cleanly on commas.
    std::vector<std::vector<uint64_t>> query_multi(const std::string& sql) {
        size_t b = 0; while (b < sql.size() && std::isspace(static_cast<unsigned char>(sql[b]))) ++b;
        std::string up = sql.substr(b);
        for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (up.rfind("SELECT", 0) != 0) return {};
        size_t tail = std::string::npos;                            // first of WHERE/GROUP/ORDER (the tail)
        for (const char* kw : {" WHERE", " GROUP", " ORDER"}) {
            const size_t p = up.find(kw, 6);
            if (p != std::string::npos) tail = std::min(tail, b + p);
        }
        const size_t listEnd = (tail == std::string::npos) ? sql.size() : tail;
        const std::string list = sql.substr(b + 6, listEnd - (b + 6));   // the comma-separated agg list
        const std::string rest = (tail == std::string::npos) ? std::string{} : sql.substr(tail); // " WHERE ..." etc.
        std::vector<std::vector<uint64_t>> results;
        for (size_t i = 0; i <= list.size(); ) {
            const size_t j = list.find(',', i);
            std::string term = list.substr(i, (j == std::string::npos ? list.size() : j) - i);
            size_t t0 = 0, t1 = term.size();                        // trim
            while (t0 < t1 && std::isspace(static_cast<unsigned char>(term[t0]))) ++t0;
            while (t1 > t0 && std::isspace(static_cast<unsigned char>(term[t1 - 1]))) --t1;
            term = term.substr(t0, t1 - t0);
            if (!term.empty()) {
                MatrixQuery q; std::vector<uint64_t> r;
                if (parse_query("SELECT " + term + rest, q) != MatrixQueryStatus::OK) return {};
                if (execute_query(q, r) != MatrixQueryStatus::OK) return {};
                results.push_back(std::move(r));
            }
            if (j == std::string::npos) break;
            i = j + 1;
        }
        return results;
    }

    // Parse + run a PROJECTION query ("SELECT col [WHERE fcol op val [AND val]] [LIMIT n]") -> the values of
    // `col` for the matching rows (encoded like gather). Distinct from the aggregate forms: a bare column,
    // no AGG(...). The WHERE predicate reuses parse_query on a synthetic "SELECT COUNT(fcol) WHERE ..." so it
    // inherits numeric + string-dict / ordered / BETWEEN filters; v1 filters on a u32 column. Empty {} if it
    // isn't a well-formed projection. ponytail: O(n) row scan behind the filter; a sorted index is the upgrade.
    std::vector<uint64_t> project_query(const std::string& sql) {
        const std::vector<std::string> tk = matrixparse_tokenize(sql);
        auto up = [](std::string s){ for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return s; };
        if (tk.size() < 2 || up(tk[0]) != "SELECT") return {};
        if (tk.size() >= 3 && tk[2] == "(") return {};               // an aggregate, not a projection
        const uint64_t col_id = column_id(tk[1]);
        if (col_id == 0) return {};
        uint64_t limit = 0; size_t whereEnd = tk.size();             // [2, whereEnd) is the optional WHERE region
        for (size_t i = 2; i < tk.size(); ++i) {
            if (up(tk[i]) == "LIMIT") {
                if (i + 2 != tk.size()) return {};                   // LIMIT must be the tail: LIMIT n
                uint64_t nlim = 0; auto [p, ec] = std::from_chars(tk[i + 1].data(), tk[i + 1].data() + tk[i + 1].size(), nlim);
                if (ec != std::errc{} || p != tk[i + 1].data() + tk[i + 1].size()) return {};
                limit = nlim; whereEnd = i; break;
            }
        }
        bool has_filter = false; uint64_t fcol_id = 0; MatrixPredicate pred{};
        if (whereEnd > 2) {
            if (up(tk[2]) != "WHERE" || tk.size() < 4) return {};
            fcol_id = column_id(tk[3]);
            if (fcol_id == 0 || column_type(fcol_id) != MatrixType::U32) return {};   // v1: u32 filter (incl. dict strings)
            std::string agg = "SELECT COUNT ( " + tk[3] + " )";       // reuse parse_query's WHERE parsing
            for (size_t i = 2; i < whereEnd; ++i) { agg += ' '; agg += tk[i]; }
            MatrixQuery q;
            if (parse_query(agg, q) != MatrixQueryStatus::OK || !q.has_filter) return {};
            if (column_rows(fcol_id) != column_rows(col_id)) return {};   // misaligned filter / projection columns
            has_filter = true; pred = MatrixPredicate{q.cmp, q.threshold, q.upper};
        }
        return project(col_id, has_filter, fcol_id, pred, limit);
    }

    // Parse + run a top-N grouped query string ("SELECT SUM(x) GROUP BY k ORDER BY SUM DESC LIMIT n").
    // Returns the (group, value) pairs by value desc; empty on parse error or a query without GROUP BY + LIMIT.
    std::vector<std::pair<uint64_t, uint64_t>> top_query(const std::string& sql) {
        MatrixQuery q;
        if (parse_query(sql, q) != MatrixQueryStatus::OK || !q.grouped || q.limit == 0) return {};
        return top_groups(q, q.limit);
    }

    // Parse + run a HAVING query string ("SELECT SUM(x) GROUP BY k HAVING SUM <cmp> v") -> the (group,value)
    // pairs whose aggregate satisfies the comparison. Splits the HAVING tail off, parses the head (the
    // grouped query) via the full parser — the tokenizer round-trips, so the space-joined head re-parses
    // identically — then runs having(). Empty on parse error or a query without GROUP BY + HAVING.
    // ponytail: the string form takes a single comparison (GT/GE/LT/LE/EQ/NE); BETWEEN-in-HAVING is
    // reachable only via the having() method (rare in practice).
    std::vector<std::pair<uint64_t, uint64_t>> having_query(const std::string& sql) {
        std::vector<std::string> tk = matrixparse_tokenize(sql);
        auto up = [](std::string s){ for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return s; };
        size_t hi = tk.size();
        for (size_t i = 0; i < tk.size(); ++i) if (up(tk[i]) == "HAVING") { hi = i; break; }
        if (hi == tk.size() || hi + 4 != tk.size()) return {};            // need exactly: HAVING <key> <cmp> <value>
        std::string head;
        for (size_t i = 0; i < hi; ++i) { if (i) head += ' '; head += tk[i]; }
        MatrixQuery q;
        if (parse_query(head, q) != MatrixQueryStatus::OK || !q.grouped) return {};
        // sort key must name the SELECT aggregate or the value column (same rule as ORDER BY)
        static const char* AGGN[] = { "COUNT", "SUM", "MIN", "MAX" };
        if (up(tk[hi + 1]) != AGGN[q.agg] && column_id(tk[hi + 1]) != q.value_col) return {};
        MatrixCmp cmp;
        const std::string op = tk[hi + 2];
        if      (op == ">")  cmp = MatrixCmp::GT;  else if (op == ">=") cmp = MatrixCmp::GE;
        else if (op == "<")  cmp = MatrixCmp::LT;  else if (op == "<=") cmp = MatrixCmp::LE;
        else if (op == "=")  cmp = MatrixCmp::EQ;  else if (op == "!=") cmp = MatrixCmp::NE;
        else return {};
        const std::string& v = tk[hi + 3];
        uint64_t threshold = 0; auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), threshold);
        if (ec != std::errc{} || p != v.data() + v.size()) return {};
        return having(q, cmp, threshold);
    }

    // Parse + run a COUNT(DISTINCT col) query string -> the distinct non-NULL value count. Completes the
    // aggregate-string surface (every supported aggregate is now expressible as SQL). Returns false on a
    // malformed / non-distinct query or an unknown column (out untouched), true with the count otherwise.
    bool distinct_query(const std::string& sql, uint64_t& out) {
        std::vector<std::string> tk = matrixparse_tokenize(sql);
        auto up = [](std::string s){ for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return s; };
        if (tk.size() != 6) return false;                                 // SELECT COUNT ( DISTINCT col )
        if (up(tk[0]) != "SELECT" || up(tk[1]) != "COUNT" || tk[2] != "(" || up(tk[3]) != "DISTINCT" || tk[5] != ")")
            return false;
        const uint64_t cid = column_id(tk[4]);
        if (cid == 0) return false;                                       // unknown column
        out = count_distinct(cid);
        return true;
    }

    // Parse a scalar query string into `out` (see DM-4 grammar). Returns OK, ERR_UNKNOWN_COLUMN (bad name),
    // or ERR_PARSE (any malformed form). Untrusted input — never crashes; on ANY non-OK status `out` is
    // reset to a default MatrixQuery (so a caller that ignores the status can't execute partial garbage).
    MatrixQueryStatus parse_query(const std::string& sql, MatrixQuery& out) {
        const MatrixQueryStatus st = parse_query_impl(sql, out);
        if (st != MatrixQueryStatus::OK) out = MatrixQuery{};   // no partial state survives a parse error
        return st;
    }

    // The parse pipeline (see grammar). `out` is reset at entry; the public parse_query wrapper above also
    // clears it on any error exit, so partial fields set before a mid-parse failure never leak to a caller.
    MatrixQueryStatus parse_query_impl(const std::string& sql, MatrixQuery& out) {
        out = MatrixQuery{};
        const std::vector<std::string> tk = matrixparse_tokenize(sql);
        size_t k = 0;
        auto up   = [](std::string s){ for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return s; };
        auto next = [&]{ return k < tk.size() ? tk[k++] : std::string{}; };
        if (up(next()) != "SELECT") return MatrixQueryStatus::ERR_PARSE;
        const std::string aggs = up(next());
        MatrixAggOp agg;
        if (aggs == "COUNT") agg = AGG_COUNT; else if (aggs == "SUM") agg = AGG_SUM;
        else if (aggs == "MIN") agg = AGG_MIN; else if (aggs == "MAX") agg = AGG_MAX;
        else return MatrixQueryStatus::ERR_PARSE;
        if (next() != "(") return MatrixQueryStatus::ERR_PARSE;
        const std::string col = next();
        if (next() != ")") return MatrixQueryStatus::ERR_PARSE;
        const uint64_t vid = column_id(col);
        if (vid == 0) return MatrixQueryStatus::ERR_UNKNOWN_COLUMN;
        out.value_col = vid; out.agg = agg;
        auto peek = [&]{ return k < tk.size() ? up(tk[k]) : std::string{}; };   // uppercased lookahead, no consume
        // optional WHERE <valuecol> <op> <val> [AND <val>]
        if (peek() == "WHERE") {
            next();                                                            // consume WHERE
            // The filter column may differ from the SELECT column (cross-column WHERE) — but only a u32
            // column can be the filter (v1: dict-encoded strings + u32 dims). Same column keeps any type.
            const uint64_t fcol = column_id(next());
            if (fcol == 0) return MatrixQueryStatus::ERR_UNKNOWN_COLUMN;
            if (fcol != vid && column_type(fcol) != MatrixType::U32) return MatrixQueryStatus::ERR_PARSE;
            const uint64_t pcol = fcol;                       // the column the predicate reads
            out.filter_col = (fcol == vid) ? 0 : fcol;        // 0 = filter the value column (existing behavior)
            const std::string ops = up(next());
            const MatrixType ty = column_type(pcol);
            out.has_filter = true;
            if (string_dicts_.find(pcol) != string_dicts_.end()) {
                // Dict-encoded string column: the dict is sorted, so a code's order == its string's order.
                // EQ/NE use the exact code (string_encode -> a no-row code if absent: '=' matches nothing,
                // '!=' all). Ordered ops + BETWEEN translate the (optionally quoted) literal to a code rank via
                // binary search on the sorted dict, so they mean lexicographic string comparison.
                auto unquote = [](std::string s){ return (s.size() >= 2 && (s.front()=='\'' || s.front()=='"') && s.back()==s.front()) ? s.substr(1, s.size()-2) : s; };
                const std::vector<std::string>& dict = string_dicts_.at(pcol);   // sorted
                auto lb = [&](const std::string& x){ return static_cast<uint32_t>(std::lower_bound(dict.begin(), dict.end(), x) - dict.begin()); }; // first code with dict[c] >= x
                auto ub = [&](const std::string& x){ return static_cast<uint32_t>(std::upper_bound(dict.begin(), dict.end(), x) - dict.begin()); }; // first code with dict[c] >  x
                if (ops == "BETWEEN") {
                    const std::string lo = unquote(next());
                    if (up(next()) != "AND") return MatrixQueryStatus::ERR_PARSE;
                    const std::string hi = unquote(next());
                    const uint32_t u = ub(hi);
                    out.cmp = MatrixCmp::BETWEEN; out.threshold = lb(lo); out.upper = (u == 0 ? 0u : u - 1); // codes [lb(lo), ub(hi)-1] == strings [lo,hi]
                } else if (ops == "=" || ops == "!=") {
                    out.cmp = (ops == "=") ? MatrixCmp::EQ : MatrixCmp::NE;
                    const std::string lit = next(); if (lit.empty()) return MatrixQueryStatus::ERR_PARSE;
                    out.threshold = string_encode(pcol, unquote(lit));
                } else {
                    const std::string x = unquote(next()); if (x.empty()) return MatrixQueryStatus::ERR_PARSE;
                    if      (ops == ">")  { out.cmp = MatrixCmp::GE; out.threshold = ub(x); }   // s >  x: codes >= upper_bound(x)
                    else if (ops == ">=") { out.cmp = MatrixCmp::GE; out.threshold = lb(x); }   // s >= x: codes >= lower_bound(x)
                    else if (ops == "<")  { out.cmp = MatrixCmp::LT; out.threshold = lb(x); }   // s <  x: codes <  lower_bound(x)
                    else if (ops == "<=") { out.cmp = MatrixCmp::LT; out.threshold = ub(x); }   // s <= x: codes <  upper_bound(x)
                    else return MatrixQueryStatus::ERR_PARSE;
                }
            } else if (ops == "BETWEEN") {
                out.cmp = MatrixCmp::BETWEEN;
                if (!set_bound(ty, out, true, next())) return MatrixQueryStatus::ERR_PARSE;
                if (up(next()) != "AND")               return MatrixQueryStatus::ERR_PARSE;
                if (!set_bound(ty, out, false, next())) return MatrixQueryStatus::ERR_PARSE;
            } else {
                if      (ops == ">")  out.cmp = MatrixCmp::GT;  else if (ops == ">=") out.cmp = MatrixCmp::GE;
                else if (ops == "<")  out.cmp = MatrixCmp::LT;  else if (ops == "<=") out.cmp = MatrixCmp::LE;
                else if (ops == "=")  out.cmp = MatrixCmp::EQ;  else if (ops == "!=") out.cmp = MatrixCmp::NE;
                else return MatrixQueryStatus::ERR_PARSE;
                if (!set_bound(ty, out, true, next())) return MatrixQueryStatus::ERR_PARSE;
            }
        }
        // optional GROUP BY <keycol> (uint32 key). num_groups is derived from the key column (max+1).
        if (peek() == "GROUP") {
            next();                                                            // consume GROUP
            if (up(next()) != "BY") return MatrixQueryStatus::ERR_PARSE;
            const uint64_t kid = column_id(next());
            if (kid == 0) return MatrixQueryStatus::ERR_UNKNOWN_COLUMN;
            if (column_type(kid) != MatrixType::U32) return MatrixQueryStatus::ERR_PARSE;  // grouped key must be u32
            out.grouped = true; out.key_col = kid; out.num_groups = derive_num_groups_u32(kid);
        }
        // optional ORDER BY <agg|valuecol> DESC LIMIT <n> -> top-N groups. DESC only (top_groups is descending)
        // and requires GROUP BY (top-N is over groups). Sets out.limit; top_query applies it.
        if (peek() == "ORDER") {
            next();                                                            // consume ORDER
            if (up(next()) != "BY") return MatrixQueryStatus::ERR_PARSE;
            if (!out.grouped)       return MatrixQueryStatus::ERR_PARSE;       // top-N is meaningless without groups
            const std::string sortraw = next();                               // sort key: the agg name or the value col
            if (up(sortraw) != aggs && column_id(sortraw) != vid) return MatrixQueryStatus::ERR_PARSE;
            if (up(next()) != "DESC")  return MatrixQueryStatus::ERR_PARSE;    // only DESC (top_groups sorts desc)
            if (up(next()) != "LIMIT") return MatrixQueryStatus::ERR_PARSE;
            const std::string lim = next();
            uint64_t n = 0; auto [p, ec] = std::from_chars(lim.data(), lim.data() + lim.size(), n);
            if (ec != std::errc{} || p != lim.data() + lim.size() || n == 0) return MatrixQueryStatus::ERR_PARSE;
            out.limit = n;
        }
        if (k != tk.size()) return MatrixQueryStatus::ERR_PARSE;     // trailing junk
        return MatrixQueryStatus::OK;
    }

private:
    // Parse the numeric literal `v` into the bound field matching the column type (lo=true -> primary/lower).
    // Returns false on junk / overflow / empty. int64 via from_chars; double via strtod; u32 via from_chars.
    bool set_bound(MatrixType ty, MatrixQuery& q, bool lo, const std::string& v) {
        if (v.empty()) return false;
        if (ty == MatrixType::F64) {
            errno = 0; char* e = nullptr; const double d = std::strtod(v.c_str(), &e);
            if (e != v.c_str() + v.size() || errno == ERANGE) return false;
            (lo ? q.lo_f64 : q.hi_f64) = d; return true;
        }
        if (ty == MatrixType::I64) {
            int64_t x = 0; auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), x);
            if (ec != std::errc{} || p != v.data() + v.size()) return false;
            (lo ? q.lo_i64 : q.hi_i64) = x; return true;
        }
        uint32_t x = 0; auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), x);
        if (ec != std::errc{} || p != v.data() + v.size()) return false;
        (lo ? q.threshold : q.upper) = x; return true;
    }

    // num_groups for a parsed GROUP BY = (max key in the u32 key column) + 1 — the dense-group count.
    // ponytail: this reads/borrows the key column's DATA at PARSE time (a max-scan), unlike pure-metadata
    // parsing — an explicit `INTO n` or a planner stats-lookup would avoid it. A sparse/huge key makes
    // num_groups large and execute_query then rejects it (ERR_TOO_MANY_GROUPS); empty -> 0 (ERR_INVALID_GROUP).
    uint32_t derive_num_groups_u32(uint64_t kid) {
        TieredColumn& col = *catalog_.at(kid);
        const MemorySpace home = col.tier();
        if (home != MemorySpace::HOST) { ++cold_borrows_; col.migrate_to(MemorySpace::HOST); }
        const uint32_t* keys = reinterpret_cast<const uint32_t*>(col.host_ptr());
        const size_t n = col.size_bytes() / sizeof(uint32_t);
        const uint64_t mx = matrix_cpu_reduce_all(keys, n, AGG_MAX);   // max key (0 if n==0)
        if (home != MemorySpace::HOST) col.migrate_to(home);
        return (n == 0) ? 0u : static_cast<uint32_t>(mx + 1);
    }

    // Cross-column scalar WHERE: aggregate value_col over the rows where the u32 filter_col satisfies the
    // predicate. Borrows both columns to HOST, dispatches by the value type, returns the encoded result
    // (same codec as the per-type scalar scans). ponytail: two-pass borrow + one fused reduce; a GPU
    // cross-column kernel + a grouped variant are the documented follow-ups.
    uint64_t scalar_cross_filter(const MatrixQuery& q) {
        TieredColumn& fc = *catalog_.at(q.filter_col);
        TieredColumn& vc = *catalog_.at(q.value_col);
        tier_mgr_.record_access(q.filter_col, fc.size_bytes());
        tier_mgr_.record_access(q.value_col, vc.size_bytes());
        const MemorySpace fh = fc.tier(); if (fh != MemorySpace::HOST) { ++cold_borrows_; fc.migrate_to(MemorySpace::HOST); }
        const MemorySpace vh = vc.tier(); if (vh != MemorySpace::HOST) { ++cold_borrows_; vc.migrate_to(MemorySpace::HOST); }
        const uint32_t* f = reinterpret_cast<const uint32_t*>(fc.host_ptr());
        const size_t n = fc.size_bytes() / sizeof(uint32_t);
        const MatrixPredicate p{q.cmp, q.threshold, q.upper};
        const MatrixType vty = column_type(q.value_col);
        uint64_t enc;
        if (vty == MatrixType::I64)
            enc = static_cast<uint64_t>(matrix_cpu_reduce_filtered_by_i64(f, p, reinterpret_cast<const int64_t*>(vc.host_ptr()), n, q.agg));
        else if (vty == MatrixType::F64)
            enc = matrix_bit_cast<uint64_t>(matrix_cpu_reduce_filtered_by_f64(f, p, reinterpret_cast<const double*>(vc.host_ptr()), n, q.agg));
        else
            enc = matrix_cpu_reduce_filtered_by(f, p, reinterpret_cast<const uint32_t*>(vc.host_ptr()), n, q.agg);
        if (vh != MemorySpace::HOST) vc.migrate_to(vh);
        if (fh != MemorySpace::HOST) fc.migrate_to(fh);
        maybe_rebalance();
        return enc;
    }

    // Grouped cross-column WHERE: GROUP BY key_col, aggregate value_col, over rows where the u32 filter_col
    // satisfies the predicate. Borrows the distinct columns (key may == filter) to HOST, dispatches by the
    // value type, writes num_groups encoded results. ponytail: CPU path; a GPU grouped cross-column kernel
    // is the follow-up (the same-column grouped path already runs on the GPU).
    void grouped_cross_filter(const MatrixQuery& q, std::vector<uint64_t>& out) {
        std::vector<std::pair<uint64_t, MemorySpace>> borrowed;
        auto borrow = [&](uint64_t id) {
            for (auto& b : borrowed) if (b.first == id) return;   // distinct columns only (key may == filter)
            TieredColumn& c = *catalog_.at(id);
            const MemorySpace home = c.tier();
            borrowed.push_back({id, home});
            tier_mgr_.record_access(id, c.size_bytes());
            if (home != MemorySpace::HOST) { ++cold_borrows_; c.migrate_to(MemorySpace::HOST); }
        };
        borrow(q.value_col); borrow(q.filter_col); borrow(q.key_col);   // all HOST-resident after this
        const uint32_t* keys = reinterpret_cast<const uint32_t*>(catalog_.at(q.key_col)->host_ptr());
        const uint32_t* f    = reinterpret_cast<const uint32_t*>(catalog_.at(q.filter_col)->host_ptr());
        TieredColumn& vc = *catalog_.at(q.value_col);
        const size_t n = catalog_.at(q.key_col)->size_bytes() / sizeof(uint32_t);
        const MatrixPredicate p{q.cmp, q.threshold, q.upper};
        const uint32_t G = q.num_groups;
        out.assign(G, 0);
        const MatrixType vty = column_type(q.value_col);
        if (vty == MatrixType::I64) {
            std::vector<int64_t> tmp(G);
            matrix_cpu_group_reduce_filtered_by_i64(keys, f, p, reinterpret_cast<const int64_t*>(vc.host_ptr()), n, G, q.agg, tmp.data());
            for (uint32_t g = 0; g < G; ++g) out[g] = static_cast<uint64_t>(tmp[g]);
        } else if (vty == MatrixType::F64) {
            std::vector<double> tmp(G);
            matrix_cpu_group_reduce_filtered_by_f64(keys, f, p, reinterpret_cast<const double*>(vc.host_ptr()), n, G, q.agg, tmp.data());
            for (uint32_t g = 0; g < G; ++g) out[g] = matrix_bit_cast<uint64_t>(tmp[g]);
        } else {
            matrix_cpu_group_reduce_filtered_by(keys, f, p, reinterpret_cast<const uint32_t*>(vc.host_ptr()), n, G, q.agg, out.data());
        }
        for (auto it = borrowed.rbegin(); it != borrowed.rend(); ++it)   // return each distinct borrow to its home
            if (it->second != MemorySpace::HOST) catalog_.at(it->first)->migrate_to(it->second);
        maybe_rebalance();
    }

    MatrixQueryStatus execute_query_impl(const MatrixQuery& q, std::vector<uint64_t>& out) {
        out.clear();
        if (!catalog_has(q.value_col)) return MatrixQueryStatus::ERR_UNKNOWN_COLUMN;
        // Cross-column scalar WHERE (v1): filter on a different u32 column than the aggregate. Grouped
        // cross-column + value NULL-awareness on this path are the documented next increments.
        if (!q.grouped && q.has_filter && q.filter_col != 0 && q.filter_col != q.value_col) {
            if (!catalog_has(q.filter_col) || column_type(q.filter_col) != MatrixType::U32) return MatrixQueryStatus::ERR_UNSUPPORTED_TYPE;
            if (column_rows(q.filter_col) != column_rows(q.value_col)) return MatrixQueryStatus::ERR_INVALID_GROUP;   // misaligned columns
            out.assign(1, scalar_cross_filter(q));
            return MatrixQueryStatus::OK;
        }
        // Grouped cross-column WHERE (v1): GROUP BY a u32 key, aggregate value_col, filter on a different
        // u32 column. Same validation as the typed grouped paths (key u32, distinct, bounded, aligned).
        if (q.grouped && q.has_filter && q.filter_col != 0 && q.filter_col != q.value_col) {
            if (!catalog_has(q.key_col) || q.key_col == q.value_col || q.num_groups == 0) return MatrixQueryStatus::ERR_INVALID_GROUP;
            if (column_type(q.key_col) != MatrixType::U32) return MatrixQueryStatus::ERR_UNSUPPORTED_TYPE;
            if (!catalog_has(q.filter_col) || column_type(q.filter_col) != MatrixType::U32) return MatrixQueryStatus::ERR_UNSUPPORTED_TYPE;
            if (q.num_groups > max_query_groups_) return MatrixQueryStatus::ERR_TOO_MANY_GROUPS;
            if (column_rows(q.key_col) != column_rows(q.value_col) || column_rows(q.filter_col) != column_rows(q.value_col))
                return MatrixQueryStatus::ERR_INVALID_GROUP;   // misaligned columns
            grouped_cross_filter(q, out);
            return MatrixQueryStatus::OK;
        }
        if (column_type(q.value_col) == MatrixType::I64) {
            if (q.grouped) {
                // Key-type check FIRST: an int64 GROUP BY key is unsupported (DM-3d) regardless of whether
                // it aliases the value column — i64val+i64key must report ERR_UNSUPPORTED_TYPE (spec §1),
                // not the ERR_INVALID_GROUP that the key==value/row-count guard below would otherwise give.
                if (catalog_has(q.key_col) && column_type(q.key_col) != MatrixType::U32)
                    return MatrixQueryStatus::ERR_UNSUPPORTED_TYPE; // int64 key = DM-3d
                if (!catalog_has(q.key_col) || q.key_col == q.value_col || q.num_groups == 0
                    || column_rows(q.key_col) != column_rows(q.value_col))
                    return MatrixQueryStatus::ERR_INVALID_GROUP;
                if (q.num_groups > max_query_groups_) return MatrixQueryStatus::ERR_TOO_MANY_GROUPS;
                grouped_aggregate_i64(q.key_col, q.value_col, q.num_groups, q.agg,
                                      MatrixPredicateI64{q.cmp, q.lo_i64, q.hi_i64}, q.has_filter, out);
                return MatrixQueryStatus::OK;
            }
            if (null_masks_.count(q.value_col)) { out.assign(1, scalar_aggregate_nullable(q)); return MatrixQueryStatus::OK; }
            out.assign(1, static_cast<uint64_t>(
                scan_tiered_column_i64(q.value_col, MatrixPredicateI64{q.cmp, q.lo_i64, q.hi_i64}, q.agg, q.has_filter)));
            return MatrixQueryStatus::OK;
        }
        if (column_type(q.value_col) == MatrixType::F64) {
            if (q.grouped) {
                if (catalog_has(q.key_col) && column_type(q.key_col) != MatrixType::U32)
                    return MatrixQueryStatus::ERR_UNSUPPORTED_TYPE;                  // double key = later
                if (!catalog_has(q.key_col) || q.key_col == q.value_col || q.num_groups == 0
                    || column_rows(q.key_col) != column_rows(q.value_col))
                    return MatrixQueryStatus::ERR_INVALID_GROUP;
                if (q.num_groups > max_query_groups_) return MatrixQueryStatus::ERR_TOO_MANY_GROUPS;
                grouped_aggregate_f64(q.key_col, q.value_col, q.num_groups, q.agg,
                                      MatrixPredicateF64{q.cmp, q.lo_f64, q.hi_f64}, q.has_filter, out);
                return MatrixQueryStatus::OK;
            }
            if (null_masks_.count(q.value_col)) { out.assign(1, scalar_aggregate_nullable(q)); return MatrixQueryStatus::OK; }
            out.assign(1, matrix_bit_cast<uint64_t>(
                scan_tiered_column_f64(q.value_col, MatrixPredicateF64{q.cmp, q.lo_f64, q.hi_f64}, q.agg, q.has_filter)));
            return MatrixQueryStatus::OK;
        }
        if (q.grouped) {
            if (!catalog_has(q.key_col) || q.key_col == q.value_col || q.num_groups == 0
                || catalog_.at(q.key_col)->size_bytes() != catalog_.at(q.value_col)->size_bytes())
                return MatrixQueryStatus::ERR_INVALID_GROUP;
            // A typed (int64) GROUP BY key would be reinterpreted as uint32 by grouped_aggregate; an
            // 8N-byte int64 key of N rows even passes the byte-length guard above (== a 2N-row u32 value).
            // Reject it — typed-key grouping lands in DM-3b. (value_col is already known U32 here.)
            if (column_type(q.key_col) != MatrixType::U32) return MatrixQueryStatus::ERR_UNSUPPORTED_TYPE;
            if (q.num_groups > max_query_groups_) return MatrixQueryStatus::ERR_TOO_MANY_GROUPS;
            if (q.has_filter) grouped_aggregate_pred(q.key_col, q.value_col, q.num_groups, q.agg, MatrixPredicate{q.cmp, q.threshold, q.upper}, out);
            else              grouped_aggregate(q.key_col, q.value_col, q.num_groups, q.agg, out);
        } else {
            // Null-aware path: a column with a null mask skips NULL rows; the filter (if any) is applied too (DM-3j).
            if (null_masks_.count(q.value_col))
                out.assign(1, scalar_aggregate_nullable(q));
            else
                out.assign(1, scan_tiered_column(q.value_col, MatrixPredicate{q.cmp, q.threshold, q.upper}, q.agg, q.has_filter));
        }
        return MatrixQueryStatus::OK;
    }

public:
    ~CPUMockEngine() override {
        // Make the fixed-capacity overflow seam loud (not silent) even in release builds:
        // if any write was dropped because the KVStore filled, report it. Inc 3's SSD
        // spill removes the drop entirely; until then this is the visible failure signal.
        if (store_overflows_ > 0) {
            // Dropped writes = data loss → an ERROR-level diagnostic (prints at the default WARN threshold).
            Log::emit(LogLevel::ERROR, std::to_string(store_overflows_)
                      + " point-op writes dropped — KVStore full (Inc 3 adds SSD spill).");
        }
        std::cerr << "CPUMockEngine shutdown cleanly." << std::endl;
    }

    void execute_batch(DatabaseQuery* batch_array, size_t count) override {
        if (count == 0) return;
        if (count > MATRIX_BATCH_MAX) count = MATRIX_BATCH_MAX;

        // Bin by page (the step the dual-trigger batcher will eventually fold in).
        matrix_bin_by_page(batch_array, count, binned_.data(), offsets_.data());

        // One logical owner per page: process only this page's queries against its
        // contiguous store slice. Pages are independent — this is the parallel unit.
        // Scans arrive on a separate path (execute_scan), so this sees only point ops.
        for (size_t page = 0; page < MATRIX_PAGE_COUNT; ++page) {
            const uint32_t begin = offsets_[page];
            const uint32_t end = offsets_[page + 1];
            for (uint32_t j = begin; j < end; ++j) {
                DatabaseQuery& q = binned_[j];
                if (q.opcode == OP_READ) {
                    uint64_t v = 0;
                    kv_.get(q.query_id, v); // miss leaves v=0 (matches old zero-init store)
                    q.transaction_id = v;
                    ++reads_;
                } else if (q.opcode == OP_WRITE) {
                    // Durability invariant: append to the WAL FIRST (fsync per policy) so a
                    // write is only counted committed once it is durable. The in-memory kv_
                    // is volatile and rebuilt from the WAL on recovery.
                    if (cold_store_) cold_store_->append_put(q.query_id, q.query_id);
                    apply_committed_write(q.query_id, q.query_id);
                }
            }
        }

        // ponytail: read results land in binned_ (reordered), not scattered back to
        // batch_array. Callers here assert on counters + store contents, not on each
        // query's transaction_id, so the scatter-back is YAGNI. Add it if a caller
        // needs per-query read results in original order.
    }

    void execute_scan(DatabaseQuery& q) override {
        // id==0 -> the legacy fixed resident column; id>0 -> a tiered catalog column. The agg op
        // (default AGG_COUNT) selects the reduction; AGG_COUNT preserves the original count result.
        const uint32_t threshold = matrix_get_scan_threshold(q);
        const uint64_t col_id = matrix_get_scan_column_id(q);
        const MatrixAggOp op = matrix_get_scan_agg_op(q);
        const auto st0 = std::chrono::steady_clock::now();
        uint64_t c = 0;
        if (col_id == 0) {
            c = matrix_cpu_reduce(scan_column_.data(), MATRIX_SCAN_COLUMN_SIZE, threshold, op);
        } else {
            c = scan_tiered_column(col_id, MatrixPredicate{MatrixCmp::GT, threshold, 0}, op);
        }
        scan_time_s_ += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - st0).count();
        q.transaction_id = c;
        ++scans_;
        scan_result_sum_ += c;
    }

    uint64_t reads() const override { return reads_; }
    uint64_t writes() const override { return writes_; }
    uint64_t commits() const override { return commits_; }
    uint64_t scans() const override { return scans_; }
    uint64_t scan_result_sum() const override { return scan_result_sum_; }
    double scan_time_s() const override { return scan_time_s_; }
    // CPU has no launch/sync layer, so kernel time == the timed scan loop (zero overhead).
    double scan_kernel_time_s() const override { return scan_time_s_; }

    uint64_t store_checksum() const override {
        return kv_.checksum();
    }

    double benchmark_scan(size_t n, uint64_t threshold, uint64_t& out_count) override {
        std::vector<uint64_t> data(n);
        for (size_t i = 0; i < n; ++i) data[i] = i; // resident; fill not timed
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t count = 0;
        for (size_t i = 0; i < n; ++i) count += (data[i] > threshold);
        const auto t1 = std::chrono::steady_clock::now();
        out_count = count;
        return std::chrono::duration<double>(t1 - t0).count();
    }

    double benchmark_scan_u32(size_t n, uint32_t threshold, uint64_t& out_count) override {
        std::vector<uint32_t> data(n);
        for (size_t i = 0; i < n; ++i) data[i] = static_cast<uint32_t>(i);
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t count = 0;
        for (size_t i = 0; i < n; ++i) count += (data[i] > threshold);
        const auto t1 = std::chrono::steady_clock::now();
        out_count = count;
        return std::chrono::duration<double>(t1 - t0).count();
    }

    double benchmark_scan_u32x4(size_t n, uint32_t threshold, uint64_t& out_count) override {
        // ponytail: uint4 vectorized loads are a GPU concept; the CPU compiler already
        // auto-vectorizes the scalar loop, so this just delegates. Keeps the interface
        // uniform without faking a "CPU vectorized" path.
        return benchmark_scan_u32(n, threshold, out_count);
    }

    double benchmark_scan_ipt(size_t n, uint32_t threshold, uint64_t& out_count) override {
        // ponytail: register-blocking is a GPU latency-hiding lever; on CPU it's the same
        // auto-vectorized loop. Delegate.
        return benchmark_scan_u32(n, threshold, out_count);
    }

private:
    // True iff `id` names a real catalog column (id 0 is the legacy fixed column, never a query target).
    bool catalog_has(uint64_t id) const { return id != 0 && catalog_.count(id) != 0; }

    // Append `n` elements of raw bytes to catalog column `id`, growing it (borrow COLD->HOST, append,
    // return the borrow, update the brain's accounting). Private — typed wrappers enforce the element type.
    void append_raw(uint64_t id, const unsigned char* bytes, size_t byte_count) {
        TieredColumn& col = *catalog_.at(id);
        const MemorySpace home = col.tier();
        if (home != MemorySpace::HOST) { ++cold_borrows_; col.migrate_to(MemorySpace::HOST); }
        col.append_bytes(bytes, byte_count);
        if (home != MemorySpace::HOST) col.migrate_to(home);     // return the borrow (grown buffer)
        tier_mgr_.update_bytes(id, col.size_bytes());
    }

    // Scan one catalog column for value>threshold. A cold column is borrowed to HOST for the
    // scan then returned to its home tier, so the engine's residency always matches the brain's
    // accounting (no side-channel migration the budget can't see). Every REBALANCE_EVERY scans,
    // run the brain + executor: promote hot columns (DEVICE inert here), demote the coldest
    // HOST columns to SSD under the budget.
    // Null-aware scalar aggregate over a U32/I64/F64 column (DM-3j): borrow-and-return like
    // scan_tiered_column, but skip NULL rows via the column's null mask. When q.has_filter, the WHERE
    // predicate is applied too (the pred reducers null-check), so a NULL row is excluded whether or not it
    // would match. Returns the result as a uint64 (U32 zero-extended; I64/F64 as the bit pattern).
    uint64_t scalar_aggregate_nullable(const MatrixQuery& q) {
        const uint64_t col_id = q.value_col; const MatrixAggOp op = q.agg;
        TieredColumn& col = *catalog_.at(col_id);
        tier_mgr_.record_access(col_id, col.size_bytes());
        const MemorySpace home = col.tier();
        if (home != MemorySpace::HOST) { ++cold_borrows_; col.migrate_to(MemorySpace::HOST); }
        const uint8_t* nulls = null_masks_.at(col_id).data();
        const MatrixType ty = column_type(col_id);
        uint64_t result;
        if (ty == MatrixType::I64) {
            const auto* v = reinterpret_cast<const int64_t*>(col.host_ptr());
            const size_t nn = col.size_bytes() / sizeof(int64_t);
            const int64_t r = q.has_filter
                ? matrix_cpu_reduce_pred_i64(v, nn, MatrixPredicateI64{q.cmp, q.lo_i64, q.hi_i64}, op, nulls)
                : matrix_cpu_reduce_all_i64_nullable(v, nn, op, nulls);
            result = static_cast<uint64_t>(r);
        } else if (ty == MatrixType::F64) {
            const auto* v = reinterpret_cast<const double*>(col.host_ptr());
            const size_t nn = col.size_bytes() / sizeof(double);
            const double r = q.has_filter
                ? matrix_cpu_reduce_pred_f64(v, nn, MatrixPredicateF64{q.cmp, q.lo_f64, q.hi_f64}, op, nulls)
                : matrix_cpu_reduce_all_f64_nullable(v, nn, op, nulls);
            result = matrix_bit_cast<uint64_t>(r);
        } else {
            const auto* v = reinterpret_cast<const uint32_t*>(col.host_ptr());
            const size_t nn = col.size_bytes() / sizeof(uint32_t);
            result = q.has_filter
                ? matrix_cpu_reduce_pred(v, nn, MatrixPredicate{q.cmp, q.threshold, q.upper}, op, nulls)
                : matrix_cpu_reduce_all_nullable(v, nn, op, nulls);
        }
        if (home != MemorySpace::HOST) col.migrate_to(home);
        maybe_rebalance();
        return result;
    }

    uint64_t scan_tiered_column(uint64_t col_id, MatrixPredicate pred, MatrixAggOp op, bool has_filter = true) {
        auto it = catalog_.find(col_id);
        if (it == catalog_.end()) {
            assert(false && "scan of unregistered column id"); // debug: catch the caller bug
            std::cerr << "CPUMockEngine: scan of unregistered column id " << col_id
                      << " — empty result\n";                 // release: diagnosable, no null-deref
            return 0;
        }
        TieredColumn& col = *it->second;
        tier_mgr_.record_access(col_id, col.size_bytes());          // heat signal
#if defined(MATRIX_USE_CUDA)
        if (col.tier() == MemorySpace::DEVICE) {                    // GPU-3: reduce in VRAM, no borrow-down
            const size_t nvals = col.size_bytes() / sizeof(uint32_t);
            const uint64_t result = matrix_gpu_reduce_dev_u32(col.device_ptr(), nvals, pred, op, has_filter);
            maybe_rebalance();
            return result;
        }
#endif

        const MemorySpace home = col.tier();
        if (home != MemorySpace::HOST) { ++cold_borrows_; col.migrate_to(MemorySpace::HOST); } // pull SSD->RAM to scan
        const uint32_t* vals = reinterpret_cast<const uint32_t*>(col.host_ptr());
        const size_t nvals = col.size_bytes() / sizeof(uint32_t);
        const uint64_t result = has_filter ? matrix_cpu_reduce_pred(vals, nvals, pred, op)
                                           : matrix_cpu_reduce_all(vals, nvals, op);
        // ponytail: returning the borrow rewrites the COLD file each cold scan; skip-if-unchanged
        // (or a TierManager note_residency) is the upgrade path if cold-scan churn ever matters.
        if (home != MemorySpace::HOST) col.migrate_to(home);        // return the borrow

        maybe_rebalance();
        return result;
    }

    // Every REBALANCE_EVERY scans, run the brain + executor: promote hot (DEVICE inert here), demote the
    // coldest HOST columns to SSD under the budget. Shared by the u32 and int64 scan paths.
    void maybe_rebalance() {
        if (++scans_since_rebalance_ >= rebalance_every_) {
            std::unordered_map<uint64_t, TieredColumn*> ptrs;
            for (auto& kv : catalog_) ptrs[kv.first] = kv.second.get();
            migrations_ += executor_.apply(tier_mgr_.rebalance(), ptrs);
            ++rebalances_;
            scans_since_rebalance_ = 0;
        }
    }

    // int64 scalar scan over a catalog column (DM-3a unfiltered; DM-3b adds the filtered path). Same
    // borrow-to-HOST-and-return as scan_tiered_column: record heat, pull a COLD column to RAM, reinterpret
    // the raw bytes as int64, reduce by op (filtered when has_filter, else over all), return the borrow to
    // its home tier, and drive the shared rebalance cadence (int64 scans now count too — DM-3a follow-up).
    int64_t scan_tiered_column_i64(uint64_t col_id, MatrixPredicateI64 pred, MatrixAggOp op, bool has_filter = false) {
        TieredColumn& col = *catalog_.at(col_id);
        tier_mgr_.record_access(col_id, col.size_bytes());
#if defined(MATRIX_USE_CUDA)
        if (col.tier() == MemorySpace::DEVICE) {                    // GPU-3: reduce in VRAM, no borrow-down
            const size_t nvals = col.size_bytes() / sizeof(int64_t);
            const int64_t result = matrix_gpu_reduce_dev_i64(col.device_ptr(), nvals, pred, op, has_filter);
            maybe_rebalance();
            return result;
        }
#endif
        const MemorySpace home = col.tier();
        if (home != MemorySpace::HOST) { ++cold_borrows_; col.migrate_to(MemorySpace::HOST); }
        const int64_t* vals = reinterpret_cast<const int64_t*>(col.host_ptr());
        const size_t nvals = col.size_bytes() / sizeof(int64_t);
        const int64_t result = has_filter ? matrix_cpu_reduce_pred_i64(vals, nvals, pred, op)
                                          : matrix_cpu_reduce_all_i64(vals, nvals, op);
        if (home != MemorySpace::HOST) col.migrate_to(home);
        maybe_rebalance();
        return result;
    }

    // double scalar scan over a catalog column (DM-3e). Same borrow-to-HOST-and-return as
    // scan_tiered_column_i64: record heat, pull a COLD column to RAM, reinterpret the raw bytes as
    // double, reduce by op (filtered when has_filter, else over all), return the borrow to its home
    // tier, and drive the shared rebalance cadence.
    double scan_tiered_column_f64(uint64_t col_id, MatrixPredicateF64 pred, MatrixAggOp op, bool has_filter = false) {
        TieredColumn& col = *catalog_.at(col_id);
        tier_mgr_.record_access(col_id, col.size_bytes());
#if defined(MATRIX_USE_CUDA)
        if (col.tier() == MemorySpace::DEVICE) {                    // GPU-3: reduce in VRAM, no borrow-down
            const size_t nvals = col.size_bytes() / sizeof(double);
            const double result = matrix_gpu_reduce_dev_f64(col.device_ptr(), nvals, pred, op, has_filter);
            maybe_rebalance();
            return result;
        }
#endif
        const MemorySpace home = col.tier();
        if (home != MemorySpace::HOST) { ++cold_borrows_; col.migrate_to(MemorySpace::HOST); }
        const double* vals = reinterpret_cast<const double*>(col.host_ptr());
        const size_t nvals = col.size_bytes() / sizeof(double);
        const double result = has_filter ? matrix_cpu_reduce_pred_f64(vals, nvals, pred, op)
                                         : matrix_cpu_reduce_all_f64(vals, nvals, op);
        if (home != MemorySpace::HOST) col.migrate_to(home);
        maybe_rebalance();
        return result;
    }

    // Point-op store: a real hash table (gap DM-1). Distinct keys never overwrite; a full
    // table is an explicit error, not silent loss. Sized with headroom over the demo's
    // distinct write-keys; real capacity / SSD-spill is gap DM-9 / Inc 3 (the seam).
    KVStore kv_{1u << 16}; // 65536 slots
    // Ordered secondary index (DM-7): key -> value, mirrors kv_ (maintained on commit, rebuilt on recovery).
    // Enables log-time range scans (kv_range_sorted) vs kv_range's O(n) full scan.
    // ponytail: a std::map mirror — doubles point-op key memory + a map insert per commit; a packed
    // B-tree/ART would be denser, and the index is rebuilt from kv_ on restart (not separately persisted).
    std::map<uint64_t, uint64_t> key_index_;
    std::unique_ptr<ColdStore> cold_store_; // null = durability off (default); set via WAL path
    std::string checkpoint_path_;     // <wal_path>.ckpt — last point-op compaction snapshot
    uint64_t checkpoints_ = 0;        // DU-4: number of WAL compactions performed
    // --- live tiering (INT-1): a catalog of analytical columns the brain auto-tiers ---
    static constexpr uint64_t REBALANCE_EVERY = 4;     // default: rebalance every N tiered scans
    uint64_t rebalance_every_ = REBALANCE_EVERY;       // OB-4: runtime-tunable rebalance cadence (default = the constant)
    static constexpr uint32_t MATRIX_CATALOG_MAGIC = 0x4D434132u; // 'MCA2' — typed+named catalog snapshot v2 (DM-2b)
    static constexpr uint32_t MATRIX_CKPT_MAGIC = 0x4D434B50u; // 'MCKP' — point-op checkpoint file
    static constexpr uint32_t MAX_QUERY_GROUPS = 1u << 28; // default grouped-query num_groups ceiling (out alloc guard)
    uint32_t max_query_groups_ = MAX_QUERY_GROUPS;     // RM-2: runtime-tunable admission cap (default = the constant)
    TierManager tier_mgr_;                              // decides migrations (heat-driven)
    MigrationExecutor executor_;                        // moves the bytes per decision
    std::unordered_map<uint64_t, std::unique_ptr<TieredColumn>> catalog_; // id -> column
    std::unordered_map<uint64_t, MatrixType> col_types_; // id -> element type (absent ⇒ U32); DM-3a
    std::unordered_map<uint64_t, std::vector<std::string>> string_columns_; // DM-3i: separate string-column store
    std::unordered_map<uint64_t, std::vector<std::string>> string_dicts_;   // dict-encoded: code -> string (decode)
    std::unordered_map<uint64_t, std::unordered_map<std::string, uint32_t>> string_encoders_; // string -> code (encode/filter)
    std::unordered_map<uint64_t, std::vector<uint8_t>> null_masks_;          // DM-3j: id -> per-row NULL flag (1=null)
    std::unordered_map<uint64_t, std::string> column_names_;   // id -> optional name
    std::unordered_map<std::string, uint64_t> name_to_id_;     // name -> id (resolve)
    std::unordered_map<std::string, std::vector<uint64_t>> tables_;   // table name -> ordered column ids (DM-2c)
    uint64_t scans_since_rebalance_ = 0;
    uint64_t cold_borrows_ = 0;    // observability: COLD->HOST borrows
    uint64_t rebalances_ = 0;      // observability: rebalance passes
    uint64_t migrations_ = 0;      // observability: migration decisions executed
    // Record one query's latency into the (atomic) read-path stats. Shared by execute_query and
    // execute_query_shared, so it is safe under concurrent readers (relaxed atomics; single-threaded
    // values are unchanged). OB-2b: bucket by floor(log2(ns+1)) for percentile estimation.
    void record_query_latency(uint64_t ns) {
        query_count_.fetch_add(1, std::memory_order_relaxed);
        total_query_ns_.fetch_add(ns, std::memory_order_relaxed);
        for (uint64_t cur = max_query_ns_.load(std::memory_order_relaxed);
             ns > cur && !max_query_ns_.compare_exchange_weak(cur, ns, std::memory_order_relaxed); ) {}
        uint64_t x = ns + 1, b = 0; while (x > 1 && b < LAT_BUCKETS - 1) { x >>= 1; ++b; }
        latency_hist_[b].fetch_add(1, std::memory_order_relaxed);
    }

    // Read-path stats: bumped by every execute_query / execute_query_shared, so atomic for concurrent
    // readers (relaxed — heuristics; values are identical single-threaded). All other counters are
    // exclusive-path-only and stay plain.
    std::atomic<uint64_t> query_count_{0};     // observability: execute_query calls served (OK and ERR)
    std::atomic<uint64_t> total_query_ns_{0};  // observability: summed execute_query wall-time (ns)
    std::atomic<uint64_t> max_query_ns_{0};    // observability: slowest single execute_query (ns)
    static constexpr int LAT_BUCKETS = 40;                  // log2 latency buckets (OB-2b); 2^39 ns ≈ 9 min
    std::array<std::atomic<uint64_t>, LAT_BUCKETS> latency_hist_{};   // per-query latency histogram (bucket = floor(log2(ns+1)))
    std::vector<DatabaseQuery> binned_;                // scratch: batch sorted by page
    std::array<uint32_t, MATRIX_PAGE_COUNT + 1> offsets_{}; // CSR page offsets
    std::vector<uint32_t> scan_column_;                // resident analytical column
    uint64_t reads_ = 0;
    uint64_t writes_ = 0;
    uint64_t commits_ = 0;
    uint64_t scans_ = 0;
    uint64_t scan_result_sum_ = 0;
    uint64_t store_overflows_ = 0; // writes dropped because the fixed-capacity KVStore was full (Inc 3 adds SSD spill)
    double scan_time_s_ = 0.0;

    // Apply one durable write to the point-op store with the standard overflow accounting.
    // mock projection: value == key. Fixed-capacity seam: a full table is counted as an
    // overflow (always live, even under NDEBUG) so a dropped write is never silent. Inc 3
    // replaces this with SSD spill; the assert makes it fail loud in debug builds too.
    void apply_committed_write(uint64_t key, uint64_t value) {
        ++writes_;
        if (kv_.put(key, value)) { ++commits_; key_index_[key] = value; }   // mirror into the ordered secondary index
        else { ++store_overflows_; assert(false && "KVStore full — point-op store capacity exceeded (Inc 3 adds spill)"); }
    }

    // --- Atomic transactions (WAL group commit) ---
    std::vector<std::pair<uint64_t, uint64_t>> txn_buf_; // pending writes in the open transaction
    bool in_txn_ = false;
    uint64_t transactions_committed_ = 0;
    uint64_t transactions_rolled_back_ = 0;
};

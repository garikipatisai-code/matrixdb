#include "compute.hpp"
#include "kv_store.hpp"
#include "cold_store.hpp"
#include "migration_executor.hpp"  // MigrationExecutor + TierManager + TieredColumn + CostModel
#include "memory_model.hpp"        // MemorySpace, MemoryModel
#include "column_io.hpp"           // matrix_write_column / matrix_read_column (binary column persistence)
#include "csv_ingest.hpp"          // matrix_read_csv_column (CSV column ingest, graceful on bad input)
#include <unordered_map>
#include <memory>
#include <string>
#include <vector>
#include <array>
#include <cassert>
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
};

class CPUMockEngine : public ComputeInterface {
public:
    // host_cap is the RAM budget (bytes) for the tiered catalog; default unbounded so the
    // existing pipeline (empty catalog) is unaffected. device_cap=1 makes the DEVICE tier
    // inert on the CPU build: scan_us() ignores gpu_available, so the brain would otherwise
    // emit HOST->DEVICE promotions the CPU executor's migrate_to(DEVICE) aborts on; a 1-byte
    // cap means no real column ever fits, so no DEVICE decision is emitted (cap==0 == unbounded).
    explicit CPUMockEngine(size_t /*worker_count*/ = 0, std::string wal_path = "",
                           size_t host_cap = SIZE_MAX)
        : tier_mgr_(CostModel(MemoryModel::detect(false), false), /*device_cap=*/1, host_cap)
        , binned_(MATRIX_BATCH_MAX)
        , scan_column_(MATRIX_SCAN_COLUMN_SIZE) {
        for (size_t i = 0; i < MATRIX_SCAN_COLUMN_SIZE; ++i)
            scan_column_[i] = static_cast<uint32_t>(i); // resident analytical column
        // Durability is opt-in: with a WAL path, recover the point-op store by replaying
        // the log into kv_ (a write committed before a crash is restored here).
        if (!wal_path.empty()) {
            cold_store_ = std::make_unique<ColdStore>(wal_path);
            cold_store_->replay([this](uint64_t k, uint64_t v){ kv_.put(k, v); });
        }
        std::cout << "CPUMockEngine initialized (page-ownership, "
                  << MATRIX_PAGE_COUNT << " pages, "
                  << MATRIX_SCAN_COLUMN_SIZE << "-value scan column"
                  << (cold_store_ ? ", WAL durability ON" : "") << ")." << std::endl;
    }

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

    // Inspection (tests): where the bytes actually live vs where the brain believes, the
    // HOST bytes the brain is accounting for, and a column's integrity checksum.
    MemorySpace column_tier(uint64_t id) const { return catalog_.at(id)->tier(); }
    MemorySpace manager_tier(uint64_t id) const { return tier_mgr_.tier_of(id); }
    size_t host_resident_bytes() const { return tier_mgr_.resident_bytes(MemorySpace::HOST); }
    uint64_t column_checksum(uint64_t id) const { return catalog_.at(id)->checksum(); }

    // Point-op read accessor (tests): true + sets out if present. Mirrors execute_batch's OP_READ.
    bool kv_get(uint64_t key, uint64_t& out) const { return kv_.get(key, out); }

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

    // Observability snapshot (counters since construction + current resident-bytes gauges).
    EngineStats stats() const {
        return EngineStats{ cold_borrows_, rebalances_, migrations_, catalog_.size(),
                            tier_mgr_.resident_bytes(MemorySpace::HOST),
                            tier_mgr_.resident_bytes(MemorySpace::COLD) };
    }

    // Persist a catalog column's bytes to `path` (borrows to HOST to read, returns the borrow).
    void save_column(uint64_t id, const std::string& path) {
        TieredColumn& col = *catalog_.at(id);
        const MemorySpace home = col.tier();
        if (home != MemorySpace::HOST) { ++cold_borrows_; col.migrate_to(MemorySpace::HOST); }
        matrix_write_column(path, reinterpret_cast<const uint32_t*>(col.host_ptr()),
                            col.size_bytes() / sizeof(uint32_t));
        if (home != MemorySpace::HOST) col.migrate_to(home);
    }
    // Load a column file into the catalog under `id` (born resident in HOST, like load_scan_column).
    void load_column_from_file(uint64_t id, const std::string& path) {
        std::vector<uint32_t> data;
        matrix_read_column(path, data);
        load_scan_column(id, data.data(), data.size());
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
            const uint64_t count = col.size_bytes() / sizeof(uint32_t);
            const uint32_t* data = reinterpret_cast<const uint32_t*>(col.host_ptr());
            ok = std::fwrite(&id, sizeof id, 1, f) == 1
              && std::fwrite(&count, sizeof count, 1, f) == 1
              && (count == 0 || std::fwrite(data, sizeof(uint32_t), count, f) == count);
            if (home != MemorySpace::HOST) col.migrate_to(home);
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
        std::vector<uint32_t> data;
        for (uint64_t c = 0; ok && c < ncols; ++c) {
            uint64_t id = 0, count = 0;
            ok = std::fread(&id, sizeof id, 1, f) == 1 && std::fread(&count, sizeof count, 1, f) == 1;
            if (!ok) break;
            data.resize(static_cast<size_t>(count));
            ok = (count == 0 || std::fread(data.data(), sizeof(uint32_t), data.size(), f) == data.size());
            if (ok) load_scan_column(id, data.data(), data.size());
        }
        std::fclose(f);
        if (!ok) { std::fprintf(stderr, "load_catalog: bad/short snapshot %s\n", path.c_str()); std::abort(); }
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

        const MemorySpace kh = kc.tier(); if (kh != MemorySpace::HOST) { ++cold_borrows_; kc.migrate_to(MemorySpace::HOST); }
        const MemorySpace vh = vc.tier(); if (vh != MemorySpace::HOST) { ++cold_borrows_; vc.migrate_to(MemorySpace::HOST); }
        const uint32_t* keys = reinterpret_cast<const uint32_t*>(kc.host_ptr());
        const uint32_t* vals = reinterpret_cast<const uint32_t*>(vc.host_ptr());
        const size_t n = kc.size_bytes() / sizeof(uint32_t);
        out.resize(num_groups);   // matrix_cpu_group_reduce initializes every slot per op (MIN sentinel ≠ 0)
        matrix_cpu_group_reduce(keys, vals, n, num_groups, op, out.data());
        if (vh != MemorySpace::HOST) vc.migrate_to(vh);       // return borrows
        if (kh != MemorySpace::HOST) kc.migrate_to(kh);
    }

    // GROUP BY key WHERE value > threshold (filtered grouped aggregate). Same contract and double
    // borrow-and-return as grouped_aggregate; only rows with value > threshold contribute.
    void grouped_aggregate_where(uint64_t key_id, uint64_t value_id, uint32_t num_groups,
                                 MatrixAggOp op, uint32_t threshold, std::vector<uint64_t>& out) {
        assert(key_id != value_id && "group-by key and value must be distinct columns");
        TieredColumn& kc = *catalog_.at(key_id);
        TieredColumn& vc = *catalog_.at(value_id);
        assert(kc.size_bytes() == vc.size_bytes() && "key and value columns must be the same length");
        tier_mgr_.record_access(key_id, kc.size_bytes());
        tier_mgr_.record_access(value_id, vc.size_bytes());
        const MemorySpace kh = kc.tier(); if (kh != MemorySpace::HOST) { ++cold_borrows_; kc.migrate_to(MemorySpace::HOST); }
        const MemorySpace vh = vc.tier(); if (vh != MemorySpace::HOST) { ++cold_borrows_; vc.migrate_to(MemorySpace::HOST); }
        const uint32_t* keys = reinterpret_cast<const uint32_t*>(kc.host_ptr());
        const uint32_t* vals = reinterpret_cast<const uint32_t*>(vc.host_ptr());
        const size_t n = kc.size_bytes() / sizeof(uint32_t);
        out.resize(num_groups);
        matrix_cpu_group_reduce_where(keys, vals, n, num_groups, op, threshold, out.data());
        if (vh != MemorySpace::HOST) vc.migrate_to(vh);
        if (kh != MemorySpace::HOST) kc.migrate_to(kh);
    }

    // Unified analytical query over catalog columns. Validates input at the boundary and returns a
    // status (never crashes on caller input); on any ERR, out is cleared. Scalar -> out[0];
    // grouped -> out[0..num_groups). Catalog columns only (the legacy id-0 fixed column is the
    // benchmark fixture, not a query target).
    MatrixQueryStatus execute_query(const MatrixQuery& q, std::vector<uint64_t>& out) {
        out.clear();
        if (!catalog_has(q.value_col)) return MatrixQueryStatus::ERR_UNKNOWN_COLUMN;
        if (q.grouped) {
            if (!catalog_has(q.key_col) || q.key_col == q.value_col || q.num_groups == 0
                || catalog_.at(q.key_col)->size_bytes() != catalog_.at(q.value_col)->size_bytes())
                return MatrixQueryStatus::ERR_INVALID_GROUP;
            if (q.num_groups > MAX_QUERY_GROUPS) return MatrixQueryStatus::ERR_TOO_MANY_GROUPS;
            if (q.has_filter) grouped_aggregate_where(q.key_col, q.value_col, q.num_groups, q.agg, q.threshold, out);
            else              grouped_aggregate(q.key_col, q.value_col, q.num_groups, q.agg, out);
        } else {
            out.assign(1, scan_tiered_column(q.value_col, q.threshold, q.agg, q.has_filter));
        }
        return MatrixQueryStatus::OK;
    }

    ~CPUMockEngine() override {
        // Make the fixed-capacity overflow seam loud (not silent) even in release builds:
        // if any write was dropped because the KVStore filled, report it. Inc 3's SSD
        // spill removes the drop entirely; until then this is the visible failure signal.
        if (store_overflows_ > 0) {
            std::cout << "WARNING: " << store_overflows_
                      << " point-op writes dropped — KVStore full (Inc 3 adds SSD spill)."
                      << std::endl;
        }
        std::cout << "CPUMockEngine shutdown cleanly." << std::endl;
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
            c = scan_tiered_column(col_id, threshold, op);
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

    // Scan one catalog column for value>threshold. A cold column is borrowed to HOST for the
    // scan then returned to its home tier, so the engine's residency always matches the brain's
    // accounting (no side-channel migration the budget can't see). Every REBALANCE_EVERY scans,
    // run the brain + executor: promote hot columns (DEVICE inert here), demote the coldest
    // HOST columns to SSD under the budget.
    uint64_t scan_tiered_column(uint64_t col_id, uint32_t threshold, MatrixAggOp op, bool has_filter = true) {
        auto it = catalog_.find(col_id);
        if (it == catalog_.end()) {
            assert(false && "scan of unregistered column id"); // debug: catch the caller bug
            std::cerr << "CPUMockEngine: scan of unregistered column id " << col_id
                      << " — empty result\n";                 // release: diagnosable, no null-deref
            return 0;
        }
        TieredColumn& col = *it->second;
        tier_mgr_.record_access(col_id, col.size_bytes());          // heat signal

        const MemorySpace home = col.tier();
        if (home != MemorySpace::HOST) { ++cold_borrows_; col.migrate_to(MemorySpace::HOST); } // pull SSD->RAM to scan
        const uint32_t* vals = reinterpret_cast<const uint32_t*>(col.host_ptr());
        const size_t nvals = col.size_bytes() / sizeof(uint32_t);
        const uint64_t result = has_filter ? matrix_cpu_reduce(vals, nvals, threshold, op)
                                           : matrix_cpu_reduce_all(vals, nvals, op);
        // ponytail: returning the borrow rewrites the COLD file each cold scan; skip-if-unchanged
        // (or a TierManager note_residency) is the upgrade path if cold-scan churn ever matters.
        if (home != MemorySpace::HOST) col.migrate_to(home);        // return the borrow

        if (++scans_since_rebalance_ >= REBALANCE_EVERY) {
            std::unordered_map<uint64_t, TieredColumn*> ptrs;
            for (auto& kv : catalog_) ptrs[kv.first] = kv.second.get();
            migrations_ += executor_.apply(tier_mgr_.rebalance(), ptrs);
            ++rebalances_;
            scans_since_rebalance_ = 0;
        }
        return result;
    }

    // Point-op store: a real hash table (gap DM-1). Distinct keys never overwrite; a full
    // table is an explicit error, not silent loss. Sized with headroom over the demo's
    // distinct write-keys; real capacity / SSD-spill is gap DM-9 / Inc 3 (the seam).
    KVStore kv_{1u << 16}; // 65536 slots
    std::unique_ptr<ColdStore> cold_store_; // null = durability off (default); set via WAL path
    // --- live tiering (INT-1): a catalog of analytical columns the brain auto-tiers ---
    static constexpr uint64_t REBALANCE_EVERY = 4;     // rebalance every N tiered scans
    static constexpr uint32_t MATRIX_CATALOG_MAGIC = 0x4D434154u; // 'MCAT' — catalog snapshot v0
    static constexpr uint32_t MAX_QUERY_GROUPS = 1u << 28; // grouped-query num_groups ceiling (out alloc guard)
    TierManager tier_mgr_;                              // decides migrations (heat-driven)
    MigrationExecutor executor_;                        // moves the bytes per decision
    std::unordered_map<uint64_t, std::unique_ptr<TieredColumn>> catalog_; // id -> column
    uint64_t scans_since_rebalance_ = 0;
    uint64_t cold_borrows_ = 0;    // observability: COLD->HOST borrows
    uint64_t rebalances_ = 0;      // observability: rebalance passes
    uint64_t migrations_ = 0;      // observability: migration decisions executed
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
        if (kv_.put(key, value)) ++commits_;
        else { ++store_overflows_; assert(false && "KVStore full — point-op store capacity exceeded (Inc 3 adds spill)"); }
    }

    // --- Atomic transactions (WAL group commit) ---
    std::vector<std::pair<uint64_t, uint64_t>> txn_buf_; // pending writes in the open transaction
    bool in_txn_ = false;
    uint64_t transactions_committed_ = 0;
    uint64_t transactions_rolled_back_ = 0;
};

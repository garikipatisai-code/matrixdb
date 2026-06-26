#include "compute.hpp"
#include "kv_store.hpp"
#include "cold_store.hpp"
#include "migration_executor.hpp"  // MigrationExecutor + TierManager + TieredColumn + CostModel
#include "memory_model.hpp"        // MemorySpace, MemoryModel
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
                    // mock projection: value == key. Fixed-capacity seam: a full table is
                    // counted as an overflow (always live, even under NDEBUG) so a dropped
                    // write is never silent. Inc 3 replaces this with SSD spill. The assert
                    // makes it fail loud in debug builds too.
                    ++writes_;
                    if (kv_.put(q.query_id, q.query_id)) {
                        ++commits_;
                    } else {
                        ++store_overflows_;
                        assert(false && "KVStore full — point-op store capacity exceeded (Inc 3 adds spill)");
                    }
                }
            }
        }

        // ponytail: read results land in binned_ (reordered), not scattered back to
        // batch_array. Callers here assert on counters + store contents, not on each
        // query's transaction_id, so the scatter-back is YAGNI. Add it if a caller
        // needs per-query read results in original order.
    }

    void execute_scan(DatabaseQuery& q) override {
        // id==0 -> the legacy fixed resident column (unchanged); id>0 -> a tiered catalog column.
        const uint32_t threshold = matrix_get_scan_threshold(q);
        const uint64_t col_id = matrix_get_scan_column_id(q);
        const auto st0 = std::chrono::steady_clock::now();
        uint64_t c = 0;
        if (col_id == 0) {
            for (size_t s2 = 0; s2 < MATRIX_SCAN_COLUMN_SIZE; ++s2)
                c += (scan_column_[s2] > threshold);
        } else {
            c = scan_tiered_column(col_id, threshold);
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
    // Scan one catalog column for value>threshold. A cold column is borrowed to HOST for the
    // scan then returned to its home tier, so the engine's residency always matches the brain's
    // accounting (no side-channel migration the budget can't see). Every REBALANCE_EVERY scans,
    // run the brain + executor: promote hot columns (DEVICE inert here), demote the coldest
    // HOST columns to SSD under the budget.
    uint64_t scan_tiered_column(uint64_t col_id, uint32_t threshold) {
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
        if (home != MemorySpace::HOST) col.migrate_to(MemorySpace::HOST); // pull SSD->RAM to scan
        const uint32_t* vals = reinterpret_cast<const uint32_t*>(col.host_ptr());
        const size_t nvals = col.size_bytes() / sizeof(uint32_t);
        uint64_t c = 0;
        for (size_t i = 0; i < nvals; ++i) c += (vals[i] > threshold);
        // ponytail: returning the borrow rewrites the COLD file each cold scan; skip-if-unchanged
        // (or a TierManager note_residency) is the upgrade path if cold-scan churn ever matters.
        if (home != MemorySpace::HOST) col.migrate_to(home);        // return the borrow

        if (++scans_since_rebalance_ >= REBALANCE_EVERY) {
            std::unordered_map<uint64_t, TieredColumn*> ptrs;
            for (auto& kv : catalog_) ptrs[kv.first] = kv.second.get();
            executor_.apply(tier_mgr_.rebalance(), ptrs);
            scans_since_rebalance_ = 0;
        }
        return c;
    }

    // Point-op store: a real hash table (gap DM-1). Distinct keys never overwrite; a full
    // table is an explicit error, not silent loss. Sized with headroom over the demo's
    // distinct write-keys; real capacity / SSD-spill is gap DM-9 / Inc 3 (the seam).
    KVStore kv_{1u << 16}; // 65536 slots
    std::unique_ptr<ColdStore> cold_store_; // null = durability off (default); set via WAL path
    // --- live tiering (INT-1): a catalog of analytical columns the brain auto-tiers ---
    static constexpr uint64_t REBALANCE_EVERY = 4;     // rebalance every N tiered scans
    TierManager tier_mgr_;                              // decides migrations (heat-driven)
    MigrationExecutor executor_;                        // moves the bytes per decision
    std::unordered_map<uint64_t, std::unique_ptr<TieredColumn>> catalog_; // id -> column
    uint64_t scans_since_rebalance_ = 0;
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
};

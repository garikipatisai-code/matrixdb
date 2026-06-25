#include "compute.hpp"
#include <vector>
#include <array>
#include <chrono>
#include <iostream>

/**
 * @brief CPU Mock Engine — the no-GPU fallback (Component 5: Local Sandbox).
 * Page-ownership model: bins the batch by owning page, then processes each page's
 * queries against its own slice of the store. One owner per page ⇒ no atomics, no
 * delta log, deterministic last-writer-wins. Mirrors the CUDA engine's semantics so
 * both produce identical store contents.
 */
class CPUMockEngine : public ComputeInterface {
public:
    explicit CPUMockEngine(size_t /*worker_count*/ = 0)
        : binned_(MATRIX_BATCH_MAX) {
        std::cout << "CPUMockEngine initialized (page-ownership, "
                  << MATRIX_PAGE_COUNT << " pages)." << std::endl;
    }

    ~CPUMockEngine() override {
        std::cout << "CPUMockEngine shutdown cleanly." << std::endl;
    }

    void execute_batch(DatabaseQuery* batch_array, size_t count) override {
        if (count == 0) return;
        if (count > MATRIX_BATCH_MAX) count = MATRIX_BATCH_MAX;

        // Bin by page (the step the dual-trigger batcher will eventually fold in).
        matrix_bin_by_page(batch_array, count, binned_.data(), offsets_.data());

        // One logical owner per page: process only this page's queries against its
        // contiguous store slice. Pages are independent — this is the parallel unit.
        for (size_t page = 0; page < MATRIX_PAGE_COUNT; ++page) {
            const uint32_t begin = offsets_[page];
            const uint32_t end = offsets_[page + 1];
            for (uint32_t j = begin; j < end; ++j) {
                DatabaseQuery& q = binned_[j];
                const size_t slot = q.query_id & MATRIX_STORE_MASK;
                if (q.opcode == OP_READ) {
                    q.transaction_id = store_[slot];
                    ++reads_;
                } else if (q.opcode == OP_WRITE) {
                    store_[slot] = q.query_id; // mock projection: value == key
                    ++writes_;
                    ++commits_;
                }
            }
        }

        // ponytail: read results land in binned_ (reordered), not scattered back to
        // batch_array. Callers here assert on counters + store contents, not on each
        // query's transaction_id, so the scatter-back is YAGNI. Add it if a caller
        // needs per-query read results in original order.
    }

    uint64_t reads() const override { return reads_; }
    uint64_t writes() const override { return writes_; }
    uint64_t commits() const override { return commits_; }

    uint64_t store_checksum() const override {
        uint64_t sum = 0;
        for (uint64_t v : store_) sum += v;
        return sum;
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
    std::array<uint64_t, MATRIX_STORE_SLOTS> store_{}; // the Value column
    std::vector<DatabaseQuery> binned_;                // scratch: batch sorted by page
    std::array<uint32_t, MATRIX_PAGE_COUNT + 1> offsets_{}; // CSR page offsets
    uint64_t reads_ = 0;
    uint64_t writes_ = 0;
    uint64_t commits_ = 0;
};

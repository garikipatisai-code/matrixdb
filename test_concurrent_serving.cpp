// Concurrent serving (single-writer / many-readers). Task 2: execute_query_shared semantics — for an
// all-HOST, non-null-masked query it serves the SAME result as execute_query with no tier side effects;
// for anything it can't serve purely (here: a null-masked column) it returns NEEDS_EXCLUSIVE without
// mutating. (Tasks 3+ add the concurrent-readers + ConcurrentServer tests.)
#include "compute_mock.cpp"
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <atomic>
#include <thread>

static const MatrixAggOp OPS[] = {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX};

static void check_equiv(CPUMockEngine& eng, const MatrixQuery& q, const char* name) {
    std::vector<uint64_t> a, b;
    assert(eng.execute_query_shared(q, a) == CPUMockEngine::ReadOutcome::SERVED && "expected SERVED (all-HOST)");
    assert(eng.execute_query(q, b) == MatrixQueryStatus::OK && a == b && "shared result == exclusive result");
    (void)name;
}

int main() {
    CPUMockEngine eng;                                    // default host_cap = unbounded -> columns stay HOST
    const size_t N = 1u << 16;
    std::vector<uint32_t> u(N), key(N); std::vector<int64_t> vi(N); std::vector<double> vf(N);
    for (size_t i = 0; i < N; ++i) {
        u[i]   = static_cast<uint32_t>(i % 1000);
        key[i] = static_cast<uint32_t>(i % 4);
        vi[i]  = (static_cast<int64_t>(i % 2000) - 1000) * 1000000LL;
        vf[i]  = (static_cast<double>(static_cast<int64_t>(i % 2000) - 1000)) * 0.5;
    }
    eng.load_scan_column(1, u.data(), N);   eng.name_column(1, "u");
    eng.load_scan_column(2, key.data(), N); eng.name_column(2, "key");
    eng.load_scan_column_i64(3, vi.data(), N);
    eng.load_scan_column_f64(4, vf.data(), N);

    // scalar, every type + op, filtered + unfiltered
    for (MatrixAggOp a : OPS) {
        check_equiv(eng, MatrixQuery{.value_col = 1, .agg = a}, "u32 scalar");
        check_equiv(eng, MatrixQuery{.value_col = 1, .agg = a, .has_filter = true, .threshold = 500, .cmp = MatrixCmp::GT}, "u32 scalar filtered");
        check_equiv(eng, MatrixQuery{.value_col = 3, .agg = a}, "i64 scalar");
        check_equiv(eng, MatrixQuery{.value_col = 4, .agg = a}, "f64 scalar");
    }
    // grouped (key col 2), all value types, filtered + unfiltered
    check_equiv(eng, MatrixQuery{.value_col = 1, .agg = AGG_SUM, .grouped = true, .key_col = 2, .num_groups = 4}, "u32 grouped");
    check_equiv(eng, MatrixQuery{.value_col = 1, .agg = AGG_SUM, .has_filter = true, .threshold = 500, .cmp = MatrixCmp::GT, .grouped = true, .key_col = 2, .num_groups = 4}, "u32 grouped filtered");
    check_equiv(eng, MatrixQuery{.value_col = 3, .agg = AGG_MAX, .grouped = true, .key_col = 2, .num_groups = 4}, "i64 grouped");
    check_equiv(eng, MatrixQuery{.value_col = 4, .agg = AGG_MIN, .grouped = true, .key_col = 2, .num_groups = 4}, "f64 grouped");
    // scalar cross-column: filter on key (col 2), aggregate u (col 1)
    check_equiv(eng, MatrixQuery{.value_col = 1, .agg = AGG_SUM, .has_filter = true, .threshold = 2, .cmp = MatrixCmp::EQ, .filter_col = 2}, "u32 cross-column");
    std::printf("[shared semantics: SERVED == exclusive] ok\n");

    // NEEDS_EXCLUSIVE on a shape the fast path won't serve (null-masked column) — deterministic, no mutation
    {
        std::vector<uint8_t> nulls(N, 0); for (size_t i = 0; i < N; i += 7) nulls[i] = 1;
        CPUMockEngine e2; e2.load_scan_column(1, u.data(), N); e2.set_column_nulls(1, nulls);
        std::vector<uint64_t> o;
        assert(e2.execute_query_shared(MatrixQuery{.value_col = 1, .agg = AGG_SUM}, o) == CPUMockEngine::ReadOutcome::NEEDS_EXCLUSIVE);
        assert(o.empty() && "no output on defer");
        std::printf("[shared defers null-masked -> NEEDS_EXCLUSIVE] ok\n");
    }

    // Concurrent readers are race-free: 8 threads issue overlapping shared reads over HOST columns; every
    // result equals the oracle and there are no data races (run this binary under ThreadSanitizer).
    {
        uint64_t oracle = 0; for (uint32_t e : u) if (e > 500) oracle += e;   // SUM(u) WHERE u > 500
        std::atomic<int> bad{0};
        auto reader = [&] {
            for (int r = 0; r < 200; ++r) {
                std::vector<uint64_t> o;
                MatrixQuery q{.value_col = 1, .agg = AGG_SUM, .has_filter = true, .threshold = 500, .cmp = MatrixCmp::GT};
                if (eng.execute_query_shared(q, o) != CPUMockEngine::ReadOutcome::SERVED || o.size() != 1 || o[0] != oracle) ++bad;
            }
        };
        std::vector<std::thread> ts; for (int t = 0; t < 8; ++t) ts.emplace_back(reader);
        for (auto& t : ts) t.join();
        assert(bad.load() == 0 && "concurrent readers all oracle-correct");
        std::printf("[concurrent readers] ok (8 threads x 200 queries)\n");
    }

    std::printf("ALL CONCURRENT-SERVING TESTS PASSED\n");
    return 0;
}

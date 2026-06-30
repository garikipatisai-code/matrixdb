// Concurrent serving (single-writer / many-readers). Task 2: execute_query_shared semantics — for an
// all-HOST, non-null-masked query it serves the SAME result as execute_query with no tier side effects;
// for anything it can't serve purely (here: a null-masked column) it returns NEEDS_EXCLUSIVE without
// mutating. (Tasks 3+ add the concurrent-readers + ConcurrentServer tests.)
#include "concurrent_server.hpp"   // pulls in server.hpp -> compute_mock.cpp (CPUMockEngine + matrix_serve)
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>

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

    // ConcurrentServer: mixed concurrent reads + writes through the wire dispatch, correct + race-free (TSan),
    // plus the escalation path (a query the fast path can't serve -> exclusive matrix_serve).
    {
        CPUMockEngine eng4;
        const size_t M = 1u << 16;
        std::vector<uint32_t> w(M); for (size_t i = 0; i < M; ++i) w[i] = static_cast<uint32_t>(i % 1000);
        eng4.load_scan_column(1, w.data(), M);
        const AccessPolicy pol = AccessPolicy::permissive();
        ConcurrentServer srv(eng4, pol);
        uint64_t qoracle = 0; for (uint32_t e : w) if (e > 500) qoracle += e;

        MatrixRequest qreq; qreq.kind = ReqKind::QUERY;
        qreq.query = MatrixQuery{.value_col = 1, .agg = AGG_SUM, .has_filter = true, .threshold = 500};
        const std::vector<uint8_t> qbytes = matrix_serialize_request(qreq);

        std::atomic<int> bad{0};
        auto reader = [&] {
            for (int r = 0; r < 200; ++r) {
                MatrixResponse resp;
                if (!matrix_deserialize_response(srv.serve(qbytes), resp) || resp.status != 0
                    || resp.results.size() != 1 || resp.results[0] != qoracle) ++bad;
            }
        };
        auto writer = [&] {
            for (uint64_t k = 0; k < 500; ++k) {
                MatrixRequest p; p.kind = ReqKind::PUT; p.key = k; p.value = k * 7;
                MatrixResponse resp;
                if (!matrix_deserialize_response(srv.serve(matrix_serialize_request(p)), resp) || resp.status != 0) ++bad;
            }
        };
        std::vector<std::thread> ts;
        for (int t = 0; t < 6; ++t) ts.emplace_back(reader);
        ts.emplace_back(writer); ts.emplace_back(writer);
        for (auto& t : ts) t.join();
        assert(bad.load() == 0 && "ConcurrentServer mixed read/write all correct");
        uint64_t v = 0; assert(eng4.kv_get(7, v) && v == 49 && "writes landed under exclusive lock");
        std::printf("[ConcurrentServer mixed read/write] ok (6 readers + 2 writers)\n");

        // escalation: a QUERY the shared fast path can't serve (unknown column) -> NEEDS_EXCLUSIVE -> the
        // exclusive matrix_serve path -> proper ERR status (proves the escalation dispatch).
        MatrixRequest ureq; ureq.kind = ReqKind::QUERY; ureq.query = MatrixQuery{.value_col = 999, .agg = AGG_COUNT};
        MatrixResponse uresp;
        assert(matrix_deserialize_response(srv.serve(matrix_serialize_request(ureq)), uresp));
        assert(uresp.status == static_cast<uint32_t>(MatrixQueryStatus::ERR_UNKNOWN_COLUMN) && uresp.results.empty());
        std::printf("[ConcurrentServer escalation -> exclusive] ok\n");
    }

    // Throughput (informational, not asserted — timing is environment-dependent): the same total read work
    // spread across N threads vs 1, over a HOST column. Demonstrates the single-writer/many-readers win.
    {
        CPUMockEngine eng5;
        const size_t BIG = 1u << 20;
        std::vector<uint32_t> w(BIG); for (size_t i = 0; i < BIG; ++i) w[i] = static_cast<uint32_t>(i % 1000);
        eng5.load_scan_column(1, w.data(), BIG);
        auto run = [&](int threads, int per) {
            auto job = [&] { for (int r = 0; r < per; ++r) { std::vector<uint64_t> o;
                eng5.execute_query_shared(MatrixQuery{.value_col = 1, .agg = AGG_SUM, .has_filter = true, .threshold = 500}, o); } };
            const auto t0 = std::chrono::steady_clock::now();
            std::vector<std::thread> ts; for (int t = 0; t < threads; ++t) ts.emplace_back(job);
            for (auto& t : ts) t.join();
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        };
        const unsigned hc = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 4u;
        const int total = 48;
        const long t1 = run(1, total);
        const long tN = run(static_cast<int>(hc), total / static_cast<int>(hc));
        std::printf("[throughput] 1 thread = %ld ms; %u threads (same total work) = %ld ms (%.1fx)\n",
                    t1, hc, tN, tN ? static_cast<double>(t1) / static_cast<double>(tN) : 0.0);
    }

    std::printf("ALL CONCURRENT-SERVING TESTS PASSED\n");
    return 0;
}

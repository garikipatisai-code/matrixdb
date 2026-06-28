// CPU test for per-query latency metrics (OB-2): execute_query records count/total_ns/max_ns in EngineStats.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

static void test_query_latency() {
    std::vector<uint32_t> v(200000);
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<uint32_t>(i);
    CPUMockEngine eng;
    eng.load_scan_column(2, v.data(), v.size());

    // 5 OK queries (real work on 200k rows) + 2 that return ERR — all must be counted/timed.
    for (int i = 0; i < 5; ++i) {
        MatrixQuery q{}; q.value_col = 2; q.agg = AGG_SUM; q.has_filter = true; q.threshold = 100;
        std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::OK);
    }
    { MatrixQuery q{}; q.value_col = 999; q.agg = AGG_COUNT; std::vector<uint64_t> o;   // unknown column
      assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_UNKNOWN_COLUMN); }
    { MatrixQuery q{}; q.value_col = 2; q.key_col = 2; q.num_groups = 4; q.grouped = true; q.agg = AGG_COUNT;
      std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_INVALID_GROUP); } // key==value

    const EngineStats s = eng.stats();
    assert(s.query_count == 7 && "every execute_query call counted (OK and ERR)");
    assert(s.total_query_ns > 0 && "real queries took measurable time");
    assert(s.max_query_ns > 0 && s.max_query_ns <= s.total_query_ns && "max is one sample of the total");
    std::cout << "[query latency] ok (count=" << s.query_count
              << " mean_ns=" << (s.total_query_ns / s.query_count) << " max_ns=" << s.max_query_ns << ")\n";
}

int main() { test_query_latency(); std::cout << "ALL QUERY-LATENCY TESTS PASSED\n"; return 0; }

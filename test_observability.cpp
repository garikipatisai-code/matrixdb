// CPU test for engine observability (EngineStats / stats()).
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

static void test_stats() {
    const size_t N = 1000;
    const size_t S = N * sizeof(uint32_t);
    std::vector<uint32_t> col(N);
    for (size_t i = 0; i < N; ++i) col[i] = static_cast<uint32_t>(i);
    CPUMockEngine eng(0, "", /*host_cap=*/2 * S);   // 2-column budget
    eng.load_scan_column(1, col.data(), N);
    eng.load_scan_column(2, col.data(), N);
    eng.load_scan_column(3, col.data(), N);
    {
        EngineStats s = eng.stats();
        assert(s.catalog_columns == 3);
        assert(s.cold_resident_bytes == 0);
        assert(s.rebalances == 0 && s.migrations == 0 && s.cold_borrows == 0);
    }
    const uint32_t T = 250;
    for (int r = 0; r < 8; ++r)
        for (uint64_t id : {1ull, 2ull}) {
            DatabaseQuery q{}; matrix_set_scan_target(q, T, id); eng.execute_scan(q);
        }
    {
        EngineStats s = eng.stats();
        assert(s.rebalances == 16 / 4);                 // 16 tiered scans / REBALANCE_EVERY(4)
        assert(s.migrations >= 1);                       // col 3 demoted (non-vacuous)
        assert(s.cold_resident_bytes == S);              // one column on SSD
        assert(s.host_resident_bytes == 2 * S);          // two columns in RAM
        assert(s.cold_borrows == 0);                     // cols 1,2 stayed HOST -> no borrow
    }
    { DatabaseQuery q{}; matrix_set_scan_target(q, T, 3); eng.execute_scan(q); }
    assert(eng.stats().cold_borrows == 1);
    std::cout << "[engine stats] ok\n";
}

int main() {
    test_stats();
    std::cout << "ALL OBSERVABILITY TESTS PASSED\n";
    return 0;
}

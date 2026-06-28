// CPU test for OB-4 runtime config: set_rebalance_interval tunes the heat-rebalance cadence at runtime.
// A scalar scan triggers maybe_rebalance; stats().rebalances counts the passes that fired.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

static void scan(CPUMockEngine& e) {
    MatrixQuery q{}; q.value_col = 2; q.agg = AGG_COUNT; std::vector<uint64_t> o;
    e.execute_query(q, o);
}

static void test_rebalance_config() {
    // default cadence is 4: rebalance fires only on the 4th scan
    {
        CPUMockEngine eng; std::vector<uint32_t> v(10, 1); eng.load_scan_column(2, v.data(), v.size());
        assert(eng.rebalance_interval() == 4 && "default cadence == REBALANCE_EVERY");
        scan(eng); scan(eng); scan(eng);
        assert(eng.stats().rebalances == 0 && "no rebalance before the 4th scan");
        scan(eng);
        assert(eng.stats().rebalances == 1 && "rebalance fires on the 4th scan");
    }
    // aggressive: interval 1 -> a rebalance pass every scan
    {
        CPUMockEngine eng; std::vector<uint32_t> v(10, 1); eng.load_scan_column(2, v.data(), v.size());
        eng.set_rebalance_interval(1);
        scan(eng); scan(eng); scan(eng);
        assert(eng.stats().rebalances == 3 && "interval 1 -> rebalance every scan");
    }
    // relaxed: a large interval suppresses rebalancing over many scans
    {
        CPUMockEngine eng; std::vector<uint32_t> v(10, 1); eng.load_scan_column(2, v.data(), v.size());
        eng.set_rebalance_interval(1000);
        for (int i = 0; i < 5; ++i) scan(eng);
        assert(eng.stats().rebalances == 0 && "large interval -> no rebalance over 5 scans");
        eng.set_rebalance_interval(0);
        assert(eng.rebalance_interval() == 1 && "0 clamps to 1 (rebalance every scan)");
    }
    std::cout << "[rebalance config] ok\n";
}

int main() { test_rebalance_config(); std::cout << "ALL REBALANCE-CONFIG TESTS PASSED\n"; return 0; }

// CPU test for top-N groups (DM-4 ORDER BY ... DESC LIMIT k): top_groups runs a grouped query and
// returns the k (group, value) pairs with the largest aggregate, descending.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>
#include <iostream>

using Pairs = std::vector<std::pair<uint64_t, uint64_t>>;

static void test_top_groups() {
    std::vector<uint32_t> region = {0, 1, 0, 2, 1, 0}, amount = {10, 20, 30, 40, 50, 60};
    CPUMockEngine eng;
    eng.load_scan_column(1, region.data(), region.size());
    eng.load_scan_column(2, amount.data(), amount.size());
    MatrixQuery q{}; q.value_col = 2; q.key_col = 1; q.num_groups = 3; q.agg = AGG_SUM; q.grouped = true;
    // group sums: r0 = 10+30+60 = 100, r1 = 20+50 = 70, r2 = 40
    assert((eng.top_groups(q, 2) == Pairs{{0, 100}, {1, 70}}) && "top-2 groups by SUM, descending");
    assert((eng.top_groups(q, 10) == Pairs{{0, 100}, {1, 70}, {2, 40}}) && "k > num_groups -> all groups, sorted");
    const std::pair<uint64_t, uint64_t> top1{0, 100};
    assert(eng.top_groups(q, 1).front() == top1 && "top-1 is the max group");   // comma in <> breaks assert()
    assert(eng.top_groups(q, 0).empty() && "k=0 -> empty");
    // a non-grouped query -> empty (top_groups is for grouped queries)
    MatrixQuery scalar{}; scalar.value_col = 2; scalar.agg = AGG_SUM;
    assert(eng.top_groups(scalar, 3).empty() && "non-grouped query -> empty");
    std::cout << "[top groups] ok\n";
}

int main() { test_top_groups(); std::cout << "ALL TOP-GROUPS TESTS PASSED\n"; return 0; }

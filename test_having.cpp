// CPU test for HAVING (DM-4): having() runs a grouped query and returns the (group, value) pairs whose
// aggregate satisfies a comparison — the SQL HAVING clause. Group-id order preserved.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>
#include <iostream>

using Pairs = std::vector<std::pair<uint64_t, uint64_t>>;

static void test_having() {
    std::vector<uint32_t> region = {0, 1, 0, 2, 1, 0}, amount = {10, 20, 30, 40, 50, 60};
    CPUMockEngine eng;
    eng.load_scan_column(1, region.data(), region.size());
    eng.load_scan_column(2, amount.data(), amount.size());
    MatrixQuery q{}; q.value_col = 2; q.key_col = 1; q.num_groups = 3; q.agg = AGG_SUM; q.grouped = true;
    // group sums: g0=100, g1=70, g2=40
    assert((eng.having(q, MatrixCmp::GT, 50) == Pairs{{0, 100}, {1, 70}}) && "HAVING SUM > 50");
    assert((eng.having(q, MatrixCmp::LT, 50) == Pairs{{2, 40}}) && "HAVING SUM < 50");
    assert((eng.having(q, MatrixCmp::EQ, 40) == Pairs{{2, 40}}) && "HAVING SUM = 40");
    assert((eng.having(q, MatrixCmp::BETWEEN, 40, 70) == Pairs{{1, 70}, {2, 40}}) && "HAVING SUM BETWEEN 40 AND 70");
    assert(eng.having(q, MatrixCmp::GT, 1000).empty() && "HAVING with no matches -> empty");

    // COUNT per group: g0=3, g1=2, g2=1 -> HAVING COUNT >= 2 keeps g0, g1
    MatrixQuery c = q; c.agg = AGG_COUNT;
    assert((eng.having(c, MatrixCmp::GE, 2) == Pairs{{0, 3}, {1, 2}}) && "HAVING COUNT >= 2");

    // non-grouped query -> empty (HAVING is for grouped queries)
    MatrixQuery scalar{}; scalar.value_col = 2; scalar.agg = AGG_SUM;
    assert(eng.having(scalar, MatrixCmp::GT, 0).empty() && "non-grouped -> empty");
    std::cout << "[having] ok\n";
}

int main() { test_having(); std::cout << "ALL HAVING TESTS PASSED\n"; return 0; }

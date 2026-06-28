// CPU test for gather (DM-8d, the "project" step): gather(col, rows) returns column values at the given
// row indices (typed: U32 value / I64+F64 bit-pattern), and composes with hash_join for join-then-project.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <algorithm>
#include <bit>
#include <vector>
#include <iostream>

static void test_gather_typed() {
    CPUMockEngine eng;
    std::vector<uint32_t> u = {10, 20, 30, 40};
    std::vector<int64_t>  s = {-7, 5000000000LL};
    std::vector<double>   d = {1.5, -2.5};
    eng.load_scan_column(1, u.data(), u.size());
    eng.load_scan_column_i64(2, s.data(), s.size());
    eng.load_scan_column_f64(3, d.data(), d.size());
    // u32: index order preserved, reorder + dup
    assert((eng.gather(1, {2, 0, 3, 0}) == std::vector<uint64_t>{30, 10, 40, 10}) && "u32 gather in index order");
    // i64: bit-pattern -> static_cast<int64_t>
    { auto g = eng.gather(2, {1, 0}); assert(static_cast<int64_t>(g[0]) == 5000000000LL && static_cast<int64_t>(g[1]) == -7); }
    // f64: bit-pattern -> bit_cast<double>
    { auto g = eng.gather(3, {0, 1}); assert(std::bit_cast<double>(g[0]) == 1.5 && std::bit_cast<double>(g[1]) == -2.5); }
    std::cout << "[gather typed] ok\n";
}

static void test_join_then_project() {
    // region[row] aligned with amount[row]; join region against a lookup of valid regions, project amount.
    std::vector<uint32_t> region = {0, 1, 0, 2}, lookup = {0, 2}, amount = {100, 200, 300, 400};
    CPUMockEngine eng;
    eng.load_scan_column(1, region.data(), region.size());
    eng.load_scan_column(2, lookup.data(), lookup.size());
    eng.load_scan_column(3, amount.data(), amount.size());
    auto pairs = eng.hash_join(1, 2);                         // (region_row, lookup_row) where region in {0,2}
    std::vector<uint64_t> left_rows;
    for (auto& p : pairs) left_rows.push_back(p.first);       // region rows 0,2 (=0) and 3 (=2)
    auto amounts = eng.gather(3, left_rows);                  // project amount at the matched region rows
    std::sort(amounts.begin(), amounts.end());
    assert((amounts == std::vector<uint64_t>{100, 300, 400}) && "join region x valid, project amount: rows 0,2,3");
    std::cout << "[join then project] ok\n";
}

int main() { test_gather_typed(); test_join_then_project();
    std::cout << "ALL GATHER TESTS PASSED\n"; return 0; }

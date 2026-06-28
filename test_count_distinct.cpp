// CPU test for COUNT(DISTINCT col): count_distinct returns the number of distinct non-NULL values,
// typed (U32/I64/F64) and null-aware (masked rows excluded).
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

static void test_count_distinct() {
    CPUMockEngine eng;
    std::vector<uint32_t> u = {1, 2, 2, 3, 3, 3};                   // distinct {1,2,3} = 3
    eng.load_scan_column(2, u.data(), u.size());
    assert(eng.count_distinct(2) == 3 && "u32 distinct count");
    // null row 0 (value 1) -> distinct {2,3} = 2
    eng.set_column_nulls(2, {1, 0, 0, 0, 0, 0});
    assert(eng.count_distinct(2) == 2 && "distinct skips the null-only value");

    std::vector<int64_t> s = {-5, -5, 100, 100, 7};                // distinct {-5,100,7} = 3
    eng.load_scan_column_i64(3, s.data(), s.size());
    assert(eng.count_distinct(3) == 3 && "int64 distinct count");

    std::vector<double> d = {1.5, 1.5, 2.5};                       // distinct {1.5,2.5} = 2
    eng.load_scan_column_f64(4, d.data(), d.size());
    assert(eng.count_distinct(4) == 2 && "double distinct count");

    std::vector<uint32_t> all = {10, 20, 30};                      // all unique
    eng.load_scan_column(5, all.data(), all.size());
    assert(eng.count_distinct(5) == 3 && "all-distinct");

    // single repeated value -> 1
    std::vector<uint32_t> one = {7, 7, 7, 7};
    eng.load_scan_column(6, one.data(), one.size());
    assert(eng.count_distinct(6) == 1 && "single distinct value");
    std::cout << "[count distinct] ok\n";
}

// distinct_query: the string entry point — "SELECT COUNT(DISTINCT col)" -> count_distinct.
static void test_distinct_query() {
    std::vector<uint32_t> u = {1, 2, 2, 3, 3, 3};                   // distinct = 3
    CPUMockEngine eng;
    eng.load_scan_column(2, u.data(), u.size());
    eng.name_column(2, "amount");
    uint64_t n = 0;
    assert(eng.distinct_query("SELECT COUNT(DISTINCT amount)", n) && n == 3 && "COUNT(DISTINCT) from string");
    // malformed / non-distinct / unknown -> false, n untouched
    n = 999;
    assert(!eng.distinct_query("SELECT COUNT(amount)", n) && n == 999 && "plain COUNT is not a distinct query");
    assert(!eng.distinct_query("SELECT COUNT(DISTINCT nope)", n) && "unknown column -> false");
    assert(!eng.distinct_query("garbage", n) && "junk -> false");
    std::cout << "[distinct query] ok\n";
}

int main() { test_count_distinct(); test_distinct_query(); std::cout << "ALL COUNT-DISTINCT TESTS PASSED\n"; return 0; }

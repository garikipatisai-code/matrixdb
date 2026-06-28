// CPU test for the equi-join primitive (DM-8): hash_join(left,right) returns matching (left_row,
// right_row) pairs; verified against a brute O(n*m) oracle, incl. duplicates + a COLD column.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <algorithm>
#include <utility>
#include <vector>
#include <iostream>

using Pairs = std::vector<std::pair<uint64_t, uint64_t>>;
static Pairs sorted(Pairs v) { std::sort(v.begin(), v.end()); return v; }
// Brute reference: every (i,j) with l[i]==r[j].
static Pairs brute(const std::vector<uint32_t>& l, const std::vector<uint32_t>& r) {
    Pairs out;
    for (uint64_t i = 0; i < l.size(); ++i) for (uint64_t j = 0; j < r.size(); ++j)
        if (l[i] == r[j]) out.emplace_back(i, j);
    return sorted(out);
}

static void test_join_basic() {
    std::vector<uint32_t> L = {10, 20, 30, 20}, R = {20, 40, 10, 20};
    CPUMockEngine eng;
    eng.load_scan_column(1, L.data(), L.size());
    eng.load_scan_column(2, R.data(), R.size());
    Pairs got = sorted(eng.hash_join(1, 2));
    Pairs exp = brute(L, R);
    assert(got == exp && "hash join == brute oracle");
    // explicit expected set: (0,2),(1,0),(1,3),(3,0),(3,3)
    assert((got == Pairs{{0,2},{1,0},{1,3},{3,0},{3,3}}) && "exact pairs incl. duplicate-key Cartesian");
    // count-only join (resource-safe): equals the materialized cardinality, no pairs stored.
    assert(eng.hash_join_count(1, 2) == got.size() && eng.hash_join_count(1, 2) == 5 && "hash_join_count == cardinality");
    std::cout << "[join basic] ok\n";
}

static void test_join_edges() {
    CPUMockEngine eng;
    std::vector<uint32_t> a = {1, 2, 3}, b = {4, 5, 6};         // disjoint
    eng.load_scan_column(1, a.data(), a.size());
    eng.load_scan_column(2, b.data(), b.size());
    assert(eng.hash_join(1, 2).empty() && "no matches -> empty");
    std::vector<uint32_t> c = {7, 7}, d = {7};                 // dup left, single right
    eng.load_scan_column(3, c.data(), c.size());
    eng.load_scan_column(4, d.data(), d.size());
    assert((sorted(eng.hash_join(3, 4)) == Pairs{{0,0},{1,0}}) && "Cartesian of dup match");
    std::cout << "[join edges] ok\n";
}

static void test_join_cold() {
    // Tiny host budget (bytes): L(16B)+R(8B)=24 > 16, so the idle column demotes to COLD; the join
    // borrows it back. Scanning L keeps it hot so R is the eviction victim.
    CPUMockEngine eng(0, "", 16);
    std::vector<uint32_t> L = {5, 9, 5, 1}, R = {9, 5};        // 16B, 8B
    eng.load_scan_column(1, L.data(), L.size());
    eng.load_scan_column(2, R.data(), R.size());
    for (int i = 0; i < 8; ++i) {                              // heat col1 -> rebalance demotes idle col2
        MatrixQuery q{}; q.value_col = 1; q.agg = AGG_COUNT; std::vector<uint64_t> o; eng.execute_query(q, o);
    }
    assert(eng.column_tier(2) == MemorySpace::COLD && "idle join column demoted -> exercises the borrow path");
    Pairs got = sorted(eng.hash_join(1, 2));
    assert(got == brute(L, R) && "join correct across the COLD tier (borrow)");
    assert((got == Pairs{{0,1},{1,0},{2,1}}) && "L{5,9,5,1} x R{9,5}: 5@0=5@1, 9@1=9@0, 5@2=5@1");
    std::cout << "[join cold] ok\n";
}

static Pairs brute_i64(const std::vector<int64_t>& l, const std::vector<int64_t>& r) {
    Pairs out;
    for (uint64_t i = 0; i < l.size(); ++i) for (uint64_t j = 0; j < r.size(); ++j)
        if (l[i] == r[j]) out.emplace_back(i, j);
    return sorted(out);
}
static void test_join_i64() {
    std::vector<int64_t> L = {-7, 5000000000LL, 30, 5000000000LL}, R = {5000000000LL, 40, -7};
    CPUMockEngine eng;
    eng.load_scan_column_i64(1, L.data(), L.size());
    eng.load_scan_column_i64(2, R.data(), R.size());
    Pairs got = sorted(eng.hash_join_i64(1, 2));
    assert(got == brute_i64(L, R) && "int64 hash join == brute oracle");
    // -7@0=-7@2, 5e9@1=5e9@0, 5e9@3=5e9@0 — >UINT32_MAX + negative keys (a u32 join would mismatch these)
    assert((got == Pairs{{0,2},{1,0},{3,0}}) && "int64-key join incl. > UINT32_MAX + negative keys");
    std::cout << "[join i64] ok\n";
}

int main() { test_join_basic(); test_join_edges(); test_join_cold(); test_join_i64();
    std::cout << "ALL JOIN TESTS PASSED\n"; return 0; }

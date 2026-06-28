// CPU test for key range scan over the point-op store (DM-7): kv_range(lo, hi) returns every
// (key, value) with lo <= key <= hi (inclusive), reusing KVStore::for_each.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <algorithm>
#include <utility>
#include <vector>
#include <iostream>

static std::vector<std::pair<uint64_t, uint64_t>> sorted(std::vector<std::pair<uint64_t, uint64_t>> v) {
    std::sort(v.begin(), v.end());
    return v;
}

static void test_kv_range() {
    CPUMockEngine eng;                                  // no WAL needed
    // value = key*10 so we can check values too.
    eng.begin();
    for (uint64_t k : {5ull, 10ull, 15ull, 20ull, 100ull}) eng.txn_put(k, k * 10);
    eng.commit();

    // inclusive [10, 20] -> 10,15,20
    auto r = sorted(eng.kv_range(10, 20));
    assert((r == std::vector<std::pair<uint64_t,uint64_t>>{{10,100},{15,150},{20,200}}) && "inclusive range");
    // full range -> all 5
    assert(eng.kv_range(0, UINT64_MAX).size() == 5 && "full range = all keys");
    // empty interior range
    assert(eng.kv_range(11, 14).empty() && "no keys in (10,15)");
    // single-key boundary
    auto one = eng.kv_range(5, 5);
    const std::pair<uint64_t, uint64_t> expect{5, 50};   // (in a var: a comma inside assert(...) args breaks the macro)
    assert(one.size() == 1 && one[0] == expect && "inclusive single-key");
    // non-vacuity: 100 is OUTSIDE [10,20] (excluded), and each boundary IS included
    for (auto& kv : r) assert(kv.first != 100 && "out-of-range key excluded");
    assert(sorted(eng.kv_range(10, 20)).front().first == 10 && sorted(eng.kv_range(10, 20)).back().first == 20
           && "both boundaries inclusive");
    std::cout << "[kv range] ok\n";
}

int main() { test_kv_range(); std::cout << "ALL KV-RANGE TESTS PASSED\n"; return 0; }

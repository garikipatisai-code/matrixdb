// CPU test for the sorted secondary index (DM-7): kv_range_sorted returns range keys in ASCENDING order
// via the ordered index (vs kv_range's unordered O(n) scan); index is maintained on commit + rebuilt on
// recovery (survives restart).
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include <iostream>

using Pairs = std::vector<std::pair<uint64_t, uint64_t>>;
static Pairs sorted(Pairs v) { std::sort(v.begin(), v.end()); return v; }

static void test_sorted_range() {
    CPUMockEngine eng;
    eng.begin();
    for (uint64_t k : {50ull, 10ull, 30ull, 20ull, 40ull, 100ull}) eng.txn_put(k, k * 10);   // inserted out of order
    eng.commit();
    // [20,40] -> 20,30,40 in ASCENDING order
    Pairs r = eng.kv_range_sorted(20, 40);
    assert((r == Pairs{{20,200},{30,300},{40,400}}) && "kv_range_sorted is ascending + inclusive");
    assert(std::is_sorted(r.begin(), r.end()) && "ascending order");
    // cross-check vs the unordered kv_range (same set, sorted)
    assert(r == sorted(eng.kv_range(20, 40)) && "kv_range_sorted == sorted(kv_range)");
    assert(eng.kv_range_sorted(0, UINT64_MAX).size() == 6 && "full range = all keys");
    assert(eng.kv_range_sorted(21, 29).empty() && "empty interior range");
    assert((eng.kv_range_sorted(50, 50) == Pairs{{50,500}}) && "single-key inclusive");
    std::cout << "[sorted range] ok\n";
}

static void test_index_survives_restart() {
    const std::string wal = "/tmp/mdb_kvindex.wal";
    std::remove(wal.c_str()); std::remove((wal + ".ckpt").c_str());
    {
        CPUMockEngine eng(0, wal);
        eng.begin(); for (uint64_t k : {7ull, 3ull, 9ull, 1ull}) eng.txn_put(k, k + 1000); eng.commit();
    }
    {
        CPUMockEngine eng(0, wal);                 // fresh engine: index rebuilt from recovered kv_
        Pairs r = eng.kv_range_sorted(2, 8);       // 3,7
        assert((r == Pairs{{3,1003},{7,1007}}) && "index rebuilt on recovery -> sorted range works after restart");
    }
    std::remove(wal.c_str()); std::remove((wal + ".ckpt").c_str());
    std::cout << "[index survives restart] ok\n";
}

int main() { test_sorted_range(); test_index_survives_restart();
    std::cout << "ALL KV-INDEX TESTS PASSED\n"; return 0; }

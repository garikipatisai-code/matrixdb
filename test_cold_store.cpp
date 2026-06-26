// CPU unit test for ColdStore (the SSD WAL). Uses real temp files.
// Build: clang++ -std=c++20 -O2 test_cold_store.cpp -o /tmp/tcs && /tmp/tcs
#include "cold_store.hpp"
#include <cstdio>
#include <cassert>
#include <vector>
#include <string>

static const char* PATH = "/tmp/matrixdb_wal_test.log";

int main() {
    // --- Task 1: append + replay round-trip, last-writer-wins ---
    {
        std::remove(PATH); // fresh
        {
            ColdStore cs(PATH, SyncPolicy::SYNC_EACH);
            cs.append_put(10, 100);
            cs.append_put(20, 200);
            cs.append_put(10, 101); // overwrite key 10 (later record wins on replay)
            assert(cs.records_written() == 3 && "three appends counted");
        }
        // Replay (a fresh ColdStore on the same path).
        ColdStore cs(PATH);
        std::vector<std::pair<uint64_t,uint64_t>> got;
        cs.replay([&](uint64_t k, uint64_t v){ got.push_back({k, v}); });
        assert(got.size() == 3 && "replay yields all three records in order");
        assert(got[0].first == 10 && got[0].second == 100);
        assert(got[1].first == 20 && got[1].second == 200);
        assert(got[2].first == 10 && got[2].second == 101 && "last write for key 10 replays last");
        std::remove(PATH);
    }

    std::printf("PASS: cold store WAL correct\n");
    return 0;
}

// CPU unit test for ColdStore (the SSD WAL). Uses real temp files.
// Build: clang++ -std=c++20 -O2 test_cold_store.cpp -o /tmp/tcs && /tmp/tcs
#include "cold_store.hpp"
#include <cstdio>
#include <cassert>
#include <vector>
#include <string>
#include <unistd.h>    // ::truncate

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

    // --- Task 2a: survives restart (write with one ColdStore, read with a fresh one) ---
    {
        std::remove(PATH);
        { ColdStore w(PATH); w.append_put(7, 70); w.append_put(8, 80); } // closed = "restart"
        ColdStore r(PATH);
        uint64_t sum = 0; int n = 0;
        r.replay([&](uint64_t, uint64_t v){ sum += v; ++n; });
        assert(n == 2 && sum == 150 && "data persists across ColdStore instances (restart)");
        std::remove(PATH);
    }

    // --- Task 2b: torn tail — truncating mid-last-record drops only that record ---
    {
        std::remove(PATH);
        { ColdStore w(PATH); w.append_put(1, 11); w.append_put(2, 22); w.append_put(3, 33); }
        // Each record on disk = 4 (len) + 4 (crc) + 20 (payload) = 28 bytes. Truncate to
        // 2 full records + a partial third (cut the third's payload).
        FILE* f = std::fopen(PATH, "rb");
        std::fseek(f, 0, SEEK_END); long size = std::ftell(f); std::fclose(f);
        assert(size == 3*28 && "three 28-byte records");
        ::truncate(PATH, 2*28 + 10); // 2 whole records + 10 bytes of the third (torn)
        ColdStore r(PATH);
        int n = 0; uint64_t last = 0;
        r.replay([&](uint64_t k, uint64_t){ ++n; last = k; });
        assert(n == 2 && last == 2 && "torn third record dropped; first two recovered");
        std::remove(PATH);
    }

    // --- Task 2c: CRC corruption — a flipped payload byte stops replay at that record ---
    {
        std::remove(PATH);
        { ColdStore w(PATH); w.append_put(5, 55); w.append_put(6, 66); }
        // Flip a byte inside the FIRST record's payload (offset 8 = start of payload).
        FILE* f = std::fopen(PATH, "rb+");
        std::fseek(f, 8, SEEK_SET);            // first record's payload byte 0
        unsigned char b; std::fread(&b, 1, 1, f);
        b ^= 0xFF;
        std::fseek(f, 8, SEEK_SET); std::fwrite(&b, 1, 1, f);
        std::fclose(f);
        ColdStore r(PATH);
        int n = 0;
        r.replay([&](uint64_t, uint64_t){ ++n; });
        assert(n == 0 && "corruption in the first record stops replay immediately");
        std::remove(PATH);
    }

    // --- Task 2d: empty / missing file replays nothing, no error ---
    {
        std::remove(PATH); // ensure missing
        ColdStore r(PATH); // "ab" creates an empty file
        int n = 0;
        r.replay([&](uint64_t, uint64_t){ ++n; });
        assert(n == 0 && "empty/missing log replays nothing");
        std::remove(PATH);
    }

    std::printf("PASS: cold store WAL correct\n");
    return 0;
}

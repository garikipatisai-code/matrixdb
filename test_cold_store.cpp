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
        if (::truncate(PATH, 2*28 + 10) != 0) std::perror("truncate"); // 2 whole records + 10 bytes of the third (torn)
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
        unsigned char b = 0; if (std::fread(&b, 1, 1, f) != 1) std::perror("fread");
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

    // --- Task 3a: transactional puts buffer until a commit marker, then all replay together ---
    // (test_transactions.cpp proves this at the CPUMockEngine level; this is the ColdStore-level
    // unit test of the exact mechanism it relies on, which had no dedicated coverage before this.)
    {
        std::remove(PATH);
        {
            ColdStore w(PATH);
            w.append_txn_put(100, 1000);
            w.append_txn_put(200, 2000);
            w.append_commit();
        }
        ColdStore r(PATH);
        std::vector<std::pair<uint64_t,uint64_t>> got;
        r.replay([&](uint64_t k, uint64_t v){ got.push_back({k, v}); });
        assert(got.size() == 2 && "both txn-puts replay once their commit marker is present");
        assert(got[0] == std::make_pair(100ull, 1000ull) && got[1] == std::make_pair(200ull, 2000ull)
               && "txn-puts replay in append order");
        std::remove(PATH);
    }

    // --- Task 3b: transactional puts with NO commit marker (crash before commit) discard on EOF ---
    {
        std::remove(PATH);
        {
            ColdStore w(PATH);
            w.append_txn_put(300, 3000);
            w.append_txn_put(400, 4000);
            // no append_commit() -- simulates a crash mid-transaction
        }
        ColdStore r(PATH);
        int n = 0;
        r.replay([&](uint64_t, uint64_t){ ++n; });
        assert(n == 0 && "uncommitted txn-puts are discarded on EOF, not applied");
        std::remove(PATH);
    }

    // --- Task 3c: an uncommitted txn group does NOT poison auto-commit writes around it ---
    // (an OP_WRITE before or after an incomplete txn group must still replay -- the buffering is
    // scoped to txn-puts only, not a blanket "stop on anything unusual").
    {
        std::remove(PATH);
        {
            ColdStore w(PATH);
            w.append_put(1, 11);              // auto-commit, before the txn group
            w.append_txn_put(500, 5000);       // buffered...
            w.append_txn_put(600, 6000);       // ...never committed (crash)
        }
        ColdStore r(PATH);
        std::vector<std::pair<uint64_t,uint64_t>> got;
        r.replay([&](uint64_t k, uint64_t v){ got.push_back({k, v}); });
        assert(got.size() == 1 && got[0] == std::make_pair(1ull, 11ull)
               && "auto-commit write before the dangling txn group still replays; the txn group doesn't");
        std::remove(PATH);
    }

    // --- Task 3d: two SEQUENTIAL committed txn groups both replay, and independently -- proves the
    // buffer is actually cleared after each commit (not accumulating across groups, and not letting
    // a later group's content leak into an earlier one's replay). ---
    {
        std::remove(PATH);
        {
            ColdStore w(PATH);
            w.append_txn_put(1, 10); w.append_txn_put(2, 20); w.append_commit();   // group 1
            w.append_txn_put(3, 30); w.append_commit();                            // group 2
        }
        ColdStore r(PATH);
        std::vector<std::pair<uint64_t,uint64_t>> got;
        r.replay([&](uint64_t k, uint64_t v){ got.push_back({k, v}); });
        assert(got.size() == 3 && "both committed groups replay in full");
        assert(got[0] == std::make_pair(1ull, 10ull) && got[1] == std::make_pair(2ull, 20ull)
               && got[2] == std::make_pair(3ull, 30ull) && "order preserved across group boundaries");
        std::remove(PATH);
    }

    std::printf("PASS: cold store WAL correct\n");
    return 0;
}

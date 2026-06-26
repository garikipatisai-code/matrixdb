// End-to-end durability: point-op writes through one CPUMockEngine survive into a fresh
// engine constructed on the same WAL path (simulates process restart).
// Build: clang++ -std=c++20 -O2 test_engine_restart.cpp -o /tmp/ter && /tmp/ter
#include "compute_mock.cpp"   // pulls in CPUMockEngine (used header-style, as in main.cpp)
#include <cstdio>
#include <cassert>
#include <vector>

static const char* WAL = "/tmp/matrixdb_engine_restart.log";

// Build a batch of OP_WRITE queries for keys [1..n].
static std::vector<DatabaseQuery> write_batch(uint64_t n) {
    std::vector<DatabaseQuery> b(n);
    for (uint64_t i = 0; i < n; ++i) {
        b[i] = DatabaseQuery{};
        b[i].query_id = i + 1;       // keys 1..n; value == key (mock projection)
        b[i].opcode = OP_WRITE;
    }
    return b;
}

int main() {
    std::remove(WAL);

    uint64_t checksum_before = 0;
    {
        CPUMockEngine eng(4, WAL);          // durability ON
        auto batch = write_batch(100);
        eng.execute_batch(batch.data(), batch.size());
        checksum_before = eng.store_checksum();
        assert(checksum_before == 5050 && "sum of values 1..100 (value==key)");
    } // engine destroyed = "process exit"

    {
        CPUMockEngine eng2(4, WAL);         // fresh engine, same WAL → recovery on construct
        const uint64_t checksum_after = eng2.store_checksum();
        assert(checksum_after == checksum_before
               && "writes survived restart: recovered store matches pre-restart");
    }

    std::remove(WAL);
    std::printf("PASS: engine survives restart (WAL recovery)\n");
    return 0;
}

// CPU test for QA-5 fault injection (engine-level WAL corruption recovery): a fresh CPUMockEngine built
// on a CORRUPT WAL must recover the intact prefix, discard the corrupt tail, and stay usable — never
// crash, never apply garbage. (test_cold_store covers the ColdStore unit; this is the engine integration.)
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <iostream>

static void clean(const std::string& wal) {
    std::remove(wal.c_str()); std::remove((wal + ".ckpt").c_str()); std::remove((wal + ".ckpt.tmp").c_str());
}
static uint64_t get(CPUMockEngine& e, uint64_t k) { uint64_t v = 0; e.kv_get(k, v); return v; }

// Write k=k*10 as single-key committed transactions for k in [1, n].
static void write_n(const std::string& wal, uint64_t n) {
    CPUMockEngine e(0, wal);
    for (uint64_t k = 1; k <= n; ++k) { e.begin(); e.txn_put(k, k * 10); e.commit(); }
}

static void test_torn_tail_recovers_prefix() {
    const std::string wal = "/tmp/mdb_fault_a.wal"; clean(wal);
    write_n(wal, 5);
    // simulate a crash mid-write: append a torn record (a few bytes — not a whole [len][crc][payload])
    { std::FILE* f = std::fopen(wal.c_str(), "ab"); assert(f);
      const unsigned char garbage[5] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01};
      std::fwrite(garbage, 1, sizeof garbage, f); std::fclose(f); }
    // fresh engine: recovers all 5 committed writes, ignores the torn tail, no crash
    CPUMockEngine e(0, wal);
    for (uint64_t k = 1; k <= 5; ++k) assert(get(e, k) == k * 10 && "committed write survives a torn tail");
    // and the engine is still usable after recovery
    e.begin(); e.txn_put(99, 990); e.commit();
    assert(get(e, 99) == 990 && "engine usable after recovering past a torn tail");
    clean(wal);
    std::cout << "[torn tail recovers prefix] ok\n";
}

static void test_early_corruption_stops_clean() {
    const std::string wal = "/tmp/mdb_fault_b.wal"; clean(wal);
    write_n(wal, 4);
    // flip a byte inside the FIRST record's payload (offset 8 = past the [u32 len][u32 crc] header) ->
    // CRC mismatch on record 1 -> replay stops immediately, recovering nothing.
    { std::FILE* f = std::fopen(wal.c_str(), "r+b"); assert(f);
      std::fseek(f, 8, SEEK_SET);
      unsigned char b = 0; size_t r = std::fread(&b, 1, 1, f); assert(r == 1);
      b ^= 0xFF; std::fseek(f, 8, SEEK_SET); std::fwrite(&b, 1, 1, f); std::fclose(f); }
    // fresh engine: corruption at record 1 -> nothing recovered, but NO crash and the engine is usable
    CPUMockEngine e(0, wal);
    assert(get(e, 1) == 0 && "corrupt first record -> not recovered (replay stopped at it)");
    e.begin(); e.txn_put(7, 77); e.commit();
    assert(get(e, 7) == 77 && "engine fully usable after a corrupt-WAL recovery");
    clean(wal);
    std::cout << "[early corruption stops clean] ok\n";
}

int main() {
    test_torn_tail_recovers_prefix();
    test_early_corruption_stops_clean();
    std::cout << "ALL FAULT-INJECTION TESTS PASSED\n";
    return 0;
}

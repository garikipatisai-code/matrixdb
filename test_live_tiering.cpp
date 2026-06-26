// CPU test for the live tiering integration. Grows across tasks; one main() runs all.
#include "compute_mock.cpp"   // CPUMockEngine + compute.hpp (codec) + tiering headers
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>
#include "tiered_column.hpp"

static void test_codec() {
    DatabaseQuery q{};
    matrix_set_scan_target(q, 42u, 7ull);
    assert(q.opcode == OP_SCAN);
    assert(matrix_get_scan_threshold(q) == 42u);
    assert(matrix_get_scan_column_id(q) == 7ull);

    DatabaseQuery q2{};
    matrix_set_scan_threshold(q2, 99u);              // legacy delegates to target(...,0)
    assert(matrix_get_scan_threshold(q2) == 99u);
    assert(matrix_get_scan_column_id(q2) == 0ull);   // legacy id is explicitly 0
    std::cout << "[codec] ok\n";
}

static void test_host_ptr() {
    std::vector<uint32_t> data(4);
    for (uint32_t i = 0; i < 4; ++i) data[i] = i * 10;          // 0,10,20,30
    TieredColumn col(101, reinterpret_cast<const unsigned char*>(data.data()),
                     data.size() * sizeof(uint32_t));
    assert(col.tier() == MemorySpace::HOST);
    const uint32_t* v = reinterpret_cast<const uint32_t*>(col.host_ptr());
    assert(v != nullptr && v[0] == 0 && v[1] == 10 && v[2] == 20 && v[3] == 30);

    const uint64_t cks = col.checksum();
    col.migrate_to(MemorySpace::COLD);
    assert(col.host_ptr() == nullptr);                          // bytes on SSD now
    col.migrate_to(MemorySpace::HOST);
    const uint32_t* v2 = reinterpret_cast<const uint32_t*>(col.host_ptr());
    assert(v2 != nullptr && v2[0] == 0 && v2[3] == 30);
    assert(col.checksum() == cks);                              // round-trip integrity
    std::cout << "[host_ptr] ok\n";
}

static void test_tiered_single_column() {
    const size_t N = 1000;
    std::vector<uint32_t> col(N);
    for (size_t i = 0; i < N; ++i) col[i] = static_cast<uint32_t>(i); // value[i]=i
    CPUMockEngine eng(0, "", /*host_cap=*/SIZE_MAX);                   // generous: no eviction
    eng.load_scan_column(1, col.data(), N);

    DatabaseQuery q{};
    const uint32_t T = 500;
    matrix_set_scan_target(q, T, 1);
    eng.execute_scan(q);
    assert(q.transaction_id == N - 1 - T);                            // count of i>T == 499
    assert(eng.column_tier(1) == MemorySpace::HOST);                  // fits budget, stays resident

    // legacy id==0 path still scans the fixed column correctly
    DatabaseQuery ql{};
    const uint32_t TL = 1000;
    matrix_set_scan_threshold(ql, TL);
    eng.execute_scan(ql);
    assert(ql.transaction_id == MATRIX_SCAN_COLUMN_SIZE - 1 - TL);
    std::cout << "[tiered single-column + legacy] ok\n";
}

static void test_eviction_holds_more_than_ram() {
    const size_t N = 1000;
    const size_t S = N * sizeof(uint32_t);            // 4000 bytes per column
    std::vector<uint32_t> col(N);
    for (size_t i = 0; i < N; ++i) col[i] = static_cast<uint32_t>(i);

    CPUMockEngine eng(0, "", /*host_cap=*/2 * S);      // room for 2 of 3 columns
    eng.load_scan_column(1, col.data(), N);
    eng.load_scan_column(2, col.data(), N);
    eng.load_scan_column(3, col.data(), N);            // 3*S > 2*S: more than fits in RAM

    // Hot/cold: scan cols 1 & 2 each round, NEVER col 3 -> col 3 heat stays 0 (the victim).
    const uint32_t T = 250;
    for (int round = 0; round < 8; ++round) {
        for (uint64_t id : {1ull, 2ull}) {
            DatabaseQuery q{};
            matrix_set_scan_target(q, T, id);
            eng.execute_scan(q);
            assert(q.transaction_id == N - 1 - T);     // hot scans stay correct (749)
        }
    }
    // 16 tiered scans -> 4 rebalances (REBALANCE_EVERY=4). MIN_RESIDENCY_TICKS=2 means the
    // 1st rebalance evicts nothing; col 3 demotes on the 2nd. Final state is deterministic:
    assert(eng.manager_tier(3) == MemorySpace::COLD);  // brain demoted the cold column
    assert(eng.column_tier(3)  == MemorySpace::COLD);  // and the bytes are actually on SSD
    assert(eng.manager_tier(1) == MemorySpace::HOST);  // hot columns retained
    assert(eng.manager_tier(2) == MemorySpace::HOST);
    assert(eng.host_resident_bytes() <= 2 * S);        // budget respected
    static_assert(3 * (1000 * sizeof(uint32_t)) > 2 * (1000 * sizeof(uint32_t)),
                  "catalog holds more bytes than the RAM budget");

    // Scan the COLD column: borrowed to HOST, scanned, returned to COLD; result still correct.
    const uint64_t cks_before = eng.column_checksum(3);
    DatabaseQuery q3{};
    matrix_set_scan_target(q3, T, 3);
    eng.execute_scan(q3);
    assert(q3.transaction_id == N - 1 - T);            // pull-back-correct (749)
    assert(eng.column_tier(3) == MemorySpace::COLD);   // borrow returned: rest tier == brain
    assert(eng.column_checksum(3) == cks_before);      // demote+borrow preserved the bytes
    std::cout << "[eviction holds-more-than-RAM + borrow] ok\n";
}

static void test_repromotion_under_pressure() {
    const size_t N = 1000;
    const size_t S = N * sizeof(uint32_t);
    std::vector<uint32_t> col(N);
    for (size_t i = 0; i < N; ++i) col[i] = static_cast<uint32_t>(i);

    CPUMockEngine eng(0, "", /*host_cap=*/2 * S);   // room for 2 of 3 columns
    eng.load_scan_column(1, col.data(), N);
    eng.load_scan_column(2, col.data(), N);
    eng.load_scan_column(3, col.data(), N);

    const uint32_t T = 250;
    auto scan = [&](uint64_t id) {
        DatabaseQuery q{};
        matrix_set_scan_target(q, T, id);
        eng.execute_scan(q);
        assert(q.transaction_id == N - 1 - T);       // 749, regardless of tier
    };

    // Phase 1: cols 1 & 2 hot, col 3 never -> col 3 demoted to COLD (the INT-1 baseline).
    for (int r = 0; r < 8; ++r) { scan(1); scan(2); }
    assert(eng.column_tier(3) == MemorySpace::COLD && "phase 1: col 3 demoted to SSD");

    // Phase 2: FLIP the heat — col 3 scanned 3x/round (decisively hot, clears the COLD->HOST
    // promotion gate), col 1 once, col 2 NEVER. Col 2 cools to keep_score 0; col 3 re-heats and
    // is RE-PROMOTED to resident HOST (swap-on-promote displaces the now-cold col 2).
    for (int r = 0; r < 16; ++r) { scan(3); scan(1); scan(3); scan(3); }
    assert(eng.column_tier(3)  == MemorySpace::HOST && "col 3 re-promoted to RESIDENT RAM (not just borrowed)");
    assert(eng.manager_tier(3) == MemorySpace::HOST && "brain agrees col 3 is resident");
    assert(eng.column_tier(2)  == MemorySpace::COLD && "col 2 displaced to SSD");
    assert(eng.host_resident_bytes() <= 2 * S       && "HOST within budget");
    std::cout << "[re-promotion under pressure] ok\n";
}

int main() {
    test_codec();
    test_host_ptr();
    test_tiered_single_column();
    test_eviction_holds_more_than_ram();
    test_repromotion_under_pressure();
    std::cout << "ALL LIVE-TIERING TESTS PASSED\n";
    return 0;
}

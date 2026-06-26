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

int main() {
    test_codec();
    test_host_ptr();
    test_tiered_single_column();
    std::cout << "ALL LIVE-TIERING TESTS PASSED\n";
    return 0;
}

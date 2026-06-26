// CPU test for the live tiering integration. Grows across tasks; one main() runs all.
#include "compute_mock.cpp"   // CPUMockEngine + compute.hpp (codec) + tiering headers
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

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

int main() {
    test_codec();
    std::cout << "ALL LIVE-TIERING TESTS PASSED\n";
    return 0;
}

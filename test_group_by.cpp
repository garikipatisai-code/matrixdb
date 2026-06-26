// CPU test for grouped aggregation (GROUP BY). Grows across tasks; one main() runs all.
#include "compute_mock.cpp"   // CPUMockEngine + compute.hpp (matrix_cpu_group_reduce)
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

// Hand-worked: keys/vals below, G=3 groups.
//   g0 rows = {5,9,15} (indices 0,2,5);  g1 = {7,13} (1,4);  g2 = {11} (3)
static void test_group_reduce_handworked() {
    const std::vector<uint32_t> keys = {0, 1, 0, 2, 1, 0};
    const std::vector<uint32_t> vals = {5, 7, 9, 11, 13, 15};
    const uint32_t G = 3;
    std::vector<uint64_t> out(G);
    matrix_cpu_group_reduce(keys.data(), vals.data(), keys.size(), G, AGG_COUNT, out.data());
    assert((out == std::vector<uint64_t>{3, 2, 1}));
    matrix_cpu_group_reduce(keys.data(), vals.data(), keys.size(), G, AGG_SUM, out.data());
    assert((out == std::vector<uint64_t>{29, 20, 11}));   // 5+9+15, 7+13, 11
    matrix_cpu_group_reduce(keys.data(), vals.data(), keys.size(), G, AGG_MIN, out.data());
    assert((out == std::vector<uint64_t>{5, 7, 11}));
    matrix_cpu_group_reduce(keys.data(), vals.data(), keys.size(), G, AGG_MAX, out.data());
    assert((out == std::vector<uint64_t>{15, 13, 11}));
    std::cout << "[group reduce hand-worked] ok\n";
}

static void test_group_reduce_edge() {
    // G=4 -> group 3 is empty; plus an out-of-range key (10) that must be ignored.
    const std::vector<uint32_t> keys = {0, 1, 0, 2, 1, 0, 10};
    const std::vector<uint32_t> vals = {5, 7, 9, 11, 13, 15, 999};
    const uint32_t G = 4;
    std::vector<uint64_t> out(G);
    matrix_cpu_group_reduce(keys.data(), vals.data(), keys.size(), G, AGG_COUNT, out.data());
    assert((out == std::vector<uint64_t>{3, 2, 1, 0}));        // group 3 empty -> 0; key 10 ignored
    matrix_cpu_group_reduce(keys.data(), vals.data(), keys.size(), G, AGG_SUM, out.data());
    assert((out == std::vector<uint64_t>{29, 20, 11, 0}));     // 999 (key 10) NOT summed anywhere
    matrix_cpu_group_reduce(keys.data(), vals.data(), keys.size(), G, AGG_MIN, out.data());
    assert((out == std::vector<uint64_t>{5, 7, 11, UINT64_MAX})); // empty-group MIN sentinel
    matrix_cpu_group_reduce(keys.data(), vals.data(), keys.size(), G, AGG_MAX, out.data());
    assert((out == std::vector<uint64_t>{15, 13, 11, 0}));     // empty-group MAX sentinel
    std::cout << "[group reduce edge] ok\n";
}

int main() {
    test_group_reduce_handworked();
    test_group_reduce_edge();
    std::cout << "ALL GROUP-BY TESTS PASSED\n";
    return 0;
}

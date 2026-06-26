// CPU test for analytical aggregations. Grows across tasks; one main() runs all.
#include "compute_mock.cpp"   // CPUMockEngine + compute.hpp (codec + reducer)
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

static void test_reduce_closed_form() {
    const size_t N = 1000;
    std::vector<uint32_t> v(N);
    for (size_t i = 0; i < N; ++i) v[i] = static_cast<uint32_t>(i); // value[i]=i
    const uint32_t T = 600;
    const uint64_t count = N - 1 - T;            // # of i>T in [0,N)
    const uint64_t mn = T + 1;
    const uint64_t mx = N - 1;
    uint64_t sum = 0; for (uint64_t i = T + 1; i <= N - 1; ++i) sum += i;
    assert(matrix_cpu_reduce(v.data(), N, T, AGG_COUNT) == count);
    assert(matrix_cpu_reduce(v.data(), N, T, AGG_SUM)   == sum);
    assert(matrix_cpu_reduce(v.data(), N, T, AGG_MIN)   == mn);
    assert(matrix_cpu_reduce(v.data(), N, T, AGG_MAX)   == mx);
    assert(sum != count); // non-vacuity: a stub returning count for every op would fail here
    std::cout << "[reduce closed-form] ok\n";
}

static void test_reduce_empty() {
    const size_t N = 1000;
    std::vector<uint32_t> v(N);
    for (size_t i = 0; i < N; ++i) v[i] = static_cast<uint32_t>(i);
    const uint32_t T = N - 1; // nothing is > N-1
    assert(matrix_cpu_reduce(v.data(), N, T, AGG_COUNT) == 0);
    assert(matrix_cpu_reduce(v.data(), N, T, AGG_SUM)   == 0);
    assert(matrix_cpu_reduce(v.data(), N, T, AGG_MIN)   == UINT64_MAX);
    assert(matrix_cpu_reduce(v.data(), N, T, AGG_MAX)   == 0);
    std::cout << "[reduce empty-set] ok\n";
}

static void test_agg_codec() {
    DatabaseQuery q{};
    matrix_set_scan_target(q, 50u, 9ull);
    assert(matrix_get_scan_agg_op(q) == AGG_COUNT);  // default 0 when not set
    matrix_set_scan_agg_op(q, AGG_SUM);
    assert(matrix_get_scan_agg_op(q) == AGG_SUM);
    assert(matrix_get_scan_threshold(q) == 50u);     // not disturbed
    assert(matrix_get_scan_column_id(q) == 9ull);    // not disturbed
    std::cout << "[agg codec] ok\n";
}

int main() {
    test_reduce_closed_form();
    test_reduce_empty();
    test_agg_codec();
    std::cout << "ALL AGGREGATION TESTS PASSED\n";
    return 0;
}

// CPU test for query input validation (execute_query returns MatrixQueryStatus, never crashes).
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

static void test_validation() {
    const size_t N = 1000;
    std::vector<uint32_t> a(N), b(500), keys(500);
    for (size_t i = 0; i < N; ++i) a[i] = static_cast<uint32_t>(i);
    for (size_t i = 0; i < 500; ++i) { b[i] = static_cast<uint32_t>(i); keys[i] = static_cast<uint32_t>(i % 4); }
    CPUMockEngine eng(0, "", SIZE_MAX);
    eng.load_scan_column(5, a.data(), N);        // len 1000
    eng.load_scan_column(6, b.data(), 500);      // len 500
    eng.load_scan_column(7, keys.data(), 500);   // len 500, keys i%4
    std::vector<uint64_t> out;
    using S = MatrixQueryStatus;

    assert(eng.execute_query(MatrixQuery{.value_col = 5, .agg = AGG_SUM}, out) == S::OK && out.size() == 1);
    assert(eng.execute_query(MatrixQuery{.value_col = 0, .agg = AGG_SUM}, out) == S::ERR_UNKNOWN_COLUMN && out.empty());
    assert(eng.execute_query(MatrixQuery{.value_col = 999, .agg = AGG_SUM}, out) == S::ERR_UNKNOWN_COLUMN && out.empty());
    assert(eng.execute_query(MatrixQuery{.value_col = 5, .agg = AGG_SUM, .grouped = true, .key_col = 5, .num_groups = 4}, out) == S::ERR_INVALID_GROUP);
    assert(eng.execute_query(MatrixQuery{.value_col = 5, .agg = AGG_SUM, .grouped = true, .key_col = 999, .num_groups = 4}, out) == S::ERR_INVALID_GROUP);
    assert(eng.execute_query(MatrixQuery{.value_col = 6, .agg = AGG_SUM, .grouped = true, .key_col = 7, .num_groups = 0}, out) == S::ERR_INVALID_GROUP);
    assert(eng.execute_query(MatrixQuery{.value_col = 5, .agg = AGG_SUM, .grouped = true, .key_col = 7, .num_groups = 4}, out) == S::ERR_INVALID_GROUP); // len 1000 vs 500
    assert(eng.execute_query(MatrixQuery{.value_col = 6, .agg = AGG_SUM, .grouped = true, .key_col = 7, .num_groups = (1u << 28) + 1}, out) == S::ERR_TOO_MANY_GROUPS);
    assert(eng.execute_query(MatrixQuery{.value_col = 6, .agg = AGG_SUM, .grouped = true, .key_col = 7, .num_groups = 4}, out) == S::OK && out.size() == 4);
    std::cout << "[query validation] ok\n";
}

int main() {
    test_validation();
    std::cout << "ALL QUERY-VALIDATION TESTS PASSED\n";
    return 0;
}

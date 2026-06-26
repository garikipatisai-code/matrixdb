// CPU test for the unified query API (MatrixQuery / execute_query). Grows; one main().
#include "compute_mock.cpp"   // CPUMockEngine + compute.hpp (MatrixQuery, matrix_cpu_reduce_all)
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

static std::vector<uint64_t> brute_group(const std::vector<uint32_t>& keys,
                                         const std::vector<uint32_t>& vals,
                                         uint32_t G, MatrixAggOp op, bool filt, uint32_t T) {
    std::vector<uint64_t> out(G, op == AGG_MIN ? UINT64_MAX : 0);
    for (size_t i = 0; i < keys.size(); ++i) {
        const uint32_t k = keys[i]; if (k >= G) continue;
        if (filt && vals[i] <= T) continue;
        if (op == AGG_SUM) out[k] += vals[i];
        else if (op == AGG_MIN) { if (vals[i] < out[k]) out[k] = vals[i]; }
        else if (op == AGG_MAX) { if (vals[i] > out[k]) out[k] = vals[i]; }
        else out[k] += 1;
    }
    return out;
}

static void test_reduce_all() {
    std::vector<uint32_t> v(1000);
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<uint32_t>(i);
    assert(matrix_cpu_reduce_all(v.data(), v.size(), AGG_COUNT) == 1000);
    assert(matrix_cpu_reduce_all(v.data(), v.size(), AGG_SUM)   == 499500ull); // 999*1000/2
    assert(matrix_cpu_reduce_all(v.data(), v.size(), AGG_MIN)   == 0);
    assert(matrix_cpu_reduce_all(v.data(), v.size(), AGG_MAX)   == 999);
    std::cout << "[reduce_all] ok\n";
}

static void test_query_routes() {
    const size_t N = 1000; const uint32_t G = 8;
    std::vector<uint32_t> vals(N), keys(N);
    for (size_t i = 0; i < N; ++i) { vals[i] = static_cast<uint32_t>(i); keys[i] = static_cast<uint32_t>(i % G); }
    CPUMockEngine eng(0, "", /*host_cap=*/SIZE_MAX);
    eng.load_scan_column(1, keys.data(), N);   // key column
    eng.load_scan_column(2, vals.data(), N);   // value column
    std::vector<uint64_t> out;

    eng.execute_query(MatrixQuery{.value_col = 2, .agg = AGG_SUM}, out);
    assert(out.size() == 1 && out[0] == 499500ull);
    eng.execute_query(MatrixQuery{.value_col = 2, .agg = AGG_SUM, .has_filter = true, .threshold = 500}, out);
    assert(out.size() == 1 && out[0] == 374250ull);   // 501..999; differs from unfiltered (non-vacuous)
    eng.execute_query(MatrixQuery{.value_col = 2, .agg = AGG_SUM, .grouped = true, .key_col = 1, .num_groups = G}, out);
    assert(out == brute_group(keys, vals, G, AGG_SUM, false, 0));
    eng.execute_query(MatrixQuery{.value_col = 2, .agg = AGG_SUM, .has_filter = true, .threshold = 500,
                                  .grouped = true, .key_col = 1, .num_groups = G}, out);
    assert(out == brute_group(keys, vals, G, AGG_SUM, true, 500));
    std::cout << "[query routes] ok\n";
}

static void test_query_cold() {
    const size_t N = 1000;
    std::vector<uint32_t> vals(N), dummy(N, 0);
    for (size_t i = 0; i < N; ++i) vals[i] = static_cast<uint32_t>(i);
    CPUMockEngine eng(0, "", /*host_cap=*/N * sizeof(uint32_t));  // RAM holds ONE column
    eng.load_scan_column(2, vals.data(), N);
    eng.load_scan_column(3, dummy.data(), N);
    for (int r = 0; r < 12; ++r) { DatabaseQuery q{}; matrix_set_scan_target(q, 0u, 3); eng.execute_scan(q); }
    assert(eng.column_tier(2) == MemorySpace::COLD && "value column demoted to SSD");
    std::vector<uint64_t> out;
    eng.execute_query(MatrixQuery{.value_col = 2, .agg = AGG_SUM}, out);   // unfiltered, COLD borrow
    assert(out.size() == 1 && out[0] == 499500ull);
    assert(eng.column_tier(2) == MemorySpace::COLD && "value column returned to SSD after the borrow");
    std::cout << "[query cold-borrow] ok\n";
}

int main() {
    test_reduce_all();
    test_query_routes();
    test_query_cold();
    std::cout << "ALL QUERY TESTS PASSED\n";
    return 0;
}

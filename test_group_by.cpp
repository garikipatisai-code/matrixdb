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

// Independent reference for the scale/COLD test (brute-force grouping).
static std::vector<uint64_t> brute_group(const std::vector<uint32_t>& keys,
                                         const std::vector<uint32_t>& vals,
                                         uint32_t G, MatrixAggOp op) {
    std::vector<uint64_t> out(G, op == AGG_MIN ? UINT64_MAX : 0);
    for (size_t i = 0; i < keys.size(); ++i) {
        const uint32_t k = keys[i]; if (k >= G) continue;
        if (op == AGG_SUM) out[k] += vals[i];
        else if (op == AGG_MIN) { if (vals[i] < out[k]) out[k] = vals[i]; }
        else if (op == AGG_MAX) { if (vals[i] > out[k]) out[k] = vals[i]; }
        else out[k] += 1;
    }
    return out;
}

static void test_engine_group_by_host() {
    const std::vector<uint32_t> keys = {0, 1, 0, 2, 1, 0};
    const std::vector<uint32_t> vals = {5, 7, 9, 11, 13, 15};
    CPUMockEngine eng(0, "", /*host_cap=*/SIZE_MAX);
    eng.load_scan_column(1, keys.data(), keys.size());     // key column
    eng.load_scan_column(2, vals.data(), vals.size());     // value column
    std::vector<uint64_t> out;
    eng.grouped_aggregate(1, 2, /*num_groups=*/3, AGG_SUM, out);
    assert((out == std::vector<uint64_t>{29, 20, 11}));
    eng.grouped_aggregate(1, 2, 3, AGG_MAX, out);
    assert((out == std::vector<uint64_t>{15, 13, 11}));
    eng.grouped_aggregate(1, 2, 3, AGG_MIN, out);            // MIN through the engine (incl. its sentinel path)
    assert((out == std::vector<uint64_t>{5, 7, 11}));
    std::cout << "[engine group-by HOST] ok\n";
}

static void test_engine_group_by_cold() {
    // Both key and value demoted to COLD, then grouped — exercises the DOUBLE borrow-and-return.
    const size_t N = 1000;
    const uint32_t G = 8;
    std::vector<uint32_t> keys(N), vals(N), dummy(N, 0);
    for (size_t i = 0; i < N; ++i) { keys[i] = static_cast<uint32_t>(i % G); vals[i] = static_cast<uint32_t>(i); }
    CPUMockEngine eng(0, "", /*host_cap=*/N * sizeof(uint32_t));  // RAM holds ONE column
    eng.load_scan_column(1, keys.data(), N);
    eng.load_scan_column(2, vals.data(), N);
    eng.load_scan_column(3, dummy.data(), N);
    // Hammer only the dummy column -> keys(1) and vals(2) go idle -> both demoted to COLD.
    for (int r = 0; r < 12; ++r) { DatabaseQuery q{}; matrix_set_scan_target(q, 0u, 3); eng.execute_scan(q); }
    assert(eng.column_tier(1) == MemorySpace::COLD && eng.column_tier(2) == MemorySpace::COLD);
    std::vector<uint64_t> out;
    eng.grouped_aggregate(1, 2, G, AGG_SUM, out);              // double borrow COLD->HOST->reduce->COLD
    assert(out == brute_group(keys, vals, G, AGG_SUM));
    eng.grouped_aggregate(1, 2, G, AGG_COUNT, out);
    assert(out == brute_group(keys, vals, G, AGG_COUNT));
    assert(eng.column_tier(1) == MemorySpace::COLD && eng.column_tier(2) == MemorySpace::COLD); // borrows returned
    std::cout << "[engine group-by COLD double-borrow] ok\n";
}

static void test_group_reduce_where() {
    const std::vector<uint32_t> keys = {0, 1, 0, 2, 1, 0};
    const std::vector<uint32_t> vals = {5, 7, 9, 11, 13, 15};
    const uint32_t G = 3;
    std::vector<uint64_t> out(G);
    // WHERE value > 8 -> kept: g0{9,15}, g1{13}, g2{11}
    matrix_cpu_group_reduce_where(keys.data(), vals.data(), keys.size(), G, AGG_COUNT, 8, out.data());
    assert((out == std::vector<uint64_t>{2, 1, 1}));
    matrix_cpu_group_reduce_where(keys.data(), vals.data(), keys.size(), G, AGG_SUM, 8, out.data());
    assert((out == std::vector<uint64_t>{24, 13, 11}));   // 9+15, 13, 11
    matrix_cpu_group_reduce_where(keys.data(), vals.data(), keys.size(), G, AGG_MIN, 8, out.data());
    assert((out == std::vector<uint64_t>{9, 13, 11}));
    matrix_cpu_group_reduce_where(keys.data(), vals.data(), keys.size(), G, AGG_MAX, 8, out.data());
    assert((out == std::vector<uint64_t>{15, 13, 11}));
    // non-vacuity + regression: unfiltered wrapper still gives GBY-1's results
    matrix_cpu_group_reduce(keys.data(), vals.data(), keys.size(), G, AGG_SUM, out.data());
    assert((out == std::vector<uint64_t>{29, 20, 11}));
    // WHERE value > 12 -> g2 emptied BY the filter
    matrix_cpu_group_reduce_where(keys.data(), vals.data(), keys.size(), G, AGG_COUNT, 12, out.data());
    assert((out == std::vector<uint64_t>{1, 1, 0}));
    matrix_cpu_group_reduce_where(keys.data(), vals.data(), keys.size(), G, AGG_MIN, 12, out.data());
    assert((out == std::vector<uint64_t>{15, 13, UINT64_MAX}));
    std::cout << "[group reduce WHERE] ok\n";
}

static void test_engine_group_by_where() {
    const std::vector<uint32_t> keys = {0, 1, 0, 2, 1, 0};
    const std::vector<uint32_t> vals = {5, 7, 9, 11, 13, 15};
    CPUMockEngine eng(0, "", /*host_cap=*/SIZE_MAX);
    eng.load_scan_column(1, keys.data(), keys.size());
    eng.load_scan_column(2, vals.data(), vals.size());
    std::vector<uint64_t> out;
    eng.grouped_aggregate_where(1, 2, /*num_groups=*/3, AGG_SUM, /*threshold=*/8, out);
    assert((out == std::vector<uint64_t>{24, 13, 11}));
    eng.grouped_aggregate_where(1, 2, 3, AGG_COUNT, 8, out);
    assert((out == std::vector<uint64_t>{2, 1, 1}));
    std::cout << "[engine group-by WHERE] ok\n";
}

int main() {
    test_group_reduce_handworked();
    test_group_reduce_edge();
    test_engine_group_by_host();
    test_engine_group_by_cold();
    test_group_reduce_where();
    test_engine_group_by_where();
    std::cout << "ALL GROUP-BY TESTS PASSED\n";
    return 0;
}

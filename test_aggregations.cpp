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

static void test_engine_agg_legacy() {
    CPUMockEngine eng(0);                          // legacy fixed column (id 0)
    const uint64_t SIZE = MATRIX_SCAN_COLUMN_SIZE;
    const uint32_t T = 8000000;                    // < SIZE
    auto run = [&](MatrixAggOp op) {
        DatabaseQuery q{}; matrix_set_scan_target(q, T, 0); matrix_set_scan_agg_op(q, op);
        eng.execute_scan(q); return q.transaction_id;
    };
    const uint64_t cnt = SIZE - 1 - T;
    assert(run(AGG_COUNT) == cnt);
    assert(run(AGG_MIN)   == static_cast<uint64_t>(T) + 1);
    assert(run(AGG_MAX)   == SIZE - 1);
    const uint64_t sum = (static_cast<uint64_t>(SIZE - 1) + (static_cast<uint64_t>(T) + 1)) * cnt / 2;
    assert(run(AGG_SUM)   == sum);
    std::cout << "[engine agg legacy] ok\n";
}

static void test_engine_agg_tiered() {
    const size_t N = 1000;
    std::vector<uint32_t> col(N);
    for (size_t i = 0; i < N; ++i) col[i] = static_cast<uint32_t>(i);
    CPUMockEngine eng(0, "", /*host_cap=*/SIZE_MAX);
    eng.load_scan_column(1, col.data(), N);
    const uint32_t T = 600;
    auto run = [&](MatrixAggOp op) {
        DatabaseQuery q{}; matrix_set_scan_target(q, T, 1); matrix_set_scan_agg_op(q, op);
        eng.execute_scan(q); return q.transaction_id;
    };
    const uint64_t cnt = N - 1 - T;
    assert(run(AGG_COUNT) == cnt);
    assert(run(AGG_MIN)   == static_cast<uint64_t>(T) + 1);
    assert(run(AGG_MAX)   == N - 1);
    uint64_t sum = 0; for (uint64_t i = T + 1; i <= N - 1; ++i) sum += i;
    assert(run(AGG_SUM)   == sum);
    std::cout << "[engine agg tiered] ok\n";
}

static void test_engine_agg_cold_borrow() {
    // Aggregate a NON-COUNT op over a column that has actually been demoted to COLD — exercises
    // the reducer through the borrow path (SSD->RAM->reduce->SSD), the headline use case, not just
    // a HOST-resident column.
    const size_t N = 1000;
    const size_t S = N * sizeof(uint32_t);
    std::vector<uint32_t> col(N);
    for (size_t i = 0; i < N; ++i) col[i] = static_cast<uint32_t>(i);
    CPUMockEngine eng(0, "", /*host_cap=*/S);          // RAM holds ONE column
    eng.load_scan_column(1, col.data(), N);
    eng.load_scan_column(2, col.data(), N);            // 2*S > budget: one must be demoted
    // Hammer col 2, never col 1 -> col 1 (heat 0) is the eviction victim -> demoted to COLD.
    for (int r = 0; r < 12; ++r) {
        DatabaseQuery q{}; matrix_set_scan_target(q, 0u, 2); eng.execute_scan(q);
    }
    assert(eng.column_tier(1) == MemorySpace::COLD && "col 1 demoted to SSD");
    // SUM over the COLD column: borrowed to HOST, reduced, returned to COLD.
    const uint32_t T = 600;
    DatabaseQuery qs{}; matrix_set_scan_target(qs, T, 1); matrix_set_scan_agg_op(qs, AGG_SUM);
    eng.execute_scan(qs);
    uint64_t sum = 0; for (uint64_t i = T + 1; i <= N - 1; ++i) sum += i;
    assert(qs.transaction_id == sum && "SUM correct over a COLD-borrowed column");
    assert(eng.column_tier(1) == MemorySpace::COLD && "col 1 returned to SSD after the borrow");
    std::cout << "[engine agg cold-borrow] ok\n";
}

int main() {
    test_reduce_closed_form();
    test_reduce_empty();
    test_agg_codec();
    test_engine_agg_legacy();
    test_engine_agg_tiered();
    test_engine_agg_cold_borrow();
    std::cout << "ALL AGGREGATION TESTS PASSED\n";
    return 0;
}

// CPU test for nullable columns (DM-3j): set_column_nulls marks NULL rows; unfiltered scalar aggregates
// skip them (SQL NULL semantics). A maskless column is byte-identical to before (oracle-safe).
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <bit>
#include <vector>
#include <iostream>

static uint64_t agg_query(CPUMockEngine& eng, uint64_t id, MatrixAggOp op) {
    MatrixQuery q{}; q.value_col = id; q.agg = op; std::vector<uint64_t> o;
    assert(eng.execute_query(q, o) == MatrixQueryStatus::OK && o.size() == 1);
    return o[0];
}

static void test_nullable() {
    std::vector<uint32_t> v = {10, 20, 30, 40, 50};
    CPUMockEngine eng;
    eng.load_scan_column(2, v.data(), v.size());
    // before any mask: full aggregates (existing path, unchanged)
    assert(agg_query(eng, 2, AGG_SUM) == 150 && agg_query(eng, 2, AGG_COUNT) == 5 && "maskless == full");
    // mark rows 1 and 3 NULL (values 20, 40) -> non-null = {10, 30, 50}
    eng.set_column_nulls(2, {0, 1, 0, 1, 0});
    assert(agg_query(eng, 2, AGG_COUNT) == 3 && "COUNT counts non-null only");
    assert(agg_query(eng, 2, AGG_SUM)   == 10 + 30 + 50 && "SUM skips nulls");
    assert(agg_query(eng, 2, AGG_MIN)   == 10 && agg_query(eng, 2, AGG_MAX) == 50 && "MIN/MAX over non-null");
    // non-vacuity: with nulls the SUM differs from the full SUM
    assert(agg_query(eng, 2, AGG_SUM) != 150 && "nulls actually excluded");

    // all-null column -> empty-set sentinels
    std::vector<uint32_t> w = {7, 8, 9};
    eng.load_scan_column(3, w.data(), w.size());
    eng.set_column_nulls(3, {1, 1, 1});
    assert(agg_query(eng, 3, AGG_COUNT) == 0 && agg_query(eng, 3, AGG_SUM) == 0);
    assert(agg_query(eng, 3, AGG_MIN) == UINT64_MAX && agg_query(eng, 3, AGG_MAX) == 0 && "all-null sentinels");
    std::cout << "[nullable] ok\n";
}

static void test_nullable_typed() {
    CPUMockEngine eng;
    std::vector<int64_t> s = {-7, 100, 5000000000LL, -3};
    eng.load_scan_column_i64(7, s.data(), s.size());
    eng.set_column_nulls(7, {0, 1, 0, 1});                 // null rows 1,3 -> non-null {-7, 5e9}
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_SUM; std::vector<uint64_t> o; eng.execute_query(q, o);
      assert(static_cast<int64_t>(o[0]) == -7 + 5000000000LL && "int64 nullable SUM skips nulls"); }
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_COUNT; std::vector<uint64_t> o; eng.execute_query(q, o);
      assert(o[0] == 2 && "int64 nullable COUNT = non-null"); }

    std::vector<double> d = {1.5, -2.5, 3.0};
    eng.load_scan_column_f64(8, d.data(), d.size());
    eng.set_column_nulls(8, {0, 0, 1});                    // null row 2 -> non-null {1.5, -2.5}
    { MatrixQuery q{}; q.value_col = 8; q.agg = AGG_SUM; std::vector<uint64_t> o; eng.execute_query(q, o);
      assert(std::bit_cast<double>(o[0]) == 1.5 - 2.5 && "double nullable SUM skips nulls"); }
    std::cout << "[nullable typed] ok\n";
}

int main() { test_nullable(); test_nullable_typed(); std::cout << "ALL NULLABLE TESTS PASSED\n"; return 0; }

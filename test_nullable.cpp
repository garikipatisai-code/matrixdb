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

// Filtered scalar aggregates now also skip NULLs: WHERE + a null mask excludes a row whether or not it
// matches the predicate. Across U32/I64/F64.
static void test_filtered_nullable() {
    CPUMockEngine eng;
    std::vector<uint32_t> v = {10, 20, 30, 40, 50};
    eng.load_scan_column(2, v.data(), v.size());
    // WHERE value >= 30 with NO mask -> {30,40,50}: SUM 120 (baseline for non-vacuity)
    { MatrixQuery q{}; q.value_col = 2; q.agg = AGG_SUM; q.has_filter = true; q.cmp = MatrixCmp::GE; q.threshold = 30;
      std::vector<uint64_t> o; eng.execute_query(q, o); assert(o[0] == 120 && "maskless filtered baseline"); }
    // mask rows 1,4 NULL (values 20,50). WHERE value >= 30 -> non-null AND >=30 = {30,40}: SUM 70, COUNT 2
    eng.set_column_nulls(2, {0, 1, 0, 0, 1});
    { MatrixQuery q{}; q.value_col = 2; q.agg = AGG_SUM; q.has_filter = true; q.cmp = MatrixCmp::GE; q.threshold = 30;
      std::vector<uint64_t> o; eng.execute_query(q, o);
      assert(o[0] == 70 && "filtered SUM skips null row 4 (50 excluded: 120 -> 70)"); }
    { MatrixQuery q{}; q.value_col = 2; q.agg = AGG_COUNT; q.has_filter = true; q.cmp = MatrixCmp::GE; q.threshold = 30;
      std::vector<uint64_t> o; eng.execute_query(q, o); assert(o[0] == 2 && "filtered COUNT of non-null matches"); }

    // int64 filtered + nullable: {-5,100,7,200}, null row1 (100), WHERE value > 0 -> {7,200}: SUM 207
    std::vector<int64_t> s = {-5, 100, 7, 200};
    eng.load_scan_column_i64(3, s.data(), s.size());
    eng.set_column_nulls(3, {0, 1, 0, 0});
    { MatrixQuery q{}; q.value_col = 3; q.agg = AGG_SUM; q.has_filter = true; q.cmp = MatrixCmp::GT; q.lo_i64 = 0;
      std::vector<uint64_t> o; eng.execute_query(q, o);
      assert(static_cast<int64_t>(o[0]) == 207 && "int64 filtered+nullable SUM"); }

    // double filtered + nullable: {1.5,2.5,3.5}, null row2 (3.5), WHERE value < 3.0 -> {1.5,2.5}: SUM 4.0
    std::vector<double> d = {1.5, 2.5, 3.5};
    eng.load_scan_column_f64(4, d.data(), d.size());
    eng.set_column_nulls(4, {0, 0, 1});
    { MatrixQuery q{}; q.value_col = 4; q.agg = AGG_SUM; q.has_filter = true; q.cmp = MatrixCmp::LT; q.lo_f64 = 3.0;
      std::vector<uint64_t> o; eng.execute_query(q, o);
      assert(std::bit_cast<double>(o[0]) == 4.0 && "double filtered+nullable SUM"); }
    std::cout << "[filtered nullable] ok\n";
}

static void grp(CPUMockEngine& eng, uint64_t key, uint64_t val, uint32_t ng, MatrixAggOp op, std::vector<uint64_t>& o) {
    MatrixQuery q{}; q.value_col = val; q.key_col = key; q.num_groups = ng; q.grouped = true; q.agg = op;
    assert(eng.execute_query(q, o) == MatrixQueryStatus::OK);
}

// Grouped aggregates now skip NULL rows too (the documented follow-up to scalar null-awareness): a NULL
// row contributes to no group. Closes the scalar-vs-grouped asymmetry across U32/I64/F64.
static void test_grouped_nullable() {
    CPUMockEngine eng;
    std::vector<uint32_t> region = {0, 1, 0, 2, 1}, amount = {10, 20, 30, 40, 50};
    eng.load_scan_column(1, region.data(), region.size());
    eng.load_scan_column(2, amount.data(), amount.size());
    std::vector<uint64_t> o;
    // baseline (maskless): g0 = 10+30 = 40, g1 = 20+50 = 70, g2 = 40  (oracle-safe: unchanged path)
    grp(eng, 1, 2, 3, AGG_SUM, o);
    assert(o[0] == 40 && o[1] == 70 && o[2] == 40 && "maskless grouped SUM unchanged");
    // mark row 2 (region 0, amount 30) NULL -> g0 loses it
    eng.set_column_nulls(2, {0, 0, 1, 0, 0});
    grp(eng, 1, 2, 3, AGG_SUM, o);
    assert(o[0] == 10 && o[1] == 70 && o[2] == 40 && "grouped SUM skips the null row (g0: 40 -> 10)");
    grp(eng, 1, 2, 3, AGG_COUNT, o);
    assert(o[0] == 1 && o[1] == 2 && o[2] == 1 && "grouped COUNT counts non-null per group");
    grp(eng, 1, 2, 3, AGG_MAX, o);
    assert(o[0] == 10 && "grouped MAX over g0 non-null is 10 (30 excluded)");

    // int64 grouped nullable
    std::vector<int64_t> iv = {-5, 100, 7, 5000000000LL};            // regions {0,0,1,1}
    std::vector<uint32_t> ik = {0, 0, 1, 1};
    eng.load_scan_column(3, ik.data(), ik.size());
    eng.load_scan_column_i64(4, iv.data(), iv.size());
    eng.set_column_nulls(4, {0, 1, 0, 0});                           // null row1 (region 0, val 100)
    grp(eng, 3, 4, 2, AGG_SUM, o);
    assert(static_cast<int64_t>(o[0]) == -5 && static_cast<int64_t>(o[1]) == 7 + 5000000000LL
           && "int64 grouped SUM skips null (g0: -5 only)");

    // double grouped nullable
    std::vector<double> dv = {1.5, 2.5, 4.0, 8.0};                   // regions {0,0,1,1}
    eng.load_scan_column(5, ik.data(), ik.size());
    eng.load_scan_column_f64(6, dv.data(), dv.size());
    eng.set_column_nulls(6, {1, 0, 0, 0});                           // null row0 (region 0, val 1.5)
    grp(eng, 5, 6, 2, AGG_SUM, o);
    assert(std::bit_cast<double>(o[0]) == 2.5 && std::bit_cast<double>(o[1]) == 12.0
           && "double grouped SUM skips null (g0: 2.5 only)");
    std::cout << "[grouped nullable] ok\n";
}

int main() { test_nullable(); test_nullable_typed(); test_filtered_nullable(); test_grouped_nullable(); std::cout << "ALL NULLABLE TESTS PASSED\n"; return 0; }

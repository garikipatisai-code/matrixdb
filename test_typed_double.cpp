// CPU test for double (float64) columns (DM-3e): MatrixType::F64, matrix_cpu_reduce_all_f64 / _pred,
// matrix_pred_match_f64 (incl. NaN), load_scan_column_f64, execute_query scalar dispatch, durability.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <bit>
#include <cmath>
#include <limits>
#include <vector>
#include <iostream>

static bool refm(double v, MatrixCmp c, double a, double b) {
    switch (c) { case MatrixCmp::GT: return v>a; case MatrixCmp::GE: return v>=a; case MatrixCmp::LT: return v<a;
                 case MatrixCmp::LE: return v<=a; case MatrixCmp::EQ: return v==a; case MatrixCmp::NE: return v!=a;
                 case MatrixCmp::BETWEEN: return v>=a && v<=b; } return false; }
static double ref_reduce(const std::vector<double>& v, bool filt, MatrixCmp c, double a, double b, MatrixAggOp op) {
    double cnt=0, sum=0, mn=std::numeric_limits<double>::infinity(), mx=-std::numeric_limits<double>::infinity();
    for (double x : v) if (!filt || refm(x,c,a,b)) { cnt+=1; sum+=x; if (x<mn) mn=x; if (x>mx) mx=x; }
    switch (op) { case AGG_SUM: return sum; case AGG_MIN: return mn; case AGG_MAX: return mx; case AGG_COUNT: default: return cnt; } }

// Exactly-representable doubles + matching order -> == is exact.
static const std::vector<double> V = {1.5, -3.0, 0.5, 2.25, 5000000000.0, -0.25, 100.0, -100.0};

static void test_reduce_f64() {
    for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX})
        assert(matrix_cpu_reduce_all_f64(V.data(), V.size(), op) == ref_reduce(V, false, MatrixCmp::GT, 0, 0, op));
    const std::pair<MatrixCmp, std::pair<double,double>> cs[] = {
        {MatrixCmp::LT,{0.0,0}}, {MatrixCmp::GE,{0.5,0}}, {MatrixCmp::EQ,{1.5,0}}, {MatrixCmp::BETWEEN,{-3.0,2.25}} };
    for (auto& c : cs) for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX}) {
        MatrixPredicateF64 p{c.first, c.second.first, c.second.second};
        assert(matrix_cpu_reduce_pred_f64(V.data(), V.size(), p, op) == ref_reduce(V, true, c.first, c.second.first, c.second.second, op)); }
    // empty sentinels
    assert(matrix_cpu_reduce_all_f64(nullptr, 0, AGG_MIN) == std::numeric_limits<double>::infinity());
    assert(matrix_cpu_reduce_all_f64(nullptr, 0, AGG_MAX) == -std::numeric_limits<double>::infinity());
    std::cout << "[reduce f64] ok\n";
}

static void test_pred_match_f64() {
    assert(matrix_pred_match_f64(1.6, {MatrixCmp::GT, 1.5, 0}) && !matrix_pred_match_f64(1.5, {MatrixCmp::GT, 1.5, 0}));
    assert(matrix_pred_match_f64(-0.25, {MatrixCmp::BETWEEN, -3.0, 0.0}) && !matrix_pred_match_f64(0.5, {MatrixCmp::BETWEEN, -3.0, 0.0}));
    const double nan = std::numeric_limits<double>::quiet_NaN();
    assert(!matrix_pred_match_f64(nan, {MatrixCmp::GT, 0, 0}) && !matrix_pred_match_f64(nan, {MatrixCmp::LE, 0, 0})
           && !matrix_pred_match_f64(nan, {MatrixCmp::EQ, nan, 0}) && matrix_pred_match_f64(nan, {MatrixCmp::NE, 0, 0}));
    std::cout << "[pred match f64] ok\n";
}

static void test_engine_f64() {
    CPUMockEngine eng;
    eng.load_scan_column_f64(7, V.data(), V.size());
    assert(eng.column_type(7) == MatrixType::F64);
    for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX}) {
        MatrixQuery q{}; q.value_col = 7; q.agg = op; std::vector<uint64_t> o;
        assert(eng.execute_query(q, o) == MatrixQueryStatus::OK && o.size() == 1);
        assert(std::bit_cast<double>(o[0]) == ref_reduce(V, false, MatrixCmp::GT, 0, 0, op) && "f64 scalar == oracle"); }
    // filtered (value > 0)
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_SUM; q.has_filter = true; q.cmp = MatrixCmp::GT; q.lo_f64 = 0.0;
      std::vector<uint64_t> o; eng.execute_query(q, o);
      assert(std::bit_cast<double>(o[0]) == ref_reduce(V, true, MatrixCmp::GT, 0, 0, AGG_SUM)); }
    // fractional value survives (non-vacuity: a uint32/int64 path would truncate 1.5)
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_MIN; q.has_filter = true; q.cmp = MatrixCmp::GT; q.lo_f64 = 0.0;
      std::vector<uint64_t> o; eng.execute_query(q, o); assert(std::bit_cast<double>(o[0]) == 0.5); }
    // grouped double still rejected (DM-3f)
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_COUNT; q.grouped = true; q.key_col = 7; q.num_groups = 2;
      std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_UNSUPPORTED_TYPE); }
    std::cout << "[engine f64] ok\n";
}

static void test_f64_durability() {
    const std::string path = "/tmp/mdb_f64_catalog.bin"; std::remove(path.c_str());
    std::vector<uint32_t> u = {1, 2, 3};
    { CPUMockEngine eng; eng.load_scan_column(3, u.data(), u.size()); eng.load_scan_column_f64(7, V.data(), V.size()); eng.save_catalog(path); }
    { CPUMockEngine eng; eng.load_catalog(path);
      assert(eng.column_type(7) == MatrixType::F64 && eng.column_type(3) == MatrixType::U32 && "types restored");
      MatrixQuery q{}; q.value_col = 7; q.agg = AGG_SUM; std::vector<uint64_t> o; eng.execute_query(q, o);
      assert(std::bit_cast<double>(o[0]) == ref_reduce(V, false, MatrixCmp::GT, 0, 0, AGG_SUM) && "f64 column survived restart"); }
    std::remove(path.c_str());
    std::cout << "[f64 durability] ok\n";
}

int main() { test_reduce_f64(); test_pred_match_f64(); test_engine_f64(); test_f64_durability();
    std::cout << "ALL TYPED-DOUBLE TESTS PASSED\n"; return 0; }

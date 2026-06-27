// CPU test for int64 filtered aggregation (DM-3b): MatrixPredicateI64, matrix_pred_match_i64,
// matrix_cpu_reduce_pred_i64, execute_query int64 filtered dispatch, and int64 scans driving rebalance.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

// Independent signed predicate (explicit ops; does NOT call matrix_pred_match_i64).
static bool ref_match(int64_t v, MatrixCmp c, int64_t a, int64_t b) {
    switch (c) {
        case MatrixCmp::GT: return v > a;   case MatrixCmp::GE: return v >= a;
        case MatrixCmp::LT: return v < a;   case MatrixCmp::LE: return v <= a;
        case MatrixCmp::EQ: return v == a;  case MatrixCmp::NE: return v != a;
        case MatrixCmp::BETWEEN: return v >= a && v <= b;
    }
    return false;
}
static int64_t ref_reduce(const std::vector<int64_t>& v, MatrixCmp c, int64_t a, int64_t b, MatrixAggOp op) {
    int64_t cnt = 0, sum = 0, mn = INT64_MAX, mx = INT64_MIN;
    for (int64_t x : v) if (ref_match(x, c, a, b)) { ++cnt; sum += x; if (x < mn) mn = x; if (x > mx) mx = x; }
    switch (op) { case AGG_SUM: return sum; case AGG_MIN: return mn; case AGG_MAX: return mx;
                  case AGG_COUNT: default: return cnt; }
}

static void test_pred_match_i64() {
    assert(matrix_pred_match_i64(-4, {MatrixCmp::GT, -5, 0}) && !matrix_pred_match_i64(-5, {MatrixCmp::GT, -5, 0}));
    assert(matrix_pred_match_i64(-1, {MatrixCmp::LT, 0, 0}) && !matrix_pred_match_i64(0, {MatrixCmp::LT, 0, 0}));
    assert(matrix_pred_match_i64(-10, {MatrixCmp::BETWEEN, -10, 10}) && matrix_pred_match_i64(10, {MatrixCmp::BETWEEN, -10, 10}));
    assert(!matrix_pred_match_i64(-11, {MatrixCmp::BETWEEN, -10, 10}) && !matrix_pred_match_i64(11, {MatrixCmp::BETWEEN, -10, 10}));
    assert(matrix_pred_match_i64(5000000000LL, {MatrixCmp::GT, 4000000000LL, 0})
           && !matrix_pred_match_i64(3000000000LL, {MatrixCmp::GT, 4000000000LL, 0}));   // > UINT32_MAX bound
    assert(matrix_pred_match_i64(7, {MatrixCmp::GE, 7, 0}) && matrix_pred_match_i64(7, {MatrixCmp::LE, 7, 0})
           && matrix_pred_match_i64(7, {MatrixCmp::EQ, 7, 0}) && !matrix_pred_match_i64(7, {MatrixCmp::NE, 7, 0}));
    std::cout << "[pred match i64] ok\n";
}

static void test_reduce_pred_i64() {
    const std::vector<int64_t> v = {-7, 0, 5, 5000000000LL, -3, 2147483648LL, 100, -100, 5};
    const std::pair<MatrixCmp, std::pair<int64_t,int64_t>> cases[] = {
        {MatrixCmp::GT,{0,0}}, {MatrixCmp::LT,{0,0}}, {MatrixCmp::GE,{5,0}}, {MatrixCmp::LE,{-3,0}},
        {MatrixCmp::EQ,{5,0}}, {MatrixCmp::NE,{5,0}}, {MatrixCmp::BETWEEN,{-7,100}}, {MatrixCmp::GT,{4000000000LL,0}} };
    for (auto& cs : cases)
        for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX}) {
            MatrixPredicateI64 p{cs.first, cs.second.first, cs.second.second};
            assert(matrix_cpu_reduce_pred_i64(v.data(), v.size(), p, op)
                   == ref_reduce(v, cs.first, cs.second.first, cs.second.second, op));
        }
    std::cout << "[reduce pred i64] ok\n";
}

static void test_engine_i64_filtered() {
    const std::vector<int64_t> v = {-7, 0, 5, 5000000000LL, -3, 2147483648LL, 100, -100, 5};
    CPUMockEngine eng;
    eng.load_scan_column_i64(7, v.data(), v.size());
    struct Case { MatrixCmp c; int64_t a, b; MatrixAggOp op; };
    const Case cases[] = {
        {MatrixCmp::LT, 0, 0, AGG_COUNT}, {MatrixCmp::EQ, 5, 0, AGG_COUNT}, {MatrixCmp::GE, 100, 0, AGG_SUM},
        {MatrixCmp::BETWEEN, -7, 100, AGG_SUM}, {MatrixCmp::GT, 4000000000LL, 0, AGG_MAX} };
    for (auto& cs : cases) {
        MatrixQuery q{}; q.value_col = 7; q.agg = cs.op; q.has_filter = true;
        q.cmp = cs.c; q.lo_i64 = cs.a; q.hi_i64 = cs.b;
        std::vector<uint64_t> out;
        assert(eng.execute_query(q, out) == MatrixQueryStatus::OK && out.size() == 1);
        assert(static_cast<int64_t>(out[0]) == ref_reduce(v, cs.c, cs.a, cs.b, cs.op) && "int64 filtered matches oracle");
    }
    // Non-vacuity: a > UINT32_MAX bound is honored as int64 (only 5000000000 exceeds it -> MAX is it).
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_MAX; q.has_filter = true; q.cmp = MatrixCmp::GT; q.lo_i64 = 4000000000LL;
      std::vector<uint64_t> o; eng.execute_query(q, o); assert(static_cast<int64_t>(o[0]) == 5000000000LL); }
    // EQ differs from GT (predicate actually applied).
    { MatrixQuery eq{}; eq.value_col = 7; eq.agg = AGG_COUNT; eq.has_filter = true; eq.cmp = MatrixCmp::EQ; eq.lo_i64 = 5;
      MatrixQuery gt{}; gt.value_col = 7; gt.agg = AGG_COUNT; gt.has_filter = true; gt.cmp = MatrixCmp::GT; gt.lo_i64 = 5;
      std::vector<uint64_t> a, b; eng.execute_query(eq, a); eng.execute_query(gt, b); assert(a[0] != b[0]); }
    // Grouped int64 still rejected (DM-3c).
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_COUNT; q.grouped = true; q.key_col = 7; q.num_groups = 2;
      std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_UNSUPPORTED_TYPE); }
    std::cout << "[engine i64 filtered] ok\n";
}

static void test_i64_drives_rebalance() {
    std::vector<int64_t> v(1000, 1);
    CPUMockEngine eng;
    eng.load_scan_column_i64(7, v.data(), v.size());
    for (int i = 0; i < 5; ++i) {   // > REBALANCE_EVERY (4) int64 scalar queries
        MatrixQuery q{}; q.value_col = 7; q.agg = AGG_SUM; std::vector<uint64_t> o; eng.execute_query(q, o);
    }
    assert(eng.stats().rebalances >= 1 && "int64 scans drive the rebalance cadence (DM-3a follow-up)");
    std::cout << "[i64 drives rebalance] ok\n";
}

int main() {
    test_pred_match_i64();
    test_reduce_pred_i64();
    test_engine_i64_filtered();
    test_i64_drives_rebalance();
    std::cout << "ALL TYPED-PREDICATE TESTS PASSED\n";
    return 0;
}

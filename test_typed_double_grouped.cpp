// CPU test for grouped double aggregation (DM-3f): matrix_group_reduce_f64[_pred], grouped_aggregate_f64,
// execute_query double GROUP BY (u32 key, double value), incl. negative-group MAX + guards. Mirrors DM-3c.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <bit>
#include <limits>
#include <vector>
#include <iostream>

static bool refm(double v, MatrixCmp c, double a, double b) {
    switch (c) { case MatrixCmp::GT: return v>a; case MatrixCmp::GE: return v>=a; case MatrixCmp::LT: return v<a;
                 case MatrixCmp::LE: return v<=a; case MatrixCmp::EQ: return v==a; case MatrixCmp::NE: return v!=a;
                 case MatrixCmp::BETWEEN: return v>=a && v<=b; } return false; }
static void ref_group(const std::vector<uint32_t>& keys, const std::vector<double>& vals, uint32_t ng,
                      MatrixAggOp op, bool filt, MatrixCmp c, double a, double b, std::vector<double>& out) {
    const double inf = std::numeric_limits<double>::infinity();
    out.assign(ng, op == AGG_MIN ? inf : (op == AGG_MAX ? -inf : 0.0));
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] >= ng) continue;
        if (filt && !refm(vals[i], c, a, b)) continue;
        double& o = out[keys[i]];
        switch (op) { case AGG_SUM: o += vals[i]; break; case AGG_MIN: if (vals[i]<o) o=vals[i]; break;
                      case AGG_MAX: if (vals[i]>o) o=vals[i]; break; case AGG_COUNT: default: o += 1.0; break; } } }

// keys 0..2; group 1 all-negative (MAX-init guard); group 2 has a large + fractional value.
static const std::vector<uint32_t> K = {0, 1, 1, 2, 0, 1, 2};
static const std::vector<double>   V = {1.5, -3.0, -100.0, 5000000000.0, 0.5, -0.25, 2.25};

static void test_group_reduce_f64() {
    for (bool filt : {false, true})
        for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX}) {
            std::vector<double> got(3), exp;
            MatrixPredicateF64 p{MatrixCmp::GE, -3.0, 0};   // value >= -3.0
            if (filt) matrix_cpu_group_reduce_f64_pred(K.data(), V.data(), K.size(), 3, op, p, got.data());
            else      matrix_cpu_group_reduce_f64(K.data(), V.data(), K.size(), 3, op, got.data());
            ref_group(K, V, 3, op, filt, MatrixCmp::GE, -3.0, 0, exp);
            assert(got == exp && "grouped double reduce matches oracle"); }
    // MAX-init guard: group 1 (all negative {-3,-100,-0.25}) MAX must be -0.25, NOT 0.
    std::vector<double> mx(3); matrix_cpu_group_reduce_f64(K.data(), V.data(), K.size(), 3, AGG_MAX, mx.data());
    assert(mx[1] == -0.25 && "MAX of a negative double group is the max negative, not 0");
    std::cout << "[group reduce f64] ok\n";
}

static void test_engine_grouped_f64() {
    CPUMockEngine eng;
    eng.load_scan_column(1, K.data(), K.size());          // u32 key
    eng.load_scan_column_f64(7, V.data(), V.size());      // double value (same ROW count, 4N vs 8N bytes)
    for (bool filt : {false, true})
        for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX}) {
            MatrixQuery q{}; q.value_col = 7; q.key_col = 1; q.num_groups = 3; q.agg = op; q.grouped = true;
            q.has_filter = filt; q.cmp = MatrixCmp::GE; q.lo_f64 = -3.0;
            std::vector<uint64_t> out;
            assert(eng.execute_query(q, out) == MatrixQueryStatus::OK && out.size() == 3);
            std::vector<double> exp; ref_group(K, V, 3, op, filt, MatrixCmp::GE, -3.0, 0, exp);
            for (uint32_t g = 0; g < 3; ++g) assert(std::bit_cast<double>(out[g]) == exp[g] && "engine grouped double == oracle"); }
    // Non-vacuity: group 2 SUM includes the large + fractional values (5e9 + 2.25).
    { MatrixQuery q{}; q.value_col = 7; q.key_col = 1; q.num_groups = 3; q.agg = AGG_SUM; q.grouped = true;
      std::vector<uint64_t> o; eng.execute_query(q, o); assert(std::bit_cast<double>(o[2]) == 5000000000.0 + 2.25); }
    std::cout << "[engine grouped f64] ok\n";
}

static void test_grouped_f64_guards() {
    CPUMockEngine eng;
    eng.load_scan_column(1, K.data(), K.size());
    eng.load_scan_column_f64(7, V.data(), V.size());
    std::vector<double> dkeys = {0, 1, 0, 1, 0, 1, 0};
    eng.load_scan_column_f64(8, dkeys.data(), dkeys.size());     // double key
    { MatrixQuery q{}; q.value_col = 7; q.key_col = 8; q.num_groups = 2; q.agg = AGG_COUNT; q.grouped = true;
      std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_UNSUPPORTED_TYPE && "double key rejected"); }
    std::vector<uint32_t> k8 = {0, 1, 0, 1, 0, 1, 0, 1};         // 8 rows vs V's 7 -> mismatch
    eng.load_scan_column(2, k8.data(), k8.size());
    { MatrixQuery q{}; q.value_col = 7; q.key_col = 2; q.num_groups = 2; q.agg = AGG_COUNT; q.grouped = true;
      std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_INVALID_GROUP && "row-count mismatch rejected"); }
    std::cout << "[grouped f64 guards] ok\n";
}

int main() { test_group_reduce_f64(); test_engine_grouped_f64(); test_grouped_f64_guards();
    std::cout << "ALL TYPED-DOUBLE-GROUPED TESTS PASSED\n"; return 0; }

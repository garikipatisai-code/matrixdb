// CPU test for grouped int64 aggregation (DM-3c): matrix_group_reduce_i64[_pred], grouped_aggregate_i64,
// execute_query int64 GROUP BY (u32 key, int64 value), incl. mixed-width + int64-key-rejection guards.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

static bool refm(int64_t v, MatrixCmp c, int64_t a, int64_t b) {
    switch (c) { case MatrixCmp::GT: return v>a; case MatrixCmp::GE: return v>=a; case MatrixCmp::LT: return v<a;
                 case MatrixCmp::LE: return v<=a; case MatrixCmp::EQ: return v==a; case MatrixCmp::NE: return v!=a;
                 case MatrixCmp::BETWEEN: return v>=a && v<=b; } return false; }
// Brute grouped oracle: out[g] for the given op over rows matching (filtered ? pred : all).
static void ref_group(const std::vector<uint32_t>& keys, const std::vector<int64_t>& vals, uint32_t ng,
                      MatrixAggOp op, bool filt, MatrixCmp c, int64_t a, int64_t b, std::vector<int64_t>& out) {
    out.assign(ng, op == AGG_MIN ? INT64_MAX : (op == AGG_MAX ? INT64_MIN : 0));
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] >= ng) continue;
        if (filt && !refm(vals[i], c, a, b)) continue;
        int64_t& o = out[keys[i]];
        switch (op) { case AGG_SUM: o += vals[i]; break; case AGG_MIN: if (vals[i]<o) o=vals[i]; break;
                      case AGG_MAX: if (vals[i]>o) o=vals[i]; break; case AGG_COUNT: default: o += 1; break; } }
}

static void test_group_reduce_i64() {
    // keys 0..2; group 1 is ALL NEGATIVE (the MAX-init guard); group 2 has a > UINT32_MAX value.
    std::vector<uint32_t> keys = {0, 1, 1, 2, 0, 1, 2};
    std::vector<int64_t>  vals = {5, -3, -100, 5000000000LL, 7, -1, -2};
    for (bool filt : {false, true})
        for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX}) {
            std::vector<int64_t> got(3), exp;
            MatrixPredicateI64 p{MatrixCmp::GE, -3, 0};   // filt: value >= -3
            if (filt) matrix_cpu_group_reduce_i64_pred(keys.data(), vals.data(), keys.size(), 3, op, p, got.data());
            else      matrix_cpu_group_reduce_i64(keys.data(), vals.data(), keys.size(), 3, op, got.data());
            ref_group(keys, vals, 3, op, filt, MatrixCmp::GE, -3, 0, exp);
            assert(got == exp && "grouped int64 reduce matches oracle");
        }
    // Explicit MAX-init guard: group 1 (all negative) MAX must be -3, NOT 0.
    std::vector<int64_t> mx(3);
    matrix_cpu_group_reduce_i64(keys.data(), vals.data(), keys.size(), 3, AGG_MAX, mx.data());
    assert(mx[1] == -1 && "MAX of a negative group is the max negative (-1), not 0");   // group1 vals {-3,-100,-1}
    std::cout << "[group reduce i64] ok\n";
}

static void test_engine_grouped_i64() {
    std::vector<uint32_t> keys = {0, 1, 1, 2, 0, 1, 2};
    std::vector<int64_t>  vals = {5, -3, -100, 5000000000LL, 7, -1, -2};
    CPUMockEngine eng;
    eng.load_scan_column(1, keys.data(), keys.size());        // u32 key
    eng.load_scan_column_i64(7, vals.data(), vals.size());    // int64 value (equal ROW count, 4N vs 8N bytes)
    for (bool filt : {false, true})
        for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX}) {
            MatrixQuery q{}; q.value_col = 7; q.key_col = 1; q.num_groups = 3; q.agg = op; q.grouped = true;
            q.has_filter = filt; q.cmp = MatrixCmp::GE; q.lo_i64 = -3;
            std::vector<uint64_t> out;
            assert(eng.execute_query(q, out) == MatrixQueryStatus::OK && out.size() == 3);
            std::vector<int64_t> exp; ref_group(keys, vals, 3, op, filt, MatrixCmp::GE, -3, 0, exp);
            for (uint32_t g = 0; g < 3; ++g) assert(static_cast<int64_t>(out[g]) == exp[g] && "engine grouped int64 == oracle");
        }
    // Non-vacuity: group 2's SUM includes the > UINT32_MAX value (genuine int64).
    { MatrixQuery q{}; q.value_col = 7; q.key_col = 1; q.num_groups = 3; q.agg = AGG_SUM; q.grouped = true;
      std::vector<uint64_t> o; eng.execute_query(q, o); assert(static_cast<int64_t>(o[2]) == 5000000000LL - 2); }
    std::cout << "[engine grouped i64] ok\n";
}

static void test_grouped_i64_guards() {
    std::vector<uint32_t> keys = {0, 1, 0, 1};
    std::vector<int64_t>  vals = {1, 2, 3, 4};
    CPUMockEngine eng;
    eng.load_scan_column(1, keys.data(), keys.size());        // u32 key, 4 rows
    eng.load_scan_column_i64(7, vals.data(), vals.size());    // i64 value, 4 rows (equal ROW count)
    // int64 KEY rejected: i64 value + i64 key.
    std::vector<int64_t> keys64 = {0, 1, 0, 1};
    eng.load_scan_column_i64(8, keys64.data(), keys64.size());
    { MatrixQuery q{}; q.value_col = 7; q.key_col = 8; q.num_groups = 2; q.agg = AGG_COUNT; q.grouped = true;
      std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_UNSUPPORTED_TYPE && "int64 key rejected"); }
    // Row-count mismatch -> ERR_INVALID_GROUP (i64 value 4 rows, u32 key 5 rows).
    std::vector<uint32_t> keys5 = {0, 1, 0, 1, 0};
    eng.load_scan_column(2, keys5.data(), keys5.size());
    { MatrixQuery q{}; q.value_col = 7; q.key_col = 2; q.num_groups = 2; q.agg = AGG_COUNT; q.grouped = true;
      std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_INVALID_GROUP && "row-count mismatch rejected"); }
    std::cout << "[grouped i64 guards] ok\n";
}

int main() {
    test_group_reduce_i64();
    test_engine_grouped_i64();
    test_grouped_i64_guards();
    std::cout << "ALL TYPED-GROUPED TESTS PASSED\n";
    return 0;
}

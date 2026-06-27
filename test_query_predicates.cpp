// CPU test for richer scan predicates (QRY-3): MatrixCmp / MatrixPredicate, matrix_cpu_reduce_pred,
// and execute_query with GE/LT/LE/EQ/NE/BETWEEN. Oracles compute the comparison EXPLICITLY (not via
// matrix_pred_match) so a bug in the predicate dispatch is caught independently.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

// Independent predicate evaluation (explicit ops; does NOT call matrix_pred_match).
static bool ref_match(uint32_t v, MatrixCmp c, uint32_t a, uint32_t b) {
    switch (c) {
        case MatrixCmp::GT: return v > a;   case MatrixCmp::GE: return v >= a;
        case MatrixCmp::LT: return v < a;   case MatrixCmp::LE: return v <= a;
        case MatrixCmp::EQ: return v == a;  case MatrixCmp::NE: return v != a;
        case MatrixCmp::BETWEEN: return v >= a && v <= b;
    }
    return false;
}
static uint64_t ref_reduce(const std::vector<uint32_t>& v, MatrixCmp c, uint32_t a, uint32_t b, MatrixAggOp op) {
    uint64_t cnt = 0, sum = 0, mn = UINT64_MAX, mx = 0;
    for (uint32_t x : v) if (ref_match(x, c, a, b)) { ++cnt; sum += x; if (x < mn) mn = x; if (x > mx) mx = x; }
    switch (op) { case AGG_SUM: return sum; case AGG_MIN: return mn; case AGG_MAX: return mx;
                  case AGG_COUNT: default: return cnt; }
}

static void test_pred_match() {
    assert(matrix_pred_match(6, {MatrixCmp::GT, 5, 0}) && !matrix_pred_match(5, {MatrixCmp::GT, 5, 0}));
    assert(matrix_pred_match(5, {MatrixCmp::GE, 5, 0}) && !matrix_pred_match(4, {MatrixCmp::GE, 5, 0}));
    assert(matrix_pred_match(4, {MatrixCmp::LT, 5, 0}) && !matrix_pred_match(5, {MatrixCmp::LT, 5, 0}));
    assert(matrix_pred_match(5, {MatrixCmp::LE, 5, 0}) && !matrix_pred_match(6, {MatrixCmp::LE, 5, 0}));
    assert(matrix_pred_match(5, {MatrixCmp::EQ, 5, 0}) && !matrix_pred_match(6, {MatrixCmp::EQ, 5, 0}));
    assert(matrix_pred_match(6, {MatrixCmp::NE, 5, 0}) && !matrix_pred_match(5, {MatrixCmp::NE, 5, 0}));
    assert(matrix_pred_match(3, {MatrixCmp::BETWEEN, 3, 7}) && matrix_pred_match(7, {MatrixCmp::BETWEEN, 3, 7}));
    assert(!matrix_pred_match(2, {MatrixCmp::BETWEEN, 3, 7}) && !matrix_pred_match(8, {MatrixCmp::BETWEEN, 3, 7}));
    std::cout << "[pred match] ok\n";
}

static void test_reduce_pred() {
    const std::vector<uint32_t> v = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 5, 5};
    const std::pair<MatrixCmp, std::pair<uint32_t,uint32_t>> cases[] = {
        {MatrixCmp::GT,{5,0}}, {MatrixCmp::GE,{5,0}}, {MatrixCmp::LT,{5,0}}, {MatrixCmp::LE,{5,0}},
        {MatrixCmp::EQ,{5,0}}, {MatrixCmp::NE,{5,0}}, {MatrixCmp::BETWEEN,{3,7}}, {MatrixCmp::LT,{1,0}} };
    for (auto& cs : cases)
        for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX}) {
            MatrixPredicate p{cs.first, cs.second.first, cs.second.second};
            assert(matrix_cpu_reduce_pred(v.data(), v.size(), p, op)
                   == ref_reduce(v, cs.first, cs.second.first, cs.second.second, op));
        }
    // MAX-0 caveat: LT 1 matches only the value 0 -> MAX returns 0; COUNT distinguishes (==1, not empty).
    MatrixPredicate lt1{MatrixCmp::LT, 1, 0};
    assert(matrix_cpu_reduce_pred(v.data(), v.size(), lt1, AGG_MAX) == 0);
    assert(matrix_cpu_reduce_pred(v.data(), v.size(), lt1, AGG_COUNT) == 1);
    std::cout << "[reduce pred] ok\n";
}

static void test_execute_query_scalar() {
    std::vector<uint32_t> v(200);
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<uint32_t>(i % 50);   // 0..49 repeating
    CPUMockEngine eng;
    eng.load_scan_column(2, v.data(), v.size());
    struct Case { MatrixCmp c; uint32_t a, b; MatrixAggOp op; };
    const Case cases[] = {
        {MatrixCmp::LT, 10, 0, AGG_COUNT}, {MatrixCmp::EQ, 7, 0, AGG_COUNT}, {MatrixCmp::NE, 7, 0, AGG_SUM},
        {MatrixCmp::BETWEEN, 20, 30, AGG_SUM}, {MatrixCmp::GE, 45, 0, AGG_MIN}, {MatrixCmp::LE, 5, 0, AGG_MAX} };
    for (auto& cs : cases) {
        MatrixQuery q{}; q.value_col = 2; q.agg = cs.op; q.has_filter = true;
        q.cmp = cs.c; q.threshold = cs.a; q.upper = cs.b;
        std::vector<uint64_t> out;
        assert(eng.execute_query(q, out) == MatrixQueryStatus::OK && out.size() == 1);
        assert(out[0] == ref_reduce(v, cs.c, cs.a, cs.b, cs.op) && "scalar predicate query matches oracle");
    }
    // Backward-compat: default cmp (GT) == old value>threshold.
    MatrixQuery old_style{}; old_style.value_col = 2; old_style.agg = AGG_COUNT; old_style.has_filter = true; old_style.threshold = 25;
    std::vector<uint64_t> o1; eng.execute_query(old_style, o1);
    assert(o1[0] == ref_reduce(v, MatrixCmp::GT, 25, 0, AGG_COUNT) && "default cmp is GT");
    // Non-vacuity: EQ 25 differs from GT 25.
    MatrixQuery eq{}; eq.value_col = 2; eq.agg = AGG_COUNT; eq.has_filter = true; eq.cmp = MatrixCmp::EQ; eq.threshold = 25;
    std::vector<uint64_t> o2; eng.execute_query(eq, o2);
    assert(o2[0] != o1[0] && "EQ is actually applied, not treated as GT");
    std::cout << "[execute_query scalar] ok\n";
}

static void test_execute_query_grouped() {
    std::vector<uint32_t> keys(300), vals(300);
    for (size_t i = 0; i < 300; ++i) { keys[i] = static_cast<uint32_t>(i % 4); vals[i] = static_cast<uint32_t>(i % 60); }
    CPUMockEngine eng;
    eng.load_scan_column(1, keys.data(), keys.size());
    eng.load_scan_column(2, vals.data(), vals.size());
    MatrixQuery q{}; q.value_col = 2; q.key_col = 1; q.num_groups = 4; q.agg = AGG_SUM;
    q.grouped = true; q.has_filter = true; q.cmp = MatrixCmp::BETWEEN; q.threshold = 20; q.upper = 40;
    std::vector<uint64_t> out;
    assert(eng.execute_query(q, out) == MatrixQueryStatus::OK && out.size() == 4);
    uint64_t ref[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < 300; ++i) if (vals[i] >= 20 && vals[i] <= 40) ref[keys[i]] += vals[i];
    for (int g = 0; g < 4; ++g) assert(out[g] == ref[g] && "grouped BETWEEN matches oracle");
    std::cout << "[execute_query grouped] ok\n";
}

int main() {
    test_pred_match();
    test_reduce_pred();
    test_execute_query_scalar();
    test_execute_query_grouped();
    std::cout << "ALL PREDICATE TESTS PASSED\n";
    return 0;
}

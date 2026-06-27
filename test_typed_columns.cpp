// CPU test for int64 typed columns (DM-3a): MatrixType, matrix_cpu_reduce_all_i64,
// load_scan_column_i64, and execute_query int64 scalar dispatch (incl. graceful ERR_UNSUPPORTED_TYPE).
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

// Hand oracle over the int64 data (independent of the SUT reducer).
static int64_t oracle(const std::vector<int64_t>& v, MatrixAggOp op) {
    int64_t cnt = static_cast<int64_t>(v.size()), sum = 0, mn = INT64_MAX, mx = INT64_MIN;
    for (int64_t x : v) { sum += x; if (x < mn) mn = x; if (x > mx) mx = x; }
    switch (op) { case AGG_SUM: return sum; case AGG_MIN: return mn; case AGG_MAX: return mx;
                  case AGG_COUNT: default: return cnt; }
}

static void test_reduce_i64() {
    const std::vector<int64_t> v = {-7, 0, 5, 5000000000LL, -3, 2147483648LL};   // negatives + > UINT32_MAX
    for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX})
        assert(matrix_cpu_reduce_all_i64(v.data(), v.size(), op) == oracle(v, op));
    // Empty-set sentinels.
    assert(matrix_cpu_reduce_all_i64(nullptr, 0, AGG_COUNT) == 0);
    assert(matrix_cpu_reduce_all_i64(nullptr, 0, AGG_SUM) == 0);
    assert(matrix_cpu_reduce_all_i64(nullptr, 0, AGG_MIN) == INT64_MAX);
    assert(matrix_cpu_reduce_all_i64(nullptr, 0, AGG_MAX) == INT64_MIN);
    std::cout << "[reduce i64] ok\n";
}

static void test_engine_i64() {
    const std::vector<int64_t> v = {-7, 0, 5, 5000000000LL, -3, 2147483648LL, 100, -100};
    CPUMockEngine eng;
    eng.load_scan_column_i64(7, v.data(), v.size());
    assert(eng.column_type(7) == MatrixType::I64);
    for (MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX}) {
        MatrixQuery q{}; q.value_col = 7; q.agg = op;
        std::vector<uint64_t> out;
        assert(eng.execute_query(q, out) == MatrixQueryStatus::OK && out.size() == 1);
        assert(static_cast<int64_t>(out[0]) == oracle(v, op) && "int64 scalar aggregate matches oracle");
    }
    // The > UINT32_MAX value proves genuine 64-bit: a uint32 read of 5000000000 would be 705032704.
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_MAX; std::vector<uint64_t> o; eng.execute_query(q, o);
      assert(static_cast<int64_t>(o[0]) == 5000000000LL && static_cast<int64_t>(o[0]) != 705032704); }
    // Graceful: filtered / grouped int64 are not yet supported.
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_SUM; q.has_filter = true; q.threshold = 0;
      std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_UNSUPPORTED_TYPE && o.empty()); }
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_COUNT; q.grouped = true; q.key_col = 7; q.num_groups = 2;
      std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_UNSUPPORTED_TYPE); }
    std::cout << "[engine i64] ok\n";
}

static void test_u32_untouched() {
    std::vector<uint32_t> v(100);
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<uint32_t>(i);
    CPUMockEngine eng;
    eng.load_scan_column(3, v.data(), v.size());
    assert(eng.column_type(3) == MatrixType::U32 && "untagged column defaults to U32");
    MatrixQuery q{}; q.value_col = 3; q.agg = AGG_SUM; std::vector<uint64_t> out;
    assert(eng.execute_query(q, out) == MatrixQueryStatus::OK && out[0] == 4950);   // 0+...+99
    MatrixQuery qf{}; qf.value_col = 3; qf.agg = AGG_COUNT; qf.has_filter = true; qf.threshold = 50;
    std::vector<uint64_t> of; assert(eng.execute_query(qf, of) == MatrixQueryStatus::OK && of[0] == 49); // 51..99
    std::cout << "[u32 untouched] ok\n";
}

int main() {
    test_reduce_i64();
    test_engine_i64();
    test_u32_untouched();
    std::cout << "ALL TYPED-COLUMN TESTS PASSED\n";
    return 0;
}

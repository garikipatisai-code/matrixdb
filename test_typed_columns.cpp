// CPU test for int64 typed columns (DM-3a): MatrixType, matrix_cpu_reduce_all_i64,
// load_scan_column_i64, and execute_query int64 scalar dispatch (incl. graceful ERR_UNSUPPORTED_TYPE).
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

// Hand oracle over the int64 data (independent of the SUT reducer). [[maybe_unused]]: unreferenced under -DNDEBUG.
[[maybe_unused]] static int64_t oracle(const std::vector<int64_t>& v, MatrixAggOp op) {
    int64_t cnt = static_cast<int64_t>(v.size()), sum = 0, mn = INT64_MAX, mx = INT64_MIN;
    for (int64_t x : v) { sum += x; if (x < mn) mn = x; if (x > mx) mx = x; }
    switch (op) { case AGG_SUM: return sum; case AGG_MIN: return mn; case AGG_MAX: return mx;
                  case AGG_COUNT: default: return cnt; }
}

static void test_reduce_i64() {
    const std::vector<int64_t> v = {-7, 0, 5, 5000000000LL, -3, 2147483648LL};   // negatives + > UINT32_MAX
    for ([[maybe_unused]] MatrixAggOp op : {AGG_COUNT, AGG_SUM, AGG_MIN, AGG_MAX})
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
    // DM-3b: filtered int64 now works (default cmp GT, lo_i64 0 -> value > 0); grouped int64 stays unsupported (DM-3c).
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_SUM; q.has_filter = true; q.lo_i64 = 0;   // WHERE value > 0
      std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::OK
        && static_cast<int64_t>(o[0]) == 5000000000LL + 2147483648LL + 5 + 100); }   // 5,5e9,2147483648,100
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_COUNT; q.grouped = true; q.key_col = 7; q.num_groups = 2;
      std::vector<uint64_t> o; assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_UNSUPPORTED_TYPE); }
    std::cout << "[engine i64] ok\n";
}

// Regression for the typed-GROUP-BY-key corruption path: an int64 key (N rows = 8N bytes) has the SAME
// byte length as a uint32 value of 2N rows, so it passes execute_query's length guard — but reading the
// int64 key bytes as uint32 would invent phantom groups. Must be rejected, not silently mis-grouped.
static void test_typed_key_rejected() {
    std::vector<int64_t> keys = {0, 1, 2, 3};                     // 4 rows  -> 32 bytes
    std::vector<uint32_t> vals = {5, 6, 7, 8, 9, 10, 11, 12};     // 8 rows  -> 32 bytes (equal length)
    CPUMockEngine eng;
    eng.load_scan_column_i64(8, keys.data(), keys.size());
    eng.load_scan_column(9, vals.data(), vals.size());
    MatrixQuery q{}; q.value_col = 9; q.key_col = 8; q.num_groups = 4; q.agg = AGG_COUNT; q.grouped = true;
    std::vector<uint64_t> out;
    assert(eng.execute_query(q, out) == MatrixQueryStatus::ERR_UNSUPPORTED_TYPE
           && out.empty() && "int64 GROUP BY key rejected, not reinterpreted as uint32");
    std::cout << "[typed key rejected] ok\n";
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
    test_typed_key_rejected();
    test_u32_untouched();
    std::cout << "ALL TYPED-COLUMN TESTS PASSED\n";
    return 0;
}

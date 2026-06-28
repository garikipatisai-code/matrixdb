// CPU test for append / dynamic column growth (DM-9): append_to_column[_i64/_f64] grow a column;
// rows become queryable; works across the COLD tier (borrow-append-return).
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <bit>
#include <vector>
#include <iostream>

static void test_append_u32() {
    std::vector<uint32_t> v(10); for (uint32_t i = 0; i < 10; ++i) v[i] = i;   // 0..9, sum 45
    CPUMockEngine eng;
    eng.load_scan_column(3, v.data(), v.size());
    MatrixQuery q{}; q.value_col = 3; q.agg = AGG_SUM; std::vector<uint64_t> o;
    eng.execute_query(q, o); assert(o[0] == 45);
    uint32_t more[] = {10, 11, 12};
    eng.append_to_column(3, more, 3);                       // 0..12, sum 78, 13 rows
    assert(eng.column_rows(3) == 13);
    eng.execute_query(q, o); assert(o[0] == 78 && "appended rows included in SUM");
    std::cout << "[append u32] ok\n";
}

static void test_append_typed() {
    CPUMockEngine eng;
    std::vector<int64_t> s = {-7, 5};               eng.load_scan_column_i64(7, s.data(), s.size());
    std::vector<double>  d = {1.5, -0.5};           eng.load_scan_column_f64(9, d.data(), d.size());
    int64_t mi[] = {5000000000LL, -100};            eng.append_to_column_i64(7, mi, 2);   // sum -7+5+5e9-100
    double  md[] = {2.25, -3.0};                    eng.append_to_column_f64(9, md, 2);   // sum 1.5-0.5+2.25-3.0
    assert(eng.column_rows(7) == 4 && eng.column_rows(9) == 4);
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_SUM; std::vector<uint64_t> o; eng.execute_query(q, o);
      assert(static_cast<int64_t>(o[0]) == -7 + 5 + 5000000000LL - 100); }
    { MatrixQuery q{}; q.value_col = 9; q.agg = AGG_SUM; std::vector<uint64_t> o; eng.execute_query(q, o);
      assert(std::bit_cast<double>(o[0]) == 1.5 - 0.5 + 2.25 - 3.0); }
    std::cout << "[append typed] ok\n";
}

static void test_append_cold() {
    // Small host budget: holds ~one 400KB column, not two -> the idle one demotes to COLD.
    CPUMockEngine eng(0, "", 600 * 1024);
    std::vector<uint32_t> a(100000, 1), b(100000, 1);       // 400KB each
    eng.load_scan_column(1, a.data(), a.size());
    eng.load_scan_column(2, b.data(), b.size());
    for (int i = 0; i < 8; ++i) {                           // scan col1 hard -> rebalance demotes idle col2
        MatrixQuery q{}; q.value_col = 1; q.agg = AGG_COUNT; std::vector<uint64_t> o; eng.execute_query(q, o);
    }
    assert(eng.column_tier(2) == MemorySpace::COLD && "idle column demoted to SSD");
    uint32_t more[] = {7, 7, 7};
    eng.append_to_column(2, more, 3);                       // append to the COLD column
    MatrixQuery q{}; q.value_col = 2; q.agg = AGG_COUNT; std::vector<uint64_t> o;
    assert(eng.execute_query(q, o) == MatrixQueryStatus::OK && o[0] == 100003 && "COLD column grew + queryable");
    std::cout << "[append cold] ok\n";
}

int main() { test_append_u32(); test_append_typed(); test_append_cold();
    std::cout << "ALL APPEND TESTS PASSED\n"; return 0; }

// CPU test for named columns + catalog introspection (DM-2): name_column / column_id / column_name /
// catalog_columns over u32 + int64 + double columns.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <algorithm>
#include <string>
#include <vector>
#include <iostream>

static void test_naming() {
    std::vector<uint32_t> u = {1, 2, 3};
    std::vector<int64_t>  s = {-7, 5000000000LL, 3, -100};
    std::vector<double>   d = {1.5, -3.25};
    CPUMockEngine eng;
    eng.load_scan_column(3, u.data(), u.size());
    eng.load_scan_column_i64(7, s.data(), s.size());
    eng.load_scan_column_f64(9, d.data(), d.size());
    eng.name_column(3, "qty"); eng.name_column(7, "revenue"); eng.name_column(9, "rate");
    assert(eng.column_id("qty") == 3 && eng.column_id("revenue") == 7 && eng.column_id("rate") == 9);
    assert(eng.column_id("nonexistent") == 0 && "unknown name -> 0");
    assert(eng.column_name(7) == "revenue");
    // load a 4th unnamed column; its name is ""
    std::vector<uint32_t> u2 = {9, 9};
    eng.load_scan_column(4, u2.data(), u2.size());
    assert(eng.column_name(4).empty() && "unnamed column -> empty name");
    std::cout << "[naming] ok\n";
}

static void test_catalog_columns() {
    std::vector<uint32_t> u = {1, 2, 3, 4, 5};       // 5 rows
    std::vector<int64_t>  s = {-7, 5000000000LL, 3}; // 3 rows
    std::vector<double>   d = {1.5, -3.25};          // 2 rows
    CPUMockEngine eng;
    eng.load_scan_column(3, u.data(), u.size());     eng.name_column(3, "qty");
    eng.load_scan_column_i64(7, s.data(), s.size()); eng.name_column(7, "revenue");
    eng.load_scan_column_f64(9, d.data(), d.size()); // unnamed
    std::vector<ColumnInfo> info = eng.catalog_columns();
    assert(info.size() == 3);
    std::sort(info.begin(), info.end(), [](const ColumnInfo& a, const ColumnInfo& b){ return a.id < b.id; });
    assert(info[0].id == 3 && info[0].name == "qty"     && info[0].type == MatrixType::U32 && info[0].rows == 5);
    assert(info[1].id == 7 && info[1].name == "revenue" && info[1].type == MatrixType::I64 && info[1].rows == 3);
    assert(info[2].id == 9 && info[2].name == ""        && info[2].type == MatrixType::F64 && info[2].rows == 2);
    for (const auto& ci : info) assert(ci.tier == MemorySpace::HOST && "freshly loaded -> HOST");
    std::cout << "[catalog columns] ok\n";
}

static void test_resolve_then_query() {
    std::vector<int64_t> s = {-7, 5000000000LL, 3, -100};
    CPUMockEngine eng;
    eng.load_scan_column_i64(7, s.data(), s.size());
    eng.name_column(7, "revenue");
    MatrixQuery q{}; q.value_col = eng.column_id("revenue"); q.agg = AGG_SUM;   // resolve name -> id
    std::vector<uint64_t> o;
    assert(eng.execute_query(q, o) == MatrixQueryStatus::OK);
    assert(static_cast<int64_t>(o[0]) == -7 + 5000000000LL + 3 - 100 && "query by resolved name");
    std::cout << "[resolve then query] ok\n";
}

int main() { test_naming(); test_catalog_columns(); test_resolve_then_query();
    std::cout << "ALL SCHEMA TESTS PASSED\n"; return 0; }

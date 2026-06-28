// CPU test for named tables (DM-2c): create_table groups equal-length columns into a named, validated,
// introspectable unit; table_columns / tables list them; row-mismatch + unknown-column are rejected.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <algorithm>
#include <string>
#include <vector>
#include <iostream>

static void test_tables() {
    std::vector<uint32_t> region = {0, 1, 0, 2}, amount = {10, 20, 30, 40};   // 4 rows
    std::vector<int64_t>  note   = {-1, -2, -3, -4};                          // 4 rows (int64)
    std::vector<uint32_t> small  = {1, 2};                                    // 2 rows (mismatch)
    CPUMockEngine eng;
    eng.load_scan_column(2, region.data(), region.size()); eng.name_column(2, "region");
    eng.load_scan_column(3, amount.data(), amount.size()); eng.name_column(3, "amount");
    eng.load_scan_column_i64(7, note.data(), note.size());  eng.name_column(7, "note");
    eng.load_scan_column(5, small.data(), small.size());

    // valid table: 3 equal-length columns
    assert(eng.create_table("sales", {2, 3, 7}) && "equal-length columns -> table created");
    auto cols = eng.table_columns("sales");
    assert(cols.size() == 3);
    assert(cols[0].name == "region" && cols[0].type == MatrixType::U32 && cols[0].rows == 4);
    assert(cols[1].name == "amount" && cols[1].type == MatrixType::U32);
    assert(cols[2].name == "note"   && cols[2].type == MatrixType::I64 && "table preserves declared order + mixed types");
    assert(eng.tables() == std::vector<std::string>{"sales"});

    // rejections (no table created, graceful false)
    assert(!eng.create_table("bad_unknown", {2, 99}) && "unknown column id rejected");
    assert(!eng.create_table("bad_rows", {2, 5}) && "row-count mismatch rejected (4 vs 2)");
    assert(!eng.create_table("bad_empty", {}) && "empty column list rejected");
    assert(eng.table_columns("nosuchtable").empty() && "unknown table -> empty");
    assert(eng.tables().size() == 1 && "rejected tables were not registered");
    std::cout << "[named tables] ok\n";
}

int main() { test_tables(); std::cout << "ALL TABLE TESTS PASSED\n"; return 0; }

// CPU test for minimal string columns (DM-3i): load_string_column + row count + equality-filter count
// + element access. A self-contained string store (separate from the fixed-width byte catalog).
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include <iostream>

static void test_string_columns() {
    CPUMockEngine eng;
    std::vector<std::string> category = {"books", "toys", "books", "food", "books"};
    eng.load_string_column(5, category);
    assert(eng.string_column_rows(5) == 5);
    // equality-filter COUNT (the string WHERE col = 'literal' count)
    assert(eng.string_count_where_eq(5, "books") == 3 && "3 rows == 'books'");
    assert(eng.string_count_where_eq(5, "toys")  == 1);
    assert(eng.string_count_where_eq(5, "none")  == 0 && "no match -> 0");
    // element access
    assert(eng.string_column_at(5, 0) == "books" && eng.string_column_at(5, 1) == "toys");
    // unknown id -> empty / 0 (graceful)
    assert(eng.string_column_rows(99) == 0 && eng.string_count_where_eq(99, "x") == 0 && "unknown string column -> 0");
    // non-vacuity: the equality count actually discriminates by value
    assert(eng.string_count_where_eq(5, "books") != eng.string_count_where_eq(5, "toys"));
    std::cout << "[string columns] ok\n";
}

int main() { test_string_columns(); std::cout << "ALL STRING-COLUMN TESTS PASSED\n"; return 0; }

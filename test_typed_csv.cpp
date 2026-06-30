// CPU test for typed CSV ingest (DM-3g): matrix_read_csv_column_i64/_f64 + load_column_from_csv_i64/_f64.
#include "compute_mock.cpp"
#include "csv_ingest.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <bit>
#include <fstream>
#include <string>
#include <vector>
#include <iostream>

static std::string wr(const std::string& name, const std::string& body) {
    const std::string p = "/tmp/" + name; std::ofstream(p) << body; return p; }

static void test_csv_i64() {
    std::vector<int64_t> out;
    std::string p = wr("mdb_csv_i64.csv", "k,v\n-7,-7\n10,5000000000\n3,3\n");   // negatives + > UINT32_MAX
    assert(matrix_read_csv_column_i64(p, 1, true, ',', out) && out == (std::vector<int64_t>{-7, 5000000000LL, 3}));
    // graceful failures
    assert(!matrix_read_csv_column_i64("/tmp/mdb_csv_nope.csv", 0, false, ',', out));
    p = wr("mdb_csv_i64_bad.csv", "1,x\n");      assert(!matrix_read_csv_column_i64(p, 1, false, ',', out));
    p = wr("mdb_csv_i64_over.csv", "99999999999999999999\n"); assert(!matrix_read_csv_column_i64(p, 0, false, ',', out));
    p = wr("mdb_csv_i64_junk.csv", "12x\n");     assert(!matrix_read_csv_column_i64(p, 0, false, ',', out));
    p = wr("mdb_csv_i64_short.csv", "1,2\n3\n"); assert(!matrix_read_csv_column_i64(p, 1, false, ',', out) && out.empty());
    std::cout << "[csv i64] ok\n";
}

static void test_csv_f64() {
    std::vector<double> out;
    std::string p = wr("mdb_csv_f64.csv", "1.5\n-3.25\n5e9\n0.5\n");   // fractional + negative + exponent
    assert(matrix_read_csv_column_f64(p, 0, false, ',', out) && out == (std::vector<double>{1.5, -3.25, 5e9, 0.5}));
    assert(!matrix_read_csv_column_f64("/tmp/mdb_csv_nope2.csv", 0, false, ',', out));
    p = wr("mdb_csv_f64_bad.csv", "x\n");     assert(!matrix_read_csv_column_f64(p, 0, false, ',', out));
    p = wr("mdb_csv_f64_junk.csv", "1.5x\n"); assert(!matrix_read_csv_column_f64(p, 0, false, ',', out));
    p = wr("mdb_csv_f64_empty.csv", "1,\n");  assert(!matrix_read_csv_column_f64(p, 1, false, ',', out));  // empty field
    std::cout << "[csv f64] ok\n";
}

static void test_engine_typed_csv() {
    CPUMockEngine eng;
    std::string pi = wr("mdb_eng_i64.csv", "k,v\n-7,-7\n10,5000000000\n3,3\n");
    assert(eng.load_column_from_csv_i64(7, pi, 1, true));
    assert(eng.column_type(7) == MatrixType::I64);
    { MatrixQuery q{}; q.value_col = 7; q.agg = AGG_SUM; std::vector<uint64_t> o;
      assert(eng.execute_query(q, o) == MatrixQueryStatus::OK && static_cast<int64_t>(o[0]) == -7 + 5000000000LL + 3); }
    std::string pf = wr("mdb_eng_f64.csv", "1.5\n-3.25\n0.25\n");
    assert(eng.load_column_from_csv_f64(8, pf, 0));
    assert(eng.column_type(8) == MatrixType::F64);
    { MatrixQuery q{}; q.value_col = 8; q.agg = AGG_SUM; std::vector<uint64_t> o;
      eng.execute_query(q, o); assert(std::bit_cast<double>(o[0]) == 1.5 - 3.25 + 0.25); }
    // malformed -> false, no catalog entry
    std::string pb = wr("mdb_eng_bad.csv", "1,x\n");
    assert(!eng.load_column_from_csv_i64(9, pb, 1));
    { MatrixQuery q{}; q.value_col = 9; q.agg = AGG_COUNT; std::vector<uint64_t> o;
      assert(eng.execute_query(q, o) == MatrixQueryStatus::ERR_UNKNOWN_COLUMN && "no entry from bad CSV"); }
    std::cout << "[engine typed csv] ok\n";
}

// String CSV ingest: load_string_column_from_csv -> dictionary-encoded, queryable column.
static void test_engine_str_csv() {
    CPUMockEngine eng;
    std::string ps = wr("mdb_eng_str.csv", "books\ngames\nbooks\nmusic\ngames\nbooks\n");  // 3 distinct / 6 rows
    assert(eng.load_string_column_from_csv(7, ps, 0));
    assert(eng.string_dict_size(7) == 3 && eng.count_distinct(7) == 3);
    assert(eng.column_rows(7) == 6);
    assert(eng.string_decode(7, 0) == "books" && eng.string_decode(7, 2) == "music");  // sorted => code order lexicographic
    std::string pb = wr("mdb_eng_str_bad.csv", "a,b\nx\n");   // want col 1 but data row has no col 1 -> short row
    assert(!eng.load_string_column_from_csv(8, pb, 1, true));
    std::cout << "[engine str csv] ok\n";
}

int main() { test_csv_i64(); test_csv_f64(); test_engine_typed_csv(); test_engine_str_csv();
    std::cout << "ALL TYPED-CSV TESTS PASSED\n"; return 0; }

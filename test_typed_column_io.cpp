// CPU test for typed single-column binary I/O (DM-3h): save_column / load_column_from_file round-trip
// int64 and double columns (not just uint32) via the typed file format; matrix_write/read_column_typed.
#include "compute_mock.cpp"
#include "column_io.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <bit>
#include <vector>
#include <string>
#include <iostream>

static void test_direct_typed_io() {
    const std::string p = "/tmp/mdb_typed_col.bin"; std::remove(p.c_str());
    std::vector<int64_t> v = {-7, 5000000000LL, 3};
    matrix_write_column_typed(p, v.data(), v.size() * sizeof(int64_t), static_cast<uint32_t>(MatrixType::I64));
    std::vector<unsigned char> bytes; uint32_t type = 99;
    matrix_read_column_typed(p, bytes, type);
    assert(type == static_cast<uint32_t>(MatrixType::I64) && bytes.size() == v.size() * sizeof(int64_t));
    assert(std::memcmp(bytes.data(), v.data(), bytes.size()) == 0 && "typed bytes round-trip");
    std::remove(p.c_str());
    std::cout << "[direct typed io] ok\n";
}

static void test_engine_save_load_typed() {
    const std::string pi = "/tmp/mdb_sc_i64.bin", pf = "/tmp/mdb_sc_f64.bin", pu = "/tmp/mdb_sc_u32.bin";
    for (auto* p : {&pi, &pf, &pu}) std::remove(p->c_str());
    std::vector<int64_t>  s = {-100, 5000000000LL, 7, -3};
    std::vector<double>   d = {1.5, -3.25, 2.0};
    std::vector<uint32_t> u = {10, 20, 30, 40, 50};
    {
        CPUMockEngine eng;
        eng.load_scan_column_i64(7, s.data(), s.size());
        eng.load_scan_column_f64(8, d.data(), d.size());
        eng.load_scan_column(9, u.data(), u.size());
        eng.save_column(7, pi); eng.save_column(8, pf); eng.save_column(9, pu);   // int64 no longer aborts
    }
    {
        CPUMockEngine eng;
        eng.load_column_from_file(7, pi); eng.load_column_from_file(8, pf); eng.load_column_from_file(9, pu);
        assert(eng.column_type(7) == MatrixType::I64 && eng.column_type(8) == MatrixType::F64 && eng.column_type(9) == MatrixType::U32);
        // values via query
        MatrixQuery qi{}; qi.value_col = 7; qi.agg = AGG_SUM; std::vector<uint64_t> oi; eng.execute_query(qi, oi);
        assert(static_cast<int64_t>(oi[0]) == -100 + 5000000000LL + 7 - 3 && "int64 column survived single-file round-trip");
        MatrixQuery qf{}; qf.value_col = 8; qf.agg = AGG_SUM; std::vector<uint64_t> of; eng.execute_query(qf, of);
        assert(std::bit_cast<double>(of[0]) == 1.5 - 3.25 + 2.0 && "double column survived");
        MatrixQuery qu{}; qu.value_col = 9; qu.agg = AGG_SUM; std::vector<uint64_t> ou; eng.execute_query(qu, ou);
        assert(ou[0] == 150 && "u32 column survived (typed format, type 0)");
    }
    for (auto* p : {&pi, &pf, &pu}) std::remove(p->c_str());
    std::cout << "[engine save/load typed] ok\n";
}

int main() { test_direct_typed_io(); test_engine_save_load_typed();
    std::cout << "ALL TYPED-COLUMN-IO TESTS PASSED\n"; return 0; }

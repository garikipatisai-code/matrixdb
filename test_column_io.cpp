// CPU test for binary column persistence (column_io.hpp + engine save/load).
#include "compute_mock.cpp"
#include "column_io.hpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <cstdio>
#include <iostream>

static void test_direct_roundtrip() {
    std::vector<uint32_t> in(500);
    for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<uint32_t>(i * 3 + 1);
    const std::string path = "/tmp/matrixdb_coltest_500.bin";
    matrix_write_column(path, in.data(), in.size());
    std::vector<uint32_t> out;
    matrix_read_column(path, out);
    assert(out == in && "round-trip preserves the column");
    FILE* f = std::fopen(path.c_str(), "rb"); assert(f);
    uint32_t magic = 0; assert(std::fread(&magic, sizeof magic, 1, f) == 1); std::fclose(f);
    assert(magic == MATRIX_COLUMN_MAGIC && "file carries the column magic");
    std::remove(path.c_str());
    std::vector<uint32_t> empty, eout;
    matrix_write_column("/tmp/matrixdb_coltest_0.bin", empty.data(), 0);
    matrix_read_column("/tmp/matrixdb_coltest_0.bin", eout);
    assert(eout.empty());
    std::remove("/tmp/matrixdb_coltest_0.bin");
    std::cout << "[column_io direct round-trip] ok\n";
}

static uint64_t sum_to(uint64_t hi) { uint64_t s = 0; for (uint64_t i = 0; i <= hi; ++i) s += i; return s; }

static void test_engine_save_load_query() {
    const size_t N = 1000;
    std::vector<uint32_t> col(N);
    for (size_t i = 0; i < N; ++i) col[i] = static_cast<uint32_t>(i);
    const std::string path = "/tmp/matrixdb_coltest_eng.bin";
    CPUMockEngine a(0, "", SIZE_MAX);
    a.load_scan_column(7, col.data(), N);
    a.save_column(7, path);
    CPUMockEngine b(0, "", SIZE_MAX);
    b.load_column_from_file(7, path);
    std::vector<uint64_t> out;
    b.execute_query(MatrixQuery{.value_col = 7, .agg = AGG_SUM}, out);
    assert(out.size() == 1 && out[0] == sum_to(N - 1) && "persisted column reloads + queries identically");
    std::remove(path.c_str());
    std::cout << "[engine save->load->query] ok\n";
}

static void test_engine_save_cold_column() {
    const size_t N = 1000;
    const size_t S = N * sizeof(uint32_t);
    std::vector<uint32_t> col(N), dummy(N, 0);
    for (size_t i = 0; i < N; ++i) col[i] = static_cast<uint32_t>(i);
    const std::string path = "/tmp/matrixdb_coltest_cold.bin";
    CPUMockEngine a(0, "", /*host_cap=*/S);   // one-column budget
    a.load_scan_column(7, col.data(), N);
    a.load_scan_column(8, dummy.data(), N);
    for (int r = 0; r < 12; ++r) { DatabaseQuery q{}; matrix_set_scan_target(q, 0u, 8); a.execute_scan(q); }
    assert(a.column_tier(7) == MemorySpace::COLD && "col 7 demoted to SSD");
    a.save_column(7, path);
    assert(a.column_tier(7) == MemorySpace::COLD && "save returned the borrow");
    CPUMockEngine b(0, "", SIZE_MAX);
    b.load_column_from_file(7, path);
    std::vector<uint64_t> out;
    b.execute_query(MatrixQuery{.value_col = 7, .agg = AGG_SUM}, out);
    assert(out.size() == 1 && out[0] == sum_to(N - 1) && "COLD column persisted + reloaded correctly");
    std::remove(path.c_str());
    std::cout << "[engine save COLD column] ok\n";
}

int main() {
    test_direct_roundtrip();
    test_engine_save_load_query();
    test_engine_save_cold_column();
    std::cout << "ALL COLUMN-IO TESTS PASSED\n";
    return 0;
}

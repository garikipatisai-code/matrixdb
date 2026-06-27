// CPU test for typed catalog snapshot (DM-3d): save_catalog/load_catalog round-trip a mixed
// u32 + int64 catalog (incl. negatives and > UINT32_MAX), with types and values preserved.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <iostream>

static int64_t i64_sum(const std::vector<int64_t>& v) { int64_t s = 0; for (int64_t x : v) s += x; return s; }
static uint64_t u32_sum(const std::vector<uint32_t>& v) { uint64_t s = 0; for (uint32_t x : v) s += x; return s; }

static void test_mixed_roundtrip() {
    const std::string path = "/tmp/mdb_typed_catalog.bin"; std::remove(path.c_str());
    std::vector<uint32_t> u(100); for (size_t i = 0; i < u.size(); ++i) u[i] = static_cast<uint32_t>(i);
    std::vector<int64_t>  s = {-7, 0, 5, 5000000000LL, -3, 2147483648LL, 100, -100};
    {
        CPUMockEngine eng;
        eng.load_scan_column(3, u.data(), u.size());          // u32
        eng.load_scan_column_i64(7, s.data(), s.size());      // int64 (negatives + > UINT32_MAX)
        eng.save_catalog(path);
    }
    {
        CPUMockEngine eng;                                    // fresh engine
        eng.load_catalog(path);
        assert(eng.column_type(3) == MatrixType::U32 && eng.column_type(7) == MatrixType::I64 && "types restored");
        // u32 column value check
        MatrixQuery qu{}; qu.value_col = 3; qu.agg = AGG_SUM; std::vector<uint64_t> ou;
        assert(eng.execute_query(qu, ou) == MatrixQueryStatus::OK && ou[0] == u32_sum(u) && "u32 column restored");
        // int64 column value checks (SUM/MIN/MAX) — the negatives + large value must survive
        MatrixQuery qs{}; qs.value_col = 7; qs.agg = AGG_SUM; std::vector<uint64_t> os;
        assert(eng.execute_query(qs, os) == MatrixQueryStatus::OK && static_cast<int64_t>(os[0]) == i64_sum(s) && "int64 SUM restored");
        MatrixQuery qmax{}; qmax.value_col = 7; qmax.agg = AGG_MAX; std::vector<uint64_t> omax;
        eng.execute_query(qmax, omax); assert(static_cast<int64_t>(omax[0]) == 5000000000LL && "int64 MAX (>UINT32_MAX) restored");
        MatrixQuery qmin{}; qmin.value_col = 7; qmin.agg = AGG_MIN; std::vector<uint64_t> omin;
        eng.execute_query(qmin, omin); assert(static_cast<int64_t>(omin[0]) == -100 && "int64 MIN (negative) restored");
    }
    std::remove(path.c_str());
    std::cout << "[mixed catalog roundtrip] ok\n";
}

static void test_empty_catalog() {
    const std::string path = "/tmp/mdb_empty_catalog.bin"; std::remove(path.c_str());
    { CPUMockEngine eng; eng.save_catalog(path); }
    { CPUMockEngine eng; eng.load_catalog(path); }   // must not crash
    std::remove(path.c_str());
    std::cout << "[empty catalog] ok\n";
}

int main() {
    test_mixed_roundtrip();
    test_empty_catalog();
    std::cout << "ALL TYPED-SNAPSHOT TESTS PASSED\n";
    return 0;
}

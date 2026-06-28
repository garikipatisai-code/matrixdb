// CPU test for backup/restore (DU-6): backup() writes <prefix>.catalog + <prefix>.kv; restore() loads
// both into a fresh engine — analytical columns (u32 + int64) and point-op writes all survive.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <iostream>

static void test_backup_roundtrip() {
    const std::string pfx = "/tmp/mdb_backup_rt";
    std::remove((pfx + ".catalog").c_str()); std::remove((pfx + ".kv").c_str());
    std::vector<uint32_t> u(50); for (size_t i = 0; i < u.size(); ++i) u[i] = static_cast<uint32_t>(i);
    std::vector<int64_t>  s = {-7, 5000000000LL, 3, -100};
    {
        CPUMockEngine eng;                                   // no WAL — backup must still capture kv_
        eng.load_scan_column(3, u.data(), u.size());
        eng.load_scan_column_i64(7, s.data(), s.size());
        eng.begin(); eng.txn_put(11, 111); eng.txn_put(22, 222); eng.commit();
        eng.begin(); eng.txn_put(33, 333); eng.commit();
        eng.backup(pfx);
    }
    {
        CPUMockEngine eng;                                   // fresh engine
        eng.restore(pfx);
        // analytical columns restored (type + values)
        assert(eng.column_type(3) == MatrixType::U32 && eng.column_type(7) == MatrixType::I64);
        MatrixQuery qu{}; qu.value_col = 3; qu.agg = AGG_SUM; std::vector<uint64_t> ou;
        assert(eng.execute_query(qu, ou) == MatrixQueryStatus::OK && ou[0] == (49u * 50u / 2u)); // 0..49 = 1225
        MatrixQuery qs{}; qs.value_col = 7; qs.agg = AGG_SUM; std::vector<uint64_t> os;
        eng.execute_query(qs, os); assert(static_cast<int64_t>(os[0]) == -7 + 5000000000LL + 3 - 100);
        // point-op writes restored
        uint64_t v = 0;
        assert(eng.kv_get(11, v) && v == 111);
        assert(eng.kv_get(22, v) && v == 222);
        assert(eng.kv_get(33, v) && v == 333);
        assert(!eng.kv_get(99, v) && "absent key still absent");
    }
    std::remove((pfx + ".catalog").c_str()); std::remove((pfx + ".kv").c_str());
    std::cout << "[backup roundtrip] ok\n";
}

static void test_backup_empty() {
    const std::string pfx = "/tmp/mdb_backup_empty";
    std::remove((pfx + ".catalog").c_str()); std::remove((pfx + ".kv").c_str());
    { CPUMockEngine eng; eng.backup(pfx); }
    { CPUMockEngine eng; eng.restore(pfx); }   // empty catalog + empty kv -> no crash
    std::remove((pfx + ".catalog").c_str()); std::remove((pfx + ".kv").c_str());
    std::cout << "[backup empty] ok\n";
}

int main() { test_backup_roundtrip(); test_backup_empty();
    std::cout << "ALL BACKUP TESTS PASSED\n"; return 0; }

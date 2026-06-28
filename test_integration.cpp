// End-to-end INTEGRATION test (QA-2): exercises the major features COMPOSING in one realistic flow —
// typed CSV ingest -> naming -> catalog introspection -> parse_query -> execute -> append -> hash_join
// -> backup/restore. Asserts at each stage. (Unit tests cover each feature in isolation; this proves
// they work together.) Also documents one finding: column names are RAM-only (not in backup/restore).
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <algorithm>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
#include <iostream>

int main() {
    const std::string csv = "/tmp/mdb_integ.csv";
    std::ofstream(csv) << "region,revenue\n0,500000000\n1,2000000000\n0,1500000000\n2,3000000000\n";
    const std::string bk = "/tmp/mdb_integ_backup";
    std::remove((bk + ".catalog").c_str()); std::remove((bk + ".kv").c_str());

    // --- Stage 1: typed CSV ingest (u32 region col 0, int64 revenue col 1) + naming ---
    CPUMockEngine eng;
    assert(eng.load_column_from_csv(2, csv, 0, /*has_header=*/true) && "region (u32) from CSV");
    assert(eng.load_column_from_csv_i64(7, csv, 1, /*has_header=*/true) && "revenue (int64) from CSV");
    eng.name_column(2, "region"); eng.name_column(7, "revenue");
    assert(eng.column_type(2) == MatrixType::U32 && eng.column_type(7) == MatrixType::I64);

    // --- Stage 2: catalog introspection ---
    {
        auto info = eng.catalog_columns();
        std::sort(info.begin(), info.end(), [](const ColumnInfo& a, const ColumnInfo& b){ return a.id < b.id; });
        assert(info.size() == 2);
        assert(info[0].id == 2 && info[0].name == "region"  && info[0].type == MatrixType::U32 && info[0].rows == 4);
        assert(info[1].id == 7 && info[1].name == "revenue" && info[1].type == MatrixType::I64 && info[1].rows == 4);
    }

    // --- Stage 3: parse + execute (SUM revenue WHERE revenue > 1e9) ---
    MatrixQuery q;
    assert(eng.parse_query("SELECT SUM(revenue) WHERE revenue > 1000000000", q) == MatrixQueryStatus::OK);
    assert(q.value_col == 7 && q.cmp == MatrixCmp::GT && q.lo_i64 == 1000000000LL);
    std::vector<uint64_t> o;
    assert(eng.execute_query(q, o) == MatrixQueryStatus::OK);
    assert(static_cast<int64_t>(o[0]) == 2000000000LL + 1500000000LL + 3000000000LL && "revenue>1e9 sum");

    // --- Stage 4: append a row, re-run the same parsed query (sum grows by the appended 4e9) ---
    int64_t more[] = {4000000000LL};
    eng.append_to_column_i64(7, more, 1);
    assert(eng.column_rows(7) == 5);
    assert(eng.execute_query(q, o) == MatrixQueryStatus::OK);
    assert(static_cast<int64_t>(o[0]) == 2000000000LL + 1500000000LL + 3000000000LL + 4000000000LL && "appended row joins the filter");

    // --- Stage 5: equi-join region against a lookup of valid regions ---
    std::vector<uint32_t> valid = {0, 2};
    eng.load_scan_column(3, valid.data(), valid.size()); eng.name_column(3, "valid_region");
    // region (rows now... region wasn't appended, still {0,1,0,2}); join region(2) x valid(3)
    auto pairs = eng.hash_join(2, 3);
    std::sort(pairs.begin(), pairs.end());
    // region {0,1,0,2} x valid {0,2}: 0@0=0@0, 0@2=0@0, 2@3=2@1
    assert((pairs == std::vector<std::pair<uint64_t,uint64_t>>{{0,0},{2,0},{3,1}}) && "join region x valid");

    // --- Stage 6: backup -> restore into a fresh engine -> revenue survived (with the appended row) ---
    eng.backup(bk);
    {
        CPUMockEngine fresh;
        fresh.restore(bk);
        assert(fresh.column_type(7) == MatrixType::I64 && fresh.column_rows(7) == 5 && "revenue restored incl. appended row");
        MatrixQuery qa{}; qa.value_col = 7; qa.agg = AGG_SUM;   // query BY ID — names are RAM-only (see finding)
        std::vector<uint64_t> oa;
        assert(fresh.execute_query(qa, oa) == MatrixQueryStatus::OK);
        assert(static_cast<int64_t>(oa[0]) == 500000000LL + 2000000000LL + 1500000000LL + 3000000000LL + 4000000000LL
               && "full revenue (incl. appended) survived backup/restore");
        // FINDING: names are not persisted by the catalog snapshot — column_id is 0 in the restored engine.
        assert(fresh.column_id("revenue") == 0 && "names are RAM-only (persisting them is a deferred enhancement)");
    }

    std::remove(csv.c_str()); std::remove((bk + ".catalog").c_str()); std::remove((bk + ".kv").c_str());
    std::cout << "ALL INTEGRATION TESTS PASSED\n";
    return 0;
}

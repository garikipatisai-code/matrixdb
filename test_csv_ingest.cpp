// CPU test for CSV ingest (matrix_read_csv_column + CPUMockEngine::load_column_from_csv).
#include "server.hpp"        // pulls in compute_mock.cpp (engine) — execute_query, MatrixQueryStatus
#include "csv_ingest.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <iostream>

static std::string write_tmp(const std::string& name, const std::string& body) {
    const std::string path = "/tmp/" + name;
    std::ofstream(path) << body;
    return path;
}

static void test_parser() {
    std::vector<uint32_t> out;

    // Basic: two columns, pick each.
    std::string p = write_tmp("mdb_csv_basic.csv", "1,10\n2,20\n3,30\n");
    assert(matrix_read_csv_column(p, 1, false, ',', out) && out == (std::vector<uint32_t>{10, 20, 30}));
    assert(matrix_read_csv_column(p, 0, false, ',', out) && out == (std::vector<uint32_t>{1, 2, 3}));

    // Header skip.
    p = write_tmp("mdb_csv_hdr.csv", "key,val\n5,50\n6,60\n");
    assert(matrix_read_csv_column(p, 1, true, ',', out) && out == (std::vector<uint32_t>{50, 60}));

    // Custom delimiter.
    p = write_tmp("mdb_csv_semi.csv", "7;70\n8;80\n");
    assert(matrix_read_csv_column(p, 1, false, ';', out) && out == (std::vector<uint32_t>{70, 80}));

    // Empty file -> true, empty. Header-only -> true, empty.
    p = write_tmp("mdb_csv_empty.csv", "");
    assert(matrix_read_csv_column(p, 0, false, ',', out) && out.empty());
    p = write_tmp("mdb_csv_hdronly.csv", "k,v\n");
    assert(matrix_read_csv_column(p, 0, true, ',', out) && out.empty());

    // CRLF tolerance.
    p = write_tmp("mdb_csv_crlf.csv", "1,2\r\n3,4\r\n");
    assert(matrix_read_csv_column(p, 0, false, ',', out) && out == (std::vector<uint32_t>{1, 3}));

    // Graceful failures: each returns false and clears out.
    assert(!matrix_read_csv_column("/tmp/mdb_csv_does_not_exist.csv", 0, false, ',', out));
    p = write_tmp("mdb_csv_short.csv", "1,2\n3\n");          // row 2 has no field index 1
    assert(!matrix_read_csv_column(p, 1, false, ',', out) && out.empty());
    p = write_tmp("mdb_csv_nonint.csv", "1,x\n");
    assert(!matrix_read_csv_column(p, 1, false, ',', out));
    p = write_tmp("mdb_csv_junk.csv", "12x\n");              // trailing junk -> not a full integer
    assert(!matrix_read_csv_column(p, 0, false, ',', out));
    p = write_tmp("mdb_csv_over.csv", "5000000000\n");       // > UINT32_MAX
    assert(!matrix_read_csv_column(p, 0, false, ',', out));
    std::cout << "[csv parser] ok\n";
}

static void test_engine_ingest() {
    const std::string wal = "/tmp/mdb_csv_eng.bin"; std::remove(wal.c_str());
    CPUMockEngine eng(0, wal);
    const std::string p = write_tmp("mdb_csv_eng.csv", "k,v\n10,100\n20,200\n30,300\n");

    assert(eng.load_column_from_csv(7, p, 1, /*has_header=*/true));      // values {100,200,300}
    MatrixQuery q{}; q.value_col = 7; q.agg = AGG_SUM;
    std::vector<uint64_t> r;
    assert(eng.execute_query(q, r) == MatrixQueryStatus::OK);
    assert(r.size() == 1 && r[0] == 600 && "SUM of ingested column");   // 100+200+300

    // Malformed CSV -> false, and NO catalog entry created (query on id 8 is unknown).
    const std::string bad = write_tmp("mdb_csv_bad.csv", "1,x\n");
    assert(!eng.load_column_from_csv(8, bad, 1));
    MatrixQuery q8{}; q8.value_col = 8; q8.agg = AGG_COUNT;
    std::vector<uint64_t> r8;
    assert(eng.execute_query(q8, r8) == MatrixQueryStatus::ERR_UNKNOWN_COLUMN && "no entry from bad CSV");
    std::remove(wal.c_str());
    std::cout << "[csv engine ingest] ok\n";
}

int main() {
    test_parser();
    test_engine_ingest();
    std::cout << "ALL CSV INGEST TESTS PASSED\n";
    return 0;
}

// CPU test for the text query parser (DM-4): parse_query parses scalar SELECT/WHERE into a MatrixQuery
// (name resolution + type-aware bound placement); graceful ERR_PARSE / ERR_UNKNOWN_COLUMN on bad input.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <bit>
#include <string>
#include <vector>
#include <iostream>

static void test_happy() {
    std::vector<uint32_t> q = {1, 2, 3, 4, 5};                 // qty, sum 15
    std::vector<int64_t>  r = {-100, 5000000000LL, 50, 2000000000LL};  // revenue
    std::vector<double>   t = {1.5, 3.5, 4.0, 2.0};            // rate
    CPUMockEngine eng;
    eng.load_scan_column(3, q.data(), q.size());     eng.name_column(3, "qty");
    eng.load_scan_column_i64(7, r.data(), r.size()); eng.name_column(7, "revenue");
    eng.load_scan_column_f64(9, t.data(), t.size()); eng.name_column(9, "rate");

    MatrixQuery m; std::vector<uint64_t> o;
    // unfiltered scalar
    assert(eng.parse_query("SELECT SUM(qty)", m) == MatrixQueryStatus::OK);
    assert(m.value_col == 3 && m.agg == AGG_SUM && !m.has_filter);
    eng.execute_query(m, o); assert(o[0] == 15);
    // int64 GT bound -> lo_i64 (NOT threshold)
    assert(eng.parse_query("SELECT COUNT(revenue) WHERE revenue > 1000000000", m) == MatrixQueryStatus::OK);
    assert(m.value_col == 7 && m.agg == AGG_COUNT && m.has_filter && m.cmp == MatrixCmp::GT
           && m.lo_i64 == 1000000000LL && m.threshold == 0 && "int64 bound in lo_i64");
    eng.execute_query(m, o); assert(o[0] == 2 && "revenue>1e9 -> {5e9, 2e9}");
    // double LE bound -> lo_f64
    assert(eng.parse_query("SELECT MAX(rate) WHERE rate <= 3.5", m) == MatrixQueryStatus::OK);
    assert(m.cmp == MatrixCmp::LE && m.lo_f64 == 3.5 && "double bound in lo_f64");
    eng.execute_query(m, o); assert(std::bit_cast<double>(o[0]) == 3.5);
    // BETWEEN (int64)
    assert(eng.parse_query("SELECT SUM(revenue) WHERE revenue BETWEEN -100 AND 5000000000", m) == MatrixQueryStatus::OK);
    assert(m.cmp == MatrixCmp::BETWEEN && m.lo_i64 == -100 && m.hi_i64 == 5000000000LL);
    eng.execute_query(m, o); assert(static_cast<int64_t>(o[0]) == -100 + 5000000000LL + 50 + 2000000000LL);
    // case-insensitive + spacing
    assert(eng.parse_query("select sum ( qty )", m) == MatrixQueryStatus::OK && m.value_col == 3);
    std::cout << "[parser happy] ok\n";
}

static void test_errors() {
    std::vector<uint32_t> q = {1, 2, 3};
    CPUMockEngine eng; eng.load_scan_column(3, q.data(), q.size()); eng.name_column(3, "qty");
    MatrixQuery m;
    assert(eng.parse_query("", m) == MatrixQueryStatus::ERR_PARSE);
    assert(eng.parse_query("SELECT FOO(qty)", m) == MatrixQueryStatus::ERR_PARSE);
    assert(eng.parse_query("SELECT SUM(nosuchcol)", m) == MatrixQueryStatus::ERR_UNKNOWN_COLUMN);
    assert(eng.parse_query("SELECT SUM(qty", m) == MatrixQueryStatus::ERR_PARSE);          // missing )
    assert(eng.parse_query("SELECT SUM(qty) WHERE qty >", m) == MatrixQueryStatus::ERR_PARSE);   // missing value
    assert(eng.parse_query("SELECT SUM(qty) WHERE qty > x", m) == MatrixQueryStatus::ERR_PARSE); // non-numeric
    assert(eng.parse_query("SELECT SUM(qty) WHERE qty BETWEEN 1 5", m) == MatrixQueryStatus::ERR_PARSE); // missing AND
    assert(eng.parse_query("SELECT SUM(qty) GROUP qty", m) == MatrixQueryStatus::ERR_PARSE);       // GROUP without BY
    assert(eng.parse_query("SELECT SUM(qty) GROUP BY nosuchkey", m) == MatrixQueryStatus::ERR_UNKNOWN_COLUMN);
    assert(eng.parse_query("SELECT SUM(qty) extra", m) == MatrixQueryStatus::ERR_PARSE);    // trailing junk
    // out is reset on a MID-parse failure (no partial state leaks to a caller that ignores the status).
    MatrixQuery dirty;
    assert(eng.parse_query("SELECT SUM(qty) WHERE qty BETWEEN 1 AND x", dirty) == MatrixQueryStatus::ERR_PARSE);
    assert(dirty.value_col == 0 && !dirty.has_filter && dirty.cmp == MatrixCmp::GT && "out reset on mid-parse error");
    std::cout << "[parser errors] ok\n";
}

static void test_groupby() {
    std::vector<uint32_t> region = {0, 1, 0, 2}, amount = {10, 20, 30, 40};
    CPUMockEngine eng;
    eng.load_scan_column(2, region.data(), region.size()); eng.name_column(2, "region");
    eng.load_scan_column(3, amount.data(), amount.size()); eng.name_column(3, "amount");
    MatrixQuery m; std::vector<uint64_t> o;
    // SUM(amount) GROUP BY region — num_groups derived as max(region)+1 = 3
    assert(eng.parse_query("SELECT SUM(amount) GROUP BY region", m) == MatrixQueryStatus::OK);
    assert(m.grouped && m.value_col == 3 && m.key_col == 2 && m.num_groups == 3 && "GROUP BY parsed; num_groups = max key + 1");
    assert(eng.execute_query(m, o) == MatrixQueryStatus::OK && o.size() == 3);
    assert(o[0] == 40 && o[1] == 20 && o[2] == 40 && "grouped SUM: r0=10+30, r1=20, r2=40");
    // filtered grouped: COUNT(amount) WHERE amount > 15 GROUP BY region
    assert(eng.parse_query("SELECT COUNT(amount) WHERE amount > 15 GROUP BY region", m) == MatrixQueryStatus::OK);
    assert(m.grouped && m.has_filter && m.cmp == MatrixCmp::GT && m.threshold == 15 && m.key_col == 2);
    assert(eng.execute_query(m, o) == MatrixQueryStatus::OK);
    assert(o[0] == 1 && o[1] == 1 && o[2] == 1 && "amount>15 per region: r0{30}, r1{20}, r2{40}");
    std::cout << "[parser group by] ok\n";
}

int main() { test_happy(); test_errors(); test_groupby();
    std::cout << "ALL QUERY-PARSER TESTS PASSED\n"; return 0; }

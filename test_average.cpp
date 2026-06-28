// CPU test for AVG (DM-4): average() derives SUM/COUNT as double(s). Scalar -> one element (NULL-aware,
// since scalar SUM/COUNT skip nulls); grouped -> one per group; zero-count -> NaN. Typed across U32/I64/F64.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <cmath>
#include <vector>
#include <iostream>

static double scalar_avg(CPUMockEngine& eng, uint64_t id) {
    MatrixQuery q{}; q.value_col = id;
    auto a = eng.average(q);
    assert(a.size() == 1 && "scalar AVG -> one element");
    return a[0];
}

static void test_avg_scalar() {
    CPUMockEngine eng;
    std::vector<uint32_t> u = {10, 20, 30, 40, 50};                 // sum 150, count 5 -> 30
    eng.load_scan_column(2, u.data(), u.size());
    assert(scalar_avg(eng, 2) == 30.0 && "u32 scalar AVG = sum/count");

    std::vector<int64_t> s = {-10, 0, 10, 5000000000LL};            // sum 5000000000, count 4 -> 1.25e9
    eng.load_scan_column_i64(3, s.data(), s.size());
    assert(scalar_avg(eng, 3) == 1250000000.0 && "i64 scalar AVG (negatives, big)");

    std::vector<double> d = {1.0, 2.0, 6.0};                        // sum 9, count 3 -> 3.0
    eng.load_scan_column_f64(4, d.data(), d.size());
    assert(scalar_avg(eng, 4) == 3.0 && "f64 scalar AVG");

    // NULL-aware (scalar): mark rows 0,4 null on the u32 col -> {20,30,40} -> 30
    eng.set_column_nulls(2, {1, 0, 0, 0, 1});
    assert(scalar_avg(eng, 2) == 30.0 && "scalar AVG skips nulls (sum 90 / count 3)");
    // non-vacuity: a different mask gives a different average
    eng.set_column_nulls(2, {0, 0, 0, 0, 1});                       // {10,20,30,40} -> 25
    assert(scalar_avg(eng, 2) == 25.0 && "mask actually changes the average");

    // all-null -> count 0 -> NaN
    std::vector<uint32_t> w = {7, 8, 9};
    eng.load_scan_column(5, w.data(), w.size());
    eng.set_column_nulls(5, {1, 1, 1});
    assert(std::isnan(scalar_avg(eng, 5)) && "all-null -> NaN");
    std::cout << "[avg scalar] ok\n";
}

static void test_avg_grouped() {
    CPUMockEngine eng;
    std::vector<uint32_t> region = {0, 1, 0, 2}, amount = {10, 20, 30, 40};
    eng.load_scan_column(1, region.data(), region.size());
    eng.load_scan_column(2, amount.data(), amount.size());
    MatrixQuery q{}; q.value_col = 2; q.key_col = 1; q.num_groups = 3; q.grouped = true;
    auto a = eng.average(q);                                        // g0=(10+30)/2=20, g1=20, g2=40
    assert(a.size() == 3 && a[0] == 20.0 && a[1] == 20.0 && a[2] == 40.0 && "grouped AVG per group");
    // grouped AVG is NULL-aware too (grouped SUM+COUNT both skip nulls): null row 2 (region 0, amount 30)
    // -> g0 = 10/1 = 10 (not 40/2 = 20)
    eng.set_column_nulls(2, {0, 0, 1, 0});
    auto an = eng.average(q);
    assert(an.size() == 3 && an[0] == 10.0 && an[1] == 20.0 && an[2] == 40.0 && "grouped AVG skips null row");
    std::cout << "[avg grouped] ok\n";
}

// avg_query: the string entry point — "SELECT AVG(col) [WHERE ...] [GROUP BY k]" -> rewrite AVG->SUM,
// parse, derive. Round-trips through the full parser (WHERE + GROUP BY supported).
static void test_avg_query() {
    CPUMockEngine eng;
    std::vector<uint32_t> region = {0, 1, 0, 2}, amount = {10, 20, 30, 40};
    eng.load_scan_column(1, region.data(), region.size());
    eng.load_scan_column(2, amount.data(), amount.size());
    eng.name_column(1, "region"); eng.name_column(2, "amount");
    auto s = eng.avg_query("SELECT AVG(amount)");                  // (10+20+30+40)/4 = 25
    assert(s.size() == 1 && s[0] == 25.0 && "scalar AVG from string");
    auto w = eng.avg_query("SELECT AVG(amount) WHERE amount >= 30");// (30+40)/2 = 35
    assert(w.size() == 1 && w[0] == 35.0 && "filtered AVG from string");
    auto g = eng.avg_query("SELECT AVG(amount) GROUP BY region");  // g0=(10+30)/2=20, g1=20, g2=40
    assert(g.size() == 3 && g[0] == 20.0 && g[1] == 20.0 && g[2] == 40.0 && "grouped AVG from string");
    assert(eng.avg_query("SELECT SUM(amount)").empty() && "non-AVG query -> empty");
    assert(eng.avg_query("garbage").empty() && "junk -> empty");
    std::cout << "[avg query] ok\n";
}

int main() { test_avg_scalar(); test_avg_grouped(); test_avg_query(); std::cout << "ALL AVG TESTS PASSED\n"; return 0; }

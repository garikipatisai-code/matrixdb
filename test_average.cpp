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
    std::cout << "[avg grouped] ok\n";
}

int main() { test_avg_scalar(); test_avg_grouped(); std::cout << "ALL AVG TESTS PASSED\n"; return 0; }

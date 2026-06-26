// CPU test for catalog snapshot durability (save_catalog / load_catalog).
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <cstdio>
#include <iostream>

static uint64_t sum_of(const std::vector<uint32_t>& v) { uint64_t s = 0; for (uint32_t x : v) s += x; return s; }
static uint64_t engine_sum(CPUMockEngine& e, uint64_t id) {
    std::vector<uint64_t> out; e.execute_query(MatrixQuery{.value_col = id, .agg = AGG_SUM}, out); return out[0];
}

static void test_multicolumn_roundtrip() {
    const size_t N = 1000;
    std::vector<uint32_t> a(N), b(N), c(N);
    for (size_t i = 0; i < N; ++i) { a[i] = i; b[i] = i + 1000; c[i] = static_cast<uint32_t>(i * 2); }
    const std::string path = "/tmp/matrixdb_catsnap.bin";
    CPUMockEngine src(0, "", SIZE_MAX);
    src.load_scan_column(1, a.data(), N);
    src.load_scan_column(2, b.data(), N);
    src.load_scan_column(3, c.data(), N);
    src.save_catalog(path);

    CPUMockEngine dst(0, "", SIZE_MAX);
    dst.load_catalog(path);
    assert(dst.stats().catalog_columns == 3);
    assert(engine_sum(dst, 1) == sum_of(a));
    assert(engine_sum(dst, 2) == sum_of(b));
    assert(engine_sum(dst, 3) == sum_of(c));
    assert(sum_of(a) != sum_of(b) && sum_of(b) != sum_of(c));
    std::remove(path.c_str());
    std::cout << "[catalog multi-column round-trip] ok\n";
}

static void test_snapshot_with_cold_column() {
    const size_t N = 1000;
    const size_t S = N * sizeof(uint32_t);
    std::vector<uint32_t> a(N), dummy(N, 0);
    for (size_t i = 0; i < N; ++i) a[i] = i;
    const std::string path = "/tmp/matrixdb_catsnap_cold.bin";
    CPUMockEngine src(0, "", /*host_cap=*/S);
    src.load_scan_column(1, a.data(), N);
    src.load_scan_column(9, dummy.data(), N);
    for (int r = 0; r < 12; ++r) { DatabaseQuery q{}; matrix_set_scan_target(q, 0u, 9); src.execute_scan(q); }
    assert(src.column_tier(1) == MemorySpace::COLD && "col 1 demoted before snapshot");
    src.save_catalog(path);
    assert(src.column_tier(1) == MemorySpace::COLD && "save returned the borrow");

    CPUMockEngine dst(0, "", SIZE_MAX);
    dst.load_catalog(path);
    assert(engine_sum(dst, 1) == sum_of(a) && "COLD column restored correctly");
    assert(engine_sum(dst, 9) == 0 && "dummy column restored");
    std::remove(path.c_str());
    std::cout << "[catalog snapshot with COLD column] ok\n";
}

static void test_empty_catalog() {
    const std::string path = "/tmp/matrixdb_catsnap_empty.bin";
    CPUMockEngine src(0, "", SIZE_MAX);
    src.save_catalog(path);
    CPUMockEngine dst(0, "", SIZE_MAX);
    dst.load_catalog(path);
    assert(dst.stats().catalog_columns == 0);
    std::remove(path.c_str());
    std::cout << "[empty catalog snapshot] ok\n";
}

int main() {
    test_multicolumn_roundtrip();
    test_snapshot_with_cold_column();
    test_empty_catalog();
    std::cout << "ALL CATALOG-SNAPSHOT TESTS PASSED\n";
    return 0;
}

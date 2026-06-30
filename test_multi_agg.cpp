// Multi-aggregate SELECT: query_multi runs a comma-separated aggregate list in one call, returning one
// result column per aggregate. It splits the SELECT list and delegates each "SELECT agg(col) <tail>" to the
// full parser+executor, so it inherits WHERE (incl. cross-column), GROUP BY, and per-type handling. Verifies
// scalar multi-stat, a shared WHERE, grouped multi-aggregate, and graceful empty on an unknown column.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <algorithm>

int main() {
    CPUMockEngine eng;
    const size_t N = 100000;
    std::vector<uint32_t> x(N), k(N), v(N);
    for (size_t i = 0; i < N; ++i) { x[i] = static_cast<uint32_t>(i % 1000); k[i] = static_cast<uint32_t>(i % 4); v[i] = static_cast<uint32_t>(i % 100); }
    eng.load_scan_column(1, x.data(), N); eng.name_column(1, "x");
    eng.load_scan_column(2, k.data(), N); eng.name_column(2, "k");
    eng.load_scan_column(3, v.data(), N); eng.name_column(3, "v");

    // scalar multi-stat over one column
    {
        auto r = eng.query_multi("SELECT COUNT(x), SUM(x), MIN(x), MAX(x)");
        uint64_t sum = 0, mn = UINT64_MAX, mx = 0;
        for (uint32_t e : x) { sum += e; mn = std::min<uint64_t>(mn, e); mx = std::max<uint64_t>(mx, e); }
        assert(r.size() == 4 && r[0].size() == 1);
        assert(r[0][0] == N && r[1][0] == sum && r[2][0] == mn && r[3][0] == mx);
        std::printf("[multi scalar] ok (count=%llu sum=%llu min=%llu max=%llu)\n",
                    (unsigned long long)r[0][0], (unsigned long long)r[1][0], (unsigned long long)r[2][0], (unsigned long long)r[3][0]);
    }

    // shared WHERE across the aggregate list
    {
        auto r = eng.query_multi("SELECT COUNT(x), SUM(x) WHERE x > 500");
        uint64_t c = 0, s = 0; for (uint32_t e : x) if (e > 500) { ++c; s += e; }
        assert(r.size() == 2 && r[0][0] == c && r[1][0] == s);
        std::printf("[multi filtered] ok (count=%llu sum=%llu)\n", (unsigned long long)r[0][0], (unsigned long long)r[1][0]);
    }

    // grouped multi-aggregate (one result column per aggregate, num_groups per column)
    {
        auto r = eng.query_multi("SELECT COUNT(v), SUM(v), MAX(v) GROUP BY k");
        assert(r.size() == 3 && r[0].size() == 4 && r[1].size() == 4 && r[2].size() == 4);
        std::vector<uint64_t> oc(4, 0), os(4, 0), om(4, 0);
        for (size_t i = 0; i < N; ++i) { oc[k[i]]++; os[k[i]] += v[i]; om[k[i]] = std::max<uint64_t>(om[k[i]], v[i]); }
        for (int g = 0; g < 4; ++g) assert(r[0][g] == oc[g] && r[1][g] == os[g] && r[2][g] == om[g]);
        std::printf("[multi grouped] ok (g0 count=%llu sum=%llu max=%llu)\n",
                    (unsigned long long)r[0][0], (unsigned long long)r[1][0], (unsigned long long)r[2][0]);
    }

    // graceful: an unknown column in the list yields an empty result, not a crash
    {
        auto r = eng.query_multi("SELECT COUNT(nope), SUM(x)");
        assert(r.empty());
        std::printf("[multi error] ok (empty on unknown column)\n");
    }

    std::printf("ALL MULTI-AGG TESTS PASSED\n");
    return 0;
}

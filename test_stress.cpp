// CPU stress / load test (QA-5): sustained query load + heavy tiering churn + append + join at scale,
// every result checked against a closed-form oracle (value[i] = i). Catches scale bugs (overflow,
// residency accounting, churn) the small fixed-input unit tests miss. Sized to run in ~1-2s.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

int main() {
    constexpr uint32_t N = 50000;                 // rows per column (value[i] = i)
    std::vector<uint32_t> col(N);
    for (uint32_t i = 0; i < N; ++i) col[i] = i;

    // Small host budget vs 4x 200KB columns -> the engine must churn (borrow/demote/promote) under load.
    CPUMockEngine eng(0, "", 300 * 1024);
    for (uint64_t id = 1; id <= 4; ++id) eng.load_scan_column(id, col.data(), col.size());

    // Closed-form: for value[i]=i over [0,N), SUM(value > t) = (N-1)N/2 - t(t+1)/2 ; COUNT(value > t) = N-1-t.
    auto sum_gt = [&](uint64_t t) { return (uint64_t)(N - 1) * N / 2 - t * (t + 1) / 2; };

    // 300 filtered queries cycling the 4 columns — sustained load + rebalance/borrow churn.
    uint64_t seed = 1;
    for (int q = 0; q < 300; ++q) {
        const uint64_t id = 1 + (q % 4);
        seed = seed * 6364136223846793005ull + 1; const uint32_t t = (seed >> 40) % N;   // varied threshold
        MatrixQuery mq{}; mq.value_col = id; mq.agg = AGG_SUM; mq.has_filter = true; mq.cmp = MatrixCmp::GT; mq.threshold = t;
        std::vector<uint64_t> o;
        assert(eng.execute_query(mq, o) == MatrixQueryStatus::OK && o[0] == sum_gt(t) && "filtered SUM correct under load");
        MatrixQuery cq{}; cq.value_col = id; cq.agg = AGG_COUNT; cq.has_filter = true; cq.cmp = MatrixCmp::GT; cq.threshold = t;
        std::vector<uint64_t> oc; eng.execute_query(cq, oc);
        assert(oc[0] == (uint64_t)(N - 1 - t) && "filtered COUNT correct under load");
    }

    // Append stress: grow column 1 by 500 small batches; final SUM includes the appended rows.
    uint64_t appended_sum = 0; uint32_t extra[8];
    for (int b = 0; b < 500; ++b) {
        for (int j = 0; j < 8; ++j) { extra[j] = 1000000u + b * 8 + j; appended_sum += extra[j]; }
        eng.append_to_column(1, extra, 8);
    }
    assert(eng.column_rows(1) == N + 500 * 8 && "appended rows present");
    { MatrixQuery mq{}; mq.value_col = 1; mq.agg = AGG_SUM; std::vector<uint64_t> o; eng.execute_query(mq, o);
      assert(o[0] == (uint64_t)(N - 1) * N / 2 + appended_sum && "full SUM incl. appended rows correct"); }

    // Join at scale: col2 (=col, value[i]=i) joined with itself-shaped col3 -> N matching pairs (each i once).
    assert(eng.hash_join_count(2, 3) == N && "self-aligned join cardinality == N");

    // The tiering must have actually churned under this load (not a no-op).
    const EngineStats s = eng.stats();
    assert(s.rebalances > 0 && s.cold_borrows > 0 && "tiering churned under sustained load");
    assert(s.query_count >= 300 && "all queries counted");
    std::cout << "stress: 300 queries + 500 appends + " << N << "-row join, churn rebalances=" << s.rebalances
              << " cold_borrows=" << s.cold_borrows << " (all results oracle-correct)\n";
    std::cout << "ALL STRESS TESTS PASSED\n";
    return 0;
}

// CPU test for RM-2 admission control: set_max_query_groups caps a single grouped query's group count
// (result-memory guard). A query above the cap returns ERR_TOO_MANY_GROUPS with no allocation; the
// default (2^28) leaves existing queries unaffected.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

static void test_admission_control() {
    std::vector<uint32_t> region = {0, 1, 2, 1, 0}, amount = {10, 20, 30, 40, 50};
    CPUMockEngine eng;
    eng.load_scan_column(1, region.data(), region.size());
    eng.load_scan_column(2, amount.data(), amount.size());
    MatrixQuery q{}; q.value_col = 2; q.key_col = 1; q.num_groups = 3; q.agg = AGG_SUM; q.grouped = true;

    // default cap (2^28): the 3-group query runs fine
    assert(eng.max_query_groups() == (1u << 28) && "default cap is 2^28");
    std::vector<uint64_t> o;
    assert(eng.execute_query(q, o) == MatrixQueryStatus::OK && o.size() == 3 && "under default cap -> OK");

    // tighten the cap below the query's group count -> rejected, no allocation
    eng.set_max_query_groups(2);
    std::vector<uint64_t> o2;
    assert(eng.execute_query(q, o2) == MatrixQueryStatus::ERR_TOO_MANY_GROUPS && "3 groups > cap 2 -> rejected");
    assert(o2.empty() && "rejected query allocates nothing");

    // cap exactly at the group count -> allowed (boundary: > cap, not >=)
    eng.set_max_query_groups(3);
    assert(eng.execute_query(q, o) == MatrixQueryStatus::OK && "num_groups == cap -> OK (strict >)");

    // raise it back -> OK again (knob is live)
    eng.set_max_query_groups(1u << 28);
    assert(eng.execute_query(q, o) == MatrixQueryStatus::OK && "cap raised -> OK again");
    std::cout << "[admission control] ok\n";
}

int main() { test_admission_control(); std::cout << "ALL ADMISSION-CONTROL TESTS PASSED\n"; return 0; }

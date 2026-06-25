// Coverage test for the items-per-thread scan kernel's index arithmetic.
// The kernel's index math is pure integer logic — hardware-independent — so we can
// simulate it on the CPU and verify every index in [0,n) is visited exactly once.
// This is the test that WOULD have caught the GPU-only undercount bug.
//
// Build: clang++ -std=c++17 -O2 test_scan_coverage.cpp -o /tmp/tcov && /tmp/tcov
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>

// Simulate the kernel's visited-index set for a given base-init convention.
// blocked_init=true reproduces the BUG (base = blockIdx*blockDim*ITEMS + tid);
// blocked_init=false is the FIX (base = blockIdx*blockDim + tid).
static bool coverage_ok(size_t n, int GRID, int TPB, int ITEMS, bool blocked_init) {
    std::vector<int> visit(n, 0);
    const size_t base_stride = (size_t)GRID * TPB;
    const size_t chunk = base_stride * ITEMS;
    for (int b = 0; b < GRID; ++b) {
        for (int x = 0; x < TPB; ++x) {
            size_t base = blocked_init
                ? (size_t)b * TPB * ITEMS + x
                : (size_t)b * TPB + x;
            for (; base < n; base += chunk) {
                for (int k = 0; k < ITEMS; ++k) {
                    const size_t idx = base + (size_t)k * base_stride;
                    if (idx < n) visit[idx]++;
                }
            }
        }
    }
    size_t missed = 0, dup = 0;
    for (size_t i = 0; i < n; ++i) {
        if (visit[i] == 0) ++missed;
        else if (visit[i] > 1) ++dup;
    }
    printf("  n=%zu init=%s  missed=%zu dup=%zu  -> %s\n",
           n, blocked_init ? "blocked(BUG)" : "striped(FIX)", missed, dup,
           (missed == 0 && dup == 0) ? "OK" : "BROKEN");
    return missed == 0 && dup == 0;
}

int main() {
    const int GRID = 1024, TPB = 256, ITEMS = 8; // exactly what the engine launches
    const size_t sizes[] = {65536, 262144, 1048576, 4194304, 67108864};

    printf("Buggy (blocked init) — expect BROKEN:\n");
    bool any_bug_broken = false;
    for (size_t n : sizes) any_bug_broken |= !coverage_ok(n, GRID, TPB, ITEMS, true);

    printf("Fixed (striped init) — expect OK:\n");
    bool all_fix_ok = true;
    for (size_t n : sizes) all_fix_ok &= coverage_ok(n, GRID, TPB, ITEMS, false);

    if (!any_bug_broken) { printf("FAIL: bug did not reproduce\n"); return 1; }
    if (!all_fix_ok)     { printf("FAIL: fix does not give exact coverage\n"); return 1; }
    printf("PASS: bug reproduced, fix gives exact coverage\n");
    return 0;
}

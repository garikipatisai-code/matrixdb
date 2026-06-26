# Live Analytical Query Demo Implementation Plan — QRY-2

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Surface the full analytical stack in the live `main` binary: load catalog columns into a RAM-constrained engine, run `execute_query` (filtered grouped + scalar), show auto-tiering hold >RAM, self-verify every result against a brute-force oracle.

**Spec:** `docs/superpowers/specs/2026-06-26-live-query-demo-design.md`

---

### Task 1: analytical_query_demo() in main.cpp

**Files:** Modify `main.cpp` (add the function after `routing_demo` ~line 191; call it near the end of `main` before the final "Engine execution loop completed successfully." print ~line 431).

- [ ] **Step 1: Add the demo function** — In `main.cpp`, insert after `routing_demo`'s closing brace (line 191), before `int main()`:

```cpp
// Demonstrates the full analytical stack live (not just in tests): load catalog columns into a
// RAM-constrained engine, run unified queries via execute_query, show auto-tiering hold a working
// set larger than RAM, and self-verify every result against an in-function brute-force oracle.
// Uses its own engine + catalog columns (id>0) — disjoint from the benchmark pipeline + scan oracle.
void analytical_query_demo() {
    const size_t N = 1u << 20;                 // 1M rows
    const uint32_t G = 4;                       // 4 groups
    const size_t S = N * sizeof(uint32_t);
    std::vector<uint32_t> keys(N), va(N), vb(N);
    for (size_t i = 0; i < N; ++i) {
        keys[i] = static_cast<uint32_t>(i % G);
        va[i]   = static_cast<uint32_t>(i % 1000);
        vb[i]   = static_cast<uint32_t>(i % 1000);
    }
    // Brute-force oracles (independent of the engine).
    std::vector<uint64_t> g_oracle(G, 0);
    uint64_t vb_sum = 0;
    for (size_t i = 0; i < N; ++i) { if (va[i] > 500) g_oracle[keys[i]] += va[i]; vb_sum += vb[i]; }

    CPUMockEngine demo(0, "", /*host_cap=*/2 * S);   // RAM holds 2 of the 3 columns
    demo.load_scan_column(1, keys.data(), N);        // key (group) column
    demo.load_scan_column(2, va.data(),  N);         // value column A
    demo.load_scan_column(3, vb.data(),  N);         // value column B

    std::cout << "\n=== Analytical query demo (tiered catalog + execute_query) ===" << std::endl;
    std::cout << "Loaded 3 columns (" << (3 * S) / (1024 * 1024) << " MB) into a "
              << (2 * S) / (1024 * 1024) << " MB RAM budget." << std::endl;

    // SELECT key, SUM(va) WHERE va > 500 GROUP BY key
    std::vector<uint64_t> grouped;
    demo.execute_query(MatrixQuery{.value_col = 2, .agg = AGG_SUM, .has_filter = true, .threshold = 500,
                                   .grouped = true, .key_col = 1, .num_groups = G}, grouped);
    assert(grouped == g_oracle && "grouped query result matches oracle");
    std::cout << "SELECT key, SUM(va) WHERE va>500 GROUP BY key:" << std::endl;
    for (uint32_t g = 0; g < G; ++g)
        std::cout << "   group " << g << " -> " << grouped[g] << std::endl;

    // Drive tiering: keep cols 1 & 2 hot (scalar scans), never touch col 3 -> col 3 (heat 0) is the
    // deterministic eviction victim once past MIN_RESIDENCY_TICKS.
    std::vector<uint64_t> scalar;
    for (int k = 0; k < 16; ++k) {
        demo.execute_query(MatrixQuery{.value_col = 1, .agg = AGG_COUNT}, scalar);
        demo.execute_query(MatrixQuery{.value_col = 2, .agg = AGG_COUNT}, scalar);
    }
    assert(demo.column_tier(3) == MemorySpace::COLD && "idle column auto-demoted to SSD");
    assert(demo.column_tier(1) == MemorySpace::HOST && demo.column_tier(2) == MemorySpace::HOST
           && "hot columns kept resident");

    const char* sp[] = {"HOST", "DEVICE", "COLD", "UNIFIED"};
    std::cout << "After a hot workload on cols 1 & 2, tier residency: col1="
              << sp[(int)demo.column_tier(1)] << " col2=" << sp[(int)demo.column_tier(2)]
              << " col3=" << sp[(int)demo.column_tier(3)]
              << "  (3-column working set in a 2-column RAM budget; col 3 auto-tiered to SSD)" << std::endl;

    // Query the demoted column -> pulled back to RAM, correct regardless of tier.
    demo.execute_query(MatrixQuery{.value_col = 3, .agg = AGG_SUM}, scalar);
    assert(scalar.size() == 1 && scalar[0] == vb_sum && "query over the SSD-resident column is correct");
    std::cout << "SELECT SUM(vb)  [col 3 was on SSD] -> " << scalar[0]
              << "  (pulled back to RAM, correct). Demo OK." << std::endl;
}
```

- [ ] **Step 2: Call it from main** — In `main.cpp`, add `analytical_query_demo();` on its own line immediately BEFORE the final `std::cout << "Engine execution loop completed successfully." << std::endl;` (near line 431).

- [ ] **Step 3: Build and run main; verify oracle unchanged + demo asserts pass** —

Run: `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/matrixdb_proto && /tmp/matrixdb_proto 2>&1 | grep -E "Scan result sum|Analytical query demo|group [0-9]|tier residency|Demo OK|Engine execution loop"`

Expected output contains:
- `Scan result sum: 83886070 (oracle 83886070)` (existing oracle UNCHANGED)
- `=== Analytical query demo (tiered catalog + execute_query) ===`
- four `group N -> ...` lines
- `tier residency: col1=HOST col2=HOST col3=COLD ...`
- `Demo OK.`
- `Engine execution loop completed successfully.`

The binary must exit 0 (all asserts — existing + the demo's grouped/scalar/tier asserts — pass). If the binary aborts on the `column_tier(3)==COLD` assert, the eviction timing is off — STOP and report BLOCKED with the printed tier residency (do NOT weaken the assert).

- [ ] **Step 4: Build with -Wall -Wextra (no new warnings from the demo)** — `clang++ -std=c++20 -O2 -Wall -Wextra main.cpp -o /tmp/mdb_w 2>&1 | grep -i "warning" | head` → expect no warnings attributable to `analytical_query_demo` (designated initializers, the `sp[(int)...]` cast, etc.).

- [ ] **Step 5: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add main.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: live analytical query demo in main — load → auto-tier → execute_query → verify (stack visible in the running binary)"
```

---

### Task 2: Regression suite + notebook

**Files:** Regenerate `matrixdb_colab.ipynb` (main.cpp's embedded copy).

- [ ] **Step 1: Full CPU unit suite** —
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by test_query; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 11× `PASS:`. If any fail, STOP / report BLOCKED.

- [ ] **Step 2: Regenerate the notebook** — `python3 make_notebook.py` → `wrote matrixdb_colab.ipynb: <N> cells, 27 source files embedded` (SOURCES unchanged; main.cpp's embedded copy refreshed with the demo).

- [ ] **Step 3: Verify the demo is embedded** — `grep -o "analytical_query_demo" matrixdb_colab.ipynb | wc -l` → `>= 1`.

- [ ] **Step 4: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: regenerate notebook with the live analytical query demo"
```

---

## Self-Review
**Spec coverage:** demo function (§1/§3)→T1S1; call site (§1)→T1S2; oracle-safe separate engine + run verification (§2/§4)→T1S3; unit suite + notebook→T2. ✓
**Placeholders:** none — full demo code + exact run command + expected output lines.
**Determinism:** the driver loop scans BOTH col1 and col2 each iteration so col3 (heat 0) is the unique lowest-keep_score victim — avoids a non-deterministic tie if col1 were left to decay. 32 scalar scans = 8 rebalances; col3 demotes by the 2nd (past MIN_RESIDENCY_TICKS=2).
**Type consistency:** `analytical_query_demo()` uses `CPUMockEngine`, `MatrixQuery{...}`, `execute_query`, `column_tier`, `load_scan_column`, `MemorySpace::HOST/COLD`, `AGG_SUM/AGG_COUNT` — all defined/included via `compute_mock.cpp` (always included by main.cpp).

# Filtered Grouped Aggregation Implementation Plan — GBY-2

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Add `WHERE value > threshold` to grouped aggregation (`matrix_cpu_group_reduce_where` + `CPUMockEngine::grouped_aggregate_where`), via a templated impl so the existing unfiltered path is byte-unchanged.

**Spec:** `docs/superpowers/specs/2026-06-26-filtered-group-by-design.md`

---

### Task 1: Filtered grouped reducer + engine method

**Files:** Modify `compute.hpp`, `compute_mock.cpp`, `test_group_by.cpp`.

- [ ] **Step 1: Write the failing tests** — In `test_group_by.cpp`, add above `main`:

```cpp
static void test_group_reduce_where() {
    const std::vector<uint32_t> keys = {0, 1, 0, 2, 1, 0};
    const std::vector<uint32_t> vals = {5, 7, 9, 11, 13, 15};
    const uint32_t G = 3;
    std::vector<uint64_t> out(G);
    // WHERE value > 8 -> kept: g0{9,15}, g1{13}, g2{11}
    matrix_cpu_group_reduce_where(keys.data(), vals.data(), keys.size(), G, AGG_COUNT, 8, out.data());
    assert((out == std::vector<uint64_t>{2, 1, 1}));
    matrix_cpu_group_reduce_where(keys.data(), vals.data(), keys.size(), G, AGG_SUM, 8, out.data());
    assert((out == std::vector<uint64_t>{24, 13, 11}));   // 9+15, 13, 11
    matrix_cpu_group_reduce_where(keys.data(), vals.data(), keys.size(), G, AGG_MIN, 8, out.data());
    assert((out == std::vector<uint64_t>{9, 13, 11}));
    matrix_cpu_group_reduce_where(keys.data(), vals.data(), keys.size(), G, AGG_MAX, 8, out.data());
    assert((out == std::vector<uint64_t>{15, 13, 11}));
    // non-vacuity: filtered SUM differs from unfiltered {29,20,11}
    matrix_cpu_group_reduce(keys.data(), vals.data(), keys.size(), G, AGG_SUM, out.data());
    assert((out == std::vector<uint64_t>{29, 20, 11}));   // unfiltered wrapper unchanged (regression guard)

    // WHERE value > 12 -> g2 emptied BY the filter (only row 11 dropped)
    matrix_cpu_group_reduce_where(keys.data(), vals.data(), keys.size(), G, AGG_COUNT, 12, out.data());
    assert((out == std::vector<uint64_t>{1, 1, 0}));      // g0{15}, g1{13}, g2{}
    matrix_cpu_group_reduce_where(keys.data(), vals.data(), keys.size(), G, AGG_MIN, 12, out.data());
    assert((out == std::vector<uint64_t>{15, 13, UINT64_MAX})); // emptied group -> MIN sentinel
    std::cout << "[group reduce WHERE] ok\n";
}

static void test_engine_group_by_where() {
    const std::vector<uint32_t> keys = {0, 1, 0, 2, 1, 0};
    const std::vector<uint32_t> vals = {5, 7, 9, 11, 13, 15};
    CPUMockEngine eng(0, "", /*host_cap=*/SIZE_MAX);
    eng.load_scan_column(1, keys.data(), keys.size());
    eng.load_scan_column(2, vals.data(), vals.size());
    std::vector<uint64_t> out;
    eng.grouped_aggregate_where(1, 2, /*num_groups=*/3, AGG_SUM, /*threshold=*/8, out);
    assert((out == std::vector<uint64_t>{24, 13, 11}));
    eng.grouped_aggregate_where(1, 2, 3, AGG_COUNT, 8, out);
    assert((out == std::vector<uint64_t>{2, 1, 1}));
    std::cout << "[engine group-by WHERE] ok\n";
}
```

Add `test_group_reduce_where();` and `test_engine_group_by_where();` to `main()` after `test_engine_group_by_cold();`.

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_group_by.cpp -o /tmp/tgby && /tmp/tgby` → FAIL to compile (`matrix_cpu_group_reduce_where` / `grouped_aggregate_where` undeclared).

- [ ] **Step 3: Refactor to a templated impl + add the filtered reducer** — In `compute.hpp`, REPLACE the existing `matrix_cpu_group_reduce` (from GBY-1) with the templated impl + two wrappers:

```cpp
// Grouped reduction core. Filtered==true applies WHERE value > threshold (compiled out when
// false via if constexpr, so the unfiltered path is byte-identical to the original). Dense groups
// [0, num_groups); keys >= num_groups ignored; out initialized per op (empty-group sentinels match
// matrix_cpu_reduce: COUNT/SUM/MAX -> 0, MIN -> UINT64_MAX). SUM accumulates in u64.
template <bool Filtered>
inline void matrix_group_reduce_impl(const uint32_t* keys, const uint32_t* values, size_t n,
                                     uint32_t num_groups, MatrixAggOp op, uint32_t threshold, uint64_t* out) {
    const uint64_t init = (op == AGG_MIN) ? UINT64_MAX : 0;
    for (uint32_t g = 0; g < num_groups; ++g) out[g] = init;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t k = keys[i];
        if (k >= num_groups) continue;                       // out-of-range key: ignored
        const uint32_t v = values[i];
        if constexpr (Filtered) { if (v <= threshold) continue; }  // WHERE value > threshold
        switch (op) {
            case AGG_SUM:   out[k] += v; break;
            case AGG_MIN:   if (v < out[k]) out[k] = v; break;
            case AGG_MAX:   if (v > out[k]) out[k] = v; break;
            case AGG_COUNT:
            default:        out[k] += 1; break;
        }
    }
}
// GROUP BY key (all rows). Unchanged signature from GBY-1 — now a thin wrapper.
inline void matrix_cpu_group_reduce(const uint32_t* keys, const uint32_t* values, size_t n,
                                    uint32_t num_groups, MatrixAggOp op, uint64_t* out) {
    matrix_group_reduce_impl<false>(keys, values, n, num_groups, op, /*threshold*/0, out);
}
// GROUP BY key WHERE value > threshold.
inline void matrix_cpu_group_reduce_where(const uint32_t* keys, const uint32_t* values, size_t n,
                                          uint32_t num_groups, MatrixAggOp op, uint32_t threshold, uint64_t* out) {
    matrix_group_reduce_impl<true>(keys, values, n, num_groups, op, threshold, out);
}
```

- [ ] **Step 4: Add grouped_aggregate_where** — In `compute_mock.cpp`, add right after `grouped_aggregate`:

```cpp
    // GROUP BY key WHERE value > threshold (filtered grouped aggregate). Same contract and double
    // borrow-and-return as grouped_aggregate; only rows with value > threshold contribute.
    void grouped_aggregate_where(uint64_t key_id, uint64_t value_id, uint32_t num_groups,
                                 MatrixAggOp op, uint32_t threshold, std::vector<uint64_t>& out) {
        assert(key_id != value_id && "group-by key and value must be distinct columns");
        TieredColumn& kc = *catalog_.at(key_id);
        TieredColumn& vc = *catalog_.at(value_id);
        assert(kc.size_bytes() == vc.size_bytes() && "key and value columns must be the same length");
        tier_mgr_.record_access(key_id, kc.size_bytes());
        tier_mgr_.record_access(value_id, vc.size_bytes());
        const MemorySpace kh = kc.tier(); if (kh != MemorySpace::HOST) kc.migrate_to(MemorySpace::HOST);
        const MemorySpace vh = vc.tier(); if (vh != MemorySpace::HOST) vc.migrate_to(MemorySpace::HOST);
        const uint32_t* keys = reinterpret_cast<const uint32_t*>(kc.host_ptr());
        const uint32_t* vals = reinterpret_cast<const uint32_t*>(vc.host_ptr());
        const size_t n = kc.size_bytes() / sizeof(uint32_t);
        out.resize(num_groups);
        matrix_cpu_group_reduce_where(keys, vals, n, num_groups, op, threshold, out.data());
        if (vh != MemorySpace::HOST) vc.migrate_to(vh);
        if (kh != MemorySpace::HOST) kc.migrate_to(kh);
    }
```

- [ ] **Step 5: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_group_by.cpp -o /tmp/tgby && /tmp/tgby` → PASS, prints `[group reduce WHERE] ok` + `[engine group-by WHERE] ok` + `ALL GROUP-BY TESTS PASSED`. No warnings.

- [ ] **Step 6: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add compute.hpp compute_mock.cpp test_group_by.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: filtered GROUP BY (WHERE value>threshold) — templated reducer + grouped_aggregate_where"
```

---

### Task 2: Regression + notebook

**Files:** Regenerate `matrixdb_colab.ipynb` (no SOURCES change — test_group_by.cpp already embedded).

- [ ] **Step 1: Full CPU suite + oracle**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t ($(tail -1 /tmp/out_$t))" || echo "FAIL: $t"
done
clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"
```
Expected: 10× `PASS:` + `Scan result sum: 83886070 (oracle 83886070)`. If any fail, STOP / report BLOCKED.

- [ ] **Step 2: Regenerate the notebook** — `python3 make_notebook.py` → `wrote matrixdb_colab.ipynb: <N> cells, 26 source files embedded` (the test grew in place; SOURCES unchanged).

- [ ] **Step 3: Verify the new test is embedded** — `grep -o "test_group_by_where\|group reduce WHERE" matrixdb_colab.ipynb | wc -l` → `>= 1`.

- [ ] **Step 4: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: regenerate notebook with filtered group-by tests; suite + oracle verified"
```

---

## Self-Review
**Spec coverage:** templated impl + 2 wrappers (§2)→T1S3; grouped_aggregate_where (§3)→T1S4; filtered + emptied-group + unfiltered-regression + engine + non-vacuity (§4)→T1S1; oracle/suite/notebook→T2. ✓
**Placeholders:** none. **Type consistency:** `matrix_group_reduce_impl<bool>`, `matrix_cpu_group_reduce_where(...,uint32_t threshold, uint64_t*)`, `grouped_aggregate_where(...,uint32_t threshold, std::vector<uint64_t>&)` consistent; `matrix_cpu_group_reduce` signature preserved (GBY-1 callers/tests unaffected).

# Design: Live Analytical Query Demo in main — QRY-2

**Status:** approved-by-standing-directive, pre-implementation. **Date:** 2026-06-26.
**Builds on:** QRY-1 (`execute_query`/`MatrixQuery`), INT-1/1b (tiered catalog), GBY/AGG.

**Thesis:** *Six increments of analytical capability (tiering, aggregates, group-by, query API)
run only in tests — the live `main` still only does point-ops + the legacy scan. Demonstrate the
whole stack in the running binary: load catalog columns into a RAM-constrained engine, run real
queries via `execute_query`, show auto-tiering hold a working set larger than RAM, and self-verify
every result against a brute-force oracle.*

---

## 1. Scope

**IN:**
- A free function `analytical_query_demo()` in `main.cpp`, called near the end of `main()` (after
  the existing pipeline + oracle, before `return 0`). Self-contained: builds its own
  `CPUMockEngine` with a small `host_cap` so it never touches the benchmark engines or the oracle.
- The demo: load 3 catalog columns (a key column + two value columns) into a 2-column RAM budget;
  run a filtered grouped query (`SELECT key, SUM(v) WHERE v>T GROUP BY key`) and a scalar query
  via `execute_query`; drive enough scalar scans that the idle third column auto-demotes to SSD
  (demonstrating "holds 3 columns in a 2-column budget"); then query the demoted column (pulled
  back, correct). Print results + per-column tier residency. **Assert every result against an
  in-function brute-force oracle** so the demo is a correctness check in the live binary, and
  assert the auto-demotion happened.

**OUT:** no new capability (this surfaces existing ones); no perf benchmarking of the analytical
path (CPU aggregates aren't the GPU perf thesis); GPU demo (Colab).

---

## 2. Oracle safety

The demo uses a **separate** `CPUMockEngine` instance and **catalog columns (id > 0)** — disjoint
from `main`'s point-op store, legacy scan column, and the `83886070` scan oracle (which is computed
and asserted before the demo runs). The demo only prints additional lines and runs its own asserts;
the existing oracle/benchmark output is unchanged. Works in both the CPU and CUDA builds (it uses
only `CPUMockEngine`, which `main` always constructs).

---

## 3. The demo (deterministic + self-verifying)

```
analytical_query_demo():
  N = 1<<20, G = 4; S = N*4 bytes
  keys[i]=i%G; va[i]=i%1000; vb[i]=i%1000   (vb a 2nd value column)
  CPUMockEngine demo(0, "", host_cap = 2*S)            // RAM holds 2 of 3 columns
  load col 1=keys, 2=va, 3=vb

  // headline: SELECT key, SUM(va) WHERE va>500 GROUP BY key
  execute_query({value_col=2, agg=SUM, has_filter, threshold=500, grouped, key_col=1, num_groups=G}, g)
  assert g == brute_grouped(keys, va, G, SUM, >500); print per group

  // drive tiering: repeated scalar scans of col 2 (hot); col 3 never queried -> demoted to SSD
  for k in 0..15: execute_query({value_col=2, agg=COUNT, has_filter, threshold=900}, s)
  assert demo.column_tier(3) == COLD   // engine holds 3 columns' working set in a 2-column RAM budget
  assert demo.column_tier(2) == HOST

  // query the demoted column -> pulled back, correct regardless of tier
  execute_query({value_col=3, agg=SUM}, s)             // unfiltered scalar over the COLD column
  assert s[0] == brute_all(vb, SUM)
  print tier residency of cols 1/2/3
```

`brute_grouped` / `brute_all` are tiny in-function reference loops (independent of the engine).
The 16 scalar scans drive ≥ 3 rebalances (REBALANCE_EVERY=4); with col 3 never accessed (heat 0)
and past MIN_RESIDENCY_TICKS, it is the deterministic eviction victim — so `column_tier(3)==COLD`
is reliable, not flaky. (If a future constant change breaks this, the assert fails loudly in `main`,
which is the desired signal.)

---

## 4. Verification
- `main` still prints `Scan result sum: 83886070 (oracle 83886070)` and all its existing asserts
  pass (the demo is additive, runs after, on a separate engine).
- The demo's own asserts (grouped == brute, scalar == brute, col 3 demoted, demoted-query correct)
  pass when `main` runs — i.e. running the binary verifies the analytical stack end-to-end.
- All 11 CPU unit tests stay green; notebook regenerated (main.cpp's embedded copy refreshed).

---

## 5. Open / deferred
- Perf metrics for the analytical path; a richer demo dataset; wiring the demo to the GPU engine
  (Colab); a CLI to run ad-hoc queries (needs the query layer + parsing, DM-4 full).

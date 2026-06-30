# matrixdb HAVING + top-N on join-aggregates Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. Steps use checkbox (`- [ ]`).

**Goal:** `SELECT agg(lcol) JOIN lk = rk GROUP BY rcol [HAVING agg <cmp> v | ORDER BY agg DESC LIMIT n]`.

**Architecture:** Fold `reduce_vals` into `reduce_agg` (returns `{text, order}`); the grouped join-agg path
builds `{label, text, order}` rows and post-processes (HAVING filter / stable_sort+resize). **Spec:**
`docs/superpowers/specs/2026-06-30-matrixdb-join-agg-having-topn-design.md`

**Tech Stack:** C++20, `std::stable_sort`, the existing join-agg buckets.

---

### Task 1: `reduce_agg` + HAVING/top-N post-processing

**Files:** Modify `matrix_cli.hpp` (`matrixcli_detail` helper + grouped join-agg path + `<algorithm>` +
`.help`); Test `test_cli.cpp`.

- [ ] **Step 1: failing test** — add to the join block in `test_cli.cpp` (reuses `je`):
```cpp
        std::ostringstream oh; std::istringstream ih(
            "SELECT SUM(ord_amt) JOIN ord_region = reg_key GROUP BY reg_name HAVING SUM > 100\n"       // south, east (north 30 out)
            "SELECT SUM(ord_amt) JOIN ord_region = reg_key GROUP BY reg_name ORDER BY SUM DESC LIMIT 2\n"  // east(950), south(900)
            "SELECT SUM(ord_amt) JOIN ord_region = reg_key GROUP BY reg_name ORDER BY SUM DESC LIMIT 1\n"  // east only
            "SELECT SUM(ord_amt) JOIN ord_region = reg_key GROUP BY reg_name HAVING\n"                  // malformed -> Error
            ".quit\n");
        matrix_repl(ih, oh, je); const std::string hh = oh.str();
        assert(has(hh, "south │ 900") && has(hh, "east │ 950") && !has(hh, "north │ 30"));   // HAVING
        assert(hh.substr(0, hh.find('\n')).find("east") != std::string::npos);               // top-N: largest first
        assert(has(hh, "Error:"));                                                           // malformed tail
        std::cout << "[cli join-agg having/topN] ok\n";
```
- [ ] **Step 2:** run → fails (the HAVING/ORDER tail trips the `ntail != 3` guard → "could not parse join GROUP BY").
- [ ] **Step 3: add `#include <algorithm>`** to `matrix_cli.hpp` (after `<limits>`).
- [ ] **Step 4: replace `reduce_vals` with `reduce_agg`** (returns text + a double sort key). Replace the whole
  `reduce_vals` function with:
```cpp
struct AggResult { std::string text; double order; };       // formatted value + a numeric sort/compare key
// Reduce gathered (encoded) values by an aggregate. text = type-correct display (u32 unsigned / i64 signed /
// f64); order = the same value as double for HAVING/ORDER BY. COUNT ignores type. Empty -> 0.
// ponytail: `order` is lossy above 2^53 for huge i64/u64 — fine for ranking what a CLI prints.
inline AggResult reduce_agg(CPUMockEngine& eng, uint64_t col, MatrixAggOp agg, const std::vector<uint64_t>& vals) {
    if (agg == AGG_COUNT) return { std::to_string(vals.size()), static_cast<double>(vals.size()) };
    const MatrixType ty = eng.column_type(col);
    if (ty == MatrixType::F64) {
        double acc = (agg == AGG_SUM) ? 0.0 : (agg == AGG_MIN ? std::numeric_limits<double>::max() : std::numeric_limits<double>::lowest());
        for (uint64_t v : vals) { const double d = matrix_bit_cast<double>(v);
            if (agg == AGG_SUM) acc += d; else if (agg == AGG_MIN) acc = d < acc ? d : acc; else acc = d > acc ? d : acc; }
        if (vals.empty()) acc = 0.0;
        std::ostringstream o; o << acc; return { o.str(), acc };
    }
    if (ty == MatrixType::I64) {
        int64_t acc = (agg == AGG_SUM) ? 0 : (agg == AGG_MIN ? std::numeric_limits<int64_t>::max() : std::numeric_limits<int64_t>::min());
        for (uint64_t v : vals) { const int64_t s = static_cast<int64_t>(v);
            if (agg == AGG_SUM) acc += s; else if (agg == AGG_MIN) acc = s < acc ? s : acc; else acc = s > acc ? s : acc; }
        if (vals.empty()) acc = 0;
        return { std::to_string(acc), static_cast<double>(acc) };
    }
    uint64_t acc = (agg == AGG_SUM) ? 0 : (agg == AGG_MIN ? std::numeric_limits<uint64_t>::max() : 0);   // u32
    for (uint64_t v : vals) { const uint64_t u = v & 0xffffffffULL;
        if (agg == AGG_SUM) acc += u; else if (agg == AGG_MIN) acc = u < acc ? u : acc; else acc = u > acc ? u : acc; }
    if (vals.empty()) acc = 0;
    return { std::to_string(acc), static_cast<double>(acc) };
}
```
- [ ] **Step 5: rewrite the grouped join-agg tail + output.** Replace the GROUP BY parse block and the
  grouped print loop (from `uint64_t gcol = 0;` through the grouped `return;`) with:
```cpp
            uint64_t gcol = 0;                              // GROUP BY <right dimension> [HAVING .. | ORDER BY .. LIMIT ..]
            enum { POST_NONE, POST_HAVING, POST_TOPN } post = POST_NONE;
            std::string hop; double hthr = 0; size_t topn = 0;
            if (ntail > 0) {
                if (ntail < 3 || upper(tk[ji + 4]) != "GROUP" || upper(tk[ji + 5]) != "BY") { out << "Error: could not parse join GROUP BY (... GROUP BY rcol)\n"; return; }
                gcol = eng.column_id(tk[ji + 6]);
                if (gcol == 0) { out << "Error: unknown GROUP BY column in join\n"; return; }
                if (ntail == 3) { /* plain grouped */ }
                else if (ntail == 7 && upper(tk[ji + 7]) == "HAVING") {
                    hop = tk[ji + 9]; hthr = std::strtod(tk[ji + 10].c_str(), nullptr); post = POST_HAVING;
                    if (hop != ">" && hop != ">=" && hop != "<" && hop != "<=" && hop != "=" && hop != "!=") { out << "Error: bad HAVING comparison (use > >= < <= = !=)\n"; return; }
                }
                else if (ntail == 9 && upper(tk[ji + 7]) == "ORDER" && upper(tk[ji + 8]) == "BY" && upper(tk[ji + 11]) == "LIMIT") {
                    topn = static_cast<size_t>(std::strtoull(tk[ji + 12].c_str(), nullptr, 10)); post = POST_TOPN;
                }
                else { out << "Error: could not parse join GROUP BY clause (HAVING agg <cmp> v | ORDER BY agg DESC LIMIT n)\n"; return; }
            }
            const auto pr = (kt == MatrixType::I64) ? eng.hash_join_i64(lkey, rkey) : eng.hash_join(lkey, rkey);
            std::vector<uint64_t> lrows, rrows; lrows.reserve(pr.size()); rrows.reserve(pr.size());
            for (const auto& p : pr) { lrows.push_back(p.first); rrows.push_back(p.second); }
            const std::vector<uint64_t> lv = eng.gather(lcol, lrows);
            if (gcol == 0) { out << reduce_agg(eng, lcol, agg, lv).text << "\n"; return; }   // scalar over the whole join
            const std::vector<uint64_t> gv = eng.gather(gcol, rrows);                        // grouped by the right dimension
            std::map<uint64_t, std::vector<uint64_t>> buckets;
            for (size_t i = 0; i < lv.size(); ++i) buckets[gv[i]].push_back(lv[i]);
            struct GRow { std::string label, text; double order; };
            std::vector<GRow> grows;
            for (const auto& kv : buckets) {
                const std::string label = eng.string_dict_size(gcol) > 0 ? eng.string_decode(gcol, static_cast<uint32_t>(kv.first)) : std::to_string(kv.first);
                const AggResult ar = reduce_agg(eng, lcol, agg, kv.second);
                grows.push_back({ label, ar.text, ar.order });
            }
            if (post == POST_HAVING) {
                std::vector<GRow> kept;
                for (const auto& r : grows) {
                    const double x = r.order; bool ok = false;
                    if      (hop == ">")  ok = x >  hthr; else if (hop == ">=") ok = x >= hthr;
                    else if (hop == "<")  ok = x <  hthr; else if (hop == "<=") ok = x <= hthr;
                    else if (hop == "=")  ok = x == hthr; else                  ok = x != hthr;
                    if (ok) kept.push_back(r);
                }
                grows.swap(kept);
            } else if (post == POST_TOPN) {
                std::stable_sort(grows.begin(), grows.end(), [](const GRow& a, const GRow& b){ return a.order > b.order; });
                if (grows.size() > topn) grows.resize(topn);
            }
            for (const auto& r : grows) out << r.label << " │ " << r.text << "\n";
            return;
```
- [ ] **Step 6: `.help`** — extend the join-aggregate help line:
```cpp
                   "          SELECT agg(lcol) JOIN lkey = rkey [GROUP BY rcol [HAVING agg <op> v | ORDER BY agg DESC LIMIT n]]\n";
```
- [ ] **Step 7:** `clang++ -std=c++20 -O1 -fsanitize=address,undefined test_cli.cpp -o /tmp/tcli && /tmp/tcli` → `[cli join-agg having/topN] ok`.
- [ ] **Step 8:** `./run_tests.sh` → 65 + oracle + demo green.
- [ ] **Step 9: commit** — `git commit -am "feat(cli): HAVING + top-N on join-aggregates (rank/filter per dimension)"`

---

### Task 2: demo + docs

**Files:** `examples/tour.sql`, `README.md`, `make_notebook.py`, memory.

- [ ] **Step 1:** add to `examples/tour.sql` after the star-query line:
  `SELECT SUM(amount) JOIN region = reg_key GROUP BY reg_name ORDER BY SUM DESC LIMIT 2` (top 2 regions).
- [ ] **Step 2:** README CLI join-aggregate sentence — add `[GROUP BY rcol [HAVING … | ORDER BY agg DESC LIMIT n]]`.
- [ ] **Step 3:** regenerate notebook; update memory `matrixdb-state.md`.
- [ ] **Step 4:** `./run_tests.sh` + `SAN=1 ./run_tests.sh` green.
- [ ] **Step 5: commit** — `git commit -am "docs(join-agg): tour top-N + README + notebook"`

---

## Self-Review

**Spec coverage:** HAVING filter + top-N (DESC) on grouped join-agg (T1) ✓; `reduce_agg` returns text+order
(T1) ✓; mutually-exclusive one-clause grammar via token-count dispatch (T1) ✓; malformed-tail error (T1
test) ✓; demo + docs (T2) ✓; non-goals (ASC, HAVING+ORDER together) untouched ✓.

**Placeholder scan:** none — full code, all branches.

**Type consistency:** `reduce_agg` returns `AggResult{std::string,double}`; both call sites updated (scalar
`.text`, grouped via `GRow`); `std::stable_sort` needs `<algorithm>`; `strtod`/`strtoull` from `<cstdlib>`
(already included); `ntail`/`ji`/`tk`/`kt`/`lcol`/`agg` are the existing join-branch locals.

# matrixdb aggregates-over-a-join Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. Steps use checkbox (`- [ ]`).

**Goal:** `SELECT agg(lcol) JOIN lk = rk [GROUP BY rcol]` — aggregate a left measure over the join, optionally
grouped by a right dimension (decoded labels).

**Architecture:** Extend the existing JOIN branch in `matrix_cli_run_sql` to dispatch on the SELECT list
(`COUNT(*)` → cardinality; `(` → aggregate; `,` → projection). Add a type-aware `reduce_vals` helper (no
engine API reduces a row subset). **Spec:** `docs/superpowers/specs/2026-06-30-matrixdb-join-aggregates-design.md`

**Tech Stack:** C++20, `hash_join`/`hash_join_i64`/`gather`, `std::map`, `std::numeric_limits`.

---

### Task 1: `reduce_vals` + the aggregate-over-join dispatch

**Files:** Modify `matrix_cli.hpp` (`matrixcli_detail` + JOIN branch + includes + `.help`); Test `test_cli.cpp`.

- [ ] **Step 1: failing test** — add to the existing join block in `test_cli.cpp` (reuses `je`):
```cpp
        std::ostringstream oa; std::istringstream ia(
            "SELECT SUM(ord_amt) JOIN ord_region = reg_key\n"                       // 10+900+20+950 = 1880
            "SELECT SUM(ord_amt) JOIN ord_region = reg_key GROUP BY reg_name\n"     // north 30, south 900, east 950
            "SELECT COUNT(ord_amt) JOIN ord_region = reg_key GROUP BY reg_name\n"   // north 2, south 1, east 1
            "SELECT MAX(ord_amt) JOIN ord_region = reg_key\n"                       // 950
            "SELECT AVG(ord_amt) JOIN ord_region = reg_key\n"                       // AVG unsupported -> Error
            ".quit\n");
        matrix_repl(ia, oa, je); const std::string a = oa.str();
        assert(has(a, "1880") && has(a, "950"));
        assert(has(a, "north │ 30") && has(a, "south │ 900") && has(a, "east │ 950"));   // grouped SUM, decoded labels
        assert(has(a, "north │ 2"));                                                      // grouped COUNT
        assert(has(a, "Error:"));                                                         // AVG over join
        std::cout << "[cli join-agg] ok\n";
```
- [ ] **Step 2:** run → fails (these route into the JOIN branch's projection sub-path → "needs two columns" / unknown col).
- [ ] **Step 3: add includes** to `matrix_cli.hpp`: `#include <map>` and `#include <limits>`.
- [ ] **Step 4: add `reduce_vals`** in `matrixcli_detail` (right after `decode_proj`):
```cpp
// Reduce gathered (encoded) values by an aggregate, formatted by the column's type. No engine API reduces an
// arbitrary row subset, so the join-aggregate path reduces here. COUNT ignores type; SUM/MIN/MAX are typed.
inline std::string reduce_vals(CPUMockEngine& eng, uint64_t col, MatrixAggOp agg, const std::vector<uint64_t>& vals) {
    if (agg == AGG_COUNT) return std::to_string(vals.size());
    const MatrixType ty = eng.column_type(col);
    if (ty == MatrixType::F64) {
        double acc = (agg == AGG_SUM) ? 0.0 : (agg == AGG_MIN ? std::numeric_limits<double>::max() : std::numeric_limits<double>::lowest());
        for (uint64_t v : vals) { const double d = matrix_bit_cast<double>(v);
            if (agg == AGG_SUM) acc += d; else if (agg == AGG_MIN) acc = d < acc ? d : acc; else acc = d > acc ? d : acc; }
        std::ostringstream o; o << (vals.empty() ? 0.0 : acc); return o.str();
    }
    if (ty == MatrixType::I64) {
        int64_t acc = (agg == AGG_SUM) ? 0 : (agg == AGG_MIN ? std::numeric_limits<int64_t>::max() : std::numeric_limits<int64_t>::min());
        for (uint64_t v : vals) { const int64_t s = static_cast<int64_t>(v);
            if (agg == AGG_SUM) acc += s; else if (agg == AGG_MIN) acc = s < acc ? s : acc; else acc = s > acc ? s : acc; }
        return std::to_string(vals.empty() ? int64_t{0} : acc);
    }
    uint64_t acc = (agg == AGG_SUM) ? 0 : (agg == AGG_MIN ? std::numeric_limits<uint64_t>::max() : 0);   // u32
    for (uint64_t v : vals) { const uint64_t u = v & 0xffffffffULL;
        if (agg == AGG_SUM) acc += u; else if (agg == AGG_MIN) acc = u < acc ? u : acc; else acc = u > acc ? u : acc; }
    return std::to_string(vals.empty() ? uint64_t{0} : acc);
}
```
- [ ] **Step 5: restructure the JOIN branch** — replace the body after the key-type checks (the `uint64_t
  limit = 0; … projection` part) with form dispatch. Full replacement (from `uint64_t limit = 0;` through the
  projection `return;`):
```cpp
        const size_t ntail = tk.size() - (ji + 4);          // # tokens after rkey (>=0; ji+3 < size guaranteed)
        std::string sel; for (size_t i = 1; i < ji; ++i) { if (i > 1) sel += ' '; sel += tk[i]; }   // the SELECT list

        if (upper(sel).find("COUNT") != std::string::npos && sel.find('*') != std::string::npos) {  // COUNT(*) cardinality
            out << eng.hash_join_count(lkey, rkey) << "\n"; return;
        }
        const size_t lp = sel.find('(');
        if (lp != std::string::npos) {                      // aggregate over the join: AGG(lcol) [GROUP BY rcol]
            const size_t rp = sel.find(')');
            if (rp == std::string::npos || rp < lp) { out << "Error: could not parse join aggregate (SELECT AGG(col) JOIN ...)\n"; return; }
            const std::string aggname = upper(trim(sel.substr(0, lp)));
            MatrixAggOp agg;
            if      (aggname == "COUNT") agg = AGG_COUNT;  else if (aggname == "SUM") agg = AGG_SUM;
            else if (aggname == "MIN")   agg = AGG_MIN;    else if (aggname == "MAX") agg = AGG_MAX;
            else { out << "Error: join aggregate supports COUNT/SUM/MIN/MAX (not " << aggname << ")\n"; return; }
            const uint64_t lcol = eng.column_id(trim(sel.substr(lp + 1, rp - lp - 1)));
            if (lcol == 0) { out << "Error: unknown aggregate column in join\n"; return; }
            uint64_t gcol = 0;                              // optional GROUP BY <right dimension>
            if (ntail > 0) {
                if (ntail != 3 || upper(tk[ji + 4]) != "GROUP" || upper(tk[ji + 5]) != "BY") { out << "Error: could not parse join GROUP BY (... GROUP BY rcol)\n"; return; }
                gcol = eng.column_id(tk[ji + 6]);
                if (gcol == 0) { out << "Error: unknown GROUP BY column in join\n"; return; }
            }
            const auto pr = (kt == MatrixType::I64) ? eng.hash_join_i64(lkey, rkey) : eng.hash_join(lkey, rkey);
            std::vector<uint64_t> lrows, rrows; lrows.reserve(pr.size()); rrows.reserve(pr.size());
            for (const auto& p : pr) { lrows.push_back(p.first); rrows.push_back(p.second); }
            const std::vector<uint64_t> lv = eng.gather(lcol, lrows);
            if (gcol == 0) { out << reduce_vals(eng, lcol, agg, lv) << "\n"; return; }   // scalar over the join
            const std::vector<uint64_t> gv = eng.gather(gcol, rrows);                    // grouped by right dimension
            std::map<uint64_t, std::vector<uint64_t>> buckets;
            for (size_t i = 0; i < lv.size(); ++i) buckets[gv[i]].push_back(lv[i]);
            for (const auto& kv : buckets) {
                const std::string label = eng.string_dict_size(gcol) > 0 ? eng.string_decode(gcol, static_cast<uint32_t>(kv.first)) : std::to_string(kv.first);
                out << label << " │ " << reduce_vals(eng, lcol, agg, kv.second) << "\n";
            }
            return;
        }
        uint64_t limit = 0;                                 // projection: SELECT lcol, rcol [LIMIT n]
        if (ntail > 0) {
            if (ntail != 2 || upper(tk[ji + 4]) != "LIMIT") { out << "Error: could not parse join (trailing tokens)\n"; return; }
            limit = std::strtoull(tk[ji + 5].c_str(), nullptr, 10);
        }
        const size_t comma = sel.find(',');
        if (comma == std::string::npos) { out << "Error: join projection needs two columns: SELECT lcol, rcol JOIN ...\n"; return; }
        const uint64_t lcol = eng.column_id(trim(sel.substr(0, comma))), rcol = eng.column_id(trim(sel.substr(comma + 1)));
        if (lcol == 0 || rcol == 0) { out << "Error: unknown projected column in join\n"; return; }
        const auto pr = (kt == MatrixType::I64) ? eng.hash_join_i64(lkey, rkey) : eng.hash_join(lkey, rkey);
        const size_t cap = limit ? static_cast<size_t>(limit) : 100;
        std::vector<uint64_t> lrows, rrows;
        for (size_t i = 0; i < pr.size() && i < cap; ++i) { lrows.push_back(pr[i].first); rrows.push_back(pr[i].second); }
        const std::vector<uint64_t> lv = eng.gather(lcol, lrows), rv = eng.gather(rcol, rrows);
        for (size_t i = 0; i < lv.size(); ++i) out << decode_proj(eng, lcol, lv[i]) << " │ " << decode_proj(eng, rcol, rv[i]) << "\n";
        if (pr.size() > cap) out << "… (" << pr.size() << " matches, showing " << cap << ")\n";
        return;
```
  (This removes the old upfront `limit` parse — `ntail` now drives form-specific tails. Keep the lines
  *above* this block — the JOIN-token find + key resolution + type checks — unchanged.)
- [ ] **Step 6: `.help`** — update the joins line to show the aggregate form:
```cpp
                   "joins:    SELECT lcol, rcol JOIN lkey = rkey [LIMIT n]  |  SELECT COUNT(*) JOIN lkey = rkey\n"
                   "          SELECT agg(lcol) JOIN lkey = rkey [GROUP BY rcol]   (aggregate a left measure by a right dimension)\n";
```
- [ ] **Step 7:** `clang++ -std=c++20 -O1 -fsanitize=address,undefined test_cli.cpp -o /tmp/tcli && /tmp/tcli` → `[cli join-agg] ok`.
- [ ] **Step 8:** `./run_tests.sh` → 65 + oracle + demo green.
- [ ] **Step 9: commit** — `git commit -am "feat(cli): aggregates over a join — SELECT agg(lcol) JOIN lk=rk [GROUP BY rcol]"`

---

### Task 2: demo + docs

**Files:** `examples/tour.sql`, `README.md`, `make_notebook.py`, memory.

- [ ] **Step 1:** add to `examples/tour.sql` after the join lines: `SELECT SUM(amount) JOIN region = reg_key GROUP BY reg_name` (the star query — total amount per region name).
- [ ] **Step 2:** README CLI join sentence — add `… , and aggregate-over-join `SELECT agg(lcol) JOIN lk = rk [GROUP BY rcol]``.
- [ ] **Step 3:** regenerate notebook (`python3 make_notebook.py`); update memory `matrixdb-state.md`.
- [ ] **Step 4:** `./run_tests.sh` + `SAN=1 ./run_tests.sh` green.
- [ ] **Step 5: commit** — `git commit -am "docs(join-agg): tour + README + notebook"`

---

## Self-Review

**Spec coverage:** scalar + grouped aggregate-over-join (T1) ✓; `reduce_vals` type-aware (T1) ✓; AVG/bad
errors (T1 test) ✓; GROUP-BY-right dimension w/ decoded labels (T1) ✓; demo + docs (T2) ✓; non-goals
(right-measure / AVG / HAVING-on-join) untouched ✓.

**Placeholder scan:** none — full code given; `reduce_vals` handles empty + all three types.

**Type consistency:** `reduce_vals(eng, col, MatrixAggOp, vals)` ↔ the enum from `compute.hpp`
(`AGG_COUNT/SUM/MIN/MAX`); `hash_join`/`hash_join_i64` → `vector<pair<u64,u64>>`; `gather` → `vector<u64>`;
JOIN-branch keeps the existing `kt`/`lkey`/`rkey`/`ji`/`tk` locals. `ntail = tk.size()-(ji+4)` is safe
(`ji+3 < tk.size()` already guaranteed by the earlier guard).

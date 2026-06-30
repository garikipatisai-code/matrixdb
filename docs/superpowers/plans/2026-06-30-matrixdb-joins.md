# matrixdb SQL joins Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. Steps use checkbox (`- [ ]`).

**Goal:** Add a `JOIN` branch to the REPL: `SELECT lcol, rcol JOIN lk = rk [LIMIT n]` and `SELECT COUNT(*)
JOIN lk = rk`, over `hash_join`/`hash_join_i64`/`hash_join_count` + `gather`.

**Architecture:** One new branch in `matrix_cli_run_sql` (checked first — `JOIN` is unambiguous), reusing v1
`decode_proj`. Key-type rules reject f64/mismatched/dict-string keys. **Spec:**
`docs/superpowers/specs/2026-06-30-matrixdb-joins-design.md`

**Tech Stack:** C++20, `hash_join`/`hash_join_i64`/`hash_join_count`/`gather`, `split_ws`.

---

### Task 1: the `JOIN` router branch

**Files:** Modify `matrix_cli.hpp` (`matrix_cli_run_sql` head + `.help`); Test `test_cli.cpp`.

- [ ] **Step 1: failing test** — add a join block to `test_cli.cpp` (fresh engine, 4 columns loaded directly):
```cpp
    {   // SQL join: orders (region-idx + amount) ⋈ regions (key + name)
        CPUMockEngine je;
        std::vector<uint32_t> oreg{0,1,0,2}, oamt{10,900,20,950}, rkey{0,1,2};
        je.load_scan_column(1, oreg.data(), oreg.size()); je.name_column(1, "ord_region");
        je.load_scan_column(2, oamt.data(), oamt.size()); je.name_column(2, "ord_amt");
        je.load_scan_column(3, rkey.data(), rkey.size()); je.name_column(3, "reg_key");
        je.load_string_column_dict(4, {"north","south","east"}); je.name_column(4, "reg_name");
        std::ostringstream o; std::istringstream i(
            "SELECT ord_amt, reg_name JOIN ord_region = reg_key\n"
            "SELECT COUNT(*) JOIN ord_region = reg_key\n"
            "SELECT ord_amt, reg_name JOIN ord_region = reg_name\n"   // dict-string key -> Error
            "SELECT ord_amt JOIN ord_region = reg_key\n"              // malformed (need 2 cols) -> Error
            ".quit\n");
        matrix_repl(i, o, je); const std::string s = o.str();
        assert(has(s, "10 │ north") && has(s, "900 │ south") && has(s, "20 │ north") && has(s, "950 │ east"));
        assert(has(s, "\n4\n") || s.find("4\n") != std::string::npos);   // COUNT(*) == 4
        assert(s.find("Error:") != std::string::npos);                   // the two bad lines errored
        std::cout << "[cli join] ok\n";
    }
```
- [ ] **Step 2:** run → fails (`JOIN` lines hit projection path → unknown column → Error for all; `10 │ north` absent).
- [ ] **Step 3: implement** — add as the **first** branch in `matrix_cli_run_sql` (right after `const std::string U = upper(line);`):
```cpp
    if (U.find(" JOIN ") != std::string::npos) {
        const std::vector<std::string> tk = split_ws(line);
        size_t ji = tk.size();
        for (size_t i = 0; i < tk.size(); ++i) if (upper(tk[i]) == "JOIN") { ji = i; break; }
        if (ji + 3 >= tk.size() || tk[ji + 2] != "=") { out << "Error: could not parse join (SELECT a, b JOIN lk = rk [LIMIT n])\n"; return; }
        const uint64_t lkey = eng.column_id(tk[ji + 1]), rkey = eng.column_id(tk[ji + 3]);
        if (lkey == 0 || rkey == 0) { out << "Error: unknown column in join\n"; return; }
        if (eng.string_dict_size(lkey) > 0 || eng.string_dict_size(rkey) > 0) { out << "Error: cannot join on string-dictionary keys (independent code spaces)\n"; return; }
        const MatrixType kt = eng.column_type(lkey);
        if (kt != eng.column_type(rkey) || kt == MatrixType::F64) { out << "Error: join keys must be matching u32 or i64 columns\n"; return; }
        uint64_t limit = 0;                                   // optional LIMIT n tail
        if (ji + 4 < tk.size()) {
            if (ji + 6 != tk.size() || upper(tk[ji + 4]) != "LIMIT") { out << "Error: could not parse join (trailing tokens)\n"; return; }
            limit = std::strtoull(tk[ji + 5].c_str(), nullptr, 10);
        }
        // select list = tokens [1, ji) joined, split on comma
        std::string sel; for (size_t i = 1; i < ji; ++i) { if (i > 1) sel += ' '; sel += tk[i]; }
        if (upper(sel).find("COUNT") != std::string::npos && sel.find('*') != std::string::npos) {
            out << (kt == MatrixType::I64 ? eng.hash_join_count(lkey, rkey) : eng.hash_join_count(lkey, rkey)) << "\n";
            return;
        }
        const size_t comma = sel.find(',');
        if (comma == std::string::npos) { out << "Error: join projection needs two columns: SELECT lcol, rcol JOIN ...\n"; return; }
        const uint64_t lcol = eng.column_id(trim(sel.substr(0, comma))), rcol = eng.column_id(trim(sel.substr(comma + 1)));
        if (lcol == 0 || rcol == 0) { out << "Error: unknown projected column in join\n"; return; }
        const auto pairs = (kt == MatrixType::I64) ? eng.hash_join_i64(lkey, rkey) : eng.hash_join(lkey, rkey);
        const size_t cap = limit ? limit : 100;
        std::vector<uint64_t> lrows, rrows;
        for (size_t i = 0; i < pairs.size() && i < cap; ++i) { lrows.push_back(pairs[i].first); rrows.push_back(pairs[i].second); }
        const std::vector<uint64_t> lv = eng.gather(lcol, lrows), rv = eng.gather(rcol, rrows);
        for (size_t i = 0; i < lv.size(); ++i) out << decode_proj(eng, lcol, lv[i]) << " │ " << decode_proj(eng, rcol, rv[i]) << "\n";
        if (pairs.size() > cap) out << "… (" << pairs.size() << " matches, showing " << cap << ")\n";
        return;
    }
```
  (Add `#include <cstdlib>` for `strtoull` if not present. The `kt==I64?count:count` ternary is intentional —
  `hash_join_count` is type-agnostic on the key bytes; written plainly as `eng.hash_join_count(lkey, rkey)`.)
- [ ] **Step 4:** simplify the COUNT(*) line to just `out << eng.hash_join_count(lkey, rkey) << "\n";` (drop
  the no-op ternary). Run test → `[cli join] ok`.
- [ ] **Step 5: update `.help`** — append a join line to the queries help:
```cpp
                   "joins:    SELECT lcol, rcol JOIN lkey = rkey [LIMIT n]  |  SELECT COUNT(*) JOIN lkey = rkey\n";
```
- [ ] **Step 6:** `clang++ -std=c++20 -O1 -fsanitize=address,undefined test_cli.cpp -o /tmp/tcli && /tmp/tcli`
  → green; `./run_tests.sh` → 65 green.
- [ ] **Step 7: commit** — `git commit -am "feat(cli): SQL equi-join — SELECT a,b JOIN lk=rk + COUNT(*)"`

---

### Task 2: docs sync

**Files:** Modify `README.md` (CLI section: add the join form), `make_notebook.py` is auto (re-embeds
`matrix_cli.hpp`); regenerate notebook; update memory.

- [ ] **Step 1:** add the join form to the README query list + a one-line example.
- [ ] **Step 2:** `python3 make_notebook.py` (re-embeds updated source).
- [ ] **Step 3:** update memory `matrixdb-state.md` (joins landed; deferred list).
- [ ] **Step 4: commit** — `git commit -am "docs(joins): README + notebook + memory"`

---

## Self-Review

**Spec coverage:** projection form (positional left/right) ✓; COUNT(*) ✓; key-type rules (f64/mismatch/dict
reject) ✓; unknown-column + malformed errors ✓; LIMIT/cap ✓; decoded output via `decode_proj` ✓; first-branch
placement ✓; tests incl. error paths ✓; docs (T2) ✓.

**Placeholder scan:** none — the COUNT(*) ternary is flagged for simplification in Step 4 (concrete).

**Type consistency:** `hash_join`/`hash_join_i64` → `std::vector<std::pair<uint64_t,uint64_t>>`;
`hash_join_count` → `uint64_t`; `gather(col, rows)` → `std::vector<uint64_t>`; `load_scan_column(id, ptr, n)`
+ `load_string_column_dict(id, vec)` + `name_column(id, name)` are the engine signatures used in the test;
`decode_proj`/`trim`/`split_ws`/`upper` are the v1 `matrixcli_detail` helpers (in scope via the `using`).

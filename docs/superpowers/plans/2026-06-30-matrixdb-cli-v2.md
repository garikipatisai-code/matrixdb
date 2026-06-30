# matrixdb CLI v2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. Steps use checkbox (`- [ ]`).

**Goal:** Add `.save`/`.open` + route multi-agg / top-N / HAVING / COUNT(DISTINCT) into the REPL.

**Architecture:** Router + formatter additions in `matrix_cli.hpp`; two dot-commands; all over existing
public engine helpers. TDD via `test_cli.cpp`. **Spec:** `docs/superpowers/specs/2026-06-30-matrixdb-cli-v2-design.md`

**Tech Stack:** C++20, `save_catalog`/`load_catalog`, `query_multi`/`top_query`/`having_query`/`distinct_query`.

---

### Task 1: `.save` / `.open` (guarded persistence)

**Files:** Modify `matrix_cli.hpp` (dot-command block + a post-`.open` `next_id` fix); Test `test_cli.cpp`.

- [ ] **Step 1: failing test** — add a persistence block to `test_cli.cpp`:
```cpp
    {   // .save then .open into a fresh engine round-trips; missing file errors (no abort)
        std::istringstream in1(".load " + csv + " amount u32 col0 header\n.save /tmp/mdb_cli_v2.db\n.quit\n");
        std::ostringstream o1; CPUMockEngine e1; matrix_repl(in1, o1, e1);
        std::istringstream in2(".open /tmp/mdb_cli_v2.db\n.columns\nSELECT SUM(amount)\n.open /tmp/nope_x.db\n.quit\n");
        std::ostringstream o2; CPUMockEngine e2; const int rc = matrix_repl(in2, o2, e2);
        const std::string s = o2.str();
        assert(rc == 0 && has(s, "amount") && has(s, "1880") && has(s, "Error:"));
        std::cout << "[cli persist] ok\n";
    }
```
- [ ] **Step 2:** run → fails (`.save`/`.open` unknown commands → "Error: unknown command", so `1880` absent).
- [ ] **Step 3: implement** — add to the dot-command chain in `matrix_repl`, before the final `else`:
```cpp
        else if (cmd == ".save") {
            if (tk.size() < 2) { out << "Error: usage: .save <file>\n"; continue; }
            if (!std::ofstream(tk[1]).good()) { out << "Error: cannot write " << tk[1] << "\n"; continue; }
            eng.save_catalog(tk[1]);  // ponytail: corrupt-path abort guarded above; engine still abort()s on a bad reopen
            out << "saved catalog to " << tk[1] << "\n";
        }
        else if (cmd == ".open") {
            if (tk.size() < 2) { out << "Error: usage: .open <file>\n"; continue; }
            if (!std::ifstream(tk[1]).good()) { out << "Error: cannot open " << tk[1] << "\n"; continue; }
            eng.load_catalog(tk[1]);  // ponytail: a corrupt snapshot still abort()s inside load_catalog (CRC/short read)
            for (const ColumnInfo& c : eng.catalog_columns()) if (c.id >= next_id) next_id = c.id + 1;  // avoid id collision
            out << "opened " << tk[1] << " (" << eng.catalog_columns().size() << " columns)\n";
        }
```
  (Add `#include <fstream>` to `matrix_cli.hpp`.)
- [ ] **Step 4:** run test → `[cli persist] ok`; then `./run_tests.sh` green.
- [ ] **Step 5: commit** — `git commit -am "feat(cli): .save/.open session persistence (guarded against abort)"`

---

### Task 2: COUNT(DISTINCT) + HAVING + top-N

**Files:** Modify `matrix_cli.hpp` (`matrix_cli_run_sql`); Test `test_cli.cpp`.

- [ ] **Step 1: failing test** — add to the query block:
```cpp
        "SELECT COUNT(DISTINCT region)\n"                              // -> 3
        "SELECT SUM(amount) GROUP BY region HAVING SUM > 100\n"        // -> games 900, music 950 (books 30 excluded)
        "SELECT SUM(amount) GROUP BY region ORDER BY SUM DESC LIMIT 2\n"  // -> music 950, games 900 (desc)
```
  asserts: `has(r,"3")`; HAVING output has `games`/`music` but a books line `"books │ 30"` is absent;
  the top-N output's first data line is `music` (highest). (Add focused sub-asserts.)
- [ ] **Step 2:** run → fails (these route to the generic `(` path: DISTINCT/HAVING parse-fail → "Error").
- [ ] **Step 3: implement** — in `matrix_cli_run_sql`, add the helper + branches **before** the AVG check.
  `decode_pairs` formats `(group,value)` pairs from a parsed `q`:
```cpp
    const std::string U = matrixcli_detail::upper(line);
    // COUNT(DISTINCT col)
    if (U.find("DISTINCT") != std::string::npos) {
        uint64_t n = 0;
        if (eng.distinct_query(line, n)) out << n << "\n";
        else out << "Error: could not run COUNT(DISTINCT) query\n";
        return;
    }
    // HAVING: parse the head (pre-HAVING) for decode, run having_query for the surviving (group,value) pairs
    if (U.find("HAVING") != std::string::npos) {
        const size_t h = U.find("HAVING");
        MatrixQuery q{};
        if (eng.parse_query(line.substr(0, h), q) != MatrixQueryStatus::OK) { out << "Error: could not parse HAVING query\n"; return; }
        for (const auto& [g, v] : eng.having_query(line)) {
            const std::string label = eng.string_dict_size(q.key_col) > 0 ? eng.string_decode(q.key_col, (uint32_t)g) : std::to_string(g);
            out << label << " │ " << matrixcli_detail::decode_agg(eng, q, v) << "\n";
        }
        return;
    }
```
  and extend the existing `(` branch: after `parse_query`→`q`, when `q.grouped && q.limit > 0`, format
  `top_query` instead of the plain grouped output:
```cpp
        if (q.grouped && q.limit > 0) {
            for (const auto& [g, v] : eng.top_query(line)) {
                const std::string label = eng.string_dict_size(q.key_col) > 0 ? eng.string_decode(q.key_col, (uint32_t)g) : std::to_string(g);
                out << label << " │ " << decode_agg(eng, q, v) << "\n";
            }
            return;
        }
```
  (place this right after the `execute_query` OK check, before the `if (!q.grouped)` scalar branch — so
  top-N short-circuits before the plain grouped print.)
- [ ] **Step 4:** run test → passes; `./run_tests.sh` + `SAN=1 ./run_tests.sh` green.
- [ ] **Step 5: commit** — `git commit -am "feat(cli): route COUNT(DISTINCT), HAVING, and top-N (ORDER BY..LIMIT)"`

---

### Task 3: multi-aggregate SELECT (scalar + grouped)

**Files:** Modify `matrix_cli.hpp`; Test `test_cli.cpp`.

- [ ] **Step 1: failing test**:
```cpp
        "SELECT COUNT(amount), SUM(amount)\n"                 // scalar multi-agg -> 4 and 1880
        "SELECT COUNT(amount), SUM(amount) GROUP BY region\n" // grouped multi-agg -> per-region count+sum
```
  asserts: a line containing both `4` and `1880` for the scalar; grouped output has `books`/`games`/`music`
  with two value columns (e.g. `has(r,"books │ 2 │ 30")`).
- [ ] **Step 2:** run → fails (comma reaches projection path → unknown column "COUNT(amount),"-ish → Error).
- [ ] **Step 3: implement** — add a multi-agg branch in `matrix_cli_run_sql` after the AVG check and before
  the single-`(` branch. Detect: `line` has a comma AND a `(`. Split the SELECT list / shared tail, run each
  term through parse+execute, keep `q_i`+`r_i`, then format scalar or grouped:
```cpp
    if (line.find('(') != std::string::npos && line.find(',') != std::string::npos) {
        // split "SELECT a, b, c <tail>" -> terms + shared tail (tail = first WHERE/GROUP/ORDER onward)
        const std::string body = line.substr(U.find("SELECT") + 6);
        size_t cut = std::string::npos;
        for (const char* kw : {" WHERE", " GROUP", " ORDER"}) { const size_t p = U.find(kw); if (p != std::string::npos) cut = std::min(cut, p); }
        const std::string list = cut == std::string::npos ? body : line.substr(U.find("SELECT") + 6, cut - (U.find("SELECT") + 6));
        const std::string tail = cut == std::string::npos ? std::string{} : line.substr(cut);
        std::vector<MatrixQuery> qs; std::vector<std::vector<uint64_t>> rs; std::vector<std::string> labels;
        for (size_t i = 0; i <= list.size();) {
            const size_t j = list.find(',', i);
            std::string term = matrixcli_detail::trim(list.substr(i, (j == std::string::npos ? list.size() : j) - i));
            if (!term.empty()) {
                MatrixQuery q{}; std::vector<uint64_t> r;
                if (eng.parse_query("SELECT " + term + tail, q) != MatrixQueryStatus::OK ||
                    eng.execute_query(q, r) != MatrixQueryStatus::OK) { out << "Error: could not run multi-aggregate query\n"; return; }
                qs.push_back(q); rs.push_back(std::move(r)); labels.push_back(term);
            }
            if (j == std::string::npos) break; i = j + 1;
        }
        if (qs.empty()) { out << "Error: empty SELECT list\n"; return; }
        const bool grouped = qs[0].grouped;
        if (!grouped) {                                   // one labeled row of scalars
            for (size_t c = 0; c < labels.size(); ++c) out << (c ? " │ " : "") << labels[c]; out << "\n";
            for (size_t c = 0; c < rs.size(); ++c) out << (c ? " │ " : "") << matrixcli_detail::decode_agg(eng, qs[c], rs[c][0]); out << "\n";
            return;
        }
        const uint64_t key = qs[0].key_col;
        out << "key";  for (const std::string& l : labels) out << " │ " << l; out << "\n";
        for (size_t g = 0; g < rs[0].size(); ++g) {
            out << (eng.string_dict_size(key) > 0 ? eng.string_decode(key, (uint32_t)g) : std::to_string(g));
            for (size_t c = 0; c < rs.size(); ++c) out << " │ " << matrixcli_detail::decode_agg(eng, qs[c], rs[c][g]);
            out << "\n";
        }
        return;
    }
```
  (`U` is the uppercased `line` from Task 2's edit — reuse it.)
- [ ] **Step 4:** run test → passes; `./run_tests.sh` + `SAN=1 ./run_tests.sh` green.
- [ ] **Step 5: commit** — `git commit -am "feat(cli): multi-aggregate SELECT (scalar + grouped tables)"`

---

## Self-Review

**Spec coverage:** `.save`/`.open` guarded + next_id (T1) ✓; COUNT(DISTINCT)/HAVING/top-N (T2) ✓; multi-agg
scalar+grouped (T3) ✓; output decoding reuses `decode_agg` ✓; tests per spec ✓; non-goals untouched ✓.

**Router order (final, in `matrix_cli_run_sql`):** DISTINCT → HAVING → AVG → multi-agg(`,`+`(`) → single `(`
(incl. top-N) → projection. DISTINCT/HAVING before AVG (a HAVING line may contain neither AVG nor a comma);
multi-agg before single `(`. Matches spec §router (the spec lists HAVING first; DISTINCT-first is equivalent
since the two are mutually exclusive in one line). `U` computed once at the top of the function.

**Type consistency:** `decode_agg(eng, q, v)` and `decode_proj`/`decode_num` are the v1 signatures (unchanged);
`having_query`/`top_query` return `std::vector<std::pair<uint64_t,uint64_t>>`; `distinct_query(line, n)` is
`bool`; `save_catalog`/`load_catalog` are `void`. `next_id` is the existing `matrix_repl` local.

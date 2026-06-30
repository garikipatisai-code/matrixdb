# matrixdb Harden & Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. Steps use checkbox (`- [ ]`).

**Goal:** A one-command demo (`matrixdb -f examples/tour.sql`), `.timing` in the REPL, `#` comment lines, and
a docs/FINDINGS pass. No new query capability. **Spec:** `docs/superpowers/specs/2026-06-30-matrixdb-polish-design.md`

**Architecture:** Two small `matrix_repl` additions (`#` skip, `.timing`); an `examples/` dir; a demo
smoke-check in `run_tests.sh` (runs the real tour, greps a known value — no test duplication).

**Tech Stack:** C++20 (`std::chrono` for timing), bash.

---

### Task 1: REPL `#` comments + `.timing on|off`

**Files:** Modify `matrix_cli.hpp` (`matrix_repl` loop, `.help`, add `#include <chrono>`); Test `test_cli.cpp`.

- [ ] **Step 1: failing test** — add to `test_cli.cpp`:
```cpp
    {   // .timing prints per-query elapsed; # lines are comments (skipped, no error)
        std::ostringstream o; std::istringstream i("# a comment\n.timing on\nSELECT SUM(amount)\n.quit\n");
        matrix_repl(i, o, eng); const std::string s = o.str();
        assert(has(s, "1880") && has(s, "µs)") && !has(s, "Error:"));   // comment didn't error; timing printed
        std::cout << "[cli timing/comments] ok\n";
    }
```
- [ ] **Step 2:** run → fails (`#` line → SQL → "Error"; `.timing` → unknown command).
- [ ] **Step 3: implement** — in `matrix_cli.hpp`: add `#include <chrono>`; in `matrix_repl` add a
  `bool timing = false;` local; skip comments; time SQL lines; add the `.timing` dot-command.
  - Comment skip — change the empty-line guard:
```cpp
        if (line.empty() || line[0] == '#') continue;   // blank or # comment
```
  - SQL dispatch with optional timing — replace `if (line[0] != '.') { matrix_cli_run_sql(line, out, eng); continue; }`:
```cpp
        if (line[0] != '.') {
            if (!timing) { matrix_cli_run_sql(line, out, eng); continue; }
            const auto t0 = std::chrono::steady_clock::now();
            matrix_cli_run_sql(line, out, eng);
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
            out << "(" << us << " µs)\n";
            continue;
        }
```
  - `.timing` dot-command (before the final `else`):
```cpp
        else if (cmd == ".timing") {
            if (tk.size() >= 2 && tk[1] == "off") timing = false;
            else timing = true;                       // ".timing" or ".timing on"
            out << "timing " << (timing ? "on" : "off") << "\n";
        }
```
- [ ] **Step 4: `.help`** — append `.timing on|off` to the commands line.
- [ ] **Step 5:** `clang++ -std=c++20 -O1 -fsanitize=address,undefined test_cli.cpp -o /tmp/tcli && /tmp/tcli` → green.
- [ ] **Step 6: commit** — `git commit -am "feat(cli): .timing on|off + # comment lines"`

---

### Task 2: `examples/` demo + run_tests smoke-check

**Files:** Create `examples/orders.csv`, `examples/regions.csv`, `examples/tour.sql`; Modify `run_tests.sh`.

- [ ] **Step 1: `examples/orders.csv`** (header `region,amount,qty`, 12 rows; SUM(amount)=2575):
```
region,amount,qty
0,100,2
1,250,5
2,80,1
0,300,3
1,150,2
2,420,4
0,90,1
1,500,5
2,60,1
0,210,2
1,340,3
2,75,1
```
- [ ] **Step 2: `examples/regions.csv`** (header `key,name`):
```
key,name
0,north
1,south
2,east
```
- [ ] **Step 3: `examples/tour.sql`** — a commented walkthrough (paths relative to repo root):
```sql
# matrixdb tour — run from the repo root:  ./matrixdb -f examples/tour.sql
.load examples/orders.csv region u32 col0 header
.load examples/orders.csv amount u32 col1 header
.load examples/orders.csv qty u32 col2 header
.load examples/regions.csv reg_key u32 col0 header
.load examples/regions.csv reg_name str col1 header
.columns
# scalar + grouped + filtered
SELECT SUM(amount)
SELECT SUM(amount) GROUP BY region
SELECT SUM(amount) WHERE amount > 200
# multi-aggregate, top-N, HAVING, distinct
SELECT COUNT(amount), SUM(amount), MAX(amount) GROUP BY region
SELECT SUM(amount) GROUP BY region ORDER BY SUM DESC LIMIT 2
SELECT SUM(amount) GROUP BY region HAVING SUM > 700
SELECT COUNT(DISTINCT region)
# join orders to region names
SELECT amount, reg_name JOIN region = reg_key LIMIT 5
SELECT COUNT(*) JOIN region = reg_key
# persistence
.save examples/tour.db
.open examples/tour.db
.stats
.quit
```
- [ ] **Step 4: run_tests.sh smoke-check** — after the oracle block (before the `echo "----"`), add:
```bash
# CLI demo smoke: the shipped example tour must run end-to-end and report the known SUM(amount)=2575.
if "$CXX" -std=c++20 -O2 matrixdb_cli.cpp -o "$TMP/mdb_cli" 2>"$TMP/mdb_cli.err" \
   && "$TMP/mdb_cli" -f examples/tour.sql 2>/dev/null | grep -q "2575"; then
    demo="OK"
else
    demo="FAIL"; fail=$((fail + 1)); failed="$failed DEMO"
fi
```
  and change the summary line to include it:
```bash
echo "tests passed: $pass   oracle: $oracle   demo: $demo"
```
- [ ] **Step 5:** `chmod` not needed; run `./run_tests.sh` → `demo: OK`, ALL GREEN. (Clean up: the tour
  writes `examples/tour.db`; add it to `.gitignore` or `rm` it — Step 6.)
- [ ] **Step 6:** add `examples/tour.db` to `.gitignore` (don't commit the generated snapshot); `git rm
  --cached` is unneeded (never added). Run the tour once, `rm -f examples/tour.db`.
- [ ] **Step 7: commit** — `git add examples run_tests.sh .gitignore && git commit -m "feat(cli): examples/ tour demo + run_tests smoke-check"`

---

### Task 3: docs pass (README + FINDINGS + notebook)

**Files:** Modify `README.md`, `FINDINGS.md`, `make_notebook.py`; regenerate notebook; update memory.

- [ ] **Step 1: README** — under the CLI section add: `Try the demo: \`./matrixdb -f examples/tour.sql\`
  (loads two CSVs and walks every form).` Verify command/query/join/`.timing` lists match `.help`.
- [ ] **Step 2: FINDINGS.md** — append one entry for the CLI arc (v1 testable shell-over-streams; v2 full
  surface + abort-guarded persistence; equi-join with dict-key rejection; polish/demo) — decision + why,
  matching the file's voice. (Read the file's tail first to match format.)
- [ ] **Step 3: notebook demo cell** — extend the piped script in `make_notebook.py` to include a multi-agg
  and a join line; regenerate (`python3 make_notebook.py`).
- [ ] **Step 4:** update memory `matrixdb-state.md` (polish/demo landed; program complete).
- [ ] **Step 5:** `./run_tests.sh` green; final `SAN=1 ./run_tests.sh` green.
- [ ] **Step 6: commit** — `git commit -am "docs(cli): README demo pointer + FINDINGS entry + notebook"`

---

## Self-Review

**Spec coverage:** `#` comments + `.timing` (T1) ✓; `examples/` demo + smoke-check (T2) ✓; README/FINDINGS/
notebook (T3) ✓; hermetic-ish verification = run the real tour in `run_tests.sh` (no test duplication, beats
the spec's inline-replay idea — the artifact itself is the test) ✓; non-goals (EXPLAIN/readline) untouched ✓.

**Placeholder scan:** none — example data + tour.sql + the grep target (`2575`) are concrete.

**Type consistency:** `.timing` toggles a `bool timing` local in `matrix_repl`; `std::chrono::steady_clock`
needs `#include <chrono>`; the smoke-check reuses `$CXX`/`$TMP`/`$fail`/`$failed` already defined in
`run_tests.sh`. `SUM(amount)` over `examples/orders.csv` = 700+1240+635 = 2575 (the grep target).

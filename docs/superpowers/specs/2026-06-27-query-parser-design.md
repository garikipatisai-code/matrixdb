# Design: Text Query Parser — scalar subset (DM-4)

**Status:** approved-by-standing-directive (continue all phases, don't wait). **Date:** 2026-06-27.
**Builds on:** QRY-1 (`MatrixQuery`/`execute_query` — "DM-4-lite"), QRY-3 (`MatrixCmp`), DM-2 (`column_id`/`column_name`), DM-3 (`column_type`).
**Fully local.**

**Thesis:** *Queries are built as `MatrixQuery` C++ structs — there's no query language (DM-4: "opcodes
only"). Add a text parser for the common scalar shape `SELECT <agg>(<col>) [WHERE <col> <op> <val> [AND
<val>]]`, resolving column names (DM-2) and placing the filter bound in the type-correct field (DM-3).
`parse_query(sql, out)` returns a `MatrixQuery` ready for `execute_query`. Untrusted input → graceful
`ERR_PARSE` (never crash), the VAL-1 discipline. Scoped to scalar aggregation; GROUP BY parsing (which
needs a `num_groups` SQL can't express) is deferred.*

---

## 1. Scope

**IN (`compute_mock.cpp` + `compute.hpp` + new `test_query_parser.cpp`):**
- `compute.hpp`: `MatrixQueryStatus` gains `ERR_PARSE` (malformed query text).
- `CPUMockEngine::parse_query(const std::string& sql, MatrixQuery& out) -> MatrixQueryStatus` —
  tokenize → parse the grammar below → resolve the column name (`column_id`; `0` ⇒ `ERR_UNKNOWN_COLUMN`)
  → set `agg`/`value_col`/`has_filter`/`cmp` and the bound in the field matching `column_type`
  (`U32`→`threshold`/`upper`, `I64`→`lo_i64`/`hi_i64`, `F64`→`lo_f64`/`hi_f64`). On any malformed input →
  `ERR_PARSE`, `out` reset. (Pure parse — the caller runs `execute_query(out, results)`.)
- Private helpers: `matrixparse_tokenize` (a small char scanner) and a `set_bound` (type-aware numeric parse).

**Grammar (case-insensitive keywords; `SUM(col)` and `SUM ( col )` both tokenize):**
```
query := SELECT agg '(' name ')' [ WHERE name op value [ AND value ] ]
agg   := COUNT | SUM | MIN | MAX
op    := '>' | '>=' | '<' | '<=' | '=' | '!=' | BETWEEN
value := integer | float literal (sign allowed)
```
- The `WHERE` column must be the SAME as the `SELECT` column (the engine's scalar filter is on
  `value_col`) — else `ERR_PARSE`. `BETWEEN` takes `value AND value`; the other ops take one `value`.
- Trailing tokens after a complete parse → `ERR_PARSE`.

**Invariants:** untrusted text never crashes — every malformed form returns `ERR_PARSE`/`ERR_UNKNOWN_COLUMN`
with `out` reset; release-safe (no asserts on input, `<cassert>`-free path). The bound is parsed for the
resolved column's type only (an out-of-range/junk literal → `ERR_PARSE`). `parse_query` only reads the
catalog (`column_id`/`column_type`); it executes nothing. Oracle untouched.

**OUT (deferred):** GROUP BY (needs `num_groups`); projections / multiple aggregates; `AND`/`OR` of
multiple predicates; filtering by a different column than the aggregate; quoted identifiers; `*`; joins
(DM-8); a full SQL grammar.

---

## 2. compute.hpp

```cpp
enum class MatrixQueryStatus { OK, ERR_UNKNOWN_COLUMN, ERR_INVALID_GROUP, ERR_TOO_MANY_GROUPS, ERR_UNSUPPORTED_TYPE, ERR_PARSE };
```
(append `ERR_PARSE`)

---

## 3. compute_mock.cpp (engine methods + helpers)

Add includes if missing: `<cctype>` (toupper/isspace), `<sstream>` not needed; `<charconv>`/`<cstdlib>`/`<cerrno>` are already pulled in via csv_ingest.hpp/compute.hpp — confirm and add to compute_mock.cpp if not.

```cpp
// Tokenize a query string: identifiers/keywords/numbers, the comparison operators (> >= < <= = !=),
// and parens. Whitespace-separated; operators and parens need no surrounding spaces. (free helper)
inline std::vector<std::string> matrixparse_tokenize(const std::string& s) {
    std::vector<std::string> t;
    const size_t n = s.size();
    for (size_t i = 0; i < n; ) {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (c == '(' || c == ')') { t.emplace_back(1, c); ++i; continue; }
        if (c == '>' || c == '<' || c == '=' || c == '!') {
            if (i + 1 < n && s[i + 1] == '=') { t.push_back(s.substr(i, 2)); i += 2; }
            else { t.emplace_back(1, c); ++i; }
            continue;
        }
        size_t j = i;   // a run up to the next space / paren / operator char (covers names and signed numbers)
        while (j < n && !std::isspace(static_cast<unsigned char>(s[j])) && s[j] != '(' && s[j] != ')'
               && s[j] != '>' && s[j] != '<' && s[j] != '=' && s[j] != '!') ++j;
        t.push_back(s.substr(i, j - i));
        i = j;
    }
    return t;
}
```

Engine methods (the parser + a type-aware bound setter):
```cpp
// Parse a scalar query string into `out` (see DM-4 grammar). Returns OK, ERR_UNKNOWN_COLUMN (bad name),
// or ERR_PARSE (any malformed form). Untrusted input — never crashes; `out` is reset first.
MatrixQueryStatus parse_query(const std::string& sql, MatrixQuery& out) {
    out = MatrixQuery{};
    const std::vector<std::string> tk = matrixparse_tokenize(sql);
    size_t k = 0;
    auto up   = [](std::string s){ for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return s; };
    auto next = [&]{ return k < tk.size() ? tk[k++] : std::string{}; };
    if (up(next()) != "SELECT") return MatrixQueryStatus::ERR_PARSE;
    const std::string aggs = up(next());
    MatrixAggOp agg;
    if (aggs == "COUNT") agg = AGG_COUNT; else if (aggs == "SUM") agg = AGG_SUM;
    else if (aggs == "MIN") agg = AGG_MIN; else if (aggs == "MAX") agg = AGG_MAX;
    else return MatrixQueryStatus::ERR_PARSE;
    if (next() != "(") return MatrixQueryStatus::ERR_PARSE;
    const std::string col = next();
    if (next() != ")") return MatrixQueryStatus::ERR_PARSE;
    const uint64_t vid = column_id(col);
    if (vid == 0) return MatrixQueryStatus::ERR_UNKNOWN_COLUMN;
    out.value_col = vid; out.agg = agg;
    if (k == tk.size()) return MatrixQueryStatus::OK;            // no WHERE
    if (up(next()) != "WHERE") return MatrixQueryStatus::ERR_PARSE;
    if (column_id(next()) != vid) return MatrixQueryStatus::ERR_PARSE;   // filter col must == select col
    const std::string ops = up(next());
    const MatrixType ty = column_type(vid);
    out.has_filter = true;
    if (ops == "BETWEEN") {
        out.cmp = MatrixCmp::BETWEEN;
        if (!set_bound(ty, out, true, next())) return MatrixQueryStatus::ERR_PARSE;
        if (up(next()) != "AND")               return MatrixQueryStatus::ERR_PARSE;
        if (!set_bound(ty, out, false, next())) return MatrixQueryStatus::ERR_PARSE;
    } else {
        if      (ops == ">")  out.cmp = MatrixCmp::GT;  else if (ops == ">=") out.cmp = MatrixCmp::GE;
        else if (ops == "<")  out.cmp = MatrixCmp::LT;  else if (ops == "<=") out.cmp = MatrixCmp::LE;
        else if (ops == "=")  out.cmp = MatrixCmp::EQ;  else if (ops == "!=") out.cmp = MatrixCmp::NE;
        else return MatrixQueryStatus::ERR_PARSE;
        if (!set_bound(ty, out, true, next())) return MatrixQueryStatus::ERR_PARSE;
    }
    if (k != tk.size()) return MatrixQueryStatus::ERR_PARSE;     // trailing junk
    return MatrixQueryStatus::OK;
}
```
Private:
```cpp
// Parse the numeric literal `v` into the bound field matching the column type (lo=true -> primary/lower).
// Returns false on junk / overflow / empty. int64 via from_chars; double via strtod; u32 via from_chars.
bool set_bound(MatrixType ty, MatrixQuery& q, bool lo, const std::string& v) {
    if (v.empty()) return false;
    if (ty == MatrixType::F64) {
        errno = 0; char* e = nullptr; const double d = std::strtod(v.c_str(), &e);
        if (e != v.c_str() + v.size() || errno == ERANGE) return false;
        (lo ? q.lo_f64 : q.hi_f64) = d; return true;
    }
    if (ty == MatrixType::I64) {
        int64_t x = 0; auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), x);
        if (ec != std::errc{} || p != v.data() + v.size()) return false;
        (lo ? q.lo_i64 : q.hi_i64) = x; return true;
    }
    uint32_t x = 0; auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), x);
    if (ec != std::errc{} || p != v.data() + v.size()) return false;
    (lo ? q.threshold : q.upper) = x; return true;
}
```

---

## 4. Verification (`test_query_parser.cpp`, CPU)

Register named columns of each type (`qty` u32, `revenue` int64, `rate` double), then:
- **Happy paths** (parse → `execute_query` → assert vs an in-test oracle, by type):
  - `"SELECT SUM(qty)"` → OK, scalar SUM all.
  - `"SELECT COUNT(revenue) WHERE revenue > 1000000000"` → OK, int64 GT bound in `lo_i64`, result correct.
  - `"SELECT MAX(rate) WHERE rate <= 3.5"` → OK, double bound in `lo_f64`, result correct.
  - `"SELECT SUM(revenue) WHERE revenue BETWEEN -100 AND 5000000000"` → OK, BETWEEN bounds in `lo_i64`/`hi_i64`.
  - case-insensitivity: `"select sum ( qty )"` parses.
- **Field placement**: after parsing an int64/double filter, assert the bound landed in `lo_i64`/`lo_f64`
  (not `threshold`) and `cmp` is right — the type-aware placement is the crux.
- **Graceful errors** (each returns the right status, no crash): empty string, `"SELECT FOO(qty)"`
  (bad agg) → ERR_PARSE; `"SELECT SUM(nosuchcol)"` → ERR_UNKNOWN_COLUMN; `"SELECT SUM(qty"` (missing `)`)
  → ERR_PARSE; `"SELECT SUM(qty) WHERE qty >"` (missing value) → ERR_PARSE; `"SELECT SUM(qty) WHERE qty > x"`
  (non-numeric) → ERR_PARSE; `"SELECT SUM(qty) WHERE revenue > 5"` (filter col ≠ select col) → ERR_PARSE;
  `"SELECT SUM(qty) GROUP BY qty"` (unsupported tail) → ERR_PARSE; `"SELECT SUM(qty) extra"` (trailing) → ERR_PARSE.
- **Non-vacuity / release-safety**: build the test with `-DNDEBUG` too — malformed inputs still return a
  status (no abort); a parsed query executed gives the SAME result as the hand-built `MatrixQuery`.

Plus: full CPU suite (now 35 tests) + oracle `83886070`; `test_query`/`test_query_validation`/`test_schema`
pass (the new enum value is appended; existing switches have defaults/are exhaustive on the old values —
confirm no `-Werror=switch`-style breakage). Notebook regenerated.

---

## 5. Open / deferred
GROUP BY parsing (`num_groups` derivation); projections / column lists; multi-predicate AND/OR; filter
by a different column; quoted identifiers; a full SQL grammar; a `query(sql, results)` parse+execute
convenience.

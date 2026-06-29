# Richer SQL Grammar — Design / Roadmap

**Date:** 2026-06-29. **Status:** design + first increment (cross-column scalar WHERE) implemented; the rest scoped here.

## Where the grammar actually is today

The text query surface is richer than a single entry point suggests — it's spread across several parse+run methods:
- `parse_query` / `execute_query`: `SELECT AGG(col) [WHERE col op val [AND val]] [GROUP BY key] [ORDER BY agg DESC LIMIT n]`, AGG ∈ {COUNT,SUM,MIN,MAX}, types u32/i64/f64, incl. **string literals** on dict-encoded columns (EQ/NE + ordered/BETWEEN via the sorted dict).
- `avg_query` (`SELECT AVG(col) …`), `having_query` (`… GROUP BY k HAVING agg op val`), `top_query` (`… ORDER BY agg DESC LIMIT n`), `distinct_query` (`SELECT COUNT(DISTINCT col)`).

So AVG / HAVING / top-N / COUNT(DISTINCT) **already parse** — the real gaps are below.

## Genuine gaps (recommended order)

1. **Cross-column WHERE** — `SELECT SUM(revenue) WHERE region = 'US'` (filter one column, aggregate another). The single biggest real-SQL gap and the natural complement to dict-encoded strings (filter by a string/dim column, aggregate a measure). **← first increment, implemented now (scalar; u32 filter column, value u32/i64/f64).**
   - Mechanism: `MatrixQuery.filter_col` (0 = filter the value column, preserving today's behavior). A new fused reducer `matrix_cpu_reduce_filtered_by[_i64/_f64](f, pred, a, n, op)` aggregates `a[i]` over rows where the u32 predicate holds on `f[i]`. The parser relaxes "filter col must == select col": any **u32** column may be the filter, and its bound is parsed by *its* type/dict (so a string filter literal Just Works).
   - **Deferred (next increments):** grouped cross-column (`GROUP BY dim WHERE other <pred> agg(measure)`); a non-u32 filter column (i64/f64 ranges); value-column NULL-awareness on the cross path; the GPU cross-column kernel.

2. **Multi-aggregate SELECT** — `SELECT COUNT(*), SUM(x), AVG(y) [WHERE …] [GROUP BY k]`. Returns a result *row* (one value per aggregate; per group when grouped). Build: parse a comma-separated agg-list sharing the WHERE+GROUP BY, run each as its own reduce over the (now possibly cross-column) filter, assemble columns. No new reducer once cross-column WHERE exists.

3. **Unified `query(sql)` entry** — one SQL door that dispatches to plain/AVG/HAVING/top-N/DISTINCT by inspecting the parsed shape, instead of the caller picking `avg_query` vs `having_query` vs … Pure dispatch over verified methods; low risk, high ergonomics.

4. **Projections** — `SELECT col [WHERE …] [LIMIT n]` (row retrieval, not aggregation). Different result shape (materialize rows); composes with `gather`. Larger; lowest priority.

## Non-goals (unchanged)
Joins-in-SQL (the `hash_join` primitive exists; SQL-level join planning is XL), full ANSI SQL, a cost-based optimizer. This roadmap is "the analytical subset people actually type," built as verified increments — not ANSI completeness.

# matrixdb HAVING + top-N on join-aggregates (Design)

**Date:** 2026-06-30  **Status:** approved (standing directive); ready for plan. Follow-on to join-aggregates.
**Goal:** Complete the join-aggregate into a full analytical query: filter and rank the per-dimension
results. `SELECT agg(lcol) JOIN lk = rk GROUP BY rcol [HAVING agg <cmp> v | ORDER BY agg DESC LIMIT n]`.
"Top 3 regions by revenue", "regions with total > 500". CPU, local.

## Why

The join-aggregate produces per-group `(label, value)` rows entirely CLI-side (a `std::map` of buckets, each
reduced by `reduce_*`). `HAVING` (keep groups whose aggregate satisfies a comparison) and top-N (sort by the
aggregate desc, take the first *n*) are pure post-processing on those rows — the same surface non-join
grouped queries already have. Without them, "which dimensions matter most" needs eyeballing the full list.

## Grammar (extends the join-aggregate tail)

```
SELECT <AGG>(<lcol>) JOIN <lkey> = <rkey> GROUP BY <rcol> [ HAVING <agg> <cmp> <v>
                                                          | ORDER BY <agg> DESC LIMIT <n> ]
```

- `HAVING <agg> <cmp> <v>` — keep groups whose aggregate value satisfies `<cmp>` (`> >= < <= = !=`) against
  numeric `<v>`. The `<agg>` token is the aggregate name (informational; the comparison is against the
  SELECT's aggregate value).
- `ORDER BY <agg> DESC LIMIT <n>` — sort groups by aggregate value descending, keep the first `n`. (DESC
  only — the top-N idiom; ASC is deferred.)
- Only one of HAVING / ORDER BY per query (mutually exclusive in v1). Requires `GROUP BY` (no meaning on a
  scalar). Without either, behavior is unchanged.

## Implementation (post-process the buckets; one reducer returns text + a sort key)

Fold the existing `reduce_vals` into **`reduce_agg(eng, col, agg, vals) -> {std::string text, double order}`**:
- `text` = the type-correct display (u32 unsigned / i64 signed / f64), exactly as today.
- `order` = the same reduced value as a `double`, for comparison/sorting. (Lossy above 2^53 for huge
  i64/u64 — fine for ranking/threshold a CLI shows; noted with a `// ponytail:`.)

Grouped join-agg path becomes: build buckets → per group compute `{label, text, order}` into a vector →
apply HAVING (filter on `order`) or ORDER BY (stable_sort by `order` desc, resize to `n`) → print
`label │ text`. `std::stable_sort` keeps the label order for ties (deterministic). Scalar path:
`out << reduce_agg(...).text`.

### Tail parsing (mechanical, token-counted)

After `JOIN lkey = rkey`, tokens `tk[ji+4 ...]`:
- `GROUP BY rcol` (3 tokens) → plain grouped (today).
- `+ HAVING agg cmp v` (→ 7 tokens) → grouped + HAVING (`tk[ji+7]=="HAVING"`, cmp `tk[ji+9]`, v `tk[ji+10]`).
- `+ ORDER BY agg DESC LIMIT n` (→ 9 tokens) → grouped + top-N (`tk[ji+7]=="ORDER"`, `tk[ji+8]=="BY"`,
  `tk[ji+11]=="LIMIT"`, n `tk[ji+12]`).
- anything else → `Error: could not parse join GROUP BY clause`.

`<cmp>` parses to a double comparison; `<v>`/`<n>` via `strtod`/`strtoull`. A bad cmp/number → `Error`.
Never crashes.

## Testing (extend the join block in `test_cli.cpp`)

Same fixture (north 30, south 900, east 950 by SUM(ord_amt)):
- `... GROUP BY reg_name HAVING SUM > 100` → `south`/`east` present, `north │ 30` absent.
- `... GROUP BY reg_name ORDER BY SUM DESC LIMIT 2` → `east`(950) then `south`(900); `north` absent; first
  data line is `east`.
- `... GROUP BY reg_name ORDER BY SUM DESC LIMIT 1` → only `east`.
- a malformed tail (`... GROUP BY reg_name HAVING`) → `Error:`, session continues.
ASan/UBSan-clean; full suite + oracle + demo green. Add a top-N line to `examples/tour.sql`.

## Scope & non-goals

**In:** HAVING + top-N (DESC) on a grouped join-aggregate, over the existing buckets, all measure types.
**Deferred:** ORDER BY ASC; HAVING + ORDER BY together; BETWEEN in HAVING; HAVING/top-N on non-join grouped
queries already exist via the engine (`having_query`/`top_query`) — unchanged.

## Success criteria

`SELECT SUM(amount) JOIN region = reg_key GROUP BY reg_name ORDER BY SUM DESC LIMIT 2` ranks the dimensions;
`... HAVING SUM > 500` filters them; bad input errors cleanly. `test_cli.cpp` green under ASan/UBSan; full
suite + oracle + demo green.

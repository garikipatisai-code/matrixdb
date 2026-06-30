# matrixdb aggregates over a join (Design)

**Date:** 2026-06-30  **Status:** design approved (standing directive); ready for plan. Follow-on to the
joins sub-project (its named deferral).
**Goal:** The star-schema query — aggregate a **fact measure** (left table) over a join, optionally **grouped
by a dimension attribute** (right table): `SELECT agg(lcol) JOIN lkey = rkey [GROUP BY rcol]`. The thing
joins are *for* in analytics. CPU, local.

## Why

The join sub-project shipped row projection + `COUNT(*)`. The deferred piece was aggregating across the join
— e.g. "total order amount per region **name**", where the measure (`amount`) is in the orders table and the
grouping attribute (`name`) lives only in the regions table. Without the join you can only group by the
foreign key (an integer index); with it you group by the human dimension. `hash_join` + `gather` already
give the matched values; the only missing step is a type-aware reduction over them.

## Semantics (fixed positional roles — unambiguous in a flat catalog)

`SELECT <AGG>(<lcol>) JOIN <lkey> = <rkey> [GROUP BY <gcol>]`

- **`<AGG>(<lcol>)`** — the aggregate (`COUNT|SUM|MIN|MAX`) of a **left** measure column (`lcol` aligned with
  `lkey`). `AVG` is **not** supported here (the engine's AVG path doesn't join) → `Error`.
- **`GROUP BY <gcol>`** (optional) — `gcol` is a **right** dimension column (aligned with `rkey`); the join
  result is grouped by its value per matched row. No `GROUP BY` → one scalar aggregate over all joined rows.
- Aggregation is over the **join result rows** (every matched `(lrow, rrow)` pair — standard SQL: a left row
  matching *k* right rows contributes *k* times). For a unique right key (N:1) this is the natural "one row
  per fact, labeled by its dimension".

This fixes the roles: **measure = left, group dimension = right.** (Grouping by a left attribute needs no
join — it's plain `SELECT agg GROUP BY`. Aggregating a right measure is the rarer case — deferred.)

## Execution (reuses `hash_join`/`gather` + a CLI-side reducer)

1. Parse `<AGG>` + `<lcol>` from the SELECT list (`agg` name before `(`, column between the parens — robust
   to spacing). Resolve `lcol`. Key resolution + type rules are the **existing** join checks (u32/i64 match;
   dict-string keys rejected).
2. `pairs = hash_join*(lkey, rkey)`.
3. `lvals = gather(lcol, lrows)` — the measures (encoded: u32 zero-extended, i64/f64 as bit pattern).
4. **Scalar:** reduce `lvals` by `agg` → one decoded value.
5. **Grouped:** `gvals = gather(gcol, rrows)` — the dimension keys. Bucket `lvals` by `gvals` into a
   `std::map<uint64_t, std::vector<uint64_t>>` (sorted → deterministic output), reduce each bucket, print
   `label │ value` where `label` decodes the dimension (string if `string_dict_size(gcol) > 0`, else the
   numeric key).

### Type-aware reducer (`reduce_vals(eng, col, agg, vals)`)

A small helper in `matrixcli_detail`, since no engine API reduces an arbitrary row subset:
- `COUNT` → `vals.size()` (integer; never reinterpreted).
- `SUM` → accumulate the raw u64s (`u32` zero-extended and `i64` two's-complement both sum correctly in u64);
  `f64` sums in `double` via `matrix_bit_cast`. Format by `lcol` type (u32 unsigned / i64 signed / f64).
- `MIN`/`MAX` → type-aware comparison (unsigned u32, signed i64, double f64) over `vals`; format by type.
- Empty `vals` (no matches): `COUNT`/`SUM` → `0`; `MIN`/`MAX` → `0` (scalar only; grouped never has an empty
  bucket). Edge, not the main path.

Reuses the existing `decode_num` philosophy; lives next to `decode_agg`/`decode_proj`.

## Router placement

Extends the **existing JOIN branch** in `matrix_cli_run_sql`. After resolving keys, dispatch on the SELECT
list `sel`:
1. `COUNT(*)` → cardinality (existing).
2. **`sel` contains `(`** → aggregate-over-join (new): parse `AGG(lcol)`; optional `GROUP BY gcol` from the
   tokens after `rkey` (`tk[ji+4]=="GROUP"`, `tk[ji+5]=="BY"`, `tk[ji+6]=gcol`).
3. `sel` contains `,` → projection (existing); tail may be `LIMIT n` (existing).
4. else → `Error`.

(So `SELECT SUM(amount) JOIN region = reg_key GROUP BY reg_name` routes through JOIN → aggregate path.)

## Testing (extend `test_cli.cpp`'s join block)

Same orders⋈regions fixture (`ord_region={0,1,0,2}`, `ord_amt={10,900,20,950}`, `reg_key={0,1,2}`,
`reg_name` dict `{north,south,east}`):
- `SELECT SUM(ord_amt) JOIN ord_region = reg_key` → `1880` (10+900+20+950, all match).
- `SELECT SUM(ord_amt) JOIN ord_region = reg_key GROUP BY reg_name` → `north │ 30` (10+20), `south │ 900`,
  `east │ 950` — decoded dimension labels, grouped sum.
- `SELECT COUNT(ord_amt) JOIN ord_region = reg_key GROUP BY reg_name` → counts (north 2, south 1, east 1).
- `SELECT MAX(ord_amt) JOIN ord_region = reg_key` → `950`.
- `SELECT AVG(ord_amt) JOIN ord_region = reg_key` → `Error` (AVG not supported over a join).
ASan/UBSan-clean; full suite + oracle + demo green. Add an aggregate line to `examples/tour.sql`
(`SELECT SUM(amount) JOIN region = reg_key GROUP BY reg_name`) so the demo shows the star query.

## Scope & non-goals

**In:** `SELECT COUNT|SUM|MIN|MAX(lcol) JOIN lk = rk [GROUP BY rcol]`, scalar + grouped, u32/i64/f64 measures,
u32/i64 keys, decoded dimension labels.
**Deferred (named):** `AVG` over a join; aggregating a **right** measure / grouping by a **left** attribute
(positional roles are fixed); `HAVING`/top-N on a join aggregate; multi-key joins; the dup-key
double-counting nuance is standard SQL, documented not "fixed".

## Success criteria

`SELECT SUM(amount) JOIN region = reg_key GROUP BY reg_name` prints the per-dimension totals with decoded
string labels; the scalar form prints the join-wide total; `AVG`/bad input error cleanly; the tour shows it.
`test_cli.cpp` green under ASan/UBSan; full suite + oracle + demo green.

# Design: Heat-Driven Re-Promotion (swap-on-promote) — INT-1b

**Status:** approved-by-standing-directive (session goal: proceed with ideal choice, no pause), pre-implementation
**Date:** 2026-06-26
**Builds on:** INT-1 (live tiering integration) — `tier_manager.hpp`, `tiered_column.hpp`, `compute_mock.cpp`, `test_live_tiering.cpp`.

**Thesis:** *Auto-tiering means hot data climbs back to fast memory, not just gets borrowed for a
single scan. INT-1 demotes cold columns to SSD and borrows them back per-scan; INT-1b lets a
re-hot column reclaim a resident RAM slot by displacing a colder resident — the missing half of
"resident-first analytics."*

This closes the honest gap INT-1's final review found: under a full RAM budget, the TierManager
*wants* to promote a re-hot COLD column (`should_promote` fires) but can't, because promotion
only fills *free* HOST space. Swap-on-promote gives it the ability to make room.

---

## 1. Scope

**IN:**
- **`tier_manager.hpp` — swap-on-promote.** In `rebalance()`, when a promotion candidate doesn't
  fit its target tier, evict the lowest-`keep_score` resident of that tier to make room — gated
  by a value margin (no thrash) and `MIN_RESIDENCY_TICKS` (no fresh-arrival eviction). Single
  victim, best-candidate-first, integrated into the existing promotion loop.
- **`test_tier_manager.cpp`** — a unit test: under a full bounded tier, a hot lower-tier column
  displaces a colder resident; a not-hot-enough candidate does NOT (margin holds the line).
- **`test_live_tiering.cpp` — the headline:** demote col 3 (INT-1 baseline), flip the heat
  (hammer col 3, idle col 2), assert col 3 ends **resident HOST** and col 2 ends COLD. Proven
  non-vacuous (fails without swap-on-promote).
- **Hardening (flagged on the now-live path by INT-1's final review):**
  - `tiered_column.hpp` — make the COLD file path per-process/instance unique
    (`/tmp/matrixdb_tcol_<pid>_<serial>.bin`, serial = process-unique counter) so two engine
    instances sharing a column id can't silently corrupt each other's COLD file.
  - `compute_mock.cpp` — release-safe guard in `scan_tiered_column` for an unregistered id
    (today `assert` → null-deref under `-DNDEBUG`): assert in debug, `return 0` in release.

**OUT (deferred):**
- Multi-victim displacement (evicting several colder residents to fit one big hot column) — this
  increment evicts a single victim; if one isn't enough, the candidate waits. Equal/typical-size
  columns need only one.
- HOST→DEVICE swap on a real GPU — the mechanism is tier-agnostic, but DEVICE is inert on the CPU
  build (device_cap=1); GPU swap rides the future GPU-catalog increment.
- The skip-if-unchanged COLD-write optimization (borrow churn) — separate perf nicety.

---

## 2. The algorithm (integrated into the existing promotion loop)

`rebalance()` today: (a) age heat (EWMA); (b) promotion pass — candidates sorted by net benefit,
each promoted one tier toward DEVICE *only if it fits free space*, else skipped; (c) eviction
pass — demote over-capacity tiers by lowest `keep_score`.

Swap-on-promote modifies **(b)**: when a candidate `C` doesn't fit its target tier `to`:

```
// (existing) C fits free space?  promote and continue.
if (cap == 0 || resident_bytes(to) + C.bytes <= cap) { promote(C); continue; }

// (new) swap-on-promote: evict the worst colder resident of `to` to make room.
V = the resident of `to` with the lowest keep_score, among those past MIN_RESIDENCY_TICKS
if (V exists
    && promote_value(C) > SWAP_MARGIN * keep_score(V)        // C decisively more valuable
    && resident_bytes(to) - V.bytes + C.bytes <= cap) {      // one eviction makes room
    demote(V, slower_tier(to));   // emit decision; V.tier/arrived_tick updated
    promote(C, to);               // emit decision; C.tier/arrived_tick updated
}
// else: C can't fit and no worthwhile swap — skip it (waits for a future rebalance)
```

- **`promote_value(C)`** = the promotion benefit already computed by `promote_eval(C).benefit`
  (scan-time saved over the horizon by moving C one tier up). **`keep_score(V)`** = scan-time
  V's residency saves vs its slower tier. Same units (µs over the horizon), so the comparison is
  apples-to-apples. For equal-size columns the bandwidth terms cancel → "promote C over V iff C's
  estimated future scans exceed V's by the margin."
- **`SWAP_MARGIN`** (new constant, = 1.5, matching `HYSTERESIS`): C must be 1.5× more valuable
  than V to displace it. This is the anti-thrash band — after a swap, heats (EWMA α=0.5) can't
  invert by 1.5× in one tick, so V won't immediately swap back; `MIN_RESIDENCY_TICKS` on the
  freshly-promoted C reinforces it.
- The existing eviction pass (c) still runs and is a safety net (a tier shouldn't be over capacity
  after the swap pass, but if it is, it's trimmed).
- DRY: reuse `promote_eval`, `keep_score`, `faster_tier`, `slower_tier`, `capacity_of`,
  `MIN_RESIDENCY_TICKS`. The only new arithmetic is the `SWAP_MARGIN` comparison.

**Why single-victim, integrated (not a separate K-residents-fit rewrite):** smallest change to a
tested component; the best-first candidate order already prioritizes the hottest waiting column;
the common case (equal-size columns) needs exactly one eviction. Multi-victim is YAGNI here.

---

## 3. Hardening details

**COLD path uniqueness (`tiered_column.hpp`):** today `cold_path()` is
`/tmp/matrixdb_tcol_<id>.bin` — keyed on column id alone. Two `CPUMockEngine`s (or two processes)
holding the same id share one file → silent cross-corruption (INT-1's final review demonstrated
checksum drift). Fix: a process-unique per-`TieredColumn` serial.
```cpp
static std::atomic<uint64_t> s_next_serial;          // inline, header-only
const uint64_t serial_ = s_next_serial.fetch_add(1);  // assigned at construction, stable for life
std::string cold_path() const {
    return "/tmp/matrixdb_tcol_" + std::to_string(getpid()) + "_" + std::to_string(serial_) + ".bin";
}
```
`getpid()` needs `<unistd.h>` (POSIX; the engine already targets mac/Linux). The serial is stable
for the object's lifetime, so HOST↔COLD round-trips use the same path. No test depends on a
deterministic path. `ponytail:` a crashed prior process with a reused pid could leave a stale file,
but COLD writes truncate (`"wb"`) before any read, so a stale file is harmlessly overwritten —
crash-leak cleanup is a separate ops concern.

**Unregistered-id guard (`compute_mock.cpp`):** in `scan_tiered_column`, replace the bare
`assert(it != catalog_.end())` (a null-deref under `-DNDEBUG`) with a release-safe form:
```cpp
auto it = catalog_.find(col_id);
if (it == catalog_.end()) {
    assert(false && "scan of unregistered column id");  // debug: catch the bug
    return 0;                                            // release: defined empty result
}
```

---

## 4. Verification

**`test_tier_manager.cpp` (new case, CPU) — algorithm isolation + thrash guard.** Two scenarios
chosen to be robust (no fragile EWMA-rounding boundary):
- **Positive swap:** 2 columns fill a `host_cap = 2*S` HOST tier, 1 on COLD. Keep col1 hot and
  col3 (COLD) hot via repeated `record_access`; leave col2 **idle** (heat 0). After enough
  `rebalance()` ticks to clear `MIN_RESIDENCY_TICKS`, assert the plan demotes col2 (the idle
  victim, `keep_score 0`) and promotes col3 — `tier_of(3)==HOST`, `tier_of(2)==COLD`.
- **Margin control (no thrash):** same setup but col2 and col3 are kept **equally** hot (identical
  `record_access` sequences). Their values are equal, so `promote_value(col3) > 1.5*keep_score(col2)`
  is false — assert NO swap: `tier_of(3)==COLD`, `tier_of(2)==HOST`. (Equal heat, not a near-boundary,
  so the assertion is stable regardless of the exact cost-model constants.)

DEVICE is held inert in these cases (device_cap=1) so the only actionable candidate is the COLD→HOST
column, keeping the scenario clean.

**`test_live_tiering.cpp` (the headline, CPU):** extend with `test_repromotion_under_pressure()`:
- 3 columns, budget 2*S. Phase 1 (INT-1 baseline): scan cols 1&2, never col 3 → col 3 demotes to
  COLD. Phase 2 (the flip): scan cols 1&3 repeatedly, **never col 2**, for enough rebalances that
  col 3's heat overtakes col 2's by the margin and col 2 ages cold.
- Assert: `manager_tier(3)==HOST` **and** `column_tier(3)==HOST` — col 3 is now *resident* (not
  just borrowed); `manager_tier(2)==COLD` — col 2 was displaced; `host_resident_bytes() <= 2*S`.
  Every scan still returns the exact oracle count.
- **Non-vacuity:** documented control — with swap-on-promote disabled (e.g. `SWAP_MARGIN` set
  enormous so no swap ever qualifies), col 3 stays COLD and the `tier_of(3)==HOST` assert fails.
  The implementer runs this control, shows the failure, then reverts.

**Cold-path uniqueness:** a small assertion that two `TieredColumn`s constructed with the same id
in one process produce different `cold_path()`s (verify via a public accessor or by round-tripping
both to COLD and back independently with intact, distinct checksums).

Plus: all existing CPU tests stay green; the pipeline oracle stays `83886070` (legacy path
untouched); notebook regenerated (no new file — the tests grow in place, but re-run the generator
to keep the embedded copies current).

---

## 5. Open / deferred (not blockers)

- Multi-victim displacement (fit one big hot column by evicting several colder ones).
- HOST→DEVICE swap on a real GPU (rides the GPU-catalog increment).
- COLD-write skip-if-unchanged (borrow churn).
- Crash-time COLD-file cleanup (ops concern; stale files are overwritten, not read stale).

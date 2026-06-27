# Design: Authorization / Access Control — SE-2

**Status:** approved-by-standing-directive ("you pick / keep going"), pre-implementation. **Date:** 2026-06-27.
**Builds on:** NW-1 (`server.hpp`, `matrix_serve`, `MatrixRequest`/`MatrixResponse`).
**Fully local: pure policy logic at the request boundary — no transport/TLS (those are SE-1/SE-3, deferred).**

**Thesis:** *The server dispatches any request to anything. Add per-principal authorization at the
`matrix_serve` boundary: column-level access for QUERY, and read/write grants for point-ops. A
principal can only touch what it's granted; everything else is `ERR_FORBIDDEN` — denied before any
engine call. The principal is supplied by the (authenticated) caller, never self-declared in the
payload (no spoofing).*

---

## 1. Scope

**IN (in `server.hpp` + new `test_security.cpp`):**
- `class AccessPolicy` — grants per principal id (`uint64_t`; 0 = anonymous):
  - `allow_column(principal, col_id)` — QUERY access to that catalog column.
  - `allow_read(principal)` — GET (point-store read).
  - `allow_write(principal)` — PUT (point-store write).
  - checks `can_query(principal, col)`, `can_read(principal)`, `can_write(principal)`.
  - `static AccessPolicy permissive()` — allows everything (the backward-compat / no-auth default).
- `ServerStatus::ERR_FORBIDDEN = 1001`.
- An overload `matrix_serve(CPUMockEngine& eng, const AccessPolicy& policy, uint64_t principal,
  const std::vector<uint8_t>& req_bytes)` that, after deserializing, checks the principal's
  permission for the request and returns `ERR_FORBIDDEN` (empty results, **no engine call**) if
  denied; otherwise dispatches as NW-1 did. Enforcement:
  - GET → `policy.can_read(principal)`.
  - PUT → `policy.can_write(principal)`.
  - QUERY → `policy.can_query(principal, value_col)` AND (if grouped) `can_query(principal, key_col)`.
- The existing `matrix_serve(eng, req_bytes)` (2-arg, from NW-1) **delegates** to the 4-arg with
  `AccessPolicy::permissive()` and principal 0 — so NW-1's tests + behavior are unchanged.
- `test_security.cpp` — grant/deny matrix across GET/PUT/QUERY + grouped (both columns required) +
  column-level (granted one column, denied another) + permissive-allows-all + denied-op-has-no-effect.

**OUT (deferred):** authentication (SE-1 — establishing *who* the principal is; rides the transport
auth handshake); TLS (SE-3); audit logging (SE-6); roles/groups (only per-principal grants here);
revocation semantics / row-level / predicate-level security; encryption at rest (SE-4).

---

## 2. Why principal-as-parameter (not in the request)

A client must not be able to *declare* its own identity in the payload (it would just claim admin).
The principal is whatever the authenticated transport established for the connection, passed to
`matrix_serve` as a parameter. So the request wire format is **unchanged** (no `principal` field) —
SE-2 is purely the policy + the enforcing overload. (SE-1 auth, when built, sets the principal from
the connection handshake before calling `matrix_serve`.)

---

## 3. AccessPolicy + enforcement

```cpp
class AccessPolicy {
public:
    static AccessPolicy permissive() { AccessPolicy p; p.allow_all_ = true; return p; }
    void allow_column(uint64_t principal, uint64_t col) { cols_[principal].insert(col); }
    void allow_read(uint64_t principal)  { read_.insert(principal); }
    void allow_write(uint64_t principal) { write_.insert(principal); }
    bool can_query(uint64_t principal, uint64_t col) const {
        if (allow_all_) return true;
        auto it = cols_.find(principal); return it != cols_.end() && it->second.count(col) != 0;
    }
    bool can_read(uint64_t principal)  const { return allow_all_ || read_.count(principal)  != 0; }
    bool can_write(uint64_t principal) const { return allow_all_ || write_.count(principal) != 0; }
private:
    bool allow_all_ = false;
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> cols_;
    std::unordered_set<uint64_t> read_, write_;
};
```

`matrix_serve(eng, policy, principal, req)`: deserialize (bad → ERR_BADREQUEST as before); then
authorize by kind (GET→can_read, PUT→can_write, QUERY→can_query value_col && (grouped→key_col));
denied → `MatrixResponse{ERR_FORBIDDEN, {}}` serialized, **without** touching the engine (so a denied
PUT writes nothing, a denied QUERY runs nothing). Allowed → the NW-1 dispatch. The 2-arg overload:
`return matrix_serve(eng, AccessPolicy::permissive(), 0, req);`.

`server.hpp` adds `#include <unordered_set>` (it already has `<unordered_map>` via `compute_mock.cpp`).

---

## 4. Verification (`test_security.cpp`, CPU)

Engine with column 2 (a value col) + column 1 (a key col, both loaded), plus the point store.
`AccessPolicy p`: `allow_column(1, 2)` (principal 1 may query col 2), `allow_read(1)`, `allow_write(1)`;
principal 2 granted nothing.
- **GET:** principal 1 → OK; principal 2 → ERR_FORBIDDEN.
- **PUT:** principal 1 → OK (and the value is readable after); principal 2 → ERR_FORBIDDEN, and a
  follow-up authorized GET shows the denied PUT **wrote nothing**.
- **QUERY scalar:** principal 1 on col 2 → OK + correct; principal 2 → ERR_FORBIDDEN; principal 1 on
  col 3 (not granted) → ERR_FORBIDDEN (**column-level** — granted col 2 ≠ col 3).
- **QUERY grouped:** principal 1 grouped on (key col 1, value col 2) → ERR_FORBIDDEN until col 1 is
  also granted (`allow_column(1, 1)`) → then OK (both columns required).
- **Permissive:** `AccessPolicy::permissive()` (or the 2-arg `matrix_serve`) → principal 0 may do
  everything (backward-compat).
- **Non-vacuity:** forbidden responses carry `ERR_FORBIDDEN` + empty results and leave the engine
  unchanged; a stub allowing everything would fail the deny assertions, one denying everything would
  fail the grant assertions.

Plus: NW-1's `test_server.cpp` passes unchanged (2-arg `matrix_serve` still permissive); oracle
`83886070` unchanged; all tests green; notebook regenerated.

---

## 5. Open / deferred
- Authentication (SE-1: who is the principal — transport handshake/token); TLS (SE-3); audit log
  (SE-6); roles/groups; revocation; row/predicate-level security; encryption at rest (SE-4).

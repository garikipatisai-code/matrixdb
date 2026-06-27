# Design: Audit Logging — SE-6

**Status:** approved-by-standing-directive (goal: continue all phases), pre-implementation. **Date:** 2026-06-27.
**Builds on:** NW-1 (`matrix_serve`), SE-2 (`AccessPolicy`, principal, `ERR_FORBIDDEN`).
**Fully local: an in-memory append-only log at the serve boundary — no transport.**

**Thesis:** *A served request leaves no trace of who did what. Add an append-only audit log at the
`matrix_serve` boundary recording every request — allowed, denied, AND malformed — with its
principal, kind, status, and target. The security value is precisely that denied/forbidden attempts
are recorded (forensics/compliance). Additive: existing serve overloads keep their exact behavior.*

---

## 1. Scope

**IN (in `server.hpp` + new `test_audit.cpp`):**
- `struct AuditEntry { uint64_t principal; uint32_t kind; uint32_t status; uint64_t key; uint64_t column; };`
  (`kind` = `ReqKind` value, or 0 for an un-deserializable request; `key` = GET/PUT key; `column` =
  QUERY value_col; the irrelevant one is 0.)
- `class AuditLog` — append-only: `record(const AuditEntry&)`, `entries()` (const ref), `size()`.
- Refactor: a private `matrixsrv_detail::serve_core(eng, policy, principal, req_bytes, AuditEntry& out)`
  holds the existing deserialize → authorize → dispatch logic AND fills `out`. The existing
  `matrix_serve(eng, policy, principal, req)` becomes a thin wrapper (`AuditEntry e; return serve_core(...e)`
  — discards `e`, byte-identical behavior). A NEW overload
  `matrix_serve(eng, policy, principal, req, AuditLog& audit)` calls `serve_core` then `audit.record(e)`.
  The 2-arg permissive delegate is unchanged.
- Every served request → exactly one audit entry: allowed (status OK / a query status), denied
  (`ERR_FORBIDDEN`), or malformed (`ERR_BADREQUEST`, kind 0).
- `test_audit.cpp` — serve a mix; assert the log captured each with the right principal/kind/status,
  **including the denied and the bad request** (the audit's whole point).

**OUT (deferred):** flushing the audit log to a file/syslog (in-memory here; a file sink is a thin
add); timestamps (no wall-clock in this deterministic harness — a clock is injected by the
transport layer later); log rotation/retention; structured/JSON export; tamper-evidence.

---

## 2. AuditLog + serve_core refactor

```cpp
struct AuditEntry { uint64_t principal; uint32_t kind; uint32_t status; uint64_t key; uint64_t column; };

class AuditLog {           // append-only audit trail of served requests
public:
    void record(const AuditEntry& e) { entries_.push_back(e); }
    const std::vector<AuditEntry>& entries() const { return entries_; }
    size_t size() const { return entries_.size(); }
private:
    std::vector<AuditEntry> entries_;
};
```

`serve_core` = the current `matrix_serve(eng, policy, principal, req)` body, with `AuditEntry& out`
filled at each exit: bad-deserialize → `{principal, 0, ERR_BADREQUEST, 0, 0}`; denied →
`{principal, (uint32_t)req.kind, ERR_FORBIDDEN, req.key, req.query.value_col}`; dispatched →
`{principal, (uint32_t)req.kind, resp.status, req.key, req.query.value_col}`. It returns the same
serialized response as today. The three public overloads:

```cpp
inline std::vector<uint8_t> matrix_serve(CPUMockEngine& e, const AccessPolicy& p, uint64_t pr, const std::vector<uint8_t>& b) {
    AuditEntry ignored; return matrixsrv_detail::serve_core(e, p, pr, b, ignored);
}
inline std::vector<uint8_t> matrix_serve(CPUMockEngine& e, const AccessPolicy& p, uint64_t pr, const std::vector<uint8_t>& b, AuditLog& audit) {
    AuditEntry entry; auto resp = matrixsrv_detail::serve_core(e, p, pr, b, entry); audit.record(entry); return resp;
}
inline std::vector<uint8_t> matrix_serve(CPUMockEngine& e, const std::vector<uint8_t>& b) {  // backward-compat (NW-1)
    static const AccessPolicy permissive = AccessPolicy::permissive();
    return matrix_serve(e, permissive, /*principal=*/0, b);
}
```

Behavior of the existing overloads is unchanged (NW-1's `test_server` + SE-2's `test_security` pass
as-is — they call the non-audit forms).

---

## 3. Verification (`test_audit.cpp`, CPU)

Engine with column 2 loaded; `AccessPolicy p` granting principal 1 (read/write/column 2) but not
principal 2. `AuditLog audit`. Serve a sequence (each via the 5-arg overload):
- principal 1, GET(5) → audit entry `{1, GET, OK, 5, 0}`.
- principal 2, PUT(5,500) → denied; audit entry `{2, PUT, ERR_FORBIDDEN, 5, 0}` (**the denied write
  is recorded** — the security point).
- principal 1, QUERY(col 2, SUM) → audit entry `{1, QUERY, OK, 0, 2}`.
- a garbage buffer → audit entry `{principal, 0, ERR_BADREQUEST, 0, 0}` (malformed request recorded).
- **Asserts:** `audit.size() == 4`; each entry's principal/kind/status/key/column match; the
  forbidden entry is present with `ERR_FORBIDDEN` and principal 2; the bad-request entry has kind 0.
- **Non-vacuity:** a stub that only logged successes would miss the denied + bad entries (size < 4);
  the principal/status fields prove it's the real attempt, not a placeholder.

Plus: NW-1 `test_server` + SE-2 `test_security` pass unchanged (non-audit overloads); oracle
`83886070` unchanged; all tests green; notebook regenerated.

---

## 4. Open / deferred
- File/syslog sink + rotation/retention; injected timestamps (with the transport's clock);
  structured/JSON export; tamper-evident (hash-chained) audit; per-entry latency (ties to OB-2).

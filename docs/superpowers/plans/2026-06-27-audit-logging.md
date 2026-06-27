# Audit Logging Implementation Plan — SE-6

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** An append-only `AuditLog` of every served request (allowed / denied / malformed), via a `matrix_serve(…, AuditLog&)` overload; existing serve overloads unchanged.

**Spec:** `docs/superpowers/specs/2026-06-27-audit-logging-design.md`

---

### Task 1: AuditLog + serve_core refactor + audit overload

**Files:** Modify `server.hpp`; Create `test_audit.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_audit.cpp`:

```cpp
// CPU test for audit logging (AuditLog + matrix_serve audit overload).
#include "server.hpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <string>
#include <cstdio>
#include <iostream>

static MatrixRequest mk(ReqKind kind) { MatrixRequest r; r.kind = kind; return r; }

static void test_audit() {
    const size_t N = 1000; std::vector<uint32_t> v(N);
    for (size_t i = 0; i < N; ++i) v[i] = static_cast<uint32_t>(i);
    const std::string wal = "/tmp/matrixdb_audit.bin"; std::remove(wal.c_str());
    CPUMockEngine eng(0, wal);
    eng.load_scan_column(2, v.data(), N);
    AccessPolicy p; p.allow_column(1, 2); p.allow_read(1); p.allow_write(1);  // principal 1 only
    AuditLog audit;

    MatrixRequest get = mk(ReqKind::GET); get.key = 5;
    matrix_serve(eng, p, 1, matrix_serialize_request(get), audit);            // allowed
    MatrixRequest put = mk(ReqKind::PUT); put.key = 5; put.value = 500;
    matrix_serve(eng, p, 2, matrix_serialize_request(put), audit);            // DENIED (principal 2)
    MatrixRequest q = mk(ReqKind::QUERY); q.query = MatrixQuery{.value_col = 2, .agg = AGG_SUM};
    matrix_serve(eng, p, 1, matrix_serialize_request(q), audit);              // allowed query
    std::vector<uint8_t> garbage = {1, 2, 3};
    matrix_serve(eng, p, 7, garbage, audit);                                  // malformed

    const auto& e = audit.entries();
    assert(audit.size() == 4 && "every served request is audited");
    // [0] allowed GET
    assert(e[0].principal == 1 && e[0].kind == static_cast<uint32_t>(ReqKind::GET)
           && e[0].status == static_cast<uint32_t>(ServerStatus::OK) && e[0].key == 5);
    // [1] DENIED PUT — the security point: the forbidden attempt is recorded with its principal
    assert(e[1].principal == 2 && e[1].kind == static_cast<uint32_t>(ReqKind::PUT)
           && e[1].status == static_cast<uint32_t>(ServerStatus::ERR_FORBIDDEN) && e[1].key == 5);
    // [2] allowed QUERY on column 2
    assert(e[2].principal == 1 && e[2].kind == static_cast<uint32_t>(ReqKind::QUERY)
           && e[2].status == static_cast<uint32_t>(MatrixQueryStatus::OK) && e[2].column == 2);
    // [3] malformed request — kind 0, ERR_BADREQUEST, principal still recorded
    assert(e[3].principal == 7 && e[3].kind == 0
           && e[3].status == static_cast<uint32_t>(ServerStatus::ERR_BADREQUEST));
    std::remove(wal.c_str());
    std::cout << "[audit log] ok\n";
}

int main() {
    test_audit();
    std::cout << "ALL AUDIT TESTS PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_audit.cpp -o /tmp/taud && /tmp/taud` → FAIL to compile (`AuditLog`/`AuditEntry`/the 5-arg `matrix_serve` undeclared).

- [ ] **Step 3: Add AuditEntry + AuditLog** — In `server.hpp`, add right after the `MatrixResponse` struct definition (and before `AccessPolicy`):

```cpp
// One audited request: principal, kind (ReqKind value, or 0 for a malformed request), the response
// status, and the target (GET/PUT key; QUERY value_col). The irrelevant target field is 0.
struct AuditEntry { uint64_t principal; uint32_t kind; uint32_t status; uint64_t key; uint64_t column; };

// Append-only audit trail of served requests (allowed, denied, and malformed alike).
class AuditLog {
public:
    void record(const AuditEntry& e) { entries_.push_back(e); }
    const std::vector<AuditEntry>& entries() const { return entries_; }
    size_t size() const { return entries_.size(); }
private:
    std::vector<AuditEntry> entries_;
};
```

- [ ] **Step 4: Refactor matrix_serve into serve_core + overloads** — In `server.hpp`, REPLACE the current `matrix_serve` 4-arg (authorizing) function AND its 2-arg delegate with:

```cpp
namespace matrixsrv_detail {
// The full serve pipeline (deserialize -> authorize -> dispatch), also filling `out` with the
// audit record for this request. Returns the serialized response. (Shared by all matrix_serve overloads.)
inline std::vector<uint8_t> serve_core(CPUMockEngine& eng, const AccessPolicy& policy,
                                       uint64_t principal, const std::vector<uint8_t>& req_bytes,
                                       AuditEntry& out) {
    MatrixRequest req; MatrixResponse resp;
    out.principal = principal; out.kind = 0; out.key = 0; out.column = 0;
    if (!matrix_deserialize_request(req_bytes, req)) {
        resp.status = static_cast<uint32_t>(ServerStatus::ERR_BADREQUEST);
        out.status = resp.status;
        return matrix_serialize_response(resp);
    }
    out.kind = static_cast<uint32_t>(req.kind);
    out.key = req.key;
    out.column = req.query.value_col;
    bool authorized = false;
    switch (req.kind) {
        case ReqKind::GET:   authorized = policy.can_read(principal);  break;
        case ReqKind::PUT:   authorized = policy.can_write(principal); break;
        case ReqKind::QUERY: authorized = policy.can_query(principal, req.query.value_col)
                                 && (!req.query.grouped || policy.can_query(principal, req.query.key_col)); break;
    }
    if (!authorized) {
        resp.status = static_cast<uint32_t>(ServerStatus::ERR_FORBIDDEN);
        out.status = resp.status;
        return matrix_serialize_response(resp);
    }
    switch (req.kind) {
        case ReqKind::GET: {
            uint64_t v = 0; if (eng.kv_get(req.key, v)) resp.results.push_back(v);
            resp.status = static_cast<uint32_t>(ServerStatus::OK);
            break;
        }
        case ReqKind::PUT: {
            eng.begin(); eng.txn_put(req.key, req.value); eng.commit();
            resp.status = static_cast<uint32_t>(ServerStatus::OK);
            break;
        }
        case ReqKind::QUERY: {
            std::vector<uint64_t> o;
            const MatrixQueryStatus st = eng.execute_query(req.query, o);
            resp.status = static_cast<uint32_t>(st);
            resp.results = std::move(o);
            break;
        }
    }
    out.status = resp.status;
    return matrix_serialize_response(resp);
}
} // namespace matrixsrv_detail

// Authorizing dispatch (principal supplied by the authenticated caller; denied -> ERR_FORBIDDEN
// with no engine side effects; bad request -> ERR_BADREQUEST). See SE-2.
inline std::vector<uint8_t> matrix_serve(CPUMockEngine& eng, const AccessPolicy& policy,
                                         uint64_t principal, const std::vector<uint8_t>& req_bytes) {
    AuditEntry ignored;
    return matrixsrv_detail::serve_core(eng, policy, principal, req_bytes, ignored);
}
// Same, but records the request (allowed / denied / malformed) to `audit`.
inline std::vector<uint8_t> matrix_serve(CPUMockEngine& eng, const AccessPolicy& policy,
                                         uint64_t principal, const std::vector<uint8_t>& req_bytes,
                                         AuditLog& audit) {
    AuditEntry entry;
    auto resp = matrixsrv_detail::serve_core(eng, policy, principal, req_bytes, entry);
    audit.record(entry);
    return resp;
}
// No-auth convenience (backward-compat): permissive policy, anonymous principal.
inline std::vector<uint8_t> matrix_serve(CPUMockEngine& eng, const std::vector<uint8_t>& req_bytes) {
    static const AccessPolicy permissive = AccessPolicy::permissive();
    return matrix_serve(eng, permissive, /*principal=*/0, req_bytes);
}
```

(AuditEntry is defined before serve_core; serve_core sits after the serialize functions + AccessPolicy, so all symbols it uses are in scope. The 4-arg/2-arg overloads keep their exact prior behavior — the entry is discarded.)

- [ ] **Step 5: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_audit.cpp -o /tmp/taud && /tmp/taud` → PASS, prints `[audit log] ok` + `ALL AUDIT TESTS PASSED`. No warnings.

- [ ] **Step 6: Confirm no regression** — `clang++ -std=c++20 -O2 test_server.cpp -o /tmp/tsv && /tmp/tsv | tail -1` → `ALL SERVER TESTS PASSED`; `clang++ -std=c++20 -O2 test_security.cpp -o /tmp/tsec && /tmp/tsec | tail -1` → `ALL SECURITY TESTS PASSED`; `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`. If any differ, STOP / report BLOCKED.

- [ ] **Step 7: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add server.hpp test_audit.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: audit logging — AuditLog + matrix_serve audit overload (records allowed/denied/malformed requests)"
```

---

### Task 2: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (19 tests).**
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by \
         test_query test_observability test_column_io test_catalog_snapshot test_query_validation \
         test_transactions test_server test_security test_audit; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 19× `PASS:`. If any fail, STOP / report BLOCKED.

- [ ] **Step 2: Notebook** — add `"test_audit.cpp"` to `make_notebook.py` SOURCES (after `"test_security.cpp"`); add a run cell after the access-control run cell:
```python
    md("### Audit logging\n"
       "AuditLog records every served request — allowed, denied (with the principal), and "
       "malformed — at the matrix_serve boundary; the forensic trail of who did what."),
    code("!clang++ -std=c++20 -O2 test_audit.cpp -o /tmp/taud 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_audit.cpp -o /tmp/taud; /tmp/taud"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 37 source files embedded`. Verify `grep -o "test_audit.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed audit-logging test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** AuditEntry/AuditLog (§2)→T1S3; serve_core refactor + 3 overloads (§2)→T1S4; allowed/denied/malformed all audited + non-vacuity (§3)→T1S1; NW-1/SE-2 unchanged→T1S6; suite+notebook→T2. ✓
**Placeholders:** none. **Type consistency:** `AuditEntry{principal,kind,status,key,column}`, `AuditLog::{record,entries,size}`, `matrixsrv_detail::serve_core(...)`, the 4-arg/5-arg/2-arg `matrix_serve` overloads consistent. serve_core preserves SE-2's authorize-before-dispatch (denied → no engine call, recorded as ERR_FORBIDDEN).

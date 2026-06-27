# Authorization / Access Control Implementation Plan — SE-2

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** `AccessPolicy` + an authorizing `matrix_serve` overload that denies unpermitted requests with `ERR_FORBIDDEN` before any engine call; the existing 2-arg `matrix_serve` stays permissive (backward-compat).

**Spec:** `docs/superpowers/specs/2026-06-27-access-control-design.md`

---

### Task 1: AccessPolicy + authorizing matrix_serve

**Files:** Modify `server.hpp`; Create `test_security.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_security.cpp`:

```cpp
// CPU test for authorization / access control (AccessPolicy + authorizing matrix_serve).
#include "server.hpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <string>
#include <cstdio>
#include <iostream>

static MatrixResponse serve_req(CPUMockEngine& e, const AccessPolicy& p, uint64_t principal, const MatrixRequest& r) {
    MatrixResponse resp;
    matrix_deserialize_response(matrix_serve(e, p, principal, matrix_serialize_request(r)), resp);
    return resp;
}
static MatrixRequest mk(ReqKind kind) { MatrixRequest r; r.kind = kind; return r; }

static void test_access_control() {
    const size_t N = 1000; std::vector<uint32_t> v(N), k(N);
    for (size_t i = 0; i < N; ++i) { v[i] = static_cast<uint32_t>(i); k[i] = static_cast<uint32_t>(i % 4); }
    const std::string wal = "/tmp/matrixdb_sec.bin"; std::remove(wal.c_str());
    CPUMockEngine eng(0, wal);
    eng.load_scan_column(1, k.data(), N);   // key column
    eng.load_scan_column(2, v.data(), N);   // value column
    AccessPolicy p; p.allow_column(1, 2); p.allow_read(1); p.allow_write(1);  // principal 1 granted; principal 2 nothing
    using S = ServerStatus;

    // GET: principal 1 allowed, principal 2 forbidden
    { MatrixRequest g = mk(ReqKind::GET); g.key = 5;
      assert(serve_req(eng, p, 1, g).status == static_cast<uint32_t>(S::OK));
      assert(serve_req(eng, p, 2, g).status == static_cast<uint32_t>(S::ERR_FORBIDDEN)); }

    // PUT: principal 2 denied writes NOTHING; principal 1 allowed writes
    { MatrixRequest put = mk(ReqKind::PUT); put.key = 5; put.value = 500;
      assert(serve_req(eng, p, 2, put).status == static_cast<uint32_t>(S::ERR_FORBIDDEN));
      MatrixRequest g = mk(ReqKind::GET); g.key = 5;
      assert(serve_req(eng, p, 1, g).results.empty() && "denied PUT wrote nothing");
      assert(serve_req(eng, p, 1, put).status == static_cast<uint32_t>(S::OK));
      auto after = serve_req(eng, p, 1, g);
      assert(after.results.size() == 1 && after.results[0] == 500); }

    // QUERY scalar: principal 1 on col 2 OK; principal 2 forbidden; principal 1 on col 3 forbidden (column-level)
    { MatrixRequest q = mk(ReqKind::QUERY); q.query = MatrixQuery{.value_col = 2, .agg = AGG_SUM};
      assert(serve_req(eng, p, 1, q).status == static_cast<uint32_t>(MatrixQueryStatus::OK));
      assert(serve_req(eng, p, 2, q).status == static_cast<uint32_t>(S::ERR_FORBIDDEN));
      MatrixRequest q3 = mk(ReqKind::QUERY); q3.query = MatrixQuery{.value_col = 3, .agg = AGG_SUM};
      assert(serve_req(eng, p, 1, q3).status == static_cast<uint32_t>(S::ERR_FORBIDDEN) && "col 3 not granted (authz before existence)"); }

    // QUERY grouped needs BOTH columns granted
    { MatrixRequest qg = mk(ReqKind::QUERY);
      qg.query = MatrixQuery{.value_col = 2, .agg = AGG_SUM, .grouped = true, .key_col = 1, .num_groups = 4};
      assert(serve_req(eng, p, 1, qg).status == static_cast<uint32_t>(S::ERR_FORBIDDEN) && "key col 1 not granted yet");
      p.allow_column(1, 1);
      assert(serve_req(eng, p, 1, qg).status == static_cast<uint32_t>(MatrixQueryStatus::OK) && "both cols granted -> OK"); }

    // Permissive policy allows anonymous everything (backward-compat path)
    { AccessPolicy perm = AccessPolicy::permissive(); MatrixRequest g = mk(ReqKind::GET); g.key = 5;
      assert(serve_req(eng, perm, 2, g).status == static_cast<uint32_t>(S::OK) && "permissive allows anon"); }

    std::remove(wal.c_str());
    std::cout << "[access control] ok\n";
}

int main() {
    test_access_control();
    std::cout << "ALL SECURITY TESTS PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_security.cpp -o /tmp/tsec && /tmp/tsec` → FAIL to compile (`AccessPolicy`, `ServerStatus::ERR_FORBIDDEN`, 4-arg `matrix_serve` undeclared).

- [ ] **Step 3: Add `<unordered_set>` + ERR_FORBIDDEN + AccessPolicy** — In `server.hpp`: add `#include <unordered_set>` to the includes. Change the `ServerStatus` enum to add the forbidden code:

```cpp
enum class ServerStatus : uint32_t { OK = 0, ERR_BADREQUEST = 1000, ERR_FORBIDDEN = 1001 };
```

Add the `AccessPolicy` class right after the `MatrixResponse` struct (before the `matrixsrv_detail` namespace):

```cpp
// Per-principal authorization. Grants are additive; a principal with no grants can do nothing.
// permissive() allows everything (the no-auth / backward-compat default).
class AccessPolicy {
public:
    static AccessPolicy permissive() { AccessPolicy p; p.allow_all_ = true; return p; }
    void allow_column(uint64_t principal, uint64_t col) { cols_[principal].insert(col); } // QUERY access
    void allow_read(uint64_t principal)  { read_.insert(principal); }                      // GET
    void allow_write(uint64_t principal) { write_.insert(principal); }                     // PUT
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

- [ ] **Step 4: Refactor matrix_serve into the authorizing overload + delegate** — In `server.hpp`, REPLACE the entire current `matrix_serve(CPUMockEngine&, const std::vector<uint8_t>&)` function with:

```cpp
// Authorizing dispatch: deserialize -> authorize the principal for this request -> run -> serialize.
// A bad request -> ERR_BADREQUEST; an unauthorized one -> ERR_FORBIDDEN (no engine call, no side
// effects); else the NW-1 dispatch. `principal` is supplied by the authenticated caller (NOT the
// payload). GET needs read, PUT needs write, QUERY needs query-access to value_col (+ key_col if grouped).
inline std::vector<uint8_t> matrix_serve(CPUMockEngine& eng, const AccessPolicy& policy,
                                         uint64_t principal, const std::vector<uint8_t>& req_bytes) {
    MatrixRequest req; MatrixResponse resp;
    if (!matrix_deserialize_request(req_bytes, req)) {
        resp.status = static_cast<uint32_t>(ServerStatus::ERR_BADREQUEST);
        return matrix_serialize_response(resp);
    }
    bool authorized = false;
    switch (req.kind) {
        case ReqKind::GET:   authorized = policy.can_read(principal);  break;
        case ReqKind::PUT:   authorized = policy.can_write(principal); break;
        case ReqKind::QUERY: authorized = policy.can_query(principal, req.query.value_col)
                                 && (!req.query.grouped || policy.can_query(principal, req.query.key_col)); break;
    }
    if (!authorized) {
        resp.status = static_cast<uint32_t>(ServerStatus::ERR_FORBIDDEN);
        return matrix_serialize_response(resp);   // denied before any engine call -> no side effects
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
            std::vector<uint64_t> out;
            const MatrixQueryStatus st = eng.execute_query(req.query, out);
            resp.status = static_cast<uint32_t>(st);
            resp.results = std::move(out);
            break;
        }
    }
    return matrix_serialize_response(resp);
}

// No-auth convenience (backward-compat): serve with a permissive policy as the anonymous principal.
inline std::vector<uint8_t> matrix_serve(CPUMockEngine& eng, const std::vector<uint8_t>& req_bytes) {
    static const AccessPolicy permissive = AccessPolicy::permissive();
    return matrix_serve(eng, permissive, /*principal=*/0, req_bytes);
}
```

- [ ] **Step 5: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_security.cpp -o /tmp/tsec && /tmp/tsec` → PASS, prints `[access control] ok` + `ALL SECURITY TESTS PASSED`. No warnings. Also `-DNDEBUG` build runs clean (no crash on forbidden paths).

- [ ] **Step 6: Confirm no regression** — `clang++ -std=c++20 -O2 test_server.cpp -o /tmp/tsv && /tmp/tsv | tail -1` → `ALL SERVER TESTS PASSED` (the 2-arg `matrix_serve` is still permissive, NW-1 tests unchanged); `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`.

- [ ] **Step 7: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add server.hpp test_security.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: authorization — AccessPolicy + authorizing matrix_serve (column-level + read/write grants, ERR_FORBIDDEN)"
```

---

### Task 2: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (18 tests).**
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by \
         test_query test_observability test_column_io test_catalog_snapshot test_query_validation \
         test_transactions test_server test_security; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 18× `PASS:`. If any fail, STOP / report BLOCKED.

- [ ] **Step 2: Notebook** — add `"test_security.cpp"` to `make_notebook.py` SOURCES (after `"test_server.cpp"`); add a run cell after the server-core run cell:
```python
    md("### Authorization / access control\n"
       "AccessPolicy enforces per-principal column-level query access + read/write grants at the "
       "matrix_serve boundary; unpermitted requests are ERR_FORBIDDEN with no engine side-effects."),
    code("!clang++ -std=c++20 -O2 test_security.cpp -o /tmp/tsec 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_security.cpp -o /tmp/tsec; /tmp/tsec"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 36 source files embedded`. Verify `grep -o "test_security.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed access-control test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** AccessPolicy + permissive (§3)→T1S3; ERR_FORBIDDEN + authorizing 4-arg matrix_serve + 2-arg delegate (§3)→T1S3/S4; grant/deny matrix incl. column-level + grouped-both-cols + denied-no-side-effect + permissive (§4)→T1S1; NW-1 unchanged→T1S6; suite+notebook→T2. ✓
**Placeholders:** none. **Type consistency:** `AccessPolicy::{permissive,allow_column,allow_read,allow_write,can_query,can_read,can_write}`, `ServerStatus::ERR_FORBIDDEN`, `matrix_serve(CPUMockEngine&, const AccessPolicy&, uint64_t, bytes)` + 2-arg delegate consistent T1/T2. Reuses MatrixRequest/Response/serialize (NW-1), execute_query/kv_get/begin/txn_put/commit. Authz runs BEFORE dispatch (denied → no engine call).

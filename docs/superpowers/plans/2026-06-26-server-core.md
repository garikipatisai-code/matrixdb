# Server Core Implementation Plan — NW-1

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** A serializable request/response protocol + a stateless `matrix_serve(engine, bytes) → bytes` dispatcher — the engine becomes request-serveable. Transport (TCP) is a deferred thin adapter.

**Spec:** `docs/superpowers/specs/2026-06-26-server-core-design.md`

---

### Task 1: server.hpp (protocol + serialize + matrix_serve) + test

**Files:** Create `server.hpp`, `test_server.cpp`.

- [ ] **Step 1: Write the failing test** — Create `test_server.cpp`:

```cpp
// CPU test for the server core (request/response protocol + matrix_serve dispatch).
#include "server.hpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <cstdio>
#include <iostream>

static void test_request_roundtrip() {
    MatrixRequest r; r.kind = ReqKind::QUERY; r.key = 7; r.value = 70;
    r.query = MatrixQuery{.value_col=2, .agg=AGG_SUM, .has_filter=true, .threshold=5,
                          .grouped=true, .key_col=1, .num_groups=4};
    auto b = matrix_serialize_request(r);
    MatrixRequest r2;
    assert(matrix_deserialize_request(b, r2));
    assert(r2.kind == ReqKind::QUERY && r2.key == 7 && r2.value == 70);
    assert(r2.query.value_col == 2 && r2.query.agg == AGG_SUM && r2.query.has_filter
           && r2.query.threshold == 5 && r2.query.grouped && r2.query.key_col == 1 && r2.query.num_groups == 4);
    b.pop_back(); assert(!matrix_deserialize_request(b, r2) && "truncated request rejected");
    std::cout << "[request round-trip] ok\n";
}

static void test_response_roundtrip() {
    MatrixResponse r; r.status = 3; r.results = {10, 20, 30};
    auto b = matrix_serialize_response(r);
    MatrixResponse r2;
    assert(matrix_deserialize_response(b, r2) && r2.status == 3 && (r2.results == std::vector<uint64_t>{10,20,30}));
    MatrixResponse e; auto be = matrix_serialize_response(e);   // empty results, status 0
    MatrixResponse e2; assert(matrix_deserialize_response(be, e2) && e2.status == 0 && e2.results.empty());
    b.pop_back(); assert(!matrix_deserialize_response(b, r2) && "truncated response rejected");
    std::cout << "[response round-trip] ok\n";
}

static void test_serve_get_put() {
    const std::string wal = "/tmp/matrixdb_srv.bin"; std::remove(wal.c_str());
    {
        CPUMockEngine eng(0, wal);
        MatrixRequest put; put.kind = ReqKind::PUT; put.key = 5; put.value = 500;
        MatrixResponse pr; assert(matrix_deserialize_response(matrix_serve(eng, matrix_serialize_request(put)), pr) && pr.status == 0);
        MatrixRequest get; get.kind = ReqKind::GET; get.key = 5;
        MatrixResponse gr; assert(matrix_deserialize_response(matrix_serve(eng, matrix_serialize_request(get)), gr));
        assert(gr.results.size() == 1 && gr.results[0] == 500);
        MatrixRequest miss; miss.kind = ReqKind::GET; miss.key = 999;
        MatrixResponse mr; assert(matrix_deserialize_response(matrix_serve(eng, matrix_serialize_request(miss)), mr) && mr.results.empty());
    }
    {   // durability: a fresh engine on the same WAL replays the PUT (it went through commit)
        CPUMockEngine eng2(0, wal);
        MatrixRequest get; get.kind = ReqKind::GET; get.key = 5;
        MatrixResponse gr; assert(matrix_deserialize_response(matrix_serve(eng2, matrix_serialize_request(get)), gr));
        assert(gr.results.size() == 1 && gr.results[0] == 500 && "PUT durable through the wire");
    }
    std::remove(wal.c_str());
    std::cout << "[serve get/put + durable] ok\n";
}

static void test_serve_query() {
    const size_t N = 1000; std::vector<uint32_t> col(N);
    for (size_t i = 0; i < N; ++i) col[i] = static_cast<uint32_t>(i);
    CPUMockEngine eng(0, "", SIZE_MAX); eng.load_scan_column(2, col.data(), N);
    MatrixRequest q; q.kind = ReqKind::QUERY; q.query = MatrixQuery{.value_col = 2, .agg = AGG_SUM};
    MatrixResponse r; assert(matrix_deserialize_response(matrix_serve(eng, matrix_serialize_request(q)), r));
    assert(r.status == 0 && r.results.size() == 1);
    std::vector<uint64_t> direct; eng.execute_query(MatrixQuery{.value_col = 2, .agg = AGG_SUM}, direct);
    assert(r.results == direct && "serve QUERY == direct execute_query");
    MatrixRequest bad; bad.kind = ReqKind::QUERY; bad.query = MatrixQuery{.value_col = 999, .agg = AGG_SUM};
    MatrixResponse br; assert(matrix_deserialize_response(matrix_serve(eng, matrix_serialize_request(bad)), br));
    assert(br.status == static_cast<uint32_t>(MatrixQueryStatus::ERR_UNKNOWN_COLUMN) && br.results.empty());
    std::cout << "[serve query] ok\n";
}

static void test_bad_request() {
    CPUMockEngine eng(0, "", SIZE_MAX);
    std::vector<uint8_t> garbage = {1, 2, 3};   // too short to be a request
    MatrixResponse r; assert(matrix_deserialize_response(matrix_serve(eng, garbage), r));
    assert(r.status == static_cast<uint32_t>(ServerStatus::ERR_BADREQUEST) && "bad request -> ERR_BADREQUEST, no crash");
    std::cout << "[bad request] ok\n";
}

int main() {
    test_request_roundtrip();
    test_response_roundtrip();
    test_serve_get_put();
    test_serve_query();
    test_bad_request();
    std::cout << "ALL SERVER TESTS PASSED\n";
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — `cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB && clang++ -std=c++20 -O2 test_server.cpp -o /tmp/tsv && /tmp/tsv` → FAIL to compile (`server.hpp` missing).

- [ ] **Step 3: Create server.hpp** — Create `server.hpp` with EXACTLY:

```cpp
#pragma once
// Server core: a serializable request/response protocol + a stateless dispatcher over the engine.
// Transport-agnostic — a TCP/Unix-socket accept-loop is a thin adapter that shuttles these byte
// buffers (deferred; not buildable in this sandbox). Wire form is host-endian (same-machine /
// trusted-transport assumption, like column_io); a cross-arch encoding is a future upgrade.
#include "compute_mock.cpp"   // CPUMockEngine + compute.hpp (MatrixQuery/MatrixQueryStatus/MatrixAggOp)
#include <cstdint>
#include <cstring>
#include <vector>

enum class ReqKind : uint32_t { GET = 1, PUT = 2, QUERY = 3 };
// Status ranges a client can disambiguate: 0 = OK (any kind); 1..3 = MatrixQueryStatus errors
// (QUERY); 1000 = bad/un-deserializable request.
enum class ServerStatus : uint32_t { OK = 0, ERR_BADREQUEST = 1000 };

struct MatrixRequest {
    ReqKind  kind  = ReqKind::GET;
    uint64_t key   = 0;          // GET/PUT
    uint64_t value = 0;          // PUT
    MatrixQuery query{};         // QUERY
};
struct MatrixResponse {
    uint32_t status = 0;
    std::vector<uint64_t> results;   // GET: {value} or {}; PUT: {}; QUERY: the aggregate/group vector
};

namespace matrixsrv_detail {
    inline void put_u32(std::vector<uint8_t>& b, uint32_t v) { const uint8_t* p = reinterpret_cast<const uint8_t*>(&v); b.insert(b.end(), p, p + 4); }
    inline void put_u64(std::vector<uint8_t>& b, uint64_t v) { const uint8_t* p = reinterpret_cast<const uint8_t*>(&v); b.insert(b.end(), p, p + 8); }
    struct Reader {
        const uint8_t* p; const uint8_t* end; bool ok = true;
        bool u8(uint8_t& o)   { if (end - p < 1) { ok = false; return false; } o = *p++; return true; }
        bool u32(uint32_t& o) { if (end - p < 4) { ok = false; return false; } std::memcpy(&o, p, 4); p += 4; return true; }
        bool u64(uint64_t& o) { if (end - p < 8) { ok = false; return false; } std::memcpy(&o, p, 8); p += 8; return true; }
        bool done() const { return p == end; }
    };
}

inline std::vector<uint8_t> matrix_serialize_request(const MatrixRequest& r) {
    using namespace matrixsrv_detail;
    std::vector<uint8_t> b;
    put_u32(b, static_cast<uint32_t>(r.kind));
    put_u64(b, r.key); put_u64(b, r.value);
    put_u64(b, r.query.value_col);
    put_u32(b, static_cast<uint32_t>(r.query.agg));
    b.push_back(r.query.has_filter ? 1 : 0);
    put_u32(b, r.query.threshold);
    b.push_back(r.query.grouped ? 1 : 0);
    put_u64(b, r.query.key_col);
    put_u32(b, r.query.num_groups);
    return b;
}
inline bool matrix_deserialize_request(const std::vector<uint8_t>& b, MatrixRequest& out) {
    using namespace matrixsrv_detail;
    Reader r{ b.data(), b.data() + b.size() };
    uint32_t kind = 0, agg = 0; uint8_t hf = 0, gr = 0;
    r.u32(kind); r.u64(out.key); r.u64(out.value);
    r.u64(out.query.value_col); r.u32(agg); r.u8(hf); r.u32(out.query.threshold);
    r.u8(gr); r.u64(out.query.key_col); r.u32(out.query.num_groups);
    if (!r.ok || !r.done()) return false;          // truncated or trailing garbage
    if (kind < 1 || kind > 3) return false;         // unknown kind
    out.kind = static_cast<ReqKind>(kind);
    out.query.agg = static_cast<MatrixAggOp>(agg);
    out.query.has_filter = (hf != 0);
    out.query.grouped = (gr != 0);
    return true;
}
inline std::vector<uint8_t> matrix_serialize_response(const MatrixResponse& r) {
    using namespace matrixsrv_detail;
    std::vector<uint8_t> b;
    put_u32(b, r.status);
    put_u64(b, static_cast<uint64_t>(r.results.size()));
    for (uint64_t x : r.results) put_u64(b, x);
    return b;
}
inline bool matrix_deserialize_response(const std::vector<uint8_t>& b, MatrixResponse& out) {
    using namespace matrixsrv_detail;
    Reader r{ b.data(), b.data() + b.size() };
    uint64_t count = 0;
    if (!r.u32(out.status) || !r.u64(count)) return false;
    if (count > (1ull << 28)) return false;                          // MAX_RESULTS guard
    if (static_cast<uint64_t>(r.end - r.p) != count * 8) return false; // exact remaining length
    out.results.resize(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) r.u64(out.results[i]);
    return r.ok && r.done();
}

// Stateless dispatch: deserialize -> run on the engine -> serialize. Never crashes on a bad
// request (returns ERR_BADREQUEST). PUT goes through the transaction/commit path (durable,
// arbitrary value); GET reads kv_; QUERY runs execute_query (its MatrixQueryStatus flows back).
inline std::vector<uint8_t> matrix_serve(CPUMockEngine& eng, const std::vector<uint8_t>& req_bytes) {
    MatrixRequest req; MatrixResponse resp;
    if (!matrix_deserialize_request(req_bytes, req)) {
        resp.status = static_cast<uint32_t>(ServerStatus::ERR_BADREQUEST);
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
            std::vector<uint64_t> out;
            const MatrixQueryStatus st = eng.execute_query(req.query, out);
            resp.status = static_cast<uint32_t>(st);
            resp.results = std::move(out);
            break;
        }
    }
    return matrix_serialize_response(resp);
}
```

- [ ] **Step 4: Run to verify it passes** — `clang++ -std=c++20 -O2 -Wall -Wextra test_server.cpp -o /tmp/tsv && /tmp/tsv` → PASS, prints all five `ok` lines + `ALL SERVER TESTS PASSED`. No warnings. Also build with `-DNDEBUG` and run to confirm the bad-request path is release-safe: `clang++ -std=c++20 -O2 -DNDEBUG test_server.cpp -o /tmp/tsv_r && /tmp/tsv_r` → same PASS, no crash.

- [ ] **Step 5: Confirm no regression** — `clang++ -std=c++20 -O3 -mcpu=apple-m1 main.cpp -o /tmp/mdb && /tmp/mdb 2>&1 | grep "Scan result sum"` → `83886070 (oracle 83886070)`; `clang++ -std=c++20 -O2 test_transactions.cpp -o /tmp/ttx && /tmp/ttx | tail -1` → `ALL TRANSACTION TESTS PASSED` (PUT uses the txn path). server.hpp is additive (a new file; the engine is untouched).

- [ ] **Step 6: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add server.hpp test_server.cpp
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "feat: server core — request/response protocol + matrix_serve dispatcher (transport-agnostic; engine is now request-serveable)"
```

---

### Task 2: Regression + notebook

**Files:** Modify `make_notebook.py`; Regenerate `matrixdb_colab.ipynb`.

- [ ] **Step 1: Full CPU suite (17 tests).**
```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
for t in test_kv_store test_cost_model test_tier_manager test_cold_store test_engine_restart \
         test_migration test_scan_coverage test_live_tiering test_aggregations test_group_by \
         test_query test_observability test_column_io test_catalog_snapshot test_query_validation \
         test_transactions test_server; do
  clang++ -std=c++20 -O2 "$t.cpp" -o "/tmp/$t" 2>/dev/null && "/tmp/$t" >/tmp/out_$t 2>&1 && echo "PASS: $t" || echo "FAIL: $t"
done
```
Expected: 17× `PASS:`. If any fail, STOP / report BLOCKED.

- [ ] **Step 2: Notebook** — add `"server.hpp"` to `make_notebook.py` SOURCES (with the header group, e.g. after `"column_io.hpp"`) AND `"test_server.cpp"` (after `"test_transactions.cpp"`). Add a run cell after the transactions run cell:
```python
    md("### Server core (request/response protocol)\n"
       "A serializable GET/PUT/QUERY request + matrix_serve dispatcher make the engine "
       "request-serveable; serialize->serve->deserialize round-trips equal direct engine calls. "
       "(The TCP accept-loop adapter runs on a non-sandboxed machine.)"),
    code("!clang++ -std=c++20 -O2 test_server.cpp -o /tmp/tsv 2>/dev/null "
         "|| g++ -std=c++20 -O2 test_server.cpp -o /tmp/tsv; /tmp/tsv"),
```
Then `python3 make_notebook.py` → expect `wrote matrixdb_colab.ipynb: <N> cells, 35 source files embedded` (33 + server.hpp + test_server.cpp). Verify `grep -o "test_server.cpp" matrixdb_colab.ipynb | wc -l` → `>= 2`.

- [ ] **Step 3: Commit**

```bash
cd /Users/saikrishna.2177481/Documents/Spike/MatrixDB
git add make_notebook.py matrixdb_colab.ipynb
git -c user.name=garikipatisai-code -c user.email=garikipatisai-code@users.noreply.github.com commit -m "chore: embed server-core test in Colab notebook"
```

---

## Self-Review
**Spec coverage:** ReqKind/ServerStatus/MatrixRequest/MatrixResponse + serialize/deserialize + MAX_RESULTS guard (§2)→T1S3; matrix_serve GET/PUT/QUERY/bad-request dispatch (§1/§3)→T1S3; round-trips + serve==direct + durable PUT + bad-request + non-vacuity (§4)→T1S1; suite+notebook→T2. ✓
**Placeholders:** none. **Type consistency:** `ReqKind`, `ServerStatus`, `MatrixRequest`/`MatrixResponse`, `matrix_serialize_request`/`matrix_deserialize_request`/`..._response`, `matrix_serve(CPUMockEngine&, const std::vector<uint8_t>&)` consistent T1/T2. Reuses `kv_get`/`begin`/`txn_put`/`commit`/`execute_query`/`MatrixQuery`/`MatrixQueryStatus`/`AGG_SUM` (all existing). server.hpp `#include "compute_mock.cpp"`; test_server.cpp includes only server.hpp (single inclusion).

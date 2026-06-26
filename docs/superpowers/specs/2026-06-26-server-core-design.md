# Design: Server Core — request/response protocol + dispatcher — NW-1

**Status:** approved-by-standing-directive (user: "you pick / keep going"), pre-implementation. **Date:** 2026-06-26.
**Builds on:** the engine's public API — `kv_get` (read), `begin/txn_put/commit` (durable write), `execute_query`/`MatrixQuery`/`MatrixQueryStatus` (analytics).
**Fully local: the protocol + dispatch are pure logic, tested in-process. Only the TCP `accept` loop is sandbox-blocked (proven) — it's a thin adapter for a non-sandboxed machine.**

**Thesis:** *The engine answers calls in-process; a database answers requests from clients. Add a
serializable request/response wire form + a stateless `matrix_serve(engine, bytes) → bytes`
dispatcher. That is the "engine → server" core — testable end-to-end here (serialize → serve →
deserialize round-trips against direct engine calls); a TCP/Unix-socket accept-loop becomes a thin
adapter that just shuttles the bytes.*

---

## 1. Scope

**IN (new header `server.hpp` + `test_server.cpp`):**
- `enum class ReqKind : uint32_t { GET = 1, PUT = 2, QUERY = 3 };`
- `struct MatrixRequest { ReqKind kind; uint64_t key=0, value=0; MatrixQuery query{}; };`
  (flat fixed layout — all fields always present; unused per-kind fields are 0. Simpler + more
  robust than a variable tagged union.)
- `struct MatrixResponse { uint32_t status=0; std::vector<uint64_t> results; };`
  (status: 0=OK for GET/PUT; for QUERY the `MatrixQueryStatus` as u32. results: GET → `{value}` if
  found else `{}`; PUT → `{}`; QUERY → the aggregate/group vector.)
- `matrix_serialize_request` / `matrix_deserialize_request` and `..._response` — binary, host-endian
  (documented, like `column_io`), length-prefixed where variable. Fail-loud on a malformed/truncated
  buffer (return false / a clear error — never read OOB).
- `std::vector<uint8_t> matrix_serve(CPUMockEngine& eng, const std::vector<uint8_t>& req_bytes)` —
  deserialize → dispatch to the engine → serialize the response:
  - GET → `eng.kv_get(key, v)` → OK + `{v}` (found) or `{}` (miss).
  - PUT → `eng.begin(); eng.txn_put(key, value); eng.commit()` (durable, arbitrary value — the txn
    path, unlike OP_WRITE's value==key mock) → OK.
  - QUERY → `eng.execute_query(query, out)` → status + `out`.
  - A request that fails to deserialize → a response with an `ERR_BADREQUEST` status (never crash).

**OUT (deferred):** the literal TCP/Unix-socket transport (`accept`/`recv`/`send` loop) — blocked
in this sandbox, a thin adapter elsewhere; concurrent multi-threaded serving (thread-safety is a
separate axis — `matrix_serve` is stateless per request, so a real server serializes or shards
calls); a LOAD-column-over-wire request (loading stays an admin/in-process op); auth (the SE axis);
TLS; richer request kinds / streaming results / batched requests.

---

## 2. Wire format (binary, host-endian — same-machine/trusted-transport assumption, documented)

**Request** (fixed length): `[u32 kind][u64 key][u64 value]` then the serialized `MatrixQuery`:
`[u64 value_col][u32 agg][u8 has_filter][u32 threshold][u8 grouped][u64 key_col][u32 num_groups]`.
Deserialize validates the buffer is exactly the expected length (else bad request).

**Response** (variable): `[u32 status][u64 count][count × u64 results]`. Deserialize validates
`count` against the remaining buffer length (reject if short — no OOB read).

A `MAX_RESULTS` cap (e.g. 1u<<28) on the response `count` guards a malformed/huge length on
deserialize (consistent with VAL-1's `MAX_QUERY_GROUPS`).

---

## 3. Dispatch & robustness

`matrix_serve` is **stateless** (no per-connection state) — each request is self-contained, so a
transport adapter can hand requests from many clients to one `matrix_serve` (serialized) or to
sharded engine instances. Robustness: a request whose bytes don't deserialize (wrong length,
unknown kind) yields `MatrixResponse{status=ERR_BADREQUEST}` — the server never crashes on bad
input (the wire analog of VAL-1's query-boundary validation). `execute_query`'s own validation
still applies for QUERY (its status flows back in the response).

```cpp
enum class ServerStatus : uint32_t { OK = 0, ERR_BADREQUEST = 1000 }; // QUERY uses MatrixQueryStatus values
```
(QUERY maps `MatrixQueryStatus` → its integer value in `status`; GET/PUT use `OK`; a bad request uses
`ERR_BADREQUEST`. Distinct ranges so a client can tell them apart — documented.)

---

## 4. Verification (`test_server.cpp`, CPU)

- **Serialize/deserialize round-trips (no engine):** a GET/PUT/QUERY request → bytes → request
  equals the original (all fields); a response (various statuses, result vectors incl. empty) →
  bytes → response equals the original. A truncated/oversized buffer → deserialize fails cleanly.
- **serve == direct engine call:** with a loaded engine — `matrix_serve` a PUT(5, 500), then a
  GET(5) → response `{500}`; a GET(missing) → `{}`. A QUERY (SUM over a loaded column) → response
  status OK + results equal to a direct `eng.execute_query`. A grouped QUERY → results equal the
  direct grouped result.
- **Durability through the wire:** a PUT via `matrix_serve` is durable (it commits) — a fresh
  engine on the same WAL replays it (proves PUT went through the transaction/commit path).
- **Bad request:** a deliberately truncated/garbage request buffer → `matrix_serve` returns
  `ERR_BADREQUEST`, no crash (also under `-DNDEBUG`).
- **Non-vacuity:** the QUERY-via-serve result differs across two different queries (so a stub
  echoing a fixed response fails); the GET-after-PUT proves the write actually applied.

Plus: oracle `83886070` unchanged (server.hpp is additive, engine untouched); all existing tests
green; notebook regenerated with `server.hpp` + `test_server.cpp`.

---

## 5. Open / deferred
- The TCP/Unix-socket accept-loop adapter (run on a non-sandboxed machine): `bind/listen/accept`,
  `recv` a length-prefixed request frame → `matrix_serve` → `send` the response frame.
- Concurrent serving (thread-safety / connection pool); a LOAD-over-wire + admin ops; auth (SE) +
  TLS; request batching / streaming; an endianness-portable wire encoding for cross-arch clients.

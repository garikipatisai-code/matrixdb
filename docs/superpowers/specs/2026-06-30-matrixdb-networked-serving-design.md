# matrixdb Networked Serving — Design & Scope

**Date:** 2026-06-30  **Status:** design + the achievable artifacts (daemon, auth accept loop, wire tests)
landed; live `bind`/`accept` and TLS are environment-gated and documented for pickup on a real host.
**Goal:** Scope the path from the existing serve primitives to a deployable, client-facing network server —
build everything verifiable in this sandbox, and write down precisely what remains (and how to finish it)
where a real socket or a vetted TLS library is required.

## What already exists (inventory — ~80% built)

The networked stack is largely present and tested; this effort wires it into a runnable daemon and scopes
the rest.

- **Wire protocol** (`server.hpp`): `ReqKind { GET=1, PUT=2, QUERY=3, HEALTH=4, STATS=5 }`,
  `ServerStatus { OK=0, ERR_BADREQUEST=1000, ERR_FORBIDDEN=1001, ERR_UNAUTHENTICATED=1002 }`,
  `matrix_serialize_request` / `matrix_serve` (dispatch + serialize response). Tested: `test_server.cpp`.
- **Authn/authz/audit**: `Authenticator` (token → principal), `AccessPolicy` (per-principal GET/PUT/QUERY
  grants, additive; `permissive()` for dev), an append-only audit trail. Tested: `test_security.cpp`,
  `test_audit.cpp`.
- **Transport** (`server_tcp.hpp`): length-prefixed framing `[u32 len][bytes]`; `matrix_serve_conn` (one
  framed request), `matrix_serve_conn_auth` (token-frame handshake → serve as principal), recv/send timeouts
  (NW-5, slowloris/slow-reader), `SO_NOSIGPIPE`/`MSG_NOSIGNAL`. Tested over a **socketpair**:
  `test_server_tcp.cpp`, `test_recv_timeout.cpp`.
- **Accept loops**: `matrix_serve_tcp` (no-auth) and **`matrix_serve_tcp_auth`** (this effort — per-connection
  authentication). HOST-ONLY: `bind` is blocked in the sandbox, so the loops are **compile-verified**; the
  per-connection auth+serve path underneath is socketpair-tested.
- **Client** (`client.hpp`): `MatrixClient` — `authenticate(token)`, `get/put/query`. Tested: `test_client.cpp`.
- **Ops adjacent**: admission control (RM-2), graceful shutdown (RM-4), leveled logging (OB-1), health/stats
  over the protocol, build version (BP-3).

## The daemon (`matrixdbd.cpp`) — built here

A thin `main` wiring the engine + policy + authenticator into an accept loop:

```
matrixdbd <port> [--open <snapshot>] [--token <token>]
  ./matrixdbd 7070 --open mydata.db --token s3cret   # token auth, serving a saved catalog
  ./matrixdbd 7070                                    # dev mode: permissive, empty catalog
```

`--open` restores a `.save`d catalog to serve; `--token` enables single-principal token auth (principal 1
granted read/write + QUERY on every loaded column) via `matrix_serve_tcp_auth`; no token → `permissive` +
`matrix_serve_tcp`. `SIGPIPE` ignored. Compile-verified in CI (`run_tests.sh`: `matrixdbd: OK`); on the
sandbox `bind` returns −1 and it exits with a clear message; on a real host it serves.

## Wire protocol (spec for client implementers)

```
Connection (auth mode):  [u32 tlen][token bytes]            -- once, first
Then per request:        [u32 len][request bytes]   client -> server
                         [u32 len][response bytes]  server -> client
len is little-endian; the server caps it at 2^28 (never allocs on a bogus length).
request bytes  = matrix_serialize_request(MatrixRequest{ kind, key, value, query })
response bytes = [u32 status] + kind-specific payload (see matrix_serve in server.hpp)
```
A bad/empty/unknown token frame → the server serves nothing and closes the connection
(`ERR_UNAUTHENTICATED` semantics at the connection level). `AccessPolicy` still gates every request after
authentication; a denied request → `ERR_FORBIDDEN`. `MatrixClient` implements the client half.

## Plan for the gated parts

### TLS (needs a vetted library — do not hand-roll)
The only secrecy gap. Slots in **between `accept` and `matrix_serve_conn_auth`**: wrap the accepted fd in a
TLS session and route `recv_all`/`send_all` through it.
- **Seam:** introduce an `IoChannel` interface (`read_all`/`write_all`) with two impls — `FdChannel` (today's
  raw `recv`/`send`) and `TlsChannel` (library read/write). `matrix_serve_conn*` take an `IoChannel&` instead
  of an `int fd`; the socketpair tests use `FdChannel`. This is a small, mechanical refactor — pure plumbing,
  no protocol change — and keeps the crypto isolated.
- **Library:** OpenSSL (ubiquitous) or mbedTLS (lighter, easier to vendor); server cert + key from config.
- **Interim (no code):** terminate TLS at a reverse proxy (nginx/Caddy/stunnel) in front of `matrixdbd` on
  loopback — gives encryption today without touching the server. Document this as the recommended v1.

### Concurrency (needs a real `accept`; logic is testable)
Today both accept loops serve **one connection at a time** (single-owner). For multiple clients:
- Thread-per-connection (or a small bounded pool, gated by admission control): each connection runs
  `matrix_serve_conn_auth` on its own thread. **Reads are already safe to run concurrently** — route QUERY
  through `execute_query_shared` under the `ConcurrentServer` shared lock (built + TSan-verified); writes
  (PUT) serialize on the exclusive lock. This reuses the proven concurrent-reads model; no new concurrency
  thesis.
- Verifiable here: the per-connection handler + the shared/exclusive dispatch (socketpair + threads, like the
  existing TSan tests). Gated: the live multi-accept dispatch.

### Deployment / ops
Run on a host; `systemd` unit (Restart=on-failure); health/readiness via the HEALTH request; metrics via
STATS; structured logs (OB-1) to stderr→journal; graceful shutdown (RM-4) on SIGTERM. Resource caps: the
recv/send timeouts (NW-5) + admission control (RM-2) bound a single stuck or abusive client; a connection cap
+ max-frame size round it out.

## Verified here vs. gated

| Piece | Status |
|---|---|
| Wire protocol, serialize/serve, authz/audit | ✅ runtime-tested |
| Per-connection auth + serve (`matrix_serve_conn_auth`) | ✅ socketpair-tested (this effort) |
| `matrix_serve_tcp_auth` accept loop, `matrixdbd` daemon | ✅ compile-verified; graceful on a blocked `bind` |
| Live `bind`/`accept`, end-to-end `MatrixClient`↔`matrixdbd` | ⛔ host-only — bind blocked in sandbox |
| TLS | ⛔ needs a vetted library (seam designed above) |
| Concurrent connections | ⚙️ model proven (`ConcurrentServer`); accept-dispatch host-only |

## Verify-on-host checklist (pickup outside the sandbox)

1. `./matrixdbd 7070 --open demo.db --token T` then `MatrixClient` round-trip over loopback (GET/PUT/QUERY/
   HEALTH/STATS); confirm `ERR_UNAUTHENTICATED` on a bad token and `ERR_FORBIDDEN` on a denied request.
2. Slowloris: a client that stalls mid-frame is dropped after the recv timeout; the server keeps serving.
3. Concurrency increment: thread-per-connection + `execute_query_shared`; load-test N readers + 1 writer,
   re-run under TSan.
4. TLS: implement `TlsChannel`, verify a handshake + an encrypted round-trip; or stand up the reverse-proxy
   interim.

## Scope & non-goals

**In (built):** `matrix_serve_tcp_auth`, `matrixdbd`, the auth-over-wire socketpair test, the CI compile-check,
this design + wire spec. **Deferred (designed, gated):** TLS, concurrent connections, the live end-to-end
test — each with a concrete plan above. **Out:** multi-writer/MVCC (contra-thesis), a custom crypto stack.

## Success criteria

A new engineer can read this to (a) run `matrixdbd` on a host and talk to it with `MatrixClient`, (b)
implement a client in any language from the wire spec, and (c) finish TLS + concurrency along the designed
seams — with the in-sandbox-verifiable parts already built, tested, and CI-gated.

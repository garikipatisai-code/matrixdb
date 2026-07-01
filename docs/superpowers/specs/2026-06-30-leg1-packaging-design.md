# Leg 1 Packaging — Design & Scope

**Date:** 2026-06-30  **Status:** design, pending implementation

**Goal:** Close the specific, already-identified gaps between "the engine works" and "a real,
shippable app" — per `PRODUCTION_READINESS.md`'s own register (BP-1 build system, BP-2 packaging
artifact) and the network+GPU spike's confirmed-still-open side-finding (Nagle/`TCP_NODELAY` on
accepted sockets). This is Leg 1 of the roadmap: package MatrixDB so it can be built, shipped, and
run as a standalone binary/container, before any Java/Spring Boot dependency work (Leg 2) starts.

## Why this scope, not a bigger one

`PRODUCTION_READINESS.md` already tracks the honest gap register for the whole project, and most
of Phases A/B (correctness, durability, transactions, data model, auth/authz/audit, concurrent
reads) are **landed**. What's actually missing for "package it as a real app" is narrow:

- **BP-1** (no real build system — `cmake` isn't even used; `build.sh` only builds the CLI, not
  the daemon or the benchmark binary).
- **BP-2** (no packaging artifact at all — no container, no OS package, no release script).
- The spike's side-finding: `server_tcp.hpp`'s accept loops (`matrix_serve_tcp`,
  `matrix_serve_tcp_auth`) never set `TCP_NODELAY` on the accepted socket — the same stall the
  spike hit and fixed in its own throwaway server, left unfixed in the real one.

Everything else the register lists as open (TLS/SE-3, remote CI/QA-6, deb/rpm/Homebrew, HA) is
either explicitly deferred by the register's own "single-box wedge" scoping or disproportionate to
what "package it" requires — a Docker image is the standard, minimal way to ship a single-box
database today (Postgres, Redis, etc. all lead with a container image, not OS packages). Converting
the unused `CMakeLists.txt` into the real build system is a bigger lift than extending the
`build.sh`/`run_tests.sh` shell pattern already proven in this repo, for no capability gain at this
size — out of scope here, noted as a deferred alternative in the register.

## Scope

**In:**
1. **NW hardening — `TCP_NODELAY` on accepted sockets.** Fix in `server_tcp.hpp`'s
   `matrix_serve_tcp` and `matrix_serve_tcp_auth`, right after `::accept`. Every served response is
   two `send_all()` calls (length prefix, then payload) — without `TCP_NODELAY`, Nagle holds the
   second send for the peer's delayed-ACK timer (~40ms), the exact stall the spike diagnosed and
   fixed in `spike/spike_server.cpp`. Not runtime-testable here (`bind` is sandbox-blocked) — the
   fix is one `setsockopt` call per accept loop, verified by compile + the existing
   `test_server_tcp.cpp` socketpair tests still passing (they don't touch `accept`, so they're
   unaffected either way; this is a real-host-behavior fix, not a new tested code path).
2. **BP-1 — one script builds every production binary.** A `build_all.sh` that builds
   `matrixdb` (CLI), `matrixdbd` (daemon), and stamps both with the `version.hpp` version in
   their `--version`/startup output. Reuses the `build.sh`/`run_tests.sh` compiler-detection
   pattern (`clang++`, fallback `g++`, `CXX` override) already established in this repo — no new
   build system, no cmake.
3. **BP-2 — a Docker image.** One multi-stage `Dockerfile`: build stage compiles `matrixdb` +
   `matrixdbd` from source with the toolchain, runtime stage copies just the two binaries onto a
   minimal base (no compiler in the shipped image). `ENTRYPOINT` runs `matrixdbd` (the thing you'd
   actually deploy); the CLI is present in the image for `docker exec` debugging/administration.
   A `.dockerignore` keeps the build context to source files (excludes generated notebooks, `.git`,
   docs).
4. **Docs.** Extend `docs/USAGE.md`'s existing (minimal) daemon section with: how to build the
   image, how to run it (port mapping, volume-mounting a snapshot for `--open`, passing `--token`
   via an env var read by an entrypoint wrapper — see below), and how to `docker exec` the CLI
   against the running data directory.
5. **Register update.** Mark BP-1/BP-2 landed (or partial, with what's deferred) in
   `PRODUCTION_READINESS.md`, same as every other landed increment there.

**Out (deferred, unchanged from the existing register):** TLS (SE-3 — needs a vetted library,
already flagged as needing the `IoChannel` seam), OS packages (deb/rpm/Homebrew — no
demonstrated demand yet, Docker covers the wedge), remote CI/CD (`.github/workflows` — no GPU
runner available to this project regardless), a `cmake` build (the shell-script pattern already
works and this repo has never used cmake for a real deliverable), HA/replication (explicitly P3
in the register).

## Design decisions

**Token via environment variable, not a baked-in CLI arg.** `matrixdbd`'s `--token` is a plaintext
CLI argument today (visible in `docker inspect`/`ps` if passed directly). The Docker entrypoint
reads `MATRIXDB_TOKEN` from the environment and passes it as `--token` internally — marginally
better (env vars are still visible via `docker inspect`, but this matches how most containerized
DBs already do it, e.g. `POSTGRES_PASSWORD`) and requires zero changes to `matrixdbd.cpp` itself.
Real secret management (mounted secret files, a vault) is future work, not blocking a first
container image — noted as a known limitation in the docs, not silently hidden.

**No entrypoint shell script beyond what Docker's `ENTRYPOINT`/`CMD` needs.** The image's
`ENTRYPOINT` is `matrixdbd` directly; `CMD` supplies the default port. Env-to-flag translation for
`MATRIXDB_TOKEN`/`MATRIXDB_OPEN` needs a couple of lines of shell, so the image's `ENTRYPOINT` is
a tiny inline `sh -c` wrapper (no separate `entrypoint.sh` file — it's short enough to live in the
`Dockerfile` directly).

**Data directory:** `matrixdbd`'s `--open <snapshot>` reads one file at startup and never writes
back to it automatically (per the existing `save_catalog`/`load_catalog` semantics — persistence
is point-op WAL, which isn't wired into `matrixdbd` at all yet, a pre-existing gap not created or
closed by this work). The Docker docs will say plainly: mount a host directory to `/data`, pass
`MATRIXDB_OPEN=/data/<file>` if you have a snapshot to load; there is currently no "durable
volume" story for the daemon beyond that. Not silently overstating what's there.

## What "done" looks like

- `./build_all.sh` builds `matrixdb` and `matrixdbd` from a clean checkout with one command,
  using the same compiler-detection convention as `build.sh`/`run_tests.sh`.
- `docker build .` produces a runnable image; `docker run -p 7070:7070 <image>` starts `matrixdbd`
  serving on 7070 in dev mode (no token); passing `-e MATRIXDB_TOKEN=...` switches it to
  authenticated mode. Both paths reachable from a real host (this sandbox can't `bind`, so the
  actual `docker run` + connect test is a manual host-verification step, same status as the
  existing "HOST-ONLY" items in the register).
- `run_tests.sh` still passes unmodified (no production test file touched by this work besides the
  two `setsockopt` lines in `server_tcp.hpp`, which don't change any function signature).
- `server_tcp.hpp`'s two accept loops set `TCP_NODELAY` on every accepted socket.
- `PRODUCTION_READINESS.md` reflects BP-1/BP-2 as landed with an honest note on what's deferred
  (TLS, OS packages, durable volumes for the daemon).

## Success criteria for this spec

A future engineer can read this and (a) understand exactly which 3 gaps are being closed and why
the rest of the register is out of scope here, (b) build the plan from the "done" list above
without further design decisions, (c) know precisely what is and isn't included so nobody mistakes
"packaged" for "production-hardened across the board."

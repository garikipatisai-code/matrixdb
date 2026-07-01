# Leg 1 Packaging Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the three specific gaps between "the engine works" and "a real, shippable app":
a confirmed network stall bug, no unified build, no packaging artifact.

**Architecture:** Fix `TCP_NODELAY` on both TCP accept loops in `server_tcp.hpp`; add a
`build_all.sh` that builds both production binaries with one command; add a multi-stage
`Dockerfile` + `.dockerignore` that builds and ships `matrixdb`/`matrixdbd` in a minimal runtime
image; extend `docs/USAGE.md`'s daemon section with build/run/Docker instructions; update
`PRODUCTION_READINESS.md` to reflect BP-1/BP-2 landed.

**Tech Stack:** C++20 (existing `clang++`/`g++` toolchain, no new build system), Docker
multi-stage build, POSIX shell (`build_all.sh`, matching `build.sh`/`run_tests.sh`'s existing
style).

**Spec:** `docs/superpowers/specs/2026-06-30-leg1-packaging-design.md`

---

### Task 1: Fix the Nagle/`TCP_NODELAY` stall in `server_tcp.hpp`

**Files:**
- Modify: `server_tcp.hpp:10-16` (includes), `server_tcp.hpp:105-112` (`matrix_serve_tcp`),
  `server_tcp.hpp:129-135` (`matrix_serve_tcp_auth`)

This is the same bug the network+GPU spike hit and fixed in its own throwaway
`spike/spike_server.cpp` (see `FINDINGS.md` §3.7's side-finding): every served response is two
`send_all()` calls (length prefix, then payload). Without `TCP_NODELAY` on the accepted socket,
Nagle holds the second send waiting to coalesce with the first, and the client's delayed-ACK
timer (~40ms) is what eventually releases it — a stall that dwarfs real compute time. Not
runtime-testable in this sandbox (`bind` is blocked, same as every other accept-loop item in
`PRODUCTION_READINESS.md`) — verified by compile (`run_tests.sh` already compiles `matrixdbd.cpp`
and runs `test_server_tcp.cpp`, neither of which changes here) plus a manual read-through.

- [ ] **Step 1: Add the `<netinet/tcp.h>` include**

In `server_tcp.hpp`, the include block currently reads (lines 10-16):

```cpp
#include "server.hpp"          // matrix_serve, AccessPolicy, CPUMockEngine
#include <sys/socket.h>
#include <sys/time.h>          // struct timeval (SO_RCVTIMEO)
#include <netinet/in.h>
#include <unistd.h>
#include <cstdint>
#include <vector>
```

Change it to:

```cpp
#include "server.hpp"          // matrix_serve, AccessPolicy, CPUMockEngine
#include <sys/socket.h>
#include <sys/time.h>          // struct timeval (SO_RCVTIMEO)
#include <netinet/in.h>
#include <netinet/tcp.h>       // TCP_NODELAY
#include <unistd.h>
#include <cstdint>
#include <vector>
```

- [ ] **Step 2: Set `TCP_NODELAY` in `matrix_serve_tcp`'s accept loop**

The loop currently reads (lines 105-112):

```cpp
    for (;;) {
        const int c = ::accept(srv, nullptr, nullptr);
        if (c < 0) continue;
        if (recv_timeout_ms) matrix_set_recv_timeout(c, recv_timeout_ms);   // NW-5: drop a stuck reader/writer
        if (recv_timeout_ms) matrix_set_send_timeout(c, recv_timeout_ms);   // both directions, same deadline
        while (matrix_serve_conn(eng, policy, /*principal=*/0, c)) { /* serve until the peer closes */ }
        ::close(c);
    }
```

Change it to:

```cpp
    for (;;) {
        const int c = ::accept(srv, nullptr, nullptr);
        if (c < 0) continue;
        // Every response is 2 send() calls (len prefix, then payload); without this, Nagle holds
        // the 2nd send for the peer's delayed-ACK timer (~40ms) — see FINDINGS.md 3.7.
        int nodelay = 1; ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
        if (recv_timeout_ms) matrix_set_recv_timeout(c, recv_timeout_ms);   // NW-5: drop a stuck reader/writer
        if (recv_timeout_ms) matrix_set_send_timeout(c, recv_timeout_ms);   // both directions, same deadline
        while (matrix_serve_conn(eng, policy, /*principal=*/0, c)) { /* serve until the peer closes */ }
        ::close(c);
    }
```

- [ ] **Step 3: Set `TCP_NODELAY` in `matrix_serve_tcp_auth`'s accept loop**

The loop currently reads (lines 129-135):

```cpp
    for (;;) {
        const int c = ::accept(srv, nullptr, nullptr);
        if (c < 0) continue;
        if (recv_timeout_ms) { matrix_set_recv_timeout(c, recv_timeout_ms); matrix_set_send_timeout(c, recv_timeout_ms); }  // NW-5
        matrix_serve_conn_auth(eng, policy, auth, c);   // authenticate (token frame) then serve until the peer closes
        ::close(c);
    }
```

Change it to:

```cpp
    for (;;) {
        const int c = ::accept(srv, nullptr, nullptr);
        if (c < 0) continue;
        int nodelay = 1; ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);  // see matrix_serve_tcp
        if (recv_timeout_ms) { matrix_set_recv_timeout(c, recv_timeout_ms); matrix_set_send_timeout(c, recv_timeout_ms); }  // NW-5
        matrix_serve_conn_auth(eng, policy, auth, c);   // authenticate (token frame) then serve until the peer closes
        ::close(c);
    }
```

- [ ] **Step 4: Run the existing CI gate to confirm nothing regressed**

Run: `cd MatrixDB && ./run_tests.sh`
Expected: `ALL GREEN (<N> tests + oracle)`, exit 0 — same as before this change (this edit doesn't
touch any function signature `test_server_tcp.cpp`/`test_client.cpp` depend on; those tests use
`matrix_serve_conn`/`matrix_serve_conn_auth` directly over a `socketpair`, not the accept loops).

- [ ] **Step 5: Commit**

```bash
cd MatrixDB
git add server_tcp.hpp
git commit -m "fix(server_tcp): set TCP_NODELAY on accepted sockets (Nagle/delayed-ACK stall)"
```

---

### Task 2: `build_all.sh` — one command builds every production binary

**Files:**
- Create: `build_all.sh`

**Files it builds:** `matrixdb_cli.cpp` → `matrixdb`, `matrixdbd.cpp` → `matrixdbd`. (`main.cpp`
→ `matrixdb_proto` is the internal benchmark/oracle harness, not a user-facing deliverable — left
out, matching how `run_tests.sh` treats it as a CI-only artifact, not a packaged binary.)

- [ ] **Step 1: Write the script**

```bash
#!/usr/bin/env bash
# Build every production MatrixDB binary from a clean checkout, one command.
#   ./build_all.sh             # uses clang++ (falls back to g++)
#   CXX=g++ ./build_all.sh     # force a compiler
# Builds matrixdb (CLI) and matrixdbd (daemon). See build.sh for the CLI-only equivalent.
set -eu
cd "$(dirname "$0")"

if [ -n "${CXX:-}" ]; then :; elif command -v clang++ >/dev/null 2>&1; then CXX=clang++; else CXX=g++; fi

VERSION=$(grep -o 'MATRIXDB_VERSION "[^"]*"' version.hpp | cut -d'"' -f2)

"$CXX" -std=c++20 -O3 matrixdb_cli.cpp -o matrixdb
"$CXX" -std=c++20 -O3 matrixdbd.cpp -o matrixdbd

echo "built ./matrixdb and ./matrixdbd  (version $VERSION, CXX=$CXX)"
echo "  CLI:    ./matrixdb -f examples/tour.sql"
echo "  daemon: ./matrixdbd 7070              # dev mode, no auth"
```

- [ ] **Step 2: Make it executable**

Run: `cd MatrixDB && chmod +x build_all.sh`

- [ ] **Step 3: Run it and verify both binaries build and report the version**

Run: `cd MatrixDB && ./build_all.sh`
Expected: two compiler invocations with no errors, then a line like
`built ./matrixdb and ./matrixdbd  (version 0.1.0, CXX=clang++)` (or `g++` if `clang++` is
unavailable) followed by the two usage hint lines.

- [ ] **Step 4: Verify the CLI binary actually works**

Run: `cd MatrixDB && ./matrixdb -f examples/tour.sql`
Expected: output containing `2575` (the tour demo's known `SUM(amount)` result — the same string
`run_tests.sh`'s DEMO check greps for).

- [ ] **Step 5: Verify the daemon binary actually works (as far as the sandbox allows)**

Run: `cd MatrixDB && ./matrixdbd 7070; echo "exit code: $?"`
Expected: stderr prints `matrixdbd: serving on port 7070 (no auth — dev mode)` followed by
`matrixdbd: could not bind/listen on port 7070 (note: bind is blocked in the build sandbox — run
this on a real host)`, then `exit code: 1`. This confirms the binary was built correctly and its
argument parsing/startup path runs — the same "compile + startup-path verified, bind is
host-only" status every other daemon-adjacent test in this repo already has.

- [ ] **Step 6: Clean up the build artifacts before committing (they're build output, not source)**

Run: `cd MatrixDB && rm -f matrixdb matrixdbd`

- [ ] **Step 7: Commit**

```bash
cd MatrixDB
git add build_all.sh
git commit -m "feat(build): add build_all.sh — one command builds matrixdb + matrixdbd (BP-1)"
```

---

### Task 3: Docker packaging

**Files:**
- Create: `Dockerfile`
- Create: `.dockerignore`

Multi-stage build: stage 1 compiles both binaries with a full C++ toolchain; stage 2 copies only
the binaries into a minimal runtime base (no compiler shipped). `ENTRYPOINT` runs `matrixdbd`
(what you'd actually deploy); `matrixdb` (the CLI) is present in the image for
`docker exec <container> matrixdb ...` administration. Env-to-flag translation for
`MATRIXDB_TOKEN`/`MATRIXDB_OPEN` is a one-line inline shell wrapper in `CMD` — no separate
`entrypoint.sh` file (per the spec's "no entrypoint script beyond what's needed" decision).

Docker isn't installed in this sandbox, so `docker build`/`docker run` are host-only manual
verification steps (same "HOST-ONLY, compile/read-verified here" status as `matrixdbd`'s bind
call) — the file itself is written carefully and syntax-checked by inspection, not by a build in
this session.

- [ ] **Step 1: Write `.dockerignore`**

```
.git
*.ipynb
docs/
examples/*.db
*.o
*.tmp
matrixdb
matrixdbd
matrixdb_proto
spike/
```

- [ ] **Step 2: Write `Dockerfile`**

```dockerfile
# MatrixDB — build the CPU-backend binaries, ship them without the toolchain.
# GPU (CUDA) build is out of scope for this image — matrixdbd/matrixdb only ever
# instantiate CPUMockEngine; see FINDINGS.md and PRODUCTION_READINESS.md for the GPU status.
FROM debian:12-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends clang && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN clang++ -std=c++20 -O3 matrixdb_cli.cpp -o matrixdb \
 && clang++ -std=c++20 -O3 matrixdbd.cpp -o matrixdbd

FROM debian:12-slim
RUN useradd -m -u 10001 matrixdb
COPY --from=build /src/matrixdb /src/matrixdbd /usr/local/bin/
USER matrixdb
WORKDIR /data
EXPOSE 7070
# MATRIXDB_TOKEN / MATRIXDB_OPEN are read from the environment and translated into matrixdbd's
# --token/--open flags — a container-native convention (cf. POSTGRES_PASSWORD), not a secrets
# manager; see docs/USAGE.md for the current limitation (env vars are visible via `docker inspect`).
ENTRYPOINT ["sh", "-c", "exec matrixdbd \"$0\" ${MATRIXDB_OPEN:+--open \"$MATRIXDB_OPEN\"} ${MATRIXDB_TOKEN:+--token \"$MATRIXDB_TOKEN\"}", "7070"]
```

- [ ] **Step 3: Manually verify the Dockerfile's shell logic without Docker**

Docker isn't available in this sandbox, so verify the `ENTRYPOINT` shell expansion directly with
`sh`, which mirrors exactly what the container will run:

Run:
```bash
cd MatrixDB
MATRIXDB_OPEN="" MATRIXDB_TOKEN="" sh -c 'set -x; echo matrixdbd "$0" ${MATRIXDB_OPEN:+--open "$MATRIXDB_OPEN"} ${MATRIXDB_TOKEN:+--token "$MATRIXDB_TOKEN"}' 7070
MATRIXDB_OPEN=/data/x.db MATRIXDB_TOKEN=s3cret sh -c 'set -x; echo matrixdbd "$0" ${MATRIXDB_OPEN:+--open "$MATRIXDB_OPEN"} ${MATRIXDB_TOKEN:+--token "$MATRIXDB_TOKEN"}' 7070
```
Expected: first line prints `matrixdbd 7070` (no flags — dev mode); second prints
`matrixdbd 7070 --open /data/x.db --token s3cret`. This confirms the env-to-flag translation is
correct without needing Docker installed.

- [ ] **Step 4: Note the host-only build/run verification in a comment-free way — record it in the plan, not the code**

No code change for this step. When this is run on a real host with Docker installed, the
verification is: `docker build -t matrixdb .` succeeds, then `docker run --rm -p 7070:7070
matrixdb` prints the same `matrixdbd: serving on port 7070 (no auth — dev mode)` line seen in
Task 2 Step 5 (this time it actually binds, since a real host isn't sandboxed), and
`docker exec <container> matrixdb -f examples/tour.sql` — wait, `examples/` isn't copied into the
runtime stage (only binaries are, per `.dockerignore`/`COPY --from=build`), so the exec-CLI use
case is for querying `/data`-mounted files, not the bundled examples. This is intentional (a
runtime image ships binaries, not sample data) and worth noting in the docs step below.

- [ ] **Step 5: Commit**

```bash
cd MatrixDB
git add Dockerfile .dockerignore
git commit -m "feat(docker): multi-stage Dockerfile for matrixdb/matrixdbd (BP-2)"
```

---

### Task 4: Docker + build docs in `docs/USAGE.md`

**Files:**
- Modify: `docs/USAGE.md` (its existing daemon section, lines 141-158)

- [ ] **Step 1: Read the current daemon section**

Run: `cd MatrixDB && sed -n '135,167p' docs/USAGE.md`

This shows the existing daemon build/run instructions and the TLS/reverse-proxy note, so the new
content can be appended after it without duplicating what's already there.

- [ ] **Step 2: Append a new subsection after the existing daemon content**

Add this text to the end of `docs/USAGE.md` (after its current last line):

```markdown

## Building everything at once

`./build_all.sh` builds both the CLI (`matrixdb`) and the daemon (`matrixdbd`) in one command
(same compiler auto-detection as `build.sh`: `clang++`, falling back to `g++`; override with
`CXX=g++ ./build_all.sh`).

## Running matrixdbd in Docker

A multi-stage `Dockerfile` builds both binaries and ships them without the compiler toolchain.

```bash
docker build -t matrixdb .

# Dev mode: no auth, empty catalog, listens on 7070
docker run --rm -p 7070:7070 matrixdb

# Authenticated, with a snapshot mounted from the host
docker run --rm -p 7070:7070 \
  -v /host/path/to/data:/data \
  -e MATRIXDB_OPEN=/data/mydata.db \
  -e MATRIXDB_TOKEN=s3cret \
  matrixdb

# Administer a running container with the bundled CLI (note: the CLI's own file args resolve
# inside the container, e.g. against /data if you mounted it there — the bundled examples/
# directory is NOT copied into the runtime image, only the two binaries are)
docker exec -it <container-id> matrixdb -c "SELECT COUNT(*) FROM ..."
```

**Current limitations, honestly stated:**
- `MATRIXDB_TOKEN` is passed as a plain environment variable, visible via `docker inspect`/
  `docker exec ... env` on the host — this is the same convention most containerized databases
  use (e.g. `POSTGRES_PASSWORD`), not a secrets-manager integration. Mount a secret file and wire
  it up yourself if you need stronger isolation.
- `matrixdbd` has no durable-volume story beyond `--open`/`MATRIXDB_OPEN` loading one snapshot at
  startup — it does not write back to that file, and the point-op WAL (`ColdStore`, see
  `PRODUCTION_READINESS.md` DU-1..6) isn't wired into `matrixdbd` at all yet. Data written to a
  running container is lost when it stops unless you build that persistence wiring yourself.
- The image only ever runs `CPUMockEngine` — there is no CUDA/GPU build of this image (the GPU
  backend is Colab-only today; see `FINDINGS.md`).
```

- [ ] **Step 3: Verify the doc renders sensibly**

Run: `cd MatrixDB && tail -40 docs/USAGE.md`
Expected: the new section appears, no broken markdown (matching code fences, no stray backticks).

- [ ] **Step 4: Commit**

```bash
cd MatrixDB
git add docs/USAGE.md
git commit -m "docs(usage): document build_all.sh and Docker packaging"
```

---

### Task 5: Update `PRODUCTION_READINESS.md`

**Files:**
- Modify: `PRODUCTION_READINESS.md` (§9 Build/packaging table + its notes section)

- [ ] **Step 1: Update the BP-1/BP-2 table rows**

Find this table (currently around line 281-284):

```markdown
| BP-1 | Build is manual `clang++`/`nvcc` one-liners; cmake not even installed | Not reproducible; no real build system in use | P1 | S | yes |
| BP-2 | No packaging (container/binary/release artifact) | Nothing to deploy | P1 | M | partial |
| BP-3 | No versioning / release process | Can't ship or roll back **[partial — BP-3: build version + wire-exposed]** | P2 | S | yes |
| BP-4 | No multi-platform build matrix (Apple ARM / Linux x86 / CUDA) | "Works on my Mac" only | P1 | M | needs CI |
```

Replace it with:

```markdown
| BP-1 | Build is manual `clang++`/`nvcc` one-liners; cmake not even installed | Not reproducible; no real build system in use **[FIXED — BP-1: build_all.sh]** | P1 | S | yes |
| BP-2 | No packaging (container/binary/release artifact) | Nothing to deploy **[FIXED — BP-2: Dockerfile]** | P1 | M | partial |
| BP-3 | No versioning / release process | Can't ship or roll back **[partial — BP-3: build version + wire-exposed]** | P2 | S | yes |
| BP-4 | No multi-platform build matrix (Apple ARM / Linux x86 / CUDA) | "Works on my Mac" only | P1 | M | needs CI |
```

- [ ] **Step 2: Add a landed-note paragraph after the BP-3 note**

Find the existing BP-3 note (currently around line 286):

```markdown
*BP-3 partial (build versioning, CPU): `version.hpp` carries the semver build version (`MATRIXDB_VERSION` "0.1.0" + major/minor/patch macros). The engine reports it via `version()` (string) and `version_u64()` (packed `major<<32|minor<<16|patch`, so versions compare with `<` and fit a wire field). STATS gained a 12th field = the packed version, and `MatrixClient::server_version()` reads it — so a client/monitor can see and compare which build it's talking to over the wire. test_version.cpp (string/packed/engine-getter agree; packed forms order like semver) + the server/client STATS tests assert the version field. ponytail: the release *process* (bump + git tag + artifact) is manual (the user pushes tags); this is the version a running instance reports, not an automated release pipeline. 57-test suite + oracle green. Deferred: a `git describe` build stamp, the release/tag automation, packaging (BP-2).*
```

Add this new paragraph immediately after it:

```markdown

*BP-1/BP-2 landed (Leg 1 packaging — build system + Docker image): `build_all.sh` builds both
production binaries (`matrixdb`, `matrixdbd`) with one command, reusing `build.sh`'s
compiler-detection convention (`clang++`, fallback `g++`, `CXX` override) — no cmake introduced;
the shell-script pattern already proven in this repo (`build.sh`/`run_tests.sh`) extends cleanly
to a second binary. A multi-stage `Dockerfile` builds both binaries with a full toolchain in stage
1 and ships only the binaries (no compiler) in a minimal `debian:12-slim` runtime stage;
`ENTRYPOINT` runs `matrixdbd`, with `MATRIXDB_OPEN`/`MATRIXDB_TOKEN` env vars translated into its
`--open`/`--token` flags (verified by direct `sh` expansion — Docker isn't installed in this dev
environment, so `docker build`/`docker run` themselves are a host-only verification step, the same
status every `bind()`-dependent capability in this register already has). This closes the literal
"nothing to deploy" gap: MatrixDB can now be built with one command and run as a container.
Side-fix bundled in: the Nagle/delayed-ACK stall the network+GPU spike diagnosed and fixed in its
own throwaway server (`FINDINGS.md` §3.7) is now also fixed in the real `matrix_serve_tcp`/
`matrix_serve_tcp_auth` accept loops (`TCP_NODELAY` on every accepted socket) — a real fix to
production code, not just the spike. Deferred, honestly scoped in `docs/USAGE.md`: TLS (SE-3, ships
behind a reverse proxy for now), OS packages (deb/rpm/Homebrew — no demonstrated need beyond
Docker), a durable-volume story for `matrixdbd` (the point-op WAL isn't wired into the daemon at
all yet — a pre-existing gap this work didn't create or close), secrets-manager integration for
`MATRIXDB_TOKEN` (env var today, same convention most containerized DBs use). See spec/plan
2026-06-30-leg1-packaging.*
```

- [ ] **Step 3: Verify the file is still valid Markdown / tables render**

Run: `cd MatrixDB && grep -n "BP-1\|BP-2" PRODUCTION_READINESS.md`
Expected: the table row edits and the new landed-note paragraph both appear; no malformed table
pipes (each row still has the same number of `|` as its header).

- [ ] **Step 4: Commit**

```bash
cd MatrixDB
git add PRODUCTION_READINESS.md
git commit -m "docs(readiness): mark BP-1/BP-2 landed — build_all.sh + Docker packaging"
```

---

### Task 6: Final verification pass

**Files:** none (verification only)

- [ ] **Step 1: Re-run the full CI gate**

Run: `cd MatrixDB && ./run_tests.sh`
Expected: `ALL GREEN (<N> tests + oracle)`, exit 0.

- [ ] **Step 2: Re-run the build script end to end and clean up**

Run: `cd MatrixDB && ./build_all.sh && ./matrixdb -f examples/tour.sql && rm -f matrixdb matrixdbd examples/tour.db`
Expected: builds cleanly, tour demo prints `2575`, then the two binaries and the demo's generated
`.db` file are removed (build output shouldn't linger as untracked files).

- [ ] **Step 3: Confirm no build artifacts are staged/tracked**

Run: `cd MatrixDB && git status --short`
Expected: clean working tree (nothing untracked besides what's intentionally still there, e.g. no
stray `matrixdb`/`matrixdbd`/`.o` files).

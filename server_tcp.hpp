#pragma once
// TCP transport adapter for the serve LOGIC (NW-1). Length-prefixed wire framing:
//   request  on the wire: [u32 len][len request bytes]   (the matrix_serialize_request blob)
//   response on the wire: [u32 len][len response bytes]   (the matrix_serialize_response blob)
// matrix_serve_conn() (serve one framed request on a connected fd) is the wire protocol — runtime-tested
// over a socketpair (no bind needed). matrix_serve_tcp() is the bind/listen/accept loop — HOST-ONLY:
// loopback bind() is blocked in the build sandbox (proven), so its accept loop is compile-verified here
// and runtime-verified only on a non-sandboxed host. Multiple connections are served concurrently: each
// accepted connection gets its own thread, all sharing one ConcurrentServer (NW-2) instance so writes
// still serialize across the whole daemon, not just within one connection.
#include "server.hpp"          // matrix_serve, AccessPolicy, CPUMockEngine
#include "concurrent_server.hpp"
#include <sys/socket.h>
#include <sys/time.h>          // struct timeval (SO_RCVTIMEO)
#include <netinet/in.h>
#include <netinet/tcp.h>       // TCP_NODELAY
#include <unistd.h>
#include <cstdint>
#include <thread>
#include <vector>

// NW-5 connection management: bound how long a recv may block, so a client that connects but never sends
// (slowloris-style) can't hang the single-owner serve loop forever. After the timeout, recv fails →
// recv_all returns false → matrix_serve_conn returns false → the loop drops the stuck connection and moves
// on. ms == 0 clears the timeout (block forever, the default). Returns true on success.
inline bool matrix_set_recv_timeout(int fd, unsigned ms) {
    struct timeval tv;
    tv.tv_sec  = static_cast<long>(ms / 1000);
    tv.tv_usec = static_cast<long>((ms % 1000) * 1000);
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0;
}
// Symmetric send timeout (SO_SNDTIMEO): bound how long a send may block, so a client that connects but
// never READS the response (slow-reader, the other half of the DoS) can't wedge the loop's send_all once
// the socket buffers fill. Same kernel mechanism as the recv timeout (verified there) — send returns
// EAGAIN past the deadline → send_all returns false → serve_conn returns false → the loop drops it.
inline bool matrix_set_send_timeout(int fd, unsigned ms) {
    struct timeval tv;
    tv.tv_sec  = static_cast<long>(ms / 1000);
    tv.tv_usec = static_cast<long>((ms % 1000) * 1000);
    return ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == 0;
}

namespace matrixsrv_detail {
// Read/write EXACTLY n bytes over a stream socket, looping over partial transfers; false on EOF/error.
inline bool recv_all(int fd, void* buf, size_t n) {
    auto* p = static_cast<unsigned char*>(buf); size_t got = 0;
    while (got < n) { const ssize_t r = ::recv(fd, p + got, n - got, 0); if (r <= 0) return false; got += static_cast<size_t>(r); }
    return true;
}
inline bool send_all(int fd, const void* buf, size_t n) {
    // MSG_NOSIGNAL (Linux): writing to a peer that closed returns EPIPE instead of raising SIGPIPE (which
    // would kill the process). macOS/BSD lacks the flag — there the caller ignores SIGPIPE / sets
    // SO_NOSIGPIPE (matrix_serve_tcp/clients should `signal(SIGPIPE, SIG_IGN)`); EPIPE then surfaces as w<0.
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    const auto* p = static_cast<const unsigned char*>(buf); size_t sent = 0;
    while (sent < n) { const ssize_t w = ::send(fd, p + sent, n - sent, flags); if (w <= 0) return false; sent += static_cast<size_t>(w); }
    return true;
}
} // namespace matrixsrv_detail

// Serve ONE framed request on a connected socket `fd`: read [u32 len][req], dispatch via matrix_serve
// (authorizing), write [u32 len][resp]. Returns false if the peer closed or the framing was short.
inline bool matrix_serve_conn(CPUMockEngine& eng, const AccessPolicy& policy, uint64_t principal, int fd) {
    uint32_t len = 0;
    if (!matrixsrv_detail::recv_all(fd, &len, sizeof len)) return false;
    if (len > (1u << 28)) return false;                       // sane cap — never alloc on a bogus length
    std::vector<uint8_t> req(len);
    if (len != 0 && !matrixsrv_detail::recv_all(fd, req.data(), len)) return false;
    const std::vector<uint8_t> resp = matrix_serve(eng, policy, principal, req);
    const uint32_t rlen = static_cast<uint32_t>(resp.size());
    return matrixsrv_detail::send_all(fd, &rlen, sizeof rlen)
        && (rlen == 0 || matrixsrv_detail::send_all(fd, resp.data(), rlen));
}

// Same framing as matrix_serve_conn, but dispatches through a shared ConcurrentServer (NW-2) instead of
// the bare matrix_serve — what a thread-per-connection accept loop uses so reads across connections run
// in parallel and writes serialize against the SAME mutex as every other connection, not a private one.
inline bool matrix_serve_conn_concurrent(ConcurrentServer& srv, uint64_t principal, int fd) {
    uint32_t len = 0;
    if (!matrixsrv_detail::recv_all(fd, &len, sizeof len)) return false;
    if (len > (1u << 28)) return false;                       // sane cap — never alloc on a bogus length
    std::vector<uint8_t> req(len);
    if (len != 0 && !matrixsrv_detail::recv_all(fd, req.data(), len)) return false;
    const std::vector<uint8_t> resp = srv.serve(req, principal);
    const uint32_t rlen = static_cast<uint32_t>(resp.size());
    return matrixsrv_detail::send_all(fd, &rlen, sizeof rlen)
        && (rlen == 0 || matrixsrv_detail::send_all(fd, resp.data(), rlen));
}

namespace matrixsrv_detail {
// SE-1 handshake shared by matrix_serve_conn_auth and the concurrent accept loop: read the leading
// token frame ([u32 len][token]) and authenticate it. False on a transport error or an invalid/empty
// token (principal untouched) — the caller closes the connection without serving anything.
inline bool authenticate_conn(const Authenticator& auth, int fd, uint64_t& principal) {
    uint32_t tlen = 0;
    if (!recv_all(fd, &tlen, sizeof tlen)) return false;
    if (tlen > 4096) return false;                            // sane token cap — never alloc on a bogus length
    std::string token(tlen, '\0');
    if (tlen != 0 && !recv_all(fd, token.data(), tlen)) return false;
    return auth.authenticate(token, principal);
}
} // namespace matrixsrv_detail

// SE-1 over the transport: a connection authenticates ONCE with a leading token frame, then serves that
// principal's requests for the life of the connection. Reads [u32 len][token]; authenticates → principal
// (on failure: serve nothing, return false so the caller closes the connection); then runs the normal
// matrix_serve_conn loop as that principal (AccessPolicy still gates each request). This is the realistic
// model — authenticate per connection, not per request — and the inverse of MatrixClient::authenticate.
inline bool matrix_serve_conn_auth(CPUMockEngine& eng, const AccessPolicy& policy,
                                   const Authenticator& auth, int fd) {
    uint64_t principal = 0;
    if (!matrixsrv_detail::authenticate_conn(auth, fd, principal)) return false;   // unauthenticated: serve nothing, close
    while (matrix_serve_conn(eng, policy, principal, fd)) { /* serve until the peer closes */ }
    return true;
}

// TCP accept-loop on `port`: each accepted connection is served on its own thread (detached — the daemon
// runs forever and connections are transient, so there is no orderly "join everyone" shutdown point
// today; this matches matrixdbd's existing lack of a graceful-shutdown story). All threads share ONE
// ConcurrentServer over `eng`, so its mutex actually serializes writes/tier-borrows across every
// connection, not just within one — this is what makes "many clients at once" true rather than nominal.
// HOST-ONLY (see file header — bind is blocked in the build sandbox). Returns -1 on a setup error.
// principal=0 here (no auth); a real deployment derives the principal from the connection (see
// matrix_serve_tcp_auth). Each accepted connection gets a recv/send timeout (NW-5) so one stuck client
// can't hang its own thread forever (it still can't hang any OTHER connection's thread either way).
// ponytail: no cap on concurrent connections/threads (NW-5's own remainder, unchanged by this — a
// connection limit or thread pool is future work, not silently solved here).
inline int matrix_serve_tcp(CPUMockEngine& eng, const AccessPolicy& policy, uint16_t port,
                            unsigned recv_timeout_ms = 30000) {
    const int srv_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) return -1;
    int yes = 1; ::setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
    if (::bind(srv_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) { ::close(srv_fd); return -1; }
    if (::listen(srv_fd, 16) != 0) { ::close(srv_fd); return -1; }
    ConcurrentServer srv(eng, policy);
    for (;;) {
        const int c = ::accept(srv_fd, nullptr, nullptr);
        if (c < 0) continue;
        // Every response is 2 send() calls (len prefix, then payload); without this, Nagle holds
        // the 2nd send for the peer's delayed-ACK timer (~40ms) — see FINDINGS.md 3.7.
        int nodelay = 1; ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
        if (recv_timeout_ms) matrix_set_recv_timeout(c, recv_timeout_ms);   // NW-5: drop a stuck reader/writer
        if (recv_timeout_ms) matrix_set_send_timeout(c, recv_timeout_ms);   // both directions, same deadline
        std::thread([&srv, c] {
            while (matrix_serve_conn_concurrent(srv, /*principal=*/0, c)) { /* serve until the peer closes */ }
            ::close(c);
        }).detach();
    }
}

// Auth-aware accept loop (the realistic production entrypoint): each connection authenticates with a leading
// token frame (matrixsrv_detail::authenticate_conn) and then serves as its principal, gated by `policy`, on
// its own thread — all threads sharing one ConcurrentServer over `eng` (see matrix_serve_tcp's comment for
// why that's the part that makes concurrent connections actually work). HOST-ONLY — bind is blocked in the
// build sandbox, so this loop is compile-verified here; the per-connection auth+serve path it relies on
// is runtime-verified over a socketpair in test_server_tcp.cpp.
inline int matrix_serve_tcp_auth(CPUMockEngine& eng, const AccessPolicy& policy, const Authenticator& auth,
                                 uint16_t port, unsigned recv_timeout_ms = 30000) {
    const int srv_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) return -1;
    int yes = 1; ::setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
    if (::bind(srv_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) { ::close(srv_fd); return -1; }
    if (::listen(srv_fd, 16) != 0) { ::close(srv_fd); return -1; }
    ConcurrentServer srv(eng, policy);
    for (;;) {
        const int c = ::accept(srv_fd, nullptr, nullptr);
        if (c < 0) continue;
        int nodelay = 1; ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);  // see matrix_serve_tcp
        if (recv_timeout_ms) { matrix_set_recv_timeout(c, recv_timeout_ms); matrix_set_send_timeout(c, recv_timeout_ms); }  // NW-5
        std::thread([&srv, &auth, c] {
            uint64_t principal = 0;
            if (matrixsrv_detail::authenticate_conn(auth, c, principal)) {
                while (matrix_serve_conn_concurrent(srv, principal, c)) { /* serve until the peer closes */ }
            }
            ::close(c);
        }).detach();
    }
}

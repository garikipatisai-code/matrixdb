// CPU test for the TCP transport adapter (NW transport): the length-prefixed wire protocol +
// matrix_serve_conn are runtime-verified over a socketpair (no bind needed) — a framed request served
// over a real socket yields exactly the same response bytes as a direct matrix_serve. (The bind/accept
// loop, matrix_serve_tcp, is HOST-ONLY and only compile-verified here — see server_tcp.hpp.)
#include "server_tcp.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>
#include <atomic>
#include <thread>
#include <iostream>

static MatrixRequest mk(ReqKind k) { MatrixRequest r; r.kind = k; return r; }

static void framed_write(int fd, const std::vector<uint8_t>& b) {
    uint32_t len = static_cast<uint32_t>(b.size());
    assert(matrixsrv_detail::send_all(fd, &len, sizeof len));
    if (len) assert(matrixsrv_detail::send_all(fd, b.data(), len));
}
static std::vector<uint8_t> framed_read(int fd) {
    uint32_t len = 0; assert(matrixsrv_detail::recv_all(fd, &len, sizeof len));
    std::vector<uint8_t> b(len); if (len) assert(matrixsrv_detail::recv_all(fd, b.data(), len));
    return b;
}

// Serve `req` over a socketpair and assert the framed response equals a direct matrix_serve of the same bytes.
static void roundtrip(CPUMockEngine& eng, const AccessPolicy& pol, uint64_t principal, const MatrixRequest& req) {
    int fd[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    const std::vector<uint8_t> reqb = matrix_serialize_request(req);
    framed_write(fd[0], reqb);                                  // client -> framed request
    assert(matrix_serve_conn(eng, pol, principal, fd[1]) && "server served one framed request");
    const std::vector<uint8_t> got = framed_read(fd[0]);        // client <- framed response
    const std::vector<uint8_t> expect = matrix_serve(eng, pol, principal, reqb);   // direct serve, same bytes
    assert(got == expect && "TCP-framed serve-over-socket == direct matrix_serve");
    ::close(fd[0]); ::close(fd[1]);
}

int main() {
    std::vector<uint32_t> v(1000);
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<uint32_t>(i);
    CPUMockEngine eng;                                  // durability off (PUT applies to kv_ in memory)
    eng.load_scan_column(2, v.data(), v.size());
    AccessPolicy pol; pol.allow_column(1, 2); pol.allow_read(1); pol.allow_write(1);   // principal 1 only

    MatrixRequest g = mk(ReqKind::GET);   g.key = 5;
    MatrixRequest p = mk(ReqKind::PUT);   p.key = 5; p.value = 500;
    MatrixRequest q = mk(ReqKind::QUERY); q.query = MatrixQuery{}; q.query.value_col = 2; q.query.agg = AGG_SUM;
    roundtrip(eng, pol, 1, g);    // allowed GET
    roundtrip(eng, pol, 1, p);    // allowed PUT
    roundtrip(eng, pol, 1, q);    // allowed QUERY (sum over 1000 values)
    roundtrip(eng, pol, 2, p);    // DENIED (principal 2) -> ERR_FORBIDDEN, faithfully framed over the socket
    // a malformed/empty framed request still serves a (bad-request) response without hanging/crashing
    { int fd[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
      framed_write(fd[0], {1, 2, 3});                   // garbage payload
      assert(matrix_serve_conn(eng, pol, 7, fd[1]));
      assert(!framed_read(fd[0]).empty() && "malformed request -> a (bad-request) response, no hang");
      ::close(fd[0]); ::close(fd[1]); }

    // SE-1 over the wire (what matrix_serve_tcp_auth / matrixdbd rely on): a leading token frame
    // authenticates the connection, then it serves as that principal until EOF.
    Authenticator auth; auth.add_credential("s3cret", 1);
    { int fd[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
      framed_write(fd[0], std::vector<uint8_t>{'s','3','c','r','e','t'});   // token frame -> principal 1
      const std::vector<uint8_t> qb = matrix_serialize_request(q);
      framed_write(fd[0], qb);                                              // one request, served as principal 1
      ::shutdown(fd[0], SHUT_WR);                                           // EOF after it -> serve loop ends
      assert(matrix_serve_conn_auth(eng, pol, auth, fd[1]) && "authenticated, then served");
      assert(framed_read(fd[0]) == matrix_serve(eng, pol, 1, qb) && "wire serve == direct serve as principal 1");
      ::close(fd[0]); ::close(fd[1]); }
    { int fd[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);    // bad token -> serve nothing, reject
      framed_write(fd[0], std::vector<uint8_t>{'n','o','p','e'});
      ::shutdown(fd[0], SHUT_WR);
      assert(!matrix_serve_conn_auth(eng, pol, auth, fd[1]) && "unauthenticated connection rejected");
      ::close(fd[0]); ::close(fd[1]); }

    // matrix_serve_conn_concurrent: same framing, dispatched through a shared ConcurrentServer instead of
    // the bare matrix_serve -- what matrix_serve_tcp's real (HOST-ONLY, unrunnable here) accept loop now
    // uses. One connection first, proving byte-for-byte equivalence with ConcurrentServer::serve() direct.
    {
        ConcurrentServer csrv(eng, pol);
        int fd[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
        const std::vector<uint8_t> qb = matrix_serialize_request(q);
        framed_write(fd[0], qb);
        assert(matrix_serve_conn_concurrent(csrv, 1, fd[1]) && "concurrent-dispatch server served one framed request");
        const std::vector<uint8_t> got = framed_read(fd[0]);
        assert(got == csrv.serve(qb, 1) && "TCP-framed concurrent serve == direct ConcurrentServer::serve");
        ::close(fd[0]); ::close(fd[1]);
    }

    // The actual concurrency claim: N independent "connections" (socketpairs standing in for what
    // matrix_serve_tcp's accept loop would hand to N threads -- the only thing bind()/accept() would add
    // over this is OS-level connection acceptance, not the serve logic under test), each served on its
    // own thread, all sharing ONE ConcurrentServer over the SAME engine. Proves two things at once: many
    // connections' requests are all served correctly under concurrent load, and a PUT on one connection is
    // durably visible to a GET on a completely different connection (the mutex actually serializes writes
    // across connections, not just within one -- the exact property that made ConcurrentServer's old
    // fixed-principal, single-instance-per-daemon design insufficient for matrixdbd before this fix).
    {
        CPUMockEngine meng;   // fresh engine: this test's PUTs must not collide with earlier tests' keys
        ConcurrentServer msrv(meng, AccessPolicy::permissive());
        constexpr int kConns = 8, kOpsPerConn = 50;
        std::vector<std::array<int, 2>> fds(kConns);
        for (auto& p : fds) { int f[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, f) == 0); p = {f[0], f[1]}; }

        std::atomic<int> bad{0};
        std::vector<std::thread> servers, clients;
        for (int c = 0; c < kConns; ++c) {
            const int server_fd = fds[static_cast<size_t>(c)][1];
            servers.emplace_back([&msrv, server_fd] {
                while (matrix_serve_conn_concurrent(msrv, /*principal=*/0, server_fd)) { /* until EOF */ }
            });
        }
        for (int c = 0; c < kConns; ++c) {
            const int client_fd = fds[static_cast<size_t>(c)][0];
            clients.emplace_back([&, c, client_fd] {
                for (int i = 0; i < kOpsPerConn; ++i) {
                    const uint64_t key = static_cast<uint64_t>(c) * 1000 + static_cast<uint64_t>(i);
                    MatrixRequest put = mk(ReqKind::PUT); put.key = key; put.value = key * 7;
                    framed_write(client_fd, matrix_serialize_request(put));
                    MatrixResponse presp;
                    if (!matrix_deserialize_response(framed_read(client_fd), presp) || presp.status != 0) ++bad;

                    MatrixRequest get = mk(ReqKind::GET); get.key = key;
                    framed_write(client_fd, matrix_serialize_request(get));
                    MatrixResponse gresp;
                    if (!matrix_deserialize_response(framed_read(client_fd), gresp)
                        || gresp.results.size() != 1 || gresp.results[0] != key * 7) ++bad;
                }
                ::shutdown(client_fd, SHUT_WR);   // EOF -> the matching server thread's loop ends
            });
        }
        for (auto& t : clients) t.join();
        for (auto& t : servers) t.join();
        assert(bad.load() == 0 && "all connections' PUT+GET pairs correct under real concurrent serving");

        // Cross-connection visibility: every key written by every connection's thread is readable now,
        // through the SAME shared engine -- not N independent, isolated per-connection states.
        for (int c = 0; c < kConns; ++c) {
            for (int i = 0; i < kOpsPerConn; ++i) {
                const uint64_t key = static_cast<uint64_t>(c) * 1000 + static_cast<uint64_t>(i);
                uint64_t v = 0; assert(meng.kv_get(key, v) && v == key * 7 && "cross-connection write visible");
            }
        }
        for (auto& p : fds) { ::close(p[0]); ::close(p[1]); }
        std::cout << "[concurrent accept-loop dispatch] ok (" << kConns << " connections x " << kOpsPerConn << " ops)\n";
    }

    std::cout << "ALL TCP-TRANSPORT TESTS PASSED\n";
    return 0;
}

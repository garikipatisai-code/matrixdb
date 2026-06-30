// CPU test for the TCP transport adapter (NW transport): the length-prefixed wire protocol +
// matrix_serve_conn are runtime-verified over a socketpair (no bind needed) — a framed request served
// over a real socket yields exactly the same response bytes as a direct matrix_serve. (The bind/accept
// loop, matrix_serve_tcp, is HOST-ONLY and only compile-verified here — see server_tcp.hpp.)
#include "server_tcp.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <cstdint>
#include <vector>
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

    std::cout << "ALL TCP-TRANSPORT TESTS PASSED\n";
    return 0;
}

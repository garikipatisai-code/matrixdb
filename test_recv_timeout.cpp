// CPU test for NW-5 connection read timeout: a client that connects but never sends must NOT hang the
// single-owner serve loop — SO_RCVTIMEO makes recv fail, so matrix_serve_conn returns false and the loop
// drops the stuck connection. A normal request still serves fine with a timeout set.
#include "server_tcp.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

static void test_recv_timeout() {
    std::vector<uint32_t> v(10, 1);
    CPUMockEngine eng; eng.load_scan_column(2, v.data(), v.size());
    const AccessPolicy pol = AccessPolicy::permissive();

    // (1) slowloris: client connects, sends NOTHING. With a short recv timeout, serve_conn must return
    // false (not block forever). The 200ms timeout bounds this — if it hung, the test would never finish.
    {
        int fd[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
        assert(matrix_set_recv_timeout(fd[1], 200) && "set recv timeout");
        assert(!matrix_serve_conn(eng, pol, 0, fd[1]) && "stuck client (no data) -> serve_conn returns false, no hang");
        ::close(fd[0]); ::close(fd[1]);
    }
    // (2) a normal framed request still serves OK with a timeout set (data arrives before the timeout)
    {
        int fd[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
        assert(matrix_set_recv_timeout(fd[1], 2000));
        MatrixRequest q; q.kind = ReqKind::QUERY; q.query = MatrixQuery{}; q.query.value_col = 2; q.query.agg = AGG_SUM;
        const std::vector<uint8_t> rb = matrix_serialize_request(q);
        const uint32_t len = static_cast<uint32_t>(rb.size());
        assert(matrixsrv_detail::send_all(fd[0], &len, sizeof len) && matrixsrv_detail::send_all(fd[0], rb.data(), len));
        assert(matrix_serve_conn(eng, pol, 0, fd[1]) && "a request that arrives in time still serves");
        uint32_t rlen = 0; assert(matrixsrv_detail::recv_all(fd[0], &rlen, sizeof rlen) && rlen > 0 && "got a response");
        ::close(fd[0]); ::close(fd[1]);
    }
    std::cout << "[recv timeout] ok\n";
}

int main() { test_recv_timeout(); std::cout << "ALL RECV-TIMEOUT TESTS PASSED\n"; return 0; }

// CPU test for the NW-4 client driver: MatrixClient drives the wire protocol end-to-end over a socketpair.
// The server serves the connection in a thread; the client thread only touches the socket (no engine race).
// Exercises every op (PUT/GET/QUERY/HEALTH/STATS) and confirms client results == direct engine calls.
#include "client.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>
#include <iostream>

static void test_client_roundtrip() {
    int fd[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    std::vector<uint32_t> v(100);
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<uint32_t>(i);
    CPUMockEngine eng; eng.load_scan_column(2, v.data(), v.size());
    const AccessPolicy pol = AccessPolicy::permissive();

    // server: serve this connection's requests until the client closes its end
    std::thread srv([&] { while (matrix_serve_conn(eng, pol, /*principal=*/1, fd[1])) {} });

    MatrixClient cli(fd[0]);
    // PUT then GET it back over the wire
    assert(cli.put(5, 500) && "PUT over the wire");
    uint64_t got = 0;
    assert(cli.get(5, got) && got == 500 && "GET returns the PUT value");
    uint64_t miss = 123;
    assert(!cli.get(99999, miss) && miss == 123 && "GET miss -> false, out untouched");

    // QUERY: SUM over the column == direct engine call
    MatrixQuery q{}; q.value_col = 2; q.agg = AGG_SUM;
    MatrixQueryStatus st; std::vector<uint64_t> out;
    assert(cli.query(q, st, out) && st == MatrixQueryStatus::OK && out.size() == 1);
    std::vector<uint64_t> direct; eng.execute_query(q, direct);
    assert(out == direct && "client QUERY == direct execute_query");
    // a query-level error rides through as a status (not a transport failure)
    MatrixQuery bad{}; bad.value_col = 999; bad.agg = AGG_SUM;
    assert(cli.query(bad, st, out) && st == MatrixQueryStatus::ERR_UNKNOWN_COLUMN && "query error -> status, transport ok");

    // HEALTH over the wire == the engine's own snapshot
    HealthStatus h;
    assert(cli.health(h) && h.ready && h.catalog_columns == 1 && "HEALTH over the wire");
    assert(h.host_resident_bytes == eng.health().host_resident_bytes && "health gauges match the engine");

    // STATS over the wire: 11 fields, query_count reflects the queries served
    std::vector<uint64_t> s;
    assert(cli.stats(s) && s.size() == 11 && "STATS over the wire");
    assert(s[6] == eng.stats().query_count && "client-observed query_count == engine's");

    ::close(fd[0]);                                            // signals the server loop to end
    srv.join();
    ::close(fd[1]);
    std::cout << "[client round-trip] ok\n";
}

int main() { test_client_roundtrip(); std::cout << "ALL CLIENT TESTS PASSED\n"; return 0; }

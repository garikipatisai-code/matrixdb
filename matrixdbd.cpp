// matrixdbd — the MatrixDB network server (preview). Serves the GET/PUT/QUERY/HEALTH/STATS protocol over
// length-prefixed TCP, one connection at a time (single-owner serve). HOST-ONLY: bind() is blocked in the
// build sandbox, so this is compile-verified in CI and runs on a real host; the per-connection auth + serve
// path it uses (matrix_serve_conn_auth) is runtime-verified over a socketpair in test_server_tcp.cpp.
//
//   clang++ -std=c++20 -O2 matrixdbd.cpp -o matrixdbd
//   ./matrixdbd 7070 --open mydata.db --token s3cret   # authenticated, serving a saved catalog
//   ./matrixdbd 7070                                    # dev mode: no auth (permissive), empty catalog
//
// A client connects with MatrixClient (client.hpp): authenticate(token) then get/put/query. See
// docs/superpowers/specs/2026-06-30-matrixdb-networked-serving-design.md for the protocol + the TLS/
// concurrency/deployment plan (the parts that need a real host or a vetted TLS library).
#include "server_tcp.hpp"
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: matrixdbd <port> [--open <snapshot>] [--token <token>]\n"; return 2; }
    const uint16_t port = static_cast<uint16_t>(std::strtoul(argv[1], nullptr, 10));
    std::string snapshot, token;
    for (int i = 2; i + 1 < argc; i += 2) {
        if      (std::strcmp(argv[i], "--open")  == 0) snapshot = argv[i + 1];
        else if (std::strcmp(argv[i], "--token") == 0) token    = argv[i + 1];
        else { std::cerr << "matrixdbd: unknown option " << argv[i] << "\n"; return 2; }
    }
    std::signal(SIGPIPE, SIG_IGN);   // a peer hanging up mid-send must not kill the server (EPIPE surfaces instead)

    CPUMockEngine eng;
    if (!snapshot.empty() && !eng.load_catalog(snapshot)) {
        std::cerr << "matrixdbd: could not open snapshot " << snapshot << "\n"; return 1;
    }

    int rc;
    if (token.empty()) {
        std::cerr << "matrixdbd: serving on port " << port << " (no auth — dev mode)\n";
        rc = matrix_serve_tcp(eng, AccessPolicy::permissive(), port);
    } else {
        Authenticator auth; auth.add_credential(token, /*principal=*/1);     // single principal for the token
        AccessPolicy pol; pol.allow_read(1); pol.allow_write(1);
        for (const ColumnInfo& c : eng.catalog_columns()) pol.allow_column(1, c.id);   // principal 1 may QUERY every loaded column
        std::cerr << "matrixdbd: serving on port " << port << " (token auth, "
                  << eng.catalog_columns().size() << " columns)\n";
        rc = matrix_serve_tcp_auth(eng, pol, auth, port);
    }
    if (rc < 0) {
        std::cerr << "matrixdbd: could not bind/listen on port " << port
                  << " (note: bind is blocked in the build sandbox — run this on a real host)\n";
        return 1;
    }
    return 0;   // the serve loop runs forever on a real host; it returns only on a setup error
}

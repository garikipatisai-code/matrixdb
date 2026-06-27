// CPU test for authorization / access control (AccessPolicy + authorizing matrix_serve).
#include "server.hpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <string>
#include <cstdio>
#include <iostream>

static MatrixResponse serve_req(CPUMockEngine& e, const AccessPolicy& p, uint64_t principal, const MatrixRequest& r) {
    MatrixResponse resp;
    matrix_deserialize_response(matrix_serve(e, p, principal, matrix_serialize_request(r)), resp);
    return resp;
}
static MatrixRequest mk(ReqKind kind) { MatrixRequest r; r.kind = kind; return r; }

static void test_access_control() {
    const size_t N = 1000; std::vector<uint32_t> v(N), k(N);
    for (size_t i = 0; i < N; ++i) { v[i] = static_cast<uint32_t>(i); k[i] = static_cast<uint32_t>(i % 4); }
    const std::string wal = "/tmp/matrixdb_sec.bin"; std::remove(wal.c_str());
    CPUMockEngine eng(0, wal);
    eng.load_scan_column(1, k.data(), N);
    eng.load_scan_column(2, v.data(), N);
    AccessPolicy p; p.allow_column(1, 2); p.allow_read(1); p.allow_write(1);
    using S = ServerStatus;

    { MatrixRequest g = mk(ReqKind::GET); g.key = 5;
      assert(serve_req(eng, p, 1, g).status == static_cast<uint32_t>(S::OK));
      assert(serve_req(eng, p, 2, g).status == static_cast<uint32_t>(S::ERR_FORBIDDEN)); }

    { MatrixRequest put = mk(ReqKind::PUT); put.key = 5; put.value = 500;
      assert(serve_req(eng, p, 2, put).status == static_cast<uint32_t>(S::ERR_FORBIDDEN));
      MatrixRequest g = mk(ReqKind::GET); g.key = 5;
      assert(serve_req(eng, p, 1, g).results.empty() && "denied PUT wrote nothing");
      assert(serve_req(eng, p, 1, put).status == static_cast<uint32_t>(S::OK));
      auto after = serve_req(eng, p, 1, g);
      assert(after.results.size() == 1 && after.results[0] == 500); }

    { MatrixRequest q = mk(ReqKind::QUERY); q.query = MatrixQuery{.value_col = 2, .agg = AGG_SUM};
      assert(serve_req(eng, p, 1, q).status == static_cast<uint32_t>(MatrixQueryStatus::OK));
      assert(serve_req(eng, p, 2, q).status == static_cast<uint32_t>(S::ERR_FORBIDDEN));
      MatrixRequest q3 = mk(ReqKind::QUERY); q3.query = MatrixQuery{.value_col = 3, .agg = AGG_SUM};
      assert(serve_req(eng, p, 1, q3).status == static_cast<uint32_t>(S::ERR_FORBIDDEN) && "col 3 not granted (authz before existence)"); }

    { MatrixRequest qg = mk(ReqKind::QUERY);
      qg.query = MatrixQuery{.value_col = 2, .agg = AGG_SUM, .grouped = true, .key_col = 1, .num_groups = 4};
      assert(serve_req(eng, p, 1, qg).status == static_cast<uint32_t>(S::ERR_FORBIDDEN) && "key col 1 not granted yet");
      p.allow_column(1, 1);
      assert(serve_req(eng, p, 1, qg).status == static_cast<uint32_t>(MatrixQueryStatus::OK) && "both cols granted -> OK"); }

    { AccessPolicy perm = AccessPolicy::permissive(); MatrixRequest g = mk(ReqKind::GET); g.key = 5;
      assert(serve_req(eng, perm, 2, g).status == static_cast<uint32_t>(S::OK) && "permissive allows anon"); }

    std::remove(wal.c_str());
    std::cout << "[access control] ok\n";
}

int main() {
    test_access_control();
    std::cout << "ALL SECURITY TESTS PASSED\n";
    return 0;
}

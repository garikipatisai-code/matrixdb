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

// SE-1: the authenticating matrix_serve validates a bearer token -> principal BEFORE authz; a bad/empty
// token is rejected with ERR_UNAUTHENTICATED and no engine work, a valid token serves as its principal.
static void test_authentication() {
    const size_t N = 100; std::vector<uint32_t> v(N);
    for (size_t i = 0; i < N; ++i) v[i] = static_cast<uint32_t>(i);
    CPUMockEngine eng; eng.load_scan_column(2, v.data(), N);
    using S = ServerStatus;

    Authenticator auth;
    auth.add_credential("s3cr3t-alice", 1);                    // token -> principal 1
    AccessPolicy p; p.allow_column(1, 2);                      // principal 1 may query col 2

    MatrixRequest q = mk(ReqKind::QUERY); q.query = MatrixQuery{.value_col = 2, .agg = AGG_SUM};
    const std::vector<uint8_t> qb = matrix_serialize_request(q);
    auto serve = [&](const std::string& tok) {
        MatrixResponse r; matrix_deserialize_response(matrix_serve(eng, p, auth, tok, qb), r); return r;
    };
    // valid token -> authenticated as principal 1 -> authorized -> OK
    assert(serve("s3cr3t-alice").status == static_cast<uint32_t>(MatrixQueryStatus::OK) && "valid token authenticates");
    // unknown / empty token -> ERR_UNAUTHENTICATED, no results
    assert(serve("wrong-token").status == static_cast<uint32_t>(S::ERR_UNAUTHENTICATED) && "bad token rejected");
    assert(serve("").status == static_cast<uint32_t>(S::ERR_UNAUTHENTICATED) && "empty token rejected");
    assert(serve("wrong-token").results.empty() && "rejected request returns no data");

    // authn precedes authz: a valid token for a principal WITHOUT the grant -> ERR_FORBIDDEN (not UNAUTH)
    auth.add_credential("s3cr3t-bob", 2);                      // principal 2 has no column grant
    assert(serve("s3cr3t-bob").status == static_cast<uint32_t>(S::ERR_FORBIDDEN) && "authenticated but unauthorized");
    std::cout << "[authentication] ok\n";
}

// Correctness of the constant-time comparison in Authenticator::authenticate (SE-1 hardening): the
// content-comparison logic changed (no early exit on the first mismatching byte, no hash-map lookup) to
// close a timing side-channel, so pin down that it still gets ordinary cases right -- a same-length
// guess that differs only in its last byte, a guess that's a proper prefix of a real token, and multiple
// registered credentials where only one should ever match.
static void test_authentication_constant_time_correctness() {
    Authenticator auth;
    auth.add_credential("s3cr3t-alice-0123", 1);
    auth.add_credential("s3cr3t-bob-4567", 2);
    uint64_t principal = 999;

    assert(auth.authenticate("s3cr3t-alice-0123", principal) && principal == 1 && "exact match: alice");
    principal = 999;
    assert(auth.authenticate("s3cr3t-bob-4567", principal) && principal == 2 && "exact match: bob");

    // same length as a real token, differs only in the final byte -- must still fail (the whole point of
    // removing the early-exit: a naive "compare until first difference, but always scan the full length"
    // rewrite could accidentally short-circuit correctness along with timing if done carelessly)
    principal = 999;
    assert(!auth.authenticate("s3cr3t-alice-0124", principal) && principal == 999 && "last-byte-only diff rejected");

    // a proper prefix of a real token (shorter, so length differs) must fail, not partially match
    principal = 999;
    assert(!auth.authenticate("s3cr3t-alice", principal) && principal == 999 && "prefix of a real token rejected");

    // a real token with one extra trailing byte (longer) must also fail
    principal = 999;
    assert(!auth.authenticate("s3cr3t-alice-0123X", principal) && principal == 999 && "real token + extra byte rejected");

    std::cout << "[authentication constant-time correctness] ok\n";
}

int main() {
    test_access_control();
    test_authentication();
    test_authentication_constant_time_correctness();
    std::cout << "ALL SECURITY TESTS PASSED\n";
    return 0;
}

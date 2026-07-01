// CPU test for the server core (request/response protocol + matrix_serve dispatch).
#include "server.hpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <cstdio>
#include <iostream>

static void test_request_roundtrip() {
    MatrixRequest r; r.kind = ReqKind::QUERY; r.key = 7; r.value = 70;
    r.query = MatrixQuery{.value_col=2, .agg=AGG_SUM, .has_filter=true, .threshold=5, .cmp=MatrixCmp::BETWEEN,
                          .upper=9, .lo_i64=-123456789012LL, .hi_i64=987654321098LL, .lo_f64=-1.5, .hi_f64=2.5,
                          .grouped=true, .key_col=1, .num_groups=4, .limit=10, .filter_col=3};
    auto b = matrix_serialize_request(r);
    MatrixRequest r2;
    assert(matrix_deserialize_request(b, r2));
    assert(r2.kind == ReqKind::QUERY && r2.key == 7 && r2.value == 70);
    assert(r2.query.value_col == 2 && r2.query.agg == AGG_SUM && r2.query.has_filter
           && r2.query.threshold == 5 && r2.query.grouped && r2.query.key_col == 1 && r2.query.num_groups == 4);
    // Previously silently dropped on the wire (the server would execute a different, wrong query than
    // the one the client asked for): cmp/upper, int64+double filter bounds, limit, filter_col. All now
    // round-trip byte-for-byte. (limit round-trips but still has no effect via the network QUERY dispatch
    // -- see compute.hpp's MatrixQuery::limit comment -- that's a separate, distinct missing-feature gap,
    // not the silent-data-corruption bug this test guards against.)
    assert(r2.query.cmp == MatrixCmp::BETWEEN && r2.query.upper == 9 && "cmp/upper now cross the wire");
    assert(r2.query.lo_i64 == -123456789012LL && r2.query.hi_i64 == 987654321098LL && "int64 bounds now cross the wire");
    assert(r2.query.lo_f64 == -1.5 && r2.query.hi_f64 == 2.5 && "double bounds now cross the wire");
    assert(r2.query.limit == 10 && r2.query.filter_col == 3 && "limit/filter_col now cross the wire");
    b.pop_back(); assert(!matrix_deserialize_request(b, r2) && "truncated request rejected");
    std::cout << "[request round-trip] ok\n";
}

static void test_response_roundtrip() {
    MatrixResponse r; r.status = 3; r.results = {10, 20, 30};
    auto b = matrix_serialize_response(r);
    MatrixResponse r2;
    assert(matrix_deserialize_response(b, r2) && r2.status == 3 && (r2.results == std::vector<uint64_t>{10,20,30}));
    MatrixResponse e; auto be = matrix_serialize_response(e);
    MatrixResponse e2; assert(matrix_deserialize_response(be, e2) && e2.status == 0 && e2.results.empty());
    b.pop_back(); assert(!matrix_deserialize_response(b, r2) && "truncated response rejected");
    std::cout << "[response round-trip] ok\n";
}

static void test_serve_get_put() {
    const std::string wal = "/tmp/matrixdb_srv.bin"; std::remove(wal.c_str());
    {
        CPUMockEngine eng(0, wal);
        MatrixRequest put; put.kind = ReqKind::PUT; put.key = 5; put.value = 500;
        MatrixResponse pr; assert(matrix_deserialize_response(matrix_serve(eng, matrix_serialize_request(put)), pr) && pr.status == 0);
        MatrixRequest get; get.kind = ReqKind::GET; get.key = 5;
        MatrixResponse gr; assert(matrix_deserialize_response(matrix_serve(eng, matrix_serialize_request(get)), gr));
        assert(gr.results.size() == 1 && gr.results[0] == 500);
        MatrixRequest miss; miss.kind = ReqKind::GET; miss.key = 999;
        MatrixResponse mr; assert(matrix_deserialize_response(matrix_serve(eng, matrix_serialize_request(miss)), mr) && mr.results.empty());
    }
    {
        CPUMockEngine eng2(0, wal);
        MatrixRequest get; get.kind = ReqKind::GET; get.key = 5;
        MatrixResponse gr; assert(matrix_deserialize_response(matrix_serve(eng2, matrix_serialize_request(get)), gr));
        assert(gr.results.size() == 1 && gr.results[0] == 500 && "PUT durable through the wire");
    }
    std::remove(wal.c_str());
    std::cout << "[serve get/put + durable] ok\n";
}

static void test_serve_query() {
    const size_t N = 1000; std::vector<uint32_t> col(N);
    for (size_t i = 0; i < N; ++i) col[i] = static_cast<uint32_t>(i);
    CPUMockEngine eng(0, "", SIZE_MAX); eng.load_scan_column(2, col.data(), N);
    MatrixRequest q; q.kind = ReqKind::QUERY; q.query = MatrixQuery{.value_col = 2, .agg = AGG_SUM};
    MatrixResponse r; assert(matrix_deserialize_response(matrix_serve(eng, matrix_serialize_request(q)), r));
    assert(r.status == 0 && r.results.size() == 1);
    std::vector<uint64_t> direct; eng.execute_query(MatrixQuery{.value_col = 2, .agg = AGG_SUM}, direct);
    assert(r.results == direct && "serve QUERY == direct execute_query");
    MatrixRequest bad; bad.kind = ReqKind::QUERY; bad.query = MatrixQuery{.value_col = 999, .agg = AGG_SUM};
    MatrixResponse br; assert(matrix_deserialize_response(matrix_serve(eng, matrix_serialize_request(bad)), br));
    assert(br.status == static_cast<uint32_t>(MatrixQueryStatus::ERR_UNKNOWN_COLUMN) && br.results.empty());
    std::cout << "[serve query] ok\n";
}

static void test_bad_request() {
    CPUMockEngine eng(0, "", SIZE_MAX);
    std::vector<uint8_t> garbage = {1, 2, 3};
    MatrixResponse r; assert(matrix_deserialize_response(matrix_serve(eng, garbage), r));
    assert(r.status == static_cast<uint32_t>(ServerStatus::ERR_BADREQUEST) && "bad request -> ERR_BADREQUEST, no crash");
    std::cout << "[bad request] ok\n";
}

// Proves the field-dropping bug is actually fixed, not just that bytes round-trip in isolation: a
// cross-column WHERE (filter_col) and a BETWEEN predicate (cmp/upper) served over the wire must produce
// the SAME result as calling execute_query directly. Before the fix, filter_col/cmp/upper never reached
// the server (silently defaulted to filter_col=0/cmp=GT), so a networked cross-column-WHERE query
// silently ran a completely different query and returned a plausible-looking wrong answer.
static void test_serve_query_previously_dropped_fields() {
    const size_t N = 20;
    std::vector<uint32_t> value(N), filt(N);
    for (size_t i = 0; i < N; ++i) { value[i] = static_cast<uint32_t>(i * 10); filt[i] = static_cast<uint32_t>(i % 3); }
    CPUMockEngine eng(0, "", SIZE_MAX);
    eng.load_scan_column(1, value.data(), N);   // value column: 0,10,20,...,190
    eng.load_scan_column(2, filt.data(), N);    // filter column: 0,1,2,0,1,2,...

    // Cross-column WHERE: SUM(value) WHERE filt == 1 (filt != value_col, so this exercises filter_col).
    MatrixQuery q{.value_col = 1, .agg = AGG_SUM, .has_filter = true, .threshold = 1, .cmp = MatrixCmp::EQ,
                  .filter_col = 2};
    std::vector<uint64_t> direct;
    assert(eng.execute_query(q, direct) == MatrixQueryStatus::OK);

    MatrixRequest req; req.kind = ReqKind::QUERY; req.query = q;
    MatrixResponse resp;
    assert(matrix_deserialize_response(matrix_serve(eng, matrix_serialize_request(req)), resp));
    assert(resp.status == 0 && resp.results == direct
           && "cross-column WHERE over the wire == direct execute_query (filter_col actually crossed the wire)");
    // Non-vacuity: this must differ from what the OLD (broken) wire format would have silently run
    // (filter_col defaulting to 0, i.e. WHERE value_col == 1 instead of WHERE filt == 1).
    MatrixQuery old_broken_equivalent{.value_col = 1, .agg = AGG_SUM, .has_filter = true, .threshold = 1,
                                       .cmp = MatrixCmp::EQ, .filter_col = 0};
    std::vector<uint64_t> old_result;
    eng.execute_query(old_broken_equivalent, old_result);
    assert(old_result != direct && "sanity: the old silently-wrong query really would have returned a different answer");

    // BETWEEN (cmp/upper) over the wire, cross-checked the same way.
    MatrixQuery bq{.value_col = 1, .agg = AGG_COUNT, .has_filter = true, .threshold = 50, .cmp = MatrixCmp::BETWEEN,
                   .upper = 100};
    std::vector<uint64_t> bdirect;
    assert(eng.execute_query(bq, bdirect) == MatrixQueryStatus::OK);
    MatrixRequest breq; breq.kind = ReqKind::QUERY; breq.query = bq;
    MatrixResponse bresp;
    assert(matrix_deserialize_response(matrix_serve(eng, matrix_serialize_request(breq)), bresp));
    assert(bresp.status == 0 && bresp.results == bdirect && "BETWEEN over the wire == direct execute_query");
    std::cout << "[serve query: previously wire-dropped fields] ok\n";
}

static void test_serve_health() {
    const size_t N = 50; std::vector<uint32_t> col(N, 1);
    CPUMockEngine eng(0, "", SIZE_MAX); eng.load_scan_column(2, col.data(), N);
    MatrixRequest hq; hq.kind = ReqKind::HEALTH;
    MatrixResponse hr; assert(matrix_deserialize_response(matrix_serve(eng, matrix_serialize_request(hq)), hr));
    // results layout: [ready, durable, catalog_columns, host_resident_bytes, wal_pending, dropped]
    assert(hr.status == 0 && hr.results.size() == 6 && "HEALTH -> 6-field snapshot");
    HealthStatus h = eng.health();
    assert(hr.results[0] == (h.ready ? 1u : 0u) && hr.results[1] == (h.durable ? 1u : 0u));
    assert(hr.results[2] == 1 && "one column in the catalog");
    assert(hr.results[3] == N * sizeof(uint32_t) && "resident bytes over the wire");
    assert(hr.results[0] == 1 && hr.results[5] == 0 && "ready, no dropped writes");
    // HEALTH is probeable even by a principal with NO grants (operational status, not data)
    AccessPolicy locked;                                       // grants nothing
    MatrixResponse lr; assert(matrix_deserialize_response(matrix_serve(eng, locked, /*principal=*/42,
                                                          matrix_serialize_request(hq)), lr));
    assert(lr.status == 0 && lr.results.size() == 6 && "HEALTH allowed without grants");
    // a GET by the same locked principal IS denied (proves the policy is otherwise restrictive)
    MatrixRequest g; g.kind = ReqKind::GET; g.key = 1;
    MatrixResponse gr; assert(matrix_deserialize_response(matrix_serve(eng, locked, 42, matrix_serialize_request(g)), gr));
    assert(gr.status == static_cast<uint32_t>(ServerStatus::ERR_FORBIDDEN) && "GET still forbidden for the locked principal");
    std::cout << "[serve health] ok\n";
}

static void test_serve_stats() {
    const size_t N = 100; std::vector<uint32_t> col(N);
    for (size_t i = 0; i < N; ++i) col[i] = static_cast<uint32_t>(i);
    CPUMockEngine eng(0, "", SIZE_MAX); eng.load_scan_column(2, col.data(), N);
    // run a few queries so the metrics are non-zero
    for (int i = 0; i < 3; ++i) { std::vector<uint64_t> o; eng.execute_query(MatrixQuery{.value_col = 2, .agg = AGG_SUM}, o); }
    MatrixRequest sq; sq.kind = ReqKind::STATS;
    MatrixResponse sr; assert(matrix_deserialize_response(matrix_serve(eng, matrix_serialize_request(sq)), sr));
    assert(sr.status == 0 && sr.results.size() == 12 && "STATS -> 12-field metrics snapshot");
    EngineStats s = eng.stats();
    assert(sr.results[3] == 1 && "catalog_columns over the wire");
    assert(sr.results[6] == s.query_count && sr.results[6] >= 3 && "query_count over the wire (>=3 ran)");
    assert(sr.results[10] >= sr.results[9] && "p99 >= p50 (monotone percentiles)");
    assert(sr.results[11] == eng.version_u64() && sr.results[11] != 0 && "server version over the wire");
    // STATS, like HEALTH, is readable without grants (operational metrics, no row data)
    AccessPolicy locked;
    MatrixResponse lr; assert(matrix_deserialize_response(matrix_serve(eng, locked, 42, matrix_serialize_request(sq)), lr));
    assert(lr.status == 0 && lr.results.size() == 12 && "STATS allowed without grants");
    std::cout << "[serve stats] ok\n";
}

int main() {
    test_request_roundtrip();
    test_response_roundtrip();
    test_serve_get_put();
    test_serve_query();
    test_serve_query_previously_dropped_fields();
    test_serve_health();
    test_serve_stats();
    test_bad_request();
    std::cout << "ALL SERVER TESTS PASSED\n";
    return 0;
}

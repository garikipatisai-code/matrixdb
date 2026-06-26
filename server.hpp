#pragma once
// Server core: a serializable request/response protocol + a stateless dispatcher over the engine.
// Transport-agnostic — a TCP/Unix-socket accept-loop is a thin adapter that shuttles these byte
// buffers (deferred; not buildable in this sandbox). Wire form is host-endian (same-machine /
// trusted-transport assumption, like column_io); a cross-arch encoding is a future upgrade.
#include "compute_mock.cpp"   // CPUMockEngine + compute.hpp (MatrixQuery/MatrixQueryStatus/MatrixAggOp)
#include <cstdint>
#include <cstring>
#include <vector>

enum class ReqKind : uint32_t { GET = 1, PUT = 2, QUERY = 3 };
enum class ServerStatus : uint32_t { OK = 0, ERR_BADREQUEST = 1000 };

struct MatrixRequest {
    ReqKind  kind  = ReqKind::GET;
    uint64_t key   = 0;
    uint64_t value = 0;
    MatrixQuery query{};
};
struct MatrixResponse {
    uint32_t status = 0;
    std::vector<uint64_t> results;
};

namespace matrixsrv_detail {
    inline void put_u32(std::vector<uint8_t>& b, uint32_t v) { const uint8_t* p = reinterpret_cast<const uint8_t*>(&v); b.insert(b.end(), p, p + 4); }
    inline void put_u64(std::vector<uint8_t>& b, uint64_t v) { const uint8_t* p = reinterpret_cast<const uint8_t*>(&v); b.insert(b.end(), p, p + 8); }
    struct Reader {
        const uint8_t* p; const uint8_t* end; bool ok = true;
        bool u8(uint8_t& o)   { if (end - p < 1) { ok = false; return false; } o = *p++; return true; }
        bool u32(uint32_t& o) { if (end - p < 4) { ok = false; return false; } std::memcpy(&o, p, 4); p += 4; return true; }
        bool u64(uint64_t& o) { if (end - p < 8) { ok = false; return false; } std::memcpy(&o, p, 8); p += 8; return true; }
        bool done() const { return p == end; }
    };
}

inline std::vector<uint8_t> matrix_serialize_request(const MatrixRequest& r) {
    using namespace matrixsrv_detail;
    std::vector<uint8_t> b;
    put_u32(b, static_cast<uint32_t>(r.kind));
    put_u64(b, r.key); put_u64(b, r.value);
    put_u64(b, r.query.value_col);
    put_u32(b, static_cast<uint32_t>(r.query.agg));
    b.push_back(r.query.has_filter ? 1 : 0);
    put_u32(b, r.query.threshold);
    b.push_back(r.query.grouped ? 1 : 0);
    put_u64(b, r.query.key_col);
    put_u32(b, r.query.num_groups);
    return b;
}
inline bool matrix_deserialize_request(const std::vector<uint8_t>& b, MatrixRequest& out) {
    using namespace matrixsrv_detail;
    Reader r{ b.data(), b.data() + b.size() };
    uint32_t kind = 0, agg = 0; uint8_t hf = 0, gr = 0;
    r.u32(kind); r.u64(out.key); r.u64(out.value);
    r.u64(out.query.value_col); r.u32(agg); r.u8(hf); r.u32(out.query.threshold);
    r.u8(gr); r.u64(out.query.key_col); r.u32(out.query.num_groups);
    if (!r.ok || !r.done()) return false;
    if (kind < 1 || kind > 3) return false;
    out.kind = static_cast<ReqKind>(kind);
    out.query.agg = static_cast<MatrixAggOp>(agg);
    out.query.has_filter = (hf != 0);
    out.query.grouped = (gr != 0);
    return true;
}
inline std::vector<uint8_t> matrix_serialize_response(const MatrixResponse& r) {
    using namespace matrixsrv_detail;
    std::vector<uint8_t> b;
    put_u32(b, r.status);
    put_u64(b, static_cast<uint64_t>(r.results.size()));
    for (uint64_t x : r.results) put_u64(b, x);
    return b;
}
inline bool matrix_deserialize_response(const std::vector<uint8_t>& b, MatrixResponse& out) {
    using namespace matrixsrv_detail;
    Reader r{ b.data(), b.data() + b.size() };
    uint64_t count = 0;
    if (!r.u32(out.status) || !r.u64(count)) return false;
    if (count > (1ull << 28)) return false;
    if (static_cast<uint64_t>(r.end - r.p) != count * 8) return false;
    out.results.resize(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) r.u64(out.results[i]);
    return r.ok && r.done();
}

// Stateless dispatch: deserialize -> run on the engine -> serialize. Never crashes on a bad request
// (returns ERR_BADREQUEST). PUT goes through the transaction/commit path (durable, arbitrary value);
// GET reads kv_; QUERY runs execute_query (its MatrixQueryStatus flows back in `status`).
inline std::vector<uint8_t> matrix_serve(CPUMockEngine& eng, const std::vector<uint8_t>& req_bytes) {
    MatrixRequest req; MatrixResponse resp;
    if (!matrix_deserialize_request(req_bytes, req)) {
        resp.status = static_cast<uint32_t>(ServerStatus::ERR_BADREQUEST);
        return matrix_serialize_response(resp);
    }
    switch (req.kind) {
        case ReqKind::GET: {
            uint64_t v = 0; if (eng.kv_get(req.key, v)) resp.results.push_back(v);
            resp.status = static_cast<uint32_t>(ServerStatus::OK);
            break;
        }
        case ReqKind::PUT: {
            eng.begin(); eng.txn_put(req.key, req.value); eng.commit();
            resp.status = static_cast<uint32_t>(ServerStatus::OK);
            break;
        }
        case ReqKind::QUERY: {
            std::vector<uint64_t> out;
            const MatrixQueryStatus st = eng.execute_query(req.query, out);
            resp.status = static_cast<uint32_t>(st);
            resp.results = std::move(out);
            break;
        }
    }
    return matrix_serialize_response(resp);
}

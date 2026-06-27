#pragma once
// Server core: a serializable request/response protocol + a stateless dispatcher over the engine.
// Transport-agnostic — a TCP/Unix-socket accept-loop is a thin adapter that shuttles these byte
// buffers (deferred; not buildable in this sandbox). Wire form is host-endian (same-machine /
// trusted-transport assumption, like column_io); a cross-arch encoding is a future upgrade.
#include "compute_mock.cpp"   // CPUMockEngine + compute.hpp (MatrixQuery/MatrixQueryStatus/MatrixAggOp)
#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <vector>

enum class ReqKind : uint32_t { GET = 1, PUT = 2, QUERY = 3 };
enum class ServerStatus : uint32_t { OK = 0, ERR_BADREQUEST = 1000, ERR_FORBIDDEN = 1001 };

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

// One audited request: principal, kind (ReqKind value, or 0 for a malformed request), the response
// status, and the target (GET/PUT key; QUERY value_col). The irrelevant target field is 0.
struct AuditEntry { uint64_t principal; uint32_t kind; uint32_t status; uint64_t key; uint64_t column; };

// Append-only audit trail of served requests (allowed, denied, and malformed alike).
class AuditLog {
public:
    void record(const AuditEntry& e) { entries_.push_back(e); }
    const std::vector<AuditEntry>& entries() const { return entries_; }
    size_t size() const { return entries_.size(); }
private:
    std::vector<AuditEntry> entries_;
};

// Per-principal authorization. Grants are additive; a principal with no grants can do nothing.
// permissive() allows everything (the no-auth / backward-compat default).
class AccessPolicy {
public:
    static AccessPolicy permissive() { AccessPolicy p; p.allow_all_ = true; return p; }
    void allow_column(uint64_t principal, uint64_t col) { cols_[principal].insert(col); } // QUERY access
    void allow_read(uint64_t principal)  { read_.insert(principal); }                      // GET
    void allow_write(uint64_t principal) { write_.insert(principal); }                     // PUT
    bool can_query(uint64_t principal, uint64_t col) const {
        if (allow_all_) return true;
        auto it = cols_.find(principal); return it != cols_.end() && it->second.count(col) != 0;
    }
    bool can_read(uint64_t principal)  const { return allow_all_ || read_.count(principal)  != 0; }
    bool can_write(uint64_t principal) const { return allow_all_ || write_.count(principal) != 0; }
private:
    bool allow_all_ = false;
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> cols_;
    std::unordered_set<uint64_t> read_, write_;
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

namespace matrixsrv_detail {
// The full serve pipeline (deserialize -> authorize -> dispatch), also filling `out` with the
// audit record for this request. Returns the serialized response. Shared by all matrix_serve overloads.
inline std::vector<uint8_t> serve_core(CPUMockEngine& eng, const AccessPolicy& policy,
                                       uint64_t principal, const std::vector<uint8_t>& req_bytes,
                                       AuditEntry& out) {
    MatrixRequest req; MatrixResponse resp;
    out.principal = principal; out.kind = 0; out.key = 0; out.column = 0;
    if (!matrix_deserialize_request(req_bytes, req)) {
        resp.status = static_cast<uint32_t>(ServerStatus::ERR_BADREQUEST);
        out.status = resp.status;
        return matrix_serialize_response(resp);
    }
    out.kind = static_cast<uint32_t>(req.kind);
    out.key = req.key;
    out.column = req.query.value_col;
    bool authorized = false;
    switch (req.kind) {
        case ReqKind::GET:   authorized = policy.can_read(principal);  break;
        case ReqKind::PUT:   authorized = policy.can_write(principal); break;
        case ReqKind::QUERY: authorized = policy.can_query(principal, req.query.value_col)
                                 && (!req.query.grouped || policy.can_query(principal, req.query.key_col)); break;
    }
    if (!authorized) {
        resp.status = static_cast<uint32_t>(ServerStatus::ERR_FORBIDDEN);
        out.status = resp.status;
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
            std::vector<uint64_t> o;
            const MatrixQueryStatus st = eng.execute_query(req.query, o);
            resp.status = static_cast<uint32_t>(st);
            resp.results = std::move(o);
            break;
        }
    }
    out.status = resp.status;
    return matrix_serialize_response(resp);
}
} // namespace matrixsrv_detail

// Authorizing dispatch (principal supplied by the authenticated caller; denied -> ERR_FORBIDDEN
// with no engine side effects; bad request -> ERR_BADREQUEST). See SE-2.
inline std::vector<uint8_t> matrix_serve(CPUMockEngine& eng, const AccessPolicy& policy,
                                         uint64_t principal, const std::vector<uint8_t>& req_bytes) {
    AuditEntry ignored;
    return matrixsrv_detail::serve_core(eng, policy, principal, req_bytes, ignored);
}
// Same, but records the request (allowed / denied / malformed) to `audit`.
inline std::vector<uint8_t> matrix_serve(CPUMockEngine& eng, const AccessPolicy& policy,
                                         uint64_t principal, const std::vector<uint8_t>& req_bytes,
                                         AuditLog& audit) {
    AuditEntry entry;
    auto resp = matrixsrv_detail::serve_core(eng, policy, principal, req_bytes, entry);
    audit.record(entry);
    return resp;
}
// No-auth convenience (backward-compat): permissive policy, anonymous principal.
inline std::vector<uint8_t> matrix_serve(CPUMockEngine& eng, const std::vector<uint8_t>& req_bytes) {
    static const AccessPolicy permissive = AccessPolicy::permissive();
    return matrix_serve(eng, permissive, /*principal=*/0, req_bytes);
}

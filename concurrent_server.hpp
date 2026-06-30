#pragma once
// NW-2 concurrent serving (single-writer / many-readers). A thin dispatch layer over the existing
// matrix_serve wire protocol: it owns a std::shared_mutex and takes it shared for reads / exclusive for
// writes, so many analytical reads run in parallel while writes (and reads that need a tier borrow)
// serialize. QUERY first tries the lock-free fast path (execute_query_shared) under the shared lock; if the
// query touches a non-HOST column it returns NEEDS_EXCLUSIVE and we re-run the full matrix_serve under the
// exclusive lock. ConcurrentServer::serve() is safe to call from many threads; that is the concurrency
// unit (verified via threads under ThreadSanitizer). A real TCP accept pool is host-only (bind() is
// sandbox-blocked) and is deferred — each connection thread would simply call serve() per framed request.
#include "server.hpp"
#include <shared_mutex>

class ConcurrentServer {
public:
    ConcurrentServer(CPUMockEngine& eng, const AccessPolicy& policy, uint64_t principal = 0)
        : eng_(eng), policy_(policy), principal_(principal) {}

    // Serve one framed request; safe to call concurrently. Returns the serialized response bytes.
    std::vector<uint8_t> serve(const std::vector<uint8_t>& req_bytes) {
        MatrixRequest req;
        const bool ok = matrix_deserialize_request(req_bytes, req);
        if (ok && req.kind == ReqKind::QUERY) {
            {
                std::shared_lock<std::shared_mutex> r(mu_);
                const bool authorized = policy_.can_query(principal_, req.query.value_col)
                                      && (!req.query.grouped || policy_.can_query(principal_, req.query.key_col));
                if (!authorized)                                   // FORBIDDEN: matrix_serve answers, no engine touch
                    return matrix_serve(eng_, policy_, principal_, req_bytes);
                std::vector<uint64_t> out;
                if (eng_.execute_query_shared(req.query, out) == CPUMockEngine::ReadOutcome::SERVED) {
                    MatrixResponse resp;
                    resp.status = static_cast<uint32_t>(ServerStatus::OK);
                    resp.results = std::move(out);
                    return matrix_serialize_response(resp);
                }
            }   // authorized but not all-HOST -> escalate to the exclusive (borrowing) path
            std::unique_lock<std::shared_mutex> w(mu_);
            return matrix_serve(eng_, policy_, principal_, req_bytes);
        }
        if (ok && req.kind == ReqKind::PUT) {                       // writes serialize
            std::unique_lock<std::shared_mutex> w(mu_);
            return matrix_serve(eng_, policy_, principal_, req_bytes);
        }
        std::shared_lock<std::shared_mutex> r(mu_);                 // GET / HEALTH / STATS / malformed
        return matrix_serve(eng_, policy_, principal_, req_bytes);
    }

private:
    CPUMockEngine& eng_;
    const AccessPolicy& policy_;
    uint64_t principal_;
    std::shared_mutex mu_;
};

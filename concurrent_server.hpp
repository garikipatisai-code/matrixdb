#pragma once
// NW-2 concurrent serving (single-writer / many-readers). A thin dispatch layer over the existing
// matrix_serve wire protocol: it owns a std::shared_mutex and takes it shared for reads / exclusive for
// writes, so many analytical reads run in parallel while writes (and reads that need a tier borrow)
// serialize. QUERY first tries the lock-free fast path (execute_query_shared) under the shared lock; if the
// query touches a non-HOST column it returns NEEDS_EXCLUSIVE and we re-run the full matrix_serve under the
// exclusive lock. ConcurrentServer::serve() is safe to call from many threads; that is the concurrency
// unit (verified via threads under ThreadSanitizer). Wired into matrixdbd's real TCP accept loop
// (server_tcp.hpp's matrix_serve_tcp/matrix_serve_tcp_auth): one thread per accepted connection, all
// sharing one ConcurrentServer instance so the mutex actually serializes across connections, not just
// within one.
#include "server.hpp"
#include <mutex>
#include <shared_mutex>

class ConcurrentServer {
public:
    // `policy` is stored BY VALUE (AccessPolicy is a small map+two-sets value type, cheap to copy, and is
    // routinely handed to callers as a temporary via AccessPolicy::permissive()/factory-style construction
    // -- storing it as `const AccessPolicy&` here made `ConcurrentServer srv(eng, AccessPolicy::permissive())`
    // a dangling reference the instant the constructor call's full expression ended (the temporary is
    // destroyed; every later serve() call reads through a reference to freed memory). By-value ownership
    // makes that call shape simply correct instead of silently undefined -- caught by this file's own new
    // test (test_server_tcp.cpp's concurrent accept-loop test), which failed every request with
    // ERR_FORBIDDEN because the dangling policy's allow_all_ read back as garbage-false.
    ConcurrentServer(CPUMockEngine& eng, AccessPolicy policy)
        : eng_(eng), policy_(std::move(policy)) {}

    // Serve one framed request as `principal`; safe to call concurrently (each call may come from a
    // different connection/thread with its own authenticated principal). Returns the serialized response.
    std::vector<uint8_t> serve(const std::vector<uint8_t>& req_bytes, uint64_t principal = 0) {
        MatrixRequest req;
        const bool ok = matrix_deserialize_request(req_bytes, req);
        if (ok && req.kind == ReqKind::QUERY) {
            {
                std::shared_lock<std::shared_mutex> r(mu_);
                const bool authorized = policy_.can_query(principal, req.query.value_col)
                                      && (!req.query.grouped || policy_.can_query(principal, req.query.key_col));
                if (!authorized)                                   // FORBIDDEN: matrix_serve answers, no engine touch
                    return matrix_serve(eng_, policy_, principal, req_bytes);
                std::vector<uint64_t> out;
                if (eng_.execute_query_shared(req.query, out) == CPUMockEngine::ReadOutcome::SERVED) {
                    MatrixResponse resp;
                    resp.status = static_cast<uint32_t>(ServerStatus::OK);
                    resp.results = std::move(out);
                    return matrix_serialize_response(resp);
                }
            }   // authorized but not all-HOST -> escalate to the exclusive (borrowing) path
            std::unique_lock<std::shared_mutex> w(mu_);
            return matrix_serve(eng_, policy_, principal, req_bytes);
        }
        if (ok && req.kind == ReqKind::PUT) {                       // writes serialize
            std::unique_lock<std::shared_mutex> w(mu_);
            return matrix_serve(eng_, policy_, principal, req_bytes);
        }
        std::shared_lock<std::shared_mutex> r(mu_);                 // GET / HEALTH / STATS / malformed
        return matrix_serve(eng_, policy_, principal, req_bytes);
    }

private:
    CPUMockEngine& eng_;
    AccessPolicy policy_;
    std::shared_mutex mu_;
};

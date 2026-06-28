#pragma once
// NW-4 client driver — the app-side of the wire protocol. Wraps a connected stream socket `fd` with
// typed calls that frame a request ([u32 len][bytes]), send it, read the framed response, and
// deserialize. The inverse of matrix_serve_conn; reuses the same length-prefixed framing + recv_all/
// send_all, so it interoperates with matrix_serve_tcp on a real host. One request in flight at a time
// (matches the single-owner serving model). A transport failure (peer closed / short frame) -> false.
#include "server.hpp"
#include "server_tcp.hpp"   // matrixsrv_detail::recv_all / send_all + the framing contract
#include <cstdint>
#include <vector>

class MatrixClient {
public:
    explicit MatrixClient(int fd) : fd_(fd) {}

    // SE-1 handshake: send the leading token frame ([u32 len][token]) that authenticates this connection.
    // Call ONCE before any op when the server uses matrix_serve_conn_auth. Returns false on a transport
    // error. (The server validates the token; an invalid one makes it close the connection, so subsequent
    // ops here return false.)
    bool authenticate(const std::string& token) {
        const uint32_t len = static_cast<uint32_t>(token.size());
        if (!matrixsrv_detail::send_all(fd_, &len, sizeof len)) return false;
        return len == 0 || matrixsrv_detail::send_all(fd_, token.data(), len);
    }

    // GET key -> value. Returns false on a transport error OR a miss (no such key); true + out on a hit.
    bool get(uint64_t key, uint64_t& out) {
        MatrixRequest r; r.kind = ReqKind::GET; r.key = key;
        MatrixResponse resp;
        if (!round_trip(r, resp) || resp.status != 0 || resp.results.empty()) return false;
        out = resp.results[0];
        return true;
    }

    // PUT key=value (durably committed server-side). Returns true on OK.
    bool put(uint64_t key, uint64_t value) {
        MatrixRequest r; r.kind = ReqKind::PUT; r.key = key; r.value = value;
        MatrixResponse resp;
        return round_trip(r, resp) && resp.status == 0;
    }

    // Run an analytical query. Returns false on a transport error; otherwise true with the query's
    // MatrixQueryStatus in `st` and the result vector in `out` (so a wire failure is distinct from a
    // query-level error like ERR_UNKNOWN_COLUMN).
    bool query(const MatrixQuery& q, MatrixQueryStatus& st, std::vector<uint64_t>& out) {
        MatrixRequest r; r.kind = ReqKind::QUERY; r.query = q;
        MatrixResponse resp;
        if (!round_trip(r, resp)) return false;
        st = static_cast<MatrixQueryStatus>(resp.status);
        out = resp.results;
        return true;
    }

    // HEALTH probe -> the server's readiness snapshot. False on transport error / unexpected shape.
    bool health(HealthStatus& out) {
        MatrixRequest r; r.kind = ReqKind::HEALTH;
        MatrixResponse resp;
        if (!round_trip(r, resp) || resp.status != 0 || resp.results.size() != 6) return false;
        const auto& f = resp.results;
        out = HealthStatus{ f[0] != 0, f[1] != 0, static_cast<size_t>(f[2]),
                            static_cast<size_t>(f[3]), f[4], f[5] };
        return true;
    }

    // STATS -> the raw 11-field metrics vector (the STATS wire layout; see serve_core). False on error.
    bool stats(std::vector<uint64_t>& out) {
        MatrixRequest r; r.kind = ReqKind::STATS;
        MatrixResponse resp;
        if (!round_trip(r, resp) || resp.status != 0 || resp.results.size() != 11) return false;
        out = resp.results;
        return true;
    }

private:
    int fd_;
    // Frame + send the request, read + deframe the response. False on any short transfer / EOF / bad frame.
    bool round_trip(const MatrixRequest& req, MatrixResponse& resp) {
        const std::vector<uint8_t> rb = matrix_serialize_request(req);
        const uint32_t len = static_cast<uint32_t>(rb.size());
        if (!matrixsrv_detail::send_all(fd_, &len, sizeof len)) return false;
        if (len != 0 && !matrixsrv_detail::send_all(fd_, rb.data(), len)) return false;
        uint32_t rlen = 0;
        if (!matrixsrv_detail::recv_all(fd_, &rlen, sizeof rlen)) return false;
        if (rlen > (1u << 28)) return false;                       // sane cap — never alloc on a bogus length
        std::vector<uint8_t> respb(rlen);
        if (rlen != 0 && !matrixsrv_detail::recv_all(fd_, respb.data(), rlen)) return false;
        return matrix_deserialize_response(respb, resp);
    }
};

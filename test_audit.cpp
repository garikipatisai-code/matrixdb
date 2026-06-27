// CPU test for audit logging (AuditLog + matrix_serve audit overload).
#include "server.hpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <string>
#include <cstdio>
#include <iostream>

static MatrixRequest mk(ReqKind kind) { MatrixRequest r; r.kind = kind; return r; }

static void test_audit() {
    const size_t N = 1000; std::vector<uint32_t> v(N);
    for (size_t i = 0; i < N; ++i) v[i] = static_cast<uint32_t>(i);
    const std::string wal = "/tmp/matrixdb_audit.bin"; std::remove(wal.c_str());
    CPUMockEngine eng(0, wal);
    eng.load_scan_column(2, v.data(), N);
    AccessPolicy p; p.allow_column(1, 2); p.allow_read(1); p.allow_write(1);
    AuditLog audit;

    MatrixRequest get = mk(ReqKind::GET); get.key = 5;
    matrix_serve(eng, p, 1, matrix_serialize_request(get), audit);
    MatrixRequest put = mk(ReqKind::PUT); put.key = 5; put.value = 500;
    matrix_serve(eng, p, 2, matrix_serialize_request(put), audit);
    MatrixRequest q = mk(ReqKind::QUERY); q.query = MatrixQuery{.value_col = 2, .agg = AGG_SUM};
    matrix_serve(eng, p, 1, matrix_serialize_request(q), audit);
    std::vector<uint8_t> garbage = {1, 2, 3};
    matrix_serve(eng, p, 7, garbage, audit);

    const auto& e = audit.entries();
    assert(audit.size() == 4 && "every served request is audited");
    assert(e[0].principal == 1 && e[0].kind == static_cast<uint32_t>(ReqKind::GET)
           && e[0].status == static_cast<uint32_t>(ServerStatus::OK) && e[0].key == 5);
    assert(e[1].principal == 2 && e[1].kind == static_cast<uint32_t>(ReqKind::PUT)
           && e[1].status == static_cast<uint32_t>(ServerStatus::ERR_FORBIDDEN) && e[1].key == 5);
    assert(e[2].principal == 1 && e[2].kind == static_cast<uint32_t>(ReqKind::QUERY)
           && e[2].status == static_cast<uint32_t>(MatrixQueryStatus::OK) && e[2].column == 2);
    assert(e[3].principal == 7 && e[3].kind == 0
           && e[3].status == static_cast<uint32_t>(ServerStatus::ERR_BADREQUEST));
    std::remove(wal.c_str());
    std::cout << "[audit log] ok\n";
}

int main() {
    test_audit();
    std::cout << "ALL AUDIT TESTS PASSED\n";
    return 0;
}

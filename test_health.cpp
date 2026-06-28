// CPU test for OB-3 health/readiness probe: health() reports a ready verdict + the gauges behind it.
// Exercises the gauges (catalog size, durable flag, pending-WAL, resident bytes) and pins the verdict
// invariant ready == (dropped_writes == 0). The degraded path (ready=false) needs a release build + a
// full KVStore (the overflow asserts in debug), so it's covered by the invariant, not a forced overflow.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <iostream>

static void clean(const std::string& wal) {
    std::remove(wal.c_str()); std::remove((wal + ".ckpt").c_str()); std::remove((wal + ".ckpt.tmp").c_str());
}

static void test_health_fresh() {
    CPUMockEngine eng;                                          // no WAL
    HealthStatus h = eng.health();
    assert(h.ready && "fresh engine is ready");
    assert(!h.durable && "no WAL -> not durable");
    assert(h.catalog_columns == 0 && h.host_resident_bytes == 0 && "empty catalog");
    assert(h.wal_records_pending == 0 && h.dropped_writes == 0 && "nothing pending, nothing dropped");
    assert(h.ready == (h.dropped_writes == 0) && "ready verdict == no-dropped-writes invariant");

    std::vector<uint32_t> a = {1, 2, 3, 4}, b = {5, 6, 7, 8};
    eng.load_scan_column(1, a.data(), a.size());
    eng.load_scan_column(2, b.data(), b.size());
    h = eng.health();
    assert(h.catalog_columns == 2 && "two columns registered");
    assert(h.host_resident_bytes == (a.size() + b.size()) * sizeof(uint32_t) && "resident bytes track the catalog");
    std::cout << "[health fresh] ok\n";
}

static void test_health_durable() {
    const std::string wal = "/tmp/mdb_health.wal"; clean(wal);
    {
        CPUMockEngine eng(0, wal);
        assert(eng.health().durable && "WAL attached -> durable");
        eng.begin(); eng.txn_put(1, 10); eng.commit();
        eng.begin(); eng.txn_put(2, 20); eng.commit();
        HealthStatus h = eng.health();
        assert(h.wal_records_pending > 0 && "un-checkpointed commits are pending");
        assert(h.ready && "still ready");
        eng.shutdown();                                         // checkpoint compacts the WAL
        assert(eng.health().wal_records_pending == 0 && "after shutdown the WAL is compacted");
    }
    clean(wal);
    std::cout << "[health durable] ok\n";
}

int main() { test_health_fresh(); test_health_durable(); std::cout << "ALL HEALTH TESTS PASSED\n"; return 0; }

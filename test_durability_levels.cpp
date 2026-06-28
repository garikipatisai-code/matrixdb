// CPU test for DU-5 durability levels: the engine constructor selects the WAL fsync policy. SYNC_EACH
// (default) fsyncs each commit (survives power loss); SYNC_OFF buffers for throughput. Both recover
// committed writes on a clean restart — the power-loss difference is inherent to fsync, not unit-testable
// (a clean process exit flushes either way), so this pins plumbing + recovery, and documents the rest.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <iostream>

static void clean(const std::string& wal) {
    std::remove(wal.c_str()); std::remove((wal + ".ckpt").c_str()); std::remove((wal + ".ckpt.tmp").c_str());
}
static uint64_t get(CPUMockEngine& e, uint64_t k) { uint64_t v = 0; e.kv_get(k, v); return v; }

static void test_default_is_sync_each() {
    const std::string wal = "/tmp/mdb_du5_a.wal"; clean(wal);
    CPUMockEngine e(0, wal);                                    // default sync policy
    assert(e.durability_level() == SyncPolicy::SYNC_EACH && "default WAL durability is SYNC_EACH (fsync per commit)");
    CPUMockEngine mem;                                         // no WAL
    assert(mem.durability_level() == SyncPolicy::SYNC_OFF && "no WAL -> nothing to sync");
    clean(wal);
    std::cout << "[du5 default sync_each] ok\n";
}

static void test_policy_selectable_and_recovers() {
    const std::string wal = "/tmp/mdb_du5_b.wal"; clean(wal);
    {
        CPUMockEngine e(0, wal, SIZE_MAX, SyncPolicy::SYNC_OFF);   // operator opts into throughput mode
        assert(e.durability_level() == SyncPolicy::SYNC_OFF && "SYNC_OFF selected via constructor");
        e.begin(); e.txn_put(1, 111); e.commit();
        e.begin(); e.txn_put(2, 222); e.commit();
    }
    // clean restart recovers committed writes regardless of policy (OS flushed buffers on close)
    {
        CPUMockEngine e(0, wal, SIZE_MAX, SyncPolicy::SYNC_OFF);
        assert(get(e, 1) == 111 && get(e, 2) == 222 && "committed writes recover under SYNC_OFF on a clean restart");
    }
    clean(wal);
    std::cout << "[du5 selectable + recovers] ok\n";
}

int main() { test_default_is_sync_each(); test_policy_selectable_and_recovers();
             std::cout << "ALL DURABILITY-LEVEL TESTS PASSED\n"; return 0; }

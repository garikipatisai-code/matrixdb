// CPU test for RM-4 graceful shutdown: shutdown() rolls back any open txn, then checkpoints the WAL so a
// restart replays an ~empty log (bounded recovery). Committed writes survive; uncommitted are discarded.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <iostream>

static void clean(const std::string& wal) {
    std::remove(wal.c_str());
    std::remove((wal + ".ckpt").c_str());
    std::remove((wal + ".ckpt.tmp").c_str());
}

static uint64_t get(CPUMockEngine& e, uint64_t k) { uint64_t v = 0; e.kv_get(k, v); return v; }

static void test_shutdown_checkpoints_and_recovers() {
    const std::string wal = "/tmp/mdb_rm4.wal"; clean(wal);
    {
        CPUMockEngine e(0, wal);
        e.begin(); e.txn_put(1, 111); e.commit();
        e.begin(); e.txn_put(2, 222); e.commit();
        assert(e.wal_records() > 0 && "writes appended to the WAL");
        e.shutdown();                                            // checkpoint: snapshot + truncate
        assert(e.wal_records() == 0 && e.checkpoints() == 1 && "shutdown truncated the WAL via a checkpoint");
    }
    // restart on the same path: recovers from the checkpoint with an empty WAL replay
    {
        CPUMockEngine e(0, wal);
        assert(get(e, 1) == 111 && get(e, 2) == 222 && "committed writes survive shutdown+restart");
        assert(e.wal_records() == 0 && "restart replayed an ~empty WAL (fast recovery)");
    }
    clean(wal);
    std::cout << "[shutdown checkpoints+recovers] ok\n";
}

static void test_shutdown_rolls_back_open_txn() {
    const std::string wal = "/tmp/mdb_rm4b.wal"; clean(wal);
    {
        CPUMockEngine e(0, wal);
        e.begin(); e.txn_put(7, 777); e.commit();
        e.begin(); e.txn_put(8, 888);                            // NOT committed
        e.shutdown();                                            // rolls back the open txn, then checkpoints
    }
    {
        CPUMockEngine e(0, wal);
        assert(get(e, 7) == 777 && "committed write survives");
        assert(get(e, 8) == 0 && "uncommitted write discarded on graceful shutdown");
    }
    clean(wal);
    std::cout << "[shutdown rolls back open txn] ok\n";
}

static void test_shutdown_idempotent_and_no_wal() {
    // idempotent: a second shutdown is safe (and a third)
    const std::string wal = "/tmp/mdb_rm4c.wal"; clean(wal);
    { CPUMockEngine e(0, wal); e.begin(); e.txn_put(1, 1); e.commit();
      e.shutdown(); e.shutdown(); assert(e.wal_records() == 0 && "repeated shutdown stays clean"); }
    clean(wal);
    // no WAL attached -> shutdown is a no-op, never crashes
    { CPUMockEngine e; e.shutdown(); assert(e.checkpoints() == 0 && "shutdown without a WAL is a no-op"); }
    std::cout << "[shutdown idempotent + no-WAL] ok\n";
}

int main() {
    test_shutdown_checkpoints_and_recovers();
    test_shutdown_rolls_back_open_txn();
    test_shutdown_idempotent_and_no_wal();
    std::cout << "ALL SHUTDOWN TESTS PASSED\n";
    return 0;
}

// CPU test for WAL checkpoint / compaction (DU-4): snapshot kv_ + truncate WAL; recover from both.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <iostream>

// Remove a WAL path and its derived checkpoint/temp files so each sub-test starts clean.
static void clean(const std::string& wal) {
    std::remove(wal.c_str());
    std::remove((wal + ".ckpt").c_str());
    std::remove((wal + ".ckpt.tmp").c_str());
}

// Write keys [lo, hi) as value=key*10+1, each in its own committed transaction.
static void write_range(CPUMockEngine& e, uint64_t lo, uint64_t hi) {
    for (uint64_t k = lo; k < hi; ++k) { e.begin(); e.txn_put(k, k * 10 + 1); e.commit(); }
}

static void test_wal_shrinks_and_recovers() {
    const std::string wal = "/tmp/mdb_ckpt_a.wal"; clean(wal);
    {
        CPUMockEngine e(0, wal);
        e.begin(); for (uint64_t k = 0; k < 100; ++k) e.txn_put(k, k * 10 + 1); e.commit();
        assert(e.wal_records() == 101 && "100 txn-puts + 1 commit marker");
        e.checkpoint();
        assert(e.wal_records() == 0 && e.checkpoints() == 1 && "WAL truncated after checkpoint");
        write_range(e, 100, 110);                       // 10 more txns post-checkpoint
        assert(e.wal_records() == 20 && "10 txn-puts + 10 commit markers since checkpoint");
    }
    {   // restart on the same WAL: checkpoint (0..99) + replayed WAL (100..109)
        CPUMockEngine e(0, wal);
        for (uint64_t k = 0; k < 110; ++k) {
            uint64_t v = 0; assert(e.kv_get(k, v) && v == k * 10 + 1 && "all 110 keys survive restart");
        }
    }
    clean(wal);
    std::cout << "[checkpoint shrink+recover] ok\n";
}

static void test_checkpoint_alone_carries_data() {   // NON-VACUITY: data must come from the checkpoint file
    const std::string wal = "/tmp/mdb_ckpt_b.wal"; clean(wal);
    {
        CPUMockEngine e(0, wal);
        e.begin(); for (uint64_t k = 0; k < 100; ++k) e.txn_put(k, k * 10 + 1); e.commit();
        e.checkpoint();                                 // WAL now empty; data only in the .ckpt file
        assert(e.wal_records() == 0);
    }
    {
        CPUMockEngine e(0, wal);                        // replay sees an EMPTY WAL — only the checkpoint can restore
        size_t found = 0;
        for (uint64_t k = 0; k < 100; ++k) { uint64_t v = 0; if (e.kv_get(k, v) && v == k * 10 + 1) ++found; }
        assert(found == 100 && "checkpoint file alone restored every key");
    }
    clean(wal);
    std::cout << "[checkpoint alone] ok\n";
}

static void test_idempotent_overlap() {                 // a key written before AND after checkpoint
    const std::string wal = "/tmp/mdb_ckpt_c.wal"; clean(wal);
    {
        CPUMockEngine e(0, wal);
        e.begin(); e.txn_put(7, 700); e.commit();
        e.checkpoint();
        e.begin(); e.txn_put(7, 777); e.commit();       // post-checkpoint overwrite
    }
    {
        CPUMockEngine e(0, wal);
        uint64_t v = 0; assert(e.kv_get(7, v) && v == 777 && "last write wins; checkpoint value not stale/doubled");
    }
    clean(wal);
    std::cout << "[idempotent overlap] ok\n";
}

static void test_no_wal_noop() {
    CPUMockEngine e(0, "");                              // durability off
    e.checkpoint();                                     // must not crash
    assert(e.checkpoints() == 0 && e.wal_records() == 0 && "checkpoint is a no-op without a WAL");
    std::cout << "[no-wal no-op] ok\n";
}

int main() {
    test_wal_shrinks_and_recovers();
    test_checkpoint_alone_carries_data();
    test_idempotent_overlap();
    test_no_wal_noop();
    std::cout << "ALL CHECKPOINT TESTS PASSED\n";
    return 0;
}

// CPU test for atomic transactions (WAL group commit). Grows across tasks; one main().
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <cstdio>
#include <iostream>

static std::vector<std::pair<uint64_t,uint64_t>> replay_all(const std::string& path) {
    std::vector<std::pair<uint64_t,uint64_t>> applied;
    ColdStore cs(path, SyncPolicy::SYNC_OFF);
    cs.replay([&](uint64_t k, uint64_t v){ applied.emplace_back(k, v); });
    return applied;
}

static void test_wal_commit_atomicity() {
    const std::string path = "/tmp/matrixdb_txn_wal.bin";
    std::remove(path.c_str());
    {
        ColdStore cs(path, SyncPolicy::SYNC_OFF);
        cs.append_txn_put(10, 100);
        cs.append_txn_put(11, 110);
        cs.append_commit();              // committed group {10,11}
        cs.append_txn_put(12, 120);      // a "crash": txn-put with no following commit marker
    }
    auto applied = replay_all(path);
    assert(applied.size() == 2 && "only the committed group replays");
    assert(applied[0].first == 10 && applied[1].first == 11);
    bool saw12 = false; for (auto& kv : applied) if (kv.first == 12) saw12 = true;
    assert(!saw12 && "uncommitted txn-put is discarded (crash-atomic)");
    std::remove(path.c_str());
    std::cout << "[wal commit atomicity] ok\n";
}

static void test_wal_autocommit_unchanged() {
    const std::string path = "/tmp/matrixdb_txn_auto.bin";
    std::remove(path.c_str());
    {
        ColdStore cs(path, SyncPolicy::SYNC_OFF);
        cs.append_put(1, 11);
        cs.append_put(2, 22);
    }
    auto applied = replay_all(path);
    assert(applied.size() == 2 && applied[0].first == 1 && applied[1].first == 2
           && "auto-commit puts still apply immediately (backward compat)");
    std::remove(path.c_str());
    std::cout << "[wal auto-commit unchanged] ok\n";
}

static void test_engine_commit_durable() {
    const std::string wal = "/tmp/matrixdb_txn_eng.bin";
    std::remove(wal.c_str());
    {
        CPUMockEngine eng(0, wal);            // durability ON
        eng.begin();
        eng.txn_put(100, 1000);
        eng.txn_put(101, 1010);
        eng.txn_put(102, 1020);
        eng.commit();
        uint64_t v = 0;
        assert(eng.kv_get(100, v) && v == 1000);
        assert(eng.kv_get(102, v) && v == 1020);
    }
    {
        CPUMockEngine eng2(0, wal);           // replay the committed txn on restart
        uint64_t v = 0;
        assert(eng2.kv_get(100, v) && v == 1000 && "committed txn survives restart");
        assert(eng2.kv_get(101, v) && v == 1010);
        assert(eng2.kv_get(102, v) && v == 1020);
    }
    std::remove(wal.c_str());
    std::cout << "[engine commit durable] ok\n";
}

static void test_engine_rollback() {
    const std::string wal = "/tmp/matrixdb_txn_rb.bin";
    std::remove(wal.c_str());
    {
        CPUMockEngine eng(0, wal);
        eng.begin();
        eng.txn_put(200, 2000);
        eng.rollback();
        uint64_t v = 0;
        assert(!eng.kv_get(200, v) && "rolled-back write is not visible");
    }
    {
        CPUMockEngine eng2(0, wal);
        uint64_t v = 0;
        assert(!eng2.kv_get(200, v) && "rolled-back write was never persisted");
    }
    std::remove(wal.c_str());
    std::cout << "[engine rollback] ok\n";
}

int main() {
    test_wal_commit_atomicity();
    test_wal_autocommit_unchanged();
    test_engine_commit_durable();
    test_engine_rollback();
    std::cout << "ALL TRANSACTION TESTS PASSED\n";
    return 0;
}

// CPU test for atomic transactions (WAL group commit). Grows across tasks; one main().
#include "compute_mock.cpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <cstdio>
#include <iostream>
#include <thread>
#include <shared_mutex>
#include <atomic>

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

// TX-2 testing gap (found by audit, closed here): no test anywhere exercised concurrent
// multi-statement transactions. begin()/txn_put()/commit() are NOT internally synchronized
// (in_txn_/txn_buf_ are plain fields) -- by design, matching the single-owner/lock-free thesis: a
// caller is responsible for serializing access, the same way ConcurrentServer serializes single-key
// auto-commit PUTs under an exclusive lock. This proves that documented usage pattern, generalized
// to a real multi-key transaction, actually delivers cross-key atomicity under real concurrent
// threads -- something no existing test covered (test_concurrent_serving.cpp only ever composes
// single-key auto-commit PUTs, never a multi-put begin/txn_put x N/commit group).
//
// Design: two keys (A, B) with the invariant B == A+1, maintained ONLY by committing both together
// in one transaction. Multiple writer threads race to advance the pair (each holding an exclusive
// lock for its whole begin->txn_put->txn_put->commit); concurrent reader threads (shared lock)
// repeatedly read both keys and check the invariant. If cross-key atomicity broke -- e.g. a reader
// could observe A updated but B still stale -- a reader would catch a "torn" pair sooner or later
// under enough concurrent iterations.
static void test_concurrent_multi_key_transactions() {
    CPUMockEngine eng;   // no WAL: pure in-memory concurrency test
    std::shared_mutex mu;
    constexpr uint64_t KEY_A = 1, KEY_B = 2;
    { std::unique_lock<std::shared_mutex> w(mu); eng.begin(); eng.txn_put(KEY_A, 0); eng.txn_put(KEY_B, 1); eng.commit(); }

    constexpr int kWriters = 3, kRoundsPerWriter = 2000, kReaders = 4;
    std::atomic<bool> stop{false};
    std::atomic<int> torn_reads{0};
    std::atomic<int> rounds_committed{0};

    auto writer = [&] {
        for (int round = 1; round <= kRoundsPerWriter; ++round) {
            std::unique_lock<std::shared_mutex> w(mu);
            eng.begin();
            eng.txn_put(KEY_A, static_cast<uint64_t>(round));
            eng.txn_put(KEY_B, static_cast<uint64_t>(round) + 1);   // invariant: B == A+1
            eng.commit();
            ++rounds_committed;
        }
    };
    auto reader = [&] {
        while (!stop.load(std::memory_order_relaxed)) {
            std::shared_lock<std::shared_mutex> r(mu);
            uint64_t a = 0, b = 0;
            if (eng.kv_get(KEY_A, a) && eng.kv_get(KEY_B, b) && b != a + 1) ++torn_reads;
        }
    };

    std::vector<std::thread> writers, readers;
    for (int i = 0; i < kWriters; ++i) writers.emplace_back(writer);
    for (int i = 0; i < kReaders; ++i) readers.emplace_back(reader);
    for (auto& t : writers) t.join();
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : readers) t.join();

    assert(torn_reads.load() == 0 && "no reader ever observed a torn (partially-committed) multi-key transaction");
    assert(rounds_committed.load() == kWriters * kRoundsPerWriter && "every writer thread's every transaction committed");
    uint64_t final_a = 0, final_b = 0;
    assert(eng.kv_get(KEY_A, final_a) && eng.kv_get(KEY_B, final_b) && final_b == final_a + 1
           && "final state still satisfies the cross-key invariant after all concurrent activity");
    std::cout << "[concurrent multi-key transactions] ok (" << kWriters << " writers x " << kRoundsPerWriter
              << " txns + " << kReaders << " concurrent readers, 0 torn reads)\n";
}

int main() {
    test_wal_commit_atomicity();
    test_wal_autocommit_unchanged();
    test_engine_commit_durable();
    test_engine_rollback();
    test_concurrent_multi_key_transactions();
    std::cout << "ALL TRANSACTION TESTS PASSED\n";
    return 0;
}

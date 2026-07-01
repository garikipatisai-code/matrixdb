// CPU test for SPSCRingBuffer (ring_buffer.hpp) — the one hand-rolled lock-free concurrency primitive in
// the codebase. Before this file existed it had zero dedicated tests: main.cpp only drives it as a
// latency/throughput benchmark, with no assertion on its own contract (FIFO order, full/empty
// transitions, wraparound at the Capacity boundary, or cross-thread visibility of the cached-cursor
// memory_order_relaxed/acquire/release scheme). A bug here would surface downstream as a rare, hard-to-
// localize flake or silent data loss somewhere else in the pipeline, not a clean local test failure.
#include "ring_buffer.hpp"
#include "types.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <thread>
#include <vector>

// Single-threaded: push then pop a handful of items, confirm strict FIFO order and content integrity.
static void test_basic_fifo_order() {
    SPSCRingBuffer<DatabaseQuery, 8> rb;
    assert(rb.empty() && rb.size() == 0);
    for (uint64_t i = 0; i < 5; ++i) {
        DatabaseQuery q{}; q.query_id = i; q.opcode = OP_WRITE;
        assert(rb.try_emplace(q) && "push into a non-full buffer must succeed");
    }
    assert(!rb.empty() && rb.size() == 5);
    for (uint64_t i = 0; i < 5; ++i) {
        DatabaseQuery out{};
        assert(rb.try_pop(out) && "pop from a non-empty buffer must succeed");
        assert(out.query_id == i && "FIFO order: items come back in the exact order they were pushed");
    }
    assert(rb.empty() && rb.size() == 0);
    std::printf("[basic FIFO order] ok\n");
}

// A buffer at Capacity must reject the next push (genuinely full, not an off-by-one). try_pop on a truly
// empty buffer must fail and must NOT touch the output parameter (no partial/garbage write on a miss).
static void test_full_and_empty_detection() {
    SPSCRingBuffer<DatabaseQuery, 4> rb;
    for (uint64_t i = 0; i < 4; ++i) {
        DatabaseQuery q{}; q.query_id = i;
        assert(rb.try_emplace(q) && "fill to exactly Capacity");
    }
    assert(rb.size() == 4);
    DatabaseQuery overflow{}; overflow.query_id = 999;
    assert(!rb.try_emplace(overflow) && "push #5 into a Capacity=4 buffer must fail (genuinely full)");
    assert(rb.size() == 4 && "a failed push must not change size");

    // Drain it, then confirm try_pop on empty fails without mutating the output.
    for (uint64_t i = 0; i < 4; ++i) { DatabaseQuery out{}; assert(rb.try_pop(out)); assert(out.query_id == i); }
    assert(rb.empty());
    DatabaseQuery sentinel{}; sentinel.query_id = 0xDEADBEEF;
    assert(!rb.try_pop(sentinel) && "pop from an empty buffer must fail");
    assert(sentinel.query_id == 0xDEADBEEF && "a failed pop must not touch the output parameter");
    std::printf("[full/empty detection] ok\n");
}

// The core untested risk: does the `current_tail & MASK` / `current_head & MASK` indexing stay correct
// once the monotonic head_/tail_ counters have advanced well past Capacity (many wraps around the
// underlying array)? Interleave partial fills and partial drains, many more total pushes than Capacity,
// and verify every single value comes back in order — a striding/indexing bug would show up as either a
// dropped/duplicated query_id or an out-of-order one, not a crash, so this checks content, not just
// return codes.
static void test_wraparound() {
    SPSCRingBuffer<DatabaseQuery, 4> rb;   // small capacity: forces many wraps for a small total count
    uint64_t next_push = 0, next_expected_pop = 0;
    for (int round = 0; round < 1000; ++round) {
        // push 3 (leaves room for 4, since capacity is 4 and it may already hold up to 1 from a partial drain)
        for (int i = 0; i < 3; ++i) {
            DatabaseQuery q{}; q.query_id = next_push; q.timestamp_us = next_push * 2;
            if (rb.try_emplace(q)) ++next_push;   // may legitimately fail if already full; that's fine, try next round
        }
        // pop 2 (or whatever's available), checking strict order and payload integrity
        for (int i = 0; i < 2; ++i) {
            DatabaseQuery out{};
            if (!rb.try_pop(out)) continue;
            assert(out.query_id == next_expected_pop && "wraparound: FIFO order preserved across many wraps");
            assert(out.timestamp_us == next_expected_pop * 2 && "wraparound: payload content intact, not just the id");
            ++next_expected_pop;
        }
    }
    // Drain whatever's left the same way, then confirm total pushed == total popped (no drops, no dupes).
    DatabaseQuery out{};
    while (rb.try_pop(out)) { assert(out.query_id == next_expected_pop); ++next_expected_pop; }
    assert(next_expected_pop == next_push && "every pushed item was popped exactly once, in order");
    assert(next_push > 4 * 1000 * 0.5 && "sanity: this actually exercised many multiples of Capacity, not just one pass");
    std::printf("[wraparound: %llu items through a Capacity=4 buffer, order+content intact] ok\n",
                static_cast<unsigned long long>(next_push));
}

// size() must track the true occupancy through an interleaved sequence, not just at the empty/full ends.
static void test_size_accounting() {
    SPSCRingBuffer<DatabaseQuery, 16> rb;
    size_t expected = 0;
    auto push = [&](uint64_t id) { DatabaseQuery q{}; q.query_id = id;
                                    if (rb.try_emplace(q)) ++expected; assert(rb.size() == expected); };
    auto pop  = [&] { DatabaseQuery o{}; if (rb.try_pop(o)) --expected; assert(rb.size() == expected); };
    for (uint64_t i = 0; i < 10; ++i) push(i);
    for (int i = 0; i < 4; ++i) pop();
    for (uint64_t i = 100; i < 106; ++i) push(i);   // refill past the earlier drain, crossing Capacity once
    while (rb.size() > 0) pop();
    assert(expected == 0);
    std::printf("[size accounting] ok\n");
}

// The real cross-thread contract: one producer, one consumer, genuinely concurrent, high volume. Every
// item produced must be consumed exactly once, in order, with its payload intact — the only way to
// actually exercise whether the memory_order_relaxed (local cursor)/acquire/release (cross-thread
// handoff) scheme is correct, since a single-threaded test can't observe a missing memory barrier.
static void test_concurrent_spsc_stress() {
    SPSCRingBuffer<DatabaseQuery, 4096> rb;
    constexpr uint64_t kTotal = 2'000'000;
    std::atomic<bool> producer_done{false};
    std::atomic<uint64_t> consumed{0};
    std::atomic<int> bad{0};

    std::thread producer([&] {
        for (uint64_t i = 0; i < kTotal; ++i) {
            DatabaseQuery q{}; q.query_id = i; q.timestamp_us = i * 3 + 1; q.opcode = OP_WRITE;
            while (!rb.try_emplace(q)) { /* spin: buffer momentarily full, consumer will drain it */ }
        }
        producer_done.store(true, std::memory_order_release);
    });
    std::thread consumer([&] {
        uint64_t expected = 0;
        for (;;) {
            DatabaseQuery out{};
            if (rb.try_pop(out)) {
                if (out.query_id != expected || out.timestamp_us != expected * 3 + 1) ++bad;
                ++expected;
                consumed.store(expected, std::memory_order_relaxed);
                if (expected == kTotal) break;
            } else if (producer_done.load(std::memory_order_acquire) && rb.empty()) {
                break;   // producer is done and the buffer has nothing left — but we may not have hit kTotal
            }
        }
    });
    producer.join();
    consumer.join();
    assert(bad.load() == 0 && "no out-of-order/corrupted item ever observed under real concurrent load");
    assert(consumed.load() == kTotal && "every produced item was consumed exactly once (no drops)");
    std::printf("[concurrent SPSC stress] ok (%llu items, 1 producer + 1 consumer thread)\n",
                static_cast<unsigned long long>(kTotal));
}

int main() {
    test_basic_fifo_order();
    test_full_and_empty_detection();
    test_wraparound();
    test_size_accounting();
    test_concurrent_spsc_stress();
    std::printf("ALL RING BUFFER TESTS PASSED\n");
    return 0;
}

// CPU unit test for KVStore: the DM-1 fix. Proves distinct keys never overwrite each
// other (the silent-data-loss bug), get/put round-trips, and a full table is an explicit
// error, not corruption.
// Build: clang++ -std=c++20 -O2 test_kv_store.cpp -o /tmp/tkv && /tmp/tkv
#include "kv_store.hpp"
#include <cstdio>
#include <cassert>

int main() {
    KVStore kv(1024); // capacity 1024 slots

    // 1. put/get round-trip.
    kv.put(42, 100);
    uint64_t v = 0;
    assert(kv.get(42, v) && v == 100 && "get must return the put value");

    // 2. Missing key returns false.
    assert(!kv.get(999, v) && "absent key must report miss");

    // 3. Overwrite same key updates value (not a collision — same key).
    kv.put(42, 200);
    assert(kv.get(42, v) && v == 200 && "same-key put updates value");
    assert(kv.size() == 1 && "same-key put does not grow size");

    // 4. THE DM-1 BUG: two DISTINCT keys that collide under masking must BOTH survive.
    //    With a 1024 table, keys 7 and 7+1024 collide on the initial slot. Old code
    //    (key & MASK) silently overwrote; the hash table must probe and keep both.
    kv.put(7, 70);
    kv.put(7 + 1024, 71);
    uint64_t a = 0, b = 0;
    assert(kv.get(7, a) && a == 70 && "colliding key 7 must survive");
    assert(kv.get(7 + 1024, b) && b == 71 && "colliding key 7+1024 must survive");
    assert(kv.size() == 3 && "two distinct colliding keys are two entries");

    // 5. Full table is an explicit error (return false), never silent loss.
    KVStore small(2); // 2 slots
    assert(small.put(1, 1) && "first put fits");
    assert(small.put(2, 2) && "second put fits");
    assert(!small.put(3, 3) && "third put must FAIL (full), not overwrite");
    uint64_t x = 0;
    assert(small.get(1, x) && x == 1 && "existing entries intact after full");
    assert(small.get(2, x) && x == 2 && "existing entries intact after full");

    // 6. checksum is order-independent sum of stored values (used by the engine).
    KVStore c(16);
    c.put(1, 10); c.put(2, 20); c.put(3, 30);
    assert(c.checksum() == 60 && "checksum is sum of values");

    std::printf("PASS: KVStore correctness (no silent overwrite, full=error)\n");
    return 0;
}

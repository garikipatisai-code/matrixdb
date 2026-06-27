#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

// Open-addressing hash table (linear probing), key -> value, fixed capacity.
// This is the point-op store (gap DM-1). The prototype used store[key & MASK], which
// SILENTLY OVERWROTE colliding keys — a data-loss bug. Here, distinct keys probe to
// distinct slots and never overwrite each other; a full table is an explicit error
// (put returns false), never silent corruption.
//
// Single-owner: the engine's point-op path is single-threaded per the page-ownership
// model, so no internal locking. capacity is rounded up to a power of two so the slot
// index is a mask, not a modulo. SSD-spill (gap DM-9 / Inc 3) is a future seam: when
// full we return false today; later that path demotes cold entries to the cold tier.
class KVStore {
public:
    explicit KVStore(size_t capacity)
        : mask_(round_up_pow2(capacity) - 1),
          slots_(round_up_pow2(capacity)) {}

    // Insert or update. Returns false ONLY if the table is full and the key is new
    // (explicit failure, never overwrite of a different key).
    bool put(uint64_t key, uint64_t value) {
        size_t i = key & mask_;
        for (size_t probe = 0; probe <= mask_; ++probe) {
            Slot& s = slots_[i];
            if (!s.occupied) {            // empty slot: new insert
                s.key = key; s.value = value; s.occupied = true;
                ++size_;
                return true;
            }
            if (s.key == key) {           // same key: update in place
                s.value = value;
                return true;
            }
            i = (i + 1) & mask_;          // collision with a DIFFERENT key: probe on
        }
        return false;                     // table full, key not present: explicit error
    }

    // Look up. Returns true and sets out if present; false if absent.
    bool get(uint64_t key, uint64_t& out) const {
        size_t i = key & mask_;
        for (size_t probe = 0; probe <= mask_; ++probe) {
            const Slot& s = slots_[i];
            if (!s.occupied) return false;     // empty slot ends the probe chain: miss
            if (s.key == key) { out = s.value; return true; }
            i = (i + 1) & mask_;
        }
        return false;
    }

    size_t size() const { return size_; }
    size_t capacity() const { return mask_ + 1; }

    // Order-independent fingerprint: sum of stored values (matches the engine's old
    // store_checksum semantics so cross-checks stay meaningful).
    uint64_t checksum() const {
        uint64_t sum = 0;
        for (const Slot& s : slots_) if (s.occupied) sum += s.value;
        return sum;
    }

    // Visit every live entry (snapshot / iteration). Allocation-free; same walk as checksum().
    template <typename F>
    void for_each(F&& f) const { for (const Slot& s : slots_) if (s.occupied) f(s.key, s.value); }

private:
    struct Slot { uint64_t key = 0; uint64_t value = 0; bool occupied = false; };

    static size_t round_up_pow2(size_t n) {
        size_t p = 1;
        while (p < n) p <<= 1;
        return p < 2 ? 2 : p; // minimum 2 slots
    }

    size_t mask_;
    size_t size_ = 0;
    std::vector<Slot> slots_;
};

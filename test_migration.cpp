// CPU unit test for cross-tier migration (HOST<->COLD; DEVICE is Colab-verified).
// Build: clang++ -std=c++20 -O2 test_migration.cpp -o /tmp/tmig && /tmp/tmig
#include "tiered_column.hpp"
#include <cstdio>
#include <cassert>
#include <vector>
#include <numeric>

int main() {
    // --- Task 1: HOST<->COLD round-trip + integrity ---
    {
        std::vector<unsigned char> data(4096);
        for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<unsigned char>(i * 7 + 1);
        TieredColumn col(1, data.data(), data.size());
        const uint64_t want = col.checksum();
        assert(col.tier() == MemorySpace::HOST && "born in HOST");
        assert(col.size_bytes() == 4096);

        col.migrate_to(MemorySpace::COLD);
        assert(col.tier() == MemorySpace::COLD && "moved to COLD");
        assert(col.checksum() == want && "checksum invariant HOST->COLD");

        col.migrate_to(MemorySpace::HOST);
        assert(col.tier() == MemorySpace::HOST && "moved back to HOST");
        assert(col.checksum() == want && "checksum invariant COLD->HOST");

        // Integrity across a chain.
        col.migrate_to(MemorySpace::COLD);
        col.migrate_to(MemorySpace::HOST);
        col.migrate_to(MemorySpace::COLD);
        assert(col.checksum() == want && "checksum invariant across a HOST/COLD chain");
        col.migrate_to(MemorySpace::HOST); // leave on HOST so dtor frees the vector (no temp file left)
    }

    std::printf("PASS: migration correct\n");
    return 0;
}

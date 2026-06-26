#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// Binary column file format v0: [u32 magic][u64 count][count × u32 data].
// ponytail: host-endian, raw little-endian-native writes — a same-machine persistence/cache, NOT
// portable across architectures (a big-endian reader would see a magic mismatch and abort, which is
// the safe failure). A byte-swapped portable encoding is the v1 upgrade if cross-machine files matter.
inline constexpr uint32_t MATRIX_COLUMN_MAGIC = 0x4D43'4F4Cu; // 'MCOL' — MatrixDB column file v0

// Write a uint32 column to `path` as [magic][u64 count][count×u32]. Fail-loud (abort) on
// open/short-write — never leave a partially/wrongly written file mistaken for valid.
inline void matrix_write_column(const std::string& path, const uint32_t* data, size_t n) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "matrix_write_column: open failed %s\n", path.c_str()); std::abort(); }
    const uint32_t magic = MATRIX_COLUMN_MAGIC;
    const uint64_t count = n;
    const bool ok = std::fwrite(&magic, sizeof magic, 1, f) == 1
                 && std::fwrite(&count, sizeof count, 1, f) == 1
                 && (n == 0 || std::fwrite(data, sizeof(uint32_t), n, f) == n);
    std::fclose(f);
    if (!ok) { std::fprintf(stderr, "matrix_write_column: short write %s\n", path.c_str()); std::abort(); }
}

// Read a column written by matrix_write_column. Fail-loud on open / bad magic / short read.
inline void matrix_read_column(const std::string& path, std::vector<uint32_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "matrix_read_column: open failed %s\n", path.c_str()); std::abort(); }
    uint32_t magic = 0; uint64_t count = 0;
    bool ok = std::fread(&magic, sizeof magic, 1, f) == 1 && magic == MATRIX_COLUMN_MAGIC
           && std::fread(&count, sizeof count, 1, f) == 1;
    if (ok) { out.resize(static_cast<size_t>(count));
              ok = (count == 0 || std::fread(out.data(), sizeof(uint32_t), out.size(), f) == out.size()); }
    std::fclose(f);
    if (!ok) { std::fprintf(stderr, "matrix_read_column: bad/short file %s\n", path.c_str()); std::abort(); }
}

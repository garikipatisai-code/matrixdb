#pragma once
// BP-3 versioning: the build's version, so a running instance can report which build it is (ops correlate
// behavior ↔ build). Semantic versioning; bump on release and pair with a git tag (the release process).
// Pre-1.0 (0.x): the analytical CPU engine + tiering + durability + server protocol are feature-complete
// and verified locally; 1.0 is gated on the GPU backend (Colab) and a real network deployment.
#include <cstdint>

#define MATRIXDB_VERSION_MAJOR 0
#define MATRIXDB_VERSION_MINOR 1
#define MATRIXDB_VERSION_PATCH 0

inline constexpr const char* MATRIXDB_VERSION = "0.1.0";
inline const char* matrixdb_version() { return MATRIXDB_VERSION; }

// Packed numeric form for the wire / a version comparison: major<<32 | minor<<16 | patch. Lets a client
// read and compare the server's version without parsing a string (fits a uint64 result field).
inline constexpr uint64_t matrixdb_version_u64() {
    return (static_cast<uint64_t>(MATRIXDB_VERSION_MAJOR) << 32)
         | (static_cast<uint64_t>(MATRIXDB_VERSION_MINOR) << 16)
         |  static_cast<uint64_t>(MATRIXDB_VERSION_PATCH);
}

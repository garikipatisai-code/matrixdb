// CPU test for BP-3 versioning: the version constant, packed form, and the engine getter agree.
#include "compute_mock.cpp"   // pulls in version.hpp + the engine version() getter
#include <cassert>
#include <cstdint>
#include <string>
#include <iostream>

static void test_version() {
    // string + numeric constants are consistent
    assert(std::string(matrixdb_version()) == "0.1.0" && "version string");
    assert(MATRIXDB_VERSION_MAJOR == 0 && MATRIXDB_VERSION_MINOR == 1 && MATRIXDB_VERSION_PATCH == 0);
    // packed form: major<<32 | minor<<16 | patch
    const uint64_t expect = (uint64_t(0) << 32) | (uint64_t(1) << 16) | uint64_t(0);
    assert(matrixdb_version_u64() == expect && matrixdb_version_u64() != 0 && "packed version");
    // a higher version packs strictly greater (ordering holds — what a client comparison relies on)
    const uint64_t v0_1_0 = (uint64_t(0) << 32) | (uint64_t(1) << 16) | 0;
    const uint64_t v0_2_0 = (uint64_t(0) << 32) | (uint64_t(2) << 16) | 0;
    const uint64_t v1_0_0 = (uint64_t(1) << 32);
    assert(v0_1_0 < v0_2_0 && v0_2_0 < v1_0_0 && "packed versions order like semver");
    // the engine reports the same build version
    CPUMockEngine eng;
    assert(std::string(eng.version()) == matrixdb_version() && eng.version_u64() == matrixdb_version_u64()
           && "engine reports the build version");
    std::cout << "[version] ok (" << eng.version() << ")\n";
}

int main() { test_version(); std::cout << "ALL VERSION TESTS PASSED\n"; return 0; }

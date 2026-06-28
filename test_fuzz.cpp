// CPU fuzz harness (QA-4): hammer the UNTRUSTED-INPUT paths with seeded pseudo-random + mutated inputs
// and assert they never crash (graceful status/bool, no UB/OOB). Run it under ASan/UBSan (SAN=1
// ./run_tests.sh) to also catch memory bugs the fixed-input unit tests miss. Deterministic (seeded LCG,
// no wall-clock/RNG) so failures reproduce.
#include "server.hpp"        // CPUMockEngine + matrix_deserialize_request
#include "csv_ingest.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <iostream>

int main() {
    uint64_t st = 0x123456789abcdef0ull;
    auto rnd = [&] { st = st * 6364136223846793005ull + 1442695040888963407ull; return st >> 33; };

    CPUMockEngine eng;
    std::vector<uint32_t> v = {1, 2, 3};
    eng.load_scan_column(2, v.data(), v.size()); eng.name_column(2, "qty");

    const std::string valid = "SELECT SUM(qty) WHERE qty > 5";
    uint64_t parsed_ok = 0, deser_ok = 0;

    // 1) random strings -> parse_query  (must return a status, never crash)
    // 2) random bytes  -> matrix_deserialize_request  (must return bool, never crash)
    for (int i = 0; i < 40000; ++i) {
        std::string s;
        const size_t len = rnd() % 48;
        for (size_t j = 0; j < len; ++j) s.push_back(static_cast<char>(rnd() % 128));   // any byte incl. control/NUL
        MatrixQuery q;
        if (eng.parse_query(s, q) == MatrixQueryStatus::OK) ++parsed_ok;

        std::vector<uint8_t> b;
        const size_t blen = rnd() % 80;
        for (size_t j = 0; j < blen; ++j) b.push_back(static_cast<uint8_t>(rnd() & 0xff));
        MatrixRequest req;
        if (matrix_deserialize_request(b, req)) ++deser_ok;
    }

    // 3) mutate a VALID query (flip/insert/delete a byte) -> reaches deeper into the parser
    for (int i = 0; i < 20000; ++i) {
        std::string s = valid;
        const int muts = 1 + static_cast<int>(rnd() % 4);
        for (int m = 0; m < muts && !s.empty(); ++m) {
            const size_t pos = rnd() % s.size();
            switch (rnd() % 3) {
                case 0: s[pos] = static_cast<char>(rnd() % 128); break;          // flip
                case 1: s.insert(pos, 1, static_cast<char>(rnd() % 128)); break; // insert
                default: s.erase(pos, 1); break;                                 // delete
            }
        }
        MatrixQuery q;
        if (eng.parse_query(s, q) == MatrixQueryStatus::OK) ++parsed_ok;
    }

    // 4) random CSV files -> matrix_read_csv_column (must return bool, never crash)
    const std::string path = "/tmp/mdb_fuzz.csv";
    for (int i = 0; i < 4000; ++i) {
        std::string body;
        const size_t len = rnd() % 64;
        for (size_t j = 0; j < len; ++j) body.push_back(static_cast<char>(rnd() % 128));
        std::ofstream(path) << body;
        std::vector<uint32_t> out;
        matrix_read_csv_column(path, rnd() % 3, false, ',', out);
    }
    std::remove(path.c_str());

    // The whole point is reaching this line — no input crashed. The counts confirm the fuzzer exercised
    // the success paths too (not just instant rejections), so the deep code was actually hit.
    std::cout << "fuzz: " << parsed_ok << " queries parsed OK, " << deser_ok
              << " requests deserialized (over 64k inputs, no crash)\n";
    assert(parsed_ok > 0 && "mutation fuzzer hit some valid parses (exercised deep parser paths)");
    std::cout << "ALL FUZZ TESTS PASSED\n";
    return 0;
}

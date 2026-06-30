// CPU test for the CLI/REPL shell: drives matrix_repl over string streams and asserts on captured output.
#include "matrix_cli.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

static std::string write_csv() {
    const std::string p = "/tmp/matrixdb_cli_test.csv";
    std::ofstream(p) << "amount,region\n10,books\n900,games\n20,books\n950,music\n";
    return p;  // amount = col 0, region = col 1; 4 data rows
}

int main() {
    const std::string csv = write_csv();
    CPUMockEngine eng;

    // --- dot-commands ---
    {
        std::istringstream in(
            ".load " + csv + " amount u32 col0 header\n"
            ".load " + csv + " region str col1 header\n"
            ".tables\n.columns\n.stats\n.help\n"
            ".bogus\n"                       // unknown command -> Error, session continues
            ".quit\n");
        std::ostringstream out;
        const int rc = matrix_repl(in, out, eng);
        const std::string s = out.str();
        assert(rc == 0);
        assert(has(s, "loaded 4 rows into \"amount\""));
        assert(has(s, "loaded 4 rows into \"region\""));
        assert(has(s, "amount") && has(s, "u32"));     // .columns numeric type
        assert(has(s, "region") && has(s, "str"));     // .columns string-dict shown as "str", not "u32"
        assert(has(s, "queries="));                    // .stats
        assert(has(s, ".load") && has(s, ".quit"));    // .help
        assert(has(s, "Error:"));                      // .bogus
        assert(eng.column_id("amount") != 0 && eng.column_id("region") != 0);
        std::cout << "[cli dot-commands] ok\n";
    }

    // --- SQL router (same eng: amount + region loaded) ---
    {
        std::istringstream in(
            "SELECT SUM(amount)\n"                      // scalar -> 10+900+20+950 = 1880
            "SELECT SUM(amount) GROUP BY region\n"      // grouped over a string key -> decoded labels
            "SELECT COUNT(amount)\n"                    // count prints as integer (4)
            "SELECT AVG(amount)\n"                      // avg -> 470
            "SELECT region\n"                           // projection -> decoded strings
            "SELECT FROM nonsense\n"                    // bad -> Error, continues
            ".quit\n");
        std::ostringstream out;
        matrix_repl(in, out, eng);
        const std::string r = out.str();
        assert(has(r, "1880"));                         // scalar SUM
        assert(has(r, "books") && has(r, "games") && has(r, "music"));  // grouped labels + projection decoded
        assert(has(r, "\n4\n") || has(r, "4\n"));       // COUNT(amount) == 4
        assert(has(r, "470"));                          // AVG
        assert(has(r, "Error:"));                       // bad query reported, not crashed
        std::cout << "[cli queries] ok\n";
    }

    // --- persistence: .save then .open into a fresh engine round-trips; missing file errors (no abort) ---
    {
        std::istringstream in1(".load " + csv + " amount u32 col0 header\n.save /tmp/mdb_cli_v2.db\n.quit\n");
        std::ostringstream o1; CPUMockEngine e1; matrix_repl(in1, o1, e1);
        assert(has(o1.str(), "saved catalog"));
        std::istringstream in2(".open /tmp/mdb_cli_v2.db\n.columns\nSELECT SUM(amount)\n.open /tmp/nope_x.db\n.quit\n");
        std::ostringstream o2; CPUMockEngine e2; const int rc = matrix_repl(in2, o2, e2);
        const std::string s = o2.str();
        assert(rc == 0 && has(s, "amount") && has(s, "1880") && has(s, "Error:"));
        std::remove("/tmp/mdb_cli_v2.db");
        std::cout << "[cli persist] ok\n";
    }

    std::remove(csv.c_str());
    std::cout << "ALL CLI TESTS PASSED\n";
    return 0;
}

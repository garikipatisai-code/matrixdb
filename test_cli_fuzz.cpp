// CPU fuzz test for the CLI: drive matrix_repl with tens of thousands of random/malformed lines and assert
// it never aborts, never trips ASan/UBSan, and always returns cleanly (0). Targets the hand-rolled query
// router in matrix_cli_run_sql (the riskiest parsing). The engine is loaded once and stays READ-ONLY during
// fuzzing — the vocabulary excludes .load/.save/.open (file/state side effects), so this is purely a
// parser/robustness sweep. Deterministic (fixed PRNG seed) so failures reproduce.
#include "matrix_cli.hpp"
#include <cassert>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

int main() {
    CPUMockEngine eng;
    std::vector<uint32_t> amount{10, 20, 30, 40, 50, 60}, region{0, 1, 0, 2, 1, 2};
    eng.load_scan_column(1, amount.data(), amount.size()); eng.name_column(1, "amount");
    eng.load_scan_column(2, region.data(), region.size()); eng.name_column(2, "region");
    eng.load_string_column_dict(3, {"north", "south", "north", "east", "south", "east"}); eng.name_column(3, "name");

    // Tokens spanning the whole grammar + malformed bits. No .load/.save/.open / file paths (read-only sweep).
    const std::vector<std::string> vocab = {
        "SELECT", "COUNT", "SUM", "MIN", "MAX", "AVG", "DISTINCT", "(", ")", "*", ",",
        "amount", "region", "name", "nonexistent", "",
        "WHERE", ">", "<", "=", ">=", "<=", "!=", "BETWEEN", "AND",
        "GROUP", "BY", "HAVING", "ORDER", "DESC", "ASC", "LIMIT", "JOIN",
        "0", "2", "5", "100", "-5", "3.5", "999999999999", "abc",
        ".columns", ".stats", ".help", ".tables", ".timing", ".mode", ".bogus",
        "csv", "list", "table", "on", "off", "#comment",
    };
    std::mt19937 rng(0xC0FFEE);
    auto pick = [&]() -> const std::string& { return vocab[rng() % vocab.size()]; };

    // 1. Structured fuzz: random token sequences (often resembling queries).
    for (int i = 0; i < 40000; ++i) {
        const int n = 1 + static_cast<int>(rng() % 9);
        std::string line;
        for (int j = 0; j < n; ++j) { if (j) line += ' '; line += pick(); }
        std::istringstream in(line + "\n");
        std::ostringstream out;
        assert(matrix_repl(in, out, eng) == 0 && "matrix_repl must return cleanly on any line");
    }

    // 2. Raw-byte fuzz: arbitrary printable junk (no token structure at all).
    std::uniform_int_distribution<int> ch(32, 126);
    for (int i = 0; i < 10000; ++i) {
        const int len = static_cast<int>(rng() % 48);
        std::string line;
        for (int j = 0; j < len; ++j) line += static_cast<char>(ch(rng));
        std::istringstream in(line + "\n");
        std::ostringstream out;
        assert(matrix_repl(in, out, eng) == 0);
    }

    // 3. A few hand-picked nasty edge cases that stress the token-index parsing.
    for (const char* bad : {
        "SELECT", "SELECT (", "SELECT )", "SELECT ,", "SELECT COUNT(", "SELECT a, ",
        "SELECT amount JOIN", "SELECT amount JOIN amount", "SELECT amount JOIN amount =",
        "SELECT , JOIN amount = region", "SELECT amount, region JOIN amount = region GROUP",
        "SELECT SUM(amount) JOIN amount = region GROUP BY name HAVING",
        "SELECT SUM(amount) JOIN amount = region GROUP BY name ORDER BY",
        "SELECT SUM(amount) GROUP BY region HAVING SUM", "GROUP BY", "JOIN", "HAVING x y z",
        "SELECT amount WHERE", "SELECT amount WHERE region", "SELECT amount LIMIT",
        ".mode", ".timing xyz", ".load", ".save", ".open", "....", "()))((",
        // .load parsing with a nonexistent path (fails gracefully, no file side effects) stresses the
        // column-index parse (must not throw on a bad/overflowing colN):
        ".load /tmp/mdb_nope.csv c u32 col", ".load /tmp/mdb_nope.csv c u32 col99999999999999999999",
        ".load /tmp/mdb_nope.csv c badtype", ".load /tmp/mdb_nope.csv c u32 colX header", }) {
        std::istringstream in(std::string(bad) + "\n");
        std::ostringstream out;
        assert(matrix_repl(in, out, eng) == 0);
    }

    std::cout << "[cli fuzz] 50000+ lines, no crash / no abort / no UB\n";
    std::cout << "ALL CLI-FUZZ TESTS PASSED\n";
    return 0;
}

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

    // --- persistence: .save then .open into a FRESH engine round-trips (incl. the string dictionary);
    //     .open into a non-empty session errors instead of aborting ---
    {
        std::istringstream in1(".load " + csv + " amount u32 col0 header\n.load " + csv + " region str col1 header\n.save /tmp/mdb_cli_v2.db\n.quit\n");
        std::ostringstream o1; CPUMockEngine e1; matrix_repl(in1, o1, e1);
        assert(has(o1.str(), "saved catalog"));
        std::istringstream in2(".open /tmp/mdb_cli_v2.db\n.columns\nSELECT SUM(amount)\nSELECT region\n.open /tmp/again.db\n.quit\n");
        std::ostringstream o2; CPUMockEngine e2; const int rc = matrix_repl(in2, o2, e2);
        const std::string s = o2.str();
        assert(rc == 0 && has(s, "amount") && has(s, "region") && has(s, "str"));   // .columns: str column restored
        assert(has(s, "1880") && has(s, "books"));                                  // SUM + decoded dictionary projection
        assert(has(s, "Error:"));                                                   // 2nd .open (non-empty) refused, not aborted
        std::remove("/tmp/mdb_cli_v2.db");
        std::cout << "[cli persist] ok\n";
    }

    // --- COUNT(DISTINCT), HAVING, top-N (each in a clean session for precise asserts; shared eng) ---
    {
        std::ostringstream o; std::istringstream i("SELECT COUNT(DISTINCT region)\n.quit\n");
        matrix_repl(i, o, eng); assert(has(o.str(), "3"));                       // 3 distinct regions

        std::ostringstream o2; std::istringstream i2("SELECT SUM(amount) GROUP BY region HAVING SUM > 100\n.quit\n");
        matrix_repl(i2, o2, eng); const std::string h = o2.str();
        assert(has(h, "games") && has(h, "music") && !has(h, "books"));         // books(30) fails HAVING

        std::ostringstream o3; std::istringstream i3("SELECT SUM(amount) GROUP BY region ORDER BY SUM DESC LIMIT 2\n.quit\n");
        matrix_repl(i3, o3, eng); const std::string t = o3.str();
        assert(has(t, "music") && has(t, "games") && !has(t, "books"));         // top 2 by value desc
        assert(t.substr(0, t.find('\n')).find("music") != std::string::npos);   // largest first (music 950)
        std::cout << "[cli distinct/having/topN] ok\n";
    }

    // --- multi-aggregate SELECT (scalar one-row + grouped table; shared eng) ---
    {
        std::ostringstream o; std::istringstream i("SELECT COUNT(amount), SUM(amount)\n.quit\n");
        matrix_repl(i, o, eng); const std::string s = o.str();
        assert(has(s, "4") && has(s, "1880"));                                  // count 4, sum 1880 on a labeled row

        std::ostringstream o2; std::istringstream i2("SELECT COUNT(amount), SUM(amount) GROUP BY region\n.quit\n");
        matrix_repl(i2, o2, eng); const std::string g = o2.str();
        assert(has(g, "books") && has(g, "games") && has(g, "music"));
        assert(has(g, "books │ 2 │ 30"));                                       // books: count 2, sum 30
        std::cout << "[cli multi-agg] ok\n";
    }

    // --- SQL join: orders (region-idx + amount) join regions (key + name) ---
    {
        CPUMockEngine je;
        std::vector<uint32_t> oreg{0,1,0,2}, oamt{10,900,20,950}, rkey{0,1,2};
        je.load_scan_column(1, oreg.data(), oreg.size()); je.name_column(1, "ord_region");
        je.load_scan_column(2, oamt.data(), oamt.size()); je.name_column(2, "ord_amt");
        je.load_scan_column(3, rkey.data(), rkey.size()); je.name_column(3, "reg_key");
        je.load_string_column_dict(4, {"north","south","east"}); je.name_column(4, "reg_name");
        std::ostringstream o; std::istringstream i(
            "SELECT ord_amt, reg_name JOIN ord_region = reg_key\n"     // 4 joined rows, region name decoded
            "SELECT COUNT(*) JOIN ord_region = reg_key\n"              // cardinality 4
            "SELECT ord_amt, reg_name JOIN ord_region = reg_name\n"    // dict-string key -> Error
            "SELECT ord_amt JOIN ord_region = reg_key\n"               // one projected column -> Error
            ".quit\n");
        matrix_repl(i, o, je); const std::string s = o.str();
        assert(has(s, "10 │ north") && has(s, "900 │ south") && has(s, "20 │ north") && has(s, "950 │ east"));
        assert(has(s, "4\n"));                                          // COUNT(*) == 4
        assert(has(s, "Error:"));                                       // bad lines errored, session continued
        std::cout << "[cli join] ok\n";

        // aggregates over the join: scalar + grouped by the right dimension (decoded labels)
        std::ostringstream oa; std::istringstream ia(
            "SELECT SUM(ord_amt) JOIN ord_region = reg_key\n"                       // 10+900+20+950 = 1880
            "SELECT SUM(ord_amt) JOIN ord_region = reg_key GROUP BY reg_name\n"     // north 30, south 900, east 950
            "SELECT COUNT(ord_amt) JOIN ord_region = reg_key GROUP BY reg_name\n"   // north 2, south 1, east 1
            "SELECT MAX(ord_amt) JOIN ord_region = reg_key\n"                       // 950
            "SELECT AVG(ord_amt) JOIN ord_region = reg_key\n"                       // AVG over a join -> Error
            ".quit\n");
        matrix_repl(ia, oa, je); const std::string a = oa.str();
        assert(has(a, "1880") && has(a, "950"));
        assert(has(a, "north │ 30") && has(a, "south │ 900") && has(a, "east │ 950"));   // grouped SUM, decoded labels
        assert(has(a, "north │ 2"));                                                      // grouped COUNT
        assert(has(a, "Error:"));                                                         // AVG unsupported over a join
        std::cout << "[cli join-agg] ok\n";

        // HAVING + top-N on the join-aggregate (post-process the per-dimension results)
        std::ostringstream oh; std::istringstream ih(
            "SELECT SUM(ord_amt) JOIN ord_region = reg_key GROUP BY reg_name HAVING SUM > 100\n"          // south 900, east 950
            "SELECT SUM(ord_amt) JOIN ord_region = reg_key GROUP BY reg_name ORDER BY SUM DESC LIMIT 2\n" // east(950), south(900)
            "SELECT SUM(ord_amt) JOIN ord_region = reg_key GROUP BY reg_name ORDER BY SUM DESC LIMIT 1\n" // east only
            "SELECT SUM(ord_amt) JOIN ord_region = reg_key GROUP BY reg_name HAVING\n"                    // malformed -> Error
            ".quit\n");
        matrix_repl(ih, oh, je); const std::string hh = oh.str();
        assert(has(hh, "south │ 900") && has(hh, "east │ 950") && !has(hh, "north │ 30"));   // HAVING filtered north(30)
        assert(hh.substr(0, hh.find('\n')).find("east") != std::string::npos);               // top-N: largest (east 950) first
        assert(has(hh, "Error:"));                                                           // malformed tail, session continued
        std::cout << "[cli join-agg having/topN] ok\n";
    }

    // --- .timing prints per-query elapsed; # lines are comments (skipped, no error) ---
    {
        std::ostringstream o; std::istringstream i("# a comment\n.timing on\nSELECT SUM(amount)\n.quit\n");
        matrix_repl(i, o, eng); const std::string s = o.str();
        assert(has(s, "1880") && has(s, "µs)") && !has(s, "Error:"));
        std::cout << "[cli timing/comments] ok\n";
    }

    // --- .mode csv emits a machine-readable separator; .mode list restores the human format ---
    {
        std::ostringstream o; std::istringstream i(
            ".mode csv\nSELECT SUM(amount) GROUP BY region\n"
            ".mode list\nSELECT SUM(amount) GROUP BY region\n.quit\n");
        matrix_repl(i, o, eng); const std::string s = o.str();
        assert(has(s, "books,30") && has(s, "music,950"));     // csv rows (region SUM: books 30, music 950)
        assert(has(s, "books │ 30"));                          // list rows restored
        std::cout << "[cli mode csv] ok\n";
    }

    // --- .mode table: aligned padded columns (post-processed LIST output) ---
    {
        std::ostringstream ol; std::istringstream il("SELECT COUNT(amount), SUM(amount) GROUP BY region\n.quit\n");
        matrix_repl(il, ol, eng);                                    // list (default)
        std::ostringstream ot; std::istringstream it(".mode table\nSELECT COUNT(amount), SUM(amount) GROUP BY region\n.quit\n");
        matrix_repl(it, ot, eng);                                    // table
        const std::string L = ol.str(), T = ot.str();
        assert(L != T);                                              // alignment changed the layout
        assert(has(T, "books") && has(T, "30"));                     // data intact
        assert(T.find("  ") != std::string::npos && L.find("  ") == std::string::npos);   // table pads (2+ spaces); list doesn't
        std::cout << "[cli mode table] ok\n";
    }

    std::remove(csv.c_str());
    std::cout << "ALL CLI TESTS PASSED\n";
    return 0;
}

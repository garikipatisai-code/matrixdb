#pragma once
// matrixdb CLI/REPL shell — a testable line loop over streams. All logic lives here (pure over the two
// streams), so test_cli.cpp drives it with std::istringstream / std::ostringstream and matrixdb_cli.cpp is a
// thin main. Dot-commands (.load/.tables/.columns/.stats/.help/.quit) + a SQL router over the existing
// engine (avg_query / parse_query+execute_query / project_query) with typed + string-decoded output.
// No input ever throws: every engine status is checked and failures print a single "Error: ..." line.
#include "compute_mock.cpp"
#include <fstream>
#include <istream>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>
#include <cctype>
#include <cstdint>

namespace matrixcli_detail {

inline std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
inline std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> t; std::istringstream is(s); std::string w;
    while (is >> w) t.push_back(w);
    return t;
}
inline std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
inline const char* type_name(MatrixType t) {
    return t == MatrixType::I64 ? "i64" : t == MatrixType::F64 ? "f64" : "u32";
}
inline const char* tier_name(MemorySpace m) {
    return m == MemorySpace::DEVICE ? "DEVICE" : m == MemorySpace::COLD ? "COLD"
         : m == MemorySpace::UNIFIED ? "UNIFIED" : "HOST";
}
// Decode a raw column value by its numeric storage type (u32 unsigned / i64 signed / f64). NOT string-aware:
// dictionary codes (column_type U32) print as integers here; string decoding happens at the call sites that
// know the value is a code (projection, grouped key).
inline std::string decode_num(CPUMockEngine& eng, uint64_t col, uint64_t v) {
    switch (eng.column_type(col)) {
        case MatrixType::I64: return std::to_string(static_cast<int64_t>(v));
        case MatrixType::F64: { std::ostringstream o; o << matrix_bit_cast<double>(v); return o.str(); }
        default:              return std::to_string(v);
    }
}
// Decode an aggregate result: COUNT is always an integer count (never a column value), so it must not be
// reinterpreted by column type; SUM/MIN/MAX are values, decoded by the value column's storage type.
inline std::string decode_agg(CPUMockEngine& eng, const MatrixQuery& q, uint64_t v) {
    return q.agg == AGG_COUNT ? std::to_string(v) : decode_num(eng, q.value_col, v);
}
// Decode a projected value: dictionary-encoded columns -> the string; otherwise numeric.
inline std::string decode_proj(CPUMockEngine& eng, uint64_t col, uint64_t v) {
    if (eng.string_dict_size(col) > 0) return eng.string_decode(col, static_cast<uint32_t>(v));
    return decode_num(eng, col, v);
}

}  // namespace matrixcli_detail

// Route a non-dot line to the right executor and format the result to `out`. Never throws; a query error
// prints "Error: ..." and returns (the REPL continues).
inline void matrix_cli_run_sql(const std::string& line, std::ostream& out, CPUMockEngine& eng) {
    using namespace matrixcli_detail;
    const std::string U = upper(line);
    // Routing, first match wins: COUNT(DISTINCT) -> HAVING -> AVG -> multi-aggregate -> single aggregate
    // (incl. top-N) -> projection. Every branch decodes results by type; a raw code is never printed.
    if (U.find("DISTINCT") != std::string::npos) {
        uint64_t n = 0;
        if (eng.distinct_query(line, n)) out << n << "\n";
        else out << "Error: could not run COUNT(DISTINCT) query\n";
        return;
    }
    if (U.find("HAVING") != std::string::npos) {            // SELECT agg(x) GROUP BY k HAVING agg <cmp> v
        MatrixQuery q{};
        if (eng.parse_query(line.substr(0, U.find("HAVING")), q) != MatrixQueryStatus::OK) {
            out << "Error: could not parse HAVING query\n"; return;
        }
        for (const auto& gv : eng.having_query(line)) {     // (group, value) pairs that pass the HAVING test
            const std::string label = eng.string_dict_size(q.key_col) > 0
                ? eng.string_decode(q.key_col, static_cast<uint32_t>(gv.first)) : std::to_string(gv.first);
            out << label << " │ " << decode_agg(eng, q, gv.second) << "\n";
        }
        return;
    }
    if (U.find("AVG(") != std::string::npos) {
        const std::vector<double> a = eng.avg_query(line);
        if (a.empty()) { out << "Error: could not run AVG query\n"; return; }
        if (a.size() == 1) { out << a[0] << "\n"; return; }
        for (size_t g = 0; g < a.size(); ++g) out << g << " │ " << a[g] << "\n";   // grouped: numeric group id (string-key decode deferred)
        return;
    }
    if (line.find('(') != std::string::npos) {
        MatrixQuery q{};
        if (eng.parse_query(line, q) != MatrixQueryStatus::OK) { out << "Error: could not parse query\n"; return; }
        std::vector<uint64_t> o;
        if (eng.execute_query(q, o) != MatrixQueryStatus::OK) { out << "Error: query rejected\n"; return; }
        if (q.grouped && q.limit > 0) {                     // top-N: ORDER BY agg DESC LIMIT n -> sorted (group,value)
            for (const auto& gv : eng.top_query(line)) {
                const std::string label = eng.string_dict_size(q.key_col) > 0
                    ? eng.string_decode(q.key_col, static_cast<uint32_t>(gv.first)) : std::to_string(gv.first);
                out << label << " │ " << decode_agg(eng, q, gv.second) << "\n";
            }
            return;
        }
        if (!q.grouped) { out << (o.empty() ? "" : decode_agg(eng, q, o[0])) << "\n"; return; }
        for (uint32_t g = 0; g < o.size(); ++g) {
            const std::string label = eng.string_dict_size(q.key_col) > 0 ? eng.string_decode(q.key_col, g) : std::to_string(g);
            out << label << " │ " << decode_agg(eng, q, o[g]) << "\n";
        }
        return;
    }
    // projection: SELECT <col> [WHERE ...] [LIMIT n]
    const std::vector<std::string> tk = split_ws(line);
    const uint64_t pcol = tk.size() >= 2 ? eng.column_id(tk[1]) : 0;
    if (pcol == 0) { out << "Error: could not run query (unknown column?)\n"; return; }
    const std::vector<uint64_t> o = eng.project_query(line);
    const size_t cap = 100;
    for (size_t i = 0; i < o.size() && i < cap; ++i) out << decode_proj(eng, pcol, o[i]) << "\n";
    if (o.size() > cap) out << "… (" << o.size() << " rows, showing " << cap << ")\n";
}

// The REPL: read lines from `in`, write results to `out`. CLI-local column-id counter starts at 1 (id 0 is
// reserved). Returns 0 on clean exit (EOF or .quit).
inline int matrix_repl(std::istream& in, std::ostream& out, CPUMockEngine& eng) {
    using namespace matrixcli_detail;
    uint64_t next_id = 1;
    std::string raw;
    while (std::getline(in, raw)) {
        const std::string line = trim(raw);
        if (line.empty()) continue;
        if (line[0] != '.') { matrix_cli_run_sql(line, out, eng); continue; }

        const std::vector<std::string> tk = split_ws(line);
        const std::string& cmd = tk[0];
        if (cmd == ".quit" || cmd == ".exit") break;
        else if (cmd == ".help") {
            out << "commands: .load <csv> <name> <u32|i64|f64|str> [colN] [header|noheader] | "
                   ".tables | .columns | .stats | .help | .quit\n"
                   "queries:  SELECT COUNT|SUM|MIN|MAX|AVG(col) [WHERE col <op> v] [GROUP BY key]  |  "
                   "SELECT col [WHERE col <op> v] [LIMIT n]\n";
        }
        else if (cmd == ".tables") {
            for (const std::string& t : eng.tables()) out << t << "\n";
        }
        else if (cmd == ".columns") {
            out << "id\tname\ttype\trows\ttier\n";
            for (const ColumnInfo& c : eng.catalog_columns()) {
                const char* ty = eng.string_dict_size(c.id) > 0 ? "str" : type_name(c.type);   // dict columns store u32 codes but are strings to the user
                out << c.id << "\t" << c.name << "\t" << ty << "\t" << c.rows << "\t" << tier_name(c.tier) << "\n";
            }
        }
        else if (cmd == ".stats") {
            const EngineStats st = eng.stats();
            out << "columns=" << st.catalog_columns
                << " host_mb=" << st.host_resident_bytes / (1024 * 1024)
                << " cold_mb=" << st.cold_resident_bytes / (1024 * 1024)
                << " cold_borrows=" << st.cold_borrows << " rebalances=" << st.rebalances
                << " migrations=" << st.migrations << " queries=" << st.query_count
                << " p50_ns=" << eng.query_latency_percentile_ns(0.50)
                << " p99_ns=" << eng.query_latency_percentile_ns(0.99) << "\n";
        }
        else if (cmd == ".load") {
            // .load <path> <name> <u32|i64|f64|str> [colN] [header|noheader]   (defaults: col0, header)
            if (tk.size() < 4) { out << "Error: usage: .load <csv> <name> <u32|i64|f64|str> [colN] [header|noheader]\n"; continue; }
            const std::string& path = tk[1]; const std::string& name = tk[2]; const std::string& type = tk[3];
            size_t col = 0; bool header = true;
            for (size_t i = 4; i < tk.size(); ++i) {
                if (tk[i] == "noheader") header = false;
                else if (tk[i] == "header") header = true;
                else if (tk[i].rfind("col", 0) == 0) col = static_cast<size_t>(std::stoul(tk[i].substr(3)));
            }
            const uint64_t id = next_id;
            bool ok = false;
            if      (type == "u32") ok = eng.load_column_from_csv(id, path, col, header);
            else if (type == "i64") ok = eng.load_column_from_csv_i64(id, path, col, header);
            else if (type == "f64") ok = eng.load_column_from_csv_f64(id, path, col, header);
            else if (type == "str") ok = eng.load_string_column_from_csv(id, path, col, header);
            else { out << "Error: unknown type '" << type << "' (use u32|i64|f64|str)\n"; continue; }
            if (!ok) { out << "Error: could not load " << path << " (col " << col << ")\n"; continue; }
            eng.name_column(id, name);
            ++next_id;
            out << "loaded " << eng.column_rows(id) << " rows into \"" << name << "\" (" << type << ", col " << col << ")\n";
        }
        else if (cmd == ".save") {
            // catalog snapshot (columns + names + string dictionaries). save_catalog abort()s on fopen
            // failure, so pre-check writability — a REPL must never abort on user input.
            if (tk.size() < 2) { out << "Error: usage: .save <file>\n"; continue; }
            if (!std::ofstream(tk[1]).good()) { out << "Error: cannot write " << tk[1] << "\n"; continue; }
            eng.save_catalog(tk[1]);
            out << "saved catalog to " << tk[1] << "\n";
        }
        else if (cmd == ".open") {
            if (tk.size() < 2) { out << "Error: usage: .open <file>\n"; continue; }
            if (!std::ifstream(tk[1]).good()) { out << "Error: cannot open " << tk[1] << "\n"; continue; }
            eng.load_catalog(tk[1]);   // ponytail: a *corrupt* snapshot still abort()s inside (CRC/short read); rare, pre-existing
            for (const ColumnInfo& c : eng.catalog_columns()) if (c.id >= next_id) next_id = c.id + 1;  // don't collide with restored ids
            out << "opened " << tk[1] << " (" << eng.catalog_columns().size() << " columns)\n";
        }
        else out << "Error: unknown command '" << cmd << "' (try .help)\n";
    }
    return 0;
}

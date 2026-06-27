#pragma once
#include <cstdint>
#include <charconv>      // std::from_chars — locale-free, no-throw integer parse
#include <fstream>
#include <string>
#include <vector>

// Parse one uint32 column (0-based col_index) out of a simple CSV file. has_header skips line 1.
// Returns true + fills `out` on success; false + empty `out` on any error (open fail / short row /
// non-integer / overflow). CSV is untrusted input, so malformed data is a graceful false, NOT abort
// (cf. column_io.hpp, which aborts on corruption of our OWN binary format). See VAL-1.
// ponytail: no quoted-field / escape handling — simple unquoted integer CSV only; quote-aware split is
// the upgrade path if real files need it.
inline bool matrix_read_csv_column(const std::string& path, size_t col_index, bool has_header,
                                   char delim, std::vector<uint32_t>& out) {
    out.clear();
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();   // tolerate CRLF
        if (has_header && first) { first = false; continue; }
        first = false;
        // Walk to the col_index-th field without allocating a vector of all fields.
        size_t start = 0, field = 0;
        while (field < col_index) {
            size_t comma = line.find(delim, start);
            if (comma == std::string::npos) { out.clear(); return false; }   // short row
            start = comma + 1;
            ++field;
        }
        size_t end = line.find(delim, start);
        if (end == std::string::npos) end = line.size();
        const char* b = line.data() + start;
        const char* e = line.data() + end;
        uint32_t value = 0;
        auto [ptr, ec] = std::from_chars(b, e, value);
        if (ec != std::errc{} || ptr != e) { out.clear(); return false; }    // non-integer, overflow, or trailing junk
        out.push_back(value);
    }
    return true;
}

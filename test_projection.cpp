// Projection: project_query returns the matching rows' values (not an aggregate) — SELECT col
// [WHERE fcol op val [AND val]] [LIMIT n]. Composes a filter (matching_rows) with gather, and reuses
// parse_query for the WHERE predicate (so numeric + string-dict / ordered filters work). Verifies a
// filtered+limited projection, LIMIT alone, full projection, an ordered string filter, and that the
// aggregate form is correctly rejected.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

int main() {
    CPUMockEngine eng;
    const size_t N = 300;
    const char* names[] = {"music", "books", "games"};   // distinct sorted: books<games<music
    std::vector<uint32_t> id(N); std::vector<std::string> cats(N);
    for (size_t i = 0; i < N; ++i) { id[i] = static_cast<uint32_t>(i); cats[i] = names[i % 3]; }
    eng.load_scan_column(1, id.data(), N);   eng.name_column(1, "id");
    eng.load_string_column_dict(2, cats);    eng.name_column(2, "cat");   // id[i]==i, so projected value == row index

    // SELECT id WHERE cat = 'books' LIMIT 5  -> first 5 ids where cat == 'books'
    {
        auto r = eng.project_query("SELECT id WHERE cat = 'books' LIMIT 5");
        std::vector<uint64_t> oracle;
        for (size_t i = 0; i < N && oracle.size() < 5; ++i) if (cats[i] == "books") oracle.push_back(i);
        assert(r == oracle);
        std::printf("[project where+limit] ok (%zu rows)\n", r.size());
    }
    // SELECT id LIMIT 10 -> ids 0..9
    {
        auto r = eng.project_query("SELECT id LIMIT 10");
        assert(r.size() == 10); for (uint64_t i = 0; i < 10; ++i) assert(r[i] == i);
        std::printf("[project limit] ok\n");
    }
    // SELECT id (no filter, no limit) -> every row in order
    {
        auto r = eng.project_query("SELECT id");
        assert(r.size() == N); for (size_t i = 0; i < N; ++i) assert(r[i] == i);
        std::printf("[project all] ok (%zu)\n", r.size());
    }
    // SELECT id WHERE cat > 'books' -> ids where cat in {games, music} (ordered string filter)
    {
        auto r = eng.project_query("SELECT id WHERE cat > 'books'");
        std::vector<uint64_t> oracle;
        for (size_t i = 0; i < N; ++i) if (cats[i] > "books") oracle.push_back(i);
        assert(r == oracle);
        std::printf("[project ordered string] ok (%zu rows)\n", r.size());
    }
    // an aggregate query is not a projection -> empty (so callers can distinguish the two forms)
    {
        assert(eng.project_query("SELECT COUNT(id)").empty());
        std::printf("[project rejects aggregate] ok\n");
    }

    std::printf("ALL PROJECTION TESTS PASSED\n");
    return 0;
}

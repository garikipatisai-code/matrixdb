// Dictionary-encoded string columns: strings as a first-class queryable type. load_string_column_dict
// encodes strings -> u32 codes and registers the codes as an ordinary u32 catalog column, so a string
// column inherits the whole analytical engine. This verifies: dict build + encode/decode round-trip,
// GROUP BY a string dimension (SUM a numeric value per category) vs a brute-force oracle, a WHERE s=='x'
// filter (incl. an absent literal matching nothing), and COUNT(DISTINCT) == the dictionary size.
#include "compute_mock.cpp"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>

int main() {
    CPUMockEngine eng;
    const char* names[] = {"books", "games", "music"};   // 3 distinct categories
    const size_t N = 30000;
    std::vector<std::string> cats; std::vector<uint32_t> rev;
    cats.reserve(N); rev.reserve(N);
    for (size_t i = 0; i < N; ++i) { cats.push_back(names[i % 3]); rev.push_back(static_cast<uint32_t>(i % 100)); }

    eng.load_string_column_dict(1, cats);          // col 1 = category (string -> u32 codes, first-class)
    eng.load_scan_column(2, rev.data(), rev.size()); // col 2 = revenue (aligned numeric value)

    // dict size + encode/decode round-trip + absent literal
    assert(eng.string_dict_size(1) == 3 && "three distinct categories");
    for (uint32_t c = 0; c < 3; ++c) assert(eng.string_encode(1, eng.string_decode(1, c)) == c && "decode∘encode == id");
    assert(eng.string_encode(1, "absent") == 3 && "absent literal -> a code no row holds");
    std::printf("[string dict build] ok (size=%u)\n", eng.string_dict_size(1));

    // GROUP BY category, SUM(revenue) vs brute-force oracle (decode each group code back to its string)
    std::unordered_map<std::string, uint64_t> oracle;
    for (size_t i = 0; i < N; ++i) oracle[cats[i]] += rev[i];
    MatrixQuery q; q.value_col = 2; q.agg = AGG_SUM; q.grouped = true; q.key_col = 1; q.num_groups = eng.string_dict_size(1);
    std::vector<uint64_t> g;
    assert(eng.execute_query(q, g) == MatrixQueryStatus::OK && g.size() == 3);
    for (uint32_t c = 0; c < 3; ++c) assert(g[c] == oracle[eng.string_decode(1, c)] && "per-category SUM == oracle");
    std::printf("[GROUP BY string] ok (books=%llu games=%llu music=%llu)\n",
                (unsigned long long)g[eng.string_encode(1, "books")],
                (unsigned long long)g[eng.string_encode(1, "games")],
                (unsigned long long)g[eng.string_encode(1, "music")]);

    // WHERE category == 'books' : COUNT on the code column, EQ the encoded literal
    uint64_t books_oracle = 0; for (const auto& s : cats) books_oracle += (s == "books");
    MatrixQuery qf; qf.value_col = 1; qf.agg = AGG_COUNT; qf.has_filter = true; qf.cmp = MatrixCmp::EQ;
    qf.threshold = eng.string_encode(1, "books");
    std::vector<uint64_t> cnt;
    assert(eng.execute_query(qf, cnt) == MatrixQueryStatus::OK && cnt.size() == 1 && cnt[0] == books_oracle);
    // an absent literal matches nothing
    MatrixQuery qa = qf; qa.threshold = eng.string_encode(1, "absent");
    std::vector<uint64_t> ca;
    assert(eng.execute_query(qa, ca) == MatrixQueryStatus::OK && ca[0] == 0);
    std::printf("[WHERE string ==] ok (books=%llu, absent=0)\n", (unsigned long long)cnt[0]);

    // COUNT(DISTINCT) over the codes == dictionary size
    assert(eng.count_distinct(1) == eng.string_dict_size(1) && "COUNT(DISTINCT) == dict size");
    std::printf("[COUNT DISTINCT string] ok (%llu)\n", (unsigned long long)eng.count_distinct(1));

    std::printf("ALL STRING-DICT TESTS PASSED\n");
    return 0;
}

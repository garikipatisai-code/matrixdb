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
    const char* names[] = {"music", "books", "games"};   // 3 distinct categories; appearance order != sorted
    const size_t N = 30000;
    std::vector<std::string> cats; std::vector<uint32_t> rev;
    cats.reserve(N); rev.reserve(N);
    for (size_t i = 0; i < N; ++i) { cats.push_back(names[i % 3]); rev.push_back(static_cast<uint32_t>(i % 100)); }

    eng.load_string_column_dict(1, cats);          // col 1 = category (string -> u32 codes, first-class)
    eng.name_column(1, "category");                // name it so the text parser can resolve it
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

    // persistence: the dictionary survives save_catalog/load_catalog (codes ride the normal column path)
    {
        const std::string path = "/tmp/matrixdb_strdict_test.catalog";
        eng.save_catalog(path);
        CPUMockEngine eng2;
        eng2.load_catalog(path);
        assert(eng2.string_dict_size(1) == 3 && "dict size survives restart");
        for (uint32_t c = 0; c < 3; ++c) assert(eng2.string_decode(1, c) == eng.string_decode(1, c) && "decode survives restart");
        MatrixQuery q2; q2.value_col = 2; q2.agg = AGG_SUM; q2.grouped = true; q2.key_col = 1; q2.num_groups = eng2.string_dict_size(1);
        std::vector<uint64_t> g2;
        assert(eng2.execute_query(q2, g2) == MatrixQueryStatus::OK && g2.size() == 3);
        for (uint32_t c = 0; c < 3; ++c) assert(g2[c] == oracle[eng2.string_decode(1, c)] && "restored GROUP BY == oracle");
        std::remove(path.c_str());
        std::printf("[dict persistence] ok\n");
    }

    // SQL-ish text query with a string literal: WHERE category == 'books' (EQ/NE on the dict-encoded column)
    {
        MatrixQuery qp; std::vector<uint64_t> r;
        assert(eng.parse_query("SELECT COUNT(category) WHERE category = 'books'", qp) == MatrixQueryStatus::OK);
        assert(eng.execute_query(qp, r) == MatrixQueryStatus::OK && r[0] == books_oracle);
        MatrixQuery qn; std::vector<uint64_t> rn;
        assert(eng.parse_query("SELECT COUNT(category) WHERE category != 'books'", qn) == MatrixQueryStatus::OK);
        assert(eng.execute_query(qn, rn) == MatrixQueryStatus::OK && rn[0] == N - books_oracle);
        MatrixQuery qz; std::vector<uint64_t> rz;   // absent literal -> matches nothing
        assert(eng.parse_query("SELECT COUNT(category) WHERE category = 'absent'", qz) == MatrixQueryStatus::OK);
        assert(eng.execute_query(qz, rz) == MatrixQueryStatus::OK && rz[0] == 0);
        std::printf("[parse string literal] ok (= %llu, != %llu)\n", (unsigned long long)r[0], (unsigned long long)rn[0]);
    }

    // sorted dictionary: codes are lexicographic rank, so ordered string predicates are meaningful
    assert(eng.string_decode(1, 0) == "books" && eng.string_decode(1, 1) == "games" && eng.string_decode(1, 2) == "music"
           && "dict is sorted (code == lexicographic rank)");
    {
        auto lex_count = [&](const std::string& op, const std::string& x) {
            uint64_t n = 0;
            for (const auto& s : cats) n += (op == ">" ? s > x : op == ">=" ? s >= x : op == "<" ? s < x : s <= x);
            return n;
        };
        struct Cse { const char* sql; const char* op; const char* lit; };
        const Cse cases[] = {
            {"SELECT COUNT(category) WHERE category > 'books'",  ">",  "books"},
            {"SELECT COUNT(category) WHERE category >= 'games'", ">=", "games"},
            {"SELECT COUNT(category) WHERE category < 'music'",  "<",  "music"},
            {"SELECT COUNT(category) WHERE category <= 'games'", "<=", "games"},
        };
        for (const auto& c : cases) {
            MatrixQuery q; std::vector<uint64_t> r;
            assert(eng.parse_query(c.sql, q) == MatrixQueryStatus::OK);
            assert(eng.execute_query(q, r) == MatrixQueryStatus::OK && r[0] == lex_count(c.op, c.lit));
        }
        uint64_t betw = 0; for (const auto& s : cats) betw += (s >= "books" && s <= "games");   // inclusive
        MatrixQuery qb; std::vector<uint64_t> rb;
        assert(eng.parse_query("SELECT COUNT(category) WHERE category BETWEEN 'books' AND 'games'", qb) == MatrixQueryStatus::OK);
        assert(eng.execute_query(qb, rb) == MatrixQueryStatus::OK && rb[0] == betw);
        std::printf("[ordered string predicates] ok (>books=%llu, BETWEEN books..games=%llu)\n",
                    (unsigned long long)lex_count(">", "books"), (unsigned long long)betw);
    }

    // cross-column WHERE: aggregate one column while filtering on ANOTHER (the string-dict complement)
    eng.name_column(2, "revenue");
    {
        uint64_t sum_books = 0, sum_gt_books = 0;
        for (size_t i = 0; i < N; ++i) {
            if (cats[i] == "books") sum_books += rev[i];
            if (cats[i] >  "books") sum_gt_books += rev[i];
        }
        MatrixQuery q1; std::vector<uint64_t> r1;
        assert(eng.parse_query("SELECT SUM(revenue) WHERE category = 'books'", q1) == MatrixQueryStatus::OK);
        assert(q1.value_col == eng.column_id("revenue") && q1.filter_col == eng.column_id("category") && "cross-column resolved");
        assert(eng.execute_query(q1, r1) == MatrixQueryStatus::OK && r1[0] == sum_books);
        MatrixQuery q2; std::vector<uint64_t> r2;   // ordered string filter, different aggregate column
        assert(eng.parse_query("SELECT SUM(revenue) WHERE category > 'books'", q2) == MatrixQueryStatus::OK);
        assert(eng.execute_query(q2, r2) == MatrixQueryStatus::OK && r2[0] == sum_gt_books);
        std::printf("[cross-column WHERE] ok (SUM revenue WHERE category='books'=%llu, >'books'=%llu)\n",
                    (unsigned long long)r1[0], (unsigned long long)r2[0]);
    }

    // cross-column WHERE over TYPED value columns (exercises the i64/f64 filtered_by reducers)
    {
        std::vector<int64_t> rev64(N); std::vector<double> revf(N);
        for (size_t i = 0; i < N; ++i) { rev64[i] = ((int64_t)(i % 100) - 50) * 1000000000LL; revf[i] = ((double)((int64_t)(i % 100) - 50)) * 0.5; }
        eng.load_scan_column_i64(3, rev64.data(), N); eng.name_column(3, "rev64");
        eng.load_scan_column_f64(4, revf.data(), N);  eng.name_column(4, "revf");
        int64_t s64 = 0; double sf = 0.0;
        for (size_t i = 0; i < N; ++i) if (cats[i] == "books") { s64 += rev64[i]; sf += revf[i]; }
        MatrixQuery qi; std::vector<uint64_t> ri;
        assert(eng.parse_query("SELECT SUM(rev64) WHERE category = 'books'", qi) == MatrixQueryStatus::OK);
        assert(eng.execute_query(qi, ri) == MatrixQueryStatus::OK && static_cast<int64_t>(ri[0]) == s64);
        MatrixQuery qf2; std::vector<uint64_t> rf;
        assert(eng.parse_query("SELECT SUM(revf) WHERE category = 'books'", qf2) == MatrixQueryStatus::OK);
        assert(eng.execute_query(qf2, rf) == MatrixQueryStatus::OK && matrix_bit_cast<double>(rf[0]) == sf);
        std::printf("[cross-column typed] ok (i64=%lld, f64=%g)\n", (long long)s64, sf);
    }

    std::printf("ALL STRING-DICT TESTS PASSED\n");
    return 0;
}

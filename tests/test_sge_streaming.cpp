// test_sge_streaming.cpp — equivalence tests for streaming MatrixBuilder + SGE
//
// Verifies that the streaming-source variant of MatrixBuilder produces a
// bit-for-bit identical MatrixBuildResult as the standard vector overload
// when both consume the same relations. Also exercises the OOC reader
// path so the full streaming pipeline (write OOC → read mmap → stream
// matrix build → SGE) matches the vector path.
//
// Test matrix:
//   - VectorRelationSource equivalence (build_with_qc(vec) ==
//     build_with_qc_streaming(VectorSource(vec)))
//   - SGE equivalence (preprocess(vec_matrix) == preprocess(stream_matrix))
//   - OOC round-trip (write relations → OOCRelationReader → stream build)
//   - RelationSelectionSource full-payload equivalence and corpus ordinals
//   - Full-matrix zero dependency → SGE expansion → exact sqrt inputs
//   - Selection identity, dependency length, and ordinal fail-closed checks
//   - Synthetic batch sizes 1 / 10 / 1000 (no internal batching but
//     verifies behavior over varying N)
//   - Empty source (n=0) edge case
//   - Cross-platform deterministic LP column layout

#include <gnfs/factor_base/builder.hpp>
#include <gnfs/linalg/matrix_builder.hpp>
#include <gnfs/linalg/relation_source.hpp>
#include <gnfs/linalg/sge.hpp>
#include <gnfs/linalg/sge_streaming.hpp>
#include <gnfs/polynomial/base_m.hpp>
#include <gnfs/relation/filter.hpp>
#include <gnfs/relation/ooc_relation_store.hpp>
#include <gnfs/relation/relation_corpus.hpp>
#include <gnfs/util/process.hpp>
#include <gnfs/util/temp_path.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace gnfs::core;
using namespace gnfs::polynomial;
using namespace gnfs::factor_base;
using namespace gnfs::linalg;
using namespace gnfs::relation;

// ───────────────────────── helpers ─────────────────────────

[[noreturn]] static void fail(const char* expression, int line) {
    throw std::runtime_error(std::string("CHECK failed at line ") + std::to_string(line) + ": " +
                             expression);
}

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            fail(#expression, __LINE__);                                                           \
        }                                                                                          \
    } while (false)

template <typename Exception, typename Callable> static void expect_throws(Callable&& callable) {
    bool caught = false;
    try {
        std::forward<Callable>(callable)();
    } catch (const Exception&) {
        caught = true;
    }
    CHECK(caught);
}

struct TestEnv {
    Integer n;
    PolynomialContext ctx;
    FactorBase fb;
};

// Use a leading-coeff-odd polynomial so Schirokauer mod 2 setup works.
// N=10403 = 101 * 103 gives degree-2 f with odd lead coeff.
static TestEnv make_env() {
    Integer n("10403");
    auto poly = BaseMSelector::select(n, 2);
    CHECK(poly.success);
    auto ctx = BaseMSelector::create_context(n, poly);

    FactorBaseBuilder::Options opts;
    opts.rational_bound = 200;
    opts.algebraic_bound = 200;
    opts.parallel = false;
    auto fb = FactorBaseBuilder::build(ctx, opts);
    return TestEnv{std::move(n), std::move(ctx), std::move(fb)};
}

static std::vector<Relation> make_synthetic_relations(std::size_t n_rels, std::uint32_t seed = 42) {
    std::vector<Relation> rels;
    rels.reserve(n_rels);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> fb_pick(0, 5);
    std::uniform_int_distribution<int> n_fb(1, 4);
    std::uniform_int_distribution<int> has_lp(0, 100);
    std::uniform_int_distribution<int> lp_p(101, 999);

    for (std::size_t i = 0; i < n_rels; ++i) {
        Relation r(static_cast<std::int64_t>(i + 1), 1u);
        // factors
        int rf_count = n_fb(rng);
        for (int j = 0; j < rf_count; ++j) {
            r.rational_factors.push_back(static_cast<std::uint32_t>(fb_pick(rng)));
        }
        int af_count = n_fb(rng);
        for (int j = 0; j < af_count; ++j) {
            r.algebraic_factors.push_back(static_cast<std::uint32_t>(fb_pick(rng)));
        }
        // optional large primes (~30% have one rational LP)
        if (has_lp(rng) < 30) {
            std::uint64_t p = static_cast<std::uint64_t>(lp_p(rng));
            r.rational_large_prime.push_back({p, 0, 1});
        }
        if (has_lp(rng) < 30) {
            std::uint64_t p = static_cast<std::uint64_t>(lp_p(rng));
            std::uint64_t root = p / 2; // arbitrary root
            r.algebraic_large_prime.push_back({p, root, 1});
        }
        rels.push_back(std::move(r));
    }
    return rels;
}

// Compare two SparseMatrix instances for exact equality
// (same row/col count, same per-row indices after sort).
static bool matrices_equal(const SparseMatrix& a, const SparseMatrix& b) {
    if (a.num_rows() != b.num_rows())
        return false;
    if (a.num_cols() != b.num_cols())
        return false;
    for (std::size_t i = 0; i < a.num_rows(); ++i) {
        const auto& ai = a.row(i).indices(); // ensure_sorted() inside
        const auto& bi = b.row(i).indices();
        if (ai.size() != bi.size())
            return false;
        for (std::size_t k = 0; k < ai.size(); ++k) {
            if (ai[k] != bi[k])
                return false;
        }
    }
    return true;
}

static bool relations_equal(const Relation& a, const Relation& b) {
    return a.a == b.a && a.b == b.b && a.rational_factors == b.rational_factors &&
           a.algebraic_factors == b.algebraic_factors &&
           a.rational_large_prime == b.rational_large_prime &&
           a.algebraic_large_prime == b.algebraic_large_prime &&
           a.extra_ab_pairs == b.extra_ab_pairs;
}

// Compare two ColumnMapping instances for full equivalence.
// LP maps must agree both in size and per-key column index.
static bool mappings_equal(const ColumnMapping& a, const ColumnMapping& b) {
    if (a.num_rational_fb != b.num_rational_fb)
        return false;
    if (a.num_algebraic_fb != b.num_algebraic_fb)
        return false;
    if (a.num_large_primes_rat != b.num_large_primes_rat)
        return false;
    if (a.num_large_primes_alg != b.num_large_primes_alg)
        return false;
    if (a.num_qc_columns != b.num_qc_columns)
        return false;
    if (a.num_class_group_columns != b.num_class_group_columns)
        return false;
    if (a.num_schirokauer_columns != b.num_schirokauer_columns)
        return false;
    if (a.sign_column != b.sign_column)
        return false;
    if (a.has_sign_column != b.has_sign_column)
        return false;
    if (a.rat_lp_to_col.size() != b.rat_lp_to_col.size())
        return false;
    for (const auto& [p, col] : a.rat_lp_to_col) {
        auto it = b.rat_lp_to_col.find(p);
        if (it == b.rat_lp_to_col.end() || it->second != col)
            return false;
    }
    if (a.alg_lp_to_col.size() != b.alg_lp_to_col.size())
        return false;
    for (const auto& [key, col] : a.alg_lp_to_col) {
        auto it = b.alg_lp_to_col.find(key);
        if (it == b.alg_lp_to_col.end() || it->second != col)
            return false;
    }
    return a.qc_prime_roots == b.qc_prime_roots && a.schirokauer_primes == b.schirokauer_primes;
}

static bool build_results_equal(const MatrixBuildResult& a, const MatrixBuildResult& b) {
    return matrices_equal(a.matrix, b.matrix) && mappings_equal(a.mapping, b.mapping) &&
           a.row_to_relation == b.row_to_relation;
}

static MatrixBuilderConfig minimal_mb_config() {
    MatrixBuilderConfig cfg;
    cfg.include_sign_column = true;
    cfg.include_qc_columns = false; // skip QC: small N, slow root search
    cfg.include_class_group = false;
    cfg.include_schirokauer = true;
    cfg.schirokauer_primes = {2};
    cfg.verbose = false;
    return cfg;
}

static MatrixBuilderConfig full_payload_mb_config() {
    MatrixBuilderConfig cfg;
    cfg.include_sign_column = true;
    cfg.include_qc_columns = true;
    cfg.include_class_group = false;
    cfg.include_schirokauer = true;
    cfg.num_qc_primes = 6;
    cfg.qc_prime_start = 211;
    cfg.schirokauer_primes = {2};
    cfg.verbose = false;
    return cfg;
}

// ───────────────────────── tests ─────────────────────────

void test_vector_source_equivalence_small() {
    std::cout << "Testing VectorRelationSource equivalence (n=1)..." << std::endl;
    auto env = make_env();
    auto rels = make_synthetic_relations(1);

    MatrixBuilder mb(minimal_mb_config());

    auto vec_result = mb.build_with_qc(rels, env.fb, env.ctx);
    VectorRelationSource src(rels);
    auto stream_result = mb.build_with_qc_streaming(src, env.fb, env.ctx);

    CHECK(mappings_equal(vec_result.mapping, stream_result.mapping));
    CHECK(matrices_equal(vec_result.matrix, stream_result.matrix));
    CHECK(vec_result.row_to_relation == stream_result.row_to_relation);
    std::cout << "  PASS" << std::endl;
}

void test_vector_source_equivalence_batch_sizes() {
    std::cout << "Testing VectorRelationSource equivalence across sizes..." << std::endl;
    auto env = make_env();

    for (std::size_t n : {1u, 10u, 100u, 1000u}) {
        auto rels = make_synthetic_relations(n);

        MatrixBuilder mb(minimal_mb_config());
        auto vec_result = mb.build_with_qc(rels, env.fb, env.ctx);
        VectorRelationSource src(rels);
        auto stream_result = mb.build_with_qc_streaming(src, env.fb, env.ctx);

        CHECK(mappings_equal(vec_result.mapping, stream_result.mapping));
        CHECK(matrices_equal(vec_result.matrix, stream_result.matrix));
        std::cout << "  n=" << n << " vec=" << vec_result.matrix.num_rows() << "x"
                  << vec_result.matrix.num_cols() << " stream=" << stream_result.matrix.num_rows()
                  << "x" << stream_result.matrix.num_cols() << " (match)" << std::endl;
    }
    std::cout << "  PASS" << std::endl;
}

void test_empty_source() {
    std::cout << "Testing empty source (n=0)..." << std::endl;
    auto env = make_env();
    std::vector<Relation> empty;

    MatrixBuilder mb(minimal_mb_config());
    VectorRelationSource src(empty);
    auto result = mb.build_with_qc_streaming(src, env.fb, env.ctx);

    CHECK(result.matrix.num_rows() == 0);
    CHECK(result.row_to_relation.empty());
    std::cout << "  PASS (empty source produces 0 rows, mapping intact)" << std::endl;
}

void test_relation_selection_source_payload_equivalence() {
    std::cout << "Testing selected-corpus source full-payload equivalence..." << std::endl;
    auto env = make_env();
    const auto rels = make_synthetic_relations(8, /*seed=*/701);
    auto corpus = RelationCorpus::from_in_memory(701, rels);
    const std::vector<std::size_t> selected_ordinals{7, 2, 5};
    const auto selection = RelationSelection::from_ordinals(corpus, selected_ordinals);
    RelationSelectionSource source(corpus, selection);

    CHECK(source.count() == selected_ordinals.size());
    for (std::size_t row = 0; row < selected_ordinals.size(); ++row) {
        CHECK(source.source_ordinal(row) == selected_ordinals[row]);
        CHECK(relations_equal(source.read(row), rels[selected_ordinals[row]]));
    }

    MatrixBuilder builder(minimal_mb_config());
    const auto materialized = materialize_selected(corpus, selection);
    auto expected = builder.build_with_qc(materialized, env.fb, env.ctx);
    const std::vector<std::size_t> identity_rows{0, 1, 2};
    CHECK(expected.row_to_relation == identity_rows);

    const auto actual = builder.build_with_qc_streaming(source, env.fb, env.ctx);
    expected.row_to_relation = selected_ordinals;
    CHECK(build_results_equal(expected, actual));

    std::cout << "  PASS (non-monotonic ordinals 7,2,5 preserved)" << std::endl;
}

void test_relation_selection_dependency_provenance() {
    std::cout << "Testing zero-dependency provenance to corpus selection..." << std::endl;
    auto env = make_env();

    Relation first(211, 1);
    first.rational_factors = {0, 1, 1};
    first.algebraic_factors = {0, 2};
    first.rational_large_prime = {{1009, 0, 1}, {1013, 0, 1}};
    first.algebraic_large_prime = {{2003, 17, 1}, {2011, 19, 1}};

    Relation second(-37, 1);
    second.rational_factors = {1, 2};
    second.algebraic_factors = {1, 2};
    second.rational_large_prime = {{1009, 0, 1}, {1019, 0, 1}};
    second.algebraic_large_prime = {{2003, 17, 1}, {2017, 23, 1}};

    const Relation merged = PartialRelationMerger::merge_two(first, second);
    auto corpus_relations = make_synthetic_relations(8, /*seed=*/702);
    corpus_relations[7] = first;
    corpus_relations[2] = second;
    corpus_relations[5] = merged;

    auto corpus = RelationCorpus::from_in_memory(702, corpus_relations);
    const auto selection = RelationSelection::from_ordinals(corpus, {7, 2, 5});
    RelationSelectionSource source(corpus, selection);

    MatrixBuilder builder(full_payload_mb_config());
    const auto materialized = materialize_selected(corpus, selection);
    auto expected_build = builder.build_with_qc(materialized, env.fb, env.ctx);
    const std::vector<std::size_t> selected_ordinals{7, 2, 5};
    expected_build.row_to_relation = selected_ordinals;
    const auto build = builder.build_with_qc_streaming(source, env.fb, env.ctx);
    CHECK(build_results_equal(expected_build, build));

    CHECK(build.mapping.has_sign_column);
    CHECK(build.mapping.num_rational_fb > 0);
    CHECK(build.mapping.num_algebraic_fb > 0);
    CHECK(build.mapping.num_large_primes_rat > 0);
    CHECK(build.mapping.num_large_primes_alg > 0);
    CHECK(build.mapping.num_qc_columns > 0);
    CHECK(build.mapping.num_schirokauer_columns > 0);

    // merge_two(first, second) represents their product. Every matrix payload
    // is a GF(2) homomorphism, so first XOR second XOR merged must be zero in
    // the actual fully configured matrix, not merely in a synthetic row map.
    std::vector<bool> column_parity(build.matrix.num_cols(), false);
    for (std::size_t row = 0; row < build.matrix.num_rows(); ++row) {
        for (const std::uint32_t column : build.matrix.row(row).indices()) {
            column_parity[column] = !column_parity[column];
        }
    }
    for (bool parity : column_parity) {
        CHECK(!parity);
    }

    SGEConfig sge_config;
    sge_config.batch_pivots = 1;
    const auto sge_result = SGE::preprocess(build.matrix, sge_config);
    const std::vector<std::size_t> all_original_rows{0, 1, 2};
    std::size_t dependency_row = sge_result.row_composition.size();
    for (std::size_t row = 0; row < sge_result.row_composition.size(); ++row) {
        if (sge_result.row_composition[row] == all_original_rows) {
            dependency_row = row;
            break;
        }
    }
    CHECK(dependency_row < sge_result.row_composition.size());
    CHECK(sge_result.reduced_matrix.row(dependency_row).indices().empty());

    std::vector<bool> reduced_dependency(sge_result.reduced_matrix.num_rows(), false);
    reduced_dependency[dependency_row] = true;
    const auto expanded_dependency = sge_result.expand_dependency(reduced_dependency);
    const std::vector<bool> expected_expanded{true, true, true};
    CHECK(expanded_dependency == expected_expanded);

    const auto dependency_selection =
        dependency_to_relation_selection(corpus, build.row_to_relation, expanded_dependency);
    const std::vector<std::size_t> expected_ordinals{2, 5, 7};
    CHECK(dependency_selection.ordinals() == expected_ordinals);

    // This is the exact relation vector that the rational/algebraic square
    // root phase will consume for the dependency.
    const auto sqrt_relations = materialize_selected(corpus, dependency_selection);
    CHECK(sqrt_relations.size() == expected_ordinals.size());
    CHECK(relations_equal(sqrt_relations[0], second));
    CHECK(relations_equal(sqrt_relations[1], merged));
    CHECK(relations_equal(sqrt_relations[2], first));

    // A corpus ordinal that appears twice cancels in GF(2), even when the
    // duplicate comes from two distinct matrix rows.
    const std::vector<std::size_t> duplicate_row_map{7, 2, 7};
    const std::vector<bool> all_rows{true, true, true};
    const auto xor_selection =
        dependency_to_relation_selection(corpus, duplicate_row_map, all_rows);
    const std::vector<std::size_t> xor_expected{2};
    CHECK(xor_selection.ordinals() == xor_expected);

    std::cout << "  PASS (full matrix dependency maps to exact sqrt inputs)" << std::endl;
}

void test_relation_selection_fail_closed() {
    std::cout << "Testing selected-corpus source and dependency fail-closed checks..." << std::endl;
    const auto rels = make_synthetic_relations(8, /*seed=*/703);
    auto corpus = RelationCorpus::from_in_memory(703, rels);
    const auto selection = RelationSelection::from_ordinals(corpus, {7, 2, 5});

    auto foreign_corpus = RelationCorpus::from_in_memory(703, rels);
    expect_throws<std::invalid_argument>([&] {
        RelationSelectionSource foreign_source(foreign_corpus, selection);
        (void)foreign_source;
    });

    const std::vector<std::size_t> row_map{7, 2, 5};
    expect_throws<std::invalid_argument>([&] {
        const std::vector<bool> wrong_length{true, false};
        (void)dependency_to_relation_selection(corpus, row_map, wrong_length);
    });

    // Validate every row mapping, including rows not enabled by the
    // dependency, so malformed provenance never passes conditionally.
    expect_throws<std::out_of_range>([&] {
        const std::vector<std::size_t> invalid_row_map{7, rels.size(), 5};
        const std::vector<bool> dependency{true, false, true};
        (void)dependency_to_relation_selection(corpus, invalid_row_map, dependency);
    });

    RelationSelectionSource source(corpus, selection);
    expect_throws<std::out_of_range>([&] { (void)source.read(source.count()); });
    expect_throws<std::out_of_range>([&] { (void)source.source_ordinal(source.count()); });

    std::cout << "  PASS" << std::endl;
}

void test_sge_equivalence() {
    std::cout << "Testing SGE equivalence on streaming vs vector matrix..." << std::endl;
    auto env = make_env();
    auto rels = make_synthetic_relations(200);

    MatrixBuilder mb(minimal_mb_config());

    auto vec_result = mb.build_with_qc(rels, env.fb, env.ctx);
    VectorRelationSource src(rels);
    auto stream_result = mb.build_with_qc_streaming(src, env.fb, env.ctx);

    SGEConfig sge_config;
    sge_config.verbose = false;

    auto vec_sge = SGE::preprocess(vec_result.matrix, sge_config);
    auto stream_sge = SGE::preprocess(stream_result.matrix, sge_config);

    // SGE result must agree (matrices were identical, deterministic algorithm)
    CHECK(vec_sge.original_rows == stream_sge.original_rows);
    CHECK(vec_sge.original_cols == stream_sge.original_cols);
    CHECK(vec_sge.passes == stream_sge.passes);
    CHECK(vec_sge.weight1_eliminated == stream_sge.weight1_eliminated);
    CHECK(vec_sge.weight2_merged == stream_sge.weight2_merged);
    CHECK(matrices_equal(vec_sge.reduced_matrix, stream_sge.reduced_matrix));

    std::cout << "  PASS (matrix " << vec_result.matrix.num_rows() << "x"
              << vec_result.matrix.num_cols() << " → " << vec_sge.reduced_matrix.num_rows() << "x"
              << vec_sge.reduced_matrix.num_cols() << ", w1=" << vec_sge.weight1_eliminated
              << " w2=" << vec_sge.weight2_merged << ")" << std::endl;
}

void test_preprocess_streaming_convenience() {
    std::cout << "Testing preprocess_streaming convenience wrapper..." << std::endl;
    auto env = make_env();
    auto rels = make_synthetic_relations(150);

    MatrixBuilder mb(minimal_mb_config());
    auto vec_result = mb.build_with_qc(rels, env.fb, env.ctx);
    SGEConfig sge_config;
    auto vec_sge = SGE::preprocess(vec_result.matrix, sge_config);

    VectorRelationSource src(rels);
    auto combined = preprocess_streaming(src, env.fb, env.ctx, minimal_mb_config(), sge_config);

    CHECK(mappings_equal(vec_result.mapping, combined.build_result.mapping));
    CHECK(matrices_equal(vec_result.matrix, combined.build_result.matrix));
    CHECK(matrices_equal(vec_sge.reduced_matrix, combined.sge_result.reduced_matrix));

    std::cout << "  PASS" << std::endl;
}

void test_ooc_roundtrip_streaming() {
    std::cout << "Testing OOC round-trip → streaming build → SGE equivalence..." << std::endl;
    auto env = make_env();
    auto rels = make_synthetic_relations(500, /*seed=*/123);

    // Write relations through OOCRelationWriter so we exercise the on-disk
    // serialize/deserialize path (mmap → Relation reconstruction).
    std::string base_path = gnfs::util::temp_path("gnfs_sge_streaming_test_" +
                                                  std::to_string(gnfs::util::process_id()));
    {
        OOCRelationWriter writer(base_path);
        for (const auto& r : rels) {
            writer.write(r);
        }
        writer.close();
    }

    OOCRelationReader reader(base_path);
    CHECK(reader.count() == rels.size());

    MatrixBuilder mb(minimal_mb_config());
    auto vec_result = mb.build_with_qc(rels, env.fb, env.ctx);

    OOCRelationSource src(reader);
    auto stream_result = mb.build_with_qc_streaming(src, env.fb, env.ctx);

    CHECK(mappings_equal(vec_result.mapping, stream_result.mapping));
    CHECK(matrices_equal(vec_result.matrix, stream_result.matrix));

    SGEConfig sge_config;
    auto vec_sge = SGE::preprocess(vec_result.matrix, sge_config);
    auto stream_sge = SGE::preprocess(stream_result.matrix, sge_config);
    CHECK(matrices_equal(vec_sge.reduced_matrix, stream_sge.reduced_matrix));

    // cleanup
    std::remove((base_path + ".reldata").c_str());
    std::remove((base_path + ".relidx").c_str());

    std::cout << "  PASS (500 rels written to disk, mmap read," << " matrix "
              << stream_result.matrix.num_rows() << "x" << stream_result.matrix.num_cols()
              << " matches vector)" << std::endl;
}

void test_ooc_relation_selection_source() {
    std::cout << "Testing finalized OOC corpus full-payload selection source..." << std::endl;
    auto env = make_env();

    // Use the same deterministic payload shape as the in-memory provenance
    // oracle so every supported matrix payload is present independent of RNG.
    Relation first(211, 1);
    first.rational_factors = {0, 1, 1};
    first.algebraic_factors = {0, 2};
    first.rational_large_prime = {{1009, 0, 1}, {1013, 0, 1}};
    first.algebraic_large_prime = {{2003, 17, 1}, {2011, 19, 1}};

    Relation second(-37, 1);
    second.rational_factors = {1, 2};
    second.algebraic_factors = {1, 2};
    second.rational_large_prime = {{1009, 0, 1}, {1019, 0, 1}};
    second.algebraic_large_prime = {{2003, 17, 1}, {2017, 23, 1}};

    const Relation merged = PartialRelationMerger::merge_two(first, second);
    auto rels = make_synthetic_relations(8, /*seed=*/704);
    rels[7] = first;
    rels[2] = second;
    rels[5] = merged;
    const std::string base_path = gnfs::util::temp_path("gnfs_sge_selection_ooc_test_" +
                                                        std::to_string(gnfs::util::process_id()));

    const auto descriptor = [&] {
        OOCRelationWriter writer(base_path);
        for (const auto& relation : rels) {
            writer.write(relation);
        }
        return writer.finalize();
    }();

    {
        auto corpus = RelationCorpus::from_finalized_ooc(704, base_path, descriptor);
        const std::vector<std::size_t> selected_ordinals{7, 2, 5};
        const auto selection = RelationSelection::from_ordinals(corpus, selected_ordinals);
        RelationSelectionSource source(corpus, selection);

        MatrixBuilder builder(full_payload_mb_config());
        const auto materialized = materialize_selected(corpus, selection);
        auto expected = builder.build_with_qc(materialized, env.fb, env.ctx);
        expected.row_to_relation = selected_ordinals;
        const auto actual = builder.build_with_qc_streaming(source, env.fb, env.ctx);
        CHECK(build_results_equal(expected, actual));
        CHECK(actual.mapping.has_sign_column);
        CHECK(actual.mapping.num_rational_fb > 0);
        CHECK(actual.mapping.num_algebraic_fb > 0);
        CHECK(actual.mapping.num_large_primes_rat > 0);
        CHECK(actual.mapping.num_large_primes_alg > 0);
        CHECK(actual.mapping.num_qc_columns > 0);
        CHECK(actual.mapping.num_schirokauer_columns > 0);
    }

    // The corpus scope closes mmap/file handles before deletion (required on
    // Windows as well as harmless on POSIX).
    CHECK(std::remove((base_path + ".relidx").c_str()) == 0);
    CHECK(std::remove((base_path + ".reldata").c_str()) == 0);

    std::cout << "  PASS (full sign/FB/LP/QC/Schirokauer payload)" << std::endl;
}

void test_relation_source_concept_conformance() {
    std::cout << "Testing RelationSource concept conformance..." << std::endl;
    static_assert(RelationSource<VectorRelationSource>);
    static_assert(RelationSource<OOCRelationSource>);
    static_assert(RelationSource<RelationSelectionSource>);
    static_assert(OrdinalRelationSource<RelationSelectionSource>);
    static_assert(!OrdinalRelationSource<VectorRelationSource>);
    static_assert(!OrdinalRelationSource<OOCRelationSource>);
    std::cout << "  PASS (static_assert checks)" << std::endl;
}

void test_sge_streaming_with_lps() {
    std::cout << "Testing SGE on streaming matrix with LP columns..." << std::endl;
    auto env = make_env();
    // Higher LP density to ensure LP columns are actually present.
    std::vector<Relation> rels;
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> fb_pick(0, 5);
    std::uniform_int_distribution<int> lp_p(101, 200);
    for (std::size_t i = 0; i < 300; ++i) {
        Relation r(static_cast<std::int64_t>(i + 1), 1u);
        r.rational_factors = {static_cast<std::uint32_t>(fb_pick(rng)),
                              static_cast<std::uint32_t>(fb_pick(rng))};
        r.algebraic_factors = {static_cast<std::uint32_t>(fb_pick(rng))};
        // High-density LP: shared across relations to give SGE work.
        // Use small LP pool so collisions create columns with weight ≥ 2.
        std::uint64_t p = static_cast<std::uint64_t>(101 + (i % 10));
        r.rational_large_prime.push_back({p, 0, 1});
        rels.push_back(std::move(r));
    }

    MatrixBuilder mb(minimal_mb_config());
    auto vec_result = mb.build_with_qc(rels, env.fb, env.ctx);
    VectorRelationSource src(rels);
    auto stream_result = mb.build_with_qc_streaming(src, env.fb, env.ctx);

    CHECK(matrices_equal(vec_result.matrix, stream_result.matrix));
    CHECK(vec_result.mapping.num_large_primes_rat == stream_result.mapping.num_large_primes_rat);
    CHECK(vec_result.mapping.num_large_primes_rat > 0);

    SGEConfig sge_config;
    auto vec_sge = SGE::preprocess(vec_result.matrix, sge_config);
    auto stream_sge = SGE::preprocess(stream_result.matrix, sge_config);
    CHECK(matrices_equal(vec_sge.reduced_matrix, stream_sge.reduced_matrix));
    CHECK(vec_sge.weight1_eliminated == stream_sge.weight1_eliminated);
    CHECK(vec_sge.weight2_merged == stream_sge.weight2_merged);

    std::cout << "  PASS (LP cols=" << vec_result.mapping.num_large_primes_rat << ", reduce "
              << vec_result.matrix.num_rows() << "x" << vec_result.matrix.num_cols() << " → "
              << vec_sge.reduced_matrix.num_rows() << "x" << vec_sge.reduced_matrix.num_cols()
              << ")" << std::endl;
}

// ───────────────────────── main ─────────────────────────

int main() {
    std::cout << "=== SGE Streaming MatrixBuilder Tests ===" << std::endl;

    test_relation_source_concept_conformance();
    test_empty_source();
    test_vector_source_equivalence_small();
    test_vector_source_equivalence_batch_sizes();
    test_relation_selection_source_payload_equivalence();
    test_relation_selection_dependency_provenance();
    test_relation_selection_fail_closed();
    test_sge_equivalence();
    test_preprocess_streaming_convenience();
    test_sge_streaming_with_lps();
    test_ooc_roundtrip_streaming();
    test_ooc_relation_selection_source();

    std::cout << "\n=== All SGE Streaming Tests PASSED ===" << std::endl;
    return 0;
}

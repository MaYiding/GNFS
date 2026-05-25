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
//   - Synthetic batch sizes 1 / 10 / 1000 (no internal batching but
//     verifies behavior over varying N)
//   - Empty source (n=0) edge case
//   - Cross-platform deterministic LP column layout

#include <gnfs/polynomial/base_m.hpp>
#include <gnfs/factor_base/builder.hpp>
#include <gnfs/linalg/matrix_builder.hpp>
#include <gnfs/linalg/sge.hpp>
#include <gnfs/linalg/sge_streaming.hpp>
#include <gnfs/linalg/relation_source.hpp>
#include <gnfs/relation/ooc_relation_store.hpp>
#include <gnfs/util/process.hpp>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace gnfs::core;
using namespace gnfs::polynomial;
using namespace gnfs::factor_base;
using namespace gnfs::linalg;
using namespace gnfs::relation;

// ───────────────────────── helpers ─────────────────────────

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
    assert(poly.success);
    auto ctx = BaseMSelector::create_context(n, poly);

    FactorBaseBuilder::Options opts;
    opts.rational_bound  = 200;
    opts.algebraic_bound = 200;
    opts.parallel = false;
    auto fb = FactorBaseBuilder::build(ctx, opts);
    return TestEnv{std::move(n), std::move(ctx), std::move(fb)};
}

static std::vector<Relation> make_synthetic_relations(std::size_t n_rels,
                                                      std::uint32_t seed = 42) {
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
            std::uint64_t root = p / 2;  // arbitrary root
            r.algebraic_large_prime.push_back({p, root, 1});
        }
        rels.push_back(std::move(r));
    }
    return rels;
}

// Compare two SparseMatrix instances for exact equality
// (same row/col count, same per-row indices after sort).
static bool matrices_equal(const SparseMatrix& a, const SparseMatrix& b) {
    if (a.num_rows() != b.num_rows()) return false;
    if (a.num_cols() != b.num_cols()) return false;
    for (std::size_t i = 0; i < a.num_rows(); ++i) {
        const auto& ai = a.row(i).indices();  // ensure_sorted() inside
        const auto& bi = b.row(i).indices();
        if (ai.size() != bi.size()) return false;
        for (std::size_t k = 0; k < ai.size(); ++k) {
            if (ai[k] != bi[k]) return false;
        }
    }
    return true;
}

// Compare two ColumnMapping instances for full equivalence.
// LP maps must agree both in size and per-key column index.
static bool mappings_equal(const ColumnMapping& a, const ColumnMapping& b) {
    if (a.num_rational_fb != b.num_rational_fb) return false;
    if (a.num_algebraic_fb != b.num_algebraic_fb) return false;
    if (a.num_large_primes_rat != b.num_large_primes_rat) return false;
    if (a.num_large_primes_alg != b.num_large_primes_alg) return false;
    if (a.num_qc_columns != b.num_qc_columns) return false;
    if (a.num_class_group_columns != b.num_class_group_columns) return false;
    if (a.num_schirokauer_columns != b.num_schirokauer_columns) return false;
    if (a.has_sign_column != b.has_sign_column) return false;
    if (a.rat_lp_to_col.size() != b.rat_lp_to_col.size()) return false;
    for (const auto& [p, col] : a.rat_lp_to_col) {
        auto it = b.rat_lp_to_col.find(p);
        if (it == b.rat_lp_to_col.end() || it->second != col) return false;
    }
    if (a.alg_lp_to_col.size() != b.alg_lp_to_col.size()) return false;
    for (const auto& [key, col] : a.alg_lp_to_col) {
        auto it = b.alg_lp_to_col.find(key);
        if (it == b.alg_lp_to_col.end() || it->second != col) return false;
    }
    return true;
}

static MatrixBuilderConfig minimal_mb_config() {
    MatrixBuilderConfig cfg;
    cfg.include_sign_column = true;
    cfg.include_qc_columns  = false;  // skip QC: small N, slow root search
    cfg.include_class_group = false;
    cfg.include_schirokauer = true;
    cfg.schirokauer_primes  = {2};
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

    assert(mappings_equal(vec_result.mapping, stream_result.mapping));
    assert(matrices_equal(vec_result.matrix, stream_result.matrix));
    assert(vec_result.row_to_relation == stream_result.row_to_relation);
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

        assert(mappings_equal(vec_result.mapping, stream_result.mapping));
        assert(matrices_equal(vec_result.matrix, stream_result.matrix));
        std::cout << "  n=" << n
                  << " vec=" << vec_result.matrix.num_rows() << "x" << vec_result.matrix.num_cols()
                  << " stream=" << stream_result.matrix.num_rows() << "x" << stream_result.matrix.num_cols()
                  << " (match)" << std::endl;
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

    assert(result.matrix.num_rows() == 0);
    assert(result.row_to_relation.empty());
    std::cout << "  PASS (empty source produces 0 rows, mapping intact)" << std::endl;
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
    assert(vec_sge.original_rows == stream_sge.original_rows);
    assert(vec_sge.original_cols == stream_sge.original_cols);
    assert(vec_sge.passes == stream_sge.passes);
    assert(vec_sge.weight1_eliminated == stream_sge.weight1_eliminated);
    assert(vec_sge.weight2_merged == stream_sge.weight2_merged);
    assert(matrices_equal(vec_sge.reduced_matrix, stream_sge.reduced_matrix));

    std::cout << "  PASS (matrix " << vec_result.matrix.num_rows() << "x"
              << vec_result.matrix.num_cols() << " → "
              << vec_sge.reduced_matrix.num_rows() << "x"
              << vec_sge.reduced_matrix.num_cols() << ", w1="
              << vec_sge.weight1_eliminated << " w2=" << vec_sge.weight2_merged << ")"
              << std::endl;
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
    auto combined = preprocess_streaming(src, env.fb, env.ctx,
                                          minimal_mb_config(), sge_config);

    assert(mappings_equal(vec_result.mapping, combined.build_result.mapping));
    assert(matrices_equal(vec_result.matrix, combined.build_result.matrix));
    assert(matrices_equal(vec_sge.reduced_matrix, combined.sge_result.reduced_matrix));

    std::cout << "  PASS" << std::endl;
}

void test_ooc_roundtrip_streaming() {
    std::cout << "Testing OOC round-trip → streaming build → SGE equivalence..." << std::endl;
    auto env = make_env();
    auto rels = make_synthetic_relations(500, /*seed=*/123);

    // Write relations through OOCRelationWriter so we exercise the on-disk
    // serialize/deserialize path (mmap → Relation reconstruction).
    std::string base_path = "/tmp/gnfs_sge_streaming_test_" +
                            std::to_string(gnfs::util::process_id());
    {
        OOCRelationWriter writer(base_path);
        for (const auto& r : rels) {
            writer.write(r);
        }
        writer.close();
    }

    OOCRelationReader reader(base_path);
    assert(reader.count() == rels.size());

    MatrixBuilder mb(minimal_mb_config());
    auto vec_result = mb.build_with_qc(rels, env.fb, env.ctx);

    OOCRelationSource src(reader);
    auto stream_result = mb.build_with_qc_streaming(src, env.fb, env.ctx);

    assert(mappings_equal(vec_result.mapping, stream_result.mapping));
    assert(matrices_equal(vec_result.matrix, stream_result.matrix));

    SGEConfig sge_config;
    auto vec_sge = SGE::preprocess(vec_result.matrix, sge_config);
    auto stream_sge = SGE::preprocess(stream_result.matrix, sge_config);
    assert(matrices_equal(vec_sge.reduced_matrix, stream_sge.reduced_matrix));

    // cleanup
    std::remove((base_path + ".reldata").c_str());
    std::remove((base_path + ".relidx").c_str());

    std::cout << "  PASS (500 rels written to disk, mmap read,"
              << " matrix " << stream_result.matrix.num_rows() << "x"
              << stream_result.matrix.num_cols() << " matches vector)" << std::endl;
}

void test_relation_source_concept_conformance() {
    std::cout << "Testing RelationSource concept conformance..." << std::endl;
    static_assert(RelationSource<VectorRelationSource>);
    static_assert(RelationSource<OOCRelationSource>);
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

    assert(matrices_equal(vec_result.matrix, stream_result.matrix));
    assert(vec_result.mapping.num_large_primes_rat ==
           stream_result.mapping.num_large_primes_rat);
    assert(vec_result.mapping.num_large_primes_rat > 0);

    SGEConfig sge_config;
    auto vec_sge = SGE::preprocess(vec_result.matrix, sge_config);
    auto stream_sge = SGE::preprocess(stream_result.matrix, sge_config);
    assert(matrices_equal(vec_sge.reduced_matrix, stream_sge.reduced_matrix));
    assert(vec_sge.weight1_eliminated == stream_sge.weight1_eliminated);
    assert(vec_sge.weight2_merged == stream_sge.weight2_merged);

    std::cout << "  PASS (LP cols=" << vec_result.mapping.num_large_primes_rat
              << ", reduce " << vec_result.matrix.num_rows() << "x"
              << vec_result.matrix.num_cols() << " → "
              << vec_sge.reduced_matrix.num_rows() << "x"
              << vec_sge.reduced_matrix.num_cols() << ")" << std::endl;
}

// ───────────────────────── main ─────────────────────────

int main() {
    std::cout << "=== SGE Streaming MatrixBuilder Tests ===" << std::endl;

    test_relation_source_concept_conformance();
    test_empty_source();
    test_vector_source_equivalence_small();
    test_vector_source_equivalence_batch_sizes();
    test_sge_equivalence();
    test_preprocess_streaming_convenience();
    test_sge_streaming_with_lps();
    test_ooc_roundtrip_streaming();

    std::cout << "\n=== All SGE Streaming Tests PASSED ===" << std::endl;
    return 0;
}

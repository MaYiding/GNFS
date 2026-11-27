// test_non_monic_matrix_constraints_e2e.cpp — End-to-end GNFS factorization
// with non-monic polynomials that need the non-monic matrix constraints
//
// PolynomialContext::algebraic_norm returns F(a, b) = b^d * f(a / b) =
// a_d * N(a - b*alpha). The factor-base, large-prime, and sign columns only
// force the product of the F values of a dependency to be a square integer.
// When a_d is not a square, a dependency that contains an odd number of
// (a, b) pairs has a non-square norm and cannot be a square in the number
// field. MatrixBuilder therefore records the parity of the pair count in one
// extra column.
//
// A Schirokauer map also needs f to keep its degree modulo ℓ = 2. When 2
// divides a_d the map is not a homomorphism, so MatrixBuilder builds no
// Schirokauer column and keeps the extra quadratic-character columns that
// compensate for the missing constraints.
//
// Each fixture must factor its N through sieve, cofactorization, filtering,
// matrix, dependencies, and square root:
//
//   even a_d          2x^2 + x + 1,               m = 14,   N = 407 = 11 * 37
//   odd non-square a_d
//                     7x^3 + 58x^2 + 213x + 143,  m = 240,  N = 100160063 = 10007 * 10009
//
// Both polynomials are irreducible over Q. The flow mirrors
// tests/test_schirokauer_non_monic_e2e.cpp; only the fixtures and the matrix
// assertions differ.

#include "support/test_check.hpp"

#include <gnfs/cofactor/cofactorizer.hpp>
#include <gnfs/core/params.hpp>
#include <gnfs/core/types.hpp>
#include <gnfs/factor_base/builder.hpp>
#include <gnfs/linalg/block_lanczos.hpp>
#include <gnfs/linalg/matrix_builder.hpp>
#include <gnfs/linalg/sge.hpp>
#include <gnfs/relation/collector.hpp>
#include <gnfs/relation/filter.hpp>
#include <gnfs/relation/reduction_engine.hpp>
#include <gnfs/sieve/lattice_sieve.hpp>
#include <gnfs/sieve/special_q.hpp>
#include <gnfs/sqrt/algebraic_sqrt.hpp>
#include <gnfs/sqrt/rational_sqrt.hpp>
#include <gnfs/util/safe_math.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace gnfs;
using namespace gnfs::core;
using namespace gnfs::factor_base;
using namespace gnfs::sieve;
using namespace gnfs::cofactor;
using namespace gnfs::relation;
using namespace gnfs::linalg;
using namespace gnfs::sqrt;

namespace {

// f(x) = Σ coefficients[i] x^i, low degree first, with f(m) = N.
struct ConstraintFixture {
    const char* label;
    std::vector<int64_t> coefficients;
    int64_t m;
    const char* n;
    const char* expected_p;
    const char* expected_q;
    // deg(f) when 2 does not divide the leading coefficient, otherwise 0.
    size_t expected_schirokauer_columns;
};

const ConstraintFixture kFixtures[] = {
    {"even a_d = 2", {1, 1, 2}, 14, "407", "11", "37", 0},
    {"odd non-square a_d = 7", {143, 213, 58, 7}, 240, "100160063", "10007", "10009", 3},
};

PolynomialContext make_fixture_context(const ConstraintFixture& fixture, const Integer& n) {
    std::vector<Integer> coefficients;
    coefficients.reserve(fixture.coefficients.size());
    for (int64_t value : fixture.coefficients)
        coefficients.emplace_back(value);
    return PolynomialContext(n, std::move(coefficients), Integer(fixture.m));
}

// Exact test: the leading coefficient is a square in Q.
bool is_rational_square(const Integer& value) {
    if (value.is_negative())
        return false;
    Integer root = core::sqrt(value);
    root *= root;
    return root == value;
}

// A dependency is a null vector of the matrix: every column has even weight
// over the selected rows. The parity column is the last one.
bool is_null_vector(const SparseMatrix& matrix, const std::vector<bool>& dependency) {
    if (dependency.size() != matrix.num_rows())
        return false;
    std::vector<bool> parity(matrix.num_cols(), false);
    for (size_t row = 0; row < matrix.num_rows(); ++row) {
        if (!dependency[row])
            continue;
        for (uint32_t column : matrix.row(row).indices())
            parity[column] = !parity[column];
    }
    return std::find(parity.begin(), parity.end(), true) == parity.end();
}

// Returns true when a nontrivial factor of n was recovered.
bool factorize_with(const ConstraintFixture& fixture) {
    Integer n(fixture.n);
    const Integer expected_p(fixture.expected_p);
    const Integer expected_q(fixture.expected_q);
    GNFS_TEST_CHECK(expected_p * expected_q == n);

    size_t bits = n.bit_length();
    auto params = GNFSParams::compute(bits);

    // Phase 1: the fixed non-monic polynomial
    const auto ctx = make_fixture_context(fixture, n);
    GNFS_TEST_CHECK(ctx.verify());
    GNFS_TEST_CHECK(ctx.degree() + 1 == fixture.coefficients.size());
    GNFS_TEST_CHECK(!is_rational_square(ctx.leading_coeff()));
    const bool even_leading = ctx.leading_coeff().is_even();
    GNFS_TEST_CHECK(even_leading == (fixture.expected_schirokauer_columns == 0));
    std::cout << "  " << fixture.label << ", N = " << n.to_string() << ", skewness "
              << ctx.skewness() << "\n";

    // Phase 2: Factor Base
    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = params.rational_bound;
    fb_opts.algebraic_bound = params.algebraic_bound;
    fb_opts.special_q_bound = params.special_q_max;
    fb_opts.parallel = true;
    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

    // Phase 3: Sieve + Cofactor
    SieveParams sp;
    sp.rational_threshold = params.rational_threshold;
    sp.algebraic_threshold = params.algebraic_threshold;
    SieveRegion sr;
    sr.i_min = params.sieve_i_min;
    sr.i_max = params.sieve_i_max;
    sr.j_min = params.sieve_j_min;
    sr.j_max = params.sieve_j_max;

    CofactorizerConfig cc;
    cc.large_prime_bound = fb.params().large_prime_bound;
    cc.allow_1lp = true;
    cc.allow_2lp = true;
    Cofactorizer cofac(ctx, fb, cc);

    SpecialQRange sqr;
    sqr.min_q = params.special_q_min;
    sqr.max_q = params.special_q_max;
    SpecialQGenerator sqg(fb, sqr);

    CollectorConfig colc;
    colc.check_duplicates = true;
    RelationCollector collector(colc);

    size_t matrix_cols = util::saturating_size_add(
        util::saturating_size_add(fb.rational_count(), fb.sieve_algebraic_count()),
        params.target_excess);
    size_t initial_target = params.raw_relation_target(matrix_cols);
    size_t batch_target = initial_target;
    size_t sq_count = 0;
    LatticeSieve sieve(ctx, fb, sp);
    sieve.set_region(sr);

    // Adaptive sieve-filter-merge loop (as in the regression gate)
    std::vector<Relation> relations;
    bool lp_enabled = params.large_prime_bound > params.algebraic_bound;
    uint64_t next_reduction_generation = 1;
    size_t reduced_lp_columns = 0;
    constexpr int MAX_ROUNDS = 10;

    for (int round = 0; round < MAX_ROUNDS; ++round) {
        while (sqg.has_next() && collector.size() < batch_target &&
               sq_count < params.max_special_q) {
            auto sq = sqg.next();
            if (!sq)
                break;
            auto sres = sieve.sieve_special_q(*sq);

            const auto& cands = sres.candidates;
            size_t n_cands = cands.size();
            size_t n_threads = std::thread::hardware_concurrency();
            if (n_threads == 0)
                n_threads = 4;
            if (n_cands < 200)
                n_threads = 1;

            std::vector<std::vector<Relation>> thread_results(n_threads);
            std::atomic<size_t> global_found{collector.size()};
            std::atomic<size_t> next_chunk{0};
            constexpr size_t CHUNK_SIZE = 256;

            uint32_t cur_sq_q = sq->q;
            uint32_t cur_sq_r = sq->r;
            auto worker = [&](size_t tid) {
                Cofactorizer local_cofac(ctx, fb, cc);
                auto& local_rels = thread_results[tid];
                while (true) {
                    size_t start = next_chunk.fetch_add(CHUNK_SIZE, std::memory_order_relaxed);
                    if (start >= n_cands)
                        break;
                    if (global_found.load(std::memory_order_relaxed) >= batch_target)
                        break;
                    size_t end = std::min(start + CHUNK_SIZE, n_cands);
                    for (size_t ci = start; ci < end; ++ci) {
                        auto rel = local_cofac.verify(cands[ci], cur_sq_q, cur_sq_r);
                        if (rel) {
                            local_rels.push_back(std::move(*rel));
                            global_found.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            };

            if (n_threads <= 1) {
                worker(0);
            } else {
                std::vector<std::thread> threads;
                threads.reserve(n_threads);
                for (size_t t = 0; t < n_threads; ++t)
                    threads.emplace_back(worker, t);
                for (auto& t : threads)
                    t.join();
            }

            for (auto& tr : thread_results)
                for (auto& rel : tr)
                    collector.add(std::move(rel));
            ++sq_count;
        }

        std::cout << "  round " << round << ": special-q " << sq_count << ", raw relations "
                  << collector.size() << "\n";

        if (collector.size() < 10)
            break;

        // Reduce a stable prefix while keeping the collector appendable when a
        // later adaptive round needs more raw relations.
        RelationReductionConfig reduction_config;
        reduction_config.filter.remove_singletons = true;
        reduction_config.filter.max_passes = 10;
        reduction_config.large_primes_enabled = lp_enabled;
        reduction_config.merge_rounds = 10;
        reduction_config.strategy =
            lp_enabled ? ReductionStrategy::StandardV0 : ReductionStrategy::NoLargePrimes;
        auto reduction = RelationReductionEngine::reduce(
            RawRelationSnapshot(next_reduction_generation++, collector.snapshot_relations()),
            reduction_config);
        reduced_lp_columns = reduction.stats.output_lp_columns;
        relations = std::move(reduction).take_relations();

        if (has_effective_column_excess(relations.size(), matrix_cols, reduced_lp_columns))
            break;

        if (!sqg.has_next() || sq_count >= params.max_special_q)
            break;

        double merge_rate = (collector.size() > 0) ? static_cast<double>(relations.size()) /
                                                         static_cast<double>(collector.size())
                                                   : 0.01;
        size_t lp_cols_for_target = reduced_lp_columns;
        size_t effective_cols_for_target = effective_column_count(matrix_cols, lp_cols_for_target);
        size_t needed_raw = util::size_from_nonnegative_double_floor(
            static_cast<double>(util::saturating_size_product(effective_cols_for_target, 2)) /
            std::max(merge_rate, 0.001));
        batch_target =
            std::min(std::max(util::saturating_size_product(batch_target, 2), needed_raw),
                     util::saturating_size_product(initial_target, 5));
    }

    const size_t final_lp_columns = reduced_lp_columns;
    if (!has_effective_column_excess(relations.size(), matrix_cols, final_lp_columns)) {
        std::cerr << "  insufficient relations: rows=" << relations.size()
                  << " base_cols=" << matrix_cols << " lp_cols=" << final_lp_columns << "\n";
        return false;
    }

    // Trim to 1.3x the effective column count, counting large-prime columns.
    {
        size_t lp_cols_for_trim = reduced_lp_columns;
        size_t effective_cols = effective_column_count(matrix_cols, lp_cols_for_trim);
        size_t max_rels =
            util::size_from_nonnegative_double_floor(static_cast<double>(effective_cols) * 1.3);
        if (relations.size() > max_rels) {
            std::mt19937 rng(42);
            std::shuffle(relations.begin(), relations.end(), rng);
            relations.resize(max_rels);
        }
    }

    // Phase 4: Linear Algebra. The matrix must carry the constraints that a
    // non-square leading coefficient needs.
    MatrixBuilderConfig mc;
    mc.include_sign_column = true;
    mc.include_qc_columns = true;
    mc.include_class_group = false;
    mc.include_schirokauer = true;
    mc.num_qc_primes = params.num_qc_primes;
    mc.qc_prime_start = 100;
    mc.schirokauer_primes = {2};
    MatrixBuilder mb(mc);
    auto br = mb.build_with_qc(relations, fb, ctx);
    const auto& mapping = br.mapping;
    GNFS_TEST_CHECK(mapping.num_pair_parity_columns == 1);
    GNFS_TEST_CHECK(mapping.num_schirokauer_columns == fixture.expected_schirokauer_columns);
    if (even_leading) {
        // f loses its degree modulo 2, so build_with_qc selects the
        // compensated number of quadratic-character columns.
        GNFS_TEST_CHECK(mapping.schirokauer_primes.empty());
        GNFS_TEST_CHECK(mapping.num_qc_columns >= mc.num_qc_primes + size_t{8} * ctx.degree());
    } else {
        GNFS_TEST_CHECK(mapping.schirokauer_primes == std::vector<uint32_t>{2});
        GNFS_TEST_CHECK(mapping.num_qc_columns >= mc.num_qc_primes);
    }
    GNFS_TEST_CHECK(mapping.pair_parity_start() + 1 == mapping.total_columns());
    GNFS_TEST_CHECK(br.matrix.num_cols() == mapping.total_columns());
    GNFS_TEST_CHECK(br.qc_zero_legendre_count == 0);
    std::cout << "  matrix " << br.matrix.num_rows() << "x" << br.matrix.num_cols()
              << ", QC columns " << mapping.num_qc_columns << ", Schirokauer columns "
              << mapping.num_schirokauer_columns << ", pair-parity columns "
              << mapping.num_pair_parity_columns << ", merged rows "
              << std::count_if(relations.begin(), relations.end(),
                               [](const Relation& rel) { return rel.is_merged(); })
              << "\n";

    auto sge_result = linalg::SGE::preprocess(br.matrix);

    BlockLanczos solver;
    auto deps = solver.find_dependencies(sge_result.reduced_matrix);

    for (auto& dep : deps) {
        dep = sge_result.expand_dependency(dep);
    }

    std::cout << "  dependencies " << deps.size() << "\n";
    if (deps.empty()) {
        std::cerr << "  no dependencies found\n";
        return false;
    }

    // Phase 5: Square Root
    auto to_bv = [](const std::vector<bool>& v) {
        BitVector bv(v.size());
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i])
                bv.set(i);
        return bv;
    };

    const auto is_nontrivial = [&n](const Integer& factor) {
        return factor.compare(Integer(1)) > 0 && factor.compare(n) < 0;
    };

    size_t rejected_dependencies = 0;
    for (size_t di = 0; di < deps.size(); ++di) {
        // Every column of the final matrix must vanish, including the
        // pair-parity column: the dependency contains an even number of pairs.
        if (!is_null_vector(br.matrix, deps[di])) {
            ++rejected_dependencies;
            continue;
        }
        auto bv = to_bv(deps[di]);

        auto rat = compute_rational_sqrt(bv, relations, fb, n, ctx.m());
        if (!rat.success)
            continue;

        auto alg = compute_algebraic_sqrt(bv, relations, ctx);
        if (!alg.success)
            continue;

        for (int sign = 0; sign < 2; ++sign) {
            Integer y = (sign == 0) ? alg.value.clone() : [&]() {
                Integer neg = n.clone();
                neg -= alg.value;
                return neg;
            }();
            auto factors = extract_factors(rat.value, y, n);

            Integer f1, f2;
            bool found = false;
            if (is_nontrivial(factors.factor1)) {
                f1 = factors.factor1.clone();
                f2 = n.clone();
                f2 /= f1;
                found = true;
            } else if (is_nontrivial(factors.factor2)) {
                f1 = factors.factor2.clone();
                f2 = n.clone();
                f2 /= f1;
                found = true;
            }

            if (found) {
                Integer chk = f1.clone();
                chk *= f2;
                if (chk.compare(n) == 0 && is_nontrivial(f1) && is_nontrivial(f2)) {
                    // N is a semiprime, so a nontrivial factor is one of its primes.
                    const bool expected =
                        (f1.compare(expected_p) == 0 && f2.compare(expected_q) == 0) ||
                        (f1.compare(expected_q) == 0 && f2.compare(expected_p) == 0);
                    std::cout << "  dependency " << di << ": " << f1.to_string() << " x "
                              << f2.to_string() << (expected ? " (expected primes)" : "")
                              << ", rejected dependencies " << rejected_dependencies << "\n";
                    GNFS_TEST_CHECK(expected);
                    return true;
                }
            }
        }
    }

    std::cerr << "  all dependencies exhausted without a nontrivial factor\n";
    return false;
}

} // namespace

int main() {
    std::cout << "=== Non-monic matrix constraints end-to-end ===\n";

    int passed = 0;
    int failed = 0;
    for (const auto& fixture : kFixtures) {
        std::cout << "[fixture] " << fixture.label << "\n";
        const auto fixture_start = std::chrono::steady_clock::now();
        bool ok = false;
        std::string error;
        try {
            ok = factorize_with(fixture);
        } catch (const std::exception& e) {
            error = e.what();
        }
        const auto elapsed = std::chrono::steady_clock::now() - fixture_start;
        const double seconds = std::chrono::duration<double>(elapsed).count();
        if (ok) {
            ++passed;
            std::cout << "  PASS (" << seconds << "s)\n";
        } else {
            ++failed;
            std::cout << "  FAIL (" << seconds << "s)";
            if (!error.empty())
                std::cout << ": " << error;
            std::cout << "\n";
        }
    }

    std::cout << "=== Results: " << passed << " passed, " << failed << " failed ===\n";
    return failed == 0 ? 0 : 1;
}

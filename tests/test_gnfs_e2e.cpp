// test_gnfs_e2e.cpp - End-to-end GNFS factorization test
//
// This test demonstrates the complete GNFS pipeline:
// 1. Polynomial selection (Base-m method)
// 2. Factor base construction
// 3. Sieving with special-q
// 4. Cofactorization and relation collection
// 5. Matrix construction and linear algebra
// 6. Square root computation
// 7. Factor extraction

#include <gnfs/core/params.hpp>
#include <gnfs/polynomial/selector_dispatch.hpp>
#include <gnfs/factor_base/builder.hpp>
#include <gnfs/sieve/special_q.hpp>
#include <gnfs/sieve/lattice_sieve.hpp>
#include <gnfs/cofactor/cofactorizer.hpp>
#include <gnfs/relation/collector.hpp>
#include <gnfs/relation/filter.hpp>
#include <gnfs/linalg/matrix_builder.hpp>
#include <gnfs/linalg/sge.hpp>
#include <gnfs/linalg/block_lanczos.hpp>
#include <gnfs/sqrt/rational_sqrt.hpp>
#include <gnfs/sqrt/algebraic_sqrt.hpp>
#include <gnfs/util/process.hpp>
#include <gnfs/util/temp_path.hpp>

#include <cassert>
#include <chrono>
#include <cstdio>      // remove() for OOC artifact cleanup
#include <cstdlib>     // getenv for GNFS_OOC_RELATIONS
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace gnfs;
using namespace gnfs::core;
using namespace gnfs::polynomial;
using namespace gnfs::factor_base;
using namespace gnfs::sieve;
using namespace gnfs::cofactor;
using namespace gnfs::relation;
using namespace gnfs::linalg;
using namespace gnfs::sqrt;

// Helper wrapper for find_dependencies (BlockLanczos method)
inline std::vector<std::vector<bool>> find_dependencies(const SparseMatrix& mat, size_t max_deps = 64) {
    BlockLanczos solver;
    return solver.find_dependencies(mat, max_deps);
}

// Helper to verify a dependency: sum of selected rows should XOR to zero
inline bool verify_dependency(const SparseMatrix& mat, const std::vector<bool>& dep) {
    if (dep.size() != mat.num_rows()) return false;

    // Count set bits in each column for selected rows
    std::vector<size_t> col_count(mat.num_cols(), 0);
    for (size_t row = 0; row < mat.num_rows(); ++row) {
        if (row < dep.size() && dep[row]) {
            for (uint32_t col : mat.row(row).indices()) {
                col_count[col]++;
            }
        }
    }

    // All columns should have even count (XOR to zero)
    for (size_t c = 0; c < col_count.size(); ++c) {
        if (col_count[c] % 2 != 0) return false;
    }
    return true;
}

// Overload for BitVector
inline bool verify_dependency(const SparseMatrix& mat, const BitVector& dep) {
    std::vector<bool> vec(mat.num_rows(), false);
    for (size_t i = 0; i < mat.num_rows(); ++i) {
        if (dep.test(i)) vec[i] = true;
    }
    return verify_dependency(mat, vec);
}

// Convert std::vector<bool> to BitVector
inline BitVector to_bitvector(const std::vector<bool>& vec) {
    BitVector bv(vec.size());
    for (size_t i = 0; i < vec.size(); ++i) {
        if (vec[i]) bv.set(i);
    }
    return bv;
}

// Get popcount for std::vector<bool>
inline size_t popcount_vec(const std::vector<bool>& vec) {
    size_t count = 0;
    for (bool b : vec) if (b) ++count;
    return count;
}

// Test index for std::vector<bool>
inline bool test_vec(const std::vector<bool>& vec, size_t i) {
    return i < vec.size() && vec[i];
}

// Helper to print section headers
void print_section(const std::string& title) {
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "  " << title << "\n";
    std::cout << "========================================\n";
}

// Helper to print timing
class Timer {
public:
    Timer() : start_(std::chrono::high_resolution_clock::now()) {}

    double elapsed_ms() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(now - start_).count();
    }

    void reset() {
        start_ = std::chrono::high_resolution_clock::now();
    }

private:
    std::chrono::high_resolution_clock::time_point start_;
};

// Factorization result
struct FactorizationResult {
    Integer n;
    Integer factor1;
    Integer factor2;
    bool success = false;
    size_t relations_collected = 0;
    size_t dependencies_found = 0;
    double total_time_ms = 0;
};

// Main GNFS factorization function
FactorizationResult factor_gnfs(const Integer& n, bool verbose = true) {
    FactorizationResult result;
    result.n = n.clone();

    // Primality check: GNFS only works on composites
    if (mpz_probab_prime_p(n.get_mpz(), 25) > 0) {
        if (verbose) std::cerr << "N is prime (or probably prime). GNFS requires a composite input.\n";
        return result;
    }
    if (n <= Integer(1)) {
        if (verbose) std::cerr << "N must be > 1.\n";
        return result;
    }

    Timer total_timer;

    // ============================================================
    // Phase 1: Polynomial Selection
    // ============================================================
    if (verbose) print_section("Phase 1: Polynomial Selection");

    Timer phase_timer;

    // Determine polynomial degree based on input size
    size_t bits = n.bit_length();
    auto params = core::GNFSParams::compute(bits);

    uint32_t degree = params.degree;

    if (verbose) {
        std::cout << "N = " << n.to_string() << "\n";
        std::cout << "Bits: " << bits << "\n";
        std::cout << "Using polynomial degree: " << degree << "\n";
        std::cout << "\nGNFS Parameters (auto-computed for " << bits << "-bit N):\n";
        std::cout << "  Degree: " << params.degree << "\n";
        std::cout << "  Factor base bound: " << params.rational_bound << "\n";
        std::cout << "  Large prime bound: " << params.large_prime_bound << "\n";
        std::cout << "  Sieve region: " << (params.sieve_i_max - params.sieve_i_min + 1) << " x " << (params.sieve_j_max - params.sieve_j_min + 1) << "\n";
        std::cout << "  Special-Q range: [" << params.special_q_min << ", " << params.special_q_max << "]\n";
        std::cout << "  Max special-Q: " << params.max_special_q << "\n";
    }

    std::optional<PolynomialContext> ctx_opt;
    try {
        ctx_opt.emplace(SelectorDispatch::select(n, degree, verbose));
    } catch (const std::exception& e) {
        std::cerr << "Polynomial selection failed: " << e.what() << "\n";
        return result;
    }
    auto& ctx = *ctx_opt;

    if (verbose) {
        std::cout << "m = " << ctx.m().to_string() << "\n";
        std::cout << "Polynomial: ";
        for (uint32_t i = 0; i <= ctx.degree(); ++i) {
            if (i > 0) std::cout << " + ";
            std::cout << ctx.coeff(i).to_string() << "*x^" << i;
        }
        std::cout << "\n";
        std::cout << "Phase 1 time: " << phase_timer.elapsed_ms() << " ms\n";
    }

    // ============================================================
    // Phase 2: Factor Base Construction
    // ============================================================
    if (verbose) print_section("Phase 2: Factor Base Construction");
    phase_timer.reset();

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = params.rational_bound;
    fb_opts.algebraic_bound = params.algebraic_bound;
    fb_opts.special_q_bound = params.special_q_max;
    fb_opts.parallel = true;

    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

    if (verbose) {
        std::cout << "Rational primes: " << fb.rational_count() << "\n";
        std::cout << "Algebraic primes: " << fb.algebraic_count()
                  << " (sieve: " << fb.sieve_algebraic_count() << ")\n";
        std::cout << "Factor base bound: " << params.rational_bound << "\n";
        std::cout << "Phase 2 time: " << phase_timer.elapsed_ms() << " ms\n";
    }

    // ============================================================
    // Phase 3: Sieving and Relation Collection
    // ============================================================
    if (verbose) print_section("Phase 3: Sieving and Relation Collection");
    phase_timer.reset();

    // Configure sieve parameters
    SieveParams sieve_params;
    sieve_params.rational_threshold = params.rational_threshold;
    sieve_params.algebraic_threshold = params.algebraic_threshold;

    // Configure sieve region
    SieveRegion sieve_region;
    sieve_region.i_min = params.sieve_i_min;
    sieve_region.i_max = params.sieve_i_max;
    sieve_region.j_min = params.sieve_j_min;
    sieve_region.j_max = params.sieve_j_max;

    // Configure cofactorizer
    CofactorizerConfig cofac_config;
    cofac_config.large_prime_bound = fb.params().large_prime_bound;
    cofac_config.allow_1lp = true;
    cofac_config.allow_2lp = true;

    Cofactorizer cofactorizer(ctx, fb, cofac_config);

    // Configure special-q range
    SpecialQRange sq_range;
    sq_range.min_q = params.special_q_min;
    sq_range.max_q = params.special_q_max;

    SpecialQGenerator sq_gen(fb, sq_range);

    // Relation collector
    CollectorConfig coll_config;
    coll_config.check_duplicates = true;
    // OOC mode (ENV GNFS_OOC_RELATIONS=1) — stress test OOC path in e2e flow.
    // RAII cleanup struct ensures .reldata/.relidx removed on any return path.
    struct OOCCleanup {
        std::string path;
        ~OOCCleanup() {
            if (!path.empty()) {
                std::remove((path + ".reldata").c_str());
                std::remove((path + ".relidx").c_str());
            }
        }
    } ooc_cleanup;
    if (const char* env = std::getenv("GNFS_OOC_RELATIONS");
        env != nullptr && std::atoi(env) == 1) {
        coll_config.ooc_enabled = true;
        ooc_cleanup.path = gnfs::util::temp_path(
            "gnfs_e2e_ooc_" + std::to_string(gnfs::util::process_id()) +
            "_" + std::to_string(reinterpret_cast<uintptr_t>(&coll_config)));
        coll_config.ooc_base_path = ooc_cleanup.path;
        if (verbose) {
            std::cout << "[OOC] streaming relations to " << ooc_cleanup.path
                      << ".{reldata,relidx}\n";
        }
    }
    RelationCollector collector(coll_config);

    // Target: LP-aware — need enough raw relations to survive singleton filtering
    size_t matrix_cols = fb.rational_count() + fb.sieve_algebraic_count() + params.target_excess;
    size_t target_relations = params.raw_relation_target(matrix_cols);

    if (verbose) {
        std::cout << "Sieve region: i=[" << sieve_region.i_min << ", "
                  << sieve_region.i_max << "], j=[" << sieve_region.j_min
                  << ", " << sieve_region.j_max << "]\n";
        std::cout << "Special-Q range: [" << sq_range.min_q << ", " << sq_range.max_q << "]\n";
        std::cout << "Target relations: " << target_relations << "\n";
        std::cout << "\nSieving...\n";
    }

    size_t sq_count = 0;
    size_t candidates_total = 0;
    size_t max_sq = params.max_special_q;

    // Create lattice sieve (reused for each special-q)
    LatticeSieve sieve(ctx, fb, sieve_params);
    sieve.set_region(sieve_region);

    while (sq_gen.has_next() && collector.size() < target_relations && sq_count < max_sq) {
        auto sq = sq_gen.next();
        if (!sq) break;

        // Run sieve for this special-q
        auto sieve_result = sieve.sieve_special_q(*sq);
        candidates_total += sieve_result.candidates.size();

        // Cofactorize candidates
        for (const auto& cand : sieve_result.candidates) {
            auto rel_opt = cofactorizer.verify(cand);
            if (rel_opt) {
                collector.add(std::move(*rel_opt));
            }
        }

        ++sq_count;

        // Progress
        if (verbose && sq_count % params.progress_interval == 0) {
            std::cout << "  Special-Q #" << sq_count << ": " << collector.size()
                      << " relations\n";
        }
    }

    if (verbose) {
        std::cout << "\nSieving complete!\n";
        std::cout << "Special-Q processed: " << sq_count << "\n";
        std::cout << "Total candidates: " << candidates_total << "\n";
        std::cout << "Relations collected: " << collector.size() << "\n";

        auto stats = collector.stats();
        std::cout << "  Full relations: " << stats.full_relations << "\n";
        std::cout << "  Partial (1LP): " << stats.partial_1lp << "\n";
        std::cout << "  Partial (2LP): " << stats.partial_2lp << "\n";
        std::cout << "Phase 3 time: " << phase_timer.elapsed_ms() << " ms\n";
    }

    result.relations_collected = collector.size();

    if (collector.size() < 10) {
        std::cerr << "Not enough relations collected!\n";
        return result;
    }

    // ============================================================
    // Phase 4: Filtering
    // ============================================================
    if (verbose) print_section("Phase 4: Relation Filtering");
    phase_timer.reset();

    // Get relations and filter singletons
    auto relations = collector.get_relations();

    FilterConfig filter_config;
    filter_config.remove_singletons = true;
    filter_config.max_passes = 10;

    RelationFilter filter(filter_config);
    relations = filter.filter(std::move(relations));

    if (verbose) {
        auto& fstats = filter.stats();
        std::cout << "After filtering: " << relations.size() << " relations\n";
        std::cout << "Singletons removed: " << fstats.singletons_removed << "\n";
        std::cout << "Passes: " << fstats.passes << "\n";
    }

    // Merge partial 1LP relations sharing a large prime
    // Only when LP is genuinely enabled (LP > algebraic_bound)
    // For small N where LP ≈ FB, "partials" are just inert-prime artifacts
    if (params.large_prime_bound > params.algebraic_bound) {
        auto sep = separate_relations(std::move(relations));

        PartialRelationMerger::MergeStats mstats;
        auto merged = PartialRelationMerger::merge_all(
            std::move(sep.partial), 10, &mstats);

        if (verbose) {
            std::cout << "Full: " << sep.full.size()
                      << ", 1LP: " << mstats.input_1lp
                      << ", 2LP: " << mstats.input_2lp
                      << ", Merged: " << merged.size()
                      << " (w2=" << mstats.weight2_merges
                      << " sngl=" << mstats.singletons_removed
                      << " rnd=" << mstats.rounds << ")\n";
        }

        // Only keep full + merged — unmerged partials create singleton LP columns
        // in the matrix that waste space and cannot participate in any dependency
        relations = std::move(sep.full);
        relations.reserve(relations.size() + merged.size());
        relations.insert(relations.end(),
            std::make_move_iterator(merged.begin()),
            std::make_move_iterator(merged.end()));
    }

    if (verbose) {
        std::cout << "Total relations for matrix: " << relations.size() << "\n";
        std::cout << "Phase 4 time: " << phase_timer.elapsed_ms() << " ms\n";
    }

    if (relations.size() < 5) {
        std::cerr << "Not enough relations after filtering!\n";
        return result;
    }

    // ============================================================
    // Phase 5: Matrix Construction and Linear Algebra
    // ============================================================
    if (verbose) print_section("Phase 5: Linear Algebra");
    phase_timer.reset();

    // Build matrix with quadratic characters and class group characters
    MatrixBuilderConfig mb_config;
    mb_config.include_sign_column = true;
    mb_config.include_qc_columns = true;
    mb_config.include_class_group = false;  // Disabled: small N typically has class number 1, no benefit
    mb_config.include_schirokauer = true;   // Enable Schirokauer maps
    mb_config.num_qc_primes = 64;   // More QC primes - powers of 2 work well
    mb_config.qc_prime_start = 100; // Start with primes around 100
    mb_config.schirokauer_primes = {2};  // Only ℓ=2 works correctly with GF(2) matrix
    mb_config.verbose = verbose;    // Enable verbose output

    MatrixBuilder mb(mb_config);
    auto build_result = mb.build_with_qc(relations, fb, ctx);

    // std::cerr << "[E2E] Computing matrix stats...\n";
    auto matrix_stats = compute_matrix_stats(build_result.matrix);
    // std::cerr << "[E2E] Matrix stats done.\n";

    if (verbose) {
        // std::cerr << "[E2E] Printing matrix stats...\n";
        std::cout << "Matrix size: " << matrix_stats.num_rows << " x "
                  << matrix_stats.num_cols << "\n";
        std::cout << "Total weight: " << matrix_stats.total_weight << "\n";
        std::cout << "Avg row weight: " << std::fixed << std::setprecision(2)
                  << matrix_stats.avg_row_weight << "\n";
        std::cout << "Excess: " << (matrix_stats.has_excess() ?
                  std::to_string(matrix_stats.excess) : "NONE") << "\n";
        // std::cerr << "[E2E] Matrix stats printed.\n";
    }

    if (!matrix_stats.has_excess()) {
        std::cerr << "Not enough relations (no excess)!\n";
        return result;
    }

    // SGE preprocessing: eliminate weight-1/weight-2 columns to reduce matrix
    SGEConfig sge_config;
    sge_config.verbose = verbose;
    auto sge_result = SGE::preprocess(build_result.matrix, sge_config);

    if (verbose) {
        std::cout << "SGE: " << build_result.matrix.num_rows() << "×"
                  << build_result.matrix.num_cols()
                  << " → " << sge_result.reduced_matrix.num_rows() << "×"
                  << sge_result.reduced_matrix.num_cols()
                  << " (w1=" << sge_result.weight1_eliminated
                  << " w2=" << sge_result.weight2_merged << ")\n";
    }

    // Find dependencies on the reduced matrix
    std::cout << std::flush;
    auto dependencies = find_dependencies(sge_result.reduced_matrix);

    // Expand dependencies back to original matrix rows
    for (auto& dep : dependencies) {
        dep = sge_result.expand_dependency(dep);
    }
    std::cout << "Dependencies found: " << dependencies.size() << "\n" << std::flush;

    if (verbose) {
        // std::cerr << "[E2E] Printing dependencies found...\n";
        std::cout << "Dependencies found: " << dependencies.size() << "\n";
        std::cout << "Phase 5 time: " << phase_timer.elapsed_ms() << " ms\n";
        // std::cerr << "[E2E] Dependencies printed.\n";
    }

    result.dependencies_found = dependencies.size();

    if (dependencies.empty()) {
        std::cerr << "No dependencies found!\n";
        return result;
    }
    // std::cerr << "[E2E] Entering Phase 6...\n";

    // ============================================================
    // Phase 6: Square Root and Factor Extraction
    // ============================================================
    if (verbose) print_section("Phase 6: Square Root and Factor Extraction");
    phase_timer.reset();

    // Try each dependency
    // std::cerr << "[E2E] Starting dependency loop with " << dependencies.size() << " deps\n";
    for (size_t dep_idx = 0; dep_idx < dependencies.size(); ++dep_idx) {
        const auto& dep = dependencies[dep_idx];

        // std::cerr << "[E2E] Trying dependency " << dep_idx << "\n";

        if (verbose) {
            std::cout << "\nTrying dependency #" << (dep_idx + 1)
                      << " (size=" << popcount_vec(dep) << ")\n";
        }

        // Verify the dependency
        // std::cerr << "[E2E] Verifying dependency " << dep_idx << "...\n";
        if (!verify_dependency(build_result.matrix, dep)) {
            if (verbose) std::cout << "  Dependency verification failed, skipping\n";
            continue;
        }
        // Debug: // std::cerr << "[E2E] Dependency " << dep_idx << " verified\n";

        // Compute rational square root
        // Debug: // std::cerr << "[E2E] Computing rational sqrt...\n";
        auto rat_result = compute_rational_sqrt(to_bitvector(dep), relations, fb, n, ctx.m());
        // Debug: // std::cerr << "[E2E] Rational sqrt done: success=" << rat_result.success << "\n";

        if (!rat_result.success) {
            if (verbose) std::cout << "  Rational sqrt failed: " << rat_result.error << "\n";
            continue;
        }

        if (verbose) {
            std::cout << "  Rational sqrt (mod N): " << rat_result.value.to_string() << "\n";
            // Debug: // std::cerr << "[E2E] Computing actual product verification...\n";

            // Verify: compute actual product of (a - bm) mod N (GNFS convention)
            Integer actual_product(1);
            for (size_t i = 0; i < relations.size(); ++i) {
                if (!test_vec(dep, i)) continue;
                const auto& rel = relations[i];
                Integer term = Integer(rel.ab().a);
                Integer bm = ctx.m().clone();
                bm *= Integer(static_cast<int64_t>(rel.ab().b));
                term -= bm;
                term %= n;
                if (term.is_negative()) term += n;
                actual_product *= term;
                actual_product %= n;
            }
            std::cout << "  Actual product mod N = " << actual_product.to_string() << "\n";
            // Debug: // std::cerr << "[E2E] Actual product verification done.\n";
        }

        // Compute algebraic square root
        // Debug: // std::cerr << "[E2E] Computing algebraic sqrt...\n";
        auto alg_result = compute_algebraic_sqrt(to_bitvector(dep), relations, ctx);
        // Debug: // std::cerr << "[E2E] Algebraic sqrt done: success=" << alg_result.success << "\n";

        if (!alg_result.success) {
            if (verbose) std::cout << "  Algebraic sqrt failed: " << alg_result.error << "\n";
            // Try with just the rational sqrt anyway
            alg_result.value = Integer(static_cast<int64_t>(1));
        }

        if (verbose) {
            std::cout << "  Algebraic sqrt (mod N): " << alg_result.value.to_string() << "\n";

            // Also compute the algebraic product and evaluate at m
            // This should equal the rational product if the number field operations are correct
            sqrt::NumberField nf(ctx);
            sqrt::NumberFieldElement alg_product = nf.one();
            for (size_t i = 0; i < relations.size(); ++i) {
                if (!test_vec(dep, i)) continue;
                const auto& rel = relations[i];
                auto factor = nf.from_ab(rel.ab().a, rel.ab().b);
                alg_product = nf.multiply_mod_n(alg_product, factor);
            }
            Integer alg_product_at_m = nf.evaluate_at_m_mod_n(alg_product);
            std::cout << "  Alg product(m) mod N = " << alg_product_at_m.to_string() << "\n";

            // Verify: compute X^2 mod N and Y^2 mod N
            Integer x_sq = rat_result.value.clone();
            x_sq *= rat_result.value;
            x_sq %= n;

            Integer y_sq = alg_result.value.clone();
            y_sq *= alg_result.value;
            y_sq %= n;

            std::cout << "  X^2 mod N = " << x_sq.to_string() << "\n";
            std::cout << "  Y^2 mod N = " << y_sq.to_string() << "\n";
            if (x_sq.compare(y_sq) == 0) {
                std::cout << "  [VERIFIED: X^2 = Y^2 mod N]\n";
            } else {
                std::cout << "  [WARNING: X^2 != Y^2 mod N - dependency may be invalid]\n";
            }
        }

        // Try both Y and -Y (N-Y) since there are multiple square roots mod N
        Integer alg_value = alg_result.value.clone();
        Integer alg_value_neg = n.clone();
        alg_value_neg -= alg_result.value;

        // Try with Y
        auto factors = extract_factors(rat_result.value, alg_value, n);

        if (verbose) {
            std::cout << "  gcd(X-Y, N) = " << factors.factor1.to_string() << "\n";
            std::cout << "  gcd(X+Y, N) = " << factors.factor2.to_string() << "\n";
        }

        // If first attempt gives trivial factors, try with -Y
        if (!factors.is_nontrivial ||
            (factors.factor1.fits_uint64() && factors.factor1.to_uint64() == 1 &&
             factors.factor2.compare(n) == 0)) {

            auto factors_neg = extract_factors(rat_result.value, alg_value_neg, n);
            if (verbose) {
                std::cout << "  Trying -Y: gcd(X-(-Y), N) = " << factors_neg.factor1.to_string() << "\n";
                std::cout << "  Trying -Y: gcd(X+(-Y), N) = " << factors_neg.factor2.to_string() << "\n";
            }
            if (factors_neg.is_nontrivial) {
                factors = std::move(factors_neg);
            }
        }

        // Check if we found non-trivial factors
        // A factor is non-trivial if it's not 1 and not N
        auto is_nontrivial_factor = [&n](const Integer& f) -> bool {
            if (f.fits_uint64() && f.to_uint64() == 1) return false;
            if (f.compare(n) == 0) return false;
            return true;
        };

        bool found_factor = false;
        Integer f1, f2;

        // Check factor1 = gcd(X-Y, N)
        if (is_nontrivial_factor(factors.factor1)) {
            f1 = factors.factor1.clone();
            f2 = n.clone();
            f2 /= f1;
            found_factor = true;
        }
        // Check factor2 = gcd(X+Y, N)
        else if (is_nontrivial_factor(factors.factor2)) {
            f1 = factors.factor2.clone();
            f2 = n.clone();
            f2 /= f1;
            found_factor = true;
        }

        if (found_factor) {
            // Verify: f1 * f2 should equal n
            Integer check = f1.clone();
            check *= f2;

            if (check.compare(n) == 0 && is_nontrivial_factor(f1) && is_nontrivial_factor(f2)) {
                result.factor1 = std::move(f1);
                result.factor2 = std::move(f2);
                result.success = true;

                if (verbose) {
                    std::cout << "\n*** FACTORIZATION SUCCESSFUL! ***\n";
                    std::cout << "Factor 1: " << result.factor1.to_string() << "\n";
                    std::cout << "Factor 2: " << result.factor2.to_string() << "\n";
                }
                break;
            }
        }
    }

    // If no single dependency worked, try XOR combinations of pairs
    // Two non-squares might combine to form a square
    if (!result.success && dependencies.size() >= 2) {
        if (verbose) {
            std::cout << "\nTrying XOR combinations of dependency pairs...\n";
        }

        for (size_t i = 0; i < std::min(dependencies.size(), size_t(20)) && !result.success; ++i) {
            for (size_t j = i + 1; j < std::min(dependencies.size(), size_t(20)) && !result.success; ++j) {
                // XOR the two dependencies
                BitVector combined = to_bitvector(dependencies[i]);
                combined.xor_with(to_bitvector(dependencies[j]));

                // Skip if the combination is empty or too small
                if (combined.popcount() < 2) continue;

                // Verify the combined dependency
                if (!verify_dependency(build_result.matrix, combined)) continue;

                // Compute sqrt
                auto rat_result = compute_rational_sqrt(combined, relations, fb, n, ctx.m());
                if (!rat_result.success) continue;

                auto alg_result = compute_algebraic_sqrt(combined, relations, ctx);
                if (!alg_result.success) {
                    alg_result.value = Integer(static_cast<int64_t>(1));
                }

                // Try both Y and -Y
                Integer alg_value = alg_result.value.clone();
                Integer alg_value_neg = n.clone();
                alg_value_neg -= alg_result.value;

                auto factors = extract_factors(rat_result.value, alg_value, n);

                auto is_nontrivial_factor = [&n](const Integer& f) -> bool {
                    if (f.fits_uint64() && f.to_uint64() == 1) return false;
                    if (f.compare(n) == 0) return false;
                    return true;
                };

                // Check factor1
                if (is_nontrivial_factor(factors.factor1)) {
                    Integer f1 = factors.factor1.clone();
                    Integer f2 = n.clone();
                    f2 /= f1;

                    Integer check = f1.clone();
                    check *= f2;

                    if (check.compare(n) == 0 && is_nontrivial_factor(f1) && is_nontrivial_factor(f2)) {
                        result.factor1 = std::move(f1);
                        result.factor2 = std::move(f2);
                        result.success = true;

                        if (verbose) {
                            std::cout << "\n*** FACTORIZATION SUCCESSFUL (combination "
                                      << i+1 << " XOR " << j+1 << ")! ***\n";
                            std::cout << "Factor 1: " << result.factor1.to_string() << "\n";
                            std::cout << "Factor 2: " << result.factor2.to_string() << "\n";
                        }
                        break;
                    }
                }
                // Check factor2
                else if (is_nontrivial_factor(factors.factor2)) {
                    Integer f1 = factors.factor2.clone();
                    Integer f2 = n.clone();
                    f2 /= f1;

                    Integer check = f1.clone();
                    check *= f2;

                    if (check.compare(n) == 0 && is_nontrivial_factor(f1) && is_nontrivial_factor(f2)) {
                        result.factor1 = std::move(f1);
                        result.factor2 = std::move(f2);
                        result.success = true;

                        if (verbose) {
                            std::cout << "\n*** FACTORIZATION SUCCESSFUL (combination "
                                      << i+1 << " XOR " << j+1 << ")! ***\n";
                            std::cout << "Factor 1: " << result.factor1.to_string() << "\n";
                            std::cout << "Factor 2: " << result.factor2.to_string() << "\n";
                        }
                        break;
                    }
                }

                // Try with -Y
                if (!result.success) {
                    auto factors_neg = extract_factors(rat_result.value, alg_value_neg, n);

                    if (is_nontrivial_factor(factors_neg.factor1)) {
                        Integer f1 = factors_neg.factor1.clone();
                        Integer f2 = n.clone();
                        f2 /= f1;

                        Integer check = f1.clone();
                        check *= f2;

                        if (check.compare(n) == 0 && is_nontrivial_factor(f1) && is_nontrivial_factor(f2)) {
                            result.factor1 = std::move(f1);
                            result.factor2 = std::move(f2);
                            result.success = true;

                            if (verbose) {
                                std::cout << "\n*** FACTORIZATION SUCCESSFUL (combination "
                                          << i+1 << " XOR " << j+1 << " with -Y)! ***\n";
                                std::cout << "Factor 1: " << result.factor1.to_string() << "\n";
                                std::cout << "Factor 2: " << result.factor2.to_string() << "\n";
                            }
                            break;
                        }
                    }
                    else if (is_nontrivial_factor(factors_neg.factor2)) {
                        Integer f1 = factors_neg.factor2.clone();
                        Integer f2 = n.clone();
                        f2 /= f1;

                        Integer check = f1.clone();
                        check *= f2;

                        if (check.compare(n) == 0 && is_nontrivial_factor(f1) && is_nontrivial_factor(f2)) {
                            result.factor1 = std::move(f1);
                            result.factor2 = std::move(f2);
                            result.success = true;

                            if (verbose) {
                                std::cout << "\n*** FACTORIZATION SUCCESSFUL (combination "
                                          << i+1 << " XOR " << j+1 << " with -Y)! ***\n";
                                std::cout << "Factor 1: " << result.factor1.to_string() << "\n";
                                std::cout << "Factor 2: " << result.factor2.to_string() << "\n";
                            }
                            break;
                        }
                    }
                }
            }
        }
    }

    if (verbose) {
        std::cout << "Phase 6 time: " << phase_timer.elapsed_ms() << " ms\n";
    }

    result.total_time_ms = total_timer.elapsed_ms();

    return result;
}

// Test with a known semiprime
bool test_factor_small_semiprime() {
    print_section("Test: Factor Small Semiprime");

    // N = 143 = 11 * 13
    Integer n(143);

    std::cout << "Factoring N = 143 = 11 * 13\n";

    auto result = factor_gnfs(n, true);

    bool ok = false;
    if (result.success) {
        std::cout << "\n=== VERIFICATION ===\n";
        std::cout << "N = " << result.n.to_string() << "\n";
        std::cout << "p = " << result.factor1.to_string() << "\n";
        std::cout << "q = " << result.factor2.to_string() << "\n";

        // Verify: p * q should equal n
        Integer product = result.factor1.clone();
        product *= result.factor2;
        ok = (product.compare(result.n) == 0);

        std::cout << "p * q = " << product.to_string() << "\n";
        std::cout << "Verified: " << (ok ? "YES" : "NO") << "\n";
    } else {
        std::cout << "\nFactorization did not complete successfully.\n";
        std::cout << "Relations collected: " << result.relations_collected << "\n";
        std::cout << "Dependencies found: " << result.dependencies_found << "\n";
    }

    std::cout << "\nTotal time: " << result.total_time_ms << " ms\n";
    return ok;
}

// Test with a 4-digit semiprime
bool test_factor_4digit_semiprime() {
    print_section("Test: Factor 4-Digit Semiprime");

    // N = 9991 = 97 * 103
    Integer n(9991);

    std::cout << "Factoring N = 9991 = 97 * 103\n";

    auto result = factor_gnfs(n, true);

    bool ok = false;
    if (result.success) {
        std::cout << "\n=== VERIFICATION ===\n";
        std::cout << "N = " << result.n.to_string() << "\n";
        std::cout << "p = " << result.factor1.to_string() << "\n";
        std::cout << "q = " << result.factor2.to_string() << "\n";

        // Verify: p * q should equal n
        Integer product = result.factor1.clone();
        product *= result.factor2;
        ok = (product.compare(result.n) == 0);

        std::cout << "p * q = " << product.to_string() << "\n";
        std::cout << "Verified: " << (ok ? "YES" : "NO") << "\n";
    } else {
        std::cout << "\nFactorization did not complete successfully.\n";
        std::cout << "Relations collected: " << result.relations_collected << "\n";
        std::cout << "Dependencies found: " << result.dependencies_found << "\n";
    }

    std::cout << "\nTotal time: " << result.total_time_ms << " ms\n";
    return ok;
}

// Test with a 5-digit semiprime
bool test_factor_5digit_semiprime() {
    print_section("Test: Factor 5-Digit Semiprime");

    // N = 10403 = 101 * 103
    Integer n(10403);

    std::cout << "Factoring N = 10403 = 101 * 103\n";

    auto result = factor_gnfs(n, true);

    bool ok = false;
    if (result.success) {
        std::cout << "\n=== VERIFICATION ===\n";
        std::cout << "N = " << result.n.to_string() << "\n";
        std::cout << "p = " << result.factor1.to_string() << "\n";
        std::cout << "q = " << result.factor2.to_string() << "\n";

        // Verify: p * q should equal n
        Integer product = result.factor1.clone();
        product *= result.factor2;
        ok = (product.compare(result.n) == 0);

        std::cout << "p * q = " << product.to_string() << "\n";
        std::cout << "Verified: " << (ok ? "YES" : "NO") << "\n";
    } else {
        std::cout << "\nFactorization did not complete successfully.\n";
        std::cout << "Relations collected: " << result.relations_collected << "\n";
        std::cout << "Dependencies found: " << result.dependencies_found << "\n";
    }

    std::cout << "\nTotal time: " << result.total_time_ms << " ms\n";
    return ok;
}

// Test with 10800 = 2^4 * 3^3 * 5^2 (not a semiprime)
bool test_factor_10800() {
    print_section("Test: Factor 10800");

    // N = 10800 = 2^4 * 3^3 * 5^2
    Integer n(10800);

    std::cout << "Factoring N = 10800 = 2^4 * 3^3 * 5^2\n";
    std::cout << "(Note: This is NOT a semiprime - has multiple small factors)\n";

    auto result = factor_gnfs(n, true);

    bool ok = false;
    if (result.success) {
        std::cout << "\n=== VERIFICATION ===\n";
        std::cout << "N = " << result.n.to_string() << "\n";
        std::cout << "p = " << result.factor1.to_string() << "\n";
        std::cout << "q = " << result.factor2.to_string() << "\n";

        // Verify: p * q should equal n
        Integer product = result.factor1.clone();
        product *= result.factor2;
        ok = (product.compare(result.n) == 0);

        std::cout << "p * q = " << product.to_string() << "\n";
        std::cout << "Verified: " << (ok ? "YES" : "NO") << "\n";
    } else {
        std::cout << "\nFactorization did not complete successfully.\n";
        std::cout << "Relations collected: " << result.relations_collected << "\n";
        std::cout << "Dependencies found: " << result.dependencies_found << "\n";
    }

    std::cout << "\nTotal time: " << result.total_time_ms << " ms\n";
    return ok;
}

// Test with a 6-digit semiprime
bool test_factor_6digit_semiprime() {
    print_section("Test: Factor 6-Digit Semiprime");

    // N = 96091 = 307 * 313
    Integer n(96091);

    std::cout << "Factoring N = 96091 = 307 * 313\n";

    auto result = factor_gnfs(n, true);

    bool ok = false;
    if (result.success) {
        std::cout << "\n=== VERIFICATION ===\n";
        std::cout << "N = " << result.n.to_string() << "\n";
        std::cout << "p = " << result.factor1.to_string() << "\n";
        std::cout << "q = " << result.factor2.to_string() << "\n";

        // Verify: p * q should equal n
        Integer product = result.factor1.clone();
        product *= result.factor2;
        ok = (product.compare(result.n) == 0);

        std::cout << "p * q = " << product.to_string() << "\n";
        std::cout << "Verified: " << (ok ? "YES" : "NO") << "\n";
    } else {
        std::cout << "\nFactorization did not complete successfully.\n";
        std::cout << "Relations collected: " << result.relations_collected << "\n";
        std::cout << "Dependencies found: " << result.dependencies_found << "\n";
    }

    std::cout << "\nTotal time: " << result.total_time_ms << " ms\n";
    return ok;
}

// Test with a larger semiprime
void test_factor_medium_semiprime() {
    print_section("Test: Factor Medium Semiprime");

    // N = 10007 * 10009 = 100160063 (~27 bits)
    Integer n("100160063");

    std::cout << "Factoring N = 100160063 = 10007 * 10009\n";

    auto result = factor_gnfs(n, true);

    if (result.success) {
        std::cout << "\n=== VERIFICATION ===\n";
        std::cout << "N = " << result.n.to_string() << "\n";
        std::cout << "p = " << result.factor1.to_string() << "\n";
        std::cout << "q = " << result.factor2.to_string() << "\n";
    } else {
        std::cout << "\nFactorization did not complete successfully.\n";
        std::cout << "(This is expected - medium numbers need more sieving)\n";
    }

    std::cout << "\nTotal time: " << result.total_time_ms << " ms\n";
}

// Demonstrate the complete pipeline even if factorization doesn't succeed
void test_pipeline_demonstration() {
    print_section("Test: Pipeline Demonstration");

    // Use a 13-digit semiprime
    // N = 1000003 * 1000033 = 1000036000099
    Integer n("1000036000099");

    std::cout << "Demonstrating full GNFS pipeline on N = 1000036000099\n";
    std::cout << "(Expected factors: 1000003 and 1000033)\n\n";

    auto result = factor_gnfs(n, true);

    std::cout << "\n=== PIPELINE SUMMARY ===\n";
    std::cout << "N = " << result.n.to_string() << "\n";
    std::cout << "Relations collected: " << result.relations_collected << "\n";
    std::cout << "Dependencies found: " << result.dependencies_found << "\n";
    std::cout << "Factorization success: " << (result.success ? "YES" : "NO") << "\n";

    if (result.success) {
        std::cout << "Factor 1: " << result.factor1.to_string() << "\n";
        std::cout << "Factor 2: " << result.factor2.to_string() << "\n";
    }

    std::cout << "Total time: " << result.total_time_ms << " ms\n";
}

int main() {
    std::cout << "================================================================\n";
    std::cout << "       GNFS End-to-End Factorization Test\n";
    std::cout << "================================================================\n";
    std::cout << "\nThis test demonstrates the complete General Number Field Sieve\n";
    std::cout << "pipeline for integer factorization.\n";

    int pass = 0, fail = 0;
    auto run = [&](auto fn, const char* name) {
        bool ok = fn();
        if (ok) { ++pass; } else { ++fail; std::cout << "  *** FAILED: " << name << "\n"; }
    };

    run(test_factor_10800, "factor_10800");
    run(test_factor_small_semiprime, "factor_small_semiprime");
    run(test_factor_4digit_semiprime, "factor_4digit_semiprime");
    run(test_factor_5digit_semiprime, "factor_5digit_semiprime");
    run(test_factor_6digit_semiprime, "factor_6digit_semiprime");

    std::cout << "\n================================================================\n";
    std::cout << "  Results: " << pass << " passed, " << fail << " failed\n";
    std::cout << "================================================================\n";

    return (fail > 0) ? 1 : 0;
}

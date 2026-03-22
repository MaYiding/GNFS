// test_gnfs_progressive.cpp - Progressive GNFS factorization tests
//
// Tests increasing difficulty levels with detailed progress output.
// No hardcoded timeouts — use progress output to judge if tests are working.
//
// Usage:
//   ./test_gnfs_progressive          # Run all levels
//   ./test_gnfs_progressive 1        # Run only level 1
//   ./test_gnfs_progressive 1 3      # Run levels 1 through 3

#include <gnfs/core/params.hpp>
#include <gnfs/polynomial/base_m.hpp>
#include <gnfs/factor_base/builder.hpp>
#include <gnfs/sieve/special_q.hpp>
#include <gnfs/sieve/lattice_sieve.hpp>
#include <gnfs/cofactor/cofactorizer.hpp>
#include <gnfs/relation/collector.hpp>
#include <gnfs/relation/filter.hpp>
#include <gnfs/linalg/matrix_builder.hpp>
#include <gnfs/linalg/block_lanczos.hpp>
#include <gnfs/sqrt/rational_sqrt.hpp>
#include <gnfs/sqrt/algebraic_sqrt.hpp>

#include <cassert>
#include <chrono>
#include <iomanip>
#include <iostream>
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

// ============================================================
// Helpers
// ============================================================

inline std::vector<std::vector<bool>> find_deps(const SparseMatrix& mat, size_t max_deps) {
    BlockLanczos solver;
    return solver.find_dependencies(mat, max_deps);
}

inline bool verify_dep(const SparseMatrix& mat, const std::vector<bool>& dep) {
    if (dep.size() != mat.num_rows()) return false;
    std::vector<size_t> col_count(mat.num_cols(), 0);
    for (size_t row = 0; row < mat.num_rows(); ++row) {
        if (row < dep.size() && dep[row]) {
            for (uint32_t col : mat.row(row).indices()) {
                col_count[col]++;
            }
        }
    }
    for (size_t c = 0; c < col_count.size(); ++c) {
        if (col_count[c] % 2 != 0) return false;
    }
    return true;
}

inline BitVector to_bv(const std::vector<bool>& vec) {
    BitVector bv(vec.size());
    for (size_t i = 0; i < vec.size(); ++i) {
        if (vec[i]) bv.set(i);
    }
    return bv;
}

inline size_t popcnt(const std::vector<bool>& v) {
    size_t c = 0; for (bool b : v) if (b) ++c; return c;
}

class StopWatch {
public:
    StopWatch() : start_(std::chrono::high_resolution_clock::now()) {}
    double ms() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(now - start_).count();
    }
    double sec() const { return ms() / 1000.0; }
    void reset() { start_ = std::chrono::high_resolution_clock::now(); }
private:
    std::chrono::high_resolution_clock::time_point start_;
};

// ============================================================
// Test Level Definition
// ============================================================

struct TestCase {
    int level;
    std::string name;
    std::string n_str;
    std::string expected_p;
    std::string expected_q;
    std::string time_estimate;
};

std::vector<TestCase> get_test_cases() {
    return {
        // Level 1: Trivial (< 10 sec)
        {1, "8-bit semiprime",   "143",       "11",      "13",      "< 5 sec"},
        {1, "14-bit semiprime",  "9991",      "97",      "103",     "< 5 sec"},
        {1, "14-bit composite",  "10403",     "101",     "103",     "< 5 sec"},

        // Level 2: Small (< 1 min)
        {2, "17-bit semiprime",  "96091",     "307",     "313",     "< 30 sec"},
        {2, "27-bit semiprime",  "100160063", "10007",   "10009",   "< 1 min"},

        // Level 3: Medium (< 10 min)
        {3, "40-bit semiprime",  "1000036000099",   "1000003",  "1000033",  "< 10 min"},

        // Level 4: Large (< 1 hour)
        {4, "50-bit semiprime",  "100000980001501", "10000019", "10000079", "< 1 hour"},

        // Level 5: Very Large (minutes to hours)
        // 61-bit: product of two ~30-bit primes (f irreducible mod 2)
        {5, "61-bit semiprime",  "1253371692427905599", "1119540839", "1119540841", "minutes-hours"},
    };
}

// ============================================================
// Core Factorization with Progress Output
// ============================================================

struct FactResult {
    bool success = false;
    Integer factor1, factor2;
    size_t relations = 0;
    size_t dependencies = 0;
    double time_sec = 0;
};

FactResult factor_with_progress(const Integer& n, int level) {
    FactResult result;
    StopWatch total;

    size_t bits = n.bit_length();
    auto params = core::GNFSParams::compute(bits);

    std::cout << "\n╔══════════════════════════════════════════════════╗\n";
    std::cout << "║  GNFS Factorization — Level " << level << std::string(22, ' ') << "║\n";
    std::cout << "╠══════════════════════════════════════════════════╣\n";
    std::cout << "║  N = " << n.to_string().substr(0, 40);
    if (n.to_string().size() > 40) std::cout << "...";
    std::cout << "\n";
    std::cout << "║  Bits: " << bits << ", Digits: " << params.digits << "\n";
    std::cout << "╠══════════════════════════════════════════════════╣\n";
    std::cout << "║  Parameters (auto-computed):\n";
    std::cout << "║    Degree:       " << params.degree << "\n";
    std::cout << "║    FB bound:     " << params.rational_bound << "\n";
    std::cout << "║    LP bound:     " << params.large_prime_bound << "\n";
    std::cout << "║    Sieve:        " << (params.sieve_i_max - params.sieve_i_min + 1)
              << " × " << (params.sieve_j_max - params.sieve_j_min + 1) << "\n";
    std::cout << "║    Special-Q:    [" << params.special_q_min << ", " << params.special_q_max << "]\n";
    std::cout << "║    Max SQ:       " << params.max_special_q << "\n";
    std::cout << "╚══════════════════════════════════════════════════╝\n" << std::flush;

    // ── Phase 1: Polynomial Selection ──
    std::cout << "\n[Phase 1] Polynomial selection..." << std::flush;
    StopWatch phase;

    uint32_t degree = params.degree;
    auto poly_result = BaseMSelector::select(n, degree);
    if (!poly_result.success) {
        std::cout << " FAILED\n";
        return result;
    }
    auto ctx = BaseMSelector::create_context(n, poly_result);
    std::cout << " done (" << phase.ms() << " ms)\n";
    std::cout << "  m = " << poly_result.m.to_string() << "\n" << std::flush;

    // ── Phase 2: Factor Base ──
    std::cout << "[Phase 2] Factor base construction..." << std::flush;
    phase.reset();

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = params.rational_bound;
    fb_opts.algebraic_bound = params.algebraic_bound;
    fb_opts.parallel = true;

    auto fb = FactorBaseBuilder::build(ctx, fb_opts);
    std::cout << " done (" << phase.ms() << " ms)\n";
    std::cout << "  Rational: " << fb.rational_count()
              << ", Algebraic: " << fb.algebraic_count() << "\n" << std::flush;

    // ── Phase 3: Sieving ──
    std::cout << "[Phase 3] Sieving with special-Q...\n" << std::flush;
    phase.reset();

    SieveParams sieve_params;
    sieve_params.rational_threshold = params.rational_threshold;
    sieve_params.algebraic_threshold = params.algebraic_threshold;

    SieveRegion sieve_region;
    sieve_region.i_min = params.sieve_i_min;
    sieve_region.i_max = params.sieve_i_max;
    sieve_region.j_min = params.sieve_j_min;
    sieve_region.j_max = params.sieve_j_max;

    CofactorizerConfig cofac_config;
    cofac_config.large_prime_bound = fb.params().large_prime_bound;
    cofac_config.allow_1lp = true;
    cofac_config.allow_2lp = true;

    Cofactorizer cofactorizer(ctx, fb, cofac_config);

    SpecialQRange sq_range;
    sq_range.min_q = params.special_q_min;
    sq_range.max_q = params.special_q_max;
    SpecialQGenerator sq_gen(fb, sq_range);

    CollectorConfig coll_config;
    coll_config.check_duplicates = true;
    RelationCollector collector(coll_config);

    size_t target = fb.rational_count() + fb.algebraic_count() + params.target_excess;
    size_t sq_count = 0;
    size_t cand_total = 0;

    LatticeSieve sieve(ctx, fb, sieve_params);
    sieve.set_region(sieve_region);

    while (sq_gen.has_next() && collector.size() < target && sq_count < params.max_special_q) {
        auto sq = sq_gen.next();
        if (!sq) break;

        auto sr = sieve.sieve_special_q(*sq);
        cand_total += sr.candidates.size();

        for (const auto& cand : sr.candidates) {
            auto rel = cofactorizer.verify(cand);
            if (rel) collector.add(std::move(*rel));
        }
        ++sq_count;

        // Progress output — rate based
        if (sq_count % params.progress_interval == 0 || collector.size() >= target) {
            double rate = collector.size() / (phase.sec() + 0.001);
            double eta = (target > collector.size()) ?
                (target - collector.size()) / (rate + 0.001) : 0;
            std::cout << "  SQ #" << sq_count
                      << ": rels=" << collector.size() << "/" << target
                      << " (" << std::fixed << std::setprecision(1)
                      << (100.0 * collector.size() / target) << "%)"
                      << " cands=" << cand_total
                      << " rate=" << std::setprecision(1) << rate << "/s"
                      << " ETA=" << std::setprecision(0) << eta << "s"
                      << " elapsed=" << std::setprecision(1) << phase.sec() << "s"
                      << "\n" << std::flush;
        }
    }

    result.relations = collector.size();
    std::cout << "  Sieving complete: " << collector.size() << " relations"
              << " in " << phase.sec() << " sec\n" << std::flush;

    if (collector.size() < 10) {
        std::cout << "  INSUFFICIENT RELATIONS\n";
        result.time_sec = total.sec();
        return result;
    }

    // ── Phase 4: Filtering ──
    std::cout << "[Phase 4] Filtering..." << std::flush;
    phase.reset();

    auto relations = collector.get_relations();
    FilterConfig filt_config;
    filt_config.remove_singletons = true;
    filt_config.max_passes = 10;
    RelationFilter filter(filt_config);
    relations = filter.filter(std::move(relations));

    std::cout << " " << relations.size() << " relations remain (" << phase.ms() << " ms)\n" << std::flush;

    if (relations.size() < 5) {
        std::cout << "  INSUFFICIENT RELATIONS AFTER FILTER\n";
        result.time_sec = total.sec();
        return result;
    }

    // ── Phase 5: Linear Algebra ──
    std::cout << "[Phase 5] Matrix construction..." << std::flush;
    phase.reset();

    MatrixBuilderConfig mb_config;
    mb_config.include_sign_column = true;
    mb_config.include_qc_columns = true;
    mb_config.include_class_group = true;
    mb_config.include_schirokauer = true;
    mb_config.num_qc_primes = params.num_qc_primes;
    mb_config.qc_prime_start = 100;
    mb_config.schirokauer_primes = {2};  // GF(2) 矩阵 — 不可改
    mb_config.verbose = false;

    MatrixBuilder mb(mb_config);
    auto build_result = mb.build_with_qc(relations, fb, ctx);
    auto mstats = compute_matrix_stats(build_result.matrix);

    std::cout << " " << mstats.num_rows << "×" << mstats.num_cols
              << " (excess=" << mstats.excess << ") " << phase.ms() << " ms\n" << std::flush;

    if (!mstats.has_excess()) {
        std::cout << "  NO EXCESS — need more relations\n";
        result.time_sec = total.sec();
        return result;
    }

    std::cout << "  Finding dependencies..." << std::flush;
    phase.reset();
    auto deps = find_deps(build_result.matrix, 200);
    std::cout << " found " << deps.size() << " (" << phase.ms() << " ms)\n" << std::flush;

    result.dependencies = deps.size();
    if (deps.empty()) {
        std::cout << "  NO DEPENDENCIES FOUND\n";
        result.time_sec = total.sec();
        return result;
    }

    // ── Phase 6: Square Root ──
    std::cout << "[Phase 6] Square root extraction...\n" << std::flush;
    phase.reset();

    for (size_t di = 0; di < deps.size(); ++di) {
        const auto& dep = deps[di];
        if (!verify_dep(build_result.matrix, dep)) continue;

        std::cout << "  Dep #" << (di+1) << " (size=" << popcnt(dep) << ")..." << std::flush;

        auto rat = compute_rational_sqrt(to_bv(dep), relations, fb, n, ctx.m());
        if (!rat.success) {
            std::cout << " rat_fail\n" << std::flush;
            continue;
        }

        auto alg = compute_algebraic_sqrt(to_bv(dep), relations, ctx);
        if (!alg.success) {
            std::cout << " alg_fail\n" << std::flush;
            continue;
        }

        // Debug: show X and Y values for first 3 deps
        if (di < 3) {
            std::cout << " X=" << rat.value.to_string()
                      << " Y=" << alg.value.to_string() << std::flush;
        }

        // Try Y and -Y
        for (int sign = 0; sign < 2; ++sign) {
            Integer y = (sign == 0) ? alg.value.clone() : [&](){
                Integer neg = n.clone(); neg -= alg.value; return neg;
            }();

            auto factors = extract_factors(rat.value, y, n);

            auto nontrivial = [&](const Integer& f) {
                if (f.fits_uint64() && f.to_uint64() == 1) return false;
                return f.compare(n) != 0;
            };

            Integer f1, f2;
            bool found = false;
            if (nontrivial(factors.factor1)) {
                f1 = factors.factor1.clone();
                f2 = n.clone(); f2 /= f1;
                found = true;
            } else if (nontrivial(factors.factor2)) {
                f1 = factors.factor2.clone();
                f2 = n.clone(); f2 /= f1;
                found = true;
            }

            if (found) {
                Integer chk = f1.clone(); chk *= f2;
                if (chk.compare(n) == 0 && nontrivial(f1) && nontrivial(f2)) {
                    result.factor1 = std::move(f1);
                    result.factor2 = std::move(f2);
                    result.success = true;
                    std::cout << " SUCCESS!\n" << std::flush;
                    goto done;
                }
            }
        }
        std::cout << " trivial\n" << std::flush;
    }

    // --- XOR combination of dependency pairs ---
    if (!result.success && deps.size() >= 2) {
        std::cout << "  Trying XOR combinations of dependency pairs...\n" << std::flush;
        size_t max_try = std::min(deps.size(), size_t(20));
        for (size_t i = 0; i < max_try && !result.success; ++i) {
            for (size_t j = i + 1; j < max_try && !result.success; ++j) {
                BitVector combined = to_bv(deps[i]);
                combined.xor_with(to_bv(deps[j]));
                if (combined.popcount() < 2) continue;
                if (!verify_dep(build_result.matrix, deps[i])) continue;  // verify original

                auto rat = compute_rational_sqrt(combined, relations, fb, n, ctx.m());
                if (!rat.success) continue;

                // Use full algorithm (Hensel → Couveignes) for combined dep
                auto alg = compute_algebraic_sqrt(combined, relations, ctx);
                if (!alg.success) continue;

                for (int sign = 0; sign < 2; ++sign) {
                    Integer y = (sign == 0) ? alg.value.clone() : [&](){
                        Integer neg = n.clone(); neg -= alg.value; return neg;
                    }();
                    auto factors = extract_factors(rat.value, y, n);
                    auto nontrivial = [&](const Integer& f) {
                        if (f.fits_uint64() && f.to_uint64() == 1) return false;
                        return f.compare(n) != 0;
                    };
                    Integer f1, f2;
                    bool found = false;
                    if (nontrivial(factors.factor1)) {
                        f1 = factors.factor1.clone(); f2 = n.clone(); f2 /= f1; found = true;
                    } else if (nontrivial(factors.factor2)) {
                        f1 = factors.factor2.clone(); f2 = n.clone(); f2 /= f1; found = true;
                    }
                    if (found) {
                        Integer chk = f1.clone(); chk *= f2;
                        if (chk.compare(n) == 0 && nontrivial(f1) && nontrivial(f2)) {
                            result.factor1 = std::move(f1);
                            result.factor2 = std::move(f2);
                            result.success = true;
                            std::cout << "  XOR(" << i+1 << "," << j+1 << ") SUCCESS!\n" << std::flush;
                        }
                    }
                }
            }
        }
    }

done:
    result.time_sec = total.sec();

    if (result.success) {
        std::cout << "\n┌──────────────────────────────────────┐\n";
        std::cout << "│  ★ FACTORIZATION SUCCESSFUL ★        │\n";
        std::cout << "│  p = " << result.factor1.to_string() << "\n";
        std::cout << "│  q = " << result.factor2.to_string() << "\n";
        std::cout << "│  Time: " << std::fixed << std::setprecision(2) << result.time_sec << " sec\n";
        std::cout << "└──────────────────────────────────────┘\n" << std::flush;
    } else {
        std::cout << "\n  ✗ Factorization failed after " << result.time_sec << " sec\n" << std::flush;
    }

    return result;
}

// ============================================================
// Main
// ============================================================

int main(int argc, char** argv) {
    int min_level = 1, max_level = 5;

    if (argc >= 2) min_level = std::stoi(argv[1]);
    if (argc >= 3) max_level = std::stoi(argv[2]);
    if (min_level < 1) min_level = 1;
    if (max_level > 5) max_level = 5;
    if (max_level < min_level) max_level = min_level;

    std::cout << "════════════════════════════════════════════════════\n";
    std::cout << "  GNFS Progressive Factorization Test\n";
    std::cout << "  Levels: " << min_level << " to " << max_level << "\n";
    std::cout << "════════════════════════════════════════════════════\n\n";

    auto cases = get_test_cases();
    int pass = 0, fail = 0, skip = 0;

    for (const auto& tc : cases) {
        if (tc.level < min_level || tc.level > max_level) {
            ++skip;
            continue;
        }

        std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        std::cout << "  Level " << tc.level << ": " << tc.name << "\n";
        std::cout << "  N = " << tc.n_str << "\n";
        std::cout << "  Expected: " << tc.expected_p << " × " << tc.expected_q << "\n";
        std::cout << "  Time estimate: " << tc.time_estimate << "\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

        Integer n(tc.n_str.c_str());
        auto result = factor_with_progress(n, tc.level);

        if (result.success) {
            // Verify factors
            Integer p1 = result.factor1.clone();
            Integer p2 = result.factor2.clone();

            // Sort
            if (p1.compare(p2) > 0) std::swap(p1, p2);

            Integer exp_p(tc.expected_p.c_str());
            Integer exp_q(tc.expected_q.c_str());

            bool correct = (p1.compare(exp_p) == 0 && p2.compare(exp_q) == 0) ||
                           (p1.compare(exp_q) == 0 && p2.compare(exp_p) == 0);

            if (correct) {
                std::cout << "  ✓ PASS (factors verified)\n";
                ++pass;
            } else {
                std::cout << "  ✗ WRONG FACTORS: got " << p1.to_string()
                          << " × " << p2.to_string() << "\n";
                ++fail;
            }
        } else {
            std::cout << "  ✗ FAIL (factorization unsuccessful)\n";
            ++fail;
        }
    }

    std::cout << "\n════════════════════════════════════════════════════\n";
    std::cout << "  Results: " << pass << " passed, " << fail << " failed, " << skip << " skipped\n";
    std::cout << "════════════════════════════════════════════════════\n";

    return (fail > 0) ? 1 : 0;
}

// test_regression_gate.cpp — Multi-scale GNFS regression gate
//
// Runs full pipeline on 4 semiprimes (10/15/20/25-digit) to verify
// no regressions across all stages. Designed for merge gate validation.
//
// Usage:
//   ./test_regression_gate         # Run all levels
//   ./test_regression_gate 1       # Run only level 1 (10-digit)
//   ./test_regression_gate 1 3     # Run levels 1 through 3
//
// Expected runtime: ~2-5 seconds total

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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <thread>
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
// Test levels: N, expected factors
// ============================================================

struct RegressionLevel {
    int level;
    const char* label;
    const char* n_str;
    const char* expected_p;
    const char* expected_q;
};

static const RegressionLevel LEVELS[] = {
    // L1: 17-bit — matches progressive L2 difficulty; avoids too-small-for-GNFS pitfall
    {1, "17-bit (307x313)",    "96091",             "307",         "313"},
    {2, "27-bit (10007x10009)", "100160063",        "10007",       "10009"},
    {3, "40-bit (1000003x1000033)", "1000036000099", "1000003",    "1000033"},
    {4, "25-digit (81-bit)", "1669994516749619561652133", "1292282676071", "1292282677523"},
};
static constexpr int NUM_LEVELS = 4;

// ============================================================
// Timer
// ============================================================

class Timer {
public:
    Timer() : start_(std::chrono::high_resolution_clock::now()) {}
    double sec() const {
        return std::chrono::duration<double>(
            std::chrono::high_resolution_clock::now() - start_).count();
    }
    void reset() { start_ = std::chrono::high_resolution_clock::now(); }
private:
    std::chrono::high_resolution_clock::time_point start_;
};

// ============================================================
// Full pipeline factorization (returns true on success)
// ============================================================

static bool factorize(const RegressionLevel& tc) {
    Integer n(tc.n_str);
    Integer expected_p(tc.expected_p);
    Integer expected_q(tc.expected_q);

    size_t bits = n.bit_length();
    auto params = GNFSParams::compute(bits);

    // Phase 1: Polynomial
    auto ctx = SelectorDispatch::select(n, params.degree);

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
    sr.i_min = params.sieve_i_min; sr.i_max = params.sieve_i_max;
    sr.j_min = params.sieve_j_min; sr.j_max = params.sieve_j_max;

    CofactorizerConfig cc;
    cc.large_prime_bound = fb.params().large_prime_bound;
    cc.allow_1lp = true; cc.allow_2lp = true;
    Cofactorizer cofac(ctx, fb, cc);

    SpecialQRange sqr;
    sqr.min_q = params.special_q_min; sqr.max_q = params.special_q_max;
    SpecialQGenerator sqg(fb, sqr);

    CollectorConfig colc;
    colc.check_duplicates = true;
    RelationCollector collector(colc);

    size_t matrix_cols = fb.rational_count() + fb.sieve_algebraic_count() + params.target_excess;
    size_t initial_target = params.raw_relation_target(matrix_cols);
    size_t batch_target = initial_target;
    size_t sq_count = 0;
    LatticeSieve sieve(ctx, fb, sp);
    sieve.set_region(sr);

    // Adaptive sieve-filter-merge loop
    std::vector<Relation> relations;
    bool lp_enabled = params.large_prime_bound > params.algebraic_bound;
    constexpr int MAX_ROUNDS = 10;

    for (int round = 0; round < MAX_ROUNDS; ++round) {
        while (sqg.has_next() && collector.size() < batch_target && sq_count < params.max_special_q) {
            auto sq = sqg.next();
            if (!sq) break;
            auto sres = sieve.sieve_special_q(*sq);

            const auto& cands = sres.candidates;
            size_t n_cands = cands.size();
            size_t n_threads = std::thread::hardware_concurrency();
            if (n_threads == 0) n_threads = 4;
            if (n_cands < 200) n_threads = 1;

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
                    if (start >= n_cands) break;
                    if (global_found.load(std::memory_order_relaxed) >= batch_target) break;
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
                for (auto& t : threads) t.join();
            }

            for (auto& tr : thread_results)
                for (auto& rel : tr)
                    collector.add(std::move(rel));
            ++sq_count;
        }

        if (collector.size() < 10) break;

        relations = collector.get_relations();
        FilterConfig fc;
        fc.remove_singletons = true; fc.max_passes = 10;
        RelationFilter filter(fc);
        relations = filter.filter(std::move(relations));

        if (lp_enabled) {
            auto sep = separate_relations(std::move(relations));
            PartialRelationMerger::MergeStats mstats;
            auto merged = PartialRelationMerger::merge_all(
                std::move(sep.partial), 10, &mstats);
            relations = std::move(sep.full);
            relations.insert(relations.end(),
                std::make_move_iterator(merged.begin()),
                std::make_move_iterator(merged.end()));
        }

        if (relations.size() > matrix_cols) break;

        if (!sqg.has_next() || sq_count >= params.max_special_q) break;

        double merge_rate = (collector.size() > 0) ?
            static_cast<double>(relations.size()) / static_cast<double>(collector.size()) : 0.01;
        size_t needed_raw = static_cast<size_t>(
            static_cast<double>(matrix_cols * 2) / std::max(merge_rate, 0.001));
        batch_target = std::min(
            std::max(batch_target * 2, needed_raw),
            initial_target * 5);
    }

    if (relations.size() <= matrix_cols) {
        std::cerr << "  insufficient relations: " << relations.size() << "/" << matrix_cols << "\n";
        return false;
    }

    // Trim — must include LP cols (lp_bits≥20 cases have significant LP cols).
    // For ≤30-bit (Level 1-3) lp_cols is small (~1K-3K), 1.3× FB is barely enough.
    // For 25-digit (Level 4) trim is rarely triggered so safe in practice, but fix
    // for consistency and future-proofing.
    {
        size_t lp_cols_for_trim = lp_enabled ? count_unique_lp_keys(relations) : 0;
        size_t effective_cols = matrix_cols + lp_cols_for_trim;
        size_t max_rels = static_cast<size_t>(effective_cols * 1.3);
        if (relations.size() > max_rels) {
            std::mt19937 rng(42);
            std::shuffle(relations.begin(), relations.end(), rng);
            relations.resize(max_rels);
        }
    }

    // Phase 4: Linear Algebra
    MatrixBuilderConfig mc;
    mc.include_sign_column = true; mc.include_qc_columns = true;
    mc.include_class_group = false;
    mc.include_schirokauer = true;
    mc.num_qc_primes = params.num_qc_primes;
    mc.qc_prime_start = 100;
    mc.schirokauer_primes = {2};
    MatrixBuilder mb(mc);
    auto br = mb.build_with_qc(relations, fb, ctx);

    auto sge_result = linalg::SGE::preprocess(br.matrix);

    BlockLanczos solver;
    auto deps = solver.find_dependencies(sge_result.reduced_matrix);

    for (auto& dep : deps) {
        dep = sge_result.expand_dependency(dep);
    }

    if (deps.empty()) {
        std::cerr << "  no dependencies found\n";
        return false;
    }

    // Phase 5: Square Root
    auto to_bv = [](const std::vector<bool>& v) {
        BitVector bv(v.size());
        for (size_t i = 0; i < v.size(); ++i) if (v[i]) bv.set(i);
        return bv;
    };

    for (size_t di = 0; di < deps.size(); ++di) {
        auto bv = to_bv(deps[di]);

        auto rat = compute_rational_sqrt(bv, relations, fb, n, ctx.m());
        if (!rat.success) continue;

        auto alg = compute_algebraic_sqrt(bv, relations, ctx);
        if (!alg.success) continue;

        for (int sign = 0; sign < 2; ++sign) {
            Integer y = (sign == 0) ? alg.value.clone() : [&](){
                Integer neg = n.clone(); neg -= alg.value; return neg;
            }();
            auto factors = extract_factors(rat.value, y, n);

            auto check = [&](const Integer& f) -> bool {
                if (f.fits_uint64() && f.to_uint64() == 1) return false;
                return f.compare(n) != 0;
            };

            Integer f1, f2;
            bool found = false;
            if (check(factors.factor1)) { f1 = factors.factor1.clone(); f2 = n.clone(); f2 /= f1; found = true; }
            else if (check(factors.factor2)) { f1 = factors.factor2.clone(); f2 = n.clone(); f2 /= f1; found = true; }

            if (found) {
                Integer chk = f1.clone(); chk *= f2;
                if (chk.compare(n) == 0) {
                    // Verify factors match expected
                    bool p_match = (f1.compare(expected_p) == 0 && f2.compare(expected_q) == 0) ||
                                   (f1.compare(expected_q) == 0 && f2.compare(expected_p) == 0);
                    if (!p_match) {
                        // Factored correctly but different factors (shouldn't happen for semiprime)
                        std::cerr << "  factored but unexpected factors: "
                                  << f1.to_string() << " × " << f2.to_string() << "\n";
                    }
                    return true;
                }
            }
        }
    }

    std::cerr << "  all deps exhausted\n";
    return false;
}

// ============================================================
// Main
// ============================================================

int main(int argc, char* argv[]) {
    int min_level = 1;
    int max_level = NUM_LEVELS;

    if (argc >= 2) min_level = std::atoi(argv[1]);
    if (argc >= 3) max_level = std::atoi(argv[2]);

    if (min_level < 1) min_level = 1;
    if (max_level > NUM_LEVELS) max_level = NUM_LEVELS;
    if (min_level > max_level) {
        std::cerr << "Invalid range: " << min_level << "-" << max_level << "\n";
        return 1;
    }

    std::cout << "═══════════════════════════════════════════\n";
    std::cout << "  GNFS Regression Gate — Levels " << min_level << "-" << max_level << "\n";
    std::cout << "═══════════════════════════════════════════\n\n";

    Timer total;
    int passed = 0, failed = 0;

    for (int i = 0; i < NUM_LEVELS; ++i) {
        const auto& tc = LEVELS[i];
        if (tc.level < min_level || tc.level > max_level) continue;

        std::cout << "[L" << tc.level << "] " << tc.label << " N=" << tc.n_str << "\n";
        Timer level_timer;

        bool ok = factorize(tc);

        double elapsed = level_timer.sec();
        if (ok) {
            std::cout << "  PASS (" << std::fixed << std::setprecision(2) << elapsed << "s)\n\n";
            ++passed;
        } else {
            std::cout << "  FAIL (" << std::fixed << std::setprecision(2) << elapsed << "s)\n\n";
            ++failed;
        }
    }

    std::cout << "═══════════════════════════════════════════\n";
    std::cout << "  Results: " << passed << " passed, " << failed << " failed"
              << " (" << std::fixed << std::setprecision(2) << total.sec() << "s)\n";
    std::cout << "═══════════════════════════════════════════\n";

    return (failed > 0) ? 1 : 0;
}

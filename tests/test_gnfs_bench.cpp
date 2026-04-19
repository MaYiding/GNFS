// test_gnfs_bench.cpp — GNFS pipeline benchmark at various digit sizes
//
// Usage:
//   ./test_gnfs_bench           # Run all sizes (20d, 25d, 30d)
//   ./test_gnfs_bench 1         # Run only first size (20d)
//   ./test_gnfs_bench 1 2       # Run first two sizes
//   ./test_gnfs_bench <N>       # Direct: factor a specific number

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
#include <cassert>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
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
// Helpers
// ============================================================

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

inline std::vector<std::vector<bool>> find_deps(const SparseMatrix& mat, size_t max_deps) {
    BlockLanczos solver;
    return solver.find_dependencies(mat, max_deps);
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

// ============================================================
// Test Cases — balanced semiprimes at various digit sizes
// ============================================================

struct TestCase {
    std::string name;
    std::string n_str;
    std::string expected_p;
    std::string expected_q;
};

std::vector<TestCase> get_test_cases() {
    return {
        // 20-digit (65 bits) — should be fast
        {"20d-65bit", "36329368269807044651",
         "3746317241", "9697355011"},

        // 25-digit (83 bits)
        {"25d-83bit", "1034776851887100518790841",
         "32165413711", "32165536631"},

        // 30-digit (100 bits)
        {"30d-100bit", "201166269974413068358266440287",
         "215400344428723", "933918051560869"},

        // 34-digit (111 bits) — getting into serious GNFS territory
        {"34d-111bit", "2351797048080285065116384330488001",
         "48576344210584453", "48414451237519117"},
    };
}

// ============================================================
// Factor one number with GNFS pipeline
// ============================================================

struct BenchResult {
    bool success = false;
    double sieve_sec = 0;
    double linalg_sec = 0;
    double sqrt_sec = 0;
    double total_sec = 0;
    size_t raw_relations = 0;
    size_t usable_relations = 0;
    size_t matrix_rows = 0;
    size_t matrix_cols = 0;
    size_t sq_count = 0;
};

BenchResult factor_gnfs(const Integer& n) {
    BenchResult result;
    StopWatch total;

    size_t bits = n.bit_length();
    auto params = GNFSParams::compute(bits);

    std::cout << "  N = " << n.to_string().substr(0, 50)
              << (n.to_string().size() > 50 ? "..." : "") << "\n";
    std::cout << "  Bits=" << bits << " Digits=" << params.digits
              << " Degree=" << params.degree
              << " FB=" << params.rational_bound << "/" << params.algebraic_bound
              << " LP=" << params.large_prime_bound << "\n" << std::flush;

    // Phase 1: Polynomial Selection
    StopWatch phase;
    auto ctx = SelectorDispatch::select(n, params.degree);
    double poly_ms = phase.ms();
    std::cout << "  [Poly] " << std::fixed << std::setprecision(1) << poly_ms << " ms"
              << " m=" << ctx.m().to_string() << "\n" << std::flush;

    // Phase 2: Factor Base
    phase.reset();
    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = params.rational_bound;
    fb_opts.algebraic_bound = params.algebraic_bound;
    fb_opts.special_q_bound = params.special_q_max;
    fb_opts.parallel = true;
    auto fb = FactorBaseBuilder::build(ctx, fb_opts);
    double fb_ms = phase.ms();
    std::cout << "  [FB] " << std::setprecision(1) << fb_ms << " ms"
              << " rat=" << fb.rational_count()
              << " alg=" << fb.algebraic_count()
              << " (sieve=" << fb.sieve_algebraic_count() << ")\n" << std::flush;

    // Phase 3: Sieving
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

    SpecialQRange sq_range;
    sq_range.min_q = params.special_q_min;
    sq_range.max_q = params.special_q_max;
    SpecialQGenerator sq_gen(fb, sq_range);

    CollectorConfig coll_config;
    coll_config.check_duplicates = true;
    RelationCollector collector(coll_config);

    size_t matrix_cols = fb.rational_count() + fb.sieve_algebraic_count() + params.target_excess;
    size_t batch_target = params.raw_relation_target(matrix_cols);

    LatticeSieve sieve(ctx, fb, sieve_params);
    sieve.set_region(sieve_region);

    std::vector<Relation> relations;
    bool lp_enabled = params.large_prime_bound > params.algebraic_bound;
    constexpr int MAX_ROUNDS = 10;

    size_t n_threads = std::thread::hardware_concurrency();
    if (n_threads == 0) n_threads = 4;
    constexpr size_t SQ_BATCH = 12;  // Batch-parallel sieve

    for (int round = 0; round < MAX_ROUNDS; ++round) {
        while (sq_gen.has_next() && collector.size() < batch_target && result.sq_count < params.max_special_q) {
            // Collect a batch of SQs
            std::vector<SpecialQ> sq_batch;
            sq_batch.reserve(SQ_BATCH);
            for (size_t b = 0; b < SQ_BATCH && sq_gen.has_next(); ++b) {
                auto sq = sq_gen.next();
                if (!sq) break;
                sq_batch.push_back(*sq);
            }
            if (sq_batch.empty()) break;

            // Parallel sieve + cofactorize: each thread handles one SQ
            // (sieve → cofac with SQ pre-division for correct & faster results)
            {
                std::vector<std::vector<Relation>> batch_relations(sq_batch.size());
                std::atomic<size_t> next_sq{0};

                auto worker = [&](size_t /*tid*/) {
                    LatticeSieve local_sieve(ctx, fb, sieve_params);
                    local_sieve.set_region(sieve_region);
                    Cofactorizer local_cofac(ctx, fb, cofac_config);

                    while (true) {
                        size_t idx = next_sq.fetch_add(1, std::memory_order_relaxed);
                        if (idx >= sq_batch.size()) break;

                        auto sr = local_sieve.sieve_special_q(sq_batch[idx]);

                        auto& local_rels = batch_relations[idx];
                        for (const auto& cand : sr.candidates) {
                            auto rel = local_cofac.verify(cand,
                                sq_batch[idx].q, sq_batch[idx].r);
                            if (rel) local_rels.push_back(std::move(*rel));
                        }
                    }
                };

                std::vector<std::thread> threads;
                size_t actual_threads = std::min(n_threads, sq_batch.size());
                threads.reserve(actual_threads);
                for (size_t t = 0; t < actual_threads; ++t)
                    threads.emplace_back(worker, t);
                for (auto& t : threads) t.join();

                for (auto& rels : batch_relations)
                    for (auto& rel : rels)
                        collector.add(std::move(rel));
            }
            result.sq_count += sq_batch.size();

            if (result.sq_count % 10 < SQ_BATCH || result.sq_count <= SQ_BATCH) {
                double rate = (phase.sec() > 0) ?
                    static_cast<double>(collector.size()) / phase.sec() : 0;
                double eta_s = (rate > 0) ?
                    static_cast<double>(batch_target - collector.size()) / rate : 0;
                std::cout << "    SQ #" << result.sq_count
                          << ": rels=" << collector.size() << "/" << batch_target
                          << " (" << std::setprecision(1)
                          << (100.0 * collector.size() / batch_target) << "%)"
                          << " elapsed=" << std::setprecision(1) << phase.sec() << "s"
                          << " rate=" << std::setprecision(0) << rate << "/s"
                          << " eta=" << std::setprecision(0) << eta_s << "s"
                          << "\n" << std::flush;
            }
        }

        if (collector.size() < 10) break;

        relations = collector.get_relations();
        FilterConfig filt_config;
        filt_config.remove_singletons = true;
        filt_config.max_passes = 10;
        RelationFilter filter(filt_config);
        relations = filter.filter(std::move(relations));

        if (lp_enabled) {
            auto sep = separate_relations(std::move(relations));
            PartialRelationMerger::MergeStats mstats;
            auto merged = PartialRelationMerger::merge_all(
                std::move(sep.partial), 10, &mstats);

            std::cout << "    [round " << (round+1) << "] Full=" << sep.full.size()
                      << " 1LP=" << mstats.input_1lp
                      << " 2LP=" << mstats.input_2lp
                      << " Merged=" << merged.size() << "\n" << std::flush;

            relations = std::move(sep.full);
            relations.insert(relations.end(),
                std::make_move_iterator(merged.begin()),
                std::make_move_iterator(merged.end()));
        }

        if (relations.size() > matrix_cols) break;

        if (!sq_gen.has_next() || result.sq_count >= params.max_special_q) {
            std::cout << "    SQ exhausted\n";
            break;
        }

        double merge_rate = (collector.size() > 0) ?
            static_cast<double>(relations.size()) / static_cast<double>(collector.size()) : 0.01;
        size_t needed_raw = static_cast<size_t>(
            static_cast<double>(matrix_cols * 2) / std::max(merge_rate, 0.001));
        // Cap: initial_target × 100 — generous for low merge rates (~2-5%).
        // Session 78 bug: cap of 5× caused adaptive loop to stall at 39d
        // (227K raw > 225K cap, but needed 450K+ for enough usable).
        batch_target = std::min(
            std::max(batch_target * 2, needed_raw),
            params.raw_relation_target(matrix_cols) * 100);
    }

    result.raw_relations = collector.size();
    result.sieve_sec = phase.sec();
    std::cout << "  [Sieve] " << std::setprecision(2) << result.sieve_sec << "s"
              << " raw=" << result.raw_relations
              << " usable=" << relations.size()
              << " SQ=" << result.sq_count << "\n" << std::flush;

    if (relations.size() < 5) {
        std::cout << "  INSUFFICIENT RELATIONS\n";
        result.total_sec = total.sec();
        return result;
    }

    result.usable_relations = relations.size();

    // Trim excess to 1.1× matrix_cols for optimal SGE + BL convergence.
    // High excess causes BL A-gram persistent rank deficiency and poor SGE reduction.
    size_t max_rels = static_cast<size_t>(matrix_cols * 1.1) + 100;
    if (relations.size() > max_rels) {
        std::mt19937 rng(42);
        std::shuffle(relations.begin(), relations.end(), rng);
        relations.resize(max_rels);
        std::cout << "  [Trim] " << result.usable_relations << " -> " << max_rels
                  << " relations (" << std::setprecision(1)
                  << (100.0 * max_rels / matrix_cols) << "% of matrix_cols)\n" << std::flush;
    }

    // Phase 4: Linear Algebra
    phase.reset();

    MatrixBuilderConfig mb_config;
    mb_config.include_sign_column = true;
    mb_config.include_qc_columns = true;
    mb_config.include_class_group = false;
    mb_config.include_schirokauer = true;
    mb_config.num_qc_primes = params.num_qc_primes;
    mb_config.qc_prime_start = 100;
    mb_config.schirokauer_primes = {2};
    mb_config.verbose = false;

    MatrixBuilder mb(mb_config);
    auto build_result = mb.build_with_qc(relations, fb, ctx);

    SGEConfig sge_config;
    sge_config.verbose = true;
    auto sge_result = SGE::preprocess(build_result.matrix, sge_config);

    result.matrix_rows = sge_result.reduced_matrix.num_rows();
    result.matrix_cols = sge_result.reduced_matrix.num_cols();
    std::cout << "  [Matrix] " << build_result.matrix.num_rows() << "x"
              << build_result.matrix.num_cols()
              << " -> SGE " << result.matrix_rows << "x" << result.matrix_cols << "\n" << std::flush;

    auto deps = find_deps(sge_result.reduced_matrix, 64);
    for (auto& dep : deps) dep = sge_result.expand_dependency(dep);

    result.linalg_sec = phase.sec();
    std::cout << "  [LinAlg] " << std::setprecision(2) << result.linalg_sec << "s"
              << " deps=" << deps.size() << "\n" << std::flush;

    if (deps.empty()) {
        result.total_sec = total.sec();
        return result;
    }

    // Phase 5: Square Root
    phase.reset();

    for (size_t di = 0; di < deps.size(); ++di) {
        const auto& dep = deps[di];

        auto rat = compute_rational_sqrt(to_bv(dep), relations, fb, n, ctx.m());
        if (!rat.success) continue;

        auto alg = compute_algebraic_sqrt(to_bv(dep), relations, ctx);
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
                    result.success = true;
                    result.sqrt_sec = phase.sec();
                    result.total_sec = total.sec();
                    std::cout << "  [Sqrt] " << std::setprecision(2) << result.sqrt_sec << "s"
                              << " dep#" << (di+1) << "\n";
                    std::cout << "  => " << f1.to_string() << " x " << f2.to_string() << "\n" << std::flush;
                    return result;
                }
            }
        }
    }

    result.sqrt_sec = phase.sec();
    result.total_sec = total.sec();
    return result;
}

// ============================================================
// Main
// ============================================================

int main(int argc, char** argv) {
    auto cases = get_test_cases();

    // Check if argument is a direct number
    if (argc == 2) {
        std::string arg(argv[1]);
        if (arg.size() > 3 && std::all_of(arg.begin(), arg.end(), ::isdigit)) {
            // Direct number mode
            Integer n(arg);
            std::cout << "═══ GNFS Direct Benchmark ═══\n" << std::flush;
            auto result = factor_gnfs(n);
            if (result.success) {
                std::cout << "\n  TOTAL: " << std::fixed << std::setprecision(2)
                          << result.total_sec << "s"
                          << " (sieve=" << result.sieve_sec
                          << " linalg=" << result.linalg_sec
                          << " sqrt=" << result.sqrt_sec << ")\n";
            } else {
                std::cout << "\n  FAILED after " << result.total_sec << "s\n";
            }
            return result.success ? 0 : 1;
        }
    }

    size_t start = 0, end = cases.size() - 1;
    if (argc >= 2) start = static_cast<size_t>(std::max(0, std::stoi(argv[1]) - 1));
    if (argc >= 3) end = static_cast<size_t>(std::max(0, std::stoi(argv[2]) - 1));
    if (start >= cases.size()) start = cases.size() - 1;
    if (end >= cases.size()) end = cases.size() - 1;
    if (end < start) end = start;

    std::cout << "═══════════════════════════════════════════════════\n";
    std::cout << "  GNFS Pipeline Benchmark\n";
    std::cout << "═══════════════════════════════════════════════════\n\n" << std::flush;

    int passed = 0, failed = 0;

    for (int i = start; i <= end; ++i) {
        auto& tc = cases[i];
        std::cout << "─── " << tc.name << " ───\n" << std::flush;

        Integer n(tc.n_str);
        auto result = factor_gnfs(n);

        if (result.success) {
            ++passed;
            std::cout << "  TOTAL: " << std::fixed << std::setprecision(2)
                      << result.total_sec << "s"
                      << " (sieve=" << result.sieve_sec
                      << " linalg=" << result.linalg_sec
                      << " sqrt=" << result.sqrt_sec << ")\n";
        } else {
            ++failed;
            std::cout << "  FAILED after " << result.total_sec << "s\n";
        }
        std::cout << "\n" << std::flush;
    }

    std::cout << "═══════════════════════════════════════════════════\n";
    std::cout << "  Results: " << passed << " passed, " << failed << " failed\n";
    std::cout << "═══════════════════════════════════════════════════\n";

    return failed > 0 ? 1 : 0;
}

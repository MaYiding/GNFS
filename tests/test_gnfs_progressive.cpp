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

#include <cassert>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
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

    // Primality check: GNFS only works on composites
    if (mpz_probab_prime_p(n.get_mpz(), 25) > 0) {
        std::cerr << "N is prime (or probably prime). GNFS requires a composite input.\n";
        return result;
    }

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
    std::optional<PolynomialContext> ctx_opt;
    try {
        ctx_opt.emplace(SelectorDispatch::select(n, degree));
    } catch (const std::exception& e) {
        std::cout << " FAILED: " << e.what() << "\n";
        return result;
    }
    auto& ctx = *ctx_opt;
    std::cout << " done (" << phase.ms() << " ms)\n";
    std::cout << "  m = " << ctx.m().to_string() << "\n" << std::flush;

    // ── Phase 2: Factor Base ──
    std::cout << "[Phase 2] Factor base construction..." << std::flush;
    phase.reset();

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = params.rational_bound;
    fb_opts.algebraic_bound = params.algebraic_bound;
    fb_opts.special_q_bound = params.special_q_max;
    fb_opts.parallel = true;

    auto fb = FactorBaseBuilder::build(ctx, fb_opts);
    std::cout << " done (" << phase.ms() << " ms)\n";
    std::cout << "  Rational: " << fb.rational_count()
              << ", Algebraic: " << fb.algebraic_count()
              << " (sieve: " << fb.sieve_algebraic_count() << ")\n" << std::flush;

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

    size_t matrix_cols = fb.rational_count() + fb.sieve_algebraic_count() + params.target_excess;
    // Initial target: small batch to test merge rate, then adaptive
    size_t initial_target = params.raw_relation_target(matrix_cols);
    size_t batch_target = initial_target;
    size_t sq_count = 0;

    LatticeSieve sieve(ctx, fb, sieve_params);
    sieve.set_region(sieve_region);

    // ── Adaptive sieve-filter-merge loop ──
    // Like CADO-NFS: sieve a batch → filter + merge → check excess → repeat if needed
    std::vector<Relation> relations;
    bool lp_enabled = params.large_prime_bound > params.algebraic_bound;
    constexpr int MAX_ROUNDS = 10;

    for (int round = 0; round < MAX_ROUNDS; ++round) {
        // Sieve until batch_target or exhaustion
        while (sq_gen.has_next() && collector.size() < batch_target && sq_count < params.max_special_q) {
            auto sq = sq_gen.next();
            if (!sq) break;

            auto sr = sieve.sieve_special_q(*sq);

            // 并行 cofactorization：将候选分块交给多线程处理
            {
                const auto& cands = sr.candidates;
                size_t n_cands = cands.size();
                size_t n_threads = std::thread::hardware_concurrency();
                if (n_threads == 0) n_threads = 4;
                if (n_cands < 1000) n_threads = 1;  // 太少候选不值得并行

                // 每线程有自己的 cofactorizer（因为 verify() 不是线程安全的）
                // 收集结果到 per-thread vector，最后合并
                std::vector<std::vector<Relation>> thread_results(n_threads);
                std::atomic<size_t> global_found{collector.size()};
                std::atomic<size_t> next_chunk{0};
                constexpr size_t CHUNK_SIZE = 256;  // 动态调度粒度

                auto worker = [&](size_t tid) {
                    Cofactorizer local_cofac(ctx, fb, cofac_config);
                    auto& local_rels = thread_results[tid];
                    local_rels.reserve(n_cands / (n_threads * 4));

                    while (true) {
                        size_t start = next_chunk.fetch_add(CHUNK_SIZE, std::memory_order_relaxed);
                        if (start >= n_cands) break;
                        // 早停：全局已收集足够
                        if (global_found.load(std::memory_order_relaxed) >= batch_target) break;

                        size_t end = std::min(start + CHUNK_SIZE, n_cands);
                        for (size_t ci = start; ci < end; ++ci) {
                            auto rel = local_cofac.verify(cands[ci]);
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

                // 合并结果
                for (auto& tr : thread_results) {
                    for (auto& rel : tr) {
                        collector.add(std::move(rel));
                    }
                }
            }
            ++sq_count;

            if (sq_count % std::max(size_t(1), static_cast<size_t>(params.progress_interval)) == 0 || collector.size() >= batch_target) {
                double rate = collector.size() / (phase.sec() + 0.001);
                std::cout << "  SQ #" << sq_count
                          << ": rels=" << collector.size() << "/" << batch_target
                          << " (" << std::fixed << std::setprecision(1)
                          << (100.0 * collector.size() / batch_target) << "%)"
                          << " rate=" << std::setprecision(1) << rate << "/s"
                          << " elapsed=" << std::setprecision(1) << phase.sec() << "s"
                          << "\n" << std::flush;
            }
        }

        if (collector.size() < 10) break;

        // Filter + merge
        relations = collector.get_relations();
        FilterConfig filt_config;
        filt_config.remove_singletons = true;
        filt_config.max_passes = 10;
        RelationFilter filter(filt_config);
        relations = filter.filter(std::move(relations));

        if (lp_enabled) {
            auto sep = separate_relations(std::move(relations));

            // 2LP merge: handles both 1LP×1LP and 2LP via iterative weight-2 processing
            PartialRelationMerger::MergeStats mstats;
            auto merged = PartialRelationMerger::merge_all(
                std::move(sep.partial), 10, &mstats);

            std::cout << "  [round " << (round+1) << "] Full=" << sep.full.size()
                      << " 1LP=" << mstats.input_1lp
                      << " 2LP=" << mstats.input_2lp
                      << " Merged=" << merged.size()
                      << " (w2=" << mstats.weight2_merges
                      << " sngl=" << mstats.singletons_removed
                      << " rnd=" << mstats.rounds << ")\n" << std::flush;

            relations = std::move(sep.full);
            relations.insert(relations.end(),
                std::make_move_iterator(merged.begin()),
                std::make_move_iterator(merged.end()));
        }

        // Check: enough for matrix?
        if (relations.size() > matrix_cols) {
            std::cout << "  Sieving complete: " << collector.size() << " raw relations, "
                      << relations.size() << " usable, in " << phase.sec() << " sec\n" << std::flush;
            break;
        }

        // Not enough — double the target and keep sieving
        if (!sq_gen.has_next() || sq_count >= params.max_special_q) {
            std::cout << "  SQ exhausted at " << sq_count << " — insufficient relations\n";
            break;
        }

        double merge_rate = (collector.size() > 0) ?
            static_cast<double>(relations.size()) / static_cast<double>(collector.size()) : 0.01;
        // Birthday effect: merge_rate improves as ~sqrt(n), so scaling by 4× raw → ~2× yield
        // Cap at 10× initial target to prevent runaway collection
        size_t needed_raw = static_cast<size_t>(
            static_cast<double>(matrix_cols * 2) / std::max(merge_rate, 0.001));
        batch_target = std::min(
            std::max(batch_target * 4, needed_raw),
            initial_target * 10);
        std::cout << "  Need more — merge_rate=" << std::setprecision(3) << (merge_rate * 100)
                  << "%, new target=" << batch_target << "\n" << std::flush;
    }

    result.relations = collector.size();

    if (relations.size() < 5) {
        std::cout << "  INSUFFICIENT RELATIONS\n";
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

    // SGE preprocessing
    phase.reset();
    SGEConfig sge_config;
    auto sge_result = SGE::preprocess(build_result.matrix, sge_config);
    std::cout << "  SGE: " << mstats.num_rows << "×" << mstats.num_cols
              << " → " << sge_result.reduced_matrix.num_rows() << "×"
              << sge_result.reduced_matrix.num_cols()
              << " (w1=" << sge_result.weight1_eliminated
              << " w2=" << sge_result.weight2_merged
              << ") " << phase.ms() << " ms\n" << std::flush;

    std::cout << "  Finding dependencies..." << std::flush;
    phase.reset();
    auto deps = find_deps(sge_result.reduced_matrix, 64);

    // Expand dependencies back to original matrix rows
    for (auto& dep : deps) {
        dep = sge_result.expand_dependency(dep);
    }
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

        // --- Diagnostic: check if norm product is a perfect square ---
        // Skip for large deps — norm product computation is O(n²) in bit-length
        // and dominates runtime when deps have 1000+ relations (LP merge scenario)
        if (di < 3 && popcnt(dep) < 300) {
            Integer norm_product(1);
            for (size_t ri = 0; ri < relations.size(); ++ri) {
                if (!(ri < dep.size() && dep[ri])) continue;
                Integer anorm = ctx.algebraic_norm(relations[ri].a, relations[ri].b);
                if (anorm.is_negative()) anorm.negate();
                norm_product *= anorm;
            }
            bool norm_is_square = (mpz_perfect_square_p(norm_product.get_mpz()) != 0);
            std::cout << " norm²=" << (norm_is_square ? "YES" : "NO") << std::flush;

            // Also check rational product mod N
            Integer rat_product(1);
            for (size_t ri = 0; ri < relations.size(); ++ri) {
                if (!(ri < dep.size() && dep[ri])) continue;
                Integer val = Integer(relations[ri].a);
                Integer bm = ctx.m().clone();
                bm *= Integer(static_cast<int64_t>(relations[ri].b));
                val -= bm;
                rat_product *= val;
                rat_product %= n;
            }
            if (rat_product.is_negative()) rat_product += n;
            // Check if rat_product is a QR mod N by checking Jacobi symbol
            int jacobi_val = mpz_jacobi(rat_product.get_mpz(), n.get_mpz());
            std::cout << " rat_jacobi=" << jacobi_val << std::flush;
        }

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

        // Debug: verify rational product and show X, Y for first 3 deps
        if (di < 3) {
            std::cout << " X=" << rat.value.to_string()
                      << " Y=" << alg.value.to_string() << std::flush;

            // Verify: product(a_i - b_i*m) mod N should equal ±X^2 mod N
            Integer rat_product(1);
            for (size_t ri = 0; ri < relations.size(); ++ri) {
                if (!(ri < dep.size() && dep[ri])) continue;
                const auto& rel = relations[ri];
                Integer val = Integer(rel.a);
                Integer bm = ctx.m().clone();
                bm *= Integer(static_cast<int64_t>(rel.b));
                val -= bm;  // a - b*m
                rat_product *= val;
                rat_product %= n;
            }
            Integer x2 = rat.value.clone();
            x2 *= rat.value;
            x2 %= n;
            if (rat_product.is_negative()) { rat_product += n; }
            Integer neg_rat = n.clone();
            neg_rat -= rat_product;
            bool rat_ok = (rat_product.compare(x2) == 0) || (neg_rat.compare(x2) == 0);
            std::cout << " rat_check=" << (rat_ok ? "OK" : "FAIL") << std::flush;
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

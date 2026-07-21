// test_stress.cpp — GNFS stress tests for large numbers (50/60-digit)
//
// These tests verify the GNFS pipeline at scale. They are NOT run routinely —
// only when changes affect large-N code paths or on explicit request.
//
// Usage:
//   ./test_stress           # Run all levels (50 + 60)
//   ./test_stress 1         # Run only level 1 (50-digit)
//   ./test_stress 1 2       # Run levels 1 through 2 (50 + 60-digit)
//   ./test_stress 2 2       # Run only level 2 (60-digit)

#include <gnfs/core/params.hpp>
#include <gnfs/polynomial/selector_dispatch.hpp>
#include <gnfs/factor_base/builder.hpp>
#include <gnfs/sieve/special_q.hpp>
#include <gnfs/sieve/lattice_sieve.hpp>
#include <gnfs/cofactor/cofactorizer.hpp>
#include <gnfs/relation/collector.hpp>
#include <gnfs/relation/filter.hpp>
#include <gnfs/relation/clique_merger.hpp>
#include <gnfs/relation/relation_identity.hpp>
#include <gnfs/relation/ooc_policy.hpp>
#include <gnfs/relation/v0_bfs_policy.hpp>
#include <gnfs/linalg/matrix_builder.hpp>
#include <gnfs/linalg/sge.hpp>
#include <gnfs/linalg/block_lanczos.hpp>
#include <gnfs/sqrt/rational_sqrt.hpp>
#include <gnfs/sqrt/algebraic_sqrt.hpp>
#include <gnfs/util/process.hpp>
#include <gnfs/util/temp_path.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>      // fprintf for [ooc] signal
#include <cstdlib>     // getenv for OOC env vars
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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

/// Verify that a dependency vector is in the left null space of the matrix.
/// Returns the number of non-zero columns in v^T * M (0 means valid).
inline size_t verify_null_space(const std::vector<bool>& dep, const SparseMatrix& matrix) {
    // Compute r = v^T * M over GF(2) by XOR-ing rows
    size_t n_cols = matrix.num_cols();
    std::vector<uint8_t> column_parity(n_cols, 0);

    for (size_t i = 0; i < dep.size() && i < matrix.num_rows(); ++i) {
        if (!dep[i]) continue;
        for (auto j : matrix.row(i).indices()) {
            if (j < n_cols) column_parity[j] ^= 1;
        }
    }

    size_t nonzero = 0;
    for (size_t j = 0; j < n_cols; ++j) {
        if (column_parity[j]) ++nonzero;
    }
    return nonzero;
}

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
        // Level 1: 50-digit (164 bits) — product of two 25-digit primes
        {1, "50-digit semiprime (164 bit)",
         "16000000000000004000000216000000000000027000000729",
         "4000000000000000000000027",
         "4000000000000001000000027",
         "minutes-hours"},

        // Level 2: 60-digit (197 bits) — product of two 30-digit primes
        {2, "60-digit semiprime (197 bit)",
         "160000000000000000000000040075200000000000000000000006908211",
         "400000000000000000000000000069",
         "400000000000000000000000100119",
         "hours"},
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
    // Phase timing breakdown
    double poly_sec = 0;
    double fb_sec = 0;
    double sieve_sec = 0;
    double linalg_sec = 0;
    double sqrt_sec = 0;
};

FactResult factor_with_progress(const Integer& n, int level) {
    FactResult result;

    if (mpz_probab_prime_p(n.get_mpz(), 25) > 0) {
        std::cerr << "N is prime. GNFS requires a composite input.\n";
        return result;
    }

    StopWatch total;
    size_t bits = n.bit_length();
    auto params = core::GNFSParams::compute(bits);

    std::cout << "\n╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  GNFS Stress Test — Level " << level << std::string(36, ' ') << "║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
    std::cout << "║  N = " << n.to_string().substr(0, 50);
    if (n.to_string().size() > 50) std::cout << "...";
    std::cout << "\n";
    std::cout << "║  Bits: " << bits << ", Digits: " << params.digits << "\n";
    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
    std::cout << "║  Parameters (auto-computed):\n";
    std::cout << "║    Degree:       " << params.degree << "\n";
    std::cout << "║    FB rational:  " << params.rational_bound << "\n";
    std::cout << "║    FB algebraic: " << params.algebraic_bound << "\n";
    std::cout << "║    LP bound:     " << params.large_prime_bound << "\n";
    std::cout << "║    LP bits:      " << params.large_prime_bits << "\n";
    std::cout << "║    Sieve:        " << (params.sieve_i_max - params.sieve_i_min + 1)
              << " × " << (params.sieve_j_max - params.sieve_j_min + 1) << "\n";
    std::cout << "║    Special-Q:    [" << params.special_q_min << ", " << params.special_q_max << "]\n";
    std::cout << "║    Max SQ:       " << params.max_special_q << "\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n" << std::flush;

    // ── Phase 1: Polynomial Selection ──
    std::cout << "\n[Phase 1] Polynomial selection..." << std::flush;
    StopWatch phase;

    uint32_t degree = params.degree;
    std::optional<PolynomialContext> ctx_opt;
    try {
        ctx_opt.emplace(SelectorDispatch::select(n, degree));
    } catch (const std::exception& e) {
        std::cout << " FAILED: " << e.what() << "\n";
        result.time_sec = total.sec();
        return result;
    }
    auto& ctx = *ctx_opt;
    result.poly_sec = phase.sec();
    std::cout << " done (" << phase.ms() << " ms)\n";
    std::cout << "  m = " << ctx.m().to_string().substr(0, 60) << "\n" << std::flush;

    // ── Phase 2: Factor Base ──
    std::cout << "[Phase 2] Factor base construction..." << std::flush;
    phase.reset();

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = params.rational_bound;
    fb_opts.algebraic_bound = params.algebraic_bound;
    fb_opts.special_q_bound = params.special_q_max;
    fb_opts.parallel = true;

    auto fb = FactorBaseBuilder::build(ctx, fb_opts);
    result.fb_sec = phase.sec();
    std::cout << " done (" << std::fixed << std::setprecision(2) << phase.sec() << " sec)\n";
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
    // 3LP cofactor + filter merge (BACKLOG #1, 2026-05-21): opt-in via ENV
    // GNFS_3LP=1. Stress (50d/60d) is the primary target: lp_bits 23/26 makes
    // weight≥3 LP keys dominate, and accepting 3LP cofactors widens the LP
    // space so V0 / V3 cascade BFS find more chain merges. Default OFF for
    // baseline preservation.
    {
        const char* env = std::getenv("GNFS_3LP");
        cofac_config.allow_3lp = (env && std::atoi(env) == 1);
        if (cofac_config.allow_3lp) {
            std::cout << "[3lp] cofactorizer accepts 3LP relations (B^3 bound)\n"
                      << std::flush;
        }
    }

    Cofactorizer cofactorizer(ctx, fb, cofac_config);

    SpecialQRange sq_range;
    sq_range.min_q = params.special_q_min;
    sq_range.max_q = params.special_q_max;
    SpecialQGenerator sq_gen(fb, sq_range);

    CollectorConfig coll_config;
    coll_config.check_duplicates = true;

    // BACKLOG #1: size-aware OOC default mirrors Pipeline::sieve_and_collect.
    // 50d (lp_bits=23) / 60d (lp_bits=26) → ON by default unless GNFS_OOC_RELATIONS=0.
    // Without this, Round 2 ~900K rels OOM-killed by macOS at 50d.
    {
        const char* ooc_env = std::getenv("GNFS_OOC_RELATIONS");
        const auto policy = gnfs::relation::decide_ooc_policy(
            ooc_env, params.large_prime_bound);
        if (policy.enabled) {
            coll_config.ooc_enabled = true;
            if (const char* path_env = std::getenv("GNFS_OOC_BASE_PATH");
                path_env != nullptr && path_env[0] != '\0') {
                coll_config.ooc_base_path = path_env;
            } else {
                coll_config.ooc_base_path =
                    gnfs::util::temp_path(
                        "gnfs_stress_relations_" +
                        std::to_string(gnfs::util::process_id()));
            }
            const size_t lp_bits_est =
                gnfs::relation::estimate_lp_bits(params.large_prime_bound);
            std::fprintf(stderr,
                "[ooc] test_stress streaming relations to %s.{reldata,relidx} "
                "(%s, lp_bits=%zu)\n",
                coll_config.ooc_base_path.c_str(),
                std::string(policy.reason).c_str(), lp_bits_est);
        }
    }

    RelationCollector collector(coll_config);

    size_t matrix_cols = fb.rational_count() + fb.sieve_algebraic_count() + params.target_excess;
    size_t initial_target = params.raw_relation_target(matrix_cols);

    // Use birthday formula target directly. For 50-digit degree 4, full_count=0
    // (all relations are LP partial) so small first batches waste time on futile
    // filter+merge. The birthday formula correctly predicts the needed raw count.
    size_t batch_target = initial_target;
    size_t sq_count = 0;
    size_t full_count = 0;  // Track non-LP (fully smooth) relations

    LatticeSieve sieve(ctx, fb, sieve_params);
    sieve.set_region(sieve_region);

    // ── Adaptive sieve-filter-merge loop ──
    std::vector<Relation> relations;
    bool lp_enabled = params.large_prime_bound > params.algebraic_bound;
    constexpr int MAX_ROUNDS = 10;

    // Progress reporting: stress tests report every 100 SQs for visibility
    size_t report_interval = 100;
    size_t detailed_report_count = 10;

    for (int round = 0; round < MAX_ROUNDS; ++round) {
        while (sq_gen.has_next() && collector.size() < batch_target && sq_count < params.max_special_q) {
            auto sq = sq_gen.next();
            if (!sq) break;

            auto sr = sieve.sieve_special_q(*sq);

            // Parallel cofactorization
            {
                const auto& cands = sr.candidates;
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
                    Cofactorizer local_cofac(ctx, fb, cofac_config);
                    auto& local_rels = thread_results[tid];
                    local_rels.reserve(n_cands / (n_threads * 4));
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
                    for (auto& rel : tr) {
                        if (rel.is_full()) ++full_count;
                        collector.add(std::move(rel));
                    }
            }
            ++sq_count;

            bool should_report = (sq_count <= detailed_report_count) ||
                                 (sq_count % report_interval == 0) ||
                                 (collector.size() >= batch_target);
            if (should_report) {
                double rate = static_cast<double>(collector.size()) / (phase.sec() + 0.001);
                double full_pct = collector.size() > 0 ?
                    100.0 * static_cast<double>(full_count) /
                    static_cast<double>(collector.size()) : 0.0;
                std::cout << "  SQ #" << sq_count
                          << ": rels=" << collector.size() << "/" << batch_target
                          << " (" << std::fixed << std::setprecision(1)
                          << (100.0 * static_cast<double>(collector.size()) /
                              static_cast<double>(batch_target)) << "%)"
                          << " full=" << full_count << " (" << std::setprecision(0)
                          << full_pct << "%)"
                          << " rate=" << std::setprecision(1) << rate << "/s"
                          << " elapsed=" << std::setprecision(1) << phase.sec() << "s"
                          << "\n" << std::flush;
            }

            // Early exit: enough full relations alone to fill the matrix.
            // No LP merge needed — just full smooth relations.
            if (full_count > matrix_cols * 3 / 2) {
                std::cout << "  [Full-exit] " << full_count << " full > "
                          << (matrix_cols * 3 / 2) << " (1.5× matrix_cols)\n" << std::flush;
                break;
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
            // BACKLOG #1 diagnostic: LP-key weight histogram pre-merge.
            // 50d plateau analysis hinges on weight distribution:
            //   w1 → singletons (kill mergeability, produce LP cols)
            //   w2 → V0 mergeable sweet spot
            //   w3+ → chain-merge territory (V0_BFS/V3 cascade only)
            {
                auto hist = count_lp_key_weights(relations);
                std::cout << "  [lp_weights] unique=" << hist.unique_keys
                          << " w1=" << hist.weight_1
                          << " w2=" << hist.weight_2
                          << " w3=" << hist.weight_3
                          << " w4+=" << hist.weight_4plus << "\n" << std::flush;
            }

            auto sep = separate_relations(std::move(relations));

            // BACKLOG #1 step 13 (2026-05-19): V0_BFS size-aware default mirror
            // Pipeline::filter(). Prior PID 96718 实测 V0_BFS unused 因 test_stress
            // 走 PartialRelationMerger 不走 Pipeline. 集成 v0_bfs_policy here.
            const auto v0_bfs_policy = decide_v0_bfs_policy(
                std::getenv("GNFS_V0_BFS"), params.large_prime_bound);
            if (v0_bfs_policy.env_force_failed) {
                std::cerr << "[v0_bfs] " << v0_bfs_policy.reason << "\n";
            }

            if (v0_bfs_policy.enabled) {
                CliqueStats cstats;
                auto merged = CliqueRelationMerger::merge_cliques(
                    std::move(sep.partial), &cstats);
                std::cerr << "[v0_bfs] reason=" << v0_bfs_policy.reason
                          << " " << cstats.to_string()
                          << " merged=" << merged.size()
                          << " (V3 cascade skipped)\n";
                std::cout << "  [round " << (round+1) << "] Full=" << sep.full.size()
                          << " Merged=" << merged.size() << " (V0_BFS)"
                          << "\n" << std::flush;

                relations = std::move(sep.full);
                relations.reserve(relations.size() + merged.size());
                relations.insert(relations.end(),
                    std::make_move_iterator(merged.begin()),
                    std::make_move_iterator(merged.end()));
            } else {
                // V3 cascade prep (ENV: GNFS_CASCADE_V3=1) — clone partials before V0 consumes
                const char* v3_env = std::getenv("GNFS_CASCADE_V3");
                const bool use_v3 = v3_env != nullptr && v3_env[0] != '\0' && v3_env[0] != '0';
                std::vector<Relation> partial_copy_for_v3;
                if (use_v3) partial_copy_for_v3 = sep.partial;

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
                // Reserve full + V0 merged + V3 estimate (~3× merged worst case).
                relations.reserve(relations.size() + merged.size() * 4);
                relations.insert(relations.end(),
                    std::make_move_iterator(merged.begin()),
                    std::make_move_iterator(merged.end()));

                // V3 cascade: BFS spanning tree merge on weight≥3 LP clique components
                if (use_v3 && !partial_copy_for_v3.empty()) {
                    CliqueStats cstats;
                    auto v3_merged = CliqueRelationMerger::merge_cliques(
                        std::move(partial_copy_for_v3), &cstats);
                    std::unordered_set<
                        RelationSourceCombination,
                        RelationSourceCombinationHash> existing_keys;
                    existing_keys.reserve(relations.size());
                    for (const auto& r : relations) {
                        if (r.is_merged()) {
                            existing_keys.insert(
                                relation_source_combination(r));
                        }
                    }
                    size_t v3_added = 0;
                    for (auto& r : v3_merged) {
                        if (!r.is_merged() ||
                            existing_keys.insert(
                                relation_source_combination(r)).second) {
                            relations.push_back(std::move(r));
                            ++v3_added;
                        }
                    }
                    std::cout << "  [v3_cascade] " << cstats.to_string()
                              << " added=" << v3_added << "\n" << std::flush;
                }
            }
        }

        // Accurate LP col count via count_unique_lp_keys (filter.hpp).
        // 50d 实测: 38K usable → 24677 lp cols (64%) vs 旧 5% guess 339 (12× under).
        size_t lp_col_estimate = lp_enabled ? count_unique_lp_keys(relations) : 0;
        size_t effective_cols = matrix_cols + lp_col_estimate;
        if (lp_enabled && !relations.empty()) {
            double beta = 100.0 * static_cast<double>(lp_col_estimate) /
                          static_cast<double>(relations.size());
            std::cout << "  [LP ratio] lp_cols=" << lp_col_estimate
                      << "/" << relations.size() << " (β=" << std::fixed
                      << std::setprecision(1) << beta << "%) eff_cols="
                      << effective_cols << "\n" << std::flush;
        }

        if (relations.size() > effective_cols) {
            std::cout << "  Sieving complete: " << collector.size() << " raw, "
                      << relations.size() << " usable (need >" << effective_cols
                      << ", lp_cols=" << lp_col_estimate << "), in "
                      << phase.sec() << " sec\n" << std::flush;
            break;
        }

        if (!sq_gen.has_next() || sq_count >= params.max_special_q) {
            std::cout << "  SQ exhausted at " << sq_count << " — insufficient relations\n";
            break;
        }

        double merge_rate = (collector.size() > 0) ?
            static_cast<double>(relations.size()) / static_cast<double>(collector.size()) : 0.01;
        // Need enough usable > effective_cols (includes LP columns)
        size_t needed_usable = effective_cols + effective_cols / 10;  // +10% safety
        size_t needed_raw = static_cast<size_t>(
            static_cast<double>(needed_usable) / std::max(merge_rate, 0.001));
        // Cap raised 5× → 20× initial. 50d needs ~7M raw (effective_cols 47K,
        // merge_rate 1%) vs initial 618K → ratio 11×. Old 5× cap (3M) blocked
        // adaptive loop from reaching required target.
        batch_target = std::min(
            std::max(batch_target + batch_target / 4, needed_raw),  // +25% growth
            initial_target * 20);
        std::cout << "  Need more — usable=" << relations.size() << "/" << effective_cols
                  << " merge_rate=" << std::setprecision(3) << (merge_rate * 100)
                  << "%, new target=" << batch_target << "\n" << std::flush;
    }

    result.relations = collector.size();
    result.sieve_sec = phase.sec();

    // Release collector memory before BL — 11M+ raw relations consume ~5-6 GB,
    // causing swap thrashing during BL's random-access SpMV patterns
    collector.clear();
    std::cout << "  [Memory] Released collector (" << result.relations << " raw relations freed)\n" << std::flush;

    if (relations.size() < 5) {
        std::cout << "  INSUFFICIENT RELATIONS (" << relations.size() << " usable from "
                  << collector.size() << " raw)\n";
        result.time_sec = total.sec();
        return result;
    }

    // ── Phase 4: Relation Trimming ──
    // CRITICAL: must include LP cols in trim limit. Old `matrix_cols * 2`
    // only counted FB cols, ignoring lp_cols (up to 65% of total in 50d/lp_bits=23).
    // 50d Round 4 evidence: usable=74568, FB-only trim=45520, actual cols=64127 →
    // matrix 45520×64127 NO EXCESS. Fix: trim = effective_cols × 1.25 (25% safety).
    {
        size_t lp_cols_for_trim = lp_enabled ? count_unique_lp_keys(relations) : 0;
        size_t effective_cols_for_trim = matrix_cols + lp_cols_for_trim;
        size_t max_rels = effective_cols_for_trim + effective_cols_for_trim / 4;
        if (relations.size() > max_rels) {
            std::cout << "  [Trim] " << relations.size() << " → " << max_rels
                      << " relations (eff_cols=" << effective_cols_for_trim
                      << ", +25% safety)\n";
            std::mt19937 rng(42);
            std::shuffle(relations.begin(), relations.end(), rng);
            relations.resize(max_rels);
        }
    }

    // ── Phase 5: Linear Algebra ──
    std::cout << "[Phase 5] Matrix construction..." << std::flush;
    phase.reset();

    MatrixBuilderConfig mb_config;
    mb_config.include_sign_column = true;
    mb_config.include_qc_columns = true;
    // Class group disabled: most N have class number 1; QC+Schirokauer suffice.
    // (class_group.hpp now supports any degree — can enable for h(K)>1 fields.)
    mb_config.include_class_group = false;
    mb_config.include_schirokauer = true;
    mb_config.num_qc_primes = params.num_qc_primes;
    mb_config.qc_prime_start = 100;
    mb_config.schirokauer_primes = {2};
    mb_config.verbose = true;  // Enable verbose for diagnostics

    MatrixBuilder mb(mb_config);
    auto build_result = mb.build_with_qc(relations, fb, ctx);
    auto mstats = compute_matrix_stats(build_result.matrix);

    std::cout << " " << mstats.num_rows << "×" << mstats.num_cols
              << " (excess=" << mstats.excess << ") " << phase.sec() << " sec\n" << std::flush;

    // Print column breakdown for diagnostics
    {
        const auto& mp = build_result.mapping;
        std::cout << "  [Columns] rat_fb=" << mp.num_rational_fb
                  << " alg_fb=" << mp.num_algebraic_fb
                  << " rat_lp=" << mp.num_large_primes_rat
                  << " alg_lp=" << mp.num_large_primes_alg
                  << " qc=" << mp.num_qc_columns
                  << " cg=" << mp.num_class_group_columns
                  << " schiro=" << mp.num_schirokauer_columns
                  << " sign=" << (mp.has_sign_column ? 1 : 0)
                  << " total=" << mp.total_columns() << "\n" << std::flush;
    }

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
              << ") " << phase.sec() << " sec\n" << std::flush;

    std::cout << "  Finding dependencies..." << std::flush;
    phase.reset();
    auto deps = find_deps(sge_result.reduced_matrix, 64);

    for (auto& dep : deps) {
        dep = sge_result.expand_dependency(dep);
    }
    result.linalg_sec = phase.sec();
    std::cout << " found " << deps.size() << " (" << phase.sec() << " sec)\n" << std::flush;

    result.dependencies = deps.size();
    if (deps.empty()) {
        std::cout << "  NO DEPENDENCIES FOUND\n";
        result.time_sec = total.sec();
        return result;
    }

    // ── Verify dependencies against ORIGINAL (pre-SGE) matrix ──
    std::cout << "  Verifying dependencies against original matrix..." << std::flush;
    size_t valid_deps = 0, invalid_deps = 0;
    for (size_t di = 0; di < deps.size(); ++di) {
        size_t bad_cols = verify_null_space(deps[di], build_result.matrix);
        if (bad_cols == 0) {
            ++valid_deps;
        } else {
            ++invalid_deps;
            if (invalid_deps <= 5) {
                std::cout << "\n    Dep #" << (di+1) << ": INVALID ("
                          << bad_cols << " non-zero columns, size=" << popcnt(deps[di]) << ")";
            }
        }
    }
    std::cout << "\n  Result: " << valid_deps << " valid, " << invalid_deps << " invalid"
              << " out of " << deps.size() << "\n" << std::flush;

    if (valid_deps == 0) {
        std::cout << "  ALL DEPENDENCIES INVALID — BL or SGE bug!\n";
        // Still try sqrt for first few to see what happens
    }

    // ── Phase 6: Square Root ──
    std::cout << "[Phase 6] Square root extraction...\n" << std::flush;
    phase.reset();

    size_t max_dep_attempts = std::min(deps.size(), size_t(10));
    for (size_t di = 0; di < max_dep_attempts; ++di) {
        const auto& dep = deps[di];
        size_t dep_bad = verify_null_space(dep, build_result.matrix);
        std::cout << "  Dep #" << (di+1) << " (size=" << popcnt(dep)
                  << ", null_check=" << (dep_bad == 0 ? "OK" : "FAIL") << ")..." << std::flush;

        auto rat = compute_rational_sqrt(to_bv(dep), relations, fb, n, ctx.m());
        if (!rat.success) {
            std::cout << " rat_fail (" << rat.error << ")\n" << std::flush;
            continue;
        }

        auto alg = compute_algebraic_sqrt(to_bv(dep), relations, ctx);
        if (!alg.success) {
            std::cout << " alg_fail (" << alg.error << ")\n" << std::flush;
            continue;
        }

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
                    result.sqrt_sec = phase.sec();
                    std::cout << " SUCCESS!\n" << std::flush;
                    goto done;
                }
            }
        }
        std::cout << " trivial\n" << std::flush;
    }

    // XOR combination of dependency pairs
    if (!result.success && deps.size() >= 2) {
        std::cout << "  Trying XOR combinations...\n" << std::flush;
        size_t max_try = std::min(deps.size(), size_t(20));
        for (size_t i = 0; i < max_try && !result.success; ++i) {
            for (size_t j = i + 1; j < max_try && !result.success; ++j) {
                BitVector combined = to_bv(deps[i]);
                combined.xor_with(to_bv(deps[j]));
                if (combined.popcount() < 2) continue;

                auto rat = compute_rational_sqrt(combined, relations, fb, n, ctx.m());
                if (!rat.success) continue;

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
                            result.sqrt_sec = phase.sec();
                            std::cout << "  XOR(" << i+1 << "," << j+1 << ") SUCCESS!\n" << std::flush;
                        }
                    }
                }
            }
        }
    }

done:
    result.time_sec = total.sec();

    std::cout << "\n┌────────────────────────────────────────────────────┐\n";
    if (result.success) {
        std::cout << "│  ★ FACTORIZATION SUCCESSFUL                        │\n";
        std::cout << "│  p = " << result.factor1.to_string().substr(0, 45) << "\n";
        std::cout << "│  q = " << result.factor2.to_string().substr(0, 45) << "\n";
    } else {
        std::cout << "│  ✗ FACTORIZATION FAILED                            │\n";
        std::cout << "│  Relations: " << result.relations << "\n";
        std::cout << "│  Dependencies: " << result.dependencies << "\n";
    }
    std::cout << "│  Phase timing:                                     │\n";
    std::cout << "│    Polynomial: " << std::fixed << std::setprecision(2) << result.poly_sec << "s\n";
    std::cout << "│    Factor Base: " << result.fb_sec << "s\n";
    std::cout << "│    Sieve+Cofac: " << result.sieve_sec << "s\n";
    std::cout << "│    Linear Algebra: " << result.linalg_sec << "s\n";
    std::cout << "│    Square Root: " << result.sqrt_sec << "s\n";
    std::cout << "│    TOTAL: " << result.time_sec << "s\n";
    std::cout << "└────────────────────────────────────────────────────┘\n" << std::flush;

    return result;
}

// ============================================================
// Main
// ============================================================

int main(int argc, char** argv) {
    int min_level = 1, max_level = 2;

    if (argc >= 2) min_level = std::stoi(argv[1]);
    if (argc >= 3) max_level = std::stoi(argv[2]);
    if (min_level < 1) min_level = 1;
    if (max_level > 2) max_level = 2;
    if (max_level < min_level) max_level = min_level;

    std::cout << "════════════════════════════════════════════════════════════\n";
    std::cout << "  GNFS Stress Test\n";
    std::cout << "  Levels: " << min_level << " to " << max_level << "\n";
    std::cout << "  Level 1: 50-digit (164 bit)\n";
    std::cout << "  Level 2: 60-digit (197 bit)\n";
    std::cout << "════════════════════════════════════════════════════════════\n\n";

    auto cases = get_test_cases();
    int pass = 0, fail = 0, skip = 0;

    for (const auto& tc : cases) {
        if (tc.level < min_level || tc.level > max_level) {
            ++skip;
            continue;
        }

        std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        std::cout << "  Level " << tc.level << ": " << tc.name << "\n";
        std::cout << "  N = " << tc.n_str.substr(0, 60);
        if (tc.n_str.size() > 60) std::cout << "...";
        std::cout << "\n";
        std::cout << "  Expected: " << tc.expected_p.substr(0, 30) << "..."
                  << " × " << tc.expected_q.substr(0, 30) << "...\n";
        std::cout << "  Time estimate: " << tc.time_estimate << "\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

        Integer n(tc.n_str.c_str());
        auto result = factor_with_progress(n, tc.level);

        if (result.success) {
            Integer p1 = result.factor1.clone();
            Integer p2 = result.factor2.clone();
            if (p1.compare(p2) > 0) std::swap(p1, p2);

            Integer exp_p(tc.expected_p.c_str());
            Integer exp_q(tc.expected_q.c_str());

            bool correct = (p1.compare(exp_p) == 0 && p2.compare(exp_q) == 0) ||
                           (p1.compare(exp_q) == 0 && p2.compare(exp_p) == 0);

            if (correct) {
                std::cout << "  ✓ PASS (factors verified)\n";
                ++pass;
            } else {
                std::cout << "  ✗ WRONG FACTORS: got " << p1.to_string() << " × " << p2.to_string() << "\n";
                ++fail;
            }
        } else {
            std::cout << "  ✗ FAIL (factorization unsuccessful — expected for stress test)\n";
            ++fail;
        }
    }

    std::cout << "\n════════════════════════════════════════════════════════════\n";
    std::cout << "  Results: " << pass << " passed, " << fail << " failed, " << skip << " skipped\n";
    std::cout << "════════════════════════════════════════════════════════════\n";

    return (fail > 0) ? 1 : 0;
}

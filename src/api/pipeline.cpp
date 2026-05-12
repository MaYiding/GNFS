#include <gnfs/api/pipeline.hpp>

#include <gnfs/polynomial/selector_dispatch.hpp>
#include <gnfs/factor_base/builder.hpp>
#include <gnfs/sieve/special_q.hpp>
#include <gnfs/sieve/lattice_sieve.hpp>
#include <gnfs/cofactor/cofactorizer.hpp>
#include <gnfs/cofactor/ecm.hpp>
#include <gnfs/relation/collector.hpp>
#include <gnfs/relation/filter.hpp>
#include <gnfs/linalg/matrix_builder.hpp>
#include <gnfs/linalg/sge.hpp>
#include <gnfs/linalg/block_lanczos.hpp>
#include <gnfs/linalg/block_wiedemann.hpp>
#include <gnfs/sqrt/rational_sqrt.hpp>
#include <gnfs/sqrt/algebraic_sqrt.hpp>
#include <gnfs/siqs/siqs.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <random>
#include <string>
#include <thread>

namespace gnfs::api {

// ============================================================
// Fast path: trial division + Pollard rho for small N
// ============================================================

namespace {

/// Trial division up to limit. Returns factor or 0.
uint64_t trial_divide(const Integer& n, uint64_t limit) {
    // Small primes
    if (mpz_divisible_ui_p(n.get_mpz(), 2)) return 2;
    if (mpz_divisible_ui_p(n.get_mpz(), 3)) return 3;
    // 6k±1 wheel
    for (uint64_t i = 5; i <= limit; i += 6) {
        if (mpz_divisible_ui_p(n.get_mpz(), i)) return i;
        if (mpz_divisible_ui_p(n.get_mpz(), i + 2)) return i + 2;
    }
    return 0;
}

// ── Fast 2-limb Pollard rho using GMP mpn_ (N ≤ 2^128) ──
// Uses GMP's optimized assembly for 2-limb arithmetic, bypassing mpz_t overhead.
// ~5-8× faster than mpz-based rho for 65-128 bit numbers.

/// 2-limb Pollard rho. Returns factor as uint64, or 0 if not found.
uint64_t pollard_rho_mpn2(const Integer& n, size_t max_iters) {
    size_t n_size = mpz_size(n.get_mpz());
    if (n_size > 2 || n_size == 0) return 0;

    mp_limb_t N[2] = {mpz_getlimbn(n.get_mpz(), 0),
                       n_size > 1 ? mpz_getlimbn(n.get_mpz(), 1) : 0};

    // n_actual_size: 1 or 2 limbs
    mp_size_t nn = (N[1] != 0) ? 2 : 1;

    // Modular square: r = a^2 mod N, where a is nn limbs
    // product = a^2 (2*nn limbs), then tdiv_qr to get remainder
    mp_limb_t prod[4], quot[3]; // max sizes for 2-limb operations
    auto sqrmod = [&](mp_limb_t* r, const mp_limb_t* a) {
        mpn_sqr(prod, a, nn);
        mpn_tdiv_qr(quot, r, 0, prod, 2 * nn, N, nn);
    };

    // Modular multiply: r = a * b mod N
    auto mulmod = [&](mp_limb_t* r, const mp_limb_t* a, const mp_limb_t* b) {
        mpn_mul_n(prod, a, b, nn);
        mpn_tdiv_qr(quot, r, 0, prod, 2 * nn, N, nn);
    };

    // Add mod: r = (a + b) mod N
    auto addmod = [&](mp_limb_t* r, const mp_limb_t* a, const mp_limb_t* b) {
        mp_limb_t carry = mpn_add_n(r, a, b, nn);
        if (carry || mpn_cmp(r, N, nn) >= 0) {
            mpn_sub_n(r, r, N, nn);
        }
    };

    // Sub absolute: r = |a - b|
    auto sub_abs = [&](mp_limb_t* r, const mp_limb_t* a, const mp_limb_t* b) {
        if (mpn_cmp(a, b, nn) >= 0)
            mpn_sub_n(r, a, b, nn);
        else
            mpn_sub_n(r, b, a, nn);
    };

    // GCD with N: compute gcd(a, N), return as uint64 if small
    mpz_t g_mpz, a_mpz, n_mpz;
    mpz_init(g_mpz); mpz_init(a_mpz); mpz_init(n_mpz);
    mpz_import(n_mpz, nn, -1, sizeof(mp_limb_t), 0, 0, N);
    auto gcd_with_n = [&](const mp_limb_t* a) -> uint64_t {
        mpz_import(a_mpz, nn, -1, sizeof(mp_limb_t), 0, 0, a);
        mpz_gcd(g_mpz, a_mpz, n_mpz);
        if (mpz_cmp_ui(g_mpz, 1) > 0 && mpz_cmp(g_mpz, n_mpz) < 0) {
            return mpz_get_ui(g_mpz);
        }
        return (mpz_cmp_ui(g_mpz, 1) > 0) ? 1 : 0; // 1 = factor but doesn't fit uint64
    };

    // (is_one not needed — GCD check handles all cases)

    // RNG
    uint64_t seed = 42;
    auto rng_next = [](uint64_t& s) -> uint64_t {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s;
    };

    mp_limb_t y[2], c[2], x[2], ys[2], q_acc[2], diff[2];
    size_t total_iters = 0;  // Track total iterations across all attempts

    for (int attempt = 0; attempt < 20 && total_iters < max_iters; attempt++) {
        y[0] = rng_next(seed); y[1] = 0;
        c[0] = rng_next(seed); c[1] = 0;
        if (c[0] == 0) c[0] = 1;
        // Reduce y, c mod N
        if (nn == 2) {
            mp_limb_t tmpq[2];
            mpn_tdiv_qr(tmpq, y, 0, y, nn, N, nn);
            mpn_tdiv_qr(tmpq, c, 0, c, nn, N, nn);
        } else {
            y[0] %= N[0]; c[0] %= N[0];
        }

        q_acc[0] = 1; q_acc[1] = 0;
        size_t r_val = 1;
        bool found = false;

        while (!found && total_iters < max_iters) {
            x[0] = y[0]; x[1] = y[1];

            // Phase 1: advance y by r steps
            for (size_t i = 0; i < r_val; i++) {
                sqrmod(y, y);
                addmod(y, y, c);
            }

            // Phase 2: accumulate product in batches
            size_t k = 0;
            while (k < r_val && !found) {
                ys[0] = y[0]; ys[1] = y[1];
                size_t batch = std::min(size_t(128), r_val - k);
                for (size_t i = 0; i < batch; i++) {
                    sqrmod(y, y);
                    addmod(y, y, c);
                    sub_abs(diff, x, y);
                    if (diff[0] == 0 && (nn < 2 || diff[1] == 0)) continue;
                    mulmod(q_acc, q_acc, diff);
                }
                // Check GCD
                uint64_t g = gcd_with_n(q_acc);
                if (g > 1) {
                    // Backtrack to find exact factor
                    for (size_t bt = 0; bt < 256; bt++) {
                        sqrmod(ys, ys);
                        addmod(ys, ys, c);
                        sub_abs(diff, x, ys);
                        g = gcd_with_n(diff);
                        if (g > 1) {
                            mpz_clear(g_mpz); mpz_clear(a_mpz); mpz_clear(n_mpz);
                            return g;
                        }
                    }
                    // g == n case: reset and retry
                    q_acc[0] = 1; q_acc[1] = 0;
                    break;
                }
                k += batch;
                total_iters += batch;
            }
            r_val *= 2;
        }
    }

    mpz_clear(g_mpz); mpz_clear(a_mpz); mpz_clear(n_mpz);
    return 0;
}

/// Pollard rho with Brent improvement. Works on GMP integers.
/// Returns a non-trivial factor or Integer(0) if not found within max_iters.
Integer pollard_rho_brent(const Integer& n, size_t max_iters = 1000000) {
    if (n <= Integer(3)) return Integer(0);

    // Use GMP directly for speed
    mpz_t y, c, m, g, r, q, x, ys, tmp;
    mpz_init(y); mpz_init(c); mpz_init(m); mpz_init(g);
    mpz_init(r); mpz_init(q); mpz_init(x); mpz_init(ys); mpz_init(tmp);

    gmp_randstate_t state;
    gmp_randinit_mt(state);
    gmp_randseed_ui(state, 42);

    const mpz_t& n_mpz = *reinterpret_cast<const mpz_t*>(&n.get_mpz());
    Integer result(0);

    for (int attempt = 0; attempt < 20 && result.compare(Integer(0)) == 0; ++attempt) {
        mpz_urandomm(y, state, n_mpz);
        mpz_urandomm(c, state, n_mpz);
        if (mpz_sgn(c) == 0) mpz_set_ui(c, 1);
        mpz_set_ui(m, 128);
        mpz_set_ui(g, 1);
        mpz_set_ui(q, 1);
        mpz_set_ui(r, 1);

        size_t iters = 0;

        while (mpz_cmp_ui(g, 1) == 0 && iters < max_iters) {
            mpz_set(x, y);
            unsigned long r_val = mpz_get_ui(r);
            for (unsigned long i = 0; i < r_val; ++i) {
                // y = (y*y + c) mod n
                mpz_mul(tmp, y, y);
                mpz_add(tmp, tmp, c);
                mpz_mod(y, tmp, n_mpz);
            }

            size_t k = 0;
            while (k < r_val && mpz_cmp_ui(g, 1) == 0) {
                mpz_set(ys, y);
                unsigned long m_val = mpz_get_ui(m);
                unsigned long batch = std::min(m_val, r_val - static_cast<unsigned long>(k));
                for (unsigned long i = 0; i < batch; ++i) {
                    // y = (y*y + c) mod n
                    mpz_mul(tmp, y, y);
                    mpz_add(tmp, tmp, c);
                    mpz_mod(y, tmp, n_mpz);
                    // q = q * |x - y| mod n
                    mpz_sub(tmp, x, y);
                    mpz_abs(tmp, tmp);
                    mpz_mul(tmp, q, tmp);
                    mpz_mod(q, tmp, n_mpz);
                }
                mpz_gcd(g, q, n_mpz);
                k += batch;
                iters += batch;
            }

            mpz_mul_ui(r, r, 2);
        }

        if (mpz_cmp(g, n_mpz) == 0) {
            // Backtrack: replay individual steps to isolate factor.
            // Bounded to 256 iterations (> max batch of 128) as safety guard.
            for (size_t bt = 0; bt < 256; ++bt) {
                mpz_mul(tmp, ys, ys);
                mpz_add(tmp, tmp, c);
                mpz_mod(ys, tmp, n_mpz);
                mpz_sub(tmp, x, ys);
                mpz_abs(tmp, tmp);
                mpz_gcd(g, tmp, n_mpz);
                if (mpz_cmp_ui(g, 1) > 0) break;
            }
        }

        if (mpz_cmp_ui(g, 1) > 0 && mpz_cmp(g, n_mpz) < 0) {
            mpz_set(result.get_mpz(), g);
        }
    }

    mpz_clear(y); mpz_clear(c); mpz_clear(m); mpz_clear(g);
    mpz_clear(r); mpz_clear(q); mpz_clear(x); mpz_clear(ys); mpz_clear(tmp);
    gmp_randclear(state);

    return result;
}

} // anonymous namespace

// ============================================================
// Method Selection
// ============================================================

std::pair<FactorizationMethod, std::string>
Pipeline::select_method(size_t n_bits, size_t n_digits,
                        std::optional<FactorizationMethod> override) {
    // Manual override
    if (override && *override != FactorizationMethod::Auto) {
        return {*override, "user specified"};
    }

    // Auto selection cascade:
    //
    // Trial division: always first (catches factors ≤ 10^6)
    // Pollard rho: ≤30 digits (≤100 bits) — O(p^{1/2}), fast for balanced ≤30d
    // ECM+SIQS: 25-100 digits — ECM tried first (O(exp(√(2·ln p·ln ln p)))),
    //           SIQS fallback (O(L_N(1/2,1)))
    // GNFS: 101+ digits — O(L_N(1/3,c)), with SIQS probe ≤100d
    //
    // Key insight: ECM depends on smallest factor p, not N.
    // For balanced k-digit semiprimes, p ≈ k/2 digits.
    // ECM beats SIQS up to ~55d (where factors are ~27d).

    if (n_digits <= 6 || n_bits <= 20) {
        return {FactorizationMethod::TrialDivision,
                std::to_string(n_digits) + "d/" + std::to_string(n_bits) +
                "bit: trial division sufficient"};
    }

    if (n_digits <= 24 || n_bits <= 80) {
        return {FactorizationMethod::PollardRho,
                std::to_string(n_digits) + "d/" + std::to_string(n_bits) +
                "bit: Pollard rho O(p^{1/2}) efficient"};
    }

    if (n_digits <= 100) {
        // 25-100d: rho quick probe → ECM → SIQS cascade
        return {FactorizationMethod::SIQS,
                std::to_string(n_digits) + "d: rho+ECM+SIQS cascade"};
    }

    return {FactorizationMethod::GNFS,
            std::to_string(n_digits) + "d: GNFS O(L_N(1/3,c)) required"};
}

// ============================================================
// Construction
// ============================================================

Pipeline::Pipeline(const Integer& n, const Config& config)
    : n_(n.clone())
    , config_(config)
    , params_(config.apply_to(n))
    , start_time_(std::chrono::high_resolution_clock::now())
{
    stats_.n_bits = n.bit_length();
    stats_.n_digits = params_.digits;
    stats_.degree = params_.degree;
    stats_.rational_bound = params_.rational_bound;
    stats_.algebraic_bound = params_.algebraic_bound;
    stats_.large_prime_bound = params_.large_prime_bound;
}

// ============================================================
// Progress / Log helpers
// ============================================================

double Pipeline::elapsed_s() const {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double>(now - start_time_).count();
}

void Pipeline::emit_progress(Phase phase, const std::string& msg, double phase_progress) {
    if (!progress_cb_) return;
    ProgressInfo info;
    info.phase = phase;
    info.phase_progress = phase_progress;
    info.elapsed_s = elapsed_s();
    info.message = msg;
    info.relations_found = stats_.relations_found;
    info.relations_target = 0;
    info.special_q_done = stats_.special_q_processed;
    info.matrix_rows = stats_.matrix_rows;
    info.matrix_cols = stats_.matrix_cols;
    info.dependency_index = stats_.dependencies_tried;
    info.dependencies_total = static_cast<int>(stats_.dependencies_found);
    progress_cb_(info);
}

void Pipeline::emit_log(LogLevel level, Phase phase, const std::string& msg) {
    if (!log_cb_) return;
    LogEntry entry;
    entry.level = level;
    entry.phase = phase;
    entry.timestamp_s = elapsed_s();
    entry.message = msg;
    log_cb_(entry);
}

// ============================================================
// Phase 1: Polynomial Selection
// ============================================================

PolynomialContext Pipeline::select_polynomial() {
    emit_progress(Phase::PolynomialSelection, "Starting polynomial selection");
    emit_log(LogLevel::Info, Phase::PolynomialSelection,
             "N=" + n_.to_string() + " bits=" + std::to_string(stats_.n_bits) +
             " degree=" + std::to_string(params_.degree));

    auto t0 = std::chrono::high_resolution_clock::now();

    bool verbose = config_.verbose.value_or(false);
    auto ctx = polynomial::SelectorDispatch::select(n_, params_.degree, verbose);

    auto t1 = std::chrono::high_resolution_clock::now();
    stats_.timings.poly_s = std::chrono::duration<double>(t1 - t0).count();

    emit_log(LogLevel::Info, Phase::PolynomialSelection,
             "m=" + ctx.m().to_string() + " time=" +
             std::to_string(stats_.timings.poly_s) + "s");
    emit_progress(Phase::PolynomialSelection, "Polynomial selected", 1.0);

    return ctx;
}

// ============================================================
// Phase 2: Factor Base Construction
// ============================================================

FactorBase Pipeline::build_factor_base(const PolynomialContext& ctx) {
    emit_progress(Phase::FactorBase, "Building factor base");

    auto t0 = std::chrono::high_resolution_clock::now();

    factor_base::FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = params_.rational_bound;
    fb_opts.algebraic_bound = params_.algebraic_bound;
    fb_opts.special_q_bound = params_.special_q_max;
    fb_opts.large_prime_bound = params_.large_prime_bound;
    fb_opts.parallel = true;

    auto fb = factor_base::FactorBaseBuilder::build(ctx, fb_opts);

    auto t1 = std::chrono::high_resolution_clock::now();
    stats_.timings.fb_s = std::chrono::duration<double>(t1 - t0).count();
    stats_.rational_primes = fb.rational_count();
    stats_.algebraic_primes = fb.algebraic_count();

    emit_log(LogLevel::Info, Phase::FactorBase,
             "rational=" + std::to_string(fb.rational_count()) +
             " algebraic=" + std::to_string(fb.algebraic_count()) +
             " (sieve=" + std::to_string(fb.sieve_algebraic_count()) + ")");
    emit_progress(Phase::FactorBase, "Factor base built", 1.0);

    return fb;
}

// ============================================================
// Phase 3: Sieving and Relation Collection
// ============================================================

std::vector<Relation> Pipeline::sieve_and_collect(
        const PolynomialContext& ctx, const FactorBase& fb) {
    emit_progress(Phase::Sieving, "Starting sieve");

    auto t0 = std::chrono::high_resolution_clock::now();

    // Sieve params
    sieve::SieveParams sieve_params;
    sieve_params.rational_threshold = params_.rational_threshold;
    sieve_params.algebraic_threshold = params_.algebraic_threshold;

    sieve::SieveRegion sieve_region;
    sieve_region.i_min = params_.sieve_i_min;
    sieve_region.i_max = params_.sieve_i_max;
    sieve_region.j_min = params_.sieve_j_min;
    sieve_region.j_max = params_.sieve_j_max;

    // Cofactorizer
    cofactor::CofactorizerConfig cofac_config;
    cofac_config.large_prime_bound = fb.params().large_prime_bound;
    cofac_config.allow_1lp = true;
    // 2LP requires SQUFOF which is expensive for large LP ranges.
    // Only enable for ≥50 digits where the LP key space is large enough to benefit.
    cofac_config.allow_2lp = (params_.digits >= 50);

    cofactor::Cofactorizer cofactorizer(ctx, fb, cofac_config);

    // Special-Q generator
    sieve::SpecialQRange sq_range;
    sq_range.min_q = params_.special_q_min;
    sq_range.max_q = params_.special_q_max;
    sieve::SpecialQGenerator sq_gen(fb, sq_range);

    // Collector
    relation::CollectorConfig coll_config;
    coll_config.check_duplicates = true;
    relation::RelationCollector collector(coll_config);
    // CLAUDE.md 强制约定:拒绝 gcd(a-bm, N)>1 的关系
    collector.set_polynomial_context(ctx.n(), ctx.m());

    // Target
    size_t matrix_cols = fb.rational_count() + fb.sieve_algebraic_count() + params_.target_excess;
    size_t initial_target = params_.raw_relation_target(matrix_cols);
    size_t batch_target = initial_target;
    bool lp_enabled = params_.large_prime_bound > params_.algebraic_bound;

    emit_log(LogLevel::Info, Phase::Sieving,
             "target=" + std::to_string(initial_target) +
             " matrix_cols=" + std::to_string(matrix_cols) +
             " sq_range=[" + std::to_string(sq_range.min_q) +
             "," + std::to_string(sq_range.max_q) + "]");

    // Create sieve
    sieve::LatticeSieve sieve_obj(ctx, fb, sieve_params);
    sieve_obj.set_region(sieve_region);

    size_t sq_count = 0;
    size_t candidates_total = 0;
    size_t max_sq = params_.max_special_q;

    // Adaptive sieve-filter-merge loop:
    // Collect raw relations, filter+merge, check if enough usable.
    // If not, increase target and continue sieving.
    std::vector<Relation> relations;
    constexpr int MAX_ROUNDS = 10;

    // Thread count for parallel cofactorization
    size_t n_cofac_threads = std::thread::hardware_concurrency();
    if (n_cofac_threads == 0) n_cofac_threads = 4;

    for (int round = 0; round < MAX_ROUNDS; ++round) {
        // ── Batch SQ processing: sieve + cofac in parallel ──
        // Collect a batch of SQ primes, sieve them in parallel (each thread
        // owns its own LatticeSieve copy), then cofac results in parallel.
        while (sq_gen.has_next() && collector.size() < batch_target && sq_count < max_sq) {
            // Collect a batch of SQs for parallel processing
            // Batch size: balance parallelism vs memory (each thread allocates sieve array)
            // For ≤50 digit: 4 parallel SQs (moderate FB, ~200MB total)
            // For >50 digit: 2 parallel SQs (large FB, high memory per thread)
            size_t SQ_BATCH_SIZE = (params_.digits <= 50) ? 4 : 2;
            std::vector<sieve::SpecialQ> sq_batch;
            sq_batch.reserve(SQ_BATCH_SIZE);
            while (sq_batch.size() < SQ_BATCH_SIZE && sq_gen.has_next() && sq_count < max_sq) {
                auto sq = sq_gen.next();
                if (!sq) break;
                sq_batch.push_back(*sq);
            }
            if (sq_batch.empty()) break;

            // Parallel sieve: each thread gets its own LatticeSieve + Cofactorizer
            std::vector<std::vector<Relation>> batch_relations(sq_batch.size());
            std::vector<size_t> batch_candidates(sq_batch.size(), 0);
            std::atomic<size_t> next_sq_idx{0};

            auto sieve_worker = [&]() {
                sieve::LatticeSieve local_sieve(ctx, fb, sieve_params);
                local_sieve.set_region(sieve_region);
                cofactor::Cofactorizer local_cofac(ctx, fb, cofac_config);

                while (true) {
                    size_t idx = next_sq_idx.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= sq_batch.size()) break;

                    auto sieve_result = local_sieve.sieve_special_q(sq_batch[idx]);
                    batch_candidates[idx] = sieve_result.candidates.size();

                    // Cofactorize all candidates for this SQ
                    auto& local_rels = batch_relations[idx];
                    for (const auto& cand : sieve_result.candidates) {
                        auto rel = local_cofac.verify(cand, sq_batch[idx].q, sq_batch[idx].r);
                        if (rel) local_rels.push_back(std::move(*rel));
                    }
                }
            };

            // Launch worker threads (one per SQ, capped at hardware threads)
            size_t n_workers = std::min(n_cofac_threads, sq_batch.size());
            std::vector<std::thread> threads;
            threads.reserve(n_workers);
            for (size_t t = 0; t < n_workers; ++t)
                threads.emplace_back(sieve_worker);
            for (auto& t : threads) t.join();

            // Collect results
            for (size_t i = 0; i < sq_batch.size(); ++i) {
                candidates_total += batch_candidates[i];
                for (auto& rel : batch_relations[i])
                    collector.add(std::move(rel));
            }
            sq_count += sq_batch.size();

            // Progress report
            if (sq_count % params_.progress_interval == 0 || sq_count <= 8) {
                auto now = std::chrono::high_resolution_clock::now();
                double elapsed = std::chrono::duration<double>(now - t0).count();
                size_t rels_per_sec = (elapsed > 0.01) ?
                    static_cast<size_t>(collector.size() / elapsed) : 0;
                double pct = static_cast<double>(collector.size()) /
                             static_cast<double>(batch_target);
                emit_progress(Phase::Sieving,
                    "SQ=" + std::to_string(sq_count) + " rels=" +
                    std::to_string(collector.size()) +
                    " " + std::to_string(rels_per_sec) + "/s",
                    std::min(pct, 1.0));

                stats_.relations_found = collector.size();
                stats_.special_q_processed = sq_count;
            }
        }

        if (collector.size() < 10) break;

        // Filter + merge to check usable relation count
        relations = collector.get_relations();

        relation::FilterConfig filter_config;
        filter_config.remove_singletons = true;
        filter_config.max_passes = 10;
        relation::RelationFilter rel_filter(filter_config);
        relations = rel_filter.filter(std::move(relations));

        if (lp_enabled) {
            auto sep = relation::separate_relations(std::move(relations));
            relation::PartialRelationMerger::MergeStats mstats;
            auto merged = relation::PartialRelationMerger::merge_all(
                std::move(sep.partial), 10, &mstats);
            relations = std::move(sep.full);
            relations.insert(relations.end(),
                std::make_move_iterator(merged.begin()),
                std::make_move_iterator(merged.end()));
        }

        // Check: enough usable relations?
        if (relations.size() > matrix_cols) break;

        // Not enough — increase target and continue if SQs available
        if (!sq_gen.has_next() || sq_count >= max_sq) break;

        double merge_rate = (collector.size() > 0) ?
            static_cast<double>(relations.size()) / static_cast<double>(collector.size()) : 0.01;
        size_t needed_raw = static_cast<size_t>(
            static_cast<double>(matrix_cols * 2) / std::max(merge_rate, 0.001));
        // Raise cap: for low merge rates (~2%), need up to 100× initial target
        batch_target = std::min(
            std::max(batch_target * 2, needed_raw),
            initial_target * 100);  // generous cap for low merge rates

        emit_log(LogLevel::Info, Phase::Sieving,
                 "round " + std::to_string(round + 1) + ": usable=" +
                 std::to_string(relations.size()) + "/" + std::to_string(matrix_cols) +
                 " merge_rate=" + std::to_string(merge_rate) +
                 " new_target=" + std::to_string(batch_target));
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    stats_.timings.sieve_s = std::chrono::duration<double>(t1 - t0).count();

    // Collect final stats
    stats_.relations_found = collector.size();
    auto coll_stats = collector.stats();
    stats_.full_relations = coll_stats.full_relations;
    stats_.partial_1lp = coll_stats.partial_1lp;
    stats_.partial_2lp = coll_stats.partial_2lp;
    stats_.special_q_processed = sq_count;
    stats_.candidates_total = candidates_total;

    emit_log(LogLevel::Info, Phase::Sieving,
             "done: sq=" + std::to_string(sq_count) +
             " raw=" + std::to_string(collector.size()) +
             " usable=" + std::to_string(relations.size()) +
             " (full=" + std::to_string(coll_stats.full_relations) +
             " 1lp=" + std::to_string(coll_stats.partial_1lp) +
             " 2lp=" + std::to_string(coll_stats.partial_2lp) + ")");
    emit_progress(Phase::Sieving, "Sieving complete", 1.0);

    return relations;
}

// ============================================================
// Phase 4: Filtering
// ============================================================

std::vector<Relation> Pipeline::filter(std::vector<Relation> relations) {
    emit_progress(Phase::Filtering, "Filtering relations");

    auto t0 = std::chrono::high_resolution_clock::now();

    // Singleton filtering
    relation::FilterConfig filter_config;
    filter_config.remove_singletons = true;
    filter_config.max_passes = 10;

    relation::RelationFilter rel_filter(filter_config);
    relations = rel_filter.filter(std::move(relations));

    stats_.singletons_removed = rel_filter.stats().singletons_removed;

    // LP merge (only when LP is genuinely enabled)
    if (params_.large_prime_bound > params_.algebraic_bound) {
        auto sep = relation::separate_relations(std::move(relations));

        relation::PartialRelationMerger::MergeStats mstats;
        auto merged = relation::PartialRelationMerger::merge_all(
            std::move(sep.partial), 10, &mstats);

        stats_.merged_relations = merged.size();

        emit_log(LogLevel::Info, Phase::Filtering,
                 "merge: full=" + std::to_string(sep.full.size()) +
                 " 1lp=" + std::to_string(mstats.input_1lp) +
                 " 2lp=" + std::to_string(mstats.input_2lp) +
                 " merged=" + std::to_string(merged.size()));

        // Only keep full + merged — unmerged partials create singleton LP columns
        relations = std::move(sep.full);
        relations.insert(relations.end(),
            std::make_move_iterator(merged.begin()),
            std::make_move_iterator(merged.end()));
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    stats_.timings.filter_s = std::chrono::duration<double>(t1 - t0).count();
    stats_.relations_after_filter = relations.size();

    emit_log(LogLevel::Info, Phase::Filtering,
             "after filter: " + std::to_string(relations.size()) + " relations");
    emit_progress(Phase::Filtering, "Filtering complete", 1.0);

    return relations;
}

// ============================================================
// Phase 5: Linear Algebra
// ============================================================

Pipeline::MatrixResult Pipeline::solve_matrix(
        std::vector<Relation> relations,
        const FactorBase& fb,
        const PolynomialContext& ctx) {
    emit_progress(Phase::LinearAlgebra, "Building matrix");

    auto t0 = std::chrono::high_resolution_clock::now();

    // Matrix builder config
    linalg::MatrixBuilderConfig mb_config;
    mb_config.include_sign_column = true;
    mb_config.include_qc_columns = true;
    mb_config.include_class_group = false;
    mb_config.include_schirokauer = true;
    mb_config.num_qc_primes = params_.num_qc_primes;
    mb_config.qc_prime_start = 100;
    mb_config.schirokauer_primes = {2};  // Only ℓ=2 for GF(2) matrix
    mb_config.verbose = false;

    linalg::MatrixBuilder mb(mb_config);
    auto build_result = mb.build_with_qc(relations, fb, ctx);

    auto matrix_stats = linalg::compute_matrix_stats(build_result.matrix);
    stats_.matrix_rows = matrix_stats.num_rows;
    stats_.matrix_cols = matrix_stats.num_cols;
    stats_.matrix_weight = matrix_stats.total_weight;
    stats_.matrix_excess = static_cast<int64_t>(matrix_stats.excess);

    emit_log(LogLevel::Info, Phase::LinearAlgebra,
             "matrix: " + std::to_string(matrix_stats.num_rows) + "x" +
             std::to_string(matrix_stats.num_cols) +
             " excess=" + std::to_string(matrix_stats.excess));

    if (!matrix_stats.has_excess()) {
        emit_log(LogLevel::Error, Phase::LinearAlgebra, "No excess — not enough relations");
        MatrixResult mr;
        mr.matrix = std::move(build_result.matrix);
        mr.relations = std::move(relations);
        return mr;
    }

    // Trim excess rows to improve BL convergence and SGE effectiveness.
    // High excess (>1.3×) causes: (1) BL A-gram persistent non-invertible columns
    // (E/F corrections can only look back 2 steps, so persistent rank deficiency
    // causes orthogonality breakdown); (2) SGE ineffectiveness (avg column weight
    // is too high for w1/w2 elimination).
    // Target: 1.1× cols for optimal SGE + BL. CADO-NFS typically uses 5-10% excess.
    if (matrix_stats.num_rows > static_cast<size_t>(matrix_stats.num_cols * 1.3)) {
        size_t target_rows = static_cast<size_t>(matrix_stats.num_cols * 1.1);
        emit_log(LogLevel::Info, Phase::LinearAlgebra,
                 "Trimming excess: " + std::to_string(matrix_stats.num_rows) +
                 " rows -> " + std::to_string(target_rows) +
                 " (keep " + std::to_string(target_rows) + "/" +
                 std::to_string(matrix_stats.num_rows) + ")");

        // Shuffle and trim relations, then rebuild matrix
        std::mt19937 rng(42);
        std::shuffle(relations.begin(), relations.end(), rng);
        relations.resize(target_rows);

        auto build2 = mb.build_with_qc(relations, fb, ctx);
        build_result.matrix = std::move(build2.matrix);
        auto ms2 = linalg::compute_matrix_stats(build_result.matrix);
        stats_.matrix_rows = ms2.num_rows;
        stats_.matrix_cols = ms2.num_cols;
        stats_.matrix_weight = ms2.total_weight;
        stats_.matrix_excess = static_cast<int64_t>(ms2.excess);

        emit_log(LogLevel::Info, Phase::LinearAlgebra,
                 "Trimmed matrix: " + std::to_string(ms2.num_rows) + "x" +
                 std::to_string(ms2.num_cols) +
                 " excess=" + std::to_string(ms2.excess));
    }

    // SGE preprocessing
    emit_progress(Phase::LinearAlgebra, "SGE preprocessing");
    linalg::SGEConfig sge_config;
    sge_config.verbose = false;
    auto sge_result = linalg::SGE::preprocess(build_result.matrix, sge_config);

    emit_log(LogLevel::Debug, Phase::LinearAlgebra,
             "SGE: " + std::to_string(build_result.matrix.num_rows()) + "x" +
             std::to_string(build_result.matrix.num_cols()) + " -> " +
             std::to_string(sge_result.reduced_matrix.num_rows()) + "x" +
             std::to_string(sge_result.reduced_matrix.num_cols()));

    // Block Lanczos (primary solver)
    emit_progress(Phase::LinearAlgebra, "Block Lanczos");
    linalg::BlockLanczos bl_solver;
    auto dependencies = bl_solver.find_dependencies(sge_result.reduced_matrix);

    // If BL/Gaussian didn't find deps, use streaming Block Wiedemann.
    // BW works for any matrix size with O(m) memory.
    if (dependencies.empty()) {
        emit_log(LogLevel::Warn, Phase::LinearAlgebra,
                 "Block Lanczos returned 0 deps, trying Block Wiedemann");
        emit_progress(Phase::LinearAlgebra, "Block Wiedemann (fallback)");
        linalg::BlockWiedemann bw_solver;
        dependencies = bw_solver.find_dependencies(sge_result.reduced_matrix);
    }

    // Expand dependencies back to original matrix
    for (auto& dep : dependencies) {
        dep = sge_result.expand_dependency(dep);
    }

    stats_.dependencies_found = dependencies.size();

    auto t1 = std::chrono::high_resolution_clock::now();
    stats_.timings.linalg_s = std::chrono::duration<double>(t1 - t0).count();

    emit_log(LogLevel::Info, Phase::LinearAlgebra,
             "deps=" + std::to_string(dependencies.size()) +
             " time=" + std::to_string(stats_.timings.linalg_s) + "s");
    emit_progress(Phase::LinearAlgebra, "Linear algebra complete", 1.0);

    MatrixResult mr;
    mr.matrix = std::move(build_result.matrix);
    mr.dependencies = std::move(dependencies);
    mr.relations = std::move(relations);
    return mr;
}

// ============================================================
// Phase 6+7: Square Root and Factor Extraction
// ============================================================

// Helper: convert vector<bool> to BitVector
static linalg::BitVector to_bitvector(const std::vector<bool>& vec) {
    linalg::BitVector bv(vec.size());
    for (size_t i = 0; i < vec.size(); ++i) {
        if (vec[i]) bv.set(i);
    }
    return bv;
}

// Helper: verify dependency (XOR of selected rows = zero)
static bool verify_dependency(const SparseMatrix& mat, const std::vector<bool>& dep) {
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

FactorResult Pipeline::extract_factors(
        const MatrixResult& mr,
        const FactorBase& fb,
        const PolynomialContext& ctx) {
    emit_progress(Phase::SquareRoot, "Starting factor extraction");

    auto t0_sqrt = std::chrono::high_resolution_clock::now();

    FactorResult result;
    result.n = n_.clone();
    result.stats = stats_;

    if (mr.dependencies.empty()) {
        emit_log(LogLevel::Error, Phase::SquareRoot, "No dependencies to try");
        result.stats.timings.total_s = elapsed_s();
        return result;
    }

    auto is_nontrivial = [this](const Integer& f) -> bool {
        if (f.fits_uint64() && f.to_uint64() == 1) return false;
        if (f.compare(n_) == 0) return false;
        return true;
    };

    auto try_factor = [&](const Integer& rat_sqrt, const Integer& alg_value) -> bool {
        auto factors = sqrt::extract_factors(rat_sqrt, alg_value, n_);

        if (is_nontrivial(factors.factor1)) {
            Integer f1 = factors.factor1.clone();
            Integer f2 = n_.clone();
            f2 /= f1;
            Integer check = f1.clone();
            check *= f2;
            if (check.compare(n_) == 0 && is_nontrivial(f2)) {
                result.factors.push_back(std::move(f1));
                result.factors.push_back(std::move(f2));
                result.success = true;
                return true;
            }
        }
        if (is_nontrivial(factors.factor2)) {
            Integer f1 = factors.factor2.clone();
            Integer f2 = n_.clone();
            f2 /= f1;
            Integer check = f1.clone();
            check *= f2;
            if (check.compare(n_) == 0 && is_nontrivial(f2)) {
                result.factors.push_back(std::move(f1));
                result.factors.push_back(std::move(f2));
                result.success = true;
                return true;
            }
        }
        return false;
    };

    // Try each dependency
    for (size_t dep_idx = 0; dep_idx < mr.dependencies.size() && !result.success; ++dep_idx) {
        const auto& dep = mr.dependencies[dep_idx];
        stats_.dependencies_tried = static_cast<int>(dep_idx + 1);

        emit_progress(Phase::SquareRoot,
            "Trying dependency " + std::to_string(dep_idx + 1) + "/" +
            std::to_string(mr.dependencies.size()),
            static_cast<double>(dep_idx) / static_cast<double>(mr.dependencies.size()));

        if (!verify_dependency(mr.matrix, dep)) continue;

        auto bv = to_bitvector(dep);

        // Rational sqrt
        auto rat_result = sqrt::compute_rational_sqrt(bv, mr.relations, fb, n_, ctx.m());
        if (!rat_result.success) continue;

        // Algebraic sqrt
        auto alg_result = sqrt::compute_algebraic_sqrt(bv, mr.relations, ctx);
        Integer alg_value = alg_result.success ? alg_result.value.clone() : Integer(1);

        // Try Y
        if (try_factor(rat_result.value, alg_value)) break;

        // Try -Y
        Integer alg_neg = n_.clone();
        alg_neg -= alg_value;
        if (try_factor(rat_result.value, alg_neg)) break;
    }

    // If no single dep worked, try XOR pairs
    if (!result.success && mr.dependencies.size() >= 2) {
        emit_progress(Phase::FactorExtraction, "Trying XOR combinations");

        size_t limit = std::min(mr.dependencies.size(), size_t(20));
        for (size_t i = 0; i < limit && !result.success; ++i) {
            for (size_t j = i + 1; j < limit && !result.success; ++j) {
                linalg::BitVector combined = to_bitvector(mr.dependencies[i]);
                combined.xor_with(to_bitvector(mr.dependencies[j]));
                if (combined.popcount() < 2) continue;

                // Convert back to vector<bool> for verify
                std::vector<bool> combined_vec(mr.matrix.num_rows(), false);
                for (size_t k = 0; k < mr.matrix.num_rows(); ++k) {
                    if (combined.test(k)) combined_vec[k] = true;
                }
                if (!verify_dependency(mr.matrix, combined_vec)) continue;

                auto rat_result = sqrt::compute_rational_sqrt(combined, mr.relations, fb, n_, ctx.m());
                if (!rat_result.success) continue;

                auto alg_result = sqrt::compute_algebraic_sqrt(combined, mr.relations, ctx);
                Integer alg_val = alg_result.success ? alg_result.value.clone() : Integer(1);

                if (try_factor(rat_result.value, alg_val)) break;

                Integer neg = n_.clone();
                neg -= alg_val;
                if (try_factor(rat_result.value, neg)) break;
            }
        }
    }

    auto t1_sqrt = std::chrono::high_resolution_clock::now();
    stats_.timings.sqrt_s = std::chrono::duration<double>(t1_sqrt - t0_sqrt).count();

    // Sort factors ascending
    if (result.factors.size() == 2 && result.factors[0].compare(result.factors[1]) > 0) {
        std::swap(result.factors[0], result.factors[1]);
    }

    result.stats = stats_;
    result.stats.timings.total_s = elapsed_s();

    if (result.success) {
        emit_log(LogLevel::Info, Phase::FactorExtraction,
                 "SUCCESS: " + result.factors[0].to_string() + " * " +
                 result.factors[1].to_string());
    } else {
        emit_log(LogLevel::Warn, Phase::FactorExtraction, "No non-trivial factor found");
    }
    emit_progress(Phase::Done, result.success ? "Factorization succeeded" : "Failed", 1.0);

    return result;
}

// ============================================================
// Run: complete pipeline
// ============================================================

FactorResult Pipeline::run() {
    // Input validation.
    // Adaptive Miller-Rabin reps: 5 for small N (fast on trial-division path),
    // 15 for large N (target 2^-30 error rate for crypto-grade composites).
    const int prime_reps = (stats_.n_bits <= 64) ? 5 : 15;
    if (mpz_probab_prime_p(n_.get_mpz(), prime_reps) > 0) {
        emit_log(LogLevel::Error, Phase::PolynomialSelection,
                 "N is prime or probably prime");
        FactorResult r;
        r.n = n_.clone();
        r.stats = stats_;
        r.stats.timings.total_s = elapsed_s();
        return r;
    }
    if (n_ <= Integer(1)) {
        FactorResult r;
        r.n = n_.clone();
        r.stats = stats_;
        return r;
    }

    // Check for perfect power
    if (mpz_perfect_power_p(n_.get_mpz())) {
        // exp upper bound: for any perfect power n = b^e with b ≥ 2,
        // we have e ≤ log2(n) = n_bits. Capping at 64 missed N = 2^65 etc.
        const unsigned long exp_max = static_cast<unsigned long>(stats_.n_bits);
        for (unsigned long exp = 2; exp <= exp_max; ++exp) {
            Integer root;
            if (mpz_root(root.get_mpz(), n_.get_mpz(), exp)) {
                Integer check;
                mpz_pow_ui(check.get_mpz(), root.get_mpz(), exp);
                if (check.compare(n_) == 0) {
                    FactorResult r;
                    r.n = n_.clone();
                    r.success = true;
                    r.factors.push_back(root.clone());
                    r.factors.push_back(n_.clone());
                    r.factors[1] /= root;
                    r.stats = stats_;
                    r.stats.timings.total_s = elapsed_s();
                    r.stats.method_used = FactorizationMethod::TrialDivision;
                    r.stats.method_reason = "perfect power";
                    emit_log(LogLevel::Info, Phase::PolynomialSelection,
                             "Perfect power detected: " + root.to_string() + "^" +
                             std::to_string(exp));
                    return r;
                }
            }
        }
    }

    // ── Method selection ──
    auto [method, reason] = select_method(
        stats_.n_bits, stats_.n_digits, config_.method);
    stats_.method_used = method;
    stats_.method_reason = reason;
    emit_log(LogLevel::Info, Phase::PolynomialSelection,
             "Method: " + std::string(method_name(method)) + " (" + reason + ")");

    // Helper: build result from a found factor
    auto make_fast_result = [this](const Integer& f1,
                                    FactorizationMethod m,
                                    const std::string& m_reason) -> FactorResult {
        FactorResult r;
        r.n = n_.clone();
        r.success = true;
        Integer f2 = n_.clone();
        f2 /= f1;
        if (f1.compare(f2) <= 0) {
            r.factors.push_back(f1.clone());
            r.factors.push_back(std::move(f2));
        } else {
            r.factors.push_back(f2.clone());
            r.factors.push_back(f1.clone());
        }
        r.stats = stats_;
        r.stats.method_used = m;
        r.stats.method_reason = m_reason;
        r.stats.timings.total_s = elapsed_s();
        return r;
    };

    // ── Phase 0: Trial division (always, instant) ──
    // For ≤12d: thorough trial to 10^6 (factors might be 6-digit).
    // For >12d: quick trial to 10^4 only (rho/SIQS catches the rest).
    // Saves ~3ms of overhead for medium/large N.
    {
        uint64_t td_limit = (stats_.n_digits <= 12) ? 1000000 : 10000;
        uint64_t small_f = trial_divide(n_, td_limit);
        if (small_f > 0) {
            Integer f1(small_f);
            emit_log(LogLevel::Info, Phase::PolynomialSelection,
                     "Trial division found factor: " + std::to_string(small_f));
            return make_fast_result(f1, FactorizationMethod::TrialDivision,
                                   "factor ≤ 10^6");
        }
    }

    // If user forced trial-only, stop here
    if (method == FactorizationMethod::TrialDivision) {
        FactorResult r;
        r.n = n_.clone();
        r.stats = stats_;
        r.stats.timings.total_s = elapsed_s();
        return r;
    }

    // ── Phase 1: Pollard rho for small N or quick unbalanced detection ──
    // For N ≤ 128 bits: use mpn2 rho (~12ns/iter).
    // For N > 128 bits: quick GMP rho probe (50K iters) for unbalanced semiprimes.
    // Skip entirely for ≥40d/≥128bit when SIQS is the target (saves ~30ms overhead).
    if (method == FactorizationMethod::PollardRho ||
        ((method == FactorizationMethod::SIQS || method == FactorizationMethod::GNFS) &&
         stats_.n_bits <= 128)) {

        // Fast mpn-based rho: N ≤ 128 bits (~38 digits)
        // Uses GMP mpn_ assembly (no mpz_t overhead): ~8-12ns/iter vs ~30ns/iter
        // Iteration budget: O(p^{1/2}) where p is smallest factor.
        // For balanced k-digit semiprime, p ≈ 10^{k/2}, so iters ≈ 10^{k/4}.
        // Keep budget modest — ECM handles what rho misses.
        if (stats_.n_bits <= 128) {
            // Quick rho probe: catch easy/unbalanced factors.
            // For balanced semiprimes, ECM is usually faster.
            // Budget: generous when rho is target method, minimal when SIQS will handle it.
            // For balanced semiprimes ≥25d, rho needs O(10^{d/4}) iters — too slow.
            // Keep budget small to catch unbalanced cases only.
            size_t rho_limit;
            bool siqs_target = (method == FactorizationMethod::SIQS ||
                                method == FactorizationMethod::GNFS);
            if (stats_.n_bits <= 40)        rho_limit = 100000;     // ~1ms
            else if (stats_.n_bits <= 50)   rho_limit = siqs_target ? 100000 : 200000;
            else if (stats_.n_bits <= 64)   rho_limit = siqs_target ? 200000 : 500000;
            else if (stats_.n_bits <= 80)   rho_limit = siqs_target ? 100000 : 1000000;
            else                            rho_limit = siqs_target ? 50000  : 1000000;

            uint64_t f128 = pollard_rho_mpn2(n_, rho_limit);
            if (f128 > 1) {
                Integer f1(f128);
                if (f1.compare(n_) != 0) {
                    emit_log(LogLevel::Info, Phase::PolynomialSelection,
                             "mpn2 rho found factor: " + std::to_string(f128));
                    return make_fast_result(f1, FactorizationMethod::PollardRho,
                                           "mpn2 rho (≤128bit)");
                }
            }
        }

        // GMP rho: only when user explicitly forced PollardRho method
        if (method == FactorizationMethod::PollardRho) {
            Integer rho_f = pollard_rho_brent(n_, 100000000);
            if (rho_f > Integer(1) && rho_f.compare(n_) != 0) {
                emit_log(LogLevel::Info, Phase::PolynomialSelection,
                         "Pollard rho found factor: " + rho_f.to_string());
                return make_fast_result(rho_f, FactorizationMethod::PollardRho,
                                       "GMP rho fallback");
            }
        }
    }

    // If user forced rho-only, stop here
    if (method == FactorizationMethod::PollardRho) {
        FactorResult r;
        r.n = n_.clone();
        r.stats = stats_;
        r.stats.timings.total_s = elapsed_s();
        return r;
    }

    // ── Phase 1.5: ECM for medium N (factor-size dependent) ──
    // ECM complexity depends on smallest factor p, not N.
    // For balanced k-digit semiprimes, p ≈ k/2 digits.
    // ECM with appropriate B1 finds factors up to ~35 digits efficiently.
    // Run ECM before SIQS for N ≤ 100 digits (factors ≤ ~50 digits).
    // ECM probe: minimal probe for 25-28d to catch unbalanced semiprimes.
    // For balanced semiprimes, ECM is too slow — SIQS handles them faster.
    // Keep ECM cost ≤ 2ms total to minimize overhead.
    if (stats_.n_digits >= 26 && stats_.n_digits <= 28 &&
        method != FactorizationMethod::TrialDivision) {
        size_t expected_factor_bits = stats_.n_bits / 2;

        // Minimal ECM probe: 3 curves at low B1. Cost: ~1ms total.
        cofactor::ECM::Config ecm_config;
        ecm_config.auto_params = false;
        if (expected_factor_bits <= 50) {
            ecm_config.B1 = 2000; ecm_config.B2 = 50000; ecm_config.num_curves = 3;
        } else {
            ecm_config.B1 = 0; ecm_config.num_curves = 0;
        }

        // Skip ECM if configured with 0 curves
        if (ecm_config.num_curves > 0) {
        emit_log(LogLevel::Info, Phase::PolynomialSelection,
                 "ECM probe: " + std::to_string(stats_.n_digits) + "d N, "
                 "expected factor ~" + std::to_string(expected_factor_bits) + " bits, "
                 "B1=" + std::to_string(ecm_config.B1) +
                 " curves=" + std::to_string(ecm_config.num_curves));

        auto ecm_t0 = std::chrono::high_resolution_clock::now();
        auto ecm_f = cofactor::ECM::factor(n_, ecm_config);
        auto ecm_t1 = std::chrono::high_resolution_clock::now();
        double ecm_ms = std::chrono::duration<double, std::milli>(ecm_t1 - ecm_t0).count();

        if (ecm_f && *ecm_f > Integer(1) && ecm_f->compare(n_) != 0) {
            emit_log(LogLevel::Info, Phase::PolynomialSelection,
                     "ECM found factor in " + std::to_string(ecm_ms) + "ms: " + ecm_f->to_string());
            return make_fast_result(*ecm_f, FactorizationMethod::SIQS,
                                   "ECM found factor (B1=" + std::to_string(ecm_config.B1) +
                                   ", " + std::to_string(ecm_ms) + "ms)");
        }
        } // if (ecm_config.num_curves > 0)
    }

    // ── Phase 2: SIQS for medium N ──
    if (method == FactorizationMethod::SIQS ||
        (method == FactorizationMethod::GNFS && stats_.n_digits <= 100)) {
        emit_log(LogLevel::Info, Phase::PolynomialSelection,
                 "Trying SIQS for " + std::to_string(stats_.n_digits) + "-digit N");

        // Adaptive timeout: generous for forced SIQS, bounded for GNFS-with-SIQS-probe
        size_t siqs_timeout;
        if (method == FactorizationMethod::SIQS) {
            // User selected SIQS: give it plenty of time
            if (stats_.n_digits <= 50)      siqs_timeout = 60;
            else if (stats_.n_digits <= 60) siqs_timeout = 300;
            else if (stats_.n_digits <= 70) siqs_timeout = 900;
            else if (stats_.n_digits <= 80) siqs_timeout = 1800;
            else if (stats_.n_digits <= 90) siqs_timeout = 3600;
            else                            siqs_timeout = 7200;
        } else {
            // Auto/GNFS: SIQS as quick probe before GNFS
            if (stats_.n_digits <= 50)      siqs_timeout = 30;
            else if (stats_.n_digits <= 60) siqs_timeout = 120;
            else if (stats_.n_digits <= 70) siqs_timeout = 300;
            else if (stats_.n_digits <= 80) siqs_timeout = 900;
            else                            siqs_timeout = 3600;
        }

        auto siqs_result = siqs::factor(n_, siqs_timeout, true);
        if (siqs_result) {
            emit_log(LogLevel::Info, Phase::PolynomialSelection,
                     "SIQS found factor: " + siqs_result->factor1.to_string() +
                     " * " + siqs_result->factor2.to_string());

            FactorResult r;
            r.success = true;
            r.n = n_.clone();
            auto f1 = siqs_result->factor1.clone();
            auto f2 = siqs_result->factor2.clone();
            if (f1 > f2) std::swap(f1, f2);
            r.factors.push_back(std::move(f1));
            r.factors.push_back(std::move(f2));
            r.stats = stats_;
            r.stats.method_used = FactorizationMethod::SIQS;
            r.stats.method_reason = std::to_string(stats_.n_digits) + "d SIQS";
            r.stats.timings.total_s = elapsed_s();
            return r;
        }

        if (method == FactorizationMethod::SIQS) {
            // User forced SIQS only — don't fall through to GNFS
            emit_log(LogLevel::Warn, Phase::PolynomialSelection,
                     "SIQS failed (timeout=" + std::to_string(siqs_timeout) + "s)");
            FactorResult r;
            r.n = n_.clone();
            r.stats = stats_;
            r.stats.timings.total_s = elapsed_s();
            return r;
        }

        emit_log(LogLevel::Info, Phase::PolynomialSelection,
                 "SIQS failed, falling back to GNFS");
    }

    // ── Phase 3: Full GNFS pipeline ──
    stats_.method_used = FactorizationMethod::GNFS;
    stats_.method_reason = std::to_string(stats_.n_digits) + "d GNFS";

    auto ctx = select_polynomial();
    auto fb = build_factor_base(ctx);
    auto relations = sieve_and_collect(ctx, fb);

    size_t matrix_cols = fb.rational_count() + fb.sieve_algebraic_count() + params_.target_excess;

    if (relations.size() <= matrix_cols) {
        emit_log(LogLevel::Error, Phase::Sieving,
                 "Not enough usable relations: " + std::to_string(relations.size()) +
                 " <= " + std::to_string(matrix_cols) + " (need excess for BL)");
        FactorResult r;
        r.n = n_.clone();
        r.stats = stats_;
        r.stats.timings.total_s = elapsed_s();
        return r;
    }

    auto mr = solve_matrix(std::move(relations), fb, ctx);
    return extract_factors(mr, fb, ctx);
}

} // namespace gnfs::api

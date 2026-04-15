#include <gnfs/api/pipeline.hpp>

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

namespace gnfs::api {

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
    cofac_config.allow_2lp = true;

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

    // Target
    size_t matrix_cols = fb.rational_count() + fb.sieve_algebraic_count() + params_.target_excess;
    size_t target_relations = params_.raw_relation_target(matrix_cols);

    emit_log(LogLevel::Info, Phase::Sieving,
             "target=" + std::to_string(target_relations) +
             " sq_range=[" + std::to_string(sq_range.min_q) +
             "," + std::to_string(sq_range.max_q) + "]");

    // Create sieve
    sieve::LatticeSieve sieve_obj(ctx, fb, sieve_params);
    sieve_obj.set_region(sieve_region);

    size_t sq_count = 0;
    size_t candidates_total = 0;
    size_t max_sq = params_.max_special_q;

    while (sq_gen.has_next() && collector.size() < target_relations && sq_count < max_sq) {
        auto sq = sq_gen.next();
        if (!sq) break;

        auto sieve_result = sieve_obj.sieve_special_q(*sq);
        candidates_total += sieve_result.candidates.size();

        for (const auto& cand : sieve_result.candidates) {
            auto rel_opt = cofactorizer.verify(cand);
            if (rel_opt) {
                collector.add(std::move(*rel_opt));
            }
        }

        ++sq_count;

        // Progress report
        if (sq_count % params_.progress_interval == 0) {
            double pct = static_cast<double>(collector.size()) /
                         static_cast<double>(target_relations);
            emit_progress(Phase::Sieving,
                "SQ#" + std::to_string(sq_count) + " rels=" +
                std::to_string(collector.size()) + "/" +
                std::to_string(target_relations),
                std::min(pct, 1.0));

            stats_.relations_found = collector.size();
            stats_.special_q_processed = sq_count;
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    stats_.timings.sieve_s = std::chrono::duration<double>(t1 - t0).count();

    // Collect final stats
    auto coll_stats = collector.stats();
    stats_.relations_found = collector.size();
    stats_.full_relations = coll_stats.full_relations;
    stats_.partial_1lp = coll_stats.partial_1lp;
    stats_.partial_2lp = coll_stats.partial_2lp;
    stats_.special_q_processed = sq_count;
    stats_.candidates_total = candidates_total;

    emit_log(LogLevel::Info, Phase::Sieving,
             "done: sq=" + std::to_string(sq_count) +
             " rels=" + std::to_string(collector.size()) +
             " (full=" + std::to_string(coll_stats.full_relations) +
             " 1lp=" + std::to_string(coll_stats.partial_1lp) +
             " 2lp=" + std::to_string(coll_stats.partial_2lp) + ")");
    emit_progress(Phase::Sieving, "Sieving complete", 1.0);

    return collector.get_relations();
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

    // Block Lanczos
    emit_progress(Phase::LinearAlgebra, "Block Lanczos");
    linalg::BlockLanczos solver;
    auto dependencies = solver.find_dependencies(sge_result.reduced_matrix);

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
    // Input validation
    if (mpz_probab_prime_p(n_.get_mpz(), 25) > 0) {
        emit_log(LogLevel::Error, Phase::PolynomialSelection,
                 "N is prime or probably prime");
        FactorResult r;
        r.n = n_.clone();
        r.stats.timings.total_s = elapsed_s();
        return r;
    }
    if (n_ <= Integer(1)) {
        FactorResult r;
        r.n = n_.clone();
        return r;
    }

    // Check for perfect power
    // mpz_perfect_power_p returns non-zero if n is a perfect power
    if (mpz_perfect_power_p(n_.get_mpz())) {
        // Try small roots
        for (unsigned long exp = 2; exp <= 64; ++exp) {
            Integer root;
            if (mpz_root(root.get_mpz(), n_.get_mpz(), exp)) {
                // Verify
                Integer check;
                mpz_pow_ui(check.get_mpz(), root.get_mpz(), exp);
                if (check.compare(n_) == 0) {
                    FactorResult r;
                    r.n = n_.clone();
                    r.success = true;
                    r.factors.push_back(root.clone());
                    r.factors.push_back(n_.clone());
                    r.factors[1] /= root;
                    r.stats.timings.total_s = elapsed_s();
                    emit_log(LogLevel::Info, Phase::PolynomialSelection,
                             "Perfect power detected: " + root.to_string() + "^" +
                             std::to_string(exp));
                    return r;
                }
            }
        }
    }

    auto ctx = select_polynomial();
    auto fb = build_factor_base(ctx);
    auto relations = sieve_and_collect(ctx, fb);

    if (relations.size() < 10) {
        emit_log(LogLevel::Error, Phase::Sieving, "Not enough relations collected");
        FactorResult r;
        r.n = n_.clone();
        r.stats = stats_;
        r.stats.timings.total_s = elapsed_s();
        return r;
    }

    relations = filter(std::move(relations));

    if (relations.size() < 5) {
        emit_log(LogLevel::Error, Phase::Filtering, "Not enough relations after filtering");
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

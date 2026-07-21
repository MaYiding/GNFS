#pragma once

#include "config.hpp"
#include "progress.hpp"
#include "result.hpp"

#include "../core/integer.hpp"
#include "../core/params.hpp"
#include "../core/polynomial_context.hpp"
#include "../core/relation.hpp"
#include "../factor_base/builder.hpp"
#include "../linalg/sparse_matrix.hpp"
#include "../relation/reduction_engine.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace gnfs::api {

using core::Integer;
using core::GNFSParams;
using core::PolynomialContext;
using core::Relation;
using factor_base::FactorBase;
using linalg::SparseMatrix;

/// Mid-level API: step-by-step pipeline control
///
/// Usage:
///   Pipeline pipeline(n, config);
///   pipeline.set_progress_callback(my_callback);
///   auto ctx = pipeline.select_polynomial();
///   auto fb = pipeline.build_factor_base(ctx);
///   auto reduced = pipeline.sieve_and_collect(ctx, fb);
///   auto matrix_result = pipeline.solve_matrix(std::move(reduced), fb, ctx);
///   auto result = pipeline.extract_factors(matrix_result, fb, ctx);
class Pipeline {
public:
    Pipeline(const Integer& n, const Config& config = {});

    // Phase access
    PolynomialContext select_polynomial();
    FactorBase build_factor_base(const PolynomialContext& ctx);
    relation::RelationReductionResult sieve_and_collect(const PolynomialContext& ctx,
                                                        const FactorBase& fb);
    relation::RelationReductionResult filter(std::vector<Relation> relations);

    struct MatrixResult {
        SparseMatrix matrix;
        std::vector<std::vector<bool>> dependencies;
        std::vector<Relation> relations;  // relations used (order matches matrix rows)
    };
    MatrixResult solve_matrix(relation::RelationReductionResult reduction, const FactorBase& fb,
                              const PolynomialContext& ctx);

    FactorResult extract_factors(const MatrixResult& mr,
                                 const FactorBase& fb,
                                 const PolynomialContext& ctx);

    /// Run complete pipeline end-to-end
    FactorResult run();

    /// Select the best factorization method for the given N.
    /// Returns (method, reason_string).
    static std::pair<FactorizationMethod, std::string>
    select_method(size_t n_bits, size_t n_digits,
                  std::optional<FactorizationMethod> override = std::nullopt);

    // Configuration
    void set_progress_callback(ProgressCallback cb) { progress_cb_ = std::move(cb); }
    void set_log_callback(LogCallback cb) { log_cb_ = std::move(cb); }

    const GNFSParams& params() const { return params_; }
    const FactorStats& stats() const { return stats_; }

private:
    Integer n_;
    Config config_;
    GNFSParams params_;
    FactorStats stats_;

    ProgressCallback progress_cb_;
    LogCallback log_cb_;

    std::chrono::high_resolution_clock::time_point start_time_;

    // Helpers
    uint64_t allocate_relation_generation();
    void emit_progress(Phase phase, const std::string& msg,
                       double phase_progress = -1.0);
    void emit_log(LogLevel level, Phase phase, const std::string& msg);
    double elapsed_s() const;

    uint64_t next_relation_generation_ = 1;
};

} // namespace gnfs::api

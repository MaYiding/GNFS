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
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gnfs::relation {
class RelationCorpus;
}

namespace gnfs::linalg {
struct SGEResult;
}

namespace gnfs::api {

namespace detail {

/// Parse the opt-in structured-stage telemetry gate without consulting process
/// state. Only unset, exact "0", and exact "1" are accepted.
[[nodiscard]] bool parse_structured_filter_stage_telemetry(const char* raw_value);

/// Requested output base reserved for one logical relation generation.
/// RelationSink adds its private lease suffix to the requested base. The raw
/// collector prefix is reduced directly, so no generation-scoped working
/// corpus is required.
struct StructuredOOCGenerationPaths final {
    std::string output_requested_base;

    [[nodiscard]] bool operator==(const StructuredOOCGenerationPaths&) const noexcept = default;
};

/// Immutable path namespace captured once for one sieve invocation.
/// No directory or relation artifact is created by this value object.
struct StructuredOOCRunPaths final {
    std::string raw_base_path;
    std::string run_identity;
    std::string run_namespace;

    [[nodiscard]] StructuredOOCGenerationPaths generation_paths(uint64_t logical_generation) const;

    [[nodiscard]] bool operator==(const StructuredOOCRunPaths&) const noexcept = default;
};

/// Freeze the configured raw base (or the process-local temp default) and bind
/// it to a stable, path-safe identity for later generation-specific derivation.
[[nodiscard]] StructuredOOCRunPaths
make_structured_ooc_run_paths(std::optional<std::string> configured_raw_base_path,
                              std::string run_identity);

/// Strict boundary between BL/BW solver coordinates and the original matrix.
/// All reduced shapes are checked before SGE validates and expands the
/// transform once for the complete solver batch.
[[nodiscard]] std::vector<std::vector<bool>>
expand_solver_dependencies_checked(const linalg::SGEResult& sge_result,
                                   const std::vector<std::vector<bool>>& reduced_dependencies,
                                   size_t expected_matrix_rows);

/// Form one square-root fallback candidate in full matrix coordinates.
/// Dependencies with fewer than two selected rows are intentionally skipped.
[[nodiscard]] std::optional<std::vector<bool>>
xor_dependency_pair_checked(const std::vector<bool>& lhs, const std::vector<bool>& rhs,
                            size_t expected_matrix_rows);

} // namespace detail

using core::GNFSParams;
using core::Integer;
using core::PolynomialContext;
using core::Relation;
using factor_base::FactorBase;
using linalg::SparseMatrix;

inline constexpr uint32_t DEFAULT_ADAPTIVE_SIEVE_ROUND_LIMIT = 10;

/// Ownership policy for the terminal raw ordinary-OOC corpus on the legacy
/// reduction route. The production default retains the finalized artifacts.
enum class LegacyRawOOCCleanupPolicy : uint8_t {
    RetainArtifacts,
    RemoveArtifacts,
};

/// Per-invocation collection controls for reproducible phase experiments.
///
/// `adaptive_round_limit` is the absolute exclusive upper bound used by the
/// existing round index, so the default preserves checkpoint/resume behavior.
struct SieveCollectionOptions final {
    uint32_t adaptive_round_limit = DEFAULT_ADAPTIVE_SIEVE_ROUND_LIMIT;
    LegacyRawOOCCleanupPolicy legacy_raw_ooc_cleanup = LegacyRawOOCCleanupPolicy::RetainArtifacts;
};

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
                                                        const FactorBase& fb,
                                                        SieveCollectionOptions options = {});
    relation::RelationReductionResult filter(std::vector<Relation> relations);

    struct MatrixResult {
    private:
        struct StructuredRelations;
        // Declared before the public payload so reverse destruction keeps the
        // structured corpus alive until matrix/dependency state is gone.
        std::unique_ptr<StructuredRelations> structured_relations_;

    public:
        MatrixResult();
        MatrixResult(const MatrixResult&) = delete;
        MatrixResult& operator=(const MatrixResult&) = delete;
        MatrixResult(MatrixResult&&) noexcept;
        MatrixResult& operator=(MatrixResult&&) noexcept;
        ~MatrixResult();

        SparseMatrix matrix;
        std::vector<std::vector<bool>> dependencies;
        // Legacy vector route only. Structured results retain their immutable
        // source corpus plus the final matrix-row mapping instead.
        std::vector<Relation> relations;

        /// Number of relations used by the final matrix.
        [[nodiscard]] size_t relation_count() const;

        [[nodiscard]] bool owns_relation_corpus() const noexcept;

        /// Final matrix row -> source corpus ordinal mapping. Empty for the
        /// legacy vector route.
        [[nodiscard]] std::span<const size_t> structured_row_to_relation() const noexcept;

    private:
        void retain_structured_relations(relation::RelationCorpus&& corpus,
                                         std::vector<size_t> row_to_relation);
        [[nodiscard]] const relation::RelationCorpus& structured_corpus() const;

        friend class Pipeline;
    };
    /// Build and deterministically trim the final full matrix, then return
    /// before SGE or a dependency solver is entered.
    MatrixResult build_matrix(relation::RelationReductionResult&& reduction, const FactorBase& fb,
                              const PolynomialContext& ctx);
    MatrixResult solve_matrix(relation::RelationReductionResult&& reduction, const FactorBase& fb,
                              const PolynomialContext& ctx);

    FactorResult extract_factors(const MatrixResult& mr, const FactorBase& fb,
                                 const PolynomialContext& ctx);

    /// Run complete pipeline end-to-end
    FactorResult run();

    /// Select the best factorization method for the given N.
    /// Returns (method, reason_string).
    static std::pair<FactorizationMethod, std::string>
    select_method(size_t n_bits, size_t n_digits,
                  std::optional<FactorizationMethod> override = std::nullopt);

    // Configuration
    void set_progress_callback(ProgressCallback cb) {
        progress_cb_ = std::move(cb);
    }
    void set_log_callback(LogCallback cb) {
        log_cb_ = std::move(cb);
    }

    const GNFSParams& params() const {
        return params_;
    }
    const FactorStats& stats() const {
        return stats_;
    }

private:
    struct StructuredRouteSnapshot final {
        relation::StructuredFilterPolicyDecision policy{};
        std::string resume_base_path;
        std::optional<detail::StructuredOOCRunPaths> ooc_paths;
        std::string ooc_reason;
        std::string distributed_base_path;
        bool large_primes_enabled = false;
        bool ooc_enabled = false;
        size_t distributed_workers = 0;
        size_t distributed_sq_per_worker = 0;
        bool distributed_size_gate_ok = false;
        bool distributed_force_small = false;
        bool distributed_route_selected = false;
        std::uint64_t distributed_worker_timeout_ms = 0;
        bool stage_telemetry_enabled = false;
    };

    Integer n_;
    Config config_;
    GNFSParams params_;
    FactorStats stats_;

    ProgressCallback progress_cb_;
    LogCallback log_cb_;
    size_t relations_target_ = 0;

    std::chrono::high_resolution_clock::time_point start_time_;

    // Helpers
    uint64_t allocate_relation_generation();
    [[nodiscard]] StructuredRouteSnapshot capture_structured_route_snapshot() const;
    PolynomialContext select_polynomial_impl(const std::string& resume_base);
    FactorBase build_factor_base_impl(const PolynomialContext& ctx, const std::string& resume_base);
    relation::RelationReductionResult
    sieve_and_collect_impl(const PolynomialContext& ctx, const FactorBase& fb,
                           const StructuredRouteSnapshot& structured_route,
                           SieveCollectionOptions options);
    static void refresh_relation_corpus_checked(relation::RelationCorpus& corpus,
                                                const relation::CorpusDigest& expected,
                                                const char* mismatch_message);
    MatrixResult matrix_phase(relation::RelationReductionResult& reduction, const FactorBase& fb,
                              const PolynomialContext& ctx, bool solve_dependencies);
    void emit_progress(Phase phase, const std::string& msg, double phase_progress = -1.0);
    void emit_log(LogLevel level, Phase phase, const std::string& msg);
    double elapsed_s() const;

    uint64_t next_relation_generation_ = 1;
};

} // namespace gnfs::api

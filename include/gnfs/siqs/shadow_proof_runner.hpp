#pragma once

/// @file shadow_proof_runner.hpp
/// @brief Bounded, read-only SIQS shadow proof orchestration.

#include <gnfs/core/integer.hpp>
#include <gnfs/siqs/live_sieve_capture.hpp>
#include <gnfs/siqs/post_merge_dependency.hpp>
#include <gnfs/siqs/relation.hpp>
#include <gnfs/siqs/shadow_assembly.hpp>
#include <gnfs/siqs/shadow_matrix.hpp>
#include <gnfs/siqs/two_large_prime_adapter.hpp>
#include <gnfs/siqs/two_large_prime_graph.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace gnfs::siqs {

using std::size_t;

inline constexpr size_t SIQS_SHADOW_PROOF_DEFAULT_MAX_RAW_RELATIONS = 32'768;
inline constexpr size_t SIQS_SHADOW_PROOF_DEFAULT_MAX_RAW_PAYLOAD_BYTES =
    size_t{64} * size_t{1024} * size_t{1024};
inline constexpr size_t SIQS_SHADOW_PROOF_DEFAULT_MAX_GRAPH_EDGES = 16'384;
inline constexpr size_t SIQS_SHADOW_PROOF_DEFAULT_MAX_GRAPH_CYCLES = 4'096;
inline constexpr size_t SIQS_SHADOW_PROOF_DEFAULT_MAX_GRAPH_INCIDENCES = 262'144;
inline constexpr size_t SIQS_SHADOW_PROOF_DEFAULT_MAX_ROW_CANDIDATES = 4'096;
inline constexpr size_t SIQS_SHADOW_PROOF_DEFAULT_MAX_PRETRIM_ROWS = 4'096;
inline constexpr uint32_t SIQS_SHADOW_PROOF_FINGERPRINT_SCHEMA_VERSION = 1;

enum class SIQSShadowProofStage : uint8_t {
    not_started,
    input_validation,
    payload_accounting,
    adapter_preflight,
    graph_preflight,
    assembly,
    matrix,
    dependency_verification,
    factor_extraction,
    complete,
};

enum class SIQSShadowProofTerminalStatus : uint8_t {
    factor_found,
    no_factor,
    bounded_fallback,
    invalid_input,
    stage_failure,
    resource_exhausted,
    exception_failure,
    internal_invariant_failure,
};

enum class SIQSShadowProofFallbackReason : uint8_t {
    none,
    raw_relation_limit,
    raw_payload_limit,
    graph_edge_limit,
    graph_cycle_limit,
    graph_incidence_limit,
    row_candidate_limit,
    pretrim_row_limit,
    insufficient_rows,
    matrix_resource_limit,
    matrix_backend_unavailable,
};

struct SIQSShadowProofLimits {
    size_t max_raw_relations = SIQS_SHADOW_PROOF_DEFAULT_MAX_RAW_RELATIONS;
    size_t max_raw_payload_bytes = SIQS_SHADOW_PROOF_DEFAULT_MAX_RAW_PAYLOAD_BYTES;
    TwoLargePrimeCycleBasisLimits graph{
        SIQS_SHADOW_PROOF_DEFAULT_MAX_GRAPH_EDGES,
        SIQS_SHADOW_PROOF_DEFAULT_MAX_GRAPH_CYCLES,
        SIQS_SHADOW_PROOF_DEFAULT_MAX_GRAPH_INCIDENCES,
    };
    size_t max_row_candidates = SIQS_SHADOW_PROOF_DEFAULT_MAX_ROW_CANDIDATES;
    size_t max_pretrim_rows = SIQS_SHADOW_PROOF_DEFAULT_MAX_PRETRIM_ROWS;
    size_t minimum_row_excess = 1;

    [[nodiscard]] friend constexpr bool operator==(const SIQSShadowProofLimits& lhs,
                                                   const SIQSShadowProofLimits& rhs) noexcept {
        return lhs.max_raw_relations == rhs.max_raw_relations &&
               lhs.max_raw_payload_bytes == rhs.max_raw_payload_bytes &&
               lhs.graph.max_edges == rhs.graph.max_edges &&
               lhs.graph.max_cycles == rhs.graph.max_cycles &&
               lhs.graph.max_cycle_incidences == rhs.graph.max_cycle_incidences &&
               lhs.max_row_candidates == rhs.max_row_candidates &&
               lhs.max_pretrim_rows == rhs.max_pretrim_rows &&
               lhs.minimum_row_excess == rhs.minimum_row_excess;
    }
};

struct SIQSShadowProofOptions {
    SIQSShadowProofLimits limits{};
    SIQSShadowAssemblyOptions assembly{};
    SIQSShadowMatrixOptions matrix{};

    [[nodiscard]] friend constexpr bool operator==(const SIQSShadowProofOptions&,
                                                   const SIQSShadowProofOptions&) = default;
};

struct SIQSShadowProofEvidence {
    SIQSShadowProofOptions options{};

    size_t raw_relations = 0;
    std::optional<size_t> raw_payload_bytes;
    size_t factor_base_columns = 0;
    uint64_t large_prime_bound = 0;

    TwoLargePrimeAdapterStats adapter{};
    std::optional<TwoLargePrimeCycleBasisStatus> graph_status;
    size_t graph_vertices = 0;
    size_t graph_edges = 0;
    size_t graph_components = 0;
    size_t graph_cycles = 0;
    size_t graph_cycle_incidences = 0;
    size_t graph_max_cycle_length = 0;
    size_t row_candidate_upper = 0;

    std::optional<SIQSShadowAssemblyStatus> assembly_status;
    std::optional<SIQSShadowAssemblyLimitEvidence> assembly_limit_evidence;
    SIQSShadowAssemblyStats assembly{};
    SIQSShadowAssemblyFingerprints assembly_fingerprints{};

    std::optional<size_t> projected_dense_matrix_bytes;
    std::optional<SIQSShadowMatrixStatus> matrix_status;
    size_t matrix_rows = 0;
    size_t matrix_columns = 0;
    size_t minimum_nullity = 0;

    size_t dependencies_returned = 0;
    size_t dependencies_examined = 0;
    size_t dependencies_verified = 0;
    size_t no_factor_count = 0;
    size_t factor_found_count = 0;
    bool dependency_cap_reached = false;
    std::optional<SIQSShadowFingerprint> dependency_fingerprint;

    std::optional<size_t> first_failed_dependency;
    std::optional<size_t> winning_dependency;
    std::optional<size_t> winning_dependency_size;
    std::optional<SIQSPostMergeDependencyStatus> dependency_status;
    std::optional<SIQSPostMergeFactorStatus> factor_status;

    [[nodiscard]] friend constexpr bool operator==(const SIQSShadowProofEvidence&,
                                                   const SIQSShadowProofEvidence&) = default;
};

namespace shadow_proof_detail {
struct SIQSShadowProofResultFactory;
}

/// Small owning terminal value. It retains deterministic evidence and, only on
/// success, the canonical factor pair; no input, graph, row, or dependency
/// storage escapes the runner.
class SIQSShadowProofResult {
public:
    SIQSShadowProofResult(const SIQSShadowProofResult&) = default;

    SIQSShadowProofResult& operator=(const SIQSShadowProofResult& other) {
        if (this != &other) {
            SIQSShadowProofResult replacement(other);
            swap(replacement);
        }
        return *this;
    }

    SIQSShadowProofResult(SIQSShadowProofResult&& other) noexcept
        : status_(other.status_), stage_(other.stage_), fallback_reason_(other.fallback_reason_),
          evidence_(std::move(other.evidence_)), factorization_(std::move(other.factorization_)) {
        other.reset_moved_from();
    }

    SIQSShadowProofResult& operator=(SIQSShadowProofResult&& other) noexcept {
        if (this != &other) {
            status_ = other.status_;
            stage_ = other.stage_;
            fallback_reason_ = other.fallback_reason_;
            evidence_ = std::move(other.evidence_);
            factorization_ = std::move(other.factorization_);
            other.reset_moved_from();
        }
        return *this;
    }

    [[nodiscard]] SIQSShadowProofTerminalStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] SIQSShadowProofStage stage() const noexcept {
        return stage_;
    }

    [[nodiscard]] SIQSShadowProofFallbackReason fallback_reason() const noexcept {
        return fallback_reason_;
    }

    [[nodiscard]] const SIQSShadowProofEvidence& evidence() const noexcept {
        return evidence_;
    }

    [[nodiscard]] const std::optional<SIQSPostMergeFactorization>& factorization() const noexcept {
        return factorization_;
    }

    [[nodiscard]] bool has_factor() const noexcept {
        return status_ == SIQSShadowProofTerminalStatus::factor_found && factorization_.has_value();
    }

private:
    friend struct shadow_proof_detail::SIQSShadowProofResultFactory;

    SIQSShadowProofResult(SIQSShadowProofTerminalStatus status, SIQSShadowProofStage stage,
                          SIQSShadowProofFallbackReason fallback_reason,
                          SIQSShadowProofEvidence evidence,
                          std::optional<SIQSPostMergeFactorization> factorization) noexcept
        : status_(status), stage_(stage), fallback_reason_(fallback_reason),
          evidence_(std::move(evidence)), factorization_(std::move(factorization)) {}

    void swap(SIQSShadowProofResult& other) noexcept {
        static_assert(std::is_nothrow_swappable_v<SIQSShadowProofTerminalStatus>);
        static_assert(std::is_nothrow_swappable_v<SIQSShadowProofStage>);
        static_assert(std::is_nothrow_swappable_v<SIQSShadowProofFallbackReason>);
        static_assert(std::is_nothrow_swappable_v<SIQSShadowProofEvidence>);
        static_assert(std::is_nothrow_swappable_v<std::optional<SIQSPostMergeFactorization>>);
        using std::swap;
        swap(status_, other.status_);
        swap(stage_, other.stage_);
        swap(fallback_reason_, other.fallback_reason_);
        swap(evidence_, other.evidence_);
        swap(factorization_, other.factorization_);
    }

    void reset_moved_from() noexcept {
        status_ = SIQSShadowProofTerminalStatus::internal_invariant_failure;
        stage_ = SIQSShadowProofStage::not_started;
        fallback_reason_ = SIQSShadowProofFallbackReason::none;
        evidence_ = SIQSShadowProofEvidence{};
        factorization_.reset();
    }

    SIQSShadowProofTerminalStatus status_;
    SIQSShadowProofStage stage_;
    SIQSShadowProofFallbackReason fallback_reason_;
    SIQSShadowProofEvidence evidence_;
    std::optional<SIQSPostMergeFactorization> factorization_;
};

/// Count the same portable logical relation payload used by live capture.
/// Fixed object headers and allocator bookkeeping remain covered separately by
/// the relation-count limit.
[[nodiscard]] inline std::optional<size_t>
checked_siqs_shadow_relation_payload_bytes(const SIQSRelation& relation) {
    const size_t value_bits = relation.value.bit_length();
    const size_t value_bytes =
        value_bits / size_t{8} + static_cast<size_t>((value_bits % size_t{8}) != 0);
    return checked_siqs_live_sieve_relation_payload_bytes(
        SIQSLiveSieveRelationPayloadShape{value_bytes, relation.exponents.size(),
                                          relation.fb_indices.size(), relation.merge_lps.size()});
}

[[nodiscard]] inline std::optional<size_t>
checked_siqs_shadow_corpus_payload_bytes(std::span<const SIQSRelation> raw_relations) {
    size_t total = 0;
    for (const SIQSRelation& relation : raw_relations) {
        const auto relation_bytes = checked_siqs_shadow_relation_payload_bytes(relation);
        if (!relation_bytes || *relation_bytes > std::numeric_limits<size_t>::max() - total) {
            return std::nullopt;
        }
        total += *relation_bytes;
    }
    return total;
}

namespace shadow_proof_detail {

struct SIQSShadowProofResultFactory {
    [[nodiscard]] static SIQSShadowProofResult
    make(SIQSShadowProofTerminalStatus status, SIQSShadowProofStage stage,
         SIQSShadowProofFallbackReason fallback_reason, SIQSShadowProofEvidence evidence,
         std::optional<SIQSPostMergeFactorization> factorization = std::nullopt) noexcept {
        const bool is_factor = status == SIQSShadowProofTerminalStatus::factor_found;
        const bool is_fallback = status == SIQSShadowProofTerminalStatus::bounded_fallback;
        const bool valid_factor_contract = is_factor == factorization.has_value();
        const bool valid_fallback_contract =
            is_fallback == (fallback_reason != SIQSShadowProofFallbackReason::none);
        if (!valid_factor_contract || !valid_fallback_contract) {
            status = SIQSShadowProofTerminalStatus::internal_invariant_failure;
            fallback_reason = SIQSShadowProofFallbackReason::none;
            factorization.reset();
        }
        return SIQSShadowProofResult(status, stage, fallback_reason, std::move(evidence),
                                     std::move(factorization));
    }
};

[[nodiscard]] inline constexpr std::optional<SIQSShadowProofFallbackReason>
graph_fallback(TwoLargePrimeCycleBasisStatus status) noexcept {
    switch (status) {
    case TwoLargePrimeCycleBasisStatus::edge_limit:
        return SIQSShadowProofFallbackReason::graph_edge_limit;
    case TwoLargePrimeCycleBasisStatus::cycle_limit:
        return SIQSShadowProofFallbackReason::graph_cycle_limit;
    case TwoLargePrimeCycleBasisStatus::incidence_limit:
        return SIQSShadowProofFallbackReason::graph_incidence_limit;
    case TwoLargePrimeCycleBasisStatus::valid:
    case TwoLargePrimeCycleBasisStatus::invalid_edge:
    case TwoLargePrimeCycleBasisStatus::duplicate_relation_index:
    case TwoLargePrimeCycleBasisStatus::size_overflow:
    case TwoLargePrimeCycleBasisStatus::internal_invariant_failure:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] inline constexpr std::optional<SIQSShadowProofFallbackReason>
matrix_fallback(SIQSShadowMatrixStatus status) noexcept {
    switch (status) {
    case SIQSShadowMatrixStatus::resource_limit:
        return SIQSShadowProofFallbackReason::matrix_resource_limit;
    case SIQSShadowMatrixStatus::unsupported_backend:
        return SIQSShadowProofFallbackReason::matrix_backend_unavailable;
    case SIQSShadowMatrixStatus::valid:
    case SIQSShadowMatrixStatus::invalid_modulus:
    case SIQSShadowMatrixStatus::invalid_factor_base:
    case SIQSShadowMatrixStatus::invalid_options:
    case SIQSShadowMatrixStatus::size_overflow:
    case SIQSShadowMatrixStatus::invalid_row:
    case SIQSShadowMatrixStatus::row_identity_mismatch:
    case SIQSShadowMatrixStatus::worker_failure:
    case SIQSShadowMatrixStatus::internal_invariant_failure:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] inline bool checked_add_size(size_t lhs, size_t rhs, size_t& result) noexcept {
    if (rhs > std::numeric_limits<size_t>::max() - lhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

[[nodiscard]] inline bool adapter_stats_are_consistent(const TwoLargePrimeAdapterStats& stats,
                                                       size_t input_relations) noexcept {
    size_t accepted_and_rejected = 0;
    size_t typed_rejections = 0;
    return stats.input_relations == input_relations &&
           checked_add_size(stats.full_relations, stats.accepted_one_lp, accepted_and_rejected) &&
           checked_add_size(accepted_and_rejected, stats.accepted_two_lp, accepted_and_rejected) &&
           checked_add_size(accepted_and_rejected, stats.rejected_relations,
                            accepted_and_rejected) &&
           accepted_and_rejected == input_relations &&
           checked_add_size(stats.malformed_source_shape, stats.unsupported_encoding,
                            typed_rejections) &&
           checked_add_size(typed_rejections, stats.invalid_one_large_prime, typed_rejections) &&
           checked_add_size(typed_rejections, stats.invalid_two_large_prime_split,
                            typed_rejections) &&
           checked_add_size(typed_rejections, stats.exact_duplicate, typed_rejections) &&
           typed_rejections == stats.rejected_relations;
}

[[nodiscard]] inline bool
has_proof_blocking_adapter_rejection(const TwoLargePrimeAdapterStats& stats) noexcept {
    return stats.malformed_source_shape != 0 || stats.unsupported_encoding != 0 ||
           stats.invalid_one_large_prime != 0;
}

[[nodiscard]] inline std::optional<SIQSShadowFingerprint>
fingerprint_dependencies(const SIQSShadowMatrixSolution& solution) noexcept {
    shadow_assembly_detail::StableFingerprint hash;
    if (!hash.add_string("GNFS-SIQS-SHADOW-PROOF-DEPENDENCIES-V1")) {
        return std::nullopt;
    }
    hash.add_u32(SIQS_SHADOW_PROOF_FINGERPRINT_SCHEMA_VERSION);
    if (!hash.add_size(solution.row_count) || !hash.add_size(solution.column_count) ||
        !hash.add_size(solution.dependencies.size())) {
        return std::nullopt;
    }
    for (const auto& dependency : solution.dependencies) {
        if (!hash.add_size(dependency.size())) {
            return std::nullopt;
        }
        for (const size_t row_ordinal : dependency) {
            if (!hash.add_size(row_ordinal)) {
                return std::nullopt;
            }
        }
    }
    return hash.finish();
}

enum class SplitterException : uint8_t {
    none,
    bad_alloc,
    other,
};

/// The assembly adapter catches splitter exceptions internally. Observing the
/// same splitter lvalue at both adapter calls preserves the public exception
/// classification without copying or moving caller state.
template <class Splitter> class ObservedSplitter final {
public:
    explicit ObservedSplitter(Splitter& splitter) noexcept : splitter_(splitter) {}

    decltype(auto) operator()(uint64_t cofactor) {
        try {
            return std::invoke(splitter_, cofactor);
        } catch (const std::bad_alloc&) {
            exception_ = SplitterException::bad_alloc;
            throw;
        } catch (...) {
            exception_ = SplitterException::other;
            throw;
        }
    }

    [[nodiscard]] SplitterException exception() const noexcept {
        return exception_;
    }

private:
    Splitter& splitter_;
    SplitterException exception_ = SplitterException::none;
};

[[nodiscard]] inline bool graph_identity_is_consistent(const TwoLargePrimeCycleBasis& graph,
                                                       size_t prepared_edge_count) noexcept {
    size_t edge_plus_components = 0;
    return graph.edge_count == prepared_edge_count && graph.cycles.size() <= graph.edge_count &&
           checked_add_size(graph.edge_count, graph.component_count, edge_plus_components) &&
           edge_plus_components >= graph.vertex_count &&
           graph.cycles.size() == edge_plus_components - graph.vertex_count &&
           graph.total_cycle_incidences >= graph.cycles.size();
}

[[nodiscard]] inline bool matrix_failure_is_internal(SIQSShadowMatrixStatus status) noexcept {
    switch (status) {
    case SIQSShadowMatrixStatus::valid:
    case SIQSShadowMatrixStatus::invalid_modulus:
    case SIQSShadowMatrixStatus::invalid_factor_base:
    case SIQSShadowMatrixStatus::invalid_options:
    case SIQSShadowMatrixStatus::invalid_row:
    case SIQSShadowMatrixStatus::row_identity_mismatch:
    case SIQSShadowMatrixStatus::internal_invariant_failure:
        return true;
    case SIQSShadowMatrixStatus::size_overflow:
    case SIQSShadowMatrixStatus::worker_failure:
    case SIQSShadowMatrixStatus::resource_limit:
    case SIQSShadowMatrixStatus::unsupported_backend:
        return false;
    }
    return true;
}

[[nodiscard]] inline std::optional<SIQSShadowProofFallbackReason>
assembly_fallback(SIQSShadowAssemblyStatus status) noexcept {
    switch (status) {
    case SIQSShadowAssemblyStatus::pretrim_row_limit:
        return SIQSShadowProofFallbackReason::pretrim_row_limit;
    case SIQSShadowAssemblyStatus::valid:
    case SIQSShadowAssemblyStatus::invalid_modulus:
    case SIQSShadowAssemblyStatus::invalid_factor_base:
    case SIQSShadowAssemblyStatus::invalid_large_prime_bound:
    case SIQSShadowAssemblyStatus::invalid_options:
    case SIQSShadowAssemblyStatus::size_overflow:
    case SIQSShadowAssemblyStatus::source_id_overflow:
    case SIQSShadowAssemblyStatus::adapter_failure:
    case SIQSShadowAssemblyStatus::graph_failure:
    case SIQSShadowAssemblyStatus::graph_edge_limit:
    case SIQSShadowAssemblyStatus::graph_cycle_limit:
    case SIQSShadowAssemblyStatus::graph_incidence_limit:
    case SIQSShadowAssemblyStatus::row_candidate_limit:
    case SIQSShadowAssemblyStatus::worker_failure:
    case SIQSShadowAssemblyStatus::internal_invariant_failure:
    case SIQSShadowAssemblyStatus::resource_exhausted:
    case SIQSShadowAssemblyStatus::exception_failure:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace shadow_proof_detail

/// Run the bounded shadow proof path without mutating or retaining raw input.
///
/// All caps are inclusive maxima. The splitter must be a deterministic pure
/// lvalue-callable function of one cofactor; the same lvalue is used for the
/// bounded preflight and the owning assembly rebuild.
template <class Splitter>
[[nodiscard]] SIQSShadowProofResult
run_siqs_shadow_proof(std::span<const SIQSRelation> raw_relations,
                      std::span<const uint32_t> factor_base_primes,
                      const core::Integer& square_modulus, const core::Integer& gcd_target,
                      uint64_t large_prime_bound, Splitter&& splitter,
                      const SIQSShadowProofOptions& options = {}) noexcept {
    using shadow_proof_detail::SIQSShadowProofResultFactory;

    SIQSShadowProofEvidence evidence;
    evidence.options = options;
    evidence.raw_relations = raw_relations.size();
    evidence.factor_base_columns = factor_base_primes.size();
    evidence.large_prime_bound = large_prime_bound;
    SIQSShadowProofStage current_stage = SIQSShadowProofStage::not_started;

    try {
        current_stage = SIQSShadowProofStage::input_validation;
        size_t required_rows = 0;
        size_t assembly_row_capacity = 0;
        const bool valid_options =
            options.limits.minimum_row_excess != 0 &&
            options.limits.minimum_row_excess <= options.assembly.trim_excess_rows &&
            options.assembly.materialization_workers != 0 && options.matrix.max_dependencies != 0 &&
            options.matrix.elimination_workers != 0 &&
            shadow_proof_detail::checked_add_size(
                factor_base_primes.size(), options.limits.minimum_row_excess, required_rows) &&
            shadow_proof_detail::checked_add_size(factor_base_primes.size(),
                                                  options.assembly.trim_excess_rows,
                                                  assembly_row_capacity);
        (void)assembly_row_capacity;
        if (!valid_options || !post_merge_row_detail::has_valid_modulus(square_modulus) ||
            !post_merge_row_detail::has_valid_factor_base(factor_base_primes) ||
            !gcd_target.is_positive() || gcd_target.is_one() || large_prime_bound < 2 ||
            mpz_divisible_p(square_modulus.get_mpz(), gcd_target.get_mpz()) == 0) {
            return SIQSShadowProofResultFactory::make(
                SIQSShadowProofTerminalStatus::invalid_input, current_stage,
                SIQSShadowProofFallbackReason::none, std::move(evidence));
        }

        current_stage = SIQSShadowProofStage::payload_accounting;
        if (raw_relations.size() > options.limits.max_raw_relations) {
            return SIQSShadowProofResultFactory::make(
                SIQSShadowProofTerminalStatus::bounded_fallback, current_stage,
                SIQSShadowProofFallbackReason::raw_relation_limit, std::move(evidence));
        }
        evidence.raw_payload_bytes = checked_siqs_shadow_corpus_payload_bytes(raw_relations);
        if (!evidence.raw_payload_bytes) {
            return SIQSShadowProofResultFactory::make(
                SIQSShadowProofTerminalStatus::stage_failure, current_stage,
                SIQSShadowProofFallbackReason::none, std::move(evidence));
        }
        if (*evidence.raw_payload_bytes > options.limits.max_raw_payload_bytes) {
            return SIQSShadowProofResultFactory::make(
                SIQSShadowProofTerminalStatus::bounded_fallback, current_stage,
                SIQSShadowProofFallbackReason::raw_payload_limit, std::move(evidence));
        }

        auto& splitter_ref = splitter;
        using SplitterType = std::remove_reference_t<decltype(splitter_ref)>;
        shadow_proof_detail::ObservedSplitter<SplitterType> observed_splitter(splitter_ref);

        // Both owning preflight products are intentionally destroyed before
        // assembly creates its second owning adapter/graph corpus.
        {
            current_stage = SIQSShadowProofStage::adapter_preflight;
            auto prepared = prepare_two_large_prime_corpus(raw_relations, factor_base_primes.size(),
                                                           large_prime_bound, observed_splitter);
            if (!prepared || !shadow_proof_detail::adapter_stats_are_consistent(
                                 prepared->stats, raw_relations.size())) {
                return SIQSShadowProofResultFactory::make(
                    SIQSShadowProofTerminalStatus::internal_invariant_failure, current_stage,
                    SIQSShadowProofFallbackReason::none, std::move(evidence));
            }
            evidence.adapter = prepared->stats;
            if (shadow_proof_detail::has_proof_blocking_adapter_rejection(evidence.adapter)) {
                return SIQSShadowProofResultFactory::make(
                    SIQSShadowProofTerminalStatus::stage_failure, current_stage,
                    SIQSShadowProofFallbackReason::none, std::move(evidence));
            }

            size_t accepted_partials = 0;
            if (!shadow_proof_detail::checked_add_size(evidence.adapter.accepted_one_lp,
                                                       evidence.adapter.accepted_two_lp,
                                                       accepted_partials) ||
                accepted_partials != prepared->edges.size() ||
                prepared->edges.size() != prepared->sources.size()) {
                return SIQSShadowProofResultFactory::make(
                    SIQSShadowProofTerminalStatus::internal_invariant_failure, current_stage,
                    SIQSShadowProofFallbackReason::none, std::move(evidence));
            }

            current_stage = SIQSShadowProofStage::graph_preflight;
            auto graph_result = build_two_large_prime_cycle_basis(
                std::span<const TwoLargePrimeEdge>(prepared->edges.data(), prepared->edges.size()),
                options.limits.graph);
            evidence.graph_status = graph_result.status();
            if (const auto fallback = shadow_proof_detail::graph_fallback(graph_result.status())) {
                return SIQSShadowProofResultFactory::make(
                    SIQSShadowProofTerminalStatus::bounded_fallback, current_stage, *fallback,
                    std::move(evidence));
            }
            if (!graph_result.is_valid() || !graph_result.basis()) {
                const auto terminal =
                    graph_result.status() == TwoLargePrimeCycleBasisStatus::size_overflow
                        ? SIQSShadowProofTerminalStatus::stage_failure
                        : SIQSShadowProofTerminalStatus::internal_invariant_failure;
                return SIQSShadowProofResultFactory::make(terminal, current_stage,
                                                          SIQSShadowProofFallbackReason::none,
                                                          std::move(evidence));
            }

            const TwoLargePrimeCycleBasis& graph = *graph_result.basis();
            evidence.graph_vertices = graph.vertex_count;
            evidence.graph_edges = graph.edge_count;
            evidence.graph_components = graph.component_count;
            evidence.graph_cycles = graph.cycles.size();
            evidence.graph_cycle_incidences = graph.total_cycle_incidences;
            evidence.graph_max_cycle_length = graph.max_cycle_length;
            if (!shadow_proof_detail::graph_identity_is_consistent(graph, prepared->edges.size()) ||
                !shadow_proof_detail::checked_add_size(evidence.adapter.full_relations,
                                                       evidence.graph_cycles,
                                                       evidence.row_candidate_upper)) {
                return SIQSShadowProofResultFactory::make(
                    SIQSShadowProofTerminalStatus::internal_invariant_failure, current_stage,
                    SIQSShadowProofFallbackReason::none, std::move(evidence));
            }
            if (evidence.row_candidate_upper > options.limits.max_row_candidates) {
                return SIQSShadowProofResultFactory::make(
                    SIQSShadowProofTerminalStatus::bounded_fallback, current_stage,
                    SIQSShadowProofFallbackReason::row_candidate_limit, std::move(evidence));
            }
        }

        current_stage = SIQSShadowProofStage::assembly;
        auto assembly_result = assemble_siqs_shadow_rows_bounded(
            raw_relations, factor_base_primes, square_modulus, large_prime_bound, options.assembly,
            SIQSShadowAssemblyLimits{
                options.limits.graph,
                options.limits.max_row_candidates,
                options.limits.max_pretrim_rows,
            },
            observed_splitter);
        evidence.assembly_status = assembly_result.status();
        evidence.assembly_limit_evidence = assembly_result.limit_evidence();
        if (observed_splitter.exception() != shadow_proof_detail::SplitterException::none) {
            const auto terminal =
                observed_splitter.exception() == shadow_proof_detail::SplitterException::bad_alloc
                    ? SIQSShadowProofTerminalStatus::resource_exhausted
                    : SIQSShadowProofTerminalStatus::exception_failure;
            return SIQSShadowProofResultFactory::make(
                terminal, current_stage, SIQSShadowProofFallbackReason::none, std::move(evidence));
        }
        if (assembly_result.status() == SIQSShadowAssemblyStatus::resource_exhausted) {
            return SIQSShadowProofResultFactory::make(
                SIQSShadowProofTerminalStatus::resource_exhausted, current_stage,
                SIQSShadowProofFallbackReason::none, std::move(evidence));
        }
        if (assembly_result.status() == SIQSShadowAssemblyStatus::exception_failure) {
            return SIQSShadowProofResultFactory::make(
                SIQSShadowProofTerminalStatus::exception_failure, current_stage,
                SIQSShadowProofFallbackReason::none, std::move(evidence));
        }
        if (const auto fallback =
                shadow_proof_detail::assembly_fallback(assembly_result.status())) {
            return SIQSShadowProofResultFactory::make(
                SIQSShadowProofTerminalStatus::bounded_fallback, current_stage, *fallback,
                std::move(evidence));
        }
        if (!assembly_result.is_valid() || !assembly_result.assembly()) {
            const bool internal =
                assembly_result.status() == SIQSShadowAssemblyStatus::valid ||
                assembly_result.status() == SIQSShadowAssemblyStatus::invalid_modulus ||
                assembly_result.status() == SIQSShadowAssemblyStatus::invalid_factor_base ||
                assembly_result.status() == SIQSShadowAssemblyStatus::invalid_large_prime_bound ||
                assembly_result.status() == SIQSShadowAssemblyStatus::invalid_options ||
                assembly_result.status() == SIQSShadowAssemblyStatus::graph_failure ||
                assembly_result.status() == SIQSShadowAssemblyStatus::graph_edge_limit ||
                assembly_result.status() == SIQSShadowAssemblyStatus::graph_cycle_limit ||
                assembly_result.status() == SIQSShadowAssemblyStatus::graph_incidence_limit ||
                assembly_result.status() == SIQSShadowAssemblyStatus::row_candidate_limit ||
                assembly_result.status() == SIQSShadowAssemblyStatus::internal_invariant_failure;
            return SIQSShadowProofResultFactory::make(
                internal ? SIQSShadowProofTerminalStatus::internal_invariant_failure
                         : SIQSShadowProofTerminalStatus::stage_failure,
                current_stage, SIQSShadowProofFallbackReason::none, std::move(evidence));
        }

        const SIQSShadowAssembly& assembly = *assembly_result.assembly();
        evidence.assembly = assembly.stats;
        evidence.assembly_fingerprints = assembly.fingerprints;
        const bool assembly_evidence_matches =
            assembly.stats.input_relations == raw_relations.size() &&
            assembly.stats.adapter == evidence.adapter &&
            assembly.stats.graph_edges == evidence.graph_edges &&
            assembly.stats.graph_cycles == evidence.graph_cycles &&
            assembly.stats.pretrim_rows <= evidence.row_candidate_upper &&
            assembly.stats.selected_rows == assembly.rows.size() &&
            shadow_assembly_detail::stats_are_consistent(assembly.stats);
        if (!assembly_evidence_matches) {
            return SIQSShadowProofResultFactory::make(
                SIQSShadowProofTerminalStatus::internal_invariant_failure, current_stage,
                SIQSShadowProofFallbackReason::none, std::move(evidence));
        }
        if (assembly.stats.rejected_full_relations != 0 ||
            assembly.stats.rejected_cycle_rows != 0) {
            return SIQSShadowProofResultFactory::make(
                SIQSShadowProofTerminalStatus::stage_failure, current_stage,
                SIQSShadowProofFallbackReason::none, std::move(evidence));
        }
        if (assembly.stats.pretrim_rows > options.limits.max_pretrim_rows) {
            return SIQSShadowProofResultFactory::make(
                SIQSShadowProofTerminalStatus::bounded_fallback, current_stage,
                SIQSShadowProofFallbackReason::pretrim_row_limit, std::move(evidence));
        }
        if (assembly.rows.size() < required_rows) {
            return SIQSShadowProofResultFactory::make(
                SIQSShadowProofTerminalStatus::bounded_fallback, current_stage,
                SIQSShadowProofFallbackReason::insufficient_rows, std::move(evidence));
        }

        current_stage = SIQSShadowProofStage::matrix;
        evidence.matrix_rows = assembly.rows.size();
        evidence.matrix_columns = factor_base_primes.size();
        evidence.minimum_nullity = evidence.matrix_rows > evidence.matrix_columns
                                       ? evidence.matrix_rows - evidence.matrix_columns
                                       : 0;
        evidence.projected_dense_matrix_bytes =
            checked_siqs_shadow_dense_matrix_bytes(evidence.matrix_rows, evidence.matrix_columns);
        auto matrix_result = solve_siqs_shadow_matrix(
            std::span<const SIQSShadowRow>(assembly.rows.data(), assembly.rows.size()),
            factor_base_primes, square_modulus, options.matrix);
        evidence.matrix_status = matrix_result.status();
        if (const auto fallback = shadow_proof_detail::matrix_fallback(matrix_result.status())) {
            return SIQSShadowProofResultFactory::make(
                SIQSShadowProofTerminalStatus::bounded_fallback, current_stage, *fallback,
                std::move(evidence));
        }
        if (!matrix_result.is_valid() || !matrix_result.solution()) {
            return SIQSShadowProofResultFactory::make(
                shadow_proof_detail::matrix_failure_is_internal(matrix_result.status())
                    ? SIQSShadowProofTerminalStatus::internal_invariant_failure
                    : SIQSShadowProofTerminalStatus::stage_failure,
                current_stage, SIQSShadowProofFallbackReason::none, std::move(evidence));
        }

        const SIQSShadowMatrixSolution& solution = *matrix_result.solution();
        evidence.dependencies_returned = solution.dependencies.size();
        evidence.dependency_cap_reached =
            solution.dependencies.size() == options.matrix.max_dependencies;
        const size_t minimum_expected_dependencies =
            std::min(evidence.minimum_nullity, options.matrix.max_dependencies);
        if (solution.row_count != evidence.matrix_rows ||
            solution.column_count != evidence.matrix_columns ||
            solution.dependencies.size() > options.matrix.max_dependencies ||
            solution.dependencies.size() < minimum_expected_dependencies) {
            return SIQSShadowProofResultFactory::make(
                SIQSShadowProofTerminalStatus::internal_invariant_failure, current_stage,
                SIQSShadowProofFallbackReason::none, std::move(evidence));
        }
        evidence.dependency_fingerprint = shadow_proof_detail::fingerprint_dependencies(solution);
        if (!evidence.dependency_fingerprint) {
            return SIQSShadowProofResultFactory::make(
                SIQSShadowProofTerminalStatus::stage_failure, current_stage,
                SIQSShadowProofFallbackReason::none, std::move(evidence));
        }

        const auto row_span =
            std::span<const SIQSShadowRow>(assembly.rows.data(), assembly.rows.size());
        for (size_t dependency_index = 0; dependency_index < solution.dependencies.size();
             ++dependency_index) {
            const auto& dependency = solution.dependencies[dependency_index];
            current_stage = SIQSShadowProofStage::dependency_verification;
            ++evidence.dependencies_examined;
            auto dependency_result = verify_siqs_post_merge_dependency(
                row_span, std::span<const size_t>(dependency.data(), dependency.size()),
                factor_base_primes, square_modulus);
            evidence.dependency_status = dependency_result.status();
            if (dependency_result.status() != SIQSPostMergeDependencyStatus::valid) {
                evidence.first_failed_dependency = dependency_index;
                return SIQSShadowProofResultFactory::make(
                    dependency_result.verified()
                        ? SIQSShadowProofTerminalStatus::internal_invariant_failure
                        : SIQSShadowProofTerminalStatus::stage_failure,
                    current_stage, SIQSShadowProofFallbackReason::none, std::move(evidence));
            }
            if (!dependency_result.is_valid() || !dependency_result.verified()) {
                evidence.first_failed_dependency = dependency_index;
                return SIQSShadowProofResultFactory::make(
                    SIQSShadowProofTerminalStatus::internal_invariant_failure, current_stage,
                    SIQSShadowProofFallbackReason::none, std::move(evidence));
            }
            ++evidence.dependencies_verified;

            current_stage = SIQSShadowProofStage::factor_extraction;
            auto factor_result = extract_siqs_post_merge_factor(dependency_result, gcd_target);
            evidence.factor_status = factor_result.status();
            if (factor_result.status() == SIQSPostMergeFactorStatus::no_factor) {
                if (factor_result.factors()) {
                    evidence.first_failed_dependency = dependency_index;
                    return SIQSShadowProofResultFactory::make(
                        SIQSShadowProofTerminalStatus::internal_invariant_failure, current_stage,
                        SIQSShadowProofFallbackReason::none, std::move(evidence));
                }
                ++evidence.no_factor_count;
                continue;
            }
            if (factor_result.status() == SIQSPostMergeFactorStatus::factor_found) {
                if (!factor_result.factors() ||
                    factor_result.factors()->factor * factor_result.factors()->cofactor !=
                        gcd_target) {
                    evidence.first_failed_dependency = dependency_index;
                    return SIQSShadowProofResultFactory::make(
                        SIQSShadowProofTerminalStatus::internal_invariant_failure, current_stage,
                        SIQSShadowProofFallbackReason::none, std::move(evidence));
                }
                ++evidence.factor_found_count;
                evidence.winning_dependency = dependency_index;
                evidence.winning_dependency_size = dependency.size();
                SIQSPostMergeFactorization factorization{
                    factor_result.factors()->factor,
                    factor_result.factors()->cofactor,
                };
                return SIQSShadowProofResultFactory::make(
                    SIQSShadowProofTerminalStatus::factor_found, current_stage,
                    SIQSShadowProofFallbackReason::none, std::move(evidence),
                    std::move(factorization));
            }

            evidence.first_failed_dependency = dependency_index;
            return SIQSShadowProofResultFactory::make(
                SIQSShadowProofTerminalStatus::internal_invariant_failure, current_stage,
                SIQSShadowProofFallbackReason::none, std::move(evidence));
        }

        current_stage = SIQSShadowProofStage::complete;
        return SIQSShadowProofResultFactory::make(
            SIQSShadowProofTerminalStatus::no_factor, current_stage,
            SIQSShadowProofFallbackReason::none, std::move(evidence));
    } catch (const std::bad_alloc&) {
        return SIQSShadowProofResultFactory::make(
            SIQSShadowProofTerminalStatus::resource_exhausted, current_stage,
            SIQSShadowProofFallbackReason::none, std::move(evidence));
    } catch (...) {
        return SIQSShadowProofResultFactory::make(
            SIQSShadowProofTerminalStatus::exception_failure, current_stage,
            SIQSShadowProofFallbackReason::none, std::move(evidence));
    }
}

} // namespace gnfs::siqs

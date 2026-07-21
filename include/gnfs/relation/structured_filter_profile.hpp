#pragma once

#include "reduction_engine.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <utility>

namespace gnfs::relation {

/// Frozen caps for the first forced-on, vector-backed research profile.
///
/// These are deliberately conservative and are not an automatic tuning
/// policy. M5 scale evidence may replace them, but changing the values must be
/// explicit and separately validated across size bands.
struct StructuredFilterExperimentalCaps final {
    static constexpr size_t max_candidate_examinations_per_pass = 4'096;
    static constexpr size_t max_emitted_rows = 4'096;
    static constexpr size_t max_commits = 1'024;
    static constexpr size_t max_total_lp_fill_growth = 0;
    static constexpr size_t max_accepted_payload_entries_per_commit = 8'192;
    static constexpr size_t max_source_atoms_per_output = 64;
    static constexpr size_t max_materialized_pairs_per_output = 64;
    static constexpr size_t max_factor_entries_per_side = 4'096;
    static constexpr size_t max_batch_candidates = 4;
    static constexpr size_t max_rows_per_incidence_shard = 4'096;
    static constexpr uint32_t max_workers = 4;
};

/// Resolve the production worker request. A platform that reports no hardware
/// concurrency uses one deterministic worker rather than disabling execution.
[[nodiscard]] inline uint32_t structured_filter_hardware_workers() noexcept {
    const unsigned reported = std::thread::hardware_concurrency();
    if (reported == 0)
        return 1;
    return static_cast<uint32_t>(
        std::min<unsigned long long>(reported, StructuredFilterExperimentalCaps::max_workers));
}

/// Build the explicit M4 vector-backed research profile for one snapshot.
///
/// Corpus-relative caps never exceed the frozen ceilings. A zero-row corpus
/// still receives nonzero execution-shape values so it completes normally as
/// NoCandidates. Positive LP fill is rejected; persistence and source limits
/// remain below the shared relation-format boundaries.
[[nodiscard]] inline RelationReductionConfig::StructuredExecutionConfig
make_structured_filter_experimental_config(size_t input_rows, uint32_t worker_count) {
    if (worker_count == 0) {
        throw std::invalid_argument("structured-filter worker count must be nonzero");
    }
    if (worker_count > StructuredFilterExperimentalCaps::max_workers) {
        throw std::invalid_argument("structured-filter worker count exceeds experimental cap");
    }

    const size_t bounded_rows = std::max<size_t>(input_rows, 1);
    const size_t candidate_cap = std::min(
        bounded_rows, StructuredFilterExperimentalCaps::max_candidate_examinations_per_pass);
    const size_t emitted_cap =
        std::min(bounded_rows, StructuredFilterExperimentalCaps::max_emitted_rows);

    StructuredReductionBudget budget(
        candidate_cap, emitted_cap, StructuredFilterExperimentalCaps::max_total_lp_fill_growth,
        StructuredFilterExperimentalCaps::max_accepted_payload_entries_per_commit);
    budget.max_commits = std::min(bounded_rows, StructuredFilterExperimentalCaps::max_commits);
    budget.max_source_atoms_per_output =
        StructuredFilterExperimentalCaps::max_source_atoms_per_output;
    budget.max_materialized_pairs_per_output =
        StructuredFilterExperimentalCaps::max_materialized_pairs_per_output;
    budget.max_factor_entries_per_side =
        StructuredFilterExperimentalCaps::max_factor_entries_per_side;

    return {
        std::move(budget),
        {.max_batch_candidates =
             std::min(bounded_rows, StructuredFilterExperimentalCaps::max_batch_candidates),
         .worker_count = worker_count},
        {.max_rows_per_shard =
             std::min(bounded_rows, StructuredFilterExperimentalCaps::max_rows_per_incidence_shard),
         .worker_count = worker_count},
        TreeBasisPlanner::DeterministicMst,
        {},
        OOCCleanupPolicy::RemoveArtifacts,
    };
}

} // namespace gnfs::relation

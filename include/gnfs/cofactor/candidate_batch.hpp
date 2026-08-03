#pragma once

#include "gnfs/cofactor/cofactorizer.hpp"
#include "gnfs/cofactor/seed_provider.hpp"
#include "gnfs/core/relation.hpp"
#include "gnfs/sieve/lattice_sieve.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace gnfs::cofactor {

inline constexpr size_t DEFAULT_CANDIDATE_CHUNK_SIZE = 256;

struct CandidateBatchOptions final {
    size_t max_candidates_per_chunk = DEFAULT_CANDIDATE_CHUNK_SIZE;
    uint32_t max_workers = 1;
};

struct CandidateBatchResult final {
    std::vector<std::vector<core::Relation>> relations_by_special_q;
    size_t total_candidates = 0;
    size_t planned_chunks = 0;
    size_t workers_used = 0;
};

/// Return the stable randomness coordinates for one original sieve candidate.
///
/// The special-Q coordinate is the global factor-base SpecialQ::index, not the
/// SieveResult span slot. The candidate coordinate is its original vector
/// index, before chunking, worker assignment, rejection, or relation folding.
/// An out-of-range candidate_index throws std::out_of_range.
[[nodiscard]] CofactorAttemptCoordinates
candidate_attempt_coordinates_v1(const sieve::SieveResult& sieve_result, size_t candidate_index);

/// Verify a batch of sieve candidates with worker-local Cofactorizers.
///
/// Work is claimed dynamically in canonical candidate-chunk order. Each chunk
/// writes to its own result slot, and the caller thread folds those slots in
/// (special-Q index, candidate index) order. Therefore worker scheduling and
/// worker-count changes cannot reorder the returned relation corpus.
[[nodiscard]] CandidateBatchResult
verify_candidate_batch(const core::PolynomialContext& ctx, const factor_base::FactorBase& fb,
                       const CofactorizerConfig& config,
                       std::span<const sieve::SieveResult> sieve_results,
                       const CandidateBatchOptions& options = CandidateBatchOptions{});

/// Deterministic-seed variant of verify_candidate_batch().
///
/// seed_provider is shared read-only by all worker-local Cofactorizers and
/// must remain alive until this synchronous call returns. Provider requests
/// use candidate_attempt_coordinates_v1(); worker, chunk, and batch-slot
/// identities never enter the coordinates.
[[nodiscard]] CandidateBatchResult
verify_candidate_batch(const core::PolynomialContext& ctx, const factor_base::FactorBase& fb,
                       const CofactorizerConfig& config,
                       std::span<const sieve::SieveResult> sieve_results,
                       const CofactorSeedProvider& seed_provider,
                       const CandidateBatchOptions& options = CandidateBatchOptions{});

} // namespace gnfs::cofactor

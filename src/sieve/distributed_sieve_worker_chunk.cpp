#include "distributed_sieve_worker_chunk_internal.hpp"

#include <gnfs/cofactor/candidate_batch.hpp>
#include <gnfs/cofactor/cofactorizer.hpp>
#include <gnfs/core/integer.hpp>
#include <gnfs/core/polynomial_context.hpp>
#include <gnfs/core/relation.hpp>
#include <gnfs/factor_base/factor_base.hpp>
#include <gnfs/sieve/lattice_sieve.hpp>
#include <gnfs/sieve/special_q.hpp>
#include <gnfs/util/safe_math.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <unordered_set>
#include <utility>

namespace gnfs::sieve::distributed_sieve_worker_execution_detail {
namespace {

[[nodiscard]] bool same_chunk(const ChunkPlanV1& left, const ChunkPlanV1& right) noexcept {
    return left.chunk_id == right.chunk_id && left.sq_begin == right.sq_begin &&
           left.sq_end == right.sq_end &&
           left.relative_artifact_stem == right.relative_artifact_stem;
}

[[nodiscard]] bool relation_is_admissible(const core::PolynomialContext& polynomial,
                                          const core::Relation& relation) {
    if (relation.b == 0 ||
        std::gcd(gnfs::util::safe_abs(relation.a), relation.b) != std::uint64_t{1}) {
        return false;
    }

    core::Integer rational_value = polynomial.rational_value(relation.a, relation.b);
    rational_value.abs();
    const core::Integer divisor = core::gcd(std::move(rational_value), polynomial.n());
    return divisor.is_one();
}

} // namespace

struct PreparedDistributedSieveWorkerChunkV1::State final {
    State(const core::PolynomialContext& polynomial, const factor_base::FactorBase& factor_base,
          const distributed_sieve_execution_policy_detail::DistributedSieveBoundWorkV1& work,
          const ChunkPlanV1& chunk)
        : polynomial(polynomial), work(work), chunk(chunk),
          generator(factor_base, SpecialQRange::from_indices(chunk.sq_begin, chunk.sq_end)),
          sieve(polynomial, factor_base, work.sieve_parameters, work.lattice.sieve),
          cofactorizer(polynomial, factor_base, work.cofactor.cofactorizer), admission(polynomial) {
        sieve.set_region(work.sieve_region);
        // M3a-2c V1 uses process-level worker parallelism. This prevents
        // nested worker topology from changing scheduling or oversubscribing
        // a wave, and avoids the standalone auto-thread hardware probe.
        sieve.set_max_threads(1);
    }

    const core::PolynomialContext& polynomial;
    const distributed_sieve_execution_policy_detail::DistributedSieveBoundWorkV1& work;
    ChunkPlanV1 chunk;
    SpecialQGenerator generator;
    LatticeSieve sieve;
    cofactor::Cofactorizer cofactorizer;
    DistributedSieveWorkerRelationAdmissionV1 admission;
    bool executed = false;
};

struct DistributedSieveWorkerRelationAdmissionV1::State final {
    explicit State(const core::PolynomialContext& polynomial) noexcept : polynomial(polynomial) {}

    const core::PolynomialContext& polynomial;
    std::unordered_set<core::ABPair, core::ABPairHash> seen;
};

DistributedSieveWorkerRelationAdmissionV1::DistributedSieveWorkerRelationAdmissionV1(
    const core::PolynomialContext& polynomial)
    : state_(std::make_unique<State>(polynomial)) {}

DistributedSieveWorkerRelationAdmissionV1::~DistributedSieveWorkerRelationAdmissionV1() noexcept =
    default;

DistributedSieveWorkerAdmissionStatusV1 DistributedSieveWorkerRelationAdmissionV1::admit(
    const core::Relation& relation, DistributedSieveWorkerRelationSinkV1 sink) noexcept {
    if (state_ == nullptr || sink.append == nullptr) {
        return DistributedSieveWorkerAdmissionStatusV1::invalid;
    }
    try {
        if (!relation_is_admissible(state_->polynomial, relation)) {
            return DistributedSieveWorkerAdmissionStatusV1::invalid;
        }
        auto [seen, inserted] = state_->seen.insert(relation.ab());
        if (!inserted) {
            return DistributedSieveWorkerAdmissionStatusV1::duplicate;
        }
        if (!sink.append(sink.context, relation)) {
            state_->seen.erase(seen);
            return DistributedSieveWorkerAdmissionStatusV1::sink_failed;
        }
        return DistributedSieveWorkerAdmissionStatusV1::accepted;
    } catch (const std::bad_alloc&) {
        return DistributedSieveWorkerAdmissionStatusV1::resource_exhausted;
    } catch (...) {
        return DistributedSieveWorkerAdmissionStatusV1::unexpected_failure;
    }
}

PreparedDistributedSieveWorkerChunkV1::PreparedDistributedSieveWorkerChunkV1(
    std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

PreparedDistributedSieveWorkerChunkV1::PreparedDistributedSieveWorkerChunkV1(
    PreparedDistributedSieveWorkerChunkV1&&) noexcept = default;

PreparedDistributedSieveWorkerChunkV1::~PreparedDistributedSieveWorkerChunkV1() noexcept = default;

DistributedSieveWorkerChunkExecutionResultV1
PreparedDistributedSieveWorkerChunkV1::execute(DistributedSieveWorkerRelationSinkV1 sink) noexcept {
    DistributedSieveWorkerChunkExecutionResultV1 result;
    if (state_ == nullptr || state_->executed) {
        result.status = DistributedSieveWorkerChunkStatusV1::already_executed;
        return result;
    }
    state_->executed = true;
    if (sink.append == nullptr) {
        result.status = DistributedSieveWorkerChunkStatusV1::sink_invalid;
        return result;
    }

    std::uint64_t processed_sq_count = 0;
    try {
        while (state_->generator.has_next()) {
            // Preserve legacy boundary priority: SQ cap is checked before the
            // soft relation cap, and neither cap truncates a special-Q midway.
            if (state_->work.sq_cap_per_worker > 0 &&
                processed_sq_count >= state_->work.sq_cap_per_worker) {
                result.completion = DistributedSieveWorkerChunkCompletionV1{
                    .processed_sq_count = processed_sq_count,
                    .next_sq_index = state_->generator.current_index(),
                    .completion_reason = WorkerCompletionReasonV1::sq_cap,
                };
                result.status = DistributedSieveWorkerChunkStatusV1::completed;
                return result;
            }
            if (state_->work.relation_cap_per_worker > 0 &&
                result.accepted_relation_count >= state_->work.relation_cap_per_worker) {
                result.completion = DistributedSieveWorkerChunkCompletionV1{
                    .processed_sq_count = processed_sq_count,
                    .next_sq_index = state_->generator.current_index(),
                    .completion_reason = WorkerCompletionReasonV1::relation_cap,
                };
                result.status = DistributedSieveWorkerChunkStatusV1::completed;
                return result;
            }

            auto special_q = state_->generator.next();
            if (!special_q.has_value()) {
                break;
            }
            auto sieve_result = state_->sieve.sieve_special_q(*special_q);
            for (std::size_t candidate_ordinal = 0;
                 candidate_ordinal < sieve_result.candidates.size(); ++candidate_ordinal) {
                const auto& candidate = sieve_result.candidates[candidate_ordinal];
                auto relation = state_->cofactorizer.verify(
                    candidate, special_q->q, special_q->r,
                    cofactor::candidate_attempt_coordinates_v1(sieve_result, candidate_ordinal),
                    state_->work.cofactor.seed_provider);
                if (!relation.has_value()) {
                    continue;
                }
                const auto admitted = state_->admission.admit(*relation, sink);
                if (admitted == DistributedSieveWorkerAdmissionStatusV1::invalid) {
                    ++result.invalid_relation_count;
                    continue;
                }
                if (admitted == DistributedSieveWorkerAdmissionStatusV1::duplicate) {
                    ++result.duplicate_relation_count;
                    continue;
                }
                if (admitted == DistributedSieveWorkerAdmissionStatusV1::resource_exhausted) {
                    result.status = DistributedSieveWorkerChunkStatusV1::resource_exhausted;
                    result.artifacts_may_remain = result.accepted_relation_count != 0;
                    return result;
                }
                if (admitted == DistributedSieveWorkerAdmissionStatusV1::sink_failed) {
                    result.status = DistributedSieveWorkerChunkStatusV1::sink_failed;
                    result.artifacts_may_remain = true;
                    return result;
                }
                if (admitted == DistributedSieveWorkerAdmissionStatusV1::unexpected_failure) {
                    result.status = DistributedSieveWorkerChunkStatusV1::execution_failed;
                    result.artifacts_may_remain = result.accepted_relation_count != 0;
                    return result;
                }
                ++result.accepted_relation_count;
            }
            ++processed_sq_count;
        }

        result.completion = DistributedSieveWorkerChunkCompletionV1{
            .processed_sq_count = processed_sq_count,
            // SpecialQGenerator intentionally leaves its cursor before
            // trailing projective entries. Exhaustion is normalized to the
            // authenticated chunk boundary for deterministic resume.
            .next_sq_index = state_->chunk.sq_end,
            .completion_reason = result.accepted_relation_count == 0
                                     ? WorkerCompletionReasonV1::zero_relations
                                     : WorkerCompletionReasonV1::range_exhausted,
        };
        result.status = DistributedSieveWorkerChunkStatusV1::completed;
        return result;
    } catch (const std::bad_alloc&) {
        result.status = DistributedSieveWorkerChunkStatusV1::resource_exhausted;
        result.artifacts_may_remain = result.accepted_relation_count != 0;
        return result;
    } catch (...) {
        result.status = DistributedSieveWorkerChunkStatusV1::execution_failed;
        result.artifacts_may_remain = result.accepted_relation_count != 0;
        return result;
    }
}

DistributedSieveWorkerChunkPrepareResultV1 prepare_distributed_sieve_worker_chunk_v1(
    const core::PolynomialContext& polynomial, const factor_base::FactorBase& factor_base,
    const distributed_sieve_execution_policy_detail::DistributedSieveBoundWorkV1& work,
    const ChunkPlanV1& chunk) noexcept {
    const auto expected =
        std::find_if(work.chunks.begin(), work.chunks.end(), [&](const ChunkPlanV1& candidate) {
            return candidate.chunk_id == chunk.chunk_id;
        });
    if (expected == work.chunks.end() || !same_chunk(*expected, chunk)) {
        return {std::nullopt, DistributedSieveWorkerChunkStatusV1::binding_invalid};
    }

    try {
        return {
            PreparedDistributedSieveWorkerChunkV1(
                std::make_unique<PreparedDistributedSieveWorkerChunkV1::State>(
                    polynomial, factor_base, work, chunk)),
            DistributedSieveWorkerChunkStatusV1::ready,
        };
    } catch (const std::bad_alloc&) {
        return {std::nullopt, DistributedSieveWorkerChunkStatusV1::resource_exhausted};
    } catch (...) {
        return {std::nullopt, DistributedSieveWorkerChunkStatusV1::binding_invalid};
    }
}

} // namespace gnfs::sieve::distributed_sieve_worker_execution_detail

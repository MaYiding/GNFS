#include "distributed_sieve_wave_result_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace gnfs::sieve {

using distributed_sieve_result_detail::RetainedMergedResultV1;

struct DistributedSieveWaveResult::State final {
    State(std::vector<ChunkCommitSummaryV1> chunks_value, util::Sha256Digest manifest_digest_value,
          util::Sha256Digest merge_commit_digest_value, std::size_t relation_count_value,
          std::size_t completed_worker_count_value) noexcept
        : chunks(std::move(chunks_value)), manifest_digest(manifest_digest_value),
          merge_commit_digest(merge_commit_digest_value), relation_count(relation_count_value),
          completed_worker_count(completed_worker_count_value) {}

    std::optional<RetainedMergedResultV1> retained;
    std::vector<ChunkCommitSummaryV1> chunks;
    util::Sha256Digest manifest_digest;
    util::Sha256Digest merge_commit_digest;
    std::size_t relation_count = 0;
    std::size_t completed_worker_count = 0;
};

DistributedSieveWaveResult::DistributedSieveWaveResult(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

DistributedSieveWaveResult::DistributedSieveWaveResult(DistributedSieveWaveResult&&) noexcept =
    default;

DistributedSieveWaveResult::~DistributedSieveWaveResult() noexcept = default;

bool DistributedSieveWaveResult::valid() const noexcept {
    try {
        return state_ != nullptr && state_->retained.has_value() && state_->retained->valid() &&
               state_->manifest_digest != util::Sha256Digest{} &&
               state_->merge_commit_digest != util::Sha256Digest{} &&
               state_->relation_count == state_->retained->merged_relations().count() &&
               state_->completed_worker_count == state_->retained->completed_worker_count() &&
               state_->completed_worker_count ==
                   static_cast<std::size_t>(std::count_if(
                       state_->chunks.begin(), state_->chunks.end(), [](const auto& chunk) {
                           return chunk.input.disposition != ChunkDispositionV1::empty;
                       }));
    } catch (...) {
        return false;
    }
}

DistributedSieveWaveResult::operator bool() const noexcept {
    return valid();
}

std::size_t DistributedSieveWaveResult::relation_count() const noexcept {
    return state_ != nullptr ? state_->relation_count : 0U;
}

std::size_t DistributedSieveWaveResult::completed_worker_count() const noexcept {
    return state_ != nullptr ? state_->completed_worker_count : 0U;
}

const util::Sha256Digest& DistributedSieveWaveResult::manifest_digest() const& {
    if (state_ == nullptr) {
        throw std::logic_error("distributed sieve wave result is moved-from");
    }
    return state_->manifest_digest;
}

const util::Sha256Digest& DistributedSieveWaveResult::merge_commit_digest() const& {
    if (state_ == nullptr) {
        throw std::logic_error("distributed sieve wave result is moved-from");
    }
    return state_->merge_commit_digest;
}

std::span<const ChunkCommitSummaryV1> DistributedSieveWaveResult::chunks() const& {
    if (state_ == nullptr) {
        throw std::logic_error("distributed sieve wave result is moved-from");
    }
    return state_->chunks;
}

const relation::ReadOnlyRelationCorpusView& DistributedSieveWaveResult::merged_relations() const& {
    if (state_ == nullptr || !state_->retained.has_value()) {
        throw std::logic_error("distributed sieve wave result is moved-from");
    }
    return state_->retained->merged_relations();
}

} // namespace gnfs::sieve

namespace gnfs::sieve::distributed_sieve_result_detail {
namespace {

using PromotionDiagnostic = DistributedSieveWaveResultPromotionDiagnosticV1;
using PromotionDisposition = DistributedSieveWaveResultPromotionDispositionV1;
using PromotionPhase = DistributedSieveWaveResultPromotionPhaseV1;
using PromotionResult = DistributedSieveWaveResultPromotionResultV1;
using PromotionStatus = DistributedSieveWaveResultPromotionStatusV1;
using PromotionFaultPoint = trusted_test::DistributedSieveWaveResultPromotionFaultPointV1;
using PromotionHooks = trusted_test::DistributedSieveWaveResultPromotionTestHooksV1;

static_assert(std::is_nothrow_move_constructible_v<RetainedMergedResultV1>);
static_assert(std::is_nothrow_move_constructible_v<DistributedSieveWaveResult>);

[[nodiscard]] PromotionDiagnostic diagnostic(PromotionPhase phase, PromotionStatus status,
                                             PromotionDisposition disposition,
                                             std::error_code native_error = {}) noexcept {
    return {
        .phase = phase,
        .status = status,
        .disposition = disposition,
        .native_error = native_error,
    };
}

[[nodiscard]] PromotionResult cold(PromotionPhase phase, PromotionStatus status,
                                   std::error_code native_error) noexcept {
    return {std::nullopt, std::nullopt,
            diagnostic(phase, status, PromotionDisposition::cold_reopen_required, native_error)};
}

[[nodiscard]] PromotionResult retry(RetainedMergedResultV1&& retained, PromotionPhase phase,
                                    PromotionStatus status, std::error_code native_error) noexcept {
    std::optional<RetainedMergedResultV1> retryable;
    retryable.emplace(std::move(retained));
    return {std::move(retryable), std::nullopt,
            diagnostic(phase, status, PromotionDisposition::retryable, native_error)};
}

} // namespace

DistributedSieveWaveResultPromotionResultV1::operator bool() const noexcept {
    return diagnostic.phase == PromotionPhase::complete &&
           diagnostic.status == PromotionStatus::promoted &&
           diagnostic.disposition == PromotionDisposition::promoted && !diagnostic.native_error &&
           !retryable.has_value() && promoted.has_value() && promoted->valid();
}

DistributedSieveWaveResultPromotionResultV1
DistributedSieveWaveResultAuthorityV1::promote(RetainedMergedResultV1&& input,
                                               PromotionHooks hooks) noexcept {
    RetainedMergedResultV1 retained(std::move(input));
    if (!retained.valid()) {
        return cold(PromotionPhase::input_validation, PromotionStatus::invalid_input,
                    std::make_error_code(std::errc::invalid_argument));
    }

    PromotionPhase phase = PromotionPhase::projection_freeze;
    std::unique_ptr<DistributedSieveWaveResult::State> state;
    try {
        const auto& commit = retained.commit_for_wave_result_promotion_v1();
        const std::size_t completed_worker_count = retained.completed_worker_count();
        const std::size_t projected_worker_count = static_cast<std::size_t>(
            std::count_if(commit.chunks.begin(), commit.chunks.end(), [](const auto& chunk) {
                return chunk.input.disposition != ChunkDispositionV1::empty;
            }));
        if (commit.manifest_digest == util::Sha256Digest{} ||
            commit.self_digest == util::Sha256Digest{} ||
            commit.output_relation_count > std::numeric_limits<std::size_t>::max() ||
            static_cast<std::size_t>(commit.output_relation_count) !=
                retained.merged_relations().count() ||
            completed_worker_count != projected_worker_count) {
            return cold(PromotionPhase::projection_freeze, PromotionStatus::projection_mismatch,
                        std::make_error_code(std::errc::state_not_recoverable));
        }

        std::vector<ChunkCommitSummaryV1> chunks = commit.chunks;
        const util::Sha256Digest manifest_digest = commit.manifest_digest;
        const util::Sha256Digest merge_commit_digest = commit.self_digest;
        const std::size_t relation_count = static_cast<std::size_t>(commit.output_relation_count);

        phase = PromotionPhase::state_allocation;
        if (hooks.stop_before != nullptr &&
            hooks.stop_before(PromotionFaultPoint::before_state_allocation, hooks.context)) {
            throw std::bad_alloc();
        }
        state = std::unique_ptr<DistributedSieveWaveResult::State>(
            new DistributedSieveWaveResult::State(std::move(chunks), manifest_digest,
                                                  merge_commit_digest, relation_count,
                                                  completed_worker_count));
    } catch (const std::bad_alloc&) {
        return retry(std::move(retained), phase, PromotionStatus::resource_exhausted,
                     std::make_error_code(std::errc::not_enough_memory));
    } catch (const std::system_error& error) {
        return retry(std::move(retained), phase, PromotionStatus::unexpected_failure, error.code());
    } catch (...) {
        return retry(std::move(retained), phase, PromotionStatus::unexpected_failure,
                     std::make_error_code(std::errc::io_error));
    }

    state->retained.emplace(std::move(retained));
    DistributedSieveWaveResult result(std::move(state));
    std::optional<DistributedSieveWaveResult> promoted;
    promoted.emplace(std::move(result));
    return {std::nullopt, std::move(promoted),
            diagnostic(PromotionPhase::complete, PromotionStatus::promoted,
                       PromotionDisposition::promoted)};
}

DistributedSieveWaveResultPromotionResultV1
promote_distributed_sieve_wave_result_v1(RetainedMergedResultV1&& retained) noexcept {
    return DistributedSieveWaveResultAuthorityV1::promote(std::move(retained), {});
}

namespace trusted_test {

DistributedSieveWaveResultPromotionResultV1 promote_distributed_sieve_wave_result_v1_with_hooks(
    RetainedMergedResultV1&& retained,
    DistributedSieveWaveResultPromotionTestHooksV1 hooks) noexcept {
    return DistributedSieveWaveResultAuthorityV1::promote(std::move(retained), hooks);
}

} // namespace trusted_test

} // namespace gnfs::sieve::distributed_sieve_result_detail

#include "distributed_sieve_merge_writer_authority_internal.hpp"

#include <gnfs/relation/ooc_relation_store.hpp>
#include <gnfs/util/process.hpp>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace gnfs::sieve::distributed_sieve_resume_detail {

std::unique_ptr<gnfs::relation::OOCRelationWriter>
DistributedSieveMergeStartedWriterMintV1::create_exact_writer() {
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
    throw std::system_error(std::make_error_code(std::errc::operation_not_supported),
                            "distributed merge exact writer is unsupported");
#else
    if (consumed_ || creator_process_id_ == 0 || !writer_lifetime_stable() ||
        target_base_lock_duplicate_ == nullptr || root_fd_ < 0 || directory_fd_ < 0 ||
        base_path_.empty() || private_directory_.empty() || lock_path_.empty()) {
        throw std::logic_error("distributed merge writer mint is invalid or already consumed");
    }

    const auto relation_identity = [](const NativeIdentityV1& identity) noexcept {
        return std::array<std::uint64_t, 3>{
            identity.volume,
            identity.object,
            identity.generation,
        };
    };
    using BaseLock = gnfs::relation::ooc_cleanup_detail::BaseLock;
    const int transferred_lock_fd = target_base_lock_duplicate_->lock_fd_;
    auto adopted_lock = std::unique_ptr<BaseLock>(new BaseLock(
        lock_path_, transferred_lock_fd, root_fd_, receipt_.merge_generation_names_.base_lock_leaf,
        root_identity_, relation_identity(target_base_lock_duplicate_->identity_),
        BaseLock::AdoptInheritedOpenFileDescription{}));
    // BaseLock now owns the exact same-open-file-description duplicate. Clear
    // the WaveStore wrapper only after the throwing adoption has succeeded.
    target_base_lock_duplicate_->lock_fd_ = -1;
    target_base_lock_duplicate_.reset();
    std::shared_ptr<BaseLock> live_lock(std::move(adopted_lock));

    gnfs::relation::OOCPrivateLeaseOwnershipReceipt lease(
        base_path_, private_directory_, lock_path_, directory_identity_, lease_id_,
        owner_marker_identity_, owned_marker_identity_, std::move(live_lock), creator_process_id_);

    gnfs::relation::OOCRelationWriter::ExactPrivateDirectoryBinding exact{
        .root_descriptor = root_fd_,
        .directory_descriptor = directory_fd_,
        .creator_process_id = creator_process_id_,
        .root_identity = root_identity_,
        .directory_identity = directory_identity_,
        .directory_leaf = receipt_.merge_generation_names_.private_directory_leaf,
        .index_leaf = "corpus.relidx",
        .data_leaf = "corpus.reldata",
        .authority_context = this,
        .authority_stable =
            [](const void* context) noexcept {
                return static_cast<const DistributedSieveMergeStartedWriterMintV1*>(context)
                    ->writer_lifetime_stable();
            },
    };

    consumed_ = true;
    return std::unique_ptr<gnfs::relation::OOCRelationWriter>(new gnfs::relation::OOCRelationWriter(
        base_path_.string(), std::move(lease),
        gnfs::relation::OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff, {},
        std::move(exact),
        gnfs::relation::OOCRelationWriter::ExactPrivateDirectoryConstructionToken{}));
#endif
}

} // namespace gnfs::sieve::distributed_sieve_resume_detail

namespace gnfs::sieve::distributed_sieve_merge_writer_authority_detail {

namespace coordinator = distributed_sieve_merge_coordinator_detail;
namespace resume = distributed_sieve_resume_detail;
namespace stream = distributed_sieve_merge_writer_detail;
namespace codec = distributed_sieve_merge_writer_codec_detail;
namespace worker = distributed_sieve_worker_coordinator_detail;
namespace ooc_store = gnfs::relation::detail;
namespace private_lease = gnfs::relation::ooc_cleanup_detail;

namespace {

using AuthorityPhase = DistributedSieveMergeWriterAuthorityPhaseV1;
using AuthorityStatus = DistributedSieveMergeWriterAuthorityStatusV1;
using AuthorityDiagnostic = DistributedSieveMergeWriterAuthorityDiagnosticV1;

[[nodiscard]] constexpr DistributedSieveProtocolStatus
protocol_failure(DistributedSieveProtocolError error,
                 std::uint32_t element_index = DISTRIBUTED_SIEVE_PROTOCOL_NO_INDEX) noexcept {
    return {
        .error = error,
        .byte_offset = DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET,
        .element_index = element_index,
    };
}

[[nodiscard]] AuthorityDiagnostic failure(AuthorityPhase phase, AuthorityStatus status,
                                          std::error_code native_error = {},
                                          bool reconciliation_required = false) noexcept {
    AuthorityDiagnostic diagnostic;
    diagnostic.phase = phase;
    diagnostic.status = status;
    diagnostic.native_error = native_error;
    diagnostic.reconciliation_required = reconciliation_required;
    return diagnostic;
}

[[nodiscard]] std::error_code generic_error(int value) noexcept {
    return value == 0 ? std::error_code{} : std::error_code(value, std::generic_category());
}

[[nodiscard]] TerminalChunkInputV1
project_terminal_input(const worker::DistributedSieveWorkerCoordinatedChunkV1& coordinated) {
    TerminalChunkInputV1 projection;
    projection.chunk_id = coordinated.chunk.chunk_id;
    projection.sq_begin = coordinated.chunk.sq_begin;
    projection.sq_end = coordinated.chunk.sq_end;
    if (coordinated.disposition == worker::DistributedSieveWorkerCoordinationDispositionV1::empty) {
        projection.disposition = ChunkDispositionV1::empty;
        projection.next_sq_index = coordinated.chunk.sq_begin;
        projection.completion_reason = WorkerCompletionReasonV1::zero_relations;
        return projection;
    }

    const auto& handoff = coordinated.adopted->handoff();
    projection.disposition = ChunkDispositionV1::handoff;
    projection.next_sq_index = handoff.next_sq_index;
    projection.processed_sq_count = handoff.processed_sq_count;
    projection.completion_reason = handoff.completion_reason;
    projection.durable_attempt_count = handoff.attempt_ordinal + 1U;
    projection.last_attempt_digest = handoff.attempt_started_digest;
    projection.lease_id = handoff.lease.lease_id;
    projection.handoff_digest = handoff.self_digest;
    projection.raw_relation_count = handoff.relation_count;
    projection.sequence_receipt = handoff.artifact.sequence_receipt;
    projection.corpus_sha256 = handoff.artifact.corpus_sha256;
    return projection;
}

[[nodiscard]] bool bind_manifest_order_readers(DistributedSieveMergeWriterAuthorityStateV1& state,
                                               AuthorityDiagnostic& diagnostic) noexcept;

class MergePreparedPayloadBuildFailure final : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "distributed merge prepared payload build failed";
    }
};

[[nodiscard]] gnfs::relation::OOCPrivateHandoffPayloadV1
build_merge_prepared_payload(const gnfs::relation::OOCFinalizedCorpusEvidenceV1& evidence,
                             void* context);

[[nodiscard]] bool
cached_prepared_payload_is_exact(const DistributedSieveMergeWriterAuthorityStateV1& state) noexcept;

[[nodiscard]] std::error_code
primary_error(const ooc_store::OOCExactFreshConstructionFailure& failure_value,
              AuthorityStatus& status) noexcept {
    try {
        if (failure_value.primary() == nullptr) {
            status = AuthorityStatus::writer_creation_failed;
            return std::make_error_code(std::errc::protocol_error);
        }
        std::rethrow_exception(failure_value.primary());
    } catch (const std::bad_alloc&) {
        status = AuthorityStatus::resource_exhausted;
        return std::make_error_code(std::errc::not_enough_memory);
    } catch (const private_lease::Failure& lease_failure) {
        status = AuthorityStatus::writer_creation_failed;
        return lease_failure.error ? lease_failure.error
                                   : std::make_error_code(std::errc::protocol_error);
    } catch (const std::system_error& error) {
        status = AuthorityStatus::writer_creation_failed;
        return error.code();
    } catch (const std::logic_error&) {
        status = AuthorityStatus::writer_creation_failed;
        return std::make_error_code(std::errc::protocol_error);
    } catch (...) {
        status = AuthorityStatus::unexpected_failure;
        return {};
    }
}

} // namespace

/// Declaration order is intentional. Reverse destruction closes the writer
/// first, then drops borrowed views, then the WaveStore mint/receipt, and only
/// then releases adopted worker readers and the coordinator/WaveLock roots.
struct DistributedSieveMergeWriterAuthorityStateV1 final {
    DistributedSieveMergeWriterAuthorityStateV1(
        worker::DistributedSieveWorkerCoordinatorResultV1&& worker_result_value,
        resume::DistributedSieveMergeStartedWriterMintV1&& mint_value) noexcept
        : worker_result(std::move(worker_result_value)), mint(std::move(mint_value)) {}

    ~DistributedSieveMergeWriterAuthorityStateV1() noexcept {
        if (writer != nullptr && (writer->state() == gnfs::relation::OOCWriterState::Open ||
                                  writer->state() == gnfs::relation::OOCWriterState::Suspended)) {
            writer->abort();
        }
        writer.reset();
    }

    worker::DistributedSieveWorkerCoordinatorResultV1 worker_result;
    resume::DistributedSieveMergeStartedWriterMintV1 mint;
    const WaveManifestV1* manifest = nullptr;
    std::span<const MergeStartedV1> merge_started_chain;
    std::vector<const gnfs::relation::OOCRelationReader*> input_readers;
    std::optional<stream::DistributedSieveMergeWriterReceiptV1> stream_receipt;
    stream::DistributedSieveMergeWriterDiagnosticV1 stream_diagnostic;
    codec::DistributedSieveMergePreparedPayloadBuildDiagnosticV1 codec_diagnostic;
    std::optional<MergePreparedV1> prepared_record;
    std::vector<std::byte> prepared_payload;
    bool handoff_published = false;
    std::unique_ptr<gnfs::relation::OOCRelationWriter> writer;
};

namespace {

bool bind_manifest_order_readers(DistributedSieveMergeWriterAuthorityStateV1& state,
                                 AuthorityDiagnostic& diagnostic) noexcept {
    try {
        if (!state.worker_result || state.manifest == nullptr ||
            state.merge_started_chain.empty()) {
            diagnostic = failure(AuthorityPhase::input_binding, AuthorityStatus::invalid_admission,
                                 {}, true);
            return false;
        }
        const auto& manifest = *state.manifest;
        const auto& latest = state.merge_started_chain.back();
        if (state.worker_result.store == nullptr ||
            std::addressof(state.worker_result.store->manifest()) != state.manifest ||
            state.worker_result.chunks.size() != manifest.chunks.size() ||
            latest.ordered_inputs.size() != manifest.chunks.size() || manifest.chunks.empty()) {
            diagnostic = failure(AuthorityPhase::input_binding,
                                 AuthorityStatus::input_projection_invalid, {}, true);
            return false;
        }

        state.input_readers.clear();
        state.input_readers.reserve(manifest.chunks.size());
        for (std::size_t index = 0; index < manifest.chunks.size(); ++index) {
            diagnostic.manifest_slot = index;
            const auto& coordinated = state.worker_result.chunks[index];
            const auto& expected = latest.ordered_inputs[index];
            if (coordinated.chunk != manifest.chunks[index]) {
                diagnostic.status = AuthorityStatus::input_projection_invalid;
                diagnostic.phase = AuthorityPhase::input_binding;
                diagnostic.reconciliation_required = true;
                return false;
            }

            if (expected.disposition == ChunkDispositionV1::empty) {
                if (coordinated.disposition !=
                        worker::DistributedSieveWorkerCoordinationDispositionV1::empty ||
                    coordinated.adopted.has_value() ||
                    project_terminal_input(coordinated) != expected) {
                    diagnostic.status = AuthorityStatus::input_projection_invalid;
                    diagnostic.phase = AuthorityPhase::input_binding;
                    diagnostic.reconciliation_required = true;
                    return false;
                }
                state.input_readers.push_back(nullptr);
                continue;
            }

            if (expected.disposition != ChunkDispositionV1::handoff ||
                (coordinated.disposition !=
                     worker::DistributedSieveWorkerCoordinationDispositionV1::adopted &&
                 coordinated.disposition !=
                     worker::DistributedSieveWorkerCoordinationDispositionV1::executed) ||
                !coordinated.adopted.has_value() || !coordinated.adopted->valid() ||
                project_terminal_input(coordinated) != expected) {
                diagnostic.status = AuthorityStatus::input_projection_invalid;
                diagnostic.phase = AuthorityPhase::input_binding;
                diagnostic.reconciliation_required = true;
                return false;
            }
            const auto& reader = coordinated.adopted->reader();
            if (!reader.valid() || reader.count() != expected.raw_relation_count) {
                diagnostic.status = AuthorityStatus::input_reader_invalid;
                diagnostic.phase = AuthorityPhase::input_binding;
                diagnostic.reconciliation_required = true;
                return false;
            }
            state.input_readers.push_back(std::addressof(reader));
        }
        diagnostic.manifest_slot = static_cast<std::size_t>(-1);
        return true;
    } catch (const std::bad_alloc&) {
        diagnostic = failure(AuthorityPhase::input_binding, AuthorityStatus::resource_exhausted,
                             std::make_error_code(std::errc::not_enough_memory), true);
        return false;
    } catch (const std::system_error& error) {
        diagnostic = failure(AuthorityPhase::input_binding, AuthorityStatus::input_reader_invalid,
                             error.code(), true);
        return false;
    } catch (...) {
        diagnostic =
            failure(AuthorityPhase::input_binding, AuthorityStatus::unexpected_failure, {}, true);
        return false;
    }
}

gnfs::relation::OOCPrivateHandoffPayloadV1
build_merge_prepared_payload(const gnfs::relation::OOCFinalizedCorpusEvidenceV1& evidence,
                             void* context) {
    auto& state = *static_cast<DistributedSieveMergeWriterAuthorityStateV1*>(context);
    if (state.manifest == nullptr || state.merge_started_chain.empty() ||
        !state.stream_receipt.has_value()) {
        throw MergePreparedPayloadBuildFailure{};
    }
    const auto& receipt = *state.stream_receipt;
    auto built = codec::build_distributed_sieve_merge_prepared_payload_v1(
        *state.manifest, state.merge_started_chain, receipt.input_relation_count,
        receipt.duplicate_relation_count, receipt.output_relation_count,
        receipt.per_chunk_retained_counts, evidence);
    state.codec_diagnostic = built.diagnostic;
    if (!built || !built.prepared.has_value()) {
        throw MergePreparedPayloadBuildFailure{};
    }

    state.prepared_record.emplace(std::move(built.prepared->record));
    state.prepared_payload = std::move(built.prepared->opaque_payload);
    if (state.prepared_payload.empty()) {
        throw MergePreparedPayloadBuildFailure{};
    }
    return {
        .kind = static_cast<std::uint32_t>(DistributedSieveRecordKindV1::merge_prepared),
        .version = state.manifest->handoff_version,
        .bytes = state.prepared_payload,
    };
}

bool cached_prepared_payload_is_exact(
    const DistributedSieveMergeWriterAuthorityStateV1& state) noexcept {
    try {
        if (state.manifest == nullptr || state.merge_started_chain.empty() ||
            !state.stream_receipt.has_value() || !state.prepared_record.has_value() ||
            state.prepared_payload.empty() || !static_cast<bool>(state.codec_diagnostic)) {
            return false;
        }
        DistributedSieveProtocolRecordV1 typed(*state.prepared_record);
        auto encoded = encode_distributed_sieve_record(typed);
        if (!encoded || !encoded.bytes.has_value() || *encoded.bytes != state.prepared_payload) {
            return false;
        }
        const auto& latest = state.merge_started_chain.back();
        const auto& receipt = *state.stream_receipt;
        const auto& prepared = *state.prepared_record;
        return prepared.manifest_digest == state.manifest->self_digest &&
               prepared.work_digest == state.manifest->work_sha256 &&
               prepared.merge_policy_version == state.manifest->merge_policy_version &&
               prepared.merge_started_digest == latest.self_digest &&
               prepared.ordered_inputs == latest.ordered_inputs &&
               prepared.input_relation_count == receipt.input_relation_count &&
               prepared.duplicate_relation_count == receipt.duplicate_relation_count &&
               prepared.output_relation_count == receipt.output_relation_count &&
               prepared.per_chunk_retained_counts == receipt.per_chunk_retained_counts &&
               prepared.merged_lease == latest.merged_lease;
    } catch (...) {
        return false;
    }
}

} // namespace

DistributedSieveMergeWriterAuthorityV1::DistributedSieveMergeWriterAuthorityV1(
    std::unique_ptr<DistributedSieveMergeWriterAuthorityStateV1> state) noexcept
    : state_(std::move(state)) {}

DistributedSieveMergeWriterAuthorityV1::DistributedSieveMergeWriterAuthorityV1(
    DistributedSieveMergeWriterAuthorityV1&& other) noexcept
    : state_(std::move(other.state_)) {}

DistributedSieveMergeWriterAuthorityV1::~DistributedSieveMergeWriterAuthorityV1() noexcept {
    release_state_noexcept();
}

bool DistributedSieveMergeWriterAuthorityV1::state_lifetime_stable(
    const DistributedSieveMergeWriterAuthorityStateV1& state) noexcept {
    return state.mint.writer_lifetime_stable();
}

bool DistributedSieveMergeWriterAuthorityV1::state_process_owned(
    const DistributedSieveMergeWriterAuthorityStateV1& state) noexcept {
    const int process_id = gnfs::util::process_id();
    return state.mint.creator_process_id_ != 0 && process_id > 0 &&
           state.mint.creator_process_id_ == static_cast<std::uint64_t>(process_id);
}

bool DistributedSieveMergeWriterAuthorityV1::validate_prepared_admission_origin(
    const void* lifetime_anchor, const MergePreparedV1* stable_record,
    std::uint64_t creator_process_id) noexcept {
    const auto* state =
        static_cast<const DistributedSieveMergeWriterAuthorityStateV1*>(lifetime_anchor);
    if (state == nullptr || state->writer == nullptr || state->manifest == nullptr ||
        state->merge_started_chain.empty() || !state->stream_receipt.has_value() ||
        !state->prepared_record.has_value() || state->prepared_payload.empty() ||
        !state->handoff_published || !state->worker_result || stable_record == nullptr ||
        stable_record != std::addressof(*state->prepared_record) ||
        creator_process_id != state->mint.creator_process_id_ || !state_process_owned(*state) ||
        state->writer->state() != gnfs::relation::OOCWriterState::Finalized ||
        !cached_prepared_payload_is_exact(*state)) {
        return false;
    }
    for (const auto& coordinated : state->worker_result.chunks) {
        if (coordinated.adopted.has_value() && !coordinated.adopted->valid()) {
            return false;
        }
    }
    return true;
}

void DistributedSieveMergeWriterAuthorityV1::close_state_noexcept(
    std::unique_ptr<DistributedSieveMergeWriterAuthorityStateV1>& state) noexcept {
    if (state == nullptr) {
        return;
    }
    if (state->writer != nullptr) {
        const bool stable =
            state->handoff_published ? state_process_owned(*state) : state_lifetime_stable(*state);
        if (!stable) {
            state->writer->discard_inherited_post_fork_child_noexcept();
        } else if (state->writer->state() == gnfs::relation::OOCWriterState::Open ||
                   state->writer->state() == gnfs::relation::OOCWriterState::Suspended) {
            state->writer->abort();
        }
        state->writer.reset();
    }
    state.reset();
}

void DistributedSieveMergeWriterAuthorityV1::release_state_noexcept() noexcept {
    close_state_noexcept(state_);
}

bool DistributedSieveMergeWriterAuthorityV1::valid() const noexcept {
    if (state_ == nullptr || state_->writer == nullptr || state_->manifest == nullptr ||
        state_->merge_started_chain.empty() || !state_->stream_receipt.has_value() ||
        state_->handoff_published || !state_->worker_result || !state_lifetime_stable(*state_) ||
        state_->writer->state() != gnfs::relation::OOCWriterState::Open) {
        return false;
    }
    for (const auto& coordinated : state_->worker_result.chunks) {
        if (coordinated.adopted.has_value() && !coordinated.adopted->valid()) {
            return false;
        }
    }
    return true;
}

DistributedSieveMergeWriterAuthorityDiagnosticV1
DistributedSieveMergeWriterAuthorityV1::bind_inputs_create_writer_and_stream(
    stream::trusted_test::DistributedSieveMergeWriterTestHooksV1 hooks) noexcept {
    AuthorityDiagnostic diagnostic =
        failure(AuthorityPhase::input_binding, AuthorityStatus::unexpected_failure, {}, true);
    if (state_ == nullptr || !state_->worker_result || !state_lifetime_stable(*state_)) {
        return failure(AuthorityPhase::input_binding, AuthorityStatus::process_mismatch,
                       std::make_error_code(std::errc::no_such_process), true);
    }

    try {
        state_->manifest = std::addressof(state_->mint.manifest());
        state_->merge_started_chain = state_->mint.merge_started_chain();
        if (state_->merge_started_chain.empty()) {
            diagnostic = failure(AuthorityPhase::input_binding,
                                 AuthorityStatus::merge_chain_invalid, {}, true);
            diagnostic.stream.phase =
                stream::DistributedSieveMergeWriterPhaseV1::merge_chain_validation;
            diagnostic.stream.status =
                stream::DistributedSieveMergeWriterStatusV1::empty_merge_chain;
            diagnostic.stream.protocol =
                protocol_failure(DistributedSieveProtocolError::invalid_value);
            return diagnostic;
        }
        if (const auto chain = validate_merge_predecessor_chain(
                *state_->manifest, state_->merge_started_chain, nullptr, nullptr);
            !chain) {
            diagnostic = failure(AuthorityPhase::input_binding,
                                 AuthorityStatus::merge_chain_invalid, {}, true);
            diagnostic.stream.phase =
                stream::DistributedSieveMergeWriterPhaseV1::merge_chain_validation;
            diagnostic.stream.status =
                stream::DistributedSieveMergeWriterStatusV1::merge_chain_invalid;
            diagnostic.stream.protocol = chain;
            return diagnostic;
        }
        if (!bind_manifest_order_readers(*state_, diagnostic)) {
            return diagnostic;
        }

        diagnostic = failure(AuthorityPhase::writer_creation,
                             AuthorityStatus::writer_creation_failed, {}, true);
        state_->writer = state_->mint.create_exact_writer();
        if (state_->writer == nullptr || !state_lifetime_stable(*state_)) {
            return failure(AuthorityPhase::writer_creation, AuthorityStatus::writer_creation_failed,
                           std::make_error_code(std::errc::state_not_recoverable), true);
        }
    } catch (const ooc_store::OOCExactFreshConstructionFailure& exact_failure) {
        AuthorityStatus status = AuthorityStatus::writer_creation_failed;
        auto native_error = primary_error(exact_failure, status);
        if (!native_error) {
            native_error = exact_failure.rollback_error();
        }
        return failure(AuthorityPhase::writer_creation, status, native_error, true);
    } catch (const std::bad_alloc&) {
        return failure(AuthorityPhase::writer_creation, AuthorityStatus::resource_exhausted,
                       std::make_error_code(std::errc::not_enough_memory), true);
    } catch (const private_lease::Failure& lease_failure) {
        return failure(AuthorityPhase::writer_creation, AuthorityStatus::writer_creation_failed,
                       lease_failure.error ? lease_failure.error
                                           : std::make_error_code(std::errc::protocol_error),
                       true);
    } catch (const std::system_error& error) {
        return failure(AuthorityPhase::writer_creation, AuthorityStatus::writer_creation_failed,
                       error.code(), true);
    } catch (...) {
        return failure(AuthorityPhase::writer_creation, AuthorityStatus::unexpected_failure, {},
                       true);
    }

    stream::DistributedSieveMergeWriterResultV1 streamed;
    try {
        streamed = [&] {
            auto batch = state_->writer->begin_exact_append_batch();
            auto result =
                hooks.after_output_write == nullptr
                    ? stream::stream_distributed_sieve_merge_inputs_v1(
                          *state_->manifest, state_->merge_started_chain, state_->input_readers,
                          *state_->writer)
                    : stream::trusted_test::stream_distributed_sieve_merge_inputs_v1_with_hooks(
                          *state_->manifest, state_->merge_started_chain, state_->input_readers,
                          *state_->writer, hooks);
            state_->stream_diagnostic = result.diagnostic;
            if (result) {
                batch.commit();
            }
            return result;
        }();
    } catch (const std::bad_alloc&) {
        diagnostic = failure(AuthorityPhase::streaming, AuthorityStatus::resource_exhausted,
                             std::make_error_code(std::errc::not_enough_memory), true);
        diagnostic.stream = state_->stream_diagnostic;
        return diagnostic;
    } catch (const std::system_error& error) {
        diagnostic =
            failure(AuthorityPhase::streaming, AuthorityStatus::stream_failed, error.code(), true);
        diagnostic.stream = state_->stream_diagnostic;
        return diagnostic;
    } catch (...) {
        diagnostic = failure(AuthorityPhase::streaming, AuthorityStatus::unexpected_failure,
                             generic_error(EIO), true);
        diagnostic.stream = state_->stream_diagnostic;
        return diagnostic;
    }
    if (!streamed || !streamed.receipt.has_value()) {
        if (state_->writer != nullptr) {
            state_->writer->abort();
        }
        diagnostic = failure(AuthorityPhase::streaming, AuthorityStatus::stream_failed, {}, true);
        diagnostic.stream = streamed.diagnostic;
        return diagnostic;
    }
    state_->stream_receipt.emplace(std::move(*streamed.receipt));

    diagnostic = failure(AuthorityPhase::complete, AuthorityStatus::ready);
    diagnostic.stream = state_->stream_diagnostic;
    return diagnostic;
}

DistributedSieveMergePreparedResultV1 DistributedSieveMergeWriterAuthorityV1::publish_impl(
    gnfs::relation::OOCPrivateHandoffTestHooks hooks) noexcept {
    auto state = std::move(state_);
    if (state == nullptr || state->writer == nullptr || state->manifest == nullptr ||
        state->merge_started_chain.empty() || !state->stream_receipt.has_value() ||
        state->handoff_published || !state->worker_result || !state_lifetime_stable(*state) ||
        state->writer->state() != gnfs::relation::OOCWriterState::Open) {
        close_state_noexcept(state);
        return {
            .admission = std::nullopt,
            .diagnostic =
                failure(AuthorityPhase::finalization, AuthorityStatus::invalid_admission, {}, true),
        };
    }

    try {
        state->writer->finalize_and_publish_private_handoff_built(build_merge_prepared_payload,
                                                                  state.get(), hooks);
        if (!state_process_owned(*state) ||
            state->writer->state() != gnfs::relation::OOCWriterState::Finalized ||
            !cached_prepared_payload_is_exact(*state)) {
            auto diagnostic = failure(AuthorityPhase::handoff_publication,
                                      AuthorityStatus::handoff_publication_failed,
                                      std::make_error_code(std::errc::protocol_error), true);
            diagnostic.stream = state->stream_diagnostic;
            diagnostic.codec = state->codec_diagnostic;
            close_state_noexcept(state);
            return {.admission = std::nullopt, .diagnostic = std::move(diagnostic)};
        }
        state->handoff_published = true;

        auto diagnostic = failure(AuthorityPhase::complete, AuthorityStatus::ready);
        diagnostic.stream = state->stream_diagnostic;
        diagnostic.codec = state->codec_diagnostic;
        const auto* stable_record = std::addressof(*state->prepared_record);
        const std::uint64_t creator_process_id = state->mint.creator_process_id_;
        std::shared_ptr<const void> lifetime_anchor(std::move(state));
        DistributedSieveMergePreparedAdmissionV1 admission(
            std::move(lifetime_anchor), stable_record, creator_process_id,
            &DistributedSieveMergeWriterAuthorityV1::validate_prepared_admission_origin);
        return {
            .admission =
                std::optional<DistributedSieveMergePreparedAdmissionV1>(std::move(admission)),
            .diagnostic = std::move(diagnostic),
        };
    } catch (const MergePreparedPayloadBuildFailure&) {
        auto diagnostic =
            failure(AuthorityPhase::payload_build, AuthorityStatus::payload_build_failed, {}, true);
        diagnostic.stream = state->stream_diagnostic;
        diagnostic.codec = state->codec_diagnostic;
        close_state_noexcept(state);
        return {.admission = std::nullopt, .diagnostic = std::move(diagnostic)};
    } catch (const std::bad_alloc&) {
        auto diagnostic =
            failure(state->prepared_record.has_value() ? AuthorityPhase::handoff_publication
                                                       : AuthorityPhase::payload_build,
                    AuthorityStatus::resource_exhausted,
                    std::make_error_code(std::errc::not_enough_memory), true);
        diagnostic.stream = state->stream_diagnostic;
        diagnostic.codec = state->codec_diagnostic;
        close_state_noexcept(state);
        return {.admission = std::nullopt, .diagnostic = std::move(diagnostic)};
    } catch (const std::system_error& error) {
        const auto phase = state->writer != nullptr && state->writer->state() ==
                                                           gnfs::relation::OOCWriterState::Finalized
                               ? AuthorityPhase::handoff_publication
                               : AuthorityPhase::finalization;
        auto diagnostic =
            failure(phase, AuthorityStatus::handoff_publication_failed, error.code(), true);
        diagnostic.stream = state->stream_diagnostic;
        diagnostic.codec = state->codec_diagnostic;
        close_state_noexcept(state);
        return {.admission = std::nullopt, .diagnostic = std::move(diagnostic)};
    } catch (...) {
        const auto phase = state->writer != nullptr && state->writer->state() ==
                                                           gnfs::relation::OOCWriterState::Finalized
                               ? AuthorityPhase::handoff_publication
                               : AuthorityPhase::finalization;
        auto diagnostic =
            failure(phase, AuthorityStatus::unexpected_failure, generic_error(EIO), true);
        diagnostic.stream = state->stream_diagnostic;
        diagnostic.codec = state->codec_diagnostic;
        close_state_noexcept(state);
        return {.admission = std::nullopt, .diagnostic = std::move(diagnostic)};
    }
}

DistributedSieveMergeWriterAdoptionResultV1
trusted_test::consume_distributed_sieve_merge_generation_v1_with_hooks(
    DistributedSieveMergeGenerationAdmissionV1&& admission,
    DistributedSieveMergeWriterAdoptionTestHooksV1 hooks) noexcept {
    if (!admission || admission.started_receipt_ == std::nullopt) {
        return {
            .authority = std::nullopt,
            .diagnostic =
                failure(AuthorityPhase::admission_validation, AuthorityStatus::invalid_admission,
                        std::make_error_code(std::errc::invalid_argument)),
        };
    }
#if !defined(__APPLE__)
    (void)admission;
    (void)hooks;
    return {
        .authority = std::nullopt,
        .diagnostic = failure(AuthorityPhase::platform_gate, AuthorityStatus::platform_unsupported,
                              std::make_error_code(std::errc::operation_not_supported)),
    };
#else
    auto minted = resume::consume_distributed_sieve_merge_started_writer_v1(
        std::move(*admission.started_receipt_));
    if (!minted || !minted.mint.has_value()) {
        auto diagnostic =
            failure(AuthorityPhase::wave_mint, AuthorityStatus::wave_mint_failed, {}, true);
        diagnostic.wave_store = std::move(minted.diagnostic);
        diagnostic.native_error = diagnostic.wave_store.native_error;
        return {.authority = std::nullopt, .diagnostic = std::move(diagnostic)};
    }

    try {
        auto state = std::make_unique<DistributedSieveMergeWriterAuthorityStateV1>(
            std::move(admission.worker_result_), std::move(*minted.mint));
        DistributedSieveMergeWriterAuthorityV1 authority(std::move(state));
        auto diagnostic = authority.bind_inputs_create_writer_and_stream(hooks.stream_hooks);
        diagnostic.wave_store = std::move(minted.diagnostic);
        if (!static_cast<bool>(diagnostic) || !authority.valid()) {
            return {.authority = std::nullopt, .diagnostic = std::move(diagnostic)};
        }
        return {
            .authority =
                std::optional<DistributedSieveMergeWriterAuthorityV1>(std::move(authority)),
            .diagnostic = std::move(diagnostic),
        };
    } catch (const std::bad_alloc&) {
        return {
            .authority = std::nullopt,
            .diagnostic = failure(AuthorityPhase::wave_mint, AuthorityStatus::resource_exhausted,
                                  std::make_error_code(std::errc::not_enough_memory), true),
        };
    } catch (...) {
        return {
            .authority = std::nullopt,
            .diagnostic =
                failure(AuthorityPhase::wave_mint, AuthorityStatus::unexpected_failure, {}, true),
        };
    }
#endif
}

DistributedSieveMergeWriterAdoptionResultV1 consume_distributed_sieve_merge_generation_v1(
    DistributedSieveMergeGenerationAdmissionV1&& admission) noexcept {
    return trusted_test::consume_distributed_sieve_merge_generation_v1_with_hooks(
        std::move(admission), {});
}

DistributedSieveMergePreparedResultV1 publish_distributed_sieve_merge_prepared_v1(
    DistributedSieveMergeWriterAuthorityV1&& authority) noexcept {
    return authority.publish_impl({});
}

namespace trusted_test {

DistributedSieveMergePreparedResultV1 publish_distributed_sieve_merge_prepared_v1_with_hooks(
    DistributedSieveMergeWriterAuthorityV1&& authority,
    DistributedSieveMergePreparedPublicationTestHooksV1 hooks) noexcept {
    return authority.publish_impl(hooks.private_handoff_hooks);
}

} // namespace trusted_test

} // namespace gnfs::sieve::distributed_sieve_merge_writer_authority_detail

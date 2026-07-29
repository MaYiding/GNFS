#include "distributed_sieve_worker_writer_internal.hpp"

#include <gnfs/relation/ooc_cleanup_transaction.hpp>
#include <gnfs/relation/ooc_relation_store.hpp>
#include <gnfs/util/process.hpp>

#include <array>
#include <cerrno>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace gnfs::sieve::distributed_sieve_worker_entry_detail {

namespace writer_detail = distributed_sieve_worker_writer_detail;
namespace private_lease = gnfs::relation::ooc_cleanup_detail;
namespace ooc_store = gnfs::relation::detail;

namespace {

static_assert(std::is_nothrow_move_constructible_v<WorkerHandoffV1>);

[[nodiscard]] std::uint64_t current_process_id() noexcept {
    const int id = gnfs::util::process_id();
    return id > 0 ? static_cast<std::uint64_t>(id) : 0;
}

[[nodiscard]] DistributedSieveWorkerWriterDiagnosticV1
failure(DistributedSieveWorkerWriterPhaseV1 phase, DistributedSieveWorkerWriterStatusV1 status,
        int native_error = 0,
        DistributedSieveWorkerWriterRollbackV1 rollback =
            DistributedSieveWorkerWriterRollbackV1::not_applicable,
        int rollback_native_error = 0) noexcept {
    return {
        .phase = phase,
        .status = status,
        .native_error = native_error,
        .rollback = rollback,
        .rollback_native_error = rollback_native_error,
    };
}

[[nodiscard]] DistributedSieveWorkerWriterRollbackV1
writer_rollback(ooc_store::OOCExactFreshRollbackDisposition rollback) noexcept {
    switch (rollback) {
    case ooc_store::OOCExactFreshRollbackDisposition::Clean:
        return DistributedSieveWorkerWriterRollbackV1::clean;
    case ooc_store::OOCExactFreshRollbackDisposition::NamedResidueMayRemain:
        return DistributedSieveWorkerWriterRollbackV1::named_residue_may_remain;
    case ooc_store::OOCExactFreshRollbackDisposition::DirectoryDurabilityUncertain:
        return DistributedSieveWorkerWriterRollbackV1::directory_durability_uncertain;
    }
    return DistributedSieveWorkerWriterRollbackV1::named_residue_may_remain;
}

[[nodiscard]] DistributedSieveWorkerWriterDiagnosticV1
map_writer_creation_failure(const std::exception_ptr& primary,
                            DistributedSieveWorkerWriterRollbackV1 rollback,
                            int rollback_native_error) noexcept {
    try {
        if (primary == nullptr) {
            throw std::runtime_error("missing exact writer primary failure");
        }
        std::rethrow_exception(primary);
    } catch (const std::bad_alloc&) {
        return failure(DistributedSieveWorkerWriterPhaseV1::writer_creation,
                       DistributedSieveWorkerWriterStatusV1::resource_exhausted, ENOMEM, rollback,
                       rollback_native_error);
    } catch (const private_lease::Failure& lease_failure) {
        return failure(DistributedSieveWorkerWriterPhaseV1::lock_adoption,
                       DistributedSieveWorkerWriterStatusV1::lock_invalid,
                       lease_failure.error ? lease_failure.error.value() : EPROTO, rollback,
                       rollback_native_error);
    } catch (const std::system_error& error) {
        return failure(DistributedSieveWorkerWriterPhaseV1::writer_creation,
                       DistributedSieveWorkerWriterStatusV1::writer_failed, error.code().value(),
                       rollback, rollback_native_error);
    } catch (const std::logic_error&) {
        return failure(DistributedSieveWorkerWriterPhaseV1::writer_creation,
                       DistributedSieveWorkerWriterStatusV1::private_lease_invalid, EPROTO,
                       rollback, rollback_native_error);
    } catch (...) {
        return failure(DistributedSieveWorkerWriterPhaseV1::writer_creation,
                       DistributedSieveWorkerWriterStatusV1::unexpected_failure, 0, rollback,
                       rollback_native_error);
    }
}

struct WorkerHandoffBuildContext final {
    const AttemptStartedV1* attempt = nullptr;
    const WaveManifestV1* manifest = nullptr;
    const ChunkPlanV1* chunk = nullptr;
    const DistributedSieveWorkerCompletionFactsV1* completion = nullptr;
    std::optional<WorkerHandoffV1>* pending_handoff = nullptr;
    std::vector<std::byte>* pending_payload = nullptr;
    bool fail_before_retry_cache_commit = false;
};

void validate_worker_completion(const AttemptStartedV1& attempt, const WaveManifestV1& manifest,
                                std::uint64_t relation_count,
                                const DistributedSieveWorkerCompletionFactsV1& completion) {
    if (attempt.sq_end <= attempt.sq_begin || completion.next_sq_index < attempt.sq_begin ||
        completion.next_sq_index > attempt.sq_end ||
        completion.processed_sq_count >
            static_cast<std::uint64_t>(completion.next_sq_index - attempt.sq_begin)) {
        throw std::invalid_argument("distributed worker completion facts are out of range");
    }
    switch (completion.completion_reason) {
    case WorkerCompletionReasonV1::range_exhausted:
        if (completion.next_sq_index != attempt.sq_end || completion.processed_sq_count == 0 ||
            relation_count == 0) {
            throw std::invalid_argument(
                "distributed worker range-exhausted completion is inconsistent");
        }
        return;
    case WorkerCompletionReasonV1::sq_cap:
        if (completion.next_sq_index >= attempt.sq_end || completion.processed_sq_count == 0) {
            throw std::invalid_argument("distributed worker SQ-cap completion is inconsistent");
        }
        if (manifest.sq_cap_per_worker == 0 ||
            completion.processed_sq_count != manifest.sq_cap_per_worker) {
            throw std::invalid_argument(
                "distributed worker SQ-cap completion does not match the manifest");
        }
        return;
    case WorkerCompletionReasonV1::relation_cap:
        if (completion.next_sq_index >= attempt.sq_end || completion.processed_sq_count == 0 ||
            relation_count == 0) {
            throw std::invalid_argument(
                "distributed worker relation-cap completion is inconsistent");
        }
        if (manifest.relation_cap_per_worker == 0 ||
            relation_count < manifest.relation_cap_per_worker ||
            (manifest.sq_cap_per_worker > 0 &&
             completion.processed_sq_count >= manifest.sq_cap_per_worker)) {
            throw std::invalid_argument(
                "distributed worker relation-cap completion does not match the manifest");
        }
        return;
    case WorkerCompletionReasonV1::zero_relations:
        if (completion.next_sq_index != attempt.sq_end || relation_count != 0) {
            throw std::invalid_argument(
                "distributed worker zero-relation completion is inconsistent");
        }
        return;
    }
    throw std::invalid_argument("distributed worker completion reason is unknown");
}

[[nodiscard]] NativeIdentityV1
protocol_native_identity(const std::array<std::uint64_t, 3>& identity) noexcept {
    return {
        .volume = identity[0],
        .object = identity[1],
        .generation = identity[2],
    };
}

[[nodiscard]] bool worker_handoff_matches_evidence(
    const WorkerHandoffV1& handoff,
    const gnfs::relation::OOCFinalizedCorpusEvidenceV1& evidence) noexcept;

[[nodiscard]] gnfs::relation::OOCPrivateHandoffPayloadV1
build_worker_handoff_payload(const gnfs::relation::OOCFinalizedCorpusEvidenceV1& evidence,
                             void* opaque_context) {
    auto& context = *static_cast<WorkerHandoffBuildContext*>(opaque_context);
    if (context.attempt == nullptr || context.manifest == nullptr || context.chunk == nullptr ||
        context.completion == nullptr || context.pending_handoff == nullptr ||
        context.pending_payload == nullptr || context.pending_handoff->has_value() ||
        !context.pending_payload->empty()) {
        throw std::logic_error("distributed worker handoff builder context is invalid");
    }
    const auto& attempt = *context.attempt;
    const auto& manifest = *context.manifest;
    const auto& chunk = *context.chunk;
    const auto& completion = *context.completion;

    if (attempt.manifest_digest != manifest.self_digest || attempt.chunk_id != chunk.chunk_id ||
        attempt.sq_begin != chunk.sq_begin || attempt.sq_end != chunk.sq_end ||
        evidence.descriptor.format_version != manifest.ooc_format_version ||
        evidence.descriptor.count != evidence.sequence_receipt.relation_count ||
        manifest.handoff_version != DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1 ||
        manifest.receipt_version != 1 || manifest.digest_version != 1) {
        throw std::logic_error(
            "distributed worker handoff inputs do not share one protocol identity");
    }

    DistributedSieveProtocolRecordV1 sealed = WorkerHandoffV1{
        .manifest_digest = manifest.self_digest,
        .work_digest = manifest.work_sha256,
        .wave_id = manifest.wave_id,
        .chunk_id = attempt.chunk_id,
        .sq_begin = attempt.sq_begin,
        .sq_end = attempt.sq_end,
        .attempt_ordinal = attempt.attempt_ordinal,
        .attempt_started_digest = attempt.self_digest,
        .lease = attempt.lease,
        .artifact =
            {
                .descriptor =
                    {
                        .format_version = evidence.descriptor.format_version,
                        .store_id = evidence.descriptor.store_id,
                        .generation = evidence.descriptor.generation,
                        .relation_count = evidence.descriptor.count,
                        .data_end = evidence.descriptor.data_end,
                    },
                .index_file =
                    {
                        .identity = protocol_native_identity(evidence.index_file.identity),
                        .extent = evidence.index_file.extent,
                    },
                .data_file =
                    {
                        .identity = protocol_native_identity(evidence.data_file.identity),
                        .extent = evidence.data_file.extent,
                    },
                .sequence_receipt =
                    {
                        .relation_count = evidence.sequence_receipt.relation_count,
                        .low = evidence.sequence_receipt.low,
                        .high = evidence.sequence_receipt.high,
                    },
                .corpus_sha256 = evidence.corpus_sha256,
            },
        .processed_sq_count = completion.processed_sq_count,
        .next_sq_index = completion.next_sq_index,
        .completion_reason = completion.completion_reason,
        .relation_count = evidence.descriptor.count,
        .cleanup_intent_absent = true,
    };
    if (const auto status = seal_distributed_sieve_record(sealed); !status) {
        throw std::runtime_error("distributed worker handoff sealing failed");
    }
    const auto encoded = encode_distributed_sieve_record(sealed);
    if (!encoded || !encoded.bytes) {
        throw std::runtime_error("distributed worker handoff encoding failed");
    }
    const auto decoded = decode_distributed_sieve_record(*encoded.bytes);
    if (!decoded || !decoded.value) {
        throw std::runtime_error("distributed worker handoff round-trip validation failed");
    }
    const auto* handoff = std::get_if<WorkerHandoffV1>(&*decoded.value);
    if (handoff == nullptr || !validate_distributed_sieve_record(*decoded.value, true) ||
        !worker_handoff_matches_evidence(*handoff, evidence)) {
        throw std::runtime_error("distributed worker handoff decoded as a foreign record");
    }

    // Complete every allocation before mutating the retry cache. Moving the
    // finished values below is noexcept, so allocation failure cannot pair a
    // typed handoff with a missing or different payload.
    WorkerHandoffV1 cached_handoff = *handoff;
    std::vector<std::byte> cached_payload = *encoded.bytes;
    std::vector<std::byte> returned_payload = *encoded.bytes;
    if (context.fail_before_retry_cache_commit) {
        throw std::bad_alloc();
    }
    context.pending_handoff->emplace(std::move(cached_handoff));
    context.pending_payload->swap(cached_payload);
    return {
        .kind = static_cast<std::uint32_t>(DistributedSieveRecordKindV1::worker_handoff),
        .version = manifest.handoff_version,
        .bytes = std::move(returned_payload),
    };
}

[[nodiscard]] bool worker_handoff_matches_evidence(
    const WorkerHandoffV1& handoff,
    const gnfs::relation::OOCFinalizedCorpusEvidenceV1& evidence) noexcept {
    const auto& artifact = handoff.artifact;
    return artifact.descriptor.format_version == evidence.descriptor.format_version &&
           artifact.descriptor.store_id == evidence.descriptor.store_id &&
           artifact.descriptor.generation == evidence.descriptor.generation &&
           artifact.descriptor.relation_count == evidence.descriptor.count &&
           artifact.descriptor.data_end == evidence.descriptor.data_end &&
           artifact.index_file.identity == protocol_native_identity(evidence.index_file.identity) &&
           artifact.index_file.extent == evidence.index_file.extent &&
           artifact.data_file.identity == protocol_native_identity(evidence.data_file.identity) &&
           artifact.data_file.extent == evidence.data_file.extent &&
           artifact.sequence_receipt.relation_count == evidence.sequence_receipt.relation_count &&
           artifact.sequence_receipt.low == evidence.sequence_receipt.low &&
           artifact.sequence_receipt.high == evidence.sequence_receipt.high &&
           artifact.corpus_sha256 == evidence.corpus_sha256;
}

[[nodiscard]] bool cached_worker_handoff_is_valid(const std::optional<WorkerHandoffV1>& handoff,
                                                  std::span<const std::byte> payload) {
    if (!handoff || payload.empty()) {
        return false;
    }
    const auto decoded = decode_distributed_sieve_record(payload);
    if (!decoded || !decoded.value || !validate_distributed_sieve_record(*decoded.value, true)) {
        return false;
    }
    const auto* decoded_handoff = std::get_if<WorkerHandoffV1>(&*decoded.value);
    return decoded_handoff != nullptr && decoded_handoff->self_digest == handoff->self_digest;
}

} // namespace

namespace distributed_sieve_worker_writer_detail {

DistributedSieveWorkerWriterLifetimeGuardV1::DistributedSieveWorkerWriterLifetimeGuardV1(
    void* state, Revalidate revalidate, Destroy destroy) noexcept
    : state_(state), revalidate_(revalidate), destroy_(destroy) {}

DistributedSieveWorkerWriterLifetimeGuardV1::DistributedSieveWorkerWriterLifetimeGuardV1(
    DistributedSieveWorkerWriterLifetimeGuardV1&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)),
      revalidate_(std::exchange(other.revalidate_, nullptr)),
      destroy_(std::exchange(other.destroy_, nullptr)) {}

DistributedSieveWorkerWriterLifetimeGuardV1& DistributedSieveWorkerWriterLifetimeGuardV1::operator=(
    DistributedSieveWorkerWriterLifetimeGuardV1&& other) noexcept {
    if (this != &other) {
        reset_noexcept();
        state_ = std::exchange(other.state_, nullptr);
        revalidate_ = std::exchange(other.revalidate_, nullptr);
        destroy_ = std::exchange(other.destroy_, nullptr);
    }
    return *this;
}

DistributedSieveWorkerWriterLifetimeGuardV1::
    ~DistributedSieveWorkerWriterLifetimeGuardV1() noexcept {
    reset_noexcept();
}

bool DistributedSieveWorkerWriterLifetimeGuardV1::valid() const noexcept {
    return state_ != nullptr && revalidate_ != nullptr && destroy_ != nullptr;
}

bool DistributedSieveWorkerWriterLifetimeGuardV1::stable() const noexcept {
    return valid() && revalidate_(state_);
}

void DistributedSieveWorkerWriterLifetimeGuardV1::reset_noexcept() noexcept {
    void* state = std::exchange(state_, nullptr);
    auto destroy = std::exchange(destroy_, nullptr);
    revalidate_ = nullptr;
    if (state != nullptr && destroy != nullptr) {
        destroy(state);
    }
}

OOCInheritedP8WriterMintV1::OOCInheritedP8WriterMintV1(
    int root_descriptor, int permanent_lock_descriptor, int attempt_lock_descriptor,
    int attempt_directory_descriptor, int package_descriptor, std::uint64_t creator_process_id,
    std::filesystem::path absolute_root_path, std::filesystem::path base_path,
    std::filesystem::path private_directory, std::filesystem::path lock_path,
    std::string private_directory_leaf, std::string lock_leaf,
    std::array<std::uint64_t, 3> root_identity, std::array<std::uint64_t, 3> attempt_lock_identity,
    std::array<std::uint64_t, 3> attempt_directory_identity, std::array<std::uint64_t, 2> lease_id,
    std::array<std::uint64_t, 3> owner_marker_identity,
    std::array<std::uint64_t, 3> owned_marker_identity, AttemptStartedV1 record,
    WaveManifestV1 manifest, DistributedSieveWorkIdentityV1 identity, ChunkPlanV1 chunk,
    distributed_sieve_work_package_codec_detail::DistributedSieveWorkPackageWitnessV1
        package_witness,
    gnfs::relation::OOCPrivateLeaseTestHooks private_lease_hooks)
    : root_descriptor_(root_descriptor), permanent_lock_descriptor_(permanent_lock_descriptor),
      attempt_lock_descriptor_(attempt_lock_descriptor),
      attempt_directory_descriptor_(attempt_directory_descriptor),
      package_descriptor_(package_descriptor), creator_process_id_(creator_process_id),
      absolute_root_path_(std::move(absolute_root_path)), base_path_(std::move(base_path)),
      private_directory_(std::move(private_directory)), lock_path_(std::move(lock_path)),
      private_directory_leaf_(std::move(private_directory_leaf)), lock_leaf_(std::move(lock_leaf)),
      root_identity_(root_identity), attempt_lock_identity_(attempt_lock_identity),
      attempt_directory_identity_(attempt_directory_identity), lease_id_(lease_id),
      owner_marker_identity_(owner_marker_identity), owned_marker_identity_(owned_marker_identity),
      record_(std::move(record)), manifest_(std::move(manifest)), identity_(std::move(identity)),
      chunk_(std::move(chunk)), package_witness_(std::move(package_witness)),
      private_lease_hooks_(private_lease_hooks) {}

OOCInheritedP8WriterMintV1::OOCInheritedP8WriterMintV1(OOCInheritedP8WriterMintV1&& other) noexcept
    : root_descriptor_(std::exchange(other.root_descriptor_, -1)),
      permanent_lock_descriptor_(std::exchange(other.permanent_lock_descriptor_, -1)),
      attempt_lock_descriptor_(std::exchange(other.attempt_lock_descriptor_, -1)),
      attempt_directory_descriptor_(std::exchange(other.attempt_directory_descriptor_, -1)),
      package_descriptor_(std::exchange(other.package_descriptor_, -1)),
      creator_process_id_(std::exchange(other.creator_process_id_, 0)),
      consumed_(std::exchange(other.consumed_, true)),
      lifetime_guard_(std::move(other.lifetime_guard_)),
      absolute_root_path_(std::move(other.absolute_root_path_)),
      base_path_(std::move(other.base_path_)),
      private_directory_(std::move(other.private_directory_)),
      lock_path_(std::move(other.lock_path_)),
      private_directory_leaf_(std::move(other.private_directory_leaf_)),
      lock_leaf_(std::move(other.lock_leaf_)), root_identity_(other.root_identity_),
      attempt_lock_identity_(other.attempt_lock_identity_),
      attempt_directory_identity_(other.attempt_directory_identity_), lease_id_(other.lease_id_),
      owner_marker_identity_(other.owner_marker_identity_),
      owned_marker_identity_(other.owned_marker_identity_), record_(std::move(other.record_)),
      manifest_(std::move(other.manifest_)), identity_(std::move(other.identity_)),
      chunk_(std::move(other.chunk_)), package_witness_(std::move(other.package_witness_)),
      private_lease_hooks_(other.private_lease_hooks_) {}

OOCInheritedP8WriterMintV1::~OOCInheritedP8WriterMintV1() noexcept {
    close_descriptors_noexcept();
}

void OOCInheritedP8WriterMintV1::close_descriptors_noexcept() noexcept {
#if !defined(_WIN32)
    const int descriptors[] = {
        package_descriptor_,      attempt_directory_descriptor_,
        attempt_lock_descriptor_, permanent_lock_descriptor_,
        root_descriptor_,
    };
    for (const int descriptor : descriptors) {
        if (descriptor >= 0) {
            (void)::close(descriptor);
        }
    }
#endif
    root_descriptor_ = -1;
    permanent_lock_descriptor_ = -1;
    attempt_lock_descriptor_ = -1;
    attempt_directory_descriptor_ = -1;
    package_descriptor_ = -1;
}

void OOCInheritedP8WriterMintV1::attach_lifetime_guard(
    DistributedSieveWorkerWriterLifetimeGuardV1&& guard) noexcept {
    lifetime_guard_ = std::move(guard);
}

void OOCInheritedP8WriterMintV1::disarm_writer_post_fork_child_noexcept(
    gnfs::relation::OOCRelationWriter& writer) noexcept {
    writer.discard_inherited_post_fork_child_noexcept();
}

std::unique_ptr<gnfs::relation::OOCRelationWriter>
OOCInheritedP8WriterMintV1::create_exact_writer() {
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
    throw std::system_error(std::make_error_code(std::errc::operation_not_supported),
                            "distributed worker exact writer is unsupported");
#else
    if (consumed_ || creator_process_id_ == 0 || current_process_id() != creator_process_id_ ||
        !lifetime_guard_.stable() || root_descriptor_ < 0 || permanent_lock_descriptor_ < 0 ||
        attempt_lock_descriptor_ < 0 || attempt_directory_descriptor_ < 0 ||
        package_descriptor_ < 0 || base_path_.empty() || private_directory_.empty() ||
        lock_path_.empty() || private_directory_leaf_.empty() || lock_leaf_.empty()) {
        throw std::logic_error("distributed worker writer mint is invalid or already consumed");
    }
    consumed_ = true;

    using BaseLock = gnfs::relation::ooc_cleanup_detail::BaseLock;
    auto adopted_lock = std::unique_ptr<BaseLock>(new BaseLock(
        lock_path_, attempt_lock_descriptor_, root_descriptor_, lock_leaf_, root_identity_,
        attempt_lock_identity_, BaseLock::AdoptInheritedOpenFileDescription{}));
    attempt_lock_descriptor_ = -1;
    std::shared_ptr<BaseLock> live_lock(std::move(adopted_lock));

    gnfs::relation::OOCPrivateLeaseOwnershipReceipt lease(
        base_path_, private_directory_, lock_path_, attempt_directory_identity_, lease_id_,
        owner_marker_identity_, owned_marker_identity_, std::move(live_lock), creator_process_id_);

    gnfs::relation::OOCRelationWriter::ExactPrivateDirectoryBinding exact{
        .root_descriptor = root_descriptor_,
        .directory_descriptor = attempt_directory_descriptor_,
        .creator_process_id = creator_process_id_,
        .root_identity = root_identity_,
        .directory_identity = attempt_directory_identity_,
        .directory_leaf = private_directory_leaf_,
        .index_leaf = "corpus.relidx",
        .data_leaf = "corpus.reldata",
        .authority_context = &lifetime_guard_,
        .authority_stable =
            [](const void* context) noexcept {
                return static_cast<const DistributedSieveWorkerWriterLifetimeGuardV1*>(context)
                    ->stable();
            },
    };
    return std::unique_ptr<gnfs::relation::OOCRelationWriter>(new gnfs::relation::OOCRelationWriter(
        base_path_.string(), std::move(lease),
        gnfs::relation::OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff,
        private_lease_hooks_, std::move(exact),
        gnfs::relation::OOCRelationWriter::ExactPrivateDirectoryConstructionToken{}));
#endif
}

} // namespace distributed_sieve_worker_writer_detail

struct DistributedSieveWorkerWriterAuthorityV1::State final {
    State(writer_detail::OOCInheritedP8WriterMintV1&& mint_value,
          std::uint64_t creator_process_id_value) noexcept
        : mint(std::move(mint_value)), creator_process_id(creator_process_id_value) {}

    ~State() noexcept {
        if (!writer) {
            return;
        }
        if (creator_process_id == 0 || current_process_id() != creator_process_id) {
            // Purge inherited stdio buffers before unregistering the child
            // copy. A normal exit must not flush the parent's pending bytes.
            mint.disarm_writer_post_fork_child_noexcept(*writer);
            writer.reset();
            return;
        }
        if (writer->state() == gnfs::relation::OOCWriterState::Open ||
            writer->state() == gnfs::relation::OOCWriterState::Suspended) {
            writer->abort();
        }
        writer.reset();
    }

    writer_detail::OOCInheritedP8WriterMintV1 mint;
    std::uint64_t creator_process_id = 0;
    std::unique_ptr<gnfs::relation::OOCRelationWriter> writer;
    std::optional<DistributedSieveWorkerCompletionFactsV1> pending_completion;
    std::optional<WorkerHandoffV1> pending_handoff;
    std::vector<std::byte> pending_payload;
    bool handoff_published = false;
};

DistributedSieveWorkerWriterAuthorityV1::DistributedSieveWorkerWriterAuthorityV1(
    std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

DistributedSieveWorkerWriterAuthorityV1::DistributedSieveWorkerWriterAuthorityV1(
    DistributedSieveWorkerWriterAuthorityV1&&) noexcept = default;

DistributedSieveWorkerWriterAuthorityV1::~DistributedSieveWorkerWriterAuthorityV1() noexcept =
    default;

bool DistributedSieveWorkerWriterAuthorityV1::valid() const noexcept {
    if (state_ == nullptr || state_->writer == nullptr || state_->creator_process_id == 0 ||
        current_process_id() != state_->creator_process_id) {
        return false;
    }
    const auto writer_state = state_->writer->state();
    return writer_state == gnfs::relation::OOCWriterState::Open ||
           writer_state == gnfs::relation::OOCWriterState::Finalized;
}

bool DistributedSieveWorkerWriterAuthorityV1::finalized() const noexcept {
    return valid() && state_->writer->state() == gnfs::relation::OOCWriterState::Finalized;
}

bool DistributedSieveWorkerWriterAuthorityV1::handoff_published() const noexcept {
    return valid() && state_->handoff_published;
}

std::size_t DistributedSieveWorkerWriterAuthorityV1::count() const noexcept {
    return valid() ? state_->writer->count() : 0;
}

const AttemptStartedV1& DistributedSieveWorkerWriterAuthorityV1::record() const noexcept {
    return state_->mint.record_;
}

const WaveManifestV1& DistributedSieveWorkerWriterAuthorityV1::manifest() const noexcept {
    return state_->mint.manifest_;
}

const DistributedSieveWorkIdentityV1&
DistributedSieveWorkerWriterAuthorityV1::identity() const noexcept {
    return state_->mint.identity_;
}

const ChunkPlanV1& DistributedSieveWorkerWriterAuthorityV1::chunk() const noexcept {
    return state_->mint.chunk_;
}

const distributed_sieve_work_package_codec_detail::DistributedSieveWorkPackageWitnessV1&
DistributedSieveWorkerWriterAuthorityV1::witness() const noexcept {
    return state_->mint.package_witness_;
}

std::size_t DistributedSieveWorkerWriterAuthorityV1::write(const gnfs::core::Relation& relation) {
    if (!valid() || finalized() || state_->pending_handoff.has_value()) {
        throw std::logic_error("distributed worker writer authority is not appendable");
    }
    return state_->writer->write(relation);
}

WorkerHandoffV1 DistributedSieveWorkerWriterAuthorityV1::finalize_and_publish_handoff(
    const DistributedSieveWorkerCompletionFactsV1& completion) {
    return finalize_and_publish_handoff_impl(completion, {}, false);
}

WorkerHandoffV1 DistributedSieveWorkerWriterAuthorityV1::finalize_and_publish_handoff_impl(
    const DistributedSieveWorkerCompletionFactsV1& completion,
    gnfs::relation::OOCPrivateHandoffTestHooks hooks, bool fail_before_retry_cache_commit) {
    if (!valid()) {
        throw std::logic_error("distributed worker writer authority is invalid");
    }
    if (state_->handoff_published) {
        throw std::logic_error("distributed worker handoff is already published");
    }
#if !defined(__APPLE__)
    throw std::system_error(std::make_error_code(std::errc::operation_not_supported),
                            "distributed worker handoff publication is unsupported");
#endif
    if (state_->pending_completion && *state_->pending_completion != completion) {
        throw std::invalid_argument("distributed worker handoff retry changed completion facts");
    }
    validate_worker_completion(state_->mint.record_, state_->mint.manifest_,
                               static_cast<std::uint64_t>(state_->writer->count()), completion);
    state_->pending_completion = completion;

    if (!state_->pending_handoff) {
        WorkerHandoffBuildContext context{
            .attempt = &state_->mint.record_,
            .manifest = &state_->mint.manifest_,
            .chunk = &state_->mint.chunk_,
            .completion = &*state_->pending_completion,
            .pending_handoff = &state_->pending_handoff,
            .pending_payload = &state_->pending_payload,
            .fail_before_retry_cache_commit = fail_before_retry_cache_commit,
        };
        state_->writer->finalize_and_publish_private_handoff_built(build_worker_handoff_payload,
                                                                   &context, hooks);
        if (!state_->pending_handoff || state_->pending_payload.empty()) {
            throw std::runtime_error(
                "distributed worker canonical handoff does not match sealed payload");
        }
    } else {
        if (!cached_worker_handoff_is_valid(state_->pending_handoff, state_->pending_payload)) {
            throw std::runtime_error("distributed worker cached handoff payload is invalid");
        }
        (void)state_->writer->finalize_and_publish_private_handoff(
            static_cast<std::uint32_t>(DistributedSieveRecordKindV1::worker_handoff),
            state_->mint.manifest_.handoff_version, state_->pending_payload, hooks);
    }

    WorkerHandoffV1 published_handoff = std::move(*state_->pending_handoff);
    state_->pending_handoff.reset();
    state_->pending_payload.clear();
    state_->pending_completion.reset();
    state_->handoff_published = true;
    return published_handoff;
}

namespace trusted_test {

WorkerHandoffV1 finalize_and_publish_distributed_sieve_worker_handoff_v1_with_hooks(
    DistributedSieveWorkerWriterAuthorityV1& writer,
    const DistributedSieveWorkerCompletionFactsV1& completion,
    DistributedSieveWorkerHandoffTestHooksV1 hooks) {
    return writer.finalize_and_publish_handoff_impl(completion, hooks.private_handoff_hooks,
                                                    hooks.fail_before_retry_cache_commit);
}

} // namespace trusted_test

namespace distributed_sieve_worker_writer_detail {

DistributedSieveWorkerWriterAdoptionResultV1
mint_distributed_sieve_worker_writer_v1(OOCInheritedP8WriterMintV1&& mint) noexcept {
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
    (void)mint;
    return {
        .writer = std::nullopt,
        .diagnostic = failure(DistributedSieveWorkerWriterPhaseV1::platform_gate,
                              DistributedSieveWorkerWriterStatusV1::platform_unsupported, ENOTSUP),
    };
#else
    const std::uint64_t creator_process_id = mint.creator_process_id_;
    if (creator_process_id == 0 || current_process_id() != creator_process_id) {
        return {
            .writer = std::nullopt,
            .diagnostic = failure(DistributedSieveWorkerWriterPhaseV1::process_gate,
                                  DistributedSieveWorkerWriterStatusV1::process_mismatch, ECHILD),
        };
    }
    try {
        auto state = std::make_unique<DistributedSieveWorkerWriterAuthorityV1::State>(
            std::move(mint), creator_process_id);
        state->writer = state->mint.create_exact_writer();
        DistributedSieveWorkerWriterAuthorityV1 authority(std::move(state));
        return {
            .writer = std::optional<DistributedSieveWorkerWriterAuthorityV1>(std::move(authority)),
            .diagnostic = failure(DistributedSieveWorkerWriterPhaseV1::writer_creation,
                                  DistributedSieveWorkerWriterStatusV1::ready),
        };
    } catch (const ooc_store::OOCExactFreshConstructionFailure& exact_failure) {
        return {
            .writer = std::nullopt,
            .diagnostic = map_writer_creation_failure(exact_failure.primary(),
                                                      writer_rollback(exact_failure.rollback()),
                                                      exact_failure.rollback_error().value()),
        };
    } catch (const std::bad_alloc&) {
        return {
            .writer = std::nullopt,
            .diagnostic = failure(DistributedSieveWorkerWriterPhaseV1::writer_creation,
                                  DistributedSieveWorkerWriterStatusV1::resource_exhausted, ENOMEM),
        };
    } catch (const private_lease::Failure& lease_failure) {
        return {
            .writer = std::nullopt,
            .diagnostic = failure(DistributedSieveWorkerWriterPhaseV1::lock_adoption,
                                  DistributedSieveWorkerWriterStatusV1::lock_invalid,
                                  lease_failure.error ? lease_failure.error.value() : EPROTO),
        };
    } catch (const std::system_error& error) {
        return {
            .writer = std::nullopt,
            .diagnostic =
                failure(DistributedSieveWorkerWriterPhaseV1::writer_creation,
                        DistributedSieveWorkerWriterStatusV1::writer_failed, error.code().value()),
        };
    } catch (const std::logic_error&) {
        return {
            .writer = std::nullopt,
            .diagnostic =
                failure(DistributedSieveWorkerWriterPhaseV1::writer_creation,
                        DistributedSieveWorkerWriterStatusV1::private_lease_invalid, EPROTO),
        };
    } catch (...) {
        return {
            .writer = std::nullopt,
            .diagnostic = failure(DistributedSieveWorkerWriterPhaseV1::writer_creation,
                                  DistributedSieveWorkerWriterStatusV1::unexpected_failure),
        };
    }
#endif
}

} // namespace distributed_sieve_worker_writer_detail

} // namespace gnfs::sieve::distributed_sieve_worker_entry_detail

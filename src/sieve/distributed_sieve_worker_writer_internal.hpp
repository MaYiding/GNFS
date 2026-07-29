#pragma once

// Source-private M3a-2 bridge from one authenticated worker-entry token to one
// exact P8 relation writer and one typed terminal handoff. Paths, native
// handles, store identity, generic handoff publication, cleanup receipts, and
// deletion authority remain unreachable.

#include "distributed_sieve_work_package_codec_internal.hpp"

#include <gnfs/relation/ooc_cleanup_transaction.hpp>
#include <gnfs/sieve/distributed_sieve_protocol.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace gnfs::core {
struct Relation;
}

namespace gnfs::relation {
class OOCRelationWriter;
}

namespace gnfs::sieve::distributed_sieve_worker_entry_detail {

class DistributedSieveWorkerEntryV1;
class DistributedSieveWorkerWriterAuthorityV1;
struct DistributedSieveWorkerWriterAdoptionResultV1;

struct DistributedSieveWorkerCompletionFactsV1 final {
    std::uint64_t processed_sq_count = 0;
    std::uint32_t next_sq_index = 0;
    WorkerCompletionReasonV1 completion_reason = WorkerCompletionReasonV1::range_exhausted;

    [[nodiscard]] friend constexpr bool
    operator==(const DistributedSieveWorkerCompletionFactsV1&,
               const DistributedSieveWorkerCompletionFactsV1&) noexcept = default;
};

enum class DistributedSieveWorkerWriterPhaseV1 : std::uint8_t {
    none,
    single_use_gate,
    platform_gate,
    process_gate,
    entry_revalidation,
    final_revalidation,
    capability_transfer,
    lock_adoption,
    writer_creation,
};

enum class DistributedSieveWorkerWriterStatusV1 : std::uint8_t {
    ready,
    already_consumed,
    platform_unsupported,
    process_mismatch,
    entry_invalid,
    lock_invalid,
    private_lease_invalid,
    resource_exhausted,
    writer_failed,
    unexpected_failure,
};

enum class DistributedSieveWorkerWriterRollbackV1 : std::uint8_t {
    not_applicable,
    clean,
    named_residue_may_remain,
    directory_durability_uncertain,
};

[[nodiscard]] constexpr std::string_view
distributed_sieve_worker_writer_status_name(DistributedSieveWorkerWriterStatusV1 status) noexcept {
    switch (status) {
    case DistributedSieveWorkerWriterStatusV1::ready:
        return "ready";
    case DistributedSieveWorkerWriterStatusV1::already_consumed:
        return "already_consumed";
    case DistributedSieveWorkerWriterStatusV1::platform_unsupported:
        return "platform_unsupported";
    case DistributedSieveWorkerWriterStatusV1::process_mismatch:
        return "process_mismatch";
    case DistributedSieveWorkerWriterStatusV1::entry_invalid:
        return "entry_invalid";
    case DistributedSieveWorkerWriterStatusV1::lock_invalid:
        return "lock_invalid";
    case DistributedSieveWorkerWriterStatusV1::private_lease_invalid:
        return "private_lease_invalid";
    case DistributedSieveWorkerWriterStatusV1::resource_exhausted:
        return "resource_exhausted";
    case DistributedSieveWorkerWriterStatusV1::writer_failed:
        return "writer_failed";
    case DistributedSieveWorkerWriterStatusV1::unexpected_failure:
        return "unexpected_failure";
    }
    return "unknown";
}

struct DistributedSieveWorkerWriterDiagnosticV1 final {
    DistributedSieveWorkerWriterPhaseV1 phase = DistributedSieveWorkerWriterPhaseV1::none;
    DistributedSieveWorkerWriterStatusV1 status = DistributedSieveWorkerWriterStatusV1::ready;
    int native_error = 0;
    DistributedSieveWorkerWriterRollbackV1 rollback =
        DistributedSieveWorkerWriterRollbackV1::not_applicable;
    int rollback_native_error = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == DistributedSieveWorkerWriterStatusV1::ready;
    }

    [[nodiscard]] bool reconciliation_required() const noexcept {
        return rollback == DistributedSieveWorkerWriterRollbackV1::named_residue_may_remain ||
               rollback == DistributedSieveWorkerWriterRollbackV1::directory_durability_uncertain;
    }
};

namespace trusted_test {

struct DistributedSieveWorkerWriterTestHooksV1 final {
    using AfterFirstValidation = void (*)(void* context) noexcept;

    AfterFirstValidation after_first_validation = nullptr;
    void* context = nullptr;
    gnfs::relation::OOCPrivateLeaseTestHooks private_lease_hooks;
};

struct DistributedSieveWorkerHandoffTestHooksV1 final {
    gnfs::relation::OOCPrivateHandoffTestHooks private_handoff_hooks;
    bool fail_before_retry_cache_commit = false;
};

[[nodiscard]] DistributedSieveWorkerWriterAdoptionResultV1
consume_distributed_sieve_worker_writer_v1_with_hooks(
    DistributedSieveWorkerEntryV1&& entry, DistributedSieveWorkerWriterTestHooksV1 hooks) noexcept;

[[nodiscard]] WorkerHandoffV1 finalize_and_publish_distributed_sieve_worker_handoff_v1_with_hooks(
    DistributedSieveWorkerWriterAuthorityV1& writer,
    const DistributedSieveWorkerCompletionFactsV1& completion,
    DistributedSieveWorkerHandoffTestHooksV1 hooks);

} // namespace trusted_test

[[nodiscard]] DistributedSieveWorkerWriterAdoptionResultV1
consume_distributed_sieve_worker_writer_v1(DistributedSieveWorkerEntryV1&& entry) noexcept;

namespace distributed_sieve_worker_writer_detail {

/// Type-erased ownership of the original authenticated entry state. The
/// writer retains it so every mutation can revalidate the named locks, P8
/// generation, marker chain, ACL policy, and staging absence without exposing
/// those capabilities to its caller.
class DistributedSieveWorkerWriterLifetimeGuardV1 final {
public:
    DistributedSieveWorkerWriterLifetimeGuardV1() noexcept = default;
    DistributedSieveWorkerWriterLifetimeGuardV1(
        const DistributedSieveWorkerWriterLifetimeGuardV1&) = delete;
    DistributedSieveWorkerWriterLifetimeGuardV1&
    operator=(const DistributedSieveWorkerWriterLifetimeGuardV1&) = delete;
    DistributedSieveWorkerWriterLifetimeGuardV1(
        DistributedSieveWorkerWriterLifetimeGuardV1&& other) noexcept;
    DistributedSieveWorkerWriterLifetimeGuardV1&
    operator=(DistributedSieveWorkerWriterLifetimeGuardV1&& other) noexcept;
    ~DistributedSieveWorkerWriterLifetimeGuardV1() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool stable() const noexcept;

private:
    using Revalidate = bool (*)(const void* state) noexcept;
    using Destroy = void (*)(void* state) noexcept;

    DistributedSieveWorkerWriterLifetimeGuardV1(void* state, Revalidate revalidate,
                                                Destroy destroy) noexcept;
    void reset_noexcept() noexcept;

    void* state_ = nullptr;
    Revalidate revalidate_ = nullptr;
    Destroy destroy_ = nullptr;

    friend auto ::gnfs::sieve::distributed_sieve_worker_entry_detail::
        consume_distributed_sieve_worker_writer_v1(
            ::gnfs::sieve::distributed_sieve_worker_entry_detail::DistributedSieveWorkerEntryV1&&
                entry) noexcept -> ::gnfs::sieve::distributed_sieve_worker_entry_detail::
            DistributedSieveWorkerWriterAdoptionResultV1;
    friend auto ::gnfs::sieve::distributed_sieve_worker_entry_detail::trusted_test::
        consume_distributed_sieve_worker_writer_v1_with_hooks(
            ::gnfs::sieve::distributed_sieve_worker_entry_detail::DistributedSieveWorkerEntryV1&&
                entry,
            ::gnfs::sieve::distributed_sieve_worker_entry_detail::trusted_test::
                DistributedSieveWorkerWriterTestHooksV1 hooks) noexcept -> ::gnfs::sieve::
            distributed_sieve_worker_entry_detail::DistributedSieveWorkerWriterAdoptionResultV1;
};

/// Unforgeable, one-shot transfer object. It owns every retained worker-entry
/// descriptor until the relation layer either adopts the exact P8 authority or
/// closes it. There are intentionally no accessors.
class OOCInheritedP8WriterMintV1 final {
public:
    OOCInheritedP8WriterMintV1(const OOCInheritedP8WriterMintV1&) = delete;
    OOCInheritedP8WriterMintV1& operator=(const OOCInheritedP8WriterMintV1&) = delete;
    OOCInheritedP8WriterMintV1(OOCInheritedP8WriterMintV1&& other) noexcept;
    OOCInheritedP8WriterMintV1& operator=(OOCInheritedP8WriterMintV1&&) = delete;
    ~OOCInheritedP8WriterMintV1() noexcept;

    void disarm_writer_post_fork_child_noexcept(gnfs::relation::OOCRelationWriter& writer) noexcept;

private:
    OOCInheritedP8WriterMintV1(
        int root_descriptor, int permanent_lock_descriptor, int attempt_lock_descriptor,
        int attempt_directory_descriptor, int package_descriptor, std::uint64_t creator_process_id,
        std::filesystem::path absolute_root_path, std::filesystem::path base_path,
        std::filesystem::path private_directory, std::filesystem::path lock_path,
        std::string private_directory_leaf, std::string lock_leaf,
        std::array<std::uint64_t, 3> root_identity,
        std::array<std::uint64_t, 3> attempt_lock_identity,
        std::array<std::uint64_t, 3> attempt_directory_identity,
        std::array<std::uint64_t, 2> lease_id, std::array<std::uint64_t, 3> owner_marker_identity,
        std::array<std::uint64_t, 3> owned_marker_identity, AttemptStartedV1 record,
        WaveManifestV1 manifest, DistributedSieveWorkIdentityV1 identity, ChunkPlanV1 chunk,
        distributed_sieve_work_package_codec_detail::DistributedSieveWorkPackageWitnessV1
            package_witness,
        gnfs::relation::OOCPrivateLeaseTestHooks private_lease_hooks);

    [[nodiscard]] std::unique_ptr<gnfs::relation::OOCRelationWriter> create_exact_writer();
    void attach_lifetime_guard(DistributedSieveWorkerWriterLifetimeGuardV1&& guard) noexcept;
    void close_descriptors_noexcept() noexcept;

    int root_descriptor_ = -1;
    int permanent_lock_descriptor_ = -1;
    int attempt_lock_descriptor_ = -1;
    int attempt_directory_descriptor_ = -1;
    int package_descriptor_ = -1;
    std::uint64_t creator_process_id_ = 0;
    bool consumed_ = false;
    DistributedSieveWorkerWriterLifetimeGuardV1 lifetime_guard_;

    std::filesystem::path absolute_root_path_;
    std::filesystem::path base_path_;
    std::filesystem::path private_directory_;
    std::filesystem::path lock_path_;
    std::string private_directory_leaf_;
    std::string lock_leaf_;
    std::array<std::uint64_t, 3> root_identity_{};
    std::array<std::uint64_t, 3> attempt_lock_identity_{};
    std::array<std::uint64_t, 3> attempt_directory_identity_{};
    std::array<std::uint64_t, 2> lease_id_{};
    std::array<std::uint64_t, 3> owner_marker_identity_{};
    std::array<std::uint64_t, 3> owned_marker_identity_{};

    AttemptStartedV1 record_;
    WaveManifestV1 manifest_;
    DistributedSieveWorkIdentityV1 identity_;
    ChunkPlanV1 chunk_;
    distributed_sieve_work_package_codec_detail::DistributedSieveWorkPackageWitnessV1
        package_witness_;
    gnfs::relation::OOCPrivateLeaseTestHooks private_lease_hooks_;

    friend auto ::gnfs::sieve::distributed_sieve_worker_entry_detail::
        consume_distributed_sieve_worker_writer_v1(
            ::gnfs::sieve::distributed_sieve_worker_entry_detail::DistributedSieveWorkerEntryV1&&
                entry) noexcept -> ::gnfs::sieve::distributed_sieve_worker_entry_detail::
            DistributedSieveWorkerWriterAdoptionResultV1;
    friend auto ::gnfs::sieve::distributed_sieve_worker_entry_detail::trusted_test::
        consume_distributed_sieve_worker_writer_v1_with_hooks(
            ::gnfs::sieve::distributed_sieve_worker_entry_detail::DistributedSieveWorkerEntryV1&&
                entry,
            ::gnfs::sieve::distributed_sieve_worker_entry_detail::trusted_test::
                DistributedSieveWorkerWriterTestHooksV1 hooks) noexcept -> ::gnfs::sieve::
            distributed_sieve_worker_entry_detail::DistributedSieveWorkerWriterAdoptionResultV1;
    friend DistributedSieveWorkerWriterAdoptionResultV1
    mint_distributed_sieve_worker_writer_v1(OOCInheritedP8WriterMintV1&& mint) noexcept;
    friend class ::gnfs::sieve::distributed_sieve_worker_entry_detail::
        DistributedSieveWorkerWriterAuthorityV1;
};

[[nodiscard]] DistributedSieveWorkerWriterAdoptionResultV1
mint_distributed_sieve_worker_writer_v1(OOCInheritedP8WriterMintV1&& mint) noexcept;

} // namespace distributed_sieve_worker_writer_detail

/// Narrow, process-bound append/finalize authority for one exact P8 corpus.
///
/// Destruction of an unfinished writer is abort/close-only. A post-fork copy
/// rejects every mutator, purges inherited stdio buffers, and closes its copied
/// descriptors without flushing or finalizing the parent's corpus.
class DistributedSieveWorkerWriterAuthorityV1 final {
public:
    DistributedSieveWorkerWriterAuthorityV1() = delete;
    DistributedSieveWorkerWriterAuthorityV1(const DistributedSieveWorkerWriterAuthorityV1&) =
        delete;
    DistributedSieveWorkerWriterAuthorityV1&
    operator=(const DistributedSieveWorkerWriterAuthorityV1&) = delete;
    DistributedSieveWorkerWriterAuthorityV1(DistributedSieveWorkerWriterAuthorityV1&&) noexcept;
    DistributedSieveWorkerWriterAuthorityV1&
    operator=(DistributedSieveWorkerWriterAuthorityV1&&) = delete;
    ~DistributedSieveWorkerWriterAuthorityV1() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool finalized() const noexcept;
    [[nodiscard]] bool handoff_published() const noexcept;
    [[nodiscard]] std::size_t count() const noexcept;

    [[nodiscard]] const AttemptStartedV1& record() const noexcept;
    [[nodiscard]] const WaveManifestV1& manifest() const noexcept;
    [[nodiscard]] const DistributedSieveWorkIdentityV1& identity() const noexcept;
    [[nodiscard]] const ChunkPlanV1& chunk() const noexcept;
    [[nodiscard]] const distributed_sieve_work_package_codec_detail::
        DistributedSieveWorkPackageWitnessV1&
        witness() const noexcept;

    std::size_t write(const gnfs::core::Relation& relation);
    [[nodiscard]] WorkerHandoffV1
    finalize_and_publish_handoff(const DistributedSieveWorkerCompletionFactsV1& completion);

private:
    struct State;
    explicit DistributedSieveWorkerWriterAuthorityV1(std::unique_ptr<State> state) noexcept;
    [[nodiscard]] WorkerHandoffV1
    finalize_and_publish_handoff_impl(const DistributedSieveWorkerCompletionFactsV1& completion,
                                      gnfs::relation::OOCPrivateHandoffTestHooks hooks,
                                      bool fail_before_retry_cache_commit);

    std::unique_ptr<State> state_;

    friend DistributedSieveWorkerWriterAdoptionResultV1
    distributed_sieve_worker_writer_detail::mint_distributed_sieve_worker_writer_v1(
        distributed_sieve_worker_writer_detail::OOCInheritedP8WriterMintV1&& mint) noexcept;
    friend WorkerHandoffV1
    trusted_test::finalize_and_publish_distributed_sieve_worker_handoff_v1_with_hooks(
        DistributedSieveWorkerWriterAuthorityV1& writer,
        const DistributedSieveWorkerCompletionFactsV1& completion,
        trusted_test::DistributedSieveWorkerHandoffTestHooksV1 hooks);
};

struct DistributedSieveWorkerWriterAdoptionResultV1 final {
    std::optional<DistributedSieveWorkerWriterAuthorityV1> writer;
    DistributedSieveWorkerWriterDiagnosticV1 diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return writer.has_value() && static_cast<bool>(diagnostic) && writer->valid();
    }
};

} // namespace gnfs::sieve::distributed_sieve_worker_entry_detail

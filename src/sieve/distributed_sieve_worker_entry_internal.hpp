#pragma once

// Source-private exec-image reconstruction boundary for one sieve worker.
//
// The entry point consumes stdin and the fixed descriptor set 3..6 exactly
// once. Success returns read-only, process-bound facts plus retained handles;
// it grants no path, writer, cleanup, publication, or retry authority.

#include "distributed_sieve_work_package_codec_internal.hpp"
#include "distributed_sieve_worker_writer_internal.hpp"

#include <gnfs/sieve/distributed_sieve_protocol.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace gnfs::sieve::distributed_sieve_worker_entry_detail {

enum class DistributedSieveWorkerEntryPhaseV1 : std::uint8_t {
    none,
    single_use_gate,
    platform_gate,
    fixed_capability_capture,
    bootstrap_read,
    bootstrap_decode,
    root_validation,
    manifest_validation,
    permanent_lock_validation,
    attempt_validation,
    attempt_base_lock_validation,
    private_lease_validation,
    work_package_validation,
    final_revalidation,
};

enum class DistributedSieveWorkerEntryStatusV1 : std::uint8_t {
    ready,
    already_adopted,
    platform_unsupported,
    process_mismatch,
    descriptor_unavailable,
    descriptor_policy_invalid,
    input_failed,
    protocol_invalid,
    namespace_invalid,
    lock_invalid,
    private_lease_invalid,
    work_package_invalid,
    resource_exhausted,
    unexpected_failure,
};

[[nodiscard]] constexpr std::string_view
distributed_sieve_worker_entry_status_name(DistributedSieveWorkerEntryStatusV1 status) noexcept {
    switch (status) {
    case DistributedSieveWorkerEntryStatusV1::ready:
        return "ready";
    case DistributedSieveWorkerEntryStatusV1::already_adopted:
        return "already_adopted";
    case DistributedSieveWorkerEntryStatusV1::platform_unsupported:
        return "platform_unsupported";
    case DistributedSieveWorkerEntryStatusV1::process_mismatch:
        return "process_mismatch";
    case DistributedSieveWorkerEntryStatusV1::descriptor_unavailable:
        return "descriptor_unavailable";
    case DistributedSieveWorkerEntryStatusV1::descriptor_policy_invalid:
        return "descriptor_policy_invalid";
    case DistributedSieveWorkerEntryStatusV1::input_failed:
        return "input_failed";
    case DistributedSieveWorkerEntryStatusV1::protocol_invalid:
        return "protocol_invalid";
    case DistributedSieveWorkerEntryStatusV1::namespace_invalid:
        return "namespace_invalid";
    case DistributedSieveWorkerEntryStatusV1::lock_invalid:
        return "lock_invalid";
    case DistributedSieveWorkerEntryStatusV1::private_lease_invalid:
        return "private_lease_invalid";
    case DistributedSieveWorkerEntryStatusV1::work_package_invalid:
        return "work_package_invalid";
    case DistributedSieveWorkerEntryStatusV1::resource_exhausted:
        return "resource_exhausted";
    case DistributedSieveWorkerEntryStatusV1::unexpected_failure:
        return "unexpected_failure";
    }
    return "unknown";
}

struct DistributedSieveWorkerEntryDiagnosticV1 final {
    DistributedSieveWorkerEntryPhaseV1 phase = DistributedSieveWorkerEntryPhaseV1::none;
    DistributedSieveWorkerEntryStatusV1 status = DistributedSieveWorkerEntryStatusV1::ready;
    int native_error = 0;
    DistributedSieveProtocolStatus protocol_status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == DistributedSieveWorkerEntryStatusV1::ready;
    }
};

struct DistributedSieveWorkerEntryAdoptionResultV1;

namespace trusted_test {

struct DistributedSieveWorkerEntryTestHooksV1 final {
    using AfterFirstValidation = void (*)(void* context) noexcept;

    AfterFirstValidation after_first_validation = nullptr;
    void* context = nullptr;
};

/// Trusted replacement-sandwich seam. It runs only after a complete
/// validation and grants no descriptor, path, or mutation authority.
[[nodiscard]] DistributedSieveWorkerEntryAdoptionResultV1
adopt_distributed_sieve_worker_entry_v1_with_hooks(
    DistributedSieveWorkerEntryTestHooksV1 hooks) noexcept;

} // namespace trusted_test

/// Read-only process-bound reconstruction of one exact launched attempt.
///
/// The retained handles are intentionally opaque. Destruction is close-only,
/// and an inherited post-fork copy is invalid.
class DistributedSieveWorkerEntryV1 final {
public:
    DistributedSieveWorkerEntryV1() = delete;
    DistributedSieveWorkerEntryV1(const DistributedSieveWorkerEntryV1&) = delete;
    DistributedSieveWorkerEntryV1& operator=(const DistributedSieveWorkerEntryV1&) = delete;
    DistributedSieveWorkerEntryV1(DistributedSieveWorkerEntryV1&&) noexcept;
    DistributedSieveWorkerEntryV1& operator=(DistributedSieveWorkerEntryV1&&) = delete;
    ~DistributedSieveWorkerEntryV1() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] DistributedSieveWorkerEntryDiagnosticV1 revalidate() const noexcept;

    [[nodiscard]] const AttemptStartedV1& record() const noexcept;
    [[nodiscard]] const WaveManifestV1& manifest() const noexcept;
    [[nodiscard]] const DistributedSieveWorkIdentityV1& identity() const noexcept;
    [[nodiscard]] const ChunkPlanV1& chunk() const noexcept;
    [[nodiscard]] const distributed_sieve_work_package_codec_detail::
        DistributedSieveWorkPackageWitnessV1&
        witness() const noexcept;

private:
    struct State;
    explicit DistributedSieveWorkerEntryV1(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;

    friend struct DistributedSieveWorkerEntryAdoptionResultV1;
    friend DistributedSieveWorkerEntryAdoptionResultV1
    adopt_distributed_sieve_worker_entry_v1() noexcept;
    friend DistributedSieveWorkerEntryAdoptionResultV1
    trusted_test::adopt_distributed_sieve_worker_entry_v1_with_hooks(
        trusted_test::DistributedSieveWorkerEntryTestHooksV1 hooks) noexcept;
    friend DistributedSieveWorkerWriterAdoptionResultV1
    consume_distributed_sieve_worker_writer_v1(DistributedSieveWorkerEntryV1&& entry) noexcept;
    friend DistributedSieveWorkerWriterAdoptionResultV1
    trusted_test::consume_distributed_sieve_worker_writer_v1_with_hooks(
        DistributedSieveWorkerEntryV1&& entry,
        trusted_test::DistributedSieveWorkerWriterTestHooksV1 hooks) noexcept;
};

struct DistributedSieveWorkerEntryAdoptionResultV1 final {
    std::optional<DistributedSieveWorkerEntryV1> entry;
    DistributedSieveWorkerEntryDiagnosticV1 diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return entry.has_value() && static_cast<bool>(diagnostic) && entry->valid();
    }
};

/// Consume stdin and fixed descriptors 3..6 exactly once in this exec image.
[[nodiscard]] DistributedSieveWorkerEntryAdoptionResultV1
adopt_distributed_sieve_worker_entry_v1() noexcept;

} // namespace gnfs::sieve::distributed_sieve_worker_entry_detail

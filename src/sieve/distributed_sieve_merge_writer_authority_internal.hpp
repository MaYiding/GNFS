#pragma once

// Source-private, one-shot authority for materializing one MergePreparedV1.
// The caller can neither append arbitrary relations nor reach paths, native
// handles, lease receipts, cleanup authority, or the underlying OOC writer.

#include "distributed_sieve_merge_coordinator.hpp"
#include "distributed_sieve_merge_prepared_admission_internal.hpp"
#include "distributed_sieve_merge_writer_codec_internal.hpp"
#include "distributed_sieve_merge_writer_internal.hpp"

#include <gnfs/relation/ooc_cleanup_transaction.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>

namespace gnfs::sieve::distributed_sieve_merge_writer_authority_detail {

using distributed_sieve_merge_coordinator_detail::DistributedSieveMergeGenerationAdmissionV1;

class DistributedSieveMergeWriterAuthorityV1;
struct DistributedSieveMergeWriterAuthorityStateV1;
struct DistributedSieveMergeWriterAdoptionResultV1;
struct DistributedSieveMergePreparedResultV1;

enum class DistributedSieveMergeWriterAuthorityPhaseV1 : std::uint8_t {
    admission_validation,
    platform_gate,
    wave_mint,
    input_binding,
    writer_creation,
    streaming,
    finalization,
    payload_build,
    handoff_publication,
    complete,
};

enum class DistributedSieveMergeWriterAuthorityStatusV1 : std::uint8_t {
    ready,
    invalid_admission,
    platform_unsupported,
    wave_mint_failed,
    merge_chain_invalid,
    input_projection_invalid,
    input_reader_invalid,
    writer_creation_failed,
    stream_failed,
    payload_build_failed,
    handoff_publication_failed,
    process_mismatch,
    resource_exhausted,
    unexpected_failure,
};

[[nodiscard]] constexpr std::string_view distributed_sieve_merge_writer_authority_status_name(
    DistributedSieveMergeWriterAuthorityStatusV1 status) noexcept {
    switch (status) {
    case DistributedSieveMergeWriterAuthorityStatusV1::ready:
        return "ready";
    case DistributedSieveMergeWriterAuthorityStatusV1::invalid_admission:
        return "invalid_admission";
    case DistributedSieveMergeWriterAuthorityStatusV1::platform_unsupported:
        return "platform_unsupported";
    case DistributedSieveMergeWriterAuthorityStatusV1::wave_mint_failed:
        return "wave_mint_failed";
    case DistributedSieveMergeWriterAuthorityStatusV1::merge_chain_invalid:
        return "merge_chain_invalid";
    case DistributedSieveMergeWriterAuthorityStatusV1::input_projection_invalid:
        return "input_projection_invalid";
    case DistributedSieveMergeWriterAuthorityStatusV1::input_reader_invalid:
        return "input_reader_invalid";
    case DistributedSieveMergeWriterAuthorityStatusV1::writer_creation_failed:
        return "writer_creation_failed";
    case DistributedSieveMergeWriterAuthorityStatusV1::stream_failed:
        return "stream_failed";
    case DistributedSieveMergeWriterAuthorityStatusV1::payload_build_failed:
        return "payload_build_failed";
    case DistributedSieveMergeWriterAuthorityStatusV1::handoff_publication_failed:
        return "handoff_publication_failed";
    case DistributedSieveMergeWriterAuthorityStatusV1::process_mismatch:
        return "process_mismatch";
    case DistributedSieveMergeWriterAuthorityStatusV1::resource_exhausted:
        return "resource_exhausted";
    case DistributedSieveMergeWriterAuthorityStatusV1::unexpected_failure:
        return "unexpected_failure";
    }
    return "unknown";
}

struct DistributedSieveMergeWriterAuthorityDiagnosticV1 final {
    DistributedSieveMergeWriterAuthorityPhaseV1 phase =
        DistributedSieveMergeWriterAuthorityPhaseV1::admission_validation;
    DistributedSieveMergeWriterAuthorityStatusV1 status =
        DistributedSieveMergeWriterAuthorityStatusV1::unexpected_failure;
    std::size_t manifest_slot = static_cast<std::size_t>(-1);
    distributed_sieve_resume_detail::DistributedSieveWaveStoreDiagnostic wave_store;
    distributed_sieve_merge_writer_detail::DistributedSieveMergeWriterDiagnosticV1 stream;
    distributed_sieve_merge_writer_codec_detail::
        DistributedSieveMergePreparedPayloadBuildDiagnosticV1 codec;
    std::error_code native_error;
    bool reconciliation_required = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return phase == DistributedSieveMergeWriterAuthorityPhaseV1::complete &&
               status == DistributedSieveMergeWriterAuthorityStatusV1::ready && !native_error &&
               !reconciliation_required;
    }
};

[[nodiscard]] DistributedSieveMergeWriterAdoptionResultV1
consume_distributed_sieve_merge_generation_v1(
    DistributedSieveMergeGenerationAdmissionV1&& admission) noexcept;

[[nodiscard]] DistributedSieveMergePreparedResultV1 publish_distributed_sieve_merge_prepared_v1(
    DistributedSieveMergeWriterAuthorityV1&& authority) noexcept;

namespace trusted_test {

struct DistributedSieveMergeWriterAdoptionTestHooksV1 final {
    distributed_sieve_merge_writer_detail::trusted_test::DistributedSieveMergeWriterTestHooksV1
        stream_hooks;
};

struct DistributedSieveMergePreparedPublicationTestHooksV1 final {
    using StopAfterPayloadBuildBeforeHandoff = bool (*)(void* context) noexcept;

    gnfs::relation::OOCPrivateHandoffTestHooks private_handoff_hooks;
    /// Runs only after final corpus evidence and the typed payload are complete,
    /// before the first private-handoff namespace mutation.
    StopAfterPayloadBuildBeforeHandoff stop_after_payload_build_before_handoff = nullptr;
    void* payload_build_context = nullptr;
};

[[nodiscard]] DistributedSieveMergeWriterAdoptionResultV1
consume_distributed_sieve_merge_generation_v1_with_hooks(
    DistributedSieveMergeGenerationAdmissionV1&& admission,
    DistributedSieveMergeWriterAdoptionTestHooksV1 hooks) noexcept;

[[nodiscard]] DistributedSieveMergePreparedResultV1
publish_distributed_sieve_merge_prepared_v1_with_hooks(
    DistributedSieveMergeWriterAuthorityV1&& authority,
    DistributedSieveMergePreparedPublicationTestHooksV1 hooks) noexcept;

} // namespace trusted_test

/// Single-use authority whose writer has already consumed every authenticated
/// manifest-order input. Its only mutation is typed prepared publication.
class DistributedSieveMergeWriterAuthorityV1 final {
public:
    DistributedSieveMergeWriterAuthorityV1() = delete;
    DistributedSieveMergeWriterAuthorityV1(const DistributedSieveMergeWriterAuthorityV1&) = delete;
    DistributedSieveMergeWriterAuthorityV1&
    operator=(const DistributedSieveMergeWriterAuthorityV1&) = delete;
    DistributedSieveMergeWriterAuthorityV1(DistributedSieveMergeWriterAuthorityV1&& other) noexcept;
    DistributedSieveMergeWriterAuthorityV1&
    operator=(DistributedSieveMergeWriterAuthorityV1&&) = delete;
    ~DistributedSieveMergeWriterAuthorityV1() noexcept;

    [[nodiscard]] bool valid() const noexcept;

private:
    explicit DistributedSieveMergeWriterAuthorityV1(
        std::unique_ptr<DistributedSieveMergeWriterAuthorityStateV1> state) noexcept;
    [[nodiscard]] DistributedSieveMergeWriterAuthorityDiagnosticV1
    bind_inputs_create_writer_and_stream(
        distributed_sieve_merge_writer_detail::trusted_test::DistributedSieveMergeWriterTestHooksV1
            hooks = {}) noexcept;
    [[nodiscard]] DistributedSieveMergePreparedResultV1
    publish_impl(trusted_test::DistributedSieveMergePreparedPublicationTestHooksV1 hooks) noexcept;
    void release_state_noexcept() noexcept;
    [[nodiscard]] static bool
    state_lifetime_stable(const DistributedSieveMergeWriterAuthorityStateV1& state) noexcept;
    [[nodiscard]] static bool
    state_process_owned(const DistributedSieveMergeWriterAuthorityStateV1& state) noexcept;
    static void close_state_noexcept(
        std::unique_ptr<DistributedSieveMergeWriterAuthorityStateV1>& state) noexcept;

    std::unique_ptr<DistributedSieveMergeWriterAuthorityStateV1> state_;

    friend DistributedSieveMergeWriterAdoptionResultV1
    consume_distributed_sieve_merge_generation_v1(
        DistributedSieveMergeGenerationAdmissionV1&& admission) noexcept;
    friend DistributedSieveMergePreparedResultV1 publish_distributed_sieve_merge_prepared_v1(
        DistributedSieveMergeWriterAuthorityV1&& authority) noexcept;
    friend DistributedSieveMergePreparedResultV1
    trusted_test::publish_distributed_sieve_merge_prepared_v1_with_hooks(
        DistributedSieveMergeWriterAuthorityV1&& authority,
        trusted_test::DistributedSieveMergePreparedPublicationTestHooksV1 hooks) noexcept;
    friend DistributedSieveMergeWriterAdoptionResultV1
    trusted_test::consume_distributed_sieve_merge_generation_v1_with_hooks(
        DistributedSieveMergeGenerationAdmissionV1&& admission,
        trusted_test::DistributedSieveMergeWriterAdoptionTestHooksV1 hooks) noexcept;
};

struct DistributedSieveMergeWriterAdoptionResultV1 final {
    std::optional<DistributedSieveMergeWriterAuthorityV1> authority;
    DistributedSieveMergeWriterAuthorityDiagnosticV1 diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return authority.has_value() && static_cast<bool>(diagnostic) && authority->valid();
    }
};

struct DistributedSieveMergePreparedResultV1 final {
    std::optional<DistributedSieveMergePreparedAdmissionV1> admission;
    DistributedSieveMergeWriterAuthorityDiagnosticV1 diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return admission.has_value() && static_cast<bool>(diagnostic) && admission->valid();
    }
};

} // namespace gnfs::sieve::distributed_sieve_merge_writer_authority_detail

#pragma once

// Source-private composition of authenticated worker entry, immutable runtime
// reconstruction, path-free chunk execution, exact writer authority, and one
// typed terminal handoff.

#include "distributed_sieve_worker_chunk_internal.hpp"
#include "distributed_sieve_worker_entry_internal.hpp"
#include "distributed_sieve_worker_writer_internal.hpp"

#include <gnfs/sieve/distributed_sieve_protocol.hpp>
#include <gnfs/util/sha256.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace gnfs::sieve::distributed_sieve_worker_execution_detail {

struct DistributedSieveCurrentExecutableDigestResultV1 final {
    std::optional<util::Sha256Digest> digest;
    int native_error = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return digest.has_value() && native_error == 0;
    }
};

/// Best-effort digest of the image path loaded by the current process.
///
/// This detects wrong binaries and ordinary deployment drift. On macOS the
/// kernel does not expose an exact-object exec primitive through this adapter,
/// so path replacement between exec and this reopen remains an explicitly
/// documented non-adversarial TOCTOU boundary.
[[nodiscard]] DistributedSieveCurrentExecutableDigestResultV1
current_distributed_sieve_worker_executable_sha256_v1() noexcept;

enum class DistributedSieveWorkerExecutionPhaseV1 : std::uint8_t {
    none,
    platform_gate,
    entry_revalidation,
    executable_identity,
    runtime_rehydration,
    chunk_preparation,
    writer_adoption,
    chunk_execution,
    handoff_publication,
};

enum class DistributedSieveWorkerExecutionStatusV1 : std::uint8_t {
    succeeded,
    platform_unsupported,
    entry_invalid,
    executable_unavailable,
    executable_mismatch,
    runtime_invalid,
    chunk_invalid,
    writer_unavailable,
    execution_failed,
    handoff_failed,
    resource_exhausted,
    unexpected_failure,
};

[[nodiscard]] constexpr std::string_view distributed_sieve_worker_execution_status_name(
    DistributedSieveWorkerExecutionStatusV1 status) noexcept {
    switch (status) {
    case DistributedSieveWorkerExecutionStatusV1::succeeded:
        return "succeeded";
    case DistributedSieveWorkerExecutionStatusV1::platform_unsupported:
        return "platform_unsupported";
    case DistributedSieveWorkerExecutionStatusV1::entry_invalid:
        return "entry_invalid";
    case DistributedSieveWorkerExecutionStatusV1::executable_unavailable:
        return "executable_unavailable";
    case DistributedSieveWorkerExecutionStatusV1::executable_mismatch:
        return "executable_mismatch";
    case DistributedSieveWorkerExecutionStatusV1::runtime_invalid:
        return "runtime_invalid";
    case DistributedSieveWorkerExecutionStatusV1::chunk_invalid:
        return "chunk_invalid";
    case DistributedSieveWorkerExecutionStatusV1::writer_unavailable:
        return "writer_unavailable";
    case DistributedSieveWorkerExecutionStatusV1::execution_failed:
        return "execution_failed";
    case DistributedSieveWorkerExecutionStatusV1::handoff_failed:
        return "handoff_failed";
    case DistributedSieveWorkerExecutionStatusV1::resource_exhausted:
        return "resource_exhausted";
    case DistributedSieveWorkerExecutionStatusV1::unexpected_failure:
        return "unexpected_failure";
    }
    return "unknown";
}

struct DistributedSieveWorkerExecutionDiagnosticV1 final {
    DistributedSieveWorkerExecutionPhaseV1 phase = DistributedSieveWorkerExecutionPhaseV1::none;
    DistributedSieveWorkerExecutionStatusV1 status =
        DistributedSieveWorkerExecutionStatusV1::unexpected_failure;
    int native_error = 0;
    DistributedSieveProtocolStatus protocol_status;
    distributed_sieve_worker_entry_detail::DistributedSieveWorkerEntryStatusV1 entry_status =
        distributed_sieve_worker_entry_detail::DistributedSieveWorkerEntryStatusV1::ready;
    distributed_sieve_worker_entry_detail::DistributedSieveWorkerWriterStatusV1 writer_status =
        distributed_sieve_worker_entry_detail::DistributedSieveWorkerWriterStatusV1::ready;
    DistributedSieveWorkerChunkStatusV1 chunk_status = DistributedSieveWorkerChunkStatusV1::ready;
    bool artifacts_may_remain = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == DistributedSieveWorkerExecutionStatusV1::succeeded;
    }
};

struct DistributedSieveWorkerExecutionResultV1 final {
    std::optional<WorkerHandoffV1> handoff;
    std::optional<DistributedSieveWorkerChunkCompletionV1> completion;
    DistributedSieveWorkerExecutionDiagnosticV1 diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return handoff.has_value() && completion.has_value() && static_cast<bool>(diagnostic);
    }
};

/// Consume one already-adopted entry and either publish its exact typed worker
/// handoff or fail closed. Validation and allocation-heavy construction occur
/// before entry -> writer authority transfer.
[[nodiscard]] DistributedSieveWorkerExecutionResultV1 execute_distributed_sieve_worker_entry_v1(
    distributed_sieve_worker_entry_detail::DistributedSieveWorkerEntryV1&& entry) noexcept;

} // namespace gnfs::sieve::distributed_sieve_worker_execution_detail

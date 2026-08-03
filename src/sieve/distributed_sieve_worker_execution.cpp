#include "distributed_sieve_worker_execution_internal.hpp"

#include "distributed_sieve_worker_runtime_internal.hpp"

#include <gnfs/core/relation.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gnfs::sieve::distributed_sieve_worker_execution_detail {
namespace {

using Entry = distributed_sieve_worker_entry_detail::DistributedSieveWorkerEntryV1;
using Writer = distributed_sieve_worker_entry_detail::DistributedSieveWorkerWriterAuthorityV1;

[[nodiscard]] int close_preserving_error(int descriptor, int prior_error) noexcept {
#if defined(__APPLE__) || defined(__linux__)
    if (::close(descriptor) != 0 && prior_error == 0) {
        return errno;
    }
#else
    (void)descriptor;
#endif
    return prior_error;
}

[[nodiscard]] DistributedSieveCurrentExecutableDigestResultV1
hash_current_executable_path() noexcept {
#if defined(__APPLE__) || defined(__linux__)
    int descriptor = -1;
#if defined(__APPLE__)
    std::uint32_t path_size = 0;
    if (_NSGetExecutablePath(nullptr, &path_size) == 0 || path_size == 0) {
        return {std::nullopt, EINVAL};
    }
    std::vector<char> path;
    try {
        path.resize(path_size);
    } catch (const std::bad_alloc&) {
        return {std::nullopt, ENOMEM};
    } catch (...) {
        return {std::nullopt, EINVAL};
    }
    if (_NSGetExecutablePath(path.data(), &path_size) != 0) {
        return {std::nullopt, EINVAL};
    }
    int open_flags = O_RDONLY | O_CLOEXEC;
    // _NSGetExecutablePath() may preserve the symlink used for exec. This
    // adapter already documents path-reopen TOCTOU as a non-adversarial
    // boundary, so following that launch symlink is required for compatibility.
    do {
        descriptor = ::open(path.data(), open_flags);
    } while (descriptor < 0 && errno == EINTR);
#else
    do {
        descriptor = ::open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
#endif
    if (descriptor < 0) {
        return {std::nullopt, errno};
    }

    struct stat metadata {};
    if (::fstat(descriptor, &metadata) != 0) {
        const int failure = close_preserving_error(descriptor, errno);
        return {std::nullopt, failure};
    }
    if (!S_ISREG(metadata.st_mode)) {
        const int failure = close_preserving_error(descriptor, EINVAL);
        return {std::nullopt, failure};
    }

    util::Sha256Accumulator accumulator;
    std::array<std::byte, 64U * 1024U> buffer{};
    while (true) {
        ssize_t count;
        do {
            count = ::read(descriptor, buffer.data(), buffer.size());
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            const int failure = close_preserving_error(descriptor, errno);
            return {std::nullopt, failure};
        }
        if (count == 0) {
            break;
        }
        if (!accumulator.update(
                std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(count)))) {
            const int failure = close_preserving_error(descriptor, EOVERFLOW);
            return {std::nullopt, failure};
        }
    }
    if (const int failure = close_preserving_error(descriptor, 0); failure != 0) {
        return {std::nullopt, failure};
    }
    auto digest = accumulator.finalize();
    if (!digest.has_value()) {
        return {std::nullopt, EIO};
    }
    return {digest, 0};
#else
    return {std::nullopt, ENOTSUP};
#endif
}

[[nodiscard]] bool exact_chunk(const ChunkPlanV1& left, const ChunkPlanV1& right) noexcept {
    return left.chunk_id == right.chunk_id && left.sq_begin == right.sq_begin &&
           left.sq_end == right.sq_end &&
           left.relative_artifact_stem == right.relative_artifact_stem;
}

struct WriterSinkContext final {
    Writer* writer = nullptr;
};

[[nodiscard]] bool append_to_writer(void* raw_context, const core::Relation& relation) noexcept {
    auto* context = static_cast<WriterSinkContext*>(raw_context);
    if (context == nullptr || context->writer == nullptr) {
        return false;
    }
    try {
        const std::size_t previous_count = context->writer->count();
        return context->writer->write(relation) == previous_count &&
               context->writer->count() == previous_count + 1U;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] DistributedSieveWorkerExecutionStatusV1
map_chunk_failure(DistributedSieveWorkerChunkStatusV1 status) noexcept {
    return status == DistributedSieveWorkerChunkStatusV1::resource_exhausted
               ? DistributedSieveWorkerExecutionStatusV1::resource_exhausted
               : DistributedSieveWorkerExecutionStatusV1::execution_failed;
}

} // namespace

DistributedSieveCurrentExecutableDigestResultV1
current_distributed_sieve_worker_executable_sha256_v1() noexcept {
    return hash_current_executable_path();
}

DistributedSieveWorkerExecutionResultV1
execute_distributed_sieve_worker_entry_v1(Entry&& entry) noexcept {
    DistributedSieveWorkerExecutionResultV1 result;
    auto& diagnostic = result.diagnostic;

#if !defined(__APPLE__)
    diagnostic.phase = DistributedSieveWorkerExecutionPhaseV1::platform_gate;
    diagnostic.status = DistributedSieveWorkerExecutionStatusV1::platform_unsupported;
    diagnostic.native_error = ENOTSUP;
    return result;
#endif

    diagnostic.phase = DistributedSieveWorkerExecutionPhaseV1::entry_revalidation;
    const auto entry_diagnostic = entry.revalidate();
    diagnostic.entry_status = entry_diagnostic.status;
    if (!entry_diagnostic) {
        diagnostic.status = DistributedSieveWorkerExecutionStatusV1::entry_invalid;
        diagnostic.native_error = entry_diagnostic.native_error;
        diagnostic.protocol_status = entry_diagnostic.protocol_status;
        return result;
    }

    diagnostic.phase = DistributedSieveWorkerExecutionPhaseV1::executable_identity;
    const auto executable = current_distributed_sieve_worker_executable_sha256_v1();
    if (!executable || !executable.digest.has_value()) {
        diagnostic.status = DistributedSieveWorkerExecutionStatusV1::executable_unavailable;
        diagnostic.native_error = executable.native_error;
        return result;
    }
    diagnostic.protocol_status =
        validate_manifest_executable_identity(entry.manifest(), *executable.digest);
    if (!diagnostic.protocol_status) {
        diagnostic.status = DistributedSieveWorkerExecutionStatusV1::executable_mismatch;
        return result;
    }

    diagnostic.phase = DistributedSieveWorkerExecutionPhaseV1::runtime_rehydration;
    auto runtime = rehydrate_distributed_sieve_worker_runtime_v1(entry.identity());
    if (!runtime || !runtime.runtime.has_value()) {
        diagnostic.status =
            runtime.status.error == DistributedSieveProtocolError::resource_exhausted
                ? DistributedSieveWorkerExecutionStatusV1::resource_exhausted
                : DistributedSieveWorkerExecutionStatusV1::runtime_invalid;
        diagnostic.protocol_status = runtime.status;
        return result;
    }

    const auto& chunk = entry.chunk();
    const auto expected_chunk = std::find_if(
        runtime.runtime->bound_work.chunks.begin(), runtime.runtime->bound_work.chunks.end(),
        [&](const ChunkPlanV1& candidate) { return candidate.chunk_id == chunk.chunk_id; });
    if (expected_chunk == runtime.runtime->bound_work.chunks.end() ||
        !exact_chunk(*expected_chunk, chunk)) {
        diagnostic.phase = DistributedSieveWorkerExecutionPhaseV1::chunk_preparation;
        diagnostic.status = DistributedSieveWorkerExecutionStatusV1::chunk_invalid;
        return result;
    }

    diagnostic.phase = DistributedSieveWorkerExecutionPhaseV1::chunk_preparation;
    auto prepared = prepare_distributed_sieve_worker_chunk_v1(runtime.runtime->polynomial,
                                                              runtime.runtime->factor_base,
                                                              runtime.runtime->bound_work, chunk);
    diagnostic.chunk_status = prepared.status;
    if (!prepared || !prepared.prepared.has_value()) {
        diagnostic.status =
            prepared.status == DistributedSieveWorkerChunkStatusV1::resource_exhausted
                ? DistributedSieveWorkerExecutionStatusV1::resource_exhausted
                : DistributedSieveWorkerExecutionStatusV1::chunk_invalid;
        return result;
    }

    diagnostic.phase = DistributedSieveWorkerExecutionPhaseV1::writer_adoption;
    auto adopted =
        distributed_sieve_worker_entry_detail::consume_distributed_sieve_worker_writer_v1(
            std::move(entry));
    diagnostic.writer_status = adopted.diagnostic.status;
    if (!adopted || !adopted.writer.has_value()) {
        diagnostic.status = DistributedSieveWorkerExecutionStatusV1::writer_unavailable;
        diagnostic.native_error = adopted.diagnostic.native_error;
        diagnostic.artifacts_may_remain =
            adopted.writer.has_value() || adopted.diagnostic.reconciliation_required();
        return result;
    }
    if (adopted.writer->count() != 0) {
        diagnostic.status = DistributedSieveWorkerExecutionStatusV1::writer_unavailable;
        diagnostic.artifacts_may_remain = true;
        return result;
    }

    WriterSinkContext sink_context{.writer = &*adopted.writer};
    diagnostic.phase = DistributedSieveWorkerExecutionPhaseV1::chunk_execution;
    auto executed =
        prepared.prepared->execute({.context = &sink_context, .append = append_to_writer});
    diagnostic.chunk_status = executed.status;
    diagnostic.artifacts_may_remain = executed.artifacts_may_remain;
    if (!executed || !executed.completion.has_value() ||
        adopted.writer->count() != executed.accepted_relation_count) {
        diagnostic.status = map_chunk_failure(executed.status);
        diagnostic.artifacts_may_remain = true;
        return result;
    }
    result.completion = executed.completion;

    diagnostic.phase = DistributedSieveWorkerExecutionPhaseV1::handoff_publication;
    try {
        distributed_sieve_worker_entry_detail::DistributedSieveWorkerCompletionFactsV1 completion{
            .processed_sq_count = executed.completion->processed_sq_count,
            .next_sq_index = executed.completion->next_sq_index,
            .completion_reason = executed.completion->completion_reason,
        };
        result.handoff = adopted.writer->finalize_and_publish_handoff(completion);
        diagnostic.status = DistributedSieveWorkerExecutionStatusV1::succeeded;
        diagnostic.artifacts_may_remain = false;
        return result;
    } catch (const std::bad_alloc&) {
        diagnostic.status = DistributedSieveWorkerExecutionStatusV1::resource_exhausted;
        diagnostic.artifacts_may_remain = true;
        return result;
    } catch (...) {
        diagnostic.status = DistributedSieveWorkerExecutionStatusV1::handoff_failed;
        diagnostic.artifacts_may_remain = true;
        return result;
    }
}

} // namespace gnfs::sieve::distributed_sieve_worker_execution_detail

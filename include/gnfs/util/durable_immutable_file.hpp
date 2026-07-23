#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <system_error>

namespace gnfs::util::durable_immutable_file {

using NativeHandle = std::intptr_t;
inline constexpr NativeHandle INVALID_NATIVE_HANDLE = static_cast<NativeHandle>(-1);

enum class OperationState : std::uint8_t {
    succeeded,
    interrupted,
    failed,
};

class OperationResult final {
public:
    OperationResult() = delete;

    [[nodiscard]] static OperationResult succeeded() noexcept {
        return OperationResult(OperationState::succeeded, {});
    }

    [[nodiscard]] static OperationResult interrupted(std::error_code error) noexcept {
        return OperationResult(OperationState::interrupted, error);
    }

    [[nodiscard]] static OperationResult failed(std::error_code error) noexcept {
        return OperationResult(OperationState::failed, error);
    }

    [[nodiscard]] constexpr OperationState state() const noexcept {
        return state_;
    }

    [[nodiscard]] const std::error_code& native_error() const noexcept {
        return native_error_;
    }

private:
    OperationResult(OperationState state, std::error_code native_error) noexcept
        : state_(state), native_error_(native_error) {}

    OperationState state_;
    std::error_code native_error_;
};

class OpenResult final {
public:
    OpenResult() = delete;

    [[nodiscard]] static OpenResult succeeded(NativeHandle handle) noexcept {
        return OpenResult(OperationState::succeeded, handle, {});
    }

    [[nodiscard]] static OpenResult interrupted(std::error_code error) noexcept {
        return OpenResult(OperationState::interrupted, INVALID_NATIVE_HANDLE, error);
    }

    [[nodiscard]] static OpenResult failed(std::error_code error) noexcept {
        return OpenResult(OperationState::failed, INVALID_NATIVE_HANDLE, error);
    }

    [[nodiscard]] constexpr OperationState state() const noexcept {
        return state_;
    }

    [[nodiscard]] constexpr NativeHandle handle() const noexcept {
        return handle_;
    }

    [[nodiscard]] const std::error_code& native_error() const noexcept {
        return native_error_;
    }

private:
    OpenResult(OperationState state, NativeHandle handle, std::error_code native_error) noexcept
        : state_(state), handle_(handle), native_error_(native_error) {}

    OperationState state_;
    NativeHandle handle_;
    std::error_code native_error_;
};

class WriteResult final {
public:
    WriteResult() = delete;

    [[nodiscard]] static WriteResult succeeded(std::size_t bytes_written) noexcept {
        return WriteResult(OperationState::succeeded, bytes_written, {});
    }

    [[nodiscard]] static WriteResult interrupted(std::error_code error) noexcept {
        return WriteResult(OperationState::interrupted, 0, error);
    }

    [[nodiscard]] static WriteResult failed(std::error_code error) noexcept {
        return WriteResult(OperationState::failed, 0, error);
    }

    [[nodiscard]] constexpr OperationState state() const noexcept {
        return state_;
    }

    [[nodiscard]] constexpr std::size_t bytes_written() const noexcept {
        return bytes_written_;
    }

    [[nodiscard]] const std::error_code& native_error() const noexcept {
        return native_error_;
    }

private:
    WriteResult(OperationState state, std::size_t bytes_written,
                std::error_code native_error) noexcept
        : state_(state), bytes_written_(bytes_written), native_error_(native_error) {}

    OperationState state_;
    std::size_t bytes_written_;
    std::error_code native_error_;
};

/// Injectable native I/O boundary. Implementations must never truncate, remove,
/// or rename the destination. An interrupted close is terminal because retrying
/// close(2) is not portable once descriptor ownership becomes indeterminate.
class FileOps {
public:
    virtual ~FileOps() = default;

    /// Open the frozen parent without following a final symlink/reparse point.
    /// The returned handle remains owned by this publish until the matching
    /// close_parent_directory() call.
    [[nodiscard]] virtual OpenResult
    open_parent_directory(const std::filesystem::path& frozen_parent_path) noexcept = 0;

    /// `parent_handle` is caller-exclusive for the complete publish operation.
    /// POSIX implementations create relative to it; Win32 implementations also
    /// receive the frozen absolute path because Win32 has no public relative
    /// CREATE_NEW operation.
    [[nodiscard]] virtual OpenResult
    open_exclusive(NativeHandle parent_handle, const std::filesystem::path& leaf,
                   const std::filesystem::path& frozen_absolute_path) noexcept = 0;

    [[nodiscard]] virtual WriteResult write_some(NativeHandle handle,
                                                 std::span<const std::byte> bytes) noexcept = 0;

    /// Called before and after the parent-directory sync; implementations must
    /// provide the platform's strongest required file durability boundary.
    [[nodiscard]] virtual OperationResult sync_file(NativeHandle handle) noexcept = 0;
    [[nodiscard]] virtual OperationResult close_file(NativeHandle handle) noexcept = 0;

    /// Sync the same parent handle returned by open_parent_directory(); never
    /// reopen the directory by path.
    [[nodiscard]] virtual OperationResult
    sync_parent_directory(NativeHandle parent_handle) noexcept = 0;
    [[nodiscard]] virtual OperationResult
    close_parent_directory(NativeHandle parent_handle) noexcept = 0;
};

enum class PublishStatus : std::uint8_t {
    durable,
    invalid_path,
    input_too_large,
    parent_directory_open_failed,
    already_exists,
    open_failed,
    write_failed,
    zero_write_progress,
    file_sync_failed,
    close_failed,
    parent_directory_sync_failed,
    parent_directory_close_failed,
    file_ops_contract_violation,
    unexpected_failure,
};

/// Closed publish outcome. There is deliberately no default constructor, so a
/// caller cannot obtain a result that accidentally looks durable.
class PublishResult final {
public:
    PublishResult() = delete;

    [[nodiscard]] constexpr PublishStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] constexpr bool is_durable() const noexcept {
        return status_ == PublishStatus::durable;
    }

    [[nodiscard]] const std::error_code& native_error() const noexcept {
        return native_error_;
    }

    [[nodiscard]] constexpr std::uint64_t bytes_written() const noexcept {
        return bytes_written_;
    }

    PublishResult(PublishStatus status, std::error_code native_error,
                  std::uint64_t bytes_written) noexcept
        : status_(status), native_error_(native_error), bytes_written_(bytes_written) {}

private:
    PublishStatus status_;
    std::error_code native_error_;
    std::uint64_t bytes_written_;
};

/// Publish one new immutable file through an injected I/O implementation.
/// Success means file contents and the parent-directory publication boundary
/// have both reached the platform durability boundary. The caller must
/// exclusively control the parent namespace while publishing; the contract
/// assumes no malicious rename or reparse-point substitution.
[[nodiscard]] PublishResult publish_with_ops(const std::filesystem::path& path,
                                             std::span<const std::byte> bytes,
                                             FileOps& ops) noexcept;

/// Production native implementation of publish_with_ops().
[[nodiscard]] PublishResult publish(const std::filesystem::path& path,
                                    std::span<const std::byte> bytes) noexcept;

} // namespace gnfs::util::durable_immutable_file

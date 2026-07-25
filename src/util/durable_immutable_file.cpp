#include <gnfs/util/durable_immutable_file.hpp>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <new>
#include <optional>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gnfs::util::durable_immutable_file {
namespace {

struct FrozenPath final {
    std::filesystem::path file;
    std::filesystem::path parent;
    std::filesystem::path leaf;
};

[[nodiscard]] std::error_code invalid_argument_error() noexcept {
    return std::make_error_code(std::errc::invalid_argument);
}

[[nodiscard]] std::error_code protocol_error() noexcept {
    return std::make_error_code(std::errc::protocol_error);
}

[[nodiscard]] bool contains_nul(const std::filesystem::path& path) noexcept {
    const auto& native = path.native();
    return std::find(native.begin(), native.end(), std::filesystem::path::value_type{}) !=
           native.end();
}

[[nodiscard]] bool invalid_leaf(const std::filesystem::path& leaf) {
    return leaf.empty() || leaf == "." || leaf == ".." || contains_nul(leaf);
}

[[nodiscard]] bool invalid_relative_leaf(const std::filesystem::path& leaf) {
    return invalid_leaf(leaf) || leaf.is_absolute() || leaf.has_parent_path() ||
           leaf.filename() != leaf;
}

[[nodiscard]] std::optional<FrozenPath> freeze_path(const std::filesystem::path& requested,
                                                    std::error_code& error) {
    if (requested.empty() || contains_nul(requested) || invalid_leaf(requested.filename())) {
        error = invalid_argument_error();
        return std::nullopt;
    }

    auto absolute = std::filesystem::absolute(requested, error);
    if (error) {
        return std::nullopt;
    }
    absolute = absolute.lexically_normal();
    if (absolute.empty() || absolute == absolute.root_path() || invalid_leaf(absolute.filename())) {
        error = invalid_argument_error();
        return std::nullopt;
    }

    auto parent = std::filesystem::weakly_canonical(absolute.parent_path(), error);
    if (error) {
        return std::nullopt;
    }
    parent = parent.lexically_normal();
    if (parent.empty() || !parent.is_absolute() || contains_nul(parent)) {
        error = invalid_argument_error();
        return std::nullopt;
    }

    auto leaf = absolute.filename();
    auto frozen = (parent / leaf).lexically_normal();
    if (frozen.empty() || !frozen.is_absolute() || frozen == frozen.root_path() ||
        invalid_leaf(frozen.filename()) || frozen.parent_path() != parent) {
        error = invalid_argument_error();
        return std::nullopt;
    }
    return FrozenPath{std::move(frozen), std::move(parent), std::move(leaf)};
}

[[nodiscard]] PublishResult failure(PublishStatus status, const std::error_code& error,
                                    std::size_t bytes_written) noexcept {
    return PublishResult(status, error, static_cast<std::uint64_t>(bytes_written));
}

[[nodiscard]] std::error_code close_error_or_protocol(const OperationResult& result) noexcept {
    return result.native_error() ? result.native_error() : protocol_error();
}

enum class ParentHandleOwnership : std::uint8_t {
    borrowed,
    owned,
};

[[nodiscard]] PublishResult close_parent_and_return(FileOps& ops, NativeHandle parent_handle,
                                                    ParentHandleOwnership ownership,
                                                    const PublishResult& intended) noexcept {
    if (ownership == ParentHandleOwnership::borrowed) {
        return intended;
    }
    const OperationResult closed = ops.close_parent_directory(parent_handle);
    switch (closed.state()) {
    case OperationState::succeeded:
        return intended;
    case OperationState::interrupted:
    case OperationState::failed:
        return PublishResult(PublishStatus::parent_directory_close_failed,
                             close_error_or_protocol(closed), intended.bytes_written());
    default:
        return PublishResult(PublishStatus::file_ops_contract_violation, protocol_error(),
                             intended.bytes_written());
    }
}

[[nodiscard]] PublishResult close_file_and_parent(FileOps& ops, NativeHandle file_handle,
                                                  NativeHandle parent_handle,
                                                  ParentHandleOwnership ownership,
                                                  const PublishResult& intended) noexcept {
    const OperationResult closed = ops.close_file(file_handle);
    switch (closed.state()) {
    case OperationState::succeeded:
        return close_parent_and_return(ops, parent_handle, ownership, intended);
    case OperationState::interrupted:
    case OperationState::failed:
        return close_parent_and_return(ops, parent_handle, ownership,
                                       PublishResult(PublishStatus::close_failed,
                                                     close_error_or_protocol(closed),
                                                     intended.bytes_written()));
    default:
        return close_parent_and_return(ops, parent_handle, ownership,
                                       PublishResult(PublishStatus::file_ops_contract_violation,
                                                     protocol_error(), intended.bytes_written()));
    }
}

[[nodiscard]] PublishResult finish_durable_file(FileOps& ops, NativeHandle file_handle,
                                                NativeHandle parent_handle,
                                                ParentHandleOwnership ownership,
                                                std::size_t bytes_written) noexcept {
    for (;;) {
        const OperationResult synced = ops.sync_file(file_handle);
        switch (synced.state()) {
        case OperationState::succeeded:
            break;
        case OperationState::interrupted:
            continue;
        case OperationState::failed:
            return close_file_and_parent(
                ops, file_handle, parent_handle, ownership,
                failure(PublishStatus::file_sync_failed, synced.native_error(), bytes_written));
        default:
            return close_file_and_parent(ops, file_handle, parent_handle, ownership,
                                         failure(PublishStatus::file_ops_contract_violation,
                                                 protocol_error(), bytes_written));
        }
        break;
    }

    for (;;) {
        const OperationResult synced_parent = ops.sync_parent_directory(parent_handle);
        switch (synced_parent.state()) {
        case OperationState::succeeded:
            break;
        case OperationState::interrupted:
            continue;
        case OperationState::failed:
            return close_file_and_parent(ops, file_handle, parent_handle, ownership,
                                         failure(PublishStatus::parent_directory_sync_failed,
                                                 synced_parent.native_error(), bytes_written));
        default:
            return close_file_and_parent(ops, file_handle, parent_handle, ownership,
                                         failure(PublishStatus::file_ops_contract_violation,
                                                 protocol_error(), bytes_written));
        }
        break;
    }

    // Keep both handles open through a final file barrier. The first file
    // barrier establishes content durability, the parent barrier publishes the
    // directory entry, and this second file barrier orders both before close.
    for (;;) {
        const OperationResult synced = ops.sync_file(file_handle);
        switch (synced.state()) {
        case OperationState::succeeded:
            break;
        case OperationState::interrupted:
            continue;
        case OperationState::failed:
            return close_file_and_parent(
                ops, file_handle, parent_handle, ownership,
                failure(PublishStatus::file_sync_failed, synced.native_error(), bytes_written));
        default:
            return close_file_and_parent(ops, file_handle, parent_handle, ownership,
                                         failure(PublishStatus::file_ops_contract_violation,
                                                 protocol_error(), bytes_written));
        }
        break;
    }

    const OperationResult closed_file = ops.close_file(file_handle);
    switch (closed_file.state()) {
    case OperationState::succeeded:
        break;
    case OperationState::interrupted:
    case OperationState::failed:
        return close_parent_and_return(ops, parent_handle, ownership,
                                       failure(PublishStatus::close_failed,
                                               close_error_or_protocol(closed_file),
                                               bytes_written));
    default:
        return close_parent_and_return(
            ops, parent_handle, ownership,
            failure(PublishStatus::file_ops_contract_violation, protocol_error(), bytes_written));
    }

    return close_parent_and_return(ops, parent_handle, ownership,
                                   failure(PublishStatus::durable, {}, bytes_written));
}

template <typename OpenExclusive>
[[nodiscard]] PublishResult
publish_to_open_parent(NativeHandle parent_handle, std::span<const std::byte> bytes, FileOps& ops,
                       ParentHandleOwnership ownership, OpenExclusive&& open_exclusive) noexcept {
    NativeHandle file_handle = INVALID_NATIVE_HANDLE;
    for (;;) {
        const OpenResult opened_file = open_exclusive();
        switch (opened_file.state()) {
        case OperationState::succeeded:
            if (opened_file.handle() == INVALID_NATIVE_HANDLE ||
                opened_file.handle() == parent_handle) {
                return close_parent_and_return(
                    ops, parent_handle, ownership,
                    PublishResult(PublishStatus::file_ops_contract_violation, protocol_error(), 0));
            }
            file_handle = opened_file.handle();
            break;
        case OperationState::interrupted:
            continue;
        case OperationState::failed:
            return close_parent_and_return(
                ops, parent_handle, ownership,
                PublishResult(opened_file.native_error() == std::errc::file_exists
                                  ? PublishStatus::already_exists
                                  : PublishStatus::open_failed,
                              opened_file.native_error(), 0));
        default:
            return close_parent_and_return(
                ops, parent_handle, ownership,
                PublishResult(PublishStatus::file_ops_contract_violation, protocol_error(), 0));
        }
        break;
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const WriteResult written = ops.write_some(file_handle, bytes.subspan(offset));
        switch (written.state()) {
        case OperationState::interrupted:
            continue;
        case OperationState::failed:
            return close_file_and_parent(
                ops, file_handle, parent_handle, ownership,
                failure(PublishStatus::write_failed, written.native_error(), offset));
        case OperationState::succeeded:
            if (written.bytes_written() == 0) {
                return close_file_and_parent(ops, file_handle, parent_handle, ownership,
                                             failure(PublishStatus::zero_write_progress,
                                                     std::make_error_code(std::errc::io_error),
                                                     offset));
            }
            if (written.bytes_written() > bytes.size() - offset) {
                return close_file_and_parent(
                    ops, file_handle, parent_handle, ownership,
                    failure(PublishStatus::file_ops_contract_violation, protocol_error(), offset));
            }
            offset += written.bytes_written();
            break;
        default:
            return close_file_and_parent(
                ops, file_handle, parent_handle, ownership,
                failure(PublishStatus::file_ops_contract_violation, protocol_error(), offset));
        }
    }

    return finish_durable_file(ops, file_handle, parent_handle, ownership, offset);
}

#ifdef _WIN32

[[nodiscard]] std::error_code windows_error(DWORD value) noexcept {
    return {static_cast<int>(value), std::system_category()};
}

[[nodiscard]] HANDLE windows_handle(NativeHandle handle) noexcept {
    return reinterpret_cast<HANDLE>(handle);
}

class ProductionFileOps final : public FileOps {
public:
    [[nodiscard]] OpenResult
    open_parent_directory(const std::filesystem::path& frozen_parent_path) noexcept override {
        const HANDLE handle = ::CreateFileW(
            frozen_parent_path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return OpenResult::failed(windows_error(::GetLastError()));
        }

        BY_HANDLE_FILE_INFORMATION information{};
        if (!::GetFileInformationByHandle(handle, &information)) {
            const DWORD saved_error = ::GetLastError();
            (void)::CloseHandle(handle);
            return OpenResult::failed(windows_error(saved_error));
        }
        if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            (void)::CloseHandle(handle);
            return OpenResult::failed(windows_error(ERROR_DIRECTORY));
        }
        if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            (void)::CloseHandle(handle);
            return OpenResult::failed(windows_error(ERROR_ACCESS_DENIED));
        }
        return OpenResult::succeeded(reinterpret_cast<NativeHandle>(handle));
    }

    [[nodiscard]] OpenResult
    open_exclusive(NativeHandle, const std::filesystem::path&,
                   const std::filesystem::path& frozen_absolute_path) noexcept override {
        const HANDLE handle = ::CreateFileW(
            frozen_absolute_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return OpenResult::failed(windows_error(::GetLastError()));
        }
        return OpenResult::succeeded(reinterpret_cast<NativeHandle>(handle));
    }

    [[nodiscard]] WriteResult write_some(NativeHandle handle,
                                         std::span<const std::byte> bytes) noexcept override {
        const auto request = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size(), std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!::WriteFile(windows_handle(handle), bytes.data(), request, &written, nullptr)) {
            return WriteResult::failed(windows_error(::GetLastError()));
        }
        return WriteResult::succeeded(static_cast<std::size_t>(written));
    }

    [[nodiscard]] OperationResult sync_file(NativeHandle handle) noexcept override {
        if (!::FlushFileBuffers(windows_handle(handle))) {
            return OperationResult::failed(windows_error(::GetLastError()));
        }
        return OperationResult::succeeded();
    }

    [[nodiscard]] OperationResult close_file(NativeHandle handle) noexcept override {
        if (!::CloseHandle(windows_handle(handle))) {
            return OperationResult::failed(windows_error(::GetLastError()));
        }
        return OperationResult::succeeded();
    }

    [[nodiscard]] OperationResult
    sync_parent_directory(NativeHandle parent_handle) noexcept override {
        // CreateFile documents that FILE_FLAG_WRITE_THROUGH flushes cached data
        // without delay and causes NTFS to flush metadata changes from the
        // write-through request. FlushFileBuffers on the held parent is the
        // Windows publication boundary; unsupported filesystems fail closed.
        if (!::FlushFileBuffers(windows_handle(parent_handle))) {
            return OperationResult::failed(windows_error(::GetLastError()));
        }
        return OperationResult::succeeded();
    }

    [[nodiscard]] OperationResult
    close_parent_directory(NativeHandle parent_handle) noexcept override {
        if (!::CloseHandle(windows_handle(parent_handle))) {
            return OperationResult::failed(windows_error(::GetLastError()));
        }
        return OperationResult::succeeded();
    }
};

#else

[[nodiscard]] std::error_code posix_error(int value) noexcept {
    return {value, std::generic_category()};
}

class ProductionFileOps final : public FileOps {
public:
    [[nodiscard]] OpenResult
    open_parent_directory(const std::filesystem::path& frozen_parent_path) noexcept override {
        const int descriptor =
            ::open(frozen_parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (descriptor >= 0) {
            return OpenResult::succeeded(static_cast<NativeHandle>(descriptor));
        }
        const int saved_errno = errno;
        if (saved_errno == EINTR) {
            return OpenResult::interrupted(posix_error(saved_errno));
        }
        return OpenResult::failed(posix_error(saved_errno));
    }

    [[nodiscard]] OpenResult open_exclusive(NativeHandle parent_handle,
                                            const std::filesystem::path& leaf,
                                            const std::filesystem::path&) noexcept override {
        return open_exclusive_at(parent_handle, leaf);
    }

    [[nodiscard]] OpenResult
    open_exclusive_at(NativeHandle parent_handle,
                      const std::filesystem::path& leaf) noexcept override {
        const int parent_descriptor = static_cast<int>(parent_handle);
        const int descriptor = ::openat(parent_descriptor, leaf.c_str(),
                                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (descriptor < 0) {
            const int saved_errno = errno;
            if (saved_errno == EINTR) {
                return OpenResult::interrupted(posix_error(saved_errno));
            }
            return OpenResult::failed(posix_error(saved_errno));
        }

        struct stat created{};
        int stat_result = -1;
        do {
            stat_result = ::fstat(descriptor, &created);
        } while (stat_result != 0 && errno == EINTR);
        if (stat_result != 0) {
            const int saved_errno = errno;
            (void)::close(descriptor);
            return OpenResult::failed(posix_error(saved_errno));
        }

        int chmod_result = -1;
        do {
            chmod_result = ::fchmod(descriptor, 0600);
        } while (chmod_result != 0 && errno == EINTR);

        int setup_error = chmod_result == 0 ? 0 : errno;
        struct stat configured{};
        if (setup_error == 0) {
            do {
                stat_result = ::fstat(descriptor, &configured);
            } while (stat_result != 0 && errno == EINTR);
            if (stat_result != 0) {
                setup_error = errno;
            } else if (configured.st_dev != created.st_dev || configured.st_ino != created.st_ino ||
                       !S_ISREG(configured.st_mode) || configured.st_nlink != 1 ||
                       static_cast<std::uint64_t>(configured.st_uid) !=
                           static_cast<std::uint64_t>(::geteuid()) ||
                       (configured.st_mode & static_cast<mode_t>(07777)) !=
                           static_cast<mode_t>(0600)) {
                setup_error = EACCES;
            }
        }
        if (setup_error == 0) {
            return OpenResult::succeeded(static_cast<NativeHandle>(descriptor));
        }

        // FileOps never removes or renames a destination. Preserve the
        // O_EXCL-created leaf after a setup failure so a higher-level protocol
        // can classify its exact identity without a path-based cleanup race.
        if (::close(descriptor) != 0) {
            return OpenResult::failed(posix_error(errno));
        }
        return OpenResult::failed(posix_error(setup_error));
    }

    [[nodiscard]] OpenResult open_existing_at(NativeHandle parent_handle,
                                              const std::filesystem::path& leaf) noexcept override {
        const int descriptor = ::openat(static_cast<int>(parent_handle), leaf.c_str(),
                                        O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (descriptor >= 0) {
            return OpenResult::succeeded(static_cast<NativeHandle>(descriptor));
        }
        const int saved_errno = errno;
        if (saved_errno == EINTR) {
            return OpenResult::interrupted(posix_error(saved_errno));
        }
        return OpenResult::failed(posix_error(saved_errno));
    }

    [[nodiscard]] WriteResult write_some(NativeHandle handle,
                                         std::span<const std::byte> bytes) noexcept override {
        const auto request = std::min<std::size_t>(
            bytes.size(), static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t written = ::write(static_cast<int>(handle), bytes.data(), request);
        if (written >= 0) {
            return WriteResult::succeeded(static_cast<std::size_t>(written));
        }
        const int saved_errno = errno;
        if (saved_errno == EINTR) {
            return WriteResult::interrupted(posix_error(saved_errno));
        }
        return WriteResult::failed(posix_error(saved_errno));
    }

    [[nodiscard]] OperationResult sync_file(NativeHandle handle) noexcept override {
#if defined(__APPLE__)
        // F_FULLFSYNC is the required macOS durability boundary. Unsupported
        // descriptors or filesystems fail closed; do not downgrade to fsync.
        const int result = ::fcntl(static_cast<int>(handle), F_FULLFSYNC);
#else
        const int result = ::fsync(static_cast<int>(handle));
#endif
        if (result == 0) {
            return OperationResult::succeeded();
        }
        const int saved_errno = errno;
        if (saved_errno == EINTR) {
            return OperationResult::interrupted(posix_error(saved_errno));
        }
        return OperationResult::failed(posix_error(saved_errno));
    }

    [[nodiscard]] OperationResult close_file(NativeHandle handle) noexcept override {
        if (::close(static_cast<int>(handle)) == 0) {
            return OperationResult::succeeded();
        }
        // POSIX leaves descriptor ownership after EINTR implementation-defined.
        // Never retry close: publication remains non-durable and the caller gets
        // the native error.
        return OperationResult::failed(posix_error(errno));
    }

    [[nodiscard]] OperationResult
    sync_parent_directory(NativeHandle parent_handle) noexcept override {
#if defined(__APPLE__)
        const int result = ::fcntl(static_cast<int>(parent_handle), F_FULLFSYNC);
#else
        const int result = ::fsync(static_cast<int>(parent_handle));
#endif
        if (result == 0) {
            return OperationResult::succeeded();
        }
        const int saved_errno = errno;
        if (saved_errno == EINTR) {
            return OperationResult::interrupted(posix_error(saved_errno));
        }
        return OperationResult::failed(posix_error(saved_errno));
    }

    [[nodiscard]] OperationResult
    close_parent_directory(NativeHandle parent_handle) noexcept override {
        if (::close(static_cast<int>(parent_handle)) == 0) {
            return OperationResult::succeeded();
        }
        // As with a file descriptor, retrying close after EINTR could close a
        // reused descriptor. One attempt is the portable ownership boundary.
        return OperationResult::failed(posix_error(errno));
    }
};

#endif

} // namespace

PublishResult publish_with_ops(const std::filesystem::path& path, std::span<const std::byte> bytes,
                               FileOps& ops) noexcept {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (bytes.size() > std::numeric_limits<std::uint64_t>::max()) {
            return PublishResult(PublishStatus::input_too_large,
                                 std::make_error_code(std::errc::value_too_large), 0);
        }
    }

    std::optional<FrozenPath> frozen;
    try {
        std::error_code path_error;
        frozen = freeze_path(path, path_error);
        if (!frozen.has_value()) {
            return PublishResult(PublishStatus::invalid_path, path_error, 0);
        }
    } catch (const std::bad_alloc&) {
        return PublishResult(PublishStatus::unexpected_failure,
                             std::make_error_code(std::errc::not_enough_memory), 0);
    } catch (const std::filesystem::filesystem_error& error) {
        return PublishResult(PublishStatus::invalid_path, error.code(), 0);
    } catch (...) {
        return PublishResult(PublishStatus::unexpected_failure,
                             std::make_error_code(std::errc::io_error), 0);
    }

    NativeHandle parent_handle = INVALID_NATIVE_HANDLE;
    for (;;) {
        const OpenResult opened_parent = ops.open_parent_directory(frozen->parent);
        switch (opened_parent.state()) {
        case OperationState::succeeded:
            if (opened_parent.handle() == INVALID_NATIVE_HANDLE) {
                return PublishResult(PublishStatus::file_ops_contract_violation, protocol_error(),
                                     0);
            }
            parent_handle = opened_parent.handle();
            break;
        case OperationState::interrupted:
            continue;
        case OperationState::failed:
            return PublishResult(PublishStatus::parent_directory_open_failed,
                                 opened_parent.native_error(), 0);
        default:
            return PublishResult(PublishStatus::file_ops_contract_violation, protocol_error(), 0);
        }
        break;
    }

    return publish_to_open_parent(
        parent_handle, bytes, ops, ParentHandleOwnership::owned,
        [&]() noexcept { return ops.open_exclusive(parent_handle, frozen->leaf, frozen->file); });
}

PublishResult publish_at_with_ops(NativeHandle parent_handle, const std::filesystem::path& leaf,
                                  std::span<const std::byte> bytes, FileOps& ops) noexcept {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (bytes.size() > std::numeric_limits<std::uint64_t>::max()) {
            return PublishResult(PublishStatus::input_too_large,
                                 std::make_error_code(std::errc::value_too_large), 0);
        }
    }

    try {
        if (parent_handle == INVALID_NATIVE_HANDLE || invalid_relative_leaf(leaf)) {
            return PublishResult(PublishStatus::invalid_path, invalid_argument_error(), 0);
        }
    } catch (const std::bad_alloc&) {
        return PublishResult(PublishStatus::unexpected_failure,
                             std::make_error_code(std::errc::not_enough_memory), 0);
    } catch (const std::filesystem::filesystem_error& error) {
        return PublishResult(PublishStatus::invalid_path, error.code(), 0);
    } catch (...) {
        return PublishResult(PublishStatus::unexpected_failure,
                             std::make_error_code(std::errc::io_error), 0);
    }

    return publish_to_open_parent(
        parent_handle, bytes, ops, ParentHandleOwnership::borrowed,
        [&]() noexcept { return ops.open_exclusive_at(parent_handle, leaf); });
}

PublishResult confirm_durable_at_with_ops(NativeHandle parent_handle,
                                          const std::filesystem::path& leaf,
                                          FileOps& ops) noexcept {
    try {
        if (parent_handle == INVALID_NATIVE_HANDLE || invalid_relative_leaf(leaf)) {
            return PublishResult(PublishStatus::invalid_path, invalid_argument_error(), 0);
        }
    } catch (const std::bad_alloc&) {
        return PublishResult(PublishStatus::unexpected_failure,
                             std::make_error_code(std::errc::not_enough_memory), 0);
    } catch (const std::filesystem::filesystem_error& error) {
        return PublishResult(PublishStatus::invalid_path, error.code(), 0);
    } catch (...) {
        return PublishResult(PublishStatus::unexpected_failure,
                             std::make_error_code(std::errc::io_error), 0);
    }

    NativeHandle file_handle = INVALID_NATIVE_HANDLE;
    for (;;) {
        const OpenResult opened_file = ops.open_existing_at(parent_handle, leaf);
        switch (opened_file.state()) {
        case OperationState::succeeded:
            if (opened_file.handle() == INVALID_NATIVE_HANDLE ||
                opened_file.handle() == parent_handle) {
                return PublishResult(PublishStatus::file_ops_contract_violation, protocol_error(),
                                     0);
            }
            file_handle = opened_file.handle();
            break;
        case OperationState::interrupted:
            continue;
        case OperationState::failed:
            return PublishResult(PublishStatus::open_failed, opened_file.native_error(), 0);
        default:
            return PublishResult(PublishStatus::file_ops_contract_violation, protocol_error(), 0);
        }
        break;
    }

    return finish_durable_file(ops, file_handle, parent_handle, ParentHandleOwnership::borrowed, 0);
}

PublishResult publish(const std::filesystem::path& path,
                      std::span<const std::byte> bytes) noexcept {
    ProductionFileOps ops;
    return publish_with_ops(path, bytes, ops);
}

PublishResult publish_at(NativeHandle parent_handle, const std::filesystem::path& leaf,
                         std::span<const std::byte> bytes) noexcept {
    ProductionFileOps ops;
    return publish_at_with_ops(parent_handle, leaf, bytes, ops);
}

PublishResult confirm_durable_at(NativeHandle parent_handle,
                                 const std::filesystem::path& leaf) noexcept {
    ProductionFileOps ops;
    return confirm_durable_at_with_ops(parent_handle, leaf, ops);
}

} // namespace gnfs::util::durable_immutable_file

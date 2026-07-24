#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "bounded_child_process_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <utility>

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <features.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace gnfs::util {
namespace {

[[nodiscard]] constexpr bool digest_is_nonzero(const Sha256Digest& digest) noexcept {
    for (const std::byte value : digest.bytes) {
        if (value != std::byte{0}) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] ExecutableImageAuthenticationResult
authentication_failure(ExecutableImageAuthenticationError error, int native_error = 0) noexcept {
    ExecutableImageAuthenticationResult result;
    result.diagnostic.error = error;
    if (native_error != 0) {
        result.diagnostic.native_error = std::error_code(native_error, std::generic_category());
    }
    return result;
}

[[nodiscard]] BoundedChildProcessResult invalid_authenticated_run() noexcept {
    BoundedChildProcessResult result;
    result.error = BoundedChildProcessError::invalid_spec;
    result.cleanup_complete = true;
    return result;
}

#if defined(__linux__)

class UniqueFd final {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() {
        reset();
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

    [[nodiscard]] int release() noexcept {
        return std::exchange(fd_, -1);
    }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            (void)::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

[[nodiscard]] bool move_above_standard_streams(UniqueFd& fd, int& native_error) noexcept {
    if (fd.get() >= 3) {
        return true;
    }
    int duplicate = -1;
    do {
        duplicate = ::fcntl(fd.get(), F_DUPFD_CLOEXEC, 3);
    } while (duplicate < 0 && errno == EINTR);
    if (duplicate < 0) {
        native_error = errno;
        return false;
    }
    fd.reset(duplicate);
    return true;
}

[[nodiscard]] bool metadata_is_trusted(const struct stat& metadata,
                                       std::uint64_t expected_owner) noexcept {
    return S_ISREG(metadata.st_mode) && metadata.st_nlink == 1 &&
           static_cast<std::uint64_t>(metadata.st_uid) == expected_owner &&
           (metadata.st_mode & S_IXUSR) != 0 &&
           (metadata.st_mode & (S_IWGRP | S_IWOTH | S_ISUID | S_ISGID)) == 0 &&
           metadata.st_size >= 4 &&
           static_cast<std::uint64_t>(metadata.st_size) <= AUTHENTICATED_EXECUTABLE_IMAGE_MAX_BYTES;
}

[[nodiscard]] bool same_source_snapshot(const struct stat& before,
                                        const struct stat& after) noexcept {
    return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
           before.st_mode == after.st_mode && before.st_nlink == after.st_nlink &&
           before.st_uid == after.st_uid && before.st_gid == after.st_gid &&
           before.st_size == after.st_size && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
           before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
           before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
           before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
}

[[nodiscard]] bool write_all(int fd, std::span<const std::byte> bytes, int& native_error) noexcept {
    while (!bytes.empty()) {
        ssize_t count = -1;
        do {
            count = ::write(fd, bytes.data(), bytes.size());
        } while (count < 0 && errno == EINTR);
        if (count <= 0) {
            native_error = count < 0 ? errno : EIO;
            return false;
        }
        bytes = bytes.subspan(static_cast<std::size_t>(count));
    }
    return true;
}

[[nodiscard]] std::optional<Sha256Digest> hash_exact_fd(int fd, std::uint64_t size,
                                                        int& native_error) noexcept {
    Sha256Accumulator accumulator;
    std::array<std::byte, 64 * 1024> buffer{};
    std::uint64_t offset = 0;
    while (offset < size) {
        const std::size_t requested =
            static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - offset));
        ssize_t count = -1;
        do {
            count = ::pread(fd, buffer.data(), requested, static_cast<off_t>(offset));
        } while (count < 0 && errno == EINTR);
        if (count <= 0) {
            native_error = count < 0 ? errno : EIO;
            return std::nullopt;
        }
        const std::size_t amount = static_cast<std::size_t>(count);
        if (!accumulator.update(std::span<const std::byte>(buffer.data(), amount))) {
            native_error = EOVERFLOW;
            return std::nullopt;
        }
        offset += static_cast<std::uint64_t>(amount);
    }
    std::byte extra{};
    ssize_t extra_count = -1;
    do {
        extra_count = ::pread(fd, &extra, 1, static_cast<off_t>(size));
    } while (extra_count < 0 && errno == EINTR);
    if (extra_count != 0) {
        native_error = extra_count < 0 ? errno : EFBIG;
        return std::nullopt;
    }
    return accumulator.finalize();
}

[[nodiscard]] int create_executable_memfd() noexcept {
#if defined(SYS_memfd_create)
    constexpr unsigned int mfd_cloexec = 0x0001U;
    constexpr unsigned int mfd_allow_sealing = 0x0002U;
    constexpr unsigned int mfd_exec = 0x0010U;
    return static_cast<int>(
        ::syscall(SYS_memfd_create, "gnfs-siqs-probe", mfd_cloexec | mfd_allow_sealing | mfd_exec));
#else
    errno = ENOSYS;
    return -1;
#endif
}

#endif

} // namespace

AuthenticatedExecutableImage::AuthenticatedExecutableImage(
    std::intptr_t native_handle, std::filesystem::path executable) noexcept
    : native_handle_(native_handle), executable_(std::move(executable)) {}

AuthenticatedExecutableImage::~AuthenticatedExecutableImage() {
#if defined(__linux__)
    if (native_handle_ >= 0) {
        (void)::close(static_cast<int>(native_handle_));
    }
#endif
    native_handle_ = -1;
}

AuthenticatedExecutableImage::AuthenticatedExecutableImage(
    AuthenticatedExecutableImage&& other) noexcept
    : native_handle_(std::exchange(other.native_handle_, -1)),
      executable_(std::move(other.executable_)) {}

AuthenticatedExecutableImage&
AuthenticatedExecutableImage::operator=(AuthenticatedExecutableImage&& other) noexcept {
    if (this != &other) {
#if defined(__linux__)
        if (native_handle_ >= 0) {
            (void)::close(static_cast<int>(native_handle_));
        }
#endif
        native_handle_ = std::exchange(other.native_handle_, -1);
        executable_ = std::move(other.executable_);
    }
    return *this;
}

bool AuthenticatedExecutableImage::active() const noexcept {
    return native_handle_ >= 0 && !executable_.empty();
}

ExecutableImageAuthenticationResult
authenticate_executable_image(const std::filesystem::path& executable,
                              const Sha256Digest& expected_sha256,
                              std::uint64_t expected_owner) noexcept {
    try {
        if (executable.empty() || !executable.is_absolute() ||
            executable.native().find('\0') != std::string::npos ||
            !digest_is_nonzero(expected_sha256)) {
            return authentication_failure(ExecutableImageAuthenticationError::invalid_spec);
        }

#if !defined(__linux__)
        (void)expected_owner;
        return authentication_failure(ExecutableImageAuthenticationError::platform_unavailable);
#else
#if !defined(__GLIBC__) || !defined(__GLIBC_PREREQ)
        (void)expected_owner;
        return authentication_failure(ExecutableImageAuthenticationError::platform_unavailable);
#elif !__GLIBC_PREREQ(2, 34) || !defined(SYS_execveat) || !defined(SYS_close_range)
        (void)expected_owner;
        return authentication_failure(ExecutableImageAuthenticationError::platform_unavailable);
#else
        constexpr unsigned int close_range_cloexec = 1U << 2U;
        if (::syscall(SYS_close_range, std::numeric_limits<unsigned int>::max(),
                      std::numeric_limits<unsigned int>::max(), close_range_cloexec) != 0) {
            return authentication_failure(ExecutableImageAuthenticationError::platform_unavailable,
                                          errno);
        }
        if (expected_owner != static_cast<std::uint64_t>(::geteuid()) ||
            expected_owner > static_cast<std::uint64_t>(std::numeric_limits<uid_t>::max())) {
            return authentication_failure(ExecutableImageAuthenticationError::trust_invalid);
        }

        int source_fd = -1;
        do {
            source_fd = ::open(executable.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        } while (source_fd < 0 && errno == EINTR);
        if (source_fd < 0) {
            return authentication_failure(ExecutableImageAuthenticationError::open_failed, errno);
        }
        UniqueFd source(source_fd);
        int native_error = 0;
        if (!move_above_standard_streams(source, native_error)) {
            return authentication_failure(ExecutableImageAuthenticationError::open_failed,
                                          native_error);
        }

        struct stat source_before{};
        if (::fstat(source.get(), &source_before) != 0) {
            return authentication_failure(ExecutableImageAuthenticationError::metadata_failed,
                                          errno);
        }
        if (!metadata_is_trusted(source_before, expected_owner)) {
            return authentication_failure(ExecutableImageAuthenticationError::trust_invalid);
        }
        const std::uint64_t size = static_cast<std::uint64_t>(source_before.st_size);

        const int raw_snapshot_fd = create_executable_memfd();
        if (raw_snapshot_fd < 0) {
            const int memfd_error = errno;
            const auto error = memfd_error == ENOSYS || memfd_error == EINVAL ||
                                       memfd_error == EACCES || memfd_error == EPERM
                                   ? ExecutableImageAuthenticationError::platform_unavailable
                                   : ExecutableImageAuthenticationError::snapshot_failed;
            return authentication_failure(error, memfd_error);
        }
        UniqueFd snapshot(raw_snapshot_fd);
        if (!move_above_standard_streams(snapshot, native_error)) {
            return authentication_failure(ExecutableImageAuthenticationError::snapshot_failed,
                                          native_error);
        }

        Sha256Accumulator source_hash;
        std::array<std::byte, 64 * 1024> buffer{};
        std::uint64_t offset = 0;
        while (offset < size) {
            const std::size_t requested =
                static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - offset));
            ssize_t count = -1;
            do {
                count = ::pread(source.get(), buffer.data(), requested, static_cast<off_t>(offset));
            } while (count < 0 && errno == EINTR);
            if (count <= 0) {
                return authentication_failure(ExecutableImageAuthenticationError::read_failed,
                                              count < 0 ? errno : EIO);
            }
            const std::size_t amount = static_cast<std::size_t>(count);
            const std::span<const std::byte> bytes(buffer.data(), amount);
            if (!source_hash.update(bytes)) {
                return authentication_failure(ExecutableImageAuthenticationError::resource_failure);
            }
            if (!write_all(snapshot.get(), bytes, native_error)) {
                return authentication_failure(ExecutableImageAuthenticationError::snapshot_failed,
                                              native_error);
            }
            offset += static_cast<std::uint64_t>(amount);
        }
        std::byte source_extra{};
        ssize_t source_extra_count = -1;
        do {
            source_extra_count = ::pread(source.get(), &source_extra, 1, static_cast<off_t>(size));
        } while (source_extra_count < 0 && errno == EINTR);
        if (source_extra_count != 0) {
            return authentication_failure(ExecutableImageAuthenticationError::snapshot_failed,
                                          source_extra_count < 0 ? errno : EFBIG);
        }

        struct stat source_after{};
        if (::fstat(source.get(), &source_after) != 0) {
            return authentication_failure(ExecutableImageAuthenticationError::metadata_failed,
                                          errno);
        }
        if (!same_source_snapshot(source_before, source_after)) {
            return authentication_failure(ExecutableImageAuthenticationError::snapshot_failed);
        }
        const auto copied_digest = source_hash.finalize();
        if (!copied_digest.has_value() || *copied_digest != expected_sha256) {
            return authentication_failure(ExecutableImageAuthenticationError::identity_mismatch);
        }

        constexpr std::array<std::byte, 4> elf_magic{std::byte{0x7f}, std::byte{'E'},
                                                     std::byte{'L'}, std::byte{'F'}};
        std::array<std::byte, elf_magic.size()> observed_magic{};
        ssize_t magic_count = -1;
        do {
            magic_count = ::pread(snapshot.get(), observed_magic.data(), observed_magic.size(), 0);
        } while (magic_count < 0 && errno == EINTR);
        if (magic_count != static_cast<ssize_t>(observed_magic.size())) {
            return authentication_failure(ExecutableImageAuthenticationError::snapshot_failed,
                                          magic_count < 0 ? errno : ENOEXEC);
        }
        if (observed_magic != elf_magic) {
            return authentication_failure(ExecutableImageAuthenticationError::trust_invalid,
                                          ENOEXEC);
        }

        if (::fchmod(snapshot.get(), S_IRUSR | S_IXUSR) != 0) {
            return authentication_failure(ExecutableImageAuthenticationError::seal_failed, errno);
        }
        constexpr int seal_future_write = 0x0010;
        constexpr int seal_exec = 0x0020;
        constexpr int required_seals = F_SEAL_WRITE | seal_future_write | F_SEAL_GROW |
                                       F_SEAL_SHRINK | seal_exec | F_SEAL_SEAL;
        if (::fcntl(snapshot.get(), F_ADD_SEALS, required_seals) != 0) {
            const int seal_error = errno;
            const auto error = seal_error == EINVAL || seal_error == ENOTSUP
                                   ? ExecutableImageAuthenticationError::platform_unavailable
                                   : ExecutableImageAuthenticationError::seal_failed;
            return authentication_failure(error, seal_error);
        }
        const int observed_seals = ::fcntl(snapshot.get(), F_GET_SEALS);
        if (observed_seals < 0 || (observed_seals & required_seals) != required_seals) {
            return authentication_failure(ExecutableImageAuthenticationError::seal_failed,
                                          observed_seals < 0 ? errno : EIO);
        }

        struct stat snapshot_metadata{};
        if (::fstat(snapshot.get(), &snapshot_metadata) != 0) {
            return authentication_failure(ExecutableImageAuthenticationError::metadata_failed,
                                          errno);
        }
        if (!S_ISREG(snapshot_metadata.st_mode) ||
            snapshot_metadata.st_size != source_before.st_size ||
            (snapshot_metadata.st_mode & (S_IRWXG | S_IRWXO | S_IWUSR)) != 0 ||
            (snapshot_metadata.st_mode & (S_IRUSR | S_IXUSR)) != (S_IRUSR | S_IXUSR)) {
            return authentication_failure(ExecutableImageAuthenticationError::seal_failed);
        }
        const auto sealed_digest = hash_exact_fd(snapshot.get(), size, native_error);
        if (!sealed_digest.has_value()) {
            return authentication_failure(ExecutableImageAuthenticationError::read_failed,
                                          native_error);
        }
        if (*sealed_digest != expected_sha256) {
            return authentication_failure(ExecutableImageAuthenticationError::identity_mismatch);
        }

        ExecutableImageAuthenticationResult result;
        result.image =
            AuthenticatedExecutableImage(snapshot.release(), std::filesystem::path(executable));
        result.diagnostic.error = ExecutableImageAuthenticationError::none;
        return result;
#endif
#endif
    } catch (const std::bad_alloc&) {
        return authentication_failure(ExecutableImageAuthenticationError::resource_failure);
    } catch (...) {
        return authentication_failure(ExecutableImageAuthenticationError::unexpected_failure);
    }
}

BoundedChildProcessResult
run_authenticated_bounded_child_process(AuthenticatedExecutableImage&& image,
                                        const BoundedChildProcessSpec& spec,
                                        std::string_view argv0) noexcept {
    try {
        const std::intptr_t native_handle = std::exchange(image.native_handle_, -1);
        const std::filesystem::path authenticated_path = std::move(image.executable_);
#if defined(__linux__)
        UniqueFd consumed(static_cast<int>(native_handle));
        if (native_handle < 3 || authenticated_path.empty() ||
            authenticated_path.native() != spec.executable.native() || argv0.empty() ||
            argv0.find('\0') != std::string_view::npos) {
            return invalid_authenticated_run();
        }
        return detail::run_bounded_child_process_from_executable_fd(spec, consumed.get(), argv0);
#else
        (void)native_handle;
        (void)authenticated_path;
        (void)spec;
        (void)argv0;
        return invalid_authenticated_run();
#endif
    } catch (...) {
        BoundedChildProcessResult result;
        result.error = BoundedChildProcessError::unexpected_failure;
        return result;
    }
}

} // namespace gnfs::util

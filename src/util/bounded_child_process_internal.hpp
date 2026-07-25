#pragma once

#include <gnfs/util/bounded_child_process.hpp>
#include <gnfs/util/sha256.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>

namespace gnfs::util {

inline constexpr std::uint64_t AUTHENTICATED_EXECUTABLE_IMAGE_MAX_BYTES =
    UINT64_C(256) * UINT64_C(1024) * UINT64_C(1024);

enum class ExecutableImageAuthenticationError : std::uint8_t {
    none,
    platform_unavailable,
    invalid_spec,
    open_failed,
    metadata_failed,
    trust_invalid,
    read_failed,
    snapshot_failed,
    seal_failed,
    identity_mismatch,
    resource_failure,
    unexpected_failure,
};

[[nodiscard]] constexpr std::string_view
executable_image_authentication_error_name(ExecutableImageAuthenticationError error) noexcept {
    switch (error) {
    case ExecutableImageAuthenticationError::none:
        return "none";
    case ExecutableImageAuthenticationError::platform_unavailable:
        return "platform_unavailable";
    case ExecutableImageAuthenticationError::invalid_spec:
        return "invalid_spec";
    case ExecutableImageAuthenticationError::open_failed:
        return "open_failed";
    case ExecutableImageAuthenticationError::metadata_failed:
        return "metadata_failed";
    case ExecutableImageAuthenticationError::trust_invalid:
        return "trust_invalid";
    case ExecutableImageAuthenticationError::read_failed:
        return "read_failed";
    case ExecutableImageAuthenticationError::snapshot_failed:
        return "snapshot_failed";
    case ExecutableImageAuthenticationError::seal_failed:
        return "seal_failed";
    case ExecutableImageAuthenticationError::identity_mismatch:
        return "identity_mismatch";
    case ExecutableImageAuthenticationError::resource_failure:
        return "resource_failure";
    case ExecutableImageAuthenticationError::unexpected_failure:
        return "unexpected_failure";
    }
    return "unknown";
}

struct ExecutableImageAuthenticationDiagnostic final {
    ExecutableImageAuthenticationError error =
        ExecutableImageAuthenticationError::unexpected_failure;
    std::error_code native_error;
};

struct ExecutableImageAuthenticationResult;

class AuthenticatedExecutableImage final {
public:
    AuthenticatedExecutableImage() = delete;
    ~AuthenticatedExecutableImage();

    AuthenticatedExecutableImage(const AuthenticatedExecutableImage&) = delete;
    AuthenticatedExecutableImage& operator=(const AuthenticatedExecutableImage&) = delete;

    AuthenticatedExecutableImage(AuthenticatedExecutableImage&& other) noexcept;
    AuthenticatedExecutableImage& operator=(AuthenticatedExecutableImage&&) = delete;

    [[nodiscard]] bool active() const noexcept;

private:
    AuthenticatedExecutableImage(std::intptr_t native_handle,
                                 std::filesystem::path executable) noexcept;

    std::intptr_t native_handle_ = -1;
    std::filesystem::path executable_;

    friend struct ExecutableImageAuthenticationResult;
    friend ExecutableImageAuthenticationResult
    authenticate_executable_image(const std::filesystem::path&, const Sha256Digest&,
                                  std::uint64_t) noexcept;
    friend BoundedChildProcessResult run_authenticated_bounded_child_process(
        AuthenticatedExecutableImage&&, const BoundedChildProcessSpec&, std::string_view) noexcept;
};

struct ExecutableImageAuthenticationResult final {
    std::optional<AuthenticatedExecutableImage> image;
    ExecutableImageAuthenticationDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return image.has_value() && image->active() &&
               diagnostic.error == ExecutableImageAuthenticationError::none;
    }
};

/// Create a one-shot, same-byte executable capability. A compile-capable
/// modern-glibc Linux target copies the approved native ELF into a sealed
/// executable memfd and hashes the sealed object. Unsupported libcs and other
/// platforms fail closed before opening the supplied path.
[[nodiscard]] ExecutableImageAuthenticationResult
authenticate_executable_image(const std::filesystem::path& executable,
                              const Sha256Digest& expected_sha256,
                              std::uint64_t expected_owner) noexcept;

/// Consume `image` exactly once and execute its held object. `argv0` is an
/// explicit, already-approved logical name; the filesystem locator in
/// `spec.executable` is never reopened by the authenticated launcher.
[[nodiscard]] BoundedChildProcessResult
run_authenticated_bounded_child_process(AuthenticatedExecutableImage&& image,
                                        const BoundedChildProcessSpec& spec,
                                        std::string_view argv0) noexcept;

#if !defined(_WIN32)

namespace detail {

/// POSIX source-private path transport with an explicit logical argv[0].
/// `spec.executable` remains the path passed to posix_spawn, but is not exposed
/// to the child through argv[0].
[[nodiscard]] BoundedChildProcessResult
run_bounded_child_process_with_argv0(const BoundedChildProcessSpec& spec,
                                     std::string_view argv0) noexcept;

#if defined(__linux__)
/// Linux-only source-private transport entry point. On a compile-capable
/// modern-glibc target, `executable_fd` must name an executable object held by
/// the caller for the duration of the call. The child executes that descriptor
/// with execveat(AT_EMPTY_PATH). Before any other child setup it arms SIGKILL
/// on exit of the launching parent thread and separately verifies the
/// process-level fork-to-prctl parent race. The caller must keep that
/// synchronous launch thread alive. This contains only the direct, no-fork
/// probe process. The approved logical `argv0` is explicit, and
/// `spec.executable` is never reopened. Unsupported Linux libc/header
/// combinations return `platform_unavailable` before spawn setup.
[[nodiscard]] BoundedChildProcessResult
run_bounded_child_process_from_executable_fd(const BoundedChildProcessSpec& spec, int executable_fd,
                                             std::string_view argv0) noexcept;
#endif

} // namespace detail

#endif

} // namespace gnfs::util

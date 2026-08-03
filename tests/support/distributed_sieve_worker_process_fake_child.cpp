#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

extern char** environ;
#endif

namespace {

inline constexpr std::uint32_t BOOTSTRAP_MAGIC = 0x47574253U;
inline constexpr std::uint32_t REPORT_MAGIC = 0x47575250U;
inline constexpr std::uint16_t PROTOCOL_VERSION = 1U;
inline constexpr std::uint64_t ARGUMENT_DIGEST_OFFSET = UINT64_C(14695981039346656037);
inline constexpr std::uint64_t ARGUMENT_DIGEST_PRIME = UINT64_C(1099511628211);
inline constexpr std::size_t FIXED_CAPABILITY_COUNT = 4U;

enum class FakeChildMode : std::uint16_t {
    report_and_exit = 1U,
    stop_then_exit = 2U,
    exit_without_report = 3U,
    terminate = 4U,
};

enum class FakeCapabilityKind : std::uint32_t {
    none = 0U,
    directory = 1U,
    regular_file = 2U,
};

struct FakeCapabilityExpectation final {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t link_count = 0;
    FakeCapabilityKind kind = FakeCapabilityKind::none;
    std::uint32_t permission_bits = 0;
    std::int32_t access_mode = -1;
};

struct FakeCapabilityObservation final {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t link_count = 0;
    std::uint32_t mode = 0;
    std::uint32_t permission_bits = 0;
    std::int32_t access_mode = -1;
    std::int32_t descriptor_flags = -1;
    std::int32_t native_error = 0;
};

struct FakeBootstrap final {
    std::uint32_t magic = BOOTSTRAP_MAGIC;
    std::uint16_t version = PROTOCOL_VERSION;
    FakeChildMode mode = FakeChildMode::report_and_exit;
    std::uint32_t slot = 0;
    std::int32_t exit_code = 0;
    std::int32_t expected_closed_descriptor = -1;
    std::uint32_t payload_tag = 0;
    std::uint32_t expected_argument_count = 0;
    std::uint64_t expected_argument_digest = ARGUMENT_DIGEST_OFFSET;
    std::uint32_t expected_capability_count = 0;
    std::int32_t first_unmapped_descriptor = -1;
    std::int32_t descriptor_scan_limit = 0;
    std::array<FakeCapabilityExpectation, FIXED_CAPABILITY_COUNT> expected_capabilities{};
};

struct FakeReport final {
    std::uint32_t magic = REPORT_MAGIC;
    std::uint32_t slot = 0;
    std::int64_t process_id = -1;
    std::int64_t parent_process_id = -1;
    std::uint32_t flags = 0;
    std::uint32_t payload_tag = 0;
    std::uint32_t argument_count = 0;
    std::uint64_t argument_digest = ARGUMENT_DIGEST_OFFSET;
    std::array<FakeCapabilityObservation, FIXED_CAPABILITY_COUNT> observed_capabilities{};
    std::int32_t first_unexpected_open_descriptor = -1;
};

inline constexpr std::uint32_t INPUT_EXACT = 1U << 0U;
inline constexpr std::uint32_t INPUT_EOF = 1U << 1U;
inline constexpr std::uint32_t STDIN_OPEN = 1U << 2U;
inline constexpr std::uint32_t STDOUT_OPEN = 1U << 3U;
inline constexpr std::uint32_t STDERR_OPEN = 1U << 4U;
inline constexpr std::uint32_t EXPECTED_DESCRIPTOR_CLOSED = 1U << 5U;
inline constexpr std::uint32_t EMPTY_ENVIRONMENT = 1U << 6U;
inline constexpr std::uint32_t ARGUMENT_COUNT_MATCHES = 1U << 7U;
inline constexpr std::uint32_t PROCESS_GROUP_ISOLATED = 1U << 8U;
inline constexpr std::uint32_t EMPTY_SIGNAL_MASK = 1U << 9U;
inline constexpr std::uint32_t DEFAULT_SIGNALS = 1U << 10U;
inline constexpr std::uint32_t ARGUMENT_DIGEST_MATCHES = 1U << 11U;
inline constexpr std::uint32_t CAPABILITY_0_MATCHES = 1U << 12U;
inline constexpr std::uint32_t CAPABILITY_1_MATCHES = 1U << 13U;
inline constexpr std::uint32_t CAPABILITY_2_MATCHES = 1U << 14U;
inline constexpr std::uint32_t CAPABILITY_3_MATCHES = 1U << 15U;
inline constexpr std::uint32_t FIXED_CAPABILITIES_NOT_CLOEXEC = 1U << 16U;
inline constexpr std::uint32_t NO_UNMAPPED_DESCRIPTOR_LEAK = 1U << 17U;
inline constexpr std::uint32_t PERMANENT_WAVE_STORE_LOCK_SAME_OFD = 1U << 18U;
inline constexpr std::uint32_t ATTEMPT_BASE_LOCK_SAME_OFD = 1U << 19U;

static_assert(std::is_trivially_copyable_v<FakeBootstrap>);
static_assert(std::is_trivially_copyable_v<FakeReport>);

#if !defined(_WIN32)
[[nodiscard]] bool read_exact(int descriptor, void* destination, std::size_t size) noexcept {
    auto* bytes = static_cast<std::byte*>(destination);
    std::size_t consumed = 0;
    while (consumed < size) {
        ssize_t received = -1;
        do {
            received = ::read(descriptor, bytes + consumed, size - consumed);
        } while (received < 0 && errno == EINTR);
        if (received <= 0) {
            return false;
        }
        consumed += static_cast<std::size_t>(received);
    }
    return true;
}

[[nodiscard]] bool observe_eof(int descriptor) noexcept {
    std::byte byte{};
    ssize_t received = -1;
    do {
        received = ::read(descriptor, &byte, sizeof(byte));
    } while (received < 0 && errno == EINTR);
    return received == 0;
}

[[nodiscard]] bool write_exact(int descriptor, const void* source, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::byte*>(source);
    std::size_t produced = 0;
    while (produced < size) {
        ssize_t written = -1;
        do {
            written = ::write(descriptor, bytes + produced, size - produced);
        } while (written < 0 && errno == EINTR);
        if (written <= 0) {
            return false;
        }
        produced += static_cast<std::size_t>(written);
    }
    return true;
}

[[nodiscard]] bool descriptor_is_open(int descriptor) noexcept {
    int result = -1;
    do {
        result = ::fcntl(descriptor, F_GETFD);
    } while (result < 0 && errno == EINTR);
    return result >= 0;
}

[[nodiscard]] bool descriptor_is_closed(int descriptor) noexcept {
    if (descriptor < 0) {
        return true;
    }
    errno = 0;
    const int result = ::fcntl(descriptor, F_GETFD);
    return result < 0 && errno == EBADF;
}

[[nodiscard]] FakeCapabilityKind object_kind(std::uint32_t mode) noexcept {
    if (S_ISDIR(static_cast<mode_t>(mode))) {
        return FakeCapabilityKind::directory;
    }
    if (S_ISREG(static_cast<mode_t>(mode))) {
        return FakeCapabilityKind::regular_file;
    }
    return FakeCapabilityKind::none;
}

[[nodiscard]] constexpr std::uint32_t file_permission_bits(mode_t mode) noexcept {
    return static_cast<std::uint32_t>(mode) & 0777U;
}

[[nodiscard]] FakeCapabilityObservation observe_capability(int descriptor) noexcept {
    FakeCapabilityObservation observation;
    struct stat metadata {};
    if (::fstat(descriptor, &metadata) != 0) {
        observation.native_error = errno;
        return observation;
    }

    int status_flags = -1;
    do {
        status_flags = ::fcntl(descriptor, F_GETFL);
    } while (status_flags < 0 && errno == EINTR);
    if (status_flags < 0) {
        observation.native_error = errno;
        return observation;
    }

    int descriptor_flags = -1;
    do {
        descriptor_flags = ::fcntl(descriptor, F_GETFD);
    } while (descriptor_flags < 0 && errno == EINTR);
    if (descriptor_flags < 0) {
        observation.native_error = errno;
        return observation;
    }

    observation.device = static_cast<std::uint64_t>(metadata.st_dev);
    observation.inode = static_cast<std::uint64_t>(metadata.st_ino);
    observation.link_count = static_cast<std::uint64_t>(metadata.st_nlink);
    observation.mode = static_cast<std::uint32_t>(metadata.st_mode);
    observation.permission_bits = file_permission_bits(metadata.st_mode);
    observation.access_mode = status_flags & O_ACCMODE;
    observation.descriptor_flags = descriptor_flags;
    return observation;
}

[[nodiscard]] bool capability_matches(const FakeCapabilityObservation& observed,
                                      const FakeCapabilityExpectation& expected) noexcept {
    return observed.native_error == 0 && observed.device == expected.device &&
           observed.inode == expected.inode && observed.link_count == expected.link_count &&
           object_kind(observed.mode) == expected.kind &&
           observed.permission_bits == expected.permission_bits &&
           observed.access_mode == expected.access_mode;
}

[[nodiscard]] bool retains_exclusive_lock(int descriptor) noexcept {
    int result = -1;
    do {
        result = ::flock(descriptor, LOCK_EX | LOCK_NB);
    } while (result < 0 && errno == EINTR);
    return result == 0;
}

[[nodiscard]] std::int32_t first_open_descriptor(std::int32_t first_descriptor,
                                                 std::int32_t scan_limit) noexcept {
    if (first_descriptor < 0 || scan_limit <= first_descriptor) {
        return -1;
    }
    for (int descriptor = first_descriptor; descriptor < scan_limit; ++descriptor) {
        if (descriptor_is_open(descriptor)) {
            return descriptor;
        }
    }
    return -1;
}

[[nodiscard]] bool signal_mask_is_empty() noexcept {
    sigset_t mask;
    if (::sigprocmask(SIG_SETMASK, nullptr, &mask) != 0) {
        return false;
    }
    for (int signal_number = 1; signal_number < NSIG; ++signal_number) {
        const int member = sigismember(&mask, signal_number);
        if (member == 1) {
            return false;
        }
        if (member < 0 && errno != EINVAL) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool selected_signals_are_default() noexcept {
    struct sigaction user_action {};
    struct sigaction pipe_action {};
    return ::sigaction(SIGUSR1, nullptr, &user_action) == 0 &&
           ::sigaction(SIGPIPE, nullptr, &pipe_action) == 0 && user_action.sa_handler == SIG_DFL &&
           pipe_action.sa_handler == SIG_DFL;
}

void update_argument_digest(std::uint64_t& digest, std::uint8_t byte) noexcept {
    digest ^= byte;
    digest *= ARGUMENT_DIGEST_PRIME;
}

[[nodiscard]] std::uint64_t argument_digest(int argc, char* argv[]) noexcept {
    std::uint64_t digest = ARGUMENT_DIGEST_OFFSET;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const auto size = static_cast<std::uint64_t>(argument.size());
        for (unsigned shift = 0; shift < 64U; shift += 8U) {
            update_argument_digest(digest,
                                   static_cast<std::uint8_t>((size >> shift) & UINT64_C(0xff)));
        }
        for (const char byte : argument) {
            update_argument_digest(digest,
                                   static_cast<std::uint8_t>(static_cast<unsigned char>(byte)));
        }
    }
    return digest;
}
#endif

} // namespace

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    (void)argc;
    (void)argv;
    return 77;
#else
    FakeBootstrap bootstrap;
    if (!read_exact(STDIN_FILENO, &bootstrap, sizeof(bootstrap)) ||
        bootstrap.magic != BOOTSTRAP_MAGIC || bootstrap.version != PROTOCOL_VERSION) {
        ::_exit(201);
    }

    FakeReport report{
        .magic = REPORT_MAGIC,
        .slot = bootstrap.slot,
        .process_id = static_cast<std::int64_t>(::getpid()),
        .parent_process_id = static_cast<std::int64_t>(::getppid()),
        .flags = INPUT_EXACT,
        .payload_tag = bootstrap.payload_tag,
        .argument_count = static_cast<std::uint32_t>(argc > 0 ? argc - 1 : 0),
        .argument_digest = argument_digest(argc, argv),
    };
    if (observe_eof(STDIN_FILENO)) {
        report.flags |= INPUT_EOF;
    }
    if (descriptor_is_open(STDIN_FILENO)) {
        report.flags |= STDIN_OPEN;
    }
    if (descriptor_is_open(STDOUT_FILENO)) {
        report.flags |= STDOUT_OPEN;
    }
    if (descriptor_is_open(STDERR_FILENO)) {
        report.flags |= STDERR_OPEN;
    }
    if (descriptor_is_closed(bootstrap.expected_closed_descriptor)) {
        report.flags |= EXPECTED_DESCRIPTOR_CLOSED;
    }
    if (environ != nullptr && environ[0] == nullptr) {
        report.flags |= EMPTY_ENVIRONMENT;
    }
    if (report.argument_count == bootstrap.expected_argument_count) {
        report.flags |= ARGUMENT_COUNT_MATCHES;
    }
    if (report.argument_digest == bootstrap.expected_argument_digest) {
        report.flags |= ARGUMENT_DIGEST_MATCHES;
    }
    if (::getpgrp() == ::getpid()) {
        report.flags |= PROCESS_GROUP_ISOLATED;
    }
    if (signal_mask_is_empty()) {
        report.flags |= EMPTY_SIGNAL_MASK;
    }
    if (selected_signals_are_default()) {
        report.flags |= DEFAULT_SIGNALS;
    }
    if (bootstrap.expected_capability_count == FIXED_CAPABILITY_COUNT) {
        constexpr std::array<int, FIXED_CAPABILITY_COUNT> descriptors{3, 4, 5, 6};
        constexpr std::array<std::uint32_t, FIXED_CAPABILITY_COUNT> match_flags{
            CAPABILITY_0_MATCHES,
            CAPABILITY_1_MATCHES,
            CAPABILITY_2_MATCHES,
            CAPABILITY_3_MATCHES,
        };
        bool all_not_cloexec = true;
        for (std::size_t index = 0; index < FIXED_CAPABILITY_COUNT; ++index) {
            report.observed_capabilities[index] = observe_capability(descriptors[index]);
            if (capability_matches(report.observed_capabilities[index],
                                   bootstrap.expected_capabilities[index])) {
                report.flags |= match_flags[index];
            }
            all_not_cloexec =
                all_not_cloexec && report.observed_capabilities[index].native_error == 0 &&
                (report.observed_capabilities[index].descriptor_flags & FD_CLOEXEC) == 0;
        }
        if (all_not_cloexec) {
            report.flags |= FIXED_CAPABILITIES_NOT_CLOEXEC;
        }
        if (retains_exclusive_lock(descriptors[1])) {
            report.flags |= PERMANENT_WAVE_STORE_LOCK_SAME_OFD;
        }
        if (retains_exclusive_lock(descriptors[2])) {
            report.flags |= ATTEMPT_BASE_LOCK_SAME_OFD;
        }
        report.first_unexpected_open_descriptor = first_open_descriptor(
            bootstrap.first_unmapped_descriptor, bootstrap.descriptor_scan_limit);
        if (report.first_unexpected_open_descriptor < 0) {
            report.flags |= NO_UNMAPPED_DESCRIPTOR_LEAK;
        }
    }

    const auto exit_code = static_cast<int>(bootstrap.exit_code) & 0xff;
    switch (bootstrap.mode) {
    case FakeChildMode::report_and_exit:
        if (!write_exact(STDOUT_FILENO, &report, sizeof(report))) {
            ::_exit(202);
        }
        ::_exit(exit_code);
    case FakeChildMode::stop_then_exit:
        if (!write_exact(STDOUT_FILENO, &report, sizeof(report))) {
            ::_exit(203);
        }
        if (::raise(SIGSTOP) != 0) {
            ::_exit(204);
        }
        ::_exit(exit_code);
    case FakeChildMode::exit_without_report:
        ::_exit(exit_code);
    case FakeChildMode::terminate:
        if (::signal(SIGTERM, SIG_DFL) == SIG_ERR || ::raise(SIGTERM) != 0) {
            ::_exit(205);
        }
        ::_exit(206);
    }
    ::_exit(207);
#endif
}

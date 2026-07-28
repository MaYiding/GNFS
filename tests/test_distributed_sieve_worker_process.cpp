#include "distributed_sieve_worker_process_internal.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <algorithm>
#include <chrono>
#include <climits>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace worker_process = gnfs::sieve::distributed_sieve_worker_process_detail;

static_assert(!std::is_default_constructible_v<worker_process::DistributedSieveWorkerProcess>);
static_assert(!std::is_copy_constructible_v<worker_process::DistributedSieveWorkerProcess>);
static_assert(!std::is_copy_assignable_v<worker_process::DistributedSieveWorkerProcess>);
static_assert(std::is_nothrow_move_constructible_v<worker_process::DistributedSieveWorkerProcess>);
static_assert(!std::is_move_assignable_v<worker_process::DistributedSieveWorkerProcess>);

static_assert(!std::is_default_constructible_v<worker_process::DistributedSieveWorkerProcessBatch>);
static_assert(!std::is_copy_constructible_v<worker_process::DistributedSieveWorkerProcessBatch>);
static_assert(!std::is_copy_assignable_v<worker_process::DistributedSieveWorkerProcessBatch>);
static_assert(
    std::is_nothrow_move_constructible_v<worker_process::DistributedSieveWorkerProcessBatch>);
static_assert(!std::is_move_assignable_v<worker_process::DistributedSieveWorkerProcessBatch>);

static_assert(worker_process::DISTRIBUTED_SIEVE_WORKER_CHILD_WAVE_ROOT_DESCRIPTOR == 3);
static_assert(worker_process::DISTRIBUTED_SIEVE_WORKER_CHILD_PERMANENT_WAVE_STORE_LOCK_DESCRIPTOR ==
              4);
static_assert(worker_process::DISTRIBUTED_SIEVE_WORKER_CHILD_ATTEMPT_BASE_LOCK_DESCRIPTOR == 5);
static_assert(worker_process::DISTRIBUTED_SIEVE_WORKER_CHILD_WORK_PACKAGE_READER_DESCRIPTOR == 6);
static_assert(worker_process::DISTRIBUTED_SIEVE_WORKER_CHILD_FIRST_UNMAPPED_DESCRIPTOR == 7);

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            throw std::runtime_error(std::string("CHECK failed: " #condition " at ") + __FILE__ +  \
                                     ":" + std::to_string(__LINE__));                              \
        }                                                                                          \
    } while (false)

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
inline constexpr std::uint32_t REQUIRED_CAPABILITY_FLAGS =
    CAPABILITY_0_MATCHES | CAPABILITY_1_MATCHES | CAPABILITY_2_MATCHES | CAPABILITY_3_MATCHES |
    FIXED_CAPABILITIES_NOT_CLOEXEC | NO_UNMAPPED_DESCRIPTOR_LEAK |
    PERMANENT_WAVE_STORE_LOCK_SAME_OFD | ATTEMPT_BASE_LOCK_SAME_OFD;
inline constexpr std::uint32_t REQUIRED_REPORT_FLAGS =
    INPUT_EXACT | INPUT_EOF | STDIN_OPEN | STDOUT_OPEN | STDERR_OPEN | EXPECTED_DESCRIPTOR_CLOSED |
    EMPTY_ENVIRONMENT | ARGUMENT_COUNT_MATCHES | PROCESS_GROUP_ISOLATED | EMPTY_SIGNAL_MASK |
    DEFAULT_SIGNALS | ARGUMENT_DIGEST_MATCHES;

static_assert(std::is_trivially_copyable_v<FakeBootstrap>);
static_assert(std::is_trivially_copyable_v<FakeReport>);
static_assert(sizeof(FakeBootstrap) <=
              worker_process::DISTRIBUTED_SIEVE_WORKER_BOOTSTRAP_FRAME_LIMIT);

template <typename T> [[nodiscard]] std::span<const std::byte> as_bytes(const T& value) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    return {reinterpret_cast<const std::byte*>(&value), sizeof(value)};
}

void update_argument_digest(std::uint64_t& digest, std::uint8_t byte) noexcept {
    digest ^= byte;
    digest *= ARGUMENT_DIGEST_PRIME;
}

[[nodiscard]] std::uint64_t argument_digest(std::span<const std::string_view> arguments) noexcept {
    std::uint64_t digest = ARGUMENT_DIGEST_OFFSET;
    for (const auto argument : arguments) {
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

void test_prepare_zero() {
    const std::span<const worker_process::DistributedSieveWorkerProcessSpawnSpec> no_children;
    auto prepared = worker_process::prepare_distributed_sieve_worker_process_batch(no_children);
    CHECK(!prepared);
    CHECK(!prepared.batch.has_value());
    CHECK(prepared.diagnostic.error ==
          worker_process::DistributedSieveWorkerProcessTransportError::invalid_request);
    CHECK(prepared.diagnostic.native_error == EINVAL);
}

#if defined(_WIN32)

void test_platform_unavailable() {
    CHECK(!worker_process::distributed_sieve_worker_process_fixed_capability_close_all_supported());
    const FakeBootstrap bootstrap;
    const worker_process::DistributedSieveWorkerProcessSpawnSpec spec{
        .executable_path = R"(C:\gnfs-fake-child.exe)",
        .arguments = {},
        .bootstrap_frame = as_bytes(bootstrap),
    };
    auto prepared =
        worker_process::prepare_distributed_sieve_worker_process_batch(std::span{&spec, 1U});
    CHECK(!prepared);
    CHECK(!prepared.batch.has_value());
    CHECK(prepared.diagnostic.error ==
          worker_process::DistributedSieveWorkerProcessTransportError::platform_unavailable);
    CHECK(prepared.diagnostic.native_error == ENOTSUP);
}

#else

using namespace std::chrono_literals;

inline constexpr auto IO_TIMEOUT = 3s;

class UniqueFd final {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int descriptor) noexcept : descriptor_(descriptor) {}
    ~UniqueFd() {
        reset();
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.descriptor_, -1));
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

    [[nodiscard]] int release() noexcept {
        return std::exchange(descriptor_, -1);
    }

    void reset(int descriptor = -1) noexcept {
        if (descriptor_ >= 0) {
            (void)::close(descriptor_);
        }
        descriptor_ = descriptor;
    }

private:
    int descriptor_ = -1;
};

[[nodiscard]] int duplicate_at_least(int descriptor, int minimum) {
    int duplicate = -1;
    do {
        duplicate = ::fcntl(descriptor, F_DUPFD_CLOEXEC, minimum);
    } while (duplicate < 0 && errno == EINTR);
    CHECK(duplicate >= minimum);
    return duplicate;
}

[[nodiscard]] int duplicate_non_cloexec_at_least(int descriptor, int minimum) {
    int duplicate = -1;
    do {
        duplicate = ::fcntl(descriptor, F_DUPFD, minimum);
    } while (duplicate < 0 && errno == EINTR);
    CHECK(duplicate >= minimum);
    int flags = -1;
    do {
        flags = ::fcntl(duplicate, F_GETFD);
    } while (flags < 0 && errno == EINTR);
    CHECK(flags >= 0);
    CHECK((flags & FD_CLOEXEC) == 0);
    return duplicate;
}

[[nodiscard]] constexpr std::uint32_t file_permission_bits(mode_t mode) noexcept {
    return static_cast<std::uint32_t>(mode) & 0777U;
}

[[nodiscard]] FakeCapabilityExpectation
capability_expectation(int descriptor, FakeCapabilityKind kind, int access_mode) {
    struct stat metadata{};
    CHECK(::fstat(descriptor, &metadata) == 0);
    return {
        .device = static_cast<std::uint64_t>(metadata.st_dev),
        .inode = static_cast<std::uint64_t>(metadata.st_ino),
        .link_count = static_cast<std::uint64_t>(metadata.st_nlink),
        .kind = kind,
        .permission_bits = file_permission_bits(metadata.st_mode),
        .access_mode = access_mode,
    };
}

class FixedDescriptorRestorer final {
public:
    FixedDescriptorRestorer() {
        for (std::size_t index = 0; index < TARGETS.size(); ++index) {
            const int target = TARGETS[index];
            int descriptor_flags = -1;
            do {
                descriptor_flags = ::fcntl(target, F_GETFD);
            } while (descriptor_flags < 0 && errno == EINTR);
            if (descriptor_flags < 0) {
                CHECK(errno == EBADF);
                continue;
            }
            original_descriptor_flags_[index] = descriptor_flags;
            backups_[index] = duplicate_at_least(target, BACKUP_DESCRIPTOR_MINIMUM);
        }
    }

    ~FixedDescriptorRestorer() {
        restore();
    }

    FixedDescriptorRestorer(const FixedDescriptorRestorer&) = delete;
    FixedDescriptorRestorer& operator=(const FixedDescriptorRestorer&) = delete;

    void install(int target, int source) {
        CHECK(target >= TARGETS.front());
        CHECK(target <= TARGETS.back());
        int result = -1;
        do {
            result = ::dup2(source, target);
        } while (result < 0 && errno == EINTR);
        CHECK(result == target);
    }

private:
    void restore() noexcept {
        for (std::size_t index = 0; index < TARGETS.size(); ++index) {
            const int target = TARGETS[index];
            if (backups_[index] < 0) {
                (void)::close(target);
                continue;
            }
            int result = -1;
            do {
                result = ::dup2(backups_[index], target);
            } while (result < 0 && errno == EINTR);
            if (result == target) {
                (void)::fcntl(target, F_SETFD, original_descriptor_flags_[index]);
            }
            (void)::close(std::exchange(backups_[index], -1));
        }
    }

    inline static constexpr std::array<int, FIXED_CAPABILITY_COUNT> TARGETS{3, 4, 5, 6};
    inline static constexpr int BACKUP_DESCRIPTOR_MINIMUM = 512;
    std::array<int, FIXED_CAPABILITY_COUNT> backups_{{-1, -1, -1, -1}};
    std::array<int, FIXED_CAPABILITY_COUNT> original_descriptor_flags_{{0, 0, 0, 0}};
};

class CapabilityFixture final {
public:
    explicit CapabilityFixture(std::uint32_t tag) {
        static std::uint64_t sequence = 0;
        for (unsigned attempt = 0; attempt < 100U; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    ("gnfs-worker-process-capabilities-" + std::to_string(::getpid()) + "-" +
                     std::to_string(++sequence) + "-" + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                break;
            }
            CHECK(!error || error == std::errc::file_exists);
        }
        CHECK(!path_.empty());
        CHECK(std::filesystem::is_directory(path_));
        CHECK(::chmod(path_.c_str(), 0700) == 0);

        UniqueFd root(open_path(path_, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0));
        root_.reset(duplicate_at_least(root.get(), SOURCE_DESCRIPTOR_MINIMUM));

        wave_lock_.reset(open_relative("wave.lock", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        lift_source(wave_lock_);
        lock_exclusively(wave_lock_);
        attempt_lock_.reset(
            open_relative("attempt.lock", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        lift_source(attempt_lock_);
        lock_exclusively(attempt_lock_);

        UniqueFd package_writer(
            open_relative("package.bin", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        const std::array<std::uint32_t, 4> payload{
            UINT32_C(0x4757504b),
            tag,
            tag ^ UINT32_C(0xa5a5a5a5),
            UINT32_C(0x01020304),
        };
        const auto* bytes = reinterpret_cast<const std::byte*>(payload.data());
        std::size_t written = 0;
        while (written < sizeof(payload)) {
            ssize_t count = -1;
            do {
                count = ::write(package_writer.get(), bytes + written, sizeof(payload) - written);
            } while (count < 0 && errno == EINTR);
            CHECK(count > 0);
            written += static_cast<std::size_t>(count);
        }
        CHECK(::fchmod(package_writer.get(), 0400) == 0);
        package_writer.reset();
        package_.reset(open_relative("package.bin", O_RDONLY | O_NOFOLLOW | O_CLOEXEC, 0));
        int unlink_result = -1;
        do {
            unlink_result = ::unlinkat(root_.get(), "package.bin", 0);
        } while (unlink_result < 0 && errno == EINTR);
        CHECK(unlink_result == 0);
        lift_source(package_);
        struct stat package_metadata{};
        CHECK(::fstat(package_.get(), &package_metadata) == 0);
        CHECK(S_ISREG(package_metadata.st_mode));
        CHECK(package_metadata.st_nlink == 0);
        CHECK(file_permission_bits(package_metadata.st_mode) == 0400U);
    }

    ~CapabilityFixture() {
        package_.reset();
        attempt_lock_.reset();
        wave_lock_.reset();
        root_.reset();
        std::error_code ignored;
        (void)std::filesystem::remove_all(path_, ignored);
    }

    CapabilityFixture(const CapabilityFixture&) = delete;
    CapabilityFixture& operator=(const CapabilityFixture&) = delete;

    [[nodiscard]] std::array<int, FIXED_CAPABILITY_COUNT> sources() const noexcept {
        return {root_.get(), wave_lock_.get(), attempt_lock_.get(), package_.get()};
    }

    [[nodiscard]] std::array<FakeCapabilityExpectation, FIXED_CAPABILITY_COUNT>
    expectations() const {
        return {
            capability_expectation(root_.get(), FakeCapabilityKind::directory, O_RDONLY),
            capability_expectation(wave_lock_.get(), FakeCapabilityKind::regular_file, O_RDWR),
            capability_expectation(attempt_lock_.get(), FakeCapabilityKind::regular_file, O_RDWR),
            capability_expectation(package_.get(), FakeCapabilityKind::regular_file, O_RDONLY),
        };
    }

private:
    [[nodiscard]] static int open_path(const std::filesystem::path& path, int flags, mode_t mode) {
        int descriptor = -1;
        do {
            descriptor = ::open(path.c_str(), flags, mode);
        } while (descriptor < 0 && errno == EINTR);
        CHECK(descriptor >= 0);
        return descriptor;
    }

    [[nodiscard]] int open_relative(const char* leaf, int flags, mode_t mode) const {
        int descriptor = -1;
        do {
            descriptor = ::openat(root_.get(), leaf, flags, mode);
        } while (descriptor < 0 && errno == EINTR);
        CHECK(descriptor >= 0);
        return descriptor;
    }

    static void lift_source(UniqueFd& descriptor) {
        UniqueFd lifted(duplicate_at_least(descriptor.get(), SOURCE_DESCRIPTOR_MINIMUM));
        descriptor = std::move(lifted);
    }

    static void lock_exclusively(const UniqueFd& descriptor) {
        int result = -1;
        do {
            result = ::flock(descriptor.get(), LOCK_EX | LOCK_NB);
        } while (result < 0 && errno == EINTR);
        CHECK(result == 0);
    }

    inline static constexpr int SOURCE_DESCRIPTOR_MINIMUM = 64;
    std::filesystem::path path_;
    UniqueFd root_;
    UniqueFd wave_lock_;
    UniqueFd attempt_lock_;
    UniqueFd package_;
};

[[nodiscard]] worker_process::DistributedSieveWorkerProcessFixedCapabilitySourcesV1
fixed_capability_sources(const std::array<int, FIXED_CAPABILITY_COUNT>& descriptors) noexcept {
    return {
        .wave_root_directory_descriptor = descriptors[0],
        .permanent_wave_store_lock_descriptor = descriptors[1],
        .attempt_base_lock_descriptor = descriptors[2],
        .work_package_reader_descriptor = descriptors[3],
    };
}

[[nodiscard]] bool
descriptor_matches_expectation(int descriptor,
                               const FakeCapabilityExpectation& expectation) noexcept {
    struct stat metadata{};
    if (::fstat(descriptor, &metadata) != 0) {
        return false;
    }
    int flags = -1;
    do {
        flags = ::fcntl(descriptor, F_GETFL);
    } while (flags < 0 && errno == EINTR);
    if (flags < 0 || static_cast<std::uint64_t>(metadata.st_dev) != expectation.device ||
        static_cast<std::uint64_t>(metadata.st_ino) != expectation.inode ||
        static_cast<std::uint64_t>(metadata.st_nlink) != expectation.link_count ||
        file_permission_bits(metadata.st_mode) != expectation.permission_bits ||
        (flags & O_ACCMODE) != expectation.access_mode) {
        return false;
    }
    if (expectation.kind == FakeCapabilityKind::directory) {
        return S_ISDIR(metadata.st_mode);
    }
    if (expectation.kind == FakeCapabilityKind::regular_file) {
        return S_ISREG(metadata.st_mode);
    }
    return false;
}

[[nodiscard]] int descriptor_scan_limit() noexcept {
    long configured_limit = ::sysconf(_SC_OPEN_MAX);
    if (configured_limit < 0) {
        configured_limit = 1024;
    }
    return static_cast<int>(std::min<long>(configured_limit, static_cast<long>(4096)));
}

[[nodiscard]] FakeBootstrap capability_bootstrap(const CapabilityFixture& fixture,
                                                 std::uint32_t slot,
                                                 std::uint32_t payload_tag = 0) {
    FakeBootstrap bootstrap{
        .mode = FakeChildMode::report_and_exit,
        .slot = slot,
        .payload_tag = payload_tag,
    };
    bootstrap.expected_capability_count = FIXED_CAPABILITY_COUNT;
    bootstrap.first_unmapped_descriptor =
        worker_process::DISTRIBUTED_SIEVE_WORKER_CHILD_FIRST_UNMAPPED_DESCRIPTOR;
    bootstrap.descriptor_scan_limit = descriptor_scan_limit();
    bootstrap.expected_capabilities = fixture.expectations();
    return bootstrap;
}

struct DescriptorSnapshot final {
    bool open = false;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint32_t mode = 0;
    std::int32_t status_flags = -1;
    std::int32_t descriptor_flags = -1;

    bool operator==(const DescriptorSnapshot&) const = default;
};

[[nodiscard]] DescriptorSnapshot descriptor_snapshot(int descriptor) {
    int descriptor_flags = -1;
    do {
        descriptor_flags = ::fcntl(descriptor, F_GETFD);
    } while (descriptor_flags < 0 && errno == EINTR);
    if (descriptor_flags < 0) {
        CHECK(errno == EBADF);
        return {};
    }

    struct stat metadata{};
    CHECK(::fstat(descriptor, &metadata) == 0);
    int status_flags = -1;
    do {
        status_flags = ::fcntl(descriptor, F_GETFL);
    } while (status_flags < 0 && errno == EINTR);
    CHECK(status_flags >= 0);
    return {
        .open = true,
        .device = static_cast<std::uint64_t>(metadata.st_dev),
        .inode = static_cast<std::uint64_t>(metadata.st_ino),
        .mode = static_cast<std::uint32_t>(metadata.st_mode),
        .status_flags = status_flags,
        .descriptor_flags = descriptor_flags,
    };
}

[[nodiscard]] std::array<DescriptorSnapshot, 3> snapshot_standard_descriptors() {
    std::array<DescriptorSnapshot, 3> snapshots{};
    for (int descriptor = STDIN_FILENO; descriptor <= STDERR_FILENO; ++descriptor) {
        snapshots[static_cast<std::size_t>(descriptor)] = descriptor_snapshot(descriptor);
    }
    return snapshots;
}

class StandardDescriptorRestorer final {
public:
    StandardDescriptorRestorer() {
        for (int descriptor = STDIN_FILENO; descriptor <= STDERR_FILENO; ++descriptor) {
            const auto index = static_cast<std::size_t>(descriptor);
            int descriptor_flags = -1;
            do {
                descriptor_flags = ::fcntl(descriptor, F_GETFD);
            } while (descriptor_flags < 0 && errno == EINTR);
            if (descriptor_flags < 0) {
                if (errno != EBADF) {
                    throw std::runtime_error("cannot inspect standard descriptor " +
                                             std::to_string(descriptor));
                }
                continue;
            }

            int duplicate = -1;
            do {
                duplicate = ::fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            } while (duplicate < 0 && errno == EINTR);
            if (duplicate < 0) {
                throw std::runtime_error("cannot preserve standard descriptor " +
                                         std::to_string(descriptor));
            }
            backups_[index] = duplicate;
            originally_open_[index] = true;
            original_descriptor_flags_[index] = descriptor_flags;
        }
    }

    ~StandardDescriptorRestorer() {
        (void)restore();
        for (auto& backup : backups_) {
            if (backup >= 0) {
                (void)::close(std::exchange(backup, -1));
            }
        }
    }

    StandardDescriptorRestorer(const StandardDescriptorRestorer&) = delete;
    StandardDescriptorRestorer& operator=(const StandardDescriptorRestorer&) = delete;

    void close_all() noexcept {
        std::cout.flush();
        std::cerr.flush();
        for (int descriptor = STDIN_FILENO; descriptor <= STDERR_FILENO; ++descriptor) {
            (void)::close(descriptor);
        }
    }

    void close_standard_error() noexcept {
        std::cerr.flush();
        (void)::close(STDERR_FILENO);
    }

    [[nodiscard]] bool restore() noexcept {
        bool complete = true;
        for (int descriptor = STDIN_FILENO; descriptor <= STDERR_FILENO; ++descriptor) {
            const auto index = static_cast<std::size_t>(descriptor);
            if (restored_[index]) {
                continue;
            }
            if (!originally_open_[index]) {
                (void)::close(descriptor);
                restored_[index] = true;
                continue;
            }

            int result = -1;
            do {
                result = ::dup2(backups_[index], descriptor);
            } while (result < 0 && errno == EINTR);
            if (result < 0) {
                complete = false;
                continue;
            }
            int flags_result = -1;
            do {
                flags_result = ::fcntl(descriptor, F_SETFD, original_descriptor_flags_[index]);
            } while (flags_result < 0 && errno == EINTR);
            if (flags_result < 0) {
                complete = false;
            }
            (void)::close(std::exchange(backups_[index], -1));
            restored_[index] = true;
        }
        return complete;
    }

private:
    std::array<int, 3> backups_{{-1, -1, -1}};
    std::array<int, 3> original_descriptor_flags_{{0, 0, 0}};
    std::array<bool, 3> originally_open_{{false, false, false}};
    std::array<bool, 3> restored_{{false, false, false}};
};

class ChildCleanup final {
public:
    ~ChildCleanup() {
        for (const pid_t process : processes_) {
            if (process > 0) {
                (void)::kill(process, SIGKILL);
            }
        }
        for (const pid_t process : processes_) {
            if (process <= 0) {
                continue;
            }
            int status = 0;
            pid_t waited = -1;
            do {
                waited = ::waitpid(process, &status, 0);
            } while (waited < 0 && errno == EINTR);
        }
    }

    void add(pid_t process) {
        if (process > 0) {
            processes_.push_back(process);
        }
    }

    void mark_reaped(pid_t process) noexcept {
        for (auto& candidate : processes_) {
            if (candidate == process) {
                candidate = -1;
                return;
            }
        }
    }

private:
    std::vector<pid_t> processes_;
};

class HostSignalState final {
public:
    HostSignalState() {
        try {
            sigset_t blocked;
            CHECK(sigemptyset(&blocked) == 0);
            CHECK(sigaddset(&blocked, SIGUSR1) == 0);
            CHECK(::sigprocmask(SIG_BLOCK, &blocked, &old_mask_) == 0);
            mask_saved_ = true;

            struct sigaction ignored{};
            ignored.sa_handler = SIG_IGN;
            CHECK(sigemptyset(&ignored.sa_mask) == 0);
            CHECK(::sigaction(SIGPIPE, &ignored, &old_pipe_action_) == 0);
            action_saved_ = true;
        } catch (...) {
            restore();
            throw;
        }
    }

    ~HostSignalState() {
        restore();
    }

    HostSignalState(const HostSignalState&) = delete;
    HostSignalState& operator=(const HostSignalState&) = delete;

private:
    void restore() noexcept {
        if (action_saved_) {
            (void)::sigaction(SIGPIPE, &old_pipe_action_, nullptr);
            action_saved_ = false;
        }
        if (mask_saved_) {
            (void)::sigprocmask(SIG_SETMASK, &old_mask_, nullptr);
            mask_saved_ = false;
        }
    }

    sigset_t old_mask_{};
    struct sigaction old_pipe_action_{};
    bool mask_saved_ = false;
    bool action_saved_ = false;
};

[[nodiscard]] pid_t waitpid_no_intr(pid_t process, int* status, int options) noexcept {
    pid_t waited = -1;
    do {
        waited = ::waitpid(process, status, options);
    } while (waited < 0 && errno == EINTR);
    return waited;
}

[[nodiscard]] int bounded_poll_timeout(std::chrono::steady_clock::time_point deadline) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<int>(
        std::clamp<std::int64_t>(remaining, INT64_C(1), static_cast<std::int64_t>(INT_MAX)));
}

[[nodiscard]] bool wait_for_read_event(int descriptor,
                                       std::chrono::steady_clock::time_point deadline,
                                       short& events) noexcept {
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd poll_descriptor{
            .fd = descriptor,
            .events = POLLIN | POLLHUP,
            .revents = 0,
        };
        const int result = ::poll(&poll_descriptor, 1, bounded_poll_timeout(deadline));
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        events = poll_descriptor.revents;
        return true;
    }
    return false;
}

FakeReport read_fake_report(int descriptor) {
    FakeReport report;
    auto* destination = reinterpret_cast<std::byte*>(&report);
    std::size_t consumed = 0;
    const auto deadline = std::chrono::steady_clock::now() + IO_TIMEOUT;
    while (consumed < sizeof(report)) {
        short events = 0;
        CHECK(wait_for_read_event(descriptor, deadline, events));
        CHECK((events & POLLNVAL) == 0);

        const ssize_t count = ::read(descriptor, destination + consumed, sizeof(report) - consumed);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        CHECK(count > 0);
        consumed += static_cast<std::size_t>(count);
    }
    return report;
}

void require_report_eof(int descriptor) {
    const auto deadline = std::chrono::steady_clock::now() + IO_TIMEOUT;
    while (true) {
        short events = 0;
        CHECK(wait_for_read_event(descriptor, deadline, events));
        CHECK((events & POLLNVAL) == 0);

        std::array<std::byte, 64> trailing{};
        const ssize_t count = ::read(descriptor, trailing.data(), trailing.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        CHECK(count == 0);
        return;
    }
}

[[nodiscard]] bool descriptor_is_open(int descriptor) noexcept {
    int result = -1;
    do {
        result = ::fcntl(descriptor, F_GETFD);
    } while (result < 0 && errno == EINTR);
    return result >= 0;
}

[[nodiscard]] std::vector<int> snapshot_open_descriptors() {
    long configured_limit = ::sysconf(_SC_OPEN_MAX);
    if (configured_limit < 0) {
        configured_limit = 1024;
    }
    const int scan_limit =
        static_cast<int>(std::min<long>(configured_limit, static_cast<long>(4096)));

    std::vector<int> descriptors;
    for (int descriptor = 0; descriptor < scan_limit; ++descriptor) {
        if (descriptor_is_open(descriptor)) {
            descriptors.push_back(descriptor);
        }
    }
    return descriptors;
}

[[nodiscard]] std::vector<int> added_descriptors(const std::vector<int>& before,
                                                 const std::vector<int>& after) {
    std::vector<int> added;
    std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                        std::back_inserter(added));
    return added;
}

[[nodiscard]] worker_process::DistributedSieveWorkerProcessBatchPrepareResult
prepare_one(std::string_view executable_path, const FakeBootstrap& bootstrap,
            std::span<const std::string_view> arguments = {}) {
    const worker_process::DistributedSieveWorkerProcessSpawnSpec spec{
        .executable_path = executable_path,
        .arguments = arguments,
        .bootstrap_frame = as_bytes(bootstrap),
    };
    return worker_process::prepare_distributed_sieve_worker_process_batch(std::span{&spec, 1U});
}

void register_started_children(
    const worker_process::DistributedSieveWorkerProcessBatchLaunchResult& launched,
    ChildCleanup& cleanup) {
    for (const auto& child : launched.children) {
        if (child.process.has_value()) {
            cleanup.add(child.process->process_id());
        }
    }
}

void require_global_pre_spawn_refusal(
    const worker_process::DistributedSieveWorkerProcessBatchLaunchResult& launched) {
    CHECK(!launched.spawn_loop_entered);
    CHECK(!launched.child_set_complete);
    CHECK(launched.children.empty());
}

void require_complete_spawn_loop_result(
    const worker_process::DistributedSieveWorkerProcessBatchLaunchResult& launched) {
    CHECK(launched.spawn_loop_entered);
    CHECK(launched.child_set_complete);
    CHECK(!launched.children.empty());
}

void check_report(const FakeReport& report, const FakeBootstrap& bootstrap, pid_t process,
                  bool expect_standard_error_open = true) {
    CHECK(report.magic == REPORT_MAGIC);
    CHECK(report.slot == bootstrap.slot);
    CHECK(report.process_id == static_cast<std::int64_t>(process));
    CHECK(report.parent_process_id == static_cast<std::int64_t>(::getpid()));
    CHECK(report.payload_tag == bootstrap.payload_tag);
    CHECK(report.argument_count == bootstrap.expected_argument_count);
    CHECK(report.argument_digest == bootstrap.expected_argument_digest);
    const std::uint32_t required_flags =
        expect_standard_error_open ? REQUIRED_REPORT_FLAGS : REQUIRED_REPORT_FLAGS & ~STDERR_OPEN;
    CHECK((report.flags & required_flags) == required_flags);
    CHECK(((report.flags & STDERR_OPEN) != 0U) == expect_standard_error_open);
    if (bootstrap.expected_capability_count == FIXED_CAPABILITY_COUNT) {
        CHECK((report.flags & REQUIRED_CAPABILITY_FLAGS) == REQUIRED_CAPABILITY_FLAGS);
        CHECK(report.first_unexpected_open_descriptor == -1);
        for (std::size_t index = 0; index < FIXED_CAPABILITY_COUNT; ++index) {
            const auto& expected = bootstrap.expected_capabilities[index];
            const auto& observed = report.observed_capabilities[index];
            CHECK(observed.native_error == 0);
            CHECK(observed.device == expected.device);
            CHECK(observed.inode == expected.inode);
            CHECK(observed.link_count == expected.link_count);
            CHECK(observed.permission_bits == expected.permission_bits);
            CHECK(observed.access_mode == expected.access_mode);
            CHECK((observed.descriptor_flags & FD_CLOEXEC) == 0);
            if (expected.kind == FakeCapabilityKind::directory) {
                CHECK(S_ISDIR(static_cast<mode_t>(observed.mode)));
            } else if (expected.kind == FakeCapabilityKind::regular_file) {
                CHECK(S_ISREG(static_cast<mode_t>(observed.mode)));
            } else {
                CHECK(false);
            }
        }
    }
}

worker_process::DistributedSieveWorkerProcessWaitResult
wait_and_mark_reaped(worker_process::DistributedSieveWorkerProcess& process,
                     ChildCleanup& cleanup) {
    const auto result = process.wait_terminal();
    if (result.reaped) {
        cleanup.mark_reaped(process.process_id());
    }
    return result;
}

struct TrapWaitContext final {
    int calls = 0;
};

worker_process::DistributedSieveWorkerProcessId
trap_wait(worker_process::DistributedSieveWorkerProcessId, int*, int, void* opaque) noexcept {
    auto& context = *static_cast<TrapWaitContext*>(opaque);
    ++context.calls;
    errno = EIO;
    return -1;
}

struct EintrEchildWaitContext final {
    int calls = 0;
    bool options_were_zero = true;
};

worker_process::DistributedSieveWorkerProcessId
eintr_then_echild_wait(worker_process::DistributedSieveWorkerProcessId, int* wait_status,
                       int options, void* opaque) noexcept {
    auto& context = *static_cast<EintrEchildWaitContext*>(opaque);
    ++context.calls;
    context.options_were_zero = context.options_were_zero && options == 0;
    if (wait_status != nullptr) {
        *wait_status = 0;
    }
    if (context.calls == 1) {
        errno = EINTR;
        return -1;
    }
    errno = ECHILD;
    return -1;
}

struct MismatchedProcessWaitContext final {
    int calls = 0;
    int observed_options = -1;
};

worker_process::DistributedSieveWorkerProcessId
return_mismatched_process(worker_process::DistributedSieveWorkerProcessId process, int* wait_status,
                          int options, void* opaque) noexcept {
    auto& context = *static_cast<MismatchedProcessWaitContext*>(opaque);
    ++context.calls;
    context.observed_options = options;
    if (wait_status != nullptr) {
        *wait_status = 0;
    }
    errno = 0;
    return process + 1;
}

struct NonterminalWaitContext final {
    int calls = 0;
    int observed_options = -1;
    int wait_status = 0;
};

worker_process::DistributedSieveWorkerProcessId
wait_for_stopped_child(worker_process::DistributedSieveWorkerProcessId process, int* wait_status,
                       int options, void* opaque) noexcept {
    auto& context = *static_cast<NonterminalWaitContext*>(opaque);
    ++context.calls;
    context.observed_options = options;
    const pid_t observed = waitpid_no_intr(process, wait_status, WUNTRACED);
    if (observed == process && wait_status != nullptr) {
        context.wait_status = *wait_status;
    }
    return observed;
}

struct SpawnFailureContext final {
    int calls = 0;
    std::size_t failure_slot = 1U;
};

int fail_selected_spawn(std::size_t slot, void* opaque) noexcept {
    auto& context = *static_cast<SpawnFailureContext*>(opaque);
    ++context.calls;
    return slot == context.failure_slot ? EAGAIN : 0;
}

int count_spawn_calls(std::size_t, void* opaque) noexcept {
    auto& calls = *static_cast<int*>(opaque);
    ++calls;
    return 0;
}

void test_invalid_prepare_has_no_descriptor_side_effect(const std::string& executable_path) {
    test_prepare_zero();
    const auto baseline = snapshot_open_descriptors();
    const FakeBootstrap valid_bootstrap;

    const auto check_invalid =
        [&baseline](std::span<const worker_process::DistributedSieveWorkerProcessSpawnSpec> specs,
                    int native_error) {
            auto prepared = worker_process::prepare_distributed_sieve_worker_process_batch(specs);
            CHECK(!prepared);
            CHECK(!prepared.batch.has_value());
            CHECK(prepared.diagnostic.error ==
                  worker_process::DistributedSieveWorkerProcessTransportError::invalid_request);
            CHECK(prepared.diagnostic.native_error == native_error);
            CHECK(snapshot_open_descriptors() == baseline);
        };

    const worker_process::DistributedSieveWorkerProcessSpawnSpec empty_path{
        .executable_path = {},
        .arguments = {},
        .bootstrap_frame = as_bytes(valid_bootstrap),
    };
    check_invalid(std::span{&empty_path, 1U}, EINVAL);

    const worker_process::DistributedSieveWorkerProcessSpawnSpec relative_path{
        .executable_path = "relative/fake-child",
        .arguments = {},
        .bootstrap_frame = as_bytes(valid_bootstrap),
    };
    check_invalid(std::span{&relative_path, 1U}, EINVAL);

    const std::string nul_path("/fake\0child", 11U);
    const worker_process::DistributedSieveWorkerProcessSpawnSpec embedded_nul_path{
        .executable_path = std::string_view(nul_path.data(), nul_path.size()),
        .arguments = {},
        .bootstrap_frame = as_bytes(valid_bootstrap),
    };
    check_invalid(std::span{&embedded_nul_path, 1U}, EINVAL);

    const worker_process::DistributedSieveWorkerProcessSpawnSpec empty_bootstrap{
        .executable_path = executable_path,
        .arguments = {},
        .bootstrap_frame = {},
    };
    check_invalid(std::span{&empty_bootstrap, 1U}, EINVAL);

    std::vector<std::byte> oversized(
        worker_process::DISTRIBUTED_SIEVE_WORKER_BOOTSTRAP_FRAME_LIMIT + 1U);
    const worker_process::DistributedSieveWorkerProcessSpawnSpec oversized_bootstrap{
        .executable_path = executable_path,
        .arguments = {},
        .bootstrap_frame = oversized,
    };
    check_invalid(std::span{&oversized_bootstrap, 1U}, E2BIG);

    const std::string nul_argument("arg\0tail", 8U);
    const std::array<std::string_view, 1> invalid_arguments{
        std::string_view(nul_argument.data(), nul_argument.size())};
    const worker_process::DistributedSieveWorkerProcessSpawnSpec embedded_nul_argument{
        .executable_path = executable_path,
        .arguments = invalid_arguments,
        .bootstrap_frame = as_bytes(valid_bootstrap),
    };
    check_invalid(std::span{&embedded_nul_argument, 1U}, EINVAL);
}

void test_invalid_standard_close_has_no_spawn(const std::string& executable_path) {
    const auto baseline = snapshot_open_descriptors();
    const FakeBootstrap bootstrap;
    auto prepared = prepare_one(executable_path, bootstrap);
    CHECK(prepared);

    int spawn_calls = 0;
    const std::array<int, 1> invalid_close{STDIN_FILENO};
    auto launched = worker_process::spawn_distributed_sieve_worker_process_batch(
        std::move(*prepared.batch), invalid_close,
        {
            .before_spawn = count_spawn_calls,
            .context = &spawn_calls,
        });
    CHECK(!launched);
    CHECK(launched.children.empty());
    CHECK(launched.diagnostic.error ==
          worker_process::DistributedSieveWorkerProcessTransportError::invalid_request);
    CHECK(launched.diagnostic.native_error == EINVAL);
    require_global_pre_spawn_refusal(launched);
    CHECK(spawn_calls == 0);
    CHECK(snapshot_open_descriptors() == baseline);
}

void test_fixed_capability_sources_at_targets_are_cycle_safe(const std::string& executable_path) {
    CapabilityFixture fixture(0x101U);
    FixedDescriptorRestorer fixed_descriptors;
    const auto backing_sources = fixture.sources();
    const auto expected = fixture.expectations();

    fixed_descriptors.install(worker_process::DISTRIBUTED_SIEVE_WORKER_CHILD_WAVE_ROOT_DESCRIPTOR,
                              backing_sources[1]);
    fixed_descriptors.install(
        worker_process::DISTRIBUTED_SIEVE_WORKER_CHILD_PERMANENT_WAVE_STORE_LOCK_DESCRIPTOR,
        backing_sources[2]);
    fixed_descriptors.install(
        worker_process::DISTRIBUTED_SIEVE_WORKER_CHILD_ATTEMPT_BASE_LOCK_DESCRIPTOR,
        backing_sources[3]);
    fixed_descriptors.install(
        worker_process::DISTRIBUTED_SIEVE_WORKER_CHILD_WORK_PACKAGE_READER_DESCRIPTOR,
        backing_sources[0]);

    const std::array<int, FIXED_CAPABILITY_COUNT> permuted_sources{
        worker_process::DISTRIBUTED_SIEVE_WORKER_CHILD_WORK_PACKAGE_READER_DESCRIPTOR,
        worker_process::DISTRIBUTED_SIEVE_WORKER_CHILD_WAVE_ROOT_DESCRIPTOR,
        worker_process::DISTRIBUTED_SIEVE_WORKER_CHILD_PERMANENT_WAVE_STORE_LOCK_DESCRIPTOR,
        worker_process::DISTRIBUTED_SIEVE_WORKER_CHILD_ATTEMPT_BASE_LOCK_DESCRIPTOR,
    };
    const auto capability_sources = fixed_capability_sources(permuted_sources);
    const FakeBootstrap bootstrap = capability_bootstrap(fixture, 100U, 0x101U);
    auto prepared = prepare_one(executable_path, bootstrap);
    CHECK(prepared);

    auto launched = worker_process::spawn_distributed_sieve_worker_process_batch_with_capabilities(
        std::move(*prepared.batch), std::span{&capability_sources, 1U});
    CHECK(launched);
    require_complete_spawn_loop_result(launched);
    CHECK(descriptor_matches_expectation(
        worker_process::DISTRIBUTED_SIEVE_WORKER_CHILD_WAVE_ROOT_DESCRIPTOR, expected[1]));
    CHECK(descriptor_matches_expectation(
        worker_process::DISTRIBUTED_SIEVE_WORKER_CHILD_PERMANENT_WAVE_STORE_LOCK_DESCRIPTOR,
        expected[2]));
    CHECK(descriptor_matches_expectation(
        worker_process::DISTRIBUTED_SIEVE_WORKER_CHILD_ATTEMPT_BASE_LOCK_DESCRIPTOR, expected[3]));
    CHECK(descriptor_matches_expectation(
        worker_process::DISTRIBUTED_SIEVE_WORKER_CHILD_WORK_PACKAGE_READER_DESCRIPTOR,
        expected[0]));

    ChildCleanup cleanup;
    register_started_children(launched, cleanup);
    auto& process = *launched.children[0].process;
    const pid_t process_id = process.process_id();
    const auto waited = wait_and_mark_reaped(process, cleanup);
    CHECK(waited.reaped);
    CHECK(waited.success);
    UniqueFd report(process.release_report_descriptor());
    check_report(read_fake_report(report.get()), bootstrap, process_id);
    require_report_eof(report.get());
}

void test_fixed_capability_requires_spawn_time_close_all(const std::string& executable_path) {
    CapabilityFixture fixture(0x100U);
    const auto sources = fixed_capability_sources(fixture.sources());
    const FakeBootstrap bootstrap = capability_bootstrap(fixture, 99U, 0x100U);
    const auto baseline = snapshot_open_descriptors();
    auto prepared = prepare_one(executable_path, bootstrap);
    CHECK(prepared);
    int spawn_calls = 0;
    auto launched = worker_process::spawn_distributed_sieve_worker_process_batch_with_capabilities(
        std::move(*prepared.batch), std::span{&sources, 1U}, {},
        {
            .before_spawn = count_spawn_calls,
            .context = &spawn_calls,
            .force_fixed_capability_close_all_unavailable = true,
        });
    CHECK(!launched);
    CHECK(launched.children.empty());
    CHECK(launched.diagnostic.error ==
          worker_process::DistributedSieveWorkerProcessTransportError::platform_unavailable);
    CHECK(launched.diagnostic.native_error == ENOTSUP);
    require_global_pre_spawn_refusal(launched);
    CHECK(spawn_calls == 0);
    CHECK(snapshot_open_descriptors() == baseline);
}

void test_fixed_capability_closes_ambient_non_cloexec_descriptor(
    const std::string& executable_path) {
    CHECK(worker_process::distributed_sieve_worker_process_fixed_capability_close_all_supported());
    CapabilityFixture fixture(0x102U);
    const auto sources = fixed_capability_sources(fixture.sources());
    const FakeBootstrap bootstrap = capability_bootstrap(fixture, 101U, 0x102U);

    UniqueFd null_descriptor(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    CHECK(null_descriptor.get() >= 0);
    UniqueFd ambient_descriptor(duplicate_non_cloexec_at_least(null_descriptor.get(), 128));
    CHECK(ambient_descriptor.get() >=
          worker_process::DISTRIBUTED_SIEVE_WORKER_CHILD_FIRST_UNMAPPED_DESCRIPTOR);
    CHECK(ambient_descriptor.get() < bootstrap.descriptor_scan_limit);

    auto prepared = prepare_one(executable_path, bootstrap);
    CHECK(prepared);
    auto launched = worker_process::spawn_distributed_sieve_worker_process_batch_with_capabilities(
        std::move(*prepared.batch), std::span{&sources, 1U});
    CHECK(launched);
    CHECK(descriptor_is_open(ambient_descriptor.get()));

    ChildCleanup cleanup;
    register_started_children(launched, cleanup);
    auto& process = *launched.children[0].process;
    const pid_t process_id = process.process_id();
    const auto waited = wait_and_mark_reaped(process, cleanup);
    CHECK(waited.reaped);
    CHECK(waited.success);
    UniqueFd report(process.release_report_descriptor());
    check_report(read_fake_report(report.get()), bootstrap, process_id);
    require_report_eof(report.get());
}

void test_fixed_capability_targets_replace_only_child_foreign_descriptors(
    const std::string& executable_path) {
    CapabilityFixture fixture(0x202U);
    CapabilityFixture foreign(0x203U);
    FixedDescriptorRestorer fixed_descriptors;
    const auto foreign_sources = foreign.sources();
    const auto foreign_expected = foreign.expectations();
    for (std::size_t index = 0; index < FIXED_CAPABILITY_COUNT; ++index) {
        fixed_descriptors.install(
            static_cast<int>(index) +
                worker_process::DISTRIBUTED_SIEVE_WORKER_CHILD_WAVE_ROOT_DESCRIPTOR,
            foreign_sources[index]);
    }

    const auto capability_sources = fixed_capability_sources(fixture.sources());
    const FakeBootstrap bootstrap = capability_bootstrap(fixture, 110U, 0x202U);
    auto prepared = prepare_one(executable_path, bootstrap);
    CHECK(prepared);

    auto launched = worker_process::spawn_distributed_sieve_worker_process_batch_with_capabilities(
        std::move(*prepared.batch), std::span{&capability_sources, 1U});
    CHECK(launched);
    for (std::size_t index = 0; index < FIXED_CAPABILITY_COUNT; ++index) {
        const int target = static_cast<int>(index) +
                           worker_process::DISTRIBUTED_SIEVE_WORKER_CHILD_WAVE_ROOT_DESCRIPTOR;
        CHECK(descriptor_matches_expectation(target, foreign_expected[index]));
    }

    ChildCleanup cleanup;
    register_started_children(launched, cleanup);
    auto& process = *launched.children[0].process;
    const pid_t process_id = process.process_id();
    const auto waited = wait_and_mark_reaped(process, cleanup);
    CHECK(waited.reaped);
    CHECK(waited.success);
    UniqueFd report(process.release_report_descriptor());
    check_report(read_fake_report(report.get()), bootstrap, process_id);
    require_report_eof(report.get());
}

void test_fixed_capability_source_standard_error_is_staged_and_closed(
    const std::string& executable_path) {
    CapabilityFixture fixture(0x303U);
    const auto ordinary_sources = fixture.sources();
    auto source_descriptors = ordinary_sources;
    source_descriptors[3] = STDERR_FILENO;
    const auto capability_sources = fixed_capability_sources(source_descriptors);
    const FakeBootstrap bootstrap = capability_bootstrap(fixture, 120U, 0x303U);

    auto launched = [&]() {
        StandardDescriptorRestorer restorer;
        restorer.close_standard_error();
        int duplicated = -1;
        do {
            duplicated = ::dup2(ordinary_sources[3], STDERR_FILENO);
        } while (duplicated < 0 && errno == EINTR);
        CHECK(duplicated == STDERR_FILENO);

        auto prepared = prepare_one(executable_path, bootstrap);
        CHECK(prepared);
        auto result =
            worker_process::spawn_distributed_sieve_worker_process_batch_with_capabilities(
                std::move(*prepared.batch), std::span{&capability_sources, 1U});
        const bool restored = restorer.restore();
        if (!restored) {
            throw std::runtime_error("failed to restore standard error after capability launch");
        }
        return result;
    }();
    CHECK(launched);

    ChildCleanup cleanup;
    register_started_children(launched, cleanup);
    auto& process = *launched.children[0].process;
    const pid_t process_id = process.process_id();
    const auto waited = wait_and_mark_reaped(process, cleanup);
    CHECK(waited.reaped);
    CHECK(waited.success);
    UniqueFd report(process.release_report_descriptor());
    check_report(read_fake_report(report.get()), bootstrap, process_id, false);
    require_report_eof(report.get());
}

void test_fixed_capability_sources_at_standard_io_are_action_order_safe(
    const std::string& executable_path) {
    CapabilityFixture fixture(0x304U);
    const auto ordinary_sources = fixture.sources();
    auto source_descriptors = ordinary_sources;
    source_descriptors[0] = STDIN_FILENO;
    source_descriptors[1] = STDOUT_FILENO;
    const auto capability_sources = fixed_capability_sources(source_descriptors);
    const FakeBootstrap bootstrap = capability_bootstrap(fixture, 121U, 0x304U);
    const auto standard_descriptors_before = snapshot_standard_descriptors();

    auto launched = [&]() {
        StandardDescriptorRestorer restorer;
        for (int descriptor = STDIN_FILENO; descriptor <= STDOUT_FILENO; ++descriptor) {
            int duplicated = -1;
            do {
                duplicated =
                    ::dup2(ordinary_sources[static_cast<std::size_t>(descriptor)], descriptor);
            } while (duplicated < 0 && errno == EINTR);
            CHECK(duplicated == descriptor);
        }

        auto prepared = prepare_one(executable_path, bootstrap);
        CHECK(prepared);
        auto result =
            worker_process::spawn_distributed_sieve_worker_process_batch_with_capabilities(
                std::move(*prepared.batch), std::span{&capability_sources, 1U});
        const bool restored = restorer.restore();
        if (!restored) {
            throw std::runtime_error(
                "failed to restore standard descriptors after capability launch");
        }
        return result;
    }();
    CHECK(snapshot_standard_descriptors() == standard_descriptors_before);
    CHECK(launched);

    ChildCleanup cleanup;
    register_started_children(launched, cleanup);
    auto& process = *launched.children[0].process;
    const pid_t process_id = process.process_id();
    const auto waited = wait_and_mark_reaped(process, cleanup);
    CHECK(waited.reaped);
    CHECK(waited.success);
    UniqueFd report(process.release_report_descriptor());
    check_report(read_fake_report(report.get()), bootstrap, process_id);
    require_report_eof(report.get());
}

void test_fixed_capability_middle_spawn_failure_is_slot_local(const std::string& executable_path) {
    CapabilityFixture fixture_0(0x401U);
    CapabilityFixture fixture_1(0x402U);
    CapabilityFixture fixture_2(0x403U);
    const std::array<FakeBootstrap, 3> bootstraps{{
        capability_bootstrap(fixture_0, 130U, 0x401U),
        capability_bootstrap(fixture_1, 131U, 0x402U),
        capability_bootstrap(fixture_2, 132U, 0x403U),
    }};
    const std::array<worker_process::DistributedSieveWorkerProcessSpawnSpec, 3> specs{{
        {
            .executable_path = executable_path,
            .arguments = {},
            .bootstrap_frame = as_bytes(bootstraps[0]),
        },
        {
            .executable_path = executable_path,
            .arguments = {},
            .bootstrap_frame = as_bytes(bootstraps[1]),
        },
        {
            .executable_path = executable_path,
            .arguments = {},
            .bootstrap_frame = as_bytes(bootstraps[2]),
        },
    }};
    const std::array capability_sources{
        fixed_capability_sources(fixture_0.sources()),
        fixed_capability_sources(fixture_1.sources()),
        fixed_capability_sources(fixture_2.sources()),
    };
    auto prepared =
        worker_process::prepare_distributed_sieve_worker_process_batch(std::span{specs});
    CHECK(prepared);
    SpawnFailureContext failure;
    auto launched = worker_process::spawn_distributed_sieve_worker_process_batch_with_capabilities(
        std::move(*prepared.batch), capability_sources, {},
        {
            .before_spawn = fail_selected_spawn,
            .context = &failure,
        });
    CHECK(!launched);
    CHECK(launched.diagnostic.error ==
          worker_process::DistributedSieveWorkerProcessTransportError::none);
    require_complete_spawn_loop_result(launched);
    CHECK(launched.children.size() == 3U);
    CHECK(failure.calls == 3);
    CHECK(launched.children[0]);
    CHECK(!launched.children[1]);
    CHECK(launched.children[1].diagnostic.error ==
          worker_process::DistributedSieveWorkerProcessTransportError::spawn_failed);
    CHECK(launched.children[1].diagnostic.native_error == EAGAIN);
    CHECK(launched.children[2]);

    ChildCleanup cleanup;
    register_started_children(launched, cleanup);
    for (const std::size_t index : {std::size_t{0}, std::size_t{2}}) {
        auto& process = *launched.children[index].process;
        const pid_t process_id = process.process_id();
        const auto waited = wait_and_mark_reaped(process, cleanup);
        CHECK(waited.reaped);
        CHECK(waited.success);
        UniqueFd report(process.release_report_descriptor());
        check_report(read_fake_report(report.get()), bootstraps[index], process_id);
        require_report_eof(report.get());
    }
}

void test_invalid_fixed_capability_requests_have_no_spawn(const std::string& executable_path) {
    CapabilityFixture fixture(0x505U);
    const auto valid_sources = fixed_capability_sources(fixture.sources());

    {
        const auto baseline = snapshot_open_descriptors();
        const FakeBootstrap bootstrap;
        auto prepared = prepare_one(executable_path, bootstrap);
        CHECK(prepared);
        int spawn_calls = 0;
        const std::span<const worker_process::DistributedSieveWorkerProcessFixedCapabilitySourcesV1>
            no_capabilities;
        auto launched =
            worker_process::spawn_distributed_sieve_worker_process_batch_with_capabilities(
                std::move(*prepared.batch), no_capabilities, {},
                {
                    .before_spawn = count_spawn_calls,
                    .context = &spawn_calls,
                });
        CHECK(!launched);
        CHECK(launched.children.empty());
        CHECK(launched.diagnostic.error ==
              worker_process::DistributedSieveWorkerProcessTransportError::invalid_request);
        CHECK(launched.diagnostic.native_error == EINVAL);
        CHECK(spawn_calls == 0);
        CHECK(snapshot_open_descriptors() == baseline);
    }

    {
        const auto baseline = snapshot_open_descriptors();
        const FakeBootstrap bootstrap;
        auto prepared = prepare_one(executable_path, bootstrap);
        CHECK(prepared);
        auto duplicate = valid_sources;
        duplicate.permanent_wave_store_lock_descriptor = duplicate.wave_root_directory_descriptor;
        int spawn_calls = 0;
        auto launched =
            worker_process::spawn_distributed_sieve_worker_process_batch_with_capabilities(
                std::move(*prepared.batch), std::span{&duplicate, 1U}, {},
                {
                    .before_spawn = count_spawn_calls,
                    .context = &spawn_calls,
                });
        CHECK(!launched);
        CHECK(launched.children.empty());
        CHECK(launched.diagnostic.error ==
              worker_process::DistributedSieveWorkerProcessTransportError::invalid_request);
        CHECK(launched.diagnostic.native_error == EINVAL);
        CHECK(spawn_calls == 0);
        CHECK(snapshot_open_descriptors() == baseline);
    }

    {
        const auto baseline = snapshot_open_descriptors();
        const FakeBootstrap bootstrap;
        auto prepared = prepare_one(executable_path, bootstrap);
        CHECK(prepared);
        auto unavailable_source = valid_sources;
        unavailable_source.work_package_reader_descriptor = INT_MAX;
        int spawn_calls = 0;
        auto launched =
            worker_process::spawn_distributed_sieve_worker_process_batch_with_capabilities(
                std::move(*prepared.batch), std::span{&unavailable_source, 1U}, {},
                {
                    .before_spawn = count_spawn_calls,
                    .context = &spawn_calls,
                });
        CHECK(!launched);
        CHECK(launched.diagnostic.error ==
              worker_process::DistributedSieveWorkerProcessTransportError::none);
        CHECK(launched.children.size() == 1U);
        CHECK(!launched.children[0]);
        CHECK(!launched.children[0].process.has_value());
        CHECK(launched.children[0].diagnostic.error ==
              worker_process::DistributedSieveWorkerProcessTransportError::spawn_failed);
        CHECK(launched.children[0].diagnostic.native_error == EBADF);
        CHECK(spawn_calls == 0);
        CHECK(snapshot_open_descriptors() == baseline);
    }

    {
        const FakeBootstrap bootstrap;
        const auto baseline = snapshot_open_descriptors();
        auto prepared = prepare_one(executable_path, bootstrap);
        CHECK(prepared);
        const auto batch_descriptors = added_descriptors(baseline, snapshot_open_descriptors());
        CHECK(batch_descriptors.size() == 3U);
        auto live_batch_source = valid_sources;
        live_batch_source.wave_root_directory_descriptor = batch_descriptors.front();
        int spawn_calls = 0;
        auto launched =
            worker_process::spawn_distributed_sieve_worker_process_batch_with_capabilities(
                std::move(*prepared.batch), std::span{&live_batch_source, 1U}, {},
                {
                    .before_spawn = count_spawn_calls,
                    .context = &spawn_calls,
                });
        CHECK(!launched);
        CHECK(launched.children.empty());
        CHECK(launched.diagnostic.error ==
              worker_process::DistributedSieveWorkerProcessTransportError::invalid_request);
        CHECK(launched.diagnostic.native_error == EINVAL);
        CHECK(spawn_calls == 0);
        CHECK(snapshot_open_descriptors() == baseline);
    }
}

void test_single_child_move_wait_release_and_empty_environment(const std::string& executable_path) {
    const std::array<std::string_view, 2> arguments{"alpha", "two words"};
    const FakeBootstrap bootstrap{
        .mode = FakeChildMode::report_and_exit,
        .slot = 7U,
        .exit_code = 0,
        .payload_tag = 0x1234U,
        .expected_argument_count = static_cast<std::uint32_t>(arguments.size()),
        .expected_argument_digest = argument_digest(arguments),
    };
    auto prepared = prepare_one(executable_path, bootstrap, arguments);
    CHECK(prepared);
    CHECK(prepared.batch->size() == 1U);

    worker_process::DistributedSieveWorkerProcessBatch batch(std::move(*prepared.batch));
    CHECK(batch.size() == 1U);
    CHECK(prepared.batch->size() == 0U);

    auto launched = [&batch]() {
        HostSignalState hostile_parent_signals;
        return worker_process::spawn_distributed_sieve_worker_process_batch(std::move(batch));
    }();
    CHECK(launched);
    require_complete_spawn_loop_result(launched);
    CHECK(launched.children.size() == 1U);
    CHECK(batch.size() == 0U);

    const pid_t process_id = launched.children[0].process->process_id();
    ChildCleanup cleanup;
    cleanup.add(process_id);

    worker_process::DistributedSieveWorkerProcess process(std::move(*launched.children[0].process));
    CHECK(launched.children[0].process->process_id() == -1);
    CHECK(launched.children[0].process->report_descriptor() == -1);
    CHECK(process.release_report_descriptor() == -1);

    const auto waited = wait_and_mark_reaped(process, cleanup);
    CHECK(waited.reaped);
    CHECK(waited.success);
    CHECK(waited.exit_status == 0);
    CHECK(waited.signal == 0);
    CHECK(waited.native_error == 0);

    TrapWaitContext trap;
    const auto cached = process.wait_terminal({
        .wait = trap_wait,
        .context = &trap,
    });
    CHECK(cached == waited);
    CHECK(trap.calls == 0);

    UniqueFd report(process.release_report_descriptor());
    CHECK(report.get() >= 0);
    CHECK(process.report_descriptor() == -1);
    check_report(read_fake_report(report.get()), bootstrap, process_id);
    require_report_eof(report.get());
}

void test_cross_child_report_closure(const std::string& executable_path) {
    const std::array<FakeBootstrap, 2> bootstraps{{
        {
            .mode = FakeChildMode::stop_then_exit,
            .slot = 10U,
            .exit_code = 0,
            .payload_tag = 0xA0U,
        },
        {
            .mode = FakeChildMode::report_and_exit,
            .slot = 11U,
            .exit_code = 0,
            .payload_tag = 0xB0U,
        },
    }};
    const std::array<worker_process::DistributedSieveWorkerProcessSpawnSpec, 2> specs{{
        {
            .executable_path = executable_path,
            .arguments = {},
            .bootstrap_frame = as_bytes(bootstraps[0]),
        },
        {
            .executable_path = executable_path,
            .arguments = {},
            .bootstrap_frame = as_bytes(bootstraps[1]),
        },
    }};
    auto prepared =
        worker_process::prepare_distributed_sieve_worker_process_batch(std::span{specs});
    CHECK(prepared);
    auto launched =
        worker_process::spawn_distributed_sieve_worker_process_batch(std::move(*prepared.batch));
    CHECK(launched);

    ChildCleanup cleanup;
    register_started_children(launched, cleanup);
    auto& stopped_process = *launched.children[0].process;
    auto& exiting_process = *launched.children[1].process;
    const pid_t stopped_pid = stopped_process.process_id();
    const pid_t exiting_pid = exiting_process.process_id();

    check_report(read_fake_report(stopped_process.report_descriptor()), bootstraps[0], stopped_pid);
    int stopped_status = 0;
    CHECK(waitpid_no_intr(stopped_pid, &stopped_status, WUNTRACED) == stopped_pid);
    CHECK(WIFSTOPPED(stopped_status));

    const auto exiting_wait = wait_and_mark_reaped(exiting_process, cleanup);
    CHECK(exiting_wait.reaped);
    CHECK(exiting_wait.success);
    UniqueFd exiting_report(exiting_process.release_report_descriptor());
    CHECK(exiting_report.get() >= 0);
    check_report(read_fake_report(exiting_report.get()), bootstraps[1], exiting_pid);
    require_report_eof(exiting_report.get());

    CHECK(::kill(stopped_pid, SIGCONT) == 0);
    const auto stopped_wait = wait_and_mark_reaped(stopped_process, cleanup);
    CHECK(stopped_wait.reaped);
    CHECK(stopped_wait.success);
    UniqueFd stopped_report(stopped_process.release_report_descriptor());
    CHECK(stopped_report.get() >= 0);
    require_report_eof(stopped_report.get());
}

void test_external_descriptor_closed_only_in_child(const std::string& executable_path) {
    UniqueFd external(::open("/dev/null", O_RDONLY));
    CHECK(external.get() > STDERR_FILENO);
    CHECK(descriptor_is_open(external.get()));

    const FakeBootstrap bootstrap{
        .mode = FakeChildMode::report_and_exit,
        .slot = 20U,
        .exit_code = 0,
        .expected_closed_descriptor = external.get(),
        .payload_tag = 0xC0U,
    };
    auto prepared = prepare_one(executable_path, bootstrap);
    CHECK(prepared);
    const std::array<int, 3> close_descriptors{external.get(), -1, external.get()};
    auto launched = worker_process::spawn_distributed_sieve_worker_process_batch(
        std::move(*prepared.batch), close_descriptors);
    CHECK(launched);

    ChildCleanup cleanup;
    register_started_children(launched, cleanup);
    auto& process = *launched.children[0].process;
    const pid_t process_id = process.process_id();
    const auto waited = wait_and_mark_reaped(process, cleanup);
    CHECK(waited.reaped);
    CHECK(waited.success);
    CHECK(descriptor_is_open(external.get()));

    UniqueFd report(process.release_report_descriptor());
    check_report(read_fake_report(report.get()), bootstrap, process_id);
    require_report_eof(report.get());
}

void test_prepare_with_closed_standard_descriptors(const std::string& executable_path) {
    const auto baseline = snapshot_open_descriptors();
    const FakeBootstrap bootstrap{
        .mode = FakeChildMode::report_and_exit,
        .slot = 30U,
        .exit_code = 0,
        .payload_tag = 0xD0U,
    };

    auto prepared = [&]() {
        StandardDescriptorRestorer restorer;
        restorer.close_all();
        auto result = prepare_one(executable_path, bootstrap);
        const bool restored = restorer.restore();
        if (!restored) {
            throw std::runtime_error("failed to restore standard descriptors");
        }
        return result;
    }();
    CHECK(prepared);

    const auto retained = added_descriptors(baseline, snapshot_open_descriptors());
    CHECK(retained.size() == 3U);
    CHECK(std::all_of(retained.begin(), retained.end(),
                      [](int descriptor) { return descriptor > STDERR_FILENO; }));

    auto launched =
        worker_process::spawn_distributed_sieve_worker_process_batch(std::move(*prepared.batch));
    CHECK(launched);
    ChildCleanup cleanup;
    register_started_children(launched, cleanup);
    auto& process = *launched.children[0].process;
    const pid_t process_id = process.process_id();
    const auto waited = wait_and_mark_reaped(process, cleanup);
    CHECK(waited.reaped);
    CHECK(waited.success);
    UniqueFd report(process.release_report_descriptor());
    check_report(read_fake_report(report.get()), bootstrap, process_id);
    require_report_eof(report.get());
}

void test_spawn_with_closed_standard_error(const std::string& executable_path) {
    const FakeBootstrap bootstrap{
        .mode = FakeChildMode::report_and_exit,
        .slot = 35U,
        .exit_code = 0,
        .payload_tag = 0xD5U,
    };
    auto prepared = prepare_one(executable_path, bootstrap);
    CHECK(prepared);

    auto launched = [&prepared]() {
        StandardDescriptorRestorer restorer;
        restorer.close_standard_error();
        auto result = worker_process::spawn_distributed_sieve_worker_process_batch(
            std::move(*prepared.batch));
        const bool restored = restorer.restore();
        if (!restored) {
            throw std::runtime_error("failed to restore standard descriptors");
        }
        return result;
    }();
    CHECK(launched);

    ChildCleanup cleanup;
    register_started_children(launched, cleanup);
    auto& process = *launched.children[0].process;
    const pid_t process_id = process.process_id();
    const auto waited = wait_and_mark_reaped(process, cleanup);
    CHECK(waited.reaped);
    CHECK(waited.success);
    UniqueFd report(process.release_report_descriptor());
    check_report(read_fake_report(report.get()), bootstrap, process_id, false);
    require_report_eof(report.get());
}

void test_middle_spawn_failure_keeps_fixed_slots(const std::string& executable_path) {
    const std::array<FakeBootstrap, 3> bootstraps{{
        {.slot = 40U, .payload_tag = 0xE0U},
        {.slot = 41U, .payload_tag = 0xE1U},
        {.slot = 42U, .payload_tag = 0xE2U},
    }};
    const std::array<worker_process::DistributedSieveWorkerProcessSpawnSpec, 3> specs{{
        {
            .executable_path = executable_path,
            .arguments = {},
            .bootstrap_frame = as_bytes(bootstraps[0]),
        },
        {
            .executable_path = executable_path,
            .arguments = {},
            .bootstrap_frame = as_bytes(bootstraps[1]),
        },
        {
            .executable_path = executable_path,
            .arguments = {},
            .bootstrap_frame = as_bytes(bootstraps[2]),
        },
    }};
    auto prepared =
        worker_process::prepare_distributed_sieve_worker_process_batch(std::span{specs});
    CHECK(prepared);
    SpawnFailureContext failure;
    auto launched = worker_process::spawn_distributed_sieve_worker_process_batch(
        std::move(*prepared.batch), {},
        {
            .before_spawn = fail_selected_spawn,
            .context = &failure,
        });
    CHECK(!launched);
    CHECK(launched.diagnostic.error ==
          worker_process::DistributedSieveWorkerProcessTransportError::none);
    require_complete_spawn_loop_result(launched);
    CHECK(launched.children.size() == 3U);
    CHECK(failure.calls == 3);
    CHECK(launched.children[0]);
    CHECK(!launched.children[1]);
    CHECK(launched.children[1].diagnostic.error ==
          worker_process::DistributedSieveWorkerProcessTransportError::spawn_failed);
    CHECK(launched.children[1].diagnostic.native_error == EAGAIN);
    CHECK(launched.children[2]);

    ChildCleanup cleanup;
    register_started_children(launched, cleanup);
    for (const std::size_t index : {std::size_t{0}, std::size_t{2}}) {
        auto& process = *launched.children[index].process;
        const pid_t process_id = process.process_id();
        const auto waited = wait_and_mark_reaped(process, cleanup);
        CHECK(waited.reaped);
        CHECK(waited.success);
        UniqueFd report(process.release_report_descriptor());
        check_report(read_fake_report(report.get()), bootstraps[index], process_id);
        require_report_eof(report.get());
    }
}

void test_fake_child_exit_code(const std::string& executable_path) {
    const FakeBootstrap bootstrap{
        .mode = FakeChildMode::exit_without_report,
        .slot = 50U,
        .exit_code = 255,
    };
    auto prepared = prepare_one(executable_path, bootstrap);
    CHECK(prepared);
    auto launched =
        worker_process::spawn_distributed_sieve_worker_process_batch(std::move(*prepared.batch));
    CHECK(launched);

    ChildCleanup cleanup;
    register_started_children(launched, cleanup);
    auto& process = *launched.children[0].process;
    const auto waited = wait_and_mark_reaped(process, cleanup);
    CHECK(waited.reaped);
    CHECK(!waited.success);
    CHECK(waited.exit_status == 255);
    CHECK(waited.signal == 0);
    UniqueFd report(process.release_report_descriptor());
    CHECK(report.get() >= 0);
    require_report_eof(report.get());
}

void test_signaled_child_is_terminal(const std::string& executable_path) {
    const FakeBootstrap bootstrap{
        .mode = FakeChildMode::terminate,
        .slot = 60U,
    };
    auto prepared = prepare_one(executable_path, bootstrap);
    CHECK(prepared);
    auto launched =
        worker_process::spawn_distributed_sieve_worker_process_batch(std::move(*prepared.batch));
    CHECK(launched);

    ChildCleanup cleanup;
    register_started_children(launched, cleanup);
    auto& process = *launched.children[0].process;
    const auto waited = wait_and_mark_reaped(process, cleanup);
    CHECK(waited.reaped);
    CHECK(!waited.success);
    CHECK(waited.exit_status == -1);
    CHECK(waited.signal == SIGTERM);
    UniqueFd report(process.release_report_descriptor());
    CHECK(report.get() >= 0);
    require_report_eof(report.get());
}

void test_eintr_then_echild_is_sticky(const std::string& executable_path) {
    const FakeBootstrap bootstrap{
        .mode = FakeChildMode::exit_without_report,
        .slot = 70U,
    };
    auto prepared = prepare_one(executable_path, bootstrap);
    CHECK(prepared);
    auto launched =
        worker_process::spawn_distributed_sieve_worker_process_batch(std::move(*prepared.batch));
    CHECK(launched);

    ChildCleanup cleanup;
    register_started_children(launched, cleanup);
    auto& process = *launched.children[0].process;
    const pid_t process_id = process.process_id();
    EintrEchildWaitContext hook_context;
    const auto uncertain = process.wait_terminal({
        .wait = eintr_then_echild_wait,
        .context = &hook_context,
    });
    CHECK(!uncertain.reaped);
    CHECK(!uncertain.success);
    CHECK(uncertain.native_error == ECHILD);
    CHECK(hook_context.calls == 2);
    CHECK(hook_context.options_were_zero);
    CHECK(process.release_report_descriptor() == -1);

    TrapWaitContext trap;
    CHECK(process.wait_terminal({.wait = trap_wait, .context = &trap}) == uncertain);
    CHECK(trap.calls == 0);

    int status = 0;
    CHECK(waitpid_no_intr(process_id, &status, 0) == process_id);
    CHECK(WIFEXITED(status));
    cleanup.mark_reaped(process_id);
}

void test_mismatched_wait_is_sticky(const std::string& executable_path) {
    const FakeBootstrap bootstrap{
        .mode = FakeChildMode::exit_without_report,
        .slot = 80U,
    };
    auto prepared = prepare_one(executable_path, bootstrap);
    CHECK(prepared);
    auto launched =
        worker_process::spawn_distributed_sieve_worker_process_batch(std::move(*prepared.batch));
    CHECK(launched);

    ChildCleanup cleanup;
    register_started_children(launched, cleanup);
    auto& process = *launched.children[0].process;
    const pid_t process_id = process.process_id();
    MismatchedProcessWaitContext hook_context;
    const auto uncertain = process.wait_terminal({
        .wait = return_mismatched_process,
        .context = &hook_context,
    });
    CHECK(!uncertain.reaped);
    CHECK(!uncertain.success);
    CHECK(uncertain.native_error == ECHILD);
    CHECK(hook_context.calls == 1);
    CHECK(hook_context.observed_options == 0);
    CHECK(process.release_report_descriptor() == -1);

    TrapWaitContext trap;
    CHECK(process.wait_terminal({.wait = trap_wait, .context = &trap}) == uncertain);
    CHECK(trap.calls == 0);

    int status = 0;
    CHECK(waitpid_no_intr(process_id, &status, 0) == process_id);
    CHECK(WIFEXITED(status));
    cleanup.mark_reaped(process_id);
}

void test_nonterminal_wait_is_sticky(const std::string& executable_path) {
    const FakeBootstrap bootstrap{
        .mode = FakeChildMode::stop_then_exit,
        .slot = 90U,
        .payload_tag = 0xF0U,
    };
    auto prepared = prepare_one(executable_path, bootstrap);
    CHECK(prepared);
    auto launched =
        worker_process::spawn_distributed_sieve_worker_process_batch(std::move(*prepared.batch));
    CHECK(launched);

    ChildCleanup cleanup;
    register_started_children(launched, cleanup);
    auto& process = *launched.children[0].process;
    const pid_t process_id = process.process_id();
    check_report(read_fake_report(process.report_descriptor()), bootstrap, process_id);

    NonterminalWaitContext hook_context;
    const auto uncertain = process.wait_terminal({
        .wait = wait_for_stopped_child,
        .context = &hook_context,
    });
    CHECK(!uncertain.reaped);
    CHECK(!uncertain.success);
    CHECK(uncertain.exit_status == -1);
    CHECK(uncertain.signal == 0);
    CHECK(uncertain.native_error == 0);
    CHECK(hook_context.calls == 1);
    CHECK(hook_context.observed_options == 0);
    CHECK(WIFSTOPPED(hook_context.wait_status));
    CHECK(process.release_report_descriptor() == -1);

    TrapWaitContext trap;
    CHECK(process.wait_terminal({.wait = trap_wait, .context = &trap}) == uncertain);
    CHECK(trap.calls == 0);

    CHECK(::kill(process_id, SIGCONT) == 0);
    int status = 0;
    CHECK(waitpid_no_intr(process_id, &status, 0) == process_id);
    CHECK(WIFEXITED(status));
    cleanup.mark_reaped(process_id);
}

[[nodiscard]] std::string resolve_fake_child_path(int argc, char* argv[]) {
    std::filesystem::path path;
    if (argc >= 2) {
        path = argv[1];
    } else {
        path = std::filesystem::absolute(argv[0]).parent_path() /
               "distributed_sieve_worker_process_fake_child";
    }
    path = std::filesystem::absolute(path);
    CHECK(path.is_absolute());
    CHECK(::access(path.c_str(), X_OK) == 0);
    return path.string();
}

#endif

} // namespace

int main(int argc, char* argv[]) {
    try {
#if defined(_WIN32)
        (void)argc;
        (void)argv;
        test_prepare_zero();
        test_platform_unavailable();
#else
        const std::string fake_child_path = resolve_fake_child_path(argc, argv);
        test_invalid_prepare_has_no_descriptor_side_effect(fake_child_path);
        test_invalid_standard_close_has_no_spawn(fake_child_path);
        test_fixed_capability_requires_spawn_time_close_all(fake_child_path);
        if (worker_process::
                distributed_sieve_worker_process_fixed_capability_close_all_supported()) {
            test_fixed_capability_sources_at_targets_are_cycle_safe(fake_child_path);
            test_fixed_capability_closes_ambient_non_cloexec_descriptor(fake_child_path);
            test_fixed_capability_targets_replace_only_child_foreign_descriptors(fake_child_path);
            test_fixed_capability_source_standard_error_is_staged_and_closed(fake_child_path);
            test_fixed_capability_sources_at_standard_io_are_action_order_safe(fake_child_path);
            test_fixed_capability_middle_spawn_failure_is_slot_local(fake_child_path);
            test_invalid_fixed_capability_requests_have_no_spawn(fake_child_path);
        }
        test_single_child_move_wait_release_and_empty_environment(fake_child_path);
        test_cross_child_report_closure(fake_child_path);
        test_external_descriptor_closed_only_in_child(fake_child_path);
        test_prepare_with_closed_standard_descriptors(fake_child_path);
        test_spawn_with_closed_standard_error(fake_child_path);
        test_middle_spawn_failure_keeps_fixed_slots(fake_child_path);
        test_fake_child_exit_code(fake_child_path);
        test_signaled_child_is_terminal(fake_child_path);
        test_eintr_then_echild_is_sticky(fake_child_path);
        test_mismatched_wait_is_sticky(fake_child_path);
        test_nonterminal_wait_is_sticky(fake_child_path);
#endif
        std::cout << "distributed sieve worker process tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

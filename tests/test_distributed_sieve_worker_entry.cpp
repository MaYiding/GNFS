#include <gnfs/core/polynomial_context.hpp>
#include <gnfs/factor_base/factor_base.hpp>
#include <gnfs/relation/ooc_cleanup_transaction.hpp>
#include <gnfs/sieve/distributed_sieve_protocol.hpp>

#include "distributed_sieve_bound_work_internal.hpp"
#include "distributed_sieve_execution_policy_internal.hpp"
#include "distributed_sieve_wave_store_internal.hpp"
#include "distributed_sieve_worker_entry_internal.hpp"
#include "distributed_sieve_worker_launcher_internal.hpp"
#include "distributed_sieve_worker_process_internal.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace sieve = gnfs::sieve;
namespace policy = gnfs::sieve::distributed_sieve_execution_policy_detail;
namespace entry = gnfs::sieve::distributed_sieve_worker_entry_detail;
namespace launcher = gnfs::sieve::distributed_sieve_worker_launcher_detail;
namespace process = gnfs::sieve::distributed_sieve_worker_process_detail;
namespace wave = gnfs::sieve::distributed_sieve_resume_detail;
namespace private_lease = gnfs::relation::ooc_cleanup_detail;

using Digest = gnfs::util::Sha256Digest;

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string_view expression, int line, std::string_view detail = {}) {
    std::string message = "CHECK failed at line " + std::to_string(line) + ": ";
    message.append(expression);
    if (!detail.empty()) {
        message.append(" (");
        message.append(detail);
        message.push_back(')');
    }
    throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view expression, int line) {
    if (!condition) {
        fail(expression, line);
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

[[nodiscard]] constexpr std::uint64_t binary64_bits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] Digest digest_with_seed(std::uint8_t seed) noexcept {
    Digest digest;
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
        digest.bytes[index] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(index));
    }
    return digest;
}

[[nodiscard]] sieve::WaveIdV1 wave_id_with_seed(std::uint8_t seed) noexcept {
    sieve::WaveIdV1 wave_id;
    for (std::size_t index = 0; index < wave_id.bytes.size(); ++index) {
        wave_id.bytes[index] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(index));
    }
    return wave_id;
}

[[nodiscard]] std::string
wave_diagnostic_detail(const wave::DistributedSieveWaveStoreDiagnostic& diagnostic) {
    std::string detail(wave::distributed_sieve_wave_store_status_name(diagnostic.status));
    if (diagnostic.native_error) {
        detail.append(": ");
        detail.append(diagnostic.native_error.message());
    }
    return detail;
}

void require_wave_ready(const wave::DistributedSieveWaveStoreDiagnostic& diagnostic,
                        std::string_view context) {
    if (diagnostic.status != wave::DistributedSieveWaveStoreStatus::ready) {
        fail(context, __LINE__, wave_diagnostic_detail(diagnostic));
    }
}

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0};
        const auto tick =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        for (std::uint64_t attempt = 0; attempt < 100; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    ("gnfs-worker-entry-" + std::to_string(tick) + "-" +
                     std::to_string(sequence.fetch_add(1)) + "-" + std::to_string(attempt));
            std::error_code error;
            if (!std::filesystem::create_directory(path_, error)) {
                if (error == std::errc::file_exists) {
                    continue;
                }
                throw std::filesystem::filesystem_error("create worker-entry fixture root", path_,
                                                        error);
            }
#if !defined(_WIN32)
            if (::chmod(path_.c_str(), 0700) != 0) {
                const int native_error = errno;
                std::filesystem::remove_all(path_, error);
                throw std::system_error(native_error, std::generic_category(),
                                        "chmod worker-entry fixture root");
            }
#endif
            path_ = std::filesystem::canonical(path_, error);
            if (error) {
                std::error_code ignored;
                (void)std::filesystem::remove_all(path_, ignored);
                throw std::filesystem::filesystem_error("canonicalize worker-entry fixture root",
                                                        path_, error);
            }
            return;
        }
        throw TestFailure("could not reserve worker-entry fixture root");
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    ~TempDirectory() {
        std::error_code ignored;
        (void)std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] policy::DistributedSieveFrozenExecutionPolicyV1 make_frozen_policy() {
    policy::DistributedSieveExecutionPolicyEnvironmentSnapshotV1 snapshot;
    snapshot.hardware_concurrency = 4;
    auto frozen = policy::freeze_distributed_sieve_execution_policy_v1(snapshot);
    if (!frozen || !frozen.policy.has_value()) {
        fail("freeze worker-entry policy", __LINE__,
             sieve::distributed_sieve_protocol_error_name(frozen.status.error));
    }
    return std::move(*frozen.policy);
}

[[nodiscard]] sieve::DistributedSieveWorkIdentityV1
make_identity(const policy::DistributedSieveFrozenExecutionPolicyV1& frozen) {
    sieve::DistributedSieveWorkIdentityV1 identity;
    identity.polynomial.n.decimal = "1000036000099";
    identity.polynomial.m.decimal = "10001";
    identity.polynomial.degree = 2;
    identity.polynomial.coefficients = {{"-5"}, {"3"}, {"1"}};
    identity.polynomial.skewness_ieee754_bits = binary64_bits(1.25);

    identity.factor_base.rational_bound = 100;
    identity.factor_base.algebraic_bound = 200;
    identity.factor_base.large_prime_bound = 10'000;
    identity.factor_base.log_scale = 16;
    identity.factor_base.rational = {{2, 16}, {5, 25}};
    identity.factor_base.algebraic = {{7, 1, 37, 1}, {11, 4, 55, 2}};
    identity.factor_base.sieve_algebraic_count = 2;

    identity.sieve = {16, 50, 51, 0, true, false};
    identity.region = {-100, 100, 1, 50};
    identity.cofactor = {0, true, true, false, 0};
    identity.original_sq_bounds = {0, 2, 0, std::numeric_limits<std::uint32_t>::max()};
    identity.effective_sq_bounds = {0, 2, 0, std::numeric_limits<std::uint32_t>::max()};
    identity.distributed.worker_count = 1;
    identity.distributed.chunks = {{0, 0, 2, "entry_chunk_0"}};
    identity.distributed.sq_cap_per_worker = 10;
    identity.distributed.relation_cap_per_worker = 100;
    identity.distributed.max_worker_attempts = 2;
    identity.distributed.max_merge_build_attempts = 2;
    identity.distributed.max_consumption_attempts = 2;
    identity.execution_policy = frozen.canonical;
    identity.semantic_versions = policy::DISTRIBUTED_SIEVE_BOUND_WORK_VERSIONS_V1;

    const auto valid = sieve::validate_distributed_sieve_work_identity(identity);
    if (!valid) {
        fail("validate worker-entry identity", __LINE__,
             sieve::distributed_sieve_protocol_error_name(valid.error));
    }
    return identity;
}

[[nodiscard]] Digest work_digest(const sieve::DistributedSieveWorkIdentityV1& identity) {
    const auto result = sieve::distributed_sieve_work_digest(identity);
    if (!result || !result.digest.has_value()) {
        fail("hash worker-entry identity", __LINE__,
             sieve::distributed_sieve_protocol_error_name(result.status.error));
    }
    return *result.digest;
}

[[nodiscard]] gnfs::core::PolynomialContext make_polynomial() {
    std::vector<gnfs::core::Integer> coefficients;
    coefficients.emplace_back("-5");
    coefficients.emplace_back("3");
    coefficients.emplace_back("1");
    return {gnfs::core::Integer("1000036000099"), std::move(coefficients),
            gnfs::core::Integer("10001"), 1.25};
}

[[nodiscard]] gnfs::factor_base::FactorBase make_factor_base() {
    gnfs::factor_base::FactorBase factor_base({100, 200, 10'000, 16});
    factor_base.reserve(2, 2);
    factor_base.add_rational(2, 16);
    factor_base.add_rational(5, 25);
    factor_base.add_algebraic(7, 1, 37, 1);
    factor_base.add_algebraic(11, 4, 55, 2);
    factor_base.set_sieve_algebraic_count(2);
    factor_base.build_index();
    return factor_base;
}

[[nodiscard]] sieve::WaveManifestV1
make_manifest_draft(const sieve::DistributedSieveWorkIdentityV1& identity,
                    Digest executable_sha256 = digest_with_seed(2)) {
    sieve::WaveManifestV1 manifest;
    manifest.wave_id = wave_id_with_seed(1);
    manifest.execution_contract_version = 1;
    manifest.executable_sha256 = executable_sha256;
    manifest.work_sha256 = work_digest(identity);
    manifest.effective_sq_begin = identity.distributed.chunks.front().sq_begin;
    manifest.effective_sq_end = identity.distributed.chunks.back().sq_end;
    manifest.worker_count = identity.distributed.worker_count;
    manifest.chunks = identity.distributed.chunks;
    manifest.sq_cap_per_worker = identity.distributed.sq_cap_per_worker;
    manifest.relation_cap_per_worker = identity.distributed.relation_cap_per_worker;
    manifest.max_worker_attempts = identity.distributed.max_worker_attempts;
    manifest.max_merge_build_attempts = identity.distributed.max_merge_build_attempts;
    manifest.max_consumption_attempts = identity.distributed.max_consumption_attempts;
    manifest.canonical_naming_version = identity.semantic_versions.chunking_version;
    manifest.retry_policy_version = identity.semantic_versions.retry_policy_version;
    manifest.durable_start_consumes_ordinal = true;
    manifest.ooc_format_version = identity.semantic_versions.ooc_format_version;
    manifest.relation_serialization_version =
        identity.semantic_versions.relation_serialization_version;
    manifest.handoff_version = identity.semantic_versions.handoff_version;
    manifest.receipt_version = identity.semantic_versions.completion_version;
    manifest.digest_version = identity.semantic_versions.digest_version;
    manifest.merge_policy_version = identity.semantic_versions.merge_policy_version;
    return manifest;
}

class WorkerEntryFixture final {
public:
    explicit WorkerEntryFixture(std::string_view label,
                                Digest executable_sha256 = digest_with_seed(2))
        : frozen(make_frozen_policy()), identity(make_identity(frozen)),
          polynomial(make_polynomial()), factor_base(make_factor_base()),
          root(temp.path() / std::string(label)),
          opened(wave::DistributedSieveWaveStore::create(
              root, make_manifest_draft(identity, executable_sha256))) {
        if (!opened || opened.store == nullptr) {
            fail("create worker-entry WaveStore", __LINE__,
                 wave_diagnostic_detail(opened.diagnostic));
        }
    }

    [[nodiscard]] wave::DistributedSieveWaveStore& store() const noexcept {
        return *opened.store;
    }

    [[nodiscard]] wave::DistributedSieveWorkerAttemptStartReceipt start_receipt() {
        auto claimed = store().create_worker_attempt_private_lease_root(0, 0);
        if (!claimed || claimed.claim == nullptr) {
            fail("claim worker-entry P0", __LINE__, wave_diagnostic_detail(claimed.diagnostic));
        }
        auto reserved = wave::reserve_worker_attempt_private_lease(std::move(claimed));
        if (!reserved || !reserved.receipt.has_value()) {
            fail("reserve worker-entry P8", __LINE__, wave_diagnostic_detail(reserved.diagnostic));
        }
        auto started = wave::publish_worker_attempt_started(std::move(*reserved.receipt));
        if (!started || !started.receipt.has_value()) {
            fail("publish worker-entry AttemptStarted", __LINE__,
                 wave_diagnostic_detail(started.diagnostic));
        }
        return std::move(*started.receipt);
    }

    TempDirectory temp;
    policy::DistributedSieveFrozenExecutionPolicyV1 frozen;
    sieve::DistributedSieveWorkIdentityV1 identity;
    gnfs::core::PolynomialContext polynomial;
    gnfs::factor_base::FactorBase factor_base;
    std::filesystem::path root;
    wave::DistributedSieveWaveStoreOpenResult opened;
};

[[nodiscard]] std::filesystem::path self_executable_path(std::string_view argv0) {
    std::error_code error;
    auto result = std::filesystem::canonical(std::filesystem::path(argv0), error);
    if (error || !result.is_absolute()) {
        fail("canonical self executable", __LINE__, error ? error.message() : "not absolute");
    }
    return result;
}

enum class ChildScenario : std::uint32_t {
    happy = 1,
    close_fd3,
    close_fd4,
    close_fd5,
    close_fd6,
    fresh_open_fd4,
    fresh_open_fd5,
    package_mode_tamper,
    package_flags_tamper,
    manifest_replacement_sandwich,
    attempt_record_replacement_sandwich,
    base_lock_replacement_sandwich,
    private_directory_replacement_sandwich,
    owner_replacement_sandwich,
    owned_replacement_sandwich,
    owned_pending_conflict_sandwich,
    forged_ordinal_zero_predecessor,
    wrong_base_path_digest,
    foreign_staging_residue,
    direct_parent_mode_tamper_sandwich,
};

inline constexpr std::string_view CHILD_ARGUMENT = "--worker-entry-child";
inline constexpr std::uint32_t CHILD_REPORT_MAGIC = 0x47574531U;
inline constexpr std::uint32_t CHILD_FLAG_FIRST_SUCCESS = 1U << 0U;
inline constexpr std::uint32_t CHILD_FLAG_TOKEN_VALID = 1U << 1U;
inline constexpr std::uint32_t CHILD_FLAG_TOKEN_REVALIDATED = 1U << 2U;
inline constexpr std::uint32_t CHILD_FLAG_FIXED_FDS_CLOSED = 1U << 3U;
inline constexpr std::uint32_t CHILD_FLAG_SECOND_REJECTED = 1U << 4U;
inline constexpr std::uint32_t CHILD_FLAG_FORK_REJECTED = 1U << 5U;
inline constexpr std::uint32_t CHILD_FLAG_PARENT_STILL_VALID = 1U << 6U;
inline constexpr std::uint32_t CHILD_FLAG_MUTATION_INVOKED = 1U << 7U;
inline constexpr std::uint32_t CHILD_FLAG_MUTATION_SUCCEEDED = 1U << 8U;
inline constexpr std::uint32_t CHILD_FLAG_BINDINGS_VALID = 1U << 9U;
inline constexpr std::uint32_t CHILD_FLAG_STDIN_CLOSED = 1U << 10U;
inline constexpr std::uint32_t CHILD_HAPPY_FLAGS =
    CHILD_FLAG_FIRST_SUCCESS | CHILD_FLAG_TOKEN_VALID | CHILD_FLAG_TOKEN_REVALIDATED |
    CHILD_FLAG_FIXED_FDS_CLOSED | CHILD_FLAG_SECOND_REJECTED | CHILD_FLAG_FORK_REJECTED |
    CHILD_FLAG_PARENT_STILL_VALID | CHILD_FLAG_BINDINGS_VALID | CHILD_FLAG_STDIN_CLOSED;

struct ChildReport final {
    std::uint32_t magic = CHILD_REPORT_MAGIC;
    std::uint32_t scenario = 0;
    std::uint32_t flags = 0;
    std::uint32_t chunk_id = 0;
    std::uint32_t attempt_ordinal = 0;
    std::uint32_t first_status = 0;
    std::uint32_t first_phase = 0;
    std::uint32_t revalidate_status = 0;
    std::uint32_t second_status = 0;
    std::uint32_t fork_status = 0;
    std::int32_t mutation_error = 0;
    Digest attempt_digest;
    Digest manifest_digest;
    Digest work_digest;
};

static_assert(std::is_trivially_copyable_v<ChildReport>);
static_assert(noexcept(entry::adopt_distributed_sieve_worker_entry_v1()));
static_assert(noexcept(std::declval<const entry::DistributedSieveWorkerEntryV1&>().revalidate()));

#if !defined(_WIN32)

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

[[nodiscard]] bool read_exact(int descriptor, void* destination, std::size_t size) {
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

[[nodiscard]] int read_to_eof_bounded(int descriptor, std::vector<std::byte>& bytes) noexcept {
    constexpr std::size_t limit = 1024U * 1024U;
    std::array<std::byte, 4096> buffer{};
    bytes.clear();
    for (;;) {
        ssize_t received = -1;
        do {
            received = ::read(descriptor, buffer.data(), buffer.size());
        } while (received < 0 && errno == EINTR);
        if (received < 0) {
            return errno;
        }
        if (received == 0) {
            return 0;
        }
        const auto count = static_cast<std::size_t>(received);
        if (bytes.size() > limit - count) {
            return EFBIG;
        }
        try {
            bytes.insert(bytes.end(), buffer.begin(),
                         buffer.begin() + static_cast<std::ptrdiff_t>(count));
        } catch (...) {
            return ENOMEM;
        }
    }
}

[[nodiscard]] bool descriptor_is_closed(int descriptor) noexcept {
    errno = 0;
    return ::fcntl(descriptor, F_GETFD) < 0 && errno == EBADF;
}

[[nodiscard]] bool all_fixed_descriptors_are_closed() noexcept {
    for (int descriptor = process::DISTRIBUTED_SIEVE_WORKER_CHILD_WAVE_ROOT_DESCRIPTOR;
         descriptor <= process::DISTRIBUTED_SIEVE_WORKER_CHILD_WORK_PACKAGE_READER_DESCRIPTOR;
         ++descriptor) {
        if (!descriptor_is_closed(descriptor)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] int close_descriptor(int descriptor) noexcept {
    const int result = ::close(descriptor);
    return result == 0 ? 0 : errno;
}

struct ReplacementHookContext final {
    enum class Kind : std::uint8_t {
        root_regular_replacement,
        private_directory_replacement,
        owner_replacement,
        root_regular_copy,
        direct_parent_mode_tamper,
    };

    Kind kind = Kind::root_regular_replacement;
    int root_descriptor = -1;
    std::string private_directory_leaf;
    std::string leaf;
    std::string displaced_leaf;
    int error = 0;
    bool invoked = false;
    bool replaced = false;
};

[[nodiscard]] int read_regular_at(int directory, std::string_view leaf,
                                  std::vector<std::byte>& bytes, mode_t& mode) noexcept {
    int reader = -1;
    do {
        reader = ::openat(directory, std::string(leaf).c_str(),
                          O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    } while (reader < 0 && errno == EINTR);
    if (reader < 0) {
        return errno;
    }

    struct stat metadata {};
    if (::fstat(reader, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
        static_cast<std::uint64_t>(metadata.st_size) > 1024U * 1024U) {
        const int failure = errno == 0 ? EINVAL : errno;
        (void)::close(reader);
        return failure;
    }
    mode = metadata.st_mode & static_cast<mode_t>(07777);
    try {
        bytes.resize(static_cast<std::size_t>(metadata.st_size));
    } catch (...) {
        (void)::close(reader);
        return ENOMEM;
    }

    std::size_t consumed = 0;
    while (consumed < bytes.size()) {
        ssize_t received = -1;
        do {
            received = ::pread(reader, bytes.data() + consumed, bytes.size() - consumed,
                               static_cast<off_t>(consumed));
        } while (received < 0 && errno == EINTR);
        if (received <= 0) {
            const int failure = received < 0 ? errno : EIO;
            (void)::close(reader);
            return failure;
        }
        consumed += static_cast<std::size_t>(received);
    }
    return ::close(reader) == 0 ? 0 : errno;
}

[[nodiscard]] int replace_regular_at_same_bytes(int directory, std::string_view leaf,
                                                std::string_view displaced_leaf) noexcept {
    std::vector<std::byte> bytes;
    mode_t mode = 0;
    if (const int failure = read_regular_at(directory, leaf, bytes, mode); failure != 0) {
        return failure;
    }

    const std::string leaf_string(leaf);
    const std::string displaced_string(displaced_leaf);
    if (::renameat(directory, leaf_string.c_str(), directory, displaced_string.c_str()) != 0) {
        return errno;
    }

    int writer = -1;
    do {
        writer = ::openat(directory, leaf_string.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    } while (writer < 0 && errno == EINTR);
    if (writer < 0) {
        return errno;
    }

    int failure = 0;
    std::size_t produced = 0;
    while (produced < bytes.size()) {
        ssize_t written = -1;
        do {
            written = ::write(writer, bytes.data() + produced, bytes.size() - produced);
        } while (written < 0 && errno == EINTR);
        if (written <= 0) {
            failure = written < 0 ? errno : EIO;
            break;
        }
        produced += static_cast<std::size_t>(written);
    }
    if (failure == 0 && ::fchmod(writer, mode) != 0) {
        failure = errno;
    }
    if (::close(writer) != 0 && failure == 0) {
        failure = errno;
    }
    return failure;
}

[[nodiscard]] int replace_regular_at_bytes(int directory, std::string_view leaf,
                                           std::string_view displaced_leaf,
                                           std::span<const std::byte> replacement) noexcept {
    std::vector<std::byte> original;
    mode_t mode = 0;
    if (const int failure = read_regular_at(directory, leaf, original, mode); failure != 0) {
        return failure;
    }

    const std::string leaf_string(leaf);
    const std::string displaced_string(displaced_leaf);
    if (::renameat(directory, leaf_string.c_str(), directory, displaced_string.c_str()) != 0) {
        return errno;
    }

    int writer = -1;
    do {
        writer = ::openat(directory, leaf_string.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    } while (writer < 0 && errno == EINTR);
    if (writer < 0) {
        return errno;
    }

    int failure = 0;
    std::size_t produced = 0;
    while (produced < replacement.size()) {
        ssize_t written = -1;
        do {
            written = ::write(writer, replacement.data() + produced, replacement.size() - produced);
        } while (written < 0 && errno == EINTR);
        if (written <= 0) {
            failure = written < 0 ? errno : EIO;
            break;
        }
        produced += static_cast<std::size_t>(written);
    }
    if (failure == 0 && ::fchmod(writer, mode) != 0) {
        failure = errno;
    }
    if (failure == 0 && ::fsync(writer) != 0) {
        failure = errno;
    }
    if (::close(writer) != 0 && failure == 0) {
        failure = errno;
    }
    return failure;
}

[[nodiscard]] int write_regular_in_place_at(int directory, std::string_view leaf,
                                            std::span<const std::byte> replacement) noexcept {
    int writer = -1;
    do {
        writer = ::openat(directory, std::string(leaf).c_str(),
                          O_WRONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    } while (writer < 0 && errno == EINTR);
    if (writer < 0) {
        return errno;
    }

    struct stat metadata {};
    int failure = 0;
    if (::fstat(writer, &metadata) != 0) {
        failure = errno;
    } else if (!S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
               static_cast<std::uint64_t>(metadata.st_size) != replacement.size()) {
        failure = EINVAL;
    }

    std::size_t produced = 0;
    while (failure == 0 && produced < replacement.size()) {
        ssize_t written = -1;
        do {
            written = ::pwrite(writer, replacement.data() + produced, replacement.size() - produced,
                               static_cast<off_t>(produced));
        } while (written < 0 && errno == EINTR);
        if (written <= 0) {
            failure = written < 0 ? errno : EIO;
            break;
        }
        produced += static_cast<std::size_t>(written);
    }
    if (failure == 0 && ::fsync(writer) != 0) {
        failure = errno;
    }
    if (::close(writer) != 0 && failure == 0) {
        failure = errno;
    }
    return failure;
}

[[nodiscard]] int copy_regular_at_same_bytes(int directory, std::string_view source_leaf,
                                             std::string_view destination_leaf) noexcept {
    std::vector<std::byte> bytes;
    mode_t mode = 0;
    if (const int failure = read_regular_at(directory, source_leaf, bytes, mode); failure != 0) {
        return failure;
    }

    const std::string destination(destination_leaf);
    int writer = -1;
    do {
        writer = ::openat(directory, destination.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    } while (writer < 0 && errno == EINTR);
    if (writer < 0) {
        return errno;
    }

    int failure = 0;
    std::size_t produced = 0;
    while (produced < bytes.size()) {
        ssize_t written = -1;
        do {
            written = ::write(writer, bytes.data() + produced, bytes.size() - produced);
        } while (written < 0 && errno == EINTR);
        if (written <= 0) {
            failure = written < 0 ? errno : EIO;
            break;
        }
        produced += static_cast<std::size_t>(written);
    }
    if (failure == 0 && ::fchmod(writer, mode) != 0) {
        failure = errno;
    }
    if (::close(writer) != 0 && failure == 0) {
        failure = errno;
    }
    return failure;
}

[[nodiscard]] int
forge_ordinal_zero_predecessor(int root_descriptor,
                               const wave::DistributedSieveWorkerAttemptNamesV1& names,
                               std::string_view displaced_leaf) noexcept {
    try {
        std::vector<std::byte> bootstrap;
        if (const int failure = read_to_eof_bounded(STDIN_FILENO, bootstrap); failure != 0) {
            return failure;
        }
        auto decoded = sieve::decode_distributed_sieve_record(bootstrap);
        if (!decoded || !decoded.value.has_value()) {
            return EPROTO;
        }
        auto* attempt = std::get_if<sieve::AttemptStartedV1>(&*decoded.value);
        if (attempt == nullptr || attempt->attempt_ordinal != 0) {
            return EPROTO;
        }

        attempt->predecessor_digest = digest_with_seed(0xd1);
        if (attempt->predecessor_digest == attempt->manifest_digest) {
            attempt->predecessor_digest = digest_with_seed(0xd2);
        }
        attempt->self_digest = {};
        if (const auto sealed = sieve::seal_distributed_sieve_record(*decoded.value); !sealed) {
            return EPROTO;
        }
        auto encoded = sieve::encode_distributed_sieve_record(*decoded.value);
        if (!encoded || !encoded.bytes.has_value()) {
            return EPROTO;
        }
        if (const int failure = replace_regular_at_bytes(
                root_descriptor, names.canonical_record_leaf, displaced_leaf, *encoded.bytes);
            failure != 0) {
            return failure;
        }

        int bootstrap_pipe[2]{-1, -1};
        if (::pipe(bootstrap_pipe) != 0) {
            return errno;
        }
        if (!write_exact(bootstrap_pipe[1], encoded.bytes->data(), encoded.bytes->size())) {
            const int failure = errno == 0 ? EIO : errno;
            (void)::close(bootstrap_pipe[0]);
            (void)::close(bootstrap_pipe[1]);
            return failure;
        }
        if (::close(bootstrap_pipe[1]) != 0) {
            const int failure = errno;
            (void)::close(bootstrap_pipe[0]);
            return failure;
        }
        bootstrap_pipe[1] = -1;
        if (::dup2(bootstrap_pipe[0], STDIN_FILENO) < 0) {
            const int failure = errno;
            (void)::close(bootstrap_pipe[0]);
            return failure;
        }
        return ::close(bootstrap_pipe[0]) == 0 ? 0 : errno;
    } catch (...) {
        return ENOMEM;
    }
}

[[nodiscard]] int rewrite_private_lease_with_wrong_base_path_digest(
    int root_descriptor, const wave::DistributedSieveWorkerAttemptNamesV1& names) noexcept {
    int private_directory = -1;
    try {
        std::vector<std::byte> reserved_bytes;
        std::vector<std::byte> owner_bytes;
        std::vector<std::byte> owned_bytes;
        mode_t mode = 0;
        if (const int failure =
                read_regular_at(root_descriptor, names.reserved_leaf, reserved_bytes, mode);
            failure != 0) {
            return failure;
        }
        if (const int failure =
                read_regular_at(root_descriptor, names.owned_leaf, owned_bytes, mode);
            failure != 0) {
            return failure;
        }

        do {
            private_directory =
                ::openat(root_descriptor, names.private_directory_leaf.c_str(),
                         O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        } while (private_directory < 0 && errno == EINTR);
        if (private_directory < 0) {
            return errno;
        }
        if (const int failure =
                read_regular_at(private_directory, wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF,
                                owner_bytes, mode);
            failure != 0) {
            (void)::close(private_directory);
            return failure;
        }

        auto reserved = private_lease::parse_private_lease_marker(reserved_bytes);
        const auto original_digest = reserved.base_path_digest;
        const auto original_owner = private_lease::parse_private_lease_marker(owner_bytes);
        const auto original_owned = private_lease::parse_private_lease_marker(owned_bytes);
        reserved.base_path_digest[0] ^= UINT64_C(0x9e3779b97f4a7c15);
        if ((reserved.base_path_digest[0] | reserved.base_path_digest[1] |
             reserved.base_path_digest[2] | reserved.base_path_digest[3]) == 0) {
            reserved.base_path_digest[0] = 1;
        }
        if (reserved.base_path_digest == original_digest) {
            (void)::close(private_directory);
            return EINVAL;
        }

        const auto owner = private_lease::make_private_lease_owner_record(
            reserved, original_owner.directory_identity);
        const auto owned =
            private_lease::make_private_lease_owned_record(owner, original_owned.owner_identity);
        const auto changed_reserved = private_lease::serialize_private_lease_marker(reserved);
        const auto changed_owner = private_lease::serialize_private_lease_marker(owner);
        const auto changed_owned = private_lease::serialize_private_lease_marker(owned);

        int failure =
            write_regular_in_place_at(root_descriptor, names.reserved_leaf, changed_reserved);
        if (failure == 0) {
            failure = write_regular_in_place_at(
                private_directory, wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF, changed_owner);
        }
        if (failure == 0) {
            failure = write_regular_in_place_at(root_descriptor, names.owned_leaf, changed_owned);
        }
        if (::close(private_directory) != 0 && failure == 0) {
            failure = errno;
        }
        return failure;
    } catch (...) {
        if (private_directory >= 0) {
            (void)::close(private_directory);
        }
        return ENOMEM;
    }
}

[[nodiscard]] int
create_foreign_staging_residue(int root_descriptor,
                               const wave::DistributedSieveWorkerAttemptNamesV1& names,
                               std::string& staging_leaf) noexcept {
    try {
        std::vector<std::byte> record_bytes;
        mode_t mode = 0;
        if (const int failure =
                read_regular_at(root_descriptor, names.canonical_record_leaf, record_bytes, mode);
            failure != 0) {
            return failure;
        }
        auto decoded = sieve::decode_distributed_sieve_record(record_bytes);
        if (!decoded || !decoded.value.has_value()) {
            return EPROTO;
        }
        const auto* attempt = std::get_if<sieve::AttemptStartedV1>(&*decoded.value);
        if (attempt == nullptr) {
            return EPROTO;
        }
        auto foreign_lease_id = attempt->lease.lease_id.limbs;
        foreign_lease_id[0] ^= 1U;
        if ((foreign_lease_id[0] | foreign_lease_id[1]) == 0) {
            foreign_lease_id[1] = 1;
        }
        if (foreign_lease_id == attempt->lease.lease_id.limbs) {
            return EINVAL;
        }

        staging_leaf = names.relative_lease_stem;
        staging_leaf.append(wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_STAGING_TAG);
        staging_leaf.append(private_lease::private_lease_id_hex(foreign_lease_id));
        if (::mkdirat(root_descriptor, staging_leaf.c_str(), 0700) != 0) {
            return errno;
        }
        return 0;
    } catch (...) {
        return ENOMEM;
    }
}

void replace_after_first_validation(void* opaque) noexcept {
    auto& context = *static_cast<ReplacementHookContext*>(opaque);
    context.invoked = true;
    switch (context.kind) {
    case ReplacementHookContext::Kind::root_regular_replacement:
        context.error = replace_regular_at_same_bytes(context.root_descriptor, context.leaf,
                                                      context.displaced_leaf);
        break;
    case ReplacementHookContext::Kind::private_directory_replacement:
        if (::renameat(context.root_descriptor, context.leaf.c_str(), context.root_descriptor,
                       context.displaced_leaf.c_str()) != 0) {
            context.error = errno;
        } else if (::mkdirat(context.root_descriptor, context.leaf.c_str(), 0700) != 0) {
            context.error = errno;
        }
        break;
    case ReplacementHookContext::Kind::owner_replacement: {
        int directory = -1;
        do {
            directory = ::openat(context.root_descriptor, context.private_directory_leaf.c_str(),
                                 O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        } while (directory < 0 && errno == EINTR);
        if (directory < 0) {
            context.error = errno;
        } else {
            context.error =
                replace_regular_at_same_bytes(directory, context.leaf, context.displaced_leaf);
            if (::close(directory) != 0 && context.error == 0) {
                context.error = errno;
            }
        }
        break;
    }
    case ReplacementHookContext::Kind::root_regular_copy:
        context.error = copy_regular_at_same_bytes(context.root_descriptor, context.leaf,
                                                   context.displaced_leaf);
        break;
    case ReplacementHookContext::Kind::direct_parent_mode_tamper: {
        int parent = -1;
        do {
            parent = ::openat(context.root_descriptor, "..",
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        } while (parent < 0 && errno == EINTR);
        if (parent < 0) {
            context.error = errno;
        } else {
            if (::fchmod(parent, 0770) != 0) {
                context.error = errno;
            }
            if (::close(parent) != 0 && context.error == 0) {
                context.error = errno;
            }
        }
        break;
    }
    }
    context.replaced = context.error == 0;
}

[[nodiscard]] std::optional<ChildScenario> parse_child_scenario(std::string_view raw) noexcept {
    std::uint32_t value = 0;
    if (raw.empty()) {
        return std::nullopt;
    }
    for (const char character : raw) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        value = value * 10U + static_cast<std::uint32_t>(character - '0');
    }
    if (value < static_cast<std::uint32_t>(ChildScenario::happy) ||
        value > static_cast<std::uint32_t>(ChildScenario::direct_parent_mode_tamper_sandwich)) {
        return std::nullopt;
    }
    return static_cast<ChildScenario>(value);
}

[[nodiscard]] int run_child(ChildScenario scenario) noexcept {
    ChildReport report;
    report.scenario = static_cast<std::uint32_t>(scenario);

    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    if (!names.has_value()) {
        (void)write_exact(STDOUT_FILENO, &report, sizeof(report));
        return 90;
    }

    ReplacementHookContext replacement;
    switch (scenario) {
    case ChildScenario::happy:
        break;
    case ChildScenario::close_fd3:
    case ChildScenario::close_fd4:
    case ChildScenario::close_fd5:
    case ChildScenario::close_fd6: {
        const int descriptor = process::DISTRIBUTED_SIEVE_WORKER_CHILD_WAVE_ROOT_DESCRIPTOR +
                               static_cast<int>(scenario) -
                               static_cast<int>(ChildScenario::close_fd3);
        report.mutation_error = close_descriptor(descriptor);
        report.flags |= CHILD_FLAG_MUTATION_INVOKED;
        if (report.mutation_error == 0) {
            report.flags |= CHILD_FLAG_MUTATION_SUCCEEDED;
        }
        break;
    }
    case ChildScenario::fresh_open_fd4:
    case ChildScenario::fresh_open_fd5: {
        report.flags |= CHILD_FLAG_MUTATION_INVOKED;
        const bool permanent = scenario == ChildScenario::fresh_open_fd4;
        const int target =
            permanent ? process::DISTRIBUTED_SIEVE_WORKER_CHILD_PERMANENT_WAVE_STORE_LOCK_DESCRIPTOR
                      : process::DISTRIBUTED_SIEVE_WORKER_CHILD_ATTEMPT_BASE_LOCK_DESCRIPTOR;
        const std::string leaf =
            permanent ? std::string(wave::DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF) : names->base_lock_leaf;
        report.mutation_error = close_descriptor(target);
        int replacement = -1;
        if (report.mutation_error == 0) {
            do {
                replacement = ::openat(process::DISTRIBUTED_SIEVE_WORKER_CHILD_WAVE_ROOT_DESCRIPTOR,
                                       leaf.c_str(), O_RDWR | O_NONBLOCK | O_NOFOLLOW);
            } while (replacement < 0 && errno == EINTR);
            if (replacement != target) {
                report.mutation_error = replacement < 0 ? errno : EINVAL;
                if (replacement >= 0) {
                    (void)::close(replacement);
                }
            }
        }
        if (report.mutation_error == 0) {
            report.flags |= CHILD_FLAG_MUTATION_SUCCEEDED;
        }
        break;
    }
    case ChildScenario::package_mode_tamper:
        report.flags |= CHILD_FLAG_MUTATION_INVOKED;
        if (::fchmod(process::DISTRIBUTED_SIEVE_WORKER_CHILD_WORK_PACKAGE_READER_DESCRIPTOR,
                     0600) == 0) {
            report.flags |= CHILD_FLAG_MUTATION_SUCCEEDED;
        } else {
            report.mutation_error = errno;
        }
        break;
    case ChildScenario::package_flags_tamper: {
        report.flags |= CHILD_FLAG_MUTATION_INVOKED;
        const int descriptor =
            process::DISTRIBUTED_SIEVE_WORKER_CHILD_WORK_PACKAGE_READER_DESCRIPTOR;
        const int flags = ::fcntl(descriptor, F_GETFL);
        if (flags >= 0 && ::fcntl(descriptor, F_SETFL, flags | O_APPEND) == 0) {
            report.flags |= CHILD_FLAG_MUTATION_SUCCEEDED;
        } else {
            report.mutation_error = errno;
        }
        break;
    }
    case ChildScenario::manifest_replacement_sandwich:
        replacement.leaf = std::string(wave::DISTRIBUTED_SIEVE_WAVE_MANIFEST_LEAF);
        replacement.displaced_leaf = ".gnfs-test-displaced-manifest";
        break;
    case ChildScenario::attempt_record_replacement_sandwich:
        replacement.leaf = names->canonical_record_leaf;
        replacement.displaced_leaf = ".gnfs-test-displaced-attempt";
        break;
    case ChildScenario::base_lock_replacement_sandwich:
        replacement.leaf = names->base_lock_leaf;
        replacement.displaced_leaf = ".gnfs-test-displaced-base-lock";
        break;
    case ChildScenario::private_directory_replacement_sandwich:
        replacement.kind = ReplacementHookContext::Kind::private_directory_replacement;
        replacement.leaf = names->private_directory_leaf;
        replacement.displaced_leaf = ".gnfs-test-displaced-private-directory";
        break;
    case ChildScenario::owner_replacement_sandwich:
        replacement.kind = ReplacementHookContext::Kind::owner_replacement;
        replacement.private_directory_leaf = names->private_directory_leaf;
        replacement.leaf = std::string(wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF);
        replacement.displaced_leaf = ".gnfs-test-displaced-owner";
        break;
    case ChildScenario::owned_replacement_sandwich:
        replacement.leaf = names->owned_leaf;
        replacement.displaced_leaf = ".gnfs-test-displaced-owned";
        break;
    case ChildScenario::owned_pending_conflict_sandwich:
        replacement.kind = ReplacementHookContext::Kind::root_regular_copy;
        replacement.leaf = names->owned_leaf;
        replacement.displaced_leaf = names->owned_pending_leaf;
        break;
    case ChildScenario::forged_ordinal_zero_predecessor:
        report.flags |= CHILD_FLAG_MUTATION_INVOKED;
        report.mutation_error = forge_ordinal_zero_predecessor(
            process::DISTRIBUTED_SIEVE_WORKER_CHILD_WAVE_ROOT_DESCRIPTOR, *names,
            ".gnfs-test-displaced-ordinal-zero-attempt");
        if (report.mutation_error == 0) {
            report.flags |= CHILD_FLAG_MUTATION_SUCCEEDED;
        }
        break;
    case ChildScenario::wrong_base_path_digest:
        report.flags |= CHILD_FLAG_MUTATION_INVOKED;
        report.mutation_error = rewrite_private_lease_with_wrong_base_path_digest(
            process::DISTRIBUTED_SIEVE_WORKER_CHILD_WAVE_ROOT_DESCRIPTOR, *names);
        if (report.mutation_error == 0) {
            report.flags |= CHILD_FLAG_MUTATION_SUCCEEDED;
        }
        break;
    case ChildScenario::foreign_staging_residue: {
        report.flags |= CHILD_FLAG_MUTATION_INVOKED;
        std::string staging_leaf;
        report.mutation_error = create_foreign_staging_residue(
            process::DISTRIBUTED_SIEVE_WORKER_CHILD_WAVE_ROOT_DESCRIPTOR, *names, staging_leaf);
        if (report.mutation_error == 0) {
            report.flags |= CHILD_FLAG_MUTATION_SUCCEEDED;
        }
        break;
    }
    case ChildScenario::direct_parent_mode_tamper_sandwich:
        replacement.kind = ReplacementHookContext::Kind::direct_parent_mode_tamper;
        replacement.leaf = "..";
        break;
    }

    if (!replacement.leaf.empty()) {
        replacement.root_descriptor =
            ::fcntl(process::DISTRIBUTED_SIEVE_WORKER_CHILD_WAVE_ROOT_DESCRIPTOR, F_DUPFD_CLOEXEC,
                    process::DISTRIBUTED_SIEVE_WORKER_CHILD_FIRST_UNMAPPED_DESCRIPTOR);
        if (replacement.root_descriptor < 0) {
            report.flags |= CHILD_FLAG_MUTATION_INVOKED;
            report.mutation_error = errno;
            (void)write_exact(STDOUT_FILENO, &report, sizeof(report));
            return 93;
        }
    }

    auto adopted = replacement.leaf.empty()
                       ? entry::adopt_distributed_sieve_worker_entry_v1()
                       : entry::trusted_test::adopt_distributed_sieve_worker_entry_v1_with_hooks({
                             .after_first_validation = replace_after_first_validation,
                             .context = &replacement,
                         });
    if (!replacement.leaf.empty()) {
        if (::close(replacement.root_descriptor) != 0 && replacement.error == 0) {
            replacement.error = errno;
        }
        replacement.root_descriptor = -1;
        report.flags |= CHILD_FLAG_MUTATION_INVOKED;
        report.mutation_error = replacement.error;
        if (replacement.invoked && replacement.replaced) {
            report.flags |= CHILD_FLAG_MUTATION_SUCCEEDED;
        }
    }
    if (all_fixed_descriptors_are_closed()) {
        report.flags |= CHILD_FLAG_FIXED_FDS_CLOSED;
    }
    if (descriptor_is_closed(STDIN_FILENO)) {
        report.flags |= CHILD_FLAG_STDIN_CLOSED;
    }

    report.first_status = static_cast<std::uint32_t>(adopted.diagnostic.status);
    report.first_phase = static_cast<std::uint32_t>(adopted.diagnostic.phase);
    if (adopted) {
        report.flags |= CHILD_FLAG_FIRST_SUCCESS;
        auto& token = *adopted.entry;
        if (token.valid()) {
            report.flags |= CHILD_FLAG_TOKEN_VALID;
        }
        const auto revalidated = token.revalidate();
        report.revalidate_status = static_cast<std::uint32_t>(revalidated.status);
        if (revalidated.status == entry::DistributedSieveWorkerEntryStatusV1::ready) {
            report.flags |= CHILD_FLAG_TOKEN_REVALIDATED;
        }
        report.chunk_id = token.record().chunk_id;
        report.attempt_ordinal = token.record().attempt_ordinal;
        report.attempt_digest = token.record().self_digest;
        report.manifest_digest = token.manifest().self_digest;
        const auto digest = sieve::distributed_sieve_work_digest(token.identity());
        if (digest && digest.digest.has_value()) {
            report.work_digest = *digest.digest;
        }
        if (token.record().manifest_digest == token.manifest().self_digest &&
            token.chunk().chunk_id == token.record().chunk_id &&
            token.chunk().sq_begin == token.record().sq_begin &&
            token.chunk().sq_end == token.record().sq_end && digest && digest.digest.has_value() &&
            *digest.digest == token.manifest().work_sha256 &&
            token.witness().work_sha256 == token.manifest().work_sha256) {
            report.flags |= CHILD_FLAG_BINDINGS_VALID;
        }

        const auto second = entry::adopt_distributed_sieve_worker_entry_v1();
        report.second_status = static_cast<std::uint32_t>(second.diagnostic.status);
        if (!second && second.diagnostic.status ==
                           entry::DistributedSieveWorkerEntryStatusV1::already_adopted) {
            report.flags |= CHILD_FLAG_SECOND_REJECTED;
        }

        const pid_t forked = ::fork();
        if (forked == 0) {
            const auto inherited = token.revalidate();
            ::_exit(inherited.status == entry::DistributedSieveWorkerEntryStatusV1::process_mismatch
                        ? 0
                        : 1);
        }
        if (forked > 0) {
            int status = 0;
            pid_t waited = -1;
            do {
                waited = ::waitpid(forked, &status, 0);
            } while (waited < 0 && errno == EINTR);
            if (waited == forked && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                report.flags |= CHILD_FLAG_FORK_REJECTED;
                report.fork_status = static_cast<std::uint32_t>(
                    entry::DistributedSieveWorkerEntryStatusV1::process_mismatch);
            }
        } else {
            report.mutation_error = errno;
        }

        const auto parent_revalidated = token.revalidate();
        if (parent_revalidated.status == entry::DistributedSieveWorkerEntryStatusV1::ready) {
            report.flags |= CHILD_FLAG_PARENT_STILL_VALID;
        }
    }

    if (!write_exact(STDOUT_FILENO, &report, sizeof(report))) {
        return 91;
    }
    return 0;
}

[[nodiscard]] ChildReport read_child_report(int descriptor) {
    ChildReport report;
    CHECK(read_exact(descriptor, &report, sizeof(report)));
    std::byte trailing{};
    ssize_t received = -1;
    do {
        received = ::read(descriptor, &trailing, 1);
    } while (received < 0 && errno == EINTR);
    CHECK(received == 0);
    CHECK(report.magic == CHILD_REPORT_MAGIC);
    return report;
}

#endif

struct LaunchedCaseResult final {
    ChildReport report;
    sieve::AttemptStartedV1 record;
};

[[nodiscard]] LaunchedCaseResult launch_case(WorkerEntryFixture& fixture,
                                             const std::filesystem::path& executable,
                                             ChildScenario scenario) {
#if defined(_WIN32)
    (void)fixture;
    (void)executable;
    (void)scenario;
    throw TestFailure("worker-entry self-exec fixture is unavailable on Windows");
#else
    auto receipt = fixture.start_receipt();
    const auto record = receipt.record();
    std::vector<launcher::DistributedSieveWorkerLaunchSlotV1> slots;
    slots.emplace_back(std::move(receipt), std::vector<std::string>{
                                               std::string(CHILD_ARGUMENT),
                                               std::to_string(static_cast<std::uint32_t>(scenario)),
                                           });
    launcher::DistributedSieveWorkerLaunchRequestV1 request(executable.string(), std::move(slots));
    auto launched = fixture.store().launch_worker_process_batch_v1(
        std::move(request), fixture.identity, fixture.frozen, fixture.polynomial,
        fixture.factor_base);
    CHECK(launched);
    CHECK(launched.children.size() == 1);
    CHECK(launched.children[0]);
    CHECK(launched.children[0].worker.has_value());
    auto& worker = *launched.children[0].worker;
    const auto waited = worker.wait_terminal();
    CHECK(waited.reaped);
    CHECK(waited.success);
    CHECK(waited.exit_status == 0);
    const int report_descriptor = worker.release_report_descriptor();
    CHECK(report_descriptor >= 0);
    const auto report = read_child_report(report_descriptor);
    CHECK(::close(report_descriptor) == 0);
    CHECK(report.scenario == static_cast<std::uint32_t>(scenario));
    return {
        .report = report,
        .record = record,
    };
#endif
}

void require_private_directory_has_only_owner(const WorkerEntryFixture& fixture) {
    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    const auto directory = fixture.root / names->private_directory_leaf;
    std::size_t children = 0;
    for (const auto& entry_path : std::filesystem::directory_iterator(directory)) {
        ++children;
        CHECK(entry_path.path().filename() ==
              std::string(wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF));
    }
    CHECK(children == 1);
}

void require_no_worker_outputs(const WorkerEntryFixture& fixture) {
    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    for (const auto& entry_path : std::filesystem::recursive_directory_iterator(fixture.root)) {
        const std::string leaf = entry_path.path().filename().string();
        CHECK(!std::string_view(leaf).ends_with(".relidx"));
        CHECK(!std::string_view(leaf).ends_with(".reldata"));
        CHECK(leaf.find("handoff") == std::string::npos);
        if (leaf.find("cleanup") != std::string::npos && leaf != names->base_lock_leaf) {
            fail("worker-entry produces no cleanup artifact", __LINE__, leaf);
        }
    }
}

void require_entry_input_descriptors_closed(const ChildReport& report) {
    CHECK((report.flags & CHILD_FLAG_STDIN_CLOSED) != 0U);
    CHECK((report.flags & CHILD_FLAG_FIXED_FDS_CLOSED) != 0U);
}

[[nodiscard]] std::string
foreign_staging_leaf_for(const wave::DistributedSieveWorkerAttemptNamesV1& names,
                         const sieve::AttemptStartedV1& attempt) {
    auto foreign_lease_id = attempt.lease.lease_id.limbs;
    foreign_lease_id[0] ^= 1U;
    if ((foreign_lease_id[0] | foreign_lease_id[1]) == 0) {
        foreign_lease_id[1] = 1;
    }
    CHECK(foreign_lease_id != attempt.lease.lease_id.limbs);
    std::string leaf = names.relative_lease_stem;
    leaf.append(wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_STAGING_TAG);
    leaf.append(private_lease::private_lease_id_hex(foreign_lease_id));
    return leaf;
}

void test_fixture_constructs_real_p8() {
    WorkerEntryFixture fixture("fixture");
    auto receipt = fixture.start_receipt();
    CHECK(receipt.owned_by_current_process());
    require_wave_ready(receipt.revalidate(), "revalidate real AttemptStarted receipt");
    CHECK(receipt.record().chunk_id == 0);
    CHECK(receipt.record().attempt_ordinal == 0);
    CHECK(receipt.record().manifest_digest == fixture.store().manifest_digest());
    CHECK(receipt.record().sq_begin == fixture.identity.distributed.chunks[0].sq_begin);
    CHECK(receipt.record().sq_end == fixture.identity.distributed.chunks[0].sq_end);
}

[[nodiscard]] bool test_explicit_platform_gate(const std::filesystem::path& executable) {
    if (process::distributed_sieve_worker_process_fixed_capability_close_all_supported()) {
        return true;
    }

    WorkerEntryFixture fixture("unsupported");
    auto receipt = fixture.start_receipt();
    std::vector<launcher::DistributedSieveWorkerLaunchSlotV1> slots;
    slots.emplace_back(
        std::move(receipt),
        std::vector<std::string>{std::string(CHILD_ARGUMENT),
                                 std::to_string(static_cast<std::uint32_t>(ChildScenario::happy))});
    launcher::DistributedSieveWorkerLaunchRequestV1 request(executable.string(), std::move(slots));
    auto rejected = fixture.store().launch_worker_process_batch_v1(
        std::move(request), fixture.identity, fixture.frozen, fixture.polynomial,
        fixture.factor_base);
    CHECK(!rejected);
    CHECK(rejected.diagnostic.phase ==
          launcher::DistributedSieveWorkerLaunchPhaseV1::process_preparation);
    CHECK(rejected.diagnostic.transport.error ==
          process::DistributedSieveWorkerProcessTransportError::platform_unavailable);
    CHECK(rejected.diagnostic.transport.native_error == ENOTSUP);
    CHECK(rejected.children.size() == 1);
    CHECK(!rejected.children[0].worker.has_value());
    require_private_directory_has_only_owner(fixture);
    require_no_worker_outputs(fixture);
    return false;
}

void test_happy_path(const std::filesystem::path& executable) {
    WorkerEntryFixture fixture("happy");
    const auto result = launch_case(fixture, executable, ChildScenario::happy);
    CHECK(result.report.first_status ==
          static_cast<std::uint32_t>(entry::DistributedSieveWorkerEntryStatusV1::ready));
    CHECK(result.report.revalidate_status ==
          static_cast<std::uint32_t>(entry::DistributedSieveWorkerEntryStatusV1::ready));
    CHECK(result.report.second_status ==
          static_cast<std::uint32_t>(entry::DistributedSieveWorkerEntryStatusV1::already_adopted));
    CHECK(result.report.fork_status ==
          static_cast<std::uint32_t>(entry::DistributedSieveWorkerEntryStatusV1::process_mismatch));
    require_entry_input_descriptors_closed(result.report);
    CHECK((result.report.flags & CHILD_HAPPY_FLAGS) == CHILD_HAPPY_FLAGS);
    CHECK(result.report.chunk_id == 0);
    CHECK(result.report.attempt_ordinal == 0);
    CHECK(result.report.attempt_digest == result.record.self_digest);
    CHECK(result.report.manifest_digest == fixture.store().manifest_digest());
    CHECK(result.report.work_digest == work_digest(fixture.identity));
    require_private_directory_has_only_owner(fixture);
    require_no_worker_outputs(fixture);
}

void test_each_missing_fixed_descriptor(const std::filesystem::path& executable) {
    constexpr std::array scenarios{
        ChildScenario::close_fd3,
        ChildScenario::close_fd4,
        ChildScenario::close_fd5,
        ChildScenario::close_fd6,
    };
    for (const auto scenario : scenarios) {
        WorkerEntryFixture fixture("closed-fd-" +
                                   std::to_string(static_cast<std::uint32_t>(scenario)));
        const auto result = launch_case(fixture, executable, scenario);
        CHECK((result.report.flags & CHILD_FLAG_MUTATION_INVOKED) != 0U);
        CHECK((result.report.flags & CHILD_FLAG_MUTATION_SUCCEEDED) != 0U);
        CHECK((result.report.flags & CHILD_FLAG_FIRST_SUCCESS) == 0U);
        require_entry_input_descriptors_closed(result.report);
        CHECK(result.report.first_status ==
              static_cast<std::uint32_t>(
                  entry::DistributedSieveWorkerEntryStatusV1::descriptor_unavailable));
        CHECK(result.report.first_phase ==
              static_cast<std::uint32_t>(
                  entry::DistributedSieveWorkerEntryPhaseV1::fixed_capability_capture));
        require_private_directory_has_only_owner(fixture);
        require_no_worker_outputs(fixture);
    }
}

void test_named_inode_fresh_open_is_not_lock_authority(const std::filesystem::path& executable) {
    struct FreshOpenCase final {
        ChildScenario scenario;
        entry::DistributedSieveWorkerEntryPhaseV1 phase;
    };
    constexpr std::array cases{
        FreshOpenCase{
            ChildScenario::fresh_open_fd4,
            entry::DistributedSieveWorkerEntryPhaseV1::permanent_lock_validation,
        },
        FreshOpenCase{
            ChildScenario::fresh_open_fd5,
            entry::DistributedSieveWorkerEntryPhaseV1::attempt_base_lock_validation,
        },
    };
    for (const auto& test_case : cases) {
        WorkerEntryFixture fixture("fresh-lock-open-" +
                                   std::to_string(static_cast<std::uint32_t>(test_case.scenario)));
        const auto result = launch_case(fixture, executable, test_case.scenario);
        CHECK((result.report.flags & CHILD_FLAG_MUTATION_INVOKED) != 0U);
        CHECK((result.report.flags & CHILD_FLAG_MUTATION_SUCCEEDED) != 0U);
        CHECK((result.report.flags & CHILD_FLAG_FIRST_SUCCESS) == 0U);
        require_entry_input_descriptors_closed(result.report);
        if (result.report.first_status !=
            static_cast<std::uint32_t>(entry::DistributedSieveWorkerEntryStatusV1::lock_invalid)) {
            fail("fresh named lock open lacks inherited lock authority", __LINE__,
                 "scenario=" + std::to_string(static_cast<std::uint32_t>(test_case.scenario)) +
                     " status=" + std::to_string(result.report.first_status) +
                     " phase=" + std::to_string(result.report.first_phase));
        }
        CHECK(result.report.first_phase == static_cast<std::uint32_t>(test_case.phase));
        require_private_directory_has_only_owner(fixture);
        require_no_worker_outputs(fixture);
    }
}

void test_package_policy_tamper(const std::filesystem::path& executable) {
    struct PackageTamperCase final {
        ChildScenario scenario;
        entry::DistributedSieveWorkerEntryStatusV1 status;
        entry::DistributedSieveWorkerEntryPhaseV1 phase;
    };
    constexpr std::array cases{
        PackageTamperCase{
            ChildScenario::package_mode_tamper,
            entry::DistributedSieveWorkerEntryStatusV1::work_package_invalid,
            entry::DistributedSieveWorkerEntryPhaseV1::work_package_validation,
        },
        PackageTamperCase{
            ChildScenario::package_flags_tamper,
            entry::DistributedSieveWorkerEntryStatusV1::descriptor_policy_invalid,
            entry::DistributedSieveWorkerEntryPhaseV1::fixed_capability_capture,
        },
    };
    for (const auto& test_case : cases) {
        WorkerEntryFixture fixture("package-tamper-" +
                                   std::to_string(static_cast<std::uint32_t>(test_case.scenario)));
        const auto result = launch_case(fixture, executable, test_case.scenario);
        CHECK((result.report.flags & CHILD_FLAG_MUTATION_INVOKED) != 0U);
        CHECK((result.report.flags & CHILD_FLAG_MUTATION_SUCCEEDED) != 0U);
        CHECK((result.report.flags & CHILD_FLAG_FIRST_SUCCESS) == 0U);
        require_entry_input_descriptors_closed(result.report);
        if (result.report.first_status != static_cast<std::uint32_t>(test_case.status)) {
            fail("package tamper returns expected status", __LINE__,
                 "scenario=" + std::to_string(static_cast<std::uint32_t>(test_case.scenario)) +
                     " status=" + std::to_string(result.report.first_status) +
                     " phase=" + std::to_string(result.report.first_phase));
        }
        CHECK(result.report.first_phase == static_cast<std::uint32_t>(test_case.phase));
        require_private_directory_has_only_owner(fixture);
        require_no_worker_outputs(fixture);
    }
}

void test_forged_ordinal_zero_predecessor(const std::filesystem::path& executable) {
    WorkerEntryFixture fixture("forged-ordinal-zero-predecessor");
    const auto result =
        launch_case(fixture, executable, ChildScenario::forged_ordinal_zero_predecessor);
    CHECK((result.report.flags & CHILD_FLAG_MUTATION_INVOKED) != 0U);
    CHECK((result.report.flags & CHILD_FLAG_MUTATION_SUCCEEDED) != 0U);
    CHECK(result.report.mutation_error == 0);
    CHECK((result.report.flags & CHILD_FLAG_FIRST_SUCCESS) == 0U);
    require_entry_input_descriptors_closed(result.report);
    if (result.report.first_status !=
            static_cast<std::uint32_t>(
                entry::DistributedSieveWorkerEntryStatusV1::protocol_invalid) ||
        result.report.first_phase !=
            static_cast<std::uint32_t>(
                entry::DistributedSieveWorkerEntryPhaseV1::attempt_validation)) {
        fail("forged ordinal-zero predecessor rejected at attempt validation", __LINE__,
             "status=" + std::to_string(result.report.first_status) +
                 " phase=" + std::to_string(result.report.first_phase));
    }
    CHECK(std::filesystem::exists(fixture.root / ".gnfs-test-displaced-ordinal-zero-attempt"));
    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    CHECK(std::filesystem::exists(fixture.root / names->canonical_record_leaf));
    require_private_directory_has_only_owner(fixture);
    require_no_worker_outputs(fixture);
}

void test_consistent_wrong_private_lease_base_path_digest(const std::filesystem::path& executable) {
    WorkerEntryFixture fixture("wrong-private-lease-base-path-digest");
    const auto result = launch_case(fixture, executable, ChildScenario::wrong_base_path_digest);
    CHECK((result.report.flags & CHILD_FLAG_MUTATION_INVOKED) != 0U);
    CHECK((result.report.flags & CHILD_FLAG_MUTATION_SUCCEEDED) != 0U);
    CHECK(result.report.mutation_error == 0);
    CHECK((result.report.flags & CHILD_FLAG_FIRST_SUCCESS) == 0U);
    require_entry_input_descriptors_closed(result.report);
    if (result.report.first_status !=
            static_cast<std::uint32_t>(
                entry::DistributedSieveWorkerEntryStatusV1::private_lease_invalid) ||
        result.report.first_phase !=
            static_cast<std::uint32_t>(
                entry::DistributedSieveWorkerEntryPhaseV1::private_lease_validation)) {
        fail("consistent wrong private-lease base-path digest rejected", __LINE__,
             "status=" + std::to_string(result.report.first_status) +
                 " phase=" + std::to_string(result.report.first_phase));
    }
    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    CHECK(std::filesystem::exists(fixture.root / names->reserved_leaf));
    CHECK(std::filesystem::exists(fixture.root / names->owned_leaf));
    require_private_directory_has_only_owner(fixture);
    require_no_worker_outputs(fixture);
}

void test_foreign_staging_residue_is_rejected_and_preserved(
    const std::filesystem::path& executable) {
    WorkerEntryFixture fixture("foreign-staging-residue");
    const auto result = launch_case(fixture, executable, ChildScenario::foreign_staging_residue);
    CHECK((result.report.flags & CHILD_FLAG_MUTATION_INVOKED) != 0U);
    CHECK((result.report.flags & CHILD_FLAG_MUTATION_SUCCEEDED) != 0U);
    CHECK(result.report.mutation_error == 0);
    CHECK((result.report.flags & CHILD_FLAG_FIRST_SUCCESS) == 0U);
    require_entry_input_descriptors_closed(result.report);
    if (result.report.first_status !=
            static_cast<std::uint32_t>(
                entry::DistributedSieveWorkerEntryStatusV1::private_lease_invalid) ||
        result.report.first_phase !=
            static_cast<std::uint32_t>(
                entry::DistributedSieveWorkerEntryPhaseV1::private_lease_validation)) {
        fail("foreign same-attempt staging residue rejected", __LINE__,
             "status=" + std::to_string(result.report.first_status) +
                 " phase=" + std::to_string(result.report.first_phase));
    }
    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    const auto staging_leaf = foreign_staging_leaf_for(*names, result.record);
    const auto staging_directory = fixture.root / staging_leaf;
    CHECK(std::filesystem::is_directory(staging_directory));
    CHECK(std::filesystem::is_empty(staging_directory));
    require_private_directory_has_only_owner(fixture);
    require_no_worker_outputs(fixture);
}

void test_direct_parent_mode_tamper_sandwich(const std::filesystem::path& executable) {
    WorkerEntryFixture fixture("direct-parent-mode-tamper");
    const auto direct_parent = fixture.root.parent_path();
    const auto before_permissions = std::filesystem::status(direct_parent).permissions();
    CHECK((before_permissions & std::filesystem::perms::group_write) ==
          std::filesystem::perms::none);
    CHECK((before_permissions & std::filesystem::perms::others_write) ==
          std::filesystem::perms::none);

    const auto result =
        launch_case(fixture, executable, ChildScenario::direct_parent_mode_tamper_sandwich);
    CHECK((result.report.flags & CHILD_FLAG_MUTATION_INVOKED) != 0U);
    CHECK((result.report.flags & CHILD_FLAG_MUTATION_SUCCEEDED) != 0U);
    CHECK(result.report.mutation_error == 0);
    CHECK((result.report.flags & CHILD_FLAG_FIRST_SUCCESS) == 0U);
    require_entry_input_descriptors_closed(result.report);
    if (result.report.first_status !=
            static_cast<std::uint32_t>(
                entry::DistributedSieveWorkerEntryStatusV1::namespace_invalid) ||
        result.report.first_phase !=
            static_cast<std::uint32_t>(
                entry::DistributedSieveWorkerEntryPhaseV1::final_revalidation)) {
        fail("direct-parent mode tamper rejected during final revalidation", __LINE__,
             "status=" + std::to_string(result.report.first_status) +
                 " phase=" + std::to_string(result.report.first_phase));
    }

    const auto after_permissions = std::filesystem::status(direct_parent).permissions();
    CHECK((after_permissions & std::filesystem::perms::all) ==
          (std::filesystem::perms::owner_all | std::filesystem::perms::group_all));
    require_private_directory_has_only_owner(fixture);
    require_no_worker_outputs(fixture);
}

void test_replacement_sandwiches(const std::filesystem::path& executable) {
    struct ReplacementCase final {
        ChildScenario scenario;
        entry::DistributedSieveWorkerEntryStatusV1 status;
        std::string_view displaced_leaf;
    };
    constexpr std::array cases{
        ReplacementCase{
            ChildScenario::manifest_replacement_sandwich,
            entry::DistributedSieveWorkerEntryStatusV1::namespace_invalid,
            ".gnfs-test-displaced-manifest",
        },
        ReplacementCase{
            ChildScenario::attempt_record_replacement_sandwich,
            entry::DistributedSieveWorkerEntryStatusV1::namespace_invalid,
            ".gnfs-test-displaced-attempt",
        },
        ReplacementCase{
            ChildScenario::base_lock_replacement_sandwich,
            entry::DistributedSieveWorkerEntryStatusV1::lock_invalid,
            ".gnfs-test-displaced-base-lock",
        },
    };
    for (const auto& test_case : cases) {
        WorkerEntryFixture fixture("replacement-" +
                                   std::to_string(static_cast<std::uint32_t>(test_case.scenario)));
        const auto result = launch_case(fixture, executable, test_case.scenario);
        CHECK((result.report.flags & CHILD_FLAG_MUTATION_INVOKED) != 0U);
        CHECK((result.report.flags & CHILD_FLAG_MUTATION_SUCCEEDED) != 0U);
        CHECK(result.report.mutation_error == 0);
        if ((result.report.flags & CHILD_FLAG_FIRST_SUCCESS) != 0U) {
            fail("replacement sandwich fails closed", __LINE__,
                 "scenario=" + std::to_string(static_cast<std::uint32_t>(test_case.scenario)) +
                     " status=" + std::to_string(result.report.first_status) +
                     " phase=" + std::to_string(result.report.first_phase));
        }
        require_entry_input_descriptors_closed(result.report);
        CHECK(result.report.first_status == static_cast<std::uint32_t>(test_case.status));
        CHECK(result.report.first_phase ==
              static_cast<std::uint32_t>(
                  entry::DistributedSieveWorkerEntryPhaseV1::final_revalidation));
        CHECK(std::filesystem::exists(fixture.root / test_case.displaced_leaf));
        require_private_directory_has_only_owner(fixture);
        require_no_worker_outputs(fixture);
    }
}

void test_private_lease_replacement_and_pending_matrix(const std::filesystem::path& executable) {
    constexpr std::array scenarios{
        ChildScenario::private_directory_replacement_sandwich,
        ChildScenario::owner_replacement_sandwich,
        ChildScenario::owned_replacement_sandwich,
        ChildScenario::owned_pending_conflict_sandwich,
    };
    for (const auto scenario : scenarios) {
        WorkerEntryFixture fixture("private-lease-mutation-" +
                                   std::to_string(static_cast<std::uint32_t>(scenario)));
        const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
        CHECK(names.has_value());
        const auto result = launch_case(fixture, executable, scenario);
        CHECK((result.report.flags & CHILD_FLAG_MUTATION_INVOKED) != 0U);
        CHECK((result.report.flags & CHILD_FLAG_MUTATION_SUCCEEDED) != 0U);
        CHECK(result.report.mutation_error == 0);
        CHECK((result.report.flags & CHILD_FLAG_FIRST_SUCCESS) == 0U);
        require_entry_input_descriptors_closed(result.report);
        CHECK(result.report.first_status ==
              static_cast<std::uint32_t>(
                  entry::DistributedSieveWorkerEntryStatusV1::private_lease_invalid));
        CHECK(result.report.first_phase ==
              static_cast<std::uint32_t>(
                  entry::DistributedSieveWorkerEntryPhaseV1::final_revalidation));

        switch (scenario) {
        case ChildScenario::private_directory_replacement_sandwich:
            CHECK(std::filesystem::is_directory(fixture.root / names->private_directory_leaf));
            CHECK(std::filesystem::is_directory(fixture.root /
                                                ".gnfs-test-displaced-private-directory"));
            break;
        case ChildScenario::owner_replacement_sandwich:
            CHECK(std::filesystem::exists(fixture.root / names->private_directory_leaf /
                                          wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF));
            CHECK(std::filesystem::exists(fixture.root / names->private_directory_leaf /
                                          ".gnfs-test-displaced-owner"));
            break;
        case ChildScenario::owned_replacement_sandwich:
            CHECK(std::filesystem::exists(fixture.root / names->owned_leaf));
            CHECK(std::filesystem::exists(fixture.root / ".gnfs-test-displaced-owned"));
            require_private_directory_has_only_owner(fixture);
            break;
        case ChildScenario::owned_pending_conflict_sandwich:
            CHECK(std::filesystem::exists(fixture.root / names->owned_leaf));
            CHECK(std::filesystem::exists(fixture.root / names->owned_pending_leaf));
            require_private_directory_has_only_owner(fixture);
            break;
        default:
            fail("closed private-lease mutation scenario", __LINE__);
        }
        require_no_worker_outputs(fixture);
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
        (void)argc;
        (void)argv;
        const auto unsupported = entry::adopt_distributed_sieve_worker_entry_v1();
        CHECK(!unsupported);
        CHECK(!unsupported.entry.has_value());
        CHECK(unsupported.diagnostic.phase ==
              entry::DistributedSieveWorkerEntryPhaseV1::platform_gate);
        CHECK(unsupported.diagnostic.status ==
              entry::DistributedSieveWorkerEntryStatusV1::platform_unsupported);
        std::cout << "distributed sieve worker-entry explicitly unsupported on this platform\n";
        return 0;
#else
        if (argc == 3 && std::string_view(argv[1]) == CHILD_ARGUMENT) {
            const auto scenario = parse_child_scenario(argv[2]);
            return scenario.has_value() ? run_child(*scenario) : 92;
        }

        CHECK(argc >= 1);
        const auto executable = self_executable_path(argv[0]);
        test_fixture_constructs_real_p8();
        if (!test_explicit_platform_gate(executable)) {
            std::cout << "distributed sieve worker-entry explicitly unsupported on this platform\n";
            return 0;
        }
        test_happy_path(executable);
        test_each_missing_fixed_descriptor(executable);
        test_named_inode_fresh_open_is_not_lock_authority(executable);
        test_package_policy_tamper(executable);
        test_forged_ordinal_zero_predecessor(executable);
        test_consistent_wrong_private_lease_base_path_digest(executable);
        test_foreign_staging_residue_is_rejected_and_preserved(executable);
        test_direct_parent_mode_tamper_sandwich(executable);
        test_replacement_sandwiches(executable);
        test_private_lease_replacement_and_pending_matrix(executable);
        std::cout << "distributed sieve worker-entry tests passed\n";
        return 0;
#endif
    } catch (const std::exception& error) {
        std::cerr << "distributed sieve worker-entry test failed: " << error.what() << '\n';
        return 1;
    }
}

#include <gnfs/util/durable_immutable_record.hpp>
#include <gnfs/util/process.hpp>
#include <gnfs/util/temp_path.hpp>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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

namespace {

namespace durable_file = gnfs::util::durable_immutable_file;
namespace durable_record = gnfs::util::durable_immutable_record;

static_assert(!std::is_default_constructible_v<durable_record::InspectResult>);
static_assert(!std::is_default_constructible_v<durable_record::MutationResult>);
static_assert(!std::is_default_constructible_v<durable_record::RecordPublishResult>);
static_assert(!std::is_constructible_v<
              durable_record::RecordPublishResult, durable_record::RecordPublishStatus,
              durable_record::RecordPublishDisposition,
              std::optional<durable_record::RecordSnapshot>, std::error_code>);

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
    throw TestFailure(message);
}

void check(bool condition, std::string_view expression, int line) {
    if (!condition) {
        fail(expression, line);
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

constexpr std::array<std::byte, 6> PAYLOAD = {
    std::byte{0x10}, std::byte{0x20}, std::byte{0x30},
    std::byte{0x40}, std::byte{0x50}, std::byte{0x60},
};
constexpr std::array<std::byte, 6> OTHER_PAYLOAD = {
    std::byte{0x61}, std::byte{0x62}, std::byte{0x63},
    std::byte{0x64}, std::byte{0x65}, std::byte{0x66},
};

[[nodiscard]] std::error_code injected_error() {
    return std::make_error_code(std::errc::io_error);
}

class TempDirectory final {
public:
    TempDirectory() {
        path_ = gnfs::util::temp_path("gnfs_durable_record_" +
                                      std::to_string(gnfs::util::process_id()) + "_" +
                                      std::to_string(++sequence_));
        std::error_code error;
        const bool created = std::filesystem::create_directory(path_, error);
        if (!created || error) {
            throw TestFailure("cannot create temporary directory: " + error.message());
        }
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
    inline static unsigned sequence_ = 0;
    std::filesystem::path path_;
};

class ScriptedRecordOps final : public durable_record::RecordOps {
public:
    durable_record::NativeHandle expected_parent = 73;
    durable_record::MutationResult probe_result = durable_record::MutationResult::succeeded();
    std::vector<durable_record::InspectResult> inspections;
    std::optional<durable_file::PublishResult> pending_publish_result;
    std::optional<durable_record::MutationResult> promotion_result;
    std::optional<durable_file::OperationResult> parent_sync_result;
    std::optional<durable_file::PublishResult> canonical_confirm_result;
    std::optional<durable_record::MutationResult> removal_result;

    std::size_t probe_calls = 0;
    std::size_t publish_calls = 0;
    std::size_t inspect_calls = 0;
    std::size_t promote_calls = 0;
    std::size_t parent_sync_calls = 0;
    std::size_t confirm_calls = 0;
    std::size_t remove_calls = 0;
    bool parent_mismatch = false;

    [[nodiscard]] durable_record::MutationResult
    probe_no_replace_at(durable_record::NativeHandle parent_handle) noexcept override {
        ++probe_calls;
        observe_parent(parent_handle);
        return probe_result;
    }

    [[nodiscard]] durable_file::PublishResult
    publish_pending_at(durable_record::NativeHandle parent_handle,
                       const std::filesystem::path& pending_leaf,
                       std::span<const std::byte> expected_bytes) noexcept override {
        ++publish_calls;
        observe_parent(parent_handle);
        if (pending_leaf.has_parent_path()) {
            parent_mismatch = true;
        }
        if (pending_publish_result.has_value()) {
            return *pending_publish_result;
        }
        return durable_file::PublishResult(durable_file::PublishStatus::durable, {},
                                           static_cast<std::uint64_t>(expected_bytes.size()));
    }

    [[nodiscard]] durable_record::InspectResult
    inspect_exact_at(durable_record::NativeHandle parent_handle, const std::filesystem::path& leaf,
                     std::span<const std::byte>) noexcept override {
        observe_parent(parent_handle);
        if (leaf.has_parent_path()) {
            parent_mismatch = true;
        }
        const std::size_t index = inspect_calls++;
        if (index < inspections.size()) {
            return inspections[index];
        }
        return durable_record::InspectResult::missing();
    }

    [[nodiscard]] durable_record::MutationResult
    promote_no_replace_at(durable_record::NativeHandle parent_handle,
                          const std::filesystem::path& pending_leaf,
                          const std::filesystem::path& canonical_leaf,
                          const durable_record::RecordSnapshot&) noexcept override {
        ++promote_calls;
        observe_parent(parent_handle);
        if (pending_leaf.has_parent_path() || canonical_leaf.has_parent_path()) {
            parent_mismatch = true;
        }
        if (promotion_result.has_value()) {
            return *promotion_result;
        }
        return durable_record::MutationResult::succeeded();
    }

    [[nodiscard]] durable_file::OperationResult
    sync_parent_directory(durable_record::NativeHandle parent_handle) noexcept override {
        ++parent_sync_calls;
        observe_parent(parent_handle);
        if (parent_sync_result.has_value()) {
            return *parent_sync_result;
        }
        return durable_file::OperationResult::succeeded();
    }

    [[nodiscard]] durable_file::PublishResult
    confirm_durable_at(durable_record::NativeHandle parent_handle,
                       const std::filesystem::path& canonical_leaf) noexcept override {
        ++confirm_calls;
        observe_parent(parent_handle);
        if (canonical_leaf.has_parent_path()) {
            parent_mismatch = true;
        }
        if (canonical_confirm_result.has_value()) {
            return *canonical_confirm_result;
        }
        return durable_file::PublishResult(durable_file::PublishStatus::durable, {}, 0);
    }

    [[nodiscard]] durable_record::MutationResult
    remove_exact_at(durable_record::NativeHandle parent_handle,
                    const std::filesystem::path& pending_leaf,
                    const durable_record::RecordSnapshot&) noexcept override {
        ++remove_calls;
        observe_parent(parent_handle);
        if (pending_leaf.has_parent_path()) {
            parent_mismatch = true;
        }
        if (removal_result.has_value()) {
            return *removal_result;
        }
        return durable_record::MutationResult::succeeded();
    }

    [[nodiscard]] std::size_t calls_after_probe() const noexcept {
        return publish_calls + inspect_calls + promote_calls + parent_sync_calls + confirm_calls +
               remove_calls;
    }

private:
    void observe_parent(durable_record::NativeHandle parent_handle) noexcept {
        parent_mismatch = parent_mismatch || parent_handle != expected_parent;
    }
};

void test_invalid_requests_stop_before_ops() {
    constexpr durable_record::NativeHandle PARENT = 73;

    const auto require_invalid = [&](const std::filesystem::path& pending,
                                     const std::filesystem::path& canonical,
                                     durable_record::NativeHandle parent = PARENT) {
        ScriptedRecordOps ops;
        const auto result =
            durable_record::publish_at_with_ops(parent, pending, canonical, PAYLOAD, ops);
        CHECK(result.status() == durable_record::RecordPublishStatus::invalid_request);
        CHECK(!result.is_durable());
        CHECK(!result.canonical_snapshot().has_value());
        CHECK(ops.probe_calls == 0);
        CHECK(ops.calls_after_probe() == 0);
    };

    require_invalid("record.pending", "record.pending");
    require_invalid("Record.Pending", "record.pending");
    require_invalid("nested/record.pending", "record.bin");
    require_invalid("record.pending", "nested/record.bin");
    require_invalid(".", "record.bin");
    require_invalid("record.pending", "..");
    require_invalid("", "record.bin");
    require_invalid("record.pending", "record.bin", durable_record::INVALID_NATIVE_HANDLE);

    const std::filesystem::path nul_pending{std::string("bad\0pending", 11)};
    const std::filesystem::path nul_canonical{std::string("bad\0canonical", 13)};
    require_invalid(nul_pending, "record.bin");
    require_invalid("record.pending", nul_canonical);
}

void test_probe_fails_closed_before_mutation() {
    {
        ScriptedRecordOps ops;
        ops.probe_result = durable_record::MutationResult::unsupported(
            std::make_error_code(std::errc::operation_not_supported));
        const auto result = durable_record::publish_at_with_ops(
            ops.expected_parent, "record.pending", "record.bin", PAYLOAD, ops);
        CHECK(result.status() == durable_record::RecordPublishStatus::platform_unsupported);
        CHECK(ops.probe_calls == 1);
        CHECK(ops.calls_after_probe() == 0);
        CHECK(!ops.parent_mismatch);
    }
    {
        ScriptedRecordOps ops;
        ops.probe_result = durable_record::MutationResult::source_missing(
            std::make_error_code(std::errc::no_such_file_or_directory));
        const auto result = durable_record::publish_at_with_ops(
            ops.expected_parent, "record.pending", "record.bin", PAYLOAD, ops);
        CHECK(result.status() == durable_record::RecordPublishStatus::ops_contract_violation);
        CHECK(ops.probe_calls == 1);
        CHECK(ops.calls_after_probe() == 0);
        CHECK(!ops.parent_mismatch);
    }
    {
        ScriptedRecordOps ops;
        ops.probe_result = durable_record::MutationResult::identity_mismatch(injected_error());
        const auto result = durable_record::publish_at_with_ops(
            ops.expected_parent, "record.pending", "record.bin", PAYLOAD, ops);
        CHECK(result.status() == durable_record::RecordPublishStatus::invalid_request);
        CHECK(ops.probe_calls == 1);
        CHECK(ops.calls_after_probe() == 0);
        CHECK(!ops.parent_mismatch);
    }
}

#ifndef _WIN32

class ParentDirectory final {
public:
    explicit ParentDirectory(const std::filesystem::path& path) {
        handle_ = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (handle_ < 0) {
            throw TestFailure("cannot open temporary parent: " +
                              std::error_code(errno, std::generic_category()).message());
        }
    }

    ParentDirectory(const ParentDirectory&) = delete;
    ParentDirectory& operator=(const ParentDirectory&) = delete;

    ~ParentDirectory() {
        if (handle_ >= 0) {
            (void)::close(handle_);
        }
    }

    [[nodiscard]] int fd() const noexcept {
        return handle_;
    }

    [[nodiscard]] durable_record::NativeHandle native_handle() const noexcept {
        return static_cast<durable_record::NativeHandle>(handle_);
    }

    [[nodiscard]] bool is_open() const noexcept {
        return ::fcntl(handle_, F_GETFD) != -1;
    }

private:
    int handle_ = -1;
};

class ScopedUmask final {
public:
    explicit ScopedUmask(mode_t value) noexcept : previous_(::umask(value)) {}

    ScopedUmask(const ScopedUmask&) = delete;
    ScopedUmask& operator=(const ScopedUmask&) = delete;

    ~ScopedUmask() {
        (void)::umask(previous_);
    }

private:
    mode_t previous_;
};

void sync_parent(int parent_fd) {
    if (::fsync(parent_fd) != 0) {
        throw TestFailure("cannot sync temporary parent");
    }
}

void write_leaf(int parent_fd, std::string_view leaf, std::span<const std::byte> bytes,
                mode_t mode = 0600) {
    const std::string name{leaf};
    const int fd = ::openat(parent_fd, name.c_str(),
                            O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        throw TestFailure("cannot create fixture leaf: " +
                          std::error_code(errno, std::generic_category()).message());
    }

    bool succeeded = true;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto* data = reinterpret_cast<const char*>(bytes.data() + offset);
        const std::size_t remaining = bytes.size() - offset;
        const ssize_t written = ::write(fd, data, remaining);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        succeeded = false;
        break;
    }
    if (succeeded && ::fchmod(fd, mode) != 0) {
        succeeded = false;
    }
    if (succeeded && ::fsync(fd) != 0) {
        succeeded = false;
    }
    const int close_result = ::close(fd);
    if (!succeeded || close_result != 0) {
        throw TestFailure("cannot finalize fixture leaf");
    }
    sync_parent(parent_fd);
}

[[nodiscard]] bool leaf_exists(int parent_fd, std::string_view leaf) {
    const std::string name{leaf};
    struct stat status{};
    return ::fstatat(parent_fd, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0;
}

[[nodiscard]] mode_t leaf_mode(int parent_fd, std::string_view leaf) {
    const std::string name{leaf};
    struct stat status{};
    if (::fstatat(parent_fd, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        throw TestFailure("cannot stat fixture leaf");
    }
    return status.st_mode & 07777;
}

[[nodiscard]] std::vector<std::byte> read_leaf(int parent_fd, std::string_view leaf) {
    const std::string name{leaf};
    const int fd = ::openat(parent_fd, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        throw TestFailure("cannot open fixture leaf for reading");
    }
    struct stat status{};
    if (::fstat(fd, &status) != 0 || status.st_size < 0) {
        (void)::close(fd);
        throw TestFailure("cannot stat fixture leaf for reading");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(status.st_size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        auto* data = reinterpret_cast<char*>(bytes.data() + offset);
        const ssize_t count = ::read(fd, data, bytes.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        (void)::close(fd);
        throw TestFailure("cannot read fixture leaf");
    }
    if (::close(fd) != 0) {
        throw TestFailure("cannot close fixture leaf");
    }
    return bytes;
}

void require_preserved(int parent_fd, std::string_view leaf, std::span<const std::byte> expected) {
    CHECK(leaf_exists(parent_fd, leaf));
    CHECK(read_leaf(parent_fd, leaf) == std::vector<std::byte>(expected.begin(), expected.end()));
}

void test_real_casefold_alias_rejected_before_mutation() {
    TempDirectory directory;
    ParentDirectory parent(directory.path());
    const auto result = durable_record::publish_at(parent.native_handle(), "Record.Pending",
                                                   "record.pending", PAYLOAD);
    CHECK(result.status() == durable_record::RecordPublishStatus::invalid_request);
    CHECK(!leaf_exists(parent.fd(), "Record.Pending"));
    CHECK(!leaf_exists(parent.fd(), "record.pending"));
    CHECK(std::filesystem::is_empty(directory.path()));
    CHECK(parent.is_open());
}

void test_parent_descriptor_policy_preflight() {
    for (const mode_t mode : {static_cast<mode_t>(0770), static_cast<mode_t>(0707)}) {
        TempDirectory directory;
        CHECK(::chmod(directory.path().c_str(), mode) == 0);
        ParentDirectory parent(directory.path());
        const auto result = durable_record::publish_at(parent.native_handle(), "record.pending",
                                                       "record.bin", PAYLOAD);
        CHECK(result.status() == durable_record::RecordPublishStatus::invalid_request);
        CHECK(std::filesystem::is_empty(directory.path()));
        CHECK(parent.is_open());
    }

    for (const mode_t mode : {static_cast<mode_t>(0700), static_cast<mode_t>(0755)}) {
        TempDirectory directory;
        CHECK(::chmod(directory.path().c_str(), mode) == 0);
        ParentDirectory parent(directory.path());
        const auto result = durable_record::publish_at(parent.native_handle(), "record.pending",
                                                       "record.bin", PAYLOAD);
        CHECK(result.status() == durable_record::RecordPublishStatus::durable);
        CHECK(!leaf_exists(parent.fd(), "record.pending"));
        require_preserved(parent.fd(), "record.bin", PAYLOAD);
        CHECK(parent.is_open());
    }
}

void test_fresh_publish_replay_and_umask() {
    TempDirectory directory;
    ParentDirectory parent(directory.path());
    write_leaf(parent.fd(), "foreign.keep", OTHER_PAYLOAD);

    durable_record::RecordPublishResult fresh =
        durable_record::publish_at(parent.native_handle(), "record.pending", "record.bin", PAYLOAD);
    CHECK(fresh.status() == durable_record::RecordPublishStatus::durable);
    CHECK(fresh.disposition() == durable_record::RecordPublishDisposition::created);
    CHECK(fresh.canonical_snapshot().has_value());
    CHECK(fresh.canonical_snapshot()->size == PAYLOAD.size());
    CHECK(!leaf_exists(parent.fd(), "record.pending"));
    require_preserved(parent.fd(), "record.bin", PAYLOAD);
    require_preserved(parent.fd(), "foreign.keep", OTHER_PAYLOAD);
    CHECK(parent.is_open());

    const auto original_snapshot = *fresh.canonical_snapshot();
    const auto replay =
        durable_record::publish_at(parent.native_handle(), "record.pending", "record.bin", PAYLOAD);
    CHECK(replay.status() == durable_record::RecordPublishStatus::durable);
    CHECK(replay.disposition() == durable_record::RecordPublishDisposition::confirmed_existing);
    CHECK(replay.canonical_snapshot() == fresh.canonical_snapshot());
    CHECK(*replay.canonical_snapshot() == original_snapshot);
    CHECK(!leaf_exists(parent.fd(), "record.pending"));
    require_preserved(parent.fd(), "foreign.keep", OTHER_PAYLOAD);
    CHECK(parent.is_open());

    TempDirectory umask_directory;
    ParentDirectory umask_parent(umask_directory.path());
    const mode_t original_umask = ::umask(0);
    (void)::umask(original_umask);
    {
        ScopedUmask restrictive_umask{0777};
        const auto result = durable_record::publish_at(umask_parent.native_handle(),
                                                       "umask.pending", "umask.bin", PAYLOAD);
        CHECK(result.status() == durable_record::RecordPublishStatus::durable);
    }
    const mode_t restored_umask = ::umask(original_umask);
    CHECK(restored_umask == original_umask);
    CHECK(leaf_mode(umask_parent.fd(), "umask.bin") == 0600);
    CHECK(umask_parent.is_open());
}

void test_pending_recovery_and_matching_convergence() {
    {
        TempDirectory directory;
        ParentDirectory parent(directory.path());
        write_leaf(parent.fd(), "record.pending", PAYLOAD);
        write_leaf(parent.fd(), "foreign.keep", OTHER_PAYLOAD);

        const auto recovered = durable_record::publish_at(parent.native_handle(), "record.pending",
                                                          "record.bin", PAYLOAD);
        CHECK(recovered.status() == durable_record::RecordPublishStatus::durable);
        CHECK(recovered.disposition() ==
              durable_record::RecordPublishDisposition::recovered_pending);
        CHECK(!leaf_exists(parent.fd(), "record.pending"));
        require_preserved(parent.fd(), "record.bin", PAYLOAD);
        require_preserved(parent.fd(), "foreign.keep", OTHER_PAYLOAD);
        CHECK(parent.is_open());
    }
    {
        TempDirectory directory;
        ParentDirectory parent(directory.path());
        const auto created = durable_record::publish_at(parent.native_handle(), "record.pending",
                                                        "record.bin", PAYLOAD);
        CHECK(created.is_durable());
        write_leaf(parent.fd(), "record.pending", PAYLOAD);
        write_leaf(parent.fd(), "foreign.keep", OTHER_PAYLOAD);

        const auto converged = durable_record::publish_at(parent.native_handle(), "record.pending",
                                                          "record.bin", PAYLOAD);
        CHECK(converged.status() == durable_record::RecordPublishStatus::durable);
        CHECK(converged.disposition() ==
              durable_record::RecordPublishDisposition::confirmed_existing);
        CHECK(converged.canonical_snapshot() == created.canonical_snapshot());
        CHECK(!leaf_exists(parent.fd(), "record.pending"));
        require_preserved(parent.fd(), "foreign.keep", OTHER_PAYLOAD);
        CHECK(parent.is_open());
    }
}

enum class RecordRole {
    pending,
    canonical,
};

[[nodiscard]] std::string_view role_leaf(RecordRole role) {
    return role == RecordRole::pending ? "record.pending" : "record.bin";
}

[[nodiscard]] durable_record::RecordPublishStatus conflict_status(RecordRole role) {
    return role == RecordRole::pending ? durable_record::RecordPublishStatus::pending_conflict
                                       : durable_record::RecordPublishStatus::canonical_conflict;
}

void require_conflict_preserved(RecordRole role, std::span<const std::byte> bytes) {
    TempDirectory directory;
    ParentDirectory parent(directory.path());
    write_leaf(parent.fd(), role_leaf(role), bytes);
    write_leaf(parent.fd(), "foreign.keep", OTHER_PAYLOAD);

    const auto result =
        durable_record::publish_at(parent.native_handle(), "record.pending", "record.bin", PAYLOAD);
    CHECK(result.status() == conflict_status(role));
    CHECK(!result.is_durable());
    require_preserved(parent.fd(), role_leaf(role), bytes);
    require_preserved(parent.fd(), "foreign.keep", OTHER_PAYLOAD);
    CHECK(parent.is_open());
}

void test_wrong_bytes_sizes_and_modes_are_preserved() {
    std::vector<std::byte> wrong_size(PAYLOAD.begin(), PAYLOAD.end());
    wrong_size.push_back(std::byte{0x70});
    for (const auto role : {RecordRole::pending, RecordRole::canonical}) {
        require_conflict_preserved(role, OTHER_PAYLOAD);
        require_conflict_preserved(role, wrong_size);
    }

    constexpr std::array<mode_t, 5> INVALID_MODES = {
        0400, 0700, 04600, 02600, 01600,
    };
    for (const auto role : {RecordRole::pending, RecordRole::canonical}) {
        for (const mode_t mode : INVALID_MODES) {
            TempDirectory directory;
            ParentDirectory parent(directory.path());
            write_leaf(parent.fd(), role_leaf(role), PAYLOAD, mode);
            CHECK(leaf_mode(parent.fd(), role_leaf(role)) == mode);

            const auto result = durable_record::publish_at(parent.native_handle(), "record.pending",
                                                           "record.bin", PAYLOAD);
            CHECK(result.status() == conflict_status(role));
            CHECK(leaf_exists(parent.fd(), role_leaf(role)));
            CHECK(leaf_mode(parent.fd(), role_leaf(role)) == mode);
            CHECK(parent.is_open());
        }
    }
}

enum class MalformedLeafKind {
    symlink,
    hardlink,
    directory,
};

void test_symlink_hardlink_and_nonregular_are_preserved() {
    for (const auto role : {RecordRole::pending, RecordRole::canonical}) {
        for (const auto kind : {MalformedLeafKind::symlink, MalformedLeafKind::hardlink,
                                MalformedLeafKind::directory}) {
            TempDirectory directory;
            ParentDirectory parent(directory.path());
            write_leaf(parent.fd(), "foreign.keep", PAYLOAD);
            const std::string leaf{role_leaf(role)};

            int result = -1;
            switch (kind) {
            case MalformedLeafKind::symlink:
                result = ::symlinkat("foreign.keep", parent.fd(), leaf.c_str());
                break;
            case MalformedLeafKind::hardlink:
                result = ::linkat(parent.fd(), "foreign.keep", parent.fd(), leaf.c_str(), 0);
                break;
            case MalformedLeafKind::directory:
                result = ::mkdirat(parent.fd(), leaf.c_str(), 0700);
                break;
            }
            CHECK(result == 0);
            sync_parent(parent.fd());

            const auto published = durable_record::publish_at(
                parent.native_handle(), "record.pending", "record.bin", PAYLOAD);
            CHECK(published.status() == conflict_status(role));
            CHECK(!published.is_durable());
            CHECK(leaf_exists(parent.fd(), leaf));
            require_preserved(parent.fd(), "foreign.keep", PAYLOAD);
            CHECK(parent.is_open());
        }
    }
}

struct StopContext final {
    durable_record::RecordFaultPoint target = durable_record::RecordFaultPoint::PendingDurable;
    std::size_t observations = 0;
    bool triggered = false;
};

[[nodiscard]] bool stop_at_fault(durable_record::RecordFaultPoint point, void* opaque) noexcept {
    auto& context = *static_cast<StopContext*>(opaque);
    ++context.observations;
    if (point == context.target) {
        context.triggered = true;
        return true;
    }
    return false;
}

void test_all_fault_points_recover() {
    for (const auto point : {durable_record::RecordFaultPoint::PendingDurable,
                             durable_record::RecordFaultPoint::CanonicalPromoted,
                             durable_record::RecordFaultPoint::CanonicalDurable}) {
        TempDirectory directory;
        ParentDirectory parent(directory.path());
        write_leaf(parent.fd(), "foreign.keep", OTHER_PAYLOAD);
        StopContext context;
        context.target = point;
        const durable_record::RecordTestHooks hooks{stop_at_fault, &context};

        const auto interrupted = durable_record::publish_at(
            parent.native_handle(), "record.pending", "record.bin", PAYLOAD, hooks);
        CHECK(interrupted.status() == durable_record::RecordPublishStatus::interrupted);
        CHECK(!interrupted.is_durable());
        CHECK(context.triggered);
        CHECK(parent.is_open());
        require_preserved(parent.fd(), "foreign.keep", OTHER_PAYLOAD);

        if (point == durable_record::RecordFaultPoint::PendingDurable) {
            CHECK(leaf_exists(parent.fd(), "record.pending"));
            CHECK(!leaf_exists(parent.fd(), "record.bin"));
        } else {
            CHECK(!leaf_exists(parent.fd(), "record.pending"));
            CHECK(leaf_exists(parent.fd(), "record.bin"));
        }

        const auto recovered = durable_record::publish_at(parent.native_handle(), "record.pending",
                                                          "record.bin", PAYLOAD);
        CHECK(recovered.status() == durable_record::RecordPublishStatus::durable);
        CHECK(recovered.canonical_snapshot().has_value());
        CHECK(!leaf_exists(parent.fd(), "record.pending"));
        require_preserved(parent.fd(), "record.bin", PAYLOAD);
        require_preserved(parent.fd(), "foreign.keep", OTHER_PAYLOAD);
        CHECK(parent.is_open());
    }
}

#else

class WindowsParentDirectory final {
public:
    explicit WindowsParentDirectory(const std::filesystem::path& path) {
        handle_ = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw TestFailure("cannot open temporary parent: " +
                              std::system_category().message(static_cast<int>(::GetLastError())));
        }
    }

    WindowsParentDirectory(const WindowsParentDirectory&) = delete;
    WindowsParentDirectory& operator=(const WindowsParentDirectory&) = delete;

    ~WindowsParentDirectory() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            (void)::CloseHandle(handle_);
        }
    }

    [[nodiscard]] durable_record::NativeHandle native_handle() const noexcept {
        return reinterpret_cast<durable_record::NativeHandle>(handle_);
    }

    [[nodiscard]] bool is_open() const noexcept {
        BY_HANDLE_FILE_INFORMATION information{};
        return ::GetFileInformationByHandle(handle_, &information) != 0;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

void test_windows_production_fails_before_mutation() {
    TempDirectory directory;
    WindowsParentDirectory parent(directory.path());
    const auto result =
        durable_record::publish_at(parent.native_handle(), "record.pending", "record.bin", PAYLOAD);
    CHECK(result.status() == durable_record::RecordPublishStatus::platform_unsupported);
    CHECK(!result.is_durable());
    CHECK(result.native_error() == std::errc::operation_not_supported);
    CHECK(std::filesystem::is_empty(directory.path()));
    CHECK(parent.is_open());
}

#endif

void test_injected_error_mappings_are_exact() {
    constexpr durable_record::RecordSnapshot SNAPSHOT_A{{1, 2, 3}, PAYLOAD.size()};
    constexpr durable_record::RecordSnapshot SNAPSHOT_B{{1, 2, 4}, PAYLOAD.size()};
    const auto unsupported = std::make_error_code(std::errc::operation_not_supported);

    {
        ScriptedRecordOps ops;
        ops.inspections = {
            durable_record::InspectResult::missing(),
            durable_record::InspectResult::missing(),
        };
        ops.pending_publish_result = durable_file::PublishResult(
            durable_file::PublishStatus::write_failed, injected_error(), 0);
        const auto result = durable_record::publish_at_with_ops(
            ops.expected_parent, "record.pending", "record.bin", PAYLOAD, ops);
        CHECK(result.status() == durable_record::RecordPublishStatus::pending_publish_failed);
        CHECK(!result.is_durable());
        CHECK(result.disposition() == durable_record::RecordPublishDisposition::none);
        CHECK(result.native_error() == injected_error());
        CHECK(ops.publish_calls == 1);
        CHECK(ops.confirm_calls == 0);
        CHECK(ops.promote_calls == 0);
        CHECK(ops.parent_sync_calls == 0);
        CHECK(ops.remove_calls == 0);
    }
    {
        ScriptedRecordOps ops;
        ops.inspections = {
            durable_record::InspectResult::missing(),
            durable_record::InspectResult::exact(SNAPSHOT_A),
        };
        ops.canonical_confirm_result = durable_file::PublishResult(
            durable_file::PublishStatus::file_sync_failed, injected_error(), 0);
        const auto result = durable_record::publish_at_with_ops(
            ops.expected_parent, "record.pending", "record.bin", PAYLOAD, ops);
        CHECK(result.status() == durable_record::RecordPublishStatus::pending_publish_failed);
        CHECK(!result.is_durable());
        CHECK(result.disposition() == durable_record::RecordPublishDisposition::recovered_pending);
        CHECK(result.native_error() == injected_error());
        CHECK(ops.publish_calls == 0);
        CHECK(ops.confirm_calls == 1);
        CHECK(ops.promote_calls == 0);
        CHECK(ops.parent_sync_calls == 0);
        CHECK(ops.remove_calls == 0);
    }
    {
        ScriptedRecordOps ops;
        ops.inspections = {
            durable_record::InspectResult::missing(),
            durable_record::InspectResult::exact(SNAPSHOT_A),
            durable_record::InspectResult::exact(SNAPSHOT_A),
        };
        ops.promotion_result = durable_record::MutationResult::failed(injected_error());
        const auto result = durable_record::publish_at_with_ops(
            ops.expected_parent, "record.pending", "record.bin", PAYLOAD, ops);
        CHECK(result.status() == durable_record::RecordPublishStatus::promotion_failed);
        CHECK(!result.is_durable());
        CHECK(result.disposition() == durable_record::RecordPublishDisposition::recovered_pending);
        CHECK(result.native_error() == injected_error());
        CHECK(ops.confirm_calls == 1);
        CHECK(ops.promote_calls == 1);
        CHECK(ops.parent_sync_calls == 0);
        CHECK(ops.remove_calls == 0);
    }
    {
        ScriptedRecordOps ops;
        ops.inspections = {
            durable_record::InspectResult::missing(),
            durable_record::InspectResult::exact(SNAPSHOT_A),
            durable_record::InspectResult::exact(SNAPSHOT_A),
        };
        ops.parent_sync_result = durable_file::OperationResult::failed(injected_error());
        const auto result = durable_record::publish_at_with_ops(
            ops.expected_parent, "record.pending", "record.bin", PAYLOAD, ops);
        CHECK(result.status() == durable_record::RecordPublishStatus::parent_sync_failed);
        CHECK(!result.is_durable());
        CHECK(result.disposition() == durable_record::RecordPublishDisposition::recovered_pending);
        CHECK(result.native_error() == injected_error());
        CHECK(ops.confirm_calls == 1);
        CHECK(ops.promote_calls == 1);
        CHECK(ops.parent_sync_calls == 1);
        CHECK(ops.remove_calls == 0);
    }
    {
        ScriptedRecordOps ops;
        ops.inspections = {
            durable_record::InspectResult::exact(SNAPSHOT_A),
            durable_record::InspectResult::missing(),
        };
        ops.canonical_confirm_result = durable_file::PublishResult(
            durable_file::PublishStatus::file_sync_failed, injected_error(), 0);
        const auto result = durable_record::publish_at_with_ops(
            ops.expected_parent, "record.pending", "record.bin", PAYLOAD, ops);
        CHECK(result.status() == durable_record::RecordPublishStatus::canonical_confirm_failed);
        CHECK(!result.is_durable());
        CHECK(result.disposition() == durable_record::RecordPublishDisposition::confirmed_existing);
        CHECK(result.native_error() == injected_error());
        CHECK(ops.confirm_calls == 1);
        CHECK(ops.promote_calls == 0);
        CHECK(ops.parent_sync_calls == 0);
        CHECK(ops.remove_calls == 0);
    }
    {
        ScriptedRecordOps ops;
        ops.inspections = {
            durable_record::InspectResult::exact(SNAPSHOT_A),
            durable_record::InspectResult::exact(SNAPSHOT_B),
            durable_record::InspectResult::exact(SNAPSHOT_A),
            durable_record::InspectResult::exact(SNAPSHOT_B),
        };
        ops.removal_result = durable_record::MutationResult::failed(injected_error());
        const auto result = durable_record::publish_at_with_ops(
            ops.expected_parent, "record.pending", "record.bin", PAYLOAD, ops);
        CHECK(result.status() == durable_record::RecordPublishStatus::pending_cleanup_failed);
        CHECK(!result.is_durable());
        CHECK(result.disposition() == durable_record::RecordPublishDisposition::confirmed_existing);
        CHECK(result.native_error() == injected_error());
        CHECK(ops.confirm_calls == 1);
        CHECK(ops.remove_calls == 1);
        CHECK(ops.parent_sync_calls == 0);
    }
    {
        ScriptedRecordOps ops;
        ops.inspections = {
            durable_record::InspectResult::exact(SNAPSHOT_A),
            durable_record::InspectResult::exact(SNAPSHOT_B),
            durable_record::InspectResult::exact(SNAPSHOT_A),
            durable_record::InspectResult::exact(SNAPSHOT_B),
        };
        ops.parent_sync_result = durable_file::OperationResult::failed(injected_error());
        const auto result = durable_record::publish_at_with_ops(
            ops.expected_parent, "record.pending", "record.bin", PAYLOAD, ops);
        CHECK(result.status() == durable_record::RecordPublishStatus::pending_cleanup_failed);
        CHECK(!result.is_durable());
        CHECK(result.disposition() == durable_record::RecordPublishDisposition::confirmed_existing);
        CHECK(result.native_error() == injected_error());
        CHECK(ops.confirm_calls == 1);
        CHECK(ops.remove_calls == 1);
        CHECK(ops.parent_sync_calls == 1);
    }
    {
        ScriptedRecordOps ops;
        ops.inspections = {
            durable_record::InspectResult::missing(),
            durable_record::InspectResult::exact(SNAPSHOT_A),
            durable_record::InspectResult::exact(SNAPSHOT_A),
            durable_record::InspectResult::exact(SNAPSHOT_A),
            durable_record::InspectResult::unsupported(unsupported),
        };
        const auto result = durable_record::publish_at_with_ops(
            ops.expected_parent, "record.pending", "record.bin", PAYLOAD, ops);
        CHECK(result.status() == durable_record::RecordPublishStatus::platform_unsupported);
        CHECK(!result.is_durable());
        CHECK(result.disposition() == durable_record::RecordPublishDisposition::recovered_pending);
        CHECK(result.native_error() == unsupported);
        CHECK(ops.confirm_calls == 1);
        CHECK(ops.promote_calls == 1);
        CHECK(ops.parent_sync_calls == 1);
        CHECK(ops.remove_calls == 0);
    }
    {
        ScriptedRecordOps ops;
        ops.inspections = {
            durable_record::InspectResult::missing(),
            durable_record::InspectResult::exact(SNAPSHOT_A),
            durable_record::InspectResult::exact(SNAPSHOT_A),
            durable_record::InspectResult::exact(SNAPSHOT_A),
            durable_record::InspectResult::failed(injected_error()),
        };
        const auto result = durable_record::publish_at_with_ops(
            ops.expected_parent, "record.pending", "record.bin", PAYLOAD, ops);
        CHECK(result.status() == durable_record::RecordPublishStatus::pending_cleanup_failed);
        CHECK(!result.is_durable());
        CHECK(result.disposition() == durable_record::RecordPublishDisposition::recovered_pending);
        CHECK(result.native_error() == injected_error());
        CHECK(ops.confirm_calls == 1);
        CHECK(ops.promote_calls == 1);
        CHECK(ops.parent_sync_calls == 1);
        CHECK(ops.remove_calls == 0);
    }
}

void test_injected_identity_drift_fails_closed() {
    constexpr durable_record::RecordSnapshot SNAPSHOT_A{{1, 2, 3}, PAYLOAD.size()};
    constexpr durable_record::RecordSnapshot SNAPSHOT_B{{1, 2, 4}, PAYLOAD.size()};

    {
        ScriptedRecordOps ops;
        ops.inspections = {
            durable_record::InspectResult::missing(),
            durable_record::InspectResult::exact(SNAPSHOT_A),
            durable_record::InspectResult::exact(SNAPSHOT_A),
        };
        ops.promotion_result = durable_record::MutationResult::identity_mismatch(injected_error());
        const auto result = durable_record::publish_at_with_ops(
            ops.expected_parent, "record.pending", "record.bin", PAYLOAD, ops);
        CHECK(result.status() == durable_record::RecordPublishStatus::pending_conflict);
        CHECK(!result.is_durable());
        CHECK(ops.promote_calls == 1);
        CHECK(ops.remove_calls == 0);
        CHECK(!ops.parent_mismatch);
    }
    {
        ScriptedRecordOps ops;
        ops.inspections = {
            durable_record::InspectResult::exact(SNAPSHOT_A),
            durable_record::InspectResult::missing(),
            durable_record::InspectResult::exact(SNAPSHOT_B),
        };
        const auto result = durable_record::publish_at_with_ops(
            ops.expected_parent, "record.pending", "record.bin", PAYLOAD, ops);
        CHECK(!result.is_durable());
        CHECK(result.status() == durable_record::RecordPublishStatus::canonical_conflict);
        CHECK(ops.publish_calls == 0);
        CHECK(ops.promote_calls == 0);
        CHECK(ops.remove_calls == 0);
        CHECK(!ops.parent_mismatch);
    }
    {
        ScriptedRecordOps ops;
        ops.inspections = {
            durable_record::InspectResult::exact(SNAPSHOT_A),
            durable_record::InspectResult::exact(SNAPSHOT_A),
            durable_record::InspectResult::exact(SNAPSHOT_A),
            durable_record::InspectResult::exact(SNAPSHOT_A),
        };
        const auto result = durable_record::publish_at_with_ops(
            ops.expected_parent, "record.pending", "record.bin", PAYLOAD, ops);
        CHECK(!result.is_durable());
        CHECK(result.status() == durable_record::RecordPublishStatus::pending_conflict);
        CHECK(ops.remove_calls == 0);
        CHECK(!ops.parent_mismatch);
    }
    {
        ScriptedRecordOps ops;
        ops.inspections = {
            durable_record::InspectResult::exact(SNAPSHOT_A),
            durable_record::InspectResult::exact(SNAPSHOT_B),
            durable_record::InspectResult::exact(SNAPSHOT_A),
            durable_record::InspectResult::exact(SNAPSHOT_B),
        };
        ops.removal_result = durable_record::MutationResult::identity_mismatch(injected_error());
        const auto result = durable_record::publish_at_with_ops(
            ops.expected_parent, "record.pending", "record.bin", PAYLOAD, ops);
        CHECK(result.status() == durable_record::RecordPublishStatus::pending_conflict);
        CHECK(!result.is_durable());
        CHECK(ops.remove_calls == 1);
        CHECK(!ops.parent_mismatch);
    }
}

using TestFunction = void (*)();

void run_tests(std::string_view suite) {
    const auto run = [](std::string_view name, TestFunction function) {
        function();
        std::cout << "  " << name << ": PASS\n";
    };

    if (suite == "core" || suite == "all") {
        std::cout << "===== Durable Immutable Record Core Tests =====\n";
        run("invalid requests stop before ops", test_invalid_requests_stop_before_ops);
        run("probe fail-closed", test_probe_fails_closed_before_mutation);
#ifndef _WIN32
        run("real casefold alias", test_real_casefold_alias_rejected_before_mutation);
        run("parent descriptor policy", test_parent_descriptor_policy_preflight);
        run("fresh publish, replay, and umask", test_fresh_publish_replay_and_umask);
        run("pending recovery and convergence", test_pending_recovery_and_matching_convergence);
        run("content and mode conflicts", test_wrong_bytes_sizes_and_modes_are_preserved);
        run("malformed leaves", test_symlink_hardlink_and_nonregular_are_preserved);
#else
        run("Windows production fail-closed", test_windows_production_fails_before_mutation);
#endif
    }
    if (suite == "crash" || suite == "all") {
        std::cout << "===== Durable Immutable Record Crash Tests =====\n";
#ifndef _WIN32
        run("fault-point recovery", test_all_fault_points_recover);
#endif
        run("injected identity drift", test_injected_identity_drift_fails_closed);
        run("injected error mappings", test_injected_error_mappings_are_exact);
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string_view suite = "all";
    if (argc == 3 && std::string_view(argv[1]) == "--suite") {
        suite = argv[2];
    } else if (argc != 1) {
        std::cerr << "usage: " << argv[0] << " [--suite core|crash]\n";
        return 2;
    }
    if (suite != "all" && suite != "core" && suite != "crash") {
        std::cerr << "usage: " << argv[0] << " [--suite core|crash]\n";
        return 2;
    }

    try {
        run_tests(suite);
        std::cout << "===== Durable Immutable Record Tests PASSED =====\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

// Cross-platform integration tests for the production bounded child transport.
// The fake executable emits only synthetic bytes.

#include <gnfs/util/bounded_child_process.hpp>

#include "bounded_child_process_internal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

using gnfs::util::authenticate_executable_image;
using gnfs::util::bounded_child_process_error_name;
using gnfs::util::BoundedChildProcessError;
using gnfs::util::BoundedChildProcessResult;
using gnfs::util::BoundedChildProcessSpec;
using gnfs::util::BoundedChildTerminationKind;
using gnfs::util::executable_image_authentication_error_name;
using gnfs::util::ExecutableImageAuthenticationError;
using gnfs::util::run_authenticated_bounded_child_process;
using gnfs::util::run_bounded_child_process;
using gnfs::util::Sha256Accumulator;
using gnfs::util::Sha256Digest;
#if !defined(_WIN32)
using gnfs::util::detail::PosixTerminationScope;
using gnfs::util::detail::select_posix_termination_scope;
#endif
using namespace std::chrono_literals;

static_assert(!std::is_default_constructible_v<gnfs::util::AuthenticatedExecutableImage>);
static_assert(!std::is_copy_constructible_v<gnfs::util::AuthenticatedExecutableImage>);
static_assert(!std::is_copy_assignable_v<gnfs::util::AuthenticatedExecutableImage>);
static_assert(std::is_nothrow_move_constructible_v<gnfs::util::AuthenticatedExecutableImage>);
static_assert(std::is_nothrow_move_assignable_v<gnfs::util::AuthenticatedExecutableImage>);

int checks_passed = 0;
int checks_failed = 0;

void check(bool condition, std::string_view expression, std::string_view context = {}) {
    if (condition) {
        ++checks_passed;
        return;
    }
    ++checks_failed;
    std::cerr << "FAIL: " << expression;
    if (!context.empty()) {
        std::cerr << " [" << context << ']';
    }
    std::cerr << '\n';
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression)
#define CHECK_CONTEXT(expression, context)                                                         \
    check(static_cast<bool>(expression), #expression, context)

[[nodiscard]] std::string describe(const BoundedChildProcessResult& result) {
    std::string description(bounded_child_process_error_name(result.error));
    description.append(" exit=");
    description.append(std::to_string(result.termination.exit_code));
    description.append(" signal=");
    description.append(std::to_string(result.termination.signal));
    description.append(" stdout=");
    description.append(std::to_string(result.stdout_bytes.size()));
    description.append(" stderr=");
    description.append(std::to_string(result.stderr_bytes.size()));
    if (result.native_error) {
        description.append(" native=");
        description.append(std::to_string(result.native_error.value()));
    }
    if (result.cleanup_error) {
        description.append(" cleanup=");
        description.append(std::to_string(result.cleanup_error.value()));
    }
    return description;
}

[[nodiscard]] BoundedChildProcessSpec make_spec(const std::filesystem::path& executable,
                                                std::vector<std::string> arguments,
                                                std::size_t stdout_limit, std::size_t stderr_limit,
                                                std::chrono::milliseconds timeout = 5s) {
    BoundedChildProcessSpec spec;
    spec.executable = executable;
    spec.arguments = std::move(arguments);
    spec.environment = {"BCP_TEST_ENV=exact-environment"};
    spec.deadline = std::chrono::steady_clock::now() + timeout;
    spec.stdout_limit = stdout_limit;
    spec.stderr_limit = stderr_limit;
    return spec;
}

[[nodiscard]] bool all_bytes_are(std::string_view bytes, char expected) {
    return std::all_of(bytes.begin(), bytes.end(),
                       [expected](char byte) { return byte == expected; });
}

void check_success(const BoundedChildProcessResult& result) {
    const std::string context = describe(result);
    CHECK_CONTEXT(result.succeeded(), context);
    CHECK_CONTEXT(result.error == BoundedChildProcessError::none, context);
    CHECK_CONTEXT(result.child_started, context);
    CHECK_CONTEXT(result.stdout_eof, context);
    CHECK_CONTEXT(result.stderr_eof, context);
    CHECK_CONTEXT(result.cleanup_complete, context);
    CHECK_CONTEXT(result.termination.kind == BoundedChildTerminationKind::exited, context);
    CHECK_CONTEXT(result.termination.exit_code == 0, context);
    CHECK_CONTEXT(!result.native_error, context);
    CHECK_CONTEXT(!result.cleanup_error, context);
}

#if defined(__linux__)
[[nodiscard]] std::optional<Sha256Digest> sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    Sha256Accumulator accumulator;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0 &&
            !accumulator.update(std::string_view(buffer.data(), static_cast<std::size_t>(count)))) {
            return std::nullopt;
        }
    }
    if (!input.eof()) {
        return std::nullopt;
    }
    return accumulator.finalize();
}
#endif

void test_error_name_contract() {
    const std::vector<std::pair<BoundedChildProcessError, std::string_view>> names{
        {BoundedChildProcessError::none, "none"},
        {BoundedChildProcessError::invalid_spec, "invalid_spec"},
        {BoundedChildProcessError::pipe_failed, "pipe_failed"},
        {BoundedChildProcessError::spawn_failed, "spawn_failed"},
        {BoundedChildProcessError::platform_unavailable, "platform_unavailable"},
        {BoundedChildProcessError::read_failed, "read_failed"},
        {BoundedChildProcessError::overflow, "overflow"},
        {BoundedChildProcessError::timeout, "timeout"},
        {BoundedChildProcessError::descendant_writer_leak, "descendant_writer_leak"},
        {BoundedChildProcessError::wait_failed, "wait_failed"},
        {BoundedChildProcessError::cleanup_failed, "cleanup_failed"},
        {BoundedChildProcessError::normal_nonzero, "normal_nonzero"},
        {BoundedChildProcessError::signaled, "signaled"},
        {BoundedChildProcessError::resource_failure, "resource_failure"},
        {BoundedChildProcessError::unexpected_failure, "unexpected_failure"},
    };
    for (const auto& [error, name] : names) {
        CHECK(bounded_child_process_error_name(error) == name);
    }
    CHECK(bounded_child_process_error_name(static_cast<BoundedChildProcessError>(255)) ==
          "unknown");

    const std::vector<std::pair<ExecutableImageAuthenticationError, std::string_view>>
        authentication_names{
            {ExecutableImageAuthenticationError::none, "none"},
            {ExecutableImageAuthenticationError::platform_unavailable, "platform_unavailable"},
            {ExecutableImageAuthenticationError::invalid_spec, "invalid_spec"},
            {ExecutableImageAuthenticationError::open_failed, "open_failed"},
            {ExecutableImageAuthenticationError::metadata_failed, "metadata_failed"},
            {ExecutableImageAuthenticationError::trust_invalid, "trust_invalid"},
            {ExecutableImageAuthenticationError::read_failed, "read_failed"},
            {ExecutableImageAuthenticationError::snapshot_failed, "snapshot_failed"},
            {ExecutableImageAuthenticationError::seal_failed, "seal_failed"},
            {ExecutableImageAuthenticationError::identity_mismatch, "identity_mismatch"},
            {ExecutableImageAuthenticationError::resource_failure, "resource_failure"},
            {ExecutableImageAuthenticationError::unexpected_failure, "unexpected_failure"},
        };
    for (const auto& [error, name] : authentication_names) {
        CHECK(executable_image_authentication_error_name(error) == name);
    }
    CHECK(executable_image_authentication_error_name(
              static_cast<ExecutableImageAuthenticationError>(255)) == "unknown");
}

void test_authenticated_platform_boundary(const std::filesystem::path& executable) {
    Sha256Digest digest{};
    digest.bytes[0] = std::byte{1};
    const auto zero_digest = authenticate_executable_image(executable, Sha256Digest{}, UINT64_C(0));
    CHECK(!static_cast<bool>(zero_digest));
    CHECK(zero_digest.diagnostic.error == ExecutableImageAuthenticationError::invalid_spec);

#if !defined(__linux__)
    const auto unavailable = authenticate_executable_image(executable, digest, UINT64_C(0));
    CHECK(!static_cast<bool>(unavailable));
    CHECK(unavailable.diagnostic.error == ExecutableImageAuthenticationError::platform_unavailable);
#else
    (void)digest;
#endif
}

#if defined(__linux__)

class AuthenticatedTempDirectory final {
public:
    AuthenticatedTempDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("gnfs-bcp-auth-" + std::to_string(static_cast<long long>(::getpid())) + "-" +
                 std::to_string(nonce));
        if (!std::filesystem::create_directory(path_)) {
            throw std::runtime_error("unable to create authenticated transport fixture");
        }
    }

    ~AuthenticatedTempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    AuthenticatedTempDirectory(const AuthenticatedTempDirectory&) = delete;
    AuthenticatedTempDirectory& operator=(const AuthenticatedTempDirectory&) = delete;

    [[nodiscard]] std::filesystem::path leaf(std::string_view name) const {
        return path_ / std::string(name);
    }

private:
    std::filesystem::path path_;
};

void copy_executable(const std::filesystem::path& source,
                     const std::filesystem::path& destination) {
    if (!std::filesystem::copy_file(source, destination,
                                    std::filesystem::copy_options::overwrite_existing)) {
        throw std::runtime_error("unable to copy authenticated executable fixture");
    }
    if (::chmod(destination.c_str(), S_IRUSR | S_IXUSR) != 0) {
        throw std::runtime_error("unable to chmod authenticated executable fixture");
    }
}

[[nodiscard]] std::uint64_t current_owner() noexcept {
    return static_cast<std::uint64_t>(::geteuid());
}

void test_authenticated_linux_rejections(const std::filesystem::path& executable) {
    AuthenticatedTempDirectory fixture;
    const auto approved = fixture.leaf("approved-probe");
    copy_executable(executable, approved);
    const auto digest = sha256_file(approved);
    CHECK(digest.has_value());
    if (!digest.has_value()) {
        return;
    }

    auto availability = authenticate_executable_image(approved, *digest, current_owner());
    if (!availability &&
        availability.diagnostic.error == ExecutableImageAuthenticationError::platform_unavailable) {
        CHECK(!availability.image.has_value());
        return;
    }
    CHECK(static_cast<bool>(availability));
    if (!availability) {
        return;
    }
    availability.image.reset();

    auto wrong_digest = *digest;
    wrong_digest.bytes[0] ^= std::byte{1};
    const auto mismatch = authenticate_executable_image(approved, wrong_digest, current_owner());
    CHECK(!static_cast<bool>(mismatch));
    CHECK(mismatch.diagnostic.error == ExecutableImageAuthenticationError::identity_mismatch);

    const auto missing =
        authenticate_executable_image(fixture.leaf("missing"), *digest, current_owner());
    CHECK(!static_cast<bool>(missing));
    CHECK(missing.diagnostic.error == ExecutableImageAuthenticationError::open_failed);

    const auto symlink = fixture.leaf("symlink-probe");
    std::filesystem::create_symlink(approved, symlink);
    const auto symlink_result = authenticate_executable_image(symlink, *digest, current_owner());
    CHECK(!static_cast<bool>(symlink_result));
    CHECK(symlink_result.diagnostic.error == ExecutableImageAuthenticationError::open_failed);

    const auto hardlink = fixture.leaf("hardlink-probe");
    std::filesystem::create_hard_link(approved, hardlink);
    const auto hardlink_result = authenticate_executable_image(approved, *digest, current_owner());
    CHECK(!static_cast<bool>(hardlink_result));
    CHECK(hardlink_result.diagnostic.error == ExecutableImageAuthenticationError::trust_invalid);
    std::filesystem::remove(hardlink);

    const auto writable = fixture.leaf("group-writable-probe");
    copy_executable(executable, writable);
    CHECK(::chmod(writable.c_str(), S_IRUSR | S_IXUSR | S_IWGRP) == 0);
    const auto writable_digest = sha256_file(writable);
    CHECK(writable_digest.has_value());
    if (writable_digest.has_value()) {
        const auto writable_result =
            authenticate_executable_image(writable, *writable_digest, current_owner());
        CHECK(!static_cast<bool>(writable_result));
        CHECK(writable_result.diagnostic.error ==
              ExecutableImageAuthenticationError::trust_invalid);
    }

    const auto fifo = fixture.leaf("fifo-probe");
    CHECK(::mkfifo(fifo.c_str(), S_IRUSR | S_IXUSR) == 0);
    const auto fifo_result = authenticate_executable_image(fifo, *digest, current_owner());
    CHECK(!static_cast<bool>(fifo_result));
    CHECK(fifo_result.diagnostic.error == ExecutableImageAuthenticationError::trust_invalid);

    const auto oversized = fixture.leaf("oversized-probe");
    {
        std::ofstream output(oversized, std::ios::binary);
        output.write("\x7f"
                     "ELF",
                     4);
    }
    std::filesystem::resize_file(oversized, gnfs::util::AUTHENTICATED_EXECUTABLE_IMAGE_MAX_BYTES +
                                                UINT64_C(1));
    CHECK(::chmod(oversized.c_str(), S_IRUSR | S_IXUSR) == 0);
    const auto oversized_result =
        authenticate_executable_image(oversized, *digest, current_owner());
    CHECK(!static_cast<bool>(oversized_result));
    CHECK(oversized_result.diagnostic.error == ExecutableImageAuthenticationError::trust_invalid);

    const auto wrong_owner =
        authenticate_executable_image(approved, *digest, current_owner() + UINT64_C(1));
    CHECK(!static_cast<bool>(wrong_owner));
    CHECK(wrong_owner.diagnostic.error == ExecutableImageAuthenticationError::trust_invalid);
}

void test_authenticated_linux_same_object_and_supervision(const std::filesystem::path& executable) {
    AuthenticatedTempDirectory fixture;
    const auto approved = fixture.leaf("approved-probe");
    const auto held_original = fixture.leaf("held-original-probe");
    copy_executable(executable, approved);
    const auto digest = sha256_file(approved);
    CHECK(digest.has_value());
    if (!digest.has_value()) {
        return;
    }

    auto authenticated = authenticate_executable_image(approved, *digest, current_owner());
    if (!authenticated && authenticated.diagnostic.error ==
                              ExecutableImageAuthenticationError::platform_unavailable) {
        CHECK(!authenticated.image.has_value());
        return;
    }
    CHECK(static_cast<bool>(authenticated));
    if (!authenticated) {
        return;
    }

    std::filesystem::rename(approved, held_original);
    {
        std::ofstream replacement(approved, std::ios::binary);
        replacement << "not an ELF executable\n";
    }
    CHECK(::chmod(approved.c_str(), S_IRUSR | S_IXUSR) == 0);

    auto argv0_spec = make_spec(approved, {"--argv0"}, 128, 0);
    auto argv0_result = run_authenticated_bounded_child_process(
        std::move(*authenticated.image), argv0_spec, "authenticated-test-probe");
    check_success(argv0_result);
    CHECK(argv0_result.stdout_bytes == "authenticated-test-probe\n");
    CHECK(!authenticated.image->active());

    const auto reused = run_authenticated_bounded_child_process(
        std::move(*authenticated.image), argv0_spec, "authenticated-test-probe");
    CHECK(reused.error == BoundedChildProcessError::invalid_spec);
    CHECK(!reused.child_started);
    CHECK(reused.cleanup_complete);

    const auto invalid_elf = fixture.leaf("invalid-elf-probe");
    {
        std::array<char, 64> bytes{};
        bytes[0] = '\x7f';
        bytes[1] = 'E';
        bytes[2] = 'L';
        bytes[3] = 'F';
        std::ofstream output(invalid_elf, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    CHECK(::chmod(invalid_elf.c_str(), S_IRUSR | S_IXUSR) == 0);
    const auto invalid_digest = sha256_file(invalid_elf);
    CHECK(invalid_digest.has_value());
    if (invalid_digest.has_value()) {
        auto invalid_image =
            authenticate_executable_image(invalid_elf, *invalid_digest, current_owner());
        CHECK(static_cast<bool>(invalid_image));
        if (invalid_image) {
            const auto invalid_run = run_authenticated_bounded_child_process(
                std::move(*invalid_image.image), make_spec(invalid_elf, {}, 32, 32),
                "authenticated-test-probe");
            CHECK_CONTEXT(invalid_run.error == BoundedChildProcessError::spawn_failed,
                          describe(invalid_run));
            CHECK_CONTEXT(!invalid_run.child_started, describe(invalid_run));
            CHECK_CONTEXT(invalid_run.cleanup_complete, describe(invalid_run));
            CHECK_CONTEXT(invalid_run.native_error.value() == ENOEXEC, describe(invalid_run));
        }
    }

    auto timeout_image = authenticate_executable_image(held_original, *digest, current_owner());
    CHECK(static_cast<bool>(timeout_image));
    if (timeout_image) {
        const auto timeout = run_authenticated_bounded_child_process(
            std::move(*timeout_image.image), make_spec(held_original, {"--hang"}, 32, 32, 200ms),
            "authenticated-test-probe");
        CHECK_CONTEXT(timeout.error == BoundedChildProcessError::timeout, describe(timeout));
        CHECK_CONTEXT(timeout.cleanup_complete, describe(timeout));
    }

    struct sigaction previous_usr1{};
    struct sigaction previous_alrm{};
    struct sigaction ignored_action{};
    ignored_action.sa_handler = SIG_IGN;
    CHECK(::sigemptyset(&ignored_action.sa_mask) == 0);
    CHECK(::sigaction(SIGUSR1, &ignored_action, &previous_usr1) == 0);
    CHECK(::sigaction(SIGALRM, &ignored_action, &previous_alrm) == 0);
    auto signal_image = authenticate_executable_image(held_original, *digest, current_owner());
    CHECK(static_cast<bool>(signal_image));
    if (signal_image) {
        const auto signal_state = run_authenticated_bounded_child_process(
            std::move(*signal_image.image),
            make_spec(held_original, {"--signal-dispositions"}, 32, 0), "authenticated-test-probe");
        check_success(signal_state);
        CHECK(signal_state.stdout_bytes == "default\n");
    }
    CHECK(::sigaction(SIGALRM, &previous_alrm, nullptr) == 0);
    CHECK(::sigaction(SIGUSR1, &previous_usr1, nullptr) == 0);

    auto overflow_image = authenticate_executable_image(held_original, *digest, current_owner());
    CHECK(static_cast<bool>(overflow_image));
    if (overflow_image) {
        const auto overflow = run_authenticated_bounded_child_process(
            std::move(*overflow_image.image),
            make_spec(held_original, {"--write-sizes", "4097", "0"}, 4096, 0),
            "authenticated-test-probe");
        CHECK_CONTEXT(overflow.error == BoundedChildProcessError::overflow, describe(overflow));
        CHECK_CONTEXT(overflow.cleanup_complete, describe(overflow));
    }

    int sentinel = ::open("/dev/null", O_RDONLY);
    CHECK(sentinel >= 0);
    if (sentinel >= 0) {
        if (sentinel < 3) {
            const int duplicate = ::fcntl(sentinel, F_DUPFD, 3);
            (void)::close(sentinel);
            sentinel = duplicate;
        }
        auto fd_image = authenticate_executable_image(held_original, *digest, current_owner());
        CHECK(static_cast<bool>(fd_image));
        if (fd_image) {
            const auto fd_result = run_authenticated_bounded_child_process(
                std::move(*fd_image.image),
                make_spec(held_original, {"--check-fd-closed", std::to_string(sentinel)}, 32, 0),
                "authenticated-test-probe");
            check_success(fd_result);
            CHECK(fd_result.stdout_bytes == "closed\n");
        }
        (void)::close(sentinel);
    }

    constexpr std::size_t parallel_count = 4;
    std::array<BoundedChildProcessResult, parallel_count> results;
    std::array<std::thread, parallel_count> threads;
    for (std::size_t index = 0; index < parallel_count; ++index) {
        threads[index] = std::thread([&, index] {
            auto image = authenticate_executable_image(held_original, *digest, current_owner());
            if (!image) {
                results[index].error = BoundedChildProcessError::unexpected_failure;
                return;
            }
            results[index] = run_authenticated_bounded_child_process(
                std::move(*image.image),
                make_spec(held_original, {"--write-sizes", "1024", "1024"}, 1024, 1024),
                "authenticated-test-probe");
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (const auto& result : results) {
        check_success(result);
        CHECK(result.stdout_bytes.size() == 1024);
        CHECK(result.stderr_bytes.size() == 1024);
    }
}

#endif

#if !defined(_WIN32)
static_assert(select_posix_termination_scope(0, 0, 41, 0, false, true, false, false) ==
              PosixTerminationScope::none);
static_assert(select_posix_termination_scope(1, 1, 41, 1, false, true, false, false) ==
              PosixTerminationScope::none);
static_assert(select_posix_termination_scope(101, 101, 101, 101, false, true, false, false) ==
              PosixTerminationScope::direct_child);

void test_posix_termination_scope_guard() {
    constexpr std::int64_t child = 101;
    constexpr std::int64_t caller_group = 41;

    CHECK(select_posix_termination_scope(0, 0, caller_group, 0, false, true, false, false) ==
          PosixTerminationScope::none);
    CHECK(select_posix_termination_scope(1, 1, caller_group, 1, false, true, false, false) ==
          PosixTerminationScope::none);
    CHECK(select_posix_termination_scope(child, child, child, child, false, true, false, false) ==
          PosixTerminationScope::direct_child);
    CHECK(select_posix_termination_scope(child, child, child, child, true, false, false, false) ==
          PosixTerminationScope::none);
    CHECK(select_posix_termination_scope(child, 0, caller_group, child, false, true, false,
                                         false) == PosixTerminationScope::direct_child);
    CHECK(select_posix_termination_scope(child, 1, caller_group, child, false, true, false,
                                         false) == PosixTerminationScope::direct_child);
    CHECK(select_posix_termination_scope(child, child, caller_group, child, false, true, false,
                                         false) == PosixTerminationScope::process_group);
    CHECK(select_posix_termination_scope(child, child, caller_group, child + 1, false, true, false,
                                         false) == PosixTerminationScope::direct_child);
    CHECK(select_posix_termination_scope(child, child, caller_group, -1, false, true, true,
                                         false) == PosixTerminationScope::process_group);
    CHECK(select_posix_termination_scope(child, child, caller_group, -1, false, true, false,
                                         true) == PosixTerminationScope::process_group);
    CHECK(select_posix_termination_scope(child, child, caller_group, -1, false, false, false,
                                         true) == PosixTerminationScope::none);
    CHECK(select_posix_termination_scope(child, child, caller_group, -1, true, false, true, true) ==
          PosixTerminationScope::none);
}
#endif

void test_invalid_specs(const std::filesystem::path& executable) {
    {
        auto spec = make_spec("relative-fake-child", {"--hang"}, 1, 1);
        const auto result = run_bounded_child_process(spec);
        CHECK(result.error == BoundedChildProcessError::invalid_spec);
        CHECK(!result.child_started);
        CHECK(result.cleanup_complete);
    }
    {
        auto spec = make_spec(executable, {"--hang"}, 1, 1);
        spec.deadline = std::chrono::steady_clock::now() - 1ms;
        const auto result = run_bounded_child_process(spec);
        CHECK(result.error == BoundedChildProcessError::invalid_spec);
        CHECK(!result.child_started);
    }
    {
        auto spec = make_spec(executable, {"--hang"}, 1, 1);
        spec.environment = {"DUPLICATE=one", "DUPLICATE=two"};
        const auto result = run_bounded_child_process(spec);
        CHECK(result.error == BoundedChildProcessError::invalid_spec);
        CHECK(!result.child_started);
    }
#if defined(_WIN32)
    {
        auto spec = make_spec(executable, {"--hang"}, 1, 1);
        spec.environment = {"CASE_NAME=one", "case_name=two"};
        const auto result = run_bounded_child_process(spec);
        CHECK(result.error == BoundedChildProcessError::invalid_spec);
        CHECK(!result.child_started);
    }
#endif
    {
        auto spec = make_spec(executable, {"--hang"}, 1, 1);
        spec.environment = {"=missing-name"};
        const auto result = run_bounded_child_process(spec);
        CHECK(result.error == BoundedChildProcessError::invalid_spec);
        CHECK(!result.child_started);
    }
    {
        const auto missing = executable.parent_path() / "bounded-child-does-not-exist";
        auto spec = make_spec(missing, {"--hang"}, 1, 1);
        const auto result = run_bounded_child_process(spec);
        CHECK(result.error == BoundedChildProcessError::spawn_failed);
        CHECK(!result.child_started);
        CHECK(result.native_error);
        CHECK(result.cleanup_complete);
    }
}

void test_dual_stream_deadlock_boundaries(const std::filesystem::path& executable) {
    constexpr std::size_t stream_size = 256 * 1024;
    constexpr std::size_t capture_limit = 300 * 1024;
    for (const std::string mode : {"--stdout-first", "--stderr-first"}) {
        const auto result = run_bounded_child_process(
            make_spec(executable, {mode, std::to_string(stream_size), std::to_string(stream_size)},
                      capture_limit, capture_limit));
        check_success(result);
        CHECK(result.stdout_bytes.size() == stream_size);
        CHECK(result.stderr_bytes.size() == stream_size);
        CHECK(all_bytes_are(result.stdout_bytes, 'O'));
        CHECK(all_bytes_are(result.stderr_bytes, 'E'));
    }

    constexpr std::size_t interleaved_size = 128 * 1024;
    const auto interleaved = run_bounded_child_process(
        make_spec(executable, {"--interleaved", std::to_string(interleaved_size), "997"},
                  interleaved_size, interleaved_size));
    check_success(interleaved);
    CHECK(interleaved.stdout_bytes.size() == interleaved_size);
    CHECK(interleaved.stderr_bytes.size() == interleaved_size);
    CHECK(all_bytes_are(interleaved.stdout_bytes, 'O'));
    CHECK(all_bytes_are(interleaved.stderr_bytes, 'E'));
}

void test_concurrent_launch_isolation(const std::filesystem::path& executable) {
    constexpr std::size_t child_count = 4;
    constexpr std::size_t stream_size = 64 * 1024;
    std::array<BoundedChildProcessResult, child_count> results;
    std::array<std::thread, child_count> threads;
    for (std::size_t index = 0; index < child_count; ++index) {
        threads[index] = std::thread([&, index] {
            results[index] = run_bounded_child_process(
                make_spec(executable, {"--interleaved", std::to_string(stream_size), "733"},
                          stream_size, stream_size));
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (const auto& result : results) {
        check_success(result);
        CHECK(result.stdout_bytes.size() == stream_size);
        CHECK(result.stderr_bytes.size() == stream_size);
        CHECK(all_bytes_are(result.stdout_bytes, 'O'));
        CHECK(all_bytes_are(result.stderr_bytes, 'E'));
    }
}

void test_exact_caps_and_overflow(const std::filesystem::path& executable) {
    {
        constexpr std::size_t stdout_size = 4096;
        constexpr std::size_t stderr_size = 2048;
        const auto result = run_bounded_child_process(make_spec(
            executable, {"--write-sizes", std::to_string(stdout_size), std::to_string(stderr_size)},
            stdout_size, stderr_size));
        check_success(result);
        CHECK(result.stdout_bytes.size() == stdout_size);
        CHECK(result.stderr_bytes.size() == stderr_size);
        CHECK(!result.stdout_overflow);
        CHECK(!result.stderr_overflow);
    }
    {
        const auto result =
            run_bounded_child_process(make_spec(executable, {"--write-sizes", "0", "0"}, 0, 0));
        check_success(result);
        CHECK(result.stdout_bytes.empty());
        CHECK(result.stderr_bytes.empty());
    }
    {
        constexpr std::size_t limit = 4096;
        const auto result = run_bounded_child_process(
            make_spec(executable, {"--write-sizes", std::to_string(limit + 1), "0"}, limit, 0));
        const std::string context = describe(result);
        CHECK_CONTEXT(result.error == BoundedChildProcessError::overflow, context);
        CHECK_CONTEXT(result.stdout_overflow, context);
        CHECK_CONTEXT(!result.stderr_overflow, context);
        CHECK_CONTEXT(result.stdout_bytes.size() == limit, context);
        CHECK_CONTEXT(all_bytes_are(result.stdout_bytes, 'O'), context);
        CHECK_CONTEXT(result.stdout_eof, context);
        CHECK_CONTEXT(result.stderr_eof, context);
        CHECK_CONTEXT(result.cleanup_complete, context);
    }
    {
        constexpr std::size_t limit = 4096;
        const auto result = run_bounded_child_process(
            make_spec(executable, {"--write-sizes", "0", std::to_string(limit + 1)}, 0, limit));
        const std::string context = describe(result);
        CHECK_CONTEXT(result.error == BoundedChildProcessError::overflow, context);
        CHECK_CONTEXT(!result.stdout_overflow, context);
        CHECK_CONTEXT(result.stderr_overflow, context);
        CHECK_CONTEXT(result.stderr_bytes.size() == limit, context);
        CHECK_CONTEXT(all_bytes_are(result.stderr_bytes, 'E'), context);
        CHECK_CONTEXT(result.stdout_eof, context);
        CHECK_CONTEXT(result.stderr_eof, context);
        CHECK_CONTEXT(result.cleanup_complete, context);
    }
}

void test_continuous_writer_is_bounded(const std::filesystem::path& executable) {
    constexpr std::size_t limit = 4096;
    const auto before = std::chrono::steady_clock::now();
    const auto result =
        run_bounded_child_process(make_spec(executable, {"--flood-stdout"}, limit, 0, 200ms));
    const auto elapsed = std::chrono::steady_clock::now() - before;
    const std::string context = describe(result);
    CHECK_CONTEXT(result.error == BoundedChildProcessError::overflow, context);
    CHECK_CONTEXT(result.stdout_overflow, context);
    CHECK_CONTEXT(!result.stderr_overflow, context);
    CHECK_CONTEXT(result.stdout_bytes.size() == limit, context);
    CHECK_CONTEXT(all_bytes_are(result.stdout_bytes, 'F'), context);
    CHECK_CONTEXT(result.stdout_eof, context);
    CHECK_CONTEXT(result.stderr_eof, context);
    CHECK_CONTEXT(result.cleanup_complete, context);
    CHECK_CONTEXT(elapsed < 3s, context);
}

void test_dual_stream_fair_drain(const std::filesystem::path& executable) {
    constexpr std::size_t stdout_limit = 8 * 1024 * 1024;
    constexpr std::size_t stderr_size = 256 * 1024;
    const auto before = std::chrono::steady_clock::now();
    const auto result = run_bounded_child_process(
        make_spec(executable, {"--fair-drain"}, stdout_limit, stderr_size, 2s));
    const auto elapsed = std::chrono::steady_clock::now() - before;
    const std::string context = describe(result);
    check_success(result);
    CHECK_CONTEXT(!result.stdout_bytes.empty(), context);
    CHECK_CONTEXT(result.stdout_bytes.size() <= stdout_limit, context);
    CHECK_CONTEXT(all_bytes_are(result.stdout_bytes, 'F'), context);
    CHECK_CONTEXT(result.stderr_bytes.size() == stderr_size, context);
    CHECK_CONTEXT(all_bytes_are(result.stderr_bytes, 'E'), context);
    CHECK_CONTEXT(elapsed < 3s, context);
}

void test_timeout_and_writer_lifecycle(const std::filesystem::path& executable) {
    {
        const auto before = std::chrono::steady_clock::now();
        const auto result =
            run_bounded_child_process(make_spec(executable, {"--hang"}, 32, 32, 200ms));
        const auto elapsed = std::chrono::steady_clock::now() - before;
        const std::string context = describe(result);
        CHECK_CONTEXT(result.error == BoundedChildProcessError::timeout, context);
        CHECK_CONTEXT(elapsed < 3s, context);
        CHECK_CONTEXT(result.stdout_eof, context);
        CHECK_CONTEXT(result.stderr_eof, context);
        CHECK_CONTEXT(result.cleanup_complete, context);
    }
    {
        const auto before = std::chrono::steady_clock::now();
        const auto result = run_bounded_child_process(
            make_spec(executable, {"--close-stdout-hang"}, 32, 32, 200ms));
        const auto elapsed = std::chrono::steady_clock::now() - before;
        const std::string context = describe(result);
        CHECK_CONTEXT(result.error == BoundedChildProcessError::timeout, context);
        CHECK_CONTEXT(elapsed < 3s, context);
        CHECK_CONTEXT(result.stdout_eof, context);
        CHECK_CONTEXT(result.stderr_eof, context);
        CHECK_CONTEXT(result.cleanup_complete, context);
    }
    {
        const auto before = std::chrono::steady_clock::now();
        const auto result =
            run_bounded_child_process(make_spec(executable, {"--descendant-writer"}, 32, 32, 3s));
        const auto elapsed = std::chrono::steady_clock::now() - before;
        const std::string context = describe(result);
        CHECK_CONTEXT(result.error == BoundedChildProcessError::descendant_writer_leak, context);
        CHECK_CONTEXT(elapsed < 3s, context);
        CHECK_CONTEXT(result.stdout_eof, context);
        CHECK_CONTEXT(result.stderr_eof, context);
        CHECK_CONTEXT(result.cleanup_complete, context);
        CHECK_CONTEXT(result.termination.kind == BoundedChildTerminationKind::exited, context);
        CHECK_CONTEXT(result.termination.exit_code == 0, context);
    }
}

void test_exit_semantics(const std::filesystem::path& executable) {
    {
        const auto result =
            run_bounded_child_process(make_spec(executable, {"--nonzero"}, 128, 128));
        const std::string context = describe(result);
        CHECK_CONTEXT(result.error == BoundedChildProcessError::normal_nonzero, context);
        CHECK_CONTEXT(result.termination.kind == BoundedChildTerminationKind::exited, context);
        CHECK_CONTEXT(result.termination.exit_code == 23, context);
        CHECK_CONTEXT(result.stdout_bytes == "stdout-before-nonzero\n", context);
        CHECK_CONTEXT(result.stderr_bytes == "stderr-before-nonzero\n", context);
        CHECK_CONTEXT(result.stdout_eof, context);
        CHECK_CONTEXT(result.stderr_eof, context);
        CHECK_CONTEXT(result.cleanup_complete, context);
    }
    {
        const auto result = run_bounded_child_process(make_spec(executable, {"--signal"}, 32, 32));
        const std::string context = describe(result);
#if defined(_WIN32)
        CHECK_CONTEXT(result.error == BoundedChildProcessError::normal_nonzero, context);
        CHECK_CONTEXT(result.termination.kind == BoundedChildTerminationKind::exited, context);
        CHECK_CONTEXT(result.termination.exit_code == UINT32_C(0xc0000409), context);
#else
        CHECK_CONTEXT(result.error == BoundedChildProcessError::signaled, context);
        CHECK_CONTEXT(result.termination.kind == BoundedChildTerminationKind::signaled, context);
        CHECK_CONTEXT(result.termination.signal == SIGTERM, context);
#endif
        CHECK_CONTEXT(result.stdout_eof, context);
        CHECK_CONTEXT(result.stderr_eof, context);
        CHECK_CONTEXT(result.cleanup_complete, context);
    }
}

void set_parent_only_environment() {
#if defined(_WIN32)
    if (::SetEnvironmentVariableW(L"BCP_PARENT_ONLY", L"must-not-leak") == 0) {
        throw std::runtime_error("SetEnvironmentVariableW failed");
    }
#else
    if (::setenv("BCP_PARENT_ONLY", "must-not-leak", 1) != 0) {
        throw std::runtime_error("setenv failed");
    }
#endif
}

void clear_parent_only_environment() noexcept {
#if defined(_WIN32)
    (void)::SetEnvironmentVariableW(L"BCP_PARENT_ONLY", nullptr);
#else
    (void)::unsetenv("BCP_PARENT_ONLY");
#endif
}

void test_exact_argv_and_environment(const std::filesystem::path& executable) {
    const std::string unicode_argument =
        "\xE5\x8F\x82\xE6\x95\xB0-\xCF\x80"; // U+53C2 U+6570 '-' U+03C0
    const std::string unicode_environment =
        "\xE7\xB2\xBE\xE7\xA1\xAE-value"; // U+7CBE U+786E "-value"
    set_parent_only_environment();
    auto spec = make_spec(
        executable, {"--echo", "", "with space", "quote\"inside", "trailing\\", unicode_argument},
        2048, 64);
    spec.environment = {"BCP_TEST_ENV=" + unicode_environment};
    const auto result = run_bounded_child_process(spec);
    clear_parent_only_environment();

    check_success(result);
    const std::string expected = "argument_count=5\n"
                                 "argument_0=\n"
                                 "argument_1=with space\n"
                                 "argument_2=quote\"inside\n"
                                 "argument_3=trailing\\\n" +
                                 std::string("argument_4=") + unicode_argument + "\n" +
                                 "environment=" + unicode_environment +
                                 "\n"
                                 "parent_only=<missing>\n";
    CHECK_CONTEXT(result.stdout_bytes == expected, result.stdout_bytes);
    CHECK(result.stderr_bytes.empty());
}

#if defined(_WIN32)
void test_windows_environment_name_order(const std::filesystem::path& executable) {
    auto spec = make_spec(executable, {"--environment-order"}, 256, 64);
    spec.environment = {
        "BCP_SORT_A1=one",
        "BCP_TEST_ENV=exact-environment",
        "BCP_SORT_A=zero",
    };
    const auto result = run_bounded_child_process(spec);
    check_success(result);
    CHECK_CONTEXT(result.stdout_bytes == "BCP_SORT_A=zero\nBCP_SORT_A1=one\n", result.stdout_bytes);
    CHECK(result.stderr_bytes.empty());
}
#endif

#if !defined(_WIN32)
void test_external_reaper_fails_closed(const std::filesystem::path& executable) {
    const auto spec = make_spec(executable, {"--write-sizes", "1", "1"}, 1, 1, 200ms);
    struct sigaction previous_action{};
    struct sigaction ignored_action{};
    ignored_action.sa_handler = SIG_IGN;
    CHECK(sigemptyset(&ignored_action.sa_mask) == 0);
    ignored_action.sa_flags = SA_NOCLDWAIT;
    if (::sigaction(SIGCHLD, &ignored_action, &previous_action) != 0) {
        CHECK(false);
        return;
    }

    const auto before = std::chrono::steady_clock::now();
    const auto result = run_bounded_child_process(spec);
    const auto elapsed = std::chrono::steady_clock::now() - before;
    const int restore_status = ::sigaction(SIGCHLD, &previous_action, nullptr);

    const std::string context = describe(result);
    CHECK_CONTEXT(result.child_started, context);
    CHECK_CONTEXT(result.error == BoundedChildProcessError::wait_failed ||
                      result.error == BoundedChildProcessError::cleanup_failed,
                  context);
    CHECK_CONTEXT(!result.cleanup_complete, context);
    CHECK_CONTEXT(result.cleanup_error, context);
    CHECK_CONTEXT(elapsed < 4s, context);
    CHECK(restore_status == 0);
}
#endif

} // namespace

template <class Char> int bounded_child_process_test_main(int argc, Char* argv[]) {
    if (argc < 1 || argc > 2) {
        std::cerr << "usage: test_bounded_child_process [absolute-fake-child]\n";
        return 2;
    }
    try {
        std::filesystem::path executable;
        if (argc == 2) {
            executable = std::filesystem::absolute(std::filesystem::path(argv[1]));
        } else {
            executable = std::filesystem::absolute(std::filesystem::path(argv[0])).parent_path() /
                         "bounded_child_process_fake_child";
#if defined(_WIN32)
            executable += ".exe";
#endif
        }
        test_error_name_contract();
        test_authenticated_platform_boundary(executable);
#if defined(__linux__)
        test_authenticated_linux_rejections(executable);
        test_authenticated_linux_same_object_and_supervision(executable);
#endif
#if !defined(_WIN32)
        test_posix_termination_scope_guard();
#endif
        test_invalid_specs(executable);
        test_dual_stream_deadlock_boundaries(executable);
        test_concurrent_launch_isolation(executable);
        test_exact_caps_and_overflow(executable);
        test_continuous_writer_is_bounded(executable);
        test_dual_stream_fair_drain(executable);
        test_timeout_and_writer_lifecycle(executable);
        test_exit_semantics(executable);
        test_exact_argv_and_environment(executable);
#if defined(_WIN32)
        test_windows_environment_name_order(executable);
#endif
#if !defined(_WIN32)
        test_external_reaper_fails_closed(executable);
#endif
    } catch (const std::exception& error) {
        ++checks_failed;
        std::cerr << "bounded child process test exception: " << error.what() << '\n';
        clear_parent_only_environment();
    }

    std::cout << "bounded child process transport: " << checks_passed << " passed, "
              << checks_failed << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}

#if defined(_WIN32)
int wmain(int argc, wchar_t* argv[]) {
    return bounded_child_process_test_main(argc, argv);
}
#else
int main(int argc, char* argv[]) {
    return bounded_child_process_test_main(argc, argv);
}
#endif

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

enum class FakeChildMode : std::uint16_t {
    report_and_exit = 1U,
    stop_then_exit = 2U,
    exit_without_report = 3U,
    terminate = 4U,
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

class StandardDescriptorRestorer final {
public:
    StandardDescriptorRestorer() {
        for (int descriptor = STDIN_FILENO; descriptor <= STDERR_FILENO; ++descriptor) {
            int duplicate = -1;
            do {
                duplicate = ::fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            } while (duplicate < 0 && errno == EINTR);
            if (duplicate < 0 && errno != EBADF) {
                throw std::runtime_error("cannot preserve standard descriptor " +
                                         std::to_string(descriptor));
            }
            backups_[static_cast<std::size_t>(descriptor)] = duplicate;
            originally_open_[static_cast<std::size_t>(descriptor)] = duplicate >= 0;
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
            (void)::close(std::exchange(backups_[index], -1));
            restored_[index] = true;
        }
        return complete;
    }

private:
    std::array<int, 3> backups_{{-1, -1, -1}};
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
    CHECK(spawn_calls == 0);
    CHECK(snapshot_open_descriptors() == baseline);
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

#include <gnfs/util/durable_immutable_file.hpp>
#include <gnfs/util/process.hpp>
#include <gnfs/util/temp_path.hpp>

#include <algorithm>
#include <array>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace durable = gnfs::util::durable_immutable_file;

static_assert(!std::is_default_constructible_v<durable::OperationResult>);
static_assert(!std::is_default_constructible_v<durable::OpenResult>);
static_assert(!std::is_default_constructible_v<durable::WriteResult>);
static_assert(!std::is_default_constructible_v<durable::PublishResult>);

int checks_passed = 0;
int checks_failed = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (condition) {                                                                           \
            ++checks_passed;                                                                       \
        } else {                                                                                   \
            ++checks_failed;                                                                       \
            std::cerr << "FAIL: " #condition " at " << __FILE__ << ':' << __LINE__ << '\n';        \
        }                                                                                          \
    } while (false)

[[nodiscard]] std::error_code injected_error() {
    return std::make_error_code(std::errc::io_error);
}

struct WriteStep final {
    durable::OperationState state = durable::OperationState::succeeded;
    std::size_t bytes = 0;
};

class ScriptedFileOps final : public durable::FileOps {
public:
    std::size_t parent_open_interrupts = 0;
    bool parent_open_fails = false;
    bool parent_open_returns_invalid_handle = false;
    std::size_t file_open_interrupts = 0;
    bool file_open_fails = false;
    bool file_open_reports_existing = false;
    bool file_open_returns_invalid_handle = false;
    bool file_open_returns_parent_handle = false;
    std::vector<WriteStep> writes;
    std::size_t sync_interrupts = 0;
    bool sync_fails = false;
    std::size_t sync_fail_on_call = 0;
    bool close_fails = false;
    bool close_returns_interrupted = false;
    std::size_t parent_interrupts = 0;
    bool parent_fails = false;
    bool parent_close_fails = false;
    bool parent_close_returns_interrupted = false;

    std::size_t parent_open_calls = 0;
    std::size_t file_open_calls = 0;
    std::size_t write_calls = 0;
    std::size_t sync_calls = 0;
    std::size_t close_calls = 0;
    std::size_t parent_calls = 0;
    std::size_t parent_close_calls = 0;
    bool file_handle_mismatch = false;
    std::filesystem::path opened_path;
    std::filesystem::path parent_path;
    std::vector<std::byte> captured;

    [[nodiscard]] durable::OpenResult
    open_parent_directory(const std::filesystem::path& path) noexcept override {
        ++parent_open_calls;
        parent_path = path;
        if (parent_open_interrupts != 0) {
            --parent_open_interrupts;
            return durable::OpenResult::interrupted(std::make_error_code(std::errc::interrupted));
        }
        if (parent_open_fails) {
            return durable::OpenResult::failed(injected_error());
        }
        if (parent_open_returns_invalid_handle) {
            return durable::OpenResult::succeeded(durable::INVALID_NATIVE_HANDLE);
        }
        return durable::OpenResult::succeeded(11);
    }

    [[nodiscard]] durable::OpenResult
    open_exclusive(durable::NativeHandle parent_handle, const std::filesystem::path& leaf,
                   const std::filesystem::path& path) noexcept override {
        ++file_open_calls;
        opened_path = path;
        if (parent_handle != 11 || leaf != path.filename()) {
            return durable::OpenResult::failed(std::make_error_code(std::errc::protocol_error));
        }
        if (file_open_interrupts != 0) {
            --file_open_interrupts;
            return durable::OpenResult::interrupted(std::make_error_code(std::errc::interrupted));
        }
        if (file_open_fails) {
            return durable::OpenResult::failed(injected_error());
        }
        if (file_open_reports_existing) {
            return durable::OpenResult::failed(std::make_error_code(std::errc::file_exists));
        }
        if (file_open_returns_invalid_handle) {
            return durable::OpenResult::succeeded(durable::INVALID_NATIVE_HANDLE);
        }
        if (file_open_returns_parent_handle) {
            return durable::OpenResult::succeeded(11);
        }
        return durable::OpenResult::succeeded(17);
    }

    [[nodiscard]] durable::WriteResult
    write_some(durable::NativeHandle handle, std::span<const std::byte> bytes) noexcept override {
        file_handle_mismatch = file_handle_mismatch || handle != 17;
        const std::size_t index = write_calls++;
        if (index < writes.size()) {
            const auto step = writes[index];
            if (step.state == durable::OperationState::interrupted) {
                return durable::WriteResult::interrupted(
                    std::make_error_code(std::errc::interrupted));
            }
            if (step.state == durable::OperationState::failed) {
                return durable::WriteResult::failed(injected_error());
            }
            if (step.bytes <= bytes.size()) {
                const auto prefix = bytes.first(step.bytes);
                captured.insert(captured.end(), prefix.begin(), prefix.end());
            }
            return durable::WriteResult::succeeded(step.bytes);
        }
        captured.insert(captured.end(), bytes.begin(), bytes.end());
        return durable::WriteResult::succeeded(bytes.size());
    }

    [[nodiscard]] durable::OperationResult
    sync_file(durable::NativeHandle handle) noexcept override {
        file_handle_mismatch = file_handle_mismatch || handle != 17;
        ++sync_calls;
        if (sync_interrupts != 0) {
            --sync_interrupts;
            return durable::OperationResult::interrupted(
                std::make_error_code(std::errc::interrupted));
        }
        return sync_fails || (sync_fail_on_call != 0 && sync_calls == sync_fail_on_call)
                   ? durable::OperationResult::failed(injected_error())
                   : durable::OperationResult::succeeded();
    }

    [[nodiscard]] durable::OperationResult
    close_file(durable::NativeHandle handle) noexcept override {
        file_handle_mismatch = file_handle_mismatch || handle != 17;
        ++close_calls;
        if (close_returns_interrupted) {
            return durable::OperationResult::interrupted(
                std::make_error_code(std::errc::interrupted));
        }
        return close_fails ? durable::OperationResult::failed(injected_error())
                           : durable::OperationResult::succeeded();
    }

    [[nodiscard]] durable::OperationResult
    sync_parent_directory(durable::NativeHandle parent_handle) noexcept override {
        ++parent_calls;
        if (parent_handle != 11) {
            return durable::OperationResult::failed(
                std::make_error_code(std::errc::protocol_error));
        }
        if (parent_interrupts != 0) {
            --parent_interrupts;
            return durable::OperationResult::interrupted(
                std::make_error_code(std::errc::interrupted));
        }
        return parent_fails ? durable::OperationResult::failed(injected_error())
                            : durable::OperationResult::succeeded();
    }

    [[nodiscard]] durable::OperationResult
    close_parent_directory(durable::NativeHandle parent_handle) noexcept override {
        ++parent_close_calls;
        if (parent_handle != 11) {
            return durable::OperationResult::failed(
                std::make_error_code(std::errc::protocol_error));
        }
        if (parent_close_returns_interrupted) {
            return durable::OperationResult::interrupted(
                std::make_error_code(std::errc::interrupted));
        }
        return parent_close_fails ? durable::OperationResult::failed(injected_error())
                                  : durable::OperationResult::succeeded();
    }
};

class TempDirectory final {
public:
    TempDirectory() {
        path_ = gnfs::util::temp_path("gnfs_durable_immutable_" +
                                      std::to_string(gnfs::util::process_id()) + "_" +
                                      std::to_string(++sequence_));
        std::error_code error;
        const bool created = std::filesystem::create_directory(path_, error);
        if (!created || error) {
            std::cerr << "cannot create temp directory: " << error.message() << '\n';
            std::terminate();
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

constexpr std::array<std::byte, 5> PAYLOAD{std::byte{0x10}, std::byte{0x20}, std::byte{0x30},
                                           std::byte{0x40}, std::byte{0x50}};

[[nodiscard]] std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<std::byte> bytes;
    for (std::istreambuf_iterator<char> it(stream), end; it != end; ++it) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(*it)));
    }
    return bytes;
}

void test_injected_happy_path_and_retries() {
    TempDirectory directory;
    ScriptedFileOps ops;
    ops.parent_open_interrupts = 2;
    ops.file_open_interrupts = 1;
    ops.writes = {{durable::OperationState::succeeded, 1},
                  {durable::OperationState::interrupted, 0},
                  {durable::OperationState::succeeded, 2}};
    ops.sync_interrupts = 1;
    ops.parent_interrupts = 1;

    const auto result = durable::publish_with_ops(directory.path() / "record.bin", PAYLOAD, ops);
    CHECK(result.status() == durable::PublishStatus::durable);
    CHECK(result.is_durable());
    CHECK(!result.native_error());
    CHECK(result.bytes_written() == PAYLOAD.size());
    CHECK(ops.parent_open_calls == 3);
    CHECK(ops.file_open_calls == 2);
    CHECK(ops.write_calls == 4);
    CHECK(ops.sync_calls == 3);
    CHECK(ops.close_calls == 1);
    CHECK(ops.parent_calls == 2);
    CHECK(ops.parent_close_calls == 1);
    CHECK(!ops.file_handle_mismatch);
    CHECK(std::equal(ops.captured.begin(), ops.captured.end(), PAYLOAD.begin(), PAYLOAD.end()));
    CHECK(ops.opened_path.is_absolute());
    CHECK(ops.opened_path.filename() == "record.bin");
    CHECK(ops.parent_path == std::filesystem::weakly_canonical(directory.path()));
}

void test_injected_failures_are_never_durable() {
    TempDirectory directory;
    const auto path = directory.path() / "record.bin";

    {
        ScriptedFileOps ops;
        ops.parent_open_fails = true;
        const auto result = durable::publish_with_ops(path, PAYLOAD, ops);
        CHECK(result.status() == durable::PublishStatus::parent_directory_open_failed);
        CHECK(!result.is_durable());
        CHECK(result.bytes_written() == 0);
        CHECK(ops.close_calls == 0);
        CHECK(ops.parent_close_calls == 0);
    }
    {
        ScriptedFileOps ops;
        ops.parent_open_returns_invalid_handle = true;
        const auto result = durable::publish_with_ops(path, PAYLOAD, ops);
        CHECK(result.status() == durable::PublishStatus::file_ops_contract_violation);
        CHECK(!result.is_durable());
        CHECK(ops.file_open_calls == 0);
        CHECK(ops.close_calls == 0);
        CHECK(ops.parent_close_calls == 0);
    }
    {
        ScriptedFileOps ops;
        ops.file_open_fails = true;
        const auto result = durable::publish_with_ops(path, PAYLOAD, ops);
        CHECK(result.status() == durable::PublishStatus::open_failed);
        CHECK(!result.is_durable());
        CHECK(result.bytes_written() == 0);
        CHECK(ops.close_calls == 0);
        CHECK(ops.parent_close_calls == 1);
    }
    {
        ScriptedFileOps ops;
        ops.file_open_reports_existing = true;
        const auto result = durable::publish_with_ops(path, PAYLOAD, ops);
        CHECK(result.status() == durable::PublishStatus::already_exists);
        CHECK(!result.is_durable());
        CHECK(ops.close_calls == 0);
        CHECK(ops.parent_close_calls == 1);
    }
    {
        ScriptedFileOps ops;
        ops.file_open_reports_existing = true;
        ops.parent_close_fails = true;
        const auto result = durable::publish_with_ops(path, PAYLOAD, ops);
        CHECK(result.status() == durable::PublishStatus::parent_directory_close_failed);
        CHECK(!result.is_durable());
        CHECK(ops.close_calls == 0);
        CHECK(ops.parent_close_calls == 1);
    }
    {
        ScriptedFileOps ops;
        ops.file_open_returns_invalid_handle = true;
        const auto result = durable::publish_with_ops(path, PAYLOAD, ops);
        CHECK(result.status() == durable::PublishStatus::file_ops_contract_violation);
        CHECK(!result.is_durable());
        CHECK(ops.close_calls == 0);
        CHECK(ops.parent_close_calls == 1);
    }
    {
        ScriptedFileOps ops;
        ops.file_open_returns_parent_handle = true;
        const auto result = durable::publish_with_ops(path, PAYLOAD, ops);
        CHECK(result.status() == durable::PublishStatus::file_ops_contract_violation);
        CHECK(!result.is_durable());
        CHECK(ops.close_calls == 0);
        CHECK(ops.parent_close_calls == 1);
    }
    {
        ScriptedFileOps ops;
        ops.writes = {{durable::OperationState::succeeded, 2},
                      {durable::OperationState::failed, 0}};
        const auto result = durable::publish_with_ops(path, PAYLOAD, ops);
        CHECK(result.status() == durable::PublishStatus::write_failed);
        CHECK(result.bytes_written() == 2);
        CHECK(ops.close_calls == 1);
        CHECK(ops.parent_close_calls == 1);
        CHECK(ops.sync_calls == 0);
        CHECK(ops.parent_calls == 0);
    }
    {
        ScriptedFileOps ops;
        ops.writes = {{durable::OperationState::succeeded, 0}};
        const auto result = durable::publish_with_ops(path, PAYLOAD, ops);
        CHECK(result.status() == durable::PublishStatus::zero_write_progress);
        CHECK(result.bytes_written() == 0);
        CHECK(ops.close_calls == 1);
        CHECK(ops.parent_close_calls == 1);
    }
    {
        ScriptedFileOps ops;
        ops.writes = {{durable::OperationState::succeeded, PAYLOAD.size() + 1}};
        const auto result = durable::publish_with_ops(path, PAYLOAD, ops);
        CHECK(result.status() == durable::PublishStatus::file_ops_contract_violation);
        CHECK(result.bytes_written() == 0);
        CHECK(ops.close_calls == 1);
        CHECK(ops.parent_close_calls == 1);
    }
    {
        ScriptedFileOps ops;
        ops.sync_fails = true;
        const auto result = durable::publish_with_ops(path, PAYLOAD, ops);
        CHECK(result.status() == durable::PublishStatus::file_sync_failed);
        CHECK(result.bytes_written() == PAYLOAD.size());
        CHECK(ops.close_calls == 1);
        CHECK(ops.parent_close_calls == 1);
        CHECK(ops.parent_calls == 0);
    }
    {
        ScriptedFileOps ops;
        ops.close_fails = true;
        const auto result = durable::publish_with_ops(path, PAYLOAD, ops);
        CHECK(result.status() == durable::PublishStatus::close_failed);
        CHECK(result.bytes_written() == PAYLOAD.size());
        CHECK(ops.close_calls == 1);
        CHECK(ops.parent_calls == 1);
        CHECK(ops.parent_close_calls == 1);
    }
    {
        ScriptedFileOps ops;
        ops.close_returns_interrupted = true;
        const auto result = durable::publish_with_ops(path, PAYLOAD, ops);
        CHECK(result.status() == durable::PublishStatus::close_failed);
        CHECK(!result.is_durable());
        CHECK(ops.close_calls == 1);
        CHECK(ops.parent_close_calls == 1);
    }
    {
        ScriptedFileOps ops;
        ops.sync_fail_on_call = 2;
        const auto result = durable::publish_with_ops(path, PAYLOAD, ops);
        CHECK(result.status() == durable::PublishStatus::file_sync_failed);
        CHECK(!result.is_durable());
        CHECK(ops.sync_calls == 2);
        CHECK(ops.parent_calls == 1);
        CHECK(ops.close_calls == 1);
        CHECK(ops.parent_close_calls == 1);
    }
    {
        ScriptedFileOps ops;
        ops.parent_fails = true;
        const auto result = durable::publish_with_ops(path, PAYLOAD, ops);
        CHECK(result.status() == durable::PublishStatus::parent_directory_sync_failed);
        CHECK(result.bytes_written() == PAYLOAD.size());
        CHECK(ops.close_calls == 1);
        CHECK(ops.parent_calls == 1);
        CHECK(ops.parent_close_calls == 1);
    }
    {
        ScriptedFileOps ops;
        ops.parent_close_fails = true;
        const auto result = durable::publish_with_ops(path, PAYLOAD, ops);
        CHECK(result.status() == durable::PublishStatus::parent_directory_close_failed);
        CHECK(!result.is_durable());
        CHECK(result.bytes_written() == PAYLOAD.size());
        CHECK(ops.close_calls == 1);
        CHECK(ops.parent_close_calls == 1);
    }
    {
        ScriptedFileOps ops;
        ops.parent_close_returns_interrupted = true;
        const auto result = durable::publish_with_ops(path, PAYLOAD, ops);
        CHECK(result.status() == durable::PublishStatus::parent_directory_close_failed);
        CHECK(!result.is_durable());
        CHECK(ops.parent_close_calls == 1);
    }
}

void test_invalid_paths_do_not_reach_file_ops() {
    ScriptedFileOps ops;
    const auto empty = durable::publish_with_ops({}, PAYLOAD, ops);
    CHECK(empty.status() == durable::PublishStatus::invalid_path);
    CHECK(ops.parent_open_calls == 0);

    const auto root = durable::publish_with_ops(std::filesystem::path("/"), PAYLOAD, ops);
    CHECK(root.status() == durable::PublishStatus::invalid_path);
    CHECK(ops.parent_open_calls == 0);

    TempDirectory directory;
    ops.parent_open_fails = true;
    const auto missing_parent =
        durable::publish_with_ops(directory.path() / "missing" / "record.bin", PAYLOAD, ops);
    CHECK(missing_parent.status() == durable::PublishStatus::parent_directory_open_failed);
    CHECK(ops.parent_open_calls == 1);
}

void test_real_publish_is_exclusive_and_preserves_existing_bytes() {
    TempDirectory directory;
    const auto path = directory.path() / "record.bin";
    const auto first = durable::publish(path, PAYLOAD);
    CHECK(first.status() == durable::PublishStatus::durable);
    CHECK(first.bytes_written() == PAYLOAD.size());
    CHECK(read_bytes(path) == std::vector<std::byte>(PAYLOAD.begin(), PAYLOAD.end()));

    constexpr std::array<std::byte, 2> replacement{std::byte{0xaa}, std::byte{0xbb}};
    const auto second = durable::publish(path, replacement);
    CHECK(!second.is_durable());
    CHECK(second.status() == durable::PublishStatus::already_exists);
    CHECK(second.bytes_written() == 0);
    CHECK(read_bytes(path) == std::vector<std::byte>(PAYLOAD.begin(), PAYLOAD.end()));

    const auto directory_result = durable::publish(directory.path(), PAYLOAD);
    CHECK(!directory_result.is_durable());
}

void test_real_concurrent_publish_has_one_winner() {
    TempDirectory directory;
    const auto path = directory.path() / "contended.bin";
    constexpr std::size_t THREADS = 8;
    std::barrier start(static_cast<std::ptrdiff_t>(THREADS));
    std::array<durable::PublishStatus, THREADS> statuses{};
    std::array<std::thread, THREADS> workers;

    for (std::size_t index = 0; index < THREADS; ++index) {
        workers[index] = std::thread([&, index] {
            const std::array<std::byte, 1> value{static_cast<std::byte>(index + 1)};
            start.arrive_and_wait();
            statuses[index] = durable::publish(path, value).status();
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    CHECK(std::count(statuses.begin(), statuses.end(), durable::PublishStatus::durable) == 1);
    CHECK(std::count(statuses.begin(), statuses.end(), durable::PublishStatus::already_exists) ==
          THREADS - 1);
    const auto bytes = read_bytes(path);
    CHECK(bytes.size() == 1);
    CHECK(std::to_integer<unsigned>(bytes.front()) >= 1);
    CHECK(std::to_integer<unsigned>(bytes.front()) <= THREADS);
}

void test_real_symlink_leaf_is_not_followed() {
    TempDirectory directory;
    const auto target = directory.path() / "target.bin";
    const auto link = directory.path() / "record.bin";
    {
        std::ofstream stream(target, std::ios::binary);
        stream << "protected";
    }
    std::error_code error;
    std::filesystem::create_symlink(target, link, error);
    if (error) {
        return;
    }

    const auto before = read_bytes(target);
    const auto result = durable::publish(link, PAYLOAD);
    CHECK(result.status() == durable::PublishStatus::already_exists);
    CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(link)));
    CHECK(read_bytes(target) == before);
}

} // namespace

int main() {
    test_injected_happy_path_and_retries();
    test_injected_failures_are_never_durable();
    test_invalid_paths_do_not_reach_file_ops();
    test_real_publish_is_exclusive_and_preserves_existing_bytes();
    test_real_concurrent_publish_has_one_winner();
    test_real_symlink_leaf_is_not_followed();

    std::cout << "durable immutable file tests: " << checks_passed << " passed, " << checks_failed
              << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}

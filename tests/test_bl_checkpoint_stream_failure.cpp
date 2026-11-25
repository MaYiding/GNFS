#include "gnfs/linalg/bl_checkpoint.hpp"
#include "gnfs/util/bounded_child_process.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/temp_path.hpp"
#include "support/test_check.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

// Only the re-executed child arms this counter, immediately around the API
// call. Throw once without exhausting memory or affecting process startup.
thread_local bool allocation_probe_enabled = false;
thread_local bool allocation_failure_triggered = false;
thread_local size_t allocation_count = 0;
thread_local size_t failing_allocation = std::numeric_limits<size_t>::max();

void* allocate(size_t size) {
    if (allocation_probe_enabled && allocation_count++ == failing_allocation) {
        allocation_probe_enabled = false;
        allocation_failure_triggered = true;
        throw std::bad_alloc{};
    }
    if (void* pointer = std::malloc(size == 0 ? 1 : size))
        return pointer;
    throw std::bad_alloc{};
}

} // namespace

// Keep replacement allocation symbols visible to a shared standard library
// even when Release LTO inlines every allocation in this translation unit.
#if defined(__GNUC__) || defined(__clang__)
#define GNFS_TEST_ALLOCATION_REPLACEMENT __attribute__((used))
#else
#define GNFS_TEST_ALLOCATION_REPLACEMENT
#endif

GNFS_TEST_ALLOCATION_REPLACEMENT void* operator new(size_t size) {
    return allocate(size);
}
GNFS_TEST_ALLOCATION_REPLACEMENT void* operator new[](size_t size) {
    return allocate(size);
}
GNFS_TEST_ALLOCATION_REPLACEMENT void operator delete(void* pointer) noexcept {
    std::free(pointer);
}
GNFS_TEST_ALLOCATION_REPLACEMENT void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}
GNFS_TEST_ALLOCATION_REPLACEMENT void operator delete(void* pointer, size_t) noexcept {
    std::free(pointer);
}
GNFS_TEST_ALLOCATION_REPLACEMENT void operator delete[](void* pointer, size_t) noexcept {
    std::free(pointer);
}

#undef GNFS_TEST_ALLOCATION_REPLACEMENT

namespace {

using Checkpoint = gnfs::linalg::BlockLanczosCheckpoint;

Checkpoint make_state(uint64_t word) {
    Checkpoint checkpoint;
    checkpoint.rows = 1;
    checkpoint.cols = 1;
    checkpoint.aug_words_per_row = 1;
    checkpoint.cur_col = 1;
    checkpoint.matrix_fingerprint.emplace();
    checkpoint.aug = {word};
    return checkpoint;
}

constexpr uint64_t old_word = 0x0123'4567'89AB'CDEF;
constexpr uint64_t new_word = 0xFEDC'BA98'7654'3210;

struct FileCleanup {
    std::string path;
    ~FileCleanup() {
        std::remove(path.c_str());
    }
};

std::vector<char> read_bytes(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return {};
    const auto size = input.tellg();
    GNFS_TEST_CHECK(size >= 0 && size <= 1024);
    std::vector<char> bytes(static_cast<size_t>(size));
    input.seekg(0);
    if (!bytes.empty()) {
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        GNFS_TEST_CHECK(input.gcount() == static_cast<std::streamsize>(bytes.size()));
    }
    return bytes;
}

int child_main(std::string_view operation, std::string_view ordinal, const std::string& path) {
    const auto checkpoint = make_state(new_word);
    const bool measure = ordinal == "measure";
    if (!measure) {
        const auto parsed =
            std::from_chars(ordinal.data(), ordinal.data() + ordinal.size(), failing_allocation);
        if (parsed.ec != std::errc{} || parsed.ptr != ordinal.data() + ordinal.size())
            return 2;
    }
    std::set_terminate([] { std::_Exit(86); });
    allocation_count = 0;
    allocation_failure_triggered = false;
    allocation_probe_enabled = true;
    bool result = false;
    if (operation == "save")
        result = checkpoint.save(path);
    else if (operation == "exists")
        result = Checkpoint::exists_and_valid(path);
    else if (operation == "load")
        result = Checkpoint::load(path).has_value();
    else if (operation == "replacement") {
        // Take the address through a volatile pointer so this self-check calls
        // the replacement allocation function even in an optimized build.
        using Allocator = void* (*)(size_t);
        Allocator volatile allocator = &::operator new;
        try {
            void* pointer = allocator(1);
            ::operator delete(pointer);
            result = true;
        } catch (const std::bad_alloc&) {
            result = false;
        }
    } else
        return 3;
    allocation_probe_enabled = false;
    if (measure) {
        if (!result)
            return 4;
        std::printf("%zu\n", allocation_count);
        return 0;
    }
    // A skipped injection must never pass as evidence for exception handling.
    return allocation_failure_triggered && !result ? 0 : 5;
}

gnfs::util::BoundedChildProcessResult run_child(const std::filesystem::path& executable,
                                                const std::string& operation,
                                                const std::string& ordinal,
                                                const std::string& path) {
    gnfs::util::BoundedChildProcessSpec spec;
    spec.executable = executable;
    spec.arguments = {"--allocation-child", operation, ordinal, path};
    spec.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    spec.stdout_limit = 1024;
    spec.stderr_limit = 4096;
    spec.inherit_parent_environment = true;
    return gnfs::util::run_bounded_child_process(spec);
}

bool test_stream_allocation_failures(const std::filesystem::path& executable) {
    const auto old_state = make_state(old_word);
    const auto new_state = make_state(new_word);
    const std::string path = gnfs::util::temp_path(
        "gnfs-bl-stream-failure-" + std::to_string(gnfs::util::process_id()) + ".ckpt");
    FileCleanup cleanup{path};
    GNFS_TEST_CHECK(old_state.save(path));
    const auto old_bytes = read_bytes(path);
    GNFS_TEST_CHECK(old_bytes.size() == 128);
    GNFS_TEST_CHECK(new_state.save(path));
    const auto new_bytes = read_bytes(path);
    GNFS_TEST_CHECK(new_bytes.size() == 128 && new_bytes != old_bytes);

    const auto replacement_check = run_child(executable, "replacement", "0", path);
    GNFS_TEST_CHECK(replacement_check.succeeded());
    size_t injected_save_or_exists = 0;
    for (const std::string operation : {"save", "exists", "load"}) {
        GNFS_TEST_CHECK(old_state.save(path));
        const auto measured = run_child(executable, operation, "measure", path);
        GNFS_TEST_CHECK(measured.succeeded());
        const size_t count = std::stoull(measured.stdout_bytes);
        // Keep the process count bounded even if a library implementation
        // unexpectedly allocates far more than the small API call requires.
        GNFS_TEST_CHECK(count <= 32);
        if (count == 0) {
            // Some standard libraries implement file buffering with C
            // allocation. This operation then has no ordinary C++ allocation
            // site to inject; do not report unexercised exception coverage.
            std::cout << operation << ": NOT APPLICABLE (no C++ allocation observed)\n";
            continue;
        }
        if (operation == "save" || operation == "exists")
            ++injected_save_or_exists;
        for (size_t ordinal = 0; ordinal < count; ++ordinal) {
            GNFS_TEST_CHECK(old_state.save(path));
            const auto failed = run_child(executable, operation, std::to_string(ordinal), path);
            GNFS_TEST_CHECK(failed.succeeded());
            const auto bytes = read_bytes(path);
            if (operation != "save") {
                GNFS_TEST_CHECK(bytes == old_bytes);
            } else if (const auto loaded = Checkpoint::load(path)) {
                // Failure before opening may preserve the old checkpoint;
                // failure after a complete commit may leave the new one.
                GNFS_TEST_CHECK(bytes == old_bytes || bytes == new_bytes);
                GNFS_TEST_CHECK(loaded->aug == old_state.aug || loaded->aug == new_state.aug);
            } else {
                GNFS_TEST_CHECK(!Checkpoint::exists_and_valid(path));
            }
            // Allocation failure must not poison a subsequent normal save.
            GNFS_TEST_CHECK(new_state.save(path));
            GNFS_TEST_CHECK(Checkpoint::exists_and_valid(path));
            const auto loaded = Checkpoint::load(path);
            GNFS_TEST_CHECK(loaded && loaded->aug == new_state.aug);
        }
        std::cout << operation << ": " << count << " allocation failures returned safely\n";
    }
    return injected_save_or_exists != 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::string_view(argv[1]) == "--allocation-child")
        return child_main(argv[2], argv[3], argv[4]);
    GNFS_TEST_CHECK(argc == 1);
    if (!test_stream_allocation_failures(std::filesystem::absolute(argv[0]))) {
        std::cout << "SKIP: neither save nor exists has an injectable C++ allocation on this STL\n";
        return 77;
    }
    std::cout << "Checkpoint allocation failure checks passed for the reported applicable APIs\n";
}

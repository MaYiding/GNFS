#include "gnfs/util/process_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

size_t checks = 0;

[[noreturn]] void check_failed(const char* expression, int line) {
    throw std::runtime_error(std::string("CHECK failed at line ") + std::to_string(line) + ": " +
                             expression);
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(condition)) {                                                                        \
            check_failed(#condition, __LINE__);                                                    \
        }                                                                                          \
    } while (false)

using gnfs::util::process_memory_backend_name;
using gnfs::util::process_memory_snapshot;
using gnfs::util::ProcessMemoryBackend;
using gnfs::util::ProcessMemorySnapshot;
using gnfs::util::process_memory_detail::checked_units_to_bytes;

void test_backend_names() {
    static_assert(noexcept(process_memory_backend_name(ProcessMemoryBackend::Unsupported)));
    static_assert(noexcept(process_memory_snapshot()));
    static_assert(process_memory_backend_name(ProcessMemoryBackend::Unsupported) == "unsupported");
    static_assert(process_memory_backend_name(ProcessMemoryBackend::DarwinGetrusage) ==
                  "darwin_getrusage");
    static_assert(process_memory_backend_name(ProcessMemoryBackend::LinuxGetrusage) ==
                  "linux_getrusage");
    static_assert(process_memory_backend_name(ProcessMemoryBackend::WindowsPsapi) ==
                  "windows_psapi");

    CHECK(process_memory_backend_name(static_cast<ProcessMemoryBackend>(-1)) == "unsupported");

    const ProcessMemorySnapshot unsupported;
    CHECK(unsupported.backend == ProcessMemoryBackend::Unsupported);
    CHECK(!unsupported.current_rss_bytes.has_value());
    CHECK(!unsupported.lifetime_peak_rss_bytes.has_value());
}

void test_checked_bytes_normalization() {
    constexpr uint64_t max = std::numeric_limits<uint64_t>::max();

    static_assert(checked_units_to_bytes(0, 1) == uint64_t{0});
    static_assert(checked_units_to_bytes(1, 1) == uint64_t{1});
    static_assert(checked_units_to_bytes(1, 1024) == uint64_t{1024});
    static_assert(checked_units_to_bytes(max, 1) == max);
    static_assert(!checked_units_to_bytes(1, 0).has_value());
    static_assert(checked_units_to_bytes(max / 1024, 1024).has_value());
    static_assert(!checked_units_to_bytes(max / 1024 + 1, 1024).has_value());

    CHECK(checked_units_to_bytes(4096, 4096) == uint64_t{16'777'216});
}

[[nodiscard]] constexpr ProcessMemoryBackend expected_backend() noexcept {
#if defined(_WIN32)
    return ProcessMemoryBackend::WindowsPsapi;
#elif defined(__APPLE__)
    return ProcessMemoryBackend::DarwinGetrusage;
#elif defined(__linux__)
    return ProcessMemoryBackend::LinuxGetrusage;
#else
    return ProcessMemoryBackend::Unsupported;
#endif
}

void test_snapshot_contract() {
    const auto before = process_memory_snapshot();
    CHECK(before.backend == expected_backend());

    if (before.backend == ProcessMemoryBackend::Unsupported) {
        CHECK(!before.current_rss_bytes.has_value());
        CHECK(!before.lifetime_peak_rss_bytes.has_value());
        return;
    }

    CHECK(before.current_rss_bytes.has_value());
    CHECK(before.lifetime_peak_rss_bytes.has_value());
    CHECK(*before.current_rss_bytes > 0);
    CHECK(*before.lifetime_peak_rss_bytes > 0);

    // Keep a page-backed allocation alive across the second snapshot. The
    // writes prevent the pages from remaining merely reserved. We intentionally
    // assert only the lifetime high-water monotonicity, not a fixed growth:
    // startup or allocator activity may already have established a higher peak.
    constexpr size_t allocation_bytes = 8U * 1024U * 1024U;
    constexpr size_t touch_stride = 4096;
    std::vector<unsigned char> allocation(allocation_bytes);
    volatile unsigned char* touched = allocation.data();
    for (size_t offset = 0; offset < allocation.size(); offset += touch_stride) {
        touched[offset] = static_cast<unsigned char>(offset / touch_stride);
    }
    touched[allocation.size() - 1] = 1;

    const auto after = process_memory_snapshot();
    CHECK(after.backend == before.backend);
    CHECK(after.current_rss_bytes.has_value());
    CHECK(after.lifetime_peak_rss_bytes.has_value());
    CHECK(*after.current_rss_bytes > 0);
    CHECK(*after.lifetime_peak_rss_bytes >= *before.lifetime_peak_rss_bytes);

    const auto repeated = process_memory_snapshot();
    CHECK(repeated.backend == after.backend);
    CHECK(repeated.lifetime_peak_rss_bytes.has_value());
    CHECK(*repeated.lifetime_peak_rss_bytes >= *after.lifetime_peak_rss_bytes);
}

} // namespace

int main() {
    try {
        test_backend_names();
        test_checked_bytes_normalization();
        test_snapshot_contract();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "Process memory tests passed (" << checks << " checks)\n";
    return 0;
}

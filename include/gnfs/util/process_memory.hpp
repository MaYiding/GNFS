#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#elif defined(__linux__)
#include <cinttypes>
#include <cstdio>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace gnfs::util {

/// Native source used for the current-process resident-memory snapshot.
enum class ProcessMemoryBackend {
    Unsupported,
    DarwinGetrusage,
    LinuxGetrusage,
    WindowsPsapi,
};

/// Stable, allocation-free name suitable for structured measurement records.
[[nodiscard]] constexpr std::string_view
process_memory_backend_name(ProcessMemoryBackend backend) noexcept {
    switch (backend) {
    case ProcessMemoryBackend::Unsupported:
        return "unsupported";
    case ProcessMemoryBackend::DarwinGetrusage:
        return "darwin_getrusage";
    case ProcessMemoryBackend::LinuxGetrusage:
        return "linux_getrusage";
    case ProcessMemoryBackend::WindowsPsapi:
        return "windows_psapi";
    }
    return "unsupported";
}

/// One observation of the calling process. Both quantities cover every thread
/// in this process and exclude child processes. Values are always bytes.
///
/// `lifetime_peak_rss_bytes` is the process high-water mark since launch; it is
/// not reset by taking a snapshot. Callers that need isolated scenario peaks
/// should run each scenario in a fresh process.
struct ProcessMemorySnapshot final {
    ProcessMemoryBackend backend = ProcessMemoryBackend::Unsupported;
    std::optional<uint64_t> current_rss_bytes;
    std::optional<uint64_t> lifetime_peak_rss_bytes;
};

namespace process_memory_detail {

/// Convert an unsigned native count to bytes without wrapping. A zero unit
/// size is invalid rather than being treated as a successful zero-byte sample.
[[nodiscard]] constexpr std::optional<uint64_t>
checked_units_to_bytes(uint64_t units, uint64_t bytes_per_unit) noexcept {
    if (bytes_per_unit == 0 || units > std::numeric_limits<uint64_t>::max() / bytes_per_unit) {
        return std::nullopt;
    }
    return units * bytes_per_unit;
}

#if defined(_WIN32)

[[nodiscard]] inline ProcessMemorySnapshot windows_process_memory_snapshot() noexcept {
    ProcessMemorySnapshot snapshot;
    snapshot.backend = ProcessMemoryBackend::WindowsPsapi;

    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = static_cast<DWORD>(sizeof(counters));
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, counters.cb) == FALSE) {
        return snapshot;
    }

    snapshot.current_rss_bytes =
        checked_units_to_bytes(static_cast<uint64_t>(counters.WorkingSetSize), 1);
    snapshot.lifetime_peak_rss_bytes =
        checked_units_to_bytes(static_cast<uint64_t>(counters.PeakWorkingSetSize), 1);
    return snapshot;
}

#elif defined(__APPLE__)

[[nodiscard]] inline std::optional<uint64_t> darwin_current_rss_bytes() noexcept {
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    const kern_return_t status = ::task_info(::mach_task_self(), MACH_TASK_BASIC_INFO,
                                             reinterpret_cast<task_info_t>(&info), &count);
    if (status != KERN_SUCCESS || count < MACH_TASK_BASIC_INFO_COUNT) {
        return std::nullopt;
    }
    return checked_units_to_bytes(static_cast<uint64_t>(info.resident_size), 1);
}

[[nodiscard]] inline std::optional<uint64_t> darwin_peak_rss_bytes() noexcept {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) {
        return std::nullopt;
    }
    // Darwin reports ru_maxrss in bytes.
    return checked_units_to_bytes(static_cast<uint64_t>(usage.ru_maxrss), 1);
}

[[nodiscard]] inline ProcessMemorySnapshot darwin_process_memory_snapshot() noexcept {
    ProcessMemorySnapshot snapshot;
    snapshot.backend = ProcessMemoryBackend::DarwinGetrusage;
    snapshot.current_rss_bytes = darwin_current_rss_bytes();
    snapshot.lifetime_peak_rss_bytes = darwin_peak_rss_bytes();
    return snapshot;
}

#elif defined(__linux__)

[[nodiscard]] inline std::optional<uint64_t> linux_current_rss_bytes() noexcept {
    std::FILE* statm = std::fopen("/proc/self/statm", "r");
    if (statm == nullptr) {
        return std::nullopt;
    }

    uint64_t resident_pages = 0;
    const int fields_read = std::fscanf(statm, "%*" SCNu64 " %" SCNu64, &resident_pages);
    const int close_status = std::fclose(statm);
    if (fields_read != 1 || close_status != 0) {
        return std::nullopt;
    }

    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return std::nullopt;
    }
    return checked_units_to_bytes(resident_pages, static_cast<uint64_t>(page_size));
}

[[nodiscard]] inline std::optional<uint64_t> linux_peak_rss_bytes() noexcept {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) {
        return std::nullopt;
    }
    // Linux reports ru_maxrss in KiB.
    return checked_units_to_bytes(static_cast<uint64_t>(usage.ru_maxrss), UINT64_C(1024));
}

[[nodiscard]] inline ProcessMemorySnapshot linux_process_memory_snapshot() noexcept {
    ProcessMemorySnapshot snapshot;
    snapshot.backend = ProcessMemoryBackend::LinuxGetrusage;
    snapshot.current_rss_bytes = linux_current_rss_bytes();
    snapshot.lifetime_peak_rss_bytes = linux_peak_rss_bytes();
    return snapshot;
}

#endif

} // namespace process_memory_detail

/// Capture current and lifetime-peak resident memory for this process.
///
/// The function never throws. Unsupported platforms return the Unsupported
/// backend with both optional values disengaged. A supported backend remains
/// identified even if one native query fails; only the failed field is nullopt.
[[nodiscard]] inline ProcessMemorySnapshot process_memory_snapshot() noexcept {
#if defined(_WIN32)
    return process_memory_detail::windows_process_memory_snapshot();
#elif defined(__APPLE__)
    return process_memory_detail::darwin_process_memory_snapshot();
#elif defined(__linux__)
    return process_memory_detail::linux_process_memory_snapshot();
#else
    return {};
#endif
}

} // namespace gnfs::util

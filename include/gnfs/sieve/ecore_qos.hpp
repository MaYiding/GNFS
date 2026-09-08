#pragma once

// Sieve E-core utilization policy (BACKLOG #4 macOS heterogeneous scheduling).
//
// macOS schedules std::thread on P-cores by default (workers inherit parent QoS
// class UserInitiated). On Apple Silicon M5 (4 P + 6 E), lattice_sieve thus uses
// only ~40% of the CPU. BACKLOG #4 records 50d V0+fix 实测 average 3.5-4 cores
// usage during sieve, P-cores saturated while E-cores idle.
//
// This helper lets the user opt-in via GNFS_SIEVE_ECORE_THREADS=N to put N of
// the sieve worker threads on Utility QoS — a hint to the macOS scheduler to
// place them on E-cores. Mixing P+E lock-step generally hurts (slowest-core
// barrier), but lattice_sieve uses work-stealing (atomic fetch_add over region
// index) so faster cores naturally grab more regions, making the design robust
// to heterogeneous cores.
//
// Defaults preserve prior behavior:
//   ENV unset / "" / non-numeric / <= 0:   ecore_count = 0   (all UserInitiated)
//   ENV > 0:                                ecore_count = min(N, num_threads-1)
//
// Linux: gnfs::util::set_current_thread_qos is a no-op (kernel scheduler free),
// so this helper's effect is macOS-only. The helper itself remains portable.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string_view>

#include "../util/thread_pool.hpp" // For QoSClass + set_current_thread_qos

namespace gnfs::sieve {

// Resolve GNFS_SIEVE_ECORE_THREADS env to a thread count under the cap
// (num_threads - 1) — always keep at least one P-core thread for master work.
//
//   env == nullptr or "" → 0 (current behavior, all threads UserInitiated)
//   env > 0   → min(env, num_threads - 1)
//   env <= 0  → 0  (treat negative / parse failure as opt-out)
inline size_t resolve_ecore_thread_count(size_t num_threads, const char* env) noexcept {
    if (!env || env[0] == '\0')
        return 0;
    if (num_threads <= 1)
        return 0;

    const char* first = env;
    while (*first == ' ' || *first == '\t' || *first == '\n' || *first == '\r' || *first == '\f' ||
           *first == '\v') {
        ++first;
    }
    if (*first == '-')
        return 0;

    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(env, &end, 10);
    if (end == env || value == 0)
        return 0;

    const size_t cap = num_threads - 1;
    if (errno == ERANGE || static_cast<std::uintmax_t>(value) >= static_cast<std::uintmax_t>(cap)) {
        return cap;
    }
    return static_cast<size_t>(value);
}

// String-view overload for captured execution-policy environments. This keeps
// distributed and direct sieve paths on the same overflow/clamping contract
// without requiring a temporary null-terminated allocation.
inline size_t resolve_ecore_thread_count(size_t num_threads, std::string_view env) noexcept {
    if (env.empty() || num_threads <= 1)
        return 0;

    size_t first = 0;
    while (first < env.size()) {
        const char value = env[first];
        if (value != ' ' && value != '\t' && value != '\n' && value != '\r' && value != '\f' &&
            value != '\v') {
            break;
        }
        ++first;
    }
    if (first == env.size() || env[first] == '-')
        return 0;
    if (env[first] == '+')
        ++first;

    const size_t digits_begin = first;
    std::uintmax_t value = 0;
    bool overflow = false;
    for (; first < env.size() && env[first] >= '0' && env[first] <= '9'; ++first) {
        const auto digit = static_cast<std::uintmax_t>(env[first] - '0');
        if (value > (std::numeric_limits<std::uintmax_t>::max() - digit) / 10U) {
            overflow = true;
        } else if (!overflow) {
            value = value * 10U + digit;
        }
    }
    if (first == digits_begin || value == 0)
        return 0;

    const size_t cap = num_threads - 1;
    if (overflow || value >= static_cast<std::uintmax_t>(cap))
        return cap;
    return static_cast<size_t>(value);
}

// Convenience overload reading GNFS_SIEVE_ECORE_THREADS directly.
inline size_t resolve_ecore_thread_count(size_t num_threads) noexcept {
    return resolve_ecore_thread_count(num_threads, std::getenv("GNFS_SIEVE_ECORE_THREADS"));
}

// Decide per-thread QoS class given (thread_idx, num_threads, ecore_count).
// First (num_threads - ecore_count) threads keep UserInitiated (P-core hint).
// Last ecore_count threads → Utility (E-core hint).
inline gnfs::util::QoSClass qos_for_sieve_thread(size_t thread_idx, size_t num_threads,
                                                 size_t ecore_count) noexcept {
    if (ecore_count == 0 || thread_idx + ecore_count < num_threads) {
        return gnfs::util::QoSClass::UserInitiated;
    }
    return gnfs::util::QoSClass::Utility;
}

} // namespace gnfs::sieve

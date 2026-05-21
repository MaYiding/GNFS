#pragma once

// Software prefetch helpers for the bucket sieve scatter / gather hot loops.
//
// Scope
// -----
// The bucket sieve scatter phase emits hits at addresses determined by the
// per-prime stride pattern, and the gather phase walks accumulated entries
// back into the sieve array. Both access patterns miss the L1 cache often
// because (a) scatter targets jump across bucket region boundaries and
// (b) gather reads each region's entry vector sequentially while writing
// non-contiguous offsets within `sieve_array_`. Speculative writes / reads
// further down the iteration window can be hinted to the memory subsystem
// via `__builtin_prefetch` to overlap the address resolution latency with
// the current iteration's arithmetic.
//
// Correctness
// -----------
// `__builtin_prefetch` is a hint to the CPU and the compiler. It never
// changes program state, never traps on invalid addresses, and is guaranteed
// to leave the abstract machine unchanged. The bucket sieve output is
// therefore bit-for-bit identical regardless of whether prefetch is enabled,
// disabled, or unsupported at compile time. The runtime ENV gate exists
// purely for regression-bisect investigations and PMU sweeps — production
// runs should leave it at the default (auto = on when supported).
//
// Build-time guards
// -----------------
// `__builtin_prefetch` is supported by GCC and Clang (including Apple
// Clang). On any other compiler the helpers compile to no-ops and the
// runtime gate reports unavailable. This keeps the header drop-in for
// MSVC / Intel ICC without conditional includes at every call site.

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace gnfs::sieve {

/// Bucket prefetch look-ahead distance in iterations. Tuned empirically
/// against M-series memory latency (~5-7 ns L2 hit, ~80-100 ns DRAM) and
/// per-iteration scatter cost (~2-3 ns) — eight iterations ahead lets the
/// L1 fill complete before the load issues. Larger distances waste prefetch
/// bandwidth, smaller ones still pay miss latency on cache-cold buckets.
inline constexpr std::size_t kBucketPrefetchDistance = 8;

/// Returns true when `__builtin_prefetch` is supported by the active
/// compiler. The runtime gate `bucket_prefetch_enabled()` falls back to
/// this when ENV is unset or set to "auto".
[[nodiscard]] inline constexpr bool bucket_prefetch_supported() noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return true;
#else
    return false;
#endif
}

/// Hint the memory subsystem to fetch `ptr` into the L1 with write intent
/// and temporal locality bias T1. Used at the scatter phase where the next
/// few iterations will append a `BucketRegionEntry` into the same vector.
inline void prefetch_bucket_write([[maybe_unused]] const void* ptr) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(ptr, 1, 1);
#endif
}

/// Hint the memory subsystem to fetch `ptr` into the L1 with read intent
/// and temporal locality bias T1. Used at the gather phase where the next
/// few iterations will read another `BucketRegionEntry` and update the
/// matching `sieve_array_` slot.
inline void prefetch_bucket_read([[maybe_unused]] const void* ptr) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(ptr, 0, 1);
#endif
}

namespace detail {

/// Cached three-state ENV gate. The first call resolves the gate based on
/// the value of `GNFS_BUCKET_PREFETCH`:
///   "0"     → disabled (force scalar, regression bisect)
///   "1"     → enabled when the compiler supports `__builtin_prefetch`
///   "auto"  → same as enabled-if-supported
///   unset   → same as auto
///   other   → treated as auto
inline std::atomic<bool>& cached_gate_state() noexcept {
    static std::atomic<bool> state{false};
    return state;
}

inline std::once_flag& cached_gate_flag() noexcept {
    static std::once_flag flag;
    return flag;
}

inline bool resolve_gate_from_env() noexcept {
    const char* v = std::getenv("GNFS_BUCKET_PREFETCH");
    if (v == nullptr) {
        return bucket_prefetch_supported();
    }
    if (std::strcmp(v, "0") == 0) {
        return false;
    }
    if (std::strcmp(v, "1") == 0) {
        // Forced on, but never claim support we lack.
        return bucket_prefetch_supported();
    }
    // "auto" or anything else → default behaviour.
    return bucket_prefetch_supported();
}

}  // namespace detail

/// Returns whether the bucket prefetch helpers should issue real prefetch
/// hints at the current call site. Result is cached after the first call
/// so the inner sieve loops branch on a single relaxed atomic load.
///
/// Tests that toggle `GNFS_BUCKET_PREFETCH` mid-process can re-resolve the
/// gate via `reload_bucket_prefetch_gate()` defined below.
[[nodiscard]] inline bool bucket_prefetch_enabled() noexcept {
    std::call_once(detail::cached_gate_flag(), []() noexcept {
        detail::cached_gate_state().store(detail::resolve_gate_from_env(),
                                          std::memory_order_relaxed);
    });
    return detail::cached_gate_state().load(std::memory_order_relaxed);
}

/// Re-read the ENV and update the cached gate state. Intended for unit
/// tests that flip `GNFS_BUCKET_PREFETCH` between scenarios. Not called
/// from the sieve hot path — production resolves the gate exactly once.
inline void reload_bucket_prefetch_gate() noexcept {
    detail::cached_gate_state().store(detail::resolve_gate_from_env(),
                                      std::memory_order_relaxed);
    // Ensure subsequent calls see the new value even if `call_once` was
    // never triggered before (the flag must record completion so the
    // lambda does not run later and overwrite us).
    std::call_once(detail::cached_gate_flag(), []() noexcept {
        // No-op: state already set above.
    });
}

}  // namespace gnfs::sieve

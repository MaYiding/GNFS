#pragma once

// Integer thread-local scratch pool (W8 T5).
//
// Hot paths in GNFS create large numbers of temporary `gnfs::core::Integer`
// objects (e.g. cofactor pipelines, Hensel lifts, Schirokauer map compute,
// inner ECM arithmetic). Every Integer ctor calls `mpz_init` and the dtor
// calls `mpz_clear`. While the mpz_t header itself is stack-allocated by
// the Integer member, the GMP-internal limb buffer is heap-allocated on
// demand. In tight loops this drives:
//   * Per-temporary `mpz_init` (small, but not free)
//   * Per-temporary heap alloc + free for limbs (significant under
//     contention on macOS / M5 multi-core dispatch)
//   * Heap fragmentation when many short-lived Integers churn the allocator
//
// This helper provides an opt-in `thread_local` pool of `Integer` objects.
// Borrowing returns an `IntegerScratchHandle`; the RAII destructor returns
// the Integer to the per-thread pool rather than freeing it. The next
// borrow can reuse the Integer (and crucially its already-allocated limb
// buffer via `mpz_realloc` semantics — when the next user assigns a value
// of comparable size, GMP can reuse the existing limb storage).
//
// What this helper actually saves vs default path:
//   1. struct header init/clear churn (small but constant overhead)
//   2. limb buffer alloc/free churn (large for tight loops over big values)
//   3. allocator-level fragmentation pressure (significant under SMP load)
//
// What this helper does NOT do:
//   * It does NOT change Integer's API or behavior. Any value computed
//     through a borrowed Integer is bit-for-bit identical to one computed
//     through a fresh Integer.
//   * It does NOT modify any main pipeline path. This is helper-only
//     infrastructure; opt-in wire-ins must be added by callers.
//
// ENV control:
//   * GNFS_INTEGER_SCRATCH_POOL=1  → pool enabled
//   * unset / "0" / anything else  → pool disabled (default, zero overhead)
//
// Per-thread storage:
//   * The pool is `thread_local`. Each thread has its own pool. There is
//     no inter-thread synchronization. Borrows from one thread never see
//     Integers returned by another thread.
//
// Bit-for-bit guarantee:
//   * Borrowed Integers are reset to 0 at borrow time (via `Integer(0)`
//     assignment, which goes through `mpz_set_si(value, 0)` and does NOT
//     reallocate limbs unless larger). Caller fills value as normal.
//   * Returned Integers retain their last value internally; the next
//     borrow's reset-to-0 clears them. No leakage of state.
//
// Move safety:
//   * `IntegerScratchHandle` is movable. The moved-from handle is marked
//     "already returned" and will NOT push the (now-empty) Integer back
//     to the pool on destruction. The moved-to handle assumes ownership.
//
// Thread-exit safety:
//   * On thread exit, the `thread_local std::vector<Integer>` destructor
//     runs, which destroys each pooled Integer, each calling `mpz_clear`.
//     No limb leak. No need for explicit teardown.

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "gnfs/core/integer.hpp"

namespace gnfs::util {

namespace detail {

inline std::once_flag& integer_scratch_pool_env_once_flag() {
    static std::once_flag flag;
    return flag;
}

inline std::atomic<bool>& integer_scratch_pool_env_value() {
    static std::atomic<bool> value{false};
    return value;
}

inline bool parse_integer_scratch_pool_env() {
    const char* env = std::getenv("GNFS_INTEGER_SCRATCH_POOL");
    if (env == nullptr) return false;
    // Strict matching: only "1" enables, all other values (incl. "0",
    // "true", "yes", empty string, "01", "2") evaluate to false. This
    // mirrors the boolean ENV convention used by other GNFS helpers
    // (e.g. GNFS_FILTER_RADIX_SORT, GNFS_V0_WEIGHT3).
    return std::string(env) == "1";
}

/// Per-thread scratch storage.
///
/// `inline thread_local` is C++17 and ensures a single per-thread instance
/// across all translation units that include this header. The destructor
/// runs at thread exit and calls each pooled Integer's dtor (mpz_clear).
inline thread_local std::vector<gnfs::core::Integer> tls_scratch_pool;

}  // namespace detail

/// Read GNFS_INTEGER_SCRATCH_POOL and return whether the pool is enabled.
///
/// Cached via std::call_once + std::atomic. The first invocation parses
/// the environment; subsequent invocations return the cached value with
/// only an atomic load (no getenv on hot path).
///
///   * Unset or empty: returns false (pool disabled)
///   * "1": returns true (pool enabled)
///   * Any other value ("0", "true", "yes", etc.): returns false
[[nodiscard]] inline bool integer_scratch_pool_enabled() noexcept {
    std::call_once(detail::integer_scratch_pool_env_once_flag(), []() {
        detail::integer_scratch_pool_env_value().store(
            detail::parse_integer_scratch_pool_env(),
            std::memory_order_release);
    });
    return detail::integer_scratch_pool_env_value().load(
        std::memory_order_acquire);
}

/// Test-only: re-parse GNFS_INTEGER_SCRATCH_POOL.
///
/// NOT thread-safe — call only from single-threaded test setup. The cached
/// once_flag is not reset; this helper directly overwrites the cached
/// atomic value, so a subsequent call to `integer_scratch_pool_enabled()`
/// returns the freshly parsed value without re-invoking the once_flag
/// initializer.
inline void integer_scratch_pool_reset_env_cache_for_testing() noexcept {
    detail::integer_scratch_pool_env_value().store(
        detail::parse_integer_scratch_pool_env(),
        std::memory_order_release);
}

/// RAII borrow handle for a thread-local pooled `Integer`.
///
/// Construction:
///   * If the pool is enabled AND the per-thread pool has at least one
///     Integer available, pop one off the back, reset it to 0, and adopt
///     it. The Integer's pre-existing limb buffer is retained (GMP frees
///     limbs on mpz_clear, not on mpz_set_si).
///   * Otherwise (pool disabled, or empty), default-construct a fresh
///     Integer.
///
/// Destruction:
///   * If the pool is enabled AND this handle still owns its Integer
///     (i.e. has not been moved-from), push the Integer back to the
///     per-thread pool. The Integer is NOT cleared; the next borrow
///     resets it to 0.
///   * Otherwise (pool disabled, or already moved-from), the Integer's
///     own dtor runs, calling mpz_clear in the normal way.
///
/// Move semantics:
///   * Move-construct / move-assign transfer ownership. The moved-from
///     handle is marked "already returned" so its dtor will NOT push the
///     (now hollow) Integer to the pool.
///
/// Access:
///   * `get()` / `operator*` / `operator->` provide direct access to the
///     underlying Integer. Caller may use any Integer operation including
///     assignment, arithmetic, and conversion. The Integer behaves
///     identically to a fresh one from the caller's perspective.
class IntegerScratchHandle {
public:
    IntegerScratchHandle() : returned_(false) {
        if (integer_scratch_pool_enabled()) {
            if (!detail::tls_scratch_pool.empty()) {
                // Adopt a pooled Integer. Move out of the pool to avoid
                // copy + limb-buffer alloc.
                value_ = std::move(detail::tls_scratch_pool.back());
                detail::tls_scratch_pool.pop_back();
                // Reset to 0. mpz_set_si(value, 0) sets the size to 0
                // but retains the limb buffer for reuse by subsequent
                // assignments. This is precisely the optimization the
                // pool aims to capture.
                value_ = gnfs::core::Integer(static_cast<int64_t>(0));
            }
            // else: pool empty, value_ is a fresh default-constructed Integer
        }
        // else: pool disabled, value_ is a fresh default-constructed Integer
    }

    ~IntegerScratchHandle() {
        if (!returned_ && integer_scratch_pool_enabled()) {
            // Return the Integer to the per-thread pool. push_back may
            // reallocate the vector backing storage, but the Integer
            // itself moves cheaply (transfers mpz_t header + limb ptr).
            try {
                detail::tls_scratch_pool.push_back(std::move(value_));
            } catch (...) {
                // If push_back throws (e.g. OOM during vector grow), let
                // the Integer's dtor run normally. Swallow the exception
                // to keep ~IntegerScratchHandle noexcept-friendly.
            }
        }
        // Otherwise: value_ dtor runs normally (mpz_clear).
    }

    // Non-copyable: a borrow is single-owner.
    IntegerScratchHandle(const IntegerScratchHandle&) = delete;
    IntegerScratchHandle& operator=(const IntegerScratchHandle&) = delete;

    // Movable: transfer ownership. The moved-from handle is marked as
    // "returned" so it does not double-push to the pool.
    IntegerScratchHandle(IntegerScratchHandle&& other) noexcept
        : value_(std::move(other.value_)),
          returned_(other.returned_) {
        other.returned_ = true;  // Prevent moved-from dtor from pushing.
    }

    IntegerScratchHandle& operator=(IntegerScratchHandle&& other) noexcept {
        if (this != &other) {
            // Release current resource back to pool (if owned).
            if (!returned_ && integer_scratch_pool_enabled()) {
                try {
                    detail::tls_scratch_pool.push_back(std::move(value_));
                } catch (...) {
                    // Same swallow rationale as dtor.
                }
            }
            value_ = std::move(other.value_);
            returned_ = other.returned_;
            other.returned_ = true;
        }
        return *this;
    }

    /// Direct access to the underlying Integer.
    [[nodiscard]] gnfs::core::Integer& get() noexcept { return value_; }
    [[nodiscard]] const gnfs::core::Integer& get() const noexcept { return value_; }

    /// Dereference shorthand.
    [[nodiscard]] gnfs::core::Integer& operator*() noexcept { return value_; }
    [[nodiscard]] const gnfs::core::Integer& operator*() const noexcept { return value_; }

    /// Member access shorthand.
    [[nodiscard]] gnfs::core::Integer* operator->() noexcept { return &value_; }
    [[nodiscard]] const gnfs::core::Integer* operator->() const noexcept { return &value_; }

private:
    gnfs::core::Integer value_;
    bool returned_;
};

/// Returns the current number of Integers held in the calling thread's
/// scratch pool. Mainly useful for testing and debugging.
[[nodiscard]] inline std::size_t integer_scratch_pool_size() noexcept {
    return detail::tls_scratch_pool.size();
}

/// Releases all pooled Integers in the calling thread's scratch pool.
/// Each pooled Integer is destroyed (mpz_clear), freeing the limb buffers.
/// After this call, `integer_scratch_pool_size()` returns 0 until the
/// next borrow that gets returned.
inline void integer_scratch_pool_clear() noexcept {
    detail::tls_scratch_pool.clear();
}

}  // namespace gnfs::util

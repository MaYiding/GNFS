#pragma once

// Batch-pivot ENV gate for Structured Gaussian Elimination (SGE).
//
// Scope
// -----
// SGE eliminates weight-1 (singleton) and weight-2 (pair) columns one at a
// time. For large matrices (e.g., 50d+ runs with ~1M columns), the linear
// sweep across columns dominates SGE wall time. When several pivots have
// disjoint row supports — they touch different rows — they can be selected
// and applied together in a single pass without serialising on shared rows.
// The `GNFS_SGE_BATCH_PIVOTS=N` environment variable opts the driver into
// this batched selection strategy.
//
// Semantics
// ---------
// - N=1 (default): the original sequential pass is used, byte-for-byte
//   identical to the historical SGE driver. No additional work is performed
//   to detect conflicts; the original loops run unchanged.
// - N in [2, 64]: each Phase tries to gather up to N pivots whose row
//   supports are disjoint from the rows already chosen in the current batch.
//   Pivots that conflict with the current batch are deferred to a later
//   batch in the same pass, or to a later pass. Within a batch the row
//   updates are independent and can be applied in any order.
// - Values outside [1, 64] are clamped:
//     N < 1   → clamped to 1 (defensive; std::strtol returns 0 for "0")
//     N > 64  → clamped to 64
//   Non-numeric or unset values default to 1.
//
// Equivalence invariant
// ---------------------
// The batched path may select pivots in a different order than the
// sequential path. Because pivots within a single batch act on disjoint
// rows, applying them in any order produces the same row state after the
// batch as applying them sequentially in any chosen order. The final
// reduced matrix therefore satisfies:
//
//   1. Same number of surviving rows and columns.
//   2. Same set of surviving column identifiers (the `col_map` content).
//   3. Same composition multiset per surviving row (composition is the
//      XOR of original-row indices, which is order-independent in GF(2)).
//
// Surviving rows may appear in a different positional order inside the
// reduced matrix because the merge sequence influences which row index
// survives a given weight-2 merge (the "heavier row" tiebreak fires at
// different times). Tests therefore compare canonicalised rows: sort each
// reduced row's column-index list, then sort the row collection.
//
// Header-only design mirrors `bucket_prefetch.hpp` to keep the gate
// resolvable from `sge.hpp` without introducing a TU.

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace gnfs::linalg {

/// Maximum supported batch size. The bound is intentionally small because
/// the conflict-detection bookkeeping is O(N) per candidate pivot, and
/// the disjoint-row constraint makes large N degenerate quickly back to
/// sequential when row supports are dense.
inline constexpr int kSGEBatchPivotsMax = 64;

/// Default batch size when the ENV is unset or invalid. The default value
/// keeps the historical sequential path active for production runs.
inline constexpr int kSGEBatchPivotsDefault = 1;

namespace detail {

inline std::atomic<int>& sge_batch_pivots_cached_value() noexcept {
    static std::atomic<int> value{kSGEBatchPivotsDefault};
    return value;
}

inline std::once_flag& sge_batch_pivots_once_flag() noexcept {
    static std::once_flag flag;
    return flag;
}

/// Parse the ENV exactly once. Returns the clamped batch size in
/// [1, kSGEBatchPivotsMax]. Anything that does not parse as a positive
/// integer falls back to the default sequential mode.
inline int sge_batch_pivots_resolve_from_env() noexcept {
    const char* raw = std::getenv("GNFS_SGE_BATCH_PIVOTS");
    if (raw == nullptr || raw[0] == '\0') {
        return kSGEBatchPivotsDefault;
    }
    char* endptr = nullptr;
    const long parsed = std::strtol(raw, &endptr, 10);
    if (endptr == raw) {
        // Not a number at all (e.g., "garbage"). Default to sequential.
        return kSGEBatchPivotsDefault;
    }
    // Reject negatives, fractional remainder, and out-of-range silently.
    if (parsed < 1) {
        return kSGEBatchPivotsDefault;
    }
    if (parsed > static_cast<long>(kSGEBatchPivotsMax)) {
        return kSGEBatchPivotsMax;
    }
    return static_cast<int>(parsed);
}

}  // namespace detail

/// Returns the active SGE batch-pivot size. Caches the ENV lookup on the
/// first call; subsequent calls cost a single relaxed atomic load. Hot
/// paths inside SGE may capture the value once per `preprocess` call and
/// branch on the cached int instead of re-reading the atomic per pivot.
[[nodiscard]] inline int sge_batch_pivots_size() noexcept {
    std::call_once(detail::sge_batch_pivots_once_flag(), []() noexcept {
        detail::sge_batch_pivots_cached_value().store(
            detail::sge_batch_pivots_resolve_from_env(),
            std::memory_order_relaxed);
    });
    return detail::sge_batch_pivots_cached_value().load(
        std::memory_order_relaxed);
}

/// Test-only hook to re-read the ENV after mutating it from a unit test.
/// Production code resolves the gate exactly once via `call_once` and must
/// not invoke this helper from the hot path. The implementation mirrors
/// `reload_bucket_prefetch_gate()` for consistency.
inline void sge_batch_pivots_reset_env_cache_for_testing() noexcept {
    detail::sge_batch_pivots_cached_value().store(
        detail::sge_batch_pivots_resolve_from_env(),
        std::memory_order_relaxed);
    // Mark the once_flag as completed so the lambda above never fires
    // later and clobbers the freshly read value.
    std::call_once(detail::sge_batch_pivots_once_flag(),
                   []() noexcept { /* state set above */ });
}

}  // namespace gnfs::linalg

#pragma once

// Memory pool for RelationCollector (W6 T4).
//
// Backs RelationCollector::relations_ via std::pmr::monotonic_buffer_resource
// to replace the std::vector<Relation> per-element malloc/free pattern with a
// bump allocator over a large pre-sized chunk. The default upstream resource
// (std::pmr::new_delete_resource) is consulted only when the initial chunk is
// exhausted and a new geometric chunk needs to be allocated.
//
// Default OFF (zero overhead): collector retains the std::allocator path
// unless GNFS_RELATION_POOL_SIZE is set to a positive byte count, in which
// case relation_pool_enabled() returns true and the collector switches to a
// std::pmr::vector<Relation> backed by RelationPoolResource.
//
// Correctness: pool-on and pool-off paths produce bit-for-bit identical
// relation sets (same (a,b) sequence in, same vector content out). Pool path
// just shuffles where the Relation's *outer storage* lives — Relation's
// internal vectors still use std::allocator (no allocator propagation).
//
// Trade-offs:
//   + Removes repeated outer-vector reallocations during sieve growth
//   + Reduces fragmentation pressure when 1M+ relations accumulate
//   - Memory not released until pool::reset() or collector destruction
//   - One extra indirection per std::pmr::vector access vs std::vector
//
// Compatible with OOC mode: OOC writes Relations to disk (no in-memory
// growth of relations_), so the pool only affects vector-mode collectors.

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <memory_resource>
#include <mutex>

namespace gnfs::util {

/// RAII wrapper around std::pmr::monotonic_buffer_resource.
///
/// Owns a single monotonic_buffer_resource instance pre-sized with a chunk
/// of `initial_chunk_bytes` (default 4 MiB). Upstream defaults to
/// std::pmr::new_delete_resource, so chunk exhaustion falls back to standard
/// allocation rather than crashing.
///
/// Use via upstream() to construct std::pmr::vector<T>(resource->upstream()).
/// Call reset() to release all allocations and start fresh (the chunk is
/// re-allocated lazily on next request).
///
/// Movable but not copyable: monotonic_buffer_resource holds owned chunks
/// that cannot be duplicated safely.
class RelationPoolResource {
public:
    static constexpr std::size_t DEFAULT_INITIAL_CHUNK_BYTES = 4 * 1024 * 1024;

    explicit RelationPoolResource(
        std::size_t initial_chunk_bytes = DEFAULT_INITIAL_CHUNK_BYTES)
        : initial_chunk_bytes_(initial_chunk_bytes),
          resource_(std::make_unique<std::pmr::monotonic_buffer_resource>(
              initial_chunk_bytes,
              std::pmr::new_delete_resource())) {}

    ~RelationPoolResource() = default;

    // Non-copyable
    RelationPoolResource(const RelationPoolResource&) = delete;
    RelationPoolResource& operator=(const RelationPoolResource&) = delete;

    // Movable: transfer ownership of the underlying resource.
    RelationPoolResource(RelationPoolResource&& other) noexcept
        : initial_chunk_bytes_(other.initial_chunk_bytes_),
          resource_(std::move(other.resource_)) {}

    RelationPoolResource& operator=(RelationPoolResource&& other) noexcept {
        if (this != &other) {
            initial_chunk_bytes_ = other.initial_chunk_bytes_;
            resource_ = std::move(other.resource_);
        }
        return *this;
    }

    /// Returns the underlying std::pmr::memory_resource* for use as the
    /// upstream of a std::pmr::vector / pmr container.
    [[nodiscard]] std::pmr::memory_resource* upstream() noexcept {
        return resource_.get();
    }

    /// Release all chunks held by the monotonic buffer and re-construct.
    /// After reset(), any std::pmr::vector backed by upstream() must be
    /// re-constructed or cleared — addresses returned from upstream() prior
    /// to reset() become dangling.
    void reset() {
        resource_ = std::make_unique<std::pmr::monotonic_buffer_resource>(
            initial_chunk_bytes_,
            std::pmr::new_delete_resource());
    }

    /// Configured initial chunk size in bytes.
    [[nodiscard]] std::size_t initial_chunk_bytes() const noexcept {
        return initial_chunk_bytes_;
    }

private:
    std::size_t initial_chunk_bytes_;
    std::unique_ptr<std::pmr::monotonic_buffer_resource> resource_;
};

namespace detail {

inline std::once_flag& relation_pool_env_once_flag() {
    static std::once_flag flag;
    return flag;
}

inline std::atomic<std::size_t>& relation_pool_env_value() {
    static std::atomic<std::size_t> value{0};
    return value;
}

inline std::size_t parse_relation_pool_env() {
    const char* env = std::getenv("GNFS_RELATION_POOL_SIZE");
    if (env == nullptr || env[0] == '\0') return 0;
    // strtoull tolerates leading whitespace; reject negative explicitly.
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(env, &end, 10);
    if (end == env) return 0;        // no digits consumed
    if (parsed == 0ULL) return 0;    // explicit 0 or overflow-to-zero
    return static_cast<std::size_t>(parsed);
}

}  // namespace detail

/// Read GNFS_RELATION_POOL_SIZE and return the initial chunk size in bytes.
/// Cached via std::call_once + std::atomic for thread safety.
///   - Unset, empty, "0", or non-numeric: returns 0 (pool disabled).
///   - Positive integer N: returns N (initial chunk bytes for the pool).
///
/// Once parsed, the cached value persists for the lifetime of the process.
/// To re-read the environment (e.g., in tests), restart the process or
/// use the test-only reset helper.
[[nodiscard]] inline std::size_t relation_pool_size_bytes() {
    std::call_once(detail::relation_pool_env_once_flag(), []() {
        detail::relation_pool_env_value().store(
            detail::parse_relation_pool_env(),
            std::memory_order_release);
    });
    return detail::relation_pool_env_value().load(std::memory_order_acquire);
}

/// Convenience: returns true iff relation_pool_size_bytes() > 0.
[[nodiscard]] inline bool relation_pool_enabled() {
    return relation_pool_size_bytes() > 0;
}

/// Test-only: re-parse GNFS_RELATION_POOL_SIZE.
/// NOT thread-safe — call only from single-threaded test setup.
inline void relation_pool_reset_env_cache_for_testing() {
    detail::relation_pool_env_value().store(
        detail::parse_relation_pool_env(),
        std::memory_order_release);
}

}  // namespace gnfs::util

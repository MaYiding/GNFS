#pragma once

#include "../core/relation.hpp"
#include "../relation/ooc_relation_store.hpp"

#include <concepts>
#include <cstddef>
#include <vector>

namespace gnfs::linalg {

/// RelationSource concept: abstracts the source of relations for the
/// streaming MatrixBuilder. Implementations must:
///   - report total count
///   - return a Relation by index (full copy / value semantics, NOT reference)
///
/// Both `OOCRelationReader` (mmap-backed disk file) and `VectorRelationSource`
/// (in-memory vector wrapper) satisfy this concept. The streaming matrix
/// builder uses this concept so the same code path serves both vector and
/// OOC inputs with zero virtual dispatch.
///
/// SAFETY: `read(i)` must be thread-safe for concurrent calls with distinct
/// `i` values — the streaming builder runs `parallel_for` over rows where
/// each thread fetches its own relation. OOCRelationReader satisfies this
/// (read-only mmap + per-call local deserialize buffer) and so does
/// VectorRelationSource (const vector access).
template <typename Source>
concept RelationSource = requires(const Source& s, std::size_t i) {
    { s.count() } -> std::convertible_to<std::size_t>;
    { s.read(i) } -> std::same_as<core::Relation>;
};

/// Adapter that exposes an existing `std::vector<Relation>` as a RelationSource.
/// Holds only a const reference — caller must keep the vector alive for the
/// builder's lifetime.
///
/// Used for two purposes:
///   1. The "vector path" of the pipeline can route through the streaming
///      builder without copying, giving uniform API
///   2. Equivalence tests: build SGE result via vector path AND via
///      streaming-on-vector — must produce bit-for-bit identical reduced
///      matrices (after deterministic LP ordering)
class VectorRelationSource {
public:
    explicit VectorRelationSource(const std::vector<core::Relation>& v) noexcept
        : v_(&v) {}

    [[nodiscard]] std::size_t count() const noexcept { return v_->size(); }
    [[nodiscard]] core::Relation read(std::size_t i) const { return (*v_)[i]; }

private:
    const std::vector<core::Relation>* v_;
};

/// Adapter exposing OOCRelationReader as a RelationSource.
/// Reader must outlive the adapter (which itself outlives the streaming builder).
class OOCRelationSource {
public:
    explicit OOCRelationSource(const relation::OOCRelationReader& reader) noexcept
        : reader_(&reader) {}

    [[nodiscard]] std::size_t count() const noexcept { return reader_->count(); }
    [[nodiscard]] core::Relation read(std::size_t i) const { return reader_->read(i); }

private:
    const relation::OOCRelationReader* reader_;
};

// Concept conformance checks (compile-time)
static_assert(RelationSource<VectorRelationSource>,
              "VectorRelationSource must satisfy RelationSource");
static_assert(RelationSource<OOCRelationSource>,
              "OOCRelationSource must satisfy RelationSource");

} // namespace gnfs::linalg

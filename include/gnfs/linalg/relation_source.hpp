#pragma once

#include "../core/relation.hpp"
#include "../relation/ooc_relation_store.hpp"
#include "../relation/relation_corpus.hpp"

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

/// Refined RelationSource concept for sources whose local row indices map to
/// stable ordinals in a larger corpus. MatrixBuilder uses this optional
/// capability to preserve relation provenance; ordinary RelationSource
/// implementations retain identity row numbering. `source_ordinal(i)` must
/// return the ordinal of `read(i)` in the owning corpus and, like `read(i)`,
/// must be safe for concurrent read-only calls on distinct indices.
template <typename Source>
concept OrdinalRelationSource = RelationSource<Source> && requires(const Source& s, std::size_t i) {
    { s.source_ordinal(i) } -> std::convertible_to<std::size_t>;
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

/// Adapter exposing an immutable RelationSelection as a RelationSource while
/// preserving the selected rows' ordinals in the owning RelationCorpus.
/// Both the corpus and selection must outlive this non-owning adapter.
class RelationSelectionSource {
public:
    RelationSelectionSource(const relation::RelationCorpus& corpus,
                            const relation::RelationSelection& selection)
        : corpus_(&corpus), selection_(&selection) {
        selection_->validate_for(*corpus_);
    }

    [[nodiscard]] std::size_t count() const noexcept {
        return selection_->count();
    }

    [[nodiscard]] core::Relation read(std::size_t i) const {
        return corpus_->read(selection_->source_ordinal(i));
    }

    [[nodiscard]] std::size_t source_ordinal(std::size_t i) const {
        return selection_->source_ordinal(i);
    }

private:
    const relation::RelationCorpus* corpus_;
    const relation::RelationSelection* selection_;
};

// Concept conformance checks (compile-time)
static_assert(RelationSource<VectorRelationSource>,
              "VectorRelationSource must satisfy RelationSource");
static_assert(RelationSource<OOCRelationSource>,
              "OOCRelationSource must satisfy RelationSource");
static_assert(RelationSource<RelationSelectionSource>,
              "RelationSelectionSource must satisfy RelationSource");
static_assert(OrdinalRelationSource<RelationSelectionSource>,
              "RelationSelectionSource must preserve corpus ordinals");

} // namespace gnfs::linalg

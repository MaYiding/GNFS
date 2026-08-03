#pragma once

#include "gnfs/core/relation.hpp"

#include <concepts>
#include <cstddef>

namespace gnfs::relation {

/// Read-only indexed relation source shared by reduction and linear algebra.
///
/// `read(i)` returns an independent value rather than a borrowed reference so
/// the same contract covers owned vectors and immutable mmap-backed corpora.
/// Implementations used by parallel consumers must permit concurrent const
/// reads while the source owner remains alive and unmoved.
template <typename Source>
concept RelationSource = requires(const Source& source, std::size_t ordinal) {
    { source.count() } -> std::convertible_to<std::size_t>;
    { source.read(ordinal) } -> std::same_as<core::Relation>;
};

} // namespace gnfs::relation

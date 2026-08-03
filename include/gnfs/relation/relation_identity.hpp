#pragma once

#include "../core/relation.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gnfs::relation {

/// Exact provenance identity for a materialized merged relation.
///
/// Legacy V0/V3 relations do not yet carry immutable numeric source IDs, so an
/// exact raw ABPair identifies each source row.  The sorted vector is the GF(2)
/// symmetric difference of the primary pair and every flattened extra pair:
/// repeated sources cancel, input order is irrelevant, and distinct source
/// combinations may safely share the same materialized primary pair.
struct RelationSourceCombination {
    std::vector<core::ABPair> sources;

    [[nodiscard]] bool operator==(const RelationSourceCombination& other) const noexcept {
        return sources == other.sources;
    }
};

struct RelationSourceCombinationHash {
    [[nodiscard]] size_t operator()(const RelationSourceCombination& combination) const noexcept {
        uint64_t h = 14695981039346656037ULL;
        auto mix = [&](uint64_t value) {
            h ^= value;
            h *= 1099511628211ULL;
        };

        mix(static_cast<uint64_t>(combination.sources.size()));
        for (const auto& source : combination.sources) {
            mix(static_cast<uint64_t>(source.a));
            mix(source.b);
        }

        if constexpr (sizeof(size_t) >= sizeof(uint64_t)) {
            return static_cast<size_t>(h);
        } else {
            return static_cast<size_t>(h ^ (h >> 32));
        }
    }
};

/// Build the exact, order-independent source combination of one relation.
[[nodiscard]] inline RelationSourceCombination
relation_source_combination(const core::Relation& relation) {
    RelationSourceCombination combination;
    combination.sources.reserve(relation.extra_ab_pairs.size() + 1);
    combination.sources.push_back(relation.ab());
    for (const auto& [a, b] : relation.extra_ab_pairs) {
        combination.sources.emplace_back(a, b);
    }

    std::sort(combination.sources.begin(), combination.sources.end());

    size_t output = 0;
    for (size_t begin = 0; begin < combination.sources.size();) {
        size_t end = begin + 1;
        while (end < combination.sources.size() &&
               combination.sources[end] == combination.sources[begin]) {
            ++end;
        }
        if (((end - begin) & size_t{1}) != 0) {
            combination.sources[output++] = combination.sources[begin];
        }
        begin = end;
    }
    combination.sources.resize(output);
    return combination;
}

} // namespace gnfs::relation

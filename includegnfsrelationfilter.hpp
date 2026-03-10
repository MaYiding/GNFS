#pragma once

#include "gnfs/core/relation.hpp"
#include <vector>

namespace gnfs::relation {

using gnfs::core::Relation;

/// Filter and deduplicate relations
class RelationFilter {
public:
    RelationFilter() = default;
    
    /// Filter relations (remove duplicates, invalid ones)
    static std::vector<Relation> filter(const std::vector<Relation>& relations);
    
    /// Remove singleton relations (appearing only once)
    static std::vector<Relation> remove_singletons(const std::vector<Relation>& relations);
};

} // namespace gnfs::relation

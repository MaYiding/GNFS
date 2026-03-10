#include "gnfs/relation/filter.hpp"
#include <set>

namespace gnfs::relation {

std::vector<Relation> RelationFilter::filter(const std::vector<Relation>& relations) {
    std::vector<Relation> filtered;
    std::set<std::pair<int64_t, int64_t>> seen;
    
    for (const auto& rel : relations) {
        if (!rel.is_valid()) continue;
        
        auto key = std::make_pair(rel.a, rel.b);
        if (seen.count(key)) continue;
        
        seen.insert(key);
        filtered.push_back(rel);
    }
    
    return filtered;
}

std::vector<Relation> RelationFilter::remove_singletons(const std::vector<Relation>& relations) {
    // Placeholder: in real implementation, this would identify and remove
    // relations with large primes that appear only once
    return relations;
}

} // namespace gnfs::relation

#pragma once

#include "gnfs/core/relation.hpp"
#include <vector>
#include <mutex>

namespace gnfs::relation {

using gnfs::core::Relation;

/// Thread-safe relation collector
class RelationCollector {
public:
    RelationCollector() = default;
    
    /// Add a relation
    void add(const Relation& rel);
    void add(Relation&& rel);
    
    /// Get all relations
    std::vector<Relation> get_relations() const;
    
    /// Get count
    size_t size() const;
    
    /// Clear all relations
    void clear();
    
private:
    mutable std::mutex mutex_;
    std::vector<Relation> relations_;
};

} // namespace gnfs::relation

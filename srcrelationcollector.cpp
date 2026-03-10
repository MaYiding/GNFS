#include "gnfs/relation/collector.hpp"

namespace gnfs::relation {

void RelationCollector::add(const Relation& rel) {
    std::lock_guard<std::mutex> lock(mutex_);
    relations_.push_back(rel);
}

void RelationCollector::add(Relation&& rel) {
    std::lock_guard<std::mutex> lock(mutex_);
    relations_.push_back(std::move(rel));
}

std::vector<Relation> RelationCollector::get_relations() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return relations_;
}

size_t RelationCollector::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return relations_.size();
}

void RelationCollector::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    relations_.clear();
}

} // namespace gnfs::relation

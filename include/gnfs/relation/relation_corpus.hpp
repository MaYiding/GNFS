#pragma once

#include "gnfs/core/relation.hpp"
#include "gnfs/relation/ooc_relation_store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace gnfs::relation {

enum class RelationStorageKind {
    InMemory,
    FinalizedOOC,
};

/// Whether an owned finalized OOC corpus should preserve or remove its two
/// artifacts when the final corpus owner is destroyed. RemoveArtifacts is an
/// explicit transfer of exclusive artifact ownership: callers must not retain
/// independent readers, replace either artifact, or reuse the same base path
/// while the corpus is alive. Ownership promotion accepts only finalized V3,
/// whose index and data headers carry the same persistent store identity.
enum class OOCCleanupPolicy {
    Preserve,
    RemoveArtifacts,
};

namespace relation_corpus_detail {

/// Opaque identity shared only by one corpus instance and selections created
/// from that instance. The token has no value semantics: two independently
/// opened corpora remain distinct even when generation, count, descriptor, and
/// relation contents are identical.
struct RelationCorpusIdentity final {};
using RelationCorpusIdentityToken = std::shared_ptr<const RelationCorpusIdentity>;

inline void validate_logical_generation(uint64_t generation) {
    if (generation == 0) {
        throw std::invalid_argument("relation corpus logical generation must be nonzero");
    }
}

inline void validate_ooc_base_path(const std::string& base_path) {
    if (base_path.empty() || base_path.find('\0') != std::string::npos) {
        throw std::invalid_argument(
            "RelationCorpus: OOC base path must be nonempty and contain no NUL");
    }
}

/// Repository-owned deterministic generator for selection policies.
///
/// Keeping both the generator and bounded-draw algorithm here avoids the
/// implementation-defined output of standard-library distributions and
/// shuffling algorithms. The constants and transitions are the SplitMix64
/// reference sequence; bounded() uses rejection sampling to remove modulo
/// bias.
class DeterministicSplitMix64 final {
public:
    explicit DeterministicSplitMix64(uint64_t seed) noexcept : state_(seed) {}

    [[nodiscard]] uint64_t next() noexcept {
        uint64_t value = (state_ += 0x9E3779B97F4A7C15ULL);
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31U);
    }

    [[nodiscard]] uint64_t bounded(uint64_t exclusive_upper_bound) {
        if (exclusive_upper_bound == 0) {
            throw std::invalid_argument("DeterministicSplitMix64: upper bound must be nonzero");
        }

        const uint64_t rejection_threshold =
            (uint64_t{0} - exclusive_upper_bound) % exclusive_upper_bound;
        for (;;) {
            const uint64_t value = next();
            if (value >= rejection_threshold) {
                return value % exclusive_upper_bound;
            }
        }
    }

private:
    uint64_t state_;
};

inline void validate_finalized_ooc_ownership_descriptor(const OOCSnapshotDescriptor& descriptor) {
    if (descriptor.format_version != OOCRelationWriter::FORMAT_VERSION_V3) {
        throw std::invalid_argument(
            "RelationCorpus: ownership promotion requires finalized OOC V3");
    }
    if (descriptor.store_id == 0) {
        throw std::invalid_argument("RelationCorpus: OOC descriptor store identity is zero");
    }
    if (descriptor.generation == 0) {
        throw std::invalid_argument("RelationCorpus: OOC descriptor generation is zero");
    }
    if (descriptor.data_end < OOCRelationWriter::DATA_HEADER_BYTES ||
        ((descriptor.count == 0) !=
         (descriptor.data_end == OOCRelationWriter::DATA_HEADER_BYTES))) {
        throw std::invalid_argument(
            "RelationCorpus: OOC V3 descriptor has invalid physical data extent");
    }
    (void)OOCRelationWriter::index_size_for_count(descriptor.count);
}

/// Best-effort deletion guard for an owned finalized V3 store.
///
/// This reopens both artifacts through the descriptor-bound mmap reader after
/// the corpus reader has closed. The same handles verify both V3 headers,
/// paired store identity, exact extents, sentinel, and offsets immediately
/// before cleanup. Any replacement or damage therefore preserves both paths.
inline void validate_finalized_ooc_cleanup_target(const std::string& base_path,
                                                  const OOCSnapshotDescriptor& descriptor) {
    validate_ooc_base_path(base_path);
    validate_finalized_ooc_ownership_descriptor(descriptor);

    // The local reader is destroyed before this function returns, which keeps
    // Windows mappings and file handles closed before remove() is attempted.
    OOCRelationReader validated_pair(base_path, descriptor);
    (void)validated_pair;
}

/// Optional artifact cleanup. The enclosing storage declares this member
/// before its reader, so C++ reverse member destruction closes every mmap/file
/// handle before this destructor runs. Before removing anything, revalidate the
/// durable index identity and exact extents; a replaced or damaged path is
/// preserved rather than deleting it. V3 paired identity also rejects a
/// same-sized data file from another store.
class FinalizedOOCArtifactCleanup final {
public:
    FinalizedOOCArtifactCleanup(std::string base_path, OOCSnapshotDescriptor descriptor)
        : base_path_(std::move(base_path)), descriptor_(descriptor) {}

    FinalizedOOCArtifactCleanup(const FinalizedOOCArtifactCleanup&) = delete;
    FinalizedOOCArtifactCleanup& operator=(const FinalizedOOCArtifactCleanup&) = delete;

    FinalizedOOCArtifactCleanup(FinalizedOOCArtifactCleanup&& other) noexcept
        : base_path_(std::move(other.base_path_)), descriptor_(other.descriptor_),
          armed_(std::exchange(other.armed_, false)) {}

    FinalizedOOCArtifactCleanup& operator=(FinalizedOOCArtifactCleanup&&) = delete;

    ~FinalizedOOCArtifactCleanup() {
        cleanup_noexcept();
    }

    void arm() noexcept {
        armed_ = true;
    }

private:
    void cleanup_noexcept() noexcept {
        if (!armed_) {
            return;
        }

        try {
            validate_finalized_ooc_cleanup_target(base_path_, descriptor_);

            // Removing the index first makes the corpus unavailable to new readers
            // before its payload is removed. Existing reader handles are already
            // closed by the enclosing storage's member destruction order.
            std::error_code ec;
            const bool index_removed = std::filesystem::remove(base_path_ + ".relidx", ec);
            if (ec || !index_removed) {
                std::fprintf(stderr, "[relation_corpus] could not remove finalized OOC index; "
                                     "preserving data artifact\n");
                return;
            }

            ec.clear();
            const bool data_removed = std::filesystem::remove(base_path_ + ".reldata", ec);
            if (ec || !data_removed) {
                std::fprintf(stderr,
                             "[relation_corpus] finalized OOC index was removed but data cleanup "
                             "failed\n");
            }
        } catch (const std::exception& error) {
            std::fprintf(stderr,
                         "[relation_corpus] preserving OOC artifacts after cleanup identity "
                         "validation failed: %s\n",
                         error.what());
        } catch (...) {
            std::fprintf(stderr, "[relation_corpus] preserving OOC artifacts after unknown cleanup "
                                 "identity failure\n");
        }
    }

    std::string base_path_;
    OOCSnapshotDescriptor descriptor_;
    bool armed_ = false;
};

} // namespace relation_corpus_detail

class RelationSelection;

/// Move-only owner of an immutable relation corpus.
///
/// In-memory storage owns its vector. Finalized OOC storage owns an mmap reader
/// plus, when explicitly requested, cleanup responsibility for the
/// descriptor-bound V3 artifact pair. The logical generation belongs to the
/// reduction snapshot and is intentionally distinct from the OOC descriptor's
/// checkpoint generation.
class RelationCorpus final {
public:
    [[nodiscard]] static RelationCorpus from_in_memory(uint64_t logical_generation,
                                                       std::vector<core::Relation> relations) {
        relation_corpus_detail::validate_logical_generation(logical_generation);
        return RelationCorpus(
            std::make_unique<State>(logical_generation, InMemoryStorage{std::move(relations)}));
    }

    [[nodiscard]] static RelationCorpus
    from_finalized_ooc(uint64_t logical_generation, std::string base_path,
                       const OOCSnapshotDescriptor& descriptor,
                       OOCCleanupPolicy cleanup_policy = OOCCleanupPolicy::Preserve) {
        relation_corpus_detail::validate_logical_generation(logical_generation);
        relation_corpus_detail::validate_ooc_base_path(base_path);
        relation_corpus_detail::validate_finalized_ooc_ownership_descriptor(descriptor);

        // Expected-descriptor construction binds the corpus to one mapped V3
        // index/data pair and validates both headers, identity, count, exact
        // extents, sentinel, and every offset. The reader is created before
        // cleanup is armed, so validation/allocation failures leave artifacts
        // untouched.
        OOCRelationReader reader(base_path, descriptor);

        auto state = std::make_unique<State>(
            logical_generation,
            FinalizedOOCStorage{std::move(base_path), descriptor, std::move(reader)});
        if (cleanup_policy == OOCCleanupPolicy::RemoveArtifacts) {
            std::get<FinalizedOOCStorage>(state->storage).arm_cleanup();
        }
        return RelationCorpus(std::move(state));
    }

    RelationCorpus(const RelationCorpus&) = delete;
    RelationCorpus& operator=(const RelationCorpus&) = delete;
    RelationCorpus(RelationCorpus&&) noexcept = default;
    RelationCorpus& operator=(RelationCorpus&&) noexcept = default;
    ~RelationCorpus() = default;

    [[nodiscard]] bool valid() const noexcept {
        return state_ != nullptr;
    }

    [[nodiscard]] uint64_t logical_generation() const {
        return require_state().logical_generation;
    }

    [[nodiscard]] RelationStorageKind storage_kind() const {
        const auto& storage = require_state().storage;
        return std::holds_alternative<InMemoryStorage>(storage) ? RelationStorageKind::InMemory
                                                                : RelationStorageKind::FinalizedOOC;
    }

    [[nodiscard]] size_t count() const {
        return std::visit([](const auto& storage) { return storage.count(); },
                          require_state().storage);
    }

    [[nodiscard]] bool empty() const {
        return count() == 0;
    }

    [[nodiscard]] core::Relation read(size_t ordinal) const {
        return std::visit([ordinal](const auto& storage) { return storage.read(ordinal); },
                          require_state().storage);
    }

private:
    using IdentityToken = relation_corpus_detail::RelationCorpusIdentityToken;

    struct InMemoryStorage final {
        std::vector<core::Relation> relations;

        [[nodiscard]] size_t count() const noexcept {
            return relations.size();
        }

        [[nodiscard]] core::Relation read(size_t ordinal) const {
            return relations.at(ordinal);
        }
    };

    struct FinalizedOOCStorage final {
        FinalizedOOCStorage(std::string base_path, OOCSnapshotDescriptor descriptor,
                            OOCRelationReader reader)
            : cleanup(std::move(base_path), descriptor), reader(std::move(reader)) {}

        FinalizedOOCStorage(const FinalizedOOCStorage&) = delete;
        FinalizedOOCStorage& operator=(const FinalizedOOCStorage&) = delete;
        FinalizedOOCStorage(FinalizedOOCStorage&&) noexcept = default;
        FinalizedOOCStorage& operator=(FinalizedOOCStorage&&) = delete;

        [[nodiscard]] size_t count() const noexcept {
            return reader.count();
        }

        [[nodiscard]] core::Relation read(size_t ordinal) const {
            return reader.read(ordinal);
        }

        void arm_cleanup() noexcept {
            cleanup.arm();
        }

        // Reverse destruction is intentional and part of the Windows contract:
        // reader (mmap + handles) first, cleanup second.
        relation_corpus_detail::FinalizedOOCArtifactCleanup cleanup;
        OOCRelationReader reader;
    };

    using Storage = std::variant<InMemoryStorage, FinalizedOOCStorage>;

    struct State final {
        State(uint64_t generation, InMemoryStorage storage_value)
            : identity(std::make_shared<const relation_corpus_detail::RelationCorpusIdentity>()),
              logical_generation(generation), storage(std::move(storage_value)) {}

        State(uint64_t generation, FinalizedOOCStorage storage_value)
            : identity(std::make_shared<const relation_corpus_detail::RelationCorpusIdentity>()),
              logical_generation(generation), storage(std::move(storage_value)) {}

        IdentityToken identity;
        uint64_t logical_generation;
        Storage storage;
    };

    explicit RelationCorpus(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    [[nodiscard]] const State& require_state() const {
        if (!state_) {
            throw std::logic_error("RelationCorpus: use of moved-from corpus");
        }
        return *state_;
    }

    [[nodiscard]] const IdentityToken& identity_token() const {
        return require_state().identity;
    }

    std::unique_ptr<State> state_;

    friend class RelationSelection;
};

/// Immutable, deterministic view of selected source ordinals.
///
/// Duplicate ordinals are removed while preserving their first occurrence, so
/// a caller-supplied deterministic ranking remains intact. The selection stores
/// an opaque shared instance token, logical generation, and source count rather
/// than borrowing a corpus pointer. Moving the corpus handle preserves the
/// token, while independently opening an identical corpus creates a new token.
class RelationSelection final {
public:
    [[nodiscard]] static RelationSelection from_ordinals(const RelationCorpus& corpus,
                                                         std::vector<size_t> ordinals) {
        const size_t source_count = corpus.count();
        std::unordered_set<size_t> seen;
        seen.reserve(ordinals.size());

        std::vector<size_t> unique;
        unique.reserve(ordinals.size());
        for (size_t ordinal : ordinals) {
            if (ordinal >= source_count) {
                throw std::out_of_range("RelationSelection: source ordinal out of range");
            }
            if (seen.insert(ordinal).second) {
                unique.push_back(ordinal);
            }
        }

        return RelationSelection(corpus.identity_token(), corpus.logical_generation(), source_count,
                                 std::move(unique));
    }

    /// Construct a canonical GF(2) selection from source ordinals.
    ///
    /// Unlike from_ordinals(), duplicate occurrences are coefficients in
    /// GF(2): an ordinal occurring an even number of times is removed and an
    /// ordinal occurring an odd number of times survives once. The result is
    /// sorted by source ordinal so equivalent XOR inputs have identical
    /// representations on every supported platform.
    [[nodiscard]] static RelationSelection from_xor_ordinals(const RelationCorpus& corpus,
                                                             std::vector<size_t> ordinals) {
        const size_t source_count = corpus.count();
        for (size_t ordinal : ordinals) {
            if (ordinal >= source_count) {
                throw std::out_of_range("RelationSelection: source ordinal out of range");
            }
        }

        std::sort(ordinals.begin(), ordinals.end());

        std::vector<size_t> canonical;
        canonical.reserve(ordinals.size());
        size_t run_begin = 0;
        while (run_begin < ordinals.size()) {
            size_t run_end = run_begin + 1;
            while (run_end < ordinals.size() && ordinals[run_end] == ordinals[run_begin]) {
                ++run_end;
            }
            if ((run_end - run_begin) % 2U != 0U) {
                canonical.push_back(ordinals[run_begin]);
            }
            run_begin = run_end;
        }

        return RelationSelection(corpus.identity_token(), corpus.logical_generation(), source_count,
                                 std::move(canonical));
    }

    /// Select a uniformly distributed subset without materializing relations.
    ///
    /// Reservoir sampling visits corpus ordinals in ascending order, stores at
    /// most keep_count ordinals, and uses the repository-owned SplitMix64
    /// bounded draw above. Sorting the final reservoir gives a canonical row
    /// order independent of STL implementation details.
    [[nodiscard]] static RelationSelection deterministic_sample(const RelationCorpus& corpus,
                                                                size_t keep_count, uint64_t seed) {
        const size_t source_count = corpus.count();
        if (keep_count > source_count) {
            throw std::invalid_argument("RelationSelection: sample size exceeds source count");
        }

        std::vector<size_t> reservoir;
        reservoir.reserve(keep_count);
        for (size_t ordinal = 0; ordinal < keep_count; ++ordinal) {
            reservoir.push_back(ordinal);
        }

        if (keep_count != 0) {
            relation_corpus_detail::DeterministicSplitMix64 generator(seed);
            for (size_t ordinal = keep_count; ordinal < source_count; ++ordinal) {
                const uint64_t upper_bound = static_cast<uint64_t>(ordinal) + 1U;
                const size_t replacement = static_cast<size_t>(generator.bounded(upper_bound));
                if (replacement < keep_count) {
                    reservoir[replacement] = ordinal;
                }
            }
        }

        std::sort(reservoir.begin(), reservoir.end());
        return RelationSelection(corpus.identity_token(), corpus.logical_generation(), source_count,
                                 std::move(reservoir));
    }

    [[nodiscard]] uint64_t logical_generation() const noexcept {
        return logical_generation_;
    }

    [[nodiscard]] size_t source_count() const noexcept {
        return source_count_;
    }

    [[nodiscard]] size_t count() const noexcept {
        return ordinals_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return ordinals_.empty();
    }

    [[nodiscard]] size_t source_ordinal(size_t selected_ordinal) const {
        return ordinals_.at(selected_ordinal);
    }

    [[nodiscard]] const std::vector<size_t>& ordinals() const noexcept {
        return ordinals_;
    }

    [[nodiscard]] core::Relation read(const RelationCorpus& corpus, size_t selected_ordinal) const {
        validate_for(corpus);
        return corpus.read(source_ordinal(selected_ordinal));
    }

    void validate_for(const RelationCorpus& corpus) const {
        if (corpus.identity_token() != identity_token_ ||
            corpus.logical_generation() != logical_generation_ || corpus.count() != source_count_) {
            throw std::invalid_argument(
                "RelationSelection: corpus instance, logical generation, or source count mismatch");
        }
    }

private:
    using IdentityToken = relation_corpus_detail::RelationCorpusIdentityToken;

    RelationSelection(IdentityToken identity_token, uint64_t logical_generation,
                      size_t source_count, std::vector<size_t> ordinals)
        : identity_token_(std::move(identity_token)), logical_generation_(logical_generation),
          source_count_(source_count), ordinals_(std::move(ordinals)) {}

    IdentityToken identity_token_;
    uint64_t logical_generation_;
    size_t source_count_;
    std::vector<size_t> ordinals_;
};

/// Materialize only the selected relations, in deterministic selection order.
[[nodiscard]] inline std::vector<core::Relation>
materialize_selected(const RelationCorpus& corpus, const RelationSelection& selection) {
    selection.validate_for(corpus);

    std::vector<core::Relation> result;
    result.reserve(selection.count());
    for (size_t ordinal : selection.ordinals()) {
        result.push_back(corpus.read(ordinal));
    }
    return result;
}

} // namespace gnfs::relation

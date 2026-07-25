#pragma once

#include "gnfs/core/relation.hpp"
#include "gnfs/relation/ooc_relation_store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace gnfs::api {
class Pipeline;
}

namespace gnfs::relation {

enum class RelationStorageKind {
    InMemory,
    FinalizedOOC,
};

/// Whether an owned finalized OOC corpus should preserve or remove its two
/// artifacts when the final corpus owner is destroyed. RemoveArtifacts is
/// accepted only by the fresh-writer ownership-transfer factory. A structural
/// descriptor can reopen bytes for reading but never grants deletion authority.
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

/// Freeze an OOC path against later process working-directory changes and
/// resolve existing parent/symlink components without following the base leaf.
/// The leaf is a namespace prefix, not an artifact: stores append two suffixes,
/// so a same-named symlink must never redirect those sibling artifact paths.
[[nodiscard]] inline std::string freeze_ooc_path(const std::string& path) {
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    const auto parent = std::filesystem::weakly_canonical(absolute.parent_path());
    return (parent / absolute.filename()).lexically_normal().string();
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

inline void validate_cleanup_policy(OOCCleanupPolicy cleanup_policy) {
    if (cleanup_policy != OOCCleanupPolicy::Preserve &&
        cleanup_policy != OOCCleanupPolicy::RemoveArtifacts) {
        throw std::invalid_argument("RelationCorpus: invalid OOC cleanup policy");
    }
}

/// Optional transaction-backed artifact cleanup. The enclosing storage declares
/// this member before its reader, so reverse destruction closes every mmap/file
/// handle before this destructor runs. Only a receipt issued by the original
/// fresh O_EXCL writer can arm the guard; descriptors and sequence digests are
/// read-integrity evidence, not deletion authority.
class FinalizedOOCArtifactCleanup final {
public:
    FinalizedOOCArtifactCleanup(std::string base_path, OOCSnapshotDescriptor descriptor)
        : base_path_(std::move(base_path)), descriptor_(descriptor) {}

    FinalizedOOCArtifactCleanup(const FinalizedOOCArtifactCleanup&) = delete;
    FinalizedOOCArtifactCleanup& operator=(const FinalizedOOCArtifactCleanup&) = delete;

    FinalizedOOCArtifactCleanup(FinalizedOOCArtifactCleanup&& other) noexcept
        : base_path_(std::move(other.base_path_)), descriptor_(other.descriptor_),
          cleanup_receipt_(std::move(other.cleanup_receipt_)),
          private_lease_receipt_(std::move(other.private_lease_receipt_)),
          armed_(std::exchange(other.armed_, false)) {}

    FinalizedOOCArtifactCleanup& operator=(FinalizedOOCArtifactCleanup&&) = delete;

    ~FinalizedOOCArtifactCleanup() {
        cleanup_noexcept();
    }

    void adopt_cleanup_ownership(OOCCleanupOwnershipReceipt receipt) noexcept {
        static_assert(std::is_nothrow_move_constructible_v<OOCCleanupOwnershipReceipt>);
        cleanup_receipt_.emplace(std::move(receipt));
    }

    void adopt_private_lease_ownership(OOCPrivateLeaseOwnershipReceipt receipt) noexcept {
        static_assert(std::is_nothrow_move_constructible_v<OOCPrivateLeaseOwnershipReceipt>);
        private_lease_receipt_.emplace(std::move(receipt));
    }

    [[nodiscard]] bool arm() noexcept {
        if (!cleanup_receipt_.has_value()) {
            return false;
        }
        armed_ = true;
        return true;
    }

    [[nodiscard]] const std::string& base_path() const noexcept {
        return base_path_;
    }

    [[nodiscard]] const OOCSnapshotDescriptor& descriptor() const noexcept {
        return descriptor_;
    }

    [[nodiscard]] std::string cleanup_directory() const {
        return private_lease_receipt_ ? private_lease_receipt_->private_directory().string()
                                      : std::string{};
    }

private:
    [[nodiscard]] OOCExactCleanupExpectation exact_expectation() const {
        return OOCExactCleanupExpectation{
            .index_magic = OOCRelationWriter::MAGIC_V3_FINAL,
            .persisted_count = descriptor_.count,
            .index_size = OOCRelationWriter::index_size_for_count(descriptor_.count),
            .data_size = descriptor_.data_end,
        };
    }

    [[nodiscard]] OOCCleanupResult remove_pair() {
        if (!cleanup_receipt_) {
            return OOCCleanupResult{
                .status = OOCCleanupStatus::InvalidRequest,
                .stage = OOCCleanupStage::None,
                .native_error = std::make_error_code(std::errc::operation_not_permitted),
            };
        }

        const auto exact = exact_expectation();
        OOCCleanupResult result;
        if (cleanup_receipt_->spent()) {
            result = OOCCleanupTransaction::resume(OOCCleanupRequest{
                .base_path = base_path_,
                .store_id = descriptor_.store_id,
                .exact = exact,
            });
        } else {
            result = OOCCleanupTransaction::begin_or_resume(*cleanup_receipt_, exact);
        }
        if (result.status == OOCCleanupStatus::NoTransaction) {
            result = OOCCleanupTransaction::confirm_pair_namespace_reusable(base_path_);
        }
        return result;
    }

    void remove_private_directory_after_pair_noexcept() noexcept {
        if (!private_lease_receipt_) {
            return;
        }
        const auto result = OOCCleanupTransaction::remove_private_lease(*private_lease_receipt_);
        if (!result.completed()) {
            std::fprintf(stderr,
                         "[relation_corpus] private OOC lease removal failed "
                         "(status=%u, stage=%u); retaining directory and external lock\n",
                         static_cast<unsigned>(result.status), static_cast<unsigned>(result.stage));
        }
    }

    void cleanup_noexcept() noexcept {
        if (!armed_ || !cleanup_receipt_) {
            return;
        }

        try {
            const auto result = remove_pair();
            if (!result.completed()) {
                std::fprintf(stderr,
                             "[relation_corpus] transaction could not remove owned OOC pair "
                             "(status=%u, stage=%u); preserving remaining namespace\n",
                             static_cast<unsigned>(result.status),
                             static_cast<unsigned>(result.stage));
                return;
            }
            remove_private_directory_after_pair_noexcept();
        } catch (const std::exception& error) {
            std::fprintf(stderr,
                         "[relation_corpus] preserving OOC cleanup namespace after failure: %s\n",
                         error.what());
        } catch (...) {
            std::fprintf(stderr, "[relation_corpus] preserving OOC cleanup namespace after unknown "
                                 "failure\n");
        }
    }

    std::string base_path_;
    OOCSnapshotDescriptor descriptor_;
    std::optional<OOCCleanupOwnershipReceipt> cleanup_receipt_;
    std::optional<OOCPrivateLeaseOwnershipReceipt> private_lease_receipt_;
    bool armed_ = false;
};

} // namespace relation_corpus_detail

/// Read-only path and identity scope of one finalized OOC corpus.
///
/// `cleanup_directory` is populated only when the corpus holds the move-only
/// capability for that exact directory. Cleanup occurs only when the corpus is
/// armed, either by RemoveArtifacts adoption or a later explicit arm call.
/// Callers creating adjacent transactional stores can use this scope to reject
/// overlapping lease roots before mutating the filesystem.
struct OOCCorpusArtifactScope final {
    std::string base_path;
    OOCSnapshotDescriptor descriptor{};
    std::string cleanup_directory;
};

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
        relation_corpus_detail::validate_cleanup_policy(cleanup_policy);
        if (cleanup_policy != OOCCleanupPolicy::Preserve) {
            throw std::invalid_argument(
                "RelationCorpus: descriptor-only OOC reopen cannot acquire cleanup ownership");
        }
        relation_corpus_detail::validate_finalized_ooc_ownership_descriptor(descriptor);
        base_path = relation_corpus_detail::freeze_ooc_path(base_path);

        // Expected-descriptor construction binds the corpus to one mapped V3
        // index/data pair and validates both headers, identity, count, exact
        // extents, sentinel, and every offset. This public reopen carries no
        // cleanup receipt, so arm_ooc_cleanup() remains fail-closed.
        OOCRelationReader reader(base_path, descriptor);

        auto state = std::make_unique<State>(
            logical_generation,
            FinalizedOOCStorage{std::move(base_path), descriptor, std::move(reader)});
        return RelationCorpus(std::move(state));
    }

    /// Finalize a fresh writer and transfer its unique cleanup capability into
    /// an immutable corpus. All path, descriptor, reader, and State operations
    /// that may throw complete before the final noexcept receipt move. If any
    /// earlier step fails, the writer retains its receipt and adoption can be
    /// retried. Public recovery writers never carry this capability.
    [[nodiscard]] static RelationCorpus
    from_owned_finalized_ooc(uint64_t logical_generation, OOCRelationWriter& owner,
                             OOCCleanupPolicy cleanup_policy = OOCCleanupPolicy::Preserve,
                             OOCPrivateLeaseOwnershipReceipt* private_lease = nullptr) {
        relation_corpus_detail::validate_logical_generation(logical_generation);
        relation_corpus_detail::validate_cleanup_policy(cleanup_policy);

        const OOCSnapshotDescriptor descriptor = owner.finalize();
        relation_corpus_detail::validate_finalized_ooc_ownership_descriptor(descriptor);
        std::string base_path = relation_corpus_detail::freeze_ooc_path(owner.base_path());
        if (!owner.has_cleanup_ownership_receipt()) {
            throw std::logic_error(
                "RelationCorpus: OOC writer has no transferable cleanup ownership");
        }
        if (private_lease != nullptr) {
            if (private_lease->spent() ||
                private_lease->base_path() != std::filesystem::path(base_path) ||
                private_lease->private_directory() !=
                    std::filesystem::path(base_path).parent_path()) {
                throw std::invalid_argument(
                    "RelationCorpus: private lease receipt does not own this OOC store");
            }
        }

        OOCRelationReader reader(base_path, descriptor);
        auto state = std::make_unique<State>(
            logical_generation,
            FinalizedOOCStorage{std::move(base_path), descriptor, std::move(reader)});

        // No throwing work follows this transfer. take_cleanup_ownership_receipt()
        // performs all rejecting checks before its noexcept move, and emplacing
        // the move-only receipt cannot allocate.
        auto receipt = owner.take_cleanup_ownership_receipt();
        auto& storage = std::get<FinalizedOOCStorage>(state->storage);
        storage.adopt_cleanup_ownership(std::move(receipt));
        if (private_lease != nullptr) {
            storage.adopt_private_lease_ownership(std::move(*private_lease));
        }
        if (cleanup_policy == OOCCleanupPolicy::RemoveArtifacts) {
            (void)storage.arm_cleanup();
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

    [[nodiscard]] std::optional<OOCCorpusArtifactScope> ooc_artifact_scope() const {
        const auto& storage = require_state().storage;
        const auto* ooc = std::get_if<FinalizedOOCStorage>(&storage);
        if (ooc == nullptr) {
            return std::nullopt;
        }
        return ooc->artifact_scope();
    }

    /// Arm cleanup only when this corpus already received the original fresh
    /// writer's move-only receipt. Descriptor-only reopens return false.
    [[nodiscard]] bool arm_ooc_cleanup() noexcept {
        if (state_ == nullptr) {
            return false;
        }
        auto* ooc = std::get_if<FinalizedOOCStorage>(&state_->storage);
        if (ooc == nullptr) {
            return false;
        }
        return ooc->arm_cleanup();
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

    /// Borrow an in-memory row without copying, or return nullptr for OOC.
    /// The pointer remains valid only while this corpus is alive and unmoved.
    /// OOC ordinals are still range-checked before nullptr is returned.
    [[nodiscard]] const core::Relation* try_borrow_in_memory(size_t ordinal) const {
        const State& state = require_state();
        if (const auto* storage = std::get_if<InMemoryStorage>(&state.storage)) {
            return &storage->relations.at(ordinal);
        }
        if (ordinal >= std::get<FinalizedOOCStorage>(state.storage).count()) {
            throw std::out_of_range("RelationCorpus: source ordinal out of range");
        }
        return nullptr;
    }

    /// Borrow the complete in-memory backend without transferring ownership.
    /// Finalized OOC corpora are rejected rather than materialized.
    [[nodiscard]] const std::vector<core::Relation>& borrow_in_memory() const {
        const State& state = require_state();
        const auto* storage = std::get_if<InMemoryStorage>(&state.storage);
        if (storage == nullptr) {
            throw std::logic_error("RelationCorpus: finalized OOC corpus is not in memory");
        }
        return storage->relations;
    }

    /// Visit every relation in stable ordinal order without copying the
    /// in-memory backend. OOC rows are decoded one at a time and the borrowed
    /// reference is valid only for the duration of the callback.
    template <typename Visitor> void for_each(Visitor&& visitor) const {
        const State& state = require_state();
        if (const auto* storage = std::get_if<InMemoryStorage>(&state.storage)) {
            for (size_t ordinal = 0; ordinal < storage->relations.size(); ++ordinal) {
                visitor(storage->relations[ordinal], ordinal);
            }
            return;
        }

        const auto& storage = std::get<FinalizedOOCStorage>(state.storage);
        for (size_t ordinal = 0; ordinal < storage.count(); ++ordinal) {
            const core::Relation relation = storage.read(ordinal);
            visitor(relation, ordinal);
        }
    }

    /// Materialize the complete corpus in stable ordinal order.
    ///
    /// This is an explicit compatibility escape hatch for legacy algorithms
    /// that still require a vector. Structured source/sink paths must prefer
    /// count()/read() so an OOC corpus is never duplicated accidentally.
    [[nodiscard]] std::vector<core::Relation> materialize_all() const {
        std::vector<core::Relation> relations;
        relations.reserve(count());
        for_each([&](const core::Relation& relation, size_t) { relations.push_back(relation); });
        return relations;
    }

    /// Consume an in-memory corpus without copying its relation payload.
    ///
    /// OOC callers must choose materialize_all() explicitly. On success this
    /// corpus becomes moved-from, matching the ordinary move-only ownership
    /// contract and preventing two live owners of the same mutable vector.
    [[nodiscard]] std::vector<core::Relation> take_in_memory() && {
        State& state = require_state();
        auto* storage = std::get_if<InMemoryStorage>(&state.storage);
        if (storage == nullptr) {
            throw std::logic_error(
                "RelationCorpus: cannot take OOC storage as an in-memory vector");
        }

        auto relations = std::move(storage->relations);
        state_.reset();
        return relations;
    }

private:
    friend class gnfs::api::Pipeline;

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

        void adopt_cleanup_ownership(OOCCleanupOwnershipReceipt receipt) noexcept {
            cleanup.adopt_cleanup_ownership(std::move(receipt));
        }

        void adopt_private_lease_ownership(OOCPrivateLeaseOwnershipReceipt receipt) noexcept {
            cleanup.adopt_private_lease_ownership(std::move(receipt));
        }

        [[nodiscard]] bool arm_cleanup() noexcept {
            return cleanup.arm();
        }

        [[nodiscard]] OOCCorpusArtifactScope artifact_scope() const {
            return {cleanup.base_path(), cleanup.descriptor(), cleanup.cleanup_directory()};
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

    void replace_ooc_reader_from(RelationCorpus&& candidate) {
        State& target_state = require_state();
        State& candidate_state = candidate.require_state();
        auto* target = std::get_if<FinalizedOOCStorage>(&target_state.storage);
        auto* source = std::get_if<FinalizedOOCStorage>(&candidate_state.storage);
        if (target == nullptr || source == nullptr ||
            target_state.logical_generation != candidate_state.logical_generation ||
            target->cleanup.base_path() != source->cleanup.base_path() ||
            target->cleanup.descriptor() != source->cleanup.descriptor()) {
            throw std::invalid_argument(
                "RelationCorpus: replacement reader does not match the owned OOC corpus");
        }
        static_assert(std::is_nothrow_move_assignable_v<OOCRelationReader>);
        target->reader = std::move(source->reader);
    }

    [[nodiscard]] State& require_state() {
        if (!state_) {
            throw std::logic_error("RelationCorpus: use of moved-from corpus");
        }
        return *state_;
    }

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

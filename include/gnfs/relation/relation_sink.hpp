#pragma once

#include "gnfs/core/relation.hpp"
#include "gnfs/relation/relation_corpus.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace gnfs::relation {

enum class RelationSinkState {
    Open,
    Finalized,
    Aborted,
};

/// Move-only transactional destination for one reduced relation corpus.
///
/// In-memory sinks retain appended relations until finalize() moves them into
/// a RelationCorpus. Out-of-core sinks create the paired V3 store inside an
/// atomically reserved private directory. One persistent external lock
/// serializes directory creation/removal and every pair transaction without
/// ever being unlinked. The directory remains the exclusive artifact container
/// after handoff and can be removed only through its move-only lease receipt.
/// Until handoff succeeds, the fresh writer separately retains the only receipt
/// capable of starting crash-recoverable pair cleanup.
///
/// Destruction never commits: an open sink is aborted, and only an explicit
/// finalize() may publish an immutable corpus.
class RelationSink final {
public:
    [[nodiscard]] static std::filesystem::path lease_root_for(const std::string& base_path) {
        validate_base_path(base_path);
        return std::filesystem::path(relation_corpus_detail::freeze_ooc_path(base_path) +
                                     lease_suffix());
    }

    [[nodiscard]] static RelationSink in_memory(uint64_t logical_generation,
                                                size_t expected_count = 0) {
        validate_logical_generation(logical_generation);
        RelationSink sink(logical_generation, Backend::InMemory, OOCCleanupPolicy::Preserve);
        sink.relations_.reserve(expected_count);
        return sink;
    }

    [[nodiscard]] static RelationSink
    out_of_core(uint64_t logical_generation, std::string base_path,
                OOCCleanupPolicy cleanup_policy = OOCCleanupPolicy::RemoveArtifacts) {
        validate_logical_generation(logical_generation);
        validate_base_path(base_path);
        validate_cleanup_policy(cleanup_policy);
        base_path = relation_corpus_detail::freeze_ooc_path(base_path);

        const std::string lease_path = base_path + lease_suffix();
        const std::string store_base = (std::filesystem::path(lease_path) / "corpus").string();
        reject_existing_artifacts(base_path);

        RelationSink sink(logical_generation, Backend::OutOfCore, cleanup_policy);
        sink.base_path_ = store_base;

        auto reservation = OOCCleanupTransaction::reserve_private_lease(store_base);
        if (!reservation.completed()) {
            if (reservation.ownership && !reservation.ownership->spent()) {
                (void)OOCCleanupTransaction::remove_private_lease(*reservation.ownership);
            }
            throw std::runtime_error(
                "RelationSink: cannot reserve OOC lease at " + lease_path + " (status=" +
                std::to_string(static_cast<unsigned>(reservation.result.status)) + ")");
        }

        try {
            sink.private_lease_receipt_ = std::make_unique<OOCPrivateLeaseOwnershipReceipt>(
                std::move(*reservation.ownership));
            sink.owns_artifacts_ = true;
            sink.ooc_writer_ = std::make_unique<OOCRelationWriter>(sink.base_path_);
        } catch (...) {
            if (reservation.ownership && !reservation.ownership->spent()) {
                (void)OOCCleanupTransaction::remove_private_lease(*reservation.ownership);
            }
            sink.abort();
            throw;
        }
        return sink;
    }

    RelationSink(const RelationSink&) = delete;
    RelationSink& operator=(const RelationSink&) = delete;

    RelationSink(RelationSink&& other) noexcept
        : logical_generation_(std::exchange(other.logical_generation_, uint64_t{0})),
          count_(std::exchange(other.count_, size_t{0})),
          state_(std::exchange(other.state_, RelationSinkState::Aborted)),
          backend_(std::exchange(other.backend_, Backend::InMemory)),
          cleanup_policy_(other.cleanup_policy_), relations_(std::move(other.relations_)),
          base_path_(std::move(other.base_path_)), ooc_writer_(std::move(other.ooc_writer_)),
          private_lease_receipt_(std::move(other.private_lease_receipt_)),
          owns_artifacts_(std::exchange(other.owns_artifacts_, false)) {}

    RelationSink& operator=(RelationSink&& other) noexcept {
        if (this != &other) {
            RelationSink incoming(std::move(other));
            swap(incoming);
        }
        return *this;
    }

    ~RelationSink() {
        abort();
    }

    /// Append one relation and return its zero-based corpus ordinal.
    /// Any failure aborts the whole sink; partial reduced corpora are never
    /// eligible for a later finalize().
    [[nodiscard]] size_t append(core::Relation relation) {
        require_open("append");
        const size_t ordinal = count_;
        try {
            // Keep memory and OOC backends on the same persisted relation
            // contract. The writer repeats this preflight defensively.
            relation.validate_persistence_limits();
            if (backend_ == Backend::InMemory) {
                relations_.push_back(std::move(relation));
            } else {
                if (!ooc_writer_) {
                    throw std::logic_error("RelationSink::append: missing OOC writer");
                }
                const size_t persisted_ordinal = ooc_writer_->write(relation);
                if (persisted_ordinal != ordinal) {
                    throw std::logic_error("RelationSink::append: OOC ordinal mismatch");
                }
            }
            ++count_;
            return ordinal;
        } catch (...) {
            abort();
            throw;
        }
    }

    [[nodiscard]] size_t count() const noexcept {
        return count_;
    }

    [[nodiscard]] uint64_t logical_generation() const noexcept {
        return logical_generation_;
    }

    [[nodiscard]] RelationSinkState state() const noexcept {
        return state_;
    }

    /// Commit the complete sink and transfer storage ownership to a corpus.
    /// This is intentionally one-shot; repeated finalize() calls are rejected
    /// because the first call has already moved the owning corpus away.
    [[nodiscard]] RelationCorpus finalize() {
        require_open("finalize");
        try {
            if (backend_ == Backend::InMemory) {
                RelationCorpus corpus =
                    RelationCorpus::from_in_memory(logical_generation_, std::move(relations_));
                state_ = RelationSinkState::Finalized;
                return corpus;
            }

            if (!ooc_writer_) {
                throw std::logic_error("RelationSink::finalize: missing OOC writer");
            }
            if (!private_lease_receipt_) {
                throw std::logic_error("RelationSink::finalize: missing private lease ownership");
            }
            const OOCSnapshotDescriptor descriptor = ooc_writer_->finalize();
            if (descriptor.count != static_cast<uint64_t>(count_)) {
                throw std::runtime_error("RelationSink::finalize: OOC count mismatch");
            }

            RelationCorpus corpus = RelationCorpus::from_owned_finalized_ooc(
                logical_generation_, *ooc_writer_, cleanup_policy_, private_lease_receipt_.get());

            // Reader/State construction completed before the factory's final
            // noexcept receipt move. Only now may the empty writer shell go
            // away; on every earlier exception abort() can retry its receipt.
            ooc_writer_.reset();
            private_lease_receipt_.reset();
            owns_artifacts_ = false;
            state_ = RelationSinkState::Finalized;
            return corpus;
        } catch (...) {
            abort();
            throw;
        }
    }

    /// Roll back an unfinished sink. Idempotent and safe from destructors.
    /// A finalized sink has already transferred ownership and is unchanged.
    void abort() noexcept {
        if (state_ == RelationSinkState::Finalized) {
            return;
        }

        relations_.clear();
        cleanup_owned_ooc_noexcept();
        state_ = RelationSinkState::Aborted;
    }

private:
    enum class Backend {
        InMemory,
        OutOfCore,
    };

    RelationSink(uint64_t logical_generation, Backend backend,
                 OOCCleanupPolicy cleanup_policy) noexcept
        : logical_generation_(logical_generation), backend_(backend),
          cleanup_policy_(cleanup_policy) {}

    static constexpr const char* lease_suffix() noexcept {
        return ".gnfs-sink-lease";
    }

    static void validate_logical_generation(uint64_t generation) {
        if (generation == 0) {
            throw std::invalid_argument("RelationSink: logical generation must be nonzero");
        }
    }

    static void validate_base_path(const std::string& base_path) {
        if (base_path.empty() || base_path.find('\0') != std::string::npos) {
            throw std::invalid_argument(
                "RelationSink: OOC base path must be nonempty and contain no NUL");
        }
    }

    static void validate_cleanup_policy(OOCCleanupPolicy cleanup_policy) {
        if (cleanup_policy != OOCCleanupPolicy::Preserve &&
            cleanup_policy != OOCCleanupPolicy::RemoveArtifacts) {
            throw std::invalid_argument("RelationSink: invalid OOC cleanup policy");
        }
    }

    [[nodiscard]] static bool path_exists_checked(const std::filesystem::path& path) {
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(path, ec);
        if (ec) {
            if (ec == std::errc::no_such_file_or_directory) {
                return false;
            }
            throw std::filesystem::filesystem_error("RelationSink: cannot inspect OOC artifact",
                                                    path, ec);
        }
        // symlink_status deliberately treats dangling symlinks as existing
        // artifacts; a fresh sink must never follow and overwrite one.
        return status.type() != std::filesystem::file_type::not_found;
    }

    static void reject_existing_artifacts(const std::string& base_path) {
        const std::filesystem::path index_path(base_path + ".relidx");
        const std::filesystem::path data_path(base_path + ".reldata");
        if (path_exists_checked(index_path) || path_exists_checked(data_path)) {
            throw std::runtime_error(
                "RelationSink: refusing to replace existing OOC artifacts at " + base_path);
        }
    }

    void require_open(const char* operation) const {
        if (state_ != RelationSinkState::Open) {
            throw std::logic_error(std::string("RelationSink::") + operation +
                                   ": sink is not open");
        }
    }

    void cleanup_owned_ooc_noexcept() noexcept {
        if (backend_ != Backend::OutOfCore) {
            return;
        }

        if (owns_artifacts_) {
            OOCCleanupResult result;
            if (ooc_writer_) {
                result = ooc_writer_->remove_owned_artifacts_noexcept();
            } else {
                // Constructor failure before receipt issuance may leave no pair
                // at all. Confirm the complete namespace before releasing the
                // private lease; live or partial artifacts remain fail-closed.
                result = OOCCleanupTransaction::confirm_pair_namespace_reusable(base_path_);
            }
            if (result.completed()) {
                owns_artifacts_ = false;
                ooc_writer_.reset();
            }
        }

        // The move-only lease receipt proves that this sink created the exact
        // private directory. Its lock is outside that directory and persists
        // across rmdir/recreate cycles.
        if (private_lease_receipt_ && !owns_artifacts_) {
            const auto result =
                OOCCleanupTransaction::remove_private_lease(*private_lease_receipt_);
            if (result.completed()) {
                private_lease_receipt_.reset();
            }
        }

        if (owns_artifacts_ || private_lease_receipt_) {
            std::fprintf(stderr,
                         "[relation_sink] incomplete OOC rollback at %s; lease retained when "
                         "possible\n",
                         base_path_.c_str());
        }
    }

    void swap(RelationSink& other) noexcept {
        using std::swap;
        swap(logical_generation_, other.logical_generation_);
        swap(count_, other.count_);
        swap(state_, other.state_);
        swap(backend_, other.backend_);
        swap(cleanup_policy_, other.cleanup_policy_);
        relations_.swap(other.relations_);
        base_path_.swap(other.base_path_);
        ooc_writer_.swap(other.ooc_writer_);
        private_lease_receipt_.swap(other.private_lease_receipt_);
        swap(owns_artifacts_, other.owns_artifacts_);
    }

    uint64_t logical_generation_ = 0;
    size_t count_ = 0;
    RelationSinkState state_ = RelationSinkState::Open;
    Backend backend_ = Backend::InMemory;
    OOCCleanupPolicy cleanup_policy_ = OOCCleanupPolicy::Preserve;
    std::vector<core::Relation> relations_;
    std::string base_path_;
    std::unique_ptr<OOCRelationWriter> ooc_writer_;
    std::unique_ptr<OOCPrivateLeaseOwnershipReceipt> private_lease_receipt_;
    bool owns_artifacts_ = false;
};

} // namespace gnfs::relation

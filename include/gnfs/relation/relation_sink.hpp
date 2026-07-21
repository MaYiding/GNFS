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
/// atomically reserved private directory. The directory remains the exclusive
/// artifact container after handoff and, for RemoveArtifacts, is removed by the
/// final corpus owner. Until handoff succeeds, every failure path closes the
/// writer and best-effort removes only that private container's pair.
///
/// Destruction never commits: an open sink is aborted, and only an explicit
/// finalize() may publish an immutable corpus.
class RelationSink final {
public:
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

        const std::string lease_path = base_path + lease_suffix();
        reject_existing_artifacts(base_path);

        RelationSink sink(logical_generation, Backend::OutOfCore, cleanup_policy);
        sink.lease_path_ = lease_path;

        std::error_code ec;
        const bool lease_created = std::filesystem::create_directory(sink.lease_path_, ec);
        if (ec) {
            throw std::filesystem::filesystem_error("RelationSink: cannot create OOC lease",
                                                    sink.lease_path_, ec);
        }
        if (!lease_created) {
            throw std::runtime_error("RelationSink: OOC lease already exists at " +
                                     sink.lease_path_);
        }
        sink.owns_lease_ = true;

        try {
            // The paired files live inside the directory created exclusively
            // above, so a concurrent ordinary writer at the requested base can
            // neither be truncated nor be mistaken for this sink's artifacts.
            sink.base_path_ = (std::filesystem::path(sink.lease_path_) / "corpus").string();
            sink.owns_artifacts_ = true;
            sink.ooc_writer_ = std::make_unique<OOCRelationWriter>(sink.base_path_);
        } catch (...) {
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
          base_path_(std::move(other.base_path_)), lease_path_(std::move(other.lease_path_)),
          ooc_writer_(std::move(other.ooc_writer_)),
          owns_lease_(std::exchange(other.owns_lease_, false)),
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
            const OOCSnapshotDescriptor descriptor = ooc_writer_->finalize();
            if (descriptor.count != static_cast<uint64_t>(count_)) {
                throw std::runtime_error("RelationSink::finalize: OOC count mismatch");
            }

            // Finalize closes both fstreams. Destroy the writer before opening
            // descriptor-bound mmap handles, which is required on Windows.
            ooc_writer_.reset();
            const std::string cleanup_directory =
                cleanup_policy_ == OOCCleanupPolicy::RemoveArtifacts ? lease_path_ : std::string{};
            RelationCorpus corpus = RelationCorpus::from_finalized_ooc(
                logical_generation_, base_path_, descriptor, cleanup_policy_, cleanup_directory);

            // The corpus is now the live reader and, when requested, the sole
            // cleanup owner. Preserve deliberately leaves the private directory
            // and pair in place for a later descriptor-bound reopen.
            owns_artifacts_ = false;
            owns_lease_ = false;
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

        if (ooc_writer_) {
            ooc_writer_->abort();
            ooc_writer_.reset();
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

        bool artifacts_removed = true;
        if (owns_artifacts_) {
            std::error_code ec;
            (void)std::filesystem::remove(base_path_ + ".relidx", ec);
            if (ec) {
                artifacts_removed = false;
            }
            ec.clear();
            (void)std::filesystem::remove(base_path_ + ".reldata", ec);
            if (ec) {
                artifacts_removed = false;
            }
            if (artifacts_removed) {
                owns_artifacts_ = false;
            }
        }

        // Keep the lease when an artifact could not be removed. That makes a
        // later sink fail closed instead of truncating a partially rolled-back
        // pair. A repeated abort() retries the exact same cleanup.
        if (owns_lease_ && (!owns_artifacts_ || artifacts_removed)) {
            std::error_code ec;
            (void)std::filesystem::remove(lease_path_, ec);
            // remove() returning false without an error means the lease is
            // already absent, which is also a completed best-effort cleanup.
            if (!ec) {
                owns_lease_ = false;
            }
        }

        if (owns_artifacts_ || owns_lease_) {
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
        lease_path_.swap(other.lease_path_);
        ooc_writer_.swap(other.ooc_writer_);
        swap(owns_lease_, other.owns_lease_);
        swap(owns_artifacts_, other.owns_artifacts_);
    }

    uint64_t logical_generation_ = 0;
    size_t count_ = 0;
    RelationSinkState state_ = RelationSinkState::Open;
    Backend backend_ = Backend::InMemory;
    OOCCleanupPolicy cleanup_policy_ = OOCCleanupPolicy::Preserve;
    std::vector<core::Relation> relations_;
    std::string base_path_;
    std::string lease_path_;
    std::unique_ptr<OOCRelationWriter> ooc_writer_;
    bool owns_lease_ = false;
    bool owns_artifacts_ = false;
};

} // namespace gnfs::relation

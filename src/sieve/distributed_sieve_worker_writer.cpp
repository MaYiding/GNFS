#include "distributed_sieve_worker_writer_internal.hpp"

#include <gnfs/relation/ooc_cleanup_transaction.hpp>
#include <gnfs/relation/ooc_relation_store.hpp>
#include <gnfs/util/process.hpp>

#include <cerrno>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace gnfs::sieve::distributed_sieve_worker_entry_detail {

namespace writer_detail = distributed_sieve_worker_writer_detail;
namespace private_lease = gnfs::relation::ooc_cleanup_detail;
namespace ooc_store = gnfs::relation::detail;

namespace {

[[nodiscard]] std::uint64_t current_process_id() noexcept {
    const int id = gnfs::util::process_id();
    return id > 0 ? static_cast<std::uint64_t>(id) : 0;
}

[[nodiscard]] DistributedSieveWorkerWriterDiagnosticV1
failure(DistributedSieveWorkerWriterPhaseV1 phase, DistributedSieveWorkerWriterStatusV1 status,
        int native_error = 0,
        DistributedSieveWorkerWriterRollbackV1 rollback =
            DistributedSieveWorkerWriterRollbackV1::not_applicable,
        int rollback_native_error = 0) noexcept {
    return {
        .phase = phase,
        .status = status,
        .native_error = native_error,
        .rollback = rollback,
        .rollback_native_error = rollback_native_error,
    };
}

[[nodiscard]] DistributedSieveWorkerWriterRollbackV1
writer_rollback(ooc_store::OOCExactFreshRollbackDisposition rollback) noexcept {
    switch (rollback) {
    case ooc_store::OOCExactFreshRollbackDisposition::Clean:
        return DistributedSieveWorkerWriterRollbackV1::clean;
    case ooc_store::OOCExactFreshRollbackDisposition::NamedResidueMayRemain:
        return DistributedSieveWorkerWriterRollbackV1::named_residue_may_remain;
    case ooc_store::OOCExactFreshRollbackDisposition::DirectoryDurabilityUncertain:
        return DistributedSieveWorkerWriterRollbackV1::directory_durability_uncertain;
    }
    return DistributedSieveWorkerWriterRollbackV1::named_residue_may_remain;
}

[[nodiscard]] DistributedSieveWorkerWriterDiagnosticV1
map_writer_creation_failure(const std::exception_ptr& primary,
                            DistributedSieveWorkerWriterRollbackV1 rollback,
                            int rollback_native_error) noexcept {
    try {
        if (primary == nullptr) {
            throw std::runtime_error("missing exact writer primary failure");
        }
        std::rethrow_exception(primary);
    } catch (const std::bad_alloc&) {
        return failure(DistributedSieveWorkerWriterPhaseV1::writer_creation,
                       DistributedSieveWorkerWriterStatusV1::resource_exhausted, ENOMEM, rollback,
                       rollback_native_error);
    } catch (const private_lease::Failure& lease_failure) {
        return failure(DistributedSieveWorkerWriterPhaseV1::lock_adoption,
                       DistributedSieveWorkerWriterStatusV1::lock_invalid,
                       lease_failure.error ? lease_failure.error.value() : EPROTO, rollback,
                       rollback_native_error);
    } catch (const std::system_error& error) {
        return failure(DistributedSieveWorkerWriterPhaseV1::writer_creation,
                       DistributedSieveWorkerWriterStatusV1::writer_failed, error.code().value(),
                       rollback, rollback_native_error);
    } catch (const std::logic_error&) {
        return failure(DistributedSieveWorkerWriterPhaseV1::writer_creation,
                       DistributedSieveWorkerWriterStatusV1::private_lease_invalid, EPROTO,
                       rollback, rollback_native_error);
    } catch (...) {
        return failure(DistributedSieveWorkerWriterPhaseV1::writer_creation,
                       DistributedSieveWorkerWriterStatusV1::unexpected_failure, 0, rollback,
                       rollback_native_error);
    }
}

} // namespace

namespace distributed_sieve_worker_writer_detail {

DistributedSieveWorkerWriterLifetimeGuardV1::DistributedSieveWorkerWriterLifetimeGuardV1(
    void* state, Revalidate revalidate, Destroy destroy) noexcept
    : state_(state), revalidate_(revalidate), destroy_(destroy) {}

DistributedSieveWorkerWriterLifetimeGuardV1::DistributedSieveWorkerWriterLifetimeGuardV1(
    DistributedSieveWorkerWriterLifetimeGuardV1&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)),
      revalidate_(std::exchange(other.revalidate_, nullptr)),
      destroy_(std::exchange(other.destroy_, nullptr)) {}

DistributedSieveWorkerWriterLifetimeGuardV1& DistributedSieveWorkerWriterLifetimeGuardV1::operator=(
    DistributedSieveWorkerWriterLifetimeGuardV1&& other) noexcept {
    if (this != &other) {
        reset_noexcept();
        state_ = std::exchange(other.state_, nullptr);
        revalidate_ = std::exchange(other.revalidate_, nullptr);
        destroy_ = std::exchange(other.destroy_, nullptr);
    }
    return *this;
}

DistributedSieveWorkerWriterLifetimeGuardV1::
    ~DistributedSieveWorkerWriterLifetimeGuardV1() noexcept {
    reset_noexcept();
}

bool DistributedSieveWorkerWriterLifetimeGuardV1::valid() const noexcept {
    return state_ != nullptr && revalidate_ != nullptr && destroy_ != nullptr;
}

bool DistributedSieveWorkerWriterLifetimeGuardV1::stable() const noexcept {
    return valid() && revalidate_(state_);
}

void DistributedSieveWorkerWriterLifetimeGuardV1::reset_noexcept() noexcept {
    void* state = std::exchange(state_, nullptr);
    auto destroy = std::exchange(destroy_, nullptr);
    revalidate_ = nullptr;
    if (state != nullptr && destroy != nullptr) {
        destroy(state);
    }
}

OOCInheritedP8WriterMintV1::OOCInheritedP8WriterMintV1(
    int root_descriptor, int permanent_lock_descriptor, int attempt_lock_descriptor,
    int attempt_directory_descriptor, int package_descriptor, std::uint64_t creator_process_id,
    std::filesystem::path absolute_root_path, std::filesystem::path base_path,
    std::filesystem::path private_directory, std::filesystem::path lock_path,
    std::string private_directory_leaf, std::string lock_leaf,
    std::array<std::uint64_t, 3> root_identity, std::array<std::uint64_t, 3> attempt_lock_identity,
    std::array<std::uint64_t, 3> attempt_directory_identity, std::array<std::uint64_t, 2> lease_id,
    std::array<std::uint64_t, 3> owner_marker_identity,
    std::array<std::uint64_t, 3> owned_marker_identity, AttemptStartedV1 record,
    WaveManifestV1 manifest, DistributedSieveWorkIdentityV1 identity, ChunkPlanV1 chunk,
    distributed_sieve_work_package_codec_detail::DistributedSieveWorkPackageWitnessV1
        package_witness,
    gnfs::relation::OOCPrivateLeaseTestHooks private_lease_hooks)
    : root_descriptor_(root_descriptor), permanent_lock_descriptor_(permanent_lock_descriptor),
      attempt_lock_descriptor_(attempt_lock_descriptor),
      attempt_directory_descriptor_(attempt_directory_descriptor),
      package_descriptor_(package_descriptor), creator_process_id_(creator_process_id),
      absolute_root_path_(std::move(absolute_root_path)), base_path_(std::move(base_path)),
      private_directory_(std::move(private_directory)), lock_path_(std::move(lock_path)),
      private_directory_leaf_(std::move(private_directory_leaf)), lock_leaf_(std::move(lock_leaf)),
      root_identity_(root_identity), attempt_lock_identity_(attempt_lock_identity),
      attempt_directory_identity_(attempt_directory_identity), lease_id_(lease_id),
      owner_marker_identity_(owner_marker_identity), owned_marker_identity_(owned_marker_identity),
      record_(std::move(record)), manifest_(std::move(manifest)), identity_(std::move(identity)),
      chunk_(std::move(chunk)), package_witness_(std::move(package_witness)),
      private_lease_hooks_(private_lease_hooks) {}

OOCInheritedP8WriterMintV1::OOCInheritedP8WriterMintV1(OOCInheritedP8WriterMintV1&& other) noexcept
    : root_descriptor_(std::exchange(other.root_descriptor_, -1)),
      permanent_lock_descriptor_(std::exchange(other.permanent_lock_descriptor_, -1)),
      attempt_lock_descriptor_(std::exchange(other.attempt_lock_descriptor_, -1)),
      attempt_directory_descriptor_(std::exchange(other.attempt_directory_descriptor_, -1)),
      package_descriptor_(std::exchange(other.package_descriptor_, -1)),
      creator_process_id_(std::exchange(other.creator_process_id_, 0)),
      consumed_(std::exchange(other.consumed_, true)),
      lifetime_guard_(std::move(other.lifetime_guard_)),
      absolute_root_path_(std::move(other.absolute_root_path_)),
      base_path_(std::move(other.base_path_)),
      private_directory_(std::move(other.private_directory_)),
      lock_path_(std::move(other.lock_path_)),
      private_directory_leaf_(std::move(other.private_directory_leaf_)),
      lock_leaf_(std::move(other.lock_leaf_)), root_identity_(other.root_identity_),
      attempt_lock_identity_(other.attempt_lock_identity_),
      attempt_directory_identity_(other.attempt_directory_identity_), lease_id_(other.lease_id_),
      owner_marker_identity_(other.owner_marker_identity_),
      owned_marker_identity_(other.owned_marker_identity_), record_(std::move(other.record_)),
      manifest_(std::move(other.manifest_)), identity_(std::move(other.identity_)),
      chunk_(std::move(other.chunk_)), package_witness_(std::move(other.package_witness_)),
      private_lease_hooks_(other.private_lease_hooks_) {}

OOCInheritedP8WriterMintV1::~OOCInheritedP8WriterMintV1() noexcept {
    close_descriptors_noexcept();
}

void OOCInheritedP8WriterMintV1::close_descriptors_noexcept() noexcept {
#if !defined(_WIN32)
    const int descriptors[] = {
        package_descriptor_,      attempt_directory_descriptor_,
        attempt_lock_descriptor_, permanent_lock_descriptor_,
        root_descriptor_,
    };
    for (const int descriptor : descriptors) {
        if (descriptor >= 0) {
            (void)::close(descriptor);
        }
    }
#endif
    root_descriptor_ = -1;
    permanent_lock_descriptor_ = -1;
    attempt_lock_descriptor_ = -1;
    attempt_directory_descriptor_ = -1;
    package_descriptor_ = -1;
}

void OOCInheritedP8WriterMintV1::attach_lifetime_guard(
    DistributedSieveWorkerWriterLifetimeGuardV1&& guard) noexcept {
    lifetime_guard_ = std::move(guard);
}

void OOCInheritedP8WriterMintV1::disarm_writer_post_fork_child_noexcept(
    gnfs::relation::OOCRelationWriter& writer) noexcept {
    writer.discard_inherited_post_fork_child_noexcept();
}

std::unique_ptr<gnfs::relation::OOCRelationWriter>
OOCInheritedP8WriterMintV1::create_exact_writer() {
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
    throw std::system_error(std::make_error_code(std::errc::operation_not_supported),
                            "distributed worker exact writer is unsupported");
#else
    if (consumed_ || creator_process_id_ == 0 || current_process_id() != creator_process_id_ ||
        !lifetime_guard_.stable() || root_descriptor_ < 0 || permanent_lock_descriptor_ < 0 ||
        attempt_lock_descriptor_ < 0 || attempt_directory_descriptor_ < 0 ||
        package_descriptor_ < 0 || base_path_.empty() || private_directory_.empty() ||
        lock_path_.empty() || private_directory_leaf_.empty() || lock_leaf_.empty()) {
        throw std::logic_error("distributed worker writer mint is invalid or already consumed");
    }
    consumed_ = true;

    using BaseLock = gnfs::relation::ooc_cleanup_detail::BaseLock;
    auto adopted_lock = std::unique_ptr<BaseLock>(new BaseLock(
        lock_path_, attempt_lock_descriptor_, root_descriptor_, lock_leaf_, root_identity_,
        attempt_lock_identity_, BaseLock::AdoptInheritedOpenFileDescription{}));
    attempt_lock_descriptor_ = -1;
    std::shared_ptr<BaseLock> live_lock(std::move(adopted_lock));

    gnfs::relation::OOCPrivateLeaseOwnershipReceipt lease(
        base_path_, private_directory_, lock_path_, attempt_directory_identity_, lease_id_,
        owner_marker_identity_, owned_marker_identity_, std::move(live_lock), creator_process_id_);

    gnfs::relation::OOCRelationWriter::ExactPrivateDirectoryBinding exact{
        .root_descriptor = root_descriptor_,
        .directory_descriptor = attempt_directory_descriptor_,
        .creator_process_id = creator_process_id_,
        .root_identity = root_identity_,
        .directory_identity = attempt_directory_identity_,
        .directory_leaf = private_directory_leaf_,
        .index_leaf = "corpus.relidx",
        .data_leaf = "corpus.reldata",
        .authority_context = &lifetime_guard_,
        .authority_stable =
            [](const void* context) noexcept {
                return static_cast<const DistributedSieveWorkerWriterLifetimeGuardV1*>(context)
                    ->stable();
            },
    };
    return std::unique_ptr<gnfs::relation::OOCRelationWriter>(new gnfs::relation::OOCRelationWriter(
        base_path_.string(), std::move(lease),
        gnfs::relation::OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff,
        private_lease_hooks_, std::move(exact),
        gnfs::relation::OOCRelationWriter::ExactPrivateDirectoryConstructionToken{}));
#endif
}

} // namespace distributed_sieve_worker_writer_detail

struct DistributedSieveWorkerWriterAuthorityV1::State final {
    State(writer_detail::OOCInheritedP8WriterMintV1&& mint_value,
          std::uint64_t creator_process_id_value) noexcept
        : mint(std::move(mint_value)), creator_process_id(creator_process_id_value) {}

    ~State() noexcept {
        if (!writer) {
            return;
        }
        if (creator_process_id == 0 || current_process_id() != creator_process_id) {
            // Purge inherited stdio buffers before unregistering the child
            // copy. A normal exit must not flush the parent's pending bytes.
            mint.disarm_writer_post_fork_child_noexcept(*writer);
            writer.reset();
            return;
        }
        if (writer->state() == gnfs::relation::OOCWriterState::Open ||
            writer->state() == gnfs::relation::OOCWriterState::Suspended) {
            writer->abort();
        }
        writer.reset();
    }

    writer_detail::OOCInheritedP8WriterMintV1 mint;
    std::uint64_t creator_process_id = 0;
    std::unique_ptr<gnfs::relation::OOCRelationWriter> writer;
    bool finalized = false;
};

DistributedSieveWorkerWriterAuthorityV1::DistributedSieveWorkerWriterAuthorityV1(
    std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

DistributedSieveWorkerWriterAuthorityV1::DistributedSieveWorkerWriterAuthorityV1(
    DistributedSieveWorkerWriterAuthorityV1&&) noexcept = default;

DistributedSieveWorkerWriterAuthorityV1::~DistributedSieveWorkerWriterAuthorityV1() noexcept =
    default;

bool DistributedSieveWorkerWriterAuthorityV1::valid() const noexcept {
    if (state_ == nullptr || state_->writer == nullptr || state_->creator_process_id == 0 ||
        current_process_id() != state_->creator_process_id) {
        return false;
    }
    const auto writer_state = state_->writer->state();
    return writer_state == gnfs::relation::OOCWriterState::Open ||
           writer_state == gnfs::relation::OOCWriterState::Finalized;
}

bool DistributedSieveWorkerWriterAuthorityV1::finalized() const noexcept {
    return valid() && state_->finalized &&
           state_->writer->state() == gnfs::relation::OOCWriterState::Finalized;
}

std::size_t DistributedSieveWorkerWriterAuthorityV1::count() const noexcept {
    return valid() ? state_->writer->count() : 0;
}

const AttemptStartedV1& DistributedSieveWorkerWriterAuthorityV1::record() const noexcept {
    return state_->mint.record_;
}

const WaveManifestV1& DistributedSieveWorkerWriterAuthorityV1::manifest() const noexcept {
    return state_->mint.manifest_;
}

const DistributedSieveWorkIdentityV1&
DistributedSieveWorkerWriterAuthorityV1::identity() const noexcept {
    return state_->mint.identity_;
}

const ChunkPlanV1& DistributedSieveWorkerWriterAuthorityV1::chunk() const noexcept {
    return state_->mint.chunk_;
}

const distributed_sieve_work_package_codec_detail::DistributedSieveWorkPackageWitnessV1&
DistributedSieveWorkerWriterAuthorityV1::witness() const noexcept {
    return state_->mint.package_witness_;
}

std::size_t DistributedSieveWorkerWriterAuthorityV1::write(const gnfs::core::Relation& relation) {
    if (!valid() || state_->finalized) {
        throw std::logic_error("distributed worker writer authority is not appendable");
    }
    return state_->writer->write(relation);
}

void DistributedSieveWorkerWriterAuthorityV1::finalize() {
    if (!valid()) {
        throw std::logic_error("distributed worker writer authority is invalid");
    }
    if (state_->finalized) {
        throw std::logic_error("distributed worker writer authority is already finalized");
    }
    (void)state_->writer->finalize();
    state_->finalized = true;
}

namespace distributed_sieve_worker_writer_detail {

DistributedSieveWorkerWriterAdoptionResultV1
mint_distributed_sieve_worker_writer_v1(OOCInheritedP8WriterMintV1&& mint) noexcept {
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
    (void)mint;
    return {
        .writer = std::nullopt,
        .diagnostic = failure(DistributedSieveWorkerWriterPhaseV1::platform_gate,
                              DistributedSieveWorkerWriterStatusV1::platform_unsupported, ENOTSUP),
    };
#else
    const std::uint64_t creator_process_id = mint.creator_process_id_;
    if (creator_process_id == 0 || current_process_id() != creator_process_id) {
        return {
            .writer = std::nullopt,
            .diagnostic = failure(DistributedSieveWorkerWriterPhaseV1::process_gate,
                                  DistributedSieveWorkerWriterStatusV1::process_mismatch, ECHILD),
        };
    }
    try {
        auto state = std::make_unique<DistributedSieveWorkerWriterAuthorityV1::State>(
            std::move(mint), creator_process_id);
        state->writer = state->mint.create_exact_writer();
        DistributedSieveWorkerWriterAuthorityV1 authority(std::move(state));
        return {
            .writer = std::optional<DistributedSieveWorkerWriterAuthorityV1>(std::move(authority)),
            .diagnostic = failure(DistributedSieveWorkerWriterPhaseV1::writer_creation,
                                  DistributedSieveWorkerWriterStatusV1::ready),
        };
    } catch (const ooc_store::OOCExactFreshConstructionFailure& exact_failure) {
        return {
            .writer = std::nullopt,
            .diagnostic = map_writer_creation_failure(exact_failure.primary(),
                                                      writer_rollback(exact_failure.rollback()),
                                                      exact_failure.rollback_error().value()),
        };
    } catch (const std::bad_alloc&) {
        return {
            .writer = std::nullopt,
            .diagnostic = failure(DistributedSieveWorkerWriterPhaseV1::writer_creation,
                                  DistributedSieveWorkerWriterStatusV1::resource_exhausted, ENOMEM),
        };
    } catch (const private_lease::Failure& lease_failure) {
        return {
            .writer = std::nullopt,
            .diagnostic = failure(DistributedSieveWorkerWriterPhaseV1::lock_adoption,
                                  DistributedSieveWorkerWriterStatusV1::lock_invalid,
                                  lease_failure.error ? lease_failure.error.value() : EPROTO),
        };
    } catch (const std::system_error& error) {
        return {
            .writer = std::nullopt,
            .diagnostic =
                failure(DistributedSieveWorkerWriterPhaseV1::writer_creation,
                        DistributedSieveWorkerWriterStatusV1::writer_failed, error.code().value()),
        };
    } catch (const std::logic_error&) {
        return {
            .writer = std::nullopt,
            .diagnostic =
                failure(DistributedSieveWorkerWriterPhaseV1::writer_creation,
                        DistributedSieveWorkerWriterStatusV1::private_lease_invalid, EPROTO),
        };
    } catch (...) {
        return {
            .writer = std::nullopt,
            .diagnostic = failure(DistributedSieveWorkerWriterPhaseV1::writer_creation,
                                  DistributedSieveWorkerWriterStatusV1::unexpected_failure),
        };
    }
#endif
}

} // namespace distributed_sieve_worker_writer_detail

} // namespace gnfs::sieve::distributed_sieve_worker_entry_detail

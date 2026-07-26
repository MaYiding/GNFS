#pragma once

// Source-private durable authority boundary for one distributed-sieve wave.
// This file is intentionally not installed as public API.

#include <gnfs/sieve/distributed_sieve_protocol.hpp>
#include <gnfs/util/durable_immutable_record.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>

namespace gnfs::sieve::distributed_sieve_resume_detail {

inline constexpr std::string_view DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF = ".gnfs-wave-v1.lock";
inline constexpr std::string_view DISTRIBUTED_SIEVE_WAVE_MANIFEST_LEAF = ".gnfs-wave-v1.manifest";
inline constexpr std::string_view DISTRIBUTED_SIEVE_WAVE_MANIFEST_PENDING_LEAF =
    ".gnfs-wave-v1.manifest.pending";
inline constexpr std::uint32_t DISTRIBUTED_SIEVE_WAVE_LOCK_SEMANTICS_VERSION_V1 = 1;

enum class DistributedSieveWaveStoreStatus : std::uint8_t {
    ready,
    interrupted,
    invalid_request,
    platform_unsupported,
    root_missing,
    root_invalid,
    lock_missing,
    lock_busy,
    lock_invalid,
    namespace_conflict,
    manifest_missing,
    manifest_conflict,
    manifest_invalid,
    publication_failed,
    durability_failed,
    io_failed,
    resource_exhausted,
    unexpected_failure,
};

[[nodiscard]] constexpr std::string_view
distributed_sieve_wave_store_status_name(DistributedSieveWaveStoreStatus status) noexcept {
    switch (status) {
    case DistributedSieveWaveStoreStatus::ready:
        return "ready";
    case DistributedSieveWaveStoreStatus::interrupted:
        return "interrupted";
    case DistributedSieveWaveStoreStatus::invalid_request:
        return "invalid_request";
    case DistributedSieveWaveStoreStatus::platform_unsupported:
        return "platform_unsupported";
    case DistributedSieveWaveStoreStatus::root_missing:
        return "root_missing";
    case DistributedSieveWaveStoreStatus::root_invalid:
        return "root_invalid";
    case DistributedSieveWaveStoreStatus::lock_missing:
        return "lock_missing";
    case DistributedSieveWaveStoreStatus::lock_busy:
        return "lock_busy";
    case DistributedSieveWaveStoreStatus::lock_invalid:
        return "lock_invalid";
    case DistributedSieveWaveStoreStatus::namespace_conflict:
        return "namespace_conflict";
    case DistributedSieveWaveStoreStatus::manifest_missing:
        return "manifest_missing";
    case DistributedSieveWaveStoreStatus::manifest_conflict:
        return "manifest_conflict";
    case DistributedSieveWaveStoreStatus::manifest_invalid:
        return "manifest_invalid";
    case DistributedSieveWaveStoreStatus::publication_failed:
        return "publication_failed";
    case DistributedSieveWaveStoreStatus::durability_failed:
        return "durability_failed";
    case DistributedSieveWaveStoreStatus::io_failed:
        return "io_failed";
    case DistributedSieveWaveStoreStatus::resource_exhausted:
        return "resource_exhausted";
    case DistributedSieveWaveStoreStatus::unexpected_failure:
        return "unexpected_failure";
    }
    return "unknown";
}

/// Trusted test-only interruption boundaries. Each point is offered only after
/// the named state has been established. `ManifestCanonicalPromoted`
/// intentionally precedes the following root-directory durability barrier.
enum class DistributedSieveWaveStoreFaultPoint : std::uint8_t {
    RootDurable,
    LockDurable,
    ManifestPendingDurable,
    ManifestCanonicalPromoted,
    ManifestCanonicalDurable,
    Count,
};

struct DistributedSieveWaveStoreTestHooks final {
    using StopAfter = bool (*)(DistributedSieveWaveStoreFaultPoint point, void* context) noexcept;

    StopAfter stop_after = nullptr;
    void* context = nullptr;
};

struct DistributedSieveWaveStoreDiagnostic final {
    DistributedSieveWaveStoreStatus status = DistributedSieveWaveStoreStatus::ready;
    std::error_code native_error;
    std::optional<DistributedSieveProtocolStatus> protocol_status;
    std::optional<util::durable_immutable_record::RecordPublishStatus> publication_status;
    std::optional<DistributedSieveWaveStoreFaultPoint> last_durable_fault_point;
};

struct DistributedSieveWaveStoreOpenResult;

/// A process-bound lease on one frozen wave root and its permanent lock.
///
/// The object is deliberately non-copyable and non-movable: closing its lock
/// descriptor is the only release operation. `revalidate()` is fail-closed and
/// rejects use after fork.
class DistributedSieveWaveStore final {
public:
    DistributedSieveWaveStore(const DistributedSieveWaveStore&) = delete;
    DistributedSieveWaveStore& operator=(const DistributedSieveWaveStore&) = delete;
    DistributedSieveWaveStore(DistributedSieveWaveStore&&) = delete;
    DistributedSieveWaveStore& operator=(DistributedSieveWaveStore&&) = delete;
    ~DistributedSieveWaveStore();

    /// Create or idempotently recover one wave. `absolute_root` must already be
    /// in strict component form: no empty, '.', '..', repeated-separator,
    /// trailing-separator, or NUL component is accepted. The four store-owned
    /// manifest fields (root identity, lock identity, lock-semantics version,
    /// and self-digest) must all carry their nil/zero draft values.
    [[nodiscard]] static DistributedSieveWaveStoreOpenResult
    create(const std::filesystem::path& absolute_root, WaveManifestV1 manifest_draft,
           DistributedSieveWaveStoreTestHooks hooks = {}) noexcept;

    /// Open or recover one existing wave using only the expected canonical
    /// manifest digest as caller authority.
    [[nodiscard]] static DistributedSieveWaveStoreOpenResult
    open(const std::filesystem::path& absolute_root,
         const util::Sha256Digest& expected_manifest_digest,
         DistributedSieveWaveStoreTestHooks hooks = {}) noexcept;

    [[nodiscard]] const std::filesystem::path& absolute_root() const noexcept;
    [[nodiscard]] const WaveManifestV1& manifest() const noexcept;
    [[nodiscard]] const util::Sha256Digest& manifest_digest() const noexcept;
    [[nodiscard]] const NativeIdentityV1& wave_root_identity() const noexcept;
    [[nodiscard]] const NativeIdentityV1& permanent_lock_identity() const noexcept;
    [[nodiscard]] const util::durable_immutable_record::RecordSnapshot&
    manifest_snapshot() const noexcept;

    /// Re-establish every held/named identity and immutable manifest binding.
    /// This operation never repairs or mutates the namespace.
    [[nodiscard]] DistributedSieveWaveStoreDiagnostic revalidate() const noexcept;

private:
    struct State;

    explicit DistributedSieveWaveStore(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;
};

struct DistributedSieveWaveStoreOpenResult final {
    std::unique_ptr<DistributedSieveWaveStore> store;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return store != nullptr && diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

} // namespace gnfs::sieve::distributed_sieve_resume_detail

#pragma once

#include "gnfs/util/durable_immutable_file.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

namespace gnfs::util::durable_immutable_record {

using NativeHandle = durable_immutable_file::NativeHandle;
inline constexpr NativeHandle INVALID_NATIVE_HANDLE = durable_immutable_file::INVALID_NATIVE_HANDLE;
inline constexpr std::uint64_t MAX_BOUNDED_READ_BYTES = 16U * 1024U * 1024U;

namespace detail {
class RecordPublishResultFactory;
}

/// Stable native identity of one held filesystem object.
///
/// The field mapping is platform-owned. Callers may compare identities only
/// within the current platform and filesystem namespace.
struct NativeIdentity final {
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    std::uint64_t third = 0;

    [[nodiscard]] friend constexpr bool operator==(const NativeIdentity&,
                                                   const NativeIdentity&) noexcept = default;
};

/// Exact immutable-file observation returned by a no-follow inspection.
struct RecordSnapshot final {
    NativeIdentity identity{};
    std::uint64_t size = 0;

    [[nodiscard]] friend constexpr bool operator==(const RecordSnapshot&,
                                                   const RecordSnapshot&) noexcept = default;
};

enum class InspectState : std::uint8_t {
    missing,
    exact,
    rejected,
    interrupted,
    unsupported,
    failed,
};

/// Closed result for an exact relative-leaf inspection.
class InspectResult final {
public:
    InspectResult() = delete;

    [[nodiscard]] static InspectResult missing() noexcept;
    [[nodiscard]] static InspectResult exact(RecordSnapshot snapshot) noexcept;
    [[nodiscard]] static InspectResult rejected(std::error_code error) noexcept;
    [[nodiscard]] static InspectResult interrupted(std::error_code error) noexcept;
    [[nodiscard]] static InspectResult unsupported(std::error_code error) noexcept;
    [[nodiscard]] static InspectResult failed(std::error_code error) noexcept;

    [[nodiscard]] constexpr InspectState state() const noexcept {
        return state_;
    }

    [[nodiscard]] const std::optional<RecordSnapshot>& snapshot() const noexcept {
        return snapshot_;
    }

    [[nodiscard]] const std::error_code& native_error() const noexcept {
        return native_error_;
    }

private:
    InspectResult(InspectState state, std::optional<RecordSnapshot> snapshot,
                  std::error_code native_error) noexcept;

    InspectState state_;
    std::optional<RecordSnapshot> snapshot_;
    std::error_code native_error_;
};

enum class BoundedReadState : std::uint8_t {
    missing,
    exact,
    rejected,
    interrupted,
    unsupported,
    failed,
};

/// Closed result for one bounded relative no-follow read.
///
/// `exact` is the only state that carries both bytes and a snapshot. The
/// snapshot describes the same held file descriptor from which all bytes were
/// read after its stable name, identity, ownership, mode, link count, and size
/// were re-established.
class BoundedReadResult final {
public:
    BoundedReadResult() = delete;

    [[nodiscard]] static BoundedReadResult missing() noexcept;
    [[nodiscard]] static BoundedReadResult exact(std::vector<std::byte> bytes,
                                                 RecordSnapshot snapshot) noexcept;
    [[nodiscard]] static BoundedReadResult rejected(std::error_code error) noexcept;
    [[nodiscard]] static BoundedReadResult interrupted(std::error_code error) noexcept;
    [[nodiscard]] static BoundedReadResult unsupported(std::error_code error) noexcept;
    [[nodiscard]] static BoundedReadResult failed(std::error_code error) noexcept;

    [[nodiscard]] constexpr BoundedReadState state() const noexcept {
        return state_;
    }

    [[nodiscard]] const std::optional<std::vector<std::byte>>& bytes() const noexcept {
        return bytes_;
    }

    [[nodiscard]] const std::optional<RecordSnapshot>& snapshot() const noexcept {
        return snapshot_;
    }

    [[nodiscard]] const std::error_code& native_error() const noexcept {
        return native_error_;
    }

private:
    BoundedReadResult(BoundedReadState state, std::optional<std::vector<std::byte>> bytes,
                      std::optional<RecordSnapshot> snapshot,
                      std::error_code native_error) noexcept;

    BoundedReadState state_;
    std::optional<std::vector<std::byte>> bytes_;
    std::optional<RecordSnapshot> snapshot_;
    std::error_code native_error_;
};

enum class MutationState : std::uint8_t {
    succeeded,
    source_missing,
    destination_exists,
    identity_mismatch,
    interrupted,
    unsupported,
    failed,
};

/// Closed result for one handle-relative namespace mutation.
class MutationResult final {
public:
    MutationResult() = delete;

    [[nodiscard]] static MutationResult succeeded() noexcept;
    [[nodiscard]] static MutationResult source_missing(std::error_code error) noexcept;
    [[nodiscard]] static MutationResult destination_exists(std::error_code error) noexcept;
    [[nodiscard]] static MutationResult identity_mismatch(std::error_code error) noexcept;
    [[nodiscard]] static MutationResult interrupted(std::error_code error) noexcept;
    [[nodiscard]] static MutationResult unsupported(std::error_code error) noexcept;
    [[nodiscard]] static MutationResult failed(std::error_code error) noexcept;

    [[nodiscard]] constexpr MutationState state() const noexcept {
        return state_;
    }

    [[nodiscard]] const std::error_code& native_error() const noexcept {
        return native_error_;
    }

private:
    MutationResult(MutationState state, std::error_code native_error) noexcept;

    MutationState state_;
    std::error_code native_error_;
};

/// Trusted test-only injectable record-level I/O boundary. Production and
/// authority-bearing callers must use publish_at(); a synthetic result from
/// this seam is never a durable authority or ownership receipt.
///
/// The caller exclusively controls `parent_handle` and its namespace for the
/// complete operation. Implementations must use only handle-relative,
/// no-follow operations and must never close the borrowed parent handle.
class RecordOps {
public:
    virtual ~RecordOps() = default;

    /// Read one leaf through a relative no-follow open, within an inclusive
    /// byte range. Production accepts only a regular, single-link,
    /// current-owner file with exact mode 0600 and stable name and identity
    /// before and after the read. The borrowed parent directory must also
    /// satisfy the production owner, mode, and ACL policy.
    ///
    /// The default is fail-closed so existing test doubles do not silently
    /// acquire read authority. Synthetic `exact` results are data only and
    /// never establish filesystem identity or deletion/adoption authority.
    [[nodiscard]] virtual BoundedReadResult read_bounded_at(NativeHandle parent_handle,
                                                            const std::filesystem::path& leaf,
                                                            std::uint64_t min_bytes,
                                                            std::uint64_t max_bytes) noexcept;

    /// Prove that this parent/filesystem supports handle-relative no-replace
    /// promotion without creating a leaf. Production uses an empty-source
    /// native call that cannot name or mutate a filesystem object and accepts
    /// only the native source-missing result.
    [[nodiscard]] virtual MutationResult
    probe_no_replace_at(NativeHandle parent_handle) noexcept = 0;

    /// Publish and confirm one new pending leaf. Production delegates to
    /// durable_immutable_file::publish_at().
    [[nodiscard]] virtual durable_immutable_file::PublishResult
    publish_pending_at(NativeHandle parent_handle, const std::filesystem::path& pending_leaf,
                       std::span<const std::byte> expected_bytes) noexcept = 0;

    /// Inspect one leaf through a relative no-follow open. `exact` requires a
    /// regular, single-link, current-owner file with exact mode 0600 whose size
    /// and bytes match, with stable native identity before and after the read.
    /// Production also rejects macOS extended ACLs before and after the read.
    [[nodiscard]] virtual InspectResult
    inspect_exact_at(NativeHandle parent_handle, const std::filesystem::path& leaf,
                     std::span<const std::byte> expected_bytes) noexcept = 0;

    /// Atomically promote the exact pending identity without replacing an
    /// existing canonical leaf.
    [[nodiscard]] virtual MutationResult
    promote_no_replace_at(NativeHandle parent_handle, const std::filesystem::path& pending_leaf,
                          const std::filesystem::path& canonical_leaf,
                          const RecordSnapshot& expected_pending) noexcept = 0;

    [[nodiscard]] virtual durable_immutable_file::OperationResult
    sync_parent_directory(NativeHandle parent_handle) noexcept = 0;

    /// Re-establish file/parent/file durability for an existing canonical leaf.
    /// Production delegates to durable_immutable_file::confirm_durable_at().
    [[nodiscard]] virtual durable_immutable_file::PublishResult
    confirm_durable_at(NativeHandle parent_handle,
                       const std::filesystem::path& canonical_leaf) noexcept = 0;

    /// Remove only the still-named exact pending identity.
    [[nodiscard]] virtual MutationResult
    remove_exact_at(NativeHandle parent_handle, const std::filesystem::path& pending_leaf,
                    const RecordSnapshot& expected_pending) noexcept = 0;
};

/// Test-only bounded-read seam. `min_bytes` and `max_bytes` are inclusive, and
/// `max_bytes` may not exceed MAX_BOUNDED_READ_BYTES.
/// Inputs are validated before `ops` is called, interrupted operations are
/// retried, and malformed synthetic results fail closed. An exact synthetic
/// result is protocol data only and never a filesystem authority receipt.
[[nodiscard]] BoundedReadResult
read_bounded_at_with_ops(NativeHandle parent_handle, const std::filesystem::path& leaf,
                         std::uint64_t min_bytes, std::uint64_t max_bytes, RecordOps& ops) noexcept;

/// Production bounded relative no-follow read. macOS performs the entire read
/// from one held descriptor and validates the parent policy plus the named and
/// held objects before and after reading. Linux, Windows, and other platforms
/// return `unsupported` after request validation and before filesystem
/// observation because this layer has no approved full-filesystem ACL adapter
/// there. Only this production entry point may support a classifier's
/// filesystem observation; the with-ops seam is non-authoritative.
[[nodiscard]] BoundedReadResult read_bounded_at(NativeHandle parent_handle,
                                                const std::filesystem::path& leaf,
                                                std::uint64_t min_bytes,
                                                std::uint64_t max_bytes) noexcept;

/// Test-only crash boundaries. `CanonicalPromoted` intentionally occurs before
/// the following parent-directory durability barrier.
enum class RecordFaultPoint : std::uint8_t {
    PendingDurable,
    CanonicalPromoted,
    CanonicalDurable,
};

struct RecordTestHooks final {
    using StopAfter = bool (*)(RecordFaultPoint point, void* context) noexcept;

    StopAfter stop_after = nullptr;
    void* context = nullptr;
};

enum class RecordPublishStatus : std::uint8_t {
    durable,
    interrupted,
    invalid_request,
    input_too_large,
    platform_unsupported,
    pending_publish_failed,
    pending_conflict,
    canonical_conflict,
    promotion_failed,
    parent_sync_failed,
    canonical_confirm_failed,
    pending_cleanup_failed,
    ops_contract_violation,
    unexpected_failure,
};

enum class RecordPublishDisposition : std::uint8_t {
    none,
    created,
    recovered_pending,
    confirmed_existing,
};

/// Closed publication outcome. A canonical snapshot is returned only after the
/// exact canonical leaf has survived its final post-durability inspection.
class RecordPublishResult final {
public:
    RecordPublishResult() = delete;

    [[nodiscard]] constexpr RecordPublishStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] constexpr RecordPublishDisposition disposition() const noexcept {
        return disposition_;
    }

    [[nodiscard]] const std::optional<RecordSnapshot>& canonical_snapshot() const noexcept {
        return canonical_snapshot_;
    }

    [[nodiscard]] const std::error_code& native_error() const noexcept {
        return native_error_;
    }

    [[nodiscard]] constexpr bool is_durable() const noexcept {
        return status_ == RecordPublishStatus::durable;
    }

private:
    RecordPublishResult(RecordPublishStatus status, RecordPublishDisposition disposition,
                        std::optional<RecordSnapshot> canonical_snapshot,
                        std::error_code native_error) noexcept;

    RecordPublishStatus status_;
    RecordPublishDisposition disposition_;
    std::optional<RecordSnapshot> canonical_snapshot_;
    std::error_code native_error_;

    friend class detail::RecordPublishResultFactory;
};

/// Publish one immutable record from `pending_leaf` to `canonical_leaf` under a
/// borrowed parent handle. Both names must be ASCII single-component leaves
/// that remain distinct after ASCII case folding.
///
/// An exact existing canonical record is idempotently confirmed. An exact
/// pending record is recoverable. Foreign, malformed, linked, or replaced
/// entries are preserved and fail closed.
[[nodiscard]] RecordPublishResult publish_at_with_ops(NativeHandle parent_handle,
                                                      const std::filesystem::path& pending_leaf,
                                                      const std::filesystem::path& canonical_leaf,
                                                      std::span<const std::byte> expected_bytes,
                                                      RecordOps& ops,
                                                      RecordTestHooks hooks = {}) noexcept;

/// Production handle-relative implementation. Windows fails with
/// `platform_unsupported` before invoking any namespace mutation because this
/// layer has no approved relative no-reparse promotion primitive there. POSIX
/// production requires the held parent directory to be current-owner and not
/// group/other writable; macOS additionally rejects an extended parent ACL.
[[nodiscard]] RecordPublishResult publish_at(NativeHandle parent_handle,
                                             const std::filesystem::path& pending_leaf,
                                             const std::filesystem::path& canonical_leaf,
                                             std::span<const std::byte> expected_bytes,
                                             RecordTestHooks hooks = {}) noexcept;

} // namespace gnfs::util::durable_immutable_record

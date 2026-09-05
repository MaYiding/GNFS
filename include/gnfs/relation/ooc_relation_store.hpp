#pragma once

#include "gnfs/core/relation.hpp"
#include "gnfs/relation/large_prime_key.hpp"
#include "gnfs/relation/ooc_cleanup_transaction.hpp"
#include "gnfs/relation/ooc_relation_format.hpp"
#include "gnfs/relation/relation_corpus_sha256.hpp"
#include "gnfs/relation/relation_sequence_receipt.hpp"
#include "gnfs/util/mmap_file.hpp"
#include "gnfs/util/native_binary_update_file.hpp"
#include "gnfs/util/owned_native_file.hpp"
#include "gnfs/util/process.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gnfs::sieve::distributed_sieve_worker_entry_detail {
class DistributedSieveWorkerWriterAuthorityV1;
}

namespace gnfs::sieve::distributed_sieve_merge_writer_authority_detail {
class DistributedSieveMergeWriterAuthorityV1;
}

namespace gnfs::sieve::distributed_sieve_resume_detail {
class DistributedSieveMergeStartedWriterMintV1;
}

namespace gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail {
class DistributedSieveWorkerCleanupIntentConversionAuthorityV1;
}

namespace gnfs::relation {

class OOCPrivateHandoffReader;

namespace ooc_cleanup_detail {
class OOCPrivateHandoffCleanupIntentConversionExecutorV2;
class OOCPrivateHandoffReadOnlyReleaseExecutorV1;
} // namespace ooc_cleanup_detail

enum class OOCWriterState {
    Open,
    Suspended,
    Finalized,
    Failed,
};

enum class OOCRecoveryOutcome {
    None,
    AppendablePrefix,
    FinalizedCorpus,
};

/// Stable description of a flushed relation prefix.
///
/// `store_id` is persisted in both V3 file headers, so a checkpoint can reject
/// either half of a different store across process restarts. `generation`
/// rejects stale in-process handoffs and is restored from the paired sieve
/// checkpoint; `count`/`data_end` describe the committed physical on-disk
/// prefix, including the immutable data header.
struct OOCSnapshotDescriptor {
    uint64_t format_version = 0;
    uint64_t store_id = 0;
    uint64_t generation = 0;
    uint64_t count = 0;
    uint64_t data_end = 0;

    friend bool operator==(const OOCSnapshotDescriptor&, const OOCSnapshotDescriptor&) = default;
};

/// Immutable facts reproduced from the exact retained handles of one durable
/// finalized V3 corpus.
///
/// This value grants no file, namespace, lease, or cleanup authority. The
/// relation writer mints it only after final MAGIC and directory durability,
/// then validates the full corpus through read-only duplicates of the same
/// native objects before closing its update streams.
struct OOCFinalizedCorpusEvidenceV1 final {
    struct NativeFileExtent final {
        std::array<std::uint64_t, 3> identity{};
        std::uint64_t extent = 0;

        [[nodiscard]] friend constexpr bool operator==(const NativeFileExtent&,
                                                       const NativeFileExtent&) noexcept = default;
    };

    OOCSnapshotDescriptor descriptor;
    NativeFileExtent index_file;
    NativeFileExtent data_file;
    RelationSequenceReceipt sequence_receipt;
    util::Sha256Digest corpus_sha256;

    [[nodiscard]] friend constexpr bool
    operator==(const OOCFinalizedCorpusEvidenceV1&,
               const OOCFinalizedCorpusEvidenceV1&) noexcept = default;
};

/// Opaque application payload produced from exact finalized corpus evidence.
struct OOCPrivateHandoffPayloadV1 final {
    std::uint32_t kind = 0;
    std::uint32_t version = 0;
    std::vector<std::byte> bytes;
};

using OOCPrivateHandoffPayloadBuilderV1 =
    OOCPrivateHandoffPayloadV1 (*)(const OOCFinalizedCorpusEvidenceV1& evidence, void* context);

/// Fully validated paired resume prefix. The writer constructs it only after
/// validating every compact record and index boundary through the descriptor's
/// committed extent. Bytes after that extent are uncommitted crash tail.
struct OOCValidatedResumePrefix {
    uint64_t count = 0;
    uint64_t data_end = 0;
    std::unordered_set<gnfs::core::ABPair, gnfs::core::ABPairHash> seen;
    RelationSequenceReceipt checkpoint_sequence_receipt;
    RelationSequenceReceiptAccumulator accepted_sequence;
    uint64_t full_relations = 0;
    uint64_t partial_1lp = 0;
    uint64_t partial_2lp = 0;
};

class OOCRelationPrefixReader;

namespace detail {

inline void sync_parent_directory_after_metadata_change(const std::filesystem::path& entry_path) {
    auto parent = entry_path.parent_path();
    if (parent.empty()) {
        parent = ".";
    }
#ifdef _WIN32
    const HANDLE directory = ::CreateFileW(
        parent.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH,
        nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(
            "OOC directory sync cannot open " + parent.string() + " (Win32 error " +
            std::to_string(static_cast<unsigned long>(::GetLastError())) + ")");
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (!::GetFileInformationByHandle(directory, &information)) {
        const auto code = static_cast<unsigned long>(::GetLastError());
        (void)::CloseHandle(directory);
        throw std::runtime_error("OOC directory sync cannot inspect " + parent.string() +
                                 " (Win32 error " + std::to_string(code) + ")");
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        const auto code = static_cast<unsigned long>(
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ? ERROR_ACCESS_DENIED
                                                                               : ERROR_DIRECTORY);
        (void)::CloseHandle(directory);
        throw std::runtime_error("OOC directory sync rejected untrusted parent " + parent.string() +
                                 " (Win32 error " + std::to_string(code) + ")");
    }
    if (!::FlushFileBuffers(directory)) {
        const auto code = static_cast<unsigned long>(::GetLastError());
        (void)::CloseHandle(directory);
        throw std::runtime_error("OOC directory sync failed for " + parent.string() +
                                 " (Win32 error " + std::to_string(code) + ")");
    }
    if (!::CloseHandle(directory)) {
        throw std::runtime_error(
            "OOC directory sync close failed for " + parent.string() + " (Win32 error " +
            std::to_string(static_cast<unsigned long>(::GetLastError())) + ")");
    }
#else
    int directory = -1;
    do {
        directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (directory < 0 && errno == EINTR);
    if (directory < 0) {
        throw std::runtime_error("OOC directory sync cannot open " + parent.string() + ": " +
                                 std::strerror(errno));
    }

    int result = -1;
    do {
#if defined(__APPLE__)
        result = ::fcntl(directory, F_FULLFSYNC);
#else
        result = ::fsync(directory);
#endif
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        const int saved_errno = errno;
        (void)::close(directory);
        throw std::runtime_error("OOC directory sync failed for " + parent.string() + ": " +
                                 std::strerror(saved_errno));
    }
    if (::close(directory) != 0) {
        throw std::runtime_error("OOC directory sync close failed for " + parent.string() + ": " +
                                 std::strerror(errno));
    }
#endif
}

inline void sync_directory_descriptor_after_metadata_change(int descriptor,
                                                            const std::string& label) {
#ifdef _WIN32
    (void)descriptor;
    throw std::system_error(std::make_error_code(std::errc::operation_not_supported),
                            "OOC directory descriptor sync is unsupported for " + label);
#else
    if (descriptor < 0) {
        throw std::invalid_argument("OOC directory descriptor sync has invalid handle for " +
                                    label);
    }
    struct stat metadata {};
    int inspected = -1;
    do {
        inspected = ::fstat(descriptor, &metadata);
    } while (inspected != 0 && errno == EINTR);
    if (inspected != 0 || !S_ISDIR(metadata.st_mode)) {
        const int saved_errno = errno == 0 ? ENOTDIR : errno;
        throw std::system_error(saved_errno, std::generic_category(),
                                "OOC directory descriptor sync rejected " + label);
    }
    int result = -1;
    do {
#if defined(__APPLE__)
        result = ::fcntl(descriptor, F_FULLFSYNC);
#else
        result = ::fsync(descriptor);
#endif
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "OOC directory descriptor sync failed for " + label);
    }
#endif
}

enum class OOCExactFreshRollbackDisposition : std::uint8_t {
    Clean,
    NamedResidueMayRemain,
    DirectoryDurabilityUncertain,
};

class OOCExactFreshConstructionFailure final : public std::exception {
public:
    OOCExactFreshConstructionFailure(std::exception_ptr primary,
                                     OOCExactFreshRollbackDisposition rollback,
                                     std::error_code rollback_error) noexcept
        : primary_(std::move(primary)), rollback_(rollback), rollback_error_(rollback_error) {}

    [[nodiscard]] const char* what() const noexcept override {
        return "OOC exact fresh construction failed";
    }

    [[nodiscard]] const std::exception_ptr& primary() const noexcept {
        return primary_;
    }

    [[nodiscard]] OOCExactFreshRollbackDisposition rollback() const noexcept {
        return rollback_;
    }

    [[nodiscard]] const std::error_code& rollback_error() const noexcept {
        return rollback_error_;
    }

private:
    std::exception_ptr primary_;
    OOCExactFreshRollbackDisposition rollback_ = OOCExactFreshRollbackDisposition::Clean;
    std::error_code rollback_error_;
};

inline constexpr uint64_t MAX_COMPACT_RELATION_BYTES =
    sizeof(int64_t) + sizeof(uint64_t) +
    2 * (sizeof(uint32_t) +
         static_cast<uint64_t>(gnfs::core::Relation::MAX_SERIALIZED_FACTORS) * sizeof(uint32_t)) +
    2 * (sizeof(uint32_t) +
         static_cast<uint64_t>(gnfs::core::Relation::MAX_SERIALIZED_LARGE_PRIMES) *
             (2 * sizeof(uint64_t) + sizeof(uint8_t))) +
    sizeof(uint32_t) +
    static_cast<uint64_t>(gnfs::core::Relation::MAX_SERIALIZED_EXTRA_AB_PAIRS) *
        (sizeof(int64_t) + sizeof(uint64_t));

// Every compact record contains a/b and the five variable-length section
// counts, even when all sections are empty.  This lower bound lets readers
// reject a forged corpus count before reserving count-sized containers.
inline constexpr uint64_t MIN_COMPACT_RELATION_BYTES =
    sizeof(int64_t) + sizeof(uint64_t) + 5 * sizeof(uint32_t);

inline void validate_compact_relation_count(uint64_t count, uint64_t data_size,
                                            uint64_t data_header_bytes, const char* operation) {
    if (data_size < data_header_bytes ||
        count > (data_size - data_header_bytes) / MIN_COMPACT_RELATION_BYTES) {
        throw std::runtime_error(std::string(operation) + ": relation count exceeds data extent");
    }
}

/// Decode one compact OOC record with the same persistence limits used by
/// core::Relation::serialize(). Keeping this as the single compact decoder lets
/// both ordinary readers and resume validation prove the identical contract.
inline gnfs::core::Relation deserialize_compact_relation(const uint8_t* ptr, size_t avail) {
    gnfs::core::Relation rel;
    size_t pos = 0;

    auto read_val = [&](auto& value) {
        if (pos > avail || sizeof(value) > avail - pos) {
            throw std::runtime_error("OOCRelationReader: corrupt record (truncated)");
        }
        std::memcpy(&value, ptr + pos, sizeof(value));
        pos += sizeof(value);
    };

    auto checked_bytes = [&](uint32_t count, size_t element_size, uint32_t max_count,
                             const char* field) -> size_t {
        if (count > max_count) {
            throw std::runtime_error(std::string("OOCRelationReader: corrupt record (") + field +
                                     " count exceeds limit)");
        }
        if (element_size == 0 || pos > avail ||
            static_cast<size_t>(count) > (avail - pos) / element_size) {
            throw std::runtime_error(std::string("OOCRelationReader: corrupt record (") + field +
                                     " byte count overflow)");
        }
        return static_cast<size_t>(count) * element_size;
    };

    read_val(rel.a);
    read_val(rel.b);
    if (rel.b == 0) {
        throw std::runtime_error("OOCRelationReader: corrupt record (b must be nonzero)");
    }

    uint32_t rational_count = 0;
    read_val(rational_count);
    const size_t rational_bytes =
        checked_bytes(rational_count, sizeof(uint32_t),
                      gnfs::core::Relation::MAX_SERIALIZED_FACTORS, "rational_factors");
    rel.rational_factors.resize(rational_count);
    if (rational_count > 0) {
        std::memcpy(rel.rational_factors.data(), ptr + pos, rational_bytes);
        pos += rational_bytes;
    }

    uint32_t algebraic_count = 0;
    read_val(algebraic_count);
    const size_t algebraic_bytes =
        checked_bytes(algebraic_count, sizeof(uint32_t),
                      gnfs::core::Relation::MAX_SERIALIZED_FACTORS, "algebraic_factors");
    rel.algebraic_factors.resize(algebraic_count);
    if (algebraic_count > 0) {
        std::memcpy(rel.algebraic_factors.data(), ptr + pos, algebraic_bytes);
        pos += algebraic_bytes;
    }

    uint32_t rational_lp_count = 0;
    read_val(rational_lp_count);
    (void)checked_bytes(rational_lp_count, sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint8_t),
                        gnfs::core::Relation::MAX_SERIALIZED_LARGE_PRIMES, "rational_large_prime");
    rel.rational_large_prime.resize(rational_lp_count);
    for (uint32_t i = 0; i < rational_lp_count; ++i) {
        read_val(rel.rational_large_prime[i].p);
        read_val(rel.rational_large_prime[i].r);
        read_val(rel.rational_large_prime[i].e);
    }

    uint32_t algebraic_lp_count = 0;
    read_val(algebraic_lp_count);
    (void)checked_bytes(algebraic_lp_count, sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint8_t),
                        gnfs::core::Relation::MAX_SERIALIZED_LARGE_PRIMES, "algebraic_large_prime");
    rel.algebraic_large_prime.resize(algebraic_lp_count);
    for (uint32_t i = 0; i < algebraic_lp_count; ++i) {
        read_val(rel.algebraic_large_prime[i].p);
        read_val(rel.algebraic_large_prime[i].r);
        read_val(rel.algebraic_large_prime[i].e);
    }

    uint32_t extra_count = 0;
    read_val(extra_count);
    (void)checked_bytes(extra_count, sizeof(int64_t) + sizeof(uint64_t),
                        gnfs::core::Relation::MAX_SERIALIZED_EXTRA_AB_PAIRS, "extra_ab_pairs");
    rel.extra_ab_pairs.resize(extra_count);
    for (uint32_t i = 0; i < extra_count; ++i) {
        read_val(rel.extra_ab_pairs[i].first);
        read_val(rel.extra_ab_pairs[i].second);
        if (rel.extra_ab_pairs[i].second == 0) {
            throw std::runtime_error(
                "OOCRelationReader: corrupt record (extra (a,b) pair b must be nonzero)");
        }
    }

    if (pos != avail) {
        throw std::runtime_error("OOCRelationReader: corrupt record (trailing bytes)");
    }

    return rel;
}

} // namespace detail

/// Out-of-core relation storage for large-scale GNFS factorizations.
///
/// Design:
///   - **Data file** (.reldata): Concatenated binary-serialized relations
///   - **Index file** (.relidx): Array of uint64_t byte offsets into the data file
///   - Write phase: append-only streaming (no random writes)
///   - Read phase: mmap both files for O(1) random access to any relation
///
/// File format:
///   V3 .relidx: [magic][version][store_id][count][offset_0][offset_1]...
///   V3 .reldata: [data_magic][version][store_id][serialized_relation_0]...
///   V2 .relidx: [magic][version][store_id][count][offset_0][offset_1]...
///   legacy finalized V1: [legacy magic][count][offset_0][offset_1]...
///   V1/V2 .reldata: [serialized_relation_0][serialized_relation_1]...
///
/// Each serialized relation uses a compact binary format (not the v2 checksum format,
/// which has overhead). Fields are written in order with explicit length prefixes.
///
/// For 25-digit (~10K relations, ~2MB): works but overkill.
/// For 50+ digit (~10M relations, ~2-5GB): essential to avoid OOM.
class OOCRelationWriter {
public:
    static constexpr uint64_t MAGIC_V1_FINAL = OOCRelationStoreFormat::MAGIC_V1_FINAL;
    static constexpr uint64_t MAGIC_INCOMPLETE_V1 = OOCRelationStoreFormat::MAGIC_INCOMPLETE_V1;
    static constexpr uint64_t MAGIC_V2_FINAL = OOCRelationStoreFormat::MAGIC_V2_FINAL;
    static constexpr uint64_t MAGIC_V2_INCOMPLETE = OOCRelationStoreFormat::MAGIC_V2_INCOMPLETE;
    static constexpr uint64_t MAGIC_V3_FINAL = OOCRelationStoreFormat::MAGIC_V3_FINAL;
    static constexpr uint64_t MAGIC_V3_INCOMPLETE = OOCRelationStoreFormat::MAGIC_V3_INCOMPLETE;
    static constexpr uint64_t MAGIC_V3_DATA = OOCRelationStoreFormat::MAGIC_V3_DATA;

    static constexpr uint64_t FORMAT_VERSION_V2 = OOCRelationStoreFormat::FORMAT_VERSION_V2;
    static constexpr uint64_t FORMAT_VERSION_V3 = OOCRelationStoreFormat::FORMAT_VERSION_V3;

    // New writes and all paired recovery use V3. Explicit V1/V2 constants are
    // retained solely for ordinary finalized-reader compatibility.
    static constexpr uint64_t MAGIC = MAGIC_V3_FINAL;
    static constexpr uint64_t MAGIC_INCOMPLETE = MAGIC_V3_INCOMPLETE;
    static constexpr uint64_t FORMAT_VERSION = FORMAT_VERSION_V3;

    static constexpr uint64_t INDEX_FORMAT_VERSION_OFFSET =
        OOCRelationStoreFormat::INDEX_FORMAT_VERSION_OFFSET;
    static constexpr uint64_t INDEX_STORE_ID_OFFSET = OOCRelationStoreFormat::INDEX_STORE_ID_OFFSET;
    static constexpr uint64_t INDEX_COUNT_OFFSET = OOCRelationStoreFormat::INDEX_COUNT_OFFSET;
    static constexpr uint64_t INDEX_HEADER_BYTES = OOCRelationStoreFormat::INDEX_HEADER_BYTES;
    static constexpr uint64_t INDEX_SENTINEL_BYTES = OOCRelationStoreFormat::INDEX_SENTINEL_BYTES;

    static constexpr uint64_t DATA_FORMAT_VERSION_OFFSET =
        OOCRelationStoreFormat::DATA_FORMAT_VERSION_OFFSET;
    static constexpr uint64_t DATA_STORE_ID_OFFSET = OOCRelationStoreFormat::DATA_STORE_ID_OFFSET;
    static constexpr uint64_t DATA_HEADER_BYTES = OOCRelationStoreFormat::DATA_HEADER_BYTES;

    enum class FinalizeStage {
        MetadataDurable,
        FinalMagicDurable,
    };
    using FinalizeStageHook = void (*)(FinalizeStage);

    enum class PrivateLeaseMode {
        ActivateImmediately,
        DeferCleanupHandoff,
    };

    enum class RecoveryFaultPoint : std::uint8_t {
        AppendablePrefixValidated,
        FinalizedPrefixValidated,
    };

    struct RecoveryTestHooks final {
        using StopAfter = bool (*)(RecoveryFaultPoint point, void* context) noexcept;

        StopAfter stop_after = nullptr;
        void* context = nullptr;
    };

    [[nodiscard]] static uint64_t index_size_for_count(uint64_t count) {
        if (count >= static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            throw std::overflow_error("OOCRelationWriter: count leaves no room for sentinel");
        }
        if (count >
            (std::numeric_limits<uint64_t>::max() - INDEX_HEADER_BYTES - INDEX_SENTINEL_BYTES) /
                sizeof(uint64_t)) {
            throw std::overflow_error("OOCRelationWriter: index size overflow");
        }
        const uint64_t size = INDEX_HEADER_BYTES + INDEX_SENTINEL_BYTES + count * sizeof(uint64_t);
        if (size > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            throw std::overflow_error("OOCRelationWriter: index position overflow");
        }
        return size;
    }

    // 1 MB stream buffer per stream — 千万级关系下减少 syscall。
    static constexpr size_t BUFFER_BYTES = 1 << 20;

private:
    struct ConstructionToken final {};
    struct ExactPrivateDirectoryConstructionToken final {};

    struct ExactPrivateDirectoryBinding final {
        int root_descriptor = -1;
        int directory_descriptor = -1;
        std::uint64_t creator_process_id = 0;
        std::array<std::uint64_t, 3> root_identity{};
        std::array<std::uint64_t, 3> directory_identity{};
        std::string directory_leaf;
        std::string index_leaf;
        std::string data_leaf;
        const void* authority_context = nullptr;
        bool (*authority_stable)(const void* context) noexcept = nullptr;
    };

    class DeferredPrivateLeaseActionGuard final {
    public:
        explicit DeferredPrivateLeaseActionGuard(OOCRelationWriter& owner) : owner_(owner) {
            if (owner_.deferred_private_lease_action_in_progress_) {
                throw std::logic_error(
                    "OOCRelationWriter: deferred private-lease action is already in progress");
            }
            owner_.deferred_private_lease_action_in_progress_ = true;
        }

        DeferredPrivateLeaseActionGuard(const DeferredPrivateLeaseActionGuard&) = delete;
        DeferredPrivateLeaseActionGuard& operator=(const DeferredPrivateLeaseActionGuard&) = delete;

        ~DeferredPrivateLeaseActionGuard() {
            owner_.deferred_private_lease_action_in_progress_ = false;
        }

    private:
        OOCRelationWriter& owner_;
    };

    /// Source-private amortization boundary for one exact writer.
    ///
    /// The begin and commit barriers run the ordinary exact named-identity
    /// and authority callback. Rows inside the batch still check the creator
    /// process before and after mutation, but avoid repeating the complete
    /// WaveStore inventory validation for every relation. Dropping an
    /// uncommitted guard poisons and closes the writer.
    class ExactAppendBatchGuard final {
    public:
        ExactAppendBatchGuard(const ExactAppendBatchGuard&) = delete;
        ExactAppendBatchGuard& operator=(const ExactAppendBatchGuard&) = delete;
        ExactAppendBatchGuard(ExactAppendBatchGuard&&) = delete;
        ExactAppendBatchGuard& operator=(ExactAppendBatchGuard&&) = delete;

        ~ExactAppendBatchGuard() noexcept {
            if (owner_ == nullptr) {
                return;
            }
            owner_->exact_append_batch_active_ = false;
            owner_->fail_and_close_noexcept();
        }

        void commit() {
            if (owner_ == nullptr || !owner_->exact_append_batch_active_) {
                throw std::logic_error("OOCRelationWriter: exact append batch is not active");
            }
            OOCRelationWriter* owner = owner_;
            try {
                owner->require_state(OOCWriterState::Open, "exact append batch commit");
                owner->require_store_named_identity("exact append batch commit");
                owner->exact_append_batch_active_ = false;
                owner_ = nullptr;
            } catch (...) {
                owner->exact_append_batch_active_ = false;
                owner->fail_and_close_noexcept();
                owner_ = nullptr;
                throw;
            }
        }

    private:
        explicit ExactAppendBatchGuard(OOCRelationWriter& owner) noexcept : owner_(&owner) {}

        OOCRelationWriter* owner_ = nullptr;

        friend class OOCRelationWriter;
    };

public:
    /// Fresh create writes paired incomplete V3 headers with one durable store
    /// identity.
    explicit OOCRelationWriter(const std::string& base_path)
        : OOCRelationWriter(base_path, std::nullopt, std::nullopt, nullptr,
                            PrivateLeaseMode::ActivateImmediately, {}, {}, {},
                            ConstructionToken{}) {}

    /// Fresh private-lease creation keeps the lease's persistent BaseLock held
    /// across both O_EXCL reservations and durable lease activation.
    OOCRelationWriter(const std::string& base_path, OOCPrivateLeaseOwnershipReceipt& private_lease)
        : OOCRelationWriter(base_path, std::nullopt, std::nullopt, &private_lease,
                            PrivateLeaseMode::ActivateImmediately, {}, {}, {},
                            ConstructionToken{}) {}

    /// Fork-worker creation consumes the child process's move-only lease copy.
    /// The writer therefore has no same-process external cleanup alias. A
    /// parent retains its independent post-fork COW receipt and must gate every
    /// cleanup attempt on confirmed child reap.
    OOCRelationWriter(const std::string& base_path, OOCPrivateLeaseOwnershipReceipt&& private_lease,
                      PrivateLeaseMode private_lease_mode)
        : OOCRelationWriter(base_path, std::move(private_lease), private_lease_mode, {}) {}

    /// Trusted test seam for deferred construction boundaries. As with the
    /// production overload, the caller receipt is moved only after every
    /// throwing construction step has completed.
    OOCRelationWriter(const std::string& base_path, OOCPrivateLeaseOwnershipReceipt&& private_lease,
                      PrivateLeaseMode private_lease_mode,
                      OOCPrivateLeaseTestHooks private_lease_hooks)
        : OOCRelationWriter(base_path, std::nullopt, std::nullopt,
                            require_deferred_private_lease(private_lease, private_lease_mode),
                            private_lease_mode, private_lease_hooks, {}, {}, ConstructionToken{}) {
        owned_deferred_private_lease_.emplace(std::move(private_lease));
    }

    OOCRelationWriter(const std::string& base_path, OOCPrivateLeaseOwnershipReceipt& private_lease,
                      PrivateLeaseMode private_lease_mode) = delete;

    /// Trusted test seam for process termination inside fresh private-writer
    /// construction. Production callers use the overload above.
    OOCRelationWriter(const std::string& base_path, OOCPrivateLeaseOwnershipReceipt& private_lease,
                      OOCPrivateLeaseTestHooks private_lease_hooks)
        : OOCRelationWriter(base_path, std::nullopt, std::nullopt, &private_lease,
                            PrivateLeaseMode::ActivateImmediately, private_lease_hooks, {}, {},
                            ConstructionToken{}) {}

    /// Paired recovery requires both the structural descriptor and the
    /// semantic relation-sequence receipt from the same durable checkpoint.
    /// There is intentionally no descriptor-only recovery overload.
    OOCRelationWriter(const std::string& base_path,
                      const OOCSnapshotDescriptor& recovery_descriptor,
                      const RelationSequenceReceipt& recovery_sequence_receipt)
        : OOCRelationWriter(base_path, recovery_descriptor, recovery_sequence_receipt, nullptr,
                            PrivateLeaseMode::ActivateImmediately, {}, {}, {},
                            ConstructionToken{}) {}

    /// Trusted deterministic seam for replacement tests between exact-handle
    /// recovery validation and the first recovery mutation.
    OOCRelationWriter(const std::string& base_path,
                      const OOCSnapshotDescriptor& recovery_descriptor,
                      const RelationSequenceReceipt& recovery_sequence_receipt,
                      RecoveryTestHooks recovery_hooks)
        : OOCRelationWriter(base_path, recovery_descriptor, recovery_sequence_receipt, nullptr,
                            PrivateLeaseMode::ActivateImmediately, {}, recovery_hooks, {},
                            ConstructionToken{}) {}

private:
    OOCRelationWriter(const std::string& base_path, OOCPrivateLeaseOwnershipReceipt&& private_lease,
                      PrivateLeaseMode private_lease_mode,
                      OOCPrivateLeaseTestHooks private_lease_hooks,
                      ExactPrivateDirectoryBinding exact_private_directory,
                      ExactPrivateDirectoryConstructionToken)
        : OOCRelationWriter(base_path, std::nullopt, std::nullopt,
                            require_deferred_private_lease(private_lease, private_lease_mode),
                            private_lease_mode, private_lease_hooks, {},
                            std::move(exact_private_directory), ConstructionToken{}) {
        owned_deferred_private_lease_.emplace(std::move(private_lease));
    }

    [[nodiscard]] static OOCPrivateLeaseOwnershipReceipt*
    require_deferred_private_lease(OOCPrivateLeaseOwnershipReceipt& private_lease,
                                   PrivateLeaseMode private_lease_mode) {
        if (private_lease_mode != PrivateLeaseMode::DeferCleanupHandoff) {
            throw std::invalid_argument(
                "OOCRelationWriter: rvalue private lease is reserved for deferred handoff");
        }
        return &private_lease;
    }

    explicit OOCRelationWriter(
        const std::string& base_path, std::optional<OOCSnapshotDescriptor> recovery_descriptor,
        std::optional<RelationSequenceReceipt> recovery_sequence_receipt,
        OOCPrivateLeaseOwnershipReceipt* private_lease, PrivateLeaseMode private_lease_mode,
        OOCPrivateLeaseTestHooks private_lease_hooks, RecoveryTestHooks recovery_hooks,
        std::optional<ExactPrivateDirectoryBinding> exact_private_directory, ConstructionToken)
        : base_path_(freeze_base_path_checked(base_path)),
          exact_private_directory_(std::move(exact_private_directory)),
          uncaught_at_ctor_(std::uncaught_exceptions()),
          fresh_store_(!recovery_descriptor.has_value()) {
        if (recovery_sequence_receipt.has_value() != recovery_descriptor.has_value()) {
            throw std::logic_error(
                "OOCRelationWriter: internal recovery descriptor/receipt pairing violated");
        }
        if (recovery_hooks.stop_after != nullptr && !recovery_descriptor) {
            throw std::invalid_argument(
                "OOCRelationWriter: recovery hooks require paired recovery");
        }
        if (private_lease != nullptr && recovery_descriptor) {
            throw std::invalid_argument(
                "OOCRelationWriter: private lease is valid only for fresh creation");
        }
        if (private_lease_mode == PrivateLeaseMode::DeferCleanupHandoff &&
            private_lease == nullptr) {
            throw std::invalid_argument(
                "OOCRelationWriter: deferred cleanup handoff requires a private lease");
        }
        if (exact_private_directory_.has_value() &&
            (private_lease == nullptr || recovery_descriptor.has_value() ||
             private_lease_mode != PrivateLeaseMode::DeferCleanupHandoff)) {
            throw std::invalid_argument(
                "OOCRelationWriter: exact private directory requires a fresh deferred lease");
        }
        if (recovery_descriptor &&
            recovery_sequence_receipt->relation_count != recovery_descriptor->count) {
            throw std::invalid_argument(
                "OOCRelationWriter recovery: sequence receipt count differs from descriptor");
        }

        if (recovery_descriptor) {
            const auto observe_recovery_boundary = [&](RecoveryFaultPoint point) {
                if (recovery_hooks.stop_after != nullptr &&
                    recovery_hooks.stop_after(point, recovery_hooks.context)) {
                    throw std::system_error(
                        std::make_error_code(std::errc::operation_canceled),
                        "OOCRelationWriter: interrupted after exact recovery validation");
                }
            };
            validate_recovery_descriptor(*recovery_descriptor);
            // Resolve the pair exactly once. All structural validation,
            // record validation, truncation, metadata rollback, and later
            // append/finalize operations use these retained handles.
            data_stream_ = gnfs::util::NativeBinaryUpdateFile::open_existing(base_path + ".reldata",
                                                                             BUFFER_BYTES);
            idx_stream_ = gnfs::util::NativeBinaryUpdateFile::open_existing(base_path + ".relidx",
                                                                            BUFFER_BYTES / 4);
            require_store_named_identity("recovery open");
            const uint64_t existing_magic = read_open_index_magic();
            if (existing_magic == MAGIC_V1_FINAL) {
                throw std::runtime_error(
                    "OOCRelationWriter recovery: legacy finalized V1 store cannot verify identity");
            }
            if (existing_magic == MAGIC_V2_FINAL) {
                throw std::runtime_error("OOCRelationWriter recovery: finalized V2 store is "
                                         "read-only compatibility data");
            }
            if (existing_magic == MAGIC_V2_INCOMPLETE) {
                throw std::runtime_error("OOCRelationWriter recovery: incomplete V2 store cannot "
                                         "verify paired identity");
            }
            if (existing_magic == MAGIC_V3_FINAL) {
                // A clean pipeline end finalizes the corpus before deleting the
                // paired sieve checkpoint. A crash in that window must never
                // turn the immutable corpus back into an appendable one.
                validated_resume_prefix_ = validate_finalized_prefix(*recovery_descriptor);
                validate_recovery_receipt(*validated_resume_prefix_, *recovery_sequence_receipt);
                require_store_named_identity("finalized recovery validation");
                observe_recovery_boundary(RecoveryFaultPoint::FinalizedPrefixValidated);
                require_store_named_identity("finalized recovery boundary");
                store_id_ = recovery_descriptor->store_id;
                generation_ = recovery_descriptor->generation;
                count_ = static_cast<size_t>(validated_resume_prefix_->count);

                OOCSnapshotDescriptor final_descriptor = *recovery_descriptor;
                final_descriptor.count = validated_resume_prefix_->count;
                final_descriptor.data_end = validated_resume_prefix_->data_end;
                finalized_descriptor_ = final_descriptor;
                state_ = OOCWriterState::Finalized;
                finalized_durable_ = true;
                recovery_outcome_ = OOCRecoveryOutcome::FinalizedCorpus;
                return;
            }
            validated_resume_prefix_ = validate_resume_prefix(*recovery_descriptor);
            // The semantic receipt is part of the durable checkpoint. Compare
            // it while recovery is still read-only: a mismatch must not
            // truncate an uncommitted tail or rewrite incomplete metadata.
            validate_recovery_receipt(*validated_resume_prefix_, *recovery_sequence_receipt);
            require_store_named_identity("resume recovery validation");
            observe_recovery_boundary(RecoveryFaultPoint::AppendablePrefixValidated);

            const uint64_t recovered_index_size = index_size_for_count(recovery_descriptor->count);

            store_id_ = recovery_descriptor->store_id;
            generation_ = recovery_descriptor->generation;
            count_ = static_cast<size_t>(validated_resume_prefix_->count);

            // Validation above was performed through these exact handles.
            // Discard crash tails without resolving either pathname again.
            validate_open_v3_pair_headers("resume constructor header validation", std::nullopt);
            data_stream_.truncate(recovery_descriptor->data_end,
                                  "resume constructor data truncate");
            idx_stream_.truncate(recovered_index_size, "resume constructor index truncate");
            // The first append overwrites the committed sentinel slot.
            data_stream_.seek(validated_resume_prefix_->data_end, "resume constructor data seek");
            // A crash during finalize may have persisted a nonzero count under
            // INCOMPLETE magic. Roll it back before returning to append mode;
            // version and store_id remain immutable throughout.
            idx_stream_.seek(INDEX_COUNT_OFFSET, "resume constructor count seek");
            const uint64_t incomplete_count = 0;
            idx_stream_.write_exact(&incomplete_count, sizeof(incomplete_count),
                                    "resume constructor count rollback");
            idx_stream_.flush("resume constructor count rollback");

            const uint64_t idx_pos =
                INDEX_HEADER_BYTES + static_cast<uint64_t>(count_) * sizeof(uint64_t);
            idx_stream_.seek(idx_pos, "resume constructor index seek");
            require_store_named_identity("resume recovery commit");
            recovery_outcome_ = OOCRecoveryOutcome::AppendablePrefix;
        } else {
            try {
                // Serialize the complete namespace-empty check and both O_EXCL
                // reservations with cleanup/recovery callers. Pending,
                // canonical, staged, or quarantine leaves fail closed; explicit
                // transaction recovery must finish before fresh reuse.
                const auto cleanup_paths = ooc_cleanup_detail::freeze_paths(base_path_);
                require_exact_private_directory_binding(cleanup_paths, "fresh binding preflight");
                std::shared_ptr<ooc_cleanup_detail::BaseLock> cleanup_lock;
                if (private_lease != nullptr) {
                    if (private_lease->spent_ || private_lease->active_ ||
                        !private_lease->live_lock_ ||
                        private_lease->base_path_ != cleanup_paths.base_path ||
                        private_lease->lock_path_ != cleanup_paths.lock_path) {
                        throw std::logic_error(
                            "OOCRelationWriter: invalid private lease activation handoff");
                    }
                    cleanup_lock = private_lease->live_lock_;
                } else {
                    cleanup_lock = std::make_shared<ooc_cleanup_detail::BaseLock>(
                        cleanup_paths.lock_path, cleanup_paths.private_directory.empty());
                }
                std::shared_ptr<ooc_cleanup_detail::PrivateCleanupActionPermit> fresh_action_permit;
                if (private_lease != nullptr) {
                    auto admission = OOCCleanupTransaction::admit_private_fresh_writer_action(
                        *private_lease,
                        private_lease_mode == PrivateLeaseMode::DeferCleanupHandoff);
                    if (!admission.admitted()) {
                        const auto error = admission.result.native_error
                                               ? admission.result.native_error
                                               : std::make_error_code(std::errc::protocol_error);
                        throw std::system_error(
                            error, "OOCRelationWriter: private lease admission failed");
                    }
                    fresh_action_permit = std::move(admission.permit);
                } else {
                    if (!cleanup_paths.private_directory.empty()) {
                        // The recognized sink-lease layout is capability-gated.
                        // A persistent lock alone never permits directory
                        // inspection or pair creation without the exact
                        // move-only private-lease receipt.
                        ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict,
                                                 OOCCleanupStage::None,
                                                 ooc_cleanup_detail::protocol_error());
                    }
                    ooc_cleanup_detail::require_pair_namespace_reusable_locked(cleanup_paths);
                    cleanup_lock->require_stable();
                }

                // Reserve both fresh names with O_EXCL before opening either
                // stream. A second writer can therefore never pass an exists
                // check and then truncate this store.
                const std::string index_path = cleanup_paths.index_path.string();
                const std::string data_path = cleanup_paths.data_path.string();
                const std::string index_leaf = cleanup_paths.index_path.filename().string();
                const std::string data_leaf = cleanup_paths.data_path.filename().string();
                const auto create_reservation =
                    [&](const std::string& path,
                        const std::string& leaf) -> FreshArtifactReservation {
                    if (!exact_private_directory_) {
                        return FreshArtifactReservation::create(path);
                    }
                    require_exact_private_directory_binding(cleanup_paths,
                                                            "fresh reservation create");
                    return FreshArtifactReservation::create_at(
                        exact_private_directory_->directory_descriptor, leaf, path);
                };
                const auto require_reservation_identity =
                    [&](const FreshArtifactReservation& reservation, const std::string& path,
                        const std::string& leaf) {
                        if (exact_private_directory_) {
                            require_exact_private_directory_binding(cleanup_paths,
                                                                    "fresh reservation validation");
                            reservation.require_named_identity_at(
                                exact_private_directory_->directory_descriptor, leaf, path);
                        } else {
                            reservation.require_named_identity(path);
                        }
                    };
                std::optional<FreshArtifactReservation> index_reservation;
                std::optional<FreshArtifactReservation> data_reservation;
                bool index_creation_attempted = false;
                bool data_creation_attempted = false;
                try {
                    const auto require_fresh_boundary =
                        [&](OOCCleanupTransaction::PrivateFreshWriterBoundary boundary,
                            std::optional<std::array<std::uint64_t, 3>> index_identity =
                                std::nullopt,
                            std::optional<std::array<std::uint64_t, 3>> data_identity =
                                std::nullopt,
                            std::uint64_t store_id = 0) {
                            if (private_lease == nullptr || !fresh_action_permit) {
                                return;
                            }
                            const auto advanced =
                                OOCCleanupTransaction::advance_private_fresh_writer_action(
                                    *fresh_action_permit, *private_lease, boundary, index_identity,
                                    data_identity, store_id);
                            if (!advanced.completed()) {
                                const auto error =
                                    advanced.native_error
                                        ? advanced.native_error
                                        : std::make_error_code(std::errc::protocol_error);
                                throw std::system_error(
                                    error, "OOCRelationWriter: private writer phase gate failed");
                            }
                        };
                    if (private_lease != nullptr &&
                        ooc_cleanup_detail::invoke_with_stable_base_lock(*cleanup_lock, [&] {
                            return ooc_cleanup_detail::should_interrupt_private_lease(
                                private_lease_hooks,
                                OOCPrivateLeaseFaultPoint::FreshWriterPermitAcquired);
                        })) {
                        throw std::system_error(
                            std::make_error_code(std::errc::operation_canceled),
                            "OOCRelationWriter: interrupted after fresh writer admission");
                    }
                    require_fresh_boundary(
                        OOCCleanupTransaction::PrivateFreshWriterBoundary::BeforeIndexReservation);
                    index_creation_attempted = true;
                    index_reservation.emplace(ooc_cleanup_detail::invoke_with_stable_base_lock(
                        *cleanup_lock, [&] { return create_reservation(index_path, index_leaf); }));
                    require_reservation_identity(*index_reservation, index_path, index_leaf);
                    require_fresh_boundary(
                        OOCCleanupTransaction::PrivateFreshWriterBoundary::IndexReserved,
                        index_reservation->identity());
                    if (ooc_cleanup_detail::invoke_with_stable_base_lock(*cleanup_lock, [&] {
                            return ooc_cleanup_detail::should_interrupt_private_lease(
                                private_lease_hooks, OOCPrivateLeaseFaultPoint::FreshIndexReserved);
                        })) {
                        throw std::system_error(
                            std::make_error_code(std::errc::operation_canceled),
                            "OOCRelationWriter: interrupted after fresh index reservation");
                    }
                    require_fresh_boundary(
                        OOCCleanupTransaction::PrivateFreshWriterBoundary::BeforeDataReservation);
                    data_creation_attempted = true;
                    data_reservation.emplace(ooc_cleanup_detail::invoke_with_stable_base_lock(
                        *cleanup_lock, [&] { return create_reservation(data_path, data_leaf); }));
                    require_reservation_identity(*index_reservation, index_path, index_leaf);
                    require_reservation_identity(*data_reservation, data_path, data_leaf);
                    require_fresh_boundary(
                        OOCCleanupTransaction::PrivateFreshWriterBoundary::DataReserved,
                        index_reservation->identity(), data_reservation->identity());
                    if (ooc_cleanup_detail::invoke_with_stable_base_lock(*cleanup_lock, [&] {
                            return ooc_cleanup_detail::should_interrupt_private_lease(
                                private_lease_hooks, OOCPrivateLeaseFaultPoint::FreshDataReserved);
                        })) {
                        throw std::system_error(
                            std::make_error_code(std::errc::operation_canceled),
                            "OOCRelationWriter: interrupted after fresh data reservation");
                    }

                    require_fresh_boundary(
                        OOCCleanupTransaction::PrivateFreshWriterBoundary::BeforeHeaderWrite);
                    require_reservation_identity(*index_reservation, index_path, index_leaf);
                    require_reservation_identity(*data_reservation, data_path, data_leaf);
                    if (private_lease != nullptr &&
                        ooc_cleanup_detail::invoke_with_stable_base_lock(*cleanup_lock, [&] {
                            return ooc_cleanup_detail::should_interrupt_private_lease(
                                private_lease_hooks,
                                OOCPrivateLeaseFaultPoint::FreshHeaderWriteAuthorized);
                        })) {
                        throw std::system_error(
                            std::make_error_code(std::errc::operation_canceled),
                            "OOCRelationWriter: interrupted after header-write authorization");
                    }
                    ooc_cleanup_detail::invoke_with_stable_base_lock(*cleanup_lock, [&] {
                        data_stream_ =
                            data_reservation->duplicate_for_update(BUFFER_BYTES, data_path);
                        idx_stream_ =
                            index_reservation->duplicate_for_update(BUFFER_BYTES / 4, index_path);
                    });
                    if (private_lease != nullptr &&
                        ooc_cleanup_detail::invoke_with_stable_base_lock(*cleanup_lock, [&] {
                            return ooc_cleanup_detail::should_interrupt_private_lease(
                                private_lease_hooks,
                                OOCPrivateLeaseFaultPoint::FreshStreamsAttached);
                        })) {
                        throw std::system_error(
                            std::make_error_code(std::errc::operation_canceled),
                            "OOCRelationWriter: interrupted after stream attachment");
                    }
                    // 先写 INCOMPLETE 标志。若 write 中途抛(磁盘满等),析构跳过
                    // finalize → reader 看到 INCOMPLETE 拒读,避免 idx/data 不一致。
                    // 成功 close 后再翻成 MAGIC。
                    const uint64_t magic = MAGIC_INCOMPLETE;
                    const uint64_t format_version = FORMAT_VERSION;
                    const uint64_t durable_store_id = store_id_;
                    const uint64_t incomplete_count = 0;
                    ooc_cleanup_detail::invoke_with_stable_base_lock(*cleanup_lock, [&] {
                        idx_stream_.write_exact(&magic, sizeof(magic), "constructor index magic");
                        idx_stream_.write_exact(&format_version, sizeof(format_version),
                                                "constructor index version");
                        idx_stream_.write_exact(&durable_store_id, sizeof(durable_store_id),
                                                "constructor index store identity");
                        idx_stream_.write_exact(&incomplete_count, sizeof(incomplete_count),
                                                "constructor index count");
                        const uint64_t data_magic = MAGIC_V3_DATA;
                        data_stream_.write_exact(&data_magic, sizeof(data_magic),
                                                 "constructor data magic");
                        data_stream_.write_exact(&format_version, sizeof(format_version),
                                                 "constructor data version");
                        data_stream_.write_exact(&durable_store_id, sizeof(durable_store_id),
                                                 "constructor data store identity");
                        validate_open_v3_pair_headers("constructor header validation",
                                                      incomplete_count);
                    });
                    require_reservation_identity(*index_reservation, index_path, index_leaf);
                    require_reservation_identity(*data_reservation, data_path, data_leaf);
                    require_fresh_boundary(
                        OOCCleanupTransaction::PrivateFreshWriterBoundary::HeadersValidated,
                        index_reservation->identity(), data_reservation->identity(), store_id_);
                    if (ooc_cleanup_detail::invoke_with_stable_base_lock(*cleanup_lock, [&] {
                            return ooc_cleanup_detail::should_interrupt_private_lease(
                                private_lease_hooks,
                                OOCPrivateLeaseFaultPoint::FreshHeadersValidated);
                        })) {
                        throw std::system_error(
                            std::make_error_code(std::errc::operation_canceled),
                            "OOCRelationWriter: interrupted after fresh header validation");
                    }
                    require_reservation_identity(*index_reservation, index_path, index_leaf);
                    require_reservation_identity(*data_reservation, data_path, data_leaf);
                    ooc_cleanup_detail::invoke_with_stable_base_lock(*cleanup_lock, [&] {
                        data_reservation->close_checked(data_path);
                        index_reservation->close_checked(index_path);
                    });
                    require_store_named_identity("fresh reservation handoff");
                    auto cleanup_receipt =
                        ooc_cleanup_detail::invoke_with_stable_base_lock(*cleanup_lock, [&] {
                            require_exact_private_directory_binding(
                                cleanup_paths, "fresh cleanup ownership preflight");
                            auto receipt = capture_fresh_cleanup_ownership_checked(
                                base_path_, store_id_, index_reservation->identity(),
                                data_reservation->identity());
                            require_exact_private_directory_binding(
                                cleanup_paths, "fresh cleanup ownership commit");
                            return receipt;
                        });
                    require_fresh_boundary(
                        OOCCleanupTransaction::PrivateFreshWriterBoundary::PairOwnershipCaptured,
                        index_reservation->identity(), data_reservation->identity(), store_id_);
                    static_assert(std::is_nothrow_move_constructible_v<OOCCleanupOwnershipReceipt>);
                    cleanup_receipt_.emplace(std::move(cleanup_receipt));
                    if (private_lease != nullptr) {
                        if (ooc_cleanup_detail::invoke_with_stable_base_lock(*cleanup_lock, [&] {
                                return ooc_cleanup_detail::should_interrupt_private_lease(
                                    private_lease_hooks,
                                    OOCPrivateLeaseFaultPoint::FreshPairOwnershipCaptured);
                            })) {
                            throw std::system_error(
                                std::make_error_code(std::errc::operation_canceled),
                                "OOCRelationWriter: interrupted after fresh pair ownership");
                        }
                        require_fresh_boundary(
                            OOCCleanupTransaction::PrivateFreshWriterBoundary::Complete);
                        fresh_action_permit.reset();
                        if (private_lease_mode == PrivateLeaseMode::DeferCleanupHandoff) {
                            cleanup_lock->require_stable();
                        } else {
                            const auto activated =
                                OOCCleanupTransaction::activate_private_lease_for_fresh_writer(
                                    *private_lease, *cleanup_receipt_, private_lease_hooks);
                            if (!activated.completed()) {
                                const auto error =
                                    activated.native_error
                                        ? activated.native_error
                                        : std::make_error_code(std::errc::protocol_error);
                                throw std::system_error(
                                    error, "OOCRelationWriter: private lease activation failed");
                            }
                        }
                    }
                    cleanup_lock->require_stable();
                } catch (...) {
                    const std::exception_ptr primary_failure = std::current_exception();
                    abort_close_noexcept();
                    const bool preactive_pair_rollback =
                        private_lease == nullptr || !private_lease->active_;
                    const bool permit_allows_rollback =
                        private_lease == nullptr ||
                        (fresh_action_permit &&
                         OOCCleanupTransaction::private_fresh_writer_rollback_allowed(
                             *fresh_action_permit, *private_lease,
                             index_reservation ? std::optional(index_reservation->identity())
                                               : std::nullopt,
                             data_reservation ? std::optional(data_reservation->identity())
                                              : std::nullopt));
                    const bool lock_stable = cleanup_lock->stable_noexcept();
                    if (exact_private_directory_) {
                        bool named_residue_may_remain = false;
                        bool directory_durability_uncertain = false;
                        std::error_code rollback_error;
                        const auto note_error = [&](const std::error_code& error) {
                            if (!rollback_error && error) {
                                rollback_error = error;
                            }
                        };
                        const auto rollback_exact =
                            [&](bool creation_attempted,
                                std::optional<FreshArtifactReservation>& reservation,
                                const std::string& leaf) {
                                if (!creation_attempted) {
                                    return;
                                }
                                if (!reservation || !preactive_pair_rollback ||
                                    !permit_allows_rollback || !lock_stable) {
                                    named_residue_may_remain = true;
                                    note_error(
                                        std::make_error_code(std::errc::state_not_recoverable));
                                    return;
                                }
                                const auto removed =
                                    reservation->remove_path_if_same_identity_at_noexcept(
                                        exact_private_directory_->directory_descriptor, leaf);
                                if (!removed.absence_proven) {
                                    named_residue_may_remain = true;
                                }
                                note_error(removed.error);
                            };
                        rollback_exact(data_creation_attempted, data_reservation, data_leaf);
                        rollback_exact(index_creation_attempted, index_reservation, index_leaf);
                        if (index_creation_attempted || data_creation_attempted) {
                            try {
                                detail::sync_directory_descriptor_after_metadata_change(
                                    exact_private_directory_->directory_descriptor, base_path_);
                            } catch (const std::system_error& error) {
                                directory_durability_uncertain = true;
                                note_error(error.code());
                            } catch (...) {
                                directory_durability_uncertain = true;
                                note_error(std::make_error_code(std::errc::io_error));
                            }
                        }
                        const auto disposition =
                            named_residue_may_remain
                                ? detail::OOCExactFreshRollbackDisposition::NamedResidueMayRemain
                            : directory_durability_uncertain
                                ? detail::OOCExactFreshRollbackDisposition::
                                      DirectoryDurabilityUncertain
                                : detail::OOCExactFreshRollbackDisposition::Clean;
                        throw detail::OOCExactFreshConstructionFailure(primary_failure, disposition,
                                                                       rollback_error);
                    }
                    if (preactive_pair_rollback && permit_allows_rollback && data_reservation &&
                        lock_stable) {
                        data_reservation->remove_path_if_same_identity_noexcept(data_path);
                    }
                    if (preactive_pair_rollback && permit_allows_rollback && index_reservation &&
                        lock_stable) {
                        index_reservation->remove_path_if_same_identity_noexcept(index_path);
                    }
                    throw;
                }
            } catch (const ooc_cleanup_detail::Failure& failure) {
                const auto error =
                    failure.error ? failure.error : std::make_error_code(std::errc::protocol_error);
                throw std::system_error(error,
                                        "OOCRelationWriter: fresh namespace is not reusable");
            }
        }
    }

public:
    /// The historical bool overload remains only to fail closed at runtime.
    /// In particular, `true` throws before either store file is opened or
    /// truncated. Passing `false` is equivalent to fresh construction.
    explicit OOCRelationWriter(const std::string& base_path, bool legacy_resume)
        : OOCRelationWriter(base_path, reject_legacy_resume(legacy_resume), std::nullopt, nullptr,
                            PrivateLeaseMode::ActivateImmediately, {}, {}, {},
                            ConstructionToken{}) {}

    /// Append a single relation. Returns the index of the written relation.
    size_t write(const gnfs::core::Relation& rel) {
        require_state(OOCWriterState::Open, "write");

        // Caller/input errors do not poison an otherwise healthy writer. This
        // preflight happens before tellp(), index writes, data writes, or count
        // mutation, so a rejected relation leaves the writer Open and unchanged.
        rel.validate_persistence_limits();

        try {
            if (exact_append_batch_active_) {
                require_exact_append_batch_process("write authority preflight");
            } else if (exact_private_directory_) {
                require_store_named_identity("write authority preflight");
            }
            const uint64_t offset = data_stream_.position("write data offset");
            idx_stream_.write_exact(&offset, sizeof(offset), "write index offset");
            serialize(rel);

            ++count_;
            if (exact_append_batch_active_) {
                require_exact_append_batch_process("write authority commit");
            } else if (exact_private_directory_) {
                require_store_named_identity("write authority commit");
            }
            return count_ - 1;
        } catch (...) {
            fail_and_close_noexcept();
            throw;
        }
    }

    /// Flush a stable prefix and suspend append authority without flipping MAGIC.
    /// `resume_append()` must be called with the returned descriptor before the
    /// next write. The exact update handles remain retained while a trusted
    /// prefix reader mmaps the immutable committed extent.
    [[nodiscard]] OOCSnapshotDescriptor checkpoint_prefix() {
        require_state(OOCWriterState::Open, "checkpoint_prefix");
        try {
            require_store_named_identity("checkpoint_prefix preflight");
            validate_open_v3_pair_headers("checkpoint_prefix header validation", uint64_t{0});

            const uint64_t end_offset = data_stream_.position("checkpoint_prefix data end");

            // The next offset slot is a temporary end sentinel. A later append
            // overwrites this exact slot with the next relation's start offset.
            idx_stream_.write_exact(&end_offset, sizeof(end_offset), "checkpoint_prefix sentinel");
            idx_stream_.flush("checkpoint_prefix index flush");

            sync_store_files_and_directory();

            OOCSnapshotDescriptor descriptor;
            descriptor.format_version = FORMAT_VERSION;
            descriptor.store_id = store_id_;
            descriptor.generation = ++generation_;
            descriptor.count = static_cast<uint64_t>(count_);
            descriptor.data_end = end_offset;
            validate_exact_v3_pair(descriptor, MAGIC_V3_INCOMPLETE, uint64_t{0},
                                   OffsetValidation::BoundaryOnly, "checkpoint_prefix");
            suspended_descriptor_ = descriptor;
            state_ = OOCWriterState::Suspended;
            return descriptor;
        } catch (...) {
            fail_and_close_noexcept();
            throw;
        }
    }

    /// Reopen a prefix checkpoint for append. The descriptor must be the exact
    /// one returned by the immediately preceding `checkpoint_prefix()` call.
    void resume_append(const OOCSnapshotDescriptor& descriptor) {
        require_state(OOCWriterState::Suspended, "resume_append");
        if (!suspended_descriptor_ || descriptor != *suspended_descriptor_) {
            throw std::invalid_argument(
                "OOCRelationWriter::resume_append: stale or foreign descriptor");
        }
        if (active_prefix_readers_ != 0) {
            throw std::logic_error(
                "OOCRelationWriter::resume_append: snapshot reader is still active");
        }

        try {
            require_store_named_identity("resume_append preflight");
            validate_exact_v3_pair(descriptor, MAGIC_V3_INCOMPLETE, uint64_t{0},
                                   OffsetValidation::BoundaryOnly, "resume_append");
            validate_open_v3_pair_headers("resume_append retained header validation", uint64_t{0});

            data_stream_.seek(descriptor.data_end, "resume_append data seek");
            (void)index_size_for_count(descriptor.count);
            const uint64_t idx_pos = INDEX_HEADER_BYTES + descriptor.count * sizeof(uint64_t);
            idx_stream_.seek(idx_pos, "resume_append index seek");
            require_store_named_identity("resume_append commit");

            suspended_descriptor_.reset();
            state_ = OOCWriterState::Open;
        } catch (...) {
            fail_and_close_noexcept();
            throw;
        }
    }

    /// Finalize the store. This is the only operation that flips MAGIC.
    /// Idempotent: repeated calls return the same final descriptor.
    [[nodiscard]] OOCSnapshotDescriptor finalize(FinalizeStageHook hook = nullptr) {
        return finalize_impl(hook, false);
    }

private:
    [[nodiscard]] OOCSnapshotDescriptor finalize_impl(FinalizeStageHook hook,
                                                      bool retain_finalized_streams) {
        if (state_ == OOCWriterState::Finalized) {
            if (retain_finalized_streams && (!data_stream_.is_open() || !idx_stream_.is_open())) {
                throw std::logic_error(
                    "OOCRelationWriter::finalize: retained finalized handles are unavailable");
            }
            if (data_stream_.is_open() && idx_stream_.is_open()) {
                require_store_named_identity("finalize completed preflight");
            } else if (!finalized_durable_ && (data_stream_.is_open() || idx_stream_.is_open())) {
                throw std::logic_error(
                    "OOCRelationWriter::finalize: incomplete retained durability pair");
            }
            if (!finalized_durable_) {
                sync_store_files_and_directory();
                finalized_durable_ = true;
            }
            if (!retain_finalized_streams) {
                close_open_streams_checked("finalize durability retry close");
            }
            return *finalized_descriptor_;
        }
        if (state_ == OOCWriterState::Failed) {
            throw std::runtime_error("OOCRelationWriter::finalize: writer is in Failed state");
        }
        if (state_ == OOCWriterState::Suspended && active_prefix_readers_ != 0) {
            throw std::logic_error("OOCRelationWriter::finalize: snapshot reader is still active");
        }

        try {
            require_store_named_identity("finalize preflight");
            OOCSnapshotDescriptor descriptor;
            bool offsets_validated_before_metadata = false;
            if (state_ == OOCWriterState::Open) {
                validate_open_v3_pair_headers("finalize header validation", uint64_t{0});

                const uint64_t end_offset = data_stream_.position("finalize data end");
                idx_stream_.write_exact(&end_offset, sizeof(end_offset), "finalize sentinel");

                descriptor.format_version = FORMAT_VERSION;
                descriptor.generation = ++generation_;
                descriptor.store_id = store_id_;
                descriptor.count = static_cast<uint64_t>(count_);
                descriptor.data_end = end_offset;
            } else {
                // Suspended already has a flushed sentinel and retains the
                // exact handles with append authority logically disabled.
                descriptor = *suspended_descriptor_;
                validate_exact_v3_pair(descriptor, MAGIC_V3_INCOMPLETE, uint64_t{0},
                                       OffsetValidation::FullTable, "finalize suspended prefix");
                offsets_validated_before_metadata = true;
                validate_open_v3_pair_headers("finalize retained header validation", uint64_t{0});
            }

            // Commit payload metadata first while the file remains INCOMPLETE.
            // MAGIC is written and flushed last, so a failed finalize cannot
            // advertise a prefix whose sentinel/count were not persisted.
            idx_stream_.seek(INDEX_COUNT_OFFSET, "finalize count seek");
            const uint64_t final_count = descriptor.count;
            idx_stream_.write_exact(&final_count, sizeof(final_count), "finalize count write");

            if (data_stream_.is_open())
                data_stream_.flush("finalize data flush");
            idx_stream_.flush("finalize index flush");

            // Durably sync and close the full payload while magic remains
            // INCOMPLETE. A process exit after this point is recoverable from
            // the paired checkpoint because version/store_id are untouched.
            sync_store_files_and_directory();
            if (hook != nullptr) {
                hook(FinalizeStage::MetadataDurable);
            }

            validate_exact_v3_pair(descriptor, MAGIC_V3_INCOMPLETE, descriptor.count,
                                   offsets_validated_before_metadata
                                       ? OffsetValidation::BoundaryOnly
                                       : OffsetValidation::FullTable,
                                   "finalize precommit");
            validate_open_v3_pair_headers("finalize precommit retained validation",
                                          descriptor.count);
            require_store_named_identity("finalize authority before magic");
            idx_stream_.seek(0, "finalize magic seek");
            const uint64_t final_magic = MAGIC_V3_FINAL;
            idx_stream_.write_exact(&final_magic, sizeof(final_magic), "finalize magic write");
            idx_stream_.flush("finalize magic flush");

            // Once final magic has been flushed and both handles have closed,
            // the pair is visibly committed. Record that outcome before the
            // final durability barrier or observer hook: either can throw, but
            // must never relabel a readable finalized pair as a failed writer.
            suspended_descriptor_.reset();
            finalized_descriptor_ = descriptor;
            state_ = OOCWriterState::Finalized;
            sync_store_files_and_directory();
            finalized_durable_ = true;
            if (!retain_finalized_streams) {
                close_open_streams_checked("finalize close");
            }
            if (hook != nullptr) {
                hook(FinalizeStage::FinalMagicDurable);
            }

            return descriptor;
        } catch (...) {
            if (state_ != OOCWriterState::Finalized) {
                fail_and_close_noexcept();
            }
            throw;
        }
    }

public:
    /// Compatibility alias for existing callers.
    void close() {
        (void)finalize();
    }

    /// Abandon an appendable store without publishing final MAGIC.
    ///
    /// This transition is deliberately idempotent and noexcept so higher-level
    /// transactional owners can call it from failure paths and destructors.
    /// Finalized stores are immutable and remain finalized; Open or Suspended
    /// stores become Failed and all writer handles are closed.
    void abort() noexcept {
        if (exact_writer_process_changed_noexcept()) {
            discard_inherited_post_fork_child_noexcept();
            return;
        }
        exact_append_batch_active_ = false;
        if (state_ == OOCWriterState::Finalized) {
            return;
        }
        if (state_ != OOCWriterState::Failed) {
            state_ = OOCWriterState::Failed;
        }
        abort_close_noexcept();
    }

    /// Transfer this writer's unique cleanup authority after all append and
    /// prefix-reader handles have closed. Public recovery writers never carry
    /// this capability, and a writer can transfer it only once.
    [[nodiscard]] bool has_cleanup_ownership_receipt() const noexcept {
        return cleanup_receipt_.has_value() && !cleanup_receipt_->spent();
    }

    [[nodiscard]] OOCCleanupOwnershipReceipt take_cleanup_ownership_receipt() {
        if (state_ != OOCWriterState::Finalized && state_ != OOCWriterState::Failed) {
            throw std::logic_error("OOCRelationWriter: cleanup ownership requires a closed writer");
        }
        if (active_prefix_readers_ != 0) {
            throw std::logic_error(
                "OOCRelationWriter: cleanup ownership cannot outlive an active prefix reader");
        }
        if (deferred_private_lease_action_in_progress_) {
            throw std::logic_error(
                "OOCRelationWriter: cleanup ownership is retained by an active publication");
        }
        if (!cleanup_receipt_ || cleanup_receipt_->spent()) {
            throw std::logic_error(
                "OOCRelationWriter: cleanup ownership is unavailable or already consumed");
        }
        OOCCleanupOwnershipReceipt receipt(std::move(*cleanup_receipt_));
        cleanup_receipt_.reset();
        return receipt;
    }

    /// Return a deferred lease only after the writer is closed.
    ///
    /// This seam supports trusted relation-layer handoff assembly without
    /// restoring the former live-writer alias. While the writer is open the
    /// lease is deliberately unreachable and cannot authorize cleanup.
    [[nodiscard]] OOCPrivateLeaseOwnershipReceipt take_deferred_private_lease_ownership() {
        if (state_ != OOCWriterState::Finalized && state_ != OOCWriterState::Failed) {
            throw std::logic_error(
                "OOCRelationWriter: deferred lease ownership requires a closed writer");
        }
        if (active_prefix_readers_ != 0) {
            throw std::logic_error(
                "OOCRelationWriter: deferred lease cannot outlive an active prefix reader");
        }
        if (deferred_private_lease_action_in_progress_) {
            throw std::logic_error(
                "OOCRelationWriter: deferred lease is retained by an active publication");
        }
        if (!owned_deferred_private_lease_ || owned_deferred_private_lease_->spent()) {
            throw std::logic_error(
                "OOCRelationWriter: deferred lease is unavailable or already consumed");
        }
        OOCPrivateLeaseOwnershipReceipt receipt(std::move(*owned_deferred_private_lease_));
        owned_deferred_private_lease_.reset();
        return receipt;
    }

    [[nodiscard]] bool has_deferred_private_lease_ownership() const noexcept {
        return owned_deferred_private_lease_.has_value() && !owned_deferred_private_lease_->spent();
    }

    /// Finalize a deferred private-lease writer and publish only the canonical
    /// cleanup intent. The live pair remains readable, RESERVED remains
    /// durable, and the inherited lease lock stays held for the parent. A
    /// parent that survives the worker can read the pair and then converge the
    /// intent through its own fork copy of the lease receipt; a worker crash
    /// before canonical publication remains a RESERVED-authorized rollback.
    [[nodiscard]] OOCSnapshotDescriptor
    finalize_and_publish_cleanup_handoff(OOCCleanupTestHooks hooks = {}) {
        if (!owned_deferred_private_lease_) {
            throw std::logic_error(
                "OOCRelationWriter: cleanup handoff requires deferred private-lease mode");
        }
        if (!cleanup_receipt_ || cleanup_receipt_->spent()) {
            throw std::logic_error("OOCRelationWriter: cleanup handoff ownership is unavailable");
        }
        DeferredPrivateLeaseActionGuard action_guard(*this);

        const auto preflight = OOCCleanupTransaction::preflight_private_lease_cleanup_handoff(
            *owned_deferred_private_lease_);
        if (!preflight.completed()) {
            const auto error = preflight.native_error
                                   ? preflight.native_error
                                   : std::make_error_code(std::errc::protocol_error);
            throw std::system_error(error, "OOCRelationWriter: cleanup handoff preflight failed");
        }

        const OOCSnapshotDescriptor descriptor = finalize();
        if (descriptor.store_id != store_id_) {
            throw std::runtime_error(
                "OOCRelationWriter: cleanup handoff descriptor identity mismatch");
        }
        const OOCExactCleanupExpectation exact{
            .index_magic = MAGIC_V3_FINAL,
            .persisted_count = descriptor.count,
            .index_size = index_size_for_count(descriptor.count),
            .data_size = descriptor.data_end,
        };
        OOCCleanupOwnershipReceipt escrow(std::move(*cleanup_receipt_));
        cleanup_receipt_.reset();
        const auto result = OOCCleanupTransaction::publish_private_lease_cleanup_handoff(
            escrow, *owned_deferred_private_lease_, exact, hooks);
        if (!escrow.spent()) {
            cleanup_receipt_.emplace(std::move(escrow));
        }
        if (!result.completed() || result.stage != OOCCleanupStage::IntentDurable) {
            const auto error = result.native_error
                                   ? result.native_error
                                   : std::make_error_code(std::errc::protocol_error);
            throw std::system_error(error, "OOCRelationWriter: cleanup handoff publication failed");
        }
        return descriptor;
    }

    /// Finalize a deferred private-lease writer and publish an immutable,
    /// application-bound no-delete handoff. The generic relation layer binds
    /// the opaque payload to the exact lease generation and finalized pair,
    /// revokes preactivation rollback, and consumes this writer's fresh cleanup
    /// receipt. The handoff itself grants read/adoption eligibility only; it
    /// does not publish cleanup intent or authorize deletion.
    [[nodiscard]] OOCSnapshotDescriptor
    finalize_and_publish_private_handoff(std::uint32_t payload_kind, std::uint32_t payload_version,
                                         std::span<const std::byte> opaque_payload,
                                         OOCPrivateHandoffTestHooks hooks = {}) {
        if (!owned_deferred_private_lease_) {
            throw std::logic_error(
                "OOCRelationWriter: private handoff requires deferred private-lease mode");
        }
        if (!cleanup_receipt_ || cleanup_receipt_->spent()) {
            throw std::logic_error("OOCRelationWriter: private handoff ownership is unavailable");
        }
        DeferredPrivateLeaseActionGuard action_guard(*this);
        if (payload_kind == 0 || payload_version == 0 ||
            opaque_payload.size() > OOC_PRIVATE_HANDOFF_MAX_OPAQUE_PAYLOAD_BYTES) {
            throw std::invalid_argument(
                "OOCRelationWriter: private handoff payload contract is invalid");
        }

        const OOCSnapshotDescriptor descriptor = finalize();
        if (descriptor.store_id != store_id_) {
            throw std::runtime_error(
                "OOCRelationWriter: private handoff descriptor identity mismatch");
        }
        const OOCPrivateHandoffPairDescriptorV1 pair{
            .format_version = descriptor.format_version,
            .store_id = descriptor.store_id,
            .generation = descriptor.generation,
            .count = descriptor.count,
            .index_extent = index_size_for_count(descriptor.count),
            .data_extent = descriptor.data_end,
        };
        const auto result = OOCCleanupTransaction::publish_private_handoff(
            *cleanup_receipt_, *owned_deferred_private_lease_, pair, payload_kind, payload_version,
            opaque_payload, hooks);
        if (!result.canonical()) {
            const auto error = result.result.native_error
                                   ? result.result.native_error
                                   : std::make_error_code(std::errc::protocol_error);
            throw std::system_error(error, "OOCRelationWriter: private handoff publication failed");
        }
        return descriptor;
    }

    /// Close and transactionally remove the exact pair authorized by this
    /// writer's move-only cleanup receipt.
    ///
    /// Fresh construction is the only public receipt issuer. Descriptor-based
    /// recovery deliberately has no receipt because structural and sequence
    /// validation prove byte consistency, not path ownership. Once a durable
    /// cleanup intent exists, retries resume it using the receipt-bound
    /// request; no path/store-id pair can create a new intent by itself.
    [[nodiscard]] OOCCleanupResult remove_owned_artifacts_noexcept() noexcept {
        if (fresh_artifacts_removed_) {
            return OOCCleanupResult{
                .status = OOCCleanupStatus::Completed,
                .stage = OOCCleanupStage::Completed,
                .native_error = {},
            };
        }
        if (!cleanup_receipt_) {
            return OOCCleanupResult{
                .status = OOCCleanupStatus::InvalidRequest,
                .stage = OOCCleanupStage::None,
                .native_error = std::make_error_code(std::errc::operation_not_permitted),
            };
        }
        if (active_prefix_readers_ != 0) {
            return OOCCleanupResult{
                .status = OOCCleanupStatus::Busy,
                .stage = OOCCleanupStage::None,
                .native_error = std::make_error_code(std::errc::device_or_resource_busy),
            };
        }

        if (owned_deferred_private_lease_) {
            // A fork worker must never mutate or unlock the parent-owned lease.
            // Its only destructive-capability transition is canonical intent
            // publication; the parent alone converges that intent or rolls
            // back a failed preactive generation.
            return OOCCleanupResult{
                .status = OOCCleanupStatus::InvalidRequest,
                .stage = OOCCleanupStage::None,
                .native_error = std::make_error_code(std::errc::operation_not_permitted),
            };
        }

        try {
            std::optional<OOCExactCleanupExpectation> exact;
            if (state_ == OOCWriterState::Finalized) {
                if (!finalized_descriptor_ || finalized_descriptor_->store_id != store_id_) {
                    return OOCCleanupResult{
                        .status = OOCCleanupStatus::SourcePairInvalid,
                        .stage = OOCCleanupStage::None,
                        .native_error = std::make_error_code(std::errc::protocol_error),
                    };
                }
                exact = OOCExactCleanupExpectation{
                    .index_magic = MAGIC_V3_FINAL,
                    .persisted_count = finalized_descriptor_->count,
                    .index_size = index_size_for_count(finalized_descriptor_->count),
                    .data_size = finalized_descriptor_->data_end,
                };
            } else {
                abort();
            }

            const OOCCleanupRequest request{
                .base_path = cleanup_receipt_->base_path_,
                .store_id = cleanup_receipt_->store_id_,
                .exact = exact,
            };
            OOCCleanupResult result;
            if (cleanup_receipt_->spent()) {
                result = OOCCleanupTransaction::resume(request);
            } else {
                result = OOCCleanupTransaction::begin_or_resume(*cleanup_receipt_, exact);
            }
            if (result.status == OOCCleanupStatus::NoTransaction) {
                result = OOCCleanupTransaction::confirm_pair_namespace_reusable(
                    cleanup_receipt_->base_path_);
            }
            if (result.completed()) {
                fresh_artifacts_removed_ = true;
            }
            return result;
        } catch (...) {
            return OOCCleanupResult{
                .status = OOCCleanupStatus::UnexpectedFailure,
                .stage = OOCCleanupStage::None,
                .native_error = {},
            };
        }
    }

    /// Compatibility wrapper for exception-only fresh-store cleanup.
    [[nodiscard]] bool abort_and_remove_owned_fresh_artifacts_noexcept() noexcept {
        if (!fresh_store_) {
            return true;
        }
        return remove_owned_artifacts_noexcept().completed();
    }

    ~OOCRelationWriter() {
        if (state_ == OOCWriterState::Open || state_ == OOCWriterState::Suspended) {
            if (std::uncaught_exceptions() > uncaught_at_ctor_) {
                abort_close_noexcept();
                return;
            }
            try {
                (void)finalize();
            } catch (...) {
                abort_close_noexcept();
            }
        } else {
            abort_close_noexcept();
        }
    }

    [[nodiscard]] size_t count() const noexcept {
        return count_;
    }
    [[nodiscard]] const std::string& base_path() const noexcept {
        return base_path_;
    }
    [[nodiscard]] OOCWriterState state() const noexcept {
        return state_;
    }
    [[nodiscard]] uint64_t store_id() const noexcept {
        return store_id_;
    }
    [[nodiscard]] OOCRecoveryOutcome recovery_outcome() const noexcept {
        return recovery_outcome_;
    }
    [[nodiscard]] std::optional<OOCValidatedResumePrefix> take_validated_resume_prefix() noexcept {
        auto result = std::move(validated_resume_prefix_);
        validated_resume_prefix_.reset();
        return result;
    }

    /// Mark a suspended snapshot as unusable after an integrity/format failure.
    /// This narrow transition lets the collector distinguish recoverable
    /// materialization failures from a prefix that must never be reopened.
    void fail_suspended_snapshot() {
        require_state(OOCWriterState::Suspended, "fail_suspended_snapshot");
        fail_and_close_noexcept();
    }

    [[nodiscard]] bool
    owns_suspended_prefix(const OOCSnapshotDescriptor& descriptor) const noexcept {
        return state_ == OOCWriterState::Suspended && suspended_descriptor_.has_value() &&
               descriptor == *suspended_descriptor_;
    }

private:
    enum class OffsetValidation {
        BoundaryOnly,
        FullTable,
    };

    [[nodiscard]] static std::string freeze_base_path_checked(const std::string& base_path) {
        try {
            return ooc_cleanup_detail::freeze_paths(base_path).base_path.string();
        } catch (const ooc_cleanup_detail::Failure& failure) {
            const auto error =
                failure.error ? failure.error : std::make_error_code(std::errc::invalid_argument);
            throw std::system_error(error, "OOCRelationWriter: invalid base path");
        }
    }

    void require_exact_private_directory_binding(const OOCCleanupPaths& paths,
                                                 const char* operation) const {
        if (!exact_private_directory_) {
            return;
        }
#ifdef _WIN32
        (void)paths;
        throw std::system_error(std::make_error_code(std::errc::operation_not_supported),
                                std::string("OOCRelationWriter::") + operation +
                                    ": exact private directory is unsupported");
#else
        const auto& exact = *exact_private_directory_;
        if (exact.root_descriptor < 0 || exact.directory_descriptor < 0 ||
            exact.creator_process_id == 0 ||
            exact.creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
            exact.directory_leaf.empty() || exact.index_leaf.empty() || exact.data_leaf.empty() ||
            paths.private_directory.empty() ||
            paths.private_directory.filename().string() != exact.directory_leaf ||
            paths.index_path.filename().string() != exact.index_leaf ||
            paths.data_path.filename().string() != exact.data_leaf) {
            throw std::logic_error(std::string("OOCRelationWriter::") + operation +
                                   ": invalid exact private-directory binding");
        }

        int root_descriptor_flags = -1;
        do {
            root_descriptor_flags = ::fcntl(exact.root_descriptor, F_GETFD);
        } while (root_descriptor_flags < 0 && errno == EINTR);
        int directory_descriptor_flags = -1;
        do {
            directory_descriptor_flags = ::fcntl(exact.directory_descriptor, F_GETFD);
        } while (directory_descriptor_flags < 0 && errno == EINTR);
        int root_status_flags = -1;
        do {
            root_status_flags = ::fcntl(exact.root_descriptor, F_GETFL);
        } while (root_status_flags < 0 && errno == EINTR);
        int directory_status_flags = -1;
        do {
            directory_status_flags = ::fcntl(exact.directory_descriptor, F_GETFL);
        } while (directory_status_flags < 0 && errno == EINTR);
        if (root_descriptor_flags < 0 || directory_descriptor_flags < 0 || root_status_flags < 0 ||
            directory_status_flags < 0 || (root_descriptor_flags & FD_CLOEXEC) == 0 ||
            (directory_descriptor_flags & FD_CLOEXEC) == 0 ||
            (root_status_flags & O_ACCMODE) != O_RDONLY ||
            (directory_status_flags & O_ACCMODE) != O_RDONLY) {
            throw std::system_error(errno == 0 ? EACCES : errno, std::generic_category(),
                                    std::string("OOCRelationWriter::") + operation +
                                        ": exact directory descriptor policy changed");
        }

        struct stat root {};
        int root_result = -1;
        do {
            root_result = ::fstat(exact.root_descriptor, &root);
        } while (root_result != 0 && errno == EINTR);
        struct stat held_directory {};
        int held_directory_result = -1;
        do {
            held_directory_result = ::fstat(exact.directory_descriptor, &held_directory);
        } while (held_directory_result != 0 && errno == EINTR);
        struct stat named_directory {};
        int named_directory_result = -1;
        do {
            named_directory_result = ::fstatat(exact.root_descriptor, exact.directory_leaf.c_str(),
                                               &named_directory, AT_SYMLINK_NOFOLLOW);
        } while (named_directory_result != 0 && errno == EINTR);
        if (root_result != 0 || held_directory_result != 0 || named_directory_result != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    std::string("OOCRelationWriter::") + operation +
                                        ": cannot inspect exact private directory");
        }
        const auto identity_for = [](const struct stat& metadata) noexcept {
            return std::array<std::uint64_t, 3>{
                static_cast<std::uint64_t>(metadata.st_dev),
                static_cast<std::uint64_t>(metadata.st_ino),
                0,
            };
        };
        const auto owner_directory = [](const struct stat& metadata) noexcept {
            return S_ISDIR(metadata.st_mode) &&
                   (metadata.st_mode & static_cast<mode_t>(07777)) == 0700 &&
                   metadata.st_uid == ::geteuid();
        };
        if (!owner_directory(root) || !owner_directory(held_directory) ||
            !owner_directory(named_directory) || identity_for(root) != exact.root_identity ||
            identity_for(held_directory) != exact.directory_identity ||
            identity_for(named_directory) != exact.directory_identity) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": exact private directory identity changed");
        }
        if (exact.authority_context == nullptr || exact.authority_stable == nullptr ||
            !exact.authority_stable(exact.authority_context)) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": inherited worker authority changed");
        }
#endif
    }

    [[nodiscard]] static OOCCleanupOwnershipReceipt capture_fresh_cleanup_ownership_checked(
        const std::string& base_path, std::uint64_t store_id,
        const std::array<std::uint64_t, 3>& expected_index_identity,
        const std::array<std::uint64_t, 3>& expected_data_identity) {
        try {
            return OOCCleanupTransaction::capture_fresh_ownership_receipt(
                base_path, store_id, expected_index_identity, expected_data_identity);
        } catch (const ooc_cleanup_detail::Failure& failure) {
            const auto error =
                failure.error ? failure.error : std::make_error_code(std::errc::protocol_error);
            throw std::system_error(error,
                                    "OOCRelationWriter: cannot issue fresh cleanup ownership");
        }
    }

    class FreshArtifactReservation final {
    public:
        struct ExactRemovalResult final {
            bool absence_proven = false;
            std::error_code error;
        };

        FreshArtifactReservation(const FreshArtifactReservation&) = delete;
        FreshArtifactReservation& operator=(const FreshArtifactReservation&) = delete;

        FreshArtifactReservation(FreshArtifactReservation&& other) noexcept
            : identity_(other.identity_)
#ifdef _WIN32
              ,
              handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE))
#else
              ,
              descriptor_(std::exchange(other.descriptor_, -1))
#endif
        {
        }

        FreshArtifactReservation& operator=(FreshArtifactReservation&&) = delete;

        ~FreshArtifactReservation() {
            close_noexcept();
        }

        [[nodiscard]] static FreshArtifactReservation create(const std::string& path) {
#ifdef _WIN32
            const std::filesystem::path filesystem_path(path);
            const HANDLE handle = ::CreateFileW(
                filesystem_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                        "OOCRelationWriter: cannot reserve fresh artifact " + path);
            }
            BY_HANDLE_FILE_INFORMATION information{};
            if (!::GetFileInformationByHandle(handle, &information)) {
                const DWORD error = ::GetLastError();
                (void)::CloseHandle(handle);
                throw std::system_error(static_cast<int>(error), std::system_category(),
                                        "OOCRelationWriter: cannot identify fresh artifact " +
                                            path);
            }
            const auto identity = ooc_cleanup_detail::windows_identity(handle, information);
            if (!identity || !ooc_cleanup_detail::windows_regular_single_link(information)) {
                (void)::CloseHandle(handle);
                throw std::runtime_error(
                    "OOCRelationWriter: fresh artifact has no stable regular-file identity");
            }
            return FreshArtifactReservation({identity->first, identity->second, identity->third},
                                            handle);
#else
            int descriptor = -1;
            do {
                descriptor = ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
            } while (descriptor < 0 && errno == EINTR);
            if (descriptor < 0) {
                throw std::system_error(errno, std::generic_category(),
                                        "OOCRelationWriter: cannot reserve fresh artifact " + path);
            }
            struct stat information {};
            int inspected = -1;
            do {
                inspected = ::fstat(descriptor, &information);
            } while (inspected != 0 && errno == EINTR);
            if (inspected != 0) {
                const int error = errno;
                (void)::close(descriptor);
                throw std::system_error(error, std::generic_category(),
                                        "OOCRelationWriter: cannot identify fresh artifact " +
                                            path);
            }
            if (!S_ISREG(information.st_mode) || information.st_nlink != 1) {
                (void)::close(descriptor);
                throw std::runtime_error(
                    "OOCRelationWriter: fresh artifact has no stable regular-file identity");
            }
            return FreshArtifactReservation({static_cast<std::uint64_t>(information.st_dev),
                                             static_cast<std::uint64_t>(information.st_ino), 0},
                                            descriptor);
#endif
        }

        [[nodiscard]] static FreshArtifactReservation
        create_at(int parent_descriptor, const std::string& leaf, const std::string& label) {
#ifdef _WIN32
            (void)parent_descriptor;
            (void)leaf;
            throw std::system_error(
                std::make_error_code(std::errc::operation_not_supported),
                "OOCRelationWriter: handle-relative fresh reservation is unsupported for " + label);
#else
            if (parent_descriptor < 0 || leaf.empty() || leaf.find('/') != std::string::npos ||
                leaf.find('\0') != std::string::npos) {
                throw std::invalid_argument(
                    "OOCRelationWriter: invalid handle-relative fresh artifact " + label);
            }
            int descriptor = -1;
            do {
                descriptor = ::openat(parent_descriptor, leaf.c_str(),
                                      O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
            } while (descriptor < 0 && errno == EINTR);
            if (descriptor < 0) {
                throw std::system_error(errno, std::generic_category(),
                                        "OOCRelationWriter: cannot reserve exact fresh artifact " +
                                            label);
            }
            struct stat information {};
            int inspected = -1;
            do {
                inspected = ::fstat(descriptor, &information);
            } while (inspected != 0 && errno == EINTR);
            if (inspected != 0) {
                const int error = errno;
                (void)::close(descriptor);
                throw std::system_error(error, std::generic_category(),
                                        "OOCRelationWriter: cannot identify exact fresh artifact " +
                                            label);
            }
            return FreshArtifactReservation({static_cast<std::uint64_t>(information.st_dev),
                                             static_cast<std::uint64_t>(information.st_ino), 0},
                                            descriptor);
#endif
        }

        void close_checked(const std::string& path) {
#ifdef _WIN32
            if (handle_ == INVALID_HANDLE_VALUE) {
                return;
            }
            const HANDLE handle = std::exchange(handle_, INVALID_HANDLE_VALUE);
            if (!::CloseHandle(handle)) {
                throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                        "OOCRelationWriter: cannot close fresh artifact " + path);
            }
#else
            if (descriptor_ < 0) {
                return;
            }
            const int descriptor = std::exchange(descriptor_, -1);
            if (::close(descriptor) != 0) {
                throw std::system_error(errno, std::generic_category(),
                                        "OOCRelationWriter: cannot close fresh artifact " + path);
            }
#endif
        }

        [[nodiscard]] const std::array<std::uint64_t, 3>& identity() const noexcept {
            return identity_;
        }

        [[nodiscard]] gnfs::util::NativeBinaryUpdateFile
        duplicate_for_update(std::size_t buffer_bytes, const std::string& path) const {
#ifdef _WIN32
            if (handle_ == INVALID_HANDLE_VALUE) {
                throw std::logic_error(
                    "OOCRelationWriter: fresh artifact handle is already closed");
            }
            return gnfs::util::NativeBinaryUpdateFile::duplicate_from(handle_, buffer_bytes, path);
#else
            if (descriptor_ < 0) {
                throw std::logic_error(
                    "OOCRelationWriter: fresh artifact descriptor is already closed");
            }
            return gnfs::util::NativeBinaryUpdateFile::duplicate_from(descriptor_, buffer_bytes,
                                                                      path);
#endif
        }

        void require_named_identity(const std::string& path) const {
#ifdef _WIN32
            if (handle_ == INVALID_HANDLE_VALUE) {
                throw std::logic_error(
                    "OOCRelationWriter: fresh artifact handle is already closed");
            }
            BY_HANDLE_FILE_INFORMATION held{};
            if (!::GetFileInformationByHandle(handle_, &held) ||
                !ooc_cleanup_detail::windows_regular_single_link(held)) {
                throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                        "OOCRelationWriter: cannot revalidate fresh handle " +
                                            path);
            }
            const auto held_identity = ooc_cleanup_detail::windows_identity(handle_, held);
            const auto named =
                ooc_cleanup_detail::inspect_file(std::filesystem::path(path), 0, false);
            if (!held_identity || named.kind != ooc_cleanup_detail::InspectKind::Present ||
                ooc_cleanup_detail::stable_identity(*held_identity) != identity_ ||
                ooc_cleanup_detail::stable_identity(named.identity) != identity_) {
                throw std::runtime_error(
                    "OOCRelationWriter: fresh artifact path no longer names held handle");
            }
#else
            if (descriptor_ < 0) {
                throw std::logic_error(
                    "OOCRelationWriter: fresh artifact descriptor is already closed");
            }
            struct stat held {};
            struct stat named {};
            int held_result = -1;
            do {
                held_result = ::fstat(descriptor_, &held);
            } while (held_result != 0 && errno == EINTR);
            int named_result = -1;
            do {
                named_result = ::lstat(std::filesystem::path(path).c_str(), &named);
            } while (named_result != 0 && errno == EINTR);
            if (held_result != 0 || named_result != 0 || !S_ISREG(held.st_mode) ||
                !S_ISREG(named.st_mode) || held.st_nlink != 1 || named.st_nlink != 1 ||
                held.st_dev != named.st_dev || held.st_ino != named.st_ino ||
                identity_ != std::array<std::uint64_t, 3>{
                                 static_cast<std::uint64_t>(held.st_dev),
                                 static_cast<std::uint64_t>(held.st_ino),
                                 0,
                             }) {
                throw std::runtime_error(
                    "OOCRelationWriter: fresh artifact path no longer names held descriptor");
            }
#endif
        }

        void require_named_identity_at(int parent_descriptor, const std::string& leaf,
                                       const std::string& label) const {
#ifdef _WIN32
            (void)parent_descriptor;
            (void)leaf;
            throw std::system_error(
                std::make_error_code(std::errc::operation_not_supported),
                "OOCRelationWriter: handle-relative reservation validation is unsupported for " +
                    label);
#else
            if (descriptor_ < 0) {
                throw std::logic_error(
                    "OOCRelationWriter: exact fresh artifact descriptor is already closed");
            }
            struct stat held {};
            struct stat named {};
            int held_result = -1;
            do {
                held_result = ::fstat(descriptor_, &held);
            } while (held_result != 0 && errno == EINTR);
            int named_result = -1;
            do {
                named_result =
                    ::fstatat(parent_descriptor, leaf.c_str(), &named, AT_SYMLINK_NOFOLLOW);
            } while (named_result != 0 && errno == EINTR);
            if (held_result != 0 || named_result != 0 || !S_ISREG(held.st_mode) ||
                !S_ISREG(named.st_mode) || held.st_nlink != 1 || named.st_nlink != 1 ||
                (held.st_mode & static_cast<mode_t>(07777)) != 0600 ||
                (named.st_mode & static_cast<mode_t>(07777)) != 0600 ||
                held.st_uid != ::geteuid() || named.st_uid != ::geteuid() ||
                held.st_dev != named.st_dev || held.st_ino != named.st_ino ||
                identity_ != std::array<std::uint64_t, 3>{
                                 static_cast<std::uint64_t>(held.st_dev),
                                 static_cast<std::uint64_t>(held.st_ino),
                                 0,
                             }) {
                throw std::runtime_error(
                    "OOCRelationWriter: exact fresh artifact leaf changed for " + label);
            }
#endif
        }

        void remove_path_if_same_identity_noexcept(const std::string& path) noexcept {
            try {
                const auto inspected =
                    ooc_cleanup_detail::inspect_file(std::filesystem::path(path), 0, false);
                if (inspected.kind != ooc_cleanup_detail::InspectKind::Present ||
                    ooc_cleanup_detail::stable_identity(inspected.identity) != identity_) {
                    return;
                }
                std::error_code ignored;
                (void)std::filesystem::remove(path, ignored);
            } catch (...) {
            }
        }

        [[nodiscard]] ExactRemovalResult
        remove_path_if_same_identity_at_noexcept(int parent_descriptor,
                                                 const std::string& leaf) noexcept {
#ifndef _WIN32
            if (parent_descriptor < 0 || leaf.empty()) {
                return {
                    .absence_proven = false,
                    .error = std::make_error_code(std::errc::invalid_argument),
                };
            }
            struct stat named {};
            int inspected = -1;
            do {
                inspected = ::fstatat(parent_descriptor, leaf.c_str(), &named, AT_SYMLINK_NOFOLLOW);
            } while (inspected != 0 && errno == EINTR);
            if (inspected != 0) {
                if (errno == ENOENT) {
                    return {.absence_proven = true, .error = {}};
                }
                return {
                    .absence_proven = false,
                    .error = std::error_code(errno, std::generic_category()),
                };
            }
            if (!S_ISREG(named.st_mode) || named.st_nlink != 1 ||
                identity_ != std::array<std::uint64_t, 3>{
                                 static_cast<std::uint64_t>(named.st_dev),
                                 static_cast<std::uint64_t>(named.st_ino),
                                 0,
                             }) {
                return {
                    .absence_proven = false,
                    .error = std::make_error_code(std::errc::state_not_recoverable),
                };
            }
            int removed = -1;
            do {
                removed = ::unlinkat(parent_descriptor, leaf.c_str(), 0);
            } while (removed != 0 && errno == EINTR);
            if (removed != 0 && errno != ENOENT) {
                return {
                    .absence_proven = false,
                    .error = std::error_code(errno, std::generic_category()),
                };
            }
            do {
                inspected = ::fstatat(parent_descriptor, leaf.c_str(), &named, AT_SYMLINK_NOFOLLOW);
            } while (inspected != 0 && errno == EINTR);
            if (inspected != 0 && errno == ENOENT) {
                return {.absence_proven = true, .error = {}};
            }
            return {
                .absence_proven = false,
                .error = inspected == 0 ? std::make_error_code(std::errc::state_not_recoverable)
                                        : std::error_code(errno, std::generic_category()),
            };
#else
            (void)parent_descriptor;
            (void)leaf;
            return {
                .absence_proven = false,
                .error = std::make_error_code(std::errc::operation_not_supported),
            };
#endif
        }

    private:
#ifdef _WIN32
        FreshArtifactReservation(std::array<std::uint64_t, 3> identity, HANDLE handle) noexcept
            : identity_(identity), handle_(handle) {}
#else
        FreshArtifactReservation(std::array<std::uint64_t, 3> identity, int descriptor) noexcept
            : identity_(identity), descriptor_(descriptor) {}
#endif

        void close_noexcept() noexcept {
#ifdef _WIN32
            if (handle_ != INVALID_HANDLE_VALUE) {
                (void)::CloseHandle(std::exchange(handle_, INVALID_HANDLE_VALUE));
            }
#else
            if (descriptor_ >= 0) {
                (void)::close(std::exchange(descriptor_, -1));
            }
#endif
        }

        std::array<std::uint64_t, 3> identity_{};
#ifdef _WIN32
        HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
        int descriptor_ = -1;
#endif
    };

    static std::optional<OOCSnapshotDescriptor> reject_legacy_resume(bool legacy_resume) {
        if (legacy_resume) {
            throw std::invalid_argument("OOCRelationWriter: bare resume is unsupported; a paired "
                                        "V3 descriptor is required");
        }
        return std::nullopt;
    }

    static void validate_recovery_descriptor(const OOCSnapshotDescriptor& descriptor) {
        if (descriptor.format_version != FORMAT_VERSION) {
            throw std::invalid_argument(
                "OOCRelationWriter recovery: unsupported descriptor format version");
        }
        if (descriptor.store_id == 0) {
            throw std::invalid_argument("OOCRelationWriter recovery: zero store identity");
        }
        if (descriptor.generation == 0) {
            throw std::invalid_argument("OOCRelationWriter recovery: zero generation");
        }
        (void)index_size_for_count(descriptor.count);
        if (descriptor.data_end < DATA_HEADER_BYTES) {
            throw std::invalid_argument(
                "OOCRelationWriter recovery: data extent predates V3 header");
        }
        if ((descriptor.count == 0 && descriptor.data_end != DATA_HEADER_BYTES) ||
            (descriptor.count != 0 && descriptor.data_end == DATA_HEADER_BYTES)) {
            throw std::invalid_argument(
                "OOCRelationWriter recovery: relation count/data extent mismatch");
        }
        if (descriptor.data_end >
            static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            throw std::overflow_error("OOCRelationWriter recovery: data position overflow");
        }
    }

    static uint64_t read_u64_checked(gnfs::util::NativeBinaryUpdateFile& file,
                                     const char* operation, const char* field) {
        uint64_t value = 0;
        const std::string read_operation = std::string(operation) + " read " + field;
        file.read_exact(&value, sizeof(value), read_operation.c_str());
        return value;
    }

    /// Validate an exact V3 snapshot through the writer's retained handles.
    /// No canonical pathname is resolved between validation and the next
    /// mutation.
    void validate_exact_v3_pair(const OOCSnapshotDescriptor& descriptor, uint64_t expected_magic,
                                std::optional<uint64_t> expected_persisted_count,
                                OffsetValidation offset_validation, const char* operation) {
        if (descriptor.format_version != FORMAT_VERSION_V3 || descriptor.store_id == 0) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": invalid V3 descriptor identity");
        }
        (void)index_size_for_count(descriptor.count);
        if (descriptor.data_end < DATA_HEADER_BYTES) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": invalid V3 data extent");
        }

        const uint64_t index_size = idx_stream_.size(operation);
        const uint64_t data_size = data_stream_.size(operation);
        if (index_size != index_size_for_count(descriptor.count) ||
            data_size != descriptor.data_end) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": snapshot extent mismatch");
        }

        idx_stream_.seek(0, operation);
        data_stream_.seek(0, operation);
        const uint64_t index_magic = read_u64_checked(idx_stream_, operation, "index magic");
        const uint64_t index_version = read_u64_checked(idx_stream_, operation, "index version");
        const uint64_t index_store_id =
            read_u64_checked(idx_stream_, operation, "index store identity");
        const uint64_t persisted_count = read_u64_checked(idx_stream_, operation, "index count");
        const uint64_t data_magic = read_u64_checked(data_stream_, operation, "data magic");
        const uint64_t data_version = read_u64_checked(data_stream_, operation, "data version");
        const uint64_t data_store_id =
            read_u64_checked(data_stream_, operation, "data store identity");

        if (index_magic != expected_magic || index_version != FORMAT_VERSION_V3 ||
            data_magic != MAGIC_V3_DATA || data_version != FORMAT_VERSION_V3 ||
            index_store_id == 0 || index_store_id != descriptor.store_id ||
            data_store_id != index_store_id) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": V3 index/data header mismatch");
        }
        if (expected_persisted_count && persisted_count != *expected_persisted_count) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": persisted count mismatch");
        }

        idx_stream_.seek(INDEX_HEADER_BYTES, operation);
        const uint64_t first_offset = read_u64_checked(idx_stream_, operation, "first offset");
        if (first_offset != DATA_HEADER_BYTES ||
            (descriptor.count == 0 && descriptor.data_end != DATA_HEADER_BYTES) ||
            (descriptor.count != 0 && descriptor.data_end <= DATA_HEADER_BYTES)) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": invalid physical offset boundary");
        }

        uint64_t terminal_offset = first_offset;
        if (offset_validation == OffsetValidation::FullTable) {
            uint64_t previous = first_offset;
            for (uint64_t ordinal = 0; ordinal < descriptor.count; ++ordinal) {
                const uint64_t current = read_u64_checked(idx_stream_, operation, "offset");
                if (previous >= current || current > descriptor.data_end) {
                    throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                             ": non-monotonic or out-of-range offset");
                }
                previous = current;
            }
            terminal_offset = previous;
        } else {
            const uint64_t sentinel_position =
                INDEX_HEADER_BYTES + descriptor.count * sizeof(uint64_t);
            idx_stream_.seek(sentinel_position, operation);
            terminal_offset = read_u64_checked(idx_stream_, operation, "final sentinel");
        }
        if (terminal_offset != descriptor.data_end) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": final sentinel mismatch");
        }
    }

    uint64_t read_open_index_magic() {
        idx_stream_.seek(0, "recovery index magic seek");
        uint64_t magic = 0;
        idx_stream_.read_exact(&magic, sizeof(magic), "recovery index magic read");
        return magic;
    }

    static OOCValidatedResumePrefix validate_records(gnfs::util::NativeBinaryUpdateFile& data,
                                                     const std::vector<uint64_t>& offsets,
                                                     uint64_t count, uint64_t data_end,
                                                     uint64_t checkpoint_count) {
        if (checkpoint_count > count) {
            throw std::logic_error(
                "OOCRelationWriter recovery: checkpoint receipt exceeds validated prefix");
        }
        detail::validate_compact_relation_count(count, data_end, DATA_HEADER_BYTES,
                                                "OOCRelationWriter recovery");
        OOCValidatedResumePrefix prefix;
        prefix.count = count;
        prefix.data_end = data_end;
        prefix.seen.reserve(static_cast<size_t>(count));
        if (checkpoint_count == 0) {
            prefix.checkpoint_sequence_receipt = prefix.accepted_sequence.finish();
        }

        std::vector<uint8_t> record;
        for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
            const uint64_t record_size_u64 = offsets[i + 1] - offsets[i];
            if (record_size_u64 > detail::MAX_COMPACT_RELATION_BYTES) {
                throw std::runtime_error(
                    "OOCRelationWriter resume: record size exceeds persistence limit");
            }
            if (record_size_u64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
                throw std::overflow_error("OOCRelationWriter resume: record size exceeds size_t");
            }
            const size_t record_size = static_cast<size_t>(record_size_u64);
            record.resize(record_size);
            data.seek(offsets[i], "recovery relation seek");
            data.read_exact(record.data(), record_size, "recovery relation read");
            const auto relation =
                detail::deserialize_compact_relation(record.data(), record.size());
            prefix.seen.insert(relation.ab());
            prefix.accepted_sequence.append(relation);
            if (prefix.accepted_sequence.count() == checkpoint_count) {
                prefix.checkpoint_sequence_receipt = prefix.accepted_sequence.finish();
            }
            const size_t lp_count = count_odd_large_prime_keys(relation);
            if (lp_count == 0) {
                ++prefix.full_relations;
            } else if (lp_count == 1) {
                ++prefix.partial_1lp;
            } else {
                ++prefix.partial_2lp;
            }
        }
        return prefix;
    }

    static void validate_recovery_receipt(const OOCValidatedResumePrefix& prefix,
                                          const RelationSequenceReceipt& expected) {
        if (prefix.checkpoint_sequence_receipt != expected) {
            throw std::runtime_error(
                "OOCRelationWriter recovery: relation-sequence receipt mismatch");
        }
    }

    OOCValidatedResumePrefix validate_finalized_prefix(const OOCSnapshotDescriptor& descriptor) {
        constexpr const char* operation = "finalized recovery validation";
        const uint64_t index_size = idx_stream_.size(operation);
        const uint64_t data_size = data_stream_.size(operation);
        if (index_size < INDEX_HEADER_BYTES || data_size < DATA_HEADER_BYTES) {
            throw std::runtime_error("OOCRelationWriter recovery: finalized V3 header truncated");
        }

        idx_stream_.seek(0, operation);
        data_stream_.seek(0, operation);
        if (read_u64_checked(idx_stream_, operation, "index magic") != MAGIC_V3_FINAL) {
            throw std::runtime_error("OOCRelationWriter recovery: finalized magic changed");
        }
        if (read_u64_checked(idx_stream_, operation, "index format version") != FORMAT_VERSION_V3) {
            throw std::runtime_error(
                "OOCRelationWriter recovery: finalized format version mismatch");
        }
        const uint64_t finalized_store_id =
            read_u64_checked(idx_stream_, operation, "index store identity");
        if (finalized_store_id == 0 || finalized_store_id != descriptor.store_id) {
            throw std::runtime_error(
                "OOCRelationWriter recovery: finalized store identity mismatch");
        }
        const uint64_t final_count = read_u64_checked(idx_stream_, operation, "index count");

        const uint64_t data_magic = read_u64_checked(data_stream_, operation, "data magic");
        const uint64_t data_version = read_u64_checked(data_stream_, operation, "data version");
        const uint64_t data_store_id =
            read_u64_checked(data_stream_, operation, "data store identity");
        if (data_magic != MAGIC_V3_DATA || data_version != FORMAT_VERSION_V3 ||
            data_store_id != finalized_store_id) {
            throw std::runtime_error(
                "OOCRelationWriter recovery: finalized V3 index/data header mismatch");
        }
        if (final_count != descriptor.count) {
            throw std::runtime_error(
                "OOCRelationWriter recovery: finalized corpus is not the exact checkpoint prefix");
        }
        if (index_size != index_size_for_count(final_count)) {
            throw std::runtime_error("OOCRelationWriter recovery: finalized index size mismatch");
        }
        detail::validate_compact_relation_count(final_count, data_size, DATA_HEADER_BYTES,
                                                "OOCRelationWriter recovery");

        std::vector<uint64_t> offsets;
        offsets.reserve(static_cast<size_t>(final_count) + 1);
        for (uint64_t i = 0; i <= final_count; ++i) {
            offsets.push_back(read_u64_checked(idx_stream_, operation, "offset"));
        }
        if (offsets.front() != DATA_HEADER_BYTES || offsets.back() != data_size) {
            throw std::runtime_error("OOCRelationWriter recovery: finalized extent mismatch");
        }
        for (size_t i = 0; i < static_cast<size_t>(final_count); ++i) {
            if (offsets[i] >= offsets[i + 1] || offsets[i + 1] > data_size) {
                throw std::runtime_error(
                    "OOCRelationWriter recovery: invalid finalized offset sequence");
            }
        }
        if (offsets[static_cast<size_t>(descriptor.count)] != descriptor.data_end) {
            throw std::runtime_error(
                "OOCRelationWriter recovery: finalized corpus does not contain checkpoint prefix");
        }
        return validate_records(data_stream_, offsets, final_count, data_size, descriptor.count);
    }

    OOCValidatedResumePrefix validate_resume_prefix(const OOCSnapshotDescriptor& descriptor) {
        constexpr const char* operation = "resume validation";
        const uint64_t index_size = idx_stream_.size(operation);
        const uint64_t data_size = data_stream_.size(operation);
        if (index_size < INDEX_HEADER_BYTES || data_size < DATA_HEADER_BYTES) {
            throw std::runtime_error("OOCRelationWriter resume: V3 file header truncated");
        }

        idx_stream_.seek(0, operation);
        data_stream_.seek(0, operation);
        const uint64_t magic = read_u64_checked(idx_stream_, operation, "index magic");
        if (magic == MAGIC_V1_FINAL) {
            throw std::runtime_error(
                "OOCRelationWriter resume: legacy finalized V1 store is unsupported");
        }
        if (magic == MAGIC_V2_FINAL) {
            throw std::runtime_error(
                "OOCRelationWriter resume: finalized V2 store is read-only compatibility data");
        }
        if (magic == MAGIC_INCOMPLETE_V1 || magic == MAGIC_V2_INCOMPLETE) {
            throw std::runtime_error(
                "OOCRelationWriter resume: legacy incomplete store is unsupported");
        }
        if (magic == MAGIC_V3_FINAL) {
            throw std::runtime_error("OOCRelationWriter resume: file already finalized (V3)");
        }
        if (magic != MAGIC_V3_INCOMPLETE) {
            throw std::runtime_error("OOCRelationWriter resume: invalid magic in idx (corrupt?)");
        }

        const uint64_t persisted_format_version =
            read_u64_checked(idx_stream_, operation, "index format version");
        if (persisted_format_version != FORMAT_VERSION_V3) {
            throw std::runtime_error("OOCRelationWriter resume: format version mismatch");
        }
        const uint64_t persisted_store_id =
            read_u64_checked(idx_stream_, operation, "index store identity");
        if (persisted_store_id == 0 || persisted_store_id != descriptor.store_id) {
            throw std::runtime_error("OOCRelationWriter resume: store identity mismatch");
        }
        // Count is advisory until final magic commits it. It can be nonzero if
        // the previous process died after the finalize metadata flush; recovery
        // trusts only the paired descriptor and resets this field to zero.
        (void)read_u64_checked(idx_stream_, operation, "incomplete count");

        const uint64_t data_magic = read_u64_checked(data_stream_, operation, "data magic");
        const uint64_t data_version = read_u64_checked(data_stream_, operation, "data version");
        const uint64_t data_store_id =
            read_u64_checked(data_stream_, operation, "data store identity");
        if (data_magic != MAGIC_V3_DATA || data_version != FORMAT_VERSION_V3 ||
            data_store_id != persisted_store_id) {
            throw std::runtime_error("OOCRelationWriter resume: V3 index/data header mismatch");
        }

        const uint64_t count = descriptor.count;
        const uint64_t expected_index_size = index_size_for_count(count);
        if (index_size < expected_index_size) {
            throw std::runtime_error("OOCRelationWriter resume: committed index prefix truncated");
        }
        if (data_size < descriptor.data_end) {
            throw std::runtime_error("OOCRelationWriter resume: committed data prefix truncated");
        }
        detail::validate_compact_relation_count(count, descriptor.data_end, DATA_HEADER_BYTES,
                                                "OOCRelationWriter resume");

        std::vector<uint64_t> offsets;
        offsets.reserve(static_cast<size_t>(count) + 1);
        for (uint64_t i = 0; i <= count; ++i) {
            offsets.push_back(read_u64_checked(idx_stream_, operation, "offset"));
        }
        if (offsets.front() != DATA_HEADER_BYTES) {
            throw std::runtime_error(
                "OOCRelationWriter resume: first offset does not follow V3 data header");
        }
        if (offsets.back() != descriptor.data_end) {
            throw std::runtime_error(
                "OOCRelationWriter resume: final sentinel does not match checkpoint");
        }
        for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
            if (offsets[i] >= offsets[i + 1] || offsets[i + 1] > descriptor.data_end) {
                throw std::runtime_error(
                    "OOCRelationWriter resume: non-monotonic or out-of-range offset");
            }
        }

        return validate_records(data_stream_, offsets, count, descriptor.data_end, count);
    }

    void acquire_prefix_reader(const OOCSnapshotDescriptor& descriptor) {
        require_state(OOCWriterState::Suspended, "acquire_prefix_reader");
        if (!suspended_descriptor_ || descriptor != *suspended_descriptor_) {
            throw std::invalid_argument(
                "OOCRelationWriter::acquire_prefix_reader: stale or foreign descriptor");
        }
        require_store_named_identity("acquire_prefix_reader");
        ++active_prefix_readers_;
    }

    void release_prefix_reader() noexcept {
        assert(active_prefix_readers_ > 0);
        if (active_prefix_readers_ > 0)
            --active_prefix_readers_;
    }

    void serialize(const gnfs::core::Relation& rel) {
        write_val(rel.a);
        write_val(rel.b);
        write_vec32(rel.rational_factors);
        write_vec32(rel.algebraic_factors);
        write_pp_vec(rel.rational_large_prime);
        write_pp_vec(rel.algebraic_large_prime);
        // extra_ab_pairs
        auto sz = static_cast<uint32_t>(rel.extra_ab_pairs.size());
        write_val(sz);
        for (const auto& [ea, eb] : rel.extra_ab_pairs) {
            write_val(ea);
            write_val(eb);
        }
    }

    template <typename T> void write_val(const T& v) {
        data_stream_.write_exact(&v, sizeof(T), "serialize scalar");
    }

    void write_vec32(const std::vector<uint32_t>& v) {
        auto sz = static_cast<uint32_t>(v.size());
        write_val(sz);
        if (sz > 0) {
            data_stream_.write_exact(v.data(), static_cast<size_t>(sz) * sizeof(uint32_t),
                                     "serialize factor vector");
        }
    }

    void write_pp_vec(const std::vector<gnfs::core::PrimePower>& v) {
        auto sz = static_cast<uint32_t>(v.size());
        write_val(sz);
        for (const auto& pp : v) {
            write_val(pp.p);
            write_val(pp.r);
            write_val(pp.e);
        }
    }

    /// Validate the exact r/w handles that will serve the next writer
    /// transition. The get positions are temporary; both put positions are
    /// restored before returning.
    void validate_open_v3_pair_headers(const char* operation,
                                       std::optional<uint64_t> expected_persisted_count) {
        if (!data_stream_.is_open() || !idx_stream_.is_open()) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": store handles are not open");
        }
        const uint64_t data_position = data_stream_.position(operation);
        const uint64_t index_position = idx_stream_.position(operation);

        data_stream_.flush(operation);
        idx_stream_.flush(operation);
        data_stream_.seek(0, operation);
        idx_stream_.seek(0, operation);

        const auto read_u64 = [&](gnfs::util::NativeBinaryUpdateFile& file, const char* field) {
            uint64_t value = 0;
            file.read_exact(&value, sizeof(value), operation);
            (void)field;
            return value;
        };
        const uint64_t index_magic = read_u64(idx_stream_, "index magic");
        const uint64_t index_version = read_u64(idx_stream_, "index version");
        const uint64_t index_store_id = read_u64(idx_stream_, "index store identity");
        const uint64_t persisted_count = read_u64(idx_stream_, "index count");
        const uint64_t data_magic = read_u64(data_stream_, "data magic");
        const uint64_t data_version = read_u64(data_stream_, "data version");
        const uint64_t data_store_id = read_u64(data_stream_, "data store identity");

        if (index_magic != MAGIC_V3_INCOMPLETE || index_version != FORMAT_VERSION_V3 ||
            data_magic != MAGIC_V3_DATA || data_version != FORMAT_VERSION_V3 ||
            index_store_id == 0 || index_store_id != store_id_ || data_store_id != index_store_id) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": V3 index/data header mismatch");
        }
        if (expected_persisted_count && persisted_count != *expected_persisted_count) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": persisted count mismatch");
        }

        data_stream_.seek(data_position, operation);
        idx_stream_.seek(index_position, operation);
    }

    void require_state(OOCWriterState expected, const char* operation) const {
        if (state_ != expected) {
            throw std::logic_error(std::string("OOCRelationWriter::") + operation +
                                   ": invalid writer state");
        }
    }

    void close_open_streams_checked(const char* operation) {
        if (data_stream_.is_open())
            data_stream_.close_checked(operation);
        if (idx_stream_.is_open())
            idx_stream_.close_checked(operation);
    }

    void abort_close_noexcept() noexcept {
        if (exact_writer_process_changed_noexcept()) {
            discard_inherited_post_fork_child_noexcept();
            return;
        }
        if (data_stream_.is_open())
            data_stream_.close_noexcept();
        if (idx_stream_.is_open())
            idx_stream_.close_noexcept();
    }

    void fail_and_close_noexcept() noexcept {
        state_ = OOCWriterState::Failed;
        abort_close_noexcept();
    }

    void discard_inherited_post_fork_child_noexcept() noexcept {
        exact_append_batch_active_ = false;
        state_ = OOCWriterState::Failed;
        data_stream_.discard_and_close_post_fork_child_noexcept();
        idx_stream_.discard_and_close_post_fork_child_noexcept();
    }

    [[nodiscard]] bool exact_writer_process_changed_noexcept() const noexcept {
        if (!exact_private_directory_) {
            return false;
        }
        const int process_id = gnfs::util::process_id();
        return exact_private_directory_->creator_process_id == 0 || process_id <= 0 ||
               exact_private_directory_->creator_process_id !=
                   static_cast<std::uint64_t>(process_id);
    }

    void require_store_named_identity(const char* operation) const {
        if (exact_private_directory_) {
            const auto paths = ooc_cleanup_detail::freeze_paths(base_path_);
            require_exact_private_directory_binding(paths, operation);
#ifndef _WIN32
            data_stream_.require_named_identity_at(exact_private_directory_->directory_descriptor,
                                                   exact_private_directory_->data_leaf, operation);
            idx_stream_.require_named_identity_at(exact_private_directory_->directory_descriptor,
                                                  exact_private_directory_->index_leaf, operation);
#endif
            return;
        }
        data_stream_.require_named_identity(base_path_ + ".reldata", operation);
        idx_stream_.require_named_identity(base_path_ + ".relidx", operation);
    }

    [[nodiscard]] ExactAppendBatchGuard begin_exact_append_batch() {
        require_state(OOCWriterState::Open, "begin exact append batch");
        if (!exact_private_directory_ || exact_append_batch_active_) {
            throw std::logic_error(
                "OOCRelationWriter: exact append batch requires one inactive exact writer");
        }
        require_store_named_identity("exact append batch begin");
        exact_append_batch_active_ = true;
        return ExactAppendBatchGuard(*this);
    }

    void require_exact_append_batch_process(const char* operation) const {
        if (!exact_private_directory_ || !exact_append_batch_active_) {
            throw std::logic_error(std::string("OOCRelationWriter::") + operation +
                                   ": exact append batch is not active");
        }
        const auto& exact = *exact_private_directory_;
        const int process_id = gnfs::util::process_id();
        if (exact.creator_process_id == 0 || process_id <= 0 ||
            exact.creator_process_id != static_cast<std::uint64_t>(process_id)) {
            throw std::system_error(std::make_error_code(std::errc::operation_not_permitted),
                                    std::string("OOCRelationWriter::") + operation +
                                        ": exact append batch process changed");
        }
    }

    void sync_store_files_and_directory() {
        const std::filesystem::path index_path(base_path_ + ".relidx");
        if (!data_stream_.is_open() || !idx_stream_.is_open()) {
            throw std::logic_error(
                "OOCRelationWriter: durability barrier requires retained store handles");
        }
        require_store_named_identity("store sync preflight");
        data_stream_.sync("store data sync");
        idx_stream_.sync("store index sync");
        if (exact_private_directory_) {
            detail::sync_directory_descriptor_after_metadata_change(
                exact_private_directory_->directory_descriptor, base_path_);
        } else {
            detail::sync_parent_directory_after_metadata_change(index_path);
        }
        require_store_named_identity("store sync commit");
    }

    static uint64_t allocate_store_id() noexcept {
        static std::atomic<uint64_t> sequence{1};
        uint64_t seed = static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        seed ^= static_cast<uint64_t>(static_cast<uint32_t>(gnfs::util::process_id())) << 32U;
        seed ^= sequence.fetch_add(1, std::memory_order_relaxed);
        try {
            std::random_device random;
            seed ^= static_cast<uint64_t>(random()) << 32U;
            seed ^= static_cast<uint64_t>(random());
        } catch (...) {
            // Time, PID, and the per-process sequence remain a safe fallback.
        }

        // splitmix64 finalizer: diffuse every input bit and avoid sequential or
        // timestamp-shaped persistent identifiers.
        seed += 0x9E3779B97F4A7C15ULL;
        seed = (seed ^ (seed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        seed = (seed ^ (seed >> 27U)) * 0x94D049BB133111EBULL;
        seed ^= seed >> 31U;
        return seed == 0 ? 1 : seed;
    }

    [[nodiscard]] OOCFinalizedCorpusEvidenceV1
    capture_finalized_corpus_evidence(const OOCSnapshotDescriptor& descriptor);

    void finalize_and_publish_private_handoff_built(OOCPrivateHandoffPayloadBuilderV1 builder,
                                                    void* context,
                                                    OOCPrivateHandoffTestHooks hooks = {});

    std::string base_path_;
    std::optional<ExactPrivateDirectoryBinding> exact_private_directory_;
    // Declared before streams so stream destruction always precedes release of
    // the lock-owning deferred lease, including exceptional destruction.
    std::optional<OOCPrivateLeaseOwnershipReceipt> owned_deferred_private_lease_;
    gnfs::util::NativeBinaryUpdateFile data_stream_;
    gnfs::util::NativeBinaryUpdateFile idx_stream_;
    size_t count_ = 0;
    int uncaught_at_ctor_ = 0;
    uint64_t store_id_ = allocate_store_id();
    uint64_t generation_ = 0;
    bool fresh_store_ = false;
    bool fresh_artifacts_removed_ = false;
    std::optional<OOCCleanupOwnershipReceipt> cleanup_receipt_;
    bool deferred_private_lease_action_in_progress_ = false;
    bool exact_append_batch_active_ = false;
    OOCWriterState state_ = OOCWriterState::Open;
    std::optional<OOCSnapshotDescriptor> suspended_descriptor_;
    std::optional<OOCSnapshotDescriptor> finalized_descriptor_;
    bool finalized_durable_ = false;
    std::optional<OOCValidatedResumePrefix> validated_resume_prefix_;
    OOCRecoveryOutcome recovery_outcome_ = OOCRecoveryOutcome::None;
    size_t active_prefix_readers_ = 0;

    friend class OOCRelationPrefixReader;
    friend class ::gnfs::sieve::distributed_sieve_worker_entry_detail::
        DistributedSieveWorkerWriterAuthorityV1;
    friend class ::gnfs::sieve::distributed_sieve_worker_entry_detail::
        distributed_sieve_worker_writer_detail::OOCInheritedP8WriterMintV1;
    friend class ::gnfs::sieve::distributed_sieve_merge_writer_authority_detail::
        DistributedSieveMergeWriterAuthorityV1;
    friend class ::gnfs::sieve::distributed_sieve_resume_detail::
        DistributedSieveMergeStartedWriterMintV1;
};

/// Read-only mmap-based access to out-of-core relations.
///
/// Maps both .relidx and .reldata files into memory.
/// Provides O(1) access to any relation by index.
class OOCRelationReader {
public:
    OOCRelationReader() = default;

    OOCRelationReader(const OOCRelationReader&) = delete;
    OOCRelationReader& operator=(const OOCRelationReader&) = delete;

    OOCRelationReader(OOCRelationReader&& other) noexcept
        : idx_file_(std::move(other.idx_file_)), data_file_(std::move(other.data_file_)),
          count_(std::exchange(other.count_, 0)), offsets_(std::exchange(other.offsets_, nullptr)),
          binding_(std::move(other.binding_)) {}

    OOCRelationReader& operator=(OOCRelationReader&& other) noexcept {
        if (this != &other) {
            idx_file_ = std::move(other.idx_file_);
            data_file_ = std::move(other.data_file_);
            count_ = std::exchange(other.count_, 0);
            offsets_ = std::exchange(other.offsets_, nullptr);
            binding_ = std::move(other.binding_);
        }
        return *this;
    }

    explicit OOCRelationReader(const std::string& base_path)
        : idx_file_(base_path + ".relidx"), data_file_(base_path + ".reldata") {
        initialize_bound(nullptr);
    }

    /// Open a descriptor-bound finalized V2 or V3 corpus.
    ///
    /// Unlike a path-level preflight followed by an ordinary reader open, this
    /// overload validates the persisted identity and both file extents against
    /// the same mapped handles that serve subsequent reads. V3 additionally
    /// requires matching store identities in the index and data headers. V2
    /// has no data-file identity, so callers opening V2 retain the historical
    /// exclusive-ownership requirement.
    OOCRelationReader(const std::string& base_path, const OOCSnapshotDescriptor& expected)
        : idx_file_(base_path + ".relidx"), data_file_(base_path + ".reldata") {
        validate_expected_descriptor(expected);
        initialize_bound(&expected);
    }

    /// Adopt a caller-validated exact finalized V3 pair without reopening a
    /// path.
    ///
    /// The caller must establish native identity, no-follow, owner/mode/link,
    /// and exact-extent policy before transferring these handles. This reader
    /// grants no adoption, cleanup, or arm authority; it validates only the V3
    /// format, paired store identity, descriptor, offsets, sentinel, and
    /// extents through the same handles that serve subsequent reads. Native
    /// identity and policy are therefore transfer-time obligations of the
    /// caller, not facts inferred by this constructor.
    ///
    /// Generation is caller-domain freshness metadata: V3 does not persist it,
    /// so this constructor can require only that it is nonzero. Both wrappers
    /// remain caller-owned if descriptor or wrapper prevalidation fails. Once
    /// both are committed, any mapping or corpus-validation failure closes
    /// both handles.
    OOCRelationReader(gnfs::util::OwnedNativeFile&& index_file,
                      gnfs::util::OwnedNativeFile&& data_file,
                      const OOCSnapshotDescriptor& expected)
        : OOCRelationReader(commit_owned_v3_pair(index_file, data_file, expected), expected) {}

    /// Whether this object owns one fully initialized mapped relation pair.
    ///
    /// Default-constructed and moved-from readers are invalid. A finalized
    /// zero-row corpus is valid because its index still contains the sentinel.
    [[nodiscard]] bool valid() const noexcept {
        return binding_ != nullptr && idx_file_.is_open() && data_file_.is_open() &&
               offsets_ != nullptr;
    }

    /// Number of stored relations.
    [[nodiscard]] size_t count() const noexcept {
        return count_;
    }

    /// Read a single relation by index.
    [[nodiscard]] gnfs::core::Relation read(size_t idx) const {
        if (idx >= count_) {
            throw std::out_of_range("OOCRelationReader::read: index out of range");
        }
        uint64_t start = offsets_[idx];
        uint64_t end = offsets_[idx + 1];
        if (start >= end || end > data_file_.size()) {
            throw std::runtime_error("OOCRelationReader::read: corrupt offsets");
        }
        if (end - start > detail::MAX_COMPACT_RELATION_BYTES) {
            throw std::runtime_error("OOCRelationReader::read: record exceeds persistence limit");
        }

        const uint8_t* ptr = data_file_.data() + start;
        size_t avail = static_cast<size_t>(end - start);
        return detail::deserialize_compact_relation(ptr, avail);
    }

    /// Read all relations into a vector (for compatibility with in-memory pipeline).
    [[nodiscard]] std::vector<gnfs::core::Relation> read_all() const {
        std::vector<gnfs::core::Relation> result;
        result.reserve(count_);
        for (size_t i = 0; i < count_; ++i) {
            result.push_back(read(i));
        }
        return result;
    }

    /// Read a range [from, to) of relations.
    [[nodiscard]] std::vector<gnfs::core::Relation> read_range(size_t from, size_t to) const {
        if (from > to || to > count_) {
            throw std::out_of_range("OOCRelationReader::read_range: invalid range");
        }
        std::vector<gnfs::core::Relation> result;
        result.reserve(to - from);
        for (size_t i = from; i < to; ++i) {
            result.push_back(read(i));
        }
        return result;
    }

private:
    struct OwnedExactPair final {
        OwnedExactPair(gnfs::util::OwnedNativeFile&& index_file,
                       gnfs::util::OwnedNativeFile&& data_file) noexcept
            : index(std::move(index_file)), data(std::move(data_file)) {}

        gnfs::util::OwnedNativeFile index;
        gnfs::util::OwnedNativeFile data;
    };

    OOCRelationReader(OwnedExactPair pair, const OOCSnapshotDescriptor& expected)
        : idx_file_(std::move(pair.index)), data_file_(std::move(pair.data)) {
        initialize_bound(&expected);
    }

    static void validate_expected_descriptor(const OOCSnapshotDescriptor& expected) {
        if (expected.format_version != OOCRelationWriter::FORMAT_VERSION_V2 &&
            expected.format_version != OOCRelationWriter::FORMAT_VERSION_V3) {
            throw std::invalid_argument(
                "OOCRelationReader: expected descriptor format version mismatch");
        }
        if (expected.store_id == 0) {
            throw std::invalid_argument("OOCRelationReader: expected store identity is zero");
        }
        if (expected.generation == 0) {
            throw std::invalid_argument("OOCRelationReader: expected generation is zero");
        }
        (void)OOCRelationWriter::index_size_for_count(expected.count);
    }

    [[nodiscard]] static OwnedExactPair
    commit_owned_v3_pair(gnfs::util::OwnedNativeFile& index_file,
                         gnfs::util::OwnedNativeFile& data_file,
                         const OOCSnapshotDescriptor& expected) {
        validate_owned_v3_descriptor(expected);
        if (&index_file == &data_file || !index_file.valid() || !data_file.valid()) {
            throw std::invalid_argument(
                "OOCRelationReader: owned exact pair requires two distinct valid wrappers");
        }

        // This is the sole dual-handle commit point. Both moves are noexcept;
        // after it returns the caller wrappers are invalid, and pair/member
        // unwinding closes both handles on every later failure.
        return OwnedExactPair(std::move(index_file), std::move(data_file));
    }

    static void validate_owned_v3_descriptor(const OOCSnapshotDescriptor& expected) {
        validate_expected_descriptor(expected);
        if (expected.format_version != OOCRelationWriter::FORMAT_VERSION_V3) {
            throw std::invalid_argument(
                "OOCRelationReader: owned exact pair requires finalized V3");
        }
        if (expected.data_end < OOCRelationWriter::DATA_HEADER_BYTES ||
            (expected.count == 0 && expected.data_end != OOCRelationWriter::DATA_HEADER_BYTES) ||
            (expected.count != 0 && expected.data_end <= OOCRelationWriter::DATA_HEADER_BYTES) ||
            expected.data_end > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            throw std::invalid_argument(
                "OOCRelationReader: owned exact pair descriptor has invalid data extent");
        }
    }

    void initialize_bound(const OOCSnapshotDescriptor* expected) {
        initialize(expected);
        binding_ = std::make_shared<const BindingIdentity>();
    }

    void initialize(const OOCSnapshotDescriptor* expected) {
        // Validate finalized V1, V2, or V3 from the exact mappings used for
        // subsequent reads. Descriptor-bound opens accept V2 and V3.
        // Incomplete files are rejected in both modes.
        if (idx_file_.size() < 16) {
            throw std::runtime_error("OOCRelationReader: index file too small");
        }
        const uint64_t magic = idx_file_.read_at<uint64_t>(0);
        uint64_t stored_count = 0;
        uint64_t stored_version = 0;
        uint64_t stored_store_id = 0;
        size_t index_header_bytes = 0;
        uint64_t expected_first_offset = 0;
        if (magic == OOCRelationWriter::MAGIC_V2_FINAL ||
            magic == OOCRelationWriter::MAGIC_V3_FINAL) {
            if (idx_file_.size() < OOCRelationWriter::INDEX_HEADER_BYTES) {
                throw std::runtime_error("OOCRelationReader: index header truncated");
            }
            stored_version =
                idx_file_.read_at<uint64_t>(OOCRelationWriter::INDEX_FORMAT_VERSION_OFFSET);
            const uint64_t required_version = magic == OOCRelationWriter::MAGIC_V3_FINAL
                                                  ? OOCRelationWriter::FORMAT_VERSION_V3
                                                  : OOCRelationWriter::FORMAT_VERSION_V2;
            if (stored_version != required_version) {
                throw std::runtime_error("OOCRelationReader: format version mismatch");
            }
            stored_store_id = idx_file_.read_at<uint64_t>(OOCRelationWriter::INDEX_STORE_ID_OFFSET);
            if (stored_store_id == 0) {
                throw std::runtime_error("OOCRelationReader: store identity is zero");
            }
            stored_count = idx_file_.read_at<uint64_t>(OOCRelationWriter::INDEX_COUNT_OFFSET);
            index_header_bytes = static_cast<size_t>(OOCRelationWriter::INDEX_HEADER_BYTES);

            if (magic == OOCRelationWriter::MAGIC_V3_FINAL) {
                if (data_file_.size() < OOCRelationWriter::DATA_HEADER_BYTES) {
                    throw std::runtime_error("OOCRelationReader: V3 data header truncated");
                }
                if (data_file_.read_at<uint64_t>(0) != OOCRelationWriter::MAGIC_V3_DATA) {
                    throw std::runtime_error("OOCRelationReader: invalid V3 data magic");
                }
                const uint64_t data_version =
                    data_file_.read_at<uint64_t>(OOCRelationWriter::DATA_FORMAT_VERSION_OFFSET);
                if (data_version != OOCRelationWriter::FORMAT_VERSION_V3) {
                    throw std::runtime_error("OOCRelationReader: V3 data version mismatch");
                }
                const uint64_t data_store_id =
                    data_file_.read_at<uint64_t>(OOCRelationWriter::DATA_STORE_ID_OFFSET);
                if (data_store_id != stored_store_id) {
                    throw std::runtime_error(
                        "OOCRelationReader: V3 index/data store identity mismatch");
                }
                expected_first_offset = OOCRelationWriter::DATA_HEADER_BYTES;
            }

            if (expected != nullptr) {
                if (stored_version != expected->format_version) {
                    throw std::runtime_error(
                        "OOCRelationReader: finalized format does not match descriptor");
                }
                if (stored_store_id != expected->store_id) {
                    throw std::runtime_error(
                        "OOCRelationReader: finalized store identity does not match descriptor");
                }
                if (stored_count != expected->count) {
                    throw std::runtime_error(
                        "OOCRelationReader: finalized relation count does not match descriptor");
                }
            }
        } else if (magic == OOCRelationWriter::MAGIC_V1_FINAL) {
            if (expected != nullptr) {
                throw std::runtime_error(
                    "OOCRelationReader: expected descriptor requires finalized V2 or V3 store");
            }
            stored_count = idx_file_.read_at<uint64_t>(8);
            index_header_bytes = 16;
        } else {
            throw std::runtime_error("OOCRelationReader: invalid magic in index");
        }
        if (stored_count > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            throw std::overflow_error("OOCRelationReader: relation count exceeds size_t");
        }
        count_ = static_cast<size_t>(stored_count);

        // Index should have: header + (count+1) × 8-byte offsets.
        if (count_ > (std::numeric_limits<size_t>::max() - index_header_bytes -
                      static_cast<size_t>(OOCRelationWriter::INDEX_SENTINEL_BYTES)) /
                         sizeof(uint64_t)) {
            throw std::overflow_error("OOCRelationReader: index size overflow");
        }
        if (expected != nullptr) {
            if (static_cast<uint64_t>(idx_file_.size()) !=
                OOCRelationWriter::index_size_for_count(expected->count)) {
                throw std::runtime_error(
                    "OOCRelationReader: finalized index extent does not match descriptor");
            }
        } else {
            const size_t expected_idx =
                index_header_bytes + static_cast<size_t>(OOCRelationWriter::INDEX_SENTINEL_BYTES) +
                count_ * sizeof(uint64_t);
            if (idx_file_.size() != expected_idx) {
                throw std::runtime_error(
                    "OOCRelationReader: index size does not match relation count");
            }
        }
        if (expected != nullptr && static_cast<uint64_t>(data_file_.size()) != expected->data_end) {
            throw std::runtime_error(
                "OOCRelationReader: finalized data extent does not match descriptor");
        }

        detail::validate_compact_relation_count(stored_count,
                                                static_cast<uint64_t>(data_file_.size()),
                                                expected_first_offset, "OOCRelationReader");

        offsets_ = idx_file_.ptr_at<uint64_t>(index_header_bytes);
        if (offsets_[0] != expected_first_offset) {
            throw std::runtime_error("OOCRelationReader: invalid first offset");
        }
        if (expected != nullptr && offsets_[count_] != expected->data_end) {
            throw std::runtime_error("OOCRelationReader: final sentinel does not match descriptor");
        }
        if (offsets_[count_] != static_cast<uint64_t>(data_file_.size())) {
            throw std::runtime_error("OOCRelationReader: final sentinel does not match data size");
        }
        for (size_t i = 0; i < count_; ++i) {
            if (offsets_[i] >= offsets_[i + 1] ||
                offsets_[i + 1] > static_cast<uint64_t>(data_file_.size())) {
                throw std::runtime_error("OOCRelationReader: non-monotonic or out-of-range offset");
            }
        }

        // Switch to random access pattern for data
        data_file_.advise_random();
    }

    gnfs::util::MmapFile idx_file_;
    gnfs::util::MmapFile data_file_;
    size_t count_ = 0;
    const uint64_t* offsets_ = nullptr;
    struct BindingIdentity final {};
    std::shared_ptr<const BindingIdentity> binding_;

    friend class ReadOnlyRelationCorpusView;
};

inline OOCFinalizedCorpusEvidenceV1
OOCRelationWriter::capture_finalized_corpus_evidence(const OOCSnapshotDescriptor& descriptor) {
    if (state_ != OOCWriterState::Finalized || !finalized_durable_ || !finalized_descriptor_ ||
        *finalized_descriptor_ != descriptor || !data_stream_.is_open() || !idx_stream_.is_open()) {
        throw std::logic_error(
            "OOCRelationWriter: finalized corpus evidence requires retained exact handles");
    }
    if (!cleanup_receipt_ || cleanup_receipt_->spent() ||
        cleanup_receipt_->store_id_ != descriptor.store_id) {
        throw std::logic_error(
            "OOCRelationWriter: finalized corpus evidence ownership is unavailable");
    }

    require_store_named_identity("finalized evidence preflight");
    const std::uint64_t expected_index_extent = index_size_for_count(descriptor.count);
    if (idx_stream_.size("finalized evidence index extent") != expected_index_extent ||
        data_stream_.size("finalized evidence data extent") != descriptor.data_end) {
        throw std::runtime_error("OOCRelationWriter: finalized corpus evidence extent mismatch");
    }

    auto index_file = idx_stream_.duplicate_for_mapping("finalized evidence index duplicate");
    auto data_file = data_stream_.duplicate_for_mapping("finalized evidence data duplicate");
    OOCRelationReader reader(std::move(index_file), std::move(data_file), descriptor);

    RelationSequenceReceiptAccumulator sequence;
    RelationCorpusSha256AccumulatorV1 corpus_sha256;
    for (std::size_t index = 0; index < reader.count(); ++index) {
        const auto relation = reader.read(index);
        sequence.append(relation);
        if (!corpus_sha256.append(relation)) {
            throw std::runtime_error(
                "OOCRelationWriter: finalized corpus SHA-256 accumulation failed");
        }
    }
    const auto digest = corpus_sha256.finalize();
    if (!digest || sequence.count() != descriptor.count ||
        corpus_sha256.count() != descriptor.count) {
        throw std::runtime_error(
            "OOCRelationWriter: finalized corpus evidence count or digest mismatch");
    }

    // The mapping above remains bound to the duplicated exact objects. Repeat
    // the retained-handle namespace and extent checks before any application
    // payload can be built from the evidence.
    require_store_named_identity("finalized evidence commit");
    if (idx_stream_.size("finalized evidence final index extent") != expected_index_extent ||
        data_stream_.size("finalized evidence final data extent") != descriptor.data_end) {
        throw std::runtime_error(
            "OOCRelationWriter: finalized corpus evidence changed during validation");
    }

    const auto index_identity = cleanup_receipt_->index_identity_;
    const auto data_identity = cleanup_receipt_->data_identity_;
    const std::array<std::uint64_t, 3> index_native{
        index_identity.first,
        index_identity.second,
        index_identity.third,
    };
    const std::array<std::uint64_t, 3> data_native{
        data_identity.first,
        data_identity.second,
        data_identity.third,
    };
    if (index_native == std::array<std::uint64_t, 3>{} ||
        data_native == std::array<std::uint64_t, 3>{} || index_native == data_native) {
        throw std::runtime_error("OOCRelationWriter: finalized corpus native identity is invalid");
    }

    return {
        .descriptor = descriptor,
        .index_file =
            {
                .identity = index_native,
                .extent = expected_index_extent,
            },
        .data_file =
            {
                .identity = data_native,
                .extent = descriptor.data_end,
            },
        .sequence_receipt = sequence.finish(),
        .corpus_sha256 = *digest,
    };
}

inline void OOCRelationWriter::finalize_and_publish_private_handoff_built(
    OOCPrivateHandoffPayloadBuilderV1 builder, void* context, OOCPrivateHandoffTestHooks hooks) {
    if (builder == nullptr) {
        throw std::invalid_argument(
            "OOCRelationWriter: private handoff payload builder is missing");
    }
    if (!owned_deferred_private_lease_) {
        throw std::logic_error(
            "OOCRelationWriter: built private handoff requires deferred private-lease mode");
    }
    if (!cleanup_receipt_ || cleanup_receipt_->spent()) {
        throw std::logic_error("OOCRelationWriter: built private handoff ownership is unavailable");
    }
#if !defined(__APPLE__)
    throw std::system_error(std::make_error_code(std::errc::operation_not_supported),
                            "OOCRelationWriter: built private handoff publication is unsupported");
#endif
    DeferredPrivateLeaseActionGuard action_guard(*this);

    const OOCSnapshotDescriptor descriptor = finalize_impl(nullptr, true);
    if (descriptor.store_id != store_id_) {
        throw std::runtime_error(
            "OOCRelationWriter: built private handoff descriptor identity mismatch");
    }
    auto evidence = capture_finalized_corpus_evidence(descriptor);
    auto payload = builder(evidence, context);
    if (payload.kind == 0 || payload.version == 0 ||
        payload.bytes.size() > OOC_PRIVATE_HANDOFF_MAX_OPAQUE_PAYLOAD_BYTES) {
        throw std::invalid_argument(
            "OOCRelationWriter: built private handoff payload contract is invalid");
    }

    // Revoke the last update-capable handles before publication. If a checked
    // close fails, the caller may retry through finalize(); no handoff or
    // cleanup namespace mutation has happened yet.
    close_open_streams_checked("built private handoff close");

    const OOCPrivateHandoffPairDescriptorV1 pair{
        .format_version = descriptor.format_version,
        .store_id = descriptor.store_id,
        .generation = descriptor.generation,
        .count = descriptor.count,
        .index_extent = evidence.index_file.extent,
        .data_extent = evidence.data_file.extent,
    };
    const auto result = OOCCleanupTransaction::publish_private_handoff(
        *cleanup_receipt_, *owned_deferred_private_lease_, pair, payload.kind, payload.version,
        payload.bytes, hooks);
    if (!result.canonical() || !result.record) {
        const auto error = result.result.native_error
                               ? result.result.native_error
                               : std::make_error_code(std::errc::protocol_error);
        throw std::system_error(error,
                                "OOCRelationWriter: built private handoff publication failed");
    }

    const auto native_identity = [](const std::array<std::uint64_t, 3>& identity) noexcept {
        return util::durable_immutable_record::NativeIdentity{
            .first = identity[0],
            .second = identity[1],
            .third = identity[2],
        };
    };
    const auto& record = *result.record;
    if (record.pair != pair ||
        record.index.identity != native_identity(evidence.index_file.identity) ||
        record.index.extent != evidence.index_file.extent ||
        record.data.identity != native_identity(evidence.data_file.identity) ||
        record.data.extent != evidence.data_file.extent || record.payload_kind != payload.kind ||
        record.payload_version != payload.version || record.opaque_payload != payload.bytes) {
        throw std::runtime_error(
            "OOCRelationWriter: canonical private handoff does not match finalized evidence");
    }
}

/// Move-only same-handle reader owner for one adopted private handoff.
///
/// Construction commits both exact files into OOCRelationReader together,
/// validates the complete finalized V3 corpus, and retains the adoption
/// receipt's parent/private-directory bindings and BaseLock for the reader
/// lifetime. The reader is destroyed before the receipt releases those
/// handles. This owner exposes no path, native handle, cleanup receipt, or arm
/// operation. Access through this owner is process-checked. A const reader
/// reference borrowed before fork remains ordinary read-only data in the child
/// and carries no cleanup or conversion authority.
class OOCPrivateHandoffReader final {
public:
    explicit OOCPrivateHandoffReader(OOCPrivateHandoffAdoptionReceipt&& adoption)
        : OOCPrivateHandoffReader(commit_adoption(adoption)) {}

    OOCPrivateHandoffReader(const OOCPrivateHandoffReader&) = delete;
    OOCPrivateHandoffReader& operator=(const OOCPrivateHandoffReader&) = delete;
    OOCPrivateHandoffReader(OOCPrivateHandoffReader&&) noexcept = default;
    OOCPrivateHandoffReader& operator=(OOCPrivateHandoffReader&&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return adoption_.live_lock_ != nullptr && adoption_.parent_directory_ != nullptr &&
               adoption_.private_directory_handle_ != nullptr &&
               adoption_.owned_by_current_process() && reader_.valid();
    }

    [[nodiscard]] const OOCRelationReader& reader() const {
        if (!valid()) {
            throw std::logic_error("OOCPrivateHandoffReader: reader is moved-from or invalid");
        }
        return reader_;
    }

    [[nodiscard]] const OOCPrivateHandoffRecordV1& record() const noexcept {
        return adoption_.record_;
    }

    [[nodiscard]] const util::durable_immutable_record::RecordSnapshot&
    handoff_snapshot() const noexcept {
        return adoption_.handoff_snapshot_;
    }

    [[nodiscard]] const util::durable_immutable_record::RecordSnapshot&
    index_snapshot() const noexcept {
        return adoption_.index_.snapshot;
    }

    [[nodiscard]] const util::durable_immutable_record::RecordSnapshot&
    data_snapshot() const noexcept {
        return adoption_.data_.snapshot;
    }

private:
    [[nodiscard]] bool cleanup_intent_conversion_ready() const noexcept {
        return adoption_.live_lock_ != nullptr && adoption_.parent_directory_ != nullptr &&
               adoption_.private_directory_handle_ != nullptr &&
               adoption_.owned_by_current_process() && !adoption_.spent_ &&
               (reader_.valid() || cleanup_intent_conversion_ready_);
    }

    void close_reader_views_for_cleanup_intent_conversion() noexcept {
        reader_ = OOCRelationReader{};
        cleanup_intent_conversion_ready_ = true;
    }

    void commit_cleanup_intent_conversion() noexcept {
        adoption_.spent_ = true;
        cleanup_intent_conversion_ready_ = false;
    }

    void release_cleanup_intent_conversion_authority() noexcept {
        reader_ = OOCRelationReader{};
        adoption_.private_directory_handle_.reset();
        adoption_.parent_directory_.reset();
        adoption_.live_lock_.reset();
        adoption_.retains_private_cleanup_action_claim_ = false;
        cleanup_intent_conversion_ready_ = false;
    }

    /// Source-private transition used after a committed merge has adopted the
    /// exact merged corpus. The implementation moves the same-handle reader
    /// out first, then releases every adoption/action-claim/namespace handle so
    /// the returned value carries read authority only.
    [[nodiscard]] OOCRelationReader take_read_only_reader_and_release_adoption_authority() &&;

    struct CommittedAdoption final {
        OOCPrivateHandoffAdoptionReceipt adoption;
        OOCSnapshotDescriptor descriptor;
    };

    [[nodiscard]] static CommittedAdoption
    commit_adoption(OOCPrivateHandoffAdoptionReceipt& adoption) {
        if (adoption.spent() ||
            adoption.record_.index.identity != adoption.index_.snapshot.identity ||
            adoption.record_.index.extent != adoption.index_.snapshot.size ||
            adoption.record_.data.identity != adoption.data_.snapshot.identity ||
            adoption.record_.data.extent != adoption.data_.snapshot.size ||
            !validate_ooc_private_handoff_record(adoption.record_, true)) {
            throw std::invalid_argument(
                "OOCPrivateHandoffReader: invalid or spent adoption receipt");
        }
        const OOCSnapshotDescriptor descriptor{
            .format_version = adoption.record_.pair.format_version,
            .store_id = adoption.record_.pair.store_id,
            .generation = adoption.record_.pair.generation,
            .count = adoption.record_.pair.count,
            .data_end = adoption.record_.pair.data_extent,
        };
        return CommittedAdoption{
            .adoption = std::move(adoption),
            .descriptor = descriptor,
        };
    }

    explicit OOCPrivateHandoffReader(CommittedAdoption committed)
        : adoption_(std::move(committed.adoption)),
          reader_(std::move(adoption_.index_.file), std::move(adoption_.data_.file),
                  committed.descriptor) {
        if (!reader_.valid()) {
            throw std::runtime_error(
                "OOCPrivateHandoffReader: same-handle reader validation failed");
        }
    }

    // Declaration order is the authority order: reverse destruction closes
    // both reader mappings before releasing the adoption receipt's BaseLock.
    OOCPrivateHandoffAdoptionReceipt adoption_;
    OOCRelationReader reader_;
    bool cleanup_intent_conversion_ready_ = false;

    friend class ooc_cleanup_detail::OOCPrivateHandoffCleanupIntentConversionExecutorV2;
    friend class ooc_cleanup_detail::OOCPrivateHandoffReadOnlyReleaseExecutorV1;
    friend class gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail::
        DistributedSieveWorkerCleanupIntentConversionAuthorityV1;
};

/// Trusted reader for a single flushed prefix of an otherwise incomplete OOC
/// store. Construction requires an explicit descriptor and performs all
/// boundary validation with runtime checks that remain active in Release.
class OOCRelationPrefixReader {
public:
    OOCRelationPrefixReader(const std::string& base_path, const OOCSnapshotDescriptor& descriptor,
                            OOCRelationWriter& owner)
        : descriptor_(descriptor), owner_(&owner) {
        std::string frozen_base_path;
        try {
            frozen_base_path = ooc_cleanup_detail::freeze_paths(base_path).base_path.string();
        } catch (const ooc_cleanup_detail::Failure& failure) {
            const auto error =
                failure.error ? failure.error : std::make_error_code(std::errc::invalid_argument);
            throw std::system_error(error, "OOCRelationPrefixReader: invalid base path");
        }
        if (descriptor.store_id == 0 || owner.base_path() != frozen_base_path ||
            !owner.owns_suspended_prefix(descriptor)) {
            throw std::invalid_argument(
                "OOCRelationPrefixReader: foreign or stale snapshot descriptor");
        }

        owner.acquire_prefix_reader(descriptor);
        try {
            // Map duplicates of the writer's exact retained handles. No
            // pathname is resolved after the Suspended ownership check, so a
            // rename/replacement cannot redirect either half of this prefix.
            auto index_file =
                owner.idx_stream_.duplicate_for_mapping("prefix reader index duplicate");
            auto data_file =
                owner.data_stream_.duplicate_for_mapping("prefix reader data duplicate");
            idx_file_ = gnfs::util::MmapFile(std::move(index_file));
            data_file_ = gnfs::util::MmapFile(std::move(data_file));
            if (idx_file_.size() < OOCRelationWriter::INDEX_HEADER_BYTES) {
                throw std::runtime_error("OOCRelationPrefixReader: index file too small");
            }
            if (data_file_.size() < OOCRelationWriter::DATA_HEADER_BYTES) {
                throw std::runtime_error("OOCRelationPrefixReader: V3 data header truncated");
            }
            const uint64_t magic = idx_file_.read_at<uint64_t>(0);
            if (magic != OOCRelationWriter::MAGIC_V3_INCOMPLETE) {
                throw std::runtime_error("OOCRelationPrefixReader: store is not incomplete");
            }
            const uint64_t index_store_id =
                idx_file_.read_at<uint64_t>(OOCRelationWriter::INDEX_STORE_ID_OFFSET);
            if (descriptor_.format_version != OOCRelationWriter::FORMAT_VERSION_V3 ||
                idx_file_.read_at<uint64_t>(OOCRelationWriter::INDEX_FORMAT_VERSION_OFFSET) !=
                    OOCRelationWriter::FORMAT_VERSION_V3 ||
                index_store_id == 0 || index_store_id != descriptor_.store_id ||
                data_file_.read_at<uint64_t>(0) != OOCRelationWriter::MAGIC_V3_DATA ||
                data_file_.read_at<uint64_t>(OOCRelationWriter::DATA_FORMAT_VERSION_OFFSET) !=
                    OOCRelationWriter::FORMAT_VERSION_V3 ||
                data_file_.read_at<uint64_t>(OOCRelationWriter::DATA_STORE_ID_OFFSET) !=
                    index_store_id) {
                throw std::runtime_error(
                    "OOCRelationPrefixReader: V3 index/data identity or format mismatch");
            }
            if (idx_file_.read_at<uint64_t>(OOCRelationWriter::INDEX_COUNT_OFFSET) != 0) {
                throw std::runtime_error("OOCRelationPrefixReader: incomplete count is not zero");
            }
            if (descriptor.count > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
                throw std::overflow_error("OOCRelationPrefixReader: relation count exceeds size_t");
            }
            count_ = static_cast<size_t>(descriptor.count);
            const uint64_t expected_idx = OOCRelationWriter::index_size_for_count(descriptor.count);
            if (static_cast<uint64_t>(idx_file_.size()) != expected_idx) {
                throw std::runtime_error(
                    "OOCRelationPrefixReader: index size does not match snapshot");
            }
            if (descriptor.data_end != static_cast<uint64_t>(data_file_.size())) {
                throw std::runtime_error(
                    "OOCRelationPrefixReader: data size does not match snapshot");
            }

            detail::validate_compact_relation_count(descriptor.count, descriptor.data_end,
                                                    OOCRelationWriter::DATA_HEADER_BYTES,
                                                    "OOCRelationPrefixReader");

            offsets_ = idx_file_.ptr_at<uint64_t>(
                static_cast<size_t>(OOCRelationWriter::INDEX_HEADER_BYTES));
            if (offsets_[count_] != descriptor.data_end) {
                throw std::runtime_error(
                    "OOCRelationPrefixReader: descriptor end does not match sentinel");
            }
            if (offsets_[0] != OOCRelationWriter::DATA_HEADER_BYTES) {
                throw std::runtime_error(
                    "OOCRelationPrefixReader: first offset does not follow V3 data header");
            }
            if (count_ == 0) {
                if (descriptor.data_end != OOCRelationWriter::DATA_HEADER_BYTES) {
                    throw std::runtime_error(
                        "OOCRelationPrefixReader: empty prefix extent is not the V3 header size");
                }
            } else {
                for (size_t i = 0; i < count_; ++i) {
                    const uint64_t start = offsets_[i];
                    const uint64_t end = offsets_[i + 1];
                    if (start >= end || end > descriptor.data_end) {
                        throw std::runtime_error(
                            "OOCRelationPrefixReader: non-monotonic or out-of-range offset");
                    }
                }
            }

            data_file_.advise_random();
        } catch (...) {
            // Close both mappings before the writer lease becomes observable as
            // released. This ordering is required on Windows and also prevents
            // a concurrent resume from racing constructor unwinding.
            data_file_.close();
            idx_file_.close();
            owner.release_prefix_reader();
            owner_ = nullptr;
            throw;
        }
    }

    ~OOCRelationPrefixReader() {
        // The owner may resume mutation as soon as the lease is released, so
        // mappings must be closed first on every platform.
        data_file_.close();
        idx_file_.close();
        if (owner_ != nullptr) {
            owner_->release_prefix_reader();
            owner_ = nullptr;
        }
    }

    OOCRelationPrefixReader(const OOCRelationPrefixReader&) = delete;
    OOCRelationPrefixReader& operator=(const OOCRelationPrefixReader&) = delete;
    OOCRelationPrefixReader(OOCRelationPrefixReader&&) = delete;
    OOCRelationPrefixReader& operator=(OOCRelationPrefixReader&&) = delete;

    [[nodiscard]] size_t count() const noexcept {
        return count_;
    }

    [[nodiscard]] gnfs::core::Relation read(size_t idx) const {
        if (idx >= count_) {
            throw std::out_of_range("OOCRelationPrefixReader::read: index out of range");
        }
        const uint64_t start = offsets_[idx];
        const uint64_t end = offsets_[idx + 1];
        if (start >= end || end > descriptor_.data_end ||
            end > static_cast<uint64_t>(data_file_.size())) {
            throw std::runtime_error("OOCRelationPrefixReader::read: corrupt offsets");
        }
        if (end - start > detail::MAX_COMPACT_RELATION_BYTES) {
            throw std::runtime_error(
                "OOCRelationPrefixReader::read: record exceeds persistence limit");
        }
        return detail::deserialize_compact_relation(data_file_.data() + start,
                                                    static_cast<size_t>(end - start));
    }

    [[nodiscard]] std::vector<gnfs::core::Relation> read_all() const {
        std::vector<gnfs::core::Relation> result;
        result.reserve(count_);
        for (size_t i = 0; i < count_; ++i) {
            result.push_back(read(i));
        }
        return result;
    }

private:
    gnfs::util::MmapFile idx_file_;
    gnfs::util::MmapFile data_file_;
    OOCSnapshotDescriptor descriptor_;
    size_t count_ = 0;
    const uint64_t* offsets_ = nullptr;
    OOCRelationWriter* owner_ = nullptr;
};

} // namespace gnfs::relation

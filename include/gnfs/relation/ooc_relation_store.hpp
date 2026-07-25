#pragma once

#include "gnfs/core/relation.hpp"
#include "gnfs/relation/large_prime_key.hpp"
#include "gnfs/relation/ooc_cleanup_transaction.hpp"
#include "gnfs/relation/ooc_relation_format.hpp"
#include "gnfs/relation/relation_sequence_receipt.hpp"
#include "gnfs/util/mmap_file.hpp"
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
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <random>
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

namespace gnfs::relation {

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
        directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    } while (directory < 0 && errno == EINTR);
    if (directory < 0) {
        throw std::runtime_error("OOC directory sync cannot open " + parent.string() + ": " +
                                 std::strerror(errno));
    }

    int result = -1;
    do {
        result = ::fsync(directory);
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

public:
    /// Fresh create writes paired incomplete V3 headers with one durable store
    /// identity.
    explicit OOCRelationWriter(const std::string& base_path)
        : OOCRelationWriter(base_path, std::nullopt, std::nullopt, nullptr, ConstructionToken{}) {}

    /// Fresh private-lease creation keeps the lease's persistent BaseLock held
    /// across both O_EXCL reservations and durable lease activation.
    OOCRelationWriter(const std::string& base_path, OOCPrivateLeaseOwnershipReceipt& private_lease)
        : OOCRelationWriter(base_path, std::nullopt, std::nullopt, &private_lease,
                            ConstructionToken{}) {}

    /// Paired recovery requires both the structural descriptor and the
    /// semantic relation-sequence receipt from the same durable checkpoint.
    /// There is intentionally no descriptor-only recovery overload.
    OOCRelationWriter(const std::string& base_path,
                      const OOCSnapshotDescriptor& recovery_descriptor,
                      const RelationSequenceReceipt& recovery_sequence_receipt)
        : OOCRelationWriter(base_path, recovery_descriptor, recovery_sequence_receipt, nullptr,
                            ConstructionToken{}) {}

private:
    explicit OOCRelationWriter(const std::string& base_path,
                               std::optional<OOCSnapshotDescriptor> recovery_descriptor,
                               std::optional<RelationSequenceReceipt> recovery_sequence_receipt,
                               OOCPrivateLeaseOwnershipReceipt* private_lease, ConstructionToken)
        : base_path_(freeze_base_path_checked(base_path)), data_buf_(BUFFER_BYTES),
          idx_buf_(BUFFER_BYTES / 4), // 256 KB suffices for index
          uncaught_at_ctor_(std::uncaught_exceptions()),
          fresh_store_(!recovery_descriptor.has_value()) {
        if (recovery_sequence_receipt.has_value() != recovery_descriptor.has_value()) {
            throw std::logic_error(
                "OOCRelationWriter: internal recovery descriptor/receipt pairing violated");
        }
        if (private_lease != nullptr && recovery_descriptor) {
            throw std::invalid_argument(
                "OOCRelationWriter: private lease is valid only for fresh creation");
        }
        if (recovery_descriptor &&
            recovery_sequence_receipt->relation_count != recovery_descriptor->count) {
            throw std::invalid_argument(
                "OOCRelationWriter recovery: sequence receipt count differs from descriptor");
        }

        // pubsetbuf 必须在 open 之前调用,所以 fstream 默认构造、
        // 然后手动 attach buffer、最后 open。
        data_stream_.rdbuf()->pubsetbuf(data_buf_.data(),
                                        static_cast<std::streamsize>(data_buf_.size()));
        idx_stream_.rdbuf()->pubsetbuf(idx_buf_.data(),
                                       static_cast<std::streamsize>(idx_buf_.size()));

        if (recovery_descriptor) {
            validate_recovery_descriptor(*recovery_descriptor);
            const uint64_t existing_magic = read_index_magic(base_path);
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
                validated_resume_prefix_ =
                    validate_finalized_prefix(base_path, *recovery_descriptor);
                validate_recovery_receipt(*validated_resume_prefix_, *recovery_sequence_receipt);
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
            validated_resume_prefix_ = validate_resume_prefix(base_path, *recovery_descriptor);
            // The semantic receipt is part of the durable checkpoint. Compare
            // it while recovery is still read-only: a mismatch must not
            // truncate an uncommitted tail or rewrite incomplete metadata.
            validate_recovery_receipt(*validated_resume_prefix_, *recovery_sequence_receipt);

            // Validation is read-only. Only after the full committed prefix has
            // proved valid do we discard uncommitted crash tails. Truncate data
            // first: if the second resize fails, another recovery can still
            // validate the same descriptor and finish the index rollback.
            std::filesystem::resize_file(base_path + ".reldata", recovery_descriptor->data_end);
            const uint64_t recovered_index_size = index_size_for_count(recovery_descriptor->count);
            std::filesystem::resize_file(base_path + ".relidx", recovered_index_size);

            store_id_ = recovery_descriptor->store_id;
            generation_ = recovery_descriptor->generation;
            count_ = static_cast<size_t>(validated_resume_prefix_->count);

            // Reopen in r/w mode (no trunc). Revalidate both exact handles
            // before the first seek or metadata mutation.
            data_stream_.open(base_path + ".reldata",
                              std::ios::in | std::ios::out | std::ios::binary);
            idx_stream_.open(base_path + ".relidx",
                             std::ios::in | std::ios::out | std::ios::binary);
            if (!data_stream_ || !idx_stream_) {
                throw std::runtime_error("OOCRelationWriter resume: cannot reopen at " + base_path);
            }
            validate_open_v3_pair_headers("resume constructor header validation", std::nullopt);
            // The first append overwrites the committed sentinel slot.
            data_stream_.seekp(static_cast<std::streamoff>(validated_resume_prefix_->data_end));
            // A crash during finalize may have persisted a nonzero count under
            // INCOMPLETE magic. Roll it back before returning to append mode;
            // version and store_id remain immutable throughout.
            idx_stream_.seekp(static_cast<std::streamoff>(INDEX_COUNT_OFFSET));
            const uint64_t incomplete_count = 0;
            idx_stream_.write(reinterpret_cast<const char*>(&incomplete_count), 8);
            idx_stream_.flush();
            ensure_streams_good("resume constructor count rollback");

            const uint64_t idx_pos =
                INDEX_HEADER_BYTES + static_cast<uint64_t>(count_) * sizeof(uint64_t);
            idx_stream_.seekp(static_cast<std::streamoff>(idx_pos));
            ensure_streams_good("resume constructor seek");
            recovery_outcome_ = OOCRecoveryOutcome::AppendablePrefix;
        } else {
            try {
                // Serialize the complete namespace-empty check and both O_EXCL
                // reservations with cleanup/recovery callers. Pending,
                // canonical, staged, or quarantine leaves fail closed; explicit
                // transaction recovery must finish before fresh reuse.
                const auto cleanup_paths = ooc_cleanup_detail::freeze_paths(base_path_);
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
                ooc_cleanup_detail::require_pair_namespace_reusable_locked(cleanup_paths);

                // Reserve both fresh names with O_EXCL before opening either
                // stream. A second writer can therefore never pass an exists
                // check and then truncate this store.
                const std::string index_path = cleanup_paths.index_path.string();
                const std::string data_path = cleanup_paths.data_path.string();
                std::optional<FreshArtifactReservation> index_reservation;
                std::optional<FreshArtifactReservation> data_reservation;
                try {
                    index_reservation.emplace(FreshArtifactReservation::create(index_path));
                    data_reservation.emplace(FreshArtifactReservation::create(data_path));

                    data_stream_.open(data_path, std::ios::in | std::ios::out | std::ios::binary);
                    idx_stream_.open(index_path, std::ios::in | std::ios::out | std::ios::binary);
                    if (!data_stream_ || !idx_stream_) {
                        throw std::runtime_error("OOCRelationWriter: cannot open files at " +
                                                 base_path);
                    }
                    // 先写 INCOMPLETE 标志。若 write 中途抛(磁盘满等),析构跳过
                    // finalize → reader 看到 INCOMPLETE 拒读,避免 idx/data 不一致。
                    // 成功 close 后再翻成 MAGIC。
                    const uint64_t magic = MAGIC_INCOMPLETE;
                    const uint64_t format_version = FORMAT_VERSION;
                    const uint64_t durable_store_id = store_id_;
                    const uint64_t incomplete_count = 0;
                    idx_stream_.write(reinterpret_cast<const char*>(&magic), 8);
                    idx_stream_.write(reinterpret_cast<const char*>(&format_version), 8);
                    idx_stream_.write(reinterpret_cast<const char*>(&durable_store_id), 8);
                    idx_stream_.write(reinterpret_cast<const char*>(&incomplete_count), 8);
                    const uint64_t data_magic = MAGIC_V3_DATA;
                    data_stream_.write(reinterpret_cast<const char*>(&data_magic), 8);
                    data_stream_.write(reinterpret_cast<const char*>(&format_version), 8);
                    data_stream_.write(reinterpret_cast<const char*>(&durable_store_id), 8);
                    ensure_streams_good("constructor header write");
                    validate_open_v3_pair_headers("constructor header validation",
                                                  incomplete_count);
                    data_reservation->close_checked(data_path);
                    index_reservation->close_checked(index_path);
                    auto cleanup_receipt = capture_fresh_cleanup_ownership_checked(
                        base_path_, store_id_, index_reservation->identity(),
                        data_reservation->identity());
                    static_assert(std::is_nothrow_move_constructible_v<OOCCleanupOwnershipReceipt>);
                    cleanup_receipt_.emplace(std::move(cleanup_receipt));
                    if (private_lease != nullptr) {
                        const auto activated =
                            OOCCleanupTransaction::activate_private_lease_for_fresh_writer(
                                *private_lease, *cleanup_receipt_);
                        if (!activated.completed()) {
                            const auto error =
                                activated.native_error
                                    ? activated.native_error
                                    : std::make_error_code(std::errc::protocol_error);
                            throw std::system_error(
                                error, "OOCRelationWriter: private lease activation failed");
                        }
                    }
                } catch (...) {
                    abort_close_noexcept();
                    if (data_reservation) {
                        data_reservation->remove_path_if_same_identity_noexcept(data_path);
                    }
                    if (index_reservation) {
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
                            ConstructionToken{}) {}

    /// Append a single relation. Returns the index of the written relation.
    size_t write(const gnfs::core::Relation& rel) {
        require_state(OOCWriterState::Open, "write");

        // Caller/input errors do not poison an otherwise healthy writer. This
        // preflight happens before tellp(), index writes, data writes, or count
        // mutation, so a rejected relation leaves the writer Open and unchanged.
        rel.validate_persistence_limits();

        try {
            const auto pos = data_stream_.tellp();
            if (pos == std::streampos(-1)) {
                throw std::runtime_error("OOCRelationWriter::write: cannot determine data offset");
            }

            const uint64_t offset = static_cast<uint64_t>(pos);
            idx_stream_.write(reinterpret_cast<const char*>(&offset), 8);
            serialize(rel);
            ensure_streams_good("write");

            ++count_;
            return count_ - 1;
        } catch (...) {
            fail_and_close_noexcept();
            throw;
        }
    }

    /// Flush a stable prefix and suspend append handles without flipping MAGIC.
    /// `resume_append()` must be called with the returned descriptor before the
    /// next write. While suspended, a trusted prefix reader may mmap the files.
    [[nodiscard]] OOCSnapshotDescriptor checkpoint_prefix() {
        require_state(OOCWriterState::Open, "checkpoint_prefix");
        try {
            validate_open_v3_pair_headers("checkpoint_prefix header validation", uint64_t{0});

            const auto pos = data_stream_.tellp();
            if (pos == std::streampos(-1)) {
                throw std::runtime_error(
                    "OOCRelationWriter::checkpoint_prefix: cannot determine data end");
            }
            const uint64_t end_offset = static_cast<uint64_t>(pos);

            // The next offset slot is a temporary end sentinel. A later append
            // overwrites this exact slot with the next relation's start offset.
            idx_stream_.write(reinterpret_cast<const char*>(&end_offset), 8);
            idx_stream_.flush();
            ensure_streams_good("checkpoint_prefix index flush");

            close_streams_checked("checkpoint_prefix close");
            sync_store_files_and_directory();

            OOCSnapshotDescriptor descriptor;
            descriptor.format_version = FORMAT_VERSION;
            descriptor.store_id = store_id_;
            descriptor.generation = ++generation_;
            descriptor.count = static_cast<uint64_t>(count_);
            descriptor.data_end = end_offset;
            validate_exact_v3_pair(base_path_, descriptor, MAGIC_V3_INCOMPLETE, uint64_t{0},
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
            validate_exact_v3_pair(base_path_, descriptor, MAGIC_V3_INCOMPLETE, uint64_t{0},
                                   OffsetValidation::BoundaryOnly, "resume_append");
            data_stream_.clear();
            idx_stream_.clear();
            data_stream_.open(base_path_ + ".reldata",
                              std::ios::in | std::ios::out | std::ios::binary);
            idx_stream_.open(base_path_ + ".relidx",
                             std::ios::in | std::ios::out | std::ios::binary);
            if (!data_stream_ || !idx_stream_) {
                throw std::runtime_error("OOCRelationWriter::resume_append: cannot reopen at " +
                                         base_path_);
            }
            validate_open_v3_pair_headers("resume_append reopened header validation", uint64_t{0});

            if (descriptor.data_end >
                static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
                throw std::overflow_error(
                    "OOCRelationWriter::resume_append: data position overflow");
            }
            data_stream_.seekp(static_cast<std::streamoff>(descriptor.data_end));
            (void)index_size_for_count(descriptor.count);
            const uint64_t idx_pos = INDEX_HEADER_BYTES + descriptor.count * sizeof(uint64_t);
            if (idx_pos > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
                throw std::overflow_error(
                    "OOCRelationWriter::resume_append: index position overflow");
            }
            idx_stream_.seekp(static_cast<std::streamoff>(idx_pos));
            ensure_streams_good("resume_append seek");

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
        if (state_ == OOCWriterState::Finalized) {
            if (!finalized_durable_) {
                sync_store_files_and_directory();
                finalized_durable_ = true;
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
            OOCSnapshotDescriptor descriptor;
            bool offsets_validated_before_metadata = false;
            if (state_ == OOCWriterState::Open) {
                validate_open_v3_pair_headers("finalize header validation", uint64_t{0});

                const auto pos = data_stream_.tellp();
                if (pos == std::streampos(-1)) {
                    throw std::runtime_error(
                        "OOCRelationWriter::finalize: cannot determine data end");
                }
                const uint64_t end_offset = static_cast<uint64_t>(pos);
                idx_stream_.write(reinterpret_cast<const char*>(&end_offset), 8);

                descriptor.format_version = FORMAT_VERSION;
                descriptor.generation = ++generation_;
                descriptor.store_id = store_id_;
                descriptor.count = static_cast<uint64_t>(count_);
                descriptor.data_end = end_offset;
            } else {
                // Suspended already has a flushed sentinel and closed handles.
                descriptor = *suspended_descriptor_;
                validate_exact_v3_pair(base_path_, descriptor, MAGIC_V3_INCOMPLETE, uint64_t{0},
                                       OffsetValidation::FullTable, "finalize suspended prefix");
                offsets_validated_before_metadata = true;
                data_stream_.clear();
                idx_stream_.clear();
                data_stream_.open(base_path_ + ".reldata",
                                  std::ios::in | std::ios::out | std::ios::binary);
                idx_stream_.open(base_path_ + ".relidx",
                                 std::ios::in | std::ios::out | std::ios::binary);
                if (!data_stream_ || !idx_stream_) {
                    throw std::runtime_error(
                        "OOCRelationWriter::finalize: cannot reopen store at " + base_path_);
                }
                validate_open_v3_pair_headers("finalize reopened header validation", uint64_t{0});
            }

            // Commit payload metadata first while the file remains INCOMPLETE.
            // MAGIC is written and flushed last, so a failed finalize cannot
            // advertise a prefix whose sentinel/count were not persisted.
            idx_stream_.seekp(static_cast<std::streamoff>(INDEX_COUNT_OFFSET));
            const uint64_t final_count = descriptor.count;
            idx_stream_.write(reinterpret_cast<const char*>(&final_count), 8);

            if (data_stream_.is_open())
                data_stream_.flush();
            idx_stream_.flush();
            ensure_open_streams_good("finalize metadata flush");

            // Close and durably sync the full payload while magic remains
            // INCOMPLETE. A process exit after this point is recoverable from
            // the paired checkpoint because version/store_id are untouched.
            close_open_streams_checked("finalize metadata close");
            sync_store_files_and_directory();
            if (hook != nullptr) {
                hook(FinalizeStage::MetadataDurable);
            }

            validate_exact_v3_pair(base_path_, descriptor, MAGIC_V3_INCOMPLETE, descriptor.count,
                                   offsets_validated_before_metadata
                                       ? OffsetValidation::BoundaryOnly
                                       : OffsetValidation::FullTable,
                                   "finalize precommit");
            data_stream_.clear();
            idx_stream_.clear();
            data_stream_.open(base_path_ + ".reldata",
                              std::ios::in | std::ios::out | std::ios::binary);
            idx_stream_.open(base_path_ + ".relidx",
                             std::ios::in | std::ios::out | std::ios::binary);
            if (!data_stream_ || !idx_stream_) {
                throw std::runtime_error(
                    "OOCRelationWriter::finalize: cannot reopen store for final magic at " +
                    base_path_);
            }
            validate_open_v3_pair_headers("finalize precommit reopened validation",
                                          descriptor.count);
            idx_stream_.seekp(0);
            const uint64_t final_magic = MAGIC_V3_FINAL;
            idx_stream_.write(reinterpret_cast<const char*>(&final_magic), 8);
            idx_stream_.flush();
            ensure_open_streams_good("finalize magic flush");
            close_open_streams_checked("finalize close");

            // Once final magic has been flushed and both handles have closed,
            // the pair is visibly committed. Record that outcome before the
            // final durability barrier or observer hook: either can throw, but
            // must never relabel a readable finalized pair as a failed writer.
            suspended_descriptor_.reset();
            finalized_descriptor_ = descriptor;
            state_ = OOCWriterState::Finalized;
            sync_store_files_and_directory();
            finalized_durable_ = true;
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
        if (!cleanup_receipt_ || cleanup_receipt_->spent()) {
            throw std::logic_error(
                "OOCRelationWriter: cleanup ownership is unavailable or already consumed");
        }
        OOCCleanupOwnershipReceipt receipt(std::move(*cleanup_receipt_));
        cleanup_receipt_.reset();
        return receipt;
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
            const int descriptor =
                ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
            if (descriptor < 0) {
                throw std::system_error(errno, std::generic_category(),
                                        "OOCRelationWriter: cannot reserve fresh artifact " + path);
            }
            struct stat information{};
            if (::fstat(descriptor, &information) != 0) {
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

    static uint64_t read_u64_checked(std::istream& stream, const char* operation,
                                     const char* field) {
        uint64_t value = 0;
        stream.read(reinterpret_cast<char*>(&value), sizeof(value));
        if (stream.gcount() != static_cast<std::streamsize>(sizeof(value))) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": truncated " + field);
        }
        return value;
    }

    static uint64_t stream_size_checked(std::ifstream& stream, const char* operation,
                                        const char* field) {
        stream.clear();
        stream.seekg(0, std::ios::end);
        const auto position = stream.tellg();
        if (position == std::streampos(-1)) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": cannot size " + field);
        }
        const auto offset = static_cast<std::streamoff>(position);
        if (offset < 0) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": negative " + field + " size");
        }
        stream.clear();
        stream.seekg(0);
        return static_cast<uint64_t>(offset);
    }

    /// Validate a closed, exact V3 snapshot through the same two read handles.
    /// Same-process checkpoint/resume uses the constant-time boundary mode to
    /// avoid rescanning a growing index every interval. Final commit and crash
    /// recovery validate the complete offset table.
    static void validate_exact_v3_pair(const std::string& base_path,
                                       const OOCSnapshotDescriptor& descriptor,
                                       uint64_t expected_magic,
                                       std::optional<uint64_t> expected_persisted_count,
                                       OffsetValidation offset_validation, const char* operation) {
        if (descriptor.format_version != FORMAT_VERSION_V3 || descriptor.store_id == 0) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": invalid V3 descriptor identity");
        }
        (void)index_size_for_count(descriptor.count);
        if (descriptor.data_end < DATA_HEADER_BYTES ||
            descriptor.data_end >
                static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": invalid V3 data extent");
        }

        std::ifstream index(base_path + ".relidx", std::ios::binary);
        std::ifstream data(base_path + ".reldata", std::ios::binary);
        if (!index || !data) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": paired store is incomplete");
        }

        const uint64_t index_magic = read_u64_checked(index, operation, "index magic");
        const uint64_t index_version = read_u64_checked(index, operation, "index version");
        const uint64_t index_store_id = read_u64_checked(index, operation, "index store identity");
        const uint64_t persisted_count = read_u64_checked(index, operation, "index count");
        const uint64_t data_magic = read_u64_checked(data, operation, "data magic");
        const uint64_t data_version = read_u64_checked(data, operation, "data version");
        const uint64_t data_store_id = read_u64_checked(data, operation, "data store identity");

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

        const uint64_t index_size = stream_size_checked(index, operation, "index");
        const uint64_t data_size = stream_size_checked(data, operation, "data");
        if (index_size != index_size_for_count(descriptor.count) ||
            data_size != descriptor.data_end) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": snapshot extent mismatch");
        }

        index.clear();
        index.seekg(static_cast<std::streamoff>(INDEX_HEADER_BYTES));
        if (!index) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": cannot seek offset table");
        }
        const uint64_t first_offset = read_u64_checked(index, operation, "first offset");
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
                const uint64_t current = read_u64_checked(index, operation, "offset");
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
            index.clear();
            index.seekg(static_cast<std::streamoff>(sentinel_position));
            if (!index) {
                throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                         ": cannot seek final sentinel");
            }
            terminal_offset = read_u64_checked(index, operation, "final sentinel");
        }
        if (terminal_offset != descriptor.data_end) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": final sentinel mismatch");
        }
    }

    static uint64_t read_index_magic(const std::string& base_path) {
        std::ifstream index(base_path + ".relidx", std::ios::binary);
        if (!index) {
            throw std::runtime_error("OOCRelationWriter recovery: idx file not found at " +
                                     base_path);
        }
        uint64_t magic = 0;
        index.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        if (index.gcount() != static_cast<std::streamsize>(sizeof(magic))) {
            throw std::runtime_error("OOCRelationWriter recovery: truncated index magic");
        }
        return magic;
    }

    [[nodiscard]] bool closed_pair_has_owned_incomplete_headers_noexcept() const noexcept {
        try {
            std::ifstream index(base_path_ + ".relidx", std::ios::binary);
            std::ifstream data(base_path_ + ".reldata", std::ios::binary);
            if (!index || !data) {
                return false;
            }

            const uint64_t index_magic =
                read_u64_checked(index, "exception cleanup", "index magic");
            const uint64_t index_version =
                read_u64_checked(index, "exception cleanup", "index version");
            const uint64_t index_store_id =
                read_u64_checked(index, "exception cleanup", "index store identity");
            const uint64_t data_magic = read_u64_checked(data, "exception cleanup", "data magic");
            const uint64_t data_version =
                read_u64_checked(data, "exception cleanup", "data version");
            const uint64_t data_store_id =
                read_u64_checked(data, "exception cleanup", "data store identity");
            return index_magic == MAGIC_V3_INCOMPLETE && index_version == FORMAT_VERSION_V3 &&
                   index_store_id == store_id_ && data_magic == MAGIC_V3_DATA &&
                   data_version == FORMAT_VERSION_V3 && data_store_id == store_id_;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool closed_pair_is_owned_finalized_store_noexcept() const noexcept {
        try {
            if (!finalized_descriptor_.has_value() ||
                finalized_descriptor_->store_id != store_id_) {
                return false;
            }
            validate_exact_v3_pair(base_path_, *finalized_descriptor_, MAGIC_V3_FINAL,
                                   finalized_descriptor_->count, OffsetValidation::FullTable,
                                   "exception cleanup");
            return true;
        } catch (...) {
            return false;
        }
    }

    static OOCValidatedResumePrefix validate_records(std::ifstream& data,
                                                     const std::vector<uint64_t>& offsets,
                                                     uint64_t count, uint64_t data_end,
                                                     uint64_t checkpoint_count) {
        if (checkpoint_count > count) {
            throw std::logic_error(
                "OOCRelationWriter recovery: checkpoint receipt exceeds validated prefix");
        }
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
            if (record_size_u64 >
                static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max())) {
                throw std::overflow_error(
                    "OOCRelationWriter resume: record size exceeds streamsize");
            }
            const size_t record_size = static_cast<size_t>(record_size_u64);
            record.resize(record_size);
            data.clear();
            data.seekg(static_cast<std::streamoff>(offsets[i]));
            if (!data) {
                throw std::runtime_error("OOCRelationWriter resume: cannot seek relation record");
            }
            data.read(reinterpret_cast<char*>(record.data()),
                      static_cast<std::streamsize>(record_size));
            if (data.gcount() != static_cast<std::streamsize>(record_size)) {
                throw std::runtime_error("OOCRelationWriter resume: relation record truncated");
            }
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

    static OOCValidatedResumePrefix
    validate_finalized_prefix(const std::string& base_path,
                              const OOCSnapshotDescriptor& descriptor) {
        std::ifstream index(base_path + ".relidx", std::ios::binary);
        std::ifstream data(base_path + ".reldata", std::ios::binary);
        if (!index || !data) {
            throw std::runtime_error("OOCRelationWriter recovery: finalized store is incomplete");
        }

        constexpr const char* operation = "finalized recovery validation";
        const uint64_t index_size = stream_size_checked(index, operation, "index");
        const uint64_t data_size = stream_size_checked(data, operation, "data");
        if (index_size < INDEX_HEADER_BYTES || data_size < DATA_HEADER_BYTES) {
            throw std::runtime_error("OOCRelationWriter recovery: finalized V3 header truncated");
        }

        if (read_u64_checked(index, operation, "index magic") != MAGIC_V3_FINAL) {
            throw std::runtime_error("OOCRelationWriter recovery: finalized magic changed");
        }
        if (read_u64_checked(index, operation, "index format version") != FORMAT_VERSION_V3) {
            throw std::runtime_error(
                "OOCRelationWriter recovery: finalized format version mismatch");
        }
        const uint64_t finalized_store_id =
            read_u64_checked(index, operation, "index store identity");
        if (finalized_store_id == 0 || finalized_store_id != descriptor.store_id) {
            throw std::runtime_error(
                "OOCRelationWriter recovery: finalized store identity mismatch");
        }
        const uint64_t final_count = read_u64_checked(index, operation, "index count");

        const uint64_t data_magic = read_u64_checked(data, operation, "data magic");
        const uint64_t data_version = read_u64_checked(data, operation, "data version");
        const uint64_t data_store_id = read_u64_checked(data, operation, "data store identity");
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

        std::vector<uint64_t> offsets;
        offsets.reserve(static_cast<size_t>(final_count) + 1);
        for (uint64_t i = 0; i <= final_count; ++i) {
            offsets.push_back(read_u64_checked(index, operation, "offset"));
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
        return validate_records(data, offsets, final_count, data_size, descriptor.count);
    }

    static OOCValidatedResumePrefix
    validate_resume_prefix(const std::string& base_path, const OOCSnapshotDescriptor& descriptor) {
        std::ifstream index(base_path + ".relidx", std::ios::binary);
        if (!index) {
            throw std::runtime_error("OOCRelationWriter resume: idx file not found at " +
                                     base_path);
        }
        std::ifstream data(base_path + ".reldata", std::ios::binary);
        if (!data) {
            throw std::runtime_error("OOCRelationWriter resume: data file not found at " +
                                     base_path);
        }

        constexpr const char* operation = "resume validation";
        const uint64_t index_size = stream_size_checked(index, operation, "index");
        const uint64_t data_size = stream_size_checked(data, operation, "data");
        if (index_size < INDEX_HEADER_BYTES || data_size < DATA_HEADER_BYTES) {
            throw std::runtime_error("OOCRelationWriter resume: V3 file header truncated");
        }

        const uint64_t magic = read_u64_checked(index, operation, "index magic");
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
            read_u64_checked(index, operation, "index format version");
        if (persisted_format_version != FORMAT_VERSION_V3) {
            throw std::runtime_error("OOCRelationWriter resume: format version mismatch");
        }
        const uint64_t persisted_store_id =
            read_u64_checked(index, operation, "index store identity");
        if (persisted_store_id == 0 || persisted_store_id != descriptor.store_id) {
            throw std::runtime_error("OOCRelationWriter resume: store identity mismatch");
        }
        // Count is advisory until final magic commits it. It can be nonzero if
        // the previous process died after the finalize metadata flush; recovery
        // trusts only the paired descriptor and resets this field to zero.
        (void)read_u64_checked(index, operation, "incomplete count");

        const uint64_t data_magic = read_u64_checked(data, operation, "data magic");
        const uint64_t data_version = read_u64_checked(data, operation, "data version");
        const uint64_t data_store_id = read_u64_checked(data, operation, "data store identity");
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

        std::vector<uint64_t> offsets;
        offsets.reserve(static_cast<size_t>(count) + 1);
        for (uint64_t i = 0; i <= count; ++i) {
            offsets.push_back(read_u64_checked(index, operation, "offset"));
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

        return validate_records(data, offsets, count, descriptor.data_end, count);
    }

    void acquire_prefix_reader(const OOCSnapshotDescriptor& descriptor) {
        require_state(OOCWriterState::Suspended, "acquire_prefix_reader");
        if (!suspended_descriptor_ || descriptor != *suspended_descriptor_) {
            throw std::invalid_argument(
                "OOCRelationWriter::acquire_prefix_reader: stale or foreign descriptor");
        }
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
        data_stream_.write(reinterpret_cast<const char*>(&v), sizeof(T));
    }

    void write_vec32(const std::vector<uint32_t>& v) {
        auto sz = static_cast<uint32_t>(v.size());
        write_val(sz);
        if (sz > 0) {
            data_stream_.write(reinterpret_cast<const char*>(v.data()),
                               static_cast<std::streamsize>(sz * sizeof(uint32_t)));
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
        const auto data_position = data_stream_.tellp();
        const auto index_position = idx_stream_.tellp();
        if (data_position == std::streampos(-1) || index_position == std::streampos(-1)) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": cannot preserve stream positions");
        }

        data_stream_.flush();
        idx_stream_.flush();
        ensure_streams_good(operation);
        data_stream_.clear();
        idx_stream_.clear();
        data_stream_.seekg(0);
        idx_stream_.seekg(0);
        if (!data_stream_ || !idx_stream_) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": cannot seek V3 headers");
        }

        const uint64_t index_magic = read_u64_checked(idx_stream_, operation, "index magic");
        const uint64_t index_version = read_u64_checked(idx_stream_, operation, "index version");
        const uint64_t index_store_id =
            read_u64_checked(idx_stream_, operation, "index store identity");
        const uint64_t persisted_count = read_u64_checked(idx_stream_, operation, "index count");
        const uint64_t data_magic = read_u64_checked(data_stream_, operation, "data magic");
        const uint64_t data_version = read_u64_checked(data_stream_, operation, "data version");
        const uint64_t data_store_id =
            read_u64_checked(data_stream_, operation, "data store identity");

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

        data_stream_.clear();
        idx_stream_.clear();
        data_stream_.seekp(data_position);
        idx_stream_.seekp(index_position);
        ensure_streams_good(operation);
    }

    void require_state(OOCWriterState expected, const char* operation) const {
        if (state_ != expected) {
            throw std::logic_error(std::string("OOCRelationWriter::") + operation +
                                   ": invalid writer state");
        }
    }

    void ensure_streams_good(const char* operation) const {
        if (!data_stream_ || !idx_stream_) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": stream I/O failed");
        }
    }

    void ensure_open_streams_good(const char* operation) const {
        if ((data_stream_.is_open() && !data_stream_) || (idx_stream_.is_open() && !idx_stream_)) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": stream I/O failed");
        }
    }

    void close_streams_checked(const char* operation) {
        data_stream_.close();
        idx_stream_.close();
        if (data_stream_.fail() || idx_stream_.fail()) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": stream close failed");
        }
    }

    void close_open_streams_checked(const char* operation) {
        if (data_stream_.is_open())
            data_stream_.close();
        if (idx_stream_.is_open())
            idx_stream_.close();
        if (data_stream_.fail() || idx_stream_.fail()) {
            throw std::runtime_error(std::string("OOCRelationWriter::") + operation +
                                     ": stream close failed");
        }
    }

    void abort_close_noexcept() noexcept {
        if (data_stream_.is_open())
            data_stream_.close();
        if (idx_stream_.is_open())
            idx_stream_.close();
    }

    void fail_and_close_noexcept() noexcept {
        state_ = OOCWriterState::Failed;
        abort_close_noexcept();
    }

    [[nodiscard]] static std::runtime_error
    sync_error(const char* operation, const std::filesystem::path& path, int error_number) {
        return std::runtime_error(std::string("OOCRelationWriter::") + operation + " " +
                                  path.string() + ": " + std::strerror(error_number));
    }

    static void sync_file(const std::filesystem::path& path) {
#ifdef _WIN32
        const int fd = ::_wopen(path.c_str(), _O_RDWR | _O_BINARY);
        if (fd < 0) {
            throw sync_error("sync cannot open", path, errno);
        }
        if (::_commit(fd) != 0) {
            const int saved_errno = errno;
            ::_close(fd);
            throw sync_error("sync failed", path, saved_errno);
        }
        if (::_close(fd) != 0) {
            throw sync_error("sync close failed", path, errno);
        }
#else
        int fd = -1;
        do {
            fd = ::open(path.c_str(), O_RDWR);
        } while (fd < 0 && errno == EINTR);
        if (fd < 0) {
            throw sync_error("sync cannot open", path, errno);
        }

        int result = -1;
        do {
            result = ::fsync(fd);
        } while (result != 0 && errno == EINTR);
        if (result != 0) {
            const int saved_errno = errno;
            ::close(fd);
            throw sync_error("sync failed", path, saved_errno);
        }
        if (::close(fd) != 0) {
            throw sync_error("sync close failed", path, errno);
        }
#endif
    }

    void sync_store_files_and_directory() const {
        const std::filesystem::path data_path(base_path_ + ".reldata");
        const std::filesystem::path index_path(base_path_ + ".relidx");
        sync_file(data_path);
        sync_file(index_path);
        detail::sync_parent_directory_after_metadata_change(index_path);
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

    std::string base_path_;
    std::vector<char> data_buf_;
    std::vector<char> idx_buf_;
    // fstream (not ofstream) to support resume mode: r/w open without trunc,
    // seek to past existing offsets/data.
    std::fstream data_stream_;
    std::fstream idx_stream_;
    size_t count_ = 0;
    int uncaught_at_ctor_ = 0;
    uint64_t store_id_ = allocate_store_id();
    uint64_t generation_ = 0;
    bool fresh_store_ = false;
    bool fresh_artifacts_removed_ = false;
    std::optional<OOCCleanupOwnershipReceipt> cleanup_receipt_;
    OOCWriterState state_ = OOCWriterState::Open;
    std::optional<OOCSnapshotDescriptor> suspended_descriptor_;
    std::optional<OOCSnapshotDescriptor> finalized_descriptor_;
    bool finalized_durable_ = false;
    std::optional<OOCValidatedResumePrefix> validated_resume_prefix_;
    OOCRecoveryOutcome recovery_outcome_ = OOCRecoveryOutcome::None;
    size_t active_prefix_readers_ = 0;

    friend class OOCRelationPrefixReader;
};

/// Read-only mmap-based access to out-of-core relations.
///
/// Maps both .relidx and .reldata files into memory.
/// Provides O(1) access to any relation by index.
class OOCRelationReader {
public:
    OOCRelationReader() = default;

    explicit OOCRelationReader(const std::string& base_path)
        : idx_file_(base_path + ".relidx"), data_file_(base_path + ".reldata") {
        initialize(nullptr);
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
        initialize(&expected);
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
            // Validate ownership and the Suspended state before opening either file.
            // On Windows, mmap must never race an open std::fstream writer handle.
            idx_file_ = gnfs::util::MmapFile(frozen_base_path + ".relidx");
            data_file_ = gnfs::util::MmapFile(frozen_base_path + ".reldata");
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
        // The owner may reopen append handles as soon as the lease is released,
        // so mappings must be closed first on every platform.
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

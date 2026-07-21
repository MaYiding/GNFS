#pragma once

#include "gnfs/core/relation.hpp"
#include "gnfs/relation/large_prime_key.hpp"
#include "gnfs/util/mmap_file.hpp"
#include "gnfs/util/process.hpp"
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
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
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
/// `store_id` is persisted in an incomplete V2 index header, so a checkpoint
/// can reject a different store across process restarts. `generation` rejects
/// stale in-process handoffs and is restored from the paired sieve checkpoint;
/// `count`/`data_end` describe the committed on-disk prefix.
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
    uint64_t full_relations = 0;
    uint64_t partial_1lp = 0;
    uint64_t partial_2lp = 0;
};

class OOCRelationPrefixReader;

namespace detail {

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
///   V2 .relidx: [magic][version][store_id][count][offset_0][offset_1]...
///   legacy finalized V1: [legacy magic][count][offset_0][offset_1]...
///   .reldata: [serialized_relation_0][serialized_relation_1]...
///
/// Each serialized relation uses a compact binary format (not the v2 checksum format,
/// which has overhead). Fields are written in order with explicit length prefixes.
///
/// For 25-digit (~10K relations, ~2MB): works but overkill.
/// For 50+ digit (~10M relations, ~2-5GB): essential to avoid OOM.
class OOCRelationWriter {
public:
    // New writes use an expanded V2 header that never overwrites store_id.
    // Readers retain compatibility with the historical finalized V1 layout.
    static constexpr uint64_t MAGIC_V1_FINAL = 0x474E46535245494CULL; // 'GNFSREIL'
    static constexpr uint64_t MAGIC_INCOMPLETE_V1 = 0x474E46535245494EULL;
    static constexpr uint64_t MAGIC_V2_FINAL = 0x474E46535232464CULL;   // 'GNFSR2FL'
    static constexpr uint64_t MAGIC_INCOMPLETE = 0x474E46535232494EULL; // 'GNFSR2IN'
    static constexpr uint64_t MAGIC = MAGIC_V2_FINAL;
    static constexpr uint64_t FORMAT_VERSION = 2;

    static constexpr uint64_t INDEX_FORMAT_VERSION_OFFSET = 8;
    static constexpr uint64_t INDEX_STORE_ID_OFFSET = 16;
    static constexpr uint64_t INDEX_COUNT_OFFSET = 24;
    static constexpr uint64_t INDEX_HEADER_BYTES = 32;
    static constexpr uint64_t INDEX_SENTINEL_BYTES = 8;

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

    /// Fresh create (default) writes an incomplete V2 header with a durable
    /// store identity. Paired recovery requires an explicit descriptor from a
    /// validated SieveCheckpoint V2; no bare "resume whatever is present" path
    /// is accepted.
    explicit OOCRelationWriter(
        const std::string& base_path,
        std::optional<OOCSnapshotDescriptor> recovery_descriptor = std::nullopt)
        : base_path_(base_path), data_buf_(BUFFER_BYTES),
          idx_buf_(BUFFER_BYTES / 4), // 256 KB suffices for index
          uncaught_at_ctor_(std::uncaught_exceptions()) {
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
                // A clean pipeline end finalizes the corpus before deleting the
                // paired sieve checkpoint. A crash in that window must never
                // turn the immutable corpus back into an appendable one. The
                // finalized V2 header retains store_id, and validation also
                // proves that the paired count/data boundary is an exact prefix.
                validated_resume_prefix_ =
                    validate_finalized_prefix(base_path, *recovery_descriptor);
                store_id_ = recovery_descriptor->store_id;
                generation_ = recovery_descriptor->generation;
                count_ = static_cast<size_t>(validated_resume_prefix_->count);

                OOCSnapshotDescriptor final_descriptor = *recovery_descriptor;
                final_descriptor.count = validated_resume_prefix_->count;
                final_descriptor.data_end = validated_resume_prefix_->data_end;
                finalized_descriptor_ = final_descriptor;
                state_ = OOCWriterState::Finalized;
                recovery_outcome_ = OOCRecoveryOutcome::FinalizedCorpus;
                return;
            }
            validated_resume_prefix_ = validate_resume_prefix(base_path, *recovery_descriptor);

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

            // Reopen in r/w mode (no trunc), seek streams past existing content.
            data_stream_.open(base_path + ".reldata",
                              std::ios::in | std::ios::out | std::ios::binary);
            idx_stream_.open(base_path + ".relidx",
                             std::ios::in | std::ios::out | std::ios::binary);
            if (!data_stream_ || !idx_stream_) {
                throw std::runtime_error("OOCRelationWriter resume: cannot reopen at " + base_path);
            }
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
            // Fresh create: trunc + write V2 INCOMPLETE header.
            data_stream_.open(base_path + ".reldata",
                              std::ios::in | std::ios::out | std::ios::trunc | std::ios::binary);
            idx_stream_.open(base_path + ".relidx",
                             std::ios::in | std::ios::out | std::ios::trunc | std::ios::binary);
            if (!data_stream_ || !idx_stream_) {
                throw std::runtime_error("OOCRelationWriter: cannot open files at " + base_path);
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
            ensure_streams_good("constructor header write");
        }
    }

    /// The historical bool overload remains only to fail closed at runtime.
    /// In particular, `true` throws before either store file is opened or
    /// truncated. Passing `false` is equivalent to fresh construction.
    explicit OOCRelationWriter(const std::string& base_path, bool legacy_resume)
        : OOCRelationWriter(base_path, reject_legacy_resume(legacy_resume)) {}

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
            data_stream_.flush();
            ensure_streams_good("checkpoint_prefix data flush");

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
            if (state_ == OOCWriterState::Open) {
                data_stream_.flush();
                ensure_streams_good("finalize data flush");

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
                idx_stream_.clear();
                idx_stream_.open(base_path_ + ".relidx",
                                 std::ios::in | std::ios::out | std::ios::binary);
                if (!idx_stream_) {
                    throw std::runtime_error(
                        "OOCRelationWriter::finalize: cannot reopen index at " + base_path_);
                }
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

            idx_stream_.clear();
            idx_stream_.open(base_path_ + ".relidx",
                             std::ios::in | std::ios::out | std::ios::binary);
            if (!idx_stream_) {
                throw std::runtime_error(
                    "OOCRelationWriter::finalize: cannot reopen index for final magic at " +
                    base_path_);
            }
            idx_stream_.seekp(0);
            const uint64_t final_magic = MAGIC_V2_FINAL;
            idx_stream_.write(reinterpret_cast<const char*>(&final_magic), 8);
            idx_stream_.flush();
            ensure_open_streams_good("finalize magic flush");
            close_open_streams_checked("finalize close");
            sync_store_files_and_directory();
            if (hook != nullptr) {
                hook(FinalizeStage::FinalMagicDurable);
            }

            suspended_descriptor_.reset();
            finalized_descriptor_ = descriptor;
            state_ = OOCWriterState::Finalized;
            return descriptor;
        } catch (...) {
            fail_and_close_noexcept();
            throw;
        }
    }

    /// Compatibility alias for existing callers.
    void close() {
        (void)finalize();
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
    static std::optional<OOCSnapshotDescriptor> reject_legacy_resume(bool legacy_resume) {
        if (legacy_resume) {
            throw std::invalid_argument("OOCRelationWriter: bare resume is unsupported; a paired "
                                        "V2 descriptor is required");
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
        if (descriptor.data_end >
            static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            throw std::overflow_error("OOCRelationWriter recovery: data position overflow");
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

    static OOCValidatedResumePrefix validate_records(std::ifstream& data,
                                                     const std::vector<uint64_t>& offsets,
                                                     uint64_t count, uint64_t data_end) {
        OOCValidatedResumePrefix prefix;
        prefix.count = count;
        prefix.data_end = data_end;
        prefix.seen.reserve(static_cast<size_t>(count));

        std::vector<uint8_t> record;
        for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
            const uint64_t record_size_u64 = offsets[i + 1] - offsets[i];
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

    static OOCValidatedResumePrefix
    validate_finalized_prefix(const std::string& base_path,
                              const OOCSnapshotDescriptor& descriptor) {
        std::ifstream index(base_path + ".relidx", std::ios::binary);
        std::ifstream data(base_path + ".reldata", std::ios::binary);
        if (!index || !data) {
            throw std::runtime_error("OOCRelationWriter recovery: finalized store is incomplete");
        }

        const auto file_size = [](std::ifstream& stream, const char* field) -> uint64_t {
            stream.seekg(0, std::ios::end);
            const auto position = stream.tellg();
            if (position == std::streampos(-1)) {
                throw std::runtime_error(std::string("OOCRelationWriter recovery: cannot size ") +
                                         field);
            }
            stream.clear();
            stream.seekg(0);
            return static_cast<uint64_t>(static_cast<std::streamoff>(position));
        };
        const auto read_u64 = [](std::ifstream& stream, const char* field) -> uint64_t {
            uint64_t value = 0;
            stream.read(reinterpret_cast<char*>(&value), sizeof(value));
            if (stream.gcount() != static_cast<std::streamsize>(sizeof(value))) {
                throw std::runtime_error(std::string("OOCRelationWriter recovery: truncated ") +
                                         field);
            }
            return value;
        };

        const uint64_t index_size = file_size(index, "finalized index");
        const uint64_t data_size = file_size(data, "finalized data");
        if (read_u64(index, "finalized magic") != MAGIC_V2_FINAL) {
            throw std::runtime_error("OOCRelationWriter recovery: finalized magic changed");
        }
        if (read_u64(index, "finalized format version") != FORMAT_VERSION) {
            throw std::runtime_error(
                "OOCRelationWriter recovery: finalized format version mismatch");
        }
        const uint64_t finalized_store_id = read_u64(index, "finalized store identity");
        if (finalized_store_id == 0 || finalized_store_id != descriptor.store_id) {
            throw std::runtime_error(
                "OOCRelationWriter recovery: finalized store identity mismatch");
        }
        const uint64_t final_count = read_u64(index, "finalized count");
        if (final_count < descriptor.count) {
            throw std::runtime_error(
                "OOCRelationWriter recovery: finalized corpus predates checkpoint prefix");
        }
        if (index_size != index_size_for_count(final_count)) {
            throw std::runtime_error("OOCRelationWriter recovery: finalized index size mismatch");
        }

        std::vector<uint64_t> offsets;
        offsets.reserve(static_cast<size_t>(final_count) + 1);
        for (uint64_t i = 0; i <= final_count; ++i) {
            offsets.push_back(read_u64(index, "finalized offset"));
        }
        if (offsets.front() != 0 || offsets.back() != data_size) {
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
        return validate_records(data, offsets, final_count, data_size);
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

        const auto file_size = [](std::ifstream& stream, const char* field) -> uint64_t {
            stream.seekg(0, std::ios::end);
            const auto position = stream.tellg();
            if (position == std::streampos(-1)) {
                throw std::runtime_error(std::string("OOCRelationWriter resume: cannot size ") +
                                         field);
            }
            const auto offset = static_cast<std::streamoff>(position);
            if (offset < 0) {
                throw std::runtime_error(std::string("OOCRelationWriter resume: negative ") +
                                         field + " size");
            }
            stream.clear();
            stream.seekg(0);
            return static_cast<uint64_t>(offset);
        };

        const uint64_t index_size = file_size(index, "index");
        const uint64_t data_size = file_size(data, "data");
        if (index_size < INDEX_HEADER_BYTES) {
            throw std::runtime_error("OOCRelationWriter resume: idx file too small");
        }

        const auto read_u64 = [](std::ifstream& stream, const char* field) -> uint64_t {
            uint64_t value = 0;
            stream.read(reinterpret_cast<char*>(&value), sizeof(value));
            if (stream.gcount() != static_cast<std::streamsize>(sizeof(value))) {
                throw std::runtime_error(std::string("OOCRelationWriter resume: truncated ") +
                                         field);
            }
            return value;
        };

        const uint64_t magic = read_u64(index, "magic");
        if (magic == MAGIC_V1_FINAL) {
            throw std::runtime_error(
                "OOCRelationWriter resume: legacy finalized V1 store is unsupported");
        }
        if (magic == MAGIC_V2_FINAL) {
            throw std::runtime_error("OOCRelationWriter resume: file already finalized (V2)");
        }
        if (magic == MAGIC_INCOMPLETE_V1) {
            throw std::runtime_error(
                "OOCRelationWriter resume: legacy incomplete V1 store is unsupported");
        }
        if (magic != MAGIC_INCOMPLETE) {
            throw std::runtime_error("OOCRelationWriter resume: invalid magic in idx (corrupt?)");
        }

        const uint64_t persisted_format_version = read_u64(index, "format version");
        if (persisted_format_version != FORMAT_VERSION) {
            throw std::runtime_error("OOCRelationWriter resume: format version mismatch");
        }
        const uint64_t persisted_store_id = read_u64(index, "store identity");
        if (persisted_store_id == 0 || persisted_store_id != descriptor.store_id) {
            throw std::runtime_error("OOCRelationWriter resume: store identity mismatch");
        }
        // Count is advisory until final magic commits it. It can be nonzero if
        // the previous process died after the finalize metadata flush; recovery
        // trusts only the paired descriptor and resets this field to zero.
        (void)read_u64(index, "incomplete count");

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
            offsets.push_back(read_u64(index, "offset"));
        }
        if (offsets.front() != 0) {
            throw std::runtime_error("OOCRelationWriter resume: first offset is not zero");
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

        return validate_records(data, offsets, count, descriptor.data_end);
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

    static void sync_parent_directory(const std::filesystem::path& file_path) {
#ifndef _WIN32
        auto parent = file_path.parent_path();
        if (parent.empty()) {
            parent = ".";
        }
        int fd = -1;
        do {
            fd = ::open(parent.c_str(), O_RDONLY);
        } while (fd < 0 && errno == EINTR);
        if (fd < 0) {
            throw sync_error("directory sync cannot open", parent, errno);
        }

        int result = -1;
        do {
            result = ::fsync(fd);
        } while (result != 0 && errno == EINTR);
        if (result != 0) {
            const int saved_errno = errno;
            ::close(fd);
            throw sync_error("directory sync failed", parent, saved_errno);
        }
        if (::close(fd) != 0) {
            throw sync_error("directory sync close failed", parent, errno);
        }
#else
        (void)file_path;
#endif
    }

    void sync_store_files_and_directory() const {
        const std::filesystem::path data_path(base_path_ + ".reldata");
        const std::filesystem::path index_path(base_path_ + ".relidx");
        sync_file(data_path);
        sync_file(index_path);
        sync_parent_directory(index_path);
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
    OOCWriterState state_ = OOCWriterState::Open;
    std::optional<OOCSnapshotDescriptor> suspended_descriptor_;
    std::optional<OOCSnapshotDescriptor> finalized_descriptor_;
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

        // Validate either the expanded V2 finalized header or the historical
        // finalized V1 header. Incomplete files are rejected in both formats.
        if (idx_file_.size() < 16) {
            throw std::runtime_error("OOCRelationReader: index file too small");
        }
        const uint64_t magic = idx_file_.read_at<uint64_t>(0);
        uint64_t stored_count = 0;
        size_t index_header_bytes = 0;
        if (magic == OOCRelationWriter::MAGIC_V2_FINAL) {
            if (idx_file_.size() < OOCRelationWriter::INDEX_HEADER_BYTES) {
                throw std::runtime_error("OOCRelationReader: V2 index header truncated");
            }
            if (idx_file_.read_at<uint64_t>(OOCRelationWriter::INDEX_FORMAT_VERSION_OFFSET) !=
                OOCRelationWriter::FORMAT_VERSION) {
                throw std::runtime_error("OOCRelationReader: V2 format version mismatch");
            }
            if (idx_file_.read_at<uint64_t>(OOCRelationWriter::INDEX_STORE_ID_OFFSET) == 0) {
                throw std::runtime_error("OOCRelationReader: V2 store identity is zero");
            }
            stored_count = idx_file_.read_at<uint64_t>(OOCRelationWriter::INDEX_COUNT_OFFSET);
            index_header_bytes = static_cast<size_t>(OOCRelationWriter::INDEX_HEADER_BYTES);
        } else if (magic == OOCRelationWriter::MAGIC_V1_FINAL) {
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
        const size_t expected_idx = index_header_bytes +
                                    static_cast<size_t>(OOCRelationWriter::INDEX_SENTINEL_BYTES) +
                                    count_ * sizeof(uint64_t);
        if (idx_file_.size() != expected_idx) {
            throw std::runtime_error("OOCRelationReader: index size does not match relation count");
        }

        offsets_ = idx_file_.ptr_at<uint64_t>(index_header_bytes);
        if (offsets_[0] != 0) {
            throw std::runtime_error("OOCRelationReader: first offset is not zero");
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
        if (descriptor.store_id == 0 || owner.base_path() != base_path ||
            !owner.owns_suspended_prefix(descriptor)) {
            throw std::invalid_argument(
                "OOCRelationPrefixReader: foreign or stale snapshot descriptor");
        }

        owner.acquire_prefix_reader(descriptor);
        try {
            // Validate ownership and the Suspended state before opening either file.
            // On Windows, mmap must never race an open std::fstream writer handle.
            idx_file_ = gnfs::util::MmapFile(base_path + ".relidx");
            data_file_ = gnfs::util::MmapFile(base_path + ".reldata");
            if (idx_file_.size() < OOCRelationWriter::INDEX_HEADER_BYTES) {
                throw std::runtime_error("OOCRelationPrefixReader: index file too small");
            }
            const uint64_t magic = idx_file_.read_at<uint64_t>(0);
            if (magic != OOCRelationWriter::MAGIC_INCOMPLETE) {
                throw std::runtime_error("OOCRelationPrefixReader: store is not incomplete");
            }
            if (descriptor_.format_version != OOCRelationWriter::FORMAT_VERSION ||
                idx_file_.read_at<uint64_t>(OOCRelationWriter::INDEX_FORMAT_VERSION_OFFSET) !=
                    OOCRelationWriter::FORMAT_VERSION ||
                idx_file_.read_at<uint64_t>(OOCRelationWriter::INDEX_STORE_ID_OFFSET) !=
                    descriptor_.store_id) {
                throw std::runtime_error(
                    "OOCRelationPrefixReader: store identity or format mismatch");
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
            if (count_ == 0) {
                if (descriptor.data_end != 0) {
                    throw std::runtime_error("OOCRelationPrefixReader: non-zero empty prefix");
                }
            } else {
                if (offsets_[0] != 0) {
                    throw std::runtime_error("OOCRelationPrefixReader: first offset is not zero");
                }
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
            owner.release_prefix_reader();
            owner_ = nullptr;
            throw;
        }
    }

    ~OOCRelationPrefixReader() {
        if (owner_ != nullptr)
            owner_->release_prefix_reader();
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

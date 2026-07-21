#pragma once

#include "gnfs/core/relation.hpp"
#include "gnfs/relation/large_prime_key.hpp"
#include "gnfs/util/mmap_file.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gnfs::relation {

enum class OOCWriterState {
    Open,
    Suspended,
    Finalized,
    Failed,
};

/// Stable description of a flushed relation prefix.
///
/// The descriptor is process-local in this first implementation: `store_id`
/// rejects a descriptor issued by another live writer, `generation` rejects a
/// stale descriptor from the same writer, and `count`/`data_end` describe the
/// validated on-disk boundary without accepting a generally incomplete store.
struct OOCSnapshotDescriptor {
    uint64_t store_id = 0;
    uint64_t generation = 0;
    uint64_t count = 0;
    uint64_t data_end = 0;

    friend bool operator==(const OOCSnapshotDescriptor&, const OOCSnapshotDescriptor&) = default;
};

/// Fully validated V1 resume prefix. This is process-local handoff data: the
/// writer constructs it only after validating the complete compact records,
/// their index boundaries, and the exact data-file extent.
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
///   .relidx: [uint64_t magic][uint64_t count][uint64_t offset_0][uint64_t offset_1]...
///   .reldata: [serialized_relation_0][serialized_relation_1]...
///
/// Each serialized relation uses a compact binary format (not the v2 checksum format,
/// which has overhead). Fields are written in order with explicit length prefixes.
///
/// For 25-digit (~10K relations, ~2MB): works but overkill.
/// For 50+ digit (~10M relations, ~2-5GB): essential to avoid OOM.
class OOCRelationWriter {
public:
    // MAGIC = 'GNFSREIL' (written only after successful finalize)
    // MAGIC_INCOMPLETE = 'GNFSREIN' (written on construction; reader rejects)
    static constexpr uint64_t MAGIC = 0x474E46535245494CULL;
    static constexpr uint64_t MAGIC_INCOMPLETE = 0x474E46535245494EULL;

    // 1 MB stream buffer per stream — 千万级关系下减少 syscall。
    static constexpr size_t BUFFER_BYTES = 1 << 20;

    /// Fresh create (default): truncates existing files, writes INCOMPLETE
    /// header. Resume mode (resume=true): opens existing .reldata/.relidx
    /// in r/w mode (no trunc), reads prior count, seeks streams past existing
    /// content. Requires existing idx magic = MAGIC_INCOMPLETE (i.e., prior
    /// session didn't finalize) — finalized files are immutable.
    explicit OOCRelationWriter(const std::string& base_path, bool resume = false)
        : base_path_(base_path), data_buf_(BUFFER_BYTES),
          idx_buf_(BUFFER_BYTES / 4), // 256 KB suffices for index
          uncaught_at_ctor_(std::uncaught_exceptions()) {
        // pubsetbuf 必须在 open 之前调用,所以 fstream 默认构造、
        // 然后手动 attach buffer、最后 open。
        data_stream_.rdbuf()->pubsetbuf(data_buf_.data(),
                                        static_cast<std::streamsize>(data_buf_.size()));
        idx_stream_.rdbuf()->pubsetbuf(idx_buf_.data(),
                                       static_cast<std::streamsize>(idx_buf_.size()));

        if (resume) {
            // V1 resume is intentionally fail-closed. Only a complete prefix
            // with an exact count+1 sentinel index and fully decodable compact
            // records may be reopened. Durable crash checkpoints with separate
            // generation metadata belong to SieveCheckpoint V2.
            validated_resume_prefix_ = validate_resume_prefix(base_path);
            count_ = static_cast<size_t>(validated_resume_prefix_->count);

            // Reopen in r/w mode (no trunc), seek streams past existing content.
            data_stream_.open(base_path + ".reldata",
                              std::ios::in | std::ios::out | std::ios::binary);
            idx_stream_.open(base_path + ".relidx",
                             std::ios::in | std::ios::out | std::ios::binary);
            if (!data_stream_ || !idx_stream_) {
                throw std::runtime_error("OOCRelationWriter resume: cannot reopen at " + base_path);
            }
            // Overwrite the validated sentinel with the first new relation's
            // offset. The data cursor is exactly at the validated file end.
            data_stream_.seekp(static_cast<std::streamoff>(validated_resume_prefix_->data_end));
            if (count_ >
                (static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) - 16) / 8) {
                throw std::overflow_error("OOCRelationWriter resume: index position overflow");
            }
            const uint64_t idx_pos = 16 + static_cast<uint64_t>(count_) * 8;
            idx_stream_.seekp(static_cast<std::streamoff>(idx_pos));
            ensure_streams_good("resume constructor seek");
        } else {
            // Fresh create: trunc + write INCOMPLETE header.
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
            uint64_t magic = MAGIC_INCOMPLETE;
            uint64_t count = 0;
            idx_stream_.write(reinterpret_cast<const char*>(&magic), 8);
            idx_stream_.write(reinterpret_cast<const char*>(&count), 8);
            ensure_streams_good("constructor header write");
        }
    }

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

            OOCSnapshotDescriptor descriptor;
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
            if (descriptor.count > (std::numeric_limits<uint64_t>::max() - 16) / 8) {
                throw std::overflow_error(
                    "OOCRelationWriter::resume_append: index position overflow");
            }
            const uint64_t idx_pos = 16 + descriptor.count * 8;
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
    [[nodiscard]] OOCSnapshotDescriptor finalize() {
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
            idx_stream_.seekp(8);
            const uint64_t final_count = descriptor.count;
            idx_stream_.write(reinterpret_cast<const char*>(&final_count), 8);

            if (data_stream_.is_open())
                data_stream_.flush();
            idx_stream_.flush();
            ensure_open_streams_good("finalize metadata flush");

            idx_stream_.seekp(0);
            const uint64_t final_magic = MAGIC;
            idx_stream_.write(reinterpret_cast<const char*>(&final_magic), 8);
            idx_stream_.flush();
            ensure_open_streams_good("finalize magic flush");
            close_open_streams_checked("finalize close");

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
    static OOCValidatedResumePrefix validate_resume_prefix(const std::string& base_path) {
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
        if (index_size < 16) {
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
        if (magic == MAGIC) {
            throw std::runtime_error("OOCRelationWriter resume: file already finalized (MAGIC)");
        }
        if (magic != MAGIC_INCOMPLETE) {
            throw std::runtime_error("OOCRelationWriter resume: invalid magic in idx (corrupt?)");
        }

        const uint64_t count = read_u64(index, "count");
        if (count >= static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            throw std::overflow_error(
                "OOCRelationWriter resume: count leaves no room for sentinel");
        }
        if (count > (std::numeric_limits<uint64_t>::max() - 24) / 8) {
            throw std::overflow_error("OOCRelationWriter resume: index size overflow");
        }
        const uint64_t expected_index_size = 24 + count * 8;
        if (index_size != expected_index_size) {
            throw std::runtime_error("OOCRelationWriter resume: index size does not match count");
        }

        std::vector<uint64_t> offsets;
        offsets.reserve(static_cast<size_t>(count) + 1);
        for (uint64_t i = 0; i <= count; ++i) {
            offsets.push_back(read_u64(index, "offset"));
        }
        if (offsets.front() != 0) {
            throw std::runtime_error("OOCRelationWriter resume: first offset is not zero");
        }
        if (offsets.back() != data_size) {
            throw std::runtime_error(
                "OOCRelationWriter resume: final sentinel does not match data size");
        }
        for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
            if (offsets[i] >= offsets[i + 1] || offsets[i + 1] > data_size) {
                throw std::runtime_error(
                    "OOCRelationWriter resume: non-monotonic or out-of-range offset");
            }
        }

        OOCValidatedResumePrefix prefix;
        prefix.count = count;
        prefix.data_end = data_size;
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

    static uint64_t allocate_store_id() noexcept {
        static std::atomic<uint64_t> next_id{1};
        return next_id.fetch_add(1, std::memory_order_relaxed);
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

        // Validate index header
        if (idx_file_.size() < 16) {
            throw std::runtime_error("OOCRelationReader: index file too small");
        }
        uint64_t magic = idx_file_.read_at<uint64_t>(0);
        if (magic != OOCRelationWriter::MAGIC) {
            throw std::runtime_error("OOCRelationReader: invalid magic in index");
        }
        const uint64_t stored_count = idx_file_.read_at<uint64_t>(8);
        if (stored_count > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            throw std::overflow_error("OOCRelationReader: relation count exceeds size_t");
        }
        count_ = static_cast<size_t>(stored_count);

        // Index should have: 16 bytes header + (count+1) × 8 bytes offsets
        if (count_ > (std::numeric_limits<size_t>::max() - 24) / 8) {
            throw std::overflow_error("OOCRelationReader: index size overflow");
        }
        const size_t expected_idx = 24 + count_ * 8;
        if (idx_file_.size() != expected_idx) {
            throw std::runtime_error("OOCRelationReader: index size does not match relation count");
        }

        offsets_ = idx_file_.ptr_at<uint64_t>(16);
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
            if (idx_file_.size() < 16) {
                throw std::runtime_error("OOCRelationPrefixReader: index file too small");
            }
            const uint64_t magic = idx_file_.read_at<uint64_t>(0);
            if (magic != OOCRelationWriter::MAGIC_INCOMPLETE) {
                throw std::runtime_error("OOCRelationPrefixReader: store is not incomplete");
            }
            if (descriptor.count > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
                throw std::overflow_error("OOCRelationPrefixReader: relation count exceeds size_t");
            }
            count_ = static_cast<size_t>(descriptor.count);
            if (count_ > (std::numeric_limits<size_t>::max() - 24) / 8) {
                throw std::overflow_error("OOCRelationPrefixReader: index size overflow");
            }
            const size_t expected_idx = 24 + count_ * 8;
            if (idx_file_.size() != expected_idx) {
                throw std::runtime_error(
                    "OOCRelationPrefixReader: index size does not match snapshot");
            }
            if (descriptor.data_end != static_cast<uint64_t>(data_file_.size())) {
                throw std::runtime_error(
                    "OOCRelationPrefixReader: data size does not match snapshot");
            }

            offsets_ = idx_file_.ptr_at<uint64_t>(16);
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

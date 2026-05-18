#pragma once

#include "gnfs/core/relation.hpp"
#include "gnfs/util/mmap_file.hpp"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace gnfs::relation {

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
        : base_path_(base_path),
          data_buf_(BUFFER_BYTES),
          idx_buf_(BUFFER_BYTES / 4),  // 256 KB suffices for index
          uncaught_at_ctor_(std::uncaught_exceptions()) {
        // pubsetbuf 必须在 open 之前调用,所以 fstream 默认构造、
        // 然后手动 attach buffer、最后 open。
        data_stream_.rdbuf()->pubsetbuf(data_buf_.data(),
                                        static_cast<std::streamsize>(data_buf_.size()));
        idx_stream_.rdbuf()->pubsetbuf(idx_buf_.data(),
                                       static_cast<std::streamsize>(idx_buf_.size()));

        if (resume) {
            // Pre-validate .relidx: existence + magic = INCOMPLETE + read count_.
            {
                std::ifstream check_idx(base_path + ".relidx", std::ios::binary);
                if (!check_idx) {
                    throw std::runtime_error(
                        "OOCRelationWriter resume: idx file not found at " + base_path);
                }
                uint64_t existing_magic = 0;
                check_idx.read(reinterpret_cast<char*>(&existing_magic), 8);
                if (check_idx.gcount() != 8) {
                    throw std::runtime_error(
                        "OOCRelationWriter resume: idx file too small");
                }
                if (existing_magic == MAGIC) {
                    throw std::runtime_error(
                        "OOCRelationWriter resume: file already finalized (MAGIC)");
                }
                if (existing_magic != MAGIC_INCOMPLETE) {
                    throw std::runtime_error(
                        "OOCRelationWriter resume: invalid magic in idx (corrupt?)");
                }
                check_idx.read(reinterpret_cast<char*>(&count_), 8);
                if (check_idx.gcount() != 8) {
                    throw std::runtime_error(
                        "OOCRelationWriter resume: count truncated");
                }
            }

            // Reopen in r/w mode (no trunc), seek streams past existing content.
            data_stream_.open(base_path + ".reldata",
                              std::ios::in | std::ios::out | std::ios::binary);
            idx_stream_.open(base_path + ".relidx",
                             std::ios::in | std::ios::out | std::ios::binary);
            if (!data_stream_ || !idx_stream_) {
                throw std::runtime_error(
                    "OOCRelationWriter resume: cannot reopen at " + base_path);
            }
            // data_stream_ → end of file (append point).
            data_stream_.seekp(0, std::ios::end);
            // idx_stream_ → past header (16) + existing offsets array (count_ × 8).
            idx_stream_.seekp(static_cast<std::streamoff>(16 + count_ * 8));
        } else {
            // Fresh create: trunc + write INCOMPLETE header.
            data_stream_.open(base_path + ".reldata",
                              std::ios::in | std::ios::out |
                              std::ios::trunc | std::ios::binary);
            idx_stream_.open(base_path + ".relidx",
                             std::ios::in | std::ios::out |
                             std::ios::trunc | std::ios::binary);
            if (!data_stream_ || !idx_stream_) {
                throw std::runtime_error(
                    "OOCRelationWriter: cannot open files at " + base_path);
            }
            // 先写 INCOMPLETE 标志。若 write 中途抛(磁盘满等),析构跳过
            // finalize → reader 看到 INCOMPLETE 拒读,避免 idx/data 不一致。
            // 成功 close 后再翻成 MAGIC。
            uint64_t magic = MAGIC_INCOMPLETE;
            uint64_t count = 0;
            idx_stream_.write(reinterpret_cast<const char*>(&magic), 8);
            idx_stream_.write(reinterpret_cast<const char*>(&count), 8);
        }
    }

    /// Append a single relation. Returns the index of the written relation.
    size_t write(const gnfs::core::Relation& rel) {
        // Record offset in index
        uint64_t offset = static_cast<uint64_t>(data_stream_.tellp());
        idx_stream_.write(reinterpret_cast<const char*>(&offset), 8);

        // Serialize relation to data file
        serialize(rel);

        count_++;
        return count_ - 1;
    }

    /// Flush and finalize. Updates the count + flips magic to MAGIC.
    void close() {
        if (closed_) return;
        closed_ = true;

        // 异常路径:不写 MAGIC,只 flush 流(让磁盘上的 INCOMPLETE 持久)
        // reader 看到 INCOMPLETE 即拒读。std::uncaught_exceptions() 比
        // std::uncaught_exception() 更可靠(支持嵌套析构)。
        if (std::uncaught_exceptions() > uncaught_at_ctor_) {
            data_stream_.flush();
            idx_stream_.flush();
            data_stream_.close();
            idx_stream_.close();
            return;
        }

        // Write final sentinel offset (= end of data)
        uint64_t end_offset = static_cast<uint64_t>(data_stream_.tellp());
        idx_stream_.write(reinterpret_cast<const char*>(&end_offset), 8);

        // Seek back and update magic + count
        idx_stream_.seekp(0);
        uint64_t final_magic = MAGIC;
        idx_stream_.write(reinterpret_cast<const char*>(&final_magic), 8);
        idx_stream_.write(reinterpret_cast<const char*>(&count_), 8);

        data_stream_.flush();
        idx_stream_.flush();
        data_stream_.close();
        idx_stream_.close();
    }

    ~OOCRelationWriter() { close(); }

    [[nodiscard]] size_t count() const noexcept { return count_; }
    [[nodiscard]] const std::string& base_path() const noexcept { return base_path_; }

private:
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

    template <typename T>
    void write_val(const T& v) {
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

    std::string base_path_;
    std::vector<char> data_buf_;
    std::vector<char> idx_buf_;
    // fstream (not ofstream) to support resume mode: r/w open without trunc,
    // seek to past existing offsets/data.
    std::fstream data_stream_;
    std::fstream idx_stream_;
    size_t count_ = 0;
    int uncaught_at_ctor_ = 0;
    bool closed_ = false;
};

/// Read-only mmap-based access to out-of-core relations.
///
/// Maps both .relidx and .reldata files into memory.
/// Provides O(1) access to any relation by index.
class OOCRelationReader {
public:
    OOCRelationReader() = default;

    explicit OOCRelationReader(const std::string& base_path)
        : idx_file_(base_path + ".relidx"),
          data_file_(base_path + ".reldata") {

        // Validate index header
        if (idx_file_.size() < 16) {
            throw std::runtime_error("OOCRelationReader: index file too small");
        }
        uint64_t magic = idx_file_.read_at<uint64_t>(0);
        if (magic != OOCRelationWriter::MAGIC) {
            throw std::runtime_error("OOCRelationReader: invalid magic in index");
        }
        count_ = idx_file_.read_at<uint64_t>(8);

        // Index should have: 16 bytes header + (count+1) × 8 bytes offsets
        size_t expected_idx = 16 + (count_ + 1) * 8;
        if (idx_file_.size() < expected_idx) {
            throw std::runtime_error("OOCRelationReader: index file truncated");
        }

        offsets_ = idx_file_.ptr_at<uint64_t>(16);

        // Switch to random access pattern for data
        data_file_.advise_random();
    }

    /// Number of stored relations.
    [[nodiscard]] size_t count() const noexcept { return count_; }

    /// Read a single relation by index.
    [[nodiscard]] gnfs::core::Relation read(size_t idx) const {
        assert(idx < count_);
        uint64_t start = offsets_[idx];
        uint64_t end = offsets_[idx + 1];
        assert(end <= data_file_.size());

        const uint8_t* ptr = data_file_.data() + start;
        size_t avail = static_cast<size_t>(end - start);
        return deserialize(ptr, avail);
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
        assert(to <= count_);
        std::vector<gnfs::core::Relation> result;
        result.reserve(to - from);
        for (size_t i = from; i < to; ++i) {
            result.push_back(read(i));
        }
        return result;
    }

private:
    static gnfs::core::Relation deserialize(const uint8_t* ptr, size_t avail) {
        gnfs::core::Relation rel;
        size_t pos = 0;

        auto read_val = [&](auto& v) {
            if (pos + sizeof(v) > avail) {
                throw std::runtime_error("OOCRelationReader: corrupt record (truncated)");
            }
            std::memcpy(&v, ptr + pos, sizeof(v));
            pos += sizeof(v);
        };

        // 检查变长数组的字节范围。count 来自磁盘,损坏文件可能是天文数字,
        // 必须在 resize 之前 reject,否则 resize 会触发巨型 allocation 或后续 memcpy 越界。
        auto check_bulk = [&](size_t bytes) {
            if (bytes > avail || pos + bytes > avail) {
                throw std::runtime_error("OOCRelationReader: corrupt record (bulk overflow)");
            }
        };

        read_val(rel.a);
        read_val(rel.b);

        // rational_factors
        uint32_t rf_count = 0;
        read_val(rf_count);
        check_bulk(static_cast<size_t>(rf_count) * sizeof(uint32_t));
        rel.rational_factors.resize(rf_count);
        if (rf_count > 0) {
            std::memcpy(rel.rational_factors.data(), ptr + pos, rf_count * sizeof(uint32_t));
            pos += rf_count * sizeof(uint32_t);
        }

        // algebraic_factors
        uint32_t af_count = 0;
        read_val(af_count);
        check_bulk(static_cast<size_t>(af_count) * sizeof(uint32_t));
        rel.algebraic_factors.resize(af_count);
        if (af_count > 0) {
            std::memcpy(rel.algebraic_factors.data(), ptr + pos, af_count * sizeof(uint32_t));
            pos += af_count * sizeof(uint32_t);
        }

        // rational_large_prime
        uint32_t rlp_count = 0;
        read_val(rlp_count);
        // 上限保护:每条 LP 三个字段,先估算需要的总字节
        check_bulk(static_cast<size_t>(rlp_count) *
                   (sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint8_t)));
        rel.rational_large_prime.resize(rlp_count);
        for (uint32_t i = 0; i < rlp_count; ++i) {
            read_val(rel.rational_large_prime[i].p);
            read_val(rel.rational_large_prime[i].r);
            read_val(rel.rational_large_prime[i].e);
        }

        // algebraic_large_prime
        uint32_t alp_count = 0;
        read_val(alp_count);
        check_bulk(static_cast<size_t>(alp_count) *
                   (sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint8_t)));
        rel.algebraic_large_prime.resize(alp_count);
        for (uint32_t i = 0; i < alp_count; ++i) {
            read_val(rel.algebraic_large_prime[i].p);
            read_val(rel.algebraic_large_prime[i].r);
            read_val(rel.algebraic_large_prime[i].e);
        }

        // extra_ab_pairs
        uint32_t extra_count = 0;
        read_val(extra_count);
        check_bulk(static_cast<size_t>(extra_count) *
                   (sizeof(int64_t) + sizeof(uint64_t)));
        rel.extra_ab_pairs.resize(extra_count);
        for (uint32_t i = 0; i < extra_count; ++i) {
            read_val(rel.extra_ab_pairs[i].first);
            read_val(rel.extra_ab_pairs[i].second);
        }

        return rel;
    }

    gnfs::util::MmapFile idx_file_;
    gnfs::util::MmapFile data_file_;
    size_t count_ = 0;
    const uint64_t* offsets_ = nullptr;
};

} // namespace gnfs::relation

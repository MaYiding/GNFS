#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace gnfs::sieve {

/// Mid-flight sieve checkpoint for long-running 50d+/60d factorizations.
///
/// Design:
///   - 配套 RelationCollector OOC mode (BACKLOG #11c) — OOC 持久化 relations,
///     SieveCheckpoint 持久化 SpecialQ 迭代位置 + adaptive round 状态.
///   - 单文件 `<base_path>.sieve_ckpt`, fixed-size header + variable path string.
///   - MAGIC_INCOMPLETE → MAGIC flip 保证 mid-write crash 时 reader 严格拒读.
///
/// Resume 流程:
///   1. Pipeline 检测 GNFS_SIEVE_RESUME=&lt;base_path&gt; ENV
///   2. 若 &lt;base_path&gt;.sieve_ckpt 存在 → load state + collector resume mode
///   3. SpecialQGenerator::reset_to(current_index) skip 已 done SQs
///   4. Adaptive round 从 checkpoint round 继续
///
/// 不持久化 (acceptable 重做):
///   - In-flight SQ batch (4-2 个 SQ 并行处理中) — drop, 从下个 batch 重 sieve
///   - Phase 4 filter 中间状态 (已 done relations from prior round)
struct SieveCheckpoint {
    // MAGIC = 'GNFSSCKP' (written only after successful finalize)
    static constexpr uint64_t MAGIC = 0x474E465353434B50ULL;
    static constexpr uint64_t MAGIC_INCOMPLETE = 0x474E465353434B4EULL;
    static constexpr uint64_t VERSION = 1;

    // State fields (持久化的 sieve loop 状态)
    uint64_t sq_count = 0;            // 累计 processed special-Qs
    uint32_t current_index = 0;       // SpecialQGenerator::current_index_ 快照
    int32_t  round = 0;               // adaptive loop 当前 round
    uint64_t batch_target = 0;        // 当前 batch_target (initial × multiplier)
    uint64_t candidates_total = 0;    // 累计 candidates 统计
    std::string ooc_base_path;        // OOC RelationCollector base path (sanity)

    /// 序列化到 path. 先写 MAGIC_INCOMPLETE, flush, 然后 seek 头翻成 MAGIC.
    /// crash mid-write → reader 看到 INCOMPLETE 拒读.
    void save(const std::string& path) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("SieveCheckpoint::save: cannot open " + path);
        }

        // Write INCOMPLETE magic first
        uint64_t magic = MAGIC_INCOMPLETE;
        uint64_t version = VERSION;
        out.write(reinterpret_cast<const char*>(&magic), 8);
        out.write(reinterpret_cast<const char*>(&version), 8);
        out.write(reinterpret_cast<const char*>(&sq_count), 8);
        out.write(reinterpret_cast<const char*>(&current_index), 4);
        out.write(reinterpret_cast<const char*>(&round), 4);
        out.write(reinterpret_cast<const char*>(&batch_target), 8);
        out.write(reinterpret_cast<const char*>(&candidates_total), 8);

        // Variable-length path
        uint32_t path_len = static_cast<uint32_t>(ooc_base_path.size());
        out.write(reinterpret_cast<const char*>(&path_len), 4);
        if (path_len > 0) {
            out.write(ooc_base_path.data(),
                      static_cast<std::streamsize>(path_len));
        }

        out.flush();
        if (!out) {
            throw std::runtime_error("SieveCheckpoint::save: write failed mid-stream");
        }

        // Seek back + flip magic to MAGIC. Atomic on most POSIX FS within
        // single 8-byte aligned write (sector-aligned).
        out.seekp(0);
        magic = MAGIC;
        out.write(reinterpret_cast<const char*>(&magic), 8);
        out.flush();
        out.close();
    }

    /// 从 path 反序列化. 若 magic ≠ MAGIC 抛异常 (force-resume mode 允许通过
    /// SieveCheckpoint::load_force 绕过).
    static SieveCheckpoint load(const std::string& path,
                                bool allow_incomplete = false) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("SieveCheckpoint::load: cannot open " + path);
        }

        uint64_t magic = 0, version = 0;
        in.read(reinterpret_cast<char*>(&magic), 8);
        in.read(reinterpret_cast<char*>(&version), 8);
        if (in.gcount() != 8) {
            throw std::runtime_error("SieveCheckpoint::load: file too small");
        }

        if (magic != MAGIC && !(allow_incomplete && magic == MAGIC_INCOMPLETE)) {
            throw std::runtime_error("SieveCheckpoint::load: invalid magic in " + path);
        }
        if (version != VERSION) {
            throw std::runtime_error("SieveCheckpoint::load: version mismatch (got " +
                                     std::to_string(version) + ", expected " +
                                     std::to_string(VERSION) + ")");
        }

        SieveCheckpoint ck;
        in.read(reinterpret_cast<char*>(&ck.sq_count), 8);
        in.read(reinterpret_cast<char*>(&ck.current_index), 4);
        in.read(reinterpret_cast<char*>(&ck.round), 4);
        in.read(reinterpret_cast<char*>(&ck.batch_target), 8);
        in.read(reinterpret_cast<char*>(&ck.candidates_total), 8);

        uint32_t path_len = 0;
        in.read(reinterpret_cast<char*>(&path_len), 4);
        if (in.gcount() != 4) {
            throw std::runtime_error("SieveCheckpoint::load: truncated before path");
        }
        // Guard: 损坏文件可能写天文数字 path_len, 防 resize OOM
        if (path_len > 4096) {
            throw std::runtime_error("SieveCheckpoint::load: path_len > 4096 (corrupt)");
        }
        if (path_len > 0) {
            ck.ooc_base_path.resize(path_len);
            in.read(ck.ooc_base_path.data(),
                    static_cast<std::streamsize>(path_len));
            if (in.gcount() != static_cast<std::streamsize>(path_len)) {
                throw std::runtime_error("SieveCheckpoint::load: path truncated");
            }
        }

        return ck;
    }

    /// 删除 checkpoint 文件 (sieve 正常完成后调用).
    static void remove(const std::string& path) noexcept {
        std::remove(path.c_str());
    }

    /// 检查 path 文件是否存在 + magic 有效 (用于 Pipeline ENV 检测).
    /// 不抛异常,返回 false 即可 fallback to fresh sieve.
    static bool exists_and_valid(const std::string& path) noexcept {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        uint64_t magic = 0;
        in.read(reinterpret_cast<char*>(&magic), 8);
        return in.gcount() == 8 && magic == MAGIC;
    }
};

} // namespace gnfs::sieve

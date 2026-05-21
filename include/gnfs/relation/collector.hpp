#pragma once

#include "../core/integer.hpp"
#include "../core/relation.hpp"
#include "../core/types.hpp"
#include "../util/safe_math.hpp"
#include "ooc_relation_store.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace gnfs::relation {

using core::ABPair;
using core::ABPairHash;
using core::Integer;
using core::Relation;

/// 关系收集器统计
struct CollectorStats {
    size_t total_relations = 0;       // 总关系数
    size_t full_relations = 0;        // 完全光滑关系
    size_t partial_1lp = 0;           // 1LP 部分关系
    size_t partial_2lp = 0;           // 2LP 部分关系
    size_t duplicates_rejected = 0;   // 拒绝的重复关系
    size_t invalid_rejected = 0;      // 拒绝的无效关系
    size_t n_divisible_rejected = 0;  // 拒绝的 gcd(a-bm, N) > 1 关系 (CLAUDE.md 强制)
};

/// 关系收集器配置
struct CollectorConfig {
    bool check_duplicates = true;     // 检查重复
    bool allow_partial = true;        // 允许部分关系 (含大素数)
    size_t max_relations = 0;         // 最大关系数 (0 = 无限制)
    std::string output_file;          // 输出文件 (可选)
    bool flush_on_add = false;        // 每次添加后刷新

    // ── OOC (Out-of-Core) 流式持久化 (BACKLOG #11c, 50d Round 2 OOM 防御) ──
    // 启用后:
    //   - add() 同时 streaming write 进 .reldata/.relidx (零 RAM 增长)
    //   - 内存只保留 seen_ (a,b dedup) + stats; relations_ 不再 grow
    //   - get_relations() 从盘 mmap 读全部 (Phase 4 入口才 spike RAM, sieve 期间 flat)
    bool ooc_enabled = false;
    std::string ooc_base_path;        // 文件 base path (无扩展; .reldata + .relidx 自动追加)

    // ── Resume mode (BACKLOG #11e, sieve mid-flight checkpoint) ──
    // 仅在 ooc_enabled=true 时有意义. 启用后 ctor 用 OOCRelationWriter(path, resume=true)
    // 接 prior session 末尾追加. ctor 同时 reload (a,b) seen set 防 resume 后 duplicate.
    // Note: stats.full_relations/partial_*/duplicates_rejected 不持久化 — 重置为 0,
    // 仅 total_relations = prior writer count.
    bool ooc_resume = false;
};

/// RelationCollector - 关系收集器
/// 线程安全的关系收集器
class RelationCollector {
public:
    /// 默认构造
    RelationCollector() = default;

    /// 带配置构造
    explicit RelationCollector(const CollectorConfig& config)
        : config_(config) {
        if (!config_.output_file.empty()) {
            open_output_file();
        }
        // OOC mode: lazy-init writer (failure → exception propagates out of ctor)
        if (config_.ooc_enabled) {
            if (config_.ooc_base_path.empty()) {
                throw std::runtime_error(
                    "RelationCollector: ooc_enabled=true requires non-empty ooc_base_path");
            }
            ooc_writer_ = std::make_unique<OOCRelationWriter>(
                config_.ooc_base_path,
                /*resume=*/config_.ooc_resume);
            if (config_.ooc_resume) {
                // Restore (a,b) seen set + total_relations stat from prior session.
                // Other stats (full/partial breakdown, dup/invalid counts) reset to 0
                // — info-only, not load-bearing for Phase 4 filter correctness.
                restore_seen_from_ooc();
            }
        }
    }

    /// 设置数论上下文(N 和 m),启用 CLAUDE.md 强制的 gcd(a-bm, N)>1 校验。
    /// 没设置时退回旧行为(只检 gcd(a,b)=1)。引用必须在 collector 生存期内有效。
    /// 设置后 add()/load()/merge() 都会拒绝 N|rat_value 的退化关系。
    void set_polynomial_context(const Integer& n, const Integer& m) {
        std::lock_guard<std::mutex> lock(mutex_);
        n_for_validation_ = &n;
        m_for_validation_ = &m;
    }

    /// 析构（关闭文件）
    ~RelationCollector() {
        close_output_file();
    }

    // 禁止拷贝和移动（mutex 不可移动）
    RelationCollector(const RelationCollector&) = delete;
    RelationCollector& operator=(const RelationCollector&) = delete;
    RelationCollector(RelationCollector&&) = delete;
    RelationCollector& operator=(RelationCollector&&) = delete;

    /// 添加关系
    /// @return true 如果关系被接受
    bool add(Relation&& rel) {
        // Callback is invoked outside the lock to prevent deadlock.
        // If callback calls size()/stats()/etc., it would deadlock
        // on the non-recursive mutex if called inside the lock.
        NewRelationCallback cb_copy;
        Relation cb_rel;
        bool fire_callback = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            // 检查限制
            if (config_.max_relations > 0 && stats_.total_relations >= config_.max_relations) {
                return false;
            }

            // 验证关系
            int kind = validate_with_kind(rel);
            if (kind != 0) {
                if (kind == -2) ++stats_.n_divisible_rejected;
                else ++stats_.invalid_rejected;
                return false;
            }

            // 检查重复 — single insert+check (returns {iter, inserted}, saves a hash lookup)
            if (config_.check_duplicates) {
                if (!seen_.insert(rel.ab()).second) {
                    ++stats_.duplicates_rejected;
                    return false;
                }
            }

            // 更新统计
            update_stats(rel);

            // 写入文件
            if (output_stream_.is_open()) {
                rel.serialize(output_stream_);
                if (config_.flush_on_add) {
                    output_stream_.flush();
                }
            }

            // Prepare callback data before moving rel
            if (callback_) {
                cb_copy = callback_;
                cb_rel = rel;
                fire_callback = true;
            }

            // 存储关系: OOC 模式流式写盘, 否则保留在内存 vector
            if (ooc_writer_) {
                ooc_writer_->write(rel);
            } else {
                relations_.push_back(std::move(rel));
            }
        }

        // Invoke callback outside the lock — safe for callback to call
        // size(), stats(), or any other method on this collector.
        if (fire_callback) {
            cb_copy(cb_rel);
        }

        return true;
    }

    /// 批量添加
    size_t add_batch(std::vector<Relation>&& batch) {
        size_t accepted = 0;
        for (auto& rel : batch) {
            if (add(std::move(rel))) {
                ++accepted;
            }
        }
        return accepted;
    }

    /// 获取关系数量
    /// OOC 模式下基于 OOCRelationWriter::count() — 反映实际写盘 relation 数。
    [[nodiscard]] size_t size() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ooc_writer_) return ooc_writer_->count();
        return relations_.size();
    }

    /// 是否为空
    [[nodiscard]] bool empty() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ooc_writer_) return ooc_writer_->count() == 0;
        return relations_.empty();
    }

    /// 获取统计
    [[nodiscard]] CollectorStats stats() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

    /// 获取所有关系（拷贝）
    /// OOC 模式: finalize writer + 从盘 mmap 读全部 → vector (spike RAM at this point).
    /// 调用后 add() 行为 undefined — 设计为 sieve 结束 → get_relations() → 释放 collector.
    [[nodiscard]] std::vector<Relation> get_relations() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ooc_writer_) {
            // close() 是 idempotent (closed_ flag); 完成 magic flip + flush
            ooc_writer_->close();
            OOCRelationReader reader(config_.ooc_base_path);
            return reader.read_all();
        }
        std::vector<Relation> result;
        result.reserve(relations_.size());
        for (const auto& rel : relations_) {
            result.push_back(rel);
        }
        return result;
    }

    /// 获取关系的只读引用（NOT thread-safe — caller must ensure no concurrent add()）
    /// For thread-safe access, use get_relations() which copies under lock.
    /// OOC 模式不可用 — relations_ vector 在 OOC 模式下不被维护。
    [[nodiscard]] const std::vector<Relation>& relations() const noexcept {
        return relations_;
    }

    /// Finalize the OOC writer without consuming the collected relations.
    /// Idempotent. After this call, `add()` behavior is undefined and the OOC
    /// store is fully flushed (MAGIC flipped), so an external OOCRelationReader
    /// can open the files. In vector mode this is a no-op.
    ///
    /// Used by distributed_sieve worker children, which need the master to read
    /// the per-worker OOC store but should not pay the cost of `get_relations()`
    /// (which copies all relations into memory and then immediately discards them
    /// when the child exits).
    void finalize_ooc() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ooc_writer_) {
            ooc_writer_->close();
        }
    }

    /// 清空收集器
    /// OOC 模式: close writer + 删 .reldata/.relidx 文件 + reopen (允许 reuse)。
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        { std::vector<Relation> tmp; relations_.swap(tmp); }
        { std::unordered_set<ABPair, ABPairHash> tmp; seen_.swap(tmp); }
        stats_ = CollectorStats{};
        if (ooc_writer_) {
            ooc_writer_->close();
            ooc_writer_.reset();
            // 删除磁盘 artifact (best-effort, 无视失败 — 文件可能已不存在)
            std::remove((config_.ooc_base_path + ".reldata").c_str());
            std::remove((config_.ooc_base_path + ".relidx").c_str());
            // 重新构造 writer 供后续 add() 使用
            ooc_writer_ = std::make_unique<OOCRelationWriter>(config_.ooc_base_path);
        }
    }

    /// 刷新输出文件
    void flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (output_stream_.is_open()) {
            output_stream_.flush();
        }
    }

    /// 保存到文件 (legacy 序列化协议; 与 OOC store 协议独立)
    /// OOC 模式不兼容 — 请用 ooc_base_path 直接访问 .reldata/.relidx
    bool save(const std::string& filename) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ooc_writer_) return false;  // OOC mode: legacy save disabled

        std::ofstream ofs(filename, std::ios::binary);
        if (!ofs) return false;

        // 写入头部
        uint64_t count = relations_.size();
        ofs.write(reinterpret_cast<const char*>(&count), sizeof(count));

        // 写入关系
        for (const auto& rel : relations_) {
            rel.serialize(ofs);
        }

        return ofs.good();
    }

    /// 从文件加载 (legacy 序列化协议)
    /// OOC 模式不兼容 — relation 已写盘, 重启时直接构造 OOCRelationReader 即可。
    bool load(const std::string& filename) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ooc_writer_) return false;  // OOC mode: legacy load disabled

        std::ifstream ifs(filename, std::ios::binary);
        if (!ifs) return false;

        // 读取头部
        uint64_t count = 0;
        ifs.read(reinterpret_cast<char*>(&count), sizeof(count));

        // 清空并预分配
        relations_.clear();
        seen_.clear();
        relations_.reserve(count);

        // 读取关系
        for (uint64_t i = 0; i < count; ++i) {
            auto rel = Relation::deserialize(ifs);
            if (!ifs) return false;

            int kind = validate_with_kind(rel);
            if (kind != 0) {
                if (kind == -2) ++stats_.n_divisible_rejected;
                else ++stats_.invalid_rejected;
                continue;
            }

            if (config_.check_duplicates) {
                seen_.insert(rel.ab());
            }

            update_stats(rel);
            relations_.push_back(std::move(rel));
        }

        return true;
    }

    /// 合并另一个收集器的关系
    /// OOC 模式: this 是 OOC 时, write to disk; other 必须是非 OOC (从内存 vector 读)
    /// 设计上 sieve worker 各自有 RelationCollector + merge 到 master, 这里 OOC 也 work
    /// 但 OOC merge OOC 不支持 (会触发 read+rewrite, 不实用)。
    size_t merge(const RelationCollector& other) {
        if (this == &other) return 0;  // Self-merge: UB with std::mutex
        std::scoped_lock lock(mutex_, other.mutex_);
        if (other.ooc_writer_) return 0;  // OOC source not supported (read overhead)

        size_t added = 0;
        for (const auto& rel : other.relations_) {
            Relation copy = rel;

            int kind = validate_with_kind(copy);
            if (kind != 0) {
                if (kind == -2) ++stats_.n_divisible_rejected;
                else ++stats_.invalid_rejected;
                continue;
            }

            // 检查重复 — single insert+check (returns {iter, inserted}, saves a hash lookup)
            if (config_.check_duplicates) {
                if (!seen_.insert(copy.ab()).second) {
                    ++stats_.duplicates_rejected;
                    continue;
                }
            }

            update_stats(copy);
            if (ooc_writer_) {
                ooc_writer_->write(copy);
            } else {
                relations_.push_back(std::move(copy));
            }
            ++added;
        }

        return added;
    }

    /// 设置回调（新关系添加时调用，callback 在 mutex 外执行）
    using NewRelationCallback = std::function<void(const Relation&)>;
    void set_callback(NewRelationCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(callback);
    }

private:
    CollectorConfig config_;
    std::vector<Relation> relations_;          // OOC 禁用时持有; OOC 启用时为空
    std::unordered_set<ABPair, ABPairHash> seen_;
    CollectorStats stats_;
    mutable std::mutex mutex_;

    std::ofstream output_stream_;
    NewRelationCallback callback_;

    // CLAUDE.md 强制约定:必须拒绝 gcd(a-bm, N)>1 的关系。
    // 通过 set_polynomial_context() 设置;未设置时退回旧行为。
    const Integer* n_for_validation_ = nullptr;
    const Integer* m_for_validation_ = nullptr;

    // OOC 模式 (BACKLOG #11c): lazy-initialized OOCRelationWriter,first add() 时构造。
    // unique_ptr 因为 OOCRelationWriter 不可移动(持有 ofstream + 内部 buffer)。
    std::unique_ptr<OOCRelationWriter> ooc_writer_;

    /// Resume mode: 从现有 .reldata/.relidx 加载 (a,b) seen set + 设 total_relations.
    /// 期望 OOCWriter 已经在 resume mode 重开 streams (idx_stream_ 指向 past offsets).
    /// 直接 fstream parse, 不依赖 OOCRelationReader (后者 enforce MAGIC, INCOMPLETE 拒读).
    /// ctor 内调用,在 mutex 锁外(对象未发布到其他线程)。
    void restore_seen_from_ooc() {
        std::ifstream idx(config_.ooc_base_path + ".relidx", std::ios::binary);
        if (!idx) return;

        uint64_t magic = 0, count = 0;
        idx.read(reinterpret_cast<char*>(&magic), 8);
        idx.read(reinterpret_cast<char*>(&count), 8);
        if (idx.gcount() != 8 || count == 0) return;

        std::vector<uint64_t> offsets(count);
        idx.read(reinterpret_cast<char*>(offsets.data()),
                 static_cast<std::streamsize>(count * 8));
        if (static_cast<size_t>(idx.gcount()) != count * 8) return;
        idx.close();

        std::ifstream data(config_.ooc_base_path + ".reldata", std::ios::binary);
        if (!data) return;

        seen_.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            data.seekg(static_cast<std::streamoff>(offsets[i]));
            int64_t a = 0;
            uint64_t b = 0;
            data.read(reinterpret_cast<char*>(&a), sizeof(a));
            data.read(reinterpret_cast<char*>(&b), sizeof(b));
            if (data.gcount() == sizeof(b)) {
                seen_.insert(ABPair{a, b});
            }
        }

        stats_.total_relations = count;
    }

    /// 验证关系。mutex 内调用。
    /// 返回 0=通过,-1=无效(b/gcd),-2=N-divisible(CLAUDE.md 强制拒绝)
    [[nodiscard]] int validate_with_kind(const Relation& rel) const {
        // b 必须 > 0
        if (rel.b == 0) return -1;

        // gcd(a, b) 必须 = 1
        if (std::gcd(util::safe_abs(rel.a), rel.b) != 1) {
            return -1;
        }

        // CLAUDE.md: gcd(a - bm, N) 必须 = 1。
        // 否则关系是退化的 (∏(a-bm) ≡ 0 mod N → X=0 → trivial gcd)。
        // thread_local: 每秒 100K+ relations 走此 path; 复用 buffer 省 2 alloc/relation
        if (n_for_validation_ && m_for_validation_) {
            thread_local Integer val, g;
            val = rel.a;  // mpz_set_si direct
            mpz_submul_ui(val.get_mpz(), m_for_validation_->get_mpz(), rel.b);
            mpz_gcd(g.get_mpz(), val.get_mpz(), n_for_validation_->get_mpz());
            if (mpz_cmp_ui(g.get_mpz(), 1) > 0) return -2;
        }

        return 0;
    }

    /// 兼容老接口
    [[nodiscard]] bool validate(const Relation& rel) const {
        return validate_with_kind(rel) == 0;
    }

    /// 更新统计（不含 callback，callback 在 mutex 外调用以防死锁）
    void update_stats(const Relation& rel) noexcept {
        ++stats_.total_relations;

        // 计算大素数数量
        size_t lp_count = rel.rational_large_prime.size() + rel.algebraic_large_prime.size();

        if (lp_count == 0) {
            ++stats_.full_relations;
        } else if (lp_count == 1) {
            ++stats_.partial_1lp;
        } else {
            ++stats_.partial_2lp;
        }
    }

    /// 打开输出文件
    void open_output_file() {
        output_stream_.open(config_.output_file, std::ios::binary | std::ios::app);
    }

    /// 关闭输出文件
    void close_output_file() {
        if (output_stream_.is_open()) {
            output_stream_.close();
        }
    }
};

/// 过滤重复关系
[[nodiscard]] inline std::vector<Relation> filter_duplicates(std::vector<Relation>&& relations) {
    std::unordered_set<ABPair, ABPairHash> seen;
    seen.reserve(relations.size());
    std::vector<Relation> result;
    result.reserve(relations.size());

    for (auto& rel : relations) {
        // Single insert+check (saves 1 hash lookup vs count + insert pattern)
        if (seen.insert(rel.ab()).second) {
            result.push_back(std::move(rel));
        }
    }

    return result;
}

/// 按 (a, b) 排序关系
inline void sort_relations(std::vector<Relation>& relations) {
    std::sort(relations.begin(), relations.end(),
              [](const Relation& r1, const Relation& r2) {
                  if (r1.b != r2.b) return r1.b < r2.b;
                  return r1.a < r2.a;
              });
}

} // namespace gnfs::relation

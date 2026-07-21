#pragma once

#include "../core/integer.hpp"
#include "../core/relation.hpp"
#include "../core/types.hpp"
#include "../util/memory_pool.hpp"
#include "../util/safe_math.hpp"
#include "large_prime_key.hpp"
#include "ooc_relation_store.hpp"
#include "radix_sort.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <functional>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
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
    size_t total_relations = 0;      // 总关系数
    size_t full_relations = 0;       // 完全光滑关系
    size_t partial_1lp = 0;          // 1LP 部分关系
    size_t partial_2lp = 0;          // 2LP 及 3+ LP 部分关系（当前最高统计桶）
    size_t duplicates_rejected = 0;  // 拒绝的重复关系
    size_t invalid_rejected = 0;     // 拒绝的无效关系
    size_t n_divisible_rejected = 0; // 拒绝的 gcd(a-bm, N) > 1 关系 (CLAUDE.md 强制)
};

/// 关系收集器配置
struct CollectorConfig {
    bool check_duplicates = true; // 检查重复
    bool allow_partial = true;    // 允许部分关系 (含大素数)
    size_t max_relations = 0;     // 最大关系数 (0 = 无限制)
    std::string output_file;      // 输出文件 (可选)
    bool flush_on_add = false;    // 每次添加后刷新

    // ── OOC (Out-of-Core) 流式持久化 (BACKLOG #11c, 50d Round 2 OOM 防御) ──
    // 启用后:
    //   - add() 同时 streaming write 进 .reldata/.relidx (零 RAM 增长)
    //   - 内存只保留 seen_ (a,b dedup) + stats; relations_ 不再 grow
    //   - get_relations() 从盘 mmap 读全部 (Phase 4 入口才 spike RAM, sieve 期间 flat)
    bool ooc_enabled = false;
    std::string ooc_base_path; // 文件 base path (无扩展; .reldata + .relidx 自动追加)

    // ── Resume mode (BACKLOG #11e, sieve mid-flight checkpoint) ──
    // 仅在 ooc_enabled=true 时有意义. 启用后 ctor 用 OOCRelationWriter(path, resume=true)
    // 接 prior session 末尾追加. ctor 在严格验证全部 compact records 时同时恢复
    // (a,b) seen set 与 full/1LP/2+LP 分类统计；拒绝计数从 0 重新开始。
    bool ooc_resume = false;

    // ── Memory pool (W6 T4) ──
    // ENV-gated: GNFS_RELATION_POOL_SIZE=N (positive int) switches the in-memory
    // relations_ vector to std::pmr::vector<Relation> backed by
    // RelationPoolResource (monotonic_buffer_resource, initial chunk = N bytes).
    // Defaults pick up the ENV value at default-construction time so that
    // `RelationCollector{}` honors the global policy without per-site code.
    // Explicit override: set use_pool=false to force std::allocator path,
    // or set use_pool=true + pool_initial_bytes to opt in regardless of ENV.
    // Pool only affects vector-mode collectors (OOC mode bypasses relations_).
    bool use_pool = util::relation_pool_enabled();
    size_t pool_initial_bytes = util::relation_pool_size_bytes();
};

/// RelationCollector - 关系收集器
/// 线程安全的关系收集器
class RelationCollector {
public:
    /// 默认构造
    RelationCollector() = default;

    /// 带配置构造
    explicit RelationCollector(const CollectorConfig& config) : config_(config) {
        if (!config_.output_file.empty()) {
            open_output_file();
        }
        // Memory pool init (W6 T4): only when explicitly opted in via ENV or
        // direct config. OOC mode bypasses relations_ entirely so the pool
        // would be wasted RAM in that case.
        if (config_.use_pool && !config_.ooc_enabled) {
            size_t chunk = config_.pool_initial_bytes > 0
                               ? config_.pool_initial_bytes
                               : util::RelationPoolResource::DEFAULT_INITIAL_CHUNK_BYTES;
            pool_ = std::make_unique<util::RelationPoolResource>(chunk);
            relations_pmr_ = std::make_unique<std::pmr::vector<Relation>>(pool_->upstream());
        }
        // OOC mode: lazy-init writer (failure → exception propagates out of ctor)
        if (config_.ooc_enabled) {
            if (config_.ooc_base_path.empty()) {
                throw std::runtime_error(
                    "RelationCollector: ooc_enabled=true requires non-empty ooc_base_path");
            }
            ooc_writer_ = std::make_unique<OOCRelationWriter>(config_.ooc_base_path,
                                                              /*resume=*/config_.ooc_resume);
            if (config_.ooc_resume) {
                auto prefix = ooc_writer_->take_validated_resume_prefix();
                if (!prefix || prefix->count != static_cast<uint64_t>(ooc_writer_->count()) ||
                    prefix->full_relations > prefix->count ||
                    prefix->partial_1lp > prefix->count - prefix->full_relations ||
                    prefix->partial_2lp !=
                        prefix->count - prefix->full_relations - prefix->partial_1lp) {
                    throw std::runtime_error("RelationCollector: missing or inconsistent validated "
                                             "OOC resume prefix");
                }

                // Validation builds the final hash set directly, avoiding a
                // full ABPair vector and hash set being resident at once.
                if (config_.check_duplicates)
                    seen_.swap(prefix->seen);
                stats_.total_relations = static_cast<size_t>(prefix->count);
                stats_.full_relations = static_cast<size_t>(prefix->full_relations);
                stats_.partial_1lp = static_cast<size_t>(prefix->partial_1lp);
                stats_.partial_2lp = static_cast<size_t>(prefix->partial_2lp);
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

            if (ooc_writer_ && ooc_writer_->state() != OOCWriterState::Open) {
                throw std::logic_error("RelationCollector::add: OOC writer is not appendable");
            }

            // 检查限制
            if (config_.max_relations > 0 && stats_.total_relations >= config_.max_relations) {
                return false;
            }

            // 验证关系
            int kind = validate_with_kind(rel);
            if (kind != 0) {
                if (kind == -2)
                    ++stats_.n_divisible_rejected;
                else
                    ++stats_.invalid_rejected;
                return false;
            }

            // Compute the key without mutating collector state. The actual set
            // insertion happens only after every other pre-storage allocation.
            const ABPair key = rel.ab();

            // Canonical LP counting may allocate for deep merged relations. Do
            // it before any accepted-relation state is committed.
            const size_t lp_count = count_odd_large_prime_keys(rel);

            // Prepare callback data before moving rel
            if (callback_) {
                cb_copy = callback_;
                cb_rel = rel;
                fire_callback = true;
            }

            // Allocate the dedup node before storage. If storage throws, erase
            // this exact insertion so size/seen/stats remain synchronized.
            bool seen_inserted = false;
            auto seen_it = seen_.end();
            if (config_.check_duplicates) {
                auto [it, inserted] = seen_.insert(key);
                if (!inserted) {
                    ++stats_.duplicates_rejected;
                    return false;
                }
                seen_it = it;
                seen_inserted = true;
            }

            // 存储关系: OOC 模式流式写盘, 否则保留在内存 vector.
            // Pool mode (W6 T4): push 到 std::pmr::vector backed by RelationPoolResource;
            // 默认走 std::vector (zero overhead path).
            try {
                // Preserve the historical auxiliary-output-before-storage order.
                if (output_stream_.is_open()) {
                    rel.serialize(output_stream_);
                    if (config_.flush_on_add) {
                        output_stream_.flush();
                    }
                }

                if (ooc_writer_) {
                    ooc_writer_->write(rel);
                } else if (relations_pmr_) {
                    relations_pmr_->push_back(std::move(rel));
                } else {
                    relations_.push_back(std::move(rel));
                }
            } catch (...) {
                if (seen_inserted) {
                    seen_.erase(seen_it);
                }
                throw;
            }
            update_stats(lp_count);
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
    /// Pool 模式 (W6 T4) 读 pmr vector size。
    [[nodiscard]] size_t size() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ooc_writer_)
            return ooc_writer_->count();
        if (relations_pmr_)
            return relations_pmr_->size();
        return relations_.size();
    }

    /// 是否为空
    [[nodiscard]] bool empty() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ooc_writer_)
            return ooc_writer_->count() == 0;
        if (relations_pmr_)
            return relations_pmr_->empty();
        return relations_.empty();
    }

    /// 获取统计
    [[nodiscard]] CollectorStats stats() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

    /// Finalize collection and return all relations by value.
    ///
    /// OOC mode flips the final MAGIC and consumes the appendable writer. Any
    /// later add() deterministically throws std::logic_error. Pool/vector mode
    /// simply returns a copy and remains appendable for compatibility.
    [[nodiscard]] std::vector<Relation> finalize_relations() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ooc_writer_) {
            (void)ooc_writer_->finalize();
            OOCRelationReader reader(config_.ooc_base_path);
            return reader.read_all();
        }
        if (relations_pmr_) {
            std::vector<Relation> result;
            result.reserve(relations_pmr_->size());
            for (const auto& rel : *relations_pmr_) {
                result.push_back(rel);
            }
            return result;
        }
        std::vector<Relation> result;
        result.reserve(relations_.size());
        for (const auto& rel : relations_) {
            result.push_back(rel);
        }
        return result;
    }

    /// Compatibility alias for the historical consuming OOC API.
    [[nodiscard]] std::vector<Relation> get_relations() const {
        return const_cast<RelationCollector*>(this)->finalize_relations();
    }

    /// Return a stable copy of all relations accepted before this call while
    /// keeping the collector appendable afterwards.
    ///
    /// OOC mode temporarily suspends and closes the writer handles, reads the
    /// explicitly validated INCOMPLETE prefix, destroys the mmap reader, then
    /// reopens the writer at the exact sentinel slot. The collector mutex makes
    /// this operation linearizable with concurrent add() calls.
    [[nodiscard]] std::vector<Relation> snapshot_relations() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ooc_writer_) {
            if (ooc_writer_->state() == OOCWriterState::Finalized) {
                OOCRelationReader reader(config_.ooc_base_path);
                return reader.read_all();
            }
            if (ooc_writer_->state() == OOCWriterState::Failed) {
                throw std::runtime_error(
                    "RelationCollector::snapshot_relations: OOC writer failed");
            }

            const auto descriptor = ooc_writer_->checkpoint_prefix();
            try {
                std::vector<Relation> result;
                {
                    OOCRelationPrefixReader reader(config_.ooc_base_path, descriptor, *ooc_writer_);
                    result = reader.read_all();
                } // Reader must unmap before Windows reopens the writer handles.
                ooc_writer_->resume_append(descriptor);
                return result;
            } catch (const std::bad_alloc&) {
                // The mapped reader has already been destroyed. A pure
                // allocation failure does not invalidate the flushed prefix, so
                // make one best-effort attempt to restore appendability while
                // preserving the original bad_alloc.
                auto original = std::current_exception();
                if (ooc_writer_->state() == OOCWriterState::Suspended) {
                    try {
                        ooc_writer_->resume_append(descriptor);
                    } catch (...) {
                        // The writer records a failed reopen when applicable.
                    }
                }
                std::rethrow_exception(original);
            } catch (...) {
                // Integrity/format/I/O failures make this prefix untrustworthy.
                // Fail closed and never reopen it for append.
                if (ooc_writer_->state() == OOCWriterState::Suspended) {
                    ooc_writer_->fail_suspended_snapshot();
                }
                throw;
            }
        }
        if (relations_pmr_) {
            std::vector<Relation> result;
            result.reserve(relations_pmr_->size());
            for (const auto& rel : *relations_pmr_) {
                result.push_back(rel);
            }
            return result;
        }
        return relations_;
    }

    /// Flush a stable OOC prefix and suspend appends without materializing it.
    /// The returned descriptor remains valid until resume_ooc() or finalize_ooc().
    /// While suspended, add()/merge() fail before mutating seen_ or stats. In
    /// vector mode, the descriptor records only the current relation count.
    [[nodiscard]] OOCSnapshotDescriptor checkpoint_ooc() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ooc_writer_) {
            OOCSnapshotDescriptor descriptor;
            descriptor.count =
                static_cast<uint64_t>(relations_pmr_ ? relations_pmr_->size() : relations_.size());
            return descriptor;
        }
        if (ooc_writer_->state() != OOCWriterState::Open) {
            throw std::logic_error("RelationCollector::checkpoint_ooc: writer is not appendable");
        }
        return ooc_writer_->checkpoint_prefix();
    }

    /// Resume an explicitly checkpointed OOC collector. Stale or foreign
    /// descriptors are rejected by the writer and leave it suspended.
    void resume_ooc(const OOCSnapshotDescriptor& descriptor) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ooc_writer_) {
            throw std::logic_error("RelationCollector::resume_ooc: collector is not in OOC mode");
        }
        ooc_writer_->resume_append(descriptor);
    }

    /// 获取关系的只读引用（NOT thread-safe — caller must ensure no concurrent add()）
    /// For an appendable thread-safe copy, use snapshot_relations(). Use
    /// finalize_relations()/get_relations() only at the consuming boundary.
    /// OOC 模式不可用 — relations_ vector 在 OOC 模式下不被维护。
    /// Pool 模式 (W6 T4) 同样不可用 — 内部容器是 std::pmr::vector，调用方需用 get_relations()。
    [[nodiscard]] const std::vector<Relation>& relations() const noexcept {
        return relations_;
    }

    /// Finalize the OOC writer without materializing collected relations.
    /// Idempotent. Afterwards add() deterministically throws std::logic_error,
    /// and an external OOCRelationReader can open the finalized files. In
    /// vector mode this is a no-op and the collector remains appendable.
    ///
    /// Used by distributed_sieve worker children, which need the master to read
    /// the per-worker OOC store but should not pay the cost of `get_relations()`
    /// (which copies all relations into memory and then immediately discards them
    /// when the child exits).
    void finalize_ooc() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ooc_writer_) {
            (void)ooc_writer_->finalize();
        }
    }

    /// 清空收集器
    /// OOC 模式: close writer + 删 .reldata/.relidx 文件 + reopen (允许 reuse)。
    /// Pool 模式 (W6 T4): 释放 pmr vector + reset RelationPoolResource (释放 chunks).
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        {
            std::vector<Relation> tmp;
            relations_.swap(tmp);
        }
        {
            std::unordered_set<ABPair, ABPairHash> tmp;
            seen_.swap(tmp);
        }
        stats_ = CollectorStats{};
        if (relations_pmr_) {
            // 先 destroy pmr::vector (析构 Relations); 再 reset pool (释放 chunks).
            relations_pmr_.reset();
            pool_->reset();
            relations_pmr_ = std::make_unique<std::pmr::vector<Relation>>(pool_->upstream());
        }
        if (ooc_writer_) {
            if (ooc_writer_->state() != OOCWriterState::Failed) {
                (void)ooc_writer_->finalize();
            }
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
    /// Pool 模式 (W6 T4): 支持 — 走 pmr::vector 路径序列化。
    bool save(const std::string& filename) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ooc_writer_)
            return false; // OOC mode: legacy save disabled

        std::ofstream ofs(filename, std::ios::binary);
        if (!ofs)
            return false;

        // 写入头部 + 关系 — 区分 pool vs std::vector path
        if (relations_pmr_) {
            uint64_t count = relations_pmr_->size();
            ofs.write(reinterpret_cast<const char*>(&count), sizeof(count));
            for (const auto& rel : *relations_pmr_) {
                rel.serialize(ofs);
            }
        } else {
            uint64_t count = relations_.size();
            ofs.write(reinterpret_cast<const char*>(&count), sizeof(count));
            for (const auto& rel : relations_) {
                rel.serialize(ofs);
            }
        }

        return ofs.good();
    }

    /// 从文件加载 (legacy 序列化协议)
    /// OOC 模式不兼容 — relation 已写盘, 重启时直接构造 OOCRelationReader 即可。
    /// Pool 模式 (W6 T4): 支持 — 加载到 pmr::vector path。
    bool load(const std::string& filename) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ooc_writer_)
            return false; // OOC mode: legacy load disabled

        std::ifstream ifs(filename, std::ios::binary);
        if (!ifs)
            return false;

        // 读取头部
        uint64_t count = 0;
        ifs.read(reinterpret_cast<char*>(&count), sizeof(count));

        // 清空并预分配 — pool vs std::vector path
        if (relations_pmr_) {
            relations_pmr_->clear();
            relations_pmr_->reserve(count);
        } else {
            relations_.clear();
            relations_.reserve(count);
        }
        seen_.clear();

        // 读取关系
        for (uint64_t i = 0; i < count; ++i) {
            auto rel = Relation::deserialize(ifs);
            if (!ifs)
                return false;

            int kind = validate_with_kind(rel);
            if (kind != 0) {
                if (kind == -2)
                    ++stats_.n_divisible_rejected;
                else
                    ++stats_.invalid_rejected;
                continue;
            }

            if (config_.check_duplicates) {
                seen_.insert(rel.ab());
            }

            update_stats(rel);
            if (relations_pmr_) {
                relations_pmr_->push_back(std::move(rel));
            } else {
                relations_.push_back(std::move(rel));
            }
        }

        return true;
    }

    /// 合并另一个收集器的关系
    /// OOC 模式: this 是 OOC 时, write to disk; other 必须是非 OOC (从内存 vector 读)
    /// 设计上 sieve worker 各自有 RelationCollector + merge 到 master, 这里 OOC 也 work
    /// 但 OOC merge OOC 不支持 (会触发 read+rewrite, 不实用)。
    /// Pool 模式 (W6 T4): this 和 other 都支持 — pool/std::vector source 都能读;
    /// destination 写到 this 当前的容器 (pool or std::vector or OOC writer)。
    size_t merge(const RelationCollector& other) {
        if (this == &other)
            return 0; // Self-merge: UB with std::mutex
        std::scoped_lock lock(mutex_, other.mutex_);
        if (ooc_writer_ && ooc_writer_->state() != OOCWriterState::Open) {
            throw std::logic_error("RelationCollector::merge: OOC writer is not appendable");
        }
        if (other.ooc_writer_)
            return 0; // OOC source not supported (read overhead)

        // Source iteration: pmr vector 优先 (pool mode), 否则 std::vector.
        // 通过 lambda 统一两个路径,避免代码重复.
        auto merge_one = [&](const Relation& src_rel) -> bool {
            Relation copy = src_rel;

            int kind = validate_with_kind(copy);
            if (kind != 0) {
                if (kind == -2)
                    ++stats_.n_divisible_rejected;
                else
                    ++stats_.invalid_rejected;
                return false;
            }

            const ABPair key = copy.ab();
            const size_t lp_count = count_odd_large_prime_keys(copy);

            bool seen_inserted = false;
            auto seen_it = seen_.end();
            if (config_.check_duplicates) {
                auto [it, inserted] = seen_.insert(key);
                if (!inserted) {
                    ++stats_.duplicates_rejected;
                    return false;
                }
                seen_it = it;
                seen_inserted = true;
            }

            try {
                if (ooc_writer_) {
                    ooc_writer_->write(copy);
                } else if (relations_pmr_) {
                    relations_pmr_->push_back(std::move(copy));
                } else {
                    relations_.push_back(std::move(copy));
                }
            } catch (...) {
                if (seen_inserted) {
                    seen_.erase(seen_it);
                }
                throw;
            }
            update_stats(lp_count);
            return true;
        };

        size_t added = 0;
        if (other.relations_pmr_) {
            for (const auto& rel : *other.relations_pmr_) {
                if (merge_one(rel))
                    ++added;
            }
        } else {
            for (const auto& rel : other.relations_) {
                if (merge_one(rel))
                    ++added;
            }
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
    std::vector<Relation> relations_; // OOC/Pool 禁用时持有; 其他模式为空
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

    // Pool 模式 (W6 T4): RelationPoolResource + std::pmr::vector<Relation>.
    // 两个 fields 联动:启用时 use_pool=true → pool_ 非空 → relations_pmr_ 非空 → relations_ 不用.
    // 析构顺序: relations_pmr_ 先析构 (释放 Relation 占用的 pool chunk 引用),
    // pool_ 后析构 (释放底层 monotonic_buffer_resource chunks).
    // 标记 declaration 顺序: pool_ 先, relations_pmr_ 后 → 析构序反 → relations_pmr_ 先析构. OK.
    std::unique_ptr<util::RelationPoolResource> pool_;
    std::unique_ptr<std::pmr::vector<Relation>> relations_pmr_;

    /// 验证关系。mutex 内调用。
    /// 返回 0=通过,-1=无效(b/gcd),-2=N-divisible(CLAUDE.md 强制拒绝)
    [[nodiscard]] int validate_with_kind(const Relation& rel) const {
        // b 必须 > 0
        if (rel.b == 0)
            return -1;

        // gcd(a, b) 必须 = 1
        if (std::gcd(util::safe_abs(rel.a), rel.b) != 1) {
            return -1;
        }

        // CLAUDE.md: gcd(a - bm, N) 必须 = 1。
        // 否则关系是退化的 (∏(a-bm) ≡ 0 mod N → X=0 → trivial gcd)。
        // thread_local: 每秒 100K+ relations 走此 path; 复用 buffer 省 2 alloc/relation
        if (n_for_validation_ && m_for_validation_) {
            thread_local Integer val, g;
            val = rel.a; // mpz_set_si direct
            mpz_submul_ui(val.get_mpz(), m_for_validation_->get_mpz(), rel.b);
            mpz_gcd(g.get_mpz(), val.get_mpz(), n_for_validation_->get_mpz());
            if (mpz_cmp_ui(g.get_mpz(), 1) > 0)
                return -2;
        }

        return 0;
    }

    /// 兼容老接口
    [[nodiscard]] bool validate(const Relation& rel) const {
        return validate_with_kind(rel) == 0;
    }

    /// 更新统计（不含 callback，callback 在 mutex 外调用以防死锁）
    void update_stats(size_t lp_count) noexcept {
        ++stats_.total_relations;

        if (lp_count == 0) {
            ++stats_.full_relations;
        } else if (lp_count == 1) {
            ++stats_.partial_1lp;
        } else {
            // CollectorStats 暂无 3LP 字段，3+ LP 归入现有最高桶。
            ++stats_.partial_2lp;
        }
    }

    void update_stats(const Relation& rel) {
        // 按 GF(2) 有效 LP 键分类：偶指数和重复同键会相消。先完成可能
        // 分配内存的 canonical 计数，再提交 total/classification stats。
        const size_t lp_count = count_odd_large_prime_keys(rel);
        update_stats(lp_count);
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
///
/// ENV `GNFS_FILTER_RADIX_SORT=1` 切到 LSD byte-radix path (radix_sort.hpp).
/// 默认 0 走 std::sort, bit-for-bit identical output (stability preserves
/// duplicate insertion order so downstream filter_duplicates picks the same
/// representative either way).
inline void sort_relations(std::vector<Relation>& relations) {
    if (filter_radix_sort_enabled()) {
        radix_sort_relations(relations);
        return;
    }
    std::sort(relations.begin(), relations.end(), [](const Relation& r1, const Relation& r2) {
        if (r1.b != r2.b)
            return r1.b < r2.b;
        return r1.a < r2.a;
    });
}

} // namespace gnfs::relation

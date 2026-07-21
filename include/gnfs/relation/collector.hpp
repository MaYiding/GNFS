#pragma once

#include "../core/integer.hpp"
#include "../core/relation.hpp"
#include "../core/types.hpp"
#include "../util/memory_pool.hpp"
#include "../util/safe_math.hpp"
#include "large_prime_key.hpp"
#include "ooc_relation_store.hpp"
#include "radix_sort.hpp"
#include "relation_sink.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <utility>
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
    // 文件 base path (无扩展; .reldata + .relidx 自动追加)。Collector 构造时冻结为
    // canonical absolute path。Fresh 模式拒绝覆盖任一既有 artifact。
    std::string ooc_base_path;

    // ── Paired resume mode (sieve mid-flight checkpoint) ──
    // Recovery is permitted only with a descriptor loaded from the paired
    // SieveCheckpoint V2. The V3 writer validates the descriptor, verifies the
    // durable store identity and prefix, restores generation, then rolls back
    // uncommitted tails and restores seen_ plus relation statistics.
    std::optional<OOCSnapshotDescriptor> ooc_resume_snapshot;

    // Source-compatibility guard for old call sites. `true` is rejected before
    // opening the store; production recovery must use ooc_resume_snapshot.
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

/// One immutable copy of an appendable collector prefix plus the authoritative
/// descriptor of the raw prefix from which it was copied. The destination
/// corpus has its own physical V3 identity; callers comparing a later terminal
/// handoff with this probe must use source_descriptor.
struct CollectorOOCCorpusSnapshot final {
    RelationCorpus corpus;
    OOCSnapshotDescriptor source_descriptor;
};

/// Callback-scoped, read-only access to one committed OOC collector prefix.
///
/// Instances cannot be copied, moved, or retained beyond the callback passed
/// to RelationCollector::with_ooc_prefix(). Concurrent read() calls are safe;
/// every worker using the source must be drained/joined before the callback
/// returns. The first storage/integrity/I/O exception is retained so the
/// collector can fail closed even when a callback catches that exception.
class CollectorOOCPrefixSource final {
public:
    CollectorOOCPrefixSource(const CollectorOOCPrefixSource&) = delete;
    CollectorOOCPrefixSource& operator=(const CollectorOOCPrefixSource&) = delete;
    CollectorOOCPrefixSource(CollectorOOCPrefixSource&&) = delete;
    CollectorOOCPrefixSource& operator=(CollectorOOCPrefixSource&&) = delete;

    [[nodiscard]] size_t count() const noexcept {
        return reader_->count();
    }

    /// Authoritative physical identity of this raw committed prefix. Returned
    /// by value so callers may pair it with callback output without retaining
    /// a view into the callback-scoped source. generation is the writer's
    /// checkpoint generation, not a logical relation generation.
    [[nodiscard]] OOCSnapshotDescriptor descriptor() const noexcept {
        return descriptor_;
    }

    [[nodiscard]] Relation read(size_t ordinal) const {
        try {
            return reader_->read(ordinal);
        } catch (const std::bad_alloc&) {
            // Allocation pressure does not make the committed source bytes
            // untrustworthy. The collector resumes before propagating it.
            throw;
        } catch (const std::out_of_range&) {
            // An invalid callback ordinal is a caller error, not corruption.
            throw;
        } catch (...) {
            record_source_failure(std::current_exception());
            throw;
        }
    }

private:
    CollectorOOCPrefixSource(const OOCRelationPrefixReader& reader,
                             const OOCSnapshotDescriptor& descriptor) noexcept
        : reader_(&reader), descriptor_(descriptor) {}

    void record_source_failure(std::exception_ptr failure) const noexcept {
        try {
            std::lock_guard<std::mutex> lock(failure_mutex_);
            if (!source_failure_) {
                source_failure_ = std::move(failure);
            }
        } catch (...) {
            // Preserve the fail-closed classification even if the original
            // exception cannot be retained.
        }
        source_failed_.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool source_failed() const noexcept {
        return source_failed_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::exception_ptr source_failure() const noexcept {
        try {
            std::lock_guard<std::mutex> lock(failure_mutex_);
            return source_failure_;
        } catch (...) {
            return nullptr;
        }
    }

    const OOCRelationPrefixReader* reader_ = nullptr;
    OOCSnapshotDescriptor descriptor_;
    mutable std::mutex failure_mutex_;
    mutable std::exception_ptr source_failure_;
    mutable std::atomic<bool> source_failed_{false};

    friend class RelationCollector;
};

/// RelationCollector - 关系收集器
/// 线程安全的关系收集器
class RelationCollector {
public:
    /// 默认构造
    RelationCollector() = default;

    /// 带配置构造
    explicit RelationCollector(const CollectorConfig& config) : config_(config) {
        if (config_.ooc_resume) {
            throw std::invalid_argument(
                "RelationCollector: legacy ooc_resume flag is unsupported; use "
                "ooc_resume_snapshot");
        }
        if (config_.ooc_resume_snapshot && !config_.ooc_enabled) {
            throw std::invalid_argument(
                "RelationCollector: ooc_resume_snapshot requires ooc_enabled=true");
        }
        if (config_.ooc_enabled) {
            if (config_.ooc_base_path.empty()) {
                throw std::runtime_error(
                    "RelationCollector: ooc_enabled=true requires non-empty ooc_base_path");
            }
            config_.ooc_base_path = relation_corpus_detail::freeze_ooc_path(config_.ooc_base_path);
            if (!config_.ooc_resume_snapshot) {
                reject_existing_fresh_ooc_artifacts(config_.ooc_base_path);
            }
        }

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
        // OOC mode: initialize only after fresh-path collision checks or paired
        // recovery validation selected the exact existing store.
        if (config_.ooc_enabled) {
            ooc_writer_ = std::make_unique<OOCRelationWriter>(config_.ooc_base_path,
                                                              config_.ooc_resume_snapshot);
            if (config_.ooc_resume_snapshot) {
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
        require_not_handed_off("set_polynomial_context");
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

            require_appendable_ooc_owner("add");
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
        if (config_.ooc_enabled)
            return stats_.total_relations;
        if (relations_pmr_)
            return relations_pmr_->size();
        return relations_.size();
    }

    /// 是否为空
    [[nodiscard]] bool empty() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ooc_writer_)
            return ooc_writer_->count() == 0;
        if (config_.ooc_enabled)
            return stats_.total_relations == 0;
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
        require_available_ooc_owner("finalize_relations");
        if (ooc_writer_) {
            const auto descriptor = ooc_writer_->finalize();
            OOCRelationReader reader(config_.ooc_base_path, descriptor);
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
        require_available_ooc_owner("snapshot_relations");
        if (ooc_writer_) {
            if (ooc_writer_->state() == OOCWriterState::Finalized) {
                const auto descriptor = ooc_writer_->finalize();
                OOCRelationReader reader(config_.ooc_base_path, descriptor);
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

    /// Invoke a callback against one immutable, committed prefix of an
    /// appendable OOC collector without copying relation payloads.
    ///
    /// The writer is checkpointed while holding the collector mutex, then an
    /// active-borrow guard lets the callback run without that mutex. Observer
    /// methods remain usable; owner and mutation methods reject while the
    /// borrow is active instead of deadlocking or touching the suspended
    /// writer. The callback may parallelize source reads, but it must drain/join
    /// every worker before returning. The source, its address, and any
    /// source-derived view must not escape in the callback result. The prefix
    /// reader is destroyed before the exact checkpoint descriptor resumes the
    /// writer.
    ///
    /// Source integrity/I/O failures fail the writer closed, including when
    /// the callback catches the read exception. Callback/output failures,
    /// invalid ordinals, and std::bad_alloc resume the writer before being
    /// propagated. A resume failure always takes precedence over the callback
    /// failure. Callback results are returned by value and may be move-only.
    /// The source reference must not escape the callback.
    template <typename Callback>
    auto with_ooc_prefix(Callback&& callback)
        -> std::invoke_result_t<Callback, const CollectorOOCPrefixSource&> {
        using Result = std::invoke_result_t<Callback, const CollectorOOCPrefixSource&>;
        static_assert(std::is_void_v<Result> ||
                          (!std::is_reference_v<Result> && !std::is_pointer_v<Result> &&
                           std::is_move_constructible_v<Result>),
                      "with_ooc_prefix callbacks must return void or a movable value");

        std::unique_lock<std::mutex> lock(mutex_);
        require_ooc_mode("with_ooc_prefix");
        require_available_ooc_owner("with_ooc_prefix");
        if (ooc_writer_->state() != OOCWriterState::Open) {
            throw std::logic_error(
                "RelationCollector::with_ooc_prefix: OOC writer is not appendable");
        }

        const OOCSnapshotDescriptor descriptor = ooc_writer_->checkpoint_prefix();
        try {
            validate_descriptor_matches_collector(descriptor, "with_ooc_prefix");
        } catch (...) {
            if (ooc_writer_->state() == OOCWriterState::Suspended) {
                ooc_writer_->fail_suspended_snapshot();
            }
            throw;
        }

        std::unique_ptr<OOCRelationPrefixReader> reader;
        try {
            reader = std::make_unique<OOCRelationPrefixReader>(config_.ooc_base_path, descriptor,
                                                               *ooc_writer_);
        } catch (const std::bad_alloc&) {
            // Construction released any partial reader lease. Exact resume is
            // mandatory and its stronger failure must not be hidden.
            const auto allocation_failure = std::current_exception();
            ooc_writer_->resume_append(descriptor);
            std::rethrow_exception(allocation_failure);
        } catch (...) {
            if (ooc_writer_->state() == OOCWriterState::Suspended) {
                ooc_writer_->fail_suspended_snapshot();
            }
            throw;
        }

        CollectorOOCPrefixSource source(*reader, descriptor);
        ooc_prefix_borrow_active_ = true;
        lock.unlock();

        auto finish_lease = [&](std::exception_ptr callback_failure) {
            lock.lock();
            const bool source_failed = source.source_failed();
            const std::exception_ptr source_failure = source.source_failure();
            reader.reset(); // Unmap/close before fail or exact resume on Windows.

            if (source_failed) {
                ooc_prefix_borrow_active_ = false;
                try {
                    if (ooc_writer_->state() == OOCWriterState::Suspended) {
                        ooc_writer_->fail_suspended_snapshot();
                    }
                } catch (...) {
                    const auto transition_failure = std::current_exception();
                    lock.unlock();
                    std::rethrow_exception(transition_failure);
                }
                if (source_failure) {
                    lock.unlock();
                    std::rethrow_exception(source_failure);
                }
                lock.unlock();
                throw std::runtime_error("RelationCollector::with_ooc_prefix: source read failed");
            }

            try {
                ooc_writer_->resume_append(descriptor);
            } catch (...) {
                const auto resume_failure = std::current_exception();
                ooc_prefix_borrow_active_ = false;
                lock.unlock();
                std::rethrow_exception(resume_failure);
            }
            ooc_prefix_borrow_active_ = false;

            if (callback_failure) {
                lock.unlock();
                std::rethrow_exception(callback_failure);
            }
        };

        if constexpr (std::is_void_v<Result>) {
            std::exception_ptr callback_failure;
            try {
                std::invoke(std::forward<Callback>(callback),
                            static_cast<const CollectorOOCPrefixSource&>(source));
            } catch (...) {
                callback_failure = std::current_exception();
            }
            finish_lease(std::move(callback_failure));
            lock.unlock();
        } else {
            std::optional<Result> callback_result;
            std::exception_ptr callback_failure;
            try {
                callback_result.emplace(
                    std::invoke(std::forward<Callback>(callback),
                                static_cast<const CollectorOOCPrefixSource&>(source)));
            } catch (...) {
                callback_failure = std::current_exception();
            }
            finish_lease(std::move(callback_failure));
            lock.unlock();
            return std::move(*callback_result);
        }
    }

    /// Copy the current appendable OOC prefix into an independent finalized
    /// V3 corpus without constructing a relation vector.
    ///
    /// The destination lease is reserved before the source writer is
    /// checkpointed. Rows are then decoded and appended one at a time. The
    /// prefix reader is destroyed before the exact descriptor resumes the raw
    /// writer, and only then is the destination committed. Destination
    /// allocation/write/finalize failures therefore leave the raw collector
    /// appendable. A source prefix integrity or I/O failure instead marks the
    /// raw writer Failed because its bytes are no longer trustworthy.
    ///
    /// Vector/pool collectors reject this API explicitly; their compatibility
    /// snapshot_relations() behavior is unchanged.
    [[nodiscard]] CollectorOOCCorpusSnapshot
    snapshot_ooc_corpus(uint64_t logical_generation, std::string destination_base_path,
                        OOCCleanupPolicy cleanup_policy = OOCCleanupPolicy::RemoveArtifacts) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_ooc_mode("snapshot_ooc_corpus");
        require_available_ooc_owner("snapshot_ooc_corpus");
        if (ooc_writer_->state() != OOCWriterState::Open) {
            throw std::logic_error(
                "RelationCollector::snapshot_ooc_corpus: OOC writer is not appendable");
        }

        // Reservation and all allocations it performs happen while the source
        // remains Open. A collision must not transiently suspend collection.
        RelationSink destination = RelationSink::out_of_core(
            logical_generation, std::move(destination_base_path), cleanup_policy);
        const OOCSnapshotDescriptor descriptor = ooc_writer_->checkpoint_prefix();
        try {
            validate_descriptor_matches_collector(descriptor, "snapshot_ooc_corpus");
        } catch (...) {
            // A self-inconsistent checkpoint cannot be reopened for appends.
            if (ooc_writer_->state() == OOCWriterState::Suspended) {
                ooc_writer_->fail_suspended_snapshot();
            }
            throw;
        }

        std::exception_ptr recoverable_destination_failure;
        std::exception_ptr untrusted_prefix_failure;
        try {
            OOCRelationPrefixReader reader(config_.ooc_base_path, descriptor, *ooc_writer_);
            for (size_t ordinal = 0; ordinal < reader.count(); ++ordinal) {
                Relation relation;
                try {
                    relation = reader.read(ordinal);
                } catch (const std::bad_alloc&) {
                    // Deserialization allocation pressure says nothing about
                    // the validity of the already-flushed source prefix.
                    recoverable_destination_failure = std::current_exception();
                    break;
                } catch (...) {
                    untrusted_prefix_failure = std::current_exception();
                    break;
                }

                try {
                    (void)destination.append(std::move(relation));
                } catch (...) {
                    recoverable_destination_failure = std::current_exception();
                    break;
                }
            }
        } catch (const std::bad_alloc&) {
            recoverable_destination_failure = std::current_exception();
        } catch (...) {
            untrusted_prefix_failure = std::current_exception();
        } // Prefix reader mappings and handles are closed before either transition.

        if (untrusted_prefix_failure) {
            if (ooc_writer_->state() == OOCWriterState::Suspended) {
                ooc_writer_->fail_suspended_snapshot();
            }
            std::rethrow_exception(untrusted_prefix_failure);
        }

        // Exact resume can itself detect external source damage. Do not mask
        // that stronger failure with a destination exception.
        ooc_writer_->resume_append(descriptor);
        if (recoverable_destination_failure) {
            std::rethrow_exception(recoverable_destination_failure);
        }

        RelationCorpus corpus = destination.finalize();
        return {std::move(corpus), descriptor};
    }

    /// Finalize and transfer the collector's original OOC V3 store into a
    /// move-only corpus. This is a one-shot terminal handoff.
    ///
    /// Corpus adoption occurs before the writer/descriptor is released. If
    /// validation or allocation throws, the collector retains its Finalized
    /// writer and the idempotent descriptor so the same handoff can be retried.
    /// After success, collection, clearing, snapshots, materialization, and
    /// repeated finalization/handoff are rejected deterministically.
    [[nodiscard]] RelationCorpus
    handoff_ooc_corpus(uint64_t logical_generation,
                       OOCCleanupPolicy cleanup_policy = OOCCleanupPolicy::RemoveArtifacts) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_ooc_mode("handoff_ooc_corpus");
        require_available_ooc_owner("handoff_ooc_corpus");

        const OOCSnapshotDescriptor descriptor = ooc_writer_->finalize();
        validate_descriptor_matches_collector(descriptor, "handoff_ooc_corpus");
        RelationCorpus corpus = RelationCorpus::from_finalized_ooc(
            logical_generation, config_.ooc_base_path, descriptor, cleanup_policy);

        // Both fstreams are already closed in Finalized. Destroying the writer
        // after the corpus reader has adopted the descriptor cannot invalidate
        // the immutable mappings or throw.
        ooc_writer_.reset();
        ooc_corpus_handed_off_ = true;
        return corpus;
    }

    /// Flush a stable OOC prefix and suspend appends without materializing it.
    /// The returned descriptor remains valid until resume_ooc() or finalize_ooc().
    /// While suspended, add()/merge() fail before mutating seen_ or stats. In
    /// vector mode, the descriptor records only the current relation count.
    [[nodiscard]] OOCSnapshotDescriptor checkpoint_ooc() {
        std::lock_guard<std::mutex> lock(mutex_);
        require_available_ooc_owner("checkpoint_ooc");
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
        require_not_handed_off("resume_ooc");
        if (!ooc_writer_) {
            throw std::logic_error("RelationCollector::resume_ooc: collector is not in OOC mode");
        }
        ooc_writer_->resume_append(descriptor);
    }

    /// Report how paired OOC recovery resolved. FinalizedCorpus means a crash
    /// occurred after the immutable relation corpus was committed but before
    /// its sieve checkpoint was deleted; callers must skip further collection.
    [[nodiscard]] OOCRecoveryOutcome ooc_recovery_outcome() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ooc_writer_ ? ooc_writer_->recovery_outcome() : OOCRecoveryOutcome::None;
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
    /// OOC mode returns the exact finalized descriptor, and repeated calls
    /// return the same descriptor. Afterwards add() deterministically throws
    /// std::logic_error. Vector mode returns std::nullopt and remains appendable.
    ///
    /// Used by distributed_sieve worker children, which need the master to read
    /// the per-worker OOC store but should not pay the cost of `get_relations()`
    /// (which copies all relations into memory and then immediately discards them
    /// when the child exits).
    std::optional<OOCSnapshotDescriptor> finalize_ooc() {
        std::lock_guard<std::mutex> lock(mutex_);
        require_available_ooc_owner("finalize_ooc");
        if (ooc_writer_) {
            return ooc_writer_->finalize();
        }
        return std::nullopt;
    }

    /// 清空收集器
    /// OOC 模式: finalize + descriptor-bound 删除 .reldata/.relidx + reopen。
    /// Failed/handoff 状态拒绝 clear，避免按不再可信或已转移的路径删除文件。
    /// Pool 模式 (W6 T4): 释放 pmr vector + reset RelationPoolResource (释放 chunks).
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        require_not_handed_off("clear");

        // OOC clear is an explicit destructive recycle of the exact store this
        // collector owns. Complete it before erasing in-memory bookkeeping so
        // a finalize/delete/reopen failure remains fail-closed and diagnosable.
        if (config_.ooc_enabled) {
            if (!ooc_writer_) {
                throw std::logic_error(
                    "RelationCollector::clear: OOC writer ownership is unavailable");
            }
            if (ooc_writer_->state() == OOCWriterState::Failed) {
                throw std::logic_error(
                    "RelationCollector::clear: failed OOC store identity is untrusted");
            }

            const OOCSnapshotDescriptor descriptor = ooc_writer_->finalize();
            validate_descriptor_matches_collector(descriptor, "clear");
            {
                RelationCorpus cleanup = RelationCorpus::from_finalized_ooc(
                    descriptor.generation, config_.ooc_base_path, descriptor,
                    OOCCleanupPolicy::RemoveArtifacts);
                ooc_writer_.reset();
            } // Descriptor-bound identity validation precedes exact pair deletion.
            ooc_writer_ = std::make_unique<OOCRelationWriter>(config_.ooc_base_path);
        }

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
        if (config_.ooc_enabled)
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
        if (config_.ooc_enabled)
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
    /// 但 OOC source（含已 handoff 的终态 collector）会显式拒绝，避免将空的兼容
    /// relations_ 误判成一次成功但零行的 merge。
    /// Pool 模式 (W6 T4): this 和 other 都支持 — pool/std::vector source 都能读;
    /// destination 写到 this 当前的容器 (pool or std::vector or OOC writer)。
    size_t merge(const RelationCollector& other) {
        if (this == &other)
            return 0; // Self-merge: UB with std::mutex
        std::scoped_lock lock(mutex_, other.mutex_);
        require_appendable_ooc_owner("merge");
        if (ooc_writer_ && ooc_writer_->state() != OOCWriterState::Open) {
            throw std::logic_error("RelationCollector::merge: OOC writer is not appendable");
        }
        if (other.config_.ooc_enabled) {
            throw std::logic_error("RelationCollector::merge: OOC source is unsupported");
        }

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
        require_not_handed_off("set_callback");
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

    // OOC 模式 (BACKLOG #11c): 构造 collector 时 eager exclusive-create writer。
    // unique_ptr 因为 OOCRelationWriter 不可移动(持有 fstream + 内部 buffer)。
    std::unique_ptr<OOCRelationWriter> ooc_writer_;
    bool ooc_corpus_handed_off_ = false;
    bool ooc_prefix_borrow_active_ = false;

    // Pool 模式 (W6 T4): RelationPoolResource + std::pmr::vector<Relation>.
    // 两个 fields 联动:启用时 use_pool=true → pool_ 非空 → relations_pmr_ 非空 → relations_ 不用.
    // 析构顺序: relations_pmr_ 先析构 (释放 Relation 占用的 pool chunk 引用),
    // pool_ 后析构 (释放底层 monotonic_buffer_resource chunks).
    // 标记 declaration 顺序: pool_ 先, relations_pmr_ 后 → 析构序反 → relations_pmr_ 先析构. OK.
    std::unique_ptr<util::RelationPoolResource> pool_;
    std::unique_ptr<std::pmr::vector<Relation>> relations_pmr_;

    [[nodiscard]] static bool path_entry_exists_checked(const std::filesystem::path& path) {
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(path, ec);
        if (ec) {
            if (ec == std::errc::no_such_file_or_directory) {
                return false;
            }
            throw std::filesystem::filesystem_error(
                "RelationCollector: cannot inspect OOC artifact", path, ec);
        }
        // symlink_status treats dangling symlinks as occupied paths. Fresh
        // collection must never follow one into an unintended truncation.
        return status.type() != std::filesystem::file_type::not_found;
    }

    static void reject_existing_fresh_ooc_artifacts(const std::string& base_path) {
        const std::filesystem::path index_path(base_path + ".relidx");
        const std::filesystem::path data_path(base_path + ".reldata");
        if (path_entry_exists_checked(index_path) || path_entry_exists_checked(data_path)) {
            throw std::runtime_error(
                "RelationCollector: refusing to replace existing fresh OOC artifacts at " +
                base_path);
        }
    }

    void require_ooc_mode(const char* operation) const {
        if (!config_.ooc_enabled) {
            throw std::logic_error(std::string("RelationCollector::") + operation +
                                   ": collector is not in OOC mode");
        }
    }

    void require_not_handed_off(const char* operation) const {
        if (ooc_prefix_borrow_active_) {
            throw std::logic_error(std::string("RelationCollector::") + operation +
                                   ": borrowed OOC prefix callback is active");
        }
        if (ooc_corpus_handed_off_) {
            throw std::logic_error(std::string("RelationCollector::") + operation +
                                   ": OOC corpus ownership was handed off");
        }
    }

    void require_available_ooc_owner(const char* operation) const {
        require_not_handed_off(operation);
        if (config_.ooc_enabled && !ooc_writer_) {
            throw std::logic_error(std::string("RelationCollector::") + operation +
                                   ": OOC writer is unavailable");
        }
    }

    void require_appendable_ooc_owner(const char* operation) const {
        require_available_ooc_owner(operation);
    }

    void validate_descriptor_matches_collector(const OOCSnapshotDescriptor& descriptor,
                                               const char* operation) const {
        if (!ooc_writer_ || descriptor.format_version != OOCRelationWriter::FORMAT_VERSION_V3 ||
            descriptor.store_id == 0 || descriptor.store_id != ooc_writer_->store_id() ||
            descriptor.count != static_cast<uint64_t>(ooc_writer_->count()) ||
            descriptor.count != static_cast<uint64_t>(stats_.total_relations)) {
            throw std::logic_error(std::string("RelationCollector::") + operation +
                                   ": OOC descriptor does not match collector state");
        }
    }

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

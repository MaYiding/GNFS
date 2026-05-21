// Multi-process distributed sieve worker pool implementation.
//
// See include/gnfs/sieve/distributed_sieve.hpp for the architectural overview.

#include "gnfs/sieve/distributed_sieve.hpp"

#include "gnfs/cofactor/cofactorizer.hpp"
#include "gnfs/relation/collector.hpp"
#include "gnfs/relation/ooc_relation_store.hpp"
#include "gnfs/sieve/lattice_sieve.hpp"
#include "gnfs/sieve/special_q.hpp"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace gnfs::sieve {

namespace {

using gnfs::core::Integer;
using gnfs::core::PolynomialContext;
using gnfs::core::Relation;
using gnfs::factor_base::FactorBase;

/// Per-worker OOC base path: `<base>.worker_<chunk_id>`.
std::string worker_ooc_base(const std::string& base, size_t chunk_id) {
    return base + ".worker_" + std::to_string(chunk_id);
}

/// Remove the .reldata and .relidx files for a worker (best-effort cleanup).
void cleanup_worker_files(const std::string& base) noexcept {
    const std::string data_path = base + ".reldata";
    const std::string idx_path = base + ".relidx";
    // Use unlink directly (no error reporting — best effort).
    ::unlink(data_path.c_str());
    ::unlink(idx_path.c_str());
}

/// Body of the child worker process. Runs the sieve over the assigned SQ index
/// range and writes relations to an OOC store at `worker_base`.
///
/// IMPORTANT: This function must never return — it must call _exit() because
/// running normal destructors after fork() can corrupt OS-level state
/// (e.g., flushing parent file streams a second time).
///
/// Exit codes:
///   0 = success, OOC store finalized with all relations
///   1 = failure (exception caught, or invariant violation)
[[noreturn]] void child_worker_main(
        size_t chunk_id,
        uint32_t sq_begin,
        uint32_t sq_end,
        size_t sq_per_worker_cap,
        size_t worker_collector_cap,
        const std::string& worker_base,
        const PolynomialContext& ctx,
        const FactorBase& fb,
        const SieveParams& sieve_params,
        const SieveRegion& sieve_region,
        const gnfs::cofactor::CofactorizerConfig& cofac_config,
        const Integer& n,
        const Integer& m) {
    try {
        // Per-worker collector configured to stream to the worker OOC store.
        gnfs::relation::CollectorConfig coll_cfg;
        coll_cfg.check_duplicates = true;
        coll_cfg.ooc_enabled = true;
        coll_cfg.ooc_base_path = worker_base;
        coll_cfg.ooc_resume = false;
        gnfs::relation::RelationCollector collector(coll_cfg);
        // gcd(a-bm, N) > 1 guard — must match Pipeline behavior.
        collector.set_polynomial_context(n, m);

        // Per-worker SQ generator initialized over [sq_begin, sq_end).
        SpecialQRange range = SpecialQRange::from_indices(sq_begin, sq_end);
        SpecialQGenerator sq_gen(fb, range);

        // Per-worker sieve + cofactorizer.
        LatticeSieve sieve(ctx, fb, sieve_params);
        sieve.set_region(sieve_region);
        gnfs::cofactor::Cofactorizer cofactorizer(ctx, fb, cofac_config);

        size_t sq_count = 0;
        while (sq_gen.has_next()) {
            if (sq_per_worker_cap > 0 && sq_count >= sq_per_worker_cap) break;
            if (worker_collector_cap > 0 && collector.size() >= worker_collector_cap) break;

            auto sq = sq_gen.next();
            if (!sq) break;

            auto sieve_result = sieve.sieve_special_q(*sq);
            for (const auto& cand : sieve_result.candidates) {
                auto rel = cofactorizer.verify(cand, sq->q, sq->r);
                if (rel) {
                    collector.add(std::move(*rel));
                }
            }
            ++sq_count;
        }

        // Force finalize the OOC store (flip MAGIC). Without this, the master
        // OOCRelationReader will reject the file with "invalid magic".
        collector.finalize_ooc();

        // Stderr trace for master diagnostics (chunk_id, sq_count, rel_count).
        std::fprintf(stderr,
            "[dist_sieve.worker] chunk=%zu pid=%d sq_range=[%u,%u) sq_done=%zu rels=%zu\n",
            chunk_id, static_cast<int>(::getpid()), sq_begin, sq_end,
            sq_count, collector.size());

        // Success: _exit(0) skips parent destructors.
        ::_exit(0);
    } catch (const std::exception& e) {
        std::fprintf(stderr,
            "[dist_sieve.worker] chunk=%zu EXCEPTION: %s\n",
            chunk_id, e.what());
        ::_exit(1);
    } catch (...) {
        std::fprintf(stderr,
            "[dist_sieve.worker] chunk=%zu UNKNOWN EXCEPTION\n",
            chunk_id);
        ::_exit(1);
    }
}

/// Spawn a single worker for a given chunk. Returns the PID of the child.
/// Throws std::runtime_error on fork() failure.
pid_t spawn_worker(
        size_t chunk_id,
        uint32_t sq_begin,
        uint32_t sq_end,
        size_t sq_per_worker_cap,
        size_t worker_collector_cap,
        const std::string& worker_base,
        const PolynomialContext& ctx,
        const FactorBase& fb,
        const SieveParams& sieve_params,
        const SieveRegion& sieve_region,
        const gnfs::cofactor::CofactorizerConfig& cofac_config,
        const Integer& n,
        const Integer& m) {
    // Clean any stale worker files from a prior aborted run.
    cleanup_worker_files(worker_base);

    pid_t pid = ::fork();
    if (pid < 0) {
        throw std::runtime_error(
            std::string("distributed_sieve: fork() failed: ") + std::strerror(errno));
    }
    if (pid == 0) {
        // Child: run sieve, never returns.
        child_worker_main(chunk_id, sq_begin, sq_end, sq_per_worker_cap,
                          worker_collector_cap, worker_base, ctx, fb, sieve_params,
                          sieve_region, cofac_config, n, m);
    }
    // Parent: return child PID.
    return pid;
}

/// Wait for a single PID and decode the exit status.
/// Returns (success, exit_status, signal). success = true iff exited normally
/// with status 0.
std::tuple<bool, int, int> wait_and_decode(pid_t pid) noexcept {
    int wstatus = 0;
    pid_t r;
    do {
        r = ::waitpid(pid, &wstatus, 0);
    } while (r == -1 && errno == EINTR);

    if (r == -1) {
        return {false, -1, 0};
    }
    if (WIFEXITED(wstatus)) {
        const int status = WEXITSTATUS(wstatus);
        return {status == 0, status, 0};
    }
    if (WIFSIGNALED(wstatus)) {
        return {false, -1, WTERMSIG(wstatus)};
    }
    return {false, -1, 0};
}

/// Read all relations from a worker OOC store into the destination vector.
/// Returns the number of relations read. On error, logs to stderr and returns 0.
size_t append_worker_relations(const std::string& worker_base,
                               std::vector<Relation>& dest) noexcept {
    try {
        gnfs::relation::OOCRelationReader reader(worker_base);
        const size_t count = reader.count();
        dest.reserve(dest.size() + count);
        for (size_t i = 0; i < count; ++i) {
            dest.push_back(reader.read(i));
        }
        return count;
    } catch (const std::exception& e) {
        std::fprintf(stderr,
            "[dist_sieve.master] worker store read failed (%s): %s\n",
            worker_base.c_str(), e.what());
        return 0;
    }
}

} // namespace

std::vector<std::pair<uint32_t, uint32_t>>
split_sq_range(uint32_t range_begin, uint32_t range_end, size_t num_chunks) noexcept {
    std::vector<std::pair<uint32_t, uint32_t>> chunks;
    if (num_chunks == 0 || range_end <= range_begin) return chunks;

    const uint32_t total = range_end - range_begin;
    const uint32_t base_size = total / static_cast<uint32_t>(num_chunks);
    const uint32_t remainder = total % static_cast<uint32_t>(num_chunks);

    chunks.reserve(num_chunks);
    uint32_t cursor = range_begin;
    for (size_t i = 0; i < num_chunks; ++i) {
        const uint32_t chunk_len = base_size + (i < remainder ? 1U : 0U);
        const uint32_t chunk_end = cursor + chunk_len;
        chunks.emplace_back(cursor, chunk_end);
        cursor = chunk_end;
    }
    return chunks;
}

DistributedSieveConfig parse_distributed_sieve_env() noexcept {
    DistributedSieveConfig cfg;
    cfg.num_workers = parse_distributed_sieve_workers_env();

    if (const char* env = std::getenv("GNFS_DISTRIBUTED_SIEVE_BASE_PATH");
        env != nullptr && env[0] != '\0') {
        cfg.base_path = env;
    } else {
        cfg.base_path = "/tmp/gnfs_distributed_" + std::to_string(::getpid());
    }

    if (const char* env = std::getenv("GNFS_DISTRIBUTED_SIEVE_SQ_PER_WORKER");
        env != nullptr && env[0] != '\0') {
        char* end = nullptr;
        long v = std::strtol(env, &end, 10);
        if (end != env && v >= 0) {
            cfg.sq_per_worker = static_cast<size_t>(v);
        }
    }

    return cfg;
}

std::vector<Relation> run_distributed_sieve(
        const DistributedSieveConfig& cfg,
        const PolynomialContext& ctx,
        const FactorBase& fb,
        const SieveParams& sieve_params,
        const SieveRegion& sieve_region,
        const gnfs::cofactor::CofactorizerConfig& cofac_config,
        const Integer& n,
        const Integer& m,
        const SpecialQRange& sq_range,
        std::vector<DistributedSieveWorkerResult>* out_worker_stats) {
    if (cfg.num_workers == 0) {
        throw std::invalid_argument(
            "run_distributed_sieve: num_workers must be > 0");
    }
    if (cfg.base_path.empty()) {
        throw std::invalid_argument(
            "run_distributed_sieve: base_path must be non-empty");
    }

    // Resolve effective SQ index range.
    // Mirror SpecialQGenerator's range handling: when range.min_q > 0 it
    // resolves to the first FB index whose prime >= min_q; range.end_index
    // caps the upper bound.
    const auto fb_alg_count = fb.algebraic_count();
    uint32_t range_begin = sq_range.start_index;
    if (sq_range.min_q > 0) {
        // Linear scan for first FB index whose prime >= min_q. Same loop
        // SpecialQGenerator::skip_to_min_q uses, but we don't have access to
        // the generator here.
        const auto& algebraics = fb.algebraic();
        while (range_begin < algebraics.size()) {
            if (algebraics[range_begin].p >= sq_range.min_q) break;
            ++range_begin;
        }
    }
    uint32_t range_end = sq_range.end_index;
    if (range_end > static_cast<uint32_t>(fb_alg_count)) {
        range_end = static_cast<uint32_t>(fb_alg_count);
    }
    // Also cap range_end by max_q. The generator stops when p > max_q.
    if (sq_range.max_q > 0 && sq_range.max_q < UINT32_MAX) {
        const auto& algebraics = fb.algebraic();
        uint32_t i = range_begin;
        while (i < range_end && i < algebraics.size()) {
            if (algebraics[i].p > sq_range.max_q) break;
            ++i;
        }
        range_end = i;
    }

    if (range_end <= range_begin) {
        // Empty range: no work. Return empty vector.
        if (out_worker_stats) out_worker_stats->clear();
        return {};
    }

    // Split range into chunks (one per worker).
    auto chunks = split_sq_range(range_begin, range_end, cfg.num_workers);

    std::fprintf(stderr,
        "[dist_sieve.master] pid=%d workers=%zu sq_range=[%u,%u) base=%s\n",
        static_cast<int>(::getpid()), cfg.num_workers,
        range_begin, range_end, cfg.base_path.c_str());

    // Stage 1: spawn all workers in parallel.
    struct WorkerSlot {
        pid_t pid = -1;
        size_t chunk_id = 0;
        uint32_t sq_begin = 0;
        uint32_t sq_end = 0;
        std::string worker_base;
        bool finished = false;
        bool success = false;
        int exit_status = -1;
        int signal = 0;
    };

    std::vector<WorkerSlot> slots;
    slots.reserve(chunks.size());
    for (size_t i = 0; i < chunks.size(); ++i) {
        const auto [c_begin, c_end] = chunks[i];
        WorkerSlot slot;
        slot.chunk_id = i;
        slot.sq_begin = c_begin;
        slot.sq_end = c_end;
        slot.worker_base = worker_ooc_base(cfg.base_path, i);
        if (c_begin >= c_end) {
            // Empty chunk (degenerate when num_workers > available SQs).
            // Skip without forking.
            slot.finished = true;
            slot.success = true;
            slot.exit_status = 0;
            slots.push_back(std::move(slot));
            continue;
        }
        try {
            slot.pid = spawn_worker(
                i, c_begin, c_end, cfg.sq_per_worker, cfg.worker_collector_cap,
                slot.worker_base, ctx, fb, sieve_params, sieve_region,
                cofac_config, n, m);
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "[dist_sieve.master] spawn failed chunk=%zu: %s\n",
                i, e.what());
            slot.finished = true;
            slot.success = false;
            slot.exit_status = -1;
        }
        slots.push_back(std::move(slot));
    }

    // Stage 2: wait for each worker.
    for (auto& slot : slots) {
        if (slot.finished) continue;  // Empty chunk or spawn failure.
        auto [ok, status, sig] = wait_and_decode(slot.pid);
        slot.finished = true;
        slot.success = ok;
        slot.exit_status = status;
        slot.signal = sig;
        if (!ok) {
            std::fprintf(stderr,
                "[dist_sieve.master] worker chunk=%zu pid=%d FAILED "
                "(exit=%d signal=%d), will retry\n",
                slot.chunk_id, static_cast<int>(slot.pid), status, sig);
        }
    }

    // Stage 3: single retry for any failed worker. The retry runs sequentially
    // (we already have all CPUs from the first wave; a second wave doesn't
    // benefit from parallelism if only a few workers failed).
    for (auto& slot : slots) {
        if (slot.success) continue;
        if (slot.sq_begin >= slot.sq_end) continue;  // Empty chunks treated as success.

        std::fprintf(stderr,
            "[dist_sieve.master] retrying chunk=%zu sq_range=[%u,%u)\n",
            slot.chunk_id, slot.sq_begin, slot.sq_end);
        try {
            slot.pid = spawn_worker(
                slot.chunk_id, slot.sq_begin, slot.sq_end,
                cfg.sq_per_worker, cfg.worker_collector_cap,
                slot.worker_base, ctx, fb, sieve_params, sieve_region,
                cofac_config, n, m);
            auto [ok, status, sig] = wait_and_decode(slot.pid);
            slot.success = ok;
            slot.exit_status = status;
            slot.signal = sig;
            if (!ok) {
                std::fprintf(stderr,
                    "[dist_sieve.master] retry FAILED chunk=%zu (exit=%d signal=%d), "
                    "chunk contributes 0 relations\n",
                    slot.chunk_id, status, sig);
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "[dist_sieve.master] retry spawn failed chunk=%zu: %s\n",
                slot.chunk_id, e.what());
            slot.success = false;
        }
    }

    // Stage 4: merge worker OOC stores into a single vector.
    std::vector<Relation> merged;
    if (out_worker_stats) out_worker_stats->clear();
    if (out_worker_stats) out_worker_stats->reserve(slots.size());

    for (const auto& slot : slots) {
        DistributedSieveWorkerResult res;
        res.pid = slot.pid;
        res.chunk_id = slot.chunk_id;
        res.sq_index_begin = slot.sq_begin;
        res.sq_index_end = slot.sq_end;
        res.success = slot.success;
        res.exit_status = slot.exit_status;
        res.signal = slot.signal;
        res.ooc_base_path = slot.worker_base;
        res.sq_count = (slot.sq_begin >= slot.sq_end) ? 0
                       : (slot.sq_end - slot.sq_begin);

        if (slot.success && slot.sq_begin < slot.sq_end) {
            const size_t before = merged.size();
            const size_t n_added = append_worker_relations(slot.worker_base, merged);
            res.relations_count = n_added;
            (void)before;
        }
        // Always cleanup, success or fail.
        cleanup_worker_files(slot.worker_base);

        if (out_worker_stats) out_worker_stats->push_back(std::move(res));
    }

    std::fprintf(stderr,
        "[dist_sieve.master] complete: merged %zu relations from %zu workers\n",
        merged.size(), slots.size());

    return merged;
}

} // namespace gnfs::sieve

// Multi-process distributed sieve worker pool implementation.
//
// See include/gnfs/sieve/distributed_sieve.hpp for the architectural overview.

#include "gnfs/sieve/distributed_sieve.hpp"

#include "gnfs/cofactor/cofactorizer.hpp"
#include "gnfs/relation/collector.hpp"
#include "gnfs/relation/ooc_relation_store.hpp"
#include "gnfs/relation/relation_sequence_receipt.hpp"
#include "gnfs/sieve/lattice_sieve.hpp"
#include "gnfs/sieve/special_q.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/temp_path.hpp"

#ifndef _WIN32
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32

namespace gnfs::sieve {

std::vector<std::pair<uint32_t, uint32_t>> split_sq_range(uint32_t range_begin, uint32_t range_end,
                                                          size_t num_chunks) noexcept {
    std::vector<std::pair<uint32_t, uint32_t>> chunks;
    if (range_end <= range_begin || num_chunks == 0)
        return chunks;

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
        cfg.base_path =
            gnfs::util::temp_path("gnfs_distributed_" + std::to_string(gnfs::util::process_id()));
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

std::vector<gnfs::core::Relation>
run_distributed_sieve(const DistributedSieveConfig& cfg, const gnfs::core::PolynomialContext&,
                      const gnfs::factor_base::FactorBase&, const SieveParams&, const SieveRegion&,
                      const gnfs::cofactor::CofactorizerConfig&, const gnfs::core::Integer&,
                      const gnfs::core::Integer&, const SpecialQRange&,
                      std::vector<DistributedSieveWorkerResult>*) {
    if (cfg.num_workers == 0) {
        throw std::invalid_argument("run_distributed_sieve: num_workers must be > 0");
    }
    throw std::runtime_error(
        "run_distributed_sieve: POSIX fork workers are not available on Windows");
}

} // namespace gnfs::sieve

#else

namespace gnfs::sieve {

namespace {

using gnfs::core::Integer;
using gnfs::core::PolynomialContext;
using gnfs::core::Relation;
using gnfs::factor_base::FactorBase;

/// Stable caller-visible root for one worker chunk.
std::string worker_artifact_root(const std::string& base, size_t chunk_id) {
    return base + ".worker_" + std::to_string(chunk_id);
}

/// Exact V3 pair base inside the parent's private lease.
std::string worker_ooc_base(const std::string& artifact_root) {
    return (std::filesystem::path(artifact_root + ".gnfs-sink-lease") / "corpus").string();
}

[[nodiscard]] bool interrupt_handoff_pending(gnfs::relation::OOCCleanupPublishFaultPoint point,
                                             void*) noexcept {
    return point == gnfs::relation::OOCCleanupPublishFaultPoint::IntentPendingDurable;
}

inline constexpr std::uint64_t WORKER_REPORT_MAGIC = 0x474e465344535752ULL;

struct WorkerCompletionReport final {
    std::uint64_t magic = WORKER_REPORT_MAGIC;
    std::uint64_t attempt_number = 0;
    std::uint64_t sq_count = 0;
    std::uint64_t relation_count = 0;
    std::uint64_t store_id = 0;
    std::uint64_t generation = 0;
    std::uint64_t data_end = 0;
    std::uint64_t sequence_count = 0;
    std::uint64_t sequence_low = 0;
    std::uint64_t sequence_high = 0;
};
static_assert(std::is_trivially_copyable_v<WorkerCompletionReport>);
static_assert(sizeof(WorkerCompletionReport) == 10 * sizeof(std::uint64_t));
static_assert(sizeof(WorkerCompletionReport) <= PIPE_BUF);

[[nodiscard]] bool write_worker_report(int descriptor,
                                       const WorkerCompletionReport& report) noexcept {
    const auto* bytes = reinterpret_cast<const std::byte*>(&report);
    size_t written = 0;
    while (written < sizeof(report)) {
        const ssize_t result = ::write(descriptor, bytes + written, sizeof(report) - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        written += static_cast<size_t>(result);
    }
    return true;
}

[[nodiscard]] std::optional<WorkerCompletionReport> read_worker_report(int descriptor) noexcept {
    WorkerCompletionReport report;
    auto* bytes = reinterpret_cast<std::byte*>(&report);
    size_t consumed = 0;
    while (consumed < sizeof(report)) {
        const ssize_t result = ::read(descriptor, bytes + consumed, sizeof(report) - consumed);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)::close(descriptor);
            return std::nullopt;
        }
        if (result == 0) {
            (void)::close(descriptor);
            return std::nullopt;
        }
        consumed += static_cast<size_t>(result);
    }
    // waitpid() has already confirmed that the sole writer exited, so an
    // exact frame must now be followed by EOF. Reject duplicate frames or
    // trailing bytes instead of silently treating the first prefix as valid.
    std::byte trailing{};
    ssize_t tail_result;
    do {
        tail_result = ::read(descriptor, &trailing, 1);
    } while (tail_result < 0 && errno == EINTR);
    (void)::close(descriptor);
    if (tail_result != 0) {
        return std::nullopt;
    }
    if (report.magic != WORKER_REPORT_MAGIC || report.attempt_number == 0 || report.store_id == 0 ||
        report.generation == 0) {
        return std::nullopt;
    }
    return report;
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
[[noreturn]] void child_worker_main(size_t chunk_id, uint32_t sq_begin, uint32_t sq_end,
                                    size_t sq_per_worker_cap, size_t worker_collector_cap,
                                    size_t attempt_number, const std::string& worker_base,
                                    gnfs::relation::OOCPrivateLeaseOwnershipReceipt& private_lease,
                                    int report_descriptor, const PolynomialContext& ctx,
                                    const FactorBase& fb, const SieveParams& sieve_params,
                                    const SieveRegion& sieve_region,
                                    const gnfs::cofactor::CofactorizerConfig& cofac_config,
                                    const Integer& n, const Integer& m) {
    try {
        // ── Test/debug crash injection ────────────────────────────────
        // ENV `GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_<chunk_id>=N` makes only
        // parent-numbered attempt N fail. Retry state never enters the worker
        // artifact namespace.
        if (const char* fail_env = std::getenv(
                ("GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_" + std::to_string(chunk_id)).c_str())) {
            const long fail_on = std::strtol(fail_env, nullptr, 10);
            if (std::string_view(fail_env) == "all" ||
                (fail_on > 0 && static_cast<size_t>(fail_on) == attempt_number)) {
                std::fprintf(stderr, "[dist_sieve.worker] chunk=%zu INJECTED FAIL on attempt %zu\n",
                             chunk_id, attempt_number);
                ::_exit(1);
            }
        }

        // Per-worker collector configured to stream to the worker OOC store.
        gnfs::relation::CollectorConfig coll_cfg;
        coll_cfg.check_duplicates = true;
        coll_cfg.ooc_enabled = true;
        coll_cfg.ooc_base_path = worker_base;
        coll_cfg.ooc_resume = false;
        gnfs::relation::RelationCollector collector(coll_cfg, private_lease);
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
            if (sq_per_worker_cap > 0 && sq_count >= sq_per_worker_cap)
                break;
            if (worker_collector_cap > 0 && collector.size() >= worker_collector_cap)
                break;

            auto sq = sq_gen.next();
            if (!sq)
                break;

            auto sieve_result = sieve.sieve_special_q(*sq);
            for (const auto& cand : sieve_result.candidates) {
                auto rel = cofactorizer.verify(cand, sq->q, sq->r);
                if (rel) {
                    collector.add(std::move(*rel));
                }
            }
            ++sq_count;
        }

        // Flip final MAGIC and publish only the canonical cleanup intent. The
        // readable pair stays live and RESERVED stays present, so the parent
        // can merge first and then converge cleanup with its fork copy of the
        // lease receipt.
        gnfs::relation::OOCCleanupTestHooks cleanup_hooks;
        if (const char* fail_pending = std::getenv(
                ("GNFS_DISTRIBUTED_SIEVE_FAIL_HANDOFF_PENDING_ATTEMPT_" + std::to_string(chunk_id))
                    .c_str())) {
            const long fail_on = std::strtol(fail_pending, nullptr, 10);
            if (fail_on > 0 && static_cast<size_t>(fail_on) == attempt_number) {
                cleanup_hooks.stop_after_publish = interrupt_handoff_pending;
            }
        }
        const auto sequence_receipt = collector.ooc_accepted_sequence_receipt();
        const auto descriptor = collector.finalize_and_publish_ooc_cleanup_handoff(cleanup_hooks);
        if (descriptor.count != static_cast<uint64_t>(collector.size())) {
            throw std::runtime_error("distributed_sieve: worker handoff count mismatch");
        }
        if (sequence_receipt.relation_count != descriptor.count) {
            throw std::runtime_error("distributed_sieve: worker sequence receipt count mismatch");
        }
        WorkerCompletionReport report{
            .magic = WORKER_REPORT_MAGIC,
            .attempt_number = static_cast<std::uint64_t>(attempt_number),
            .sq_count = static_cast<std::uint64_t>(sq_count),
            .relation_count = descriptor.count,
            .store_id = descriptor.store_id,
            .generation = descriptor.generation,
            .data_end = descriptor.data_end,
            .sequence_count = sequence_receipt.relation_count,
            .sequence_low = sequence_receipt.low,
            .sequence_high = sequence_receipt.high,
        };
        // Test-only fault injection: the child still exits successfully after
        // publishing cleanup ownership, but the parent must reject this
        // descriptor, clean the exact lease, and recompute with a new
        // generation.
        if (const char* corrupt_report = std::getenv(
                ("GNFS_DISTRIBUTED_SIEVE_CORRUPT_REPORT_ATTEMPT_" + std::to_string(chunk_id))
                    .c_str())) {
            const long corrupt_on = std::strtol(corrupt_report, nullptr, 10);
            if (corrupt_on > 0 && static_cast<size_t>(corrupt_on) == attempt_number) {
                ++report.data_end;
            }
        }
        if (const char* corrupt_receipt = std::getenv(
                ("GNFS_DISTRIBUTED_SIEVE_CORRUPT_RECEIPT_ATTEMPT_" + std::to_string(chunk_id))
                    .c_str())) {
            const long corrupt_on = std::strtol(corrupt_receipt, nullptr, 10);
            if (corrupt_on > 0 && static_cast<size_t>(corrupt_on) == attempt_number) {
                report.sequence_low ^= UINT64_C(1);
            }
        }
        if (!write_worker_report(report_descriptor, report)) {
            throw std::runtime_error("distributed_sieve: worker completion report failed");
        }
        (void)::close(report_descriptor);

        // Stderr trace for master diagnostics (chunk_id, sq_count, rel_count).
        std::fprintf(
            stderr, "[dist_sieve.worker] chunk=%zu pid=%d sq_range=[%u,%u) sq_done=%zu rels=%zu\n",
            chunk_id, gnfs::util::process_id(), sq_begin, sq_end, sq_count, collector.size());

        // Success: _exit(0) skips parent destructors.
        ::_exit(0);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[dist_sieve.worker] chunk=%zu EXCEPTION: %s\n", chunk_id, e.what());
        ::_exit(1);
    } catch (...) {
        std::fprintf(stderr, "[dist_sieve.worker] chunk=%zu UNKNOWN EXCEPTION\n", chunk_id);
        ::_exit(1);
    }
}

/// Spawn a single worker for a given chunk. Returns the PID of the child.
/// Throws std::runtime_error on fork() failure.
struct SpawnedWorker final {
    pid_t pid = -1;
    int report_descriptor = -1;
};

SpawnedWorker spawn_worker(size_t chunk_id, uint32_t sq_begin, uint32_t sq_end,
                           size_t sq_per_worker_cap, size_t worker_collector_cap,
                           size_t attempt_number, const std::string& worker_base,
                           gnfs::relation::OOCPrivateLeaseOwnershipReceipt& private_lease,
                           const PolynomialContext& ctx, const FactorBase& fb,
                           const SieveParams& sieve_params, const SieveRegion& sieve_region,
                           const gnfs::cofactor::CofactorizerConfig& cofac_config, const Integer& n,
                           const Integer& m) {
    int report_pipe[2]{-1, -1};
    if (::pipe(report_pipe) != 0) {
        throw std::runtime_error(std::string("distributed_sieve: pipe() failed: ") +
                                 std::strerror(errno));
    }
    pid_t pid = ::fork();
    if (pid < 0) {
        const int saved_errno = errno;
        (void)::close(report_pipe[0]);
        (void)::close(report_pipe[1]);
        throw std::runtime_error(std::string("distributed_sieve: fork() failed: ") +
                                 std::strerror(saved_errno));
    }
    if (pid == 0) {
        (void)::close(report_pipe[0]);
        // Child: run sieve, never returns.
        child_worker_main(chunk_id, sq_begin, sq_end, sq_per_worker_cap, worker_collector_cap,
                          attempt_number, worker_base, private_lease, report_pipe[1], ctx, fb,
                          sieve_params, sieve_region, cofac_config, n, m);
    }
    (void)::close(report_pipe[1]);
    // Parent: return child PID.
    return SpawnedWorker{
        .pid = pid,
        .report_descriptor = report_pipe[0],
    };
}

struct WorkerWaitResult final {
    bool reaped = false;
    bool success = false;
    int exit_status = -1;
    int signal = 0;
    int native_error = 0;
};

/// Wait for a single PID and decode the exit status. An uncertain waitpid
/// failure is distinct from a confirmed child failure: without a reap
/// boundary the parent must neither delete that lease nor reuse its path.
WorkerWaitResult wait_and_decode(pid_t pid) noexcept {
    int wstatus = 0;
    pid_t r;
    do {
        r = ::waitpid(pid, &wstatus, 0);
    } while (r == -1 && errno == EINTR);

    if (r == -1) {
        return WorkerWaitResult{
            .reaped = false,
            .success = false,
            .exit_status = -1,
            .signal = 0,
            .native_error = errno,
        };
    }
    if (WIFEXITED(wstatus)) {
        const int status = WEXITSTATUS(wstatus);
        return WorkerWaitResult{
            .reaped = true,
            .success = status == 0,
            .exit_status = status,
            .signal = 0,
            .native_error = 0,
        };
    }
    if (WIFSIGNALED(wstatus)) {
        return WorkerWaitResult{
            .reaped = true,
            .success = false,
            .exit_status = -1,
            .signal = WTERMSIG(wstatus),
            .native_error = 0,
        };
    }
    return WorkerWaitResult{
        .reaped = true,
        .success = false,
        .exit_status = -1,
        .signal = 0,
        .native_error = 0,
    };
}

/// Read one complete worker store into an isolated buffer. A read error never
/// leaves a successful slot with a silently empty chunk.
[[nodiscard]] bool read_worker_relations(const std::string& worker_base,
                                         const gnfs::relation::OOCSnapshotDescriptor& expected,
                                         std::vector<Relation>& dest) noexcept {
    try {
        gnfs::relation::OOCRelationReader reader(worker_base, expected);
        const size_t count = reader.count();
        std::vector<Relation> loaded;
        loaded.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            loaded.push_back(reader.read(i));
        }
        dest = std::move(loaded);
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[dist_sieve.master] worker store read failed (%s): %s\n",
                     worker_base.c_str(), e.what());
        dest.clear();
        return false;
    } catch (...) {
        std::fprintf(stderr,
                     "[dist_sieve.master] worker store read failed (%s): unknown exception\n",
                     worker_base.c_str());
        dest.clear();
        return false;
    }
}

} // namespace

std::vector<std::pair<uint32_t, uint32_t>> split_sq_range(uint32_t range_begin, uint32_t range_end,
                                                          size_t num_chunks) noexcept {
    std::vector<std::pair<uint32_t, uint32_t>> chunks;
    if (num_chunks == 0 || range_end <= range_begin)
        return chunks;

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
        cfg.base_path =
            gnfs::util::temp_path("gnfs_distributed_" + std::to_string(gnfs::util::process_id()));
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
    const DistributedSieveConfig& cfg, const PolynomialContext& ctx, const FactorBase& fb,
    const SieveParams& sieve_params, const SieveRegion& sieve_region,
    const gnfs::cofactor::CofactorizerConfig& cofac_config, const Integer& n, const Integer& m,
    const SpecialQRange& sq_range, std::vector<DistributedSieveWorkerResult>* out_worker_stats) {
    if (cfg.num_workers == 0) {
        throw std::invalid_argument("run_distributed_sieve: num_workers must be > 0");
    }
    if (cfg.base_path.empty()) {
        throw std::invalid_argument("run_distributed_sieve: base_path must be non-empty");
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
            if (algebraics[range_begin].p >= sq_range.min_q)
                break;
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
            if (algebraics[i].p > sq_range.max_q)
                break;
            ++i;
        }
        range_end = i;
    }

    if (range_end <= range_begin) {
        // Empty range: no work. Return empty vector.
        if (out_worker_stats)
            out_worker_stats->clear();
        return {};
    }

    // Split range into chunks (one per worker).
    auto chunks = split_sq_range(range_begin, range_end, cfg.num_workers);

    std::fprintf(stderr, "[dist_sieve.master] pid=%d workers=%zu sq_range=[%u,%u) base=%s\n",
                 gnfs::util::process_id(), cfg.num_workers, range_begin, range_end,
                 cfg.base_path.c_str());

    // Stage 1: spawn all workers in parallel.
    struct WorkerSlot {
        pid_t pid = -1;
        int report_descriptor = -1;
        size_t chunk_id = 0;
        uint32_t sq_begin = 0;
        uint32_t sq_end = 0;
        std::string artifact_root;
        std::string worker_base;
        std::optional<gnfs::relation::OOCPrivateLeaseOwnershipReceipt> private_lease;
        std::vector<Relation> relations;
        size_t attempt_count = 0;
        size_t sq_count = 0;
        size_t persisted_relation_count = 0;
        bool finished = false;
        bool reap_confirmed = true;
        bool success = false;
        int exit_status = -1;
        int signal = 0;
    };

    const auto cleanup_attempt = [](WorkerSlot& slot) noexcept {
        if (!slot.private_lease) {
            return true;
        }
        const auto result =
            gnfs::relation::OOCCleanupTransaction::remove_private_lease(*slot.private_lease);
        if (!result.completed()) {
            std::fprintf(stderr,
                         "[dist_sieve.master] worker cleanup failed chunk=%zu status=%u stage=%u\n",
                         slot.chunk_id, static_cast<unsigned>(result.status),
                         static_cast<unsigned>(result.stage));
            return false;
        }
        slot.private_lease.reset();
        return true;
    };

    const auto start_attempt = [&](WorkerSlot& slot) {
        slot.relations.clear();
        slot.success = false;
        slot.finished = false;
        slot.exit_status = -1;
        slot.signal = 0;
        slot.pid = -1;
        slot.sq_count = 0;
        slot.persisted_relation_count = 0;

        if (!cleanup_attempt(slot)) {
            slot.finished = true;
            return false;
        }

        auto reservation =
            gnfs::relation::OOCCleanupTransaction::reserve_private_lease(slot.worker_base);
        if (!reservation.completed()) {
            std::fprintf(
                stderr,
                "[dist_sieve.master] lease reservation failed chunk=%zu status=%u stage=%u\n",
                slot.chunk_id, static_cast<unsigned>(reservation.result.status),
                static_cast<unsigned>(reservation.result.stage));
            if (reservation.ownership && !reservation.ownership->spent()) {
                (void)gnfs::relation::OOCCleanupTransaction::remove_private_lease(
                    *reservation.ownership);
            }
            slot.finished = true;
            return false;
        }
        slot.private_lease.emplace(std::move(*reservation.ownership));
        const size_t next_attempt_number = slot.attempt_count + 1;

        try {
            const auto spawned = spawn_worker(
                slot.chunk_id, slot.sq_begin, slot.sq_end, cfg.sq_per_worker,
                cfg.worker_collector_cap, next_attempt_number, slot.worker_base,
                *slot.private_lease, ctx, fb, sieve_params, sieve_region, cofac_config, n, m);
            slot.pid = spawned.pid;
            slot.report_descriptor = spawned.report_descriptor;
            slot.attempt_count = next_attempt_number;
            slot.reap_confirmed = false;
            return true;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[dist_sieve.master] spawn failed chunk=%zu: %s\n", slot.chunk_id,
                         e.what());
            slot.finished = true;
            (void)cleanup_attempt(slot);
            return false;
        }
    };

    const auto finish_attempt = [&](WorkerSlot& slot) {
        const auto waited = wait_and_decode(slot.pid);
        slot.finished = true;
        slot.reap_confirmed = waited.reaped;
        slot.exit_status = waited.exit_status;
        slot.signal = waited.signal;
        if (!waited.reaped) {
            if (slot.report_descriptor >= 0) {
                (void)::close(slot.report_descriptor);
                slot.report_descriptor = -1;
            }
            std::fprintf(stderr,
                         "[dist_sieve.master] waitpid uncertain chunk=%zu pid=%d errno=%d; "
                         "lease preserved and retry suppressed\n",
                         slot.chunk_id, static_cast<int>(slot.pid), waited.native_error);
            slot.success = false;
            slot.relations.clear();
            return false;
        }
        bool ok = waited.success;
        std::optional<WorkerCompletionReport> report;
        if (slot.report_descriptor >= 0) {
            report = read_worker_report(slot.report_descriptor);
            slot.report_descriptor = -1;
        }
        if (ok) {
            if (!report || report->attempt_number != slot.attempt_count ||
                report->sequence_count != report->relation_count ||
                report->sq_count > static_cast<std::uint64_t>(slot.sq_end - slot.sq_begin) ||
                (cfg.sq_per_worker > 0 && report->sq_count > cfg.sq_per_worker)) {
                ok = false;
            } else {
                const gnfs::relation::OOCSnapshotDescriptor expected{
                    .format_version = gnfs::relation::OOCRelationWriter::FORMAT_VERSION_V3,
                    .store_id = report->store_id,
                    .generation = report->generation,
                    .count = report->relation_count,
                    .data_end = report->data_end,
                };
                const gnfs::relation::RelationSequenceReceipt expected_sequence{
                    .relation_count = report->sequence_count,
                    .low = report->sequence_low,
                    .high = report->sequence_high,
                };
                if (!read_worker_relations(slot.worker_base, expected, slot.relations) ||
                    static_cast<std::uint64_t>(slot.relations.size()) != report->relation_count ||
                    gnfs::relation::relation_sequence_receipt(slot.relations) !=
                        expected_sequence) {
                    ok = false;
                } else {
                    slot.sq_count = static_cast<size_t>(report->sq_count);
                    slot.persisted_relation_count = static_cast<size_t>(report->relation_count);
                }
            }
        }
        const bool cleaned = cleanup_attempt(slot);
        slot.success = ok && cleaned;
        if (!slot.success) {
            slot.relations.clear();
        }
        return slot.success;
    };

    std::vector<WorkerSlot> slots;
    slots.reserve(chunks.size());
    for (size_t i = 0; i < chunks.size(); ++i) {
        const auto [c_begin, c_end] = chunks[i];
        WorkerSlot slot;
        slot.chunk_id = i;
        slot.sq_begin = c_begin;
        slot.sq_end = c_end;
        slot.artifact_root = worker_artifact_root(cfg.base_path, i);
        slot.worker_base = worker_ooc_base(slot.artifact_root);
        if (c_begin >= c_end) {
            // Empty chunk (degenerate when num_workers > available SQs).
            // Skip without forking.
            slot.finished = true;
            slot.success = true;
            slot.exit_status = 0;
        }
        slots.push_back(std::move(slot));
    }
    // Finish every allocation and path freeze before the first fork. From
    // this point until all children are reaped, parent-side operations are
    // bounded no-throw protocol calls or locally caught spawn/read failures.
    for (auto& slot : slots) {
        if (!slot.finished) {
            (void)start_attempt(slot);
        }
    }

    // Stage 2: wait, validate the complete finalized store, then converge the
    // exact lease before declaring an attempt successful.
    for (auto& slot : slots) {
        if (slot.finished)
            continue; // Empty chunk or spawn failure.
        if (!finish_attempt(slot)) {
            std::fprintf(stderr,
                         "[dist_sieve.master] worker chunk=%zu pid=%d FAILED "
                         "(exit=%d signal=%d), will retry\n",
                         slot.chunk_id, static_cast<int>(slot.pid), slot.exit_status, slot.signal);
        }
    }

    // Stage 3: single retry for any failed worker. The retry runs sequentially
    // (we already have all CPUs from the first wave; a second wave doesn't
    // benefit from parallelism if only a few workers failed).
    bool retries_reap_safe = std::all_of(
        slots.begin(), slots.end(), [](const WorkerSlot& slot) { return slot.reap_confirmed; });
    for (auto& slot : slots) {
        if (slot.success)
            continue;
        if (slot.sq_begin >= slot.sq_end)
            continue; // Empty chunks treated as success.
        if (!retries_reap_safe) {
            std::fprintf(
                stderr,
                "[dist_sieve.master] retry suppressed chunk=%zu: distributed wave contains an "
                "unreaped child\n",
                slot.chunk_id);
            continue;
        }

        std::fprintf(stderr, "[dist_sieve.master] retrying chunk=%zu sq_range=[%u,%u)\n",
                     slot.chunk_id, slot.sq_begin, slot.sq_end);
        if (!start_attempt(slot) || !finish_attempt(slot)) {
            std::fprintf(stderr,
                         "[dist_sieve.master] retry FAILED chunk=%zu (exit=%d signal=%d), "
                         "chunk contributes 0 relations\n",
                         slot.chunk_id, slot.exit_status, slot.signal);
        }
        if (!slot.reap_confirmed) {
            retries_reap_safe = false;
        }
    }

    // Stage 4: merge worker OOC stores into a single vector with cross-worker
    // (a, b) dedup. Per-worker collectors dedup within their chunk, but the
    // sieve can produce the same (a, b) from multiple Special-Q values (when
    // the algebraic side factorization happens to land on both q's prime in
    // FB), so adjacent chunks may emit overlapping relations. The in-process
    // sieve dedups across all SQs through a single collector — to preserve
    // that semantic we dedup on merge.
    std::vector<Relation> merged;
    // These are raw sieve rows: exact ABPair identity is assigned before any
    // source IDs or structured relation combinations exist. Inserting while
    // visiting workers preserves the first occurrence and its stable order.
    std::unordered_set<gnfs::core::ABPair, gnfs::core::ABPairHash> seen_ab;
    if (out_worker_stats)
        out_worker_stats->clear();
    if (out_worker_stats)
        out_worker_stats->reserve(slots.size());

    size_t dup_dropped = 0;
    for (auto& slot : slots) {
        DistributedSieveWorkerResult res;
        res.pid = slot.pid;
        res.chunk_id = slot.chunk_id;
        res.sq_index_begin = slot.sq_begin;
        res.sq_index_end = slot.sq_end;
        res.success = slot.success;
        res.exit_status = slot.exit_status;
        res.signal = slot.signal;
        res.attempt_count = slot.attempt_count;
        res.reap_confirmed = slot.reap_confirmed;
        res.ooc_base_path = slot.worker_base;
        res.sq_count = slot.sq_count;
        res.relations_count = slot.persisted_relation_count;

        if (slot.success && slot.sq_begin < slot.sq_end) {
            size_t added = 0;
            merged.reserve(merged.size() + slot.relations.size());
            seen_ab.reserve(seen_ab.size() + slot.relations.size());
            for (auto& r : slot.relations) {
                if (seen_ab.insert(r.ab()).second) {
                    merged.push_back(std::move(r));
                    ++added;
                } else {
                    ++dup_dropped;
                }
            }
            res.merged_relations_count = added;
        }

        if (out_worker_stats)
            out_worker_stats->push_back(std::move(res));
    }

    std::fprintf(stderr,
                 "[dist_sieve.master] complete: merged %zu relations from %zu workers "
                 "(dup_dropped=%zu)\n",
                 merged.size(), slots.size(), dup_dropped);

    return merged;
}

} // namespace gnfs::sieve

#endif

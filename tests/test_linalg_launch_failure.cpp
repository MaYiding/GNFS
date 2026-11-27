// Launch-failure coverage for the hand-written ThreadPool submit loops of the
// GF(2) solvers.
//
// A pool accepts tasks one submit at a time. When a later submit throws (for
// example std::bad_alloc), the tasks accepted earlier keep running, and the
// loops under test capture caller locals by reference. The checks below cover
// the guard that drains those tasks:
//
//  * FutureLaunchGuard: the destructor waits unless release() was called, and
//    it keeps the launch exception when the accepted callback throws.
//  * detail::spmv_transpose: an ordinal scan finds the first allocation that
//    the calling thread makes after the pool accepted a task. That task is
//    parked inside a MatrixView, the next submit fails at that allocation, and
//    the function must not return while the task is parked.
//  * BlockLanczos::find_dependencies: the parallel Gaussian branch runs for a
//    matrix with more than 2000 rows whose only pivot eliminates more rows
//    than the dynamic threshold. A sweep fails each main-thread allocation in
//    turn and compares the same-process retry with the fault-free result.
//
// Injection is thread-local. Only the thread that arms it can fail an
// allocation, and every pool worker starts with injection disabled.

#include <gnfs/linalg/block_lanczos.hpp>
#include <gnfs/linalg/detail/spmv_kernels.hpp>
#include <gnfs/linalg/sparse_matrix.hpp>
#include <gnfs/util/thread_pool.hpp>

#include "support/scoped_environment_stderr.hpp"
#include "support/test_check.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t disarmed = std::numeric_limits<std::size_t>::max();

// Armed only by the thread that sweeps allocation ordinals. The count lets a
// number of allocations through and fails the next one.
thread_local bool allocation_failure_armed = false;
thread_local std::size_t allocations_before_failure = disarmed;
thread_local bool injected_failure = false;
thread_local std::size_t triggered_faults = 0;
// Allocation count of this thread while enabled.
thread_local bool count_allocations = false;
thread_local std::size_t allocation_count = 0;

} // namespace

// Match the replacement-new convention used by test_factor_base_add_rollback.
// Keep these symbols visible in Release builds with LTO.
#if defined(__GNUC__) || defined(__clang__)
#define GNFS_TEST_ALLOCATION_REPLACEMENT __attribute__((used))
#else
#define GNFS_TEST_ALLOCATION_REPLACEMENT
#endif

// The replacement accesses state of the calling thread only. It can run while
// ThreadPool::submit holds the pool mutex, so it must not lock anything.
GNFS_TEST_ALLOCATION_REPLACEMENT void* operator new(std::size_t size) {
    if (allocation_failure_armed) {
        if (allocations_before_failure == 0) {
            allocation_failure_armed = false;
            injected_failure = true;
            ++triggered_faults;
            throw std::bad_alloc{};
        }
        --allocations_before_failure;
    }
    if (count_allocations) {
        ++allocation_count;
    }
    if (void* ptr = std::malloc(size == 0 ? 1 : size))
        return ptr;
    throw std::bad_alloc{};
}

GNFS_TEST_ALLOCATION_REPLACEMENT void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

GNFS_TEST_ALLOCATION_REPLACEMENT void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

#undef GNFS_TEST_ALLOCATION_REPLACEMENT

namespace {

using gnfs::linalg::BlockLanczos;
using gnfs::linalg::BlockVector;
using gnfs::linalg::CSRMatrix;
using gnfs::linalg::SparseMatrix;
using gnfs::tests::support::ScopedEnvironmentVariable;
using gnfs::util::FutureLaunchGuard;
using gnfs::util::ThreadPool;

// Upper bound for a task to start on a slow or loaded host.
constexpr std::chrono::milliseconds kStartBudget{10000};
// An early return shows up at once, so a short window is enough to observe it.
constexpr std::chrono::milliseconds kEarlyReturnWindow{200};
// Keep the CPU transpose path regardless of the Metal opt-in environment.
constexpr auto kCpuOnly = gnfs::linalg::detail::SpmvMetalPolicy::disabled;
// Upper bound for the ordinals scanned before one transpose launch.
constexpr std::size_t kMaxLaunchOrdinals = 64;

// Spin until `condition` holds or `budget` elapses. Returns false on timeout.
template <typename Condition>
bool wait_until(Condition&& condition, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

// ---------------------------------------------------------------------------
// FutureLaunchGuard
// ---------------------------------------------------------------------------

// After release() the destructor leaves the futures to the owner, which must
// still be able to observe every result.
void check_launch_guard_release_keeps_futures() {
    std::cout << "FutureLaunchGuard release keeps futures" << std::endl;
    ThreadPool pool(1);
    std::vector<std::future<void>> futures;
    futures.reserve(1);
    {
        FutureLaunchGuard guard(futures);
        futures.push_back(pool.submit([] {}));
        guard.release();
    }
    bool observed = false;
    try {
        futures.front().get();
        observed = true;
    } catch (const std::future_error&) {
        // The guard consumed the future after release().
    }
    GNFS_TEST_CHECK(observed);
}

struct LaunchOutcome {
    bool task_finished_before_return = false;
    bool kept_launch_failure = false;
};

// An exception thrown after one accepted task must not leave the scope before
// that task finishes, and a callback failure must not replace it.
void check_launch_guard_waits_for_accepted_task() {
    std::cout << "FutureLaunchGuard waits for accepted task" << std::endl;
    ThreadPool pool(1);
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> returned{false};

    auto caller = std::async(std::launch::async, [&] {
        LaunchOutcome outcome;
        std::vector<std::future<void>> futures;
        futures.reserve(1);
        std::string error;
        try {
            FutureLaunchGuard guard(futures);
            futures.push_back(pool.submit([&] {
                started.store(true, std::memory_order_release);
                while (!release.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                finished.store(true, std::memory_order_release);
                throw std::runtime_error("accepted callback failure");
            }));
            throw std::runtime_error("launch failure");
        } catch (const std::runtime_error& failure) {
            error = failure.what();
        }
        outcome.task_finished_before_return = finished.load(std::memory_order_acquire);
        outcome.kept_launch_failure = error == "launch failure";
        returned.store(true, std::memory_order_release);
        // Keep this thread alive until the task is released, so an early return
        // fails a check instead of destroying state the worker still uses.
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return outcome;
    });

    auto task_started = [&] { return started.load(std::memory_order_acquire); };
    auto caller_returned = [&] { return returned.load(std::memory_order_acquire); };
    const bool observed_start = wait_until(task_started, kStartBudget);
    // Release before checking so a failed check cannot strand the caller.
    const bool returned_before_release = wait_until(caller_returned, kEarlyReturnWindow);
    release.store(true, std::memory_order_release);
    const LaunchOutcome outcome = caller.get();

    GNFS_TEST_CHECK(observed_start);
    GNFS_TEST_CHECK(!returned_before_release);
    GNFS_TEST_CHECK(outcome.task_finished_before_return);
    GNFS_TEST_CHECK(outcome.kept_launch_failure);
}

// ---------------------------------------------------------------------------
// SpMV transpose
// ---------------------------------------------------------------------------

// State shared by the calling thread, the helper, and the worker that runs the
// first accepted transpose task.
struct ParkingState {
    std::atomic<bool> head_running{false};
    std::atomic<bool> release_head{false};
    std::atomic<bool> head_finished{false};
    std::atomic<bool> returned{false};
};

// MatrixView whose first row parks the transpose task that visits it until
// `release_head` is set. Only an accepted task reaches that row, so a parked
// task shows whether spmv_transpose returns before it finishes.
class ParkingMatrixView {
public:
    explicit ParkingMatrixView(ParkingState& state) : state_(state) {}

    [[nodiscard]] std::size_t num_rows() const noexcept {
        return 2;
    }
    [[nodiscard]] std::size_t num_cols() const noexcept {
        return 1;
    }
    [[nodiscard]] std::size_t nnz() const noexcept {
        return 2;
    }

    [[nodiscard]] const std::uint32_t* row_begin(std::size_t row) const {
        if (row == 0) {
            state_.head_running.store(true, std::memory_order_release);
            while (!state_.release_head.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            state_.head_finished.store(true, std::memory_order_release);
        }
        return columns_;
    }

    [[nodiscard]] const std::uint32_t* row_end(std::size_t) const noexcept {
        return columns_ + 1;
    }

    [[nodiscard]] std::size_t row_nnz(std::size_t row) const noexcept {
        return row < 2 ? std::size_t{1} : std::size_t{0};
    }

private:
    ParkingState& state_;
    std::uint32_t columns_[1] = {0};
};

static_assert(gnfs::linalg::MatrixView<ParkingMatrixView>, "the parking view is a MatrixView");

BlockVector scalar_transpose(const CSRMatrix& matrix, const BlockVector& x) {
    BlockVector y(matrix.num_cols());
    for (std::size_t row = 0; row < matrix.num_rows(); ++row) {
        const std::uint64_t xi = x.data[row];
        const std::uint32_t* entry = matrix.row_begin(row);
        const std::uint32_t* const end = matrix.row_end(row);
        for (; entry != end; ++entry) {
            y.data[*entry] ^= xi;
        }
    }
    return y;
}

struct LaunchTrial {
    bool caught_bad_alloc = false;
    bool unexpected_exception = false;
    bool injected = false;
    // The pool accepted the first transpose task and a worker ran it.
    bool accepted = false;
    bool head_finished_before_return = false;
    bool returned_before_release = false;
};

// Runs one transpose launch and fails allocation `ordinal` of the calling
// thread. A helper on another thread releases the parked task and reports an
// early return.
LaunchTrial run_launch_trial(std::size_t ordinal, const ParkingMatrixView& matrix,
                             const BlockVector& x, BlockVector& y, ThreadPool& pool,
                             ParkingState& state) {
    state.head_running.store(false);
    state.release_head.store(false);
    state.head_finished.store(false);
    state.returned.store(false);
    auto head_or_returned = [&] {
        if (state.head_running.load(std::memory_order_acquire)) {
            return true;
        }
        return state.returned.load(std::memory_order_acquire);
    };
    auto launch_returned = [&] { return state.returned.load(std::memory_order_acquire); };
    // The helper allocates on its own thread, so the ordinal sweep leaves it
    // alone.
    auto helper = std::async(std::launch::async, [&] {
        // Wait until the accepted task parks or the launch returns early.
        wait_until(head_or_returned, kStartBudget);
        const bool returned_before_release = wait_until(launch_returned, kEarlyReturnWindow);
        state.release_head.store(true, std::memory_order_release);
        return returned_before_release;
    });

    LaunchTrial trial;
    injected_failure = false;
    allocations_before_failure = ordinal;
    allocation_failure_armed = true;
    try {
        gnfs::linalg::detail::spmv_transpose(matrix, x, y, pool, kCpuOnly);
    } catch (const std::bad_alloc&) {
        trial.caught_bad_alloc = true;
    } catch (...) {
        trial.unexpected_exception = true;
    }
    // Checks below must not consume the injected budget.
    allocation_failure_armed = false;
    allocations_before_failure = disarmed;
    trial.injected = injected_failure;
    trial.head_finished_before_return = state.head_finished.load(std::memory_order_acquire);
    state.returned.store(true, std::memory_order_release);
    trial.returned_before_release = helper.get();
    // An accepted task has run to completion once the helper released it.
    pool.wait_all();
    trial.accepted = state.head_running.load(std::memory_order_acquire);
    return trial;
}

// Every ordinal before the first allocation that follows an accepted task
// fails in the reservation or inside the first submit, before the pool holds
// any task. The first ordinal with an accepted task belongs to the next
// submit, and the transpose must then wait for the parked task.
void check_transpose_launch_failure_drains_accepted_task() {
    std::cout << "SpMV transpose launch failure drains accepted task" << std::endl;
    ThreadPool pool(2);
    ParkingState state;
    ParkingMatrixView matrix(state);
    BlockVector x(2);
    x.data[0] = 1;
    x.data[1] = 1;
    BlockVector y(1);

    // A released park lets this launch finish. It also creates the transpose
    // scratch of the calling thread, so every trial below allocates alike.
    state.release_head.store(true);
    gnfs::linalg::detail::spmv_transpose(matrix, x, y, pool, kCpuOnly);
    GNFS_TEST_CHECK(state.head_finished.load(std::memory_order_acquire));

    bool found = false;
    std::size_t found_ordinal = kMaxLaunchOrdinals;
    LaunchTrial trial;
    for (std::size_t ordinal = 0; ordinal < kMaxLaunchOrdinals; ++ordinal) {
        trial = run_launch_trial(ordinal, matrix, x, y, pool, state);
        if (trial.caught_bad_alloc && trial.accepted) {
            found = true;
            found_ordinal = ordinal;
            break;
        }
        // An earlier ordinal fails before the pool accepts any task.
        GNFS_TEST_CHECK(trial.caught_bad_alloc);
        GNFS_TEST_CHECK(!trial.accepted);
    }
    std::cout << "  first ordinal after an accepted task=" << found_ordinal << '\n';
    GNFS_TEST_CHECK(found);
    // The futures reservation and the first submit come before that ordinal.
    GNFS_TEST_CHECK(found_ordinal >= 2);
    GNFS_TEST_CHECK(trial.injected);
    GNFS_TEST_CHECK(!trial.unexpected_exception);
    GNFS_TEST_CHECK(!trial.returned_before_release);
    GNFS_TEST_CHECK(trial.head_finished_before_return);

    // The pool and the transpose stay usable after the launch failure.
    GNFS_TEST_CHECK(pool.submit([] { return 42; }).get() == 42);
    SparseMatrix storage(3, 4);
    storage.set(0, 0);
    storage.set(0, 1);
    storage.set(1, 1);
    storage.set(1, 2);
    storage.set(1, 3);
    storage.set(2, 0);
    storage.set(2, 3);
    CSRMatrix csr(storage);
    BlockVector input(3);
    input.data[0] = 1;
    input.data[1] = 1;
    input.data[2] = 1;
    BlockVector output(4);
    gnfs::linalg::detail::spmv_transpose(csr, input, output, pool, kCpuOnly);
    const BlockVector expected = scalar_transpose(csr, input);
    GNFS_TEST_CHECK(output.length == expected.length);
    GNFS_TEST_CHECK(output.data == expected.data);
}

// ---------------------------------------------------------------------------
// Parallel Gaussian elimination
// ---------------------------------------------------------------------------

constexpr std::size_t kMaxDeps = 4;
// Rows by which the first pivot must clear the dynamic threshold.
constexpr std::size_t kPivotMargin = 250;

// Thread count that find_dependencies_sparse derives on this host.
std::size_t solver_thread_count() {
    const unsigned hardware = std::thread::hardware_concurrency();
    return hardware == 0 ? std::size_t{4} : static_cast<std::size_t>(hardware);
}

// Same expression as the dynamic threshold in find_dependencies_sparse, so the
// fixture can show that its only pivot takes the parallel branch.
std::size_t gaussian_dynamic_threshold(std::size_t rows, std::size_t threads) {
    const std::size_t words_per_row = std::max<std::size_t>((rows + 64) / 64, std::size_t{100});
    const std::size_t raw = std::size_t{100000} * threads / words_per_row;
    return std::clamp<std::size_t>(raw, std::size_t{500}, std::size_t{10000});
}

// Entries in the dense fixture's only column: nine out of ten rows.
std::size_t dense_ones(std::size_t rows) {
    return rows - rows / 10;
}

// Smallest row count on a coarse grid whose only pivot eliminates enough rows.
std::size_t choose_gaussian_rows(std::size_t threads) {
    for (std::size_t rows = 2048;; rows += 64) {
        if (dense_ones(rows) - 1 > gaussian_dynamic_threshold(rows, threads) + kPivotMargin) {
            return rows;
        }
    }
}

// One matrix column with `ones` entries on randomly chosen rows. Every other
// entry is zero, so the solver performs a single pivot.
SparseMatrix make_single_column_matrix(std::size_t rows, std::size_t ones, std::uint64_t seed) {
    SparseMatrix matrix(rows, 1);
    std::vector<std::size_t> order(rows);
    for (std::size_t row = 0; row < rows; ++row) {
        order[row] = row;
    }
    std::mt19937_64 rng(seed);
    std::shuffle(order.begin(), order.end(), rng);
    for (std::size_t i = 0; i < ones; ++i) {
        matrix.set(order[i], 0);
    }
    // Sorted rows keep the solver's ensure_all_sorted() free of allocations, so
    // the sweep only visits allocation sites that belong to the solver.
    matrix.ensure_all_sorted();
    return matrix;
}

// Every dependency of a one-column matrix must be nonzero and orthogonal to
// that column, and the solver must fill the requested budget.
void check_dependencies(const std::vector<std::vector<bool>>& dependencies,
                        const SparseMatrix& matrix) {
    GNFS_TEST_CHECK(dependencies.size() == kMaxDeps);
    for (const std::vector<bool>& dependency : dependencies) {
        GNFS_TEST_CHECK(dependency.size() == matrix.num_rows());
        bool nonzero = false;
        bool parity = false;
        for (std::size_t row = 0; row < matrix.num_rows(); ++row) {
            if (!dependency[row]) {
                continue;
            }
            nonzero = true;
            if (matrix.test(row, 0)) {
                parity = !parity;
            }
        }
        GNFS_TEST_CHECK(nonzero);
        // The dependency is orthogonal to the column.
        GNFS_TEST_CHECK(!parity);
    }
}

void check_gaussian_launch_failure_sweep() {
    std::cout << "Gaussian launch failure sweep" << std::endl;
    // A checkpoint or debug trace would add allocation sites to the sweep.
    const ScopedEnvironmentVariable unset_checkpoint("GNFS_BL_CHECKPOINT", nullptr);
    const ScopedEnvironmentVariable unset_trace("GNFS_DEBUG_GAUSSIAN", nullptr);
    const unsigned hardware = std::thread::hardware_concurrency();
    const std::size_t threads = solver_thread_count();
    const std::size_t rows = choose_gaussian_rows(threads);
    const std::size_t threshold = gaussian_dynamic_threshold(rows, threads);
    const std::size_t eliminated = dense_ones(rows) - 1;
    const auto dense = make_single_column_matrix(rows, dense_ones(rows), std::uint64_t{0x51ed270b});
    // Five entries leave four rows to eliminate, below every threshold, and
    // yield as many dependencies as the dense fixture.
    const auto sparse = make_single_column_matrix(rows, 5, std::uint64_t{0x1f83d9ab});
    const std::size_t max_fault_ordinals = 64 + 8 * threads;
    std::cout << "  hardware_concurrency=" << hardware << ", solver threads=" << threads << '\n';
    std::cout << "  rows=" << rows << ", eliminated rows=" << eliminated << '\n';
    std::cout << "  threshold=" << threshold << ", sweep limit=" << max_fault_ordinals << '\n';
    if (threads == 1) {
        std::cout << "  Single hardware thread: the solver stays serial.\n";
        std::cout << "  This run covers allocation failure and retry only.\n";
    } else {
        // Above 2000 rows the solver creates a pool, and the pivot must clear
        // the threshold for the elimination to be submitted.
        GNFS_TEST_CHECK(rows > 2000);
        GNFS_TEST_CHECK(eliminated > threshold);
    }

    // The fault-free result is the oracle for every retry.
    const auto reference = BlockLanczos{}.find_dependencies(dense, kMaxDeps);
    check_dependencies(reference, dense);

    // Allocations of one serial solve. A parallel solve adds one reservation
    // and at least one allocation per submitted chunk.
    allocation_count = 0;
    count_allocations = true;
    const auto serial = BlockLanczos{}.find_dependencies(sparse, kMaxDeps);
    count_allocations = false;
    const std::size_t serial_allocations = allocation_count;
    check_dependencies(serial, sparse);

    // The transpose trials above ran on this thread and raised faults of their
    // own. Count the faults of this sweep from zero.
    triggered_faults = 0;
    std::size_t caught_faults = 0;
    std::size_t no_fault_ordinal = max_fault_ordinals;
    for (std::size_t ordinal = 0; ordinal < max_fault_ordinals; ++ordinal) {
        std::vector<std::vector<bool>> result;
        bool failed = false;
        injected_failure = false;
        allocations_before_failure = ordinal;
        allocation_failure_armed = true;
        try {
            result = BlockLanczos{}.find_dependencies(dense, kMaxDeps);
        } catch (const std::bad_alloc&) {
            failed = true;
        } catch (...) {
            allocation_failure_armed = false;
            throw;
        }
        // Checks and retries below must not consume the injected budget.
        allocation_failure_armed = false;
        allocations_before_failure = disarmed;
        if (failed) {
            GNFS_TEST_CHECK(injected_failure);
            ++caught_faults;
            GNFS_TEST_CHECK(triggered_faults == caught_faults);
            const auto retry = BlockLanczos{}.find_dependencies(dense, kMaxDeps);
            GNFS_TEST_CHECK(retry == reference);
            std::cout << "  ordinal " << ordinal << " raised bad_alloc; retry matched\n";
            continue;
        }
        GNFS_TEST_CHECK(!injected_failure);
        GNFS_TEST_CHECK(triggered_faults == caught_faults);
        GNFS_TEST_CHECK(result == reference);
        GNFS_TEST_CHECK(caught_faults > 0);
        no_fault_ordinal = ordinal;
        break;
    }
    std::cout << "  caught " << caught_faults << " faults\n";
    std::cout << "  first no-fault ordinal=" << no_fault_ordinal << '\n';
    std::cout << "  serial solve allocations=" << serial_allocations << '\n';
    // The sweep must pass every allocation site of the dense solve.
    GNFS_TEST_CHECK(no_fault_ordinal < max_fault_ordinals);
    if (threads > 1) {
        GNFS_TEST_CHECK(no_fault_ordinal >= serial_allocations + 1 + threads);
    }
}

} // namespace

int main() {
    std::cout << "=== linalg launch failure ===\n";
    check_launch_guard_release_keeps_futures();
    check_launch_guard_waits_for_accepted_task();
    check_transpose_launch_failure_drains_accepted_task();
    check_gaussian_launch_failure_sweep();
    std::cout << "All launch failure checks passed\n";
    return 0;
}

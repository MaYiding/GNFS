#pragma once

/// @file shadow_matrix.hpp
/// @brief Deterministic GF(2) left-nullspace solving for canonical SIQS shadow rows.

#include <gnfs/siqs/shadow_assembly.hpp>
#include <gnfs/util/joining_thread.hpp>
#include <gnfs/util/thread_pool.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace gnfs::siqs {

using std::size_t;

inline constexpr size_t SIQS_SHADOW_DEFAULT_MAX_DENSE_MATRIX_BYTES =
    size_t{256} * size_t{1024} * size_t{1024};
inline constexpr size_t SIQS_SHADOW_DEFAULT_MAX_DENSE_VARIABLE_COUNT = 100'000;

struct SIQSShadowMatrixOptions {
    size_t max_dependencies = 64;
    uint32_t elimination_workers = 1;
    size_t parallel_column_threshold = 20'000;
    /// Budget for the packed M^T payload, excluding input rows and metadata.
    size_t max_dense_matrix_bytes = SIQS_SHADOW_DEFAULT_MAX_DENSE_MATRIX_BYTES;
    /// Maximum shadow-row variables admitted by the deterministic dense backend.
    size_t max_dense_variable_count = SIQS_SHADOW_DEFAULT_MAX_DENSE_VARIABLE_COUNT;

    [[nodiscard]] friend constexpr bool operator==(const SIQSShadowMatrixOptions&,
                                                   const SIQSShadowMatrixOptions&) = default;
};

struct SIQSShadowMatrixSolution {
    size_t row_count = 0;
    size_t column_count = 0;
    std::vector<std::vector<size_t>> dependencies;

    [[nodiscard]] friend bool operator==(const SIQSShadowMatrixSolution&,
                                         const SIQSShadowMatrixSolution&) = default;
};

enum class SIQSShadowMatrixStatus : uint8_t {
    valid,
    invalid_modulus,
    invalid_factor_base,
    invalid_options,
    size_overflow,
    invalid_row,
    row_identity_mismatch,
    worker_failure,
    internal_invariant_failure,
    resource_limit,
    unsupported_backend,
};

/// Return the exact packed M^T allocation size for the dense shadow backend.
/// @param variable_count Number of shadow rows.
/// @param equation_count Factor-base columns, including the sign sentinel.
[[nodiscard]] inline constexpr std::optional<size_t>
checked_siqs_shadow_dense_matrix_bytes(size_t variable_count, size_t equation_count) noexcept {
    size_t words_per_equation = variable_count / size_t{64};
    if ((variable_count % size_t{64}) != 0) {
        ++words_per_equation;
    }
    if (equation_count != 0 &&
        words_per_equation > std::numeric_limits<size_t>::max() / equation_count) {
        return std::nullopt;
    }
    const size_t matrix_word_count = equation_count * words_per_equation;
    if (matrix_word_count > std::numeric_limits<size_t>::max() / sizeof(uint64_t)) {
        return std::nullopt;
    }
    return matrix_word_count * sizeof(uint64_t);
}

/// Invariant-safe result: a solution is present exactly when status() is valid.
class SIQSShadowMatrixResult {
public:
    SIQSShadowMatrixResult(const SIQSShadowMatrixResult&) = default;
    SIQSShadowMatrixResult& operator=(const SIQSShadowMatrixResult& other) {
        if (this != &other) {
            SIQSShadowMatrixResult copy(other);
            *this = std::move(copy);
        }
        return *this;
    }

    SIQSShadowMatrixResult(SIQSShadowMatrixResult&& other) noexcept
        : status_(other.status_), solution_(std::move(other.solution_)) {
        other.status_ = SIQSShadowMatrixStatus::internal_invariant_failure;
        other.solution_.reset();
    }

    SIQSShadowMatrixResult& operator=(SIQSShadowMatrixResult&& other) noexcept {
        if (this != &other) {
            status_ = other.status_;
            solution_ = std::move(other.solution_);
            other.status_ = SIQSShadowMatrixStatus::internal_invariant_failure;
            other.solution_.reset();
        }
        return *this;
    }

    [[nodiscard]] SIQSShadowMatrixStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] const std::optional<SIQSShadowMatrixSolution>& solution() const noexcept {
        return solution_;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return status_ == SIQSShadowMatrixStatus::valid && solution_.has_value();
    }

private:
    friend SIQSShadowMatrixResult
    solve_siqs_shadow_matrix(std::span<const SIQSShadowRow> rows,
                             std::span<const uint32_t> factor_base_primes,
                             const core::Integer& modulus, const SIQSShadowMatrixOptions& options);

    SIQSShadowMatrixResult(SIQSShadowMatrixStatus status,
                           std::optional<SIQSShadowMatrixSolution> solution)
        : status_(status), solution_(std::move(solution)) {}

    [[nodiscard]] static SIQSShadowMatrixResult failure(SIQSShadowMatrixStatus status) {
        if (status == SIQSShadowMatrixStatus::valid) {
            status = SIQSShadowMatrixStatus::internal_invariant_failure;
        }
        return SIQSShadowMatrixResult(status, std::nullopt);
    }

    [[nodiscard]] static SIQSShadowMatrixResult success(SIQSShadowMatrixSolution solution) {
        return SIQSShadowMatrixResult(SIQSShadowMatrixStatus::valid, std::move(solution));
    }

    SIQSShadowMatrixStatus status_;
    std::optional<SIQSShadowMatrixSolution> solution_;
};

namespace shadow_matrix_detail {

[[nodiscard]] inline bool has_known_origin(SIQSShadowRowOrigin origin) noexcept {
    return origin == SIQSShadowRowOrigin::raw_full ||
           origin == SIQSShadowRowOrigin::large_prime_cycle;
}

[[nodiscard]] inline size_t leftmost_set_bit(std::span<const uint64_t> row,
                                             size_t bit_count) noexcept {
    for (size_t word_index = 0; word_index < row.size(); ++word_index) {
        const uint64_t word = row[word_index];
        if (word == 0) {
            continue;
        }
        const size_t bit_index =
            word_index * size_t{64} + static_cast<size_t>(std::countr_zero(word));
        return bit_index < bit_count ? bit_index : std::numeric_limits<size_t>::max();
    }
    return std::numeric_limits<size_t>::max();
}

inline void eliminate_pivot_range(std::vector<uint64_t>& matrix, size_t words_per_row,
                                  size_t pivot_row, size_t pivot_column, size_t begin, size_t end) {
    const size_t pivot_offset = pivot_row * words_per_row;
    const size_t pivot_word = pivot_column / size_t{64};
    const uint64_t pivot_mask = uint64_t{1} << (pivot_column % size_t{64});

    for (size_t row = begin; row < end; ++row) {
        if (row == pivot_row) {
            continue;
        }
        const size_t row_offset = row * words_per_row;
        if ((matrix[row_offset + pivot_word] & pivot_mask) == 0) {
            continue;
        }
        for (size_t word = 0; word < words_per_row; ++word) {
            matrix[row_offset + word] ^= matrix[pivot_offset + word];
        }
    }
}

using EliminatePivotRangeFunction = void (*)(std::vector<uint64_t>&, size_t, size_t, size_t, size_t,
                                             size_t);
using PivotWorkerStartupHook = void (*)(size_t);

/// Fixed-partition workers retained for the lifetime of one shadow solve.
///
/// Every ThreadPool worker receives exactly one long-lived task. A pivot uses
/// a mutex-protected generation and completion count, so dispatch performs no
/// task, future, or vector allocation. The matrix pointer and its allocation
/// remain stable for the complete lifetime of this team.
class PersistentPivotEliminationTeam final {
public:
    PersistentPivotEliminationTeam(std::vector<uint64_t>& matrix, size_t equation_count,
                                   size_t words_per_row, size_t worker_count,
                                   EliminatePivotRangeFunction eliminate_range,
                                   PivotWorkerStartupHook startup_hook)
        : state_(std::make_shared<State>(matrix, equation_count, words_per_row, worker_count,
                                         eliminate_range)),
          pool_(static_cast<uint32_t>(worker_count)) {
        try {
            for (size_t worker = 0; worker < worker_count; ++worker) {
                if (startup_hook != nullptr) {
                    startup_hook(worker);
                }
                auto completion = pool_.submit(
                    [state = state_, worker]() noexcept { run_worker(state, worker); });
                (void)completion;
            }
        } catch (...) {
            // Submitted tasks may already be waiting for the first generation.
            // Release them before ThreadPool joins during constructor unwind.
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                state_->cancelled = true;
            }
            state_->work_available.notify_all();
            throw;
        }
    }

    ~PersistentPivotEliminationTeam() {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->stopping = true;
        }
        state_->work_available.notify_all();
    }

    PersistentPivotEliminationTeam(const PersistentPivotEliminationTeam&) = delete;
    PersistentPivotEliminationTeam& operator=(const PersistentPivotEliminationTeam&) = delete;
    PersistentPivotEliminationTeam(PersistentPivotEliminationTeam&&) = delete;
    PersistentPivotEliminationTeam& operator=(PersistentPivotEliminationTeam&&) = delete;

    [[nodiscard]] SIQSShadowMatrixStatus eliminate(size_t pivot_row, size_t pivot_column) {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->worker_failed = false;
            state_->pivot_row = pivot_row;
            state_->pivot_column = pivot_column;
            state_->remaining_workers = state_->worker_count;
            ++state_->generation;
        }
        state_->work_available.notify_all();

        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->work_complete.wait(lock, [&] { return state_->remaining_workers == 0; });
        return state_->worker_failed ? SIQSShadowMatrixStatus::worker_failure
                                     : SIQSShadowMatrixStatus::valid;
    }

private:
    struct State final {
        State(std::vector<uint64_t>& matrix_arg, size_t equation_count_arg,
              size_t words_per_row_arg, size_t worker_count_arg,
              EliminatePivotRangeFunction eliminate_range_arg)
            : matrix(&matrix_arg), equation_count(equation_count_arg),
              words_per_row(words_per_row_arg), worker_count(worker_count_arg),
              eliminate_range(eliminate_range_arg) {}

        std::vector<uint64_t>* matrix;
        size_t equation_count;
        size_t words_per_row;
        size_t worker_count;
        EliminatePivotRangeFunction eliminate_range;
        std::mutex mutex;
        std::condition_variable work_available;
        std::condition_variable work_complete;
        bool cancelled = false;
        bool worker_failed = false;
        bool stopping = false;
        size_t generation = 0;
        size_t remaining_workers = 0;
        size_t pivot_row = 0;
        size_t pivot_column = 0;
    };

    static void run_worker(const std::shared_ptr<State>& state, size_t worker) noexcept {
        const size_t base_range = state->equation_count / state->worker_count;
        const size_t remainder = state->equation_count % state->worker_count;
        const size_t begin = worker * base_range + std::min(worker, remainder);
        const size_t end = begin + base_range + (worker < remainder ? size_t{1} : size_t{0});
        size_t observed_generation = 0;
        std::unique_lock<std::mutex> lock(state->mutex);

        while (true) {
            state->work_available.wait(lock, [&] {
                return state->cancelled || state->stopping ||
                       state->generation != observed_generation;
            });
            if (state->cancelled || state->stopping) {
                return;
            }

            observed_generation = state->generation;
            const size_t pivot_row = state->pivot_row;
            const size_t pivot_column = state->pivot_column;
            lock.unlock();

            bool failed = false;
            try {
                state->eliminate_range(*state->matrix, state->words_per_row, pivot_row,
                                       pivot_column, begin, end);
            } catch (...) {
                failed = true;
            }

            lock.lock();
            if (failed) {
                state->worker_failed = true;
            }
            if (state->remaining_workers == 0) {
                state->worker_failed = true;
                state->work_complete.notify_one();
                return;
            }
            --state->remaining_workers;
            if (state->remaining_workers == 0) {
                state->work_complete.notify_one();
            }
        }
    }

    // state_ must outlive pool_: ThreadPool joins the long-lived tasks before
    // the shared state is released during reverse member destruction.
    std::shared_ptr<State> state_;
    util::ThreadPool pool_;
};

[[nodiscard]] inline SIQSShadowMatrixStatus create_persistent_pivot_elimination_team(
    std::vector<uint64_t>& matrix, size_t equation_count, size_t words_per_row, size_t worker_count,
    std::unique_ptr<PersistentPivotEliminationTeam>& output,
    EliminatePivotRangeFunction eliminate_range = eliminate_pivot_range,
    PivotWorkerStartupHook startup_hook = nullptr) noexcept {
    output.reset();
    if (worker_count == 0 || eliminate_range == nullptr ||
        worker_count > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        return SIQSShadowMatrixStatus::worker_failure;
    }

    try {
        output = std::make_unique<PersistentPivotEliminationTeam>(
            matrix, equation_count, words_per_row, worker_count, eliminate_range, startup_hook);
    } catch (...) {
        return SIQSShadowMatrixStatus::worker_failure;
    }
    return SIQSShadowMatrixStatus::valid;
}

[[nodiscard]] inline SIQSShadowMatrixStatus
eliminate_pivot(std::vector<uint64_t>& matrix, size_t equation_count, size_t words_per_row,
                size_t pivot_row, size_t pivot_column, const SIQSShadowMatrixOptions& options) {
    const bool use_parallel =
        options.elimination_workers > 1 && equation_count >= options.parallel_column_threshold;
    if (!use_parallel) {
        eliminate_pivot_range(matrix, words_per_row, pivot_row, pivot_column, 0, equation_count);
        return SIQSShadowMatrixStatus::valid;
    }

    const size_t worker_count =
        std::min(equation_count, static_cast<size_t>(options.elimination_workers));
    std::atomic<bool> worker_failed{false};
    try {
        std::vector<gnfs::util::JoiningThread> workers;
        workers.reserve(worker_count);
        const size_t base_range = equation_count / worker_count;
        const size_t remainder = equation_count % worker_count;
        size_t begin = 0;
        for (size_t worker = 0; worker < worker_count; ++worker) {
            const size_t range_size = base_range + (worker < remainder ? size_t{1} : size_t{0});
            const size_t end = begin + range_size;
            workers.emplace_back([&, begin, end]() noexcept {
                try {
                    eliminate_pivot_range(matrix, words_per_row, pivot_row, pivot_column, begin,
                                          end);
                } catch (...) {
                    worker_failed.store(true, std::memory_order_relaxed);
                }
            });
            begin = end;
        }
    } catch (...) {
        return SIQSShadowMatrixStatus::worker_failure;
    }
    return worker_failed.load(std::memory_order_relaxed) ? SIQSShadowMatrixStatus::worker_failure
                                                         : SIQSShadowMatrixStatus::valid;
}

[[nodiscard]] inline bool dependency_is_null(std::span<const size_t> dependency,
                                             std::span<const uint64_t> reduced_matrix,
                                             size_t equation_count, size_t words_per_row,
                                             size_t variable_count,
                                             std::vector<uint64_t>& packed_dependency) noexcept {
    if (dependency.empty() || packed_dependency.size() != words_per_row) {
        return false;
    }
    std::fill(packed_dependency.begin(), packed_dependency.end(), uint64_t{0});

    size_t previous = 0;
    bool have_previous = false;
    for (const size_t variable : dependency) {
        if (variable >= variable_count || (have_previous && variable <= previous)) {
            return false;
        }
        packed_dependency[variable / size_t{64}] |= uint64_t{1} << (variable % size_t{64});
        previous = variable;
        have_previous = true;
    }

    for (size_t row = 0; row < equation_count; ++row) {
        const size_t row_offset = row * words_per_row;
        unsigned parity = 0;
        for (size_t word = 0; word < words_per_row; ++word) {
            parity ^= static_cast<unsigned>(std::popcount(reduced_matrix[row_offset + word] &
                                                          packed_dependency[word])) &
                      1U;
        }
        if (parity != 0) {
            return false;
        }
    }
    return true;
}

} // namespace shadow_matrix_detail

/// Build packed M^T directly from canonical wide rows and find row dependencies.
///
/// Factor-base columns remain equations while input rows are variables. Reduction
/// always chooses the leftmost bit of each equation as its pivot. Parallel
/// elimination partitions equation rows into fixed contiguous ranges; the pivot
/// row is read-only and every other equation row has exactly one writer.
[[nodiscard]] inline SIQSShadowMatrixResult
solve_siqs_shadow_matrix(std::span<const SIQSShadowRow> rows,
                         std::span<const uint32_t> factor_base_primes, const core::Integer& modulus,
                         const SIQSShadowMatrixOptions& options = {}) {
    using namespace shadow_matrix_detail;

    if (!post_merge_row_detail::has_valid_modulus(modulus)) {
        return SIQSShadowMatrixResult::failure(SIQSShadowMatrixStatus::invalid_modulus);
    }
    if (!post_merge_row_detail::has_valid_factor_base(factor_base_primes)) {
        return SIQSShadowMatrixResult::failure(SIQSShadowMatrixStatus::invalid_factor_base);
    }
    if (options.max_dependencies == 0 || options.elimination_workers == 0) {
        return SIQSShadowMatrixResult::failure(SIQSShadowMatrixStatus::invalid_options);
    }

    for (const SIQSShadowRow& shadow_row : rows) {
        if (!has_known_origin(shadow_row.origin)) {
            return SIQSShadowMatrixResult::failure(SIQSShadowMatrixStatus::invalid_row);
        }
        const SIQSPostMergeRowStatus row_status =
            post_merge_row_detail::check_siqs_post_merge_row_identity_prevalidated(
                shadow_row.row, factor_base_primes, modulus);
        if (row_status == SIQSPostMergeRowStatus::row_identity_mismatch) {
            return SIQSShadowMatrixResult::failure(SIQSShadowMatrixStatus::row_identity_mismatch);
        }
        if (row_status != SIQSPostMergeRowStatus::valid) {
            return SIQSShadowMatrixResult::failure(SIQSShadowMatrixStatus::invalid_row);
        }
    }

    const size_t variable_count = rows.size();
    const size_t equation_count = factor_base_primes.size();
    const auto dense_matrix_bytes =
        checked_siqs_shadow_dense_matrix_bytes(variable_count, equation_count);
    if (!dense_matrix_bytes) {
        return SIQSShadowMatrixResult::failure(SIQSShadowMatrixStatus::size_overflow);
    }
    if (variable_count == 0) {
        return SIQSShadowMatrixResult::success(SIQSShadowMatrixSolution{0, equation_count, {}});
    }
    if (variable_count > options.max_dense_variable_count) {
        return SIQSShadowMatrixResult::failure(SIQSShadowMatrixStatus::unsupported_backend);
    }
    if (*dense_matrix_bytes > options.max_dense_matrix_bytes) {
        return SIQSShadowMatrixResult::failure(SIQSShadowMatrixStatus::resource_limit);
    }

    const size_t dependency_limit = std::min(variable_count, options.max_dependencies);
    const size_t dependency_reserve =
        equation_count < variable_count ? equation_count + size_t{1} : variable_count;
    const size_t words_per_row =
        variable_count / size_t{64} + ((variable_count % size_t{64}) != 0 ? size_t{1} : size_t{0});
    const size_t matrix_word_count = *dense_matrix_bytes / sizeof(uint64_t);
    if (matrix_word_count > std::vector<uint64_t>{}.max_size() ||
        equation_count > std::vector<size_t>{}.max_size() ||
        variable_count > std::vector<uint8_t>{}.max_size() ||
        words_per_row > std::vector<uint64_t>{}.max_size() ||
        dependency_limit > std::vector<std::vector<size_t>>{}.max_size() ||
        dependency_reserve > std::vector<size_t>{}.max_size()) {
        return SIQSShadowMatrixResult::failure(SIQSShadowMatrixStatus::size_overflow);
    }

    std::vector<uint64_t> matrix(matrix_word_count, uint64_t{0});
    for (size_t row = 0; row < variable_count; ++row) {
        const size_t variable_word = row / size_t{64};
        const uint64_t variable_mask = uint64_t{1} << (row % size_t{64});
        visit_siqs_post_merge_odd_columns(rows[row].row, [&](size_t column) {
            matrix[column * words_per_row + variable_word] |= variable_mask;
        });
    }

    const size_t no_pivot = std::numeric_limits<size_t>::max();
    std::vector<size_t> pivot_columns(equation_count, no_pivot);
    std::vector<uint8_t> is_pivot(variable_count, uint8_t{0});
    const bool use_parallel_elimination =
        options.elimination_workers > 1 && equation_count >= options.parallel_column_threshold;
    const size_t elimination_worker_count =
        std::min(equation_count, static_cast<size_t>(options.elimination_workers));
    std::unique_ptr<PersistentPivotEliminationTeam> elimination_team;
    for (size_t equation = 0; equation < equation_count; ++equation) {
        const size_t equation_offset = equation * words_per_row;
        const size_t pivot_column = leftmost_set_bit(
            std::span<const uint64_t>(matrix.data() + equation_offset, words_per_row),
            variable_count);
        if (pivot_column == no_pivot) {
            continue;
        }
        if (is_pivot[pivot_column] != 0) {
            return SIQSShadowMatrixResult::failure(
                SIQSShadowMatrixStatus::internal_invariant_failure);
        }
        pivot_columns[equation] = pivot_column;
        is_pivot[pivot_column] = uint8_t{1};

        SIQSShadowMatrixStatus elimination_status = SIQSShadowMatrixStatus::valid;
        if (use_parallel_elimination) {
            if (!elimination_team) {
                elimination_status = create_persistent_pivot_elimination_team(
                    matrix, equation_count, words_per_row, elimination_worker_count,
                    elimination_team);
            }
            if (elimination_status == SIQSShadowMatrixStatus::valid) {
                elimination_status = elimination_team->eliminate(equation, pivot_column);
            }
        } else {
            eliminate_pivot_range(matrix, words_per_row, equation, pivot_column, 0, equation_count);
        }
        if (elimination_status != SIQSShadowMatrixStatus::valid) {
            return SIQSShadowMatrixResult::failure(elimination_status);
        }
    }

    SIQSShadowMatrixSolution solution;
    solution.row_count = variable_count;
    solution.column_count = equation_count;
    solution.dependencies.reserve(dependency_limit);
    for (size_t free_column = 0;
         free_column < variable_count && solution.dependencies.size() < options.max_dependencies;
         ++free_column) {
        if (is_pivot[free_column] != 0) {
            continue;
        }

        std::vector<size_t> dependency;
        dependency.reserve(dependency_reserve);
        dependency.push_back(free_column);
        const size_t free_word = free_column / size_t{64};
        const uint64_t free_mask = uint64_t{1} << (free_column % size_t{64});
        for (size_t equation = 0; equation < equation_count; ++equation) {
            if (pivot_columns[equation] != no_pivot &&
                (matrix[equation * words_per_row + free_word] & free_mask) != 0) {
                dependency.push_back(pivot_columns[equation]);
            }
        }
        std::sort(dependency.begin(), dependency.end());
        if (std::adjacent_find(dependency.begin(), dependency.end()) != dependency.end()) {
            return SIQSShadowMatrixResult::failure(
                SIQSShadowMatrixStatus::internal_invariant_failure);
        }
        solution.dependencies.push_back(std::move(dependency));
    }

    std::vector<uint64_t> packed_dependency(words_per_row, uint64_t{0});
    for (const auto& dependency : solution.dependencies) {
        if (!dependency_is_null(dependency, matrix, equation_count, words_per_row, variable_count,
                                packed_dependency)) {
            return SIQSShadowMatrixResult::failure(
                SIQSShadowMatrixStatus::internal_invariant_failure);
        }
    }

    return SIQSShadowMatrixResult::success(std::move(solution));
}

} // namespace gnfs::siqs

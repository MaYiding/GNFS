// SpMV transpose scratch benchmark.
//
// Usage:
//   spmv_transpose_scratch [rows] [cols] [nnz_per_row] [threads] [reps]
//
// The generated matrix and input vector are deterministic so this binary can
// compare scratch implementations built from different revisions. The final
// checksum keeps the measured result observable without adding work inside
// the timed loop.

#include <gnfs/linalg/detail/spmv_kernels.hpp>
#include <gnfs/linalg/sparse_matrix.hpp>
#include <gnfs/util/thread_pool.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>
#include <vector>

using gnfs::linalg::BlockVector;
using gnfs::linalg::CSRMatrix;
using gnfs::linalg::SparseMatrix;

namespace {

constexpr std::ptrdiff_t kPrefetchAhead = 8;

SparseMatrix build_matrix(std::size_t rows, std::size_t cols, std::size_t nnz_per_row) {
    SparseMatrix matrix(rows, cols);
    if (cols == 0)
        return matrix;

    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    for (std::size_t row = 0; row < rows; ++row) {
        for (std::size_t k = 0; k < nnz_per_row; ++k) {
            state ^= state << 7;
            state ^= state >> 9;
            state ^= state << 8;
            matrix.row(row).set(static_cast<std::uint32_t>(state % cols));
        }
    }
    matrix.ensure_all_sorted();
    return matrix;
}

BlockVector build_input(std::size_t rows) {
    BlockVector input(rows);
    std::uint64_t state = 0xd1b54a32d192ed03ULL;
    for (std::size_t i = 0; i < rows; ++i) {
        state ^= state << 7;
        state ^= state >> 9;
        state ^= state << 8;
        input.data[i] = state;
    }
    return input;
}

std::size_t argument_or(int argc, char** argv, std::size_t index,
                        std::size_t fallback) {
    return index < static_cast<std::size_t>(argc) ? std::stoull(argv[index]) : fallback;
}

struct FullClearScratch {
    std::vector<std::vector<std::uint64_t>> locals;

    void ensure(std::size_t thread_count, std::size_t columns) {
        if (locals.size() < thread_count)
            locals.resize(thread_count);
        for (std::size_t thread = 0; thread < thread_count; ++thread) {
            if (locals[thread].size() < columns)
                locals[thread].resize(columns);
            std::fill(locals[thread].begin(),
                      locals[thread].begin() + static_cast<std::ptrdiff_t>(columns), 0);
        }
    }
};

void transpose_full_clear(const CSRMatrix& matrix, const BlockVector& input,
                          BlockVector& output, gnfs::util::ThreadPool& pool,
                          FullClearScratch& scratch) {
    const std::size_t rows = matrix.num_rows();
    const std::size_t columns = matrix.num_cols();
    const std::size_t thread_count = pool.num_threads();
    const std::size_t chunk = rows / thread_count + (rows % thread_count != 0 ? 1 : 0);
    scratch.ensure(thread_count, columns);

    std::vector<std::future<void>> futures;
    futures.reserve(thread_count);
    std::size_t used_threads = 0;
    const bool simd_on = gnfs::linalg::detail::simd::use_simd_runtime();
    for (std::size_t thread = 0; thread < thread_count; ++thread) {
        const std::size_t begin = thread * chunk;
        const std::size_t end = std::min(begin + chunk, rows);
        if (begin >= rows)
            break;
        used_threads = thread + 1;
        futures.push_back(pool.submit([&matrix, &input, &scratch, thread, begin, end, simd_on] {
            auto& local = scratch.locals[thread];
            for (std::size_t row = begin; row < end; ++row) {
                const std::uint64_t value = input.data[row];
                if (value == 0)
                    continue;
                const std::uint32_t* row_begin = matrix.row_begin(row);
                const std::uint32_t* row_end = matrix.row_end(row);
                const std::uint32_t* prefetch_end =
                    row_end - row_begin > kPrefetchAhead ? row_end - kPrefetchAhead : row_begin;
                const std::uint32_t* column = row_begin;
                for (; column < prefetch_end; ++column) {
                    gnfs::util::prefetch_read<0>(&local[*(column + kPrefetchAhead)]);
                    local[*column] ^= value;
                }
                if (simd_on) {
                    gnfs::linalg::detail::simd::scatter_xor_row(column, row_end, value,
                                                                 local.data());
                } else {
                    for (; column < row_end; ++column)
                        local[*column] ^= value;
                }
            }
        }));
    }
    for (auto& future : futures)
        future.get();

    pool.parallel_for_index(0, columns, [&output, &scratch, used_threads](std::size_t column) {
        std::uint64_t value = 0;
        for (std::size_t thread = 0; thread < used_threads; ++thread)
            value ^= scratch.locals[thread][column];
        output.data[column] = value;
    });
}

std::uint64_t checksum(const BlockVector& vector) {
    std::uint64_t value = 0;
    for (const std::uint64_t word : vector.data)
        value ^= word;
    return value;
}

} // namespace

int main(int argc, char** argv) {
    gnfs::util::set_current_thread_qos(gnfs::util::QoSClass::UserInitiated);

    const std::size_t rows = argument_or(argc, argv, 1, 100000);
    const std::size_t cols = argument_or(argc, argv, 2, 500000);
    const std::size_t nnz_per_row = argument_or(argc, argv, 3, 12);
    const std::size_t threads = argument_or(argc, argv, 4, 4);
    const std::size_t reps = argument_or(argc, argv, 5, 20);

    if (threads == 0 || reps == 0) {
        std::cerr << "threads and reps must be positive\n";
        return EXIT_FAILURE;
    }

    SparseMatrix sparse = build_matrix(rows, cols, nnz_per_row);
    CSRMatrix matrix(sparse);
    BlockVector input = build_input(rows);
    BlockVector lazy_output(cols);
    BlockVector full_output(cols);
    gnfs::util::ThreadPool pool(static_cast<std::uint32_t>(threads));
    FullClearScratch full_scratch;

    // Warm both persistent scratch allocations before starting the clock.
    gnfs::linalg::detail::spmv_transpose(matrix, input, lazy_output, pool);
    transpose_full_clear(matrix, input, full_output, pool, full_scratch);

    const auto full_start = std::chrono::steady_clock::now();
    for (std::size_t rep = 0; rep < reps; ++rep)
        transpose_full_clear(matrix, input, full_output, pool, full_scratch);
    const auto full_finish = std::chrono::steady_clock::now();

    const auto lazy_start = std::chrono::steady_clock::now();
    for (std::size_t rep = 0; rep < reps; ++rep)
        gnfs::linalg::detail::spmv_transpose(matrix, input, lazy_output, pool);
    const auto lazy_finish = std::chrono::steady_clock::now();

    const double full_elapsed_ms =
        std::chrono::duration<double, std::milli>(full_finish - full_start).count();
    const double lazy_elapsed_ms =
        std::chrono::duration<double, std::milli>(lazy_finish - lazy_start).count();
    if (checksum(full_output) != checksum(lazy_output)) {
        std::cerr << "full-clear and lazy-clear checksums differ\n";
        return EXIT_FAILURE;
    }
    std::cout << "rows=" << rows << " cols=" << cols << " nnz_per_row=" << nnz_per_row
              << " threads=" << threads << " reps=" << reps
              << " full_clear_ms=" << full_elapsed_ms
              << " lazy_clear_ms=" << lazy_elapsed_ms
              << " full_per_call_ms=" << (full_elapsed_ms / static_cast<double>(reps))
              << " lazy_per_call_ms=" << (lazy_elapsed_ms / static_cast<double>(reps))
              << " speedup=" << (full_elapsed_ms / lazy_elapsed_ms)
              << " checksum=" << checksum(lazy_output) << '\n';
    return EXIT_SUCCESS;
}

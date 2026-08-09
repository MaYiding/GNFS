// End-to-end contract for BlockWiedemann's KrylovSequenceMmap route.
//
// The fixture is large enough to select the block solver. It verifies that
// GNFS_BW_KRYLOV_MMAP changes only the sequence storage medium: both routes
// must return the same valid left-null-space dependencies, while the captured
// trace proves that the mmap-enabled run did not fall back to the scalar path.

#include <gnfs/linalg/block_wiedemann.hpp>
#include <gnfs/linalg/sparse_matrix.hpp>

#include "support/scoped_environment_stderr.hpp"
#include "support/test_check.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace gnfs::linalg;
using gnfs::tests::support::ScopedEnvironmentVariable;
using gnfs::tests::support::ScopedStderrCapture;

namespace {

constexpr std::size_t kBaseRows = 5400;
constexpr std::size_t kColumns = 5000;
constexpr std::size_t kInjectedDependencies = 150;
constexpr std::size_t kTotalRows = kBaseRows + kInjectedDependencies;

using Dependencies = std::vector<std::vector<bool>>;

struct SolveResult {
    Dependencies dependencies;
    std::string stderr_output;
};

bool verify_dependency(const SparseMatrix& matrix, const std::vector<bool>& dependency) {
    const std::size_t rows = matrix.num_rows();
    const std::size_t columns = matrix.num_cols();
    if (dependency.size() != rows) {
        return false;
    }

    std::vector<std::uint8_t> parity(columns, 0);
    for (std::size_t row_index = 0; row_index < rows; ++row_index) {
        if (!dependency[row_index]) {
            continue;
        }
        const SparseRow& row = matrix.row(row_index);
        for (const std::uint32_t column : row.indices()) {
            parity[column] ^= 1U;
        }
    }
    return std::all_of(parity.begin(), parity.end(), [](std::uint8_t value) { return value == 0; });
}

SparseMatrix build_large_matrix(std::size_t rows, std::size_t columns, std::size_t extras,
                                std::uint32_t seed) {
    SparseMatrix matrix(rows + extras, columns);
    std::mt19937 random(seed);
    const auto column_count = static_cast<std::mt19937::result_type>(columns);
    const auto row_count = static_cast<std::mt19937::result_type>(rows);

    for (std::size_t row_index = 0; row_index < rows; ++row_index) {
        const std::size_t nonzeros = 5U + static_cast<std::size_t>(random() % 10U);
        for (std::size_t index = 0; index < nonzeros; ++index) {
            matrix.row(row_index).set(static_cast<std::uint32_t>(random() % column_count));
        }
    }
    for (std::size_t index = 0; index < extras; ++index) {
        const std::size_t first = static_cast<std::size_t>(random() % row_count);
        const std::size_t second = static_cast<std::size_t>(random() % row_count);
        matrix.row(rows + index).xor_with(matrix.row(first));
        matrix.row(rows + index).xor_with(matrix.row(second));
    }
    return matrix;
}

SolveResult run_solver(const SparseMatrix& matrix, std::size_t max_dependencies, bool use_mmap) {
    ScopedEnvironmentVariable mmap("GNFS_BW_KRYLOV_MMAP", use_mmap ? "1" : nullptr);
    ScopedEnvironmentVariable compression("GNFS_BW_KRYLOV_COMPRESS", nullptr);
    ScopedEnvironmentVariable streams("GNFS_BW_KRYLOV_STREAMS", "1");
    ScopedEnvironmentVariable algorithm("GNFS_BW_ALGORITHM", nullptr);

    ScopedStderrCapture capture;
    BlockWiedemann solver;
    auto dependencies = solver.find_dependencies(matrix, max_dependencies);
    std::cerr.flush();
    auto stderr_output = capture.finish();
    return {std::move(dependencies), std::move(stderr_output)};
}

void print_trace(std::string_view label, const std::string& trace) {
    std::cout << "  " << label << " trace:\n" << trace;
    if (!trace.empty() && trace.back() != '\n') {
        std::cout << '\n';
    }
}

void require_block_route(const SolveResult& result, bool use_mmap) {
    const std::string_view trace(result.stderr_output);
    GNFS_TEST_CHECK(trace.find("[BW-block] Block Wiedemann") != std::string_view::npos);
    GNFS_TEST_CHECK(trace.find("falling back to scalar") == std::string_view::npos);
    GNFS_TEST_CHECK(trace.find("[BW-scalar]") == std::string_view::npos);
    GNFS_TEST_CHECK(trace.find("[krylov_mmap]") == std::string_view::npos);

    if (use_mmap) {
        GNFS_TEST_CHECK(trace.find("Phase 1: Krylov (L=190, mmap)") != std::string_view::npos);
        GNFS_TEST_CHECK(trace.find("mmap+zip") == std::string_view::npos);
    } else {
        GNFS_TEST_CHECK(trace.find(", mmap") == std::string_view::npos);
    }
}

void require_dependencies(const SparseMatrix& matrix, const SolveResult& result,
                          std::size_t expected_count) {
    GNFS_TEST_CHECK(result.dependencies.size() == expected_count);
    for (const auto& dependency : result.dependencies) {
        GNFS_TEST_CHECK(dependency.size() == kTotalRows);
        GNFS_TEST_CHECK(
            std::any_of(dependency.begin(), dependency.end(), [](bool value) { return value; }));
        GNFS_TEST_CHECK(verify_dependency(matrix, dependency));
    }
}

void test_block_solve_mmap_off() {
    std::cout << "Testing block_solve with mmap OFF (baseline)...\n";
    SparseMatrix matrix = build_large_matrix(kBaseRows, kColumns, kInjectedDependencies, 11111);
    const auto result = run_solver(matrix, 10, false);

    print_trace("mmap OFF", result.stderr_output);
    require_block_route(result, false);
    require_dependencies(matrix, result, 10);
    std::cout << "  mmap OFF: 10/10 dependencies valid\n";
}

void test_block_solve_mmap_on() {
    std::cout << "Testing block_solve with mmap ON...\n";
    SparseMatrix matrix = build_large_matrix(kBaseRows, kColumns, kInjectedDependencies, 11111);
    const auto result = run_solver(matrix, 10, true);

    print_trace("mmap ON", result.stderr_output);
    require_block_route(result, true);
    require_dependencies(matrix, result, 10);
    std::cout << "  mmap ON: 10/10 dependencies valid\n";
}

void test_block_solve_mmap_correctness_invariant() {
    std::cout << "Testing mmap ON vs OFF bit-for-bit invariant...\n";
    SparseMatrix matrix = build_large_matrix(kBaseRows, kColumns, kInjectedDependencies, 22222);
    const auto result_off = run_solver(matrix, 5, false);
    const auto result_on = run_solver(matrix, 5, true);

    print_trace("invariant OFF", result_off.stderr_output);
    print_trace("invariant ON", result_on.stderr_output);
    require_block_route(result_off, false);
    require_block_route(result_on, true);
    require_dependencies(matrix, result_off, 5);
    require_dependencies(matrix, result_on, 5);
    GNFS_TEST_CHECK(result_off.dependencies == result_on.dependencies);
    std::cout << "  ON/OFF: 5/5 identical valid dependencies\n";
}

} // namespace

int main() {
    try {
        std::cout << "===== BW Krylov mmap Integration Tests =====\n";

        test_block_solve_mmap_off();
        test_block_solve_mmap_on();
        test_block_solve_mmap_correctness_invariant();

        std::cout << "===== All BW Krylov mmap integration tests PASSED =====\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "BW Krylov mmap integration tests FAILED: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "BW Krylov mmap integration tests FAILED: unknown exception\n";
        return 1;
    }
}

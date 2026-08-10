// End-to-end contract for BlockWiedemann's KrylovSequenceMmap route.
//
// The fixture is large enough to select the block solver. It verifies that
// GNFS_BW_KRYLOV_MMAP and GNFS_BW_KRYLOV_COMPRESS change only the sequence
// storage medium: memory, mmap, and mmap+zip must return the same valid
// left-null-space dependencies, while the captured trace proves the route.

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

enum class StorageMode : std::uint8_t { memory, mmap, mmap_zip };

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

SolveResult run_solver(const SparseMatrix& matrix, std::size_t max_dependencies, StorageMode mode) {
    const bool use_mmap = mode != StorageMode::memory;
    const bool use_compression = mode == StorageMode::mmap_zip;
    ScopedEnvironmentVariable mmap("GNFS_BW_KRYLOV_MMAP", use_mmap ? "1" : nullptr);
    ScopedEnvironmentVariable compression("GNFS_BW_KRYLOV_COMPRESS",
                                          use_compression ? "1" : nullptr);
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

void require_block_route(const SolveResult& result, StorageMode mode) {
    const std::string_view trace(result.stderr_output);
    GNFS_TEST_CHECK(trace.find("[BW-block] Block Wiedemann") != std::string_view::npos);
    GNFS_TEST_CHECK(trace.find("falling back to scalar") == std::string_view::npos);
    GNFS_TEST_CHECK(trace.find("[BW-scalar]") == std::string_view::npos);
    GNFS_TEST_CHECK(trace.find("[krylov_mmap]") == std::string_view::npos);

    if (mode == StorageMode::mmap) {
        GNFS_TEST_CHECK(trace.find("Phase 1: Krylov (L=190, mmap)") != std::string_view::npos);
        GNFS_TEST_CHECK(trace.find("mmap+zip") == std::string_view::npos);
        GNFS_TEST_CHECK(trace.find("[bw_krylov_compress]") == std::string_view::npos);
    } else if (mode == StorageMode::mmap_zip) {
        GNFS_TEST_CHECK(trace.find("Phase 1: Krylov (L=190, mmap+zip)") != std::string_view::npos);
        GNFS_TEST_CHECK(trace.find("[bw_krylov_compress] orig=") != std::string_view::npos);
        GNFS_TEST_CHECK(trace.find(" compressed=") != std::string_view::npos);
        GNFS_TEST_CHECK(trace.find(" ratio=") != std::string_view::npos);
        GNFS_TEST_CHECK(trace.find("[bw_krylov_compress] copied_entries=190 cleanup=removed") !=
                        std::string_view::npos);
        GNFS_TEST_CHECK(trace.find("falling back to uncompressed mmap") == std::string_view::npos);
        GNFS_TEST_CHECK(trace.find("WARN:") == std::string_view::npos);
    } else {
        GNFS_TEST_CHECK(trace.find(", mmap") == std::string_view::npos);
        GNFS_TEST_CHECK(trace.find("[bw_krylov_compress]") == std::string_view::npos);
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

void test_storage_modes_are_bit_for_bit_identical() {
    std::cout << "Testing memory, mmap, and mmap+zip bit-for-bit invariant...\n";
    SparseMatrix matrix = build_large_matrix(kBaseRows, kColumns, kInjectedDependencies, 22222);
    const auto memory = run_solver(matrix, 5, StorageMode::memory);
    const auto mmap = run_solver(matrix, 5, StorageMode::mmap);
    const auto mmap_zip = run_solver(matrix, 5, StorageMode::mmap_zip);

    print_trace("memory", memory.stderr_output);
    print_trace("mmap", mmap.stderr_output);
    print_trace("mmap+zip", mmap_zip.stderr_output);
    require_block_route(memory, StorageMode::memory);
    require_block_route(mmap, StorageMode::mmap);
    require_block_route(mmap_zip, StorageMode::mmap_zip);
    require_dependencies(matrix, memory, 5);
    require_dependencies(matrix, mmap, 5);
    require_dependencies(matrix, mmap_zip, 5);
    GNFS_TEST_CHECK(memory.dependencies == mmap.dependencies);
    GNFS_TEST_CHECK(memory.dependencies == mmap_zip.dependencies);
    std::cout << "  memory/mmap/mmap+zip: 5/5 identical valid dependencies\n";
}

} // namespace

int main() {
    try {
        std::cout << "===== BW Krylov mmap Integration Tests =====\n";

        test_storage_modes_are_bit_for_bit_identical();

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

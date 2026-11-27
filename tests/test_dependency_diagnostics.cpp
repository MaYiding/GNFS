// test_dependency_diagnostics.cpp — Traces for failures in the matrix phase
//
// Covers:
//   1. detail::retain_verified_dependencies drops a dependency that is not a
//      null vector of the final matrix, keeps the valid dependencies in input
//      order, and reports every rejected dependency exactly once.
//   2. A batch without a rejected dependency is returned unchanged and
//      produces no report.

#include <gnfs/api/pipeline.hpp>
#include <gnfs/api/progress.hpp>
#include <gnfs/linalg/sparse_matrix.hpp>

#include "support/test_check.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using gnfs::api::LogLevel;
using gnfs::api::Phase;
using gnfs::api::detail::DependencyRejectionSink;
using gnfs::api::detail::retain_verified_dependencies;
using gnfs::linalg::SparseMatrix;

// One report as a Pipeline log callback receives it.
struct LogRecord {
    LogLevel level;
    Phase phase;
    std::string message;
};

// Collect every report.
DependencyRejectionSink collect_into(std::vector<LogRecord>& records) {
    return [&records](LogLevel level, Phase phase, const std::string& message) {
        records.push_back({level, phase, message});
    };
}

// Rows over GF(2): r0 = {0, 1}, r1 = {1, 2}, r2 = {0, 2}.
// r0 ^ r1 ^ r2 = 0, so selecting all three rows is a dependency, while r0 ^ r1
// leaves columns 0 and 2 set.
SparseMatrix triangle_matrix() {
    SparseMatrix matrix(3, 3);
    matrix.set(0, 0);
    matrix.set(0, 1);
    matrix.set(1, 1);
    matrix.set(1, 2);
    matrix.set(2, 0);
    matrix.set(2, 2);
    return matrix;
}

const std::vector<bool> all_rows{true, true, true};
const std::vector<bool> first_two_rows{true, true, false};

void test_rejected_dependency_is_dropped_and_reported_once() {
    std::cout << "Testing rejected dependency is dropped and reported once...\n";

    std::vector<std::vector<bool>> dependencies{all_rows, first_two_rows, all_rows};
    std::vector<LogRecord> records;

    const auto kept =
        retain_verified_dependencies(dependencies, triangle_matrix(), collect_into(records));

    GNFS_TEST_CHECK(kept == (std::vector<std::vector<bool>>{all_rows, all_rows}));
    GNFS_TEST_CHECK(records.size() == 1);
    GNFS_TEST_CHECK(records[0].level == LogLevel::Warn);
    GNFS_TEST_CHECK(records[0].phase == Phase::LinearAlgebra);
    GNFS_TEST_CHECK(records[0].message ==
                    "Dropped dependency 2/3: not a null vector of the 3x3 matrix");
    std::cout << "  PASSED\n";
}

void test_verified_batch_is_silent() {
    std::cout << "Testing verified batch is silent...\n";

    const std::vector<std::vector<bool>> dependencies{all_rows, all_rows};
    std::vector<LogRecord> records;

    const auto kept =
        retain_verified_dependencies(dependencies, triangle_matrix(), collect_into(records));

    GNFS_TEST_CHECK(kept == dependencies);
    GNFS_TEST_CHECK(records.empty());

    const std::vector<std::vector<bool>> empty_batch;
    const auto none =
        retain_verified_dependencies(empty_batch, triangle_matrix(), collect_into(records));

    GNFS_TEST_CHECK(none.empty());
    GNFS_TEST_CHECK(records.empty());
    std::cout << "  PASSED\n";
}

void test_length_mismatch_is_dropped_and_reported() {
    std::cout << "Testing length mismatch is dropped and reported...\n";

    const std::vector<bool> two_entries{true, true};
    std::vector<std::vector<bool>> dependencies{two_entries, all_rows};
    std::vector<LogRecord> records;

    const auto kept =
        retain_verified_dependencies(dependencies, triangle_matrix(), collect_into(records));

    GNFS_TEST_CHECK(kept == (std::vector<std::vector<bool>>{all_rows}));
    GNFS_TEST_CHECK(records.size() == 1);
    GNFS_TEST_CHECK(records[0].message ==
                    "Dropped dependency 1/2: not a null vector of the 3x3 matrix");
    std::cout << "  PASSED\n";
}

void test_out_of_range_column_is_dropped_and_reported() {
    std::cout << "Testing out-of-range column is dropped and reported...\n";

    // Row 0 indexes column 1 of a one-column matrix. Rows 1 and 2 cancel.
    SparseMatrix matrix(3, 1);
    matrix.row(0).append_unchecked(1);
    matrix.set(1, 0);
    matrix.set(2, 0);
    const std::vector<bool> malformed_row{true, false, false};
    const std::vector<bool> cancelling_rows{false, true, true};
    std::vector<std::vector<bool>> dependencies{malformed_row, cancelling_rows};
    std::vector<LogRecord> records;

    const auto kept = retain_verified_dependencies(dependencies, matrix, collect_into(records));

    GNFS_TEST_CHECK(kept == (std::vector<std::vector<bool>>{cancelling_rows}));
    GNFS_TEST_CHECK(records.size() == 1);
    GNFS_TEST_CHECK(records[0].level == LogLevel::Warn);
    GNFS_TEST_CHECK(records[0].phase == Phase::LinearAlgebra);
    GNFS_TEST_CHECK(records[0].message ==
                    "Dropped dependency 1/2: not a null vector of the 3x1 matrix");
    std::cout << "  PASSED\n";
}

} // namespace

int main() {
    std::cout << "=== Dependency diagnostics tests ===\n";

    test_rejected_dependency_is_dropped_and_reported_once();
    test_verified_batch_is_silent();
    test_length_mismatch_is_dropped_and_reported();
    test_out_of_range_column_is_dropped_and_reported();

    std::cout << "\n=== All dependency diagnostics tests PASSED ===\n";
    return 0;
}

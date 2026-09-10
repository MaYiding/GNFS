// test_siqs_shadow_sparse_backend.cpp - sparse shadow backend boundary contracts

#include <gnfs/core/integer.hpp>
#include <gnfs/linalg/block_wiedemann.hpp>
#include <gnfs/linalg/sparse_matrix.hpp>
#include <gnfs/siqs/shadow_matrix.hpp>
#include <gnfs/siqs/shadow_proof_runner.hpp>
#include <gnfs/util/process.hpp>
#include <gnfs/util/temp_path.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using gnfs::core::Integer;
using gnfs::linalg::BlockWiedemann;
using gnfs::linalg::CSRMatrix;
using gnfs::siqs::SIQSFactorPower;
using gnfs::siqs::SIQSPostMergeRow;
using gnfs::siqs::SIQSShadowMatrixBackend;
using gnfs::siqs::SIQSShadowMatrixOptions;
using gnfs::siqs::SIQSShadowMatrixStatus;
using gnfs::siqs::SIQSShadowProofFallbackReason;
using gnfs::siqs::SIQSShadowRow;
using gnfs::siqs::SIQSShadowRowOrigin;
using gnfs::siqs::solve_siqs_shadow_matrix;
using std::size_t;

int checks_passed = 0;
int checks_failed = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (condition) {                                                                           \
            ++checks_passed;                                                                       \
        } else {                                                                                   \
            ++checks_failed;                                                                       \
            std::cerr << "FAIL: " #condition << " at " << __FILE__ << ':' << __LINE__ << '\n';     \
        }                                                                                          \
    } while (false)

/// Environment changes are process-global. Keep them scoped and restore the
/// exact previous value so this contract test remains composable with a test
/// runner that reuses the process.
class ScopedEnvironmentVariable final {
public:
    ScopedEnvironmentVariable(const char* name, const char* value) : name_(name) {
        const char* previous = std::getenv(name_.c_str());
        if (previous != nullptr) {
            previous_ = std::string(previous);
        }
        set(value);
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

    ~ScopedEnvironmentVariable() {
        if (previous_) {
            set(previous_->c_str());
        } else {
            set(nullptr);
        }
    }

private:
    void set(const char* value) const {
#ifdef _WIN32
        const int result = _putenv_s(name_.c_str(), value == nullptr ? "" : value);
#else
        const int result =
            value == nullptr ? ::unsetenv(name_.c_str()) : ::setenv(name_.c_str(), value, 1);
#endif
        if (result != 0) {
            std::terminate();
        }
    }

    std::string name_;
    std::optional<std::string> previous_;
};

[[nodiscard]] std::vector<std::string> capture_bw_scratch_names(const std::string& extension) {
    std::vector<std::string> names;
    const std::string prefix = "gnfs_bw_krylov_" + std::to_string(gnfs::util::process_id()) + "_";
    std::error_code error;
    std::filesystem::directory_iterator entries(gnfs::util::temp_directory_path(), error);
    if (error) {
        return names;
    }
    for (const auto& entry : entries) {
        const std::string leaf = entry.path().filename().string();
        if (leaf.starts_with(prefix) && leaf.ends_with(extension)) {
            names.push_back(leaf);
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

[[nodiscard]] CSRMatrix make_seeded_contract_matrix() {
    // The last four rows are zero, so the result has a large, deterministic
    // left nullspace while still exercising the full block-BW view path.
    return CSRMatrix(8, 4, std::vector<size_t>{0, 1, 2, 3, 5, 5, 5, 5, 5},
                     std::vector<uint32_t>{0, 1, 0, 0, 1});
}

void test_owning_csr_round_trip() {
    std::vector<size_t> offsets{0, 1, 3, 3, 4};
    std::vector<uint32_t> columns{0, 1, 2, 3};
    CSRMatrix matrix(4, 5, std::move(offsets), std::move(columns));

    CHECK(matrix.num_rows() == 4);
    CHECK(matrix.num_cols() == 5);
    CHECK(matrix.nnz() == 4);
    CHECK(matrix.row_offsets() == std::vector<size_t>({0, 1, 3, 3, 4}));
    CHECK(matrix.col_indices() == std::vector<uint32_t>({0, 1, 2, 3}));
    CHECK(matrix.row_nnz(0) == 1);
    CHECK(matrix.row_nnz(1) == 2);
    CHECK(matrix.row_nnz(2) == 0);
    CHECK(matrix.row_nnz(3) == 1);
    CHECK(*matrix.row_begin(1) == 1 && *(matrix.row_end(1) - 1) == 2);
    CHECK(matrix.row_offsets_u32()[0] == 0 && matrix.row_offsets_u32()[4] == 4);
}

template <class Builder> void check_invalid_csr(Builder&& builder) {
    bool threw = false;
    try {
        builder();
    } catch (const std::invalid_argument&) {
        threw = true;
    } catch (...) {
    }
    CHECK(threw);
}

void test_owning_csr_validation() {
    check_invalid_csr([] {
        CSRMatrix matrix(2, 4, std::vector<size_t>{0, 1}, std::vector<uint32_t>{0});
        (void)matrix;
    });
    check_invalid_csr([] {
        CSRMatrix matrix(1, 4, std::vector<size_t>{0, 2}, std::vector<uint32_t>{0});
        (void)matrix;
    });
    check_invalid_csr([] {
        CSRMatrix matrix(1, 4, std::vector<size_t>{1, 1}, std::vector<uint32_t>{});
        (void)matrix;
    });
    check_invalid_csr([] {
        CSRMatrix matrix(1, 4, std::vector<size_t>{0, 2}, std::vector<uint32_t>{2, 1});
        (void)matrix;
    });
    check_invalid_csr([] {
        CSRMatrix matrix(1, 4, std::vector<size_t>{0, 1}, std::vector<uint32_t>{4});
        (void)matrix;
    });
    check_invalid_csr([] {
        // Every intermediate row offset must stay within the packed payload;
        // checking only the final offset would let row 0 index past the data.
        CSRMatrix matrix(2, 4, std::vector<size_t>{0, 2, 1}, std::vector<uint32_t>{0});
        (void)matrix;
    });
    check_invalid_csr([] {
        CSRMatrix matrix(0, std::numeric_limits<uint32_t>::max() + size_t{1},
                         std::vector<size_t>{0}, std::vector<uint32_t>{});
        (void)matrix;
    });
}

[[nodiscard]] bool verifies_left_nullspace(const CSRMatrix& matrix,
                                           const std::vector<bool>& dependency) {
    if (dependency.size() != matrix.num_rows()) {
        return false;
    }
    std::vector<uint8_t> parity(matrix.num_cols(), uint8_t{0});
    for (size_t row = 0; row < matrix.num_rows(); ++row) {
        if (!dependency[row]) {
            continue;
        }
        for (const uint32_t* cursor = matrix.row_begin(row); cursor != matrix.row_end(row);
             ++cursor) {
            parity[*cursor] ^= uint8_t{1};
        }
    }
    for (const uint8_t bit : parity) {
        if (bit != 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool
dependency_vectors_are_independent(const std::vector<std::vector<bool>>& dependencies) {
    if (dependencies.empty()) {
        return true;
    }
    const size_t width = dependencies.front().size();
    std::vector<std::vector<uint64_t>> pivots(width);
    for (const auto& dependency : dependencies) {
        if (dependency.size() != width) {
            return false;
        }
        std::vector<uint64_t> reduced(
            width / size_t{64} + (width % size_t{64} != 0 ? size_t{1} : size_t{0}), uint64_t{0});
        for (size_t bit = 0; bit < width; ++bit) {
            if (dependency[bit]) {
                reduced[bit / size_t{64}] |= uint64_t{1} << (bit % size_t{64});
            }
        }
        bool admitted = false;
        for (size_t bit = 0; bit < width; ++bit) {
            const size_t word = bit / size_t{64};
            const uint64_t mask = uint64_t{1} << (bit % size_t{64});
            if ((reduced[word] & mask) == 0) {
                continue;
            }
            if (pivots[bit].empty()) {
                pivots[bit] = std::move(reduced);
                admitted = true;
                break;
            }
            for (size_t index = 0; index < reduced.size(); ++index) {
                reduced[index] ^= pivots[bit][index];
            }
        }
        if (!admitted) {
            return false;
        }
    }
    return true;
}

void test_seeded_view_contract() {
    // Rows 0, 1, 2 are independent; row 3 is row 0 XOR row 1. Empty rows
    // make the nullspace non-trivial without requiring a large allocation.
    const CSRMatrix matrix = make_seeded_contract_matrix();
    BlockWiedemann solver;

    CHECK(solver.find_dependencies_view_seeded(matrix, 0, 17, 1).empty());
    bool invalid_attempts = false;
    try {
        (void)solver.find_dependencies_view_seeded(matrix, 1, 17, 0);
    } catch (const std::invalid_argument&) {
        invalid_attempts = true;
    } catch (...) {
    }
    CHECK(invalid_attempts);

    try {
        const auto first = solver.find_dependencies_view_seeded(matrix, 8, 17, 1);
        const auto second = solver.find_dependencies_view_seeded(matrix, 8, 17, 1);
        CHECK(first == second);
        CHECK(dependency_vectors_are_independent(first));
        // The fixture has rank two over four columns and therefore a six-
        // dimensional left null space.  A seeded result must not report more
        // than six independent vectors merely because it found more distinct
        // (but dependent) candidates.
        CHECK(first.size() <= 6);
        for (const auto& dependency : first) {
            CHECK(verifies_left_nullspace(matrix, dependency));
        }
        const auto retried = solver.find_dependencies_view_seeded(matrix, 8, 17, 2);
        CHECK(dependency_vectors_are_independent(retried));
        CHECK(retried.size() <= 6);
        for (const auto& dependency : retried) {
            CHECK(verifies_left_nullspace(matrix, dependency));
        }
    } catch (...) {
        CHECK(false);
    }
}

void test_seeded_policy_is_environment_independent() {
    const CSRMatrix matrix = make_seeded_contract_matrix();
    BlockWiedemann solver;
    const BlockWiedemann::SeededPolicy cpu_policy{/*pool_threads=*/1,
                                                  /*use_krylov_mmap=*/false,
                                                  /*use_krylov_compression=*/false,
                                                  /*allow_metal=*/false};

    const auto clean = solver.find_dependencies_view_seeded(matrix, 8, 91, 1, cpu_policy);
    {
        // These values would select a different storage/stream/accelerator
        // route through the legacy environment-driven entry point. The fully
        // explicit boundary must ignore all four of them.
        ScopedEnvironmentVariable streams("GNFS_BW_KRYLOV_STREAMS", "16");
        ScopedEnvironmentVariable mmap("GNFS_BW_KRYLOV_MMAP", "1");
        ScopedEnvironmentVariable compression("GNFS_BW_KRYLOV_COMPRESS", "1");
        ScopedEnvironmentVariable metal("GNFS_METAL_SPMV", "1");
        const auto isolated = solver.find_dependencies_view_seeded(matrix, 8, 91, 1, cpu_policy);
        CHECK(isolated == clean);
    }

    const BlockWiedemann::SeededPolicy mmap_policy{/*pool_threads=*/1,
                                                   /*use_krylov_mmap=*/true,
                                                   /*use_krylov_compression=*/false,
                                                   /*allow_metal=*/false};
    const BlockWiedemann::SeededPolicy compressed_policy{/*pool_threads=*/1,
                                                         /*use_krylov_mmap=*/true,
                                                         /*use_krylov_compression=*/true,
                                                         /*allow_metal=*/false};
    const auto before_mmap = capture_bw_scratch_names(".kry");
    const auto mmap_result = solver.find_dependencies_view_seeded(matrix, 8, 91, 1, mmap_policy);
    const auto after_mmap = capture_bw_scratch_names(".kry");
    CHECK(mmap_result == clean);
    CHECK(after_mmap == before_mmap);

    const auto before_compressed = capture_bw_scratch_names(".kryz");
    const auto compressed_result =
        solver.find_dependencies_view_seeded(matrix, 8, 91, 1, compressed_policy);
    const auto after_compressed = capture_bw_scratch_names(".kryz");
    CHECK(compressed_result == clean);
    CHECK(after_compressed == before_compressed);

    bool invalid_compression = false;
    try {
        const BlockWiedemann::SeededPolicy invalid{/*pool_threads=*/1,
                                                   /*use_krylov_mmap=*/false,
                                                   /*use_krylov_compression=*/true,
                                                   /*allow_metal=*/false};
        (void)solver.find_dependencies_view_seeded(matrix, 1, 91, 1, invalid);
    } catch (const std::invalid_argument&) {
        invalid_compression = true;
    } catch (...) {
    }
    CHECK(invalid_compression);

    bool invalid_workers = false;
    try {
        const BlockWiedemann::SeededPolicy invalid{/*pool_threads=*/
                                                   BlockWiedemann::kMaxSeededPoolThreads + 1,
                                                   /*use_krylov_mmap=*/false,
                                                   /*use_krylov_compression=*/false,
                                                   /*allow_metal=*/false};
        (void)solver.find_dependencies_view_seeded(matrix, 1, 91, 1, invalid);
    } catch (const std::invalid_argument&) {
        invalid_workers = true;
    } catch (...) {
    }
    CHECK(invalid_workers);
}

void test_seeded_scratch_isolation_under_concurrency() {
    const CSRMatrix matrix = make_seeded_contract_matrix();
    const BlockWiedemann::SeededPolicy mmap_policy{/*pool_threads=*/1,
                                                   /*use_krylov_mmap=*/true,
                                                   /*use_krylov_compression=*/false,
                                                   /*allow_metal=*/false};
    const BlockWiedemann::SeededPolicy compressed_policy{/*pool_threads=*/1,
                                                         /*use_krylov_mmap=*/true,
                                                         /*use_krylov_compression=*/true,
                                                         /*allow_metal=*/false};

    const auto run_pair = [&](const BlockWiedemann::SeededPolicy& policy,
                              const std::string& extension) {
        const auto before = capture_bw_scratch_names(extension);
        const auto run = [&matrix, &policy] {
            BlockWiedemann solver;
            return solver.find_dependencies_view_seeded(matrix, 8, 123, 1, policy);
        };
        auto first = std::async(std::launch::async, run);
        auto second = std::async(std::launch::async, run);
        std::vector<std::vector<bool>> first_result;
        std::vector<std::vector<bool>> second_result;
        bool threw = false;
        try {
            first_result = first.get();
            second_result = second.get();
        } catch (...) {
            threw = true;
        }
        const auto after = capture_bw_scratch_names(extension);
        CHECK(!threw);
        CHECK(!first_result.empty());
        CHECK(first_result == second_result);
        CHECK(after == before);
    };

    run_pair(mmap_policy, ".kry");
    run_pair(compressed_policy, ".kryz");
}

void test_seeded_view_zero_column_boundary() {
    // A CSR view may have rows but no equations.  The public BW boundary is
    // intentionally a no-op for this degenerate shape; SIQS itself handles
    // such rows before invoking BW.
    CSRMatrix matrix(3, 0, std::vector<size_t>{0, 0, 0, 0}, {});
    BlockWiedemann solver;
    CHECK(solver.find_dependencies_view_seeded(matrix, 3, 42, 1).empty());
}

void test_seeded_empty_view_validation_order() {
    CSRMatrix empty(0, 0, std::vector<size_t>{0}, std::vector<uint32_t>{});
    BlockWiedemann solver;
    const BlockWiedemann::SeededPolicy invalid_policy{/*pool_threads=*/1,
                                                      /*use_krylov_mmap=*/false,
                                                      /*use_krylov_compression=*/true,
                                                      /*allow_metal=*/false};

    // max_deps==0 is a documented strict no-op, even when the remaining
    // policy fields are malformed.
    CHECK(solver.find_dependencies_view_seeded(empty, 0, 42, 0, invalid_policy).empty());

    bool invalid_retry = false;
    try {
        (void)solver.find_dependencies_view_seeded(empty, 1, 42, 0);
    } catch (const std::invalid_argument&) {
        invalid_retry = true;
    } catch (...) {
    }
    CHECK(invalid_retry);
    CHECK(solver.find_dependencies_view_seeded(empty, 1, 42, 1).empty());

    bool invalid_policy_seen = false;
    try {
        (void)solver.find_dependencies_view_seeded(empty, 1, 42, 1, invalid_policy);
    } catch (const std::invalid_argument&) {
        invalid_policy_seen = true;
    } catch (...) {
    }
    CHECK(invalid_policy_seen);
    CHECK(empty.row_offsets_u32()[0] == 0);
}

void test_matrix_status_mapping_contract() {
    using gnfs::siqs::shadow_proof_detail::matrix_failure_is_internal;
    using gnfs::siqs::shadow_proof_detail::matrix_fallback;
    CHECK(matrix_fallback(SIQSShadowMatrixStatus::resource_limit) ==
          SIQSShadowProofFallbackReason::matrix_resource_limit);
    CHECK(matrix_fallback(SIQSShadowMatrixStatus::unsupported_backend) ==
          SIQSShadowProofFallbackReason::matrix_backend_unavailable);
    CHECK(!matrix_fallback(SIQSShadowMatrixStatus::valid).has_value());
    CHECK(!matrix_fallback(SIQSShadowMatrixStatus::worker_failure).has_value());
    CHECK(!matrix_fallback(SIQSShadowMatrixStatus::no_dependencies).has_value());
    CHECK(!matrix_fallback(SIQSShadowMatrixStatus::solver_failure).has_value());
    CHECK(!matrix_failure_is_internal(SIQSShadowMatrixStatus::no_dependencies));
    CHECK(!matrix_failure_is_internal(SIQSShadowMatrixStatus::solver_failure));
}

[[nodiscard]] SIQSShadowRow make_zero_shadow_row(size_t source_id) {
    return SIQSShadowRow{SIQSShadowRowOrigin::raw_full,
                         SIQSPostMergeRow{Integer(1),
                                          false,
                                          {},
                                          {},
                                          std::vector<gnfs::siqs::SIQSSourceId>{
                                              gnfs::siqs::SIQSSourceId{source_id}}}};
}

[[nodiscard]] std::vector<SIQSShadowRow> make_rank_deficient_rows() {
    // Four parity columns with one deliberately dependent row.  Modulus 2
    // makes every odd factor-base contribution equal to one, so the row
    // identity remains exact while the sparse solver sees the intended GF(2)
    // incidence pattern.
    const std::vector<uint8_t> patterns{0b0011, 0b0101, 0b0110, 0b0110, 0b0000};
    std::vector<SIQSShadowRow> rows;
    rows.reserve(patterns.size());
    for (size_t row_index = 0; row_index < patterns.size(); ++row_index) {
        const uint8_t pattern = patterns[row_index];
        std::vector<SIQSFactorPower> powers;
        for (uint32_t bit = 0; bit < 4; ++bit) {
            if ((pattern & (uint8_t{1} << bit)) != 0) {
                powers.push_back(SIQSFactorPower{bit + 1, 1});
            }
        }
        rows.push_back(SIQSShadowRow{SIQSShadowRowOrigin::raw_full,
                                     SIQSPostMergeRow{Integer(1),
                                                      false,
                                                      std::move(powers),
                                                      {},
                                                      std::vector<gnfs::siqs::SIQSSourceId>{
                                                          gnfs::siqs::SIQSSourceId{row_index}}}});
    }
    return rows;
}

[[nodiscard]] bool dependency_is_zero(const std::vector<SIQSShadowRow>& rows,
                                      const std::vector<size_t>& dependency, size_t column_count) {
    std::vector<uint8_t> parity(column_count, uint8_t{0});
    for (const size_t row_index : dependency) {
        if (row_index >= rows.size()) {
            return false;
        }
        for (const SIQSFactorPower& power : rows[row_index].row.factor_powers) {
            if ((power.exponent & 1U) != 0) {
                if (power.factor_base_index == 0 || power.factor_base_index >= column_count) {
                    return false;
                }
                parity[power.factor_base_index - 1] ^= uint8_t{1};
            }
        }
    }
    return std::all_of(parity.begin(), parity.end(), [](uint8_t bit) { return bit == 0; });
}

void test_sparse_dispatch_and_automatic_promotion() {
    const std::vector<uint32_t> factor_base{0, 2, 3};
    std::vector<SIQSShadowRow> rows;
    for (size_t source_id = 0; source_id < 8; ++source_id) {
        rows.push_back(make_zero_shadow_row(source_id));
    }
    const auto row_span = std::span<const SIQSShadowRow>(rows.data(), rows.size());
    const auto fb_span = std::span<const uint32_t>(factor_base.data(), factor_base.size());

    SIQSShadowMatrixOptions sparse_options;
    sparse_options.backend = SIQSShadowMatrixBackend::sparse_only;
    sparse_options.max_dependencies = rows.size();
    sparse_options.max_sparse_csr_bytes = 4096;
    sparse_options.max_sparse_nonzero_count = 0;
    sparse_options.sparse_retry_count = 1;
    const auto sparse = solve_siqs_shadow_matrix(row_span, fb_span, Integer(2), sparse_options);
    CHECK(sparse.status() == SIQSShadowMatrixStatus::valid);
    CHECK(sparse.solution().has_value());
    if (sparse.solution()) {
        CHECK(sparse.solution()->dependencies.size() == rows.size());
        for (size_t row = 0; row < rows.size(); ++row) {
            CHECK(sparse.solution()->dependencies[row] == std::vector<size_t>{row});
        }
    }

    SIQSShadowMatrixOptions automatic = sparse_options;
    automatic.backend = SIQSShadowMatrixBackend::automatic;
    automatic.max_dense_matrix_bytes = 0;
    automatic.max_dense_variable_count = 0;
    const auto promoted = solve_siqs_shadow_matrix(row_span, fb_span, Integer(2), automatic);
    CHECK(promoted.status() == SIQSShadowMatrixStatus::valid);
    CHECK(promoted.solution() == sparse.solution());

    SIQSShadowMatrixOptions malformed = sparse_options;
    malformed.backend = static_cast<SIQSShadowMatrixBackend>(255);
    CHECK(solve_siqs_shadow_matrix(row_span, fb_span, Integer(2), malformed).status() ==
          SIQSShadowMatrixStatus::unsupported_backend);
}

void test_dense_route_options_contract() {
    const std::vector<uint32_t> factor_base{0, 2, 3};
    std::vector<SIQSShadowRow> rows;
    for (size_t source_id = 0; source_id < 8; ++source_id) {
        rows.push_back(make_zero_shadow_row(source_id));
    }
    const auto row_span = std::span<const SIQSShadowRow>(rows.data(), rows.size());
    const auto fb_span = std::span<const uint32_t>(factor_base.data(), factor_base.size());

    SIQSShadowMatrixOptions dense_options;
    dense_options.backend = SIQSShadowMatrixBackend::dense_only;
    dense_options.max_dependencies = rows.size();
    const auto dense = solve_siqs_shadow_matrix(row_span, fb_span, Integer(2), dense_options);
    CHECK(dense.status() == SIQSShadowMatrixStatus::valid);
    CHECK(dense.solution().has_value());
    if (dense.solution()) {
        CHECK(dense.solution()->dependencies.size() == rows.size());
    }

    SIQSShadowMatrixOptions automatic_options = dense_options;
    automatic_options.backend = SIQSShadowMatrixBackend::automatic;
    const auto automatic =
        solve_siqs_shadow_matrix(row_span, fb_span, Integer(2), automatic_options);
    CHECK(automatic.status() == SIQSShadowMatrixStatus::valid);
    CHECK(automatic.solution() == dense.solution());

    SIQSShadowMatrixOptions dense_invalid = dense_options;
    dense_invalid.sparse_retry_count = 0;
    CHECK(solve_siqs_shadow_matrix(row_span, fb_span, Integer(2), dense_invalid).status() ==
          SIQSShadowMatrixStatus::invalid_options);

    SIQSShadowMatrixOptions automatic_invalid = automatic_options;
    automatic_invalid.sparse_use_krylov_compression = true;
    CHECK(solve_siqs_shadow_matrix(row_span, fb_span, Integer(2), automatic_invalid).status() ==
          SIQSShadowMatrixStatus::invalid_options);
}

void test_sparse_solver_returns_verified_basis() {
    const auto rows = make_rank_deficient_rows();
    const std::vector<uint32_t> factor_base{0, 3, 5, 7, 11};
    SIQSShadowMatrixOptions options;
    options.backend = SIQSShadowMatrixBackend::sparse_only;
    options.max_dependencies = 5;
    options.sparse_retry_count = 3;
    const auto result = solve_siqs_shadow_matrix(
        std::span<const SIQSShadowRow>(rows.data(), rows.size()),
        std::span<const uint32_t>(factor_base.data(), factor_base.size()), Integer(2), options);
    CHECK(result.status() == SIQSShadowMatrixStatus::valid);
    CHECK(result.solution().has_value());
    if (!result.solution()) {
        return;
    }
    CHECK(result.solution()->dependencies.size() == 3);
    for (const auto& dependency : result.solution()->dependencies) {
        CHECK(!dependency.empty());
        CHECK(std::is_sorted(dependency.begin(), dependency.end()));
    }
    CHECK(std::any_of(result.solution()->dependencies.begin(),
                      result.solution()->dependencies.end(),
                      [](const auto& dependency) { return dependency == std::vector<size_t>{4}; }));
    CHECK(std::all_of(result.solution()->dependencies.begin(),
                      result.solution()->dependencies.end(), [&](const auto& dependency) {
                          return dependency_is_zero(rows, dependency, factor_base.size());
                      }));
}

[[nodiscard]] SIQSShadowRow make_single_factor_shadow_row(size_t source_id, uint32_t factor_index) {
    return SIQSShadowRow{
        SIQSShadowRowOrigin::raw_full,
        SIQSPostMergeRow{
            Integer(1),
            false,
            std::vector<SIQSFactorPower>{SIQSFactorPower{factor_index, 1}},
            {},
            std::vector<gnfs::siqs::SIQSSourceId>{gnfs::siqs::SIQSSourceId{source_id}}}};
}

void test_sparse_provider_budget_includes_admitted_zero_rows() {
    // Keep the shape outside the deterministic exact pass.  The zero row is
    // admitted before the seeded solver, but the provider still needs room to
    // return the independent non-singleton candidate that completes the
    // requested two-vector basis.
    const size_t row_count = gnfs::siqs::SIQS_SHADOW_EXACT_SPARSE_MAX_ROWS + 1;
    const std::vector<uint32_t> factor_base{0, 3};
    std::vector<SIQSShadowRow> rows;
    rows.reserve(row_count);
    rows.push_back(make_zero_shadow_row(0));
    for (size_t row_index = 1; row_index < row_count; ++row_index) {
        rows.push_back(make_single_factor_shadow_row(row_index, 1));
    }

    SIQSShadowMatrixOptions options;
    options.backend = SIQSShadowMatrixBackend::sparse_only;
    options.max_dependencies = 2;
    options.sparse_worker_threads = 1;
    options.sparse_retry_count = 1;

    bool provider_called = false;
    auto provider = [&](const CSRMatrix&, size_t max_dependencies, uint64_t, uint32_t,
                        const gnfs::linalg::BlockWiedemannSeededPolicy&) {
        provider_called = true;
        CHECK(max_dependencies == options.max_dependencies);

        std::vector<std::vector<bool>> candidates;
        if (max_dependencies >= 1) {
            std::vector<bool> zero_singleton(row_count, false);
            zero_singleton[0] = true;
            candidates.push_back(std::move(zero_singleton));
        }
        if (max_dependencies >= 2) {
            std::vector<bool> independent_pair(row_count, false);
            independent_pair[1] = true;
            independent_pair[2] = true;
            candidates.push_back(std::move(independent_pair));
        }
        return candidates;
    };

    const auto outcome = gnfs::siqs::shadow_matrix_detail::solve_sparse_backend_with_provider(
        std::span<const SIQSShadowRow>(rows.data(), rows.size()), factor_base.size(), options,
        provider);
    CHECK(provider_called);
    CHECK(outcome.status == SIQSShadowMatrixStatus::valid);
    CHECK(outcome.solution.has_value());
    if (outcome.solution) {
        const std::vector<std::vector<size_t>> expected_dependencies{{0}, {1, 2}};
        CHECK(outcome.solution->dependencies == expected_dependencies);
    }
}

[[nodiscard]] bool
sparse_dependency_basis_is_independent(const gnfs::siqs::SIQSShadowMatrixSolution& solution);

void test_sparse_invalid_candidate_is_not_masked() {
    // Keep this shape just outside the bounded exact pass so the injected
    // provider exercises the same candidate-admission path as a wide sparse
    // solve.  Two identical rows have the valid dependency {0, 1}.
    const size_t row_count = gnfs::siqs::SIQS_SHADOW_EXACT_SPARSE_MAX_ROWS + 1;
    const std::vector<uint32_t> factor_base{0, 3};
    std::vector<SIQSShadowRow> rows;
    rows.reserve(row_count);
    for (size_t row_index = 0; row_index < row_count; ++row_index) {
        rows.push_back(make_single_factor_shadow_row(row_index, 1));
    }

    SIQSShadowMatrixOptions options;
    options.backend = SIQSShadowMatrixBackend::sparse_only;
    options.max_dependencies = 1;
    options.sparse_worker_threads = 1;
    options.sparse_retry_count = 1;

    bool provider_called = false;
    auto provider = [&](const CSRMatrix&, size_t max_dependencies, uint64_t, uint32_t,
                        const gnfs::linalg::BlockWiedemannSeededPolicy&) {
        provider_called = true;
        CHECK(max_dependencies == 1);
        std::vector<bool> malformed(1, true);
        std::vector<bool> valid(row_count, false);
        valid[0] = true;
        valid[1] = true;
        // Put the malformed result after a valid dependency to prove that a
        // filled dependency budget does not suppress validation of later data.
        return std::vector<std::vector<bool>>{std::move(valid), std::move(malformed)};
    };

    const auto outcome = gnfs::siqs::shadow_matrix_detail::solve_sparse_backend_with_provider(
        std::span<const SIQSShadowRow>(rows.data(), rows.size()), factor_base.size(), options,
        provider);
    CHECK(provider_called);
    CHECK(outcome.status == SIQSShadowMatrixStatus::solver_failure);
    CHECK(!outcome.solution.has_value());
}

void test_sparse_wide_shape_uses_real_seeded_bw() {
    // Cross the deterministic exact-pass row bound without introducing zero
    // rows, so the production wrapper must construct CSR and invoke seeded
    // Block Wiedemann. Every row has the same odd factor, making any
    // two-row combination a valid left-nullspace dependency.
    const size_t row_count = gnfs::siqs::SIQS_SHADOW_EXACT_SPARSE_MAX_ROWS + 1;
    const std::vector<uint32_t> factor_base{0, 3};
    std::vector<SIQSShadowRow> rows;
    rows.reserve(row_count);
    for (size_t row_index = 0; row_index < row_count; ++row_index) {
        rows.push_back(make_single_factor_shadow_row(row_index, 1));
    }

    SIQSShadowMatrixOptions options;
    options.backend = SIQSShadowMatrixBackend::sparse_only;
    options.max_dependencies = 1;
    options.sparse_worker_threads = 1;
    options.sparse_retry_count = 3;

    const auto result = solve_siqs_shadow_matrix(
        std::span<const SIQSShadowRow>(rows.data(), rows.size()),
        std::span<const uint32_t>(factor_base.data(), factor_base.size()), Integer(2), options);
    CHECK(result.status() == SIQSShadowMatrixStatus::valid);
    CHECK(result.solution().has_value());
    if (!result.solution()) {
        return;
    }
    CHECK(result.solution()->row_count == row_count);
    CHECK(result.solution()->column_count == factor_base.size());
    CHECK(result.solution()->dependencies.size() == 1);
    if (result.solution()->dependencies.empty()) {
        return;
    }
    const auto& dependency = result.solution()->dependencies.front();
    CHECK(!dependency.empty());
    CHECK(std::is_sorted(dependency.begin(), dependency.end()));
    CHECK(dependency_is_zero(rows, dependency, factor_base.size()));
    CHECK(sparse_dependency_basis_is_independent(*result.solution()));
}

void test_sparse_exact_small_rank_deficient_regression() {
    // A^T has rows [1,1,0] and [0,1,1].  Its one-dimensional left nullspace
    // is the dependency {0,1,2}; seeded BW used to return no candidates for
    // this valid shape, incorrectly reporting no_dependencies.
    const std::vector<uint32_t> factor_base{0, 3, 5};
    const std::vector<SIQSShadowRow> rows{
        SIQSShadowRow{
            SIQSShadowRowOrigin::raw_full,
            SIQSPostMergeRow{Integer(1),
                             false,
                             std::vector<SIQSFactorPower>{{1, 1}},
                             {},
                             std::vector<gnfs::siqs::SIQSSourceId>{gnfs::siqs::SIQSSourceId{0}}}},
        SIQSShadowRow{
            SIQSShadowRowOrigin::raw_full,
            SIQSPostMergeRow{Integer(1),
                             false,
                             std::vector<SIQSFactorPower>{{1, 1}, {2, 1}},
                             {},
                             std::vector<gnfs::siqs::SIQSSourceId>{gnfs::siqs::SIQSSourceId{1}}}},
        SIQSShadowRow{
            SIQSShadowRowOrigin::raw_full,
            SIQSPostMergeRow{Integer(1),
                             false,
                             std::vector<SIQSFactorPower>{{2, 1}},
                             {},
                             std::vector<gnfs::siqs::SIQSSourceId>{gnfs::siqs::SIQSSourceId{2}}}},
    };

    SIQSShadowMatrixOptions options;
    options.backend = SIQSShadowMatrixBackend::sparse_only;
    options.max_dependencies = 4;
    options.sparse_retry_count = 1;
    options.sparse_worker_threads = 1;
    const auto result = solve_siqs_shadow_matrix(
        std::span<const SIQSShadowRow>(rows.data(), rows.size()),
        std::span<const uint32_t>(factor_base.data(), factor_base.size()), Integer(2), options);

    CHECK(result.status() == SIQSShadowMatrixStatus::valid);
    CHECK(result.solution().has_value());
    if (!result.solution()) {
        return;
    }
    CHECK(result.solution()->dependencies.size() == 1);
    if (result.solution()->dependencies.size() == 1) {
        const auto& dependency = result.solution()->dependencies.front();
        CHECK(dependency == std::vector<size_t>({0, 1, 2}));
        CHECK(dependency_is_zero(rows, dependency, factor_base.size()));
    }
}

void test_sparse_limits_and_rank_evidence() {
    const std::vector<uint32_t> factor_base{0, 3, 5, 7, 11};
    const auto rows = make_rank_deficient_rows();
    const auto row_span = std::span<const SIQSShadowRow>(rows.data(), rows.size());
    const auto factor_span = std::span<const uint32_t>(factor_base.data(), factor_base.size());
    const auto estimate =
        gnfs::siqs::checked_siqs_shadow_sparse_workspace_estimate(row_span, factor_base.size(), 5);
    CHECK(estimate.has_value());
    if (!estimate) {
        return;
    }
    CHECK(estimate->nonzero_count == 8);
    CHECK(estimate->csr_bytes ==
          (rows.size() + 1) * sizeof(size_t) + estimate->nonzero_count * sizeof(uint32_t));
    CHECK(estimate->total_bytes >= estimate->csr_bytes);
    CHECK(estimate->vector_bytes != 0);
    CHECK(estimate->bm_bytes != 0);
    CHECK(estimate->candidate_bytes != 0);
    CHECK(estimate->dependency_bytes != 0);
    CHECK(estimate->rank_proof_bytes != 0);

    SIQSShadowMatrixOptions options;
    options.backend = SIQSShadowMatrixBackend::sparse_only;
    options.max_dependencies = 5;
    options.sparse_retry_count = 1;

    auto nnz_limited = options;
    nnz_limited.max_sparse_nonzero_count = estimate->nonzero_count - 1;
    CHECK(solve_siqs_shadow_matrix(row_span, factor_span, Integer(2), nnz_limited).status() ==
          SIQSShadowMatrixStatus::resource_limit);

    auto bytes_limited = options;
    bytes_limited.max_sparse_csr_bytes = estimate->csr_bytes - 1;
    CHECK(solve_siqs_shadow_matrix(row_span, factor_span, Integer(2), bytes_limited).status() ==
          SIQSShadowMatrixStatus::resource_limit);

    auto workspace_limited = options;
    workspace_limited.max_sparse_workspace_bytes = estimate->total_bytes - 1;
    CHECK(solve_siqs_shadow_matrix(row_span, factor_span, Integer(2), workspace_limited).status() ==
          SIQSShadowMatrixStatus::resource_limit);

    auto retry_zero = options;
    retry_zero.sparse_retry_count = 0;
    CHECK(solve_siqs_shadow_matrix(row_span, factor_span, Integer(2), retry_zero).status() ==
          SIQSShadowMatrixStatus::invalid_options);

    auto retry_too_large = options;
    retry_too_large.sparse_retry_count =
        gnfs::siqs::SIQS_SHADOW_MAX_SPARSE_RETRY_COUNT + uint32_t{1};
    CHECK(solve_siqs_shadow_matrix(row_span, factor_span, Integer(2), retry_too_large).status() ==
          SIQSShadowMatrixStatus::invalid_options);

    std::vector<SIQSShadowRow> full_rank_rows;
    full_rank_rows.push_back(make_single_factor_shadow_row(0, 1));
    full_rank_rows.push_back(make_single_factor_shadow_row(1, 2));
    const auto full_span =
        std::span<const SIQSShadowRow>(full_rank_rows.data(), full_rank_rows.size());
    const auto full_rank = solve_siqs_shadow_matrix(full_span, factor_span, Integer(2), options);
    CHECK(full_rank.status() == SIQSShadowMatrixStatus::valid);
    CHECK(full_rank.solution().has_value());
    if (full_rank.solution()) {
        CHECK(full_rank.solution()->dependencies.empty());
    }
}

void test_sparse_estimate_overflow_and_zero_row_budget() {
    using gnfs::siqs::checked_siqs_shadow_sparse_csr_estimate;
    using gnfs::siqs::checked_siqs_shadow_sparse_workspace_estimate;

    static_assert(noexcept(checked_siqs_shadow_sparse_csr_estimate(0, 0)));
    CHECK(checked_siqs_shadow_sparse_csr_estimate(0, 0).has_value());
    CHECK(checked_siqs_shadow_sparse_csr_estimate(0, 0)->csr_bytes == sizeof(size_t));
    CHECK(!checked_siqs_shadow_sparse_csr_estimate(std::numeric_limits<size_t>::max(), 0));
    CHECK(!checked_siqs_shadow_sparse_csr_estimate(1, std::numeric_limits<size_t>::max()));
    CHECK(!checked_siqs_shadow_sparse_workspace_estimate(std::numeric_limits<size_t>::max(), 1, 0,
                                                         1));
    CHECK(!checked_siqs_shadow_sparse_workspace_estimate(1, std::numeric_limits<size_t>::max(), 0,
                                                         1));

    const std::vector<uint32_t> factor_base{0, 3};
    std::vector<SIQSShadowRow> rows;
    rows.push_back(make_zero_shadow_row(0));
    rows.push_back(make_zero_shadow_row(1));
    const auto row_span = std::span<const SIQSShadowRow>(rows.data(), rows.size());
    const auto factor_span = std::span<const uint32_t>(factor_base.data(), factor_base.size());
    SIQSShadowMatrixOptions options;
    options.backend = SIQSShadowMatrixBackend::sparse_only;
    options.max_dependencies = rows.size();
    options.max_sparse_workspace_bytes = 0;
    const auto low_budget = solve_siqs_shadow_matrix(row_span, factor_span, Integer(2), options);
    // Workspace admission is performed before zero-row singleton promotion;
    // this conservative policy is part of the current sparse contract.
    CHECK(low_budget.status() == SIQSShadowMatrixStatus::resource_limit);
}

void test_sparse_exact_admission_boundaries() {
    using gnfs::siqs::checked_siqs_shadow_sparse_workspace_estimate;
    using gnfs::siqs::SIQS_SHADOW_EXACT_SPARSE_MAX_COLUMNS;
    using gnfs::siqs::SIQS_SHADOW_EXACT_SPARSE_MAX_NONZERO_COUNT;
    using gnfs::siqs::SIQS_SHADOW_EXACT_SPARSE_MAX_ROWS;

    // The admission arithmetic must keep the exact path representable at all
    // three documented limits.  This is an estimator-only check for the NNZ
    // boundary; constructing a million-entry SIQS corpus would add no
    // coverage for the checked dimension arithmetic.
    const auto boundary_estimate = checked_siqs_shadow_sparse_workspace_estimate(
        SIQS_SHADOW_EXACT_SPARSE_MAX_ROWS, SIQS_SHADOW_EXACT_SPARSE_MAX_COLUMNS,
        SIQS_SHADOW_EXACT_SPARSE_MAX_NONZERO_COUNT, 1, 1, 1, false, false, false);
    CHECK(boundary_estimate.has_value());
    if (boundary_estimate) {
        CHECK(boundary_estimate->nonzero_count == SIQS_SHADOW_EXACT_SPARSE_MAX_NONZERO_COUNT);
        CHECK(boundary_estimate->rank_proof_bytes >=
              SIQS_SHADOW_EXACT_SPARSE_MAX_COLUMNS * sizeof(size_t));
    }

    // A zero-entry corpus at the row/column limits takes the real exact route
    // and returns its first singleton nullspace vector without invoking BW.
    std::vector<uint32_t> factor_base(SIQS_SHADOW_EXACT_SPARSE_MAX_COLUMNS);
    factor_base[0] = 0;
    for (size_t index = 1; index < factor_base.size(); ++index) {
        factor_base[index] = static_cast<uint32_t>(index + 1);
    }
    std::vector<SIQSShadowRow> rows;
    rows.reserve(SIQS_SHADOW_EXACT_SPARSE_MAX_ROWS);
    for (size_t source_id = 0; source_id < SIQS_SHADOW_EXACT_SPARSE_MAX_ROWS; ++source_id) {
        rows.push_back(make_zero_shadow_row(source_id));
    }

    SIQSShadowMatrixOptions options;
    options.backend = SIQSShadowMatrixBackend::sparse_only;
    options.max_dependencies = 1;
    options.sparse_worker_threads = 1;
    options.sparse_retry_count = 1;
    const auto result = solve_siqs_shadow_matrix(
        std::span<const SIQSShadowRow>(rows.data(), rows.size()),
        std::span<const uint32_t>(factor_base.data(), factor_base.size()), Integer(2), options);
    CHECK(result.status() == SIQSShadowMatrixStatus::valid);
    CHECK(result.solution().has_value());
    if (result.solution()) {
        CHECK(result.solution()->row_count == SIQS_SHADOW_EXACT_SPARSE_MAX_ROWS);
        CHECK(result.solution()->column_count == SIQS_SHADOW_EXACT_SPARSE_MAX_COLUMNS);
        CHECK(result.solution()->dependencies == std::vector<std::vector<size_t>>{{0}});
    }
}

[[nodiscard]] bool
sparse_dependency_basis_is_independent(const gnfs::siqs::SIQSShadowMatrixSolution& solution) {
    const size_t words = solution.row_count / size_t{64} +
                         ((solution.row_count % size_t{64}) != 0 ? size_t{1} : size_t{0});
    std::vector<std::vector<uint64_t>> pivots(solution.row_count);
    for (const auto& dependency : solution.dependencies) {
        if (dependency.empty()) {
            return false;
        }
        std::vector<uint64_t> candidate(words, uint64_t{0});
        for (const size_t row : dependency) {
            if (row >= solution.row_count) {
                return false;
            }
            candidate[row / size_t{64}] |= uint64_t{1} << (row % size_t{64});
        }
        bool inserted = false;
        for (size_t bit = 0; bit < solution.row_count; ++bit) {
            if ((candidate[bit / size_t{64}] & (uint64_t{1} << (bit % size_t{64}))) == 0) {
                continue;
            }
            if (pivots[bit].empty()) {
                pivots[bit] = std::move(candidate);
                inserted = true;
                break;
            }
            for (size_t word = 0; word < words; ++word) {
                candidate[word] ^= pivots[bit][word];
            }
        }
        if (!inserted) {
            return false;
        }
    }
    return true;
}

void test_sparse_exact_matches_dense_random_small() {
    // Both implementations solve the same left nullspace, but their pivot
    // order and basis vectors need not match.  Compare the exact nullity and
    // independently validate every sparse dependency instead of comparing
    // serialized basis order.
    std::mt19937_64 rng(0x5A17E2C4D901ULL);
    for (size_t trial = 0; trial < 96; ++trial) {
        const size_t equation_count = 1 + static_cast<size_t>(rng() % 18);
        const size_t row_count = 1 + static_cast<size_t>(rng() % 20);
        std::vector<uint32_t> factor_base(equation_count + 1);
        factor_base[0] = 0;
        for (size_t column = 1; column < factor_base.size(); ++column) {
            factor_base[column] = static_cast<uint32_t>(2 * column + 1);
        }

        std::vector<SIQSShadowRow> rows;
        rows.reserve(row_count);
        for (size_t row_index = 0; row_index < row_count; ++row_index) {
            std::vector<SIQSFactorPower> powers;
            for (uint32_t column = 1; column < factor_base.size(); ++column) {
                if ((rng() & 1U) != 0) {
                    powers.push_back(SIQSFactorPower{column, 1});
                }
            }
            rows.push_back(
                SIQSShadowRow{SIQSShadowRowOrigin::raw_full,
                              SIQSPostMergeRow{Integer(1),
                                               (rng() & 1U) != 0,
                                               std::move(powers),
                                               {},
                                               std::vector<gnfs::siqs::SIQSSourceId>{
                                                   gnfs::siqs::SIQSSourceId{row_index}}}});
        }

        const auto row_span = std::span<const SIQSShadowRow>(rows.data(), rows.size());
        const auto factor_span = std::span<const uint32_t>(factor_base.data(), factor_base.size());

        SIQSShadowMatrixOptions dense_options;
        dense_options.backend = SIQSShadowMatrixBackend::dense_only;
        dense_options.max_dependencies = row_count;
        dense_options.max_dense_matrix_bytes = std::numeric_limits<size_t>::max();
        dense_options.max_dense_variable_count = std::numeric_limits<size_t>::max();
        const auto dense =
            solve_siqs_shadow_matrix(row_span, factor_span, Integer(2), dense_options);

        SIQSShadowMatrixOptions sparse_options;
        sparse_options.backend = SIQSShadowMatrixBackend::sparse_only;
        sparse_options.max_dependencies = row_count;
        sparse_options.sparse_retry_count = 1;
        sparse_options.sparse_worker_threads = 1;
        const auto sparse =
            solve_siqs_shadow_matrix(row_span, factor_span, Integer(2), sparse_options);

        CHECK(dense.status() == SIQSShadowMatrixStatus::valid);
        CHECK(sparse.status() == SIQSShadowMatrixStatus::valid);
        if (!dense.solution() || !sparse.solution()) {
            continue;
        }
        CHECK(dense.solution()->dependencies.size() == sparse.solution()->dependencies.size());
        CHECK(sparse.solution()->row_count == row_count);
        CHECK(sparse.solution()->column_count == factor_base.size());
        CHECK(std::all_of(sparse.solution()->dependencies.begin(),
                          sparse.solution()->dependencies.end(), [&](const auto& dependency) {
                              return !dependency.empty() &&
                                     std::is_sorted(dependency.begin(), dependency.end()) &&
                                     dependency_is_zero(rows, dependency, factor_base.size());
                          }));
        CHECK(sparse_dependency_basis_is_independent(*sparse.solution()));
    }
}

} // namespace

int main() {
    test_owning_csr_round_trip();
    test_owning_csr_validation();
    test_seeded_view_contract();
    test_seeded_policy_is_environment_independent();
    test_seeded_scratch_isolation_under_concurrency();
    test_seeded_view_zero_column_boundary();
    test_seeded_empty_view_validation_order();
    test_matrix_status_mapping_contract();
    test_sparse_dispatch_and_automatic_promotion();
    test_dense_route_options_contract();
    test_sparse_solver_returns_verified_basis();
    test_sparse_invalid_candidate_is_not_masked();
    test_sparse_provider_budget_includes_admitted_zero_rows();
    test_sparse_wide_shape_uses_real_seeded_bw();
    test_sparse_exact_small_rank_deficient_regression();
    test_sparse_limits_and_rank_evidence();
    test_sparse_estimate_overflow_and_zero_row_budget();
    test_sparse_exact_admission_boundaries();
    test_sparse_exact_matches_dense_random_small();
    std::cout << "SIQS shadow sparse backend: " << checks_passed << " checks passed, "
              << checks_failed << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}

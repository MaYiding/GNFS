#pragma once

// Frozen deterministic identity for the bounded 256-A SIQS proof-shadow path.
// Worker counts, timings, and process-memory observations are evidence only.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace gnfs::tests {

struct SIQSMultiAProofGoldenV4 final {
    std::size_t max_dependencies;
    std::size_t parallel_column_threshold;
    std::size_t max_dense_matrix_bytes;
    std::size_t max_dense_variable_count;
    std::size_t matrix_rows;
    std::size_t matrix_columns;
    std::size_t minimum_nullity;
    std::size_t dependencies_returned;
    std::size_t dependencies_examined;
    std::size_t dependencies_verified;
    bool dependency_cap_reached;
    uint64_t dependency_digest_low;
    uint64_t dependency_digest_high;
    std::size_t no_factor_count;
    std::size_t factor_found_count;
    std::size_t winning_dependency;
    std::size_t winning_dependency_size;
    std::string_view factor;
    std::string_view cofactor;
    std::string_view terminal_status;
    std::string_view matrix_status;
    std::string_view dependency_status;
    std::string_view factor_status;
    std::string_view failure_status;
};

inline constexpr SIQSMultiAProofGoldenV4 SIQS_MULTI_A_PROOF_GOLDEN_V4{
    64,
    0,
    345'816,
    1'701,
    1'701,
    1'601,
    100,
    64,
    4,
    4,
    true,
    UINT64_C(10254149926071895734),
    UINT64_C(17300745096364993287),
    3,
    1,
    3,
    703,
    "2041646378661656688438487",
    "8829847714527711737483339",
    "factor_found",
    "valid",
    "valid",
    "factor_found",
    "none",
};

} // namespace gnfs::tests

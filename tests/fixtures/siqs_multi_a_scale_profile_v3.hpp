#pragma once

// Frozen deterministic identity for the bounded 256-A SIQS scale profile.
// Worker counts, timings, and process-memory observations are evidence only.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace gnfs::tests {

struct SIQSMultiAScaleGoldenV3 final {
    std::string_view first_a;
    std::string_view last_a;
    std::size_t planner_attempts;
    std::size_t planner_duplicate_draws;
    uint64_t plan_digest_low;
    uint64_t plan_digest_high;
    std::size_t raw_relations;
    std::size_t raw_payload_bytes;
    uint64_t raw_digest_low;
    uint64_t raw_digest_high;
    uint64_t slot_digest_low;
    uint64_t slot_digest_high;
    std::size_t raw_full_relations;
    std::size_t raw_one_lp_relations;
    std::size_t raw_two_lp_candidates;
    std::size_t adapter_accepted_one_lp;
    std::size_t adapter_accepted_two_lp;
    std::size_t adapter_rejected_relations;
    std::size_t adapter_exact_duplicate;
    std::size_t graph_vertices;
    std::size_t graph_edges;
    std::size_t graph_components;
    std::size_t graph_cycles;
    std::size_t graph_cycle_incidences;
    std::size_t graph_max_cycle_length;
    std::size_t cycles_with_accepted_two_lp;
    std::size_t cycles_without_accepted_two_lp;
    std::size_t two_lp_edge_source_a_count;
    std::size_t cycle_source_a_count;
    uint64_t cycle_provenance_digest_low;
    uint64_t cycle_provenance_digest_high;
    std::size_t pretrim_rows;
    std::size_t selected_rows;
    std::size_t selected_full_rows;
    std::size_t selected_cycle_rows;
    uint64_t source_fingerprint_low;
    uint64_t source_fingerprint_high;
    uint64_t pretrim_fingerprint_low;
    uint64_t pretrim_fingerprint_high;
    uint64_t selected_fingerprint_low;
    uint64_t selected_fingerprint_high;
};

struct SIQSMultiAScaleFixtureV3 final {
    std::string_view profile_id;
    uint32_t band;
    std::string_view modulus;
    std::string_view factor_p;
    std::string_view factor_q;
    uint32_t factor_base_size;
    uint32_t sieve_half;
    uint32_t large_prime_multiplier;
    uint32_t a_factor_count;
    uint32_t sieve_error;
    uint32_t small_prime_cutoff;
    uint32_t seed;
    std::size_t a_count;
    std::size_t b_slots_per_a;
    SIQSMultiAScaleGoldenV3 golden;
};

inline constexpr SIQSMultiAScaleFixtureV3 SIQS_MULTI_A_SCALE_FIXTURE_V3{
    "siqs50_multi_a_256x32_scale_v3",
    50,
    "18027426610499408447671494571938206274555088868093",
    "2041646378661656688438487",
    "8829847714527711737483339",
    1600,
    65536,
    120,
    6,
    12,
    25,
    42,
    256,
    32,
    {
        "228011737959984857761",
        "235884298804888144139",
        256,
        0,
        UINT64_C(2132402111948970426),
        UINT64_C(3331495609548214574),
        18008,
        30050394,
        UINT64_C(5272179497076428132),
        UINT64_C(15848963677271175240),
        UINT64_C(2675139373410695744),
        UINT64_C(17317603334185087565),
        1385,
        7420,
        9203,
        7419,
        4624,
        4580,
        1,
        13581,
        12043,
        2346,
        808,
        1857,
        6,
        191,
        617,
        160,
        256,
        UINT64_C(12977246761921163132),
        UINT64_C(12380521641813228706),
        2191,
        1701,
        1383,
        318,
        UINT64_C(13792072274280994075),
        UINT64_C(7730575491251011754),
        UINT64_C(11231947477657928681),
        UINT64_C(13110892638711325127),
        UINT64_C(11745144848901110871),
        UINT64_C(3340986363997617983),
    },
};

} // namespace gnfs::tests

// test_siqs_shadow_proof_runner.cpp - bounded read-only shadow proof contracts

#include <gnfs/core/integer.hpp>
#include <gnfs/siqs/raw_relation_corpus_view.hpp>
#include <gnfs/siqs/relation.hpp>
#include <gnfs/siqs/shadow_proof_runner.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using std::int64_t;
using std::size_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

using gnfs::core::Integer;
using gnfs::siqs::checked_siqs_shadow_corpus_payload_bytes;
using gnfs::siqs::checked_siqs_shadow_relation_payload_bytes;
using gnfs::siqs::run_siqs_shadow_proof;
using gnfs::siqs::SIQSPostMergeDependencyStatus;
using gnfs::siqs::SIQSPostMergeFactorStatus;
using gnfs::siqs::SIQSRawRelationCorpusView;
using gnfs::siqs::SIQSRelation;
using gnfs::siqs::SIQSShadowAssemblyStatus;
using gnfs::siqs::SIQSShadowFingerprint;
using gnfs::siqs::SIQSShadowMatrixStatus;
using gnfs::siqs::SIQSShadowProofEvidence;
using gnfs::siqs::SIQSShadowProofFallbackReason;
using gnfs::siqs::SIQSShadowProofOptions;
using gnfs::siqs::SIQSShadowProofResult;
using gnfs::siqs::SIQSShadowProofStage;
using gnfs::siqs::SIQSShadowProofTerminalStatus;
using gnfs::siqs::TwoLargePrimeCycleBasisLimits;
using gnfs::siqs::TwoLargePrimeCycleBasisStatus;

int checks_passed = 0;
int checks_failed = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (condition) {                                                                           \
            ++checks_passed;                                                                       \
        } else {                                                                                   \
            ++checks_failed;                                                                       \
            std::cerr << "FAIL: " #condition " at " << __FILE__ << ':' << __LINE__ << '\n';        \
        }                                                                                          \
    } while (false)

const Integer oracle_modulus(91);
const Integer prime_divisor(13);
const std::vector<uint32_t> sign_only_factor_base{0};
const std::vector<uint32_t> mixed_factor_base{0, 2, 3, 5};

[[nodiscard]] SIQSRelation make_relation(int64_t value, bool negative,
                                         std::vector<uint8_t> exponents, uint64_t large_prime = 0,
                                         uint64_t large_prime2 = 0) {
    SIQSRelation relation{};
    relation.value = Integer(value);
    relation.negative = negative;
    relation.exponents = std::move(exponents);
    for (size_t i = 1; i < relation.exponents.size(); ++i) {
        if (relation.exponents[i] != 0) {
            relation.fb_indices.push_back(static_cast<uint32_t>(i));
        }
    }
    relation.large_prime = large_prime;
    relation.large_prime2 = large_prime2;
    return relation;
}

[[nodiscard]] SIQSRelation make_sign_only_full(int64_t value) {
    return make_relation(value, false, {0});
}

[[nodiscard]] std::vector<SIQSRelation> make_factor_corpus() {
    // Both rows have zero parity. Canonical ordering is x=1 then x=27:
    // the first square is trivial and the second splits 91 into 7 * 13.
    return {make_sign_only_full(27), make_sign_only_full(1)};
}

[[nodiscard]] std::vector<SIQSRelation> make_no_factor_corpus() {
    return {make_sign_only_full(90), make_sign_only_full(1)};
}

[[nodiscard]] std::vector<SIQSRelation> make_parallel_one_lp_corpus() {
    // Two parallel (0,29) edges form one two-incidence fundamental cycle.
    return {
        make_relation(5, false, {0, 2, 0, 0}, 29, 0),
        make_relation(22, false, {0, 0, 0, 0}, 29, 0),
    };
}

struct OracleSplitter {
    [[nodiscard]] std::pair<uint64_t, uint64_t> operator()(uint64_t cofactor) const noexcept {
        switch (cofactor) {
        case 121:
            return {11, 11};
        case 319:
            return {11, 29};
        case 451:
            return {11, 41};
        case 1189:
            return {29, 41};
        default:
            return {0, 0};
        }
    }
};

struct NoSplit {
    [[nodiscard]] std::pair<uint64_t, uint64_t> operator()(uint64_t) const noexcept {
        return {0, 0};
    }
};

[[nodiscard]] std::vector<SIQSRelation> make_main_corpus() {
    std::vector<SIQSRelation> relations;

    relations.push_back(make_relation(1, true, {0, 1, 2, 1}));
    relations.push_back(relations.back());
    relations.push_back(make_relation(92, true, {0, 1, 2, 1}));
    relations.push_back(make_relation(1, true, {0, 13, 2, 1}));
    relations.push_back(make_relation(4, true, {0, 0, 1, 2}));
    relations.push_back(make_relation(8, true, {0, 0, 3, 0}));
    relations.push_back(make_relation(9, true, {0, 1, 0, 1}));
    relations.push_back(make_relation(10, false, {0, 0, 2, 0}));

    relations.push_back(make_relation(2, true, {0, 252, 1, 0}, 29, 0));
    relations.push_back(relations.back());
    relations.push_back(make_relation(31, false, {0, 253, 1, 1}, 29, 0));

    relations.push_back(make_relation(379, false, {0, 1, 2, 2}, 319, 1));
    relations.push_back(make_relation(38, false, {0, 24, 1, 0}, 451, 1));
    relations.push_back(make_relation(85, false, {0, 1, 1, 0}, 1189, 1));
    relations.push_back(make_relation(11, false, {0, 0, 0, 0}, 121, 1));
    return relations;
}

struct VectorStorageSnapshot {
    const void* data = nullptr;
    size_t size = 0;
    size_t capacity = 0;
};

template <class T>
[[nodiscard]] VectorStorageSnapshot snapshot_storage(const std::vector<T>& values) noexcept {
    return VectorStorageSnapshot{values.data(), values.size(), values.capacity()};
}

struct RelationSnapshot {
    Integer value;
    std::vector<uint32_t> fb_indices;
    std::vector<uint8_t> exponents;
    uint64_t large_prime = 0;
    uint64_t large_prime2 = 0;
    std::vector<uint64_t> merge_lps;
    bool negative = false;
    VectorStorageSnapshot fb_storage;
    VectorStorageSnapshot exponent_storage;
    VectorStorageSnapshot merge_lp_storage;
};

[[nodiscard]] std::vector<RelationSnapshot>
snapshot_relations(const std::vector<SIQSRelation>& relations) {
    std::vector<RelationSnapshot> snapshots;
    snapshots.reserve(relations.size());
    for (const SIQSRelation& relation : relations) {
        snapshots.push_back(RelationSnapshot{
            relation.value,
            relation.fb_indices,
            relation.exponents,
            relation.large_prime,
            relation.large_prime2,
            relation.merge_lps,
            relation.negative,
            snapshot_storage(relation.fb_indices),
            snapshot_storage(relation.exponents),
            snapshot_storage(relation.merge_lps),
        });
    }
    return snapshots;
}

void check_storage_unchanged(const VectorStorageSnapshot& before,
                             const VectorStorageSnapshot& after) {
    CHECK(after.data == before.data);
    CHECK(after.size == before.size);
    CHECK(after.capacity == before.capacity);
}

void check_relations_unchanged(const std::vector<SIQSRelation>& relations,
                               const std::vector<RelationSnapshot>& snapshots) {
    CHECK(relations.size() == snapshots.size());
    if (relations.size() != snapshots.size()) {
        return;
    }
    for (size_t i = 0; i < relations.size(); ++i) {
        const SIQSRelation& relation = relations[i];
        const RelationSnapshot& before = snapshots[i];
        CHECK(relation.value == before.value);
        CHECK(relation.fb_indices == before.fb_indices);
        CHECK(relation.exponents == before.exponents);
        CHECK(relation.large_prime == before.large_prime);
        CHECK(relation.large_prime2 == before.large_prime2);
        CHECK(relation.merge_lps == before.merge_lps);
        CHECK(relation.negative == before.negative);
        check_storage_unchanged(before.fb_storage, snapshot_storage(relation.fb_indices));
        check_storage_unchanged(before.exponent_storage, snapshot_storage(relation.exponents));
        check_storage_unchanged(before.merge_lp_storage, snapshot_storage(relation.merge_lps));
    }
}

template <class Splitter>
[[nodiscard]] SIQSShadowProofResult
run_immutable(const std::vector<SIQSRelation>& relations, const std::vector<uint32_t>& factor_base,
              const Integer& square_modulus, const Integer& gcd_target, uint64_t large_prime_bound,
              Splitter& splitter, const SIQSShadowProofOptions& options = {}) {
    const auto before = snapshot_relations(relations);
    auto result =
        run_siqs_shadow_proof(std::span<const SIQSRelation>(relations.data(), relations.size()),
                              std::span<const uint32_t>(factor_base.data(), factor_base.size()),
                              square_modulus, gcd_target, large_prime_bound, splitter, options);
    check_relations_unchanged(relations, before);
    return result;
}

template <class Splitter>
[[nodiscard]] SIQSShadowProofResult
run_segmented_immutable(const std::vector<SIQSRelation>& first,
                        const std::vector<SIQSRelation>& second,
                        const std::vector<uint32_t>& factor_base, const Integer& square_modulus,
                        const Integer& gcd_target, uint64_t large_prime_bound, Splitter& splitter,
                        const SIQSShadowProofOptions& options = {}) {
    const auto first_before = snapshot_relations(first);
    const auto second_before = snapshot_relations(second);
    const auto view = SIQSRawRelationCorpusView::try_create(
        std::span<const SIQSRelation>(first.data(), first.size()),
        std::span<const SIQSRelation>(second.data(), second.size()));
    if (!view) {
        throw std::runtime_error("segmented SIQS test corpus size overflow");
    }
    auto result = run_siqs_shadow_proof(
        *view, std::span<const uint32_t>(factor_base.data(), factor_base.size()), square_modulus,
        gcd_target, large_prime_bound, splitter, options);
    check_relations_unchanged(first, first_before);
    check_relations_unchanged(second, second_before);
    return result;
}

void check_terminal_contract(const SIQSShadowProofResult& result,
                             SIQSShadowProofTerminalStatus status, SIQSShadowProofStage stage,
                             SIQSShadowProofFallbackReason fallback) {
    CHECK(result.status() == status);
    CHECK(result.stage() == stage);
    CHECK(result.fallback_reason() == fallback);
    CHECK(result.has_factor() == (status == SIQSShadowProofTerminalStatus::factor_found));
    CHECK(result.factorization().has_value() ==
          (status == SIQSShadowProofTerminalStatus::factor_found));
    CHECK((status == SIQSShadowProofTerminalStatus::bounded_fallback) ==
          (fallback != SIQSShadowProofFallbackReason::none));
}

[[nodiscard]] bool same_factorization(const SIQSShadowProofResult& lhs,
                                      const SIQSShadowProofResult& rhs) {
    if (lhs.factorization().has_value() != rhs.factorization().has_value()) {
        return false;
    }
    return !lhs.factorization() || (lhs.factorization()->factor == rhs.factorization()->factor &&
                                    lhs.factorization()->cofactor == rhs.factorization()->cofactor);
}

[[nodiscard]] bool same_worker_independent_evidence(const SIQSShadowProofEvidence& lhs,
                                                    const SIQSShadowProofEvidence& rhs) {
    return lhs.raw_relations == rhs.raw_relations &&
           lhs.raw_payload_bytes == rhs.raw_payload_bytes &&
           lhs.factor_base_columns == rhs.factor_base_columns &&
           lhs.large_prime_bound == rhs.large_prime_bound && lhs.adapter == rhs.adapter &&
           lhs.graph_status == rhs.graph_status && lhs.graph_vertices == rhs.graph_vertices &&
           lhs.graph_edges == rhs.graph_edges && lhs.graph_components == rhs.graph_components &&
           lhs.graph_cycles == rhs.graph_cycles &&
           lhs.graph_cycle_incidences == rhs.graph_cycle_incidences &&
           lhs.graph_max_cycle_length == rhs.graph_max_cycle_length &&
           lhs.row_candidate_upper == rhs.row_candidate_upper &&
           lhs.assembly_status == rhs.assembly_status &&
           lhs.assembly_limit_evidence == rhs.assembly_limit_evidence &&
           lhs.assembly == rhs.assembly && lhs.assembly_fingerprints == rhs.assembly_fingerprints &&
           lhs.projected_dense_matrix_bytes == rhs.projected_dense_matrix_bytes &&
           lhs.matrix_status == rhs.matrix_status && lhs.matrix_rows == rhs.matrix_rows &&
           lhs.matrix_columns == rhs.matrix_columns && lhs.minimum_nullity == rhs.minimum_nullity &&
           lhs.dependencies_returned == rhs.dependencies_returned &&
           lhs.dependencies_examined == rhs.dependencies_examined &&
           lhs.dependencies_verified == rhs.dependencies_verified &&
           lhs.no_factor_count == rhs.no_factor_count &&
           lhs.factor_found_count == rhs.factor_found_count &&
           lhs.dependency_cap_reached == rhs.dependency_cap_reached &&
           lhs.dependency_fingerprint == rhs.dependency_fingerprint &&
           lhs.first_failed_dependency == rhs.first_failed_dependency &&
           lhs.winning_dependency == rhs.winning_dependency &&
           lhs.winning_dependency_size == rhs.winning_dependency_size &&
           lhs.dependency_status == rhs.dependency_status && lhs.factor_status == rhs.factor_status;
}

void check_factor_oracle(const SIQSShadowProofResult& result) {
    check_terminal_contract(result, SIQSShadowProofTerminalStatus::factor_found,
                            SIQSShadowProofStage::factor_extraction,
                            SIQSShadowProofFallbackReason::none);
    const auto& evidence = result.evidence();
    CHECK(evidence.raw_relations == 2);
    CHECK(evidence.raw_payload_bytes == 4);
    CHECK(evidence.matrix_rows == 2);
    CHECK(evidence.matrix_columns == 1);
    CHECK(evidence.minimum_nullity == 1);
    CHECK(evidence.projected_dense_matrix_bytes == 8);
    CHECK(evidence.matrix_status == SIQSShadowMatrixStatus::valid);
    CHECK(!evidence.assembly_limit_evidence.has_value());
    CHECK(evidence.dependencies_returned == 2);
    CHECK(evidence.dependencies_examined == 2);
    CHECK(evidence.dependencies_verified == 2);
    CHECK(evidence.no_factor_count == 1);
    CHECK(evidence.factor_found_count == 1);
    CHECK(!evidence.dependency_cap_reached);
    CHECK(evidence.dependency_fingerprint.has_value());
    CHECK(!evidence.first_failed_dependency.has_value());
    CHECK(evidence.winning_dependency == 1);
    CHECK(evidence.winning_dependency_size == 1);
    CHECK(evidence.dependency_status == SIQSPostMergeDependencyStatus::valid);
    CHECK(evidence.factor_status == SIQSPostMergeFactorStatus::factor_found);
    if (result.factorization()) {
        CHECK(result.factorization()->factor == Integer(7));
        CHECK(result.factorization()->cofactor == Integer(13));
        CHECK(result.factorization()->factor * result.factorization()->cofactor == oracle_modulus);
    }
}

void test_minimal_factor_no_factor_and_dependency_cap() {
    NoSplit splitter;
    const auto factor_relations = make_factor_corpus();
    const auto factor = run_immutable(factor_relations, sign_only_factor_base, oracle_modulus,
                                      oracle_modulus, 47, splitter);
    check_factor_oracle(factor);

    const auto no_factor_relations = make_no_factor_corpus();
    const auto no_factor = run_immutable(no_factor_relations, sign_only_factor_base, oracle_modulus,
                                         oracle_modulus, 47, splitter);
    check_terminal_contract(no_factor, SIQSShadowProofTerminalStatus::no_factor,
                            SIQSShadowProofStage::complete, SIQSShadowProofFallbackReason::none);
    CHECK(no_factor.evidence().dependencies_returned == 2);
    CHECK(no_factor.evidence().dependencies_examined == 2);
    CHECK(no_factor.evidence().dependencies_verified == 2);
    CHECK(no_factor.evidence().no_factor_count == 2);
    CHECK(no_factor.evidence().factor_found_count == 0);
    CHECK(no_factor.evidence().factor_status == SIQSPostMergeFactorStatus::no_factor);
    CHECK(!no_factor.evidence().winning_dependency.has_value());

    SIQSShadowProofOptions capped_options;
    capped_options.matrix.max_dependencies = 1;
    const auto capped = run_immutable(factor_relations, sign_only_factor_base, oracle_modulus,
                                      oracle_modulus, 47, splitter, capped_options);
    check_terminal_contract(capped, SIQSShadowProofTerminalStatus::no_factor,
                            SIQSShadowProofStage::complete, SIQSShadowProofFallbackReason::none);
    CHECK(capped.evidence().dependencies_returned == 1);
    CHECK(capped.evidence().dependencies_examined == 1);
    CHECK(capped.evidence().dependencies_verified == 1);
    CHECK(capped.evidence().no_factor_count == 1);
    CHECK(capped.evidence().factor_found_count == 0);
    CHECK(capped.evidence().dependency_cap_reached);
    CHECK(!capped.evidence().winning_dependency.has_value());
}

void test_raw_payload_row_and_pretrim_inclusive_caps() {
    NoSplit splitter;
    const auto relations = make_factor_corpus();
    CHECK(checked_siqs_shadow_relation_payload_bytes(relations[0]) == 2);
    CHECK(checked_siqs_shadow_relation_payload_bytes(relations[1]) == 2);
    const auto payload_bytes = checked_siqs_shadow_corpus_payload_bytes(relations);
    CHECK(payload_bytes == 4);

    SIQSShadowProofOptions exact_raw;
    exact_raw.limits.max_raw_relations = relations.size();
    check_factor_oracle(run_immutable(relations, sign_only_factor_base, oracle_modulus,
                                      oracle_modulus, 47, splitter, exact_raw));

    SIQSShadowProofOptions raw_short = exact_raw;
    raw_short.limits.max_raw_relations = relations.size() - 1;
    const auto raw_rejected = run_immutable(relations, sign_only_factor_base, oracle_modulus,
                                            oracle_modulus, 47, splitter, raw_short);
    check_terminal_contract(raw_rejected, SIQSShadowProofTerminalStatus::bounded_fallback,
                            SIQSShadowProofStage::payload_accounting,
                            SIQSShadowProofFallbackReason::raw_relation_limit);
    CHECK(!raw_rejected.evidence().raw_payload_bytes.has_value());

    SIQSShadowProofOptions exact_payload;
    exact_payload.limits.max_raw_payload_bytes = *payload_bytes;
    check_factor_oracle(run_immutable(relations, sign_only_factor_base, oracle_modulus,
                                      oracle_modulus, 47, splitter, exact_payload));

    SIQSShadowProofOptions payload_short = exact_payload;
    payload_short.limits.max_raw_payload_bytes = *payload_bytes - 1;
    const auto payload_rejected = run_immutable(relations, sign_only_factor_base, oracle_modulus,
                                                oracle_modulus, 47, splitter, payload_short);
    check_terminal_contract(payload_rejected, SIQSShadowProofTerminalStatus::bounded_fallback,
                            SIQSShadowProofStage::payload_accounting,
                            SIQSShadowProofFallbackReason::raw_payload_limit);
    CHECK(payload_rejected.evidence().raw_payload_bytes == payload_bytes);

    SIQSShadowProofOptions exact_candidates;
    exact_candidates.limits.max_row_candidates = 2;
    check_factor_oracle(run_immutable(relations, sign_only_factor_base, oracle_modulus,
                                      oracle_modulus, 47, splitter, exact_candidates));

    SIQSShadowProofOptions candidate_short = exact_candidates;
    candidate_short.limits.max_row_candidates = 1;
    const auto candidate_rejected = run_immutable(relations, sign_only_factor_base, oracle_modulus,
                                                  oracle_modulus, 47, splitter, candidate_short);
    check_terminal_contract(candidate_rejected, SIQSShadowProofTerminalStatus::bounded_fallback,
                            SIQSShadowProofStage::graph_preflight,
                            SIQSShadowProofFallbackReason::row_candidate_limit);
    CHECK(candidate_rejected.evidence().row_candidate_upper == 2);

    SIQSShadowProofOptions exact_pretrim;
    exact_pretrim.limits.max_pretrim_rows = 2;
    check_factor_oracle(run_immutable(relations, sign_only_factor_base, oracle_modulus,
                                      oracle_modulus, 47, splitter, exact_pretrim));

    SIQSShadowProofOptions pretrim_short = exact_pretrim;
    pretrim_short.limits.max_pretrim_rows = 1;
    const auto pretrim_rejected = run_immutable(relations, sign_only_factor_base, oracle_modulus,
                                                oracle_modulus, 47, splitter, pretrim_short);
    check_terminal_contract(pretrim_rejected, SIQSShadowProofTerminalStatus::bounded_fallback,
                            SIQSShadowProofStage::assembly,
                            SIQSShadowProofFallbackReason::pretrim_row_limit);
    CHECK(pretrim_rejected.evidence().assembly_status ==
          SIQSShadowAssemblyStatus::pretrim_row_limit);
    CHECK(pretrim_rejected.evidence().assembly_limit_evidence ==
          (gnfs::siqs::SIQSShadowAssemblyLimitEvidence{2, 1}));
    CHECK(pretrim_rejected.evidence().assembly == gnfs::siqs::SIQSShadowAssemblyStats{});
}

void test_graph_inclusive_caps() {
    NoSplit splitter;
    const auto relations = make_parallel_one_lp_corpus();

    SIQSShadowProofOptions exact;
    exact.limits.graph = TwoLargePrimeCycleBasisLimits{2, 1, 2};
    exact.limits.max_row_candidates = 1;
    const auto exact_result = run_immutable(relations, mixed_factor_base, oracle_modulus,
                                            oracle_modulus, 47, splitter, exact);
    check_terminal_contract(exact_result, SIQSShadowProofTerminalStatus::bounded_fallback,
                            SIQSShadowProofStage::assembly,
                            SIQSShadowProofFallbackReason::insufficient_rows);
    CHECK(exact_result.evidence().graph_status == TwoLargePrimeCycleBasisStatus::valid);
    CHECK(exact_result.evidence().graph_edges == 2);
    CHECK(exact_result.evidence().graph_cycles == 1);
    CHECK(exact_result.evidence().graph_cycle_incidences == 2);
    CHECK(exact_result.evidence().row_candidate_upper == 1);
    CHECK(exact_result.evidence().assembly.valid_cycle_rows == 1);

    SIQSShadowProofOptions edge_short = exact;
    edge_short.limits.graph.max_edges = 1;
    const auto edge_rejected = run_immutable(relations, mixed_factor_base, oracle_modulus,
                                             oracle_modulus, 47, splitter, edge_short);
    check_terminal_contract(edge_rejected, SIQSShadowProofTerminalStatus::bounded_fallback,
                            SIQSShadowProofStage::graph_preflight,
                            SIQSShadowProofFallbackReason::graph_edge_limit);
    CHECK(edge_rejected.evidence().graph_status == TwoLargePrimeCycleBasisStatus::edge_limit);

    SIQSShadowProofOptions cycle_short = exact;
    cycle_short.limits.graph.max_cycles = 0;
    const auto cycle_rejected = run_immutable(relations, mixed_factor_base, oracle_modulus,
                                              oracle_modulus, 47, splitter, cycle_short);
    check_terminal_contract(cycle_rejected, SIQSShadowProofTerminalStatus::bounded_fallback,
                            SIQSShadowProofStage::graph_preflight,
                            SIQSShadowProofFallbackReason::graph_cycle_limit);
    CHECK(cycle_rejected.evidence().graph_status == TwoLargePrimeCycleBasisStatus::cycle_limit);

    SIQSShadowProofOptions incidence_short = exact;
    incidence_short.limits.graph.max_cycle_incidences = 1;
    const auto incidence_rejected = run_immutable(relations, mixed_factor_base, oracle_modulus,
                                                  oracle_modulus, 47, splitter, incidence_short);
    check_terminal_contract(incidence_rejected, SIQSShadowProofTerminalStatus::bounded_fallback,
                            SIQSShadowProofStage::graph_preflight,
                            SIQSShadowProofFallbackReason::graph_incidence_limit);
    CHECK(incidence_rejected.evidence().graph_status ==
          TwoLargePrimeCycleBasisStatus::incidence_limit);

    SIQSShadowProofOptions candidate_short = exact;
    candidate_short.limits.max_row_candidates = 0;
    const auto candidate_rejected = run_immutable(relations, mixed_factor_base, oracle_modulus,
                                                  oracle_modulus, 47, splitter, candidate_short);
    check_terminal_contract(candidate_rejected, SIQSShadowProofTerminalStatus::bounded_fallback,
                            SIQSShadowProofStage::graph_preflight,
                            SIQSShadowProofFallbackReason::row_candidate_limit);
}

void test_insufficient_and_matrix_inclusive_caps() {
    NoSplit splitter;
    const std::vector<SIQSRelation> insufficient_relations{make_sign_only_full(1)};
    const auto insufficient = run_immutable(insufficient_relations, sign_only_factor_base,
                                            oracle_modulus, oracle_modulus, 47, splitter);
    check_terminal_contract(insufficient, SIQSShadowProofTerminalStatus::bounded_fallback,
                            SIQSShadowProofStage::assembly,
                            SIQSShadowProofFallbackReason::insufficient_rows);
    CHECK(insufficient.evidence().assembly.selected_rows == 1);
    CHECK(insufficient.evidence().factor_base_columns == 1);

    const auto relations = make_factor_corpus();
    SIQSShadowProofOptions exact_bytes;
    exact_bytes.matrix.max_dense_matrix_bytes = 8;
    check_factor_oracle(run_immutable(relations, sign_only_factor_base, oracle_modulus,
                                      oracle_modulus, 47, splitter, exact_bytes));

    SIQSShadowProofOptions byte_short = exact_bytes;
    byte_short.matrix.max_dense_matrix_bytes = 7;
    const auto bytes_rejected = run_immutable(relations, sign_only_factor_base, oracle_modulus,
                                              oracle_modulus, 47, splitter, byte_short);
    check_terminal_contract(bytes_rejected, SIQSShadowProofTerminalStatus::bounded_fallback,
                            SIQSShadowProofStage::matrix,
                            SIQSShadowProofFallbackReason::matrix_resource_limit);
    CHECK(bytes_rejected.evidence().projected_dense_matrix_bytes == 8);
    CHECK(bytes_rejected.evidence().matrix_status == SIQSShadowMatrixStatus::resource_limit);

    SIQSShadowProofOptions exact_variables;
    exact_variables.matrix.max_dense_variable_count = 2;
    check_factor_oracle(run_immutable(relations, sign_only_factor_base, oracle_modulus,
                                      oracle_modulus, 47, splitter, exact_variables));

    SIQSShadowProofOptions variable_short = exact_variables;
    variable_short.matrix.max_dense_variable_count = 1;
    const auto variables_rejected = run_immutable(relations, sign_only_factor_base, oracle_modulus,
                                                  oracle_modulus, 47, splitter, variable_short);
    check_terminal_contract(variables_rejected, SIQSShadowProofTerminalStatus::bounded_fallback,
                            SIQSShadowProofStage::matrix,
                            SIQSShadowProofFallbackReason::matrix_backend_unavailable);
    CHECK(variables_rejected.evidence().matrix_status ==
          SIQSShadowMatrixStatus::unsupported_backend);
}

void test_invalid_context_and_options() {
    NoSplit splitter;
    const auto relations = make_factor_corpus();

    const auto invalid_modulus =
        run_immutable(relations, sign_only_factor_base, Integer(1), oracle_modulus, 47, splitter);
    check_terminal_contract(invalid_modulus, SIQSShadowProofTerminalStatus::invalid_input,
                            SIQSShadowProofStage::input_validation,
                            SIQSShadowProofFallbackReason::none);

    const std::vector<uint32_t> invalid_factor_base{0, 3, 2};
    const auto invalid_fb =
        run_immutable(relations, invalid_factor_base, oracle_modulus, oracle_modulus, 47, splitter);
    check_terminal_contract(invalid_fb, SIQSShadowProofTerminalStatus::invalid_input,
                            SIQSShadowProofStage::input_validation,
                            SIQSShadowProofFallbackReason::none);

    const auto invalid_target =
        run_immutable(relations, sign_only_factor_base, oracle_modulus, Integer(1), 47, splitter);
    check_terminal_contract(invalid_target, SIQSShadowProofTerminalStatus::invalid_input,
                            SIQSShadowProofStage::input_validation,
                            SIQSShadowProofFallbackReason::none);

    const auto nondivisor_target =
        run_immutable(relations, sign_only_factor_base, oracle_modulus, Integer(10), 47, splitter);
    check_terminal_contract(nondivisor_target, SIQSShadowProofTerminalStatus::invalid_input,
                            SIQSShadowProofStage::input_validation,
                            SIQSShadowProofFallbackReason::none);

    const auto invalid_lp_bound = run_immutable(relations, sign_only_factor_base, oracle_modulus,
                                                oracle_modulus, 1, splitter);
    check_terminal_contract(invalid_lp_bound, SIQSShadowProofTerminalStatus::invalid_input,
                            SIQSShadowProofStage::input_validation,
                            SIQSShadowProofFallbackReason::none);

    SIQSShadowProofOptions zero_assembly_workers;
    zero_assembly_workers.assembly.materialization_workers = 0;
    const auto invalid_assembly_workers =
        run_immutable(relations, sign_only_factor_base, oracle_modulus, oracle_modulus, 47,
                      splitter, zero_assembly_workers);
    check_terminal_contract(invalid_assembly_workers, SIQSShadowProofTerminalStatus::invalid_input,
                            SIQSShadowProofStage::input_validation,
                            SIQSShadowProofFallbackReason::none);

    SIQSShadowProofOptions zero_dependencies;
    zero_dependencies.matrix.max_dependencies = 0;
    const auto invalid_dependencies =
        run_immutable(relations, sign_only_factor_base, oracle_modulus, oracle_modulus, 47,
                      splitter, zero_dependencies);
    check_terminal_contract(invalid_dependencies, SIQSShadowProofTerminalStatus::invalid_input,
                            SIQSShadowProofStage::input_validation,
                            SIQSShadowProofFallbackReason::none);

    SIQSShadowProofOptions zero_matrix_workers;
    zero_matrix_workers.matrix.elimination_workers = 0;
    const auto invalid_matrix_workers =
        run_immutable(relations, sign_only_factor_base, oracle_modulus, oracle_modulus, 47,
                      splitter, zero_matrix_workers);
    check_terminal_contract(invalid_matrix_workers, SIQSShadowProofTerminalStatus::invalid_input,
                            SIQSShadowProofStage::input_validation,
                            SIQSShadowProofFallbackReason::none);

    SIQSShadowProofOptions impossible_excess;
    impossible_excess.limits.minimum_row_excess = 2;
    impossible_excess.assembly.trim_excess_rows = 1;
    const auto invalid_excess_capacity =
        run_immutable(relations, sign_only_factor_base, oracle_modulus, oracle_modulus, 47,
                      splitter, impossible_excess);
    check_terminal_contract(invalid_excess_capacity, SIQSShadowProofTerminalStatus::invalid_input,
                            SIQSShadowProofStage::input_validation,
                            SIQSShadowProofFallbackReason::none);

    SIQSShadowProofOptions excess_overflow;
    excess_overflow.limits.minimum_row_excess = std::numeric_limits<size_t>::max();
    const auto invalid_excess = run_immutable(relations, sign_only_factor_base, oracle_modulus,
                                              oracle_modulus, 47, splitter, excess_overflow);
    check_terminal_contract(invalid_excess, SIQSShadowProofTerminalStatus::invalid_input,
                            SIQSShadowProofStage::input_validation,
                            SIQSShadowProofFallbackReason::none);

    SIQSShadowProofOptions trim_overflow;
    trim_overflow.assembly.trim_excess_rows = std::numeric_limits<size_t>::max();
    const auto invalid_trim = run_immutable(relations, sign_only_factor_base, oracle_modulus,
                                            oracle_modulus, 47, splitter, trim_overflow);
    check_terminal_contract(invalid_trim, SIQSShadowProofTerminalStatus::invalid_input,
                            SIQSShadowProofStage::input_validation,
                            SIQSShadowProofFallbackReason::none);
}

void test_malformed_full_and_rejected_cycle() {
    NoSplit splitter;

    // Adapter-valid full encoding whose arithmetic identity 2^2 == 1 (mod 91)
    // is false. This reaches assembly's rejected-full accounting rather than
    // being classified as malformed by the adapter preflight.
    const std::vector<SIQSRelation> malformed_relations{make_sign_only_full(2)};
    const auto malformed = run_immutable(malformed_relations, sign_only_factor_base, oracle_modulus,
                                         oracle_modulus, 47, splitter);
    check_terminal_contract(malformed, SIQSShadowProofTerminalStatus::stage_failure,
                            SIQSShadowProofStage::assembly, SIQSShadowProofFallbackReason::none);
    CHECK(malformed.evidence().assembly_status == SIQSShadowAssemblyStatus::valid);
    CHECK(malformed.evidence().assembly.rejected_full_relations == 1);

    const std::vector<SIQSRelation> rejected_cycle_relations{
        make_relation(3, true, {0, 252, 1, 0}, 29, 0),
        make_relation(31, false, {0, 253, 1, 1}, 29, 0),
    };
    const auto rejected_cycle = run_immutable(rejected_cycle_relations, mixed_factor_base,
                                              oracle_modulus, oracle_modulus, 47, splitter);
    check_terminal_contract(rejected_cycle, SIQSShadowProofTerminalStatus::stage_failure,
                            SIQSShadowProofStage::assembly, SIQSShadowProofFallbackReason::none);
    CHECK(rejected_cycle.evidence().graph_cycles == 1);
    CHECK(rejected_cycle.evidence().assembly.rejected_cycle_rows == 1);
}

struct AlwaysBadAllocSplitter {
    [[nodiscard]] std::pair<uint64_t, uint64_t> operator()(uint64_t) {
        throw std::bad_alloc{};
    }
};

struct AlwaysRuntimeSplitter {
    [[nodiscard]] std::pair<uint64_t, uint64_t> operator()(uint64_t) {
        throw std::runtime_error("injected adapter-preflight splitter failure");
    }
};

struct RuntimeOnSecondCallSplitter {
    size_t calls = 0;

    [[nodiscard]] std::pair<uint64_t, uint64_t> operator()(uint64_t cofactor) {
        ++calls;
        if (calls == 2) {
            throw std::runtime_error("injected assembly splitter failure");
        }
        return cofactor == 319 ? std::pair<uint64_t, uint64_t>{11, 29}
                               : std::pair<uint64_t, uint64_t>{0, 0};
    }
};

struct BadAllocOnSecondCallSplitter {
    size_t calls = 0;

    [[nodiscard]] std::pair<uint64_t, uint64_t> operator()(uint64_t cofactor) {
        ++calls;
        if (calls == 2) {
            throw std::bad_alloc{};
        }
        return cofactor == 319 ? std::pair<uint64_t, uint64_t>{11, 29}
                               : std::pair<uint64_t, uint64_t>{0, 0};
    }
};

struct DriftingSplitter {
    size_t calls = 0;

    [[nodiscard]] std::pair<uint64_t, uint64_t> operator()(uint64_t cofactor) noexcept {
        ++calls;
        if (cofactor == 319 && calls == 1) {
            return {11, 29};
        }
        return {0, 0};
    }
};

struct LateOracleSplitter {
    size_t calls = 0;

    [[nodiscard]] std::pair<uint64_t, uint64_t> operator()(uint64_t cofactor) noexcept {
        ++calls;
        if (calls <= 4) {
            return {0, 0};
        }
        return OracleSplitter{}(cofactor);
    }
};

void test_splitter_exceptions_and_drift() {
    const std::vector<SIQSRelation> relations{
        make_relation(379, false, {0, 1, 2, 2}, 319, 1),
    };

    AlwaysBadAllocSplitter bad_alloc_splitter;
    const auto bad_alloc_result = run_immutable(relations, mixed_factor_base, oracle_modulus,
                                                oracle_modulus, 47, bad_alloc_splitter);
    check_terminal_contract(bad_alloc_result, SIQSShadowProofTerminalStatus::resource_exhausted,
                            SIQSShadowProofStage::adapter_preflight,
                            SIQSShadowProofFallbackReason::none);

    AlwaysRuntimeSplitter always_runtime_splitter;
    const auto adapter_runtime_result = run_immutable(relations, mixed_factor_base, oracle_modulus,
                                                      oracle_modulus, 47, always_runtime_splitter);
    check_terminal_contract(
        adapter_runtime_result, SIQSShadowProofTerminalStatus::exception_failure,
        SIQSShadowProofStage::adapter_preflight, SIQSShadowProofFallbackReason::none);

    RuntimeOnSecondCallSplitter runtime_splitter;
    const auto runtime_result = run_immutable(relations, mixed_factor_base, oracle_modulus,
                                              oracle_modulus, 47, runtime_splitter);
    check_terminal_contract(runtime_result, SIQSShadowProofTerminalStatus::exception_failure,
                            SIQSShadowProofStage::assembly, SIQSShadowProofFallbackReason::none);
    CHECK(runtime_splitter.calls == 2);
    CHECK(runtime_result.evidence().assembly_status == SIQSShadowAssemblyStatus::exception_failure);

    BadAllocOnSecondCallSplitter assembly_bad_alloc_splitter;
    const auto assembly_bad_alloc_result =
        run_immutable(relations, mixed_factor_base, oracle_modulus, oracle_modulus, 47,
                      assembly_bad_alloc_splitter);
    check_terminal_contract(assembly_bad_alloc_result,
                            SIQSShadowProofTerminalStatus::resource_exhausted,
                            SIQSShadowProofStage::assembly, SIQSShadowProofFallbackReason::none);
    CHECK(assembly_bad_alloc_splitter.calls == 2);
    CHECK(assembly_bad_alloc_result.evidence().assembly_status ==
          SIQSShadowAssemblyStatus::resource_exhausted);

    DriftingSplitter drifting_splitter;
    const auto drift_result = run_immutable(relations, mixed_factor_base, oracle_modulus,
                                            oracle_modulus, 47, drifting_splitter);
    check_terminal_contract(drift_result, SIQSShadowProofTerminalStatus::internal_invariant_failure,
                            SIQSShadowProofStage::assembly, SIQSShadowProofFallbackReason::none);
    CHECK(drifting_splitter.calls == 2);
    CHECK(drift_result.evidence().adapter.accepted_two_lp == 1);
    CHECK(drift_result.evidence().assembly.adapter.invalid_two_large_prime_split == 1);
}

void test_segmented_corpus_matches_flattened_caps_and_exception_stages() {
    const auto relations = make_main_corpus();
    // The exact duplicate 1LP records at ordinals 8 and 9 are deliberately
    // split across independent backing vectors.
    const std::vector<SIQSRelation> first(relations.begin(), relations.begin() + 9);
    const std::vector<SIQSRelation> second(relations.begin() + 9, relations.end());
    const auto view = SIQSRawRelationCorpusView::try_create(
        std::span<const SIQSRelation>(first.data(), first.size()),
        std::span<const SIQSRelation>(second.data(), second.size()));
    if (!view) {
        throw std::runtime_error("segmented SIQS test corpus size overflow");
    }

    SIQSShadowProofOptions baseline_options;
    baseline_options.assembly.trim_excess_rows = 3;
    baseline_options.assembly.materialization_workers = 1;
    baseline_options.matrix.max_dependencies = 64;
    baseline_options.matrix.elimination_workers = 1;
    baseline_options.matrix.parallel_column_threshold = 0;

    OracleSplitter flat_splitter;
    const auto baseline = run_immutable(relations, mixed_factor_base, oracle_modulus,
                                        oracle_modulus, 41, flat_splitter, baseline_options);
    OracleSplitter segmented_splitter;
    const auto segmented =
        run_segmented_immutable(first, second, mixed_factor_base, oracle_modulus, oracle_modulus,
                                41, segmented_splitter, baseline_options);
    CHECK(segmented.status() == baseline.status());
    CHECK(segmented.stage() == baseline.stage());
    CHECK(segmented.fallback_reason() == baseline.fallback_reason());
    CHECK(segmented.evidence() == baseline.evidence());
    CHECK(same_factorization(segmented, baseline));
    CHECK(segmented.evidence().adapter.exact_duplicate == 1);

    const auto flat_payload = checked_siqs_shadow_corpus_payload_bytes(
        std::span<const SIQSRelation>(relations.data(), relations.size()));
    const auto segmented_payload = checked_siqs_shadow_corpus_payload_bytes(*view);
    CHECK(segmented_payload == flat_payload);
    CHECK(segmented_payload.has_value());
    if (!segmented_payload) {
        return;
    }

    SIQSShadowProofOptions exact_count = baseline_options;
    exact_count.limits.max_raw_relations = view->size();
    OracleSplitter exact_count_splitter;
    const auto exact_count_result =
        run_segmented_immutable(first, second, mixed_factor_base, oracle_modulus, oracle_modulus,
                                41, exact_count_splitter, exact_count);
    CHECK(same_worker_independent_evidence(exact_count_result.evidence(), baseline.evidence()));
    CHECK(same_factorization(exact_count_result, baseline));

    SIQSShadowProofOptions short_count = exact_count;
    short_count.limits.max_raw_relations = view->size() - 1;
    OracleSplitter short_count_splitter;
    const auto short_count_result =
        run_segmented_immutable(first, second, mixed_factor_base, oracle_modulus, oracle_modulus,
                                41, short_count_splitter, short_count);
    check_terminal_contract(short_count_result, SIQSShadowProofTerminalStatus::bounded_fallback,
                            SIQSShadowProofStage::payload_accounting,
                            SIQSShadowProofFallbackReason::raw_relation_limit);
    CHECK(short_count_result.evidence().raw_relations == view->size());
    CHECK(!short_count_result.evidence().raw_payload_bytes.has_value());

    SIQSShadowProofOptions exact_payload = baseline_options;
    exact_payload.limits.max_raw_payload_bytes = *segmented_payload;
    OracleSplitter exact_payload_splitter;
    const auto exact_payload_result =
        run_segmented_immutable(first, second, mixed_factor_base, oracle_modulus, oracle_modulus,
                                41, exact_payload_splitter, exact_payload);
    CHECK(same_worker_independent_evidence(exact_payload_result.evidence(), baseline.evidence()));
    CHECK(same_factorization(exact_payload_result, baseline));

    SIQSShadowProofOptions short_payload = exact_payload;
    short_payload.limits.max_raw_payload_bytes = *segmented_payload - 1;
    OracleSplitter short_payload_splitter;
    const auto short_payload_result =
        run_segmented_immutable(first, second, mixed_factor_base, oracle_modulus, oracle_modulus,
                                41, short_payload_splitter, short_payload);
    check_terminal_contract(short_payload_result, SIQSShadowProofTerminalStatus::bounded_fallback,
                            SIQSShadowProofStage::payload_accounting,
                            SIQSShadowProofFallbackReason::raw_payload_limit);
    CHECK(short_payload_result.evidence().raw_payload_bytes == segmented_payload);

    // The only splitter-bearing relation is in the second segment. Preserve
    // both the adapter-preflight and second-pass assembly exception mappings.
    const std::vector<SIQSRelation> exception_first{
        make_relation(1, false, {0, 0, 0, 0}),
    };
    const std::vector<SIQSRelation> exception_second{
        make_relation(379, false, {0, 1, 2, 2}, 319, 1),
    };

    AlwaysRuntimeSplitter adapter_runtime_splitter;
    const auto adapter_runtime =
        run_segmented_immutable(exception_first, exception_second, mixed_factor_base,
                                oracle_modulus, oracle_modulus, 47, adapter_runtime_splitter);
    check_terminal_contract(adapter_runtime, SIQSShadowProofTerminalStatus::exception_failure,
                            SIQSShadowProofStage::adapter_preflight,
                            SIQSShadowProofFallbackReason::none);

    AlwaysBadAllocSplitter adapter_bad_alloc_splitter;
    const auto adapter_bad_alloc =
        run_segmented_immutable(exception_first, exception_second, mixed_factor_base,
                                oracle_modulus, oracle_modulus, 47, adapter_bad_alloc_splitter);
    check_terminal_contract(adapter_bad_alloc, SIQSShadowProofTerminalStatus::resource_exhausted,
                            SIQSShadowProofStage::adapter_preflight,
                            SIQSShadowProofFallbackReason::none);

    RuntimeOnSecondCallSplitter assembly_runtime_splitter;
    const auto assembly_runtime =
        run_segmented_immutable(exception_first, exception_second, mixed_factor_base,
                                oracle_modulus, oracle_modulus, 47, assembly_runtime_splitter);
    check_terminal_contract(assembly_runtime, SIQSShadowProofTerminalStatus::exception_failure,
                            SIQSShadowProofStage::assembly, SIQSShadowProofFallbackReason::none);
    CHECK(assembly_runtime_splitter.calls == 2);
    CHECK(assembly_runtime.evidence().assembly_status ==
          SIQSShadowAssemblyStatus::exception_failure);

    BadAllocOnSecondCallSplitter assembly_bad_alloc_splitter;
    const auto assembly_bad_alloc =
        run_segmented_immutable(exception_first, exception_second, mixed_factor_base,
                                oracle_modulus, oracle_modulus, 47, assembly_bad_alloc_splitter);
    check_terminal_contract(assembly_bad_alloc, SIQSShadowProofTerminalStatus::resource_exhausted,
                            SIQSShadowProofStage::assembly, SIQSShadowProofFallbackReason::none);
    CHECK(assembly_bad_alloc_splitter.calls == 2);
    CHECK(assembly_bad_alloc.evidence().assembly_status ==
          SIQSShadowAssemblyStatus::resource_exhausted);
}

void test_second_pass_assembly_limits_cannot_be_bypassed() {
    const auto relations = make_main_corpus();

    SIQSShadowProofOptions edge_options;
    edge_options.limits.graph = TwoLargePrimeCycleBasisLimits{2, 3, 6};
    LateOracleSplitter edge_splitter;
    const auto edge_result = run_immutable(relations, mixed_factor_base, oracle_modulus,
                                           oracle_modulus, 41, edge_splitter, edge_options);
    check_terminal_contract(edge_result, SIQSShadowProofTerminalStatus::internal_invariant_failure,
                            SIQSShadowProofStage::assembly, SIQSShadowProofFallbackReason::none);
    CHECK(edge_splitter.calls == 8);
    CHECK(edge_result.evidence().graph_status == TwoLargePrimeCycleBasisStatus::valid);
    CHECK(edge_result.evidence().graph_edges == 2);
    CHECK(edge_result.evidence().assembly_status == SIQSShadowAssemblyStatus::graph_edge_limit);

    SIQSShadowProofOptions cycle_options;
    cycle_options.limits.graph = TwoLargePrimeCycleBasisLimits{6, 1, 6};
    LateOracleSplitter cycle_splitter;
    const auto cycle_result = run_immutable(relations, mixed_factor_base, oracle_modulus,
                                            oracle_modulus, 41, cycle_splitter, cycle_options);
    check_terminal_contract(cycle_result, SIQSShadowProofTerminalStatus::internal_invariant_failure,
                            SIQSShadowProofStage::assembly, SIQSShadowProofFallbackReason::none);
    CHECK(cycle_splitter.calls == 8);
    CHECK(cycle_result.evidence().graph_cycles == 1);
    CHECK(cycle_result.evidence().assembly_status == SIQSShadowAssemblyStatus::graph_cycle_limit);

    SIQSShadowProofOptions incidence_options;
    incidence_options.limits.graph = TwoLargePrimeCycleBasisLimits{6, 3, 2};
    LateOracleSplitter incidence_splitter;
    const auto incidence_result =
        run_immutable(relations, mixed_factor_base, oracle_modulus, oracle_modulus, 41,
                      incidence_splitter, incidence_options);
    check_terminal_contract(incidence_result,
                            SIQSShadowProofTerminalStatus::internal_invariant_failure,
                            SIQSShadowProofStage::assembly, SIQSShadowProofFallbackReason::none);
    CHECK(incidence_splitter.calls == 8);
    CHECK(incidence_result.evidence().graph_cycle_incidences == 2);
    CHECK(incidence_result.evidence().assembly_status ==
          SIQSShadowAssemblyStatus::graph_incidence_limit);

    SIQSShadowProofOptions candidate_options;
    candidate_options.limits.graph = TwoLargePrimeCycleBasisLimits{6, 3, 6};
    candidate_options.limits.max_row_candidates = 9;
    LateOracleSplitter candidate_splitter;
    const auto candidate_result =
        run_immutable(relations, mixed_factor_base, oracle_modulus, oracle_modulus, 41,
                      candidate_splitter, candidate_options);
    check_terminal_contract(candidate_result,
                            SIQSShadowProofTerminalStatus::internal_invariant_failure,
                            SIQSShadowProofStage::assembly, SIQSShadowProofFallbackReason::none);
    CHECK(candidate_splitter.calls == 8);
    CHECK(candidate_result.evidence().row_candidate_upper == 9);
    CHECK(candidate_result.evidence().assembly_status ==
          SIQSShadowAssemblyStatus::row_candidate_limit);
    CHECK(candidate_result.evidence().assembly_limit_evidence ==
          (gnfs::siqs::SIQSShadowAssemblyLimitEvidence{10, 9}));

    SIQSShadowProofOptions pretrim_options;
    pretrim_options.limits.graph = TwoLargePrimeCycleBasisLimits{6, 3, 6};
    pretrim_options.limits.max_row_candidates = 10;
    pretrim_options.limits.max_pretrim_rows = 8;
    LateOracleSplitter pretrim_splitter;
    const auto pretrim_result =
        run_immutable(relations, mixed_factor_base, oracle_modulus, oracle_modulus, 41,
                      pretrim_splitter, pretrim_options);
    check_terminal_contract(pretrim_result, SIQSShadowProofTerminalStatus::bounded_fallback,
                            SIQSShadowProofStage::assembly,
                            SIQSShadowProofFallbackReason::pretrim_row_limit);
    CHECK(pretrim_splitter.calls == 8);
    CHECK(pretrim_result.evidence().assembly_status == SIQSShadowAssemblyStatus::pretrim_row_limit);
    CHECK(pretrim_result.evidence().assembly_limit_evidence ==
          (gnfs::siqs::SIQSShadowAssemblyLimitEvidence{9, 8}));
}

void test_result_copy_and_move_contracts() {
    NoSplit splitter;
    const auto relations = make_factor_corpus();
    auto original = run_immutable(relations, sign_only_factor_base, oracle_modulus, oracle_modulus,
                                  47, splitter);
    check_factor_oracle(original);

    SIQSShadowProofResult copied(original);
    CHECK(copied.status() == original.status());
    CHECK(copied.stage() == original.stage());
    CHECK(copied.fallback_reason() == original.fallback_reason());
    CHECK(copied.evidence() == original.evidence());
    CHECK(same_factorization(copied, original));

    auto copy_assigned = run_immutable(make_no_factor_corpus(), sign_only_factor_base,
                                       oracle_modulus, oracle_modulus, 47, splitter);
    copy_assigned = original;
    CHECK(copy_assigned.evidence() == original.evidence());
    CHECK(same_factorization(copy_assigned, original));
    const SIQSShadowProofResult* copy_alias = &copy_assigned;
    copy_assigned = *copy_alias;
    CHECK(copy_assigned.evidence() == original.evidence());

    SIQSShadowProofResult moved(std::move(original));
    check_factor_oracle(moved);
    check_terminal_contract(original, SIQSShadowProofTerminalStatus::internal_invariant_failure,
                            SIQSShadowProofStage::not_started, SIQSShadowProofFallbackReason::none);
    CHECK(original.evidence() == SIQSShadowProofEvidence{});

    auto move_assigned = run_immutable(make_no_factor_corpus(), sign_only_factor_base,
                                       oracle_modulus, oracle_modulus, 47, splitter);
    move_assigned = std::move(moved);
    check_factor_oracle(move_assigned);
    check_terminal_contract(moved, SIQSShadowProofTerminalStatus::internal_invariant_failure,
                            SIQSShadowProofStage::not_started, SIQSShadowProofFallbackReason::none);
    CHECK(moved.evidence() == SIQSShadowProofEvidence{});
}

void test_mixed_corpus_worker_determinism() {
    const auto relations = make_main_corpus();
    OracleSplitter splitter;

    SIQSShadowProofOptions baseline_options;
    baseline_options.assembly.trim_excess_rows = 3;
    baseline_options.assembly.materialization_workers = 1;
    baseline_options.matrix.max_dependencies = 64;
    baseline_options.matrix.elimination_workers = 1;
    baseline_options.matrix.parallel_column_threshold = 0;
    const auto baseline = run_immutable(relations, mixed_factor_base, oracle_modulus,
                                        oracle_modulus, 41, splitter, baseline_options);
    check_terminal_contract(baseline, SIQSShadowProofTerminalStatus::factor_found,
                            SIQSShadowProofStage::factor_extraction,
                            SIQSShadowProofFallbackReason::none);
    CHECK(baseline.evidence().assembly.selected_rows == 7);
    CHECK(baseline.evidence().assembly.pretrim_rows == 9);
    CHECK(baseline.evidence().assembly.valid_cycle_rows == 3);
    CHECK(baseline.evidence().adapter.accepted_one_lp == 2);
    CHECK(baseline.evidence().adapter.accepted_two_lp == 4);
    CHECK(baseline.evidence().adapter.exact_duplicate == 1);
    CHECK(baseline.evidence().dependencies_returned == 5);
    CHECK(baseline.evidence().dependencies_examined == 1);
    CHECK(baseline.evidence().dependencies_verified == 1);
    CHECK(baseline.evidence().winning_dependency == 0);
    CHECK(baseline.evidence().winning_dependency_size == 2);
    CHECK(baseline.evidence().dependency_fingerprint.has_value());
    CHECK(baseline.evidence().assembly_fingerprints.selected_rows != SIQSShadowFingerprint{});
    if (baseline.factorization()) {
        CHECK(baseline.factorization()->factor == Integer(7));
        CHECK(baseline.factorization()->cofactor == Integer(13));
    }

    for (const uint32_t workers : std::array<uint32_t, 2>{2, 4}) {
        SIQSShadowProofOptions options = baseline_options;
        options.assembly.materialization_workers = workers;
        options.matrix.elimination_workers = workers;
        const auto candidate = run_immutable(relations, mixed_factor_base, oracle_modulus,
                                             oracle_modulus, 41, splitter, options);
        check_terminal_contract(candidate, SIQSShadowProofTerminalStatus::factor_found,
                                SIQSShadowProofStage::factor_extraction,
                                SIQSShadowProofFallbackReason::none);
        CHECK(same_worker_independent_evidence(candidate.evidence(), baseline.evidence()));
        CHECK(same_factorization(candidate, baseline));
    }

    // The same square proof cannot nontrivially split the prime divisor 13.
    const auto no_factor = run_immutable(relations, mixed_factor_base, oracle_modulus,
                                         prime_divisor, 41, splitter, baseline_options);
    check_terminal_contract(no_factor, SIQSShadowProofTerminalStatus::no_factor,
                            SIQSShadowProofStage::complete, SIQSShadowProofFallbackReason::none);
    CHECK(no_factor.evidence().dependencies_returned == 5);
    CHECK(no_factor.evidence().dependencies_examined == 5);
    CHECK(no_factor.evidence().dependencies_verified == 5);
    CHECK(no_factor.evidence().no_factor_count == 5);
    CHECK(no_factor.evidence().factor_found_count == 0);
    CHECK(no_factor.evidence().dependency_fingerprint ==
          baseline.evidence().dependency_fingerprint);
}

} // namespace

int main() {
    test_minimal_factor_no_factor_and_dependency_cap();
    test_raw_payload_row_and_pretrim_inclusive_caps();
    test_graph_inclusive_caps();
    test_insufficient_and_matrix_inclusive_caps();
    test_invalid_context_and_options();
    test_malformed_full_and_rejected_cycle();
    test_splitter_exceptions_and_drift();
    test_segmented_corpus_matches_flattened_caps_and_exception_stages();
    test_second_pass_assembly_limits_cannot_be_bypassed();
    test_result_copy_and_move_contracts();
    test_mixed_corpus_worker_determinism();

    std::cout << "SIQS shadow proof runner: " << checks_passed << " checks passed, "
              << checks_failed << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}

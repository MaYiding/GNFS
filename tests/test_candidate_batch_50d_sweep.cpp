#include <gnfs/api/pipeline.hpp>
#include <gnfs/cofactor/candidate_batch.hpp>
#include <gnfs/cofactor/candidate_chunk_plan.hpp>
#include <gnfs/cofactor/cofactorizer.hpp>
#include <gnfs/relation/reduction_engine.hpp>
#include <gnfs/sieve/adaptive_lattice.hpp>
#include <gnfs/sieve/lattice_sieve.hpp>
#include <gnfs/sieve/local_thread_budget.hpp>
#include <gnfs/sieve/sieve_run_identity.hpp>
#include <gnfs/sieve/special_q.hpp>
#include <gnfs/util/msvc_compat.hpp>
#include <gnfs/util/ordered_parallel_map.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using gnfs::api::Config;
using gnfs::api::FactorizationMethod;
using gnfs::api::Pipeline;
using gnfs::cofactor::CandidateBatchOptions;
using gnfs::cofactor::CandidateBatchResult;
using gnfs::cofactor::Cofactorizer;
using gnfs::cofactor::CofactorizerConfig;
using gnfs::cofactor::verify_candidate_batch;
using gnfs::core::GNFSParams;
using gnfs::core::Integer;
using gnfs::core::PolynomialContext;
using gnfs::core::Relation;
using gnfs::factor_base::FactorBase;
using gnfs::relation::CorpusDigest;
using gnfs::relation::CorpusDigestAccumulator;
using gnfs::sieve::LocalSieveThreadPlan;
using gnfs::sieve::SieveCandidate;
using gnfs::sieve::SieveResult;
using gnfs::sieve::SieveRunIdentity;
using gnfs::sieve::SpecialQ;

constexpr std::string_view PROBE_N = "16000000000000004000000216000000000000027000000729";
constexpr size_t PROBE_BITS = 164;
constexpr size_t PROBE_DIGITS = 50;
constexpr size_t PRODUCTION_BATCH_WIDTH = 4;
constexpr size_t DEFAULT_REPETITIONS = 3;
constexpr size_t MIN_REPETITIONS = 1;
constexpr size_t MAX_REPETITIONS = 9;
constexpr size_t CASE_ROTATION_STRIDE = 7;

constexpr std::array<uint32_t, 6> WORKER_CAPS{1, 2, 4, 6, 8, 10};
constexpr std::array<size_t, 5> CHUNK_SIZES{64, 128, 256, 512, 1024};
constexpr size_t SWEEP_CASES = WORKER_CAPS.size() * CHUNK_SIZES.size();

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

[[nodiscard]] size_t checked_add(size_t lhs, size_t rhs, std::string_view label) {
    if (lhs > std::numeric_limits<size_t>::max() - rhs) {
        throw std::overflow_error(std::string(label));
    }
    return lhs + rhs;
}

class ScopedEnvironment final {
public:
    ScopedEnvironment() = default;
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

    ~ScopedEnvironment() {
        for (auto entry = entries_.rbegin(); entry != entries_.rend(); ++entry) {
            if (entry->previous.has_value()) {
                (void)setenv(entry->name.c_str(), entry->previous->c_str(), 1);
            } else {
                (void)unsetenv(entry->name.c_str());
            }
        }
    }

    void set(std::string name, std::string value) {
        remember(name);
        if (setenv(name.c_str(), value.c_str(), 1) != 0) {
            entries_.pop_back();
            throw std::runtime_error("could not set environment variable " + name);
        }
    }

    void unset(std::string name) {
        remember(name);
        if (unsetenv(name.c_str()) != 0) {
            entries_.pop_back();
            throw std::runtime_error("could not unset environment variable " + name);
        }
    }

private:
    struct Entry final {
        std::string name;
        std::optional<std::string> previous;
    };

    void remember(const std::string& name) {
        require(std::none_of(entries_.begin(), entries_.end(),
                             [&](const Entry& entry) { return entry.name == name; }),
                "environment variable frozen more than once: " + name);
        std::optional<std::string> previous;
        if (const char* value = std::getenv(name.c_str()); value != nullptr) {
            previous = value;
        }
        entries_.push_back(Entry{name, std::move(previous)});
    }

    std::vector<Entry> entries_;
};

void freeze_probe_environment(ScopedEnvironment& environment) {
    // Keep the mathematical family aligned with the bounded production 50d probe.
    environment.set("GNFS_STRUCTURED_FILTER", "1");
    environment.set("GNFS_OOC_RELATIONS", "1");
    environment.unset("GNFS_OOC_BASE_PATH");
    environment.unset("GNFS_RESUME");
    environment.unset("GNFS_SIEVE_RESUME");
    environment.set("GNFS_DISTRIBUTED_SIEVE_WORKERS", "0");
    environment.set("GNFS_DISTRIBUTED_SIEVE_FORCE_SMALL", "0");
    environment.set("GNFS_ADAPTIVE_LATTICE", "0");
    environment.unset("GNFS_ADAPTIVE_LATTICE_THRESHOLD");
    environment.unset("GNFS_ADAPTIVE_LATTICE_MAX_RETRIES");
    environment.unset("GNFS_ADAPTIVE_LATTICE_SEED");
    environment.set("GNFS_3LP", "0");
    environment.set("GNFS_CASCADE_V3", "0");
    environment.set("GNFS_V0_BFS", "0");
    environment.unset("GNFS_OVERRIDE_LP_BITS");
    environment.unset("GNFS_SIEVE_TARGET_MULT");
    environment.unset("GNFS_LATTICE_LLL");
    environment.set("GNFS_LATTICE_SKEW", "0");
    environment.set("GNFS_NO_THIN_SOLVE", "1");
    environment.set("GNFS_SGE_STREAMING", "off");

    // CandidateBatch owns the only intended cofactor parallelism in this sweep.
    // Freeze nested or cached cofactor helpers to their sequential/default-off forms.
    environment.set("GNFS_COFACTOR_BRENT", "0");
    environment.set("GNFS_BRENT_POLLARD_RHO_THREADS", "1");
    environment.set("GNFS_ECM_STAGE1_PARALLEL_THREADS", "1");
    environment.set("GNFS_ECM_STAGE2_PARALLEL", "1");
    environment.set("GNFS_COFACTOR_BATCH_SIZE", "1");
    environment.set("GNFS_ECM_CURVE_POOL", "0");
    environment.set("GNFS_ECM_BATCH_INV", "0");
    environment.set("GNFS_ECM_SIGMA_POOL_SIZE", "0");
    environment.set("GNFS_ECM_B1_CACHE_SIZE", "0");
    environment.set("GNFS_COFACTOR_RESULT_CACHE_SIZE", "0");
    environment.set("GNFS_COFACTOR_TIMING_ENABLE", "0");
    environment.set("GNFS_SURVIVAL_FILTER", "0");
    environment.unset("GNFS_SURVIVAL_THRESHOLD");
    environment.set("GNFS_TRIAL_DIV_SIMD", "auto");
    environment.set("GNFS_ECM_BRENT_SUYAMA", "0");
    environment.unset("GNFS_ECM_BS_DEGREE");
}

struct CandidateFixtureDigest final {
    uint64_t low = 0;
    uint64_t high = 0;

    [[nodiscard]] bool operator==(const CandidateFixtureDigest&) const noexcept = default;
};

class CandidateFixtureDigestBuilder final {
public:
    CandidateFixtureDigestBuilder() {
        constexpr std::array<uint8_t, 23> domain{
            'G', 'N', 'F', 'S', '-', 'C', 'A', 'N', 'D', 'I', 'D', 'A',
            'T', 'E', '-', 'F', 'I', 'X', 'T', 'U', 'R', 'E', 1,
        };
        for (const uint8_t byte : domain) {
            append_byte(byte);
        }
    }

    void append_byte(uint8_t value) noexcept {
        low_ ^= static_cast<uint64_t>(value);
        low_ *= UINT64_C(1099511628211);

        high_ ^= static_cast<uint64_t>(value) + byte_index_ * UINT64_C(0x9e3779b97f4a7c15);
        high_ = std::rotl(high_, 27);
        high_ *= UINT64_C(0x94d049bb133111eb);
        high_ += UINT64_C(0x2545f4914f6cdd1d);
        ++byte_index_;
    }

    void append_u32(uint32_t value) noexcept {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            append_byte(static_cast<uint8_t>(value >> shift));
        }
    }

    void append_u64(uint64_t value) noexcept {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            append_byte(static_cast<uint8_t>(value >> shift));
        }
    }

    [[nodiscard]] CandidateFixtureDigest finish() const noexcept {
        return {avalanche(low_ ^ byte_index_), avalanche(high_ ^ std::rotl(byte_index_, 17))};
    }

private:
    [[nodiscard]] static uint64_t avalanche(uint64_t value) noexcept {
        value ^= value >> 30;
        value *= UINT64_C(0xbf58476d1ce4e5b9);
        value ^= value >> 27;
        value *= UINT64_C(0x94d049bb133111eb);
        value ^= value >> 31;
        return value;
    }

    uint64_t low_ = UINT64_C(14695981039346656037);
    uint64_t high_ = UINT64_C(0x243f6a8885a308d3);
    uint64_t byte_index_ = 0;
};

[[nodiscard]] CandidateFixtureDigest
candidate_fixture_digest(std::span<const SieveResult> sieve_results) {
    CandidateFixtureDigestBuilder builder;
    builder.append_u64(static_cast<uint64_t>(sieve_results.size()));
    for (size_t special_q_index = 0; special_q_index < sieve_results.size(); ++special_q_index) {
        const SieveResult& result = sieve_results[special_q_index];
        builder.append_byte(0x51);
        builder.append_u64(static_cast<uint64_t>(special_q_index));
        builder.append_u32(result.special_q.q);
        builder.append_u32(result.special_q.r);
        builder.append_u32(result.special_q.index);
        builder.append_u64(static_cast<uint64_t>(result.sieved_positions));
        builder.append_u64(static_cast<uint64_t>(result.smooth_count));
        builder.append_u64(static_cast<uint64_t>(result.candidates.size()));
        for (size_t candidate_index = 0; candidate_index < result.candidates.size();
             ++candidate_index) {
            const SieveCandidate& candidate = result.candidates[candidate_index];
            builder.append_byte(0x43);
            builder.append_u64(static_cast<uint64_t>(candidate_index));
            builder.append_u32(std::bit_cast<uint32_t>(candidate.i));
            builder.append_u32(std::bit_cast<uint32_t>(candidate.j));
            builder.append_u64(std::bit_cast<uint64_t>(candidate.a));
            builder.append_u64(candidate.b);
            builder.append_byte(candidate.residual);
        }
    }
    return builder.finish();
}

[[nodiscard]] size_t relation_count(const std::vector<std::vector<Relation>>& batches) {
    size_t total = 0;
    for (const auto& relations : batches) {
        total = checked_add(total, relations.size(), "relation total exceeds size_t");
    }
    return total;
}

[[nodiscard]] CorpusDigest relation_digest(const std::vector<std::vector<Relation>>& batches) {
    CorpusDigestAccumulator accumulator(relation_count(batches));
    for (const auto& relations : batches) {
        for (const Relation& relation : relations) {
            accumulator.append(relation);
        }
    }
    return accumulator.finish();
}

void require_same_relation(const Relation& actual, const Relation& expected,
                           const std::string& label) {
    require(actual.a == expected.a && actual.b == expected.b, label + ": (a,b) differs");
    require(actual.rational_factors == expected.rational_factors,
            label + ": rational factors differ");
    require(actual.algebraic_factors == expected.algebraic_factors,
            label + ": algebraic factors differ");
    require(actual.rational_large_prime == expected.rational_large_prime,
            label + ": rational large primes differ");
    require(actual.algebraic_large_prime == expected.algebraic_large_prime,
            label + ": algebraic large primes differ");
    require(actual.extra_ab_pairs == expected.extra_ab_pairs, label + ": extra (a,b) pairs differ");
}

void require_same_batches(const std::vector<std::vector<Relation>>& actual,
                          const std::vector<std::vector<Relation>>& expected,
                          const std::string& label) {
    require(actual.size() == expected.size(), label + ": outer shape differs");
    for (size_t special_q_index = 0; special_q_index < expected.size(); ++special_q_index) {
        require(actual[special_q_index].size() == expected[special_q_index].size(),
                label + ": per-special-Q relation count differs at index " +
                    std::to_string(special_q_index));
        for (size_t relation_index = 0; relation_index < expected[special_q_index].size();
             ++relation_index) {
            require_same_relation(actual[special_q_index][relation_index],
                                  expected[special_q_index][relation_index],
                                  label + ": relation " + std::to_string(special_q_index) + "/" +
                                      std::to_string(relation_index));
        }
    }
}

[[nodiscard]] std::vector<std::vector<Relation>>
serial_oracle(const PolynomialContext& context, const FactorBase& factor_base,
              const CofactorizerConfig& config, std::span<const SieveResult> sieve_results) {
    Cofactorizer cofactorizer(context, factor_base, config);
    std::vector<std::vector<Relation>> relations(sieve_results.size());
    for (size_t special_q_index = 0; special_q_index < sieve_results.size(); ++special_q_index) {
        const SieveResult& sieve_result = sieve_results[special_q_index];
        for (const SieveCandidate& candidate : sieve_result.candidates) {
            auto relation =
                cofactorizer.verify(candidate, sieve_result.special_q.q, sieve_result.special_q.r);
            if (relation.has_value()) {
                relations[special_q_index].push_back(std::move(*relation));
            }
        }
    }
    return relations;
}

struct FixedCandidateFixture final {
    PolynomialContext context;
    FactorBase factor_base;
    GNFSParams params;
    CofactorizerConfig cofactor_config;
    std::vector<SieveResult> sieve_results;
    SieveRunIdentity run_identity;
    LocalSieveThreadPlan generation_thread_plan;
    size_t generation_worker_limit = 0;
    uint64_t generation_wall_ns = 0;
};

void validate_probe_parameters(const Integer& n, const GNFSParams& params) {
    require(n.bit_length() == PROBE_BITS, "50-digit fixture no longer has 164 bits");
    require(PROBE_N.size() == PROBE_DIGITS, "50-digit fixture literal length changed");
    require(params.bits == PROBE_BITS, "Pipeline parameter bit count changed");
    require(params.digits == PROBE_DIGITS, "Pipeline parameter digit count changed");
    require(params.degree == 3, "50-digit polynomial degree changed");
    require(params.rational_bound == 80'000, "50-digit rational bound changed");
    require(params.algebraic_bound == 160'000, "50-digit algebraic bound changed");
    require(params.large_prime_bits == 23, "50-digit large-prime bit bound changed");
    require(params.large_prime_bound == (uint64_t{1} << 23), "50-digit large-prime bound changed");
    require(params.sieve_i_min == -2'048 && params.sieve_i_max == 2'047,
            "50-digit sieve width changed");
    require(params.sieve_j_min == 1 && params.sieve_j_max == 2'048,
            "50-digit sieve height changed");
    require(params.max_special_q == PRODUCTION_BATCH_WIDTH,
            "fixed candidate fixture lost its four-special-Q cap");
    require(params.max_special_q_batch_workers == 4, "50-digit special-Q worker cap changed");
    require(params.max_local_sieve_threads > 0,
            "Pipeline did not freeze a positive local sieve thread budget");
}

[[nodiscard]] FixedCandidateFixture make_fixed_candidate_fixture() {
    const Integer n{std::string(PROBE_N)};
    Config config;
    config.set_method(FactorizationMethod::GNFS)
        .set_max_special_q(PRODUCTION_BATCH_WIDTH)
        .set_max_special_q_batch_workers(4)
        .set_verbose(false);
    Pipeline pipeline(n, config);
    const GNFSParams params = pipeline.params();
    validate_probe_parameters(n, params);

    PolynomialContext context = pipeline.select_polynomial();
    FactorBase factor_base = pipeline.build_factor_base(context);
    require(factor_base.params().large_prime_bound == params.large_prime_bound,
            "factor-base and Pipeline large-prime bounds differ");

    gnfs::sieve::SieveParams sieve_params;
    sieve_params.rational_threshold = params.rational_threshold;
    sieve_params.algebraic_threshold = params.algebraic_threshold;

    gnfs::sieve::SieveRegion sieve_region;
    sieve_region.i_min = params.sieve_i_min;
    sieve_region.i_max = params.sieve_i_max;
    sieve_region.j_min = params.sieve_j_min;
    sieve_region.j_max = params.sieve_j_max;

    CofactorizerConfig cofactor_config;
    cofactor_config.large_prime_bound = factor_base.params().large_prime_bound;
    cofactor_config.allow_1lp = true;
    cofactor_config.allow_2lp = params.digits >= 50;
    cofactor_config.allow_3lp = false;

    gnfs::sieve::SpecialQRange special_q_range;
    special_q_range.min_q = params.special_q_min;
    special_q_range.max_q = params.special_q_max;
    gnfs::sieve::SpecialQGenerator generator(factor_base, special_q_range);

    const size_t production_batch_width = params.digits <= 50 ? 4 : 2;
    require(production_batch_width == PRODUCTION_BATCH_WIDTH,
            "50-digit production batch width changed");
    std::vector<SpecialQ> special_qs;
    special_qs.reserve(production_batch_width);
    while (special_qs.size() < production_batch_width && generator.has_next()) {
        auto special_q = generator.next();
        require(special_q.has_value(), "special-Q generator contradicted has_next()");
        special_qs.push_back(*special_q);
    }
    require(special_qs.size() == production_batch_width,
            "factor base cannot supply one production special-Q batch");

    const size_t generation_worker_limit =
        std::min<size_t>(params.max_local_sieve_threads, params.max_special_q_batch_workers);
    const LocalSieveThreadPlan thread_plan = gnfs::sieve::plan_local_sieve_threads(
        params.max_local_sieve_threads, generation_worker_limit, special_qs.size());
    require(!thread_plan.threads_per_worker.empty(),
            "production candidate generation planned no workers");
    require(thread_plan.assigned_threads == params.max_local_sieve_threads,
            "production candidate generation did not assign its full lane budget");

    std::vector<size_t> configured_threads(thread_plan.threads_per_worker.size(), 0);
    gnfs::sieve::AdaptiveBasisManager adaptive_manager;
    const auto generation_started = std::chrono::steady_clock::now();
    auto sieve_results = gnfs::util::ordered_work_stealing_map<SieveResult>(
        special_qs.size(), static_cast<uint32_t>(thread_plan.threads_per_worker.size()),
        [&](size_t worker_index) {
            auto sieve =
                std::make_unique<gnfs::sieve::LatticeSieve>(context, factor_base, sieve_params);
            sieve->set_region(sieve_region);
            sieve->set_max_threads(thread_plan.threads_per_worker[worker_index]);
            configured_threads[worker_index] = sieve->configured_max_threads();
            sieve->set_adaptive_manager(&adaptive_manager);
            return sieve;
        },
        [&](std::unique_ptr<gnfs::sieve::LatticeSieve>& sieve, size_t special_q_index) {
            return sieve->sieve_special_q(special_qs[special_q_index]);
        });
    const auto generation_finished = std::chrono::steady_clock::now();

    for (size_t worker_index = 0; worker_index < configured_threads.size(); ++worker_index) {
        require(configured_threads[worker_index] == thread_plan.threads_per_worker[worker_index],
                "production candidate generation did not apply its lane plan");
    }
    require(sieve_results.size() == special_qs.size(),
            "candidate generation changed special-Q batch shape");
    size_t total_candidates = 0;
    for (size_t special_q_index = 0; special_q_index < sieve_results.size(); ++special_q_index) {
        const auto& actual = sieve_results[special_q_index].special_q;
        const auto& expected = special_qs[special_q_index];
        require(actual.q == expected.q && actual.r == expected.r && actual.index == expected.index,
                "candidate generation changed special-Q order or metadata");
        total_candidates =
            checked_add(total_candidates, sieve_results[special_q_index].candidates.size(),
                        "candidate total exceeds size_t");
    }
    require(total_candidates > 0, "fixed production batch produced no candidates");

    const auto generation_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        generation_finished - generation_started)
                                        .count();
    require(generation_elapsed >= 0, "candidate generation clock moved backwards");
    const SieveRunIdentity run_identity =
        gnfs::sieve::make_sieve_run_identity(context, factor_base, params);

    return FixedCandidateFixture{
        .context = std::move(context),
        .factor_base = std::move(factor_base),
        .params = params,
        .cofactor_config = cofactor_config,
        .sieve_results = std::move(sieve_results),
        .run_identity = run_identity,
        .generation_thread_plan = thread_plan,
        .generation_worker_limit = generation_worker_limit,
        .generation_wall_ns = static_cast<uint64_t>(generation_elapsed),
    };
}

[[nodiscard]] size_t total_candidates(std::span<const SieveResult> sieve_results) {
    size_t total = 0;
    for (const SieveResult& result : sieve_results) {
        total = checked_add(total, result.candidates.size(), "candidate total exceeds size_t");
    }
    return total;
}

[[nodiscard]] size_t expected_chunk_count(std::span<const SieveResult> sieve_results,
                                          size_t chunk_size) {
    require(chunk_size > 0, "expected chunk size must be positive");
    size_t chunks = 0;
    for (const SieveResult& result : sieve_results) {
        const size_t count = result.candidates.size();
        const size_t per_special_q = count / chunk_size + (count % chunk_size != 0 ? 1 : 0);
        chunks = checked_add(chunks, per_special_q, "candidate chunk total exceeds size_t");
    }
    return chunks;
}

struct SweepCase final {
    uint32_t worker_cap = 1;
    size_t chunk_size = 1;
};

struct CaseMeasurements final {
    SweepCase sweep_case;
    size_t planned_chunks = 0;
    size_t workers_used = 0;
    std::vector<uint64_t> wall_ns;
};

[[nodiscard]] std::vector<SweepCase> make_sweep_cases() {
    std::vector<SweepCase> cases;
    cases.reserve(SWEEP_CASES);
    for (const uint32_t worker_cap : WORKER_CAPS) {
        for (const size_t chunk_size : CHUNK_SIZES) {
            cases.push_back(SweepCase{worker_cap, chunk_size});
        }
    }
    require(cases.size() == SWEEP_CASES, "candidate sweep case grid is incomplete");
    return cases;
}

[[nodiscard]] std::string case_label(const SweepCase& sweep_case, size_t repetition) {
    return "workers=" + std::to_string(sweep_case.worker_cap) +
           " chunk=" + std::to_string(sweep_case.chunk_size) +
           " repetition=" + std::to_string(repetition + 1);
}

void validate_candidate_batch_result(const CandidateBatchResult& result,
                                     const SweepCase& sweep_case, size_t repetition,
                                     size_t expected_candidates, size_t expected_chunks,
                                     const std::vector<std::vector<Relation>>& oracle,
                                     const CorpusDigest& oracle_digest) {
    const std::string label = case_label(sweep_case, repetition);
    require(result.total_candidates == expected_candidates,
            label + ": candidate total differs from fixed fixture");
    require(result.planned_chunks == expected_chunks,
            label + ": planned chunk count differs from the independent formula");
    require(result.workers_used == std::min<size_t>(sweep_case.worker_cap, expected_chunks),
            label + ": worker clamp differs");
    require_same_batches(result.relations_by_special_q, oracle, label);
    require(relation_digest(result.relations_by_special_q) == oracle_digest,
            label + ": relation digest differs from the serial oracle");
}

[[nodiscard]] uint64_t median_sample(std::vector<uint64_t> samples) {
    require(!samples.empty(), "cannot compute a median for no samples");
    std::sort(samples.begin(), samples.end());
    const size_t upper_index = samples.size() / 2;
    if (samples.size() % 2 != 0) {
        return samples[upper_index];
    }
    const uint64_t lower = samples[upper_index - 1];
    const uint64_t upper = samples[upper_index];
    return lower + (upper - lower) / 2;
}

[[nodiscard]] std::string join_sizes(std::span<const size_t> values) {
    std::string output;
    for (size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        output += std::to_string(values[index]);
    }
    return output;
}

[[nodiscard]] std::string join_u32(std::span<const uint32_t> values) {
    std::string output;
    for (size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        output += std::to_string(values[index]);
    }
    return output;
}

void emit_case_record(const CaseMeasurements& measurement, size_t repetitions,
                      size_t candidate_count, size_t oracle_relations,
                      const CandidateFixtureDigest& fixture_digest,
                      const CorpusDigest& oracle_digest, const SieveRunIdentity& run_identity) {
    require(measurement.wall_ns.size() == repetitions,
            "candidate sweep case has an incomplete timing sample set");
    const auto [minimum, maximum] =
        std::minmax_element(measurement.wall_ns.begin(), measurement.wall_ns.end());
    std::cout << "GNFS_CANDIDATE_SWEEP_CASE_V1"
              << " status=pass"
              << " scope=fixed_50d_first_production_batch"
              << " timing_scope=verify_candidate_batch_only"
              << " timing_asserted=false"
              << " worker_cap=" << measurement.sweep_case.worker_cap
              << " chunk_size=" << measurement.sweep_case.chunk_size
              << " planned_chunks=" << measurement.planned_chunks
              << " workers_used=" << measurement.workers_used << " candidates=" << candidate_count
              << " relations=" << oracle_relations << " repetitions=" << repetitions
              << " wall_min_ns=" << *minimum
              << " wall_median_ns=" << median_sample(measurement.wall_ns)
              << " wall_max_ns=" << *maximum
              << " run_fingerprint_low=" << run_identity.fingerprint_lo
              << " run_fingerprint_high=" << run_identity.fingerprint_hi
              << " candidate_digest_low=" << fixture_digest.low
              << " candidate_digest_high=" << fixture_digest.high
              << " relation_digest_low=" << oracle_digest.low
              << " relation_digest_high=" << oracle_digest.high << '\n';
}

void emit_summary_record(const FixedCandidateFixture& fixture, size_t repetitions,
                         size_t candidate_count, size_t oracle_relations,
                         const CandidateFixtureDigest& fixture_digest,
                         const CorpusDigest& oracle_digest,
                         const std::vector<std::vector<Relation>>& oracle) {
    std::vector<size_t> relations_per_special_q;
    relations_per_special_q.reserve(oracle.size());
    for (const auto& relations : oracle) {
        relations_per_special_q.push_back(relations.size());
    }

    std::cout << "GNFS_CANDIDATE_SWEEP_SUMMARY_V1"
              << " status=pass"
              << " scope=fixed_50d_first_production_batch"
              << " claim_boundary=candidate_batch_scheduler_only"
              << " timing_scope=verify_candidate_batch_only"
              << " timing_asserted=false"
              << " n_digits=" << PROBE_DIGITS << " n_bits=" << PROBE_BITS
              << " special_q_count=" << fixture.sieve_results.size()
              << " candidates=" << candidate_count << " relations=" << oracle_relations
              << " relations_per_special_q=" << join_sizes(relations_per_special_q)
              << " cases=" << SWEEP_CASES << " repetitions=" << repetitions
              << " worker_caps=" << join_u32(WORKER_CAPS)
              << " chunk_sizes=" << join_sizes(CHUNK_SIZES)
              << " local_sieve_thread_budget=" << fixture.params.max_local_sieve_threads
              << " generation_worker_limit=" << fixture.generation_worker_limit
              << " generation_workers=" << fixture.generation_thread_plan.threads_per_worker.size()
              << " generation_lane_plan="
              << join_sizes(fixture.generation_thread_plan.threads_per_worker)
              << " generation_assigned_threads=" << fixture.generation_thread_plan.assigned_threads
              << " generation_peak_worker_threads="
              << fixture.generation_thread_plan.peak_worker_threads
              << " generation_wall_ns=" << fixture.generation_wall_ns
              << " run_fingerprint_low=" << fixture.run_identity.fingerprint_lo
              << " run_fingerprint_high=" << fixture.run_identity.fingerprint_hi
              << " candidate_digest_low=" << fixture_digest.low
              << " candidate_digest_high=" << fixture_digest.high
              << " relation_digest_low=" << oracle_digest.low
              << " relation_digest_high=" << oracle_digest.high << '\n';
}

void run_sweep(size_t repetitions, std::string& failure_stage) {
    failure_stage = "environment";
    ScopedEnvironment environment;
    freeze_probe_environment(environment);

    failure_stage = "candidate_generation";
    FixedCandidateFixture fixture = make_fixed_candidate_fixture();
    const CandidateFixtureDigest fixture_digest = candidate_fixture_digest(fixture.sieve_results);
    const size_t candidate_count = total_candidates(fixture.sieve_results);

    failure_stage = "serial_oracle";
    const auto oracle = serial_oracle(fixture.context, fixture.factor_base, fixture.cofactor_config,
                                      fixture.sieve_results);
    require(oracle.size() == fixture.sieve_results.size(),
            "serial oracle changed the special-Q batch shape");
    const size_t oracle_relations = relation_count(oracle);
    require(oracle_relations > 0, "fixed candidate fixture produced no verified relations");
    const CorpusDigest oracle_digest = relation_digest(oracle);

    failure_stage = "candidate_sweep";
    const std::vector<SweepCase> cases = make_sweep_cases();
    std::vector<CaseMeasurements> measurements;
    measurements.reserve(cases.size());
    for (const SweepCase& sweep_case : cases) {
        measurements.push_back(CaseMeasurements{
            .sweep_case = sweep_case,
            .planned_chunks = expected_chunk_count(fixture.sieve_results, sweep_case.chunk_size),
        });
        require(measurements.back().planned_chunks > 0,
                "nonempty candidate fixture planned no chunks");
        measurements.back().wall_ns.reserve(repetitions);
    }

    for (size_t repetition = 0; repetition < repetitions; ++repetition) {
        const size_t start = (repetition * CASE_ROTATION_STRIDE) % cases.size();
        for (size_t offset = 0; offset < cases.size(); ++offset) {
            const size_t case_index = (start + offset) % cases.size();
            CaseMeasurements& measurement = measurements[case_index];
            CandidateBatchOptions options;
            options.max_candidates_per_chunk = measurement.sweep_case.chunk_size;
            options.max_workers = measurement.sweep_case.worker_cap;

            const auto started = std::chrono::steady_clock::now();
            CandidateBatchResult result =
                verify_candidate_batch(fixture.context, fixture.factor_base,
                                       fixture.cofactor_config, fixture.sieve_results, options);
            const auto finished = std::chrono::steady_clock::now();
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count();
            require(elapsed >= 0, "candidate batch clock moved backwards");

            validate_candidate_batch_result(result, measurement.sweep_case, repetition,
                                            candidate_count, measurement.planned_chunks, oracle,
                                            oracle_digest);
            require(candidate_fixture_digest(fixture.sieve_results) == fixture_digest,
                    case_label(measurement.sweep_case, repetition) +
                        ": CandidateBatch mutated the fixed sieve-result fixture");

            const size_t observed_workers = result.workers_used;
            if (repetition == 0) {
                measurement.workers_used = observed_workers;
            } else {
                require(measurement.workers_used == observed_workers,
                        case_label(measurement.sweep_case, repetition) +
                            ": worker topology changed across repetitions");
            }
            measurement.wall_ns.push_back(static_cast<uint64_t>(elapsed));
        }
    }

    require(candidate_fixture_digest(fixture.sieve_results) == fixture_digest,
            "candidate fixture digest changed after the sweep");
    for (const CaseMeasurements& measurement : measurements) {
        emit_case_record(measurement, repetitions, candidate_count, oracle_relations,
                         fixture_digest, oracle_digest, fixture.run_identity);
    }
    emit_summary_record(fixture, repetitions, candidate_count, oracle_relations, fixture_digest,
                        oracle_digest, oracle);
    failure_stage = "none";
}

struct CliOptions final {
    size_t repetitions = DEFAULT_REPETITIONS;
    bool help = false;
};

[[nodiscard]] size_t parse_repetitions(std::string_view text) {
    uint64_t parsed = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [position, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || position != end || parsed < MIN_REPETITIONS ||
        parsed > MAX_REPETITIONS) {
        throw std::invalid_argument("repetitions must be an integer in [1,9]");
    }
    return static_cast<size_t>(parsed);
}

[[nodiscard]] CliOptions parse_cli(int argc, char** argv) {
    if (argc == 1) {
        return {};
    }
    if (argc == 2) {
        const std::string_view argument(argv[1]);
        if (argument == "--help") {
            return CliOptions{.help = true};
        }
        return CliOptions{.repetitions = parse_repetitions(argument)};
    }
    throw std::invalid_argument("usage: test_candidate_batch_50d_sweep [repetitions|--help]");
}

[[nodiscard]] std::string sanitize_token(std::string_view input) {
    std::string output;
    output.reserve(std::min<size_t>(input.size(), 200));
    for (const char character : input) {
        const auto byte = static_cast<unsigned char>(character);
        const bool safe = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                          (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' || byte == '.';
        output.push_back(safe ? static_cast<char>(byte) : '_');
        if (output.size() == 200) {
            break;
        }
    }
    return output.empty() ? "unknown" : output;
}

} // namespace

int main(int argc, char** argv) {
    std::string failure_stage = "cli";
    try {
        const CliOptions options = parse_cli(argc, argv);
        if (options.help) {
            std::cout << "Usage: test_candidate_batch_50d_sweep [repetitions]\n"
                         "  repetitions: integer in [1,9], default 3\n";
            return 0;
        }
        run_sweep(options.repetitions, failure_stage);
        return 0;
    } catch (const std::exception& error) {
        std::cout << "GNFS_CANDIDATE_SWEEP_SUMMARY_V1"
                  << " status=fail"
                  << " scope=fixed_50d_first_production_batch"
                  << " failure_stage=" << sanitize_token(failure_stage)
                  << " error=" << sanitize_token(error.what()) << '\n';
        return 1;
    } catch (...) {
        std::cout << "GNFS_CANDIDATE_SWEEP_SUMMARY_V1"
                  << " status=fail"
                  << " scope=fixed_50d_first_production_batch"
                  << " failure_stage=" << sanitize_token(failure_stage)
                  << " error=unknown_exception\n";
        return 1;
    }
}

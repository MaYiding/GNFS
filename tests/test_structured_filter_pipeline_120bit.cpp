#include "gnfs/api/pipeline.hpp"
#include "gnfs/relation/reduction_engine.hpp"
#include "gnfs/relation/structured_filter_profile.hpp"
#include "gnfs/util/msvc_compat.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using gnfs::api::Config;
using gnfs::api::FactorizationMethod;
using gnfs::api::Pipeline;
using gnfs::core::Integer;
using gnfs::core::PolynomialContext;
using gnfs::factor_base::FactorBase;
using gnfs::relation::CorpusDigest;
using gnfs::relation::LpKeyWeightHistogram;
using gnfs::relation::ReductionStrategy;
using gnfs::relation::RelationStorageKind;

constexpr std::string_view FIXTURE_N = "664613997892503507403755373348813853";
constexpr size_t FIXTURE_BITS = 120;
constexpr size_t EXPECTED_PARAM_DIGITS = 37;
constexpr size_t MAX_SPECIAL_Q = 32;
constexpr uint32_t MAX_BATCH_WORKERS = 4;
constexpr size_t EXPECTED_RAW_ROWS = 9'170;
constexpr CorpusDigest EXPECTED_RAW_DIGEST{
    16'200'879'394'137'992'316ULL,
    17'871'977'238'653'261'677ULL,
};
constexpr LpKeyWeightHistogram EXPECTED_RAW_LP_HISTOGRAM{
    .weight_1 = 13'048,
    .weight_2 = 370,
    .weight_3 = 29,
    .weight_4plus = 32,
    .unique_keys = 13'479,
};
constexpr size_t EXPECTED_LEGACY_OUTPUT_ROWS = 248;
constexpr size_t EXPECTED_LEGACY_OUTPUT_LP_COLUMNS = 18;
constexpr CorpusDigest EXPECTED_LEGACY_OUTPUT_DIGEST{
    10'700'067'927'127'482'413ULL,
    7'933'828'173'714'541'669ULL,
};
constexpr size_t EXPECTED_STRUCTURED_OUTPUT_ROWS = 477;
constexpr size_t EXPECTED_STRUCTURED_OUTPUT_LP_COLUMNS = 23;
constexpr CorpusDigest EXPECTED_STRUCTURED_OUTPUT_DIGEST{
    16'984'277'476'308'836'056ULL,
    7'231'745'490'714'097'264ULL,
};
constexpr size_t EXPECTED_STRUCTURED_COMMITS = 45;
constexpr size_t EXPECTED_STRUCTURED_EMITTED_ROWS = 79;
constexpr size_t EXPECTED_LEGACY_MATRIX_COLUMNS = 14'648;
constexpr size_t EXPECTED_LEGACY_MATRIX_NONZEROS = 15'585;
constexpr CorpusDigest EXPECTED_LEGACY_MATRIX_DIGEST{
    14'525'310'064'104'378'093ULL,
    16'319'658'707'909'074'699ULL,
};
constexpr size_t EXPECTED_STRUCTURED_MATRIX_COLUMNS = 14'653;
constexpr size_t EXPECTED_STRUCTURED_MATRIX_NONZEROS = 25'678;
constexpr CorpusDigest EXPECTED_STRUCTURED_MATRIX_DIGEST{
    14'532'202'369'606'426'594ULL,
    8'411'150'515'085'501'241ULL,
};

[[nodiscard]] PolynomialContext make_fixture_context(const Integer& n) {
    std::vector<Integer> coefficients;
    coefficients.emplace_back("61547052323");
    coefficients.emplace_back("344271107786");
    coefficients.emplace_back(static_cast<int64_t>(109));
    coefficients.emplace_back(static_cast<int64_t>(1));
    constexpr uint64_t SKEW_BITS = 4'660'900'664'167'253'969ULL;
    return PolynomialContext(n, std::move(coefficients), Integer("872682957255"),
                             std::bit_cast<double>(SKEW_BITS));
}

[[noreturn]] void fail(std::string_view message) {
    throw std::runtime_error(std::string(message));
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

class ScopedEnvironmentVariable final {
public:
    ScopedEnvironmentVariable(std::string name, const std::string& value) : name_(std::move(name)) {
        if (const char* previous = std::getenv(name_.c_str()); previous != nullptr) {
            previous_ = previous;
        }
        if (setenv(name_.c_str(), value.c_str(), 1) != 0) {
            throw std::runtime_error("could not set environment variable " + name_);
        }
    }

    ScopedEnvironmentVariable(std::string name, std::nullopt_t) : name_(std::move(name)) {
        if (const char* previous = std::getenv(name_.c_str()); previous != nullptr) {
            previous_ = previous;
        }
        if (unsetenv(name_.c_str()) != 0) {
            throw std::runtime_error("could not unset environment variable " + name_);
        }
    }

    ~ScopedEnvironmentVariable() {
        if (previous_.has_value()) {
            (void)setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            (void)unsetenv(name_.c_str());
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
    std::string name_;
    std::optional<std::string> previous_;
};

struct RunEvidence final {
    ReductionStrategy strategy = ReductionStrategy::NoLargePrimes;
    RelationStorageKind storage = RelationStorageKind::InMemory;
    size_t input_rows = 0;
    size_t raw_duplicates = 0;
    CorpusDigest raw_digest{};
    LpKeyWeightHistogram raw_lp_histogram{};
    size_t output_rows = 0;
    size_t output_lp_columns = 0;
    CorpusDigest output_digest{};
    size_t singleton_rows_removed = 0;
    size_t structured_commits = 0;
    size_t structured_emitted_rows = 0;
    uint32_t structured_requested_workers = 0;
    uint32_t structured_peak_workers = 0;
    size_t special_q_processed = 0;
    size_t special_q_peak_workers = 0;
    size_t local_sieve_threads = 0;
    size_t candidates = 0;
    size_t matrix_rows = 0;
    size_t matrix_columns = 0;
    size_t matrix_nonzeros = 0;
    CorpusDigest matrix_digest{};
    bool owns_structured_corpus = false;
    bool structured_row_mapping_is_identity = false;
};

[[nodiscard]] CorpusDigest matrix_digest(const gnfs::linalg::SparseMatrix& matrix) {
    gnfs::relation::detail::CorpusDigestBuilder builder;
    constexpr uint8_t domain[] = {'G', 'N', 'F', 'S', '-', 'M', 'D', 'G', 1};
    for (uint8_t byte : domain) {
        builder.append_byte(byte);
    }
    builder.append_u64_le(static_cast<uint64_t>(matrix.num_rows()));
    builder.append_u64_le(static_cast<uint64_t>(matrix.num_cols()));
    for (size_t row = 0; row < matrix.num_rows(); ++row) {
        const auto& indices = matrix.row(row).indices();
        builder.append_u64_le(static_cast<uint64_t>(row));
        builder.append_u64_le(static_cast<uint64_t>(indices.size()));
        for (uint32_t column : indices) {
            builder.append_u32_le(column);
        }
    }
    return builder.finish();
}

[[nodiscard]] Config make_config(uint32_t local_threads) {
    Config config;
    config.set_method(FactorizationMethod::GNFS)
        .set_max_special_q(MAX_SPECIAL_Q)
        .set_max_special_q_batch_workers(MAX_BATCH_WORKERS)
        .set_max_local_sieve_threads(local_threads)
        .set_verbose(false);
    return config;
}

[[nodiscard]] RunEvidence run_case(const Integer& n, const Config& config,
                                   const gnfs::core::PolynomialContext& context,
                                   const FactorBase& factor_base, std::string_view mode) {
    ScopedEnvironmentVariable structured_filter("GNFS_STRUCTURED_FILTER", std::string(mode));
    Pipeline pipeline(n, config);

    auto reduction = pipeline.sieve_and_collect(context, factor_base);
    const auto reduction_stats = reduction.stats;
    const auto pipeline_stats = pipeline.stats();

    RunEvidence evidence;
    evidence.strategy = reduction_stats.strategy;
    evidence.storage = reduction.storage_kind();
    evidence.input_rows = reduction_stats.input_relations;
    evidence.raw_duplicates = reduction_stats.raw_duplicates_removed;
    evidence.raw_digest = reduction_stats.raw_input_digest;
    evidence.raw_lp_histogram = reduction_stats.deduplicated_input_lp_histogram;
    evidence.output_rows = reduction_stats.output_relations;
    evidence.output_lp_columns = reduction_stats.output_lp_columns;
    evidence.output_digest = reduction_stats.output_digest;
    evidence.singleton_rows_removed = reduction_stats.singleton_rows_removed;
    evidence.structured_commits = reduction_stats.structured_run.commits;
    evidence.structured_emitted_rows = reduction_stats.structured_run.emitted_rows;
    evidence.structured_requested_workers =
        reduction_stats.structured_incidence.requested_worker_count;
    evidence.structured_peak_workers = reduction_stats.structured_incidence.peak_worker_count;
    evidence.special_q_processed = pipeline_stats.special_q_processed;
    evidence.special_q_peak_workers = pipeline_stats.special_q_batch_peak_workers;
    evidence.local_sieve_threads = pipeline_stats.local_sieve_thread_budget;
    evidence.candidates = pipeline_stats.candidates_total;

    require(evidence.input_rows > 0, "real 120-bit sieve produced no raw relations");
    require(evidence.raw_duplicates == 0,
            "real 120-bit route unexpectedly produced duplicate raw AB pairs");
    require(evidence.raw_lp_histogram.unique_keys > 0,
            "real 120-bit relation corpus exercised no large-prime columns");
    require(evidence.special_q_processed == MAX_SPECIAL_Q,
            "real 120-bit route stopped before the fixed special-Q bound");
    require(evidence.candidates > 0, "real 120-bit sieve produced no candidates");
    require(evidence.output_rows == reduction.size(),
            "reduction output count differs from its corpus size");

    const size_t base_columns = factor_base.rational_count() + factor_base.sieve_algebraic_count();
    require(reduction.size() <= base_columns,
            "fixed 120-bit prefix is no longer provably thin before matrix construction");

    const size_t expected_rows = reduction.size();
    auto matrix_result = pipeline.solve_matrix(std::move(reduction), factor_base, context);
    evidence.matrix_rows = matrix_result.matrix.num_rows();
    evidence.matrix_columns = matrix_result.matrix.num_cols();
    evidence.matrix_nonzeros = matrix_result.matrix.total_weight();
    evidence.matrix_digest = matrix_digest(matrix_result.matrix);
    evidence.owns_structured_corpus = matrix_result.owns_relation_corpus();
    evidence.structured_row_mapping_is_identity = evidence.owns_structured_corpus;
    for (size_t row = 0; row < matrix_result.structured_row_to_relation().size(); ++row) {
        evidence.structured_row_mapping_is_identity =
            evidence.structured_row_mapping_is_identity &&
            matrix_result.structured_row_to_relation()[row] == row;
    }

    require(matrix_result.dependencies.empty(),
            "thin-solver opt-out unexpectedly attempted dependency extraction");
    require(evidence.matrix_rows == expected_rows,
            "matrix construction changed the reduced relation count");
    require(evidence.matrix_columns >= base_columns,
            "matrix construction lost factor-base columns");
    require(evidence.matrix_rows < evidence.matrix_columns,
            "fixed 120-bit prefix no longer reaches the intended thin boundary");
    require(pipeline.stats().matrix_rows == evidence.matrix_rows &&
                pipeline.stats().matrix_cols == evidence.matrix_columns,
            "Pipeline matrix telemetry differs from the returned matrix");
    return evidence;
}

void validate_route_contract(const RunEvidence& legacy, const RunEvidence& structured,
                             uint32_t expected_structured_lanes) {
    require(legacy.strategy == ReductionStrategy::StandardV0,
            "legacy route did not select the frozen StandardV0 baseline");
    require(structured.strategy == ReductionStrategy::Structured,
            "forced structured route did not select structured reduction");
    require(legacy.storage == RelationStorageKind::InMemory &&
                structured.storage == RelationStorageKind::InMemory,
            "120-bit size transition left the explicit in-memory route");

    require(legacy.input_rows == structured.input_rows,
            "one-lane and multi-lane sieves produced different raw row counts");
    require(legacy.raw_digest == structured.raw_digest,
            "one-lane and multi-lane sieves produced different raw corpora");
    require(legacy.raw_lp_histogram == structured.raw_lp_histogram,
            "one-lane and multi-lane sieves produced different LP histograms");
    require(legacy.candidates == structured.candidates,
            "one-lane and multi-lane sieves produced different candidate counts");

    require(legacy.local_sieve_threads == 1 && legacy.special_q_peak_workers == 1,
            "legacy oracle did not exercise the one-lane schedule");
    require(structured.local_sieve_threads == expected_structured_lanes,
            "structured route did not freeze the requested local compute budget");
    require(structured.special_q_peak_workers == expected_structured_lanes,
            "structured route did not exercise the bounded multi-worker schedule");
    require(structured.structured_requested_workers == expected_structured_lanes,
            "structured reducer worker request differs from the hardware-bounded profile");
    require(structured.structured_peak_workers > 0,
            "nonempty structured incidence build used no worker");

    require(!legacy.owns_structured_corpus && legacy.structured_row_mapping_is_identity == false,
            "legacy matrix unexpectedly retained a structured corpus");
    require(structured.owns_structured_corpus && structured.structured_row_mapping_is_identity,
            "structured matrix lost corpus ownership or canonical row mapping");
    require(structured.structured_commits > 0 && structured.structured_emitted_rows > 0,
            "real 120-bit corpus did not exercise a structured commit");

    require(structured.input_rows == EXPECTED_RAW_ROWS &&
                structured.raw_digest == EXPECTED_RAW_DIGEST &&
                structured.raw_lp_histogram == EXPECTED_RAW_LP_HISTOGRAM,
            "fixed 120-bit raw corpus identity changed");
    require(legacy.output_rows == EXPECTED_LEGACY_OUTPUT_ROWS &&
                legacy.output_lp_columns == EXPECTED_LEGACY_OUTPUT_LP_COLUMNS &&
                legacy.output_digest == EXPECTED_LEGACY_OUTPUT_DIGEST &&
                legacy.matrix_rows == EXPECTED_LEGACY_OUTPUT_ROWS &&
                legacy.matrix_columns == EXPECTED_LEGACY_MATRIX_COLUMNS &&
                legacy.matrix_nonzeros == EXPECTED_LEGACY_MATRIX_NONZEROS &&
                legacy.matrix_digest == EXPECTED_LEGACY_MATRIX_DIGEST,
            "fixed 120-bit legacy reduction or matrix identity changed");
    require(structured.output_rows == EXPECTED_STRUCTURED_OUTPUT_ROWS &&
                structured.output_lp_columns == EXPECTED_STRUCTURED_OUTPUT_LP_COLUMNS &&
                structured.output_digest == EXPECTED_STRUCTURED_OUTPUT_DIGEST &&
                structured.structured_commits == EXPECTED_STRUCTURED_COMMITS &&
                structured.structured_emitted_rows == EXPECTED_STRUCTURED_EMITTED_ROWS &&
                structured.matrix_rows == EXPECTED_STRUCTURED_OUTPUT_ROWS &&
                structured.matrix_columns == EXPECTED_STRUCTURED_MATRIX_COLUMNS &&
                structured.matrix_nonzeros == EXPECTED_STRUCTURED_MATRIX_NONZEROS &&
                structured.matrix_digest == EXPECTED_STRUCTURED_MATRIX_DIGEST,
            "fixed 120-bit structured reduction or matrix identity changed");
}

void run_test() {
    ScopedEnvironmentVariable ordinary_ooc("GNFS_OOC_RELATIONS", "0");
    ScopedEnvironmentVariable ooc_base("GNFS_OOC_BASE_PATH", std::nullopt);
    ScopedEnvironmentVariable resume("GNFS_RESUME", std::nullopt);
    ScopedEnvironmentVariable sieve_resume("GNFS_SIEVE_RESUME", std::nullopt);
    ScopedEnvironmentVariable distributed_workers("GNFS_DISTRIBUTED_SIEVE_WORKERS", "0");
    ScopedEnvironmentVariable distributed_force_small("GNFS_DISTRIBUTED_SIEVE_FORCE_SMALL", "0");
    ScopedEnvironmentVariable adaptive_lattice("GNFS_ADAPTIVE_LATTICE", "0");
    ScopedEnvironmentVariable three_large_primes("GNFS_3LP", "0");
    ScopedEnvironmentVariable cascade_v3("GNFS_CASCADE_V3", "0");
    ScopedEnvironmentVariable v0_bfs("GNFS_V0_BFS", "0");
    ScopedEnvironmentVariable v0_weight3("GNFS_V0_WEIGHT3", "0");
    ScopedEnvironmentVariable weight_cutoff("GNFS_WEIGHT_CUTOFF", "0");
    ScopedEnvironmentVariable drop_residual("GNFS_DROP_RESIDUAL", "0");
    ScopedEnvironmentVariable override_lp_bits("GNFS_OVERRIDE_LP_BITS", std::nullopt);
    ScopedEnvironmentVariable target_multiplier("GNFS_SIEVE_TARGET_MULT", std::nullopt);
    ScopedEnvironmentVariable lattice_lll("GNFS_LATTICE_LLL", std::nullopt);
    ScopedEnvironmentVariable lattice_skew("GNFS_LATTICE_SKEW", "0");
    ScopedEnvironmentVariable thin_solver("GNFS_NO_THIN_SOLVE", "1");
    ScopedEnvironmentVariable streaming_matrix("GNFS_SGE_STREAMING", "off");
    ScopedEnvironmentVariable cofactor_brent("GNFS_COFACTOR_BRENT", "0");
    ScopedEnvironmentVariable ecm_brent_suyama("GNFS_ECM_BRENT_SUYAMA", "0");
    ScopedEnvironmentVariable ecm_bs_degree("GNFS_ECM_BS_DEGREE", std::nullopt);
    ScopedEnvironmentVariable ecm_stage1_threads("GNFS_ECM_STAGE1_PARALLEL_THREADS", "1");
    ScopedEnvironmentVariable ecm_stage2_threads("GNFS_ECM_STAGE2_PARALLEL", "1");
    ScopedEnvironmentVariable rho_threads("GNFS_BRENT_POLLARD_RHO_THREADS", "1");
    ScopedEnvironmentVariable ecm_curve_pool("GNFS_ECM_CURVE_POOL", "0");
    ScopedEnvironmentVariable cofactor_batch_size("GNFS_COFACTOR_BATCH_SIZE", "1");
    ScopedEnvironmentVariable ecm_sigma_pool("GNFS_ECM_SIGMA_POOL_SIZE", "0");
    ScopedEnvironmentVariable ecm_b1_cache("GNFS_ECM_B1_CACHE_SIZE", "0");
    ScopedEnvironmentVariable cofactor_result_cache("GNFS_COFACTOR_RESULT_CACHE_SIZE", "0");
    ScopedEnvironmentVariable ecm_batch_inverse("GNFS_ECM_BATCH_INV", "0");
    ScopedEnvironmentVariable survival_filter("GNFS_SURVIVAL_FILTER", "0");
    ScopedEnvironmentVariable survival_threshold("GNFS_SURVIVAL_THRESHOLD", std::nullopt);
    ScopedEnvironmentVariable cofactor_timing("GNFS_COFACTOR_TIMING_ENABLE", "0");
    ScopedEnvironmentVariable trial_division_simd("GNFS_TRIAL_DIV_SIMD", "auto");

    const Integer n{std::string(FIXTURE_N)};
    require(n.bit_length() == FIXTURE_BITS, "120-bit fixture bit length changed");

    const uint32_t structured_threads = gnfs::relation::structured_filter_hardware_workers();

    Pipeline setup_pipeline(n, make_config(structured_threads));
    require(setup_pipeline.params().bits == FIXTURE_BITS,
            "Pipeline parameter bit count changed for the 120-bit fixture");
    require(setup_pipeline.params().digits == EXPECTED_PARAM_DIGITS,
            "Pipeline parameter digit band changed for the 120-bit fixture");
    require(setup_pipeline.params().large_prime_bits == 22,
            "120-bit fixture left the intended LP size transition");
    auto context = make_fixture_context(n);
    auto factor_base = setup_pipeline.build_factor_base(context);

    const RunEvidence legacy = run_case(n, make_config(1), context, factor_base, "0");
    const RunEvidence structured =
        run_case(n, make_config(structured_threads), context, factor_base, "1");
    validate_route_contract(legacy, structured, structured_threads);

    std::cout << "GNFS_STRUCTURED_120BIT_V1"
              << " status=pass"
              << " n_bits=" << FIXTURE_BITS << " max_special_q=" << MAX_SPECIAL_Q
              << " raw_rows=" << structured.input_rows
              << " raw_digest_low=" << structured.raw_digest.low
              << " raw_digest_high=" << structured.raw_digest.high
              << " raw_lp_columns=" << structured.raw_lp_histogram.unique_keys
              << " legacy_output_rows=" << legacy.output_rows
              << " legacy_output_lp_columns=" << legacy.output_lp_columns
              << " legacy_output_digest_low=" << legacy.output_digest.low
              << " legacy_output_digest_high=" << legacy.output_digest.high
              << " structured_output_rows=" << structured.output_rows
              << " structured_output_lp_columns=" << structured.output_lp_columns
              << " structured_output_digest_low=" << structured.output_digest.low
              << " structured_output_digest_high=" << structured.output_digest.high
              << " structured_commits=" << structured.structured_commits
              << " structured_emitted_rows=" << structured.structured_emitted_rows
              << " legacy_matrix_rows=" << legacy.matrix_rows
              << " legacy_matrix_columns=" << legacy.matrix_columns
              << " legacy_matrix_nonzeros=" << legacy.matrix_nonzeros
              << " legacy_matrix_digest_low=" << legacy.matrix_digest.low
              << " legacy_matrix_digest_high=" << legacy.matrix_digest.high
              << " structured_matrix_rows=" << structured.matrix_rows
              << " structured_matrix_columns=" << structured.matrix_columns
              << " structured_matrix_nonzeros=" << structured.matrix_nonzeros
              << " structured_matrix_digest_low=" << structured.matrix_digest.low
              << " structured_matrix_digest_high=" << structured.matrix_digest.high
              << " legacy_sieve_threads=" << legacy.local_sieve_threads
              << " structured_sieve_threads=" << structured.local_sieve_threads
              << " structured_incidence_peak_workers=" << structured.structured_peak_workers
              << " timing_asserted=false\n";
}

} // namespace

int main() {
    try {
        run_test();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GNFS_STRUCTURED_120BIT_V1 status=fail error=" << error.what() << '\n';
        return 1;
    }
}

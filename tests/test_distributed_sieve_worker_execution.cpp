#include <gnfs/core/integer.hpp>
#include <gnfs/core/polynomial_context.hpp>
#include <gnfs/core/relation.hpp>
#include <gnfs/factor_base/builder.hpp>
#include <gnfs/factor_base/factor_base.hpp>
#include <gnfs/relation/relation_corpus_sha256.hpp>
#include <gnfs/relation/relation_sequence_receipt.hpp>
#include <gnfs/sieve/distributed_sieve_protocol.hpp>

#include "distributed_sieve_bound_work_internal.hpp"
#include "distributed_sieve_execution_policy_internal.hpp"
#include "distributed_sieve_worker_chunk_internal.hpp"
#include "distributed_sieve_worker_execution_internal.hpp"
#include "distributed_sieve_worker_runtime_internal.hpp"

#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace execution = gnfs::sieve::distributed_sieve_worker_execution_detail;
namespace policy = gnfs::sieve::distributed_sieve_execution_policy_detail;
namespace sieve = gnfs::sieve;

using gnfs::core::Integer;
using gnfs::core::PolynomialContext;
using gnfs::core::Relation;
using gnfs::factor_base::FactorBase;
using gnfs::factor_base::FactorBaseBuilder;

#if defined(__APPLE__)
inline constexpr char EXECUTABLE_DIGEST_CHILD_ARGUMENT[] = "--worker-executable-digest-child";
#endif

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string_view expression, int line, std::string_view detail = {}) {
    std::string message = "CHECK failed at line " + std::to_string(line) + ": ";
    message.append(expression);
    if (!detail.empty()) {
        message.append(" (");
        message.append(detail);
        message.push_back(')');
    }
    throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view expression, int line) {
    if (!condition) {
        fail(expression, line);
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

[[nodiscard]] constexpr std::size_t policy_index(sieve::ExecutionPolicyKeyV1 key) noexcept {
    return static_cast<std::size_t>(static_cast<std::uint16_t>(key) - 1U);
}

[[nodiscard]] policy::DistributedSieveFrozenExecutionPolicyV1 frozen_policy() {
    policy::DistributedSieveExecutionPolicyEnvironmentSnapshotV1 snapshot;
    snapshot.hardware_concurrency = 8;
    snapshot.canonical_values[policy_index(sieve::ExecutionPolicyKeyV1::cofactor_brent)] = "1";
    auto frozen = policy::freeze_distributed_sieve_execution_policy_v1(snapshot);
    CHECK(frozen);
    return std::move(*frozen.policy);
}

[[nodiscard]] PolynomialContext seeded_polynomial() {
    Integer input("93185905945582757");
    input *= 15;
    input += 1;

    std::vector<Integer> coefficients;
    coefficients.emplace_back(input);
    coefficients.back().negate();
    coefficients.emplace_back(1);
    return PolynomialContext(Integer(3), std::move(coefficients), std::move(input));
}

[[nodiscard]] FactorBase seeded_factor_base(const PolynomialContext& polynomial) {
    FactorBaseBuilder::Options options;
    options.rational_bound = 100;
    options.algebraic_bound = 100;
    options.special_q_bound = 200;
    options.log_scale = 16;
    options.parallel = false;
    return FactorBaseBuilder::build(polynomial, options);
}

[[nodiscard]] sieve::DistributedSieveWorkIdentityV1
make_identity(const PolynomialContext& polynomial, const FactorBase& factor_base,
              const policy::DistributedSieveFrozenExecutionPolicyV1& frozen, std::uint32_t sq_begin,
              std::uint32_t sq_end, std::uint64_t sq_cap = 0, std::uint64_t relation_cap = 0) {
    sieve::DistributedSieveWorkIdentityV1 identity;
    identity.polynomial.n.decimal = polynomial.n().to_string();
    identity.polynomial.m.decimal = polynomial.m().to_string();
    identity.polynomial.degree = polynomial.degree();
    for (const auto& coefficient : polynomial.coefficients()) {
        identity.polynomial.coefficients.push_back({coefficient.to_string()});
    }
    identity.polynomial.skewness_ieee754_bits = std::bit_cast<std::uint64_t>(polynomial.skewness());

    const auto& parameters = factor_base.params();
    identity.factor_base.rational_bound = parameters.rational_bound;
    identity.factor_base.algebraic_bound = parameters.algebraic_bound;
    identity.factor_base.large_prime_bound = parameters.large_prime_bound;
    identity.factor_base.log_scale = parameters.log_scale;
    for (const auto& prime : factor_base.rational()) {
        identity.factor_base.rational.push_back({prime.p, prime.log_p});
    }
    for (const auto& prime : factor_base.algebraic()) {
        identity.factor_base.algebraic.push_back({prime.p, prime.r, prime.log_p, prime.degree});
    }
    identity.factor_base.sieve_algebraic_count = factor_base.sieve_algebraic_count();

    identity.sieve = {
        .log_scale = 16,
        .rational_threshold = 320,
        .algebraic_threshold = 320,
        .large_prime_bound = 0,
        .allow_2lp = true,
        .allow_3lp = false,
    };
    identity.region = {
        .i_min = -100,
        .i_max = 100,
        .j_min = 1,
        .j_max = 50,
    };
    identity.cofactor = {
        .large_prime_bound = 500'000'000,
        .allow_1lp = true,
        .allow_2lp = true,
        .allow_3lp = false,
        .max_factorization_attempts = 50'000,
    };
    identity.original_sq_bounds = {
        .start_index = sq_begin,
        .end_index = sq_end,
        .min_q = 0,
        .max_q = std::numeric_limits<std::uint32_t>::max(),
    };
    identity.effective_sq_bounds = identity.original_sq_bounds;
    identity.distributed.worker_count = 1;
    identity.distributed.chunks = {{0, sq_begin, sq_end, "worker_execution_chunk_0"}};
    identity.distributed.sq_cap_per_worker = sq_cap;
    identity.distributed.relation_cap_per_worker = relation_cap;
    identity.distributed.max_worker_attempts = 2;
    identity.distributed.max_merge_build_attempts = 2;
    identity.distributed.max_consumption_attempts = 2;
    identity.execution_policy = frozen.canonical;
    identity.semantic_versions = policy::DISTRIBUTED_SIEVE_BOUND_WORK_VERSIONS_V1;
    CHECK(sieve::validate_distributed_sieve_work_identity(identity));
    return identity;
}

struct VectorSink final {
    std::vector<Relation> relations;
    std::optional<std::size_t> fail_at;
};

[[nodiscard]] bool append_vector(void* raw_context, const Relation& relation) noexcept {
    auto* sink = static_cast<VectorSink*>(raw_context);
    if (sink == nullptr ||
        (sink->fail_at.has_value() && sink->relations.size() >= *sink->fail_at)) {
        return false;
    }
    try {
        sink->relations.push_back(relation);
        return true;
    } catch (...) {
        return false;
    }
}

struct ChunkRun final {
    execution::DistributedSieveWorkerChunkExecutionResultV1 result;
    std::vector<Relation> relations;
};

[[nodiscard]] ChunkRun run_identity(const sieve::DistributedSieveWorkIdentityV1& identity,
                                    std::optional<std::size_t> fail_at = std::nullopt) {
    auto runtime = execution::rehydrate_distributed_sieve_worker_runtime_v1(identity);
    CHECK(runtime);
    CHECK(runtime.runtime.has_value());
    CHECK(runtime.runtime->polynomial.n().to_string() == identity.polynomial.n.decimal);
    CHECK(runtime.runtime->factor_base.rational_count() == identity.factor_base.rational.size());
    CHECK(runtime.runtime->factor_base.algebraic_count() == identity.factor_base.algebraic.size());

    auto prepared = execution::prepare_distributed_sieve_worker_chunk_v1(
        runtime.runtime->polynomial, runtime.runtime->factor_base, runtime.runtime->bound_work,
        identity.distributed.chunks.front());
    CHECK(prepared);

    VectorSink sink{.fail_at = fail_at};
    auto result = prepared.prepared->execute({.context = &sink, .append = append_vector});
    const auto second = prepared.prepared->execute({.context = &sink, .append = append_vector});
    CHECK(second.status == execution::DistributedSieveWorkerChunkStatusV1::already_executed);
    return {std::move(result), std::move(sink.relations)};
}

void test_runtime_rehydration_and_real_chunk_determinism() {
    std::cout << "[runtime + real deterministic chunk] ... " << std::flush;
    auto polynomial = seeded_polynomial();
    auto factor_base = seeded_factor_base(polynomial);
    const auto frozen = frozen_policy();
    const auto identity = make_identity(polynomial, factor_base, frozen, 1, 4);

    const auto first = run_identity(identity);
    const auto second = run_identity(identity);
    CHECK(first.result);
    CHECK(second.result);
    CHECK(first.result.accepted_relation_count == first.relations.size());
    CHECK(!first.relations.empty());
    CHECK(first.result.completion->processed_sq_count == 3);
    CHECK(first.result.completion->next_sq_index == 4);
    CHECK(first.result.completion->completion_reason ==
          sieve::WorkerCompletionReasonV1::range_exhausted);
    CHECK(gnfs::relation::relation_sequence_receipt(first.relations) ==
          gnfs::relation::relation_sequence_receipt(second.relations));
    CHECK(gnfs::relation::relation_corpus_sha256_v1(std::span<const Relation>(first.relations)) ==
          gnfs::relation::relation_corpus_sha256_v1(std::span<const Relation>(second.relations)));
    std::cout << "PASS (" << first.relations.size() << " relations)\n";
}

void test_sq_and_relation_cap_boundary_priority() {
    std::cout << "[SQ/relation cap boundary priority] ... " << std::flush;
    auto polynomial = seeded_polynomial();
    auto factor_base = seeded_factor_base(polynomial);
    const auto frozen = frozen_policy();

    const auto sq_capped = run_identity(make_identity(polynomial, factor_base, frozen, 1, 4, 1, 0));
    CHECK(sq_capped.result);
    CHECK(sq_capped.result.completion->processed_sq_count == 1);
    CHECK(sq_capped.result.completion->next_sq_index == 2);
    CHECK(sq_capped.result.completion->completion_reason ==
          sieve::WorkerCompletionReasonV1::sq_cap);

    const auto relation_capped =
        run_identity(make_identity(polynomial, factor_base, frozen, 1, 4, 0, 1));
    CHECK(relation_capped.result);
    CHECK(relation_capped.result.accepted_relation_count >= 1);
    CHECK(relation_capped.result.completion->completion_reason ==
          sieve::WorkerCompletionReasonV1::relation_cap);
    CHECK(relation_capped.result.completion->next_sq_index < 4);

    const auto both = run_identity(make_identity(polynomial, factor_base, frozen, 1, 4, 1, 1));
    CHECK(both.result);
    CHECK(both.result.accepted_relation_count >= 1);
    CHECK(both.result.completion->completion_reason == sieve::WorkerCompletionReasonV1::sq_cap);
    std::cout << "PASS\n";
}

void test_sink_failure_is_terminal_and_preserves_residue_signal() {
    std::cout << "[sink failure residue signal] ... " << std::flush;
    auto polynomial = seeded_polynomial();
    auto factor_base = seeded_factor_base(polynomial);
    const auto frozen = frozen_policy();
    const auto failed = run_identity(make_identity(polynomial, factor_base, frozen, 1, 4), 0);
    CHECK(!failed.result);
    CHECK(failed.result.status == execution::DistributedSieveWorkerChunkStatusV1::sink_failed);
    CHECK(failed.result.artifacts_may_remain);
    CHECK(failed.result.accepted_relation_count == 0);
    CHECK(failed.relations.empty());
    std::cout << "PASS\n";
}

void test_admission_rejects_invalid_and_duplicate_relations() {
    std::cout << "[relation admission invariants] ... " << std::flush;
    auto polynomial = seeded_polynomial();
    auto factor_base = seeded_factor_base(polynomial);
    const auto frozen = frozen_policy();
    const auto identity = make_identity(polynomial, factor_base, frozen, 1, 2);
    auto runtime = execution::rehydrate_distributed_sieve_worker_runtime_v1(identity);
    CHECK(runtime);

    execution::DistributedSieveWorkerRelationAdmissionV1 admission(runtime.runtime->polynomial);
    VectorSink sink;
    const execution::DistributedSieveWorkerRelationSinkV1 relation_sink{
        .context = &sink,
        .append = append_vector,
    };

    const Relation accepted(2, 1);
    CHECK(admission.admit(accepted, relation_sink) ==
          execution::DistributedSieveWorkerAdmissionStatusV1::accepted);
    CHECK(admission.admit(accepted, relation_sink) ==
          execution::DistributedSieveWorkerAdmissionStatusV1::duplicate);
    CHECK(admission.admit(Relation(2, 0), relation_sink) ==
          execution::DistributedSieveWorkerAdmissionStatusV1::invalid);
    CHECK(admission.admit(Relation(4, 2), relation_sink) ==
          execution::DistributedSieveWorkerAdmissionStatusV1::invalid);
    // m == 1 (mod 3), so a=1,b=1 makes a-bm divisible by N=3.
    CHECK(admission.admit(Relation(1, 1), relation_sink) ==
          execution::DistributedSieveWorkerAdmissionStatusV1::invalid);

    sink.fail_at = sink.relations.size();
    const Relation retry_after_sink_failure(3, 1);
    CHECK(admission.admit(retry_after_sink_failure, relation_sink) ==
          execution::DistributedSieveWorkerAdmissionStatusV1::sink_failed);
    sink.fail_at.reset();
    CHECK(admission.admit(retry_after_sink_failure, relation_sink) ==
          execution::DistributedSieveWorkerAdmissionStatusV1::accepted);
    CHECK(sink.relations.size() == 2);
    std::cout << "PASS\n";
}

void test_trailing_projective_special_q_normalizes_cursor() {
    std::cout << "[trailing projective cursor normalization] ... " << std::flush;
    auto polynomial = seeded_polynomial();
    FactorBase factor_base({100, 100, 10'000, 16});
    factor_base.add_rational(2, 16);
    factor_base.add_algebraic(5, 1, 32, 1);
    factor_base.add_algebraic(7, gnfs::core::AlgebraicPrime::PROJECTIVE_ROOT, 44, 1);
    factor_base.set_sieve_algebraic_count(2);
    factor_base.build_index();

    const auto frozen = frozen_policy();
    const auto completed = run_identity(make_identity(polynomial, factor_base, frozen, 0, 2));
    CHECK(completed.result);
    CHECK(completed.result.completion->processed_sq_count == 1);
    CHECK(completed.result.completion->next_sq_index == 2);
    CHECK(completed.result.completion->completion_reason ==
          (completed.relations.empty() ? sieve::WorkerCompletionReasonV1::zero_relations
                                       : sieve::WorkerCompletionReasonV1::range_exhausted));
    std::cout << "PASS\n";
}

void test_sq_cap_preserves_cursor_before_projective_hole() {
    std::cout << "[SQ cap cursor before projective hole] ... " << std::flush;
    auto polynomial = seeded_polynomial();
    FactorBase factor_base({100, 100, 10'000, 16});
    factor_base.add_rational(2, 16);
    factor_base.add_algebraic(5, 1, 32, 1);
    factor_base.add_algebraic(7, gnfs::core::AlgebraicPrime::PROJECTIVE_ROOT, 44, 1);
    factor_base.add_algebraic(11, 8, 55, 1);
    factor_base.set_sieve_algebraic_count(3);
    factor_base.build_index();

    const auto frozen = frozen_policy();
    const auto capped = run_identity(make_identity(polynomial, factor_base, frozen, 0, 3, 1));
    CHECK(capped.result);
    CHECK(capped.result.completion->processed_sq_count == 1);
    CHECK(capped.result.completion->next_sq_index == 1);
    CHECK(capped.result.completion->completion_reason == sieve::WorkerCompletionReasonV1::sq_cap);

    const auto resumed = run_identity(make_identity(polynomial, factor_base, frozen, 1, 3));
    CHECK(resumed.result);
    CHECK(resumed.result.completion->processed_sq_count == 1);
    CHECK(resumed.result.completion->next_sq_index == 3);
    std::cout << "PASS\n";
}

void test_current_executable_digest_is_stable(
    [[maybe_unused]] const std::filesystem::path& executable) {
    std::cout << "[current executable digest] ... " << std::flush;
    const auto first = execution::current_distributed_sieve_worker_executable_sha256_v1();
    const auto second = execution::current_distributed_sieve_worker_executable_sha256_v1();
#if defined(__APPLE__) || defined(__linux__)
    CHECK(first);
    CHECK(second);
    CHECK(first.digest == second.digest);
#if defined(__APPLE__)
    char directory_template[] = "/tmp/gnfs-worker-exec-symlink-XXXXXX";
    const char* raw_directory = ::mkdtemp(directory_template);
    CHECK(raw_directory != nullptr);
    const std::filesystem::path directory(raw_directory);
    const std::filesystem::path link = directory / "worker-execution-via-symlink";
    std::filesystem::create_symlink(std::filesystem::canonical(executable), link);

    const auto encoded = gnfs::util::encode_sha256_hex(*first.digest);
    const std::string expected(encoded.data(), encoded.size());
    const std::string link_text = link.string();
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        char* const arguments[] = {
            const_cast<char*>(link_text.c_str()),
            const_cast<char*>(EXECUTABLE_DIGEST_CHILD_ARGUMENT),
            const_cast<char*>(expected.c_str()),
            nullptr,
        };
        ::execv(link_text.c_str(), arguments);
        ::_exit(127);
    }

    int child_status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(child, &child_status, 0);
    } while (waited < 0 && errno == EINTR);
    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);
    CHECK(!cleanup_error);
    CHECK(waited == child);
    CHECK(WIFEXITED(child_status));
    CHECK(WEXITSTATUS(child_status) == 0);
#endif
#else
    CHECK(!first);
#endif
    std::cout << "PASS\n";
}

} // namespace

int main(int argc, char** argv) {
#if defined(__APPLE__)
    if (argc == 3 && std::string_view(argv[1]) == EXECUTABLE_DIGEST_CHILD_ARGUMENT) {
        const auto expected = gnfs::util::decode_sha256_hex(argv[2]);
        const auto observed = execution::current_distributed_sieve_worker_executable_sha256_v1();
        return expected.has_value() && observed && observed.digest == expected ? 0 : 91;
    }
#endif
    try {
        CHECK(argc >= 1);
        test_runtime_rehydration_and_real_chunk_determinism();
        test_sq_and_relation_cap_boundary_priority();
        test_sink_failure_is_terminal_and_preserves_residue_signal();
        test_admission_rejects_invalid_and_duplicate_relations();
        test_trailing_projective_special_q_normalizes_cursor();
        test_sq_cap_preserves_cursor_before_projective_hole();
        test_current_executable_digest_is_stable(argv[0]);
        std::cout << "All distributed sieve worker execution tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

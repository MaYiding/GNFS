// Reuse the worker-entry test's real P8 self-exec fixture in the same
// translation unit. Renaming its main keeps this test focused on the next
// single-use transition: authenticated entry -> exact relation-writer authority.
#define main gnfs_embedded_distributed_sieve_worker_entry_test_main
#include "test_distributed_sieve_worker_entry.cpp"
#undef main

#include "distributed_sieve_worker_execution_internal.hpp"
#include "distributed_sieve_worker_writer_internal.hpp"

#include <gnfs/core/integer.hpp>
#include <gnfs/core/polynomial_context.hpp>
#include <gnfs/core/relation.hpp>
#include <gnfs/factor_base/builder.hpp>
#include <gnfs/factor_base/factor_base.hpp>
#include <gnfs/polynomial/base_m.hpp>
#include <gnfs/relation/ooc_relation_store.hpp>
#include <gnfs/sieve/special_q.hpp>
#include <gnfs/util/primes.hpp>
#include <gnfs/util/safe_math.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <numeric>
#include <set>
#include <signal.h>

#if !defined(_WIN32)
#include <sys/file.h>
#endif

#if defined(__APPLE__)
#include <sys/param.h>
#endif

namespace {

using WriterAuthority = entry::DistributedSieveWorkerWriterAuthorityV1;
using WriterCompletion = entry::DistributedSieveWorkerCompletionFactsV1;
using WriterResult = entry::DistributedSieveWorkerWriterAdoptionResultV1;
using WriterPhase = entry::DistributedSieveWorkerWriterPhaseV1;
using WriterRollback = entry::DistributedSieveWorkerWriterRollbackV1;
using WriterStatus = entry::DistributedSieveWorkerWriterStatusV1;
using Relation = gnfs::core::Relation;
namespace worker_execution = gnfs::sieve::distributed_sieve_worker_execution_detail;

static_assert(!std::is_default_constructible_v<WriterAuthority>);
static_assert(!std::is_copy_constructible_v<WriterAuthority>);
static_assert(!std::is_copy_assignable_v<WriterAuthority>);
static_assert(std::is_nothrow_move_constructible_v<WriterAuthority>);
static_assert(!std::is_move_assignable_v<WriterAuthority>);
static_assert(!std::is_constructible_v<WriterAuthority, std::filesystem::path>);
static_assert(!std::is_constructible_v<WriterAuthority, std::string>);
static_assert(!std::is_constructible_v<WriterAuthority, int>);
static_assert(!std::is_copy_constructible_v<WriterResult>);
static_assert(std::is_nothrow_move_constructible_v<WriterResult>);
static_assert(noexcept(entry::consume_distributed_sieve_worker_writer_v1(
    std::declval<entry::DistributedSieveWorkerEntryV1&&>())));
static_assert(noexcept(std::declval<const WriterAuthority&>().valid()));
static_assert(noexcept(std::declval<const WriterAuthority&>().finalized()));
static_assert(noexcept(std::declval<const WriterAuthority&>().handoff_published()));
static_assert(noexcept(std::declval<const WriterAuthority&>().count()));
static_assert(noexcept(std::declval<const WriterAuthority&>().record()));
static_assert(noexcept(std::declval<const WriterAuthority&>().manifest()));
static_assert(noexcept(std::declval<const WriterAuthority&>().identity()));
static_assert(noexcept(std::declval<const WriterAuthority&>().chunk()));
static_assert(noexcept(std::declval<const WriterAuthority&>().witness()));

#if defined(__APPLE__)
[[nodiscard]] constexpr std::size_t
positive_execution_policy_index(sieve::ExecutionPolicyKeyV1 key) noexcept {
    return static_cast<std::size_t>(static_cast<std::uint16_t>(key) - 1U);
}

[[nodiscard]] policy::DistributedSieveFrozenExecutionPolicyV1 make_positive_execution_policy() {
    policy::DistributedSieveExecutionPolicyEnvironmentSnapshotV1 snapshot;
    snapshot.hardware_concurrency = 8;
    snapshot.canonical_values[positive_execution_policy_index(
        sieve::ExecutionPolicyKeyV1::cofactor_brent)] = "1";
    auto frozen = policy::freeze_distributed_sieve_execution_policy_v1(snapshot);
    CHECK(frozen);
    CHECK(frozen.policy.has_value());
    return std::move(*frozen.policy);
}

[[nodiscard]] gnfs::core::PolynomialContext make_positive_execution_polynomial() {
    gnfs::core::Integer n("1000036000099");
    const auto selection = gnfs::polynomial::BaseMSelector::select(n, 3);
    CHECK(selection.success);
    auto polynomial = gnfs::polynomial::BaseMSelector::create_context(n, selection);
    CHECK(polynomial.degree() == 3);
    CHECK(polynomial.verify());
    return polynomial;
}

[[nodiscard]] gnfs::factor_base::FactorBase
make_positive_execution_factor_base(const gnfs::core::PolynomialContext& polynomial) {
    gnfs::factor_base::FactorBaseBuilder::Options options;
    options.rational_bound = 5000;
    options.algebraic_bound = 5000;
    options.log_scale = 16;
    options.parallel = false;
    const auto built = gnfs::factor_base::FactorBaseBuilder::build(polynomial, options);
    CHECK(built.rational_count() > 0);
    CHECK(built.algebraic_count() > 0);

    // The work identity requires canonical (p, r) order. BaseM's cubic can
    // expose several roots for one prime, while the builder's deterministic
    // Cantor-Zassenhaus traversal does not promise root order.
    std::vector<gnfs::core::AlgebraicPrime> algebraic(built.algebraic().begin(),
                                                      built.algebraic().end());
    std::sort(algebraic.begin(), algebraic.end(), [](const auto& left, const auto& right) {
        return left.p < right.p || (left.p == right.p && left.r < right.r);
    });

    gnfs::factor_base::FactorBase factor_base(built.params());
    factor_base.reserve(built.rational_count(), algebraic.size());
    for (const auto& factor : built.rational()) {
        factor_base.add_rational(factor.p, factor.log_p);
    }
    for (const auto& factor : algebraic) {
        factor_base.add_algebraic(factor.p, factor.r, factor.log_p, factor.degree);
    }
    factor_base.set_sieve_algebraic_count(built.sieve_algebraic_count());
    factor_base.build_index();
    return factor_base;
}

[[nodiscard]] gnfs::sieve::SpecialQ
make_positive_execution_special_q(const gnfs::factor_base::FactorBase& factor_base) {
    gnfs::sieve::SpecialQRange range;
    range.min_q = 1000;
    range.max_q = 1300;
    gnfs::sieve::SpecialQGenerator generator(factor_base, range);
    const auto special_q = generator.next();
    CHECK(special_q.has_value());
    CHECK(special_q->is_affine());
    return *special_q;
}

[[nodiscard]] sieve::DistributedSieveWorkIdentityV1
make_positive_execution_identity(const policy::DistributedSieveFrozenExecutionPolicyV1& frozen,
                                 const gnfs::core::PolynomialContext& polynomial,
                                 const gnfs::factor_base::FactorBase& factor_base,
                                 const gnfs::sieve::SpecialQ& special_q) {
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
        .rational_threshold = 60,
        .algebraic_threshold = 60,
        .large_prime_bound = 0,
        .allow_2lp = true,
        .allow_3lp = false,
    };
    identity.region = {
        .i_min = -200,
        .i_max = 199,
        .j_min = 1,
        .j_max = 40,
    };
    identity.cofactor = {
        .large_prime_bound = factor_base.params().large_prime_bound,
        .allow_1lp = true,
        .allow_2lp = false,
        .allow_3lp = false,
        .max_factorization_attempts = 10'000,
    };
    identity.original_sq_bounds = {
        .start_index = special_q.index,
        .end_index = static_cast<std::uint32_t>(special_q.index + 1U),
        .min_q = 0,
        .max_q = std::numeric_limits<std::uint32_t>::max(),
    };
    identity.effective_sq_bounds = identity.original_sq_bounds;
    identity.distributed.worker_count = 1;
    identity.distributed.chunks = {
        {0, special_q.index, static_cast<std::uint32_t>(special_q.index + 1U), "entry_chunk_0"}};
    identity.distributed.max_worker_attempts = 2;
    identity.distributed.max_merge_build_attempts = 2;
    identity.distributed.max_consumption_attempts = 2;
    identity.execution_policy = frozen.canonical;
    identity.semantic_versions = policy::DISTRIBUTED_SIEVE_BOUND_WORK_VERSIONS_V1;
    CHECK(sieve::validate_distributed_sieve_work_identity(identity));
    return identity;
}

class PositiveWorkerExecutionFixture final {
public:
    explicit PositiveWorkerExecutionFixture(std::string_view label, Digest executable_sha256)
        : frozen(make_positive_execution_policy()),
          polynomial(make_positive_execution_polynomial()),
          factor_base(make_positive_execution_factor_base(polynomial)),
          special_q(make_positive_execution_special_q(factor_base)),
          identity(make_positive_execution_identity(frozen, polynomial, factor_base, special_q)),
          root(temp.path() / std::string(label)),
          opened(wave::DistributedSieveWaveStore::create(
              root, make_manifest_draft(identity, executable_sha256))) {
        if (!opened || opened.store == nullptr) {
            fail("create positive worker-execution WaveStore", __LINE__,
                 wave_diagnostic_detail(opened.diagnostic));
        }
    }

    [[nodiscard]] wave::DistributedSieveWaveStore& store() const noexcept {
        return *opened.store;
    }

    [[nodiscard]] wave::DistributedSieveWorkerAttemptStartReceipt start_receipt() {
        auto claimed = store().create_worker_attempt_private_lease_root(0, 0);
        CHECK(claimed);
        CHECK(claimed.claim != nullptr);
        auto reserved = wave::reserve_worker_attempt_private_lease(std::move(claimed));
        CHECK(reserved);
        CHECK(reserved.receipt.has_value());
        auto started = wave::publish_worker_attempt_started(std::move(*reserved.receipt));
        CHECK(started);
        CHECK(started.receipt.has_value());
        return std::move(*started.receipt);
    }

    TempDirectory temp;
    policy::DistributedSieveFrozenExecutionPolicyV1 frozen;
    gnfs::core::PolynomialContext polynomial;
    gnfs::factor_base::FactorBase factor_base;
    gnfs::sieve::SpecialQ special_q;
    sieve::DistributedSieveWorkIdentityV1 identity;
    std::filesystem::path root;
    wave::DistributedSieveWaveStoreOpenResult opened;
};

[[nodiscard]] std::uint64_t positive_execution_signed_mod(std::int64_t value,
                                                          std::uint64_t modulus) noexcept {
    const std::uint64_t magnitude = gnfs::util::safe_abs(value) % modulus;
    if (value >= 0 || magnitude == 0) {
        return magnitude;
    }
    return modulus - magnitude;
}

void require_positive_execution_prime_power(gnfs::core::Integer& product, std::uint64_t prime,
                                            std::uint8_t exponent) {
    CHECK(prime > 1);
    CHECK(exponent > 0);
    CHECK(gnfs::util::is_prime_u64(prime));
    for (std::uint32_t power = 0; power < exponent; ++power) {
        product *= gnfs::core::Integer(prime);
    }
}

void require_positive_execution_algebraic_ideal(const gnfs::core::PolynomialContext& polynomial,
                                                const Relation& relation, std::uint64_t prime,
                                                std::uint64_t root) {
    CHECK(prime > 1);
    CHECK(gnfs::util::is_prime_u64(prime));
    if (root == gnfs::core::AlgebraicPrime::PROJECTIVE_ROOT) {
        CHECK(relation.b % prime == 0);
        gnfs::core::Integer leading_remainder;
        gnfs::core::Integer::mod(leading_remainder, polynomial.leading_coeff(),
                                 gnfs::core::Integer(prime));
        CHECK(leading_remainder.is_zero());
        return;
    }

    CHECK(root < prime);
    CHECK(polynomial.evaluate_mod(root, prime) == 0);
    CHECK(positive_execution_signed_mod(relation.a, prime) ==
          gnfs::util::mul_mod_u64(relation.b % prime, root, prime));
}

void require_positive_execution_relation(const gnfs::core::PolynomialContext& polynomial,
                                         const gnfs::factor_base::FactorBase& factor_base,
                                         const gnfs::sieve::SpecialQ& special_q,
                                         const Relation& relation) {
    CHECK(polynomial.degree() == 3);
    CHECK(polynomial.verify());
    CHECK(relation.b > 0);
    CHECK(relation.extra_ab_pairs.empty());
    CHECK(std::gcd(gnfs::util::safe_abs(relation.a), relation.b) == std::uint64_t{1});

    auto rational_value = polynomial.rational_value(relation.a, relation.b);
    rational_value.abs();
    CHECK(!rational_value.is_zero());
    CHECK(gnfs::core::gcd(rational_value, polynomial.n()).is_one());

    auto algebraic_norm = polynomial.algebraic_norm(relation.a, relation.b);
    algebraic_norm.abs();
    CHECK(!algebraic_norm.is_zero());

    gnfs::core::Integer rational_product(1);
    for (const std::uint32_t index : relation.rational_factors) {
        CHECK(index < factor_base.rational_count());
        const auto& factor = factor_base.rational()[index];
        CHECK(factor.p <= factor_base.params().rational_bound);
        require_positive_execution_prime_power(rational_product, factor.p, 1);
    }
    for (const auto& prime_power : relation.rational_large_prime) {
        CHECK(prime_power.r == 0);
        CHECK(prime_power.p <= factor_base.params().large_prime_bound);
        require_positive_execution_prime_power(rational_product, prime_power.p, prime_power.e);
    }
    CHECK(rational_product == rational_value);

    gnfs::core::Integer algebraic_product(1);
    for (const std::uint32_t index : relation.algebraic_factors) {
        CHECK(index < factor_base.sieve_algebraic_count());
        const auto& factor = factor_base.algebraic()[index];
        CHECK(factor.p <= factor_base.params().algebraic_bound);
        CHECK(factor.degree == 1);
        require_positive_execution_algebraic_ideal(polynomial, relation, factor.p, factor.r);
        require_positive_execution_prime_power(algebraic_product, factor.p, 1);
    }

    bool special_q_recorded = false;
    for (const auto& prime_power : relation.algebraic_large_prime) {
        CHECK(prime_power.p <= factor_base.params().large_prime_bound);
        require_positive_execution_algebraic_ideal(polynomial, relation, prime_power.p,
                                                   prime_power.r);
        require_positive_execution_prime_power(algebraic_product, prime_power.p, prime_power.e);
        if (prime_power.p == special_q.q && prime_power.r == special_q.r) {
            special_q_recorded = true;
        }
    }
    CHECK(algebraic_product == algebraic_norm);

    CHECK(special_q.is_affine());
    CHECK(special_q_recorded);
    CHECK(mpz_divisible_ui_p(algebraic_norm.get_mpz(), special_q.q) != 0);
    require_positive_execution_algebraic_ideal(polynomial, relation, special_q.q, special_q.r);
}
#endif

[[nodiscard]] Relation make_writer_relation(std::int64_t a, std::uint64_t b, std::uint32_t seed) {
    Relation relation(a, b);
    relation.rational_factors = {seed, static_cast<std::uint32_t>(seed + 2)};
    relation.algebraic_factors = {
        static_cast<std::uint32_t>(seed + 1),
        static_cast<std::uint32_t>(seed + 3),
    };
    relation.rational_large_prime = {
        {static_cast<std::uint64_t>(1009 + seed), 0, static_cast<std::uint8_t>(1 + seed % 2)},
    };
    relation.algebraic_large_prime = {
        {static_cast<std::uint64_t>(2003 + seed), static_cast<std::uint64_t>(17 + seed), 1},
    };
    relation.extra_ab_pairs = {{a - 10, b + 1}, {a + 20, b + 2}};
    return relation;
}

[[nodiscard]] bool writer_relation_equal(const Relation& left, const Relation& right) noexcept {
    return left.a == right.a && left.b == right.b &&
           left.rational_factors == right.rational_factors &&
           left.algebraic_factors == right.algebraic_factors &&
           left.rational_large_prime == right.rational_large_prime &&
           left.algebraic_large_prime == right.algebraic_large_prime &&
           left.extra_ab_pairs == right.extra_ab_pairs;
}

[[nodiscard]] int replace_regular_at_corrupt_same_extent(int directory, std::string_view leaf,
                                                         std::string_view displaced_leaf) noexcept {
    std::vector<std::byte> replacement;
    mode_t mode = 0;
    if (const int failure = read_regular_at(directory, leaf, replacement, mode); failure != 0) {
        return failure;
    }
    if (replacement.empty()) {
        return EINVAL;
    }
    for (auto& byte : replacement) {
        byte ^= std::byte{0xff};
    }
    return replace_regular_at_bytes(directory, leaf, displaced_leaf, replacement);
}

[[nodiscard]] std::vector<std::byte>
make_valid_nonidentical_worker_handoff_bytes(std::span<const std::byte> original) {
    auto outer = gnfs::relation::decode_ooc_private_handoff_record(original);
    CHECK(outer);
    CHECK(outer.value.has_value());

    auto payload = sieve::decode_distributed_sieve_record(outer.value->opaque_payload);
    CHECK(payload);
    CHECK(payload.value.has_value());
    auto* handoff = std::get_if<sieve::WorkerHandoffV1>(&*payload.value);
    CHECK(handoff != nullptr);
    CHECK(handoff->processed_sq_count == 1);
    CHECK(handoff->sq_end - handoff->sq_begin >= 2);
    handoff->processed_sq_count = 2;
    handoff->self_digest = {};
    CHECK(sieve::seal_distributed_sieve_record(*payload.value));
    const auto encoded_payload = sieve::encode_distributed_sieve_record(*payload.value);
    CHECK(encoded_payload);
    CHECK(encoded_payload.bytes.has_value());

    outer.value->opaque_payload = *encoded_payload.bytes;
    outer.value->payload_digest = {};
    outer.value->self_digest = {};
    CHECK(gnfs::relation::seal_ooc_private_handoff_record(*outer.value));
    const auto encoded_outer = gnfs::relation::encode_ooc_private_handoff_record(*outer.value);
    CHECK(encoded_outer);
    CHECK(encoded_outer.bytes.has_value());
    CHECK(encoded_outer.bytes->size() == original.size());
    CHECK(!std::equal(encoded_outer.bytes->begin(), encoded_outer.bytes->end(), original.begin(),
                      original.end()));
    return std::move(*encoded_outer.bytes);
}

[[nodiscard]] WriterCompletion successful_completion(const WriterAuthority& authority) {
    return {
        .processed_sq_count = 1,
        .next_sq_index = authority.record().sq_end,
        .completion_reason = authority.count() == 0
                                 ? sieve::WorkerCompletionReasonV1::zero_relations
                                 : sieve::WorkerCompletionReasonV1::range_exhausted,
    };
}

template <typename Callable>
[[nodiscard]] bool rejects_writer_mutation(Callable&& callable) noexcept {
    try {
        std::forward<Callable>(callable)();
    } catch (const std::logic_error&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

template <typename Callable>
[[nodiscard]] bool rejects_writer_runtime_failure(Callable&& callable) noexcept {
    try {
        std::forward<Callable>(callable)();
    } catch (const std::runtime_error&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

enum class WriterChildScenario : std::uint32_t {
    happy = 1,
    zero_row,
    handoff_cache_commit_failure,
    handoff_pending_durable,
    handoff_canonical_promoted,
    handoff_canonical_durable,
    handoff_reserved_revoked_durable,
    reserved_replacement_sandwich,
    private_directory_replacement_sandwich,
    base_lock_replacement_sandwich,
    wrong_base_path_digest,
    foreign_staging_residue,
    post_authority_base_lock_replacement,
    post_authority_reserved_replacement,
    post_authority_private_directory_replacement,
    construction_clean_rollback,
    construction_foreign_index_residue,
    real_worker_execution,
    predecessor_handoff_interleaving,
};

inline constexpr std::string_view WRITER_CHILD_ARGUMENT = "--worker-writer-authority-child";
inline constexpr std::string_view WRITER_COLD_CRASH_CHILD_ARGUMENT =
    "--worker-writer-authority-cold-crash-child";
inline constexpr std::uint32_t WRITER_CHILD_REPORT_MAGIC = 0x47575731U;
inline constexpr int WRITER_HANDOFF_CRASH_EXIT_BASE = 180;

inline constexpr std::uint32_t WRITER_FLAG_ENTRY_ADOPTED = 1U << 0U;
inline constexpr std::uint32_t WRITER_FLAG_FIXED_FDS_CLOSED = 1U << 1U;
inline constexpr std::uint32_t WRITER_FLAG_AUTHORITY_READY = 1U << 2U;
inline constexpr std::uint32_t WRITER_FLAG_BINDINGS_VALID = 1U << 3U;
inline constexpr std::uint32_t WRITER_FLAG_SECOND_CONSUME_REJECTED = 1U << 4U;
inline constexpr std::uint32_t WRITER_FLAG_FORK_WRITE_REJECTED = 1U << 5U;
inline constexpr std::uint32_t WRITER_FLAG_FORK_FINALIZE_REJECTED = 1U << 6U;
inline constexpr std::uint32_t WRITER_FLAG_PARENT_UNCHANGED_AFTER_FORK = 1U << 7U;
inline constexpr std::uint32_t WRITER_FLAG_WROTE_EXACT_PAIR = 1U << 8U;
inline constexpr std::uint32_t WRITER_FLAG_FINALIZED = 1U << 9U;
inline constexpr std::uint32_t WRITER_FLAG_POST_FINALIZE_REJECTED = 1U << 10U;
inline constexpr std::uint32_t WRITER_FLAG_MUTATION_INVOKED = 1U << 11U;
inline constexpr std::uint32_t WRITER_FLAG_MUTATION_SUCCEEDED = 1U << 12U;
inline constexpr std::uint32_t WRITER_FLAG_FORK_NORMAL_RETURN = 1U << 13U;
inline constexpr std::uint32_t WRITER_FLAG_POST_AUTHORITY_WRITE_REJECTED = 1U << 14U;
inline constexpr std::uint32_t WRITER_FLAG_POST_AUTHORITY_INVALIDATED = 1U << 15U;
inline constexpr std::uint32_t WRITER_FLAG_POST_AUTHORITY_FINALIZE_REJECTED = 1U << 16U;
inline constexpr std::uint32_t WRITER_FLAG_CONSTRUCTION_HOOK_INVOKED = 1U << 17U;
inline constexpr std::uint32_t WRITER_FLAG_RECONCILIATION_REQUIRED = 1U << 18U;
inline constexpr std::uint32_t WRITER_FLAG_HANDOFF_PUBLISHED = 1U << 19U;
inline constexpr std::uint32_t WRITER_FLAG_ZERO_ROW = 1U << 20U;
inline constexpr std::uint32_t WRITER_FLAG_HANDOFF_INTERRUPTED = 1U << 21U;
inline constexpr std::uint32_t WRITER_FLAG_HANDOFF_RETRY_SUCCEEDED = 1U << 22U;
inline constexpr std::uint32_t WRITER_FLAG_HANDOFF_RETRY_DRIFT_REJECTED = 1U << 23U;
inline constexpr std::uint32_t WRITER_FLAG_HANDOFF_PREFIX_OBSERVED = 1U << 24U;
inline constexpr std::uint32_t WRITER_FLAG_HANDOFF_PREFIX_NO_CLEANUP = 1U << 25U;
#if !defined(__APPLE__)
inline constexpr std::uint32_t WRITER_FLAG_HANDOFF_PLATFORM_UNSUPPORTED = 1U << 26U;
inline constexpr std::uint32_t WRITER_FLAG_HANDOFF_APPEND_PRESERVED = 1U << 29U;
#endif
inline constexpr std::uint32_t WRITER_FLAG_HANDOFF_CACHE_FAILURE = 1U << 27U;
inline constexpr std::uint32_t WRITER_FLAG_HANDOFF_CACHE_RETRY_SUCCEEDED = 1U << 28U;
#if defined(__APPLE__)
inline constexpr std::uint32_t WRITER_FLAG_HANDOFF_MANIFEST_CAP_REJECTED = 1U << 30U;
#endif
inline constexpr std::uint32_t WRITER_FLAG_REAL_EXECUTION_SUCCEEDED = 1U << 31U;

inline constexpr std::uint32_t WRITER_HAPPY_FLAGS =
    WRITER_FLAG_ENTRY_ADOPTED | WRITER_FLAG_FIXED_FDS_CLOSED | WRITER_FLAG_AUTHORITY_READY |
    WRITER_FLAG_BINDINGS_VALID | WRITER_FLAG_SECOND_CONSUME_REJECTED |
    WRITER_FLAG_FORK_WRITE_REJECTED | WRITER_FLAG_FORK_FINALIZE_REJECTED |
    WRITER_FLAG_PARENT_UNCHANGED_AFTER_FORK | WRITER_FLAG_WROTE_EXACT_PAIR | WRITER_FLAG_FINALIZED |
    WRITER_FLAG_POST_FINALIZE_REJECTED | WRITER_FLAG_FORK_NORMAL_RETURN |
    WRITER_FLAG_HANDOFF_PUBLISHED
#if defined(__APPLE__)
    | WRITER_FLAG_HANDOFF_MANIFEST_CAP_REJECTED
#endif
    ;

struct WriterChildReport final {
    std::uint32_t magic = WRITER_CHILD_REPORT_MAGIC;
    std::uint32_t scenario = 0;
    std::uint32_t flags = 0;
    std::uint32_t entry_status = 0;
    std::uint32_t entry_phase = 0;
    std::uint32_t writer_status = 0;
    std::uint32_t writer_phase = 0;
    std::uint32_t writer_rollback = 0;
    std::uint32_t second_status = 0;
    std::uint32_t second_phase = 0;
    std::uint32_t chunk_id = 0;
    std::uint32_t attempt_ordinal = 0;
    std::uint64_t count_after_write = 0;
    std::int32_t mutation_error = 0;
    std::int32_t writer_native_error = 0;
    std::int32_t rollback_native_error = 0;
    Digest attempt_digest;
    Digest manifest_digest;
    Digest work_digest;
    Digest handoff_digest;
    Digest corpus_sha256;
    std::uint64_t sequence_count = 0;
    std::uint64_t sequence_low = 0;
    std::uint64_t sequence_high = 0;
};

static_assert(std::is_trivially_copyable_v<WriterChildReport>);

[[nodiscard]] std::optional<WriterChildScenario>
parse_writer_child_scenario(std::string_view raw) noexcept {
    std::uint32_t value = 0;
    if (raw.empty()) {
        return std::nullopt;
    }
    for (const char character : raw) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        value = value * 10U + static_cast<std::uint32_t>(character - '0');
    }
    if (value < static_cast<std::uint32_t>(WriterChildScenario::happy) ||
        value > static_cast<std::uint32_t>(WriterChildScenario::predecessor_handoff_interleaving)) {
        return std::nullopt;
    }
    return static_cast<WriterChildScenario>(value);
}

#if !defined(_WIN32)

[[nodiscard]] constexpr bool is_post_authority_drift(WriterChildScenario scenario) noexcept {
    return scenario == WriterChildScenario::post_authority_base_lock_replacement ||
           scenario == WriterChildScenario::post_authority_reserved_replacement ||
           scenario == WriterChildScenario::post_authority_private_directory_replacement;
}

[[nodiscard]] constexpr bool is_construction_failure(WriterChildScenario scenario) noexcept {
    return scenario == WriterChildScenario::construction_clean_rollback ||
           scenario == WriterChildScenario::construction_foreign_index_residue;
}

[[nodiscard]] constexpr bool is_handoff_fault(WriterChildScenario scenario) noexcept {
    return scenario == WriterChildScenario::handoff_pending_durable ||
           scenario == WriterChildScenario::handoff_canonical_promoted ||
           scenario == WriterChildScenario::handoff_canonical_durable ||
           scenario == WriterChildScenario::handoff_reserved_revoked_durable;
}

[[nodiscard]] gnfs::relation::OOCPrivateHandoffFaultPoint
handoff_fault_point(WriterChildScenario scenario) {
    switch (scenario) {
    case WriterChildScenario::handoff_pending_durable:
        return gnfs::relation::OOCPrivateHandoffFaultPoint::PendingDurable;
    case WriterChildScenario::handoff_canonical_promoted:
        return gnfs::relation::OOCPrivateHandoffFaultPoint::CanonicalPromoted;
    case WriterChildScenario::handoff_canonical_durable:
        return gnfs::relation::OOCPrivateHandoffFaultPoint::CanonicalDurable;
    case WriterChildScenario::handoff_reserved_revoked_durable:
        return gnfs::relation::OOCPrivateHandoffFaultPoint::ReservedRevokedDurable;
    default:
        throw std::invalid_argument("writer scenario has no handoff fault point");
    }
}

struct WorkerHandoffStopContext final {
    gnfs::relation::OOCPrivateHandoffFaultPoint target =
        gnfs::relation::OOCPrivateHandoffFaultPoint::PendingDurable;
    bool terminate_process = false;
    bool invoked = false;
};

[[nodiscard]] bool stop_worker_handoff(gnfs::relation::OOCPrivateHandoffFaultPoint point,
                                       void* opaque) noexcept {
    auto& context = *static_cast<WorkerHandoffStopContext*>(opaque);
    if (point != context.target) {
        return false;
    }
    context.invoked = true;
    if (context.terminate_process) {
        ::_exit(WRITER_HANDOFF_CRASH_EXIT_BASE + static_cast<int>(point));
    }
    return true;
}

struct LeafFingerprint final {
    bool present = false;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t size = 0;

    [[nodiscard]] friend constexpr bool operator==(const LeafFingerprint&,
                                                   const LeafFingerprint&) noexcept = default;
};

[[nodiscard]] bool regular_leaf_state_at(int directory, std::string_view leaf,
                                         bool expected_present, int& error,
                                         LeafFingerprint* fingerprint = nullptr) {
    struct stat metadata {};
    int result = -1;
    do {
        result = ::fstatat(directory, std::string(leaf).c_str(), &metadata, AT_SYMLINK_NOFOLLOW);
    } while (result != 0 && errno == EINTR);
    if (result == 0) {
        if (fingerprint != nullptr) {
            *fingerprint = {
                .present = true,
                .device = static_cast<std::uint64_t>(metadata.st_dev),
                .inode = static_cast<std::uint64_t>(metadata.st_ino),
                .size = metadata.st_size < 0 ? 0 : static_cast<std::uint64_t>(metadata.st_size),
            };
        }
        return expected_present && S_ISREG(metadata.st_mode);
    }
    if (errno == ENOENT) {
        if (fingerprint != nullptr) {
            *fingerprint = {};
        }
        return !expected_present;
    }
    error = errno;
    return false;
}

struct WorkerHandoffPrefixObservation final {
    bool exact_shape = false;
    bool no_cleanup_artifact = false;
    int error = 0;
    std::array<LeafFingerprint, 6> protected_leaves{};

    [[nodiscard]] friend constexpr bool
    operator==(const WorkerHandoffPrefixObservation&,
               const WorkerHandoffPrefixObservation&) noexcept = default;
};

[[nodiscard]] WorkerHandoffPrefixObservation
observe_worker_handoff_prefix(int root_descriptor,
                              const wave::DistributedSieveWorkerAttemptNamesV1& names,
                              WriterChildScenario scenario) {
    WorkerHandoffPrefixObservation observation;
    int directory = -1;
    do {
        directory = ::openat(root_descriptor, names.private_directory_leaf.c_str(),
                             O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (directory < 0 && errno == EINTR);
    if (directory < 0) {
        observation.error = errno;
        return observation;
    }

    const bool pending = scenario == WriterChildScenario::handoff_pending_durable;
    const bool canonical =
        !pending && scenario != WriterChildScenario::handoff_cache_commit_failure;
    const bool reserved = scenario != WriterChildScenario::handoff_reserved_revoked_durable;
    observation.exact_shape =
        regular_leaf_state_at(directory, "corpus.relidx", true, observation.error,
                              &observation.protected_leaves[0]) &&
        regular_leaf_state_at(directory, "corpus.reldata", true, observation.error,
                              &observation.protected_leaves[1]) &&
        regular_leaf_state_at(directory, "corpus.gnfs-ooc-private-handoff-v1", canonical,
                              observation.error, &observation.protected_leaves[2]) &&
        regular_leaf_state_at(directory, "corpus.gnfs-ooc-private-handoff-v1.pending", pending,
                              observation.error, &observation.protected_leaves[3]) &&
        regular_leaf_state_at(root_descriptor, names.reserved_leaf, reserved, observation.error,
                              &observation.protected_leaves[4]) &&
        regular_leaf_state_at(root_descriptor, names.owned_leaf, true, observation.error,
                              &observation.protected_leaves[5]) &&
        regular_leaf_state_at(root_descriptor, names.reserved_pending_leaf, false,
                              observation.error) &&
        regular_leaf_state_at(root_descriptor, names.owned_pending_leaf, false, observation.error);

    constexpr std::array<std::string_view, 6> cleanup_leaves{
        "corpus.gnfs-ooc-cleanup-v1.intent", "corpus.gnfs-ooc-cleanup-v1.intent.pending",
        "corpus.gnfs-ooc-cleanup-v1.staged", "corpus.gnfs-ooc-cleanup-v1.staged.pending",
        "corpus.gnfs-ooc-cleanup-v1.relidx", "corpus.gnfs-ooc-cleanup-v1.reldata",
    };
    observation.no_cleanup_artifact = true;
    for (const auto leaf : cleanup_leaves) {
        if (!regular_leaf_state_at(directory, leaf, false, observation.error)) {
            observation.no_cleanup_artifact = false;
            break;
        }
    }

    if (::close(directory) != 0 && observation.error == 0) {
        observation.error = errno;
        observation.exact_shape = false;
        observation.no_cleanup_artifact = false;
    }
    return observation;
}

struct NamespaceNode final {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    mode_t mode = 0;
    std::vector<std::byte> bytes;
};

using NamespaceSnapshot = std::map<std::string, NamespaceNode>;

[[nodiscard]] std::vector<std::byte> read_snapshot_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    CHECK(stream.is_open());
    stream.seekg(0, std::ios::end);
    const auto end = stream.tellg();
    CHECK(end >= 0);
    stream.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        CHECK(stream.good());
    }
    return bytes;
}

[[nodiscard]] NamespaceSnapshot snapshot_namespace(const std::filesystem::path& root) {
    NamespaceSnapshot result;
    for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
        struct stat metadata {};
        CHECK(::lstat(item.path().c_str(), &metadata) == 0);
        NamespaceNode node{
            .device = static_cast<std::uint64_t>(metadata.st_dev),
            .inode = static_cast<std::uint64_t>(metadata.st_ino),
            .mode = metadata.st_mode,
        };
        if (S_ISREG(metadata.st_mode)) {
            node.bytes = read_snapshot_file(item.path());
        }
        const auto relative = std::filesystem::relative(item.path(), root).generic_string();
        CHECK(result.emplace(relative, std::move(node)).second);
    }
    return result;
}

[[nodiscard]] int create_durable_empty_regular_at_root(const std::filesystem::path& root,
                                                       std::string_view leaf) {
    int directory = -1;
    do {
        directory =
            ::open(root.c_str(), O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (directory < 0 && errno == EINTR);
    if (directory < 0) {
        return errno;
    }

    const std::string owned_leaf(leaf);
    int file = -1;
    do {
        file = ::openat(directory, owned_leaf.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    } while (file < 0 && errno == EINTR);
    int failure = file < 0 ? errno : 0;
    if (file >= 0) {
        int synchronized = -1;
        do {
            synchronized = ::fsync(file);
        } while (synchronized != 0 && errno == EINTR);
        if (synchronized != 0) {
            failure = errno;
        }
        if (::close(file) != 0 && failure == 0) {
            failure = errno;
        }
    }

    if (failure == 0) {
        int synchronized = -1;
        do {
            synchronized = ::fsync(directory);
        } while (synchronized != 0 && errno == EINTR);
        if (synchronized != 0) {
            failure = errno;
        }
    }
    if (::close(directory) != 0 && failure == 0) {
        failure = errno;
    }
    return failure;
}

inline constexpr std::string_view WORKER_HANDOFF_CANONICAL_LEAF =
    "corpus.gnfs-ooc-private-handoff-v1";
inline constexpr std::string_view WORKER_HANDOFF_PENDING_LEAF =
    "corpus.gnfs-ooc-private-handoff-v1.pending";

[[nodiscard]] int synchronize_descriptor(int descriptor) noexcept {
    int synchronized = -1;
    do {
        synchronized = ::fsync(descriptor);
    } while (synchronized != 0 && errno == EINTR);
    return synchronized == 0 ? 0 : errno;
}

[[nodiscard]] int synchronize_regular_at(int directory, std::string_view leaf) noexcept {
    int file = -1;
    do {
        file = ::openat(directory, std::string(leaf).c_str(),
                        O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    } while (file < 0 && errno == EINTR);
    if (file < 0) {
        return errno;
    }
    struct stat metadata {};
    int failure = 0;
    if (::fstat(file, &metadata) != 0) {
        failure = errno;
    } else if (!S_ISREG(metadata.st_mode)) {
        failure = EINVAL;
    } else {
        failure = synchronize_descriptor(file);
    }
    if (::close(file) != 0 && failure == 0) {
        failure = errno;
    }
    return failure;
}

[[nodiscard]] int copy_regular_at_same_bytes_durable(int directory, std::string_view source_leaf,
                                                     std::string_view destination_leaf) noexcept {
    int failure = copy_regular_at_same_bytes(directory, source_leaf, destination_leaf);
    if (failure == 0) {
        failure = synchronize_regular_at(directory, destination_leaf);
    }
    if (failure == 0) {
        failure = synchronize_descriptor(directory);
    }
    return failure;
}

[[nodiscard]] int unlink_regular_at_durable(int directory, std::string_view leaf) noexcept {
    struct stat metadata {};
    if (::fstatat(directory, std::string(leaf).c_str(), &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno;
    }
    if (!S_ISREG(metadata.st_mode)) {
        return EINVAL;
    }
    int unlinked = -1;
    do {
        unlinked = ::unlinkat(directory, std::string(leaf).c_str(), 0);
    } while (unlinked != 0 && errno == EINTR);
    if (unlinked != 0) {
        return errno;
    }
    return synchronize_descriptor(directory);
}

[[nodiscard]] int replace_regular_at_same_bytes_durable(int directory, std::string_view leaf,
                                                        std::string_view displaced_leaf) noexcept {
    int failure = replace_regular_at_same_bytes(directory, leaf, displaced_leaf);
    if (failure == 0) {
        failure = synchronize_regular_at(directory, leaf);
    }
    if (failure == 0) {
        failure = unlink_regular_at_durable(directory, displaced_leaf);
    }
    return failure;
}

[[nodiscard]] int append_byte_at_durable(int directory, std::string_view leaf) noexcept {
    int file = -1;
    do {
        file = ::openat(directory, std::string(leaf).c_str(),
                        O_WRONLY | O_APPEND | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    } while (file < 0 && errno == EINTR);
    if (file < 0) {
        return errno;
    }
    const std::byte byte{0xa5};
    ssize_t written = -1;
    do {
        written = ::write(file, &byte, 1);
    } while (written < 0 && errno == EINTR);
    int failure = written == 1 ? synchronize_descriptor(file) : (written < 0 ? errno : EIO);
    if (::close(file) != 0 && failure == 0) {
        failure = errno;
    }
    if (failure == 0) {
        failure = synchronize_descriptor(directory);
    }
    return failure;
}

#if defined(__APPLE__)
[[nodiscard]] std::filesystem::path snapshot_root_path(int descriptor) {
    std::array<char, MAXPATHLEN> buffer{};
    int result = -1;
    do {
        result = ::fcntl(descriptor, F_GETPATH, buffer.data());
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        throw std::system_error(errno, std::generic_category(), "recover snapshot root path");
    }
    const auto terminator = std::find(buffer.begin(), buffer.end(), '\0');
    if (terminator == buffer.begin() || terminator == buffer.end()) {
        throw std::runtime_error("snapshot root path is invalid");
    }
    return std::filesystem::path(std::string(buffer.begin(), terminator));
}
#endif

[[nodiscard]] bool same_baseline_node(const NamespaceNode& left,
                                      const NamespaceNode& right) noexcept {
    if (left.device != right.device || left.inode != right.inode || left.mode != right.mode) {
        return false;
    }
    if (S_ISREG(left.mode)) {
        return left.bytes == right.bytes;
    }
    return true;
}

[[nodiscard]] bool same_namespace_snapshot(const NamespaceSnapshot& left,
                                           const NamespaceSnapshot& right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (const auto& [path, expected] : left) {
        const auto found = right.find(path);
        if (found == right.end() || !same_baseline_node(expected, found->second)) {
            return false;
        }
    }
    return true;
}

template <typename Fixture>
void require_exact_happy_namespace_delta(const Fixture& fixture,
                                         const NamespaceSnapshot& baseline) {
    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    const auto after = snapshot_namespace(fixture.root);

    for (const auto& [path, expected] : baseline) {
        if (path == names->reserved_leaf) {
            CHECK(!after.contains(path));
            continue;
        }
        const auto found = after.find(path);
        CHECK(found != after.end());
        CHECK(same_baseline_node(expected, found->second));
    }

    std::set<std::string> additions;
    for (const auto& [path, node] : after) {
        (void)node;
        if (!baseline.contains(path)) {
            additions.insert(path);
        }
    }
    const std::string prefix = names->private_directory_leaf + "/";
    const std::set<std::string> expected{
        prefix + "corpus.relidx",
        prefix + "corpus.reldata",
        prefix + "corpus.gnfs-ooc-private-handoff-v1",
    };
    CHECK(additions == expected);
}

#if !defined(__APPLE__)
void require_exact_unsupported_namespace_delta(const WorkerEntryFixture& fixture,
                                               const NamespaceSnapshot& baseline) {
    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    const auto after = snapshot_namespace(fixture.root);

    for (const auto& [path, expected] : baseline) {
        const auto found = after.find(path);
        CHECK(found != after.end());
        CHECK(same_baseline_node(expected, found->second));
    }

    std::set<std::string> additions;
    for (const auto& [path, node] : after) {
        (void)node;
        if (!baseline.contains(path)) {
            additions.insert(path);
        }
    }
    const std::string prefix = names->private_directory_leaf + "/";
    const std::set<std::string> expected{
        prefix + "corpus.relidx",
        prefix + "corpus.reldata",
    };
    CHECK(additions == expected);
}
#endif

template <typename Fixture> void require_no_cleanup_publication(const Fixture& fixture) {
    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    for (const auto& item : std::filesystem::recursive_directory_iterator(fixture.root)) {
        const std::string leaf = item.path().filename().string();
        if (leaf.find("cleanup") != std::string::npos && leaf != names->base_lock_leaf) {
            fail("writer authority publishes no cleanup artifact", __LINE__, leaf);
        }
    }
}

void require_no_handoff_or_cleanup_publication(const WorkerEntryFixture& fixture) {
    require_no_cleanup_publication(fixture);
    for (const auto& item : std::filesystem::recursive_directory_iterator(fixture.root)) {
        CHECK(item.path().filename().string().find("handoff") == std::string::npos);
    }
}

void require_unfinalized_corpus(const std::filesystem::path& base) {
    CHECK(std::filesystem::is_regular_file(base.string() + ".relidx"));
    CHECK(std::filesystem::is_regular_file(base.string() + ".reldata"));
    bool rejected = false;
    try {
        gnfs::relation::OOCRelationReader reader(base.string());
        (void)reader;
    } catch (const std::exception&) {
        rejected = true;
    }
    CHECK(rejected);
}

void require_distinct_regular_inodes(const std::filesystem::path& left,
                                     const std::filesystem::path& right) {
    struct stat left_metadata {};
    struct stat right_metadata {};
    CHECK(::lstat(left.c_str(), &left_metadata) == 0);
    CHECK(::lstat(right.c_str(), &right_metadata) == 0);
    CHECK(S_ISREG(left_metadata.st_mode));
    CHECK(S_ISREG(right_metadata.st_mode));
    CHECK(left_metadata.st_dev != right_metadata.st_dev ||
          left_metadata.st_ino != right_metadata.st_ino);
}

void require_distinct_owner_directories(const std::filesystem::path& left,
                                        const std::filesystem::path& right) {
    struct stat left_metadata {};
    struct stat right_metadata {};
    CHECK(::lstat(left.c_str(), &left_metadata) == 0);
    CHECK(::lstat(right.c_str(), &right_metadata) == 0);
    CHECK(S_ISDIR(left_metadata.st_mode));
    CHECK(S_ISDIR(right_metadata.st_mode));
    CHECK((left_metadata.st_mode & static_cast<mode_t>(07777)) == 0700);
    CHECK((right_metadata.st_mode & static_cast<mode_t>(07777)) == 0700);
    CHECK(left_metadata.st_dev != right_metadata.st_dev ||
          left_metadata.st_ino != right_metadata.st_ino);
}

void configure_writer_replacement(WriterChildScenario scenario,
                                  const wave::DistributedSieveWorkerAttemptNamesV1& names,
                                  ReplacementHookContext& replacement) {
    switch (scenario) {
    case WriterChildScenario::reserved_replacement_sandwich:
        replacement.leaf = names.reserved_leaf;
        replacement.displaced_leaf = ".gnfs-test-writer-displaced-reserved";
        break;
    case WriterChildScenario::private_directory_replacement_sandwich:
        replacement.kind = ReplacementHookContext::Kind::private_directory_replacement;
        replacement.leaf = names.private_directory_leaf;
        replacement.displaced_leaf = ".gnfs-test-writer-displaced-private-directory";
        break;
    case WriterChildScenario::base_lock_replacement_sandwich:
        replacement.leaf = names.base_lock_leaf;
        replacement.displaced_leaf = ".gnfs-test-writer-displaced-base-lock";
        break;
    case WriterChildScenario::happy:
    case WriterChildScenario::zero_row:
    case WriterChildScenario::handoff_cache_commit_failure:
    case WriterChildScenario::handoff_pending_durable:
    case WriterChildScenario::handoff_canonical_promoted:
    case WriterChildScenario::handoff_canonical_durable:
    case WriterChildScenario::handoff_reserved_revoked_durable:
    case WriterChildScenario::wrong_base_path_digest:
    case WriterChildScenario::foreign_staging_residue:
    case WriterChildScenario::post_authority_base_lock_replacement:
    case WriterChildScenario::post_authority_reserved_replacement:
    case WriterChildScenario::post_authority_private_directory_replacement:
    case WriterChildScenario::construction_clean_rollback:
    case WriterChildScenario::construction_foreign_index_residue:
    case WriterChildScenario::real_worker_execution:
    case WriterChildScenario::predecessor_handoff_interleaving:
        break;
    }
}

void configure_post_authority_replacement(WriterChildScenario scenario,
                                          const wave::DistributedSieveWorkerAttemptNamesV1& names,
                                          ReplacementHookContext& replacement) {
    switch (scenario) {
    case WriterChildScenario::post_authority_base_lock_replacement:
        replacement.leaf = names.base_lock_leaf;
        replacement.displaced_leaf = ".gnfs-test-writer-post-displaced-base-lock";
        break;
    case WriterChildScenario::post_authority_reserved_replacement:
        replacement.leaf = names.reserved_leaf;
        replacement.displaced_leaf = ".gnfs-test-writer-post-displaced-reserved";
        break;
    case WriterChildScenario::post_authority_private_directory_replacement:
        replacement.kind = ReplacementHookContext::Kind::private_directory_replacement;
        replacement.leaf = names.private_directory_leaf;
        replacement.displaced_leaf = ".gnfs-test-writer-post-displaced-private-directory";
        break;
    default:
        fail("post-authority replacement scenario", __LINE__);
    }
}

struct ConstructionFailureHookContext final {
    bool replace_index = false;
    int root_descriptor = -1;
    std::string private_directory_leaf;
    bool invoked = false;
    bool replaced = false;
    int error = 0;
};

[[nodiscard]] bool stop_after_fresh_index_reserved(gnfs::relation::OOCPrivateLeaseFaultPoint point,
                                                   void* opaque) noexcept {
    auto& context = *static_cast<ConstructionFailureHookContext*>(opaque);
    if (point != gnfs::relation::OOCPrivateLeaseFaultPoint::FreshIndexReserved) {
        return false;
    }
    context.invoked = true;
    if (!context.replace_index) {
        return true;
    }

    int directory = -1;
    do {
        directory = ::openat(context.root_descriptor, context.private_directory_leaf.c_str(),
                             O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (directory < 0 && errno == EINTR);
    if (directory < 0) {
        context.error = errno;
        return true;
    }

    context.error = replace_regular_at_same_bytes(directory, "corpus.relidx",
                                                  ".gnfs-test-writer-owned-index-displaced");
    context.replaced = context.error == 0;
    if (::close(directory) != 0 && context.error == 0) {
        context.error = errno;
        context.replaced = false;
    }
    return true;
}

[[nodiscard]] int fork_inherited_authority_probe(WriterResult& converted,
                                                 const Relation& relation) {
    auto& authority = *converted.writer;
    const auto completion = successful_completion(authority);
    const bool invalid = !authority.valid() && !authority.finalized() && authority.count() == 0;
    const bool write_rejected = rejects_writer_mutation([&] { (void)authority.write(relation); });
    const bool finalize_rejected =
        rejects_writer_mutation([&] { (void)authority.finalize_and_publish_handoff(completion); });

    // Exercise the inherited authority destructor before a normal return from
    // main. This intentionally does not use _exit(): buffered native streams
    // in the forked copy must not flush or finalize the parent's corpus.
    converted.writer.reset();
    return invalid && write_rejected && finalize_rejected ? EXIT_SUCCESS : EXIT_FAILURE;
}

[[nodiscard]] int run_writer_child(WriterChildScenario scenario,
                                   bool crash_at_handoff_fault = false) noexcept {
    WriterChildReport report;
    report.scenario = static_cast<std::uint32_t>(scenario);
    try {
        const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
        if (!names.has_value()) {
            (void)write_exact(STDOUT_FILENO, &report, sizeof(report));
            return 90;
        }

        int root_descriptor = -1;
        do {
            root_descriptor = ::fcntl(
                process::DISTRIBUTED_SIEVE_WORKER_CHILD_WAVE_ROOT_DESCRIPTOR, F_DUPFD_CLOEXEC,
                process::DISTRIBUTED_SIEVE_WORKER_CHILD_FIRST_UNMAPPED_DESCRIPTOR);
        } while (root_descriptor < 0 && errno == EINTR);
        if (root_descriptor < 0) {
            report.mutation_error = errno;
            (void)write_exact(STDOUT_FILENO, &report, sizeof(report));
            return 91;
        }

        auto adopted = entry::adopt_distributed_sieve_worker_entry_v1();
        report.entry_status = static_cast<std::uint32_t>(adopted.diagnostic.status);
        report.entry_phase = static_cast<std::uint32_t>(adopted.diagnostic.phase);
        if (all_fixed_descriptors_are_closed() && descriptor_is_closed(STDIN_FILENO)) {
            report.flags |= WRITER_FLAG_FIXED_FDS_CLOSED;
        }
        if (!adopted) {
            (void)::close(root_descriptor);
            (void)write_exact(STDOUT_FILENO, &report, sizeof(report));
            return 0;
        }
        report.flags |= WRITER_FLAG_ENTRY_ADOPTED;

        if (scenario == WriterChildScenario::real_worker_execution) {
            const auto executed = gnfs::sieve::distributed_sieve_worker_execution_detail::
                execute_distributed_sieve_worker_entry_v1(std::move(*adopted.entry));
            report.writer_status = static_cast<std::uint32_t>(executed.diagnostic.status);
            report.writer_phase = static_cast<std::uint32_t>(executed.diagnostic.phase);
            report.writer_native_error = executed.diagnostic.native_error;
            if (executed && executed.handoff.has_value() && executed.completion.has_value()) {
                const auto& handoff = *executed.handoff;
                report.flags |= WRITER_FLAG_REAL_EXECUTION_SUCCEEDED | WRITER_FLAG_FINALIZED |
                                WRITER_FLAG_HANDOFF_PUBLISHED;
                report.chunk_id = handoff.chunk_id;
                report.attempt_ordinal = handoff.attempt_ordinal;
                report.attempt_digest = handoff.attempt_started_digest;
                report.manifest_digest = handoff.manifest_digest;
                report.work_digest = handoff.work_digest;
                report.handoff_digest = handoff.self_digest;
                report.corpus_sha256 = handoff.artifact.corpus_sha256;
                report.count_after_write = handoff.relation_count;
                report.sequence_count = handoff.artifact.sequence_receipt.relation_count;
                report.sequence_low = handoff.artifact.sequence_receipt.low;
                report.sequence_high = handoff.artifact.sequence_receipt.high;
            }
            if (::close(root_descriptor) != 0 && report.mutation_error == 0) {
                report.mutation_error = errno;
            }
            if (!write_exact(STDOUT_FILENO, &report, sizeof(report))) {
                return 92;
            }
            return 0;
        }

        if (scenario == WriterChildScenario::predecessor_handoff_interleaving) {
            // Stop after entry adoption has retained the inherited attempt
            // BaseLock, but before writer construction mutates the private
            // namespace.  The parent uses this exact stable interval to run
            // retry creation through its former final-inventory/O_EXCL race.
            if (::raise(SIGSTOP) != 0) {
                report.mutation_error = errno;
                (void)::close(root_descriptor);
                (void)write_exact(STDOUT_FILENO, &report, sizeof(report));
                return 95;
            }
        }

        ReplacementHookContext replacement;
        configure_writer_replacement(scenario, *names, replacement);
        replacement.root_descriptor = root_descriptor;
        ConstructionFailureHookContext construction{
            .replace_index = scenario == WriterChildScenario::construction_foreign_index_residue,
            .root_descriptor = root_descriptor,
            .private_directory_leaf = names->private_directory_leaf,
        };

        if (scenario == WriterChildScenario::wrong_base_path_digest) {
            report.flags |= WRITER_FLAG_MUTATION_INVOKED;
            report.mutation_error =
                rewrite_private_lease_with_wrong_base_path_digest(root_descriptor, *names);
            if (report.mutation_error == 0) {
                report.flags |= WRITER_FLAG_MUTATION_SUCCEEDED;
            }
        } else if (scenario == WriterChildScenario::foreign_staging_residue) {
            report.flags |= WRITER_FLAG_MUTATION_INVOKED;
            std::string staging_leaf;
            report.mutation_error =
                create_foreign_staging_residue(root_descriptor, *names, staging_leaf);
            if (report.mutation_error == 0) {
                report.flags |= WRITER_FLAG_MUTATION_SUCCEEDED;
            }
        }

        entry::trusted_test::DistributedSieveWorkerWriterTestHooksV1 conversion_hooks;
        if (!replacement.leaf.empty()) {
            conversion_hooks.after_first_validation = replace_after_first_validation;
            conversion_hooks.context = &replacement;
        }
        if (is_construction_failure(scenario)) {
            conversion_hooks.private_lease_hooks = {
                .stop_after = stop_after_fresh_index_reserved,
                .context = &construction,
            };
        }
        const bool use_conversion_hooks =
            !replacement.leaf.empty() || is_construction_failure(scenario);
        WriterResult converted =
            use_conversion_hooks
                ? entry::trusted_test::consume_distributed_sieve_worker_writer_v1_with_hooks(
                      std::move(*adopted.entry), conversion_hooks)
                : entry::consume_distributed_sieve_worker_writer_v1(std::move(*adopted.entry));

        if (!replacement.leaf.empty()) {
            report.flags |= WRITER_FLAG_MUTATION_INVOKED;
            report.mutation_error = replacement.error;
            if (replacement.invoked && replacement.replaced && replacement.error == 0) {
                report.flags |= WRITER_FLAG_MUTATION_SUCCEEDED;
            }
        }
        if (is_construction_failure(scenario)) {
            report.flags |= WRITER_FLAG_MUTATION_INVOKED;
            report.mutation_error = construction.error;
            if (construction.invoked) {
                report.flags |= WRITER_FLAG_CONSTRUCTION_HOOK_INVOKED;
            }
            if (construction.invoked && construction.error == 0 &&
                (!construction.replace_index || construction.replaced)) {
                report.flags |= WRITER_FLAG_MUTATION_SUCCEEDED;
            }
        }
        if (!is_post_authority_drift(scenario) && !is_handoff_fault(scenario) &&
            scenario != WriterChildScenario::handoff_cache_commit_failure) {
            if (::close(root_descriptor) != 0 && report.mutation_error == 0) {
                report.mutation_error = errno;
            }
            root_descriptor = -1;
        }

        report.writer_status = static_cast<std::uint32_t>(converted.diagnostic.status);
        report.writer_phase = static_cast<std::uint32_t>(converted.diagnostic.phase);
        report.writer_rollback = static_cast<std::uint32_t>(converted.diagnostic.rollback);
        report.writer_native_error = converted.diagnostic.native_error;
        report.rollback_native_error = converted.diagnostic.rollback_native_error;
        if (converted.diagnostic.reconciliation_required()) {
            report.flags |= WRITER_FLAG_RECONCILIATION_REQUIRED;
        }

        const auto second =
            entry::consume_distributed_sieve_worker_writer_v1(std::move(*adopted.entry));
        report.second_status = static_cast<std::uint32_t>(second.diagnostic.status);
        report.second_phase = static_cast<std::uint32_t>(second.diagnostic.phase);
        if (!second && second.diagnostic.status == WriterStatus::already_consumed &&
            second.diagnostic.phase == WriterPhase::single_use_gate) {
            report.flags |= WRITER_FLAG_SECOND_CONSUME_REJECTED;
        }

        if (converted) {
            report.flags |= WRITER_FLAG_AUTHORITY_READY;
            auto& authority = *converted.writer;
            report.chunk_id = authority.record().chunk_id;
            report.attempt_ordinal = authority.record().attempt_ordinal;
            report.attempt_digest = authority.record().self_digest;
            report.manifest_digest = authority.manifest().self_digest;
            const auto digest = sieve::distributed_sieve_work_digest(authority.identity());
            if (digest && digest.digest.has_value()) {
                report.work_digest = *digest.digest;
            }
            if (authority.valid() && !authority.finalized() && authority.count() == 0 &&
                authority.record().manifest_digest == authority.manifest().self_digest &&
                authority.chunk().chunk_id == authority.record().chunk_id &&
                authority.chunk().sq_begin == authority.record().sq_begin &&
                authority.chunk().sq_end == authority.record().sq_end && digest &&
                digest.digest.has_value() && *digest.digest == authority.manifest().work_sha256 &&
                authority.witness().work_sha256 == authority.manifest().work_sha256) {
                report.flags |= WRITER_FLAG_BINDINGS_VALID;
            }

            const Relation first_relation = make_writer_relation(31, 7, 10);
            const Relation second_relation = make_writer_relation(32, 8, 11);
            if (is_post_authority_drift(scenario)) {
                ReplacementHookContext post_authority;
                configure_post_authority_replacement(scenario, *names, post_authority);
                post_authority.root_descriptor = root_descriptor;
                replace_after_first_validation(&post_authority);
                report.flags |= WRITER_FLAG_MUTATION_INVOKED;
                report.mutation_error = post_authority.error;
                if (post_authority.invoked && post_authority.replaced &&
                    post_authority.error == 0) {
                    report.flags |= WRITER_FLAG_MUTATION_SUCCEEDED;
                }

#if defined(__APPLE__)
                const bool finalize_before_write =
                    scenario == WriterChildScenario::post_authority_private_directory_replacement;
#else
                constexpr bool finalize_before_write = false;
#endif
                if (finalize_before_write) {
                    const auto completion = successful_completion(authority);
                    if (rejects_writer_runtime_failure(
                            [&] { (void)authority.finalize_and_publish_handoff(completion); })) {
                        report.flags |= WRITER_FLAG_POST_AUTHORITY_FINALIZE_REJECTED;
                    }
                    if (!authority.valid() && !authority.finalized() && authority.count() == 0) {
                        report.flags |= WRITER_FLAG_POST_AUTHORITY_INVALIDATED;
                    }
                    if (rejects_writer_mutation([&] { (void)authority.write(first_relation); })) {
                        report.flags |= WRITER_FLAG_POST_AUTHORITY_WRITE_REJECTED;
                    }
                } else {
                    if (rejects_writer_runtime_failure(
                            [&] { (void)authority.write(first_relation); })) {
                        report.flags |= WRITER_FLAG_POST_AUTHORITY_WRITE_REJECTED;
                    }
                    if (!authority.valid() && !authority.finalized() && authority.count() == 0) {
                        report.flags |= WRITER_FLAG_POST_AUTHORITY_INVALIDATED;
                    }
                    const auto completion = successful_completion(authority);
                    if (rejects_writer_mutation(
                            [&] { (void)authority.finalize_and_publish_handoff(completion); })) {
                        report.flags |= WRITER_FLAG_POST_AUTHORITY_FINALIZE_REJECTED;
                    }
                }
            } else if (scenario == WriterChildScenario::handoff_cache_commit_failure) {
                const bool first_written =
                    authority.write(first_relation) == 0 && authority.count() == 1;
                const bool second_written =
                    authority.write(second_relation) == 1 && authority.count() == 2;
                if (first_written && second_written) {
                    report.flags |= WRITER_FLAG_WROTE_EXACT_PAIR;
                    report.count_after_write = static_cast<std::uint64_t>(authority.count());
                }

                const auto completion = successful_completion(authority);
                try {
                    (void)entry::trusted_test::
                        finalize_and_publish_distributed_sieve_worker_handoff_v1_with_hooks(
                            authority, completion,
                            {
                                .fail_before_retry_cache_commit = true,
                            });
                } catch (const std::bad_alloc&) {
                    const auto unpublished =
                        observe_worker_handoff_prefix(root_descriptor, *names, scenario);
                    if (authority.valid() && authority.finalized() &&
                        !authority.handoff_published() && authority.count() == 2 &&
                        unpublished.exact_shape && unpublished.no_cleanup_artifact) {
                        report.flags |= WRITER_FLAG_HANDOFF_CACHE_FAILURE;
                    }
                    if (unpublished.error != 0) {
                        report.mutation_error = unpublished.error;
                    }
                }

                const auto handoff = authority.finalize_and_publish_handoff(completion);
                report.handoff_digest = handoff.self_digest;
                if (authority.valid() && authority.finalized() && authority.handoff_published() &&
                    authority.count() == 2) {
                    report.flags |= WRITER_FLAG_FINALIZED | WRITER_FLAG_HANDOFF_PUBLISHED |
                                    WRITER_FLAG_HANDOFF_CACHE_RETRY_SUCCEEDED;
                }
            } else if (is_handoff_fault(scenario)) {
                const bool first_written =
                    authority.write(first_relation) == 0 && authority.count() == 1;
                const bool second_written =
                    authority.write(second_relation) == 1 && authority.count() == 2;
                if (first_written && second_written) {
                    report.flags |= WRITER_FLAG_WROTE_EXACT_PAIR;
                    report.count_after_write = static_cast<std::uint64_t>(authority.count());
                }

                const auto completion = successful_completion(authority);
                WorkerHandoffStopContext stop{
                    .target = handoff_fault_point(scenario),
                    .terminate_process = crash_at_handoff_fault,
                };
                bool interrupted = false;
                try {
                    (void)entry::trusted_test::
                        finalize_and_publish_distributed_sieve_worker_handoff_v1_with_hooks(
                            authority, completion,
                            {
                                .private_handoff_hooks =
                                    {
                                        .stop_after = stop_worker_handoff,
                                        .context = &stop,
                                    },
                            });
                } catch (const std::system_error& error) {
                    interrupted =
                        stop.invoked &&
                        error.code() == std::make_error_code(std::errc::operation_canceled);
                    report.writer_native_error = error.code().value();
                }
                if (interrupted && authority.valid() && authority.finalized() &&
                    !authority.handoff_published() && authority.count() == 2) {
                    report.flags |= WRITER_FLAG_HANDOFF_INTERRUPTED;
                }

                const auto prefix_before_drift =
                    observe_worker_handoff_prefix(root_descriptor, *names, scenario);
#if defined(__APPLE__)
                const auto namespace_before_drift =
                    snapshot_namespace(snapshot_root_path(root_descriptor));
#endif
                if (prefix_before_drift.exact_shape) {
                    report.flags |= WRITER_FLAG_HANDOFF_PREFIX_OBSERVED;
                }
                if (prefix_before_drift.no_cleanup_artifact) {
                    report.flags |= WRITER_FLAG_HANDOFF_PREFIX_NO_CLEANUP;
                }
                if (prefix_before_drift.error != 0) {
                    report.mutation_error = prefix_before_drift.error;
                }

                auto drifted = completion;
                drifted.processed_sq_count = 2;
                try {
                    (void)authority.finalize_and_publish_handoff(drifted);
                } catch (const std::invalid_argument&) {
                    const auto prefix_after_drift =
                        observe_worker_handoff_prefix(root_descriptor, *names, scenario);
#if defined(__APPLE__)
                    const auto namespace_after_drift =
                        snapshot_namespace(snapshot_root_path(root_descriptor));
                    if (prefix_after_drift == prefix_before_drift &&
                        same_namespace_snapshot(namespace_before_drift, namespace_after_drift)) {
#else
                    if (prefix_after_drift == prefix_before_drift) {
#endif
                        report.flags |= WRITER_FLAG_HANDOFF_RETRY_DRIFT_REJECTED;
                    }
                }

                const auto handoff = authority.finalize_and_publish_handoff(completion);
                report.handoff_digest = handoff.self_digest;
                report.corpus_sha256 = handoff.artifact.corpus_sha256;
                report.sequence_count = handoff.artifact.sequence_receipt.relation_count;
                report.sequence_low = handoff.artifact.sequence_receipt.low;
                report.sequence_high = handoff.artifact.sequence_receipt.high;
                if (authority.valid() && authority.finalized() && authority.handoff_published() &&
                    authority.count() == 2) {
                    report.flags |= WRITER_FLAG_FINALIZED | WRITER_FLAG_HANDOFF_PUBLISHED |
                                    WRITER_FLAG_HANDOFF_RETRY_SUCCEEDED;
                }
                const bool repeated = rejects_writer_mutation(
                    [&] { (void)authority.finalize_and_publish_handoff(completion); });
                if (repeated) {
                    report.flags |= WRITER_FLAG_POST_FINALIZE_REJECTED;
                }
            } else if (scenario == WriterChildScenario::zero_row ||
                       scenario == WriterChildScenario::predecessor_handoff_interleaving) {
                const auto handoff =
                    authority.finalize_and_publish_handoff(successful_completion(authority));
                report.handoff_digest = handoff.self_digest;
                report.corpus_sha256 = handoff.artifact.corpus_sha256;
                report.sequence_count = handoff.artifact.sequence_receipt.relation_count;
                report.sequence_low = handoff.artifact.sequence_receipt.low;
                report.sequence_high = handoff.artifact.sequence_receipt.high;
                if (authority.valid() && authority.finalized() && authority.handoff_published() &&
                    authority.count() == 0) {
                    report.flags |= WRITER_FLAG_FINALIZED | WRITER_FLAG_HANDOFF_PUBLISHED |
                                    WRITER_FLAG_ZERO_ROW;
                }
                const bool write_rejected =
                    rejects_writer_mutation([&] { (void)authority.write(first_relation); });
                const bool finalize_rejected = rejects_writer_mutation([&] {
                    (void)authority.finalize_and_publish_handoff(successful_completion(authority));
                });
                if (write_rejected && finalize_rejected) {
                    report.flags |= WRITER_FLAG_POST_FINALIZE_REJECTED;
                }
            } else if (scenario == WriterChildScenario::happy) {
#if !defined(__APPLE__)
                const bool first_written =
                    authority.write(first_relation) == 0 && authority.count() == 1;
                const bool second_written =
                    authority.write(second_relation) == 1 && authority.count() == 2;
                if (first_written && second_written) {
                    report.flags |= WRITER_FLAG_WROTE_EXACT_PAIR;
                    report.count_after_write = static_cast<std::uint64_t>(authority.count());
                }
                try {
                    (void)authority.finalize_and_publish_handoff(successful_completion(authority));
                } catch (const std::system_error& error) {
                    report.writer_native_error = error.code().value();
                    if (error.code() == std::make_error_code(std::errc::operation_not_supported) &&
                        authority.valid() && !authority.finalized() &&
                        !authority.handoff_published() && authority.count() == 2 &&
                        authority.write(make_writer_relation(33, 9, 12)) == 2 &&
                        authority.count() == 3) {
                        report.flags |= WRITER_FLAG_HANDOFF_PLATFORM_UNSUPPORTED;
                        report.flags |= WRITER_FLAG_HANDOFF_APPEND_PRESERVED;
                        report.count_after_write = static_cast<std::uint64_t>(authority.count());
                    }
                }
#else
                // Keep one relation buffered across fork. A child-side normal
                // return must purge, rather than flush, this inherited 1 MB
                // stdio buffer before the parent appends the second relation.
                const bool first_written =
                    authority.write(first_relation) == 0 && authority.count() == 1;
                const pid_t forked = ::fork();
                if (forked == 0) {
                    return fork_inherited_authority_probe(converted, first_relation);
                }
                if (forked < 0) {
                    report.mutation_error = errno;
                } else {
                    int status = 0;
                    pid_t waited = -1;
                    do {
                        waited = ::waitpid(forked, &status, 0);
                    } while (waited < 0 && errno == EINTR);
                    if (waited == forked && WIFEXITED(status) &&
                        WEXITSTATUS(status) == EXIT_SUCCESS) {
                        report.flags |= WRITER_FLAG_FORK_WRITE_REJECTED |
                                        WRITER_FLAG_FORK_FINALIZE_REJECTED |
                                        WRITER_FLAG_FORK_NORMAL_RETURN;
                    }
                }

                if (first_written && authority.valid() && !authority.finalized() &&
                    authority.count() == 1) {
                    report.flags |= WRITER_FLAG_PARENT_UNCHANGED_AFTER_FORK;
                }
                try {
                    (void)authority.finalize_and_publish_handoff({
                        .processed_sq_count = 1,
                        .next_sq_index =
                            static_cast<std::uint32_t>(authority.record().sq_begin + 1U),
                        .completion_reason = sieve::WorkerCompletionReasonV1::sq_cap,
                    });
                } catch (const std::invalid_argument&) {
                    if (authority.valid() && !authority.finalized() &&
                        !authority.handoff_published() && authority.count() == 1) {
                        report.flags |= WRITER_FLAG_HANDOFF_MANIFEST_CAP_REJECTED;
                    }
                }
                if (first_written && authority.write(second_relation) == 1 &&
                    authority.count() == 2) {
                    report.flags |= WRITER_FLAG_WROTE_EXACT_PAIR;
                    report.count_after_write = static_cast<std::uint64_t>(authority.count());
                }
                const auto handoff =
                    authority.finalize_and_publish_handoff(successful_completion(authority));
                report.handoff_digest = handoff.self_digest;
                report.corpus_sha256 = handoff.artifact.corpus_sha256;
                report.sequence_count = handoff.artifact.sequence_receipt.relation_count;
                report.sequence_low = handoff.artifact.sequence_receipt.low;
                report.sequence_high = handoff.artifact.sequence_receipt.high;
                if (authority.valid() && authority.finalized() && authority.handoff_published() &&
                    authority.count() == 2) {
                    report.flags |= WRITER_FLAG_FINALIZED;
                    report.flags |= WRITER_FLAG_HANDOFF_PUBLISHED;
                }
                const bool write_rejected =
                    rejects_writer_mutation([&] { (void)authority.write(first_relation); });
                const bool finalize_rejected = rejects_writer_mutation([&] {
                    (void)authority.finalize_and_publish_handoff(successful_completion(authority));
                });
                if (write_rejected && finalize_rejected) {
                    report.flags |= WRITER_FLAG_POST_FINALIZE_REJECTED;
                }
#endif
            }
        }

        if (root_descriptor >= 0) {
            if (::close(root_descriptor) != 0 && report.mutation_error == 0) {
                report.mutation_error = errno;
            }
            root_descriptor = -1;
        }
        if (!write_exact(STDOUT_FILENO, &report, sizeof(report))) {
            return 92;
        }
        return 0;
    } catch (...) {
        (void)write_exact(STDOUT_FILENO, &report, sizeof(report));
        return 93;
    }
}

[[nodiscard]] WriterChildReport read_writer_child_report(int descriptor) {
    WriterChildReport report;
    CHECK(read_exact(descriptor, &report, sizeof(report)));
    std::byte trailing{};
    ssize_t received = -1;
    do {
        received = ::read(descriptor, &trailing, 1);
    } while (received < 0 && errno == EINTR);
    CHECK(received == 0);
    CHECK(report.magic == WRITER_CHILD_REPORT_MAGIC);
    return report;
}

#endif

struct LaunchedWriterCaseResult final {
    WriterChildReport report;
    sieve::AttemptStartedV1 record;
#if !defined(_WIN32)
    NamespaceSnapshot baseline;
#endif
};

template <typename Fixture>
[[nodiscard]] LaunchedWriterCaseResult launch_writer_case(Fixture& fixture,
                                                          const std::filesystem::path& executable,
                                                          WriterChildScenario scenario) {
#if defined(_WIN32)
    (void)fixture;
    (void)executable;
    (void)scenario;
    throw TestFailure("worker-writer self-exec fixture is unavailable on Windows");
#else
    auto receipt = fixture.start_receipt();
    const auto record = receipt.record();
    auto baseline = snapshot_namespace(fixture.root);
    std::vector<launcher::DistributedSieveWorkerLaunchSlotV1> slots;
    slots.emplace_back(std::move(receipt), std::vector<std::string>{
                                               std::string(WRITER_CHILD_ARGUMENT),
                                               std::to_string(static_cast<std::uint32_t>(scenario)),
                                           });
    launcher::DistributedSieveWorkerLaunchRequestV1 request(executable.string(), std::move(slots));
    auto launched = fixture.store().launch_worker_process_batch_v1(
        std::move(request), fixture.identity, fixture.frozen, fixture.polynomial,
        fixture.factor_base);
    CHECK(launched);
    CHECK(launched.children.size() == 1);
    CHECK(launched.children[0]);
    CHECK(launched.children[0].worker.has_value());
    auto& worker = *launched.children[0].worker;
    const auto waited = worker.wait_terminal();
    CHECK(waited.reaped);
    CHECK(waited.success);
    CHECK(waited.exit_status == 0);
    const int report_descriptor = worker.release_report_descriptor();
    CHECK(report_descriptor >= 0);
    const auto report = read_writer_child_report(report_descriptor);
    CHECK(::close(report_descriptor) == 0);
    CHECK(report.scenario == static_cast<std::uint32_t>(scenario));
    return {
        .report = report,
        .record = record,
        .baseline = std::move(baseline),
    };
#endif
}

void require_writer_entry_was_adopted(const WriterChildReport& report) {
    CHECK((report.flags & WRITER_FLAG_ENTRY_ADOPTED) != 0U);
    CHECK((report.flags & WRITER_FLAG_FIXED_FDS_CLOSED) != 0U);
    CHECK(report.entry_status ==
          static_cast<std::uint32_t>(entry::DistributedSieveWorkerEntryStatusV1::ready));
}

void require_writer_rejection(const WriterChildReport& report, WriterPhase phase,
                              WriterStatus status) {
    require_writer_entry_was_adopted(report);
    CHECK((report.flags & WRITER_FLAG_MUTATION_INVOKED) != 0U);
    CHECK((report.flags & WRITER_FLAG_MUTATION_SUCCEEDED) != 0U);
    CHECK((report.flags & WRITER_FLAG_AUTHORITY_READY) == 0U);
    CHECK((report.flags & WRITER_FLAG_SECOND_CONSUME_REJECTED) != 0U);
    CHECK(report.mutation_error == 0);
    CHECK(report.writer_phase == static_cast<std::uint32_t>(phase));
    CHECK(report.writer_status == static_cast<std::uint32_t>(status));
    CHECK(report.second_phase == static_cast<std::uint32_t>(WriterPhase::single_use_gate));
    CHECK(report.second_status == static_cast<std::uint32_t>(WriterStatus::already_consumed));
}

void require_post_authority_drift_rejection(const WriterChildReport& report) {
    require_writer_entry_was_adopted(report);
    CHECK((report.flags & WRITER_FLAG_AUTHORITY_READY) != 0U);
    CHECK((report.flags & WRITER_FLAG_BINDINGS_VALID) != 0U);
    CHECK((report.flags & WRITER_FLAG_SECOND_CONSUME_REJECTED) != 0U);
    CHECK((report.flags & WRITER_FLAG_MUTATION_INVOKED) != 0U);
    CHECK((report.flags & WRITER_FLAG_MUTATION_SUCCEEDED) != 0U);
    CHECK((report.flags & WRITER_FLAG_POST_AUTHORITY_WRITE_REJECTED) != 0U);
    CHECK((report.flags & WRITER_FLAG_POST_AUTHORITY_INVALIDATED) != 0U);
    CHECK((report.flags & WRITER_FLAG_POST_AUTHORITY_FINALIZE_REJECTED) != 0U);
    CHECK((report.flags & WRITER_FLAG_FINALIZED) == 0U);
    CHECK((report.flags & WRITER_FLAG_RECONCILIATION_REQUIRED) == 0U);
    CHECK(report.mutation_error == 0);
    CHECK(report.writer_phase == static_cast<std::uint32_t>(WriterPhase::writer_creation));
    CHECK(report.writer_status == static_cast<std::uint32_t>(WriterStatus::ready));
    CHECK(report.writer_rollback == static_cast<std::uint32_t>(WriterRollback::not_applicable));
    CHECK(report.count_after_write == 0);
}

void require_construction_failure(const WriterChildReport& report, WriterRollback rollback,
                                  bool reconciliation_required) {
    require_writer_entry_was_adopted(report);
    CHECK((report.flags & WRITER_FLAG_AUTHORITY_READY) == 0U);
    CHECK((report.flags & WRITER_FLAG_SECOND_CONSUME_REJECTED) != 0U);
    CHECK((report.flags & WRITER_FLAG_MUTATION_INVOKED) != 0U);
    CHECK((report.flags & WRITER_FLAG_MUTATION_SUCCEEDED) != 0U);
    CHECK((report.flags & WRITER_FLAG_CONSTRUCTION_HOOK_INVOKED) != 0U);
    CHECK(((report.flags & WRITER_FLAG_RECONCILIATION_REQUIRED) != 0U) == reconciliation_required);
    CHECK(report.mutation_error == 0);
    CHECK(report.writer_phase == static_cast<std::uint32_t>(WriterPhase::writer_creation));
    CHECK(report.writer_status == static_cast<std::uint32_t>(WriterStatus::writer_failed));
    CHECK(report.writer_rollback == static_cast<std::uint32_t>(rollback));
    CHECK(report.writer_native_error == ECANCELED);
    CHECK(report.second_phase == static_cast<std::uint32_t>(WriterPhase::single_use_gate));
    CHECK(report.second_status == static_cast<std::uint32_t>(WriterStatus::already_consumed));
}

#if defined(__APPLE__)
struct CrashedWriterHandoffCaseResult final {
    sieve::AttemptStartedV1 record;
    NamespaceSnapshot crashed_namespace;
};

[[nodiscard]] CrashedWriterHandoffCaseResult
launch_writer_handoff_crash(WorkerEntryFixture& fixture, const std::filesystem::path& executable,
                            WriterChildScenario scenario) {
    CHECK(is_handoff_fault(scenario));
    auto receipt = fixture.start_receipt();
    const auto record = receipt.record();

    std::vector<launcher::DistributedSieveWorkerLaunchSlotV1> slots;
    slots.emplace_back(std::move(receipt), std::vector<std::string>{
                                               std::string(WRITER_COLD_CRASH_CHILD_ARGUMENT),
                                               std::to_string(static_cast<std::uint32_t>(scenario)),
                                           });
    launcher::DistributedSieveWorkerLaunchRequestV1 request(executable.string(), std::move(slots));
    auto launched = fixture.store().launch_worker_process_batch_v1(
        std::move(request), fixture.identity, fixture.frozen, fixture.polynomial,
        fixture.factor_base);
    CHECK(launched);
    CHECK(launched.children.size() == 1);
    CHECK(launched.children[0]);
    CHECK(launched.children[0].worker.has_value());
    auto& worker = *launched.children[0].worker;
    const auto waited = worker.wait_terminal();
    CHECK(waited.reaped);
    CHECK(!waited.success);
    CHECK(waited.exit_status ==
          WRITER_HANDOFF_CRASH_EXIT_BASE + static_cast<int>(handoff_fault_point(scenario)));
    CHECK(waited.signal == 0);
    CHECK(waited.native_error == 0);

    const int report_descriptor = worker.release_report_descriptor();
    CHECK(report_descriptor >= 0);
    std::byte trailing{};
    ssize_t received = -1;
    do {
        received = ::read(report_descriptor, &trailing, 1);
    } while (received < 0 && errno == EINTR);
    CHECK(received == 0);
    CHECK(::close(report_descriptor) == 0);

    int root_descriptor = -1;
    do {
        root_descriptor = ::open(fixture.root.c_str(),
                                 O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (root_descriptor < 0 && errno == EINTR);
    CHECK(root_descriptor >= 0);
    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    const auto prefix = observe_worker_handoff_prefix(root_descriptor, *names, scenario);
    CHECK(::close(root_descriptor) == 0);
    CHECK(prefix.error == 0);
    CHECK(prefix.exact_shape);
    CHECK(prefix.no_cleanup_artifact);

    return {
        .record = record,
        .crashed_namespace = snapshot_namespace(fixture.root),
    };
}

template <typename Fixture>
void fresh_reopen_worker_handoff(Fixture& fixture, const Digest& manifest_digest,
                                 wave::DistributedSieveWaveStoreTestHooks hooks = {}) {
    // Keep the cold-recovery production dependency at this one boundary. When
    // WaveStore forwards the relation-layer retained-prefix sandwich hook, the
    // test-only adapter belongs here rather than in individual cases.
    fixture.opened.store.reset();
    fixture.opened = wave::DistributedSieveWaveStore::open(fixture.root, manifest_digest, hooks);
}

template <typename Fixture>
void fresh_reopen_worker_handoff(Fixture& fixture,
                                 wave::DistributedSieveWaveStoreTestHooks hooks = {}) {
    const Digest manifest_digest = fixture.store().manifest_digest();
    fresh_reopen_worker_handoff(fixture, manifest_digest, hooks);
}

struct ColdHandoffSameByteReplacementContext final {
    std::filesystem::path private_directory;
    std::string leaf;
    std::string displaced_leaf;
    bool invoked = false;
    int error = 0;
    LeafFingerprint replacement;
};

[[nodiscard]] bool replace_cold_handoff_after_expected_prefix_validation(
    wave::DistributedSieveWorkerHandoffResumeObservationPointV1 point, void* opaque) noexcept {
    if (point !=
        wave::DistributedSieveWorkerHandoffResumeObservationPointV1::AfterExpectedPrefixValidated) {
        return false;
    }
    auto& context = *static_cast<ColdHandoffSameByteReplacementContext*>(opaque);
    context.invoked = true;
    int directory = -1;
    do {
        directory = ::open(context.private_directory.c_str(),
                           O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (directory < 0 && errno == EINTR);
    if (directory < 0) {
        context.error = errno;
        return false;
    }
    context.error =
        replace_regular_at_same_bytes_durable(directory, context.leaf, context.displaced_leaf);
    if (context.error == 0) {
        int fingerprint_error = 0;
        if (!regular_leaf_state_at(directory, context.leaf, true, fingerprint_error,
                                   &context.replacement)) {
            context.error = fingerprint_error != 0 ? fingerprint_error : EPROTO;
        }
    }
    if (::close(directory) != 0 && context.error == 0) {
        context.error = errno;
    }
    return false;
}

class LiveBaseLockHolder final {
public:
    LiveBaseLockHolder() = default;
    LiveBaseLockHolder(pid_t process_id, int release_descriptor) noexcept
        : process_id_(process_id), release_descriptor_(release_descriptor) {}
    LiveBaseLockHolder(const LiveBaseLockHolder&) = delete;
    LiveBaseLockHolder& operator=(const LiveBaseLockHolder&) = delete;
    LiveBaseLockHolder(LiveBaseLockHolder&& other) noexcept
        : process_id_(std::exchange(other.process_id_, -1)),
          release_descriptor_(std::exchange(other.release_descriptor_, -1)) {}
    LiveBaseLockHolder& operator=(LiveBaseLockHolder&&) = delete;

    ~LiveBaseLockHolder() noexcept {
        release_without_checks();
    }

    void release_and_wait() {
        CHECK(process_id_ > 0);
        CHECK(release_descriptor_ >= 0);
        const std::byte release{0x1};
        CHECK(write_exact(release_descriptor_, &release, 1));
        CHECK(::close(release_descriptor_) == 0);
        release_descriptor_ = -1;

        int status = 0;
        pid_t waited = -1;
        do {
            waited = ::waitpid(process_id_, &status, 0);
        } while (waited < 0 && errno == EINTR);
        CHECK(waited == process_id_);
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
        process_id_ = -1;
    }

private:
    void release_without_checks() noexcept {
        if (release_descriptor_ >= 0) {
            (void)::close(release_descriptor_);
            release_descriptor_ = -1;
        }
        if (process_id_ <= 0) {
            return;
        }
        (void)::kill(process_id_, SIGTERM);
        int status = 0;
        pid_t waited = -1;
        do {
            waited = ::waitpid(process_id_, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited != process_id_) {
            (void)::kill(process_id_, SIGKILL);
            do {
                waited = ::waitpid(process_id_, &status, 0);
            } while (waited < 0 && errno == EINTR);
        }
        process_id_ = -1;
    }

    pid_t process_id_ = -1;
    int release_descriptor_ = -1;
};

[[nodiscard]] LiveBaseLockHolder
hold_base_lock_in_clean_child(const std::filesystem::path& base_lock_path) {
    std::array<int, 2> release_pipe{-1, -1};
    std::array<int, 2> ready_pipe{-1, -1};
    CHECK(::pipe(release_pipe.data()) == 0);
    CHECK(::pipe(ready_pipe.data()) == 0);

    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)::close(release_pipe[1]);
        (void)::close(ready_pipe[0]);
        if (::dup2(release_pipe[0], STDIN_FILENO) < 0 || ::dup2(ready_pipe[1], STDOUT_FILENO) < 0) {
            ::_exit(92);
        }
        const long maximum = ::sysconf(_SC_OPEN_MAX);
        const int last = maximum > 3 && maximum <= std::numeric_limits<int>::max()
                             ? static_cast<int>(maximum)
                             : 4096;
        for (int descriptor = 3; descriptor < last; ++descriptor) {
            (void)::close(descriptor);
        }

        int base_lock = -1;
        do {
            base_lock =
                ::open(base_lock_path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        } while (base_lock < 0 && errno == EINTR);
        int failure = base_lock < 0 ? errno : 0;
        if (failure == 0 && ::flock(base_lock, LOCK_EX | LOCK_NB) != 0) {
            failure = errno;
        }
        if (!write_exact(STDOUT_FILENO, &failure, sizeof(failure)) || failure != 0) {
            ::_exit(93);
        }
        std::byte release{};
        const bool released = read_exact(STDIN_FILENO, &release, 1);
        if (base_lock >= 0) {
            (void)::close(base_lock);
        }
        ::_exit(released ? 0 : 94);
    }

    CHECK(::close(release_pipe[0]) == 0);
    CHECK(::close(ready_pipe[1]) == 0);
    int child_failure = 0;
    const bool ready = read_exact(ready_pipe[0], &child_failure, sizeof(child_failure));
    CHECK(::close(ready_pipe[0]) == 0);
    if (!ready || child_failure != 0) {
        (void)::close(release_pipe[1]);
        int status = 0;
        pid_t waited = -1;
        do {
            waited = ::waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        fail("acquire live attempt BaseLock in clean child", __LINE__,
             ready ? std::generic_category().message(child_failure)
                   : "child readiness report truncated");
    }
    return LiveBaseLockHolder(child, release_pipe[1]);
}

void test_retry_creation_serializes_with_predecessor_handoff(
    const std::filesystem::path& executable) {
    WorkerEntryFixture fixture("writer-predecessor-handoff-interleaving");
    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 1);
    CHECK(names.has_value());
    WriterChildReport report;

    {
        auto receipt = fixture.start_receipt();
        std::vector<launcher::DistributedSieveWorkerLaunchSlotV1> slots;
        slots.emplace_back(std::move(receipt),
                           std::vector<std::string>{
                               std::string(WRITER_CHILD_ARGUMENT),
                               std::to_string(static_cast<std::uint32_t>(
                                   WriterChildScenario::predecessor_handoff_interleaving)),
                           });
        launcher::DistributedSieveWorkerLaunchRequestV1 request(executable.string(),
                                                                std::move(slots));
        auto launched = fixture.store().launch_worker_process_batch_v1(
            std::move(request), fixture.identity, fixture.frozen, fixture.polynomial,
            fixture.factor_base);
        CHECK(launched);
        CHECK(launched.children.size() == 1);
        CHECK(launched.children[0]);
        CHECK(launched.children[0].worker.has_value());
        auto& worker = *launched.children[0].worker;
        const pid_t child = worker.process_id();
        CHECK(child > 0);

        struct ContinueStoppedChild final {
            pid_t process_id = -1;
            bool armed = true;
            ~ContinueStoppedChild() noexcept {
                if (armed && process_id > 0) {
                    (void)::kill(process_id, SIGCONT);
                }
            }
        } continuation{.process_id = child};

        int stopped_status = 0;
        pid_t stopped = -1;
        do {
            stopped = ::waitpid(child, &stopped_status, WUNTRACED);
        } while (stopped < 0 && errno == EINTR);
        CHECK(stopped == child);
        CHECK(WIFSTOPPED(stopped_status));
        CHECK(WSTOPSIG(stopped_status) == SIGSTOP);

        const auto before_retry = snapshot_namespace(fixture.root);
        bool final_window_entered = false;
        auto blocked = fixture.store().create_worker_attempt_private_lease_root(
            0, 1,
            wave::DistributedSievePrivateLeaseBaseLockTestHooks{
                .after_initial_phase_validation =
                    [](void* context) noexcept { *static_cast<bool*>(context) = true; },
                .context = &final_window_entered,
            });
        CHECK(final_window_entered);
        CHECK(!blocked);
        CHECK(blocked.claim == nullptr);
        CHECK(blocked.diagnostic.status ==
              wave::DistributedSieveWaveStoreStatus::private_lease_lock_busy);
        const auto after_retry = snapshot_namespace(fixture.root);
        CHECK(same_namespace_snapshot(before_retry, after_retry));
        CHECK(!after_retry.contains(names->base_lock_leaf));

        CHECK(::kill(child, SIGCONT) == 0);
        continuation.armed = false;
        const auto waited = worker.wait_terminal();
        CHECK(waited.reaped);
        CHECK(waited.success);
        CHECK(waited.exit_status == 0);
        const int report_descriptor = worker.release_report_descriptor();
        CHECK(report_descriptor >= 0);
        report = read_writer_child_report(report_descriptor);
        CHECK(::close(report_descriptor) == 0);
    }

    CHECK(report.scenario ==
          static_cast<std::uint32_t>(WriterChildScenario::predecessor_handoff_interleaving));
    require_writer_entry_was_adopted(report);
    CHECK(report.writer_status == static_cast<std::uint32_t>(WriterStatus::ready));
    CHECK((report.flags & WRITER_FLAG_AUTHORITY_READY) != 0U);
    CHECK((report.flags & WRITER_FLAG_FINALIZED) != 0U);
    CHECK((report.flags & WRITER_FLAG_HANDOFF_PUBLISHED) != 0U);
    CHECK((report.flags & WRITER_FLAG_ZERO_ROW) != 0U);
    CHECK(!snapshot_namespace(fixture.root).contains(names->base_lock_leaf));

    require_wave_ready(fixture.store().revalidate(),
                       "handoff winner remains a valid terminal attempt chain");
    const Digest manifest_digest = fixture.store().manifest_digest();
    fixture.opened.store.reset();
    fixture.opened = wave::DistributedSieveWaveStore::open(fixture.root, manifest_digest);
    CHECK(fixture.opened);
    require_wave_ready(fixture.store().revalidate(),
                       "reopened handoff winner remains a valid terminal attempt chain");
    CHECK(!snapshot_namespace(fixture.root).contains(names->base_lock_leaf));
}
#endif

void test_writer_authority_happy_path(const std::filesystem::path& executable) {
    WorkerEntryFixture fixture("writer-happy");
    const auto result = launch_writer_case(fixture, executable, WriterChildScenario::happy);
#if !defined(__APPLE__)
    require_writer_entry_was_adopted(result.report);
    CHECK((result.report.flags & WRITER_FLAG_AUTHORITY_READY) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_BINDINGS_VALID) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_SECOND_CONSUME_REJECTED) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_WROTE_EXACT_PAIR) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_HANDOFF_PLATFORM_UNSUPPORTED) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_HANDOFF_APPEND_PRESERVED) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_FINALIZED) == 0U);
    CHECK((result.report.flags & WRITER_FLAG_HANDOFF_PUBLISHED) == 0U);
    CHECK(result.report.count_after_write == 3);
    CHECK(result.report.writer_native_error ==
          std::make_error_code(std::errc::operation_not_supported).value());
    require_exact_unsupported_namespace_delta(fixture, result.baseline);
    require_no_handoff_or_cleanup_publication(fixture);
#else
    require_writer_entry_was_adopted(result.report);
    CHECK(result.report.writer_phase == static_cast<std::uint32_t>(WriterPhase::writer_creation));
    CHECK(result.report.writer_status == static_cast<std::uint32_t>(WriterStatus::ready));
    CHECK((result.report.flags & WRITER_HAPPY_FLAGS) == WRITER_HAPPY_FLAGS);
    CHECK(result.report.count_after_write == 2);
    CHECK(result.report.chunk_id == 0);
    CHECK(result.report.attempt_ordinal == 0);
    CHECK(result.report.attempt_digest == result.record.self_digest);
    CHECK(result.report.manifest_digest == fixture.store().manifest_digest());
    CHECK(result.report.work_digest == work_digest(fixture.identity));

    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    const auto base = fixture.root / names->private_directory_leaf / "corpus";
    const Relation first = make_writer_relation(31, 7, 10);
    const Relation second = make_writer_relation(32, 8, 11);
    const std::vector<Relation> expected_relations{first, second};
    const auto expected_sequence = gnfs::relation::relation_sequence_receipt(expected_relations);
    const auto expected_corpus_sha256 =
        gnfs::relation::relation_corpus_sha256_v1(expected_relations);
    CHECK(expected_corpus_sha256.has_value());

    const auto inspected = gnfs::relation::OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.canonical());
    CHECK(inspected.record.has_value());
    const auto& envelope = *inspected.record;
    CHECK(envelope.payload_kind ==
          static_cast<std::uint32_t>(sieve::DistributedSieveRecordKindV1::worker_handoff));
    CHECK(envelope.payload_version == 1);
    const auto decoded = sieve::decode_distributed_sieve_record(envelope.opaque_payload);
    CHECK(decoded);
    CHECK(decoded.value.has_value());
    const auto* handoff = std::get_if<sieve::WorkerHandoffV1>(&*decoded.value);
    CHECK(handoff != nullptr);
    CHECK(handoff->self_digest == result.report.handoff_digest);
    CHECK(handoff->manifest_digest == fixture.store().manifest_digest());
    CHECK(handoff->work_digest == work_digest(fixture.identity));
    CHECK(handoff->attempt_started_digest == result.record.self_digest);
    CHECK(handoff->lease == result.record.lease);
    CHECK(handoff->chunk_id == result.record.chunk_id);
    CHECK(handoff->sq_begin == result.record.sq_begin);
    CHECK(handoff->sq_end == result.record.sq_end);
    CHECK(handoff->next_sq_index == result.record.sq_end);
    CHECK(handoff->processed_sq_count == 1);
    CHECK(handoff->completion_reason == sieve::WorkerCompletionReasonV1::range_exhausted);
    CHECK(handoff->relation_count == 2);
    CHECK(handoff->cleanup_intent_absent);
    CHECK(handoff->artifact.sequence_receipt.relation_count == expected_sequence.relation_count);
    CHECK(handoff->artifact.sequence_receipt.low == expected_sequence.low);
    CHECK(handoff->artifact.sequence_receipt.high == expected_sequence.high);
    CHECK(handoff->artifact.corpus_sha256 == *expected_corpus_sha256);
    CHECK(handoff->artifact.corpus_sha256 == result.report.corpus_sha256);
    CHECK(result.report.sequence_count == expected_sequence.relation_count);
    CHECK(result.report.sequence_low == expected_sequence.low);
    CHECK(result.report.sequence_high == expected_sequence.high);
    CHECK(handoff->artifact.index_file.extent == envelope.index.extent);
    CHECK(handoff->artifact.data_file.extent == envelope.data.extent);
    CHECK(handoff->artifact.index_file.identity.volume == envelope.index.identity.first);
    CHECK(handoff->artifact.index_file.identity.object == envelope.index.identity.second);
    CHECK(handoff->artifact.index_file.identity.generation == envelope.index.identity.third);
    CHECK(handoff->artifact.data_file.identity.volume == envelope.data.identity.first);
    CHECK(handoff->artifact.data_file.identity.object == envelope.data.identity.second);
    CHECK(handoff->artifact.data_file.identity.generation == envelope.data.identity.third);
    CHECK(handoff->wave_id == fixture.store().manifest().wave_id);
    CHECK(handoff->attempt_ordinal == result.record.attempt_ordinal);
    CHECK(handoff->artifact.descriptor.format_version == envelope.pair.format_version);
    CHECK(handoff->artifact.descriptor.store_id == envelope.pair.store_id);
    CHECK(handoff->artifact.descriptor.generation == envelope.pair.generation);
    CHECK(handoff->artifact.descriptor.relation_count == envelope.pair.count);
    CHECK(handoff->artifact.descriptor.data_end == envelope.pair.data_extent);
    require_exact_happy_namespace_delta(fixture, result.baseline);

    const auto terminal_namespace = snapshot_namespace(fixture.root);
    bool retry_phase_advanced = false;
    const auto blocked_retry = fixture.store().create_worker_attempt_private_lease_root(
        0, 1,
        wave::DistributedSievePrivateLeaseBaseLockTestHooks{
            .after_initial_phase_validation =
                [](void* context) noexcept { *static_cast<bool*>(context) = true; },
            .context = &retry_phase_advanced,
        });
    CHECK(!blocked_retry);
    CHECK(blocked_retry.claim == nullptr);
    CHECK(blocked_retry.diagnostic.status ==
          wave::DistributedSieveWaveStoreStatus::namespace_conflict);
    CHECK(!retry_phase_advanced);
    CHECK(same_namespace_snapshot(terminal_namespace, snapshot_namespace(fixture.root)));

    const auto manifest_digest = fixture.store().manifest_digest();
    const auto revalidated = fixture.store().revalidate();
    if (revalidated.status != wave::DistributedSieveWaveStoreStatus::ready) {
        fail("revalidate successful worker handoff", __LINE__, wave_diagnostic_detail(revalidated));
    }
    fixture.opened.store.reset();
    fixture.opened = wave::DistributedSieveWaveStore::open(fixture.root, manifest_digest);
    if (!fixture.opened) {
        fail("reopen successful worker handoff", __LINE__,
             wave_diagnostic_detail(fixture.opened.diagnostic));
    }
    const auto reopened_revalidated = fixture.store().revalidate();
    if (reopened_revalidated.status != wave::DistributedSieveWaveStoreStatus::ready) {
        fail("revalidate reopened worker handoff", __LINE__,
             wave_diagnostic_detail(reopened_revalidated));
    }
    const auto reopened_terminal_namespace = snapshot_namespace(fixture.root);
    bool reopened_retry_phase_advanced = false;
    const auto reopened_blocked_retry = fixture.store().create_worker_attempt_private_lease_root(
        0, 1,
        wave::DistributedSievePrivateLeaseBaseLockTestHooks{
            .after_initial_phase_validation =
                [](void* context) noexcept { *static_cast<bool*>(context) = true; },
            .context = &reopened_retry_phase_advanced,
        });
    CHECK(!reopened_blocked_retry);
    CHECK(reopened_blocked_retry.claim == nullptr);
    CHECK(reopened_blocked_retry.diagnostic.status ==
          wave::DistributedSieveWaveStoreStatus::namespace_conflict);
    CHECK(!reopened_retry_phase_advanced);
    CHECK(same_namespace_snapshot(reopened_terminal_namespace, snapshot_namespace(fixture.root)));

    auto adoption = gnfs::relation::OOCCleanupTransaction::adopt_private_handoff(base);
    CHECK(adoption.adopted());
    CHECK(adoption.adoption.has_value());

    const auto private_directory = fixture.root / names->private_directory_leaf;
    int directory = -1;
    do {
        directory = ::open(private_directory.c_str(),
                           O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (directory < 0 && errno == EINTR);
    CHECK(directory >= 0);
    CHECK(replace_regular_at_corrupt_same_extent(directory, "corpus.relidx",
                                                 ".gnfs-test-adopted-index-displaced") == 0);
    CHECK(replace_regular_at_corrupt_same_extent(directory, "corpus.reldata",
                                                 ".gnfs-test-adopted-data-displaced") == 0);
    struct stat named_index {};
    struct stat named_data {};
    CHECK(::fstatat(directory, "corpus.relidx", &named_index, AT_SYMLINK_NOFOLLOW) == 0);
    CHECK(::fstatat(directory, "corpus.reldata", &named_data, AT_SYMLINK_NOFOLLOW) == 0);
    CHECK(static_cast<std::uint64_t>(named_index.st_dev) != envelope.index.identity.first ||
          static_cast<std::uint64_t>(named_index.st_ino) != envelope.index.identity.second);
    CHECK(static_cast<std::uint64_t>(named_data.st_dev) != envelope.data.identity.first ||
          static_cast<std::uint64_t>(named_data.st_ino) != envelope.data.identity.second);
    CHECK(static_cast<std::uint64_t>(named_index.st_size) == envelope.index.extent);
    CHECK(static_cast<std::uint64_t>(named_data.st_size) == envelope.data.extent);
    CHECK(::close(directory) == 0);

    const gnfs::relation::OOCSnapshotDescriptor expected_descriptor{
        .format_version = handoff->artifact.descriptor.format_version,
        .store_id = handoff->artifact.descriptor.store_id,
        .generation = handoff->artifact.descriptor.generation,
        .count = handoff->artifact.descriptor.relation_count,
        .data_end = handoff->artifact.descriptor.data_end,
    };
    bool named_reopen_rejected = false;
    try {
        gnfs::relation::OOCRelationReader named_reader(base.string(), expected_descriptor);
        (void)named_reader;
    } catch (const std::exception&) {
        named_reopen_rejected = true;
    }
    CHECK(named_reopen_rejected);

    gnfs::relation::OOCPrivateHandoffReader adopted_reader(std::move(*adoption.adoption));
    CHECK(adopted_reader.valid());
    CHECK(adopted_reader.reader().count() == 2);
    CHECK(writer_relation_equal(adopted_reader.reader().read(0), first));
    CHECK(writer_relation_equal(adopted_reader.reader().read(1), second));
    require_no_cleanup_publication(fixture);
#endif
}

#if defined(__APPLE__)
void test_real_worker_execution_facade(const std::filesystem::path& executable) {
    const auto executable_digest =
        worker_execution::current_distributed_sieve_worker_executable_sha256_v1();
    CHECK(executable_digest);
    CHECK(executable_digest.digest.has_value());

    WorkerEntryFixture fixture("worker-real-execution", *executable_digest.digest);
    const auto result =
        launch_writer_case(fixture, executable, WriterChildScenario::real_worker_execution);
    require_writer_entry_was_adopted(result.report);
    CHECK(result.report.writer_status ==
          static_cast<std::uint32_t>(
              worker_execution::DistributedSieveWorkerExecutionStatusV1::succeeded));
    CHECK(result.report.writer_phase ==
          static_cast<std::uint32_t>(
              worker_execution::DistributedSieveWorkerExecutionPhaseV1::handoff_publication));
    CHECK((result.report.flags & WRITER_FLAG_REAL_EXECUTION_SUCCEEDED) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_FINALIZED) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_HANDOFF_PUBLISHED) != 0U);
    CHECK(result.report.chunk_id == 0);
    CHECK(result.report.attempt_ordinal == 0);
    CHECK(result.report.attempt_digest == result.record.self_digest);
    CHECK(result.report.manifest_digest == fixture.store().manifest_digest());
    CHECK(result.report.work_digest == work_digest(fixture.identity));
    CHECK(result.report.count_after_write == 0);
    CHECK(result.report.sequence_count == 0);

    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    const auto base = fixture.root / names->private_directory_leaf / "corpus";
    const auto inspected = gnfs::relation::OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.canonical());
    CHECK(inspected.record.has_value());
    const auto decoded = sieve::decode_distributed_sieve_record(inspected.record->opaque_payload);
    CHECK(decoded);
    CHECK(decoded.value.has_value());
    const auto* handoff = std::get_if<sieve::WorkerHandoffV1>(&*decoded.value);
    CHECK(handoff != nullptr);
    CHECK(handoff->self_digest == result.report.handoff_digest);
    CHECK(handoff->attempt_started_digest == result.record.self_digest);
    CHECK(handoff->processed_sq_count == 2);
    CHECK(handoff->next_sq_index == result.record.sq_end);
    CHECK(handoff->completion_reason == sieve::WorkerCompletionReasonV1::zero_relations);
    CHECK(handoff->relation_count == 0);
    CHECK(handoff->cleanup_intent_absent);
    require_no_cleanup_publication(fixture);

    const auto manifest_digest = fixture.store().manifest_digest();
    require_wave_ready(fixture.store().revalidate(), "revalidate real worker execution handoff");
    fixture.opened.store.reset();
    fixture.opened = wave::DistributedSieveWaveStore::open(fixture.root, manifest_digest);
    if (!fixture.opened) {
        fail("reopen real worker execution handoff", __LINE__,
             wave_diagnostic_detail(fixture.opened.diagnostic));
    }
    require_wave_ready(fixture.store().revalidate(),
                       "revalidate reopened real worker execution handoff");

    WorkerEntryFixture mismatch_fixture("worker-real-execution-mismatch");
    const auto mismatch = launch_writer_case(mismatch_fixture, executable,
                                             WriterChildScenario::real_worker_execution);
    require_writer_entry_was_adopted(mismatch.report);
    CHECK(mismatch.report.writer_status ==
          static_cast<std::uint32_t>(
              worker_execution::DistributedSieveWorkerExecutionStatusV1::executable_mismatch));
    CHECK(mismatch.report.writer_phase ==
          static_cast<std::uint32_t>(
              worker_execution::DistributedSieveWorkerExecutionPhaseV1::executable_identity));
    CHECK((mismatch.report.flags & WRITER_FLAG_REAL_EXECUTION_SUCCEEDED) == 0U);
    CHECK((mismatch.report.flags & WRITER_FLAG_HANDOFF_PUBLISHED) == 0U);
    require_no_worker_outputs(mismatch_fixture);
    require_no_handoff_or_cleanup_publication(mismatch_fixture);
}

void test_positive_relation_worker_execution_facade(const std::filesystem::path& executable) {
    const auto executable_digest =
        worker_execution::current_distributed_sieve_worker_executable_sha256_v1();
    CHECK(executable_digest);
    CHECK(executable_digest.digest.has_value());

    PositiveWorkerExecutionFixture fixture("worker-positive-execution", *executable_digest.digest);
    const auto result =
        launch_writer_case(fixture, executable, WriterChildScenario::real_worker_execution);
    require_writer_entry_was_adopted(result.report);
    CHECK(result.report.writer_status ==
          static_cast<std::uint32_t>(
              worker_execution::DistributedSieveWorkerExecutionStatusV1::succeeded));
    CHECK((result.report.flags & WRITER_FLAG_REAL_EXECUTION_SUCCEEDED) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_HANDOFF_PUBLISHED) != 0U);
    CHECK(result.report.count_after_write > 0);
    CHECK(result.report.sequence_count == result.report.count_after_write);

    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    const auto base = fixture.root / names->private_directory_leaf / "corpus";
    const auto inspected = gnfs::relation::OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.canonical());
    CHECK(inspected.record.has_value());
    const auto decoded = sieve::decode_distributed_sieve_record(inspected.record->opaque_payload);
    CHECK(decoded);
    CHECK(decoded.value.has_value());
    const auto* handoff = std::get_if<sieve::WorkerHandoffV1>(&*decoded.value);
    CHECK(handoff != nullptr);
    CHECK(handoff->self_digest == result.report.handoff_digest);
    CHECK(handoff->attempt_started_digest == result.record.self_digest);
    CHECK(handoff->processed_sq_count == 1);
    CHECK(handoff->next_sq_index == fixture.identity.distributed.chunks.front().sq_end);
    CHECK(handoff->completion_reason == sieve::WorkerCompletionReasonV1::range_exhausted);
    CHECK(handoff->relation_count == result.report.count_after_write);
    CHECK(handoff->artifact.sequence_receipt.relation_count == result.report.sequence_count);
    CHECK(handoff->artifact.sequence_receipt.low == result.report.sequence_low);
    CHECK(handoff->artifact.sequence_receipt.high == result.report.sequence_high);
    CHECK(handoff->artifact.corpus_sha256 == result.report.corpus_sha256);
    CHECK(handoff->cleanup_intent_absent);
    require_exact_happy_namespace_delta(fixture, result.baseline);
    require_no_cleanup_publication(fixture);

    const auto manifest_digest = fixture.store().manifest_digest();
    require_wave_ready(fixture.store().revalidate(),
                       "revalidate positive worker execution handoff");
    fixture.opened.store.reset();
    fixture.opened = wave::DistributedSieveWaveStore::open(fixture.root, manifest_digest);
    CHECK(fixture.opened);
    require_wave_ready(fixture.store().revalidate(),
                       "revalidate reopened positive worker execution handoff");

    auto adoption = fixture.store().adopt_worker_handoff_v1(0);
    CHECK(adoption);
    CHECK(adoption.adopted.has_value());
    CHECK(adoption.adopted->handoff().self_digest == handoff->self_digest);
    CHECK(adoption.adopted->reader().count() == handoff->relation_count);

    std::vector<Relation> rows;
    rows.reserve(adoption.adopted->reader().count());
    std::set<gnfs::core::ABPair> unique_pairs;
    for (std::size_t index = 0; index < adoption.adopted->reader().count(); ++index) {
        rows.push_back(adoption.adopted->reader().read(index));
        CHECK(unique_pairs.insert(rows.back().ab()).second);
        require_positive_execution_relation(fixture.polynomial, fixture.factor_base,
                                            fixture.special_q, rows.back());
    }
    const auto sequence = gnfs::relation::relation_sequence_receipt(rows);
    const auto corpus = gnfs::relation::relation_corpus_sha256_v1(rows);
    CHECK(corpus.has_value());
    CHECK(sequence.relation_count == handoff->artifact.sequence_receipt.relation_count);
    CHECK(sequence.low == handoff->artifact.sequence_receipt.low);
    CHECK(sequence.high == handoff->artifact.sequence_receipt.high);
    CHECK(*corpus == handoff->artifact.corpus_sha256);
    std::cout << "positive worker execution validated " << rows.size() << " relations\n";
}

void test_writer_zero_row_handoff(const std::filesystem::path& executable) {
    WorkerEntryFixture fixture("writer-zero-row");
    const auto result = launch_writer_case(fixture, executable, WriterChildScenario::zero_row);
    require_writer_entry_was_adopted(result.report);
    CHECK((result.report.flags & WRITER_FLAG_AUTHORITY_READY) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_BINDINGS_VALID) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_SECOND_CONSUME_REJECTED) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_FINALIZED) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_HANDOFF_PUBLISHED) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_ZERO_ROW) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_POST_FINALIZE_REJECTED) != 0U);
    CHECK(result.report.count_after_write == 0);

    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    const auto base = fixture.root / names->private_directory_leaf / "corpus";
    const auto inspected = gnfs::relation::OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.canonical());
    CHECK(inspected.record.has_value());
    const auto& envelope = *inspected.record;
    const auto decoded = sieve::decode_distributed_sieve_record(envelope.opaque_payload);
    CHECK(decoded);
    CHECK(decoded.value.has_value());
    const auto* handoff = std::get_if<sieve::WorkerHandoffV1>(&*decoded.value);
    CHECK(handoff != nullptr);
    CHECK(handoff->self_digest == result.report.handoff_digest);
    CHECK(handoff->completion_reason == sieve::WorkerCompletionReasonV1::zero_relations);
    CHECK(handoff->processed_sq_count == 1);
    CHECK(handoff->sq_begin == result.record.sq_begin);
    CHECK(handoff->sq_end == result.record.sq_end);
    CHECK(handoff->next_sq_index == result.record.sq_end);
    CHECK(handoff->relation_count == 0);
    CHECK(handoff->artifact.descriptor.relation_count == 0);
    const std::vector<Relation> empty;
    const auto empty_sequence = gnfs::relation::relation_sequence_receipt(empty);
    CHECK(handoff->artifact.sequence_receipt.relation_count == empty_sequence.relation_count);
    CHECK(handoff->artifact.sequence_receipt.low == empty_sequence.low);
    CHECK(handoff->artifact.sequence_receipt.high == empty_sequence.high);
    const auto empty_sha256 = gnfs::relation::relation_corpus_sha256_v1(empty);
    CHECK(empty_sha256.has_value());
    CHECK(handoff->artifact.corpus_sha256 == *empty_sha256);
    CHECK(result.report.corpus_sha256 == *empty_sha256);
    CHECK(envelope.pair.count == 0);
    CHECK(envelope.pair.index_extent ==
          gnfs::relation::OOCRelationWriter::INDEX_HEADER_BYTES +
              gnfs::relation::OOCRelationWriter::INDEX_SENTINEL_BYTES);
    CHECK(envelope.pair.data_extent == gnfs::relation::OOCRelationWriter::DATA_HEADER_BYTES);

    auto adoption = fixture.store().adopt_worker_handoff_v1(0);
    CHECK(adoption);
    CHECK(adoption.adopted.has_value());
    CHECK(adoption.adopted->handoff().self_digest == handoff->self_digest);
    CHECK(adoption.adopted->reader().count() == 0);
    require_exact_happy_namespace_delta(fixture, result.baseline);
    require_no_cleanup_publication(fixture);
}

void test_worker_handoff_inventory_rejects_foreign_state(const std::filesystem::path& executable) {
    const auto require_ready_handoff = [&](WorkerEntryFixture& fixture, std::string_view label) {
        const auto result = launch_writer_case(fixture, executable, WriterChildScenario::happy);
        require_writer_entry_was_adopted(result.report);
        CHECK((result.report.flags & WRITER_FLAG_HANDOFF_PUBLISHED) != 0U);
        const auto revalidated = fixture.store().revalidate();
        if (revalidated.status != wave::DistributedSieveWaveStoreStatus::ready) {
            fail(label, __LINE__, wave_diagnostic_detail(revalidated));
        }
    };
    const auto open_attempt_directory =
        [](const WorkerEntryFixture& fixture,
           const wave::DistributedSieveWorkerAttemptNamesV1& names) {
            const auto private_directory = fixture.root / names.private_directory_leaf;
            int directory = -1;
            do {
                directory = ::open(private_directory.c_str(),
                                   O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            } while (directory < 0 && errno == EINTR);
            return directory;
        };

    {
        WorkerEntryFixture fixture("writer-handoff-cleanup-intent-rejected");
        require_ready_handoff(fixture, "ready handoff before cleanup-intent injection");
        const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
        CHECK(names.has_value());
        const int directory = open_attempt_directory(fixture, *names);
        CHECK(directory >= 0);
        CHECK(copy_regular_at_same_bytes(directory, "corpus.gnfs-ooc-private-handoff-v1",
                                         "corpus.gnfs-ooc-cleanup-v1.intent") == 0);
        CHECK(::close(directory) == 0);
        CHECK(fixture.store().revalidate().status ==
              wave::DistributedSieveWaveStoreStatus::namespace_conflict);
    }

    {
        WorkerEntryFixture fixture("writer-handoff-index-replacement-rejected");
        require_ready_handoff(fixture, "ready handoff before index replacement");
        const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
        CHECK(names.has_value());
        const int directory = open_attempt_directory(fixture, *names);
        CHECK(directory >= 0);
        constexpr std::string_view displaced = ".gnfs-test-handoff-index-displaced";
        CHECK(replace_regular_at_same_bytes(directory, "corpus.relidx", displaced) == 0);
        int unlinked = -1;
        do {
            unlinked = ::unlinkat(directory, std::string(displaced).c_str(), 0);
        } while (unlinked != 0 && errno == EINTR);
        CHECK(unlinked == 0);
        CHECK(::close(directory) == 0);
        CHECK(fixture.store().revalidate().status ==
              wave::DistributedSieveWaveStoreStatus::namespace_conflict);
    }

    {
        WorkerEntryFixture fixture("writer-handoff-later-recordless-lease-rejected");
        require_ready_handoff(fixture, "ready handoff before later BaseLock injection");
        const auto later = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 1);
        CHECK(later.has_value());
        CHECK(create_durable_empty_regular_at_root(fixture.root, later->base_lock_leaf) == 0);

        const auto injected_namespace = snapshot_namespace(fixture.root);
        CHECK(fixture.store().revalidate().status ==
              wave::DistributedSieveWaveStoreStatus::namespace_conflict);
        CHECK(same_namespace_snapshot(injected_namespace, snapshot_namespace(fixture.root)));

        const auto manifest_digest = fixture.store().manifest_digest();
        fixture.opened.store.reset();
        fixture.opened = wave::DistributedSieveWaveStore::open(fixture.root, manifest_digest);
        CHECK(!fixture.opened);
        CHECK(fixture.opened.store == nullptr);
        CHECK(fixture.opened.diagnostic.status ==
              wave::DistributedSieveWaveStoreStatus::namespace_conflict);
        CHECK(same_namespace_snapshot(injected_namespace, snapshot_namespace(fixture.root)));
    }
}

void test_worker_handoff_cache_commit_failure_retry(const std::filesystem::path& executable) {
    WorkerEntryFixture fixture("writer-handoff-cache-commit-failure");
    const auto result =
        launch_writer_case(fixture, executable, WriterChildScenario::handoff_cache_commit_failure);
    require_writer_entry_was_adopted(result.report);
    CHECK((result.report.flags & WRITER_FLAG_AUTHORITY_READY) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_BINDINGS_VALID) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_SECOND_CONSUME_REJECTED) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_WROTE_EXACT_PAIR) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_HANDOFF_CACHE_FAILURE) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_HANDOFF_CACHE_RETRY_SUCCEEDED) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_FINALIZED) != 0U);
    CHECK((result.report.flags & WRITER_FLAG_HANDOFF_PUBLISHED) != 0U);
    CHECK(result.report.count_after_write == 2);
    CHECK(result.report.mutation_error == 0);

    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    const auto base = fixture.root / names->private_directory_leaf / "corpus";
    const auto inspected = gnfs::relation::OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.canonical());
    CHECK(inspected.record.has_value());
    const auto decoded = sieve::decode_distributed_sieve_record(inspected.record->opaque_payload);
    CHECK(decoded);
    CHECK(decoded.value.has_value());
    const auto* handoff = std::get_if<sieve::WorkerHandoffV1>(&*decoded.value);
    CHECK(handoff != nullptr);
    CHECK(handoff->self_digest == result.report.handoff_digest);
    CHECK(handoff->relation_count == 2);
    require_exact_happy_namespace_delta(fixture, result.baseline);
    require_no_cleanup_publication(fixture);
}

void test_worker_handoff_durable_prefix_retries(const std::filesystem::path& executable) {
    constexpr std::array scenarios{
        WriterChildScenario::handoff_pending_durable,
        WriterChildScenario::handoff_canonical_promoted,
        WriterChildScenario::handoff_canonical_durable,
        WriterChildScenario::handoff_reserved_revoked_durable,
    };
    for (const auto scenario : scenarios) {
        WorkerEntryFixture fixture("writer-handoff-prefix-" +
                                   std::to_string(static_cast<std::uint32_t>(scenario)));
        const auto result = launch_writer_case(fixture, executable, scenario);
        require_writer_entry_was_adopted(result.report);
        CHECK((result.report.flags & WRITER_FLAG_AUTHORITY_READY) != 0U);
        CHECK((result.report.flags & WRITER_FLAG_BINDINGS_VALID) != 0U);
        CHECK((result.report.flags & WRITER_FLAG_SECOND_CONSUME_REJECTED) != 0U);
        CHECK((result.report.flags & WRITER_FLAG_WROTE_EXACT_PAIR) != 0U);
        if ((result.report.flags & WRITER_FLAG_HANDOFF_INTERRUPTED) == 0U) {
            std::cerr << "handoff interruption failed for scenario "
                      << static_cast<std::uint32_t>(scenario) << ", flags=" << result.report.flags
                      << ", mutation_error=" << result.report.mutation_error
                      << ", writer_error=" << result.report.writer_native_error << '\n';
        }
        CHECK((result.report.flags & WRITER_FLAG_HANDOFF_INTERRUPTED) != 0U);
        if ((result.report.flags & WRITER_FLAG_HANDOFF_PREFIX_OBSERVED) == 0U) {
            std::cerr << "handoff prefix observation failed for scenario "
                      << static_cast<std::uint32_t>(scenario) << ", flags=" << result.report.flags
                      << ", mutation_error=" << result.report.mutation_error
                      << ", writer_error=" << result.report.writer_native_error << '\n';
        }
        CHECK((result.report.flags & WRITER_FLAG_HANDOFF_PREFIX_OBSERVED) != 0U);
        CHECK((result.report.flags & WRITER_FLAG_HANDOFF_PREFIX_NO_CLEANUP) != 0U);
        CHECK((result.report.flags & WRITER_FLAG_HANDOFF_RETRY_DRIFT_REJECTED) != 0U);
        CHECK((result.report.flags & WRITER_FLAG_HANDOFF_RETRY_SUCCEEDED) != 0U);
        CHECK((result.report.flags & WRITER_FLAG_HANDOFF_PUBLISHED) != 0U);
        CHECK((result.report.flags & WRITER_FLAG_FINALIZED) != 0U);
        CHECK((result.report.flags & WRITER_FLAG_POST_FINALIZE_REJECTED) != 0U);
        CHECK(result.report.count_after_write == 2);
        CHECK(result.report.mutation_error == 0);
        CHECK(result.report.writer_native_error ==
              std::make_error_code(std::errc::operation_canceled).value());

        const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
        CHECK(names.has_value());
        const auto base = fixture.root / names->private_directory_leaf / "corpus";
        const auto inspected = gnfs::relation::OOCCleanupTransaction::inspect_private_handoff(base);
        CHECK(inspected.canonical());
        CHECK(inspected.record.has_value());
        CHECK(!std::filesystem::exists(base.string() + ".gnfs-ooc-private-handoff-v1.pending"));
        CHECK(!std::filesystem::exists(fixture.root / names->reserved_leaf));
        const auto decoded =
            sieve::decode_distributed_sieve_record(inspected.record->opaque_payload);
        CHECK(decoded);
        CHECK(decoded.value.has_value());
        const auto* handoff = std::get_if<sieve::WorkerHandoffV1>(&*decoded.value);
        CHECK(handoff != nullptr);
        CHECK(handoff->self_digest == result.report.handoff_digest);
        CHECK(handoff->artifact.corpus_sha256 == result.report.corpus_sha256);
        CHECK(handoff->artifact.sequence_receipt.relation_count == result.report.sequence_count);
        CHECK(handoff->artifact.sequence_receipt.low == result.report.sequence_low);
        CHECK(handoff->artifact.sequence_receipt.high == result.report.sequence_high);
        require_exact_happy_namespace_delta(fixture, result.baseline);
        require_no_cleanup_publication(fixture);
    }
}

[[nodiscard]] int open_test_directory(const std::filesystem::path& path) noexcept {
    int directory = -1;
    do {
        directory =
            ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (directory < 0 && errno == EINTR);
    return directory;
}

[[nodiscard]] NamespaceSnapshot
expected_pending_rollback_namespace(NamespaceSnapshot crashed,
                                    const wave::DistributedSieveWorkerAttemptNamesV1& names) {
    crashed.erase(names.reserved_leaf);
    crashed.erase(names.reserved_pending_leaf);
    crashed.erase(names.owned_leaf);
    crashed.erase(names.owned_pending_leaf);
    const std::string private_prefix = names.private_directory_leaf + "/";
    for (auto item = crashed.begin(); item != crashed.end();) {
        if (item->first == names.private_directory_leaf ||
            std::string_view(item->first).starts_with(private_prefix)) {
            item = crashed.erase(item);
        } else {
            ++item;
        }
    }
    return crashed;
}

void require_fresh_handoff_reopen_ready(WorkerEntryFixture& fixture, std::string_view label) {
    fresh_reopen_worker_handoff(fixture);
    if (!fixture.opened) {
        fail(label, __LINE__, wave_diagnostic_detail(fixture.opened.diagnostic));
    }
    require_wave_ready(fixture.store().revalidate(), label);
}

void require_fresh_handoff_reopen_rejected_without_mutation(WorkerEntryFixture& fixture,
                                                            std::string_view label) {
    const auto attacked_namespace = snapshot_namespace(fixture.root);
    fresh_reopen_worker_handoff(fixture);
    if (fixture.opened || fixture.opened.diagnostic.status !=
                              wave::DistributedSieveWaveStoreStatus::namespace_conflict) {
        fail(label, __LINE__, wave_diagnostic_detail(fixture.opened.diagnostic));
    }
    CHECK(fixture.opened.store == nullptr);
    CHECK(same_namespace_snapshot(attacked_namespace, snapshot_namespace(fixture.root)));
}

void require_cold_terminal_handoff_adoptable(
    WorkerEntryFixture& fixture, const wave::DistributedSieveWorkerAttemptNamesV1& names) {
    const auto base = fixture.root / names.private_directory_leaf / "corpus";
    const auto inspected = gnfs::relation::OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.canonical());
    CHECK(inspected.record.has_value());
    const auto decoded = sieve::decode_distributed_sieve_record(inspected.record->opaque_payload);
    CHECK(decoded);
    CHECK(decoded.value.has_value());
    const auto* handoff = std::get_if<sieve::WorkerHandoffV1>(&*decoded.value);
    CHECK(handoff != nullptr);
    CHECK(handoff->attempt_ordinal == 0);
    CHECK(handoff->relation_count == 2);

    auto adoption = gnfs::relation::OOCCleanupTransaction::adopt_private_handoff(base);
    CHECK(adoption.adopted());
    CHECK(adoption.adoption.has_value());
    gnfs::relation::OOCPrivateHandoffReader reader(std::move(*adoption.adoption));
    CHECK(reader.valid());
    CHECK(reader.reader().count() == 2);
    CHECK(writer_relation_equal(reader.reader().read(0), make_writer_relation(31, 7, 10)));
    CHECK(writer_relation_equal(reader.reader().read(1), make_writer_relation(32, 8, 11)));
}

void test_worker_handoff_cold_restart_recovers_durable_prefixes(
    const std::filesystem::path& executable) {
    constexpr std::array scenarios{
        WriterChildScenario::handoff_pending_durable,
        WriterChildScenario::handoff_canonical_promoted,
        WriterChildScenario::handoff_canonical_durable,
        WriterChildScenario::handoff_reserved_revoked_durable,
    };
    for (const auto scenario : scenarios) {
        WorkerEntryFixture fixture("writer-handoff-cold-prefix-" +
                                   std::to_string(static_cast<std::uint32_t>(scenario)));
        const auto crashed = launch_writer_handoff_crash(fixture, executable, scenario);
        CHECK(same_namespace_snapshot(crashed.crashed_namespace, snapshot_namespace(fixture.root)));

        const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
        CHECK(names.has_value());
        const auto attempt_record_before =
            crashed.crashed_namespace.find(names->canonical_record_leaf);
        CHECK(attempt_record_before != crashed.crashed_namespace.end());
        CHECK(crashed.crashed_namespace.contains(names->base_lock_leaf));

        if (scenario == WriterChildScenario::handoff_pending_durable) {
            const auto expected =
                expected_pending_rollback_namespace(crashed.crashed_namespace, *names);
            require_fresh_handoff_reopen_ready(
                fixture, "fresh reopen recovers PendingDurable handoff prefix");
            CHECK(same_namespace_snapshot(expected, snapshot_namespace(fixture.root)));
            CHECK(std::filesystem::is_regular_file(fixture.root / names->canonical_record_leaf));
            CHECK(!std::filesystem::exists(fixture.root / names->pending_record_leaf));
            CHECK(std::filesystem::is_regular_file(fixture.root / names->base_lock_leaf));
            CHECK(!std::filesystem::exists(fixture.root / names->reserved_leaf));
            CHECK(!std::filesystem::exists(fixture.root / names->owned_leaf));
            CHECK(!std::filesystem::exists(fixture.root / names->private_directory_leaf));

            const auto before_rejected_same_ordinal = snapshot_namespace(fixture.root);
            auto same_ordinal = fixture.store().create_worker_attempt_private_lease_root(0, 0);
            CHECK(!same_ordinal);
            CHECK(same_ordinal.claim == nullptr);
            CHECK(same_ordinal.diagnostic.status ==
                  wave::DistributedSieveWaveStoreStatus::namespace_conflict);
            CHECK(same_namespace_snapshot(before_rejected_same_ordinal,
                                          snapshot_namespace(fixture.root)));

            auto next_claim = fixture.store().create_worker_attempt_private_lease_root(0, 1);
            if (!next_claim || next_claim.claim == nullptr) {
                fail("create ordinal N+1 after PendingDurable rollback", __LINE__,
                     wave_diagnostic_detail(next_claim.diagnostic));
            }
            auto next_reservation =
                wave::reserve_worker_attempt_private_lease(std::move(next_claim));
            if (!next_reservation || !next_reservation.receipt.has_value()) {
                fail("reserve ordinal N+1 after PendingDurable rollback", __LINE__,
                     wave_diagnostic_detail(next_reservation.diagnostic));
            }
            auto next_start =
                wave::publish_worker_attempt_started(std::move(*next_reservation.receipt));
            if (!next_start || !next_start.receipt.has_value()) {
                fail("publish ordinal N+1 after PendingDurable rollback", __LINE__,
                     wave_diagnostic_detail(next_start.diagnostic));
            }
            CHECK(next_start.receipt->record().attempt_ordinal == 1);
            CHECK(next_start.receipt->record().predecessor_digest == crashed.record.self_digest);
            continue;
        }

        NamespaceSnapshot expected = crashed.crashed_namespace;
        if (scenario != WriterChildScenario::handoff_reserved_revoked_durable) {
            CHECK(expected.erase(names->reserved_leaf) == 1);
        }
        require_fresh_handoff_reopen_ready(fixture,
                                           "fresh reopen converges canonical handoff prefix");
        CHECK(same_namespace_snapshot(expected, snapshot_namespace(fixture.root)));
        CHECK(std::filesystem::is_regular_file(fixture.root / names->private_directory_leaf /
                                               std::string(WORKER_HANDOFF_CANONICAL_LEAF)));
        CHECK(!std::filesystem::exists(fixture.root / names->private_directory_leaf /
                                       std::string(WORKER_HANDOFF_PENDING_LEAF)));
        CHECK(!std::filesystem::exists(fixture.root / names->reserved_leaf));
        CHECK(std::filesystem::is_regular_file(fixture.root / names->owned_leaf));
        require_cold_terminal_handoff_adoptable(fixture, *names);
        require_no_cleanup_publication(fixture);
    }
}

void test_worker_handoff_cold_restart_converges_identical_dual(
    const std::filesystem::path& executable) {
    constexpr std::array scenarios{
        WriterChildScenario::handoff_canonical_promoted,
        WriterChildScenario::handoff_canonical_durable,
        WriterChildScenario::handoff_reserved_revoked_durable,
    };
    for (const auto scenario : scenarios) {
        WorkerEntryFixture fixture("writer-handoff-cold-identical-dual-" +
                                   std::to_string(static_cast<std::uint32_t>(scenario)));
        (void)launch_writer_handoff_crash(fixture, executable, scenario);
        const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
        CHECK(names.has_value());
        const auto private_directory = fixture.root / names->private_directory_leaf;
        const int directory = open_test_directory(private_directory);
        CHECK(directory >= 0);
        CHECK(copy_regular_at_same_bytes_durable(directory, WORKER_HANDOFF_CANONICAL_LEAF,
                                                 WORKER_HANDOFF_PENDING_LEAF) == 0);
        CHECK(::close(directory) == 0);

        NamespaceSnapshot expected = snapshot_namespace(fixture.root);
        const std::string pending_path =
            names->private_directory_leaf + "/" + std::string(WORKER_HANDOFF_PENDING_LEAF);
        CHECK(expected.erase(pending_path) == 1);
        if (scenario != WriterChildScenario::handoff_reserved_revoked_durable) {
            CHECK(expected.erase(names->reserved_leaf) == 1);
        }

        require_fresh_handoff_reopen_ready(fixture,
                                           "fresh reopen converges identical dual handoff");
        CHECK(same_namespace_snapshot(expected, snapshot_namespace(fixture.root)));
        require_cold_terminal_handoff_adoptable(fixture, *names);
    }
}

void test_worker_handoff_cold_restart_rejects_invalid_prefixes(
    const std::filesystem::path& executable) {
    {
        WorkerEntryFixture fixture("writer-handoff-cold-missing-reserved");
        (void)launch_writer_handoff_crash(fixture, executable,
                                          WriterChildScenario::handoff_pending_durable);
        const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
        CHECK(names.has_value());
        const int root = open_test_directory(fixture.root);
        CHECK(root >= 0);
        CHECK(unlink_regular_at_durable(root, names->reserved_leaf) == 0);
        CHECK(::close(root) == 0);
        require_fresh_handoff_reopen_rejected_without_mutation(
            fixture, "PendingDurable without exact RESERVED fails closed");
    }

    {
        WorkerEntryFixture fixture("writer-handoff-cold-nonidentical-dual");
        (void)launch_writer_handoff_crash(fixture, executable,
                                          WriterChildScenario::handoff_pending_durable);
        const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
        CHECK(names.has_value());
        const int directory = open_test_directory(fixture.root / names->private_directory_leaf);
        CHECK(directory >= 0);
        CHECK(copy_regular_at_same_bytes_durable(directory, WORKER_HANDOFF_PENDING_LEAF,
                                                 WORKER_HANDOFF_CANONICAL_LEAF) == 0);
        std::vector<std::byte> pending_bytes;
        mode_t mode = 0;
        CHECK(read_regular_at(directory, WORKER_HANDOFF_PENDING_LEAF, pending_bytes, mode) == 0);
        const auto canonical_bytes = make_valid_nonidentical_worker_handoff_bytes(pending_bytes);
        CHECK(write_regular_in_place_at(directory, WORKER_HANDOFF_CANONICAL_LEAF,
                                        canonical_bytes) == 0);
        CHECK(synchronize_descriptor(directory) == 0);
        CHECK(::close(directory) == 0);
        require_fresh_handoff_reopen_rejected_without_mutation(
            fixture, "nonidentical canonical and pending handoff fails closed");
    }

    {
        WorkerEntryFixture fixture("writer-handoff-cold-pair-identity-mismatch");
        (void)launch_writer_handoff_crash(fixture, executable,
                                          WriterChildScenario::handoff_pending_durable);
        const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
        CHECK(names.has_value());
        const int directory = open_test_directory(fixture.root / names->private_directory_leaf);
        CHECK(directory >= 0);
        CHECK(replace_regular_at_same_bytes_durable(
                  directory, "corpus.relidx", ".gnfs-test-cold-recovery-displaced-index") == 0);
        CHECK(::close(directory) == 0);
        require_fresh_handoff_reopen_rejected_without_mutation(
            fixture, "PendingDurable pair identity mismatch fails closed");
    }

    {
        WorkerEntryFixture fixture("writer-handoff-cold-pair-extent-mismatch");
        (void)launch_writer_handoff_crash(fixture, executable,
                                          WriterChildScenario::handoff_pending_durable);
        const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
        CHECK(names.has_value());
        const int directory = open_test_directory(fixture.root / names->private_directory_leaf);
        CHECK(directory >= 0);
        CHECK(append_byte_at_durable(directory, "corpus.reldata") == 0);
        CHECK(::close(directory) == 0);
        require_fresh_handoff_reopen_rejected_without_mutation(
            fixture, "PendingDurable pair extent mismatch fails closed");
    }
}

void test_worker_handoff_cold_restart_rejects_same_byte_replacement_sandwiches(
    const std::filesystem::path& executable) {
    struct Case final {
        WriterChildScenario scenario;
        std::string_view leaf;
        std::string_view label;
    };
    constexpr std::array cases{
        Case{
            .scenario = WriterChildScenario::handoff_pending_durable,
            .leaf = WORKER_HANDOFF_PENDING_LEAF,
            .label = "pending",
        },
        Case{
            .scenario = WriterChildScenario::handoff_canonical_durable,
            .leaf = WORKER_HANDOFF_CANONICAL_LEAF,
            .label = "canonical",
        },
    };

    for (const auto& test_case : cases) {
        WorkerEntryFixture fixture("writer-handoff-cold-same-byte-" + std::string(test_case.label));
        (void)launch_writer_handoff_crash(fixture, executable, test_case.scenario);
        const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
        CHECK(names.has_value());
        ColdHandoffSameByteReplacementContext replacement{
            .private_directory = fixture.root / names->private_directory_leaf,
            .leaf = std::string(test_case.leaf),
            .displaced_leaf = ".gnfs-test-cold-recovery-displaced-" + std::string(test_case.label),
        };
        const auto before_attack = snapshot_namespace(fixture.root);
        const std::string attacked_path =
            names->private_directory_leaf + "/" + std::string(test_case.leaf);
        const auto original = before_attack.find(attacked_path);
        CHECK(original != before_attack.end());

        fresh_reopen_worker_handoff(
            fixture,
            wave::DistributedSieveWaveStoreTestHooks{
                .worker_handoff_resume =
                    {
                        .stop_after = replace_cold_handoff_after_expected_prefix_validation,
                        .context = &replacement,
                    },
            });
        CHECK(!fixture.opened);
        CHECK(fixture.opened.store == nullptr);
        CHECK(fixture.opened.diagnostic.status ==
              wave::DistributedSieveWaveStoreStatus::namespace_conflict);
        CHECK(replacement.invoked);
        CHECK(replacement.error == 0);
        CHECK(replacement.replacement.present);
        CHECK(replacement.replacement.device != original->second.device ||
              replacement.replacement.inode != original->second.inode);
        CHECK(replacement.replacement.size == original->second.bytes.size());

        NamespaceSnapshot expected = before_attack;
        auto attacked = expected.find(attacked_path);
        CHECK(attacked != expected.end());
        attacked->second.device = replacement.replacement.device;
        attacked->second.inode = replacement.replacement.inode;
        CHECK(same_namespace_snapshot(expected, snapshot_namespace(fixture.root)));
    }
}

void test_worker_handoff_cold_restart_respects_live_base_lock(
    const std::filesystem::path& executable) {
    WorkerEntryFixture fixture("writer-handoff-cold-live-base-lock");
    const auto crashed = launch_writer_handoff_crash(fixture, executable,
                                                     WriterChildScenario::handoff_pending_durable);
    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    const Digest manifest_digest = fixture.store().manifest_digest();
    auto live_lock = hold_base_lock_in_clean_child(fixture.root / names->base_lock_leaf);

    const auto locked_namespace = snapshot_namespace(fixture.root);
    fresh_reopen_worker_handoff(fixture, manifest_digest);
    CHECK(!fixture.opened);
    CHECK(fixture.opened.store == nullptr);
    CHECK(fixture.opened.diagnostic.status ==
          wave::DistributedSieveWaveStoreStatus::private_lease_lock_busy);
    CHECK(same_namespace_snapshot(locked_namespace, snapshot_namespace(fixture.root)));

    live_lock.release_and_wait();
    fresh_reopen_worker_handoff(fixture, manifest_digest);
    if (!fixture.opened) {
        fail("fresh reopen after live BaseLock release", __LINE__,
             wave_diagnostic_detail(fixture.opened.diagnostic));
    }
    require_wave_ready(fixture.store().revalidate(), "revalidate after live BaseLock release");
    const auto expected = expected_pending_rollback_namespace(crashed.crashed_namespace, *names);
    CHECK(same_namespace_snapshot(expected, snapshot_namespace(fixture.root)));
}
#endif

void test_writer_replacement_sandwiches(const std::filesystem::path& executable) {
    struct Case final {
        WriterChildScenario scenario;
        WriterStatus status;
        std::string_view displaced_leaf;
    };
    constexpr std::array cases{
        Case{
            WriterChildScenario::reserved_replacement_sandwich,
            WriterStatus::private_lease_invalid,
            ".gnfs-test-writer-displaced-reserved",
        },
        Case{
            WriterChildScenario::private_directory_replacement_sandwich,
            WriterStatus::private_lease_invalid,
            ".gnfs-test-writer-displaced-private-directory",
        },
        Case{
            WriterChildScenario::base_lock_replacement_sandwich,
            WriterStatus::lock_invalid,
            ".gnfs-test-writer-displaced-base-lock",
        },
    };

    for (const auto& test_case : cases) {
        WorkerEntryFixture fixture("writer-sandwich-" +
                                   std::to_string(static_cast<std::uint32_t>(test_case.scenario)));
        const auto result = launch_writer_case(fixture, executable, test_case.scenario);
        require_writer_rejection(result.report, WriterPhase::final_revalidation, test_case.status);
        CHECK(std::filesystem::exists(fixture.root / test_case.displaced_leaf));

        const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
        CHECK(names.has_value());
        switch (test_case.scenario) {
        case WriterChildScenario::reserved_replacement_sandwich:
            CHECK(std::filesystem::is_regular_file(fixture.root / names->reserved_leaf));
            break;
        case WriterChildScenario::private_directory_replacement_sandwich:
            CHECK(std::filesystem::is_directory(fixture.root / names->private_directory_leaf));
            CHECK(std::filesystem::is_empty(fixture.root / names->private_directory_leaf));
            CHECK(std::filesystem::exists(
                fixture.root / test_case.displaced_leaf /
                std::string(wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF)));
            break;
        case WriterChildScenario::base_lock_replacement_sandwich:
            CHECK(std::filesystem::is_regular_file(fixture.root / names->base_lock_leaf));
            break;
        default:
            fail("writer replacement scenario", __LINE__);
        }
        require_no_worker_outputs(fixture);
        require_no_handoff_or_cleanup_publication(fixture);
    }
}

void test_writer_wrong_consistent_base_digest(const std::filesystem::path& executable) {
    WorkerEntryFixture fixture("writer-wrong-base-digest");
    const auto result =
        launch_writer_case(fixture, executable, WriterChildScenario::wrong_base_path_digest);
    require_writer_rejection(result.report, WriterPhase::entry_revalidation,
                             WriterStatus::private_lease_invalid);

    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    CHECK(std::filesystem::exists(fixture.root / names->reserved_leaf));
    CHECK(std::filesystem::exists(fixture.root / names->owned_leaf));
    CHECK(std::filesystem::exists(fixture.root / names->private_directory_leaf /
                                  std::string(wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF)));
    require_no_worker_outputs(fixture);
    require_no_handoff_or_cleanup_publication(fixture);
}

void test_writer_foreign_staging_residue(const std::filesystem::path& executable) {
    WorkerEntryFixture fixture("writer-foreign-staging");
    const auto result =
        launch_writer_case(fixture, executable, WriterChildScenario::foreign_staging_residue);
    require_writer_rejection(result.report, WriterPhase::entry_revalidation,
                             WriterStatus::private_lease_invalid);

    const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
    CHECK(names.has_value());
    const auto staging = foreign_staging_leaf_for(*names, result.record);
    CHECK(std::filesystem::is_directory(fixture.root / staging));
    require_no_worker_outputs(fixture);
    require_no_handoff_or_cleanup_publication(fixture);
}

void test_post_authority_namespace_drift(const std::filesystem::path& executable) {
    enum class Target : std::uint8_t {
        base_lock,
        reserved,
        private_directory,
    };
    struct Case final {
        WriterChildScenario scenario;
        std::string_view displaced_leaf;
        Target target = Target::base_lock;
    };
    constexpr std::array cases{
        Case{
            WriterChildScenario::post_authority_base_lock_replacement,
            ".gnfs-test-writer-post-displaced-base-lock",
            Target::base_lock,
        },
        Case{
            WriterChildScenario::post_authority_reserved_replacement,
            ".gnfs-test-writer-post-displaced-reserved",
            Target::reserved,
        },
        Case{
            WriterChildScenario::post_authority_private_directory_replacement,
            ".gnfs-test-writer-post-displaced-private-directory",
            Target::private_directory,
        },
    };

    for (const auto& test_case : cases) {
        WorkerEntryFixture fixture("writer-post-authority-" +
                                   std::to_string(static_cast<std::uint32_t>(test_case.scenario)));
        const auto result = launch_writer_case(fixture, executable, test_case.scenario);
        require_post_authority_drift_rejection(result.report);

        const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
        CHECK(names.has_value());
        const auto displaced = fixture.root / test_case.displaced_leaf;
        if (test_case.target == Target::private_directory) {
            const auto named = fixture.root / names->private_directory_leaf;
            require_distinct_owner_directories(named, displaced);
            CHECK(std::filesystem::is_empty(named));
            CHECK(std::filesystem::is_regular_file(
                displaced / std::string(wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF)));
            require_unfinalized_corpus(displaced / "corpus");
        } else {
            const auto canonical =
                fixture.root / (test_case.target == Target::base_lock ? names->base_lock_leaf
                                                                      : names->reserved_leaf);
            require_distinct_regular_inodes(canonical, displaced);
            const auto base = fixture.root / names->private_directory_leaf / "corpus";
            require_unfinalized_corpus(base);
        }
        require_no_handoff_or_cleanup_publication(fixture);
    }
}

void test_exact_writer_construction_rollback_diagnostics(const std::filesystem::path& executable) {
    {
        WorkerEntryFixture fixture("writer-construction-clean");
        const auto result = launch_writer_case(fixture, executable,
                                               WriterChildScenario::construction_clean_rollback);
        require_construction_failure(result.report, WriterRollback::clean, false);
        CHECK(result.report.rollback_native_error == 0);

        const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
        CHECK(names.has_value());
        const auto base = fixture.root / names->private_directory_leaf / "corpus";
        CHECK(!std::filesystem::exists(base.string() + ".relidx"));
        CHECK(!std::filesystem::exists(base.string() + ".reldata"));
        require_no_worker_outputs(fixture);
        require_no_handoff_or_cleanup_publication(fixture);
    }

    {
        WorkerEntryFixture fixture("writer-construction-foreign-residue");
        const auto result = launch_writer_case(
            fixture, executable, WriterChildScenario::construction_foreign_index_residue);
        require_construction_failure(result.report, WriterRollback::named_residue_may_remain, true);
        CHECK(result.report.rollback_native_error != 0);

        const auto names = wave::distributed_sieve_worker_attempt_names_v1("entry_chunk_0", 0, 0);
        CHECK(names.has_value());
        const auto directory = fixture.root / names->private_directory_leaf;
        const auto foreign_index = directory / "corpus.relidx";
        const auto displaced_owned = directory / ".gnfs-test-writer-owned-index-displaced";
        require_distinct_regular_inodes(foreign_index, displaced_owned);
        CHECK(!std::filesystem::exists(directory / "corpus.reldata"));
        require_no_handoff_or_cleanup_publication(fixture);
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
        (void)argc;
        (void)argv;
        const auto unsupported = entry::adopt_distributed_sieve_worker_entry_v1();
        CHECK(!unsupported);
        CHECK(unsupported.diagnostic.phase ==
              entry::DistributedSieveWorkerEntryPhaseV1::platform_gate);
        CHECK(unsupported.diagnostic.status ==
              entry::DistributedSieveWorkerEntryStatusV1::platform_unsupported);
        std::cout << "distributed sieve worker-writer authority explicitly "
                     "unsupported on this platform\n";
        return 0;
#else
        if (argc == 3 && (std::string_view(argv[1]) == WRITER_CHILD_ARGUMENT ||
                          std::string_view(argv[1]) == WRITER_COLD_CRASH_CHILD_ARGUMENT)) {
            const auto scenario = parse_writer_child_scenario(argv[2]);
            const bool cold_crash = std::string_view(argv[1]) == WRITER_COLD_CRASH_CHILD_ARGUMENT;
            return scenario.has_value() ? run_writer_child(*scenario, cold_crash) : 94;
        }

        CHECK(argc >= 1);
        const auto executable = self_executable_path(argv[0]);
        test_writer_authority_happy_path(executable);
#if defined(__APPLE__)
        test_retry_creation_serializes_with_predecessor_handoff(executable);
        test_real_worker_execution_facade(executable);
        test_positive_relation_worker_execution_facade(executable);
        test_writer_zero_row_handoff(executable);
        test_worker_handoff_inventory_rejects_foreign_state(executable);
        test_worker_handoff_cache_commit_failure_retry(executable);
        test_worker_handoff_durable_prefix_retries(executable);
        test_worker_handoff_cold_restart_recovers_durable_prefixes(executable);
        test_worker_handoff_cold_restart_converges_identical_dual(executable);
        test_worker_handoff_cold_restart_rejects_invalid_prefixes(executable);
        test_worker_handoff_cold_restart_rejects_same_byte_replacement_sandwiches(executable);
        test_worker_handoff_cold_restart_respects_live_base_lock(executable);
#endif
        test_writer_replacement_sandwiches(executable);
        test_writer_wrong_consistent_base_digest(executable);
        test_writer_foreign_staging_residue(executable);
        test_post_authority_namespace_drift(executable);
        test_exact_writer_construction_rollback_diagnostics(executable);
        std::cout << "distributed sieve worker-writer authority tests passed\n";
        return 0;
#endif
    } catch (const std::exception& error) {
        std::cerr << "distributed sieve worker-writer authority test failed: " << error.what()
                  << '\n';
        return 1;
    }
}

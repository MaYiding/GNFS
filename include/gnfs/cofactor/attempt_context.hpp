#pragma once

#include "gnfs/core/integer.hpp"
#include "gnfs/util/sha256.hpp"

#include <cstdint>
#include <optional>

namespace gnfs::cofactor {

enum class CofactorSide : std::uint8_t {
    rational = 0,
    algebraic = 1,
};

/// Algorithm families that may request deterministic cofactor randomness.
///
/// Values are protocol identities. Additions require a versioned contract
/// review instead of reusing an existing value for a different algorithm.
/// This is a cofactor-provider-local namespace; adapters must map other
/// protocol domains explicitly and must not rely on numeric casts.
enum class CofactorRandomDomainV1 : std::uint8_t {
    brent_pollard_rho = 1,
    ecm_curve_schedule = 2,
};

/// SHA-256 ordinal stream -> `(draw % 1'000'000) + 6` ECM sigma mapping.
///
/// Increment this identity whenever the draw interpretation, curve ordinal
/// mapping, modulus, or sigma offset changes.
inline constexpr std::uint32_t COFACTOR_ECM_CURVE_SCHEDULE_ALGORITHM_IDENTITY_V1 = 1;

struct CofactorAttemptCoordinates final {
    std::uint64_t special_q_index = 0;
    std::uint64_t candidate_ordinal = 0;

    [[nodiscard]] friend constexpr bool
    operator==(const CofactorAttemptCoordinates&,
               const CofactorAttemptCoordinates&) noexcept = default;
};

/// Full-width deterministic seed. The all-zero digest is a valid seed.
struct CofactorSeed256 final {
    util::Sha256Digest digest{};

    [[nodiscard]] friend constexpr bool operator==(const CofactorSeed256&,
                                                   const CofactorSeed256&) noexcept = default;
};

struct CofactorAttemptContext final {
    CofactorAttemptCoordinates coordinates{};
    CofactorSide side = CofactorSide::rational;
    util::Sha256Digest cofactor_digest{};
    CofactorRandomDomainV1 domain = CofactorRandomDomainV1::brent_pollard_rho;
    std::uint32_t algorithm_identity = 0;
    CofactorSeed256 seed{};

    [[nodiscard]] friend constexpr bool
    operator==(const CofactorAttemptContext&, const CofactorAttemptContext&) noexcept = default;
};

/// Hash a canonical unsigned-magnitude encoding of a cofactor.
///
/// The encoding is:
///   "GNFS-COFACTOR-INPUT-V1" || side:u8 || magnitude_length:u64be ||
///   minimal_unsigned_big_endian_magnitude
///
/// Zero has an empty magnitude. Negative Integers use their absolute magnitude.
/// Unknown CofactorSide values throw std::invalid_argument.
[[nodiscard]] util::Sha256Digest canonical_cofactor_input_digest(const core::Integer& cofactor,
                                                                 CofactorSide side);

/// Return deterministic draw block `draw_ordinal` for the complete 256-bit seed.
[[nodiscard]] util::Sha256Digest cofactor_random_block(const CofactorSeed256& seed,
                                                       std::uint64_t draw_ordinal);

/// Interpret the first eight bytes of cofactor_random_block() as a big-endian u64.
[[nodiscard]] std::uint64_t cofactor_random_u64(const CofactorSeed256& seed,
                                                std::uint64_t draw_ordinal);

/// Sequential view of the ordinal-addressed deterministic random stream.
///
/// next_block() and next_u64() consume the same ordinal sequence. The
/// UINT64_MAX draw succeeds, marks the stream exhausted, and is never wrapped.
class Seed256Stream final {
public:
    explicit Seed256Stream(CofactorSeed256 seed, std::uint64_t initial_draw_ordinal = 0) noexcept;

    [[nodiscard]] std::optional<util::Sha256Digest> next_block();
    [[nodiscard]] std::optional<std::uint64_t> next_u64();

    [[nodiscard]] bool exhausted() const noexcept;

private:
    void advance() noexcept;

    CofactorSeed256 seed_{};
    std::uint64_t draw_ordinal_ = 0;
    bool exhausted_ = false;
};

} // namespace gnfs::cofactor

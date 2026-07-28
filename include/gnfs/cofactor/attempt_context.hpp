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

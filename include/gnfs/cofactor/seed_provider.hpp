#pragma once

#include "gnfs/cofactor/attempt_context.hpp"

#include <cstdint>

namespace gnfs::cofactor {

/// Complete semantic request presented to a cofactor seed provider.
///
/// algorithm_identity must be nonzero and identifies the exact versioned
/// algorithm schedule within domain.
struct CofactorSeedRequestV1 final {
    CofactorAttemptCoordinates coordinates{};
    CofactorSide side = CofactorSide::rational;
    util::Sha256Digest cofactor_digest{};
    CofactorRandomDomainV1 domain = CofactorRandomDomainV1::brent_pollard_rho;
    std::uint32_t algorithm_identity = 0;

    [[nodiscard]] friend constexpr bool operator==(const CofactorSeedRequestV1&,
                                                   const CofactorSeedRequestV1&) noexcept = default;
};

/// Read-only source of full-width deterministic cofactor seeds.
///
/// Implementations must be immutable from the caller's perspective and safe
/// for concurrent seed_for() calls. A provider failure must throw; callers may
/// not replace it with ambient entropy or another fallback seed.
class CofactorSeedProvider {
public:
    virtual ~CofactorSeedProvider() = default;

    [[nodiscard]] virtual CofactorSeed256 seed_for(const CofactorSeedRequestV1& request) const = 0;
};

/// Atomically construct one validated cofactor attempt context.
///
/// The helper validates side, domain, and algorithm_identity before computing
/// the canonical cofactor digest or invoking provider. It invokes seed_for()
/// exactly once. Provider exceptions propagate unchanged and never trigger an
/// ambient-randomness fallback.
[[nodiscard]] CofactorAttemptContext
make_cofactor_attempt_context_v1(const core::Integer& cofactor,
                                 CofactorAttemptCoordinates coordinates, CofactorSide side,
                                 CofactorRandomDomainV1 domain, std::uint32_t algorithm_identity,
                                 const CofactorSeedProvider& provider);

} // namespace gnfs::cofactor

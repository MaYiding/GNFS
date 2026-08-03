#include "gnfs/cofactor/seed_provider.hpp"

#include <stdexcept>

namespace gnfs::cofactor {
namespace {

void validate_side(CofactorSide side) {
    switch (side) {
    case CofactorSide::rational:
    case CofactorSide::algebraic:
        return;
    }
    throw std::invalid_argument("unknown cofactor side");
}

void validate_domain(CofactorRandomDomainV1 domain) {
    switch (domain) {
    case CofactorRandomDomainV1::brent_pollard_rho:
    case CofactorRandomDomainV1::ecm_curve_schedule:
        return;
    }
    throw std::invalid_argument("unknown cofactor random domain");
}

} // namespace

CofactorAttemptContext make_cofactor_attempt_context_v1(const core::Integer& cofactor,
                                                        CofactorAttemptCoordinates coordinates,
                                                        CofactorSide side,
                                                        CofactorRandomDomainV1 domain,
                                                        std::uint32_t algorithm_identity,
                                                        const CofactorSeedProvider& provider) {
    validate_side(side);
    validate_domain(domain);
    if (algorithm_identity == 0) {
        throw std::invalid_argument("cofactor algorithm identity must be nonzero");
    }

    const util::Sha256Digest cofactor_digest = canonical_cofactor_input_digest(cofactor, side);
    const CofactorSeedRequestV1 request{
        .coordinates = coordinates,
        .side = side,
        .cofactor_digest = cofactor_digest,
        .domain = domain,
        .algorithm_identity = algorithm_identity,
    };
    const CofactorSeed256 seed = provider.seed_for(request);

    return CofactorAttemptContext{
        .coordinates = coordinates,
        .side = side,
        .cofactor_digest = cofactor_digest,
        .domain = domain,
        .algorithm_identity = algorithm_identity,
        .seed = seed,
    };
}

} // namespace gnfs::cofactor

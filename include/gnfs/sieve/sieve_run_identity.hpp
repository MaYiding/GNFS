#pragma once

#include "../core/params.hpp"
#include "../core/polynomial_context.hpp"
#include "../factor_base/factor_base.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace gnfs::sieve {

/// Stable identity for all mathematical inputs that make a sieve checkpoint
/// safe to resume. run_n remains independently inspectable while the two
/// fingerprint lanes bind the full polynomial, factor base, and sieve policy.
struct SieveRunIdentity {
    std::string run_n;
    uint64_t fingerprint_lo = 0;
    uint64_t fingerprint_hi = 0;

    friend bool operator==(const SieveRunIdentity&, const SieveRunIdentity&) = default;
};

inline constexpr uint32_t SIEVE_RUN_IDENTITY_SCHEMA_VERSION = 2;

namespace sieve_run_identity_detail {

/// Two stable, independently mixed 64-bit lanes. Every multi-byte value is fed
/// in an explicit little-endian order; no object representation or std::hash
/// participates in the persisted identity.
class StableFingerprint {
public:
    void add_u8(uint8_t value) noexcept {
        lo_ ^= value;
        lo_ *= 1099511628211ULL;

        hi_ ^= static_cast<uint64_t>(value) + byte_index_ * 0x9e3779b97f4a7c15ULL;
        hi_ = std::rotl(hi_, 27);
        hi_ *= 0x94d049bb133111ebULL;
        hi_ += 0x2545f4914f6cdd1dULL;
        ++byte_index_;
    }

    void add_u16(uint16_t value) noexcept {
        for (unsigned shift = 0; shift < 16; shift += 8) {
            add_u8(static_cast<uint8_t>((value >> shift) & 0xffU));
        }
    }

    void add_u32(uint32_t value) noexcept {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            add_u8(static_cast<uint8_t>((value >> shift) & 0xffU));
        }
    }

    void add_u64(uint64_t value) noexcept {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            add_u8(static_cast<uint8_t>((value >> shift) & 0xffULL));
        }
    }

    void add_i32(int32_t value) noexcept {
        add_u32(std::bit_cast<uint32_t>(value));
    }

    void add_string(std::string_view value) noexcept {
        add_u64(static_cast<uint64_t>(value.size()));
        for (const char byte : value) {
            add_u8(static_cast<uint8_t>(static_cast<unsigned char>(byte)));
        }
    }

    void add_field(std::string_view name) noexcept {
        add_string(name);
    }

    [[nodiscard]] uint64_t finish_lo() const noexcept {
        uint64_t value = avalanche(lo_ ^ byte_index_);
        return value == 0 ? 0x6a09e667f3bcc909ULL : value;
    }

    [[nodiscard]] uint64_t finish_hi() const noexcept {
        uint64_t value = avalanche(hi_ ^ std::rotl(byte_index_, 17));
        return value == 0 ? 0xbb67ae8584caa73bULL : value;
    }

private:
    [[nodiscard]] static uint64_t avalanche(uint64_t value) noexcept {
        value ^= value >> 30;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27;
        value *= 0x94d049bb133111ebULL;
        value ^= value >> 31;
        return value;
    }

    uint64_t lo_ = 14695981039346656037ULL;
    uint64_t hi_ = 0x243f6a8885a308d3ULL;
    uint64_t byte_index_ = 0;
};

inline void add_integer(StableFingerprint& hash, std::string_view field,
                        const core::Integer& value) {
    hash.add_field(field);
    hash.add_string(value.to_string());
}

inline void add_u32(StableFingerprint& hash, std::string_view field, uint32_t value) noexcept {
    hash.add_field(field);
    hash.add_u32(value);
}

inline void add_u64(StableFingerprint& hash, std::string_view field, uint64_t value) noexcept {
    hash.add_field(field);
    hash.add_u64(value);
}

inline void add_i32(StableFingerprint& hash, std::string_view field, int32_t value) noexcept {
    hash.add_field(field);
    hash.add_i32(value);
}

} // namespace sieve_run_identity_detail

/// Compute a portable identity once after polynomial selection and factor-base
/// construction. The hashed GNFSParams subset contains every field that changes
/// relation generation, stopping, or the target used by the sieve phase.
[[nodiscard]] inline SieveRunIdentity
make_sieve_run_identity(const core::PolynomialContext& context,
                        const factor_base::FactorBase& factor_base,
                        const core::GNFSParams& params) {
    using namespace sieve_run_identity_detail;

    StableFingerprint hash;
    hash.add_field("gnfs.sieve.run-identity");
    hash.add_u32(SIEVE_RUN_IDENTITY_SCHEMA_VERSION); // Independent of checkpoint V2.

    const std::string run_n = context.n().to_string();
    hash.add_field("polynomial");
    hash.add_field("n");
    hash.add_string(run_n);
    add_integer(hash, "m", context.m());
    add_u32(hash, "degree", context.degree());
    hash.add_field("coefficients.count");
    hash.add_u64(static_cast<uint64_t>(context.coefficients().size()));
    for (size_t i = 0; i < context.coefficients().size(); ++i) {
        hash.add_field("coefficient.index");
        hash.add_u64(static_cast<uint64_t>(i));
        add_integer(hash, "coefficient.value", context.coefficients()[i]);
    }
    static_assert(sizeof(double) == sizeof(uint64_t));
    static_assert(std::numeric_limits<double>::is_iec559);
    hash.add_field("skewness.bits");
    hash.add_u64(std::bit_cast<uint64_t>(context.skewness()));

    const auto& fb_params = factor_base.params();
    hash.add_field("factor-base.params");
    add_u32(hash, "rational_bound", fb_params.rational_bound);
    add_u32(hash, "algebraic_bound", fb_params.algebraic_bound);
    add_u64(hash, "large_prime_bound", fb_params.large_prime_bound);
    hash.add_field("log_scale");
    hash.add_u8(fb_params.log_scale);

    hash.add_field("factor-base.rational");
    hash.add_u64(static_cast<uint64_t>(factor_base.rational_count()));
    for (size_t i = 0; i < factor_base.rational().size(); ++i) {
        const auto& entry = factor_base.rational()[i];
        hash.add_u64(static_cast<uint64_t>(i));
        hash.add_u32(entry.p);
        hash.add_u32(entry.log_p);
    }

    hash.add_field("factor-base.algebraic");
    hash.add_u64(static_cast<uint64_t>(factor_base.algebraic_count()));
    for (size_t i = 0; i < factor_base.algebraic().size(); ++i) {
        const auto& entry = factor_base.algebraic()[i];
        hash.add_u64(static_cast<uint64_t>(i));
        hash.add_u32(entry.p);
        hash.add_u32(entry.r);
        hash.add_u32(entry.log_p);
        hash.add_u8(entry.degree);
    }
    hash.add_field("factor-base.sieve-algebraic-count");
    hash.add_u64(static_cast<uint64_t>(factor_base.sieve_algebraic_count()));

    hash.add_field("gnfs-params.sieve");
    add_u64(hash, "bits", static_cast<uint64_t>(params.bits));
    add_u64(hash, "digits", static_cast<uint64_t>(params.digits));
    add_u32(hash, "degree", params.degree);
    add_u32(hash, "rational_bound", params.rational_bound);
    add_u32(hash, "algebraic_bound", params.algebraic_bound);
    add_u64(hash, "large_prime_bound", params.large_prime_bound);
    add_u32(hash, "large_prime_bits", params.large_prime_bits);
    hash.add_field("log_scale");
    hash.add_u8(params.log_scale);
    add_i32(hash, "sieve_i_min", params.sieve_i_min);
    add_i32(hash, "sieve_i_max", params.sieve_i_max);
    add_i32(hash, "sieve_j_min", params.sieve_j_min);
    add_i32(hash, "sieve_j_max", params.sieve_j_max);
    hash.add_field("rational_threshold");
    hash.add_u16(params.rational_threshold);
    hash.add_field("algebraic_threshold");
    hash.add_u16(params.algebraic_threshold);
    add_u32(hash, "special_q_min", params.special_q_min);
    add_u32(hash, "special_q_max", params.special_q_max);
    add_u32(hash, "max_special_q", params.max_special_q);
    add_u32(hash, "target_excess", params.target_excess);

    return SieveRunIdentity{
        .run_n = run_n,
        .fingerprint_lo = hash.finish_lo(),
        .fingerprint_hi = hash.finish_hi(),
    };
}

} // namespace gnfs::sieve

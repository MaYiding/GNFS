#pragma once

#include "gnfs/core/relation.hpp"
#include "gnfs/util/sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace gnfs::relation {

inline constexpr std::uint32_t RELATION_CORPUS_SHA256_VERSION_V1 = 1;

namespace detail {

enum class RelationCorpusSha256TagV1 : std::uint8_t {
    CorpusBegin = 0x01,
    RelationBegin = 0x02,
    RelationA = 0x10,
    RelationB = 0x11,
    RationalFactors = 0x20,
    AlgebraicFactors = 0x21,
    RationalLargePrimes = 0x30,
    AlgebraicLargePrimes = 0x31,
    PrimePower = 0x32,
    ExtraAbPairs = 0x40,
    ExtraAbPair = 0x41,
    RelationEnd = 0x7e,
    TerminalCount = 0x7f,
};

} // namespace detail

/// Streaming, order-sensitive SHA-256 of the semantic relation sequence.
///
/// V1 encodes the fixed domain `GNFS-RCS` followed by version byte 1 and the
/// CorpusBegin tag. Each relation then encodes its zero-based ordinal, signed
/// `a` as its two's-complement uint64_t bits, `b`, and every ordered vector.
/// Vector lengths and element ordinals are explicit. All integers are
/// little-endian; prime powers encode p/r/e in that order. The stream ends
/// with TerminalCount and the final relation count, so the zero-row digest is
/// defined. The accumulator retains constant memory and never stores rows.
///
/// append() and finalize() fail closed. finalize() succeeds exactly once.
class RelationCorpusSha256AccumulatorV1 final {
public:
    RelationCorpusSha256AccumulatorV1() noexcept {
        constexpr std::array<std::byte, 9> domain_bytes = {
            std::byte{'G'}, std::byte{'N'}, std::byte{'F'}, std::byte{'S'}, std::byte{'-'},
            std::byte{'R'}, std::byte{'C'}, std::byte{'S'}, std::byte{1},
        };
        if (!accumulator_.update(domain_bytes) ||
            !append_tag(detail::RelationCorpusSha256TagV1::CorpusBegin)) {
            failed_ = true;
        }
    }

    [[nodiscard]] bool append(const core::Relation& relation) noexcept {
        if (failed_ || finalized_) {
            return false;
        }
        if (relation_count_ == std::numeric_limits<std::uint64_t>::max() ||
            relation.rational_factors.size() > core::Relation::MAX_SERIALIZED_FACTORS ||
            relation.algebraic_factors.size() > core::Relation::MAX_SERIALIZED_FACTORS ||
            relation.rational_large_prime.size() > core::Relation::MAX_SERIALIZED_LARGE_PRIMES ||
            relation.algebraic_large_prime.size() > core::Relation::MAX_SERIALIZED_LARGE_PRIMES ||
            relation.extra_ab_pairs.size() > core::Relation::MAX_SERIALIZED_EXTRA_AB_PAIRS) {
            failed_ = true;
            return false;
        }

        if (!append_tag(detail::RelationCorpusSha256TagV1::RelationBegin) ||
            !append_u64_le(relation_count_) ||
            !append_tag(detail::RelationCorpusSha256TagV1::RelationA) ||
            !append_u64_le(static_cast<std::uint64_t>(relation.a)) ||
            !append_tag(detail::RelationCorpusSha256TagV1::RelationB) ||
            !append_u64_le(relation.b) ||
            !append_factors(detail::RelationCorpusSha256TagV1::RationalFactors,
                            relation.rational_factors) ||
            !append_factors(detail::RelationCorpusSha256TagV1::AlgebraicFactors,
                            relation.algebraic_factors) ||
            !append_prime_powers(detail::RelationCorpusSha256TagV1::RationalLargePrimes,
                                 relation.rational_large_prime) ||
            !append_prime_powers(detail::RelationCorpusSha256TagV1::AlgebraicLargePrimes,
                                 relation.algebraic_large_prime) ||
            !append_extra_ab_pairs(relation.extra_ab_pairs) ||
            !append_tag(detail::RelationCorpusSha256TagV1::RelationEnd)) {
            failed_ = true;
            return false;
        }

        ++relation_count_;
        return true;
    }

    [[nodiscard]] std::uint64_t count() const noexcept {
        return relation_count_;
    }

    [[nodiscard]] bool failed() const noexcept {
        return failed_ || accumulator_.failed();
    }

    [[nodiscard]] bool finalized() const noexcept {
        return finalized_;
    }

    [[nodiscard]] std::optional<util::Sha256Digest> finalize() noexcept {
        if (failed() || finalized_) {
            return std::nullopt;
        }
        if (!append_tag(detail::RelationCorpusSha256TagV1::TerminalCount) ||
            !append_u64_le(relation_count_)) {
            failed_ = true;
            return std::nullopt;
        }
        auto digest = accumulator_.finalize();
        if (!digest.has_value()) {
            failed_ = true;
            return std::nullopt;
        }
        finalized_ = true;
        return digest;
    }

private:
    [[nodiscard]] bool append_byte(std::uint8_t value) noexcept {
        const std::array<std::byte, 1> bytes{static_cast<std::byte>(value)};
        return accumulator_.update(bytes);
    }

    [[nodiscard]] bool append_tag(detail::RelationCorpusSha256TagV1 tag) noexcept {
        return append_byte(static_cast<std::uint8_t>(tag));
    }

    [[nodiscard]] bool append_u32_le(std::uint32_t value) noexcept {
        std::array<std::byte, sizeof(value)> bytes{};
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            bytes[index] = static_cast<std::byte>(value >> (index * 8U));
        }
        return accumulator_.update(bytes);
    }

    [[nodiscard]] bool append_u64_le(std::uint64_t value) noexcept {
        std::array<std::byte, sizeof(value)> bytes{};
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            bytes[index] = static_cast<std::byte>(value >> (index * 8U));
        }
        return accumulator_.update(bytes);
    }

    [[nodiscard]] bool append_factors(detail::RelationCorpusSha256TagV1 tag,
                                      const std::vector<std::uint32_t>& factors) noexcept {
        if (!append_tag(tag) || !append_u64_le(static_cast<std::uint64_t>(factors.size()))) {
            return false;
        }
        for (std::size_t index = 0; index < factors.size(); ++index) {
            if (!append_u64_le(static_cast<std::uint64_t>(index)) ||
                !append_u32_le(factors[index])) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool
    append_prime_powers(detail::RelationCorpusSha256TagV1 tag,
                        const core::Relation::LargePrimeList& prime_powers) noexcept {
        if (!append_tag(tag) || !append_u64_le(static_cast<std::uint64_t>(prime_powers.size()))) {
            return false;
        }
        for (std::size_t index = 0; index < prime_powers.size(); ++index) {
            const auto& prime_power = prime_powers[index];
            if (!append_tag(detail::RelationCorpusSha256TagV1::PrimePower) ||
                !append_u64_le(static_cast<std::uint64_t>(index)) ||
                !append_u64_le(prime_power.p) || !append_u64_le(prime_power.r) ||
                !append_byte(prime_power.e)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool append_extra_ab_pairs(
        const std::vector<std::pair<std::int64_t, std::uint64_t>>& pairs) noexcept {
        if (!append_tag(detail::RelationCorpusSha256TagV1::ExtraAbPairs) ||
            !append_u64_le(static_cast<std::uint64_t>(pairs.size()))) {
            return false;
        }
        for (std::size_t index = 0; index < pairs.size(); ++index) {
            const auto& [a, b] = pairs[index];
            if (!append_tag(detail::RelationCorpusSha256TagV1::ExtraAbPair) ||
                !append_u64_le(static_cast<std::uint64_t>(index)) ||
                !append_u64_le(static_cast<std::uint64_t>(a)) || !append_u64_le(b)) {
                return false;
            }
        }
        return true;
    }

    util::Sha256Accumulator accumulator_;
    std::uint64_t relation_count_ = 0;
    bool failed_ = false;
    bool finalized_ = false;
};

[[nodiscard]] inline std::optional<util::Sha256Digest>
relation_corpus_sha256_v1(std::span<const core::Relation> relations) noexcept {
    RelationCorpusSha256AccumulatorV1 accumulator;
    for (const auto& relation : relations) {
        if (!accumulator.append(relation)) {
            return std::nullopt;
        }
    }
    return accumulator.finalize();
}

} // namespace gnfs::relation

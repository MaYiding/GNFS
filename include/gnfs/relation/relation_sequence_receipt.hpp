#pragma once

#include "gnfs/core/relation.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gnfs::relation {

/// Constant-memory receipt for the exact accepted relation sequence.
///
/// This is an in-process integrity invariant, not a cryptographic
/// authentication tag. The collector advances it only after a relation is
/// durably accepted. A later OOC scan must reproduce the same receipt before
/// it may establish a reduction digest baseline.
struct RelationSequenceReceipt final {
    uint64_t relation_count = 0;
    uint64_t low = 0;
    uint64_t high = 0;

    [[nodiscard]] constexpr bool
    operator==(const RelationSequenceReceipt&) const noexcept = default;
};

namespace detail {

class RelationSequenceReceiptBuilder final {
public:
    void append_byte(uint8_t value) noexcept {
        low_ ^= static_cast<uint64_t>(value);
        low_ *= UINT64_C(1099511628211);

        high_ ^= static_cast<uint64_t>(value) + UINT64_C(0x9e3779b97f4a7c15);
        high_ *= UINT64_C(0xc2b2ae3d27d4eb4f);
        high_ ^= high_ >> 29U;
    }

    void append_u32_le(uint32_t value) noexcept {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            append_byte(static_cast<uint8_t>(value >> shift));
        }
    }

    void append_u64_le(uint64_t value) noexcept {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            append_byte(static_cast<uint8_t>(value >> shift));
        }
    }

    [[nodiscard]] RelationSequenceReceipt finish(uint64_t relation_count) const noexcept {
        return {relation_count, low_, high_};
    }

private:
    uint64_t low_ = UINT64_C(14695981039346656037);
    uint64_t high_ = UINT64_C(0x84222325cbf29ce4);
};

} // namespace detail

/// Incremental fixed-V1 relation-sequence receipt.
///
/// Unlike CorpusDigestAccumulator, this accumulator does not need the final
/// row count up front. That lets an appendable collector retain an independent
/// baseline without storing one fingerprint per relation.
class RelationSequenceReceiptAccumulator final {
public:
    RelationSequenceReceiptAccumulator() noexcept {
        constexpr uint8_t domain[] = {'G', 'N', 'F', 'S', '-', 'R', 'S', 'Q', 1};
        for (uint8_t byte : domain) {
            builder_.append_byte(byte);
        }
        builder_.append_byte(0x01);
    }

    void append(const core::Relation& relation) noexcept {
        auto& builder = builder_;
        builder.append_byte(0x02);
        builder.append_u64_le(relation_count_);

        builder.append_byte(0x10);
        builder.append_u64_le(static_cast<uint64_t>(relation.a));
        builder.append_byte(0x11);
        builder.append_u64_le(relation.b);

        const auto append_factors = [&](uint8_t tag, const std::vector<uint32_t>& factors) {
            builder.append_byte(tag);
            builder.append_u64_le(static_cast<uint64_t>(factors.size()));
            for (size_t index = 0; index < factors.size(); ++index) {
                builder.append_u64_le(static_cast<uint64_t>(index));
                builder.append_u32_le(factors[index]);
            }
        };
        append_factors(0x20, relation.rational_factors);
        append_factors(0x21, relation.algebraic_factors);

        const auto append_prime_powers = [&](uint8_t tag,
                                             const core::Relation::LargePrimeList& prime_powers) {
            builder.append_byte(tag);
            builder.append_u64_le(static_cast<uint64_t>(prime_powers.size()));
            for (size_t index = 0; index < prime_powers.size(); ++index) {
                const auto& prime_power = prime_powers[index];
                builder.append_byte(0x32);
                builder.append_u64_le(static_cast<uint64_t>(index));
                builder.append_u64_le(prime_power.p);
                builder.append_u64_le(prime_power.r);
                builder.append_byte(prime_power.e);
            }
        };
        append_prime_powers(0x30, relation.rational_large_prime);
        append_prime_powers(0x31, relation.algebraic_large_prime);

        builder.append_byte(0x40);
        builder.append_u64_le(static_cast<uint64_t>(relation.extra_ab_pairs.size()));
        for (size_t index = 0; index < relation.extra_ab_pairs.size(); ++index) {
            const auto& [a, b] = relation.extra_ab_pairs[index];
            builder.append_byte(0x41);
            builder.append_u64_le(static_cast<uint64_t>(index));
            builder.append_u64_le(static_cast<uint64_t>(a));
            builder.append_u64_le(b);
        }
        builder.append_byte(0x7e);
        ++relation_count_;
    }

    [[nodiscard]] uint64_t count() const noexcept {
        return relation_count_;
    }

    [[nodiscard]] RelationSequenceReceipt finish() const noexcept {
        auto final_builder = builder_;
        final_builder.append_byte(0x7f);
        final_builder.append_u64_le(relation_count_);
        return final_builder.finish(relation_count_);
    }

private:
    detail::RelationSequenceReceiptBuilder builder_;
    uint64_t relation_count_ = 0;
};

[[nodiscard]] inline RelationSequenceReceipt
relation_sequence_receipt(const std::vector<core::Relation>& relations) noexcept {
    RelationSequenceReceiptAccumulator accumulator;
    for (const auto& relation : relations) {
        accumulator.append(relation);
    }
    return accumulator.finish();
}

} // namespace gnfs::relation

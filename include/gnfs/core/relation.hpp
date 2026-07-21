#pragma once

#include "gnfs/core/integer.hpp"
#include "gnfs/core/types.hpp"
#include "gnfs/util/safe_math.hpp"
#include <cassert>
#include <cstdint>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <vector>

namespace gnfs::core {

/// A relation in GNFS represents a smooth (a,b) pair
struct Relation {
    /// Type alias for large prime lists
    using LargePrimeList = std::vector<PrimePower>;

    int64_t a = 0;
    uint64_t b = 0;  // b > 0 always (matches ABPair::b / SieveCandidate::b)

    // Rational side factorization: indices into factor base
    std::vector<uint32_t> rational_factors;

    // Algebraic side factorization: indices into factor base
    std::vector<uint32_t> algebraic_factors;

    // Large primes (if any) - stored as vectors for multiple large primes
    LargePrimeList rational_large_prime;
    LargePrimeList algebraic_large_prime;

    // For merged relations: additional (a,b) pairs from constituent relations.
    // The primary (a,b) is in the a/b fields; extra pairs stored here.
    // Sqrt step must process all pairs (primary + extra) for correct computation.
    std::vector<std::pair<int64_t, uint64_t>> extra_ab_pairs;

    // Default constructor
    Relation() = default;

    // Constructor with (a, b) pair
    Relation(int64_t a_, uint64_t b_) : a(a_), b(b_) {}

    // Get as ABPair
    [[nodiscard]] ABPair ab() const {
        return ABPair(a, b);
    }

    // Check if valid (b != 0)
    [[nodiscard]] bool is_valid() const {
        return b != 0;
    }

    // Raw-storage predicate: true only when both persisted LP vectors are
    // empty.  Merged rows whose repeated/even LP entries cancel over GF(2)
    // can be matrix-full while this remains false; relation algorithms use
    // relation::odd_large_prime_keys_empty() for that effective view.
    [[nodiscard]] bool is_full() const {
        return rational_large_prime.empty() && algebraic_large_prime.empty();
    }

    // Raw number of persisted PrimePower entries, without parity folding or
    // repeated-key cancellation.  Use relation::count_odd_large_prime_keys()
    // when counting effective GF(2) large-prime columns.
    [[nodiscard]] size_t num_large_primes() const {
        return rational_large_prime.size() + algebraic_large_prime.size();
    }

    // Check if this is a merged relation (product of two partial relations)
    [[nodiscard]] bool is_merged() const {
        return !extra_ab_pairs.empty();
    }

    // Serialization format constants
    static constexpr uint32_t SERIALIZE_MAGIC   = 0x52454C46;  // "RELF"
    static constexpr uint32_t SERIALIZE_VERSION = 2;

    // Shared persistence limits for both the checksummed stream format and
    // the compact OOC format. Writers reject an oversized relation before
    // emitting any bytes; readers use the same values as allocation bounds.
    static constexpr uint32_t MAX_SERIALIZED_FACTORS = 1u << 20;
    static constexpr uint32_t MAX_SERIALIZED_LARGE_PRIMES = 16;
    static constexpr uint32_t MAX_SERIALIZED_EXTRA_AB_PAIRS = 1u << 16;

    void validate_persistence_limits() const {
        if (rational_factors.size() > MAX_SERIALIZED_FACTORS) {
            throw std::length_error(
                "Relation: rational factor count exceeds persistence limit");
        }
        if (algebraic_factors.size() > MAX_SERIALIZED_FACTORS) {
            throw std::length_error(
                "Relation: algebraic factor count exceeds persistence limit");
        }
        if (rational_large_prime.size() > MAX_SERIALIZED_LARGE_PRIMES) {
            throw std::length_error(
                "Relation: rational large-prime count exceeds persistence limit");
        }
        if (algebraic_large_prime.size() > MAX_SERIALIZED_LARGE_PRIMES) {
            throw std::length_error(
                "Relation: algebraic large-prime count exceeds persistence limit");
        }
        if (extra_ab_pairs.size() > MAX_SERIALIZED_EXTRA_AB_PAIRS) {
            throw std::length_error(
                "Relation: extra (a,b) count exceeds persistence limit");
        }
    }

    // Serialize to output stream (v2: magic + version + extra_ab_pairs + checksum)
    void serialize(std::ostream& os) const {
        validate_persistence_limits();

        uint64_t checksum = 0;
        auto write_and_xor = [&](const void* ptr, size_t n) {
            os.write(reinterpret_cast<const char*>(ptr), static_cast<std::streamsize>(n));
            auto* bytes = reinterpret_cast<const uint8_t*>(ptr);
            for (size_t i = 0; i < n; ++i)
                checksum ^= static_cast<uint64_t>(bytes[i]) << ((i & 7) * 8);
        };

        // Header
        write_and_xor(&SERIALIZE_MAGIC, sizeof(SERIALIZE_MAGIC));
        write_and_xor(&SERIALIZE_VERSION, sizeof(SERIALIZE_VERSION));

        // Core fields
        write_and_xor(&a, sizeof(a));
        write_and_xor(&b, sizeof(b));

        // Rational factors
        uint32_t rat_count = static_cast<uint32_t>(rational_factors.size());
        write_and_xor(&rat_count, sizeof(rat_count));
        for (uint32_t i = 0; i < rat_count; ++i)
            write_and_xor(&rational_factors[i], sizeof(uint32_t));

        // Algebraic factors
        uint32_t alg_count = static_cast<uint32_t>(algebraic_factors.size());
        write_and_xor(&alg_count, sizeof(alg_count));
        for (uint32_t i = 0; i < alg_count; ++i)
            write_and_xor(&algebraic_factors[i], sizeof(uint32_t));

        // Large primes (rational)
        uint32_t lp_rat_count = static_cast<uint32_t>(rational_large_prime.size());
        write_and_xor(&lp_rat_count, sizeof(lp_rat_count));
        for (const auto& lp : rational_large_prime) {
            write_and_xor(&lp.p, sizeof(lp.p));
            write_and_xor(&lp.r, sizeof(lp.r));
            write_and_xor(&lp.e, sizeof(lp.e));
        }

        // Large primes (algebraic)
        uint32_t lp_alg_count = static_cast<uint32_t>(algebraic_large_prime.size());
        write_and_xor(&lp_alg_count, sizeof(lp_alg_count));
        for (const auto& lp : algebraic_large_prime) {
            write_and_xor(&lp.p, sizeof(lp.p));
            write_and_xor(&lp.r, sizeof(lp.r));
            write_and_xor(&lp.e, sizeof(lp.e));
        }

        // Extra (a,b) pairs (for merged relations)
        uint32_t extra_count = static_cast<uint32_t>(extra_ab_pairs.size());
        write_and_xor(&extra_count, sizeof(extra_count));
        for (const auto& [ea, eb] : extra_ab_pairs) {
            write_and_xor(&ea, sizeof(ea));
            write_and_xor(&eb, sizeof(eb));
        }

        // Trailing checksum (not included in its own XOR)
        os.write(reinterpret_cast<const char*>(&checksum), sizeof(checksum));
    }

    // Deserialize from input stream (v2 format with validation)
    static Relation deserialize(std::istream& is) {
        uint64_t checksum = 0;
        auto read_and_xor = [&](void* ptr, size_t n) {
            is.read(reinterpret_cast<char*>(ptr), static_cast<std::streamsize>(n));
            if (!is.good())
                throw std::runtime_error("Relation::deserialize: stream read error");
            auto* bytes = reinterpret_cast<const uint8_t*>(ptr);
            for (size_t i = 0; i < n; ++i)
                checksum ^= static_cast<uint64_t>(bytes[i]) << ((i & 7) * 8);
        };

        // Header
        uint32_t magic, version;
        read_and_xor(&magic, sizeof(magic));
        read_and_xor(&version, sizeof(version));
        if (magic != SERIALIZE_MAGIC)
            throw std::runtime_error("Relation::deserialize: invalid magic");
        if (version != SERIALIZE_VERSION)
            throw std::runtime_error("Relation::deserialize: unsupported version");

        Relation rel;

        // Core fields
        read_and_xor(&rel.a, sizeof(rel.a));
        read_and_xor(&rel.b, sizeof(rel.b));

        // Rational factors
        uint32_t rat_count;
        read_and_xor(&rat_count, sizeof(rat_count));
        if (rat_count > MAX_SERIALIZED_FACTORS)
            throw std::runtime_error("Relation::deserialize: rat_count exceeds limit");
        rel.rational_factors.resize(rat_count);
        for (uint32_t i = 0; i < rat_count; ++i)
            read_and_xor(&rel.rational_factors[i], sizeof(uint32_t));

        // Algebraic factors
        uint32_t alg_count;
        read_and_xor(&alg_count, sizeof(alg_count));
        if (alg_count > MAX_SERIALIZED_FACTORS)
            throw std::runtime_error("Relation::deserialize: alg_count exceeds limit");
        rel.algebraic_factors.resize(alg_count);
        for (uint32_t i = 0; i < alg_count; ++i)
            read_and_xor(&rel.algebraic_factors[i], sizeof(uint32_t));

        // Large primes (rational)
        uint32_t lp_rat_count;
        read_and_xor(&lp_rat_count, sizeof(lp_rat_count));
        if (lp_rat_count > MAX_SERIALIZED_LARGE_PRIMES)
            throw std::runtime_error("Relation::deserialize: lp_rat_count exceeds limit");
        rel.rational_large_prime.reserve(lp_rat_count);
        for (uint32_t i = 0; i < lp_rat_count; ++i) {
            PrimePower lp;
            read_and_xor(&lp.p, sizeof(lp.p));
            read_and_xor(&lp.r, sizeof(lp.r));
            read_and_xor(&lp.e, sizeof(lp.e));
            rel.rational_large_prime.push_back(lp);
        }

        // Large primes (algebraic)
        uint32_t lp_alg_count;
        read_and_xor(&lp_alg_count, sizeof(lp_alg_count));
        if (lp_alg_count > MAX_SERIALIZED_LARGE_PRIMES)
            throw std::runtime_error("Relation::deserialize: lp_alg_count exceeds limit");
        rel.algebraic_large_prime.reserve(lp_alg_count);
        for (uint32_t i = 0; i < lp_alg_count; ++i) {
            PrimePower lp;
            read_and_xor(&lp.p, sizeof(lp.p));
            read_and_xor(&lp.r, sizeof(lp.r));
            read_and_xor(&lp.e, sizeof(lp.e));
            rel.algebraic_large_prime.push_back(lp);
        }

        // Extra (a,b) pairs
        uint32_t extra_count;
        read_and_xor(&extra_count, sizeof(extra_count));
        if (extra_count > MAX_SERIALIZED_EXTRA_AB_PAIRS)
            throw std::runtime_error("Relation::deserialize: extra_count exceeds limit");
        rel.extra_ab_pairs.reserve(extra_count);
        for (uint32_t i = 0; i < extra_count; ++i) {
            int64_t ea;
            uint64_t eb;
            read_and_xor(&ea, sizeof(ea));
            read_and_xor(&eb, sizeof(eb));
            rel.extra_ab_pairs.emplace_back(ea, eb);
        }

        // Verify checksum
        uint64_t stored_checksum;
        is.read(reinterpret_cast<char*>(&stored_checksum), sizeof(stored_checksum));
        if (!is.good())
            throw std::runtime_error("Relation::deserialize: stream read error at checksum");
        if (checksum != stored_checksum)
            throw std::runtime_error("Relation::deserialize: checksum mismatch");

        return rel;
    }
};

} // namespace gnfs::core

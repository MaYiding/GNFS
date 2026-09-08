#pragma once

#include "../core/integer.hpp"
#include "../core/polynomial_context.hpp"
#include "../core/types.hpp"
#include "../util/primes.hpp"
#include "factor_base.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace gnfs::factor_base {

using core::AlgebraicPrime;
using core::FactorBaseParams;
using core::Integer;
using core::PolynomialContext;
using core::RationalPrime;

/// Phase 2 (Factor Base) checkpoint.
///
/// 设计 (2026-05-21):
///   - 仅在 Phase 2 完成后 save 完整 FB (result-only, 不增量)。
///   - Cantor-Zassenhaus 求根是一次性大批量操作, 无 in-flight state 需保留。
///   - 文件 `<base_path>.fb_ckpt`, MAGIC/INCOMPLETE flip 保证 crash safety。
///   - 加载时校验 build params (bounds 必须一致) 和 ctx fingerprint
///     (degree + leading_coeff + N 哈希) 防止用错 FB。
///
/// Binary layout (all fixed-width scalar fields are little-endian):
///   u64 magic
///   u64 version
///   ── Build params ──
///   u32 rational_bound
///   u32 algebraic_bound
///   u32 special_q_bound
///   u64 large_prime_bound
///   u8  log_scale + 3 pad
///   ── Context fingerprint ──
///   u32 ctx_degree
///   i32 sign(N) + u32 lc_bytes + bytes(N)           [fingerprint via raw N]
///   ── Rational FB ──
///   u32 rational_count + [u32 p, u32 log_p] × n
///   ── Algebraic FB ──
///   u32 algebraic_count + [u32 p, u32 r, u32 log_p, u8 degree, u8 pad×3] × n
///   ── Sieve algebraic split ──
///   u64 sieve_algebraic_count (high bit marks an explicit zero)
struct FbCheckpoint {
    static constexpr uint64_t MAGIC = 0x474E465346434B50ULL;            // 'GNFSFCKP'
    static constexpr uint64_t MAGIC_INCOMPLETE = 0x474E465346434B4EULL; // 'GNFSFCKN'
    static constexpr uint64_t VERSION = 1;
    // Counts are bounded to uint32_t, so reserve the high bit to distinguish
    // an explicit zero (no sieve entries) from the legacy unset-zero value.
    static constexpr uint64_t EXPLICIT_ZERO_SIEVE_COUNT = UINT64_C(1) << 63;

    // Build params
    uint32_t rational_bound = 0;
    uint32_t algebraic_bound = 0;
    uint32_t special_q_bound = 0;
    uint64_t large_prime_bound = 0;
    uint8_t log_scale = core::SIEVE_LOG_SCALE;

    // Context fingerprint (cheap hash to detect "wrong N / wrong poly" mistakes)
    uint32_t ctx_degree = 0;
    Integer ctx_n; // full N stored for strict equality check

    // FB content
    std::vector<RationalPrime> rational;
    std::vector<AlgebraicPrime> algebraic;
    uint64_t sieve_algebraic_count = 0;
    bool sieve_algebraic_count_explicit = false;

    /// Rebuild FactorBase from this checkpoint.
    [[nodiscard]] FactorBase to_factor_base() const {
        const bool marker_explicit_zero = (sieve_algebraic_count & EXPLICIT_ZERO_SIEVE_COUNT) != 0;
        const uint64_t count = sieve_algebraic_count & ~EXPLICIT_ZERO_SIEVE_COUNT;
        if (marker_explicit_zero && count != 0) {
            throw std::runtime_error(
                "FbCheckpoint::to_factor_base: invalid explicit-zero sieve count marker");
        }
        if (count > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
            throw std::overflow_error(
                "FbCheckpoint::to_factor_base: sieve algebraic count exceeds size_t");
        }
        if (count > algebraic.size()) {
            throw std::runtime_error(
                "FbCheckpoint::to_factor_base: sieve algebraic count exceeds algebraic count");
        }

        FactorBaseParams params;
        params.rational_bound = rational_bound;
        params.algebraic_bound = algebraic_bound;
        params.large_prime_bound = large_prime_bound;
        params.log_scale = log_scale;

        FactorBase fb(params);
        fb.reserve(rational.size(), algebraic.size());
        for (const auto& rp : rational) {
            fb.add_rational(rp.p, rp.log_p);
        }
        for (const auto& ap : algebraic) {
            fb.add_algebraic(ap.p, ap.r, ap.log_p, ap.degree);
        }
        // Preserve legacy zero-as-unset checkpoints while carrying explicit
        // zero through the reserved marker used by from_factor_base().
        if (sieve_algebraic_count_explicit || marker_explicit_zero || count != 0) {
            fb.set_sieve_algebraic_count_explicit(static_cast<size_t>(count));
        }
        fb.build_index();
        return fb;
    }

    /// Capture FB + params + ctx into a checkpoint.
    static FbCheckpoint from_factor_base(const FactorBase& fb, const PolynomialContext& ctx,
                                         uint32_t special_q_bound_for_save) {
        FbCheckpoint ck;
        const auto& p = fb.params();
        ck.rational_bound = p.rational_bound;
        ck.algebraic_bound = p.algebraic_bound;
        ck.special_q_bound = special_q_bound_for_save;
        ck.large_prime_bound = p.large_prime_bound;
        ck.log_scale = p.log_scale;

        ck.ctx_degree = ctx.degree();
        ck.ctx_n = ctx.n();

        auto rat = fb.rational();
        ck.rational.assign(rat.begin(), rat.end());
        auto alg = fb.algebraic();
        ck.algebraic.assign(alg.begin(), alg.end());
        ck.sieve_algebraic_count = static_cast<uint64_t>(fb.sieve_algebraic_count());
        ck.sieve_algebraic_count_explicit = fb.has_explicit_sieve_algebraic_count();
        return ck;
    }

    void save(const std::string& path) const {
        if (rational.size() > max_serialized_count() || algebraic.size() > max_serialized_count()) {
            throw std::overflow_error("FbCheckpoint::save: factor-base count exceeds uint32_t");
        }
        const bool marker_explicit_zero = (sieve_algebraic_count & EXPLICIT_ZERO_SIEVE_COUNT) != 0;
        const uint64_t count = sieve_algebraic_count & ~EXPLICIT_ZERO_SIEVE_COUNT;
        if (marker_explicit_zero && count != 0) {
            throw std::runtime_error(
                "FbCheckpoint::save: invalid explicit-zero sieve count marker");
        }
        if (count > algebraic.size()) {
            throw std::runtime_error(
                "FbCheckpoint::save: sieve algebraic count exceeds algebraic count");
        }

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("FbCheckpoint::save: cannot open " + path);
        }

        uint64_t magic = MAGIC_INCOMPLETE;
        uint64_t version = VERSION;
        write_u64(out, magic);
        write_u64(out, version);

        write_u32(out, rational_bound);
        write_u32(out, algebraic_bound);
        write_u32(out, special_q_bound);
        write_u64(out, large_prime_bound);
        uint32_t scale_pad = static_cast<uint32_t>(log_scale); // pad to 4 bytes
        write_u32(out, scale_pad);

        write_u32(out, ctx_degree);
        write_integer(out, ctx_n);

        uint32_t rat_count = static_cast<uint32_t>(rational.size());
        write_u32(out, rat_count);
        for (const auto& rp : rational) {
            write_u32(out, rp.p);
            write_u32(out, rp.log_p);
        }

        uint32_t alg_count = static_cast<uint32_t>(algebraic.size());
        write_u32(out, alg_count);
        for (const auto& ap : algebraic) {
            write_u32(out, ap.p);
            write_u32(out, ap.r);
            write_u32(out, ap.log_p);
            // degree is u8; pad to 4 bytes for alignment / future fields
            uint32_t deg_pad = static_cast<uint32_t>(ap.degree);
            write_u32(out, deg_pad);
        }

        uint64_t encoded_sieve_count = count;
        if ((sieve_algebraic_count_explicit || marker_explicit_zero) && count == 0) {
            encoded_sieve_count = EXPLICIT_ZERO_SIEVE_COUNT;
        }
        write_u64(out, encoded_sieve_count);

        out.flush();
        if (!out) {
            throw std::runtime_error("FbCheckpoint::save: write failed mid-stream");
        }

        out.seekp(0);
        magic = MAGIC;
        write_u64(out, magic);
        out.flush();
        out.close();
    }

    static FbCheckpoint load(const std::string& path, bool allow_incomplete = false) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("FbCheckpoint::load: cannot open " + path);
        }

        const uint64_t magic = read_u64(in, "magic");
        const uint64_t version = read_u64(in, "version");
        if (magic != MAGIC && !(allow_incomplete && magic == MAGIC_INCOMPLETE)) {
            throw std::runtime_error("FbCheckpoint::load: invalid magic in " + path);
        }
        if (version != VERSION) {
            throw std::runtime_error("FbCheckpoint::load: version mismatch (got " +
                                     std::to_string(version) + ", expected " +
                                     std::to_string(VERSION) + ")");
        }

        FbCheckpoint ck;
        ck.rational_bound = read_u32(in, "rational bound");
        ck.algebraic_bound = read_u32(in, "algebraic bound");
        ck.special_q_bound = read_u32(in, "special-q bound");
        ck.large_prime_bound = read_u64(in, "large-prime bound");
        const uint32_t scale_pad = read_u32(in, "log scale");
        ck.log_scale = static_cast<uint8_t>(scale_pad);

        ck.ctx_degree = read_u32(in, "context degree");
        read_integer(in, ck.ctx_n);

        const uint32_t rat_count = read_u32(in, "rational count");
        if (rat_count > 100'000'000u) {
            throw std::runtime_error("FbCheckpoint::load: rat_count corrupt");
        }
        require_records_fit(in, rat_count, 8, 12, "rational count");
        ck.rational.clear();
        std::unordered_set<uint32_t> rational_keys;
        for (size_t i = 0; i < static_cast<size_t>(rat_count); ++i) {
            RationalPrime rp;
            rp.p = read_u32(in, "rational prime");
            rp.log_p = read_u32(in, "rational log");
            if (rp.p < 2 || !util::is_prime_u32(rp.p)) {
                throw std::runtime_error("FbCheckpoint::load: rational-prime value is not prime");
            }
            if (!rational_keys.insert(rp.p).second) {
                throw std::runtime_error("FbCheckpoint::load: duplicate rational-prime value");
            }
            ck.rational.push_back(rp);
        }

        const uint32_t alg_count = read_u32(in, "algebraic count");
        if (alg_count > 100'000'000u) {
            throw std::runtime_error("FbCheckpoint::load: alg_count corrupt");
        }
        require_records_fit(in, alg_count, 16, 8, "algebraic count");
        ck.algebraic.clear();
        std::unordered_set<uint64_t> algebraic_keys;
        for (size_t i = 0; i < static_cast<size_t>(alg_count); ++i) {
            AlgebraicPrime ap;
            uint32_t deg_pad = 0;
            ap.p = read_u32(in, "algebraic prime");
            ap.r = read_u32(in, "algebraic root");
            ap.log_p = read_u32(in, "algebraic log");
            deg_pad = read_u32(in, "algebraic degree");
            if (deg_pad > (std::numeric_limits<uint8_t>::max)()) {
                throw std::runtime_error(
                    "FbCheckpoint::load: algebraic-prime degree exceeds uint8_t");
            }
            ap.degree = static_cast<uint8_t>(deg_pad);
            if (ap.p < 2 || !util::is_prime_u32(ap.p)) {
                throw std::runtime_error("FbCheckpoint::load: algebraic-prime value is not prime");
            }
            if (ap.r != AlgebraicPrime::PROJECTIVE_ROOT && ap.r >= ap.p) {
                throw std::runtime_error(
                    "FbCheckpoint::load: algebraic-prime root is out of range");
            }
            if (ap.degree == 0) {
                throw std::runtime_error("FbCheckpoint::load: algebraic-prime degree is zero");
            }
            const uint64_t key = (static_cast<uint64_t>(ap.p) << 32) | ap.r;
            if (!algebraic_keys.insert(key).second) {
                throw std::runtime_error("FbCheckpoint::load: duplicate algebraic-prime key");
            }
            ck.algebraic.push_back(ap);
        }

        uint64_t encoded_sieve_count = 0;
        encoded_sieve_count = read_u64(in, "sieve algebraic count");
        const bool marker_explicit_zero = (encoded_sieve_count & EXPLICIT_ZERO_SIEVE_COUNT) != 0;
        const uint64_t count = encoded_sieve_count & ~EXPLICIT_ZERO_SIEVE_COUNT;
        if (marker_explicit_zero && count != 0) {
            throw std::runtime_error(
                "FbCheckpoint::load: invalid explicit-zero sieve count marker");
        }
        if (count > ck.algebraic.size()) {
            throw std::runtime_error(
                "FbCheckpoint::load: sieve algebraic count exceeds algebraic count");
        }
        ck.sieve_algebraic_count = count;
        ck.sieve_algebraic_count_explicit = marker_explicit_zero;

        return ck;
    }

    static void remove(const std::string& path) noexcept {
        std::remove(path.c_str());
    }

    static bool exists_and_valid(const std::string& path) noexcept {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        unsigned char bytes[sizeof(uint64_t)]{};
        in.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
        return in.gcount() == static_cast<std::streamsize>(sizeof(bytes)) &&
               decode_u64(bytes) == MAGIC;
    }

    /// Reasons a checkpoint may be unsuitable for the current run.
    enum class MatchStatus {
        Ok,
        NMismatch,
        DegreeMismatch,
        ParamsMismatch,
    };

    /// Cheap compatibility check (does not throw).
    [[nodiscard]] MatchStatus matches(const PolynomialContext& ctx, uint32_t want_rational_bound,
                                      uint32_t want_algebraic_bound, uint32_t want_special_q_bound,
                                      uint64_t want_large_prime_bound,
                                      uint8_t want_log_scale) const {
        if (ctx_n != ctx.n())
            return MatchStatus::NMismatch;
        if (ctx_degree != ctx.degree())
            return MatchStatus::DegreeMismatch;
        if (rational_bound != want_rational_bound || algebraic_bound != want_algebraic_bound ||
            special_q_bound != want_special_q_bound ||
            large_prime_bound != want_large_prime_bound || log_scale != want_log_scale) {
            return MatchStatus::ParamsMismatch;
        }
        return MatchStatus::Ok;
    }

private:
    static void write_u32(std::ofstream& out, uint32_t value) {
        const unsigned char bytes[4] = {
            static_cast<unsigned char>(value & 0xffU),
            static_cast<unsigned char>((value >> 8U) & 0xffU),
            static_cast<unsigned char>((value >> 16U) & 0xffU),
            static_cast<unsigned char>((value >> 24U) & 0xffU),
        };
        out.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }

    static void write_u64(std::ofstream& out, uint64_t value) {
        const unsigned char bytes[8] = {
            static_cast<unsigned char>(value & 0xffULL),
            static_cast<unsigned char>((value >> 8U) & 0xffULL),
            static_cast<unsigned char>((value >> 16U) & 0xffULL),
            static_cast<unsigned char>((value >> 24U) & 0xffULL),
            static_cast<unsigned char>((value >> 32U) & 0xffULL),
            static_cast<unsigned char>((value >> 40U) & 0xffULL),
            static_cast<unsigned char>((value >> 48U) & 0xffULL),
            static_cast<unsigned char>((value >> 56U) & 0xffULL),
        };
        out.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }

    static uint32_t decode_u32(const unsigned char* bytes) noexcept {
        return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8U) |
               (static_cast<uint32_t>(bytes[2]) << 16U) | (static_cast<uint32_t>(bytes[3]) << 24U);
    }

    static uint64_t decode_u64(const unsigned char* bytes) noexcept {
        return static_cast<uint64_t>(bytes[0]) | (static_cast<uint64_t>(bytes[1]) << 8U) |
               (static_cast<uint64_t>(bytes[2]) << 16U) | (static_cast<uint64_t>(bytes[3]) << 24U) |
               (static_cast<uint64_t>(bytes[4]) << 32U) | (static_cast<uint64_t>(bytes[5]) << 40U) |
               (static_cast<uint64_t>(bytes[6]) << 48U) | (static_cast<uint64_t>(bytes[7]) << 56U);
    }

    static uint32_t read_u32(std::ifstream& in, const char* field) {
        unsigned char bytes[4]{};
        in.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
        if (in.gcount() != static_cast<std::streamsize>(sizeof(bytes))) {
            throw std::runtime_error(std::string("FbCheckpoint::load: truncated ") + field);
        }
        return decode_u32(bytes);
    }

    static uint64_t read_u64(std::ifstream& in, const char* field) {
        unsigned char bytes[8]{};
        in.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
        if (in.gcount() != static_cast<std::streamsize>(sizeof(bytes))) {
            throw std::runtime_error(std::string("FbCheckpoint::load: truncated ") + field);
        }
        return decode_u64(bytes);
    }

    // Count fields are attacker-controlled when resuming from a persisted file.
    // Validate the minimum payload and trailer before growing any containers.
    static uint64_t remaining_bytes(std::ifstream& in, const char* field) {
        const std::streampos current = in.tellg();
        if (current == std::streampos(-1)) {
            throw std::runtime_error(std::string("FbCheckpoint::load: cannot seek before ") +
                                     field);
        }
        in.seekg(0, std::ios::end);
        const std::streampos end = in.tellg();
        if (end == std::streampos(-1) || end < current) {
            in.clear();
            in.seekg(current);
            throw std::runtime_error(std::string("FbCheckpoint::load: cannot measure ") + field);
        }
        in.seekg(current);
        if (!in) {
            throw std::runtime_error(std::string("FbCheckpoint::load: cannot restore ") + field);
        }
        return static_cast<uint64_t>(end - current);
    }

    static void require_records_fit(std::ifstream& in, uint32_t count, uint64_t record_size,
                                    uint64_t trailer_size, const char* field) {
        const uint64_t remaining = remaining_bytes(in, field);
        if (remaining < trailer_size ||
            static_cast<uint64_t>(count) > (remaining - trailer_size) / record_size) {
            throw std::runtime_error(std::string("FbCheckpoint::load: truncated ") + field);
        }
    }

    [[nodiscard]] static constexpr size_t max_serialized_count() noexcept {
        return static_cast<size_t>((std::numeric_limits<uint32_t>::max)());
    }

    static void write_integer(std::ofstream& out, const Integer& x) {
        const mpz_t& mz = x.get_mpz();
        int32_t sgn = mpz_sgn(mz);
        write_u32(out, static_cast<uint32_t>(sgn));
        if (sgn == 0) {
            uint32_t zero = 0;
            write_u32(out, zero);
            return;
        }
        size_t byte_count = 0;
        size_t bits = mpz_sizeinbase(mz, 2);
        size_t max_bytes = (bits + 7) / 8 + 1;
        std::vector<unsigned char> buf(max_bytes);
        mpz_export(buf.data(), &byte_count, /*order=*/1, /*size=*/1,
                   /*endian=*/1, /*nails=*/0, mz);
        if (byte_count > (std::numeric_limits<uint32_t>::max)()) {
            throw std::overflow_error("FbCheckpoint::write_integer: integer is too large");
        }
        uint32_t bc = static_cast<uint32_t>(byte_count);
        write_u32(out, bc);
        if (bc > 0) {
            out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(bc));
        }
    }

    static void read_integer(std::ifstream& in, Integer& x) {
        const uint32_t sign_bits = read_u32(in, "integer sign");
        int32_t sgn = 0;
        if (sign_bits == 1U) {
            sgn = 1;
        } else if (sign_bits == 0xffffffffU) {
            sgn = -1;
        } else if (sign_bits != 0U) {
            throw std::runtime_error("FbCheckpoint::read_integer: invalid sign");
        }
        const uint32_t byte_count = read_u32(in, "integer byte count");
        if (byte_count > (1u << 30)) {
            throw std::runtime_error("FbCheckpoint::read_integer: byte_count too large");
        }
        constexpr uint64_t minimum_trailer_bytes =
            sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint64_t);
        const uint64_t remaining = remaining_bytes(in, "integer body");
        if (remaining < minimum_trailer_bytes) {
            throw std::runtime_error("FbCheckpoint::read_integer: truncated body");
        }
        if (sgn == 0) {
            if (byte_count != 0) {
                throw std::runtime_error(
                    "FbCheckpoint::read_integer: zero integer has non-zero byte count");
            }
            x = Integer(static_cast<int64_t>(0));
            return;
        }
        if (byte_count == 0) {
            throw std::runtime_error(
                "FbCheckpoint::read_integer: non-zero integer has zero byte count");
        }
        if (static_cast<uint64_t>(byte_count) > remaining - minimum_trailer_bytes) {
            throw std::runtime_error("FbCheckpoint::read_integer: truncated body");
        }
        std::vector<unsigned char> buf(byte_count);
        if (byte_count > 0) {
            in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(byte_count));
            if (in.gcount() != static_cast<std::streamsize>(byte_count)) {
                throw std::runtime_error("FbCheckpoint::read_integer: truncated body");
            }
        }
        mpz_import(x.get_mpz(), byte_count, /*order=*/1, /*size=*/1,
                   /*endian=*/1, /*nails=*/0, buf.data());
        if (sgn < 0) {
            mpz_neg(x.get_mpz(), x.get_mpz());
        }
    }
};

} // namespace gnfs::factor_base

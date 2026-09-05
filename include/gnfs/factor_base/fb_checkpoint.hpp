#pragma once

#include "../core/integer.hpp"
#include "../core/polynomial_context.hpp"
#include "../core/types.hpp"
#include "factor_base.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
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
/// Binary layout:
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
///   u64 sieve_algebraic_count
struct FbCheckpoint {
    static constexpr uint64_t MAGIC = 0x474E465346434B50ULL;            // 'GNFSFCKP'
    static constexpr uint64_t MAGIC_INCOMPLETE = 0x474E465346434B4EULL; // 'GNFSFCKN'
    static constexpr uint64_t VERSION = 1;

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

    /// Rebuild FactorBase from this checkpoint.
    [[nodiscard]] FactorBase to_factor_base() const {
        if (sieve_algebraic_count > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
            throw std::overflow_error(
                "FbCheckpoint::to_factor_base: sieve algebraic count exceeds size_t");
        }
        if (sieve_algebraic_count > algebraic.size()) {
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
        fb.set_sieve_algebraic_count(static_cast<size_t>(sieve_algebraic_count));
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
        return ck;
    }

    void save(const std::string& path) const {
        if (rational.size() > max_serialized_count() || algebraic.size() > max_serialized_count()) {
            throw std::overflow_error("FbCheckpoint::save: factor-base count exceeds uint32_t");
        }
        if (sieve_algebraic_count > algebraic.size()) {
            throw std::runtime_error(
                "FbCheckpoint::save: sieve algebraic count exceeds algebraic count");
        }

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("FbCheckpoint::save: cannot open " + path);
        }

        uint64_t magic = MAGIC_INCOMPLETE;
        uint64_t version = VERSION;
        out.write(reinterpret_cast<const char*>(&magic), 8);
        out.write(reinterpret_cast<const char*>(&version), 8);

        out.write(reinterpret_cast<const char*>(&rational_bound), 4);
        out.write(reinterpret_cast<const char*>(&algebraic_bound), 4);
        out.write(reinterpret_cast<const char*>(&special_q_bound), 4);
        out.write(reinterpret_cast<const char*>(&large_prime_bound), 8);
        uint32_t scale_pad = static_cast<uint32_t>(log_scale); // pad to 4 bytes
        out.write(reinterpret_cast<const char*>(&scale_pad), 4);

        out.write(reinterpret_cast<const char*>(&ctx_degree), 4);
        write_integer(out, ctx_n);

        uint32_t rat_count = static_cast<uint32_t>(rational.size());
        out.write(reinterpret_cast<const char*>(&rat_count), 4);
        for (const auto& rp : rational) {
            out.write(reinterpret_cast<const char*>(&rp.p), 4);
            out.write(reinterpret_cast<const char*>(&rp.log_p), 4);
        }

        uint32_t alg_count = static_cast<uint32_t>(algebraic.size());
        out.write(reinterpret_cast<const char*>(&alg_count), 4);
        for (const auto& ap : algebraic) {
            out.write(reinterpret_cast<const char*>(&ap.p), 4);
            out.write(reinterpret_cast<const char*>(&ap.r), 4);
            out.write(reinterpret_cast<const char*>(&ap.log_p), 4);
            // degree is u8; pad to 4 bytes for alignment / future fields
            uint32_t deg_pad = static_cast<uint32_t>(ap.degree);
            out.write(reinterpret_cast<const char*>(&deg_pad), 4);
        }

        out.write(reinterpret_cast<const char*>(&sieve_algebraic_count), 8);

        out.flush();
        if (!out) {
            throw std::runtime_error("FbCheckpoint::save: write failed mid-stream");
        }

        out.seekp(0);
        magic = MAGIC;
        out.write(reinterpret_cast<const char*>(&magic), 8);
        out.flush();
        out.close();
    }

    static FbCheckpoint load(const std::string& path, bool allow_incomplete = false) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("FbCheckpoint::load: cannot open " + path);
        }

        uint64_t magic = 0, version = 0;
        in.read(reinterpret_cast<char*>(&magic), 8);
        in.read(reinterpret_cast<char*>(&version), 8);
        if (in.gcount() != 8) {
            throw std::runtime_error("FbCheckpoint::load: file too small");
        }
        if (magic != MAGIC && !(allow_incomplete && magic == MAGIC_INCOMPLETE)) {
            throw std::runtime_error("FbCheckpoint::load: invalid magic in " + path);
        }
        if (version != VERSION) {
            throw std::runtime_error("FbCheckpoint::load: version mismatch (got " +
                                     std::to_string(version) + ", expected " +
                                     std::to_string(VERSION) + ")");
        }

        FbCheckpoint ck;
        in.read(reinterpret_cast<char*>(&ck.rational_bound), 4);
        in.read(reinterpret_cast<char*>(&ck.algebraic_bound), 4);
        in.read(reinterpret_cast<char*>(&ck.special_q_bound), 4);
        in.read(reinterpret_cast<char*>(&ck.large_prime_bound), 8);
        uint32_t scale_pad = 0;
        in.read(reinterpret_cast<char*>(&scale_pad), 4);
        ck.log_scale = static_cast<uint8_t>(scale_pad);

        in.read(reinterpret_cast<char*>(&ck.ctx_degree), 4);
        read_integer(in, ck.ctx_n);

        uint32_t rat_count = 0;
        in.read(reinterpret_cast<char*>(&rat_count), 4);
        if (in.gcount() != 4) {
            throw std::runtime_error("FbCheckpoint::load: truncated rational header");
        }
        if (rat_count > 100'000'000u) {
            throw std::runtime_error("FbCheckpoint::load: rat_count corrupt");
        }
        ck.rational.clear();
        ck.rational.reserve(rat_count);
        for (size_t i = 0; i < static_cast<size_t>(rat_count); ++i) {
            RationalPrime rp;
            in.read(reinterpret_cast<char*>(&rp.p), 4);
            in.read(reinterpret_cast<char*>(&rp.log_p), 4);
            ck.rational.push_back(rp);
        }

        uint32_t alg_count = 0;
        in.read(reinterpret_cast<char*>(&alg_count), 4);
        if (in.gcount() != 4) {
            throw std::runtime_error("FbCheckpoint::load: truncated algebraic header");
        }
        if (alg_count > 100'000'000u) {
            throw std::runtime_error("FbCheckpoint::load: alg_count corrupt");
        }
        ck.algebraic.clear();
        ck.algebraic.reserve(alg_count);
        for (size_t i = 0; i < static_cast<size_t>(alg_count); ++i) {
            AlgebraicPrime ap;
            uint32_t deg_pad = 0;
            in.read(reinterpret_cast<char*>(&ap.p), 4);
            in.read(reinterpret_cast<char*>(&ap.r), 4);
            in.read(reinterpret_cast<char*>(&ap.log_p), 4);
            in.read(reinterpret_cast<char*>(&deg_pad), 4);
            ap.degree = static_cast<uint8_t>(deg_pad);
            ck.algebraic.push_back(ap);
        }

        in.read(reinterpret_cast<char*>(&ck.sieve_algebraic_count), 8);
        if (in.gcount() != 8) {
            throw std::runtime_error("FbCheckpoint::load: truncated sieve count");
        }

        return ck;
    }

    static void remove(const std::string& path) noexcept {
        std::remove(path.c_str());
    }

    static bool exists_and_valid(const std::string& path) noexcept {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        uint64_t magic = 0;
        in.read(reinterpret_cast<char*>(&magic), 8);
        return in.gcount() == 8 && magic == MAGIC;
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
    [[nodiscard]] static constexpr size_t max_serialized_count() noexcept {
        return static_cast<size_t>((std::numeric_limits<uint32_t>::max)());
    }

    static void write_integer(std::ofstream& out, const Integer& x) {
        const mpz_t& mz = x.get_mpz();
        int32_t sgn = mpz_sgn(mz);
        out.write(reinterpret_cast<const char*>(&sgn), 4);
        if (sgn == 0) {
            uint32_t zero = 0;
            out.write(reinterpret_cast<const char*>(&zero), 4);
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
        out.write(reinterpret_cast<const char*>(&bc), 4);
        if (bc > 0) {
            out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(bc));
        }
    }

    static void read_integer(std::ifstream& in, Integer& x) {
        int32_t sgn = 0;
        uint32_t byte_count = 0;
        in.read(reinterpret_cast<char*>(&sgn), 4);
        in.read(reinterpret_cast<char*>(&byte_count), 4);
        if (in.gcount() != 4) {
            throw std::runtime_error("FbCheckpoint::read_integer: truncated header");
        }
        if (byte_count > (1u << 30)) {
            throw std::runtime_error("FbCheckpoint::read_integer: byte_count too large");
        }
        if (sgn == 0) {
            x = Integer(static_cast<int64_t>(0));
            return;
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

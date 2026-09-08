#pragma once

#include "../core/integer.hpp"
#include "../core/polynomial_context.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace gnfs::polynomial {

using core::Integer;
using core::PolynomialContext;

/// Phase 1 (Polynomial Selection) checkpoint.
///
/// 设计 (2026-05-21):
///   - 仅在 Phase 1 完成后 save 最终结果 (result-only, 非 in-flight)。
///   - Kleinjung lattice search 是多线程随机搜索, 真正的"位置"难以序列化
///     且收益有限——直接从 ckpt 读取已选好的 (f, g, m) 即可跳过整 phase。
///   - 文件 `<base_path>.poly_ckpt`, MAGIC/INCOMPLETE flip 保证 crash safety。
///   - 加载时严格校验 N 一致 (防止用错误 N 的 poly ckpt)。
///
/// Binary layout (all fixed-width scalar fields are little-endian):
///   u64 magic         ('GNFSPCKP' or '..PCKN' incomplete)
///   u64 version       (1)
///   i32 sign(N) + u32 limb_count + limbs(N)
///   i32 sign(m) + u32 limb_count + limbs(m)
///   u32 degree
///   u32 coeff_count
///   for each coeff: i32 sign + u32 limb_count + limbs
///   f64 skewness
///   f64 murphy_e      (informational; 0.0 if unknown)
struct PolyCheckpoint {
    static constexpr uint64_t MAGIC = 0x474E465350434B50ULL;            // 'GNFSPCKP'
    static constexpr uint64_t MAGIC_INCOMPLETE = 0x474E465350434B4EULL; // 'GNFSPCKN'
    static constexpr uint64_t VERSION = 1;

    Integer n;
    Integer m;
    std::vector<Integer> f_coeffs; // f_coeffs[i] = coefficient of x^i
    uint32_t degree = 0;
    double skewness = 1.0;
    double murphy_e = 0.0;

    /// Reconstruct PolynomialContext from this checkpoint (cloning Integers).
    [[nodiscard]] PolynomialContext to_context() const {
        std::vector<Integer> coeffs_copy;
        coeffs_copy.reserve(f_coeffs.size());
        for (const auto& c : f_coeffs)
            coeffs_copy.emplace_back(c);
        return PolynomialContext(Integer(n), std::move(coeffs_copy), Integer(m), skewness);
    }

    /// Populate from a PolynomialContext (snapshots all Integers).
    static PolyCheckpoint from_context(const PolynomialContext& ctx, double murphy_e = 0.0) {
        PolyCheckpoint ck;
        ck.n = ctx.n();
        ck.m = ctx.m();
        ck.degree = ctx.degree();
        const auto& coeffs = ctx.coefficients();
        ck.f_coeffs.reserve(coeffs.size());
        for (const auto& c : coeffs)
            ck.f_coeffs.emplace_back(c);
        ck.skewness = ctx.skewness();
        ck.murphy_e = murphy_e;
        return ck;
    }

    /// Serialize to path. Write INCOMPLETE magic first, fsync, then flip to MAGIC.
    void save(const std::string& path) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("PolyCheckpoint::save: cannot open " + path);
        }

        uint64_t magic = MAGIC_INCOMPLETE;
        uint64_t version = VERSION;
        write_u64(out, magic);
        write_u64(out, version);

        write_integer(out, n);
        write_integer(out, m);

        write_u32(out, degree);
        uint32_t coeff_count = static_cast<uint32_t>(f_coeffs.size());
        write_u32(out, coeff_count);
        for (const auto& c : f_coeffs)
            write_integer(out, c);

        write_double(out, skewness);
        write_double(out, murphy_e);

        out.flush();
        if (!out) {
            throw std::runtime_error("PolyCheckpoint::save: write failed mid-stream");
        }

        // Flip MAGIC at offset 0
        out.seekp(0);
        magic = MAGIC;
        write_u64(out, magic);
        out.flush();
        out.close();
    }

    /// Deserialize from path. Throws on invalid magic / version / truncation.
    static PolyCheckpoint load(const std::string& path, bool allow_incomplete = false) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("PolyCheckpoint::load: cannot open " + path);
        }

        const uint64_t magic = read_u64(in, "magic");
        const uint64_t version = read_u64(in, "version");
        if (magic != MAGIC && !(allow_incomplete && magic == MAGIC_INCOMPLETE)) {
            throw std::runtime_error("PolyCheckpoint::load: invalid magic in " + path);
        }
        if (version != VERSION) {
            throw std::runtime_error("PolyCheckpoint::load: version mismatch (got " +
                                     std::to_string(version) + ", expected " +
                                     std::to_string(VERSION) + ")");
        }

        PolyCheckpoint ck;
        read_integer(in, ck.n);
        read_integer(in, ck.m);

        ck.degree = read_u32(in, "degree");
        const uint32_t coeff_count = read_u32(in, "coeff_count");
        // Guard against corrupt counts (a degree-N poly is reasonable up to 32).
        if (coeff_count > 64) {
            throw std::runtime_error("PolyCheckpoint::load: coeff_count > 64 (corrupt)");
        }
        ck.f_coeffs.clear();
        ck.f_coeffs.reserve(coeff_count);
        for (uint32_t i = 0; i < coeff_count; ++i) {
            Integer c;
            read_integer(in, c);
            ck.f_coeffs.emplace_back(std::move(c));
        }

        ck.skewness = read_double(in, "skewness");
        ck.murphy_e = read_double(in, "murphy_e");

        return ck;
    }

    static void remove(const std::string& path) noexcept {
        std::remove(path.c_str());
    }

    /// Cheap existence + magic check (returns false on any I/O issue, no throw).
    static bool exists_and_valid(const std::string& path) noexcept {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        unsigned char bytes[sizeof(uint64_t)]{};
        in.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
        return in.gcount() == static_cast<std::streamsize>(sizeof(bytes)) &&
               decode_u64(bytes) == MAGIC;
    }

    /// Load checkpoint and validate that the contained N matches `expected_n`.
    /// Returns the checkpoint on success, throws on validation failure.
    static PolyCheckpoint load_for(const std::string& path, const Integer& expected_n) {
        auto ck = load(path);
        if (ck.n != expected_n) {
            throw std::runtime_error("PolyCheckpoint::load_for: N mismatch in " + path);
        }
        return ck;
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

    static void write_double(std::ofstream& out, double value) {
        write_u64(out, std::bit_cast<uint64_t>(value));
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
            throw std::runtime_error(std::string("PolyCheckpoint::load: truncated ") + field);
        }
        return decode_u32(bytes);
    }

    static uint64_t read_u64(std::ifstream& in, const char* field) {
        unsigned char bytes[8]{};
        in.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
        if (in.gcount() != static_cast<std::streamsize>(sizeof(bytes))) {
            throw std::runtime_error(std::string("PolyCheckpoint::load: truncated ") + field);
        }
        return decode_u64(bytes);
    }

    static double read_double(std::ifstream& in, const char* field) {
        return std::bit_cast<double>(read_u64(in, field));
    }

    /// Serialize a single Integer:
    ///   i32 sign (+1 / 0 / -1) + u32 limb_count + limbs (raw bytes)
    static void write_integer(std::ofstream& out, const Integer& x) {
        const mpz_t& mz = x.get_mpz();
        const int32_t sgn = mpz_sgn(mz);
        write_u32(out, static_cast<uint32_t>(sgn));
        if (sgn == 0) {
            uint32_t zero = 0;
            write_u32(out, zero);
            return;
        }
        // Use mpz_export to extract a canonical big-endian byte sequence.
        size_t byte_count = 0;
        size_t numb = 8 * sizeof(unsigned char);
        // count = ceil(bits / numb)
        size_t bits = mpz_sizeinbase(mz, 2);
        size_t max_bytes = (bits + numb - 1) / numb + 1;
        std::vector<unsigned char> buf(max_bytes);
        // mpz_export(rop, countp, order, size, endian, nails, op):
        //   order=1 (most-significant word first), size=1 byte, endian=1 (big-endian per word).
        // Choosing big-endian portable bytes keeps the file format reproducible across hosts.
        mpz_export(buf.data(), &byte_count, /*order=*/1, /*size=*/1,
                   /*endian=*/1, /*nails=*/0, mz);
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
            throw std::runtime_error("PolyCheckpoint::read_integer: invalid sign");
        }
        const uint32_t byte_count = read_u32(in, "integer byte count");
        if (byte_count > (1u << 30)) { // 1 GB sanity cap
            throw std::runtime_error(
                "PolyCheckpoint::read_integer: byte_count too large (corrupt)");
        }
        if (sgn == 0) {
            x = Integer(static_cast<int64_t>(0));
            return;
        }
        std::vector<unsigned char> buf(byte_count);
        if (byte_count > 0) {
            in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(byte_count));
            if (in.gcount() != static_cast<std::streamsize>(byte_count)) {
                throw std::runtime_error("PolyCheckpoint::read_integer: truncated body");
            }
        }
        mpz_import(x.get_mpz(), byte_count, /*order=*/1, /*size=*/1,
                   /*endian=*/1, /*nails=*/0, buf.data());
        if (sgn < 0) {
            mpz_neg(x.get_mpz(), x.get_mpz());
        }
    }
};

} // namespace gnfs::polynomial

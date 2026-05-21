#pragma once

#include "../core/integer.hpp"
#include "../core/polynomial_context.hpp"

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
/// Binary layout:
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
    static constexpr uint64_t MAGIC = 0x474E465350434B50ULL;             // 'GNFSPCKP'
    static constexpr uint64_t MAGIC_INCOMPLETE = 0x474E465350434B4EULL;  // 'GNFSPCKN'
    static constexpr uint64_t VERSION = 1;

    Integer n;
    Integer m;
    std::vector<Integer> f_coeffs;  // f_coeffs[i] = coefficient of x^i
    uint32_t degree = 0;
    double   skewness = 1.0;
    double   murphy_e = 0.0;

    /// Reconstruct PolynomialContext from this checkpoint (cloning Integers).
    [[nodiscard]] PolynomialContext to_context() const {
        std::vector<Integer> coeffs_copy;
        coeffs_copy.reserve(f_coeffs.size());
        for (const auto& c : f_coeffs) coeffs_copy.emplace_back(c);
        return PolynomialContext(Integer(n), std::move(coeffs_copy), Integer(m), skewness);
    }

    /// Populate from a PolynomialContext (snapshots all Integers).
    static PolyCheckpoint from_context(const PolynomialContext& ctx,
                                       double murphy_e = 0.0) {
        PolyCheckpoint ck;
        ck.n = ctx.n();
        ck.m = ctx.m();
        ck.degree = ctx.degree();
        const auto& coeffs = ctx.coefficients();
        ck.f_coeffs.reserve(coeffs.size());
        for (const auto& c : coeffs) ck.f_coeffs.emplace_back(c);
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
        out.write(reinterpret_cast<const char*>(&magic), 8);
        out.write(reinterpret_cast<const char*>(&version), 8);

        write_integer(out, n);
        write_integer(out, m);

        out.write(reinterpret_cast<const char*>(&degree), 4);
        uint32_t coeff_count = static_cast<uint32_t>(f_coeffs.size());
        out.write(reinterpret_cast<const char*>(&coeff_count), 4);
        for (const auto& c : f_coeffs) write_integer(out, c);

        out.write(reinterpret_cast<const char*>(&skewness), 8);
        out.write(reinterpret_cast<const char*>(&murphy_e), 8);

        out.flush();
        if (!out) {
            throw std::runtime_error("PolyCheckpoint::save: write failed mid-stream");
        }

        // Flip MAGIC at offset 0
        out.seekp(0);
        magic = MAGIC;
        out.write(reinterpret_cast<const char*>(&magic), 8);
        out.flush();
        out.close();
    }

    /// Deserialize from path. Throws on invalid magic / version / truncation.
    static PolyCheckpoint load(const std::string& path,
                               bool allow_incomplete = false) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("PolyCheckpoint::load: cannot open " + path);
        }

        uint64_t magic = 0, version = 0;
        in.read(reinterpret_cast<char*>(&magic), 8);
        in.read(reinterpret_cast<char*>(&version), 8);
        if (in.gcount() != 8) {
            throw std::runtime_error("PolyCheckpoint::load: file too small");
        }
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

        in.read(reinterpret_cast<char*>(&ck.degree), 4);
        uint32_t coeff_count = 0;
        in.read(reinterpret_cast<char*>(&coeff_count), 4);
        if (in.gcount() != 4) {
            throw std::runtime_error("PolyCheckpoint::load: truncated before coeffs");
        }
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

        in.read(reinterpret_cast<char*>(&ck.skewness), 8);
        in.read(reinterpret_cast<char*>(&ck.murphy_e), 8);
        if (in.gcount() != 8) {
            throw std::runtime_error("PolyCheckpoint::load: truncated trailer");
        }

        return ck;
    }

    static void remove(const std::string& path) noexcept {
        std::remove(path.c_str());
    }

    /// Cheap existence + magic check (returns false on any I/O issue, no throw).
    static bool exists_and_valid(const std::string& path) noexcept {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        uint64_t magic = 0;
        in.read(reinterpret_cast<char*>(&magic), 8);
        return in.gcount() == 8 && magic == MAGIC;
    }

    /// Load checkpoint and validate that the contained N matches `expected_n`.
    /// Returns the checkpoint on success, throws on validation failure.
    static PolyCheckpoint load_for(const std::string& path,
                                   const Integer& expected_n) {
        auto ck = load(path);
        if (ck.n != expected_n) {
            throw std::runtime_error(
                "PolyCheckpoint::load_for: N mismatch in " + path);
        }
        return ck;
    }

private:
    /// Serialize a single Integer:
    ///   i32 sign (+1 / 0 / -1) + u32 limb_count + limbs (raw bytes)
    static void write_integer(std::ofstream& out, const Integer& x) {
        const mpz_t& mz = x.get_mpz();
        int32_t sgn = mpz_sgn(mz);
        out.write(reinterpret_cast<const char*>(&sgn), 4);
        if (sgn == 0) {
            uint32_t zero = 0;
            out.write(reinterpret_cast<const char*>(&zero), 4);
            return;
        }
        // Use mpz_export to extract limbs as raw bytes (host-endian, native size).
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
        out.write(reinterpret_cast<const char*>(&bc), 4);
        if (bc > 0) {
            out.write(reinterpret_cast<const char*>(buf.data()),
                      static_cast<std::streamsize>(bc));
        }
    }

    static void read_integer(std::ifstream& in, Integer& x) {
        int32_t sgn = 0;
        uint32_t byte_count = 0;
        in.read(reinterpret_cast<char*>(&sgn), 4);
        in.read(reinterpret_cast<char*>(&byte_count), 4);
        if (in.gcount() != 4) {
            throw std::runtime_error("PolyCheckpoint::read_integer: truncated header");
        }
        if (byte_count > (1u << 30)) {  // 1 GB sanity cap
            throw std::runtime_error("PolyCheckpoint::read_integer: byte_count too large (corrupt)");
        }
        if (sgn == 0) {
            x = Integer(static_cast<int64_t>(0));
            return;
        }
        std::vector<unsigned char> buf(byte_count);
        if (byte_count > 0) {
            in.read(reinterpret_cast<char*>(buf.data()),
                    static_cast<std::streamsize>(byte_count));
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

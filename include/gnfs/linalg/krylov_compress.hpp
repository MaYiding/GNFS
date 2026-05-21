#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace gnfs::linalg {

/// Self-contained byte-RLE compressor for BW Krylov sequence chunks.
///
/// Design rationale (see task_plan.md / BACKLOG #17 OPT):
///
/// 1. Krylov sequences for BW Phase 1 store either 64×64 GF(2) blocks
///    (matrix BM, 512 B each) or 64 long bit-streams (scalar BM, 1 byte/bit).
///    Block density varies — early Krylov iterates are sparse (mostly identical
///    to V_0 = Y), late iterates may be ~50% dense.
///
/// 2. XOR-delta of consecutive blocks (block_k XOR block_{k-1}) exposes that
///    consecutive Krylov iterates differ in low Hamming weight rows, so the
///    delta stream has long zero runs ideal for RLE.
///
/// 3. Byte-level RLE chosen over bit-level: cross-platform identical (no
///    endian-dependent bit packing), faster decode (~1 GB/s on M5 P-core),
///    and worst-case bound is +1.5% on incompressible data.
///
/// 4. Self-contained: no external libs (LZ4 / zstd / zlib forbidden per project
///    cross-platform CI constraint).
///
/// Wire format of a compressed chunk (after delta computation, before RLE):
///
///   [MAGIC: 4 bytes "KRYZ"]
///   [VERSION: u8 = 1]
///   [FLAGS: u8 — bit 0 = 1 if delta-encoded]
///   [PADDING: 2 bytes 0]
///   [UNCOMPRESSED_SIZE: u64 LE]
///   [RLE PAYLOAD]
///
/// RLE payload encoding (per token):
///
///   if HEADER_BYTE & 0x80 == 0:
///     literal run; len = HEADER_BYTE + 1 (1..128 literal bytes follow)
///   else:
///     repeat run; len = (HEADER_BYTE & 0x7F) + 3 (3..130 copies of one byte)
///     next byte = the repeated value
///
/// "Literal run" carries up to 128 bytes verbatim. "Repeat run" carries
/// a 1-byte value repeated 3..130 times. A run of length 2 always encodes
/// as literal (smaller). This guarantees worst-case blowup of
///   ceil(n / 128) bytes of headers, i.e. < 0.8% over n bytes.
class KrylovCompressor {
public:
    static constexpr uint32_t MAGIC = 0x5A59524BU;  // "KRYZ" little-endian: 'K'(0x4B) 'R'(0x52) 'Y'(0x59) 'Z'(0x5A)
    static constexpr uint8_t VERSION = 1;
    static constexpr uint8_t FLAG_DELTA = 0x01;
    static constexpr size_t HEADER_BYTES = 16;
    static constexpr size_t MAX_LITERAL_RUN = 128;
    static constexpr size_t MAX_REPEAT_RUN = 130;
    static constexpr size_t MIN_REPEAT_RUN = 3;

    /// Compress `in_size` bytes from `in` into a self-contained chunk.
    /// If `apply_delta` is true, the input is XOR-delta'd in `block_stride`
    /// blocks first (caller must guarantee in_size % block_stride == 0).
    /// Setting block_stride = 0 disables delta (raw byte RLE only).
    static std::vector<uint8_t> compress_chunk(
        const uint8_t* in, size_t in_size,
        size_t block_stride = 0) {

        if (in == nullptr || in_size == 0) {
            // Empty input: emit header with size=0, no payload.
            std::vector<uint8_t> out;
            out.reserve(HEADER_BYTES);
            write_header(out, in_size, /*delta=*/false);
            return out;
        }

        const bool apply_delta = (block_stride > 0);
        if (apply_delta && (in_size % block_stride != 0)) {
            throw std::invalid_argument(
                "KrylovCompressor::compress_chunk: in_size must be a multiple of block_stride");
        }

        // Step 1: optional XOR-delta into scratch buffer
        std::vector<uint8_t> scratch;
        const uint8_t* payload = in;
        if (apply_delta) {
            scratch.resize(in_size);
            // First block: verbatim
            std::memcpy(scratch.data(), in, block_stride);
            // Subsequent blocks: XOR against previous block
            for (size_t off = block_stride; off < in_size; off += block_stride) {
                for (size_t b = 0; b < block_stride; ++b) {
                    scratch[off + b] =
                        static_cast<uint8_t>(in[off + b] ^ in[off - block_stride + b]);
                }
            }
            payload = scratch.data();
        }

        // Step 2: byte RLE
        std::vector<uint8_t> out;
        out.reserve(HEADER_BYTES + in_size + (in_size / 128) + 8);
        write_header(out, in_size, apply_delta);

        size_t i = 0;
        while (i < in_size) {
            // Probe for a run of repeated bytes
            const uint8_t cur = payload[i];
            size_t run_len = 1;
            while (i + run_len < in_size &&
                   payload[i + run_len] == cur &&
                   run_len < MAX_REPEAT_RUN) {
                ++run_len;
            }

            if (run_len >= MIN_REPEAT_RUN) {
                // Emit repeat token
                out.push_back(static_cast<uint8_t>(
                    0x80 | static_cast<uint8_t>(run_len - MIN_REPEAT_RUN)));
                out.push_back(cur);
                i += run_len;
            } else {
                // Accumulate literal bytes until we either hit MAX_LITERAL_RUN
                // or detect a profitable repeat run ahead. We need to be a bit
                // careful: stop the literal run when the next 3+ bytes are
                // identical, so the next iteration emits an efficient repeat.
                size_t lit_start = i;
                size_t lit_len = 0;
                while (i < in_size && lit_len < MAX_LITERAL_RUN) {
                    // Look-ahead: peek at next ≥3 identical bytes
                    if (i + MIN_REPEAT_RUN <= in_size) {
                        const uint8_t v = payload[i];
                        if (payload[i + 1] == v && payload[i + 2] == v) {
                            break;  // bail; let next outer loop emit a repeat
                        }
                    }
                    ++i;
                    ++lit_len;
                }
                if (lit_len == 0) {
                    // Defensive: must consume at least 1 byte to avoid infinite loop
                    ++i;
                    lit_len = 1;
                }
                // Emit literal header: high bit clear, low 7 = lit_len - 1
                out.push_back(static_cast<uint8_t>(lit_len - 1));
                out.insert(out.end(), payload + lit_start, payload + lit_start + lit_len);
            }
        }

        return out;
    }

    /// Decompress a chunk into `out` (must be sized `out_size`).
    /// Returns false on any format error (bad magic, version, size mismatch,
    /// truncated payload). The output buffer must be exactly the right size —
    /// caller knows uncompressed_size from chunk index.
    [[nodiscard]] static bool decompress_chunk(
        const uint8_t* in, size_t in_size,
        uint8_t* out, size_t out_size,
        size_t block_stride = 0) noexcept {

        if (in == nullptr || in_size < HEADER_BYTES) return false;

        // Parse header
        uint32_t magic;
        std::memcpy(&magic, in, 4);
        if (magic != MAGIC) return false;

        const uint8_t version = in[4];
        if (version != VERSION) return false;

        const uint8_t flags = in[5];
        const bool delta_flag = (flags & FLAG_DELTA) != 0;

        // PADDING bytes 6,7 are reserved; do not validate (forward compat)

        uint64_t uncompressed_size;
        std::memcpy(&uncompressed_size, in + 8, 8);
        if (uncompressed_size != out_size) return false;

        if (delta_flag && block_stride == 0) {
            // Caller expected no delta but chunk says delta — mismatch
            return false;
        }
        if (!delta_flag && block_stride > 0) {
            // Caller expected delta but chunk says raw — mismatch
            return false;
        }
        if (delta_flag && (out_size % block_stride != 0)) return false;

        // Decode RLE into out buffer (or scratch if delta-encoded)
        // First pass writes raw bytes (post-RLE); delta inverse fixes up later.
        size_t in_pos = HEADER_BYTES;
        size_t out_pos = 0;

        while (in_pos < in_size) {
            const uint8_t header = in[in_pos++];
            if ((header & 0x80) == 0) {
                // Literal run
                const size_t lit_len = static_cast<size_t>(header) + 1;
                if (in_pos + lit_len > in_size) return false;
                if (out_pos + lit_len > out_size) return false;
                std::memcpy(out + out_pos, in + in_pos, lit_len);
                in_pos += lit_len;
                out_pos += lit_len;
            } else {
                // Repeat run
                const size_t rep_len = static_cast<size_t>(header & 0x7F) + MIN_REPEAT_RUN;
                if (in_pos + 1 > in_size) return false;
                const uint8_t v = in[in_pos++];
                if (out_pos + rep_len > out_size) return false;
                std::memset(out + out_pos, v, rep_len);
                out_pos += rep_len;
            }
        }

        if (out_pos != out_size) return false;
        if (in_pos != in_size) return false;

        // Step 3 (optional): undo XOR-delta in-place. block_k = delta_k XOR block_{k-1}
        // for k > 0; block_0 already verbatim.
        if (delta_flag) {
            for (size_t off = block_stride; off < out_size; off += block_stride) {
                for (size_t b = 0; b < block_stride; ++b) {
                    out[off + b] =
                        static_cast<uint8_t>(out[off + b] ^ out[off - block_stride + b]);
                }
            }
        }

        return true;
    }

    /// Inspect chunk header without full decode. Useful for cache routing.
    /// Returns 0 if header invalid; otherwise uncompressed payload size.
    [[nodiscard]] static uint64_t peek_uncompressed_size(
        const uint8_t* in, size_t in_size) noexcept {

        if (in == nullptr || in_size < HEADER_BYTES) return 0;
        uint32_t magic;
        std::memcpy(&magic, in, 4);
        if (magic != MAGIC) return 0;
        if (in[4] != VERSION) return 0;
        uint64_t sz;
        std::memcpy(&sz, in + 8, 8);
        return sz;
    }

private:
    static void write_header(std::vector<uint8_t>& out, uint64_t uncompressed_size,
                             bool delta) {
        const size_t start = out.size();
        out.resize(start + HEADER_BYTES);
        std::memcpy(out.data() + start, &MAGIC, 4);
        out[start + 4] = VERSION;
        out[start + 5] = delta ? FLAG_DELTA : 0;
        out[start + 6] = 0;
        out[start + 7] = 0;
        std::memcpy(out.data() + start + 8, &uncompressed_size, 8);
    }
};

}  // namespace gnfs::linalg

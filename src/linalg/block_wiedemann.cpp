#include "gnfs/linalg/block_wiedemann.hpp"
#include "gnfs/linalg/detail/spmv_kernels.hpp"
#include "gnfs/linalg/krylov_sequence_compressed.hpp"
#include "gnfs/linalg/krylov_sequence_mmap.hpp"
#include "gnfs/util/bit_intrin.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/thread_pool.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace gnfs::linalg {

// ============================================================================
// Multi-stream Krylov parallelisation (GNFS_BW_KRYLOV_STREAMS)
// ============================================================================
// Default K=1 → existing serial multi-seed retry behaviour (no regression).
// K>1 → run K independent (seed, stream) workers concurrently. Each worker is
// an isolated block_solve_view_impl / thin_solve_view_impl instance with its
// own internal ThreadPool sized at max(1, hardware_concurrency/K). The first
// worker(s) to produce non-empty deps win; all results are merged and
// deduplicated. Trades extra concurrency for shorter wall-time on small-to-
// medium matrices where pool-barrier overhead dominates per-SpMV inner work.
namespace {

constexpr uint32_t kMaxBwStreams = 16;

inline uint32_t bw_num_streams() noexcept {
    const char* env = std::getenv("GNFS_BW_KRYLOV_STREAMS");
    if (env == nullptr || env[0] == '\0') return 1;
    char* end = nullptr;
    long v = std::strtol(env, &end, 10);
    if (end == env || v < 1) return 1;
    if (v > static_cast<long>(kMaxBwStreams)) return kMaxBwStreams;
    return static_cast<uint32_t>(v);
}

inline uint32_t bw_pool_size_per_stream(uint32_t num_streams) noexcept {
    uint32_t hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    if (num_streams == 0) num_streams = 1;
    uint32_t per = hw / num_streams;
    if (per == 0) per = 1;
    return per;
}

// ENV `GNFS_BW_KRYLOV_COMPRESS=1`: opt-in chunked compression of the Krylov
// sequence file. Caller must also have GNFS_BW_KRYLOV_MMAP=1 (compression is
// layered on the mmap path). When set, KrylovSequenceCompressed replaces
// KrylovSequenceMmap; the disk file grows as XOR-delta + byte-RLE chunks
// rather than a raw entry array. Default 0 (no compression, no regression).
inline bool bw_use_compress() noexcept {
    const char* env = std::getenv("GNFS_BW_KRYLOV_COMPRESS");
    return env != nullptr && env[0] == '1';
}

// Stream-aware random seeds. Use a fixed prime stride so reruns under the
// same K are deterministic. Seeds list is independent of, but compatible with,
// the existing 3-seed retry constants (so K=1 still hits seed=42 first).
inline uint64_t bw_stream_seed(uint64_t base_seed, uint32_t stream_id) noexcept {
    static constexpr uint64_t kStride = 0x9E3779B97F4A7C15ULL;  // golden-ratio prime
    return base_seed + static_cast<uint64_t>(stream_id) * kStride;
}

}  // namespace

// Forward declaration: defined below near the view-overload public API; the
// SparseMatrix BlockWiedemann::find_dependencies() routes here when K>1.
template <MatrixView MV>
static std::vector<std::vector<bool>> find_dependencies_view_impl(
    const MV& matrix, size_t max_deps);

// ============================================================================
// SpMV utilities — moved to gnfs/linalg/detail/spmv_kernels.hpp so that the
// BW solvers can run over any MatrixView (CSRMatrix or MmapCSRMatrix), and
// SGE/Phase 5 can flip between in-memory and out-of-core matrix storage
// without losing the prefetch / persistent-scratch optimisations.
// All call sites below use detail::spmv_forward / spmv_transpose / spmv_B /
// spmv_B_prime instantiated with CSRMatrix (the standard in-memory path).
// ============================================================================

namespace {

// Thin in-file alias wrappers so the historical call sites below
// (bw_spmv_forward / bw_spmv_B / ...) stay the same. Templated on the
// matrix view so the same call sites work for CSRMatrix (in-memory,
// default path) and MmapCSRMatrix (out-of-core, Pipeline auto-route).
template <MatrixView MV>
inline void bw_spmv_forward(const MV& M, const BlockVector& x, BlockVector& y,
                            gnfs::util::ThreadPool& pool) {
    detail::spmv_forward(M, x, y, pool);
}
template <MatrixView MV>
inline void bw_spmv_transpose(const MV& M, const BlockVector& x, BlockVector& y,
                              gnfs::util::ThreadPool& pool) {
    detail::spmv_transpose(M, x, y, pool);
}
template <MatrixView MV>
inline void bw_spmv_B(const MV& M, const BlockVector& x, BlockVector& y,
                      BlockVector& tmp, gnfs::util::ThreadPool& pool) {
    detail::spmv_B(M, x, y, tmp, pool);
}
template <MatrixView MV>
inline void bw_spmv_B_prime(const MV& M, const BlockVector& x, BlockVector& y,
                            BlockVector& tmp, gnfs::util::ThreadPool& pool) {
    detail::spmv_B_prime(M, x, y, tmp, pool);
}

// ============================================================================
// Scalar Berlekamp-Massey over GF(2)
// ============================================================================
// Given a binary sequence s[0..N-1], find the shortest LFSR that generates it.
// Returns the LFSR polynomial coefficients (connection polynomial).
// The polynomial p(t) = 1 + c_1*t + c_2*t^2 + ... + c_L*t^L satisfies:
//   s[n] + c_1*s[n-1] + ... + c_L*s[n-L] = 0 for all n >= L.
//
// For the Wiedemann algorithm: if p(t) = t^d + ... has p(0) = 0 (constant
// term is 0, i.e., p(t) is divisible by t), then q(t) = p(t)/t gives
// a null vector w = q(B) * v.

struct LFSRPolynomial {
    std::vector<uint8_t> coeffs;  // coeffs[0] = constant term
    size_t degree = 0;
};

// Bit-packed GF(2) vector for fast BM
struct BitPoly {
    std::vector<uint64_t> words;  // packed bits, word[i] bit j = coefficient i*64+j
    size_t len = 0;               // number of coefficients

    BitPoly() = default;
    explicit BitPoly(size_t n) : words((n + 63) / 64, 0), len(n) {}

    void set(size_t i) {
        if (i >= len) resize(i + 1);
        words[i / 64] |= (1ULL << (i % 64));
    }
    bool get(size_t i) const {
        if (i >= len) return false;
        return (words[i / 64] >> (i % 64)) & 1;
    }
    void flip(size_t i) {
        if (i >= len) resize(i + 1);
        words[i / 64] ^= (1ULL << (i % 64));
    }
    void resize(size_t n) {
        len = n;
        words.resize((n + 63) / 64, 0);
    }
    void xor_shifted(const BitPoly& other, size_t shift) {
        // this ^= other << shift (in coefficient space)
        size_t needed = other.len + shift;
        if (needed > len) resize(needed);
        size_t word_shift = shift / 64;
        size_t bit_shift = shift % 64;
        size_t ow = other.words.size();
        if (bit_shift == 0) {
            for (size_t i = 0; i < ow; ++i)
                words[i + word_shift] ^= other.words[i];
        } else {
            for (size_t i = 0; i < ow; ++i) {
                words[i + word_shift] ^= (other.words[i] << bit_shift);
                if (i + word_shift + 1 < words.size())
                    words[i + word_shift + 1] ^= (other.words[i] >> (64 - bit_shift));
            }
        }
    }

    // Convert to uint8_t vector for compatibility
    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> out(len, 0);
        for (size_t i = 0; i < len; ++i)
            out[i] = get(i) ? 1 : 0;
        return out;
    }
};

// Compute discrepancy: d = s[n] XOR (C·s_reversed) using 64-bit word AND+popcount
// C = connection poly coeffs (packed), s_window = s[n-L..n] reversed (packed)
static uint8_t compute_discrepancy_packed(
    const BitPoly& C, const std::vector<uint8_t>& s,
    size_t n, size_t L) {
    // d = s[n] + sum_{i=1}^{L} C[i] * s[n-i]
    // Pack s[n-1], s[n-2], ..., s[n-L] into words, AND with C[1..L], popcount
    uint8_t d = s[n];
    size_t nw = (L + 63) / 64;
    uint64_t parity = 0;
    for (size_t w = 0; w < nw && w < C.words.size(); ++w) {
        // C word w covers coefficients [w*64 .. w*64+63]
        // We need C[i] * s[n-i] for i = w*64..w*64+63
        // Build s_word: bit j = s[n - (w*64 + j + 1)] for j=0..63 (skip i=0, start from i=1)
        uint64_t s_word = 0;
        size_t base_i = w * 64 + 1;  // i starts at 1 (skip C[0])
        for (size_t j = 0; j < 64 && (base_i + j) <= L; ++j) {
            size_t i = base_i + j;
            if (i <= n && s[n - i])
                s_word |= (1ULL << j);
        }
        // C_word shifted: C[w*64+1..w*64+64] packed as bits 0..63
        uint64_t c_word;
        if (w == 0) {
            // Need C[1..64], but C.words[0] has C[0..63]
            // Shift right by 1 to get C[1..64] in bits 0..63
            c_word = C.words[0] >> 1;
            if (C.words.size() > 1)
                c_word |= (C.words[1] << 63);
        } else {
            // C[w*64+1..] — need bits from words[w] shifted
            size_t bit_off = w * 64 + 1;
            size_t wi = bit_off / 64;
            size_t bi = bit_off % 64;
            c_word = (wi < C.words.size()) ? (C.words[wi] >> bi) : 0;
            if (bi > 0 && wi + 1 < C.words.size())
                c_word |= (C.words[wi + 1] << (64 - bi));
        }
        parity ^= static_cast<uint64_t>(gnfs::util::popcount64(c_word & s_word));
    }
    d ^= (parity & 1);
    return d;
}

LFSRPolynomial scalar_berlekamp_massey(const std::vector<uint8_t>& s) {
    const size_t N = s.size();
    if (N == 0) return {{1}, 0};

    // Bit-packed connection polynomial C (starts as 1)
    BitPoly C(1); C.set(0);
    // Previous polynomial B
    BitPoly B(1); B.set(0);
    size_t L = 0;     // Current LFSR length
    size_t m = 1;     // Shift counter

    for (size_t n = 0; n < N; ++n) {
        // Compute discrepancy using packed word operations
        uint8_t d;
        if (L <= 128) {
            // Small L: scalar path (avoid overhead of packing)
            d = s[n];
            for (size_t i = 1; i <= L && i <= n; ++i) {
                if (C.get(i)) d ^= s[n - i];
            }
        } else {
            d = compute_discrepancy_packed(C, s, n, L);
        }

        if (d == 0) {
            m++;
        } else {
            BitPoly T = C;  // Save C

            // C = C + B * x^m
            C.xor_shifted(B, m);

            if (2 * L <= n) {
                L = n + 1 - L;
                B = std::move(T);
                m = 1;
            } else {
                m++;
            }
        }
    }

    LFSRPolynomial result;
    result.coeffs = C.to_bytes();
    result.degree = L;
    return result;
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

std::vector<std::vector<bool>> BlockWiedemann::find_dependencies(
    const SparseMatrix& matrix, size_t max_deps) {

    const size_t m = matrix.num_rows();
    const size_t n = matrix.num_cols();
    if (m == 0 || n == 0) return {};

    // BACKLOG #80 step 7: thin matrix (m<n) over GF(2) requires a different
    // operator (B' = M^T·M) than the standard B = M·M^T, because over GF(2)
    // null(B) ⊋ null(M^T) when rank-deficient (quadratic-form quirk:
    // v^T·M·M^T·v = parity(M^T·v), can be 0 without M^T·v = 0).
    // block_wiedemann_thin_solve operates in R^n and recovers u = M·w which
    // strictly satisfies M^T·u = (M^T·M)·w = 0 by associativity.

    // For small matrices, delegate to Gaussian (same threshold as BL)
    if (m < 5000 && n < 5000) {
        BlockLanczos bl;
        return bl.find_dependencies(matrix, max_deps);
    }

    // Algorithm selection: env GNFS_BW_ALGORITHM=scalar forces the legacy
    // scalar-BM × 64 path (validation / debug); default uses true block BM
    // (Coppersmith) for ~64× fewer SpMV calls.
    const char* algo_env = std::getenv("GNFS_BW_ALGORITHM");
    const bool use_scalar = (algo_env != nullptr &&
                             std::string(algo_env) == "scalar");

    // Thin matrix detection: prefer B'=M^T·M variant when m<n. For square or
    // wide (m≥n) matrices, the standard path is correct and more efficient
    // (B is m×m, smaller). For thin, only the new variant guarantees correctness
    // over GF(2). Scalar BW path lacks a thin variant (legacy code, low ROI to
    // re-implement). For users explicitly forcing scalar, document via stderr.
    const bool is_thin = (m < n);
    if (is_thin && use_scalar) {
        std::cerr << "  [BW] WARN: GNFS_BW_ALGORITHM=scalar + thin matrix (m<n) "
                     "is unsupported; falling back to block thin variant\n";
    }

    // Retry up to 3 seeds — Phase 1's projections can occasionally be rank-
    // deficient, producing too few valid generators. Different seeds recover.
    //
    // Multi-stream (GNFS_BW_KRYLOV_STREAMS=K>1): for block/thin paths route
    // through find_dependencies_view_impl which runs K parallel streams per
    // base seed. Scalar fallback path retains the legacy sequential loop
    // (multi-stream not implemented for scalar; legacy code, low ROI).
    const uint32_t num_streams = bw_num_streams();
    if (num_streams > 1 && !use_scalar) {
        const_cast<SparseMatrix&>(matrix).ensure_all_sorted();
        CSRMatrix csr(matrix);
        auto deps = find_dependencies_view_impl(csr, max_deps);
        if (!deps.empty()) return deps;
        // Fall through to scalar fallback (block path only); thin path has no
        // scalar variant so empty result returns to caller.
        if (!is_thin) {
            std::cerr << "  [BW] multi-stream block path empty, falling back to scalar\n";
            static constexpr uint64_t fb_seeds[] = {
                42, 0xDEADBEEFCAFEBABEULL, 0x12345678ABCDEFULL,
            };
            for (uint64_t seed : fb_seeds) {
                auto fb_deps = block_wiedemann_scalar_solve(matrix, max_deps, seed);
                if (!fb_deps.empty()) return fb_deps;
            }
        }
        return {};
    }

    static constexpr uint64_t seeds[] = { 42, 0xDEADBEEFCAFEBABEULL, 0x12345678ABCDEFULL };
    for (uint64_t seed : seeds) {
        std::vector<std::vector<bool>> deps;
        if (is_thin) {
            deps = block_wiedemann_thin_solve(matrix, max_deps, seed);
        } else if (use_scalar) {
            deps = block_wiedemann_scalar_solve(matrix, max_deps, seed);
        } else {
            deps = block_wiedemann_block_solve(matrix, max_deps, seed);
        }
        if (!deps.empty()) return deps;
        std::cerr << "  [BW] seed=" << seed
                  << (is_thin ? " (thin)" : (use_scalar ? " (scalar)" : " (block)"))
                  << " produced no deps, retrying\n";
    }

    // If block-BM path failed for all seeds, fall back to scalar (in case
    // the block-BM extraction has issues for this matrix). Thin path has no
    // such fallback (no thin scalar variant); empty result returns to caller.
    if (!use_scalar && !is_thin) {
        std::cerr << "  [BW] block path exhausted seeds, falling back to scalar\n";
        for (uint64_t seed : seeds) {
            auto deps = block_wiedemann_scalar_solve(matrix, max_deps, seed);
            if (!deps.empty()) return deps;
        }
    }
    return {};
}

// ============================================================================
// Streaming Block Wiedemann with Scalar Berlekamp-Massey
//
// Memory: O(m) — only current block vector + accumulators
// Time: O(L × nnz) — L Krylov steps, each requires 2 SpMV
//
// Three phases:
//   Phase 1: Compute projected scalar sequences a_{j,k} = X_j^T * B^k * Y_j
//   Phase 2: Scalar BM for each of 64 sequences → 64 minimal polynomials
//   Phase 3: Recompute Krylov, accumulate solutions w_j = q_j(B) * Y
// ============================================================================

std::vector<std::vector<bool>> BlockWiedemann::block_wiedemann_scalar_solve(
    const SparseMatrix& matrix, size_t max_deps, uint64_t seed) {

    const size_t m = matrix.num_rows();
    const size_t n = matrix.num_cols();

    std::cout << "  [BW-scalar] Streaming Wiedemann: " << m << "×" << n
              << " (seed=" << seed << ")" << std::endl;

    const_cast<SparseMatrix&>(matrix).ensure_all_sorted();
    CSRMatrix csr(matrix);

    // Krylov sequence length for SCALAR Berlekamp-Massey:
    // Need seq_len ≥ 2 * deg(minpoly(B)) where minpoly degree ≤ rank(B) ≤ min(m,n).
    // Scalar path is wide-only — thin matrices route to block_wiedemann_thin_solve.
    const size_t L = n + 50;
    const size_t seq_len = 2 * L + 10;

    gnfs::util::ThreadPool pool(0);

    // Random block vectors X (for projection) and Y (starting vector)
    BlockVector X(m), Y(m);
    {
        std::mt19937_64 rng(seed);
        for (size_t i = 0; i < m; ++i) X.data[i] = rng();
        for (size_t i = 0; i < m; ++i) Y.data[i] = rng();
    }

    // ── Phase 1: Compute projected scalar sequences ──
    // For each j = 0..63: s_{j,k} = X_j^T * B^k * Y_j
    // where X_j = j-th packed column of X, Y_j = j-th packed column of Y.
    // A_k = X^T * V_k is a 64×64 matrix; diagonal entry A_k[j][j] = s_{j,k}.
    //
    // ENV GNFS_BW_KRYLOV_MMAP=1: store sequences in mmap-backed file (BACKLOG
    // #11d). 64 × seq_len bytes is the dominant scalar-path RAM cost (~128 MB
    // for n=1M). Layout: KrylovSequenceMmap with L=64 entries, entry_size=seq_len.

    const char* mmap_env = std::getenv("GNFS_BW_KRYLOV_MMAP");
    const bool use_mmap = (mmap_env != nullptr && mmap_env[0] == '1');
    // Scalar BM path has a transposed access pattern (write k×j → j-major
    // streams). KrylovSequenceCompressed requires sequential write k=0..L-1
    // per entry, so the scalar-BM hot loop cannot stream directly into a
    // compressed file without materialising the full sequences[64][seq_len]
    // in RAM (defeating the point). Future work: support patch-write in
    // KrylovSequenceCompressed. For now, scalar path uses uncompressed mmap.
    if (use_mmap && bw_use_compress()) {
        std::fprintf(stderr,
            "[bw_krylov_compress] WARN: scalar BM path does not support "
            "compression yet; falling back to uncompressed mmap\n");
    }

    std::cout << "  [BW] Phase 1: Krylov projection (L=" << L
              << ", seq_len=" << seq_len << (use_mmap ? ", mmap" : "")
              << ")..." << std::flush;

    std::vector<std::vector<uint8_t>> sequences;
    std::unique_ptr<KrylovSequenceMmap> seq_mmap;
    if (use_mmap) {
        char path_buf[128];
        std::snprintf(path_buf, sizeof(path_buf),
                      "/tmp/gnfs_bw_krylov_scalar_%d_%llu.kry",
                      gnfs::util::process_id(),
                      static_cast<unsigned long long>(seed));
        seq_mmap = std::make_unique<KrylovSequenceMmap>(
            path_buf, /*L=*/64, /*entry_size=*/seq_len);
    } else {
        sequences.assign(64, std::vector<uint8_t>(seq_len, 0));
    }

    BlockVector V(m), Vnext(m), tmp(n);
    // V_0 = Y
    for (size_t i = 0; i < m; ++i) V.data[i] = Y.data[i];

    // NOTE: this is 64 independent SCALAR Wiedemann sequences, one per column
    // pair (X_j, Y_j) — not a true block algorithm. sequences[j][k] depends only
    // on V_j (the j-th packed column of V_k), so Phase 3's bit-j accumulation
    // pairs correctly with each minpoly. A true block BW would need its own
    // matrix BM (Coppersmith / Thomé lingen). Retried with multiple seeds in
    // find_dependencies() if a given seed yields too many trivial sequences.
    for (size_t k = 0; k < seq_len; ++k) {
        for (size_t j = 0; j < 64; ++j) {
            uint64_t mask = 1ULL << j;
            uint64_t parity = 0;
            for (size_t i = 0; i < m; ++i)
                parity ^= (X.data[i] & V.data[i] & mask);
            const uint8_t bit = static_cast<uint8_t>((parity >> j) & 1);
            if (use_mmap) {
                seq_mmap->raw_at(static_cast<uint64_t>(j))[k] = bit;
            } else {
                sequences[j][k] = bit;
            }
        }

        // V_{k+1} = B * V_k = M * (M^T * V_k)
        if (k + 1 < seq_len) {
            bw_spmv_B(csr, V, Vnext, tmp, pool);
            std::swap(V.data, Vnext.data);
        }
    }
    std::cout << " done" << std::endl;

    if (use_mmap) {
        seq_mmap->msync();
        // Copy mmap → vector<vector> once at BM entry. BM signature takes
        // const std::vector<uint8_t>&. After copy, mmap can be released.
        sequences.assign(64, std::vector<uint8_t>(seq_len, 0));
        for (uint64_t j = 0; j < 64; ++j) {
            std::memcpy(sequences[j].data(), seq_mmap->raw_at(j), seq_len);
        }
        seq_mmap->remove_file();
        seq_mmap.reset();
    }

    // ── Phase 2: Scalar BM for each of 64 sequences ──
    std::cout << "  [BW] Phase 2: Berlekamp-Massey..." << std::flush;

    std::vector<LFSRPolynomial> polys(64);
    size_t valid_polys = 0;
    size_t max_degree = 0;

    for (size_t j = 0; j < 64; ++j) {
        polys[j] = scalar_berlekamp_massey(sequences[j]);
        if (polys[j].degree > 0) {
            // The connection polynomial C(x) = 1 + c_1*x + ... + c_L*x^L
            // has coeffs[0] = 1 always.
            // The characteristic polynomial p(t) = t^L + c_1*t^{L-1} + ... + c_L
            // is divisible by t iff c_L = 0 (the TRAILING coefficient of C).
            // When p(t) = t * q(t), w = q(B) * v is a null vector of B.
            size_t L = polys[j].degree;
            bool div_by_t = (L < polys[j].coeffs.size()) ?
                            (polys[j].coeffs[L] == 0) : true;
            if (div_by_t) {
                valid_polys++;
                max_degree = std::max(max_degree, polys[j].degree);
            }
        }
    }

    // Diagnostic: check if sequences are trivial
    size_t trivial_seqs = 0;
    for (size_t j = 0; j < 64; ++j) {
        bool has_nonzero = false;
        for (size_t k = 0; k < seq_len; ++k) {
            if (sequences[j][k]) { has_nonzero = true; break; }
        }
        if (!has_nonzero) trivial_seqs++;
    }
    if (trivial_seqs > 0) {
        std::cout << " [WARN: " << trivial_seqs << "/64 trivial sequences]";
    }

    // Also print BM degree distribution
    size_t div_by_t = 0, not_div = 0;
    for (size_t j = 0; j < 64; ++j) {
        if (polys[j].degree > 0) {
            size_t L = polys[j].degree;
            bool is_div = (L < polys[j].coeffs.size()) ?
                          (polys[j].coeffs[L] == 0) : true;
            if (is_div) div_by_t++; else not_div++;
        }
    }

    std::cout << " " << valid_polys << " valid (div_by_t=" << div_by_t
              << " not_div=" << not_div << " trivial=" << trivial_seqs
              << " max_deg=" << max_degree << ")" << std::endl;

    if (valid_polys == 0) {
        std::cerr << "  [BW] No valid polynomials found" << std::endl;
        return {};
    }

    // ── Phase 3: Recompute Krylov, accumulate solutions ──
    // For each valid polynomial j with p_j(t) = c_0 + c_1*t + ... + c_d*t^d:
    //   Since c_0 = 0, define q_j(t) = c_1 + c_2*t + ... + c_d*t^{d-1}
    //   Then w_j = q_j(B) * Y_j = Σ_{k=0}^{d-1} c_{k+1} * (B^k * Y)_j
    //   w_j should satisfy B * w_j = 0, hence M^T * w_j = 0.

    std::cout << "  [BW] Phase 3: Solution extraction (max_degree=" << max_degree
              << ")..." << std::flush;

    // Accumulators: 64 solution vectors (m bools each, packed as vector<bool>)
    // We only accumulate bit j from V_k into solution j.
    std::vector<std::vector<bool>> solutions(64, std::vector<bool>(m, false));

    // Recompute V_k = B^k * Y from scratch
    for (size_t i = 0; i < m; ++i) V.data[i] = Y.data[i];

    for (size_t k = 0; k <= max_degree; ++k) {
        // For each valid polynomial j with connection poly C(x) = 1 + c_1*x + ... + c_L*x^L:
        // Characteristic poly: p(t) = t^L + c_1*t^{L-1} + ... + c_{L-1}*t + c_L
        // Since c_L = 0 (divisible by t): p(t) = t * q(t) where
        //   q(t) = t^{L-1} + c_1*t^{L-2} + ... + c_{L-1}
        // Null vector: w = q(B)*Y_j = B^{L-1}*Y_j + c_1*B^{L-2}*Y_j + ... + c_{L-1}*Y_j
        // At Krylov step k: V_k = B^k * Y. We want coefficient of B^k in q(B).
        // q(t) = sum_{i=0}^{L-1} c_i * t^{L-1-i} where c_0 = 1 (from p(t)).
        // So the coefficient of t^k in q(t) is c_{L-1-k} from the connection polynomial.
        for (size_t j = 0; j < 64; ++j) {
            size_t Lj = polys[j].degree;
            if (Lj == 0) continue;
            bool is_div = (Lj < polys[j].coeffs.size()) ?
                          (polys[j].coeffs[Lj] == 0) : true;
            if (!is_div) continue;

            // Coefficient of t^k in q(t) = c_{L-1-k} from connection polynomial
            if (k >= Lj) continue;  // k can be 0..L-1
            size_t conn_idx = Lj - 1 - k;
            if (conn_idx >= polys[j].coeffs.size()) continue;
            if (polys[j].coeffs[conn_idx] == 0) continue;

            // Accumulate bit j of V_k into solution j
            uint64_t mask = 1ULL << j;
            for (size_t i = 0; i < m; ++i) {
                if (V.data[i] & mask)
                    solutions[j][i] = !solutions[j][i];
            }
        }

        // V_{k+1} = B * V_k
        if (k < max_degree) {
            bw_spmv_B(csr, V, Vnext, tmp, pool);
            std::swap(V.data, Vnext.data);
        }
    }
    std::cout << " done" << std::endl;

    // ── Verify solutions ──
    std::vector<std::vector<bool>> deps;
    deps.reserve(std::min(max_deps, static_cast<size_t>(64)));
    size_t verified = 0, failed = 0, zero_vecs = 0;

    for (size_t j = 0; j < 64 && deps.size() < max_deps; ++j) {
        if (polys[j].degree == 0) continue;
        size_t Lj = polys[j].degree;
        bool is_div = (Lj < polys[j].coeffs.size()) ?
                      (polys[j].coeffs[Lj] == 0) : true;
        if (!is_div) continue;

        const auto& sol = solutions[j];

        // Check non-zero
        bool nonzero = false;
        for (size_t i = 0; i < m; ++i) {
            if (sol[i]) { nonzero = true; break; }
        }
        if (!nonzero) { zero_vecs++; continue; }

        // Verify: M^T * sol = 0
        std::vector<uint8_t> check(n, 0);
        for (size_t i = 0; i < m; ++i) {
            if (!sol[i]) continue;
            for (const uint32_t* p = csr.row_begin(i); p != csr.row_end(i); ++p)
                check[*p] ^= 1;
        }

        bool valid = true;
        for (size_t c = 0; c < n; ++c) {
            if (check[c]) { valid = false; break; }
        }

        if (valid) {
            deps.push_back(sol);
            verified++;
        } else {
            failed++;
        }
    }

    std::cout << "  [BW-scalar] Results: " << deps.size() << " valid deps"
              << " (verified=" << verified << " failed=" << failed
              << " zero=" << zero_vecs << ")" << std::endl;

    return deps;
}

// ============================================================================
// Block Wiedemann with Coppersmith Matrix BM
//
// Phase 1: Collect L = 2·⌈n/64⌉ + 32 matrices A_k = X^T · V_k (~64× fewer
//          SpMV than scalar path's 2n+110).
// Phase 2: matrix_berlekamp_massey → F(z), 64-column generator polynomial.
// Phase 3: Block mksol: W = sum_k V_k · F_k. Each column of W is a candidate
//          null vector; verify M^T · w_j = 0.
// ============================================================================

// Templated body for the wide/square Block Wiedemann solver. Takes any
// MatrixView (CSRMatrix for in-memory, MmapCSRMatrix for OOC). Caller is
// responsible for ensuring rows are sorted and CSR-style layout exists.
//
// pool_threads=0 → ThreadPool default (hardware_concurrency). Multi-stream
// dispatchers pass a small per-stream pool size so K concurrent streams share
// the physical cores cleanly. stream_tag is appended to log lines + mmap path
// to disambiguate concurrent stream output.
template <MatrixView MV>
static std::vector<std::vector<bool>> block_solve_view_impl(
    const MV& csr, size_t max_deps, uint64_t seed,
    uint32_t pool_threads = 0, uint32_t stream_tag = 0) {

    const size_t m = csr.num_rows();
    const size_t n = csr.num_cols();

    std::cout << "  [BW-block";
    if (stream_tag) std::cout << " s" << stream_tag;
    std::cout << "] Block Wiedemann (matrix BM): " << m << "×" << n
              << " (seed=" << seed << ")" << std::endl;

    // Krylov sequence length for matrix BM: L = 2·⌈n/64⌉ + 32 (buffer).
    // Compared to scalar BM's 2n+110, this is ~64× fewer SpMV calls.
    // Block path handles square/wide (m≥n); thin (m<n) routes elsewhere.
    const size_t L = 2 * ((n + 63) / 64) + 32;

    gnfs::util::ThreadPool pool(pool_threads);

    BlockVector X(m), Y(m);
    {
        std::mt19937_64 rng(seed);
        for (size_t i = 0; i < m; ++i) X.data[i] = rng();
        for (size_t i = 0; i < m; ++i) Y.data[i] = rng();
    }

    // ── Phase 1: Krylov sequence A_k = X^T · V_k ──
    // ENV GNFS_BW_KRYLOV_MMAP=1 opt-in: store A_seq on disk via
    // KrylovSequenceMmap (BACKLOG #11d, releases ~16 MB physical RAM for n=1M).
    auto phase_start = std::chrono::steady_clock::now();
    const char* mmap_env = std::getenv("GNFS_BW_KRYLOV_MMAP");
    const bool use_mmap = (mmap_env != nullptr && mmap_env[0] == '1');
    const bool use_compress = use_mmap && bw_use_compress();
    std::cout << "  [BW-block] Phase 1: Krylov (L=" << L
              << (use_mmap ? (use_compress ? ", mmap+zip" : ", mmap") : "")
              << ")..." << std::flush;

    std::vector<DenseGF2_64x64> A_seq;
    std::unique_ptr<KrylovSequenceMmap> A_mmap;
    std::unique_ptr<KrylovSequenceCompressed> A_kryz;
    if (use_compress) {
        char path_buf[160];
        std::snprintf(path_buf, sizeof(path_buf),
                      "/tmp/gnfs_bw_krylov_%d_s%u_%llu.kryz",
                      gnfs::util::process_id(),
                      static_cast<unsigned>(stream_tag),
                      static_cast<unsigned long long>(seed));
        A_kryz = std::make_unique<KrylovSequenceCompressed>(
            path_buf, L, sizeof(DenseGF2_64x64));
    } else if (use_mmap) {
        char path_buf[160];
        std::snprintf(path_buf, sizeof(path_buf),
                      "/tmp/gnfs_bw_krylov_%d_s%u_%llu.kry",
                      gnfs::util::process_id(),
                      static_cast<unsigned>(stream_tag),
                      static_cast<unsigned long long>(seed));
        A_mmap = std::make_unique<KrylovSequenceMmap>(
            path_buf, L, sizeof(DenseGF2_64x64));
    } else {
        A_seq.resize(L);
    }

    BlockVector V(m), Vnext(m), tmp(n);
    for (size_t i = 0; i < m; ++i) V.data[i] = Y.data[i];

    for (size_t k = 0; k < L; ++k) {
        DenseGF2_64x64 a = inner_product_64x64(X, V);
        if (use_compress) {
            *A_kryz->write_at_typed<DenseGF2_64x64>(k) = a;
        } else if (use_mmap) {
            *A_mmap->at<DenseGF2_64x64>(k) = a;
        } else {
            A_seq[k] = a;
        }
        if (k + 1 < L) {
            bw_spmv_B(csr, V, Vnext, tmp, pool);
            std::swap(V.data, Vnext.data);
        }
    }
    double phase1_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - phase_start).count();
    std::cout << " done (" << phase1_ms << " ms)" << std::endl;

    if (use_compress) {
        A_kryz->close();
        // Status line: stderr ratio for OOC observability
        const uint64_t orig = A_kryz->total_uncompressed_bytes();
        const uint64_t comp = A_kryz->total_compressed_bytes();
        const double ratio = (orig > 0) ?
            static_cast<double>(comp) / static_cast<double>(orig) : 0.0;
        std::fprintf(stderr,
            "[bw_krylov_compress] orig=%.2f MB compressed=%.2f MB ratio=%.4f\n",
            static_cast<double>(orig) / (1024.0 * 1024.0),
            static_cast<double>(comp) / (1024.0 * 1024.0),
            ratio);
        // Reopen for read in Phase 2 BM copy step
        const std::string path = A_kryz->path();
        A_kryz.reset();
        auto reader = KrylovSequenceCompressed::open_readonly(path);
        A_seq.resize(L);
        for (size_t k = 0; k < L; ++k) {
            const auto* p = reader.read_at_typed<DenseGF2_64x64>(k);
            A_seq[k] = *p;
        }
        reader.close();
        ::unlink(path.c_str());
    } else if (use_mmap) {
        A_mmap->msync();
        // Copy mmap → vector once at BM entry (BM signature requires contiguous
        // vector). After copy, mmap can be released — Phase 3 doesn't need A_seq.
        A_seq.resize(L);
        for (size_t k = 0; k < L; ++k) {
            A_seq[k] = *A_mmap->at<DenseGF2_64x64>(k);
        }
        A_mmap->remove_file();
        A_mmap.reset();
    }

    // ── Phase 2: Matrix Berlekamp-Massey ──
    phase_start = std::chrono::steady_clock::now();
    std::cout << "  [BW-block] Phase 2: matrix BM..." << std::flush;
    auto F = BlockWiedemann::matrix_berlekamp_massey(A_seq, n);
    const int valid_count = gnfs::util::popcount64(F.valid_mask);
    const int max_deg = static_cast<int>(F.poly.size()) - 1;
    double phase2_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - phase_start).count();
    std::cout << " " << valid_count << " valid cols, max_deg=" << max_deg
              << " (" << phase2_ms << " ms)" << std::endl;

    if (F.valid_mask == 0 || max_deg < 0) {
        std::cerr << "  [BW-block] No valid generator — falling through" << std::endl;
        return {};
    }

    // ── Phase 3: Block mksol ──
    // Analog of scalar Wiedemann extraction. Scalar uses w = q(B)·y where
    // q(z) = z^{-1} · (reverse of connection poly C). So at Krylov step k,
    // multiply V_k by c_{L-1-k} (reversed coefficient).
    //
    // Block analog: w_j = sum_k V_k · F_{D_j - k}[*, j], i.e., per column j,
    // use F's coefficient at degree (D_j - k) at Krylov step k. Combine all
    // columns into one m×64 accumulator block.
    phase_start = std::chrono::steady_clock::now();
    std::cout << "  [BW-block] Phase 3: block mksol (max_deg=" << max_deg
              << ")..." << std::flush;

    for (size_t i = 0; i < m; ++i) V.data[i] = Y.data[i];

    BlockVector accumulator(m);
    for (size_t i = 0; i < m; ++i) accumulator.data[i] = 0;

    // At step k, build F_step: column j = F.poly[F.degrees[j] - k][:, j] (or 0
    // if k > degrees[j]). Then accumulator += V_k · F_step.
    for (int k = 0; k <= max_deg; ++k) {
        DenseGF2_64x64 F_step;
        F_step.clear();
        bool any_active = false;
        for (size_t j = 0; j < 64; ++j) {
            if (!((F.valid_mask >> j) & 1ULL)) continue;
            const int D_j = F.degrees[j];
            const int coef_idx = D_j - k;
            if (coef_idx < 0) continue;  // exhausted column j's polynomial
            if (coef_idx >= static_cast<int>(F.poly.size())) continue;
            // Extract column j of F.poly[coef_idx] into column j of F_step
            const DenseGF2_64x64& src = F.poly[static_cast<size_t>(coef_idx)];
            for (int i = 0; i < 64; ++i) {
                if ((src.rows[i] >> j) & 1ULL) {
                    F_step.rows[i] |= (1ULL << j);
                    any_active = true;
                }
            }
        }
        if (any_active) {
            mksol_accumulate(V, F_step, accumulator);
        }
        if (k < max_deg) {
            bw_spmv_B(csr, V, Vnext, tmp, pool);
            std::swap(V.data, Vnext.data);
        }
    }
    double phase3_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - phase_start).count();
    std::cout << " done (" << phase3_ms << " ms)" << std::endl;

    // ── Verify each candidate column of accumulator ──
    std::vector<std::vector<bool>> deps;
    deps.reserve(std::min(max_deps, static_cast<size_t>(64)));
    size_t verified = 0, failed = 0, zero_vecs = 0;

    for (int j = 0; j < 64 && deps.size() < max_deps; ++j) {
        if (!((F.valid_mask >> j) & 1ULL)) continue;

        const uint64_t mask = 1ULL << j;
        std::vector<bool> sol(m, false);
        bool nonzero = false;
        for (size_t i = 0; i < m; ++i) {
            if (accumulator.data[i] & mask) {
                sol[i] = true;
                nonzero = true;
            }
        }
        if (!nonzero) { zero_vecs++; continue; }

        // Verify M^T · sol = 0
        std::vector<uint8_t> check(n, 0);
        for (size_t i = 0; i < m; ++i) {
            if (!sol[i]) continue;
            for (const uint32_t* p = csr.row_begin(i); p != csr.row_end(i); ++p)
                check[*p] ^= 1;
        }

        bool valid = true;
        for (size_t c = 0; c < n; ++c) {
            if (check[c]) { valid = false; break; }
        }

        if (valid) {
            deps.push_back(std::move(sol));
            verified++;
        } else {
            failed++;
        }
    }

    std::cout << "  [BW-block] Results: " << deps.size() << " valid deps"
              << " (verified=" << verified << " failed=" << failed
              << " zero=" << zero_vecs << ")" << std::endl;

    return deps;
}

// ----------------------------------------------------------------------------
// SparseMatrix entry point (today's in-memory default path). Sorts rows,
// builds CSRMatrix, then calls the templated impl above. The MmapCSRMatrix
// entry point lives in BlockWiedemann::find_dependencies_view (see header).
// ----------------------------------------------------------------------------
std::vector<std::vector<bool>> BlockWiedemann::block_wiedemann_block_solve(
    const SparseMatrix& matrix, size_t max_deps, uint64_t seed) {
    const_cast<SparseMatrix&>(matrix).ensure_all_sorted();
    CSRMatrix csr(matrix);
    return block_solve_view_impl(csr, max_deps, seed);
}

// ============================================================================
// Block Wiedemann thin matrix variant — operator B' = M^T·M (BACKLOG #80)
//
// For thin matrices (m < n), the standard BW path with B = M·M^T fails over
// GF(2): null(B) ⊋ null(M^T) due to the quadratic-form quirk
// v^T·M·M^T·v = parity(M^T·v) (can be 0 without M^T·v = 0).
//
// Mirror image: work in R^n with B' = M^T·M. BW phase 3 gives w ∈ R^n
// strictly satisfying B'·w = M^T·(M·w) = 0. Set u = M·w ∈ R^m; then
// M^T·u = (M^T·M)·w = 0 by associativity, so u ∈ left null(M) (assuming
// u ≠ 0; the only degenerate case is w ∈ null(M) → u = 0, which we discard).
//
// L = 2·⌈m/64⌉ + 32 since rank(B') ≤ rank(M) ≤ m, so minpoly degree ≤ m.
// ============================================================================

// Templated body for the thin (m < n) Block Wiedemann solver. Same MV
// contract as block_solve_view_impl — caller must hand in a CSR-style
// matrix view with sorted rows. pool_threads + stream_tag mirror the block
// variant for multi-stream Krylov parallelisation.
template <MatrixView MV>
static std::vector<std::vector<bool>> thin_solve_view_impl(
    const MV& csr, size_t max_deps, uint64_t seed,
    uint32_t pool_threads = 0, uint32_t stream_tag = 0) {

    const size_t m = csr.num_rows();
    const size_t n = csr.num_cols();

    std::cout << "  [BW-thin";
    if (stream_tag) std::cout << " s" << stream_tag;
    std::cout << "] Thin matrix BW (B'=M^T·M): " << m << "×" << n
              << " (seed=" << seed << ")" << std::endl;

    // For B' = M^T·M (n×n), rank ≤ m, so minpoly degree ≤ m.
    // Block Krylov length: L = 2·⌈m/64⌉ + 32.
    const size_t L = 2 * ((m + 63) / 64) + 32;

    gnfs::util::ThreadPool pool(pool_threads);

    // X, Y random vectors in R^n (length n, not m as in the standard path).
    BlockVector X(n), Y(n);
    {
        std::mt19937_64 rng(seed);
        for (size_t i = 0; i < n; ++i) X.data[i] = rng();
        for (size_t i = 0; i < n; ++i) Y.data[i] = rng();
    }

    // ── Phase 1: Krylov sequence A_k = X^T · V_k where V_k = (B')^k · Y ──
    auto phase_start = std::chrono::steady_clock::now();
    std::cout << "  [BW-thin] Phase 1: Krylov (L=" << L << ")..." << std::flush;
    std::vector<DenseGF2_64x64> A_seq(L);

    // V, Vnext live in R^n; tmp lives in R^m for B'·V = M^T·(M·V) intermediate.
    BlockVector V(n), Vnext(n), tmp(m);
    for (size_t i = 0; i < n; ++i) V.data[i] = Y.data[i];

    for (size_t k = 0; k < L; ++k) {
        A_seq[k] = inner_product_64x64(X, V);
        if (k + 1 < L) {
            bw_spmv_B_prime(csr, V, Vnext, tmp, pool);
            std::swap(V.data, Vnext.data);
        }
    }
    double phase1_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - phase_start).count();
    std::cout << " done (" << phase1_ms << " ms)" << std::endl;

    // ── Phase 2: Matrix Berlekamp-Massey ──
    // BM operates only on the sequence, not on the original matrix; reused as-is.
    // Pass m as the "size" (rank bound) instead of n.
    phase_start = std::chrono::steady_clock::now();
    std::cout << "  [BW-thin] Phase 2: matrix BM..." << std::flush;
    auto F = BlockWiedemann::matrix_berlekamp_massey(A_seq, m);
    const int valid_count = gnfs::util::popcount64(F.valid_mask);
    const int max_deg = static_cast<int>(F.poly.size()) - 1;
    // BACKLOG #1 rank lower-bound: see compute_rank_est doc in block_wiedemann.hpp.
    const int rank_est = compute_rank_est(F);
    double phase2_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - phase_start).count();
    std::cout << " " << valid_count << " valid cols, max_deg=" << max_deg
              << ", rank_est=" << rank_est << "/" << m
              << " (" << phase2_ms << " ms)" << std::endl;
    // stderr fallback for stress/progressive
    std::fprintf(stderr,
        "[bw-thin] valid=%d/64 max_deg=%d rank_est=%d/%zu (m=%zu n=%zu)\n",
        valid_count, max_deg, rank_est, m, m, n);

    if (F.valid_mask == 0 || max_deg < 0) {
        std::cerr << "  [BW-thin] No valid generator — falling through" << std::endl;
        return {};
    }

    // ── Phase 3: Block mksol — accumulator w_j = sum_k V_k · F_k[*,j] in R^n ──
    phase_start = std::chrono::steady_clock::now();
    std::cout << "  [BW-thin] Phase 3: block mksol (max_deg=" << max_deg
              << ")..." << std::flush;

    for (size_t i = 0; i < n; ++i) V.data[i] = Y.data[i];

    BlockVector accumulator(n);
    for (size_t i = 0; i < n; ++i) accumulator.data[i] = 0;

    for (int k = 0; k <= max_deg; ++k) {
        DenseGF2_64x64 F_step;
        F_step.clear();
        bool any_active = false;
        for (size_t j = 0; j < 64; ++j) {
            if (!((F.valid_mask >> j) & 1ULL)) continue;
            const int D_j = F.degrees[j];
            const int coef_idx = D_j - k;
            if (coef_idx < 0) continue;
            if (coef_idx >= static_cast<int>(F.poly.size())) continue;
            const DenseGF2_64x64& src = F.poly[static_cast<size_t>(coef_idx)];
            for (int i = 0; i < 64; ++i) {
                if ((src.rows[i] >> j) & 1ULL) {
                    F_step.rows[i] |= (1ULL << j);
                    any_active = true;
                }
            }
        }
        if (any_active) {
            mksol_accumulate(V, F_step, accumulator);
        }
        if (k < max_deg) {
            bw_spmv_B_prime(csr, V, Vnext, tmp, pool);
            std::swap(V.data, Vnext.data);
        }
    }
    double phase3_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - phase_start).count();
    std::cout << " done (" << phase3_ms << " ms)" << std::endl;

    // ── Phase 4 (recovery): u_j = M·w_j ∈ R^m for each valid column ──
    // Then verify u_j ≠ 0 AND M^T·u_j = 0 (the latter holds by construction
    // by associativity over GF(2); guard the degenerate w_j ∈ null(M) case).
    BlockVector U(m);
    bw_spmv_forward(csr, accumulator, U, pool);

    std::vector<std::vector<bool>> deps;
    deps.reserve(std::min(max_deps, static_cast<size_t>(64)));
    size_t verified = 0, failed = 0, zero_vecs = 0;

    for (int j = 0; j < 64 && deps.size() < max_deps; ++j) {
        if (!((F.valid_mask >> j) & 1ULL)) continue;

        const uint64_t mask = 1ULL << j;
        std::vector<bool> sol(m, false);
        bool nonzero = false;
        for (size_t i = 0; i < m; ++i) {
            if (U.data[i] & mask) {
                sol[i] = true;
                nonzero = true;
            }
        }
        if (!nonzero) { zero_vecs++; continue; }

        // Verify M^T·sol = 0
        std::vector<uint8_t> check(n, 0);
        for (size_t i = 0; i < m; ++i) {
            if (!sol[i]) continue;
            for (const uint32_t* p = csr.row_begin(i); p != csr.row_end(i); ++p)
                check[*p] ^= 1;
        }

        bool valid = true;
        for (size_t c = 0; c < n; ++c) {
            if (check[c]) { valid = false; break; }
        }

        if (valid) {
            deps.push_back(std::move(sol));
            verified++;
        } else {
            failed++;
        }
    }

    std::cout << "  [BW-thin] Results: " << deps.size() << " valid deps"
              << " (verified=" << verified << " failed=" << failed
              << " zero=" << zero_vecs << ")" << std::endl;

    return deps;
}

// ----------------------------------------------------------------------------
// SparseMatrix entry point for the thin solver. Sorts rows, builds CSRMatrix,
// then calls the templated impl above.
// ----------------------------------------------------------------------------
std::vector<std::vector<bool>> BlockWiedemann::block_wiedemann_thin_solve(
    const SparseMatrix& matrix, size_t max_deps, uint64_t seed) {
    const_cast<SparseMatrix&>(matrix).ensure_all_sorted();
    CSRMatrix csr(matrix);
    return thin_solve_view_impl(csr, max_deps, seed);
}

// ============================================================================
// View-based dispatch (Phase 5: in-memory CSR + out-of-core MmapCSR)
// ============================================================================
//
// Routes the matrix view to block / thin BW based on m vs n, with the same
// multi-seed retry policy as find_dependencies(SparseMatrix). Scalar BW
// fallback is intentionally omitted: it is wide-only and the view path is
// only reached from Pipeline::solve_matrix's Phase 5 (large matrices that
// already chose block / thin BW).
//
// Multi-stream parallelism (GNFS_BW_KRYLOV_STREAMS=K, default 1): launches K
// worker threads concurrently, each running one stream with a distinct seed
// over a small dedicated ThreadPool (hw_concurrency / K each). The first
// non-empty result determines success; all completed streams' deps are merged
// and deduped (content-equal vectors removed) before trimming to max_deps.
template <MatrixView MV>
static std::vector<std::vector<bool>> find_dependencies_view_impl(
    const MV& matrix, size_t max_deps) {

    const size_t m = matrix.num_rows();
    const size_t n = matrix.num_cols();
    if (m == 0 || n == 0) return {};

    const bool is_thin = (m < n);
    const uint32_t num_streams = bw_num_streams();
    const uint32_t pool_size = bw_pool_size_per_stream(num_streams);

    static constexpr uint64_t base_seeds[] = {
        42, 0xDEADBEEFCAFEBABEULL, 0x12345678ABCDEFULL,
    };

    // Single-stream fast path (default K=1): preserve original behaviour bit-
    // for-bit including stderr retry diagnostics.
    if (num_streams == 1) {
        for (uint64_t seed : base_seeds) {
            std::vector<std::vector<bool>> deps;
            if (is_thin) {
                deps = thin_solve_view_impl(matrix, max_deps, seed);
            } else {
                deps = block_solve_view_impl(matrix, max_deps, seed);
            }
            if (!deps.empty()) return deps;
            std::cerr << "  [BW-view] seed=" << seed
                      << (is_thin ? " (thin)" : " (block)")
                      << " produced no deps, retrying\n";
        }
        return {};
    }

    // Multi-stream: each base_seed round dispatches num_streams workers in
    // parallel. Return as soon as one round produces ≥1 dep, merging all
    // successful streams' deps.
    std::cerr << "  [BW-view] multi-stream K=" << num_streams
              << " pool/stream=" << pool_size
              << (is_thin ? " (thin)" : " (block)") << "\n";

    for (uint64_t base_seed : base_seeds) {
        std::vector<std::vector<std::vector<bool>>> per_stream(num_streams);
        std::vector<std::thread> workers;
        workers.reserve(num_streams);

        for (uint32_t s = 0; s < num_streams; ++s) {
            const uint64_t seed = bw_stream_seed(base_seed, s);
            workers.emplace_back([&, s, seed]() {
                if (is_thin) {
                    per_stream[s] = thin_solve_view_impl(
                        matrix, max_deps, seed, pool_size, s + 1);
                } else {
                    per_stream[s] = block_solve_view_impl(
                        matrix, max_deps, seed, pool_size, s + 1);
                }
            });
        }
        for (auto& t : workers) t.join();

        // Merge + dedupe across streams.
        std::vector<std::vector<bool>> merged;
        std::unordered_set<std::string> seen;
        for (auto& sol_set : per_stream) {
            for (auto& sol : sol_set) {
                std::string key(sol.size(), '0');
                for (size_t i = 0; i < sol.size(); ++i) key[i] = sol[i] ? '1' : '0';
                if (seen.insert(std::move(key)).second) {
                    merged.push_back(std::move(sol));
                    if (merged.size() >= max_deps) break;
                }
            }
            if (merged.size() >= max_deps) break;
        }

        if (!merged.empty()) return merged;
        std::cerr << "  [BW-view] base_seed=" << base_seed
                  << " all " << num_streams
                  << " streams empty, retrying next base_seed\n";
    }
    return {};
}

std::vector<std::vector<bool>> BlockWiedemann::find_dependencies_view(
    const CSRMatrix& matrix, size_t max_deps) {
    return find_dependencies_view_impl(matrix, max_deps);
}

std::vector<std::vector<bool>> BlockWiedemann::find_dependencies_view(
    const MmapCSRMatrix& matrix, size_t max_deps) {
    return find_dependencies_view_impl(matrix, max_deps);
}

// ============================================================================
// Reserved stubs (old interface, unused in streaming BW)
// ============================================================================

// These stubs throw in both Debug and Release. The old code used assert(false)
// + a default return, which under NDEBUG silently handed back an empty result
// and the caller proceeded with a wrong zero-vector dependency. throw makes
// any future caller fail loudly with a stack trace.
std::vector<DenseGF2_64x64> BlockWiedemann::compute_krylov_sequence(
    const CSRMatrix&, size_t, size_t, const BlockVector&, BlockVector&) {
    throw std::logic_error("BlockWiedemann::compute_krylov_sequence: reserved for true block-BW; "
                           "streaming BW uses find_dependencies directly");
}

// ============================================================================
// Coppersmith Block Berlekamp-Massey (lingen base case)
// ============================================================================
// Algorithm: column-extended Coppersmith with quadratic basecase, following
// CADO-NFS lingen_qcode_binary.cpp.
//
// Input: sequence A_0, ..., A_{L-1} ∈ GF(2)^{64×64}. L ≤ 64 in this initial
// implementation (single uint64_t per polynomial entry — bit e = degree e
// coefficient).
//
// Construct input matpoly E ∈ GF(2)[z]^{64×128}:
//   E[i, j]  for j ∈ [0, 64): the (i,j) entry of the sequence A (bit e = A_e[i,j])
//   E[i, j+64]: identity at z=0 (E[i, i+64] = 1 at z=0, all others zero)
//
// Initialize P ∈ GF(2)[z]^{128×128} = I_{128} at z=0 (diagonal ones).
// Per-column delta[j] = 0 for all j.
//
// For each step e = 0..L-1:
//   For each row i = 0..63:
//     Find pivot j_p ∈ [0, 128) with min delta[j_p] such that bit e of E[i, j_p] = 1.
//     For all other cols k where bit e of E[i, k] = 1:
//       E[:, k] ^= E[:, j_p]   (column XOR over all 64 rows of E)
//       P[:, k] ^= P[:, j_p]   (column XOR over all 128 rows of P)
//     E[:, j_p] <<= 1   (shift pivot column up — "delay" / consume bit e)
//     P[:, j_p] <<= 1
//     delta[j_p]++
//
// Output: F ∈ GF(2)[z]^{64×64} extracted from top 64 rows of the 64 columns of
// P with smallest delta (these are the "good" generator columns).
//
// Reference: CADO-NFS source linalg/bwc/lingen_qcode_binary.cpp,
// function lingen_qcode_do_tmpl (read 2026-05-14).
LingenResult BlockWiedemann::matrix_berlekamp_massey(
    const std::vector<DenseGF2_64x64>& A, size_t /*N*/) {
    constexpr int m = 64;
    constexpr int n = 64;
    constexpr int b = m + n;  // 128
    const int L = static_cast<int>(A.size());

    if (L == 0) return LingenResult{};

    // W = words per polynomial. Polynomial degree can grow to L (max shifts
    // per col over L steps), plus buffer for the extraction phase.
    const int W = (L + 10 + 63) / 64;

    // E ∈ GF(2)[z]^{m × b}: flat layout E[(i*b + j)*W + w].
    // P ∈ GF(2)[z]^{b × b}: flat layout P[(i*b + j)*W + w].
    std::vector<uint64_t> E(static_cast<size_t>(m) * static_cast<size_t>(b) * static_cast<size_t>(W), 0);
    std::vector<uint64_t> P(static_cast<size_t>(b) * static_cast<size_t>(b) * static_cast<size_t>(W), 0);

    auto E_at = [&E, W](int i, int j) -> uint64_t* {
        return &E[(static_cast<size_t>(i) * static_cast<size_t>(b) +
                   static_cast<size_t>(j)) * static_cast<size_t>(W)];
    };
    auto P_at = [&P, W](int i, int j) -> uint64_t* {
        return &P[(static_cast<size_t>(i) * static_cast<size_t>(b) +
                   static_cast<size_t>(j)) * static_cast<size_t>(W)];
    };

    // ── Build E: left 64 cols = A sequence, right 64 cols = I_{64} at z=0 ──
    for (int e = 0; e < L; ++e) {
        const DenseGF2_64x64& Ae = A[static_cast<size_t>(e)];
        const int e_w = e / 64, e_b = e % 64;
        const uint64_t e_mask = 1ULL << e_b;
        for (int i = 0; i < m; ++i) {
            uint64_t row_bits = Ae.rows[i];
            while (row_bits) {
                int j = gnfs::util::ctz64(row_bits);
                E_at(i, j)[e_w] |= e_mask;
                row_bits &= row_bits - 1;
            }
        }
    }
    for (int i = 0; i < m; ++i) {
        E_at(i, n + i)[0] |= 1ULL;
    }

    // ── Init P = I_{b} at z=0 ──
    for (int i = 0; i < b; ++i) {
        P_at(i, i)[0] |= 1ULL;
    }

    std::array<int, b> delta{};
    delta.fill(0);

    // Poly helpers (W-word polynomials).
    auto poly_xor = [W](uint64_t* dst, const uint64_t* src) noexcept {
        for (int w = 0; w < W; ++w) dst[w] ^= src[w];
    };
    auto poly_lshift1 = [W](uint64_t* poly) noexcept {
        uint64_t carry = 0;
        for (int w = 0; w < W; ++w) {
            uint64_t new_carry = poly[w] >> 63;
            poly[w] = (poly[w] << 1) | carry;
            carry = new_carry;
        }
    };
    auto poly_get_bit = [](const uint64_t* poly, int e) noexcept -> bool {
        return (poly[e / 64] >> (e % 64)) & 1ULL;
    };

    // ── Main loop ──
    for (int e = 0; e < L; ++e) {
        for (int i = 0; i < m; ++i) {
            int pivot = -1;
            int min_delta = INT_MAX;
            for (int j = 0; j < b; ++j) {
                if (poly_get_bit(E_at(i, j), e) &&
                    delta[static_cast<size_t>(j)] < min_delta) {
                    min_delta = delta[static_cast<size_t>(j)];
                    pivot = j;
                }
            }
            if (pivot < 0) continue;

            const uint64_t* E_piv_col = nullptr;  // for caching pivot col data
            const uint64_t* P_piv_col = nullptr;
            for (int k = 0; k < b; ++k) {
                if (k == pivot) continue;
                if (!poly_get_bit(E_at(i, k), e)) continue;
                // E[:, k] ^= E[:, pivot]
                for (int l = 0; l < m; ++l) {
                    poly_xor(E_at(l, k), E_at(l, pivot));
                }
                // P[:, k] ^= P[:, pivot]
                for (int l = 0; l < b; ++l) {
                    poly_xor(P_at(l, k), P_at(l, pivot));
                }
            }
            (void)E_piv_col; (void)P_piv_col;

            // Shift pivot col up by 1 (consume bit e).
            for (int l = 0; l < m; ++l) poly_lshift1(E_at(l, pivot));
            for (int l = 0; l < b; ++l) poly_lshift1(P_at(l, pivot));
            delta[static_cast<size_t>(pivot)]++;
        }
    }

    // ── Extract F from top n rows of n smallest-delta cols of P ──
    std::array<int, b> col_order;
    for (int j = 0; j < b; ++j) col_order[static_cast<size_t>(j)] = j;
    std::sort(col_order.begin(), col_order.end(),
              [&delta](int a, int c) {
                  return delta[static_cast<size_t>(a)] < delta[static_cast<size_t>(c)];
              });

    auto poly_max_degree = [W](const uint64_t* poly) -> int {
        for (int w = W - 1; w >= 0; --w) {
            if (poly[w]) return w * 64 + (63 - gnfs::util::clz64(poly[w]));
        }
        return -1;
    };

    int max_deg = 0;
    for (int idx = 0; idx < n; ++idx) {
        int c = col_order[static_cast<size_t>(idx)];
        for (int i = 0; i < n; ++i) {
            int deg = poly_max_degree(P_at(i, c));
            if (deg > max_deg) max_deg = deg;
        }
    }

    LingenResult result;
    result.poly.assign(static_cast<size_t>(max_deg + 1), DenseGF2_64x64{});
    result.degrees.fill(0);
    result.valid_mask = 0;

    for (int j_out = 0; j_out < n; ++j_out) {
        int c = col_order[static_cast<size_t>(j_out)];
        bool nontrivial = false;
        int dj = 0;
        for (int i = 0; i < n; ++i) {
            const uint64_t* poly = P_at(i, c);
            int deg = poly_max_degree(poly);
            if (deg < 0) continue;
            nontrivial = true;
            if (deg > dj) dj = deg;
            for (int w = 0; w < W; ++w) {
                uint64_t bits = poly[w];
                while (bits) {
                    int local_bit = gnfs::util::ctz64(bits);
                    int k = w * 64 + local_bit;
                    if (k <= max_deg) {
                        result.poly[static_cast<size_t>(k)].rows[static_cast<size_t>(i)] |=
                            (1ULL << j_out);
                    }
                    bits &= bits - 1;
                }
            }
        }
        if (nontrivial) {
            result.valid_mask |= (1ULL << j_out);
            result.degrees[static_cast<size_t>(j_out)] = dj;
        }
    }

    return result;
}

std::vector<std::vector<bool>> BlockWiedemann::extract_solutions(
    const CSRMatrix&, size_t, const LingenResult&, const BlockVector&, size_t) {
    throw std::logic_error("BlockWiedemann::extract_solutions: reserved for true block-BW; "
                           "streaming BW extracts in find_dependencies");
}

} // namespace gnfs::linalg

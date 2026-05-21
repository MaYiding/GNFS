// Metal-backed SpMV for the GF(2) 64-bit block kernels. See header
// `gnfs/linalg/metal_spmv.hpp` for the public contract and design notes.
//
// Implementation summary
// ----------------------
// * `MetalContext` is a process-singleton that holds the MTLDevice,
//   command queue, and the two compiled pipeline objects. It is lazily
//   initialised on the first call. Failures during shader compilation
//   set the "unavailable" flag so subsequent calls short-circuit to
//   false.
// * Buffer reuse: the matrix CSR pointers, the dense input pointer, and
//   the dense output pointer are tracked via address-keyed caches. Most
//   BW Phase 1 iterations re-use the same matrix and only swap the x/y
//   buffers — the cache keeps the GPU-side buffers hot.
// * 64-bit GF(2) XOR is implemented as a pair of independent 32-bit
//   atomic XORs. The Metal Shading Language exposes 32-bit
//   `atomic_uint` fetch-xor on every M-series device. 64-bit fetch-xor
//   is not yet exposed even on M3+ (verified empirically against the
//   shipping toolchain). Splitting is safe because GF(2) XOR commutes
//   across bit positions: the high and low halves of a `uint64_t` XOR
//   independently and the combined result is bit-identical to a single
//   atomic 64-bit XOR.
// * No memory reordering ambiguity exists: GF(2) XOR is commutative and
//   associative, so `memory_order_relaxed` is sufficient. The CPU-side
//   blits + commit + waitUntilCompleted establish the data-race-free
//   handover.

#include "gnfs/linalg/metal_spmv.hpp"

#include <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace gnfs::linalg::metal {

namespace {

// ---------------------------------------------------------------------------
// Env var parsing
// ---------------------------------------------------------------------------

bool parse_env_truthy(const char* val) noexcept {
    if (!val) return false;
    std::string s(val);
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
    return false;
}

// ---------------------------------------------------------------------------
// MSL source (compiled at runtime via `newLibraryWithSource:`)
// ---------------------------------------------------------------------------
//
// We split each 64-bit GF(2) block into two 32-bit halves stored at
// (lane*2, lane*2+1) of a `uint32_t` array. `spmv_forward` reads x as
// `uint2` pairs, accumulates per-lane in registers, and writes once per
// row (no atomics — gather + write-once). `spmv_transpose` scatters
// each row's contribution to `y[col_idx[j]]` using two `atomic_uint`
// XOR operations per half.
//
// Threadgroup size is left at 64 (single-warp Apple GPUs). The dispatch
// grid uses `threadgroups = ceil(num_rows / 64)`. Threads beyond
// `num_rows` early-out via the row-index bounds check.
NSString* const kSpmvShaderSource = @R"MSL(
#include <metal_stdlib>
using namespace metal;

struct SpmvParams {
    uint num_rows;
    uint num_cols;
};

// Forward SpMV. y[i] = XOR over j in row(i) of x[col_indices[j]].
// Each thread owns a single row; no atomics needed.
kernel void spmv_forward_gf2_64(
    device const uint*    row_offsets [[buffer(0)]],
    device const uint*    col_indices [[buffer(1)]],
    device const uint2*   x           [[buffer(2)]],   // packed (lo, hi) per row
    device uint2*         y           [[buffer(3)]],
    constant SpmvParams&  params      [[buffer(4)]],
    uint                  gid         [[thread_position_in_grid]])
{
    if (gid >= params.num_rows) return;
    uint start = row_offsets[gid];
    uint stop  = row_offsets[gid + 1];
    uint2 acc = uint2(0, 0);
    for (uint p = start; p < stop; ++p) {
        acc ^= x[col_indices[p]];
    }
    y[gid] = acc;
}

// Transpose SpMV. For each row i with x[i] != 0, XOR x[i] into
// y[col_indices[j]] for every j in row(i). Atomic XOR (32-bit halves)
// handles the gather-scatter conflicts.
kernel void spmv_transpose_gf2_64(
    device const uint*          row_offsets [[buffer(0)]],
    device const uint*          col_indices [[buffer(1)]],
    device const uint2*         x           [[buffer(2)]],
    device atomic_uint*         y           [[buffer(3)]],
    constant SpmvParams&        params      [[buffer(4)]],
    uint                        gid         [[thread_position_in_grid]])
{
    if (gid >= params.num_rows) return;
    uint2 xi = x[gid];
    if (xi.x == 0 && xi.y == 0) return;
    uint start = row_offsets[gid];
    uint stop  = row_offsets[gid + 1];
    for (uint p = start; p < stop; ++p) {
        uint c = col_indices[p];
        atomic_fetch_xor_explicit(&y[2u * c],     xi.x, memory_order_relaxed);
        atomic_fetch_xor_explicit(&y[2u * c + 1], xi.y, memory_order_relaxed);
    }
}
)MSL";

// ---------------------------------------------------------------------------
// MetalContext — lazily initialised process singleton
// ---------------------------------------------------------------------------

class MetalContext {
public:
    static MetalContext& instance() {
        static MetalContext ctx;
        return ctx;
    }

    bool ready() const noexcept { return ready_; }

    id<MTLDevice>            device()           const noexcept { return device_; }
    id<MTLCommandQueue>      queue()            const noexcept { return queue_; }
    id<MTLComputePipelineState> forward_pso()   const noexcept { return forward_pso_; }
    id<MTLComputePipelineState> transpose_pso() const noexcept { return transpose_pso_; }

private:
    MetalContext() {
        @autoreleasepool {
            device_ = MTLCreateSystemDefaultDevice();
            if (!device_) return;
            queue_  = [device_ newCommandQueue];
            if (!queue_) return;

            NSError* err = nil;
            id<MTLLibrary> lib = [device_ newLibraryWithSource:kSpmvShaderSource
                                                       options:nil
                                                         error:&err];
            if (!lib) return;

            id<MTLFunction> fwd_fn = [lib newFunctionWithName:@"spmv_forward_gf2_64"];
            id<MTLFunction> tr_fn  = [lib newFunctionWithName:@"spmv_transpose_gf2_64"];
            if (!fwd_fn || !tr_fn) {
                [lib release];
                return;
            }

            forward_pso_   = [device_ newComputePipelineStateWithFunction:fwd_fn   error:&err];
            transpose_pso_ = [device_ newComputePipelineStateWithFunction:tr_fn    error:&err];
            [fwd_fn release];
            [tr_fn  release];
            [lib    release];

            if (!forward_pso_ || !transpose_pso_) return;
            ready_ = true;
        }
    }

    // Singletons never destruct (process lifetime), so we deliberately do
    // not release the held Objective-C objects in a destructor. This
    // avoids ordering hazards with the Metal runtime at exit time.

    id<MTLDevice>               device_         = nil;
    id<MTLCommandQueue>         queue_          = nil;
    id<MTLComputePipelineState> forward_pso_    = nil;
    id<MTLComputePipelineState> transpose_pso_  = nil;
    bool                        ready_          = false;
};

// ---------------------------------------------------------------------------
// Buffer cache: address-keyed reuse of MTLBuffer wrappers
// ---------------------------------------------------------------------------
//
// SpMV is invoked thousands of times against the same matrix during BW
// Phase 1. We cache one `MTLBuffer` per host-side pointer (the CSR
// arrays do not move) and reuse it across calls. The cache is small
// (~6 entries: row_offsets, col_indices, x, y plus a couple of
// alternates) and short-lived (cleared on size mismatch).

struct CachedBuffer {
    id<MTLBuffer> buf       = nil;
    std::size_t   size_bytes = 0;
    const void*   src_ptr    = nullptr;  // host pointer that produced this buffer
};

class BufferCache {
public:
    static BufferCache& instance() {
        static BufferCache c;
        return c;
    }

    // Acquire (and populate if needed) a buffer for a host source of
    // `size_bytes` bytes. If `do_copy` is true the host bytes are blited
    // into the buffer. Returns nil on allocation failure.
    id<MTLBuffer> upload(const void* host_ptr, std::size_t size_bytes,
                          bool do_copy) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = cache_.find(host_ptr);
        if (it == cache_.end() || it->second.size_bytes != size_bytes) {
            // Either fresh pointer or size mismatch → reallocate.
            if (it != cache_.end()) {
                [it->second.buf release];
                cache_.erase(it);
            }
            id<MTLBuffer> buf = [MetalContext::instance().device()
                newBufferWithLength:size_bytes
                            options:MTLResourceStorageModeShared];
            if (!buf) return nil;
            cache_[host_ptr] = CachedBuffer{buf, size_bytes, host_ptr};
            it = cache_.find(host_ptr);
        }
        if (do_copy) {
            std::memcpy([it->second.buf contents], host_ptr, size_bytes);
        }
        return it->second.buf;
    }

    // Download from a Metal buffer back into host memory. Caller knows
    // the buffer was earlier uploaded via `upload()`.
    static void download(id<MTLBuffer> buf, void* host_ptr,
                         std::size_t size_bytes) {
        std::memcpy(host_ptr, [buf contents], size_bytes);
    }

    // Drop the cached buffer for `host_ptr`, if any. Called when the
    // caller signals the host buffer is going away (defensive — the BW
    // code keeps CSR arrays alive for the whole Phase 1).
    void evict(const void* host_ptr) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = cache_.find(host_ptr);
        if (it != cache_.end()) {
            [it->second.buf release];
            cache_.erase(it);
        }
    }

private:
    std::mutex mu_;
    std::unordered_map<const void*, CachedBuffer> cache_;
};

// ---------------------------------------------------------------------------
// Common per-call submission helpers
// ---------------------------------------------------------------------------

// Computes the (threads_per_threadgroup, threadgroups) tuple that
// covers `num_rows` rows. Threadgroup size of 64 matches the inner
// SIMD width on Apple GPUs and is large enough to amortise dispatch
// overhead without overflowing tile memory.
struct GridDims {
    MTLSize threads_per_tg;
    MTLSize threadgroups;
};

GridDims grid_dims_for(std::size_t num_rows,
                       id<MTLComputePipelineState> pso) {
    NSUInteger tg = pso.maxTotalThreadsPerThreadgroup;
    if (tg > 64) tg = 64;
    if (tg < 1)  tg = 1;
    NSUInteger groups = (num_rows + tg - 1) / tg;
    return GridDims{
        MTLSizeMake(tg, 1, 1),
        MTLSizeMake(groups, 1, 1),
    };
}

struct ParamsLayout {
    uint32_t num_rows;
    uint32_t num_cols;
};

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool is_available() noexcept {
    return MetalContext::instance().ready();
}

bool env_opt_in() noexcept {
    static std::atomic<int> cached{-1};  // -1 = not probed
    int v = cached.load(std::memory_order_relaxed);
    if (v < 0) {
        bool on = parse_env_truthy(std::getenv("GNFS_METAL_SPMV"));
        v = on ? 1 : 0;
        cached.store(v, std::memory_order_relaxed);
    }
    return v == 1;
}

bool spmv_forward(std::size_t num_rows,
                  std::size_t num_cols,
                  const std::uint32_t* row_offsets,
                  const std::uint32_t* col_indices,
                  std::size_t nnz,
                  const std::uint64_t* x,
                  std::uint64_t* y) noexcept {
    if (!is_available()) return false;
    if (num_rows == 0) {
        // No work; zero-sized matrix is a valid identity element. The
        // CPU kernels assert on length matches but a zero-row matrix
        // would simply write zero bytes to y. Skip to keep parity.
        return true;
    }
    if (!row_offsets || (!col_indices && nnz != 0) || !x || !y) return false;

    @autoreleasepool {
        auto& ctx = MetalContext::instance();

        const std::size_t row_off_bytes = (num_rows + 1) * sizeof(std::uint32_t);
        const std::size_t col_bytes     = nnz * sizeof(std::uint32_t);
        const std::size_t x_bytes       = num_cols * sizeof(std::uint64_t);
        const std::size_t y_bytes       = num_rows * sizeof(std::uint64_t);

        auto& cache = BufferCache::instance();
        id<MTLBuffer> row_buf = cache.upload(row_offsets, row_off_bytes, /*do_copy=*/true);
        if (!row_buf) return false;
        // nnz may be zero; skip allocating col buffer in that case.
        id<MTLBuffer> col_buf = nil;
        if (nnz > 0) {
            col_buf = cache.upload(col_indices, col_bytes, /*do_copy=*/true);
            if (!col_buf) return false;
        }
        id<MTLBuffer> x_buf = cache.upload(x, x_bytes, /*do_copy=*/true);
        if (!x_buf) return false;
        // y is write-only output; we still upload to allocate, but no copy needed.
        id<MTLBuffer> y_buf = cache.upload(y, y_bytes, /*do_copy=*/false);
        if (!y_buf) return false;

        ParamsLayout params{
            static_cast<uint32_t>(num_rows),
            static_cast<uint32_t>(num_cols),
        };

        id<MTLCommandBuffer> cmd = [ctx.queue() commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:ctx.forward_pso()];
        [enc setBuffer:row_buf offset:0 atIndex:0];
        // When nnz == 0 we still must bind buffer 1 to satisfy the
        // shader signature; bind the row buffer as a harmless filler
        // (it is never indexed because the loop body never runs).
        [enc setBuffer:(col_buf ? col_buf : row_buf) offset:0 atIndex:1];
        [enc setBuffer:x_buf   offset:0 atIndex:2];
        [enc setBuffer:y_buf   offset:0 atIndex:3];
        [enc setBytes:&params length:sizeof(params) atIndex:4];

        GridDims grid = grid_dims_for(num_rows, ctx.forward_pso());
        [enc dispatchThreadgroups:grid.threadgroups
            threadsPerThreadgroup:grid.threads_per_tg];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];

        if ([cmd status] != MTLCommandBufferStatusCompleted) {
            return false;
        }

        BufferCache::download(y_buf, y, y_bytes);
    }
    return true;
}

bool spmv_transpose(std::size_t num_rows,
                    std::size_t num_cols,
                    const std::uint32_t* row_offsets,
                    const std::uint32_t* col_indices,
                    std::size_t nnz,
                    const std::uint64_t* x,
                    std::uint64_t* y) noexcept {
    if (!is_available()) return false;
    if (num_rows == 0 || num_cols == 0) {
        // Nothing to write; mirror the CPU kernel's "all zeros" output.
        if (y && num_cols > 0) {
            std::memset(y, 0, num_cols * sizeof(std::uint64_t));
        }
        return true;
    }
    if (!row_offsets || (!col_indices && nnz != 0) || !x || !y) return false;

    @autoreleasepool {
        auto& ctx = MetalContext::instance();

        const std::size_t row_off_bytes = (num_rows + 1) * sizeof(std::uint32_t);
        const std::size_t col_bytes     = nnz * sizeof(std::uint32_t);
        const std::size_t x_bytes       = num_rows * sizeof(std::uint64_t);
        const std::size_t y_bytes       = num_cols * sizeof(std::uint64_t);

        auto& cache = BufferCache::instance();
        id<MTLBuffer> row_buf = cache.upload(row_offsets, row_off_bytes, /*do_copy=*/true);
        if (!row_buf) return false;
        id<MTLBuffer> col_buf = nil;
        if (nnz > 0) {
            col_buf = cache.upload(col_indices, col_bytes, /*do_copy=*/true);
            if (!col_buf) return false;
        }
        id<MTLBuffer> x_buf = cache.upload(x, x_bytes, /*do_copy=*/true);
        if (!x_buf) return false;
        // The transpose kernel atomic-XORs into y, so it must start as
        // zero. Always zero the buffer regardless of caller-side state
        // because the cache may be reusing a buffer that holds an older
        // result.
        id<MTLBuffer> y_buf = cache.upload(y, y_bytes, /*do_copy=*/false);
        if (!y_buf) return false;
        std::memset([y_buf contents], 0, y_bytes);

        ParamsLayout params{
            static_cast<uint32_t>(num_rows),
            static_cast<uint32_t>(num_cols),
        };

        id<MTLCommandBuffer> cmd = [ctx.queue() commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:ctx.transpose_pso()];
        [enc setBuffer:row_buf offset:0 atIndex:0];
        [enc setBuffer:(col_buf ? col_buf : row_buf) offset:0 atIndex:1];
        [enc setBuffer:x_buf   offset:0 atIndex:2];
        [enc setBuffer:y_buf   offset:0 atIndex:3];
        [enc setBytes:&params length:sizeof(params) atIndex:4];

        GridDims grid = grid_dims_for(num_rows, ctx.transpose_pso());
        [enc dispatchThreadgroups:grid.threadgroups
            threadsPerThreadgroup:grid.threads_per_tg];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];

        if ([cmd status] != MTLCommandBufferStatusCompleted) {
            return false;
        }

        BufferCache::download(y_buf, y, y_bytes);
    }
    return true;
}

} // namespace gnfs::linalg::metal

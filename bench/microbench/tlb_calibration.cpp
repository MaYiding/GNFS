// P1.B-3 micro-bench: calibrate L1D_TLB_MISS PMU event semantics on Apple M5.
//
// Goal: verify that the L1D_TLB_MISS counter (as5.plist) really reflects
// TLB miss events, by sweeping working-set size and access pattern.
//
// Background — doctrine §6 P1.B-1b showed TLBMissRate = TLB_MISS/INST drop from
// 61.6% → 52.7% after adding __builtin_prefetch to xor_rows. But we never
// validated that L1D_TLB_MISS actually counts TLB misses (vs. some related
// event reverse-engineered from the M-series PMU). Before applying super-pages
// (macOS VM_FLAGS_SUPERPAGE_SIZE_2MB or Linux MADV_HUGEPAGE) project-wide, we
// need this calibration.
//
// Apple M5 (per ARM docs + tzakharko reverse-engineering):
//   - L1 dTLB capacity ≈ 256 entries
//   - Page size on macOS arm64: 16 KB (default)
//   - L1 dTLB coverage: 256 × 16 KB ≈ 4 MB
//
// Expected outcomes:
//   - small (1 MB) seq: TLB-miss rate ≈ 0 (working set inside dTLB)
//   - small random:     TLB-miss rate ≈ 0 (still inside dTLB)
//   - large (256 MB) seq: TLB miss ≈ N_pages × N_passes (page-walk on first touch)
//   - large random:    TLB miss ≈ N_accesses × (1 - dTLB_capacity/N_pages) → near 1.0 per access
//
// Usage:
//   tlb_calibration <mode> <bytes> <iters>
//     mode: 0 = sequential, 1 = random
//
// Driven by scripts/bench/tlb_calibration_run.sh which pipes through
// scripts/perf/pmu-stat.sh.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

// We touch one cache line per page to expose TLB pressure without polluting
// the L1D cache with redundant accesses inside a page. Each touch is a tagged
// write so the compiler cannot DCE the loop.
static inline void touch(uint8_t* p, uint64_t tag) noexcept {
    // Single-byte write at offset 0 of the page is enough to walk the PTE.
    p[0] = static_cast<uint8_t>(tag ^ p[0]);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "Usage: %s <mode 0=seq|1=rand> <bytes> <iters>\n", argv[0]);
        return 1;
    }
    const int mode = std::atoi(argv[1]);
    const std::size_t bytes = static_cast<std::size_t>(std::atoll(argv[2]));
    const int iters = std::atoi(argv[3]);

    // macOS arm64: 16 KB default page size. Use this to compute page count
    // and stride. (sysconf(_SC_PAGESIZE) would work too but compile-time is fine.)
    constexpr std::size_t kPageBytes = 16 * 1024;
    const std::size_t pages = (bytes + kPageBytes - 1) / kPageBytes;

    // Allocate page-aligned buffer. malloc doesn't guarantee 16K alignment, so
    // use posix_memalign.
    void* raw = nullptr;
    if (posix_memalign(&raw, kPageBytes, pages * kPageBytes) != 0 || !raw) {
        std::fprintf(stderr, "posix_memalign failed\n");
        return 2;
    }
    auto* buf = static_cast<uint8_t*>(raw);

    // First-touch every page to ensure physical backing — we want to measure
    // steady-state TLB misses, not minor faults.
    std::memset(buf, 0, pages * kPageBytes);

    // Precompute access pattern (page indices) so the inner loop has no
    // dependency on the access mode. Length = iters × pages so each iteration
    // visits every page exactly once (in seq order or shuffled).
    std::vector<uint32_t> order(pages);
    for (std::size_t i = 0; i < pages; ++i) order[i] = static_cast<uint32_t>(i);
    if (mode == 1) {
        std::mt19937_64 rng(0xC0FFEEull ^ bytes);
        std::shuffle(order.begin(), order.end(), rng);
    }

    // Sink to prevent DCE.
    uint64_t sink = 0;

    auto t0 = std::chrono::steady_clock::now();
    for (int it = 0; it < iters; ++it) {
        for (std::size_t k = 0; k < pages; ++k) {
            uint8_t* p = buf + static_cast<std::size_t>(order[k]) * kPageBytes;
            touch(p, static_cast<uint64_t>(it) * 31 + k);
            sink ^= p[0];
        }
    }
    auto t1 = std::chrono::steady_clock::now();

    const double wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const std::size_t total_touches = static_cast<std::size_t>(iters) * pages;
    std::printf(
        "tlb_calibration mode=%s bytes=%zu pages=%zu iters=%d touches=%zu "
        "wall_ms=%.3f ns_per_touch=%.2f sink=%llu\n",
        (mode == 0 ? "seq" : "rand"), bytes, pages, iters, total_touches,
        wall_ms, wall_ms * 1e6 / static_cast<double>(total_touches),
        static_cast<unsigned long long>(sink));

    std::free(raw);
    return 0;
}

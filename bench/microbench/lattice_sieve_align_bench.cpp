// P1.B-2 micro-bench: isolate row-stride alignment effect in sieve_row_chunk.
// Mimics small-prime stride-add hot path for various widths.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

struct CompactSmallPrime {
    uint32_t p;
    uint16_t log_p;
    int16_t delta;
    int16_t i_min_mod;
    int16_t i_mod;
};

// Mimics sieve_row_chunk's hot loop
static inline void sieve_inner(std::vector<uint16_t>& arr,
                                const std::vector<CompactSmallPrime>& primes,
                                size_t width, size_t height) {
    std::vector<CompactSmallPrime> sp = primes;
    for (size_t j = 0; j < height; ++j) {
        size_t row_base = j * width;
        size_t row_end = row_base + width;
        for (auto& s : sp) {
            int32_t off = static_cast<int32_t>(s.i_mod) - static_cast<int32_t>(s.i_min_mod);
            if (off < 0) off += static_cast<int32_t>(s.p);
            size_t idx = row_base + static_cast<size_t>(off);
            uint16_t lp = s.log_p;
            size_t stride = static_cast<size_t>(s.p);
            for (; idx < row_end; idx += stride) {
                arr[idx] += lp;
            }
            int32_t new_mod = static_cast<int32_t>(s.i_mod) + static_cast<int32_t>(s.delta);
            int32_t p32 = static_cast<int32_t>(s.p);
            if (new_mod >= p32) new_mod -= p32;
            s.i_mod = static_cast<int16_t>(new_mod);
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <width>\n", argv[0]);
        return 1;
    }
    size_t width = static_cast<size_t>(std::atoll(argv[1]));
    size_t height = 800;  // matches test_factor_with_kleinjung
    size_t iters = 200;   // simulate 200 SQ-runs of sieve_row_chunk

    std::vector<uint16_t> arr(width * height, 0);

    // Build representative small-prime set (mimics small primes in 81-bit test, ~500 small)
    std::mt19937 rng(42);
    std::vector<CompactSmallPrime> primes;
    primes.reserve(500);
    // FB small primes: log-spaced from 7 to ~6000
    for (uint32_t p = 7; p < 6000; ) {
        CompactSmallPrime sp;
        sp.p = p;
        sp.log_p = static_cast<uint16_t>(__builtin_ctzl(p) > 0 ? 0 : __builtin_clzll(p) ^ 63);  // ~log_p
        if (sp.log_p == 0) sp.log_p = static_cast<uint16_t>(64 - __builtin_clzll(p));
        sp.delta = static_cast<int16_t>((rng() % p) - p/2);
        sp.i_min_mod = static_cast<int16_t>(rng() % p);
        sp.i_mod = static_cast<int16_t>(rng() % p);
        primes.push_back(sp);
        // Next prime, log-spaced (approximation)
        p = static_cast<uint32_t>(p * 1.02) + 1;
    }

    // Print alignment info
    uintptr_t base_addr = reinterpret_cast<uintptr_t>(arr.data());
    size_t row_bytes = width * 2;
    printf("width=%zu height=%zu n_primes=%zu base=%lx mod128=%lu row_stride=%zu row_bytes_mod128=%zu\n",
           width, height, primes.size(),
           static_cast<unsigned long>(base_addr),
           static_cast<unsigned long>(base_addr % 128),
           row_bytes,
           row_bytes % 128);

    // Warmup
    sieve_inner(arr, primes, width, height);

    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iters; ++i) {
        sieve_inner(arr, primes, width, height);
    }
    auto end = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double per_iter_ms = ns / 1e6 / iters;
    double per_inner_ns = static_cast<double>(ns) / iters / height / primes.size();
    printf("Total: %lld ns, per-iter: %.3f ms, per-prime-per-row: %.2f ns, checksum=%u\n",
           static_cast<long long>(ns), per_iter_ms, per_inner_ns,
           static_cast<unsigned>(arr[0] + arr[width * height / 2] + arr[width * height - 1]));
    return 0;
}

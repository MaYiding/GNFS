# Factor-Base Root Buffer Measurements

The builder stores root results in one flat vector per contiguous worker
range, then merges ranges in prime order. This removes the retained vector
and allocation for every candidate prime. The worker cap, parallel threshold,
and root finder remain unchanged. Each chunk restores the builder's historical
two-entry-per-prime reserve estimate.

## Bounded 50-Digit Experiment

Measurements on 2026-11-04 compared `ddeda118` with the chunk-buffer change
using macOS arm64, AppleClang 21, Release/NDEBUG, and native architecture
tuning disabled. Each table entry is the median of three fresh processes.
The same degree-four Base-M context was constructed before each timed build
from the product of `3000000000000000000020161` and
`4000000000000000000011001`.

| Ordinary Bound / Special-Q Bound | Mode | Previous Peak RSS (Bytes) | Chunk Peak RSS (Bytes) | Reduction |
| --- | --- | ---: | ---: | ---: |
| 1,000,000 / 1,000,000 | Two workers | 17,350,656 | 14,368,768 | 17.19% |
| 1,000,000 / 4,000,000 | Two workers | 41,074,688 | 33,128,448 | 19.35% |
| 1,000,000 / 4,000,000 | `parallel=false` | 41,025,536 | 34,242,560 | 16.53% |

The two-worker ordinary-only run reduced counted C++ allocations from
11,313,749 to 11,235,253. With the special-Q extension, the count fell from
43,557,845 to 43,274,703: 283,142 fewer calls, equal to the candidate-prime
count minus the four retained chunk buffers. The serial extended run fell
from 43,557,813 to 43,274,669. Counts were identical across repetitions.
These counters cover ordinary C++ `new` calls during `build()`, not GMP's
internal allocator or aligned allocations. RSS comes from macOS
`/usr/bin/time -l` and includes the whole process.

All before/after runs had identical ordered algebraic-entry digests and
counts. The ordinary band contained 78,403 algebraic entries with digest
`14864646122494902973`; the extended band contained 282,411 entries with
digest `10032168097100610286`. Both retained 78,498 rational entries and the
same ordinary/special-Q boundary. This experiment measures buffer overhead
on one bounded 50-digit workload. It makes no wall-time or larger-size claim.

## Reproduction

Save the probe below as `.cache/fb-root-chunks/probe.cpp`. On each revision,
build and link it against that revision's library in an isolated directory.
`GMP_PREFIX` and `NTL_PREFIX` identify the local dependency installations.
The direct target build is for this diagnostic; ordinary regressions use
`scripts/test.sh`.

```bash
cmake -S . -B build-fb-roots-release -DCMAKE_BUILD_TYPE=Release -DGNFS_ENABLE_NATIVE_ARCH=OFF
cmake --build build-fb-roots-release --target gnfs_core -j 2
c++ -std=c++20 -O3 -DNDEBUG -Iinclude -I"${GMP_PREFIX}/include" \
  .cache/fb-root-chunks/probe.cpp build-fb-roots-release/libgnfs_core.a \
  build-fb-roots-release/libgnfs_util.a -L"${NTL_PREFIX}/lib" \
  -L"${GMP_PREFIX}/lib" -lntl -lgmp -o .cache/fb-root-chunks/probe
GNFS_FB_ROOTS_THREADS=2 /usr/bin/time -l .cache/fb-root-chunks/probe 1000000 1000000 1
GNFS_FB_ROOTS_THREADS=2 /usr/bin/time -l .cache/fb-root-chunks/probe 1000000 4000000 1
GNFS_FB_ROOTS_THREADS=1 /usr/bin/time -l .cache/fb-root-chunks/probe 1000000 4000000 0
```

Repeat each invocation three times. The final argument controls
`Options::parallel`; the first two set ordinary and special-Q bounds.

```cpp
#include <gnfs/core/integer.hpp>
#include <gnfs/factor_base/builder.hpp>
#include <gnfs/polynomial/base_m.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>

namespace {
std::atomic<bool> counting{false};
std::atomic<unsigned long long> allocations{0};
std::atomic<unsigned long long> requested_bytes{0};
} // namespace

void* operator new(std::size_t size) {
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) {
        if (counting.load(std::memory_order_relaxed)) {
            allocations.fetch_add(1, std::memory_order_relaxed);
            requested_bytes.fetch_add(size, std::memory_order_relaxed);
        }
        return pointer;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* pointer) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

int main(int argc, char** argv) {
    using gnfs::core::Integer;
    using gnfs::factor_base::FactorBaseBuilder;
    using gnfs::polynomial::BaseMSelector;
    if (argc != 4)
        return 2;
    const Integer n = Integer("3000000000000000000020161") * Integer("4000000000000000000011001");
    auto selection = BaseMSelector::select(n, 4);
    if (!selection.success)
        return 3;
    auto context = BaseMSelector::create_context(n, selection);
    if (!context.verify())
        return 4;
    FactorBaseBuilder::Options options;
    options.rational_bound = static_cast<uint32_t>(std::stoul(argv[1]));
    options.algebraic_bound = options.rational_bound;
    options.special_q_bound = static_cast<uint32_t>(std::stoul(argv[2]));
    options.parallel = std::stoul(argv[3]) != 0;
    counting.store(true, std::memory_order_relaxed);
    const auto start = std::chrono::steady_clock::now();
    const auto base = FactorBaseBuilder::build(context, options);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    counting.store(false, std::memory_order_relaxed);
    uint64_t digest = UINT64_C(14695981039346656037);
    const auto mix = [&](uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            digest ^= (value >> shift) & 0xffU;
            digest *= UINT64_C(1099511628211);
        }
    };
    for (const auto& prime : base.algebraic()) {
        mix(prime.p);
        mix(prime.r);
        mix(prime.log_p);
        mix(prime.degree);
    }
    std::cout << "digits=" << n.to_string().size() << " degree=" << context.degree()
              << " fb_bound=" << options.algebraic_bound << " sq_bound=" << options.special_q_bound
              << " parallel=" << options.parallel << " rational=" << base.rational_count()
              << " sieve_algebraic=" << base.sieve_algebraic_count()
              << " algebraic=" << base.algebraic_count() << " digest=" << digest
              << " cpp_allocations=" << allocations.load()
              << " cpp_requested_bytes=" << requested_bytes.load()
              << " elapsed_s=" << elapsed.count() << '\n';
}
```

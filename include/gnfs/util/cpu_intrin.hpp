#pragma once

/// @file cpu_intrin.hpp
/// Cross-compiler CPU hint intrinsics (prefetch, pause/yield).
///
/// GCC/Clang expose `__builtin_prefetch` and inline assembly
/// (`asm volatile("pause")`, `asm volatile("yield")`). MSVC exposes
/// `_mm_prefetch`, `_mm_pause`, `__yield()` via <intrin.h>. This header
/// provides portable wrappers so call sites can stay compiler-agnostic.

#include <cstddef>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace gnfs::util {

/// Prefetch hint for reads. `Locality` is 0..3 (0 = no temporal
/// locality, 3 = highest). `__builtin_prefetch` and `_mm_prefetch`
/// require compile-time constants for locality, so this is a template
/// parameter (not runtime). Maps to:
///   - GCC/Clang: `__builtin_prefetch(addr, 0, Locality)`
///   - MSVC x86: `_mm_prefetch((const char*)addr, _MM_HINT_*)`
///   - MSVC ARM: `__prefetch(addr)` (loses locality info)
///   - Other:    no-op
template <int Locality = 0>
inline void prefetch_read(const void* addr) noexcept {
    static_assert(Locality >= 0 && Locality <= 3,
                  "prefetch locality must be in [0..3]");
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(addr, 0, Locality);
#elif defined(_MSC_VER)
    #if defined(_M_X64) || defined(_M_IX86)
        // _MM_HINT_NTA=0, _MM_HINT_T2=1, _MM_HINT_T1=2, _MM_HINT_T0=3
        _mm_prefetch(reinterpret_cast<const char*>(addr), Locality);
    #elif defined(_M_ARM64) || defined(_M_ARM)
        __prefetch(addr);
    #else
        (void)addr;
    #endif
#else
    (void)addr;
#endif
}

/// Prefetch hint for writes.
template <int Locality = 1>
inline void prefetch_write(const void* addr) noexcept {
    static_assert(Locality >= 0 && Locality <= 3,
                  "prefetch locality must be in [0..3]");
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(addr, 1, Locality);
#elif defined(_MSC_VER)
    // MSVC lacks a write-prefetch separate from read; fall back to read prefetch.
    prefetch_read<Locality>(addr);
#else
    (void)addr;
#endif
}

/// CPU spin-loop hint (pause / yield). On ARM `yield`, x86 `pause`,
/// elsewhere fall back to a no-op (caller should use
/// `std::this_thread::yield()` for a heavier fallback).
inline void cpu_pause() noexcept {
#if defined(__GNUC__) || defined(__clang__)
    #if defined(__aarch64__) || defined(__arm__)
        asm volatile("yield" ::: "memory");
    #elif defined(__x86_64__) || defined(__i386__)
        asm volatile("pause" ::: "memory");
    #endif
#elif defined(_MSC_VER)
    #if defined(_M_X64) || defined(_M_IX86)
        _mm_pause();
    #elif defined(_M_ARM64) || defined(_M_ARM)
        __yield();
    #endif
#endif
}

}  // namespace gnfs::util

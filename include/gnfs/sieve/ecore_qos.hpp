#pragma once

// Sieve E-core utilization policy (BACKLOG #4 macOS heterogeneous scheduling).
//
// macOS schedules std::thread on P-cores by default (workers inherit parent QoS
// class UserInitiated). On Apple Silicon M5 (4 P + 6 E), lattice_sieve thus uses
// only ~40% of the CPU. BACKLOG #4 records 50d V0+fix 实测 average 3.5-4 cores
// usage during sieve, P-cores saturated while E-cores idle.
//
// This helper lets the user opt-in via GNFS_SIEVE_ECORE_THREADS=N to put N of
// the sieve worker threads on Utility QoS — a hint to the macOS scheduler to
// place them on E-cores. Mixing P+E lock-step generally hurts (slowest-core
// barrier), but lattice_sieve uses work-stealing (atomic fetch_add over region
// index) so faster cores naturally grab more regions, making the design robust
// to heterogeneous cores.
//
// Defaults preserve prior behavior:
//   ENV unset / "" / non-numeric / <= 0:   ecore_count = 0   (all UserInitiated)
//   ENV > 0:                                ecore_count = min(N, num_threads-1)
//
// Linux: gnfs::util::set_current_thread_qos is a no-op (kernel scheduler free),
// so this helper's effect is macOS-only. The helper itself remains portable.

#include <cstdint>
#include <cstdlib>

#include "../util/thread_pool.hpp"  // For QoSClass + set_current_thread_qos

namespace gnfs::sieve {

// Resolve GNFS_SIEVE_ECORE_THREADS env to a thread count under the cap
// (num_threads - 1) — always keep at least one P-core thread for master work.
//
//   env == nullptr or "" → 0 (current behavior, all threads UserInitiated)
//   env > 0   → min(env, num_threads - 1)
//   env <= 0  → 0  (treat negative / atoi-failure as opt-out)
inline size_t resolve_ecore_thread_count(size_t num_threads,
                                          const char* env) noexcept {
    if (!env || env[0] == '\0') return 0;
    int v = std::atoi(env);
    if (v <= 0) return 0;
    if (num_threads <= 1) return 0;
    if (static_cast<size_t>(v) >= num_threads) {
        return num_threads - 1;
    }
    return static_cast<size_t>(v);
}

// Convenience overload reading GNFS_SIEVE_ECORE_THREADS directly.
inline size_t resolve_ecore_thread_count(size_t num_threads) noexcept {
    return resolve_ecore_thread_count(
        num_threads, std::getenv("GNFS_SIEVE_ECORE_THREADS"));
}

// Decide per-thread QoS class given (thread_idx, num_threads, ecore_count).
// First (num_threads - ecore_count) threads keep UserInitiated (P-core hint).
// Last ecore_count threads → Utility (E-core hint).
inline gnfs::util::QoSClass qos_for_sieve_thread(size_t thread_idx,
                                                  size_t num_threads,
                                                  size_t ecore_count) noexcept {
    if (ecore_count == 0 || thread_idx + ecore_count < num_threads) {
        return gnfs::util::QoSClass::UserInitiated;
    }
    return gnfs::util::QoSClass::Utility;
}

}  // namespace gnfs::sieve

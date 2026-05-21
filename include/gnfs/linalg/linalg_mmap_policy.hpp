#pragma once

// Linear-algebra mmap policy parser for Phase 5 (`GNFS_LINALG_MMAP` ENV).
//
// Three-state ENV identical in shape to GNFS_CASCADE_V3 (see CLAUDE.md):
//   unset / "0" / "off" / "false"        → Off
//   "1" / "on" / "true"                  → On  (force MmapCSRMatrix route)
//   "auto"                               → Auto (let nnz heuristic decide)
//
// The pipeline maps Auto → mmap when the projected on-disk size exceeds
// `auto_threshold_bytes`. Default threshold = 2 GiB nnz·sizeof(uint32_t)
// (≈ 500M nnz), chosen so 50d-class matrices (~10-50M nnz) keep using
// the in-memory CSR path while real 60d+ matrices migrate to disk.

#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace gnfs::linalg {

enum class MmapPolicy : std::uint8_t { Off, On, Auto };

inline MmapPolicy parse_mmap_policy(const char* env_value) noexcept {
    if (env_value == nullptr) return MmapPolicy::Off;
    const std::string_view s = env_value;
    if (s.empty()) return MmapPolicy::Off;
    if (s == "0" || s == "off" || s == "false") return MmapPolicy::Off;
    if (s == "1" || s == "on" || s == "true")   return MmapPolicy::On;
    if (s == "auto" || s == "AUTO" || s == "Auto") return MmapPolicy::Auto;
    return MmapPolicy::Off;  // unknown → safe default
}

inline MmapPolicy linalg_mmap_policy_from_env() noexcept {
    return parse_mmap_policy(std::getenv("GNFS_LINALG_MMAP"));
}

/// Default Auto-mode threshold: 2 GiB of nnz·uint32_t (col indices alone).
/// Tunable via ENV `GNFS_LINALG_MMAP_THRESHOLD_BYTES` (decimal byte count).
inline std::uint64_t linalg_mmap_threshold_bytes() noexcept {
    constexpr std::uint64_t DEFAULT_THRESHOLD = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    const char* env = std::getenv("GNFS_LINALG_MMAP_THRESHOLD_BYTES");
    if (env == nullptr || *env == '\0') return DEFAULT_THRESHOLD;
    // strtoull returns 0 on parse failure; treat that as "use default".
    const std::uint64_t v = std::strtoull(env, nullptr, 10);
    return v == 0 ? DEFAULT_THRESHOLD : v;
}

/// Returns true iff Phase 5 should flip the matrix to disk-resident.
/// `csr_nnz` is the SGE-reduced matrix non-zero count (the SpMV operand
/// the pipeline is about to hand to BW).
inline bool should_use_mmap(MmapPolicy policy, std::uint64_t csr_nnz) noexcept {
    switch (policy) {
        case MmapPolicy::Off:  return false;
        case MmapPolicy::On:   return true;
        case MmapPolicy::Auto: {
            // col_indices alone = nnz * 4 bytes; add row_offsets (≈ rows * 8)
            // is negligible compared to col_indices once nnz > ~few-M.
            const std::uint64_t projected_bytes = csr_nnz * sizeof(std::uint32_t);
            return projected_bytes >= linalg_mmap_threshold_bytes();
        }
    }
    return false;
}

} // namespace gnfs::linalg

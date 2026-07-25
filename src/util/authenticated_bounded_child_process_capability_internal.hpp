#pragma once

// Source-private compile-time contract for the authenticated Linux child
// transport. A supported result is a positive grant: every required libc and
// kernel-header surface must be present. Unknown or incomplete targets remain
// fail closed.

#include <cstdint>
#include <string_view>

#if defined(__linux__)
#include <fcntl.h>
#include <features.h>
#include <linux/prctl.h>
#include <sys/syscall.h>
#endif

#if defined(__linux__)
#define GNFS_AUTHENTICATED_CHILD_FACT_LINUX 1
#else
#define GNFS_AUTHENTICATED_CHILD_FACT_LINUX 0
#endif

#if defined(__linux__) && defined(__GLIBC__)
#define GNFS_AUTHENTICATED_CHILD_FACT_GLIBC 1
#else
#define GNFS_AUTHENTICATED_CHILD_FACT_GLIBC 0
#endif

#if defined(__linux__) && defined(__GLIBC_PREREQ)
#define GNFS_AUTHENTICATED_CHILD_FACT_GLIBC_PREREQ 1
#else
#define GNFS_AUTHENTICATED_CHILD_FACT_GLIBC_PREREQ 0
#endif

// Keep the function-like glibc macro in a nested directive. Mentioning an
// undefined function-like macro in a compound #if is rejected by musl's
// preprocessor before boolean short-circuiting can apply.
#if GNFS_AUTHENTICATED_CHILD_FACT_GLIBC_PREREQ
#if __GLIBC_PREREQ(2, 34)
#define GNFS_AUTHENTICATED_CHILD_FACT_GLIBC_2_34 1
#else
#define GNFS_AUTHENTICATED_CHILD_FACT_GLIBC_2_34 0
#endif
#else
#define GNFS_AUTHENTICATED_CHILD_FACT_GLIBC_2_34 0
#endif

#if defined(__linux__) && defined(SYS_memfd_create)
#define GNFS_AUTHENTICATED_CHILD_FACT_MEMFD_CREATE 1
#else
#define GNFS_AUTHENTICATED_CHILD_FACT_MEMFD_CREATE 0
#endif

#if defined(__linux__) && defined(SYS_execveat)
#define GNFS_AUTHENTICATED_CHILD_FACT_EXECVEAT 1
#else
#define GNFS_AUTHENTICATED_CHILD_FACT_EXECVEAT 0
#endif

#if defined(__linux__) && defined(SYS_close_range)
#define GNFS_AUTHENTICATED_CHILD_FACT_CLOSE_RANGE 1
#else
#define GNFS_AUTHENTICATED_CHILD_FACT_CLOSE_RANGE 0
#endif

#if defined(__linux__) && defined(SYS_prctl)
#define GNFS_AUTHENTICATED_CHILD_FACT_PRCTL 1
#else
#define GNFS_AUTHENTICATED_CHILD_FACT_PRCTL 0
#endif

#if defined(__linux__) && defined(SYS_getppid)
#define GNFS_AUTHENTICATED_CHILD_FACT_GETPPID 1
#else
#define GNFS_AUTHENTICATED_CHILD_FACT_GETPPID 0
#endif

#if defined(__linux__) && defined(PR_SET_PDEATHSIG)
#define GNFS_AUTHENTICATED_CHILD_FACT_PARENT_DEATH_SIGNAL 1
#else
#define GNFS_AUTHENTICATED_CHILD_FACT_PARENT_DEATH_SIGNAL 0
#endif

#if defined(__linux__) && defined(F_ADD_SEALS) && defined(F_GET_SEALS) && defined(F_SEAL_WRITE) && \
    defined(F_SEAL_GROW) && defined(F_SEAL_SHRINK) && defined(F_SEAL_SEAL)
#define GNFS_AUTHENTICATED_CHILD_FACT_SEALING_API 1
#else
#define GNFS_AUTHENTICATED_CHILD_FACT_SEALING_API 0
#endif

#if defined(__linux__) && defined(AT_EMPTY_PATH)
#define GNFS_AUTHENTICATED_CHILD_FACT_AT_EMPTY_PATH 1
#else
#define GNFS_AUTHENTICATED_CHILD_FACT_AT_EMPTY_PATH 0
#endif

// This macro is intentionally always defined to the literal 0 or 1 so it can
// safely guard declarations such as glibc's _Fork that cannot be parsed on an
// unsupported libc.
#if GNFS_AUTHENTICATED_CHILD_FACT_LINUX && GNFS_AUTHENTICATED_CHILD_FACT_GLIBC &&                  \
    GNFS_AUTHENTICATED_CHILD_FACT_GLIBC_PREREQ && GNFS_AUTHENTICATED_CHILD_FACT_GLIBC_2_34 &&      \
    GNFS_AUTHENTICATED_CHILD_FACT_MEMFD_CREATE && GNFS_AUTHENTICATED_CHILD_FACT_EXECVEAT &&        \
    GNFS_AUTHENTICATED_CHILD_FACT_CLOSE_RANGE && GNFS_AUTHENTICATED_CHILD_FACT_PRCTL &&            \
    GNFS_AUTHENTICATED_CHILD_FACT_GETPPID && GNFS_AUTHENTICATED_CHILD_FACT_PARENT_DEATH_SIGNAL &&  \
    GNFS_AUTHENTICATED_CHILD_FACT_SEALING_API && GNFS_AUTHENTICATED_CHILD_FACT_AT_EMPTY_PATH
#define GNFS_AUTHENTICATED_BOUNDED_CHILD_COMPILE_CAPABLE 1
#else
#define GNFS_AUTHENTICATED_BOUNDED_CHILD_COMPILE_CAPABLE 0
#endif

namespace gnfs::util::authenticated_bounded_child_capability_detail {

struct CompileFacts final {
    bool linux_target = false;
    bool glibc = false;
    bool glibc_version_predicate = false;
    bool glibc_at_least_2_34 = false;
    bool memfd_create = false;
    bool execveat = false;
    bool close_range = false;
    bool prctl = false;
    bool getppid = false;
    bool parent_death_signal = false;
    bool sealing_api = false;
    bool at_empty_path = false;
};

enum class CompileCapabilityReason : std::uint8_t {
    supported,
    unsupported_platform,
    unsupported_libc,
    missing_glibc_version_predicate,
    glibc_too_old,
    missing_memfd_create,
    missing_execveat,
    missing_close_range,
    missing_prctl,
    missing_getppid,
    missing_parent_death_signal,
    missing_sealing_api,
    missing_at_empty_path,
};

[[nodiscard]] constexpr std::string_view
compile_capability_reason_name(CompileCapabilityReason reason) noexcept {
    switch (reason) {
    case CompileCapabilityReason::supported:
        return "supported";
    case CompileCapabilityReason::unsupported_platform:
        return "unsupported_platform";
    case CompileCapabilityReason::unsupported_libc:
        return "unsupported_libc";
    case CompileCapabilityReason::missing_glibc_version_predicate:
        return "missing_glibc_version_predicate";
    case CompileCapabilityReason::glibc_too_old:
        return "glibc_too_old";
    case CompileCapabilityReason::missing_memfd_create:
        return "missing_memfd_create";
    case CompileCapabilityReason::missing_execveat:
        return "missing_execveat";
    case CompileCapabilityReason::missing_close_range:
        return "missing_close_range";
    case CompileCapabilityReason::missing_prctl:
        return "missing_prctl";
    case CompileCapabilityReason::missing_getppid:
        return "missing_getppid";
    case CompileCapabilityReason::missing_parent_death_signal:
        return "missing_parent_death_signal";
    case CompileCapabilityReason::missing_sealing_api:
        return "missing_sealing_api";
    case CompileCapabilityReason::missing_at_empty_path:
        return "missing_at_empty_path";
    }
    return "unknown";
}

[[nodiscard]] constexpr CompileCapabilityReason
classify_compile_capability(const CompileFacts& facts) noexcept {
    if (!facts.linux_target) {
        return CompileCapabilityReason::unsupported_platform;
    }
    if (!facts.glibc) {
        return CompileCapabilityReason::unsupported_libc;
    }
    if (!facts.glibc_version_predicate) {
        return CompileCapabilityReason::missing_glibc_version_predicate;
    }
    if (!facts.glibc_at_least_2_34) {
        return CompileCapabilityReason::glibc_too_old;
    }
    if (!facts.memfd_create) {
        return CompileCapabilityReason::missing_memfd_create;
    }
    if (!facts.execveat) {
        return CompileCapabilityReason::missing_execveat;
    }
    if (!facts.close_range) {
        return CompileCapabilityReason::missing_close_range;
    }
    if (!facts.prctl) {
        return CompileCapabilityReason::missing_prctl;
    }
    if (!facts.getppid) {
        return CompileCapabilityReason::missing_getppid;
    }
    if (!facts.parent_death_signal) {
        return CompileCapabilityReason::missing_parent_death_signal;
    }
    if (!facts.sealing_api) {
        return CompileCapabilityReason::missing_sealing_api;
    }
    if (!facts.at_empty_path) {
        return CompileCapabilityReason::missing_at_empty_path;
    }
    return CompileCapabilityReason::supported;
}

inline constexpr CompileFacts current_compile_facts{
    .linux_target = GNFS_AUTHENTICATED_CHILD_FACT_LINUX != 0,
    .glibc = GNFS_AUTHENTICATED_CHILD_FACT_GLIBC != 0,
    .glibc_version_predicate = GNFS_AUTHENTICATED_CHILD_FACT_GLIBC_PREREQ != 0,
    .glibc_at_least_2_34 = GNFS_AUTHENTICATED_CHILD_FACT_GLIBC_2_34 != 0,
    .memfd_create = GNFS_AUTHENTICATED_CHILD_FACT_MEMFD_CREATE != 0,
    .execveat = GNFS_AUTHENTICATED_CHILD_FACT_EXECVEAT != 0,
    .close_range = GNFS_AUTHENTICATED_CHILD_FACT_CLOSE_RANGE != 0,
    .prctl = GNFS_AUTHENTICATED_CHILD_FACT_PRCTL != 0,
    .getppid = GNFS_AUTHENTICATED_CHILD_FACT_GETPPID != 0,
    .parent_death_signal = GNFS_AUTHENTICATED_CHILD_FACT_PARENT_DEATH_SIGNAL != 0,
    .sealing_api = GNFS_AUTHENTICATED_CHILD_FACT_SEALING_API != 0,
    .at_empty_path = GNFS_AUTHENTICATED_CHILD_FACT_AT_EMPTY_PATH != 0,
};

inline constexpr CompileCapabilityReason current_compile_capability =
    classify_compile_capability(current_compile_facts);
inline constexpr bool compile_capable =
    current_compile_capability == CompileCapabilityReason::supported;

static_assert(compile_capable == (GNFS_AUTHENTICATED_BOUNDED_CHILD_COMPILE_CAPABLE != 0));

} // namespace gnfs::util::authenticated_bounded_child_capability_detail

#undef GNFS_AUTHENTICATED_CHILD_FACT_LINUX
#undef GNFS_AUTHENTICATED_CHILD_FACT_GLIBC
#undef GNFS_AUTHENTICATED_CHILD_FACT_GLIBC_PREREQ
#undef GNFS_AUTHENTICATED_CHILD_FACT_GLIBC_2_34
#undef GNFS_AUTHENTICATED_CHILD_FACT_MEMFD_CREATE
#undef GNFS_AUTHENTICATED_CHILD_FACT_EXECVEAT
#undef GNFS_AUTHENTICATED_CHILD_FACT_CLOSE_RANGE
#undef GNFS_AUTHENTICATED_CHILD_FACT_PRCTL
#undef GNFS_AUTHENTICATED_CHILD_FACT_GETPPID
#undef GNFS_AUTHENTICATED_CHILD_FACT_PARENT_DEATH_SIGNAL
#undef GNFS_AUTHENTICATED_CHILD_FACT_SEALING_API
#undef GNFS_AUTHENTICATED_CHILD_FACT_AT_EMPTY_PATH

cmake_minimum_required(VERSION 3.20)

project(GNFS
    VERSION 0.1.0
    DESCRIPTION "Industrial-grade General Number Field Sieve implementation"
    LANGUAGES CXX
)

# ============================================================
# 编译选项
# ============================================================

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译警告
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    option(GNFS_ENABLE_NATIVE_ARCH "Enable native CPU tuning flags in optimized builds" ON)
    # 优化:
    #   - Apple Silicon: -mcpu=native (隐含 -march)
    #   - Linux x86_64:  -march=native
    #   - Linux ARM64:   -mcpu=native (preferred over -march=native on ARM)
    #   - 其它:          omit (let toolchain default; avoid GitHub-hosted
    #                    runner CPU surprises producing illegal-instruction
    #                    binaries on differently-provisioned re-runs)
    if(NOT GNFS_ENABLE_NATIVE_ARCH)
        set(GNFS_NATIVE_FLAG "")
    elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(GNFS_NATIVE_FLAG -mcpu=native)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        set(GNFS_NATIVE_FLAG -march=native)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(GNFS_NATIVE_FLAG -mcpu=native)
    else()
        set(GNFS_NATIVE_FLAG "")
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3 ${GNFS_NATIVE_FLAG})
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND APPLE)
            add_compile_options(-flto=thin)
            add_link_options(-flto=thin)
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            add_compile_options(-flto=auto)
            add_link_options(-flto=auto)
        endif()
    elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        add_compile_options(-O2 -g ${GNFS_NATIVE_FLAG})
    endif()
elseif(MSVC)
    # /W4 = warning level 4 (project target); /permissive- = strict standards
    # conformance; /Zc:__cplusplus reports correct __cplusplus macro value
    # which C++20 feature-test macros depend on.
    # NOMINMAX prevents <windows.h> defining min/max macros that conflict
    # with std::min/std::max.
    # WIN32_LEAN_AND_MEAN trims rarely-used windows.h surface.
    # _CRT_SECURE_NO_WARNINGS silences C4996 deprecation for fopen/strcpy etc.
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8)
    add_compile_definitions(
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        _SILENCE_ALL_CXX23_DEPRECATION_WARNINGS
    )
    add_compile_options(/FI${CMAKE_CURRENT_SOURCE_DIR}/include/gnfs/util/msvc_compat.hpp)
    # MSVC does not implement -fsanitize=*; instrumentation is via /fsanitize=address
    # but only with cl 16.9+. Out of scope for this CI matrix.
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        # /O2 is the MSVC default for Release-style configs; /GL = whole-program
        # optimisation (corresponds to LTO). /Qpar = auto-parallelize trivial
        # loops. Caller can override.
        add_compile_options(/O2 /GL)
        add_link_options(/LTCG)
    endif()
endif()

# Sanitizer 选项
option(GNFS_ENABLE_ASAN  "Enable AddressSanitizer"        OFF)
option(GNFS_ENABLE_TSAN  "Enable ThreadSanitizer"         OFF)
option(GNFS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

if(GNFS_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
if(GNFS_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()
if(GNFS_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined)
    add_link_options(-fsanitize=undefined)
endif()

# ============================================================
# Profile-Guided Optimization (PGO)
# ============================================================
# 两阶段 PGO 工作流（详见 docs/perf/performance-doctrine.md §5.2）:
#   阶段 1 (训练): -DGNFS_ENABLE_PGO_GEN=ON, 跑训练样本生成 .profraw
#   阶段 2 (优化): -DGNFS_ENABLE_PGO_USE=ON, 消费 merged.profdata 重编
# 自动化入口: scripts/pgo-train.sh
option(GNFS_ENABLE_PGO_GEN "Enable PGO instrumentation (training run)"   OFF)
option(GNFS_ENABLE_PGO_USE "Enable PGO optimized build (consume profile)" OFF)
set(GNFS_PGO_PROFILE_DIR "${CMAKE_BINARY_DIR}/pgo-profiles"
    CACHE PATH "Directory for PGO .profraw / merged.profdata")

if(GNFS_ENABLE_PGO_GEN AND GNFS_ENABLE_PGO_USE)
    message(FATAL_ERROR
        "GNFS PGO: cannot enable both GNFS_ENABLE_PGO_GEN and GNFS_ENABLE_PGO_USE. "
        "Use separate build directories for the two phases.")
endif()

if(GNFS_ENABLE_PGO_GEN OR GNFS_ENABLE_PGO_USE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "GNFS PGO requires Clang (got ${CMAKE_CXX_COMPILER_ID})")
    endif()
endif()

if(GNFS_ENABLE_PGO_GEN)
    file(MAKE_DIRECTORY ${GNFS_PGO_PROFILE_DIR})
    add_compile_options(-fprofile-instr-generate)
    add_link_options(-fprofile-instr-generate)
    message(STATUS "GNFS PGO: instrumentation enabled, profiles -> ${GNFS_PGO_PROFILE_DIR}")
endif()

if(GNFS_ENABLE_PGO_USE)
    set(_pgo_data "${GNFS_PGO_PROFILE_DIR}/merged.profdata")
    if(NOT EXISTS ${_pgo_data})
        message(FATAL_ERROR
            "GNFS PGO_USE: profile not found at ${_pgo_data}. "
            "Run 'scripts/pgo-train.sh' first, or set GNFS_PGO_PROFILE_DIR to a populated dir.")
    endif()
    add_compile_options(-fprofile-instr-use=${_pgo_data})
    add_link_options(-fprofile-instr-use=${_pgo_data})
    # 防止小代码改动后强失败 — PGO 数据稍旧仍可用
    add_compile_options(-Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled)
    message(STATUS "GNFS PGO: consuming profile ${_pgo_data}")
endif()

# 导出编译命令（用于 IDE 和 clangd）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================
# 依赖查找
# ============================================================

# GMP - GNU Multiple Precision Arithmetic Library
# On Windows GMP is typically installed via vcpkg as "gmp" or via MPIR.
# When using vcpkg toolchain file, CONFIG-based find_package works for both.
# Falls back to plain find_library on POSIX where Homebrew/apt installs
# straight into prefix lib dirs.
if(MSVC OR DEFINED ENV{VCPKG_ROOT} OR DEFINED CMAKE_TOOLCHAIN_FILE)
    # Prefer vcpkg-style package first; many ports expose "GMP" or "gmp".
    find_package(unofficial-gmp CONFIG QUIET)
    if(TARGET unofficial::gmp::gmp)
        set(GMP_LIBRARY unofficial::gmp::gmp)
        get_target_property(GMP_INCLUDE_DIR unofficial::gmp::gmp
            INTERFACE_INCLUDE_DIRECTORIES)
        message(STATUS "Found GMP via vcpkg (unofficial-gmp)")
    else()
        find_library(GMP_LIBRARY NAMES gmp gmp-10 libgmp libgmp-10 REQUIRED)
        find_path(GMP_INCLUDE_DIR gmp.h REQUIRED)
    endif()
else()
    find_library(GMP_LIBRARY gmp REQUIRED)
    find_path(GMP_INCLUDE_DIR gmp.h REQUIRED)
endif()

# NTL - Number Theory Library (可选)
# NTL is harder to obtain on Windows; vcpkg has a port but it's experimental.
# Skip the heavy probing on MSVC unless explicitly requested.
if(NOT MSVC)
    find_library(NTL_LIBRARY ntl)
    find_path(NTL_INCLUDE_DIR NTL/ZZ.h)
endif()

if(NTL_LIBRARY AND NTL_INCLUDE_DIR)
    set(HAVE_NTL ON)
    message(STATUS "Found NTL: ${NTL_LIBRARY}")
else()
    set(HAVE_NTL OFF)
    message(STATUS "NTL not found, some features will be disabled")
endif()

# 线程
find_package(Threads REQUIRED)

# Metal (macOS only)
if(APPLE)
    find_library(METAL_FRAMEWORK Metal)
    find_library(FOUNDATION_FRAMEWORK Foundation)
    find_library(METALKIT_FRAMEWORK MetalKit)
    if(METAL_FRAMEWORK)
        set(HAVE_METAL ON)
        message(STATUS "Found Metal framework")
    endif()
endif()

# ============================================================
# 核心库
# ============================================================

# gnfs_util - 工具库
add_library(gnfs_util INTERFACE)
target_include_directories(gnfs_util INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# gnfs_core - 核心类型库
add_library(gnfs_core STATIC
    src/core/integer.cpp
    src/core/polynomial.cpp
    src/core/relation.cpp
)

# 添加可选的源文件（如果存在）
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/polynomial/base_m.cpp")
    target_sources(gnfs_core PRIVATE src/polynomial/base_m.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/polynomial/murphy_evaluator.cpp")
    target_sources(gnfs_core PRIVATE src/polynomial/murphy_evaluator.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/polynomial/kleinjung_selector.cpp")
    target_sources(gnfs_core PRIVATE src/polynomial/kleinjung_selector.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/factor_base/builder.cpp")
    target_sources(gnfs_core PRIVATE src/factor_base/builder.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/sieve/special_q.cpp")
    target_sources(gnfs_core PRIVATE src/sieve/special_q.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/sieve/lattice_sieve.cpp")
    target_sources(gnfs_core PRIVATE src/sieve/lattice_sieve.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/sieve/distributed_sieve.cpp")
    target_sources(gnfs_core PRIVATE src/sieve/distributed_sieve.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/cofactor/cofactorizer.cpp")
    target_sources(gnfs_core PRIVATE src/cofactor/cofactorizer.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/relation/collector.cpp")
    target_sources(gnfs_core PRIVATE src/relation/collector.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/relation/filter.cpp")
    target_sources(gnfs_core PRIVATE src/relation/filter.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/relation/structured_reduction.cpp")
    target_sources(gnfs_core PRIVATE src/relation/structured_reduction.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/relation/structured_batch.cpp")
    target_sources(gnfs_core PRIVATE src/relation/structured_batch.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/linalg/matrix_builder.cpp")
    target_sources(gnfs_core PRIVATE src/linalg/matrix_builder.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/linalg/block_lanczos.cpp")
    target_sources(gnfs_core PRIVATE src/linalg/block_lanczos.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/linalg/block_wiedemann.cpp")
    target_sources(gnfs_core PRIVATE src/linalg/block_wiedemann.cpp)
endif()

# Metal SpMV (macOS only). On other platforms compile the stub which
# returns false from is_available(), keeping the CPU fallback live.
if(HAVE_METAL)
    target_sources(gnfs_core PRIVATE src/linalg/metal_spmv.mm)
    # ObjC++ source compiled with the same -fno-objc-arc default; we
    # release-track manually inside the .mm to avoid imposing ARC on the
    # whole static library.
    set_source_files_properties(src/linalg/metal_spmv.mm
        PROPERTIES COMPILE_FLAGS "-fno-objc-arc -Wno-unused-parameter")
    target_link_libraries(gnfs_core PUBLIC
        ${METAL_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK}
    )
    target_compile_definitions(gnfs_core PUBLIC GNFS_HAVE_METAL=1)
else()
    target_sources(gnfs_core PRIVATE src/linalg/metal_spmv_stub.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/sqrt/rational_sqrt.cpp")
    target_sources(gnfs_core PRIVATE src/sqrt/rational_sqrt.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/sqrt/algebraic_sqrt.cpp")
    target_sources(gnfs_core PRIVATE src/sqrt/algebraic_sqrt.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/util/thread_pool.cpp")
    target_sources(gnfs_core PRIVATE src/util/thread_pool.cpp)
endif()

# API 层
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/api/pipeline.cpp")
    target_sources(gnfs_core PRIVATE src/api/pipeline.cpp)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/api/factorizer.cpp")
    target_sources(gnfs_core PRIVATE src/api/factorizer.cpp)
endif()

target_include_directories(gnfs_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(gnfs_core SYSTEM PUBLIC ${GMP_INCLUDE_DIR})
target_link_libraries(gnfs_core PUBLIC
    ${GMP_LIBRARY}
    gnfs_util
    Threads::Threads
)

if(HAVE_NTL)
    target_include_directories(gnfs_core SYSTEM PUBLIC ${NTL_INCLUDE_DIR})
    target_link_libraries(gnfs_core PUBLIC ${NTL_LIBRARY})
    target_compile_definitions(gnfs_core PUBLIC GNFS_HAVE_NTL=1)
endif()

# ============================================================
# 可执行文件
# ============================================================

# GNFS CLI 主程序
add_executable(gnfs
    src/cli/main.cpp
)
target_link_libraries(gnfs PRIVATE
    gnfs_core
    Threads::Threads
)

# ============================================================
# 测试
# ============================================================

option(GNFS_BUILD_TESTS "Build tests" ON)
option(GNFS_BUILD_FUZZERS "Build libFuzzer fuzz targets" OFF)

if(GNFS_BUILD_TESTS)
    enable_testing()

    find_program(BASH_EXECUTABLE bash)
    find_program(PYTHON_EXECUTABLE NAMES python3 python)
    if(BASH_EXECUTABLE AND PYTHON_EXECUTABLE)
        add_test(NAME HarnessHooks
            COMMAND ${CMAKE_COMMAND} -E env
                PYTHON=${PYTHON_EXECUTABLE}
                ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_harness_hooks.sh)
        set_tests_properties(HarnessHooks PROPERTIES
            LABELS "instant" TIMEOUT 10
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()

    # 简单测试程序
    add_executable(test_small_vector
        tests/test_small_vector.cpp
    )
    target_link_libraries(test_small_vector PRIVATE gnfs_util)
    add_test(NAME SmallVector COMMAND test_small_vector)
    set_tests_properties(SmallVector PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_integer
        tests/test_integer.cpp
    )
    target_link_libraries(test_integer PRIVATE gnfs_core)
    add_test(NAME Integer COMMAND test_integer)
    set_tests_properties(Integer PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_thread_pool
        tests/test_thread_pool.cpp
    )
    target_link_libraries(test_thread_pool PRIVATE gnfs_util Threads::Threads)
    add_test(NAME ThreadPool COMMAND test_thread_pool)
    set_tests_properties(ThreadPool PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_ordered_parallel_map
        tests/test_ordered_parallel_map.cpp
    )
    target_link_libraries(test_ordered_parallel_map PRIVATE gnfs_util Threads::Threads)
    add_test(NAME OrderedParallelMap COMMAND test_ordered_parallel_map)
    set_tests_properties(OrderedParallelMap PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_logger
        tests/test_logger.cpp
    )
    target_link_libraries(test_logger PRIVATE gnfs_util Threads::Threads)
    add_test(NAME Logger COMMAND test_logger)
    set_tests_properties(Logger PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_primes
        tests/test_primes.cpp
    )
    target_link_libraries(test_primes PRIVATE gnfs_util)
    add_test(NAME Primes COMMAND test_primes)
    set_tests_properties(Primes PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_timer
        tests/test_timer.cpp
    )
    target_link_libraries(test_timer PRIVATE gnfs_util)
    add_test(NAME UtilTimer COMMAND test_timer)
    set_tests_properties(UtilTimer PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_mmap_file
        tests/test_mmap_file.cpp
    )
    target_link_libraries(test_mmap_file PRIVATE gnfs_util)
    add_test(NAME MmapFile COMMAND test_mmap_file)
    set_tests_properties(MmapFile PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_resultant
        tests/test_resultant.cpp
    )
    target_link_libraries(test_resultant PRIVATE gnfs_core)
    add_test(NAME Resultant COMMAND test_resultant)
    set_tests_properties(Resultant PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_core_types
        tests/test_core_types.cpp
    )
    target_link_libraries(test_core_types PRIVATE gnfs_core)
    add_test(NAME CoreTypes COMMAND test_core_types)
    set_tests_properties(CoreTypes PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_safe_math
        tests/test_safe_math.cpp
    )
    target_link_libraries(test_safe_math PRIVATE gnfs_util)
    add_test(NAME SafeMath COMMAND test_safe_math)
    set_tests_properties(SafeMath PROPERTIES LABELS "instant" TIMEOUT 10)

    # Cross-compiler bit intrinsic + CPU hint wrappers (MSVC compatibility)
    add_executable(test_bit_intrin
        tests/test_bit_intrin.cpp
    )
    target_link_libraries(test_bit_intrin PRIVATE gnfs_util)
    add_test(NAME BitIntrin COMMAND test_bit_intrin)
    set_tests_properties(BitIntrin PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_ooc_policy
        tests/test_ooc_policy.cpp
    )
    target_link_libraries(test_ooc_policy PRIVATE gnfs_core)
    add_test(NAME OocPolicy COMMAND test_ooc_policy)
    set_tests_properties(OocPolicy PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_bw_rank_est
        tests/test_bw_rank_est.cpp
    )
    target_link_libraries(test_bw_rank_est PRIVATE gnfs_core)
    add_test(NAME BWRankEst COMMAND test_bw_rank_est)
    set_tests_properties(BWRankEst PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_matrix_diagnostics
        tests/test_matrix_diagnostics.cpp
    )
    target_link_libraries(test_matrix_diagnostics PRIVATE gnfs_core)
    add_test(NAME MatrixDiagnostics COMMAND test_matrix_diagnostics)
    set_tests_properties(MatrixDiagnostics PROPERTIES LABELS "instant" TIMEOUT 10)

    # SGE-OOC: streaming MatrixBuilder + SGE equivalence tests
    add_executable(test_sge_streaming
        tests/test_sge_streaming.cpp
    )
    target_link_libraries(test_sge_streaming PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SGEStreaming COMMAND test_sge_streaming)
    set_tests_properties(SGEStreaming PROPERTIES LABELS "instant" TIMEOUT 30)

    add_executable(test_v0_bfs_policy
        tests/test_v0_bfs_policy.cpp
    )
    target_link_libraries(test_v0_bfs_policy PRIVATE gnfs_core)
    add_test(NAME V0BfsPolicy COMMAND test_v0_bfs_policy)
    set_tests_properties(V0BfsPolicy PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_sieve_ecore_qos
        tests/test_sieve_ecore_qos.cpp
    )
    target_link_libraries(test_sieve_ecore_qos PRIVATE gnfs_core)
    add_test(NAME SieveEcoreQoS COMMAND test_sieve_ecore_qos)
    set_tests_properties(SieveEcoreQoS PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_factor_base
        tests/test_factor_base.cpp
    )
    target_link_libraries(test_factor_base PRIVATE gnfs_core Threads::Threads)
    add_test(NAME FactorBase COMMAND test_factor_base)
    set_tests_properties(FactorBase PROPERTIES LABELS "instant" TIMEOUT 30)

    add_executable(test_special_q
        tests/test_special_q.cpp
    )
    target_link_libraries(test_special_q PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SpecialQ COMMAND test_special_q)
    set_tests_properties(SpecialQ PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_lattice_sieve
        tests/test_lattice_sieve.cpp
    )
    target_link_libraries(test_lattice_sieve PRIVATE gnfs_core Threads::Threads)
    add_test(NAME LatticeSieve COMMAND test_lattice_sieve)
    set_tests_properties(LatticeSieve PROPERTIES LABELS "slow" TIMEOUT 300)

    add_executable(test_lll_lattice
        tests/test_lll_lattice.cpp
    )
    target_link_libraries(test_lll_lattice PRIVATE gnfs_core Threads::Threads)
    add_test(NAME LLLLattice COMMAND test_lll_lattice)
    set_tests_properties(LLLLattice PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_adaptive_lattice
        tests/test_adaptive_lattice.cpp
    )
    target_link_libraries(test_adaptive_lattice PRIVATE gnfs_core Threads::Threads)
    add_test(NAME AdaptiveLattice COMMAND test_adaptive_lattice)
    set_tests_properties(AdaptiveLattice PROPERTIES LABELS "instant" TIMEOUT 30)

    add_executable(test_relation_collector
        tests/test_relation_collector.cpp
    )
    target_link_libraries(test_relation_collector PRIVATE gnfs_core Threads::Threads)
    add_test(NAME RelationCollector COMMAND test_relation_collector)
    set_tests_properties(RelationCollector PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_ooc_store_integrity
        tests/test_ooc_store_integrity.cpp
    )
    target_link_libraries(test_ooc_store_integrity PRIVATE gnfs_core Threads::Threads)
    add_test(NAME OOCStoreIntegrity COMMAND test_ooc_store_integrity)
    set_tests_properties(OOCStoreIntegrity PROPERTIES LABELS "instant" TIMEOUT 10)

    # ── W6 T4: relation collector memory pool ──
    add_executable(test_memory_pool
        tests/test_memory_pool.cpp
    )
    target_link_libraries(test_memory_pool PRIVATE gnfs_core Threads::Threads)
    add_test(NAME MemoryPool COMMAND test_memory_pool)
    set_tests_properties(MemoryPool PROPERTIES LABELS "instant" TIMEOUT 30)

    add_executable(test_relation_pool_integration
        tests/test_relation_pool_integration.cpp
    )
    target_link_libraries(test_relation_pool_integration PRIVATE gnfs_core Threads::Threads)
    add_test(NAME RelationPoolIntegration COMMAND test_relation_pool_integration)
    set_tests_properties(RelationPoolIntegration PROPERTIES LABELS "instant" TIMEOUT 60)

    # ── W8 T5: Integer thread-local scratch pool ──
    add_executable(test_integer_scratch_pool
        tests/test_integer_scratch_pool.cpp
    )
    target_link_libraries(test_integer_scratch_pool PRIVATE gnfs_core Threads::Threads)
    add_test(NAME IntegerScratchPool COMMAND test_integer_scratch_pool)
    set_tests_properties(IntegerScratchPool PROPERTIES LABELS "instant" TIMEOUT 60)

    # ── W11 T3: GMP mpz_powm batch parallel dispatcher (GNFS_MPZ_POWM_BATCH_THREADS) ──
    add_executable(test_mpz_powm_parallel
        tests/test_mpz_powm_parallel.cpp
    )
    target_link_libraries(test_mpz_powm_parallel PRIVATE gnfs_core Threads::Threads)
    add_test(NAME MpzPowmParallel COMMAND test_mpz_powm_parallel)
    set_tests_properties(MpzPowmParallel PROPERTIES LABELS "instant" TIMEOUT 60)

    # ── W12 T3: GMP mpz_invert batch parallel dispatcher (GNFS_MPZ_INVERT_BATCH_THREADS) ──
    add_executable(test_mpz_invert_parallel
        tests/test_mpz_invert_parallel.cpp
    )
    target_link_libraries(test_mpz_invert_parallel PRIVATE gnfs_core Threads::Threads)
    add_test(NAME MpzInvertParallel COMMAND test_mpz_invert_parallel)
    set_tests_properties(MpzInvertParallel PROPERTIES LABELS "instant" TIMEOUT 60)

    # ── W13 T5: GMP mpz_mod batch parallel dispatcher (GNFS_MPZ_MOD_BATCH_THREADS) ──
    add_executable(test_mpz_mod_parallel
        tests/test_mpz_mod_parallel.cpp
    )
    target_link_libraries(test_mpz_mod_parallel PRIVATE gnfs_core Threads::Threads)
    add_test(NAME MpzModParallel COMMAND test_mpz_mod_parallel)
    set_tests_properties(MpzModParallel PROPERTIES LABELS "instant" TIMEOUT 60)

    # ── W14 T5: GMP mpz_gcd batch parallel dispatcher (GNFS_MPZ_GCD_BATCH_THREADS) ──
    add_executable(test_mpz_gcd_parallel
        tests/test_mpz_gcd_parallel.cpp
    )
    target_link_libraries(test_mpz_gcd_parallel PRIVATE gnfs_core Threads::Threads)
    add_test(NAME MpzGcdParallel COMMAND test_mpz_gcd_parallel)
    set_tests_properties(MpzGcdParallel PROPERTIES LABELS "instant" TIMEOUT 60)

    # ── W15 T5: GMP mpz_mul batch parallel dispatcher (GNFS_MPZ_MUL_BATCH_THREADS) ──
    add_executable(test_mpz_mul_parallel
        tests/test_mpz_mul_parallel.cpp
    )
    target_link_libraries(test_mpz_mul_parallel PRIVATE gnfs_core Threads::Threads)
    add_test(NAME MpzMulParallel COMMAND test_mpz_mul_parallel)
    set_tests_properties(MpzMulParallel PROPERTIES LABELS "instant" TIMEOUT 60)

    add_executable(test_sieve_basic
        tests/test_sieve_basic.cpp
    )
    target_link_libraries(test_sieve_basic PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SieveIntegration COMMAND test_sieve_basic)
    set_tests_properties(SieveIntegration PROPERTIES LABELS "fast" TIMEOUT 120)

    add_executable(test_sieve_checkpoint
        tests/test_sieve_checkpoint.cpp
    )
    target_link_libraries(test_sieve_checkpoint PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SieveCheckpoint COMMAND test_sieve_checkpoint)
    set_tests_properties(SieveCheckpoint PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_bl_checkpoint
        tests/test_bl_checkpoint.cpp
    )
    target_link_libraries(test_bl_checkpoint PRIVATE gnfs_core Threads::Threads)
    add_test(NAME BlockLanczosCheckpoint COMMAND test_bl_checkpoint)
    set_tests_properties(BlockLanczosCheckpoint PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_bl_resume_integration
        tests/test_bl_resume_integration.cpp
    )
    target_link_libraries(test_bl_resume_integration PRIVATE gnfs_core Threads::Threads)
    add_test(NAME BlockLanczosResumeIntegration COMMAND test_bl_resume_integration)
    set_tests_properties(BlockLanczosResumeIntegration PROPERTIES LABELS "fast" TIMEOUT 60)

    add_executable(test_distributed_sieve
        tests/test_distributed_sieve.cpp
    )
    target_link_libraries(test_distributed_sieve PRIVATE gnfs_core Threads::Threads)
    add_test(NAME DistributedSieve COMMAND test_distributed_sieve)
    set_tests_properties(DistributedSieve PROPERTIES LABELS "fast" TIMEOUT 180)

    add_executable(test_poly_checkpoint
        tests/test_poly_checkpoint.cpp
    )
    target_link_libraries(test_poly_checkpoint PRIVATE gnfs_core Threads::Threads)
    add_test(NAME PolyCheckpoint COMMAND test_poly_checkpoint)
    set_tests_properties(PolyCheckpoint PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_fb_checkpoint
        tests/test_fb_checkpoint.cpp
    )
    target_link_libraries(test_fb_checkpoint PRIVATE gnfs_core Threads::Threads)
    add_test(NAME FbCheckpoint COMMAND test_fb_checkpoint)
    set_tests_properties(FbCheckpoint PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_full_resume
        tests/test_full_resume.cpp
    )
    target_link_libraries(test_full_resume PRIVATE gnfs_core Threads::Threads)
    add_test(NAME FullResume COMMAND test_full_resume)
    set_tests_properties(FullResume PROPERTIES LABELS "slow" TIMEOUT 120)

    add_executable(test_krylov_sequence_mmap
        tests/test_krylov_sequence_mmap.cpp
    )
    target_link_libraries(test_krylov_sequence_mmap PRIVATE gnfs_core Threads::Threads)
    add_test(NAME KrylovSequenceMmap COMMAND test_krylov_sequence_mmap)
    set_tests_properties(KrylovSequenceMmap PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_bw_krylov_mmap_integration
        tests/test_bw_krylov_mmap_integration.cpp
    )
    target_link_libraries(test_bw_krylov_mmap_integration PRIVATE gnfs_core Threads::Threads)
    add_test(NAME BWKrylovMmapIntegration COMMAND test_bw_krylov_mmap_integration)
    set_tests_properties(BWKrylovMmapIntegration PROPERTIES LABELS "fast" TIMEOUT 60)

    add_executable(test_bw_krylov_parallel
        tests/test_bw_krylov_parallel.cpp
    )
    target_link_libraries(test_bw_krylov_parallel PRIVATE gnfs_core Threads::Threads)
    add_test(NAME BWKrylovParallel COMMAND test_bw_krylov_parallel)
    set_tests_properties(BWKrylovParallel PROPERTIES LABELS "fast" TIMEOUT 120)

    add_executable(test_krylov_compress
        tests/test_krylov_compress.cpp
    )
    target_link_libraries(test_krylov_compress PRIVATE gnfs_core Threads::Threads)
    add_test(NAME KrylovCompress COMMAND test_krylov_compress)
    set_tests_properties(KrylovCompress PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_krylov_compression
        tests/test_krylov_compression.cpp
    )
    target_link_libraries(test_krylov_compression PRIVATE gnfs_core Threads::Threads)
    add_test(NAME KrylovCompression COMMAND test_krylov_compression)
    set_tests_properties(KrylovCompression PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_cofactor
        tests/test_cofactor.cpp
    )
    target_link_libraries(test_cofactor PRIVATE gnfs_core Threads::Threads)
    add_test(NAME Cofactor COMMAND test_cofactor)
    set_tests_properties(Cofactor PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_trial_wheel
        tests/test_trial_wheel.cpp
    )
    target_link_libraries(test_trial_wheel PRIVATE gnfs_core Threads::Threads)
    add_test(NAME TrialWheel COMMAND test_trial_wheel)
    set_tests_properties(TrialWheel PROPERTIES LABELS "instant" TIMEOUT 10)

    # Micro-benchmark: not run by default test suites (label "bench")
    # Invoke manually:  ./build-release/test_trial_wheel_bench [count] [seed]
    add_executable(test_trial_wheel_bench
        tests/test_trial_wheel_bench.cpp
    )
    target_link_libraries(test_trial_wheel_bench PRIVATE gnfs_core Threads::Threads)
    add_test(NAME TrialWheelBench COMMAND test_trial_wheel_bench 100000)
    set_tests_properties(TrialWheelBench PROPERTIES LABELS "bench" TIMEOUT 120)

    add_executable(test_squfof
        tests/test_squfof.cpp
    )
    target_link_libraries(test_squfof PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SQUFOF COMMAND test_squfof)
    set_tests_properties(SQUFOF PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_brent_pollard_rho
        tests/test_brent_pollard_rho.cpp
    )
    target_link_libraries(test_brent_pollard_rho PRIVATE gnfs_core Threads::Threads)
    add_test(NAME BrentPollardRho COMMAND test_brent_pollard_rho)
    set_tests_properties(BrentPollardRho PROPERTIES LABELS "instant" TIMEOUT 60)

    add_executable(test_survival_predictor
        tests/test_survival_predictor.cpp
    )
    target_link_libraries(test_survival_predictor PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SurvivalPredictor COMMAND test_survival_predictor)
    set_tests_properties(SurvivalPredictor PROPERTIES LABELS "instant" TIMEOUT 30)

    add_executable(test_batch_ecm
        tests/test_batch_ecm.cpp
    )
    target_link_libraries(test_batch_ecm PRIVATE gnfs_core Threads::Threads)
    add_test(NAME BatchECM COMMAND test_batch_ecm)
    # Debug build runs ECM Stage 1/Stage 2 inside test_batch_vs_factor_success_parity,
    # which can take ~5-30s under sanitizers (mont_mul + mont_double inner loop slow).
    # Marked as fast (60s budget) to accommodate Debug+sanitizers.
    set_tests_properties(BatchECM PROPERTIES LABELS "fast" TIMEOUT 60)

    # Micro-benchmark: not run by default test suites (label "bench")
    # Invoke manually:  ./build-release/test_batch_ecm_bench [count] [seed]
    add_executable(test_batch_ecm_bench
        tests/test_batch_ecm_bench.cpp
    )
    target_link_libraries(test_batch_ecm_bench PRIVATE gnfs_core Threads::Threads)
    add_test(NAME BatchECMBench COMMAND test_batch_ecm_bench 500)
    set_tests_properties(BatchECMBench PROPERTIES LABELS "bench" TIMEOUT 300)

    # ECM Stage 3 (Brent-Suyama polynomial extension) -- unit tests
    add_executable(test_ecm_brent_suyama
        tests/test_ecm_brent_suyama.cpp
    )
    target_link_libraries(test_ecm_brent_suyama PRIVATE gnfs_core Threads::Threads)
    add_test(NAME EcmBrentSuyama COMMAND test_ecm_brent_suyama)
    set_tests_properties(EcmBrentSuyama PROPERTIES LABELS "instant" TIMEOUT 30)

    # ECM Stage 3 benchmark -- Brent-Suyama vs classical BSGS
    # Slow tier (60s) -- not run by smoke; run via `./scripts/test.sh module cofactor --slow`
    add_executable(test_ecm_brent_suyama_bench
        tests/test_ecm_brent_suyama_bench.cpp
    )
    target_link_libraries(test_ecm_brent_suyama_bench PRIVATE gnfs_core Threads::Threads)
    add_test(NAME EcmBrentSuyamaBench COMMAND test_ecm_brent_suyama_bench 100)
    set_tests_properties(EcmBrentSuyamaBench PROPERTIES LABELS "slow" TIMEOUT 120)

    add_executable(test_linalg
        tests/test_linalg.cpp
    )
    target_link_libraries(test_linalg PRIVATE gnfs_core Threads::Threads)
    add_test(NAME LinearAlgebra COMMAND test_linalg)
    set_tests_properties(LinearAlgebra PROPERTIES LABELS "instant" TIMEOUT 10)

    # SGE batch-pivot ENV gate + canonical-form parity tests.
    # Verifies GNFS_SGE_BATCH_PIVOTS=N produces equivalent SGE-reduced matrix
    # for N in {1, 2, 4, 8, 16, 32, 64}. See
    # include/gnfs/linalg/sge_batch_pivots.hpp for the equivalence invariant.
    add_executable(test_sge_batch_pivots
        tests/test_sge_batch_pivots.cpp
    )
    target_link_libraries(test_sge_batch_pivots PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SGEBatchPivots COMMAND test_sge_batch_pivots)
    set_tests_properties(SGEBatchPivots PROPERTIES LABELS "instant" TIMEOUT 60)

    add_executable(test_sqrt
        tests/test_sqrt.cpp
    )
    target_link_libraries(test_sqrt PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SquareRoot COMMAND test_sqrt)
    set_tests_properties(SquareRoot PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_gnfs_e2e
        tests/test_gnfs_e2e.cpp
    )
    target_link_libraries(test_gnfs_e2e PRIVATE gnfs_core Threads::Threads)
    add_test(NAME GNFS_E2E COMMAND test_gnfs_e2e)
    set_tests_properties(GNFS_E2E PROPERTIES LABELS "slow" TIMEOUT 600)

    # Kleinjung 多项式选择测试
    add_executable(test_murphy
        tests/test_murphy.cpp
    )
    target_link_libraries(test_murphy PRIVATE gnfs_core Threads::Threads)
    add_test(NAME Murphy COMMAND test_murphy)
    set_tests_properties(Murphy PROPERTIES LABELS "instant" TIMEOUT 10)

    add_executable(test_root_property_cache
        tests/test_root_property_cache.cpp
    )
    target_link_libraries(test_root_property_cache PRIVATE gnfs_core Threads::Threads)
    add_test(NAME RootPropertyCache COMMAND test_root_property_cache)
    set_tests_properties(RootPropertyCache PROPERTIES LABELS "instant" TIMEOUT 60)

    add_executable(test_rotation_incremental
        tests/test_rotation_incremental.cpp
    )
    target_link_libraries(test_rotation_incremental PRIVATE gnfs_core Threads::Threads)
    add_test(NAME RotationIncremental COMMAND test_rotation_incremental)
    set_tests_properties(RotationIncremental PROPERTIES LABELS "instant" TIMEOUT 30)

    add_executable(test_kleinjung
        tests/test_kleinjung.cpp
    )
    target_link_libraries(test_kleinjung PRIVATE gnfs_core Threads::Threads)
    add_test(NAME Kleinjung COMMAND test_kleinjung)
    set_tests_properties(Kleinjung PROPERTIES LABELS "slow" TIMEOUT 300)

    add_executable(test_kleinjung_large
        tests/test_kleinjung_large.cpp
    )
    target_link_libraries(test_kleinjung_large PRIVATE gnfs_core Threads::Threads)
    add_test(NAME KleinjungLarge COMMAND test_kleinjung_large)
    set_tests_properties(KleinjungLarge PROPERTIES LABELS "heavy" TIMEOUT 1800)

    add_executable(test_bai_brent_poly
        tests/test_bai_brent_poly.cpp
    )
    target_link_libraries(test_bai_brent_poly PRIVATE gnfs_core Threads::Threads)
    add_test(NAME BaiBrentPoly COMMAND test_bai_brent_poly)
    set_tests_properties(BaiBrentPoly PROPERTIES LABELS "fast" TIMEOUT 60)

    add_executable(test_bai_brent_benchmark
        tests/test_bai_brent_benchmark.cpp
    )
    target_link_libraries(test_bai_brent_benchmark PRIVATE gnfs_core Threads::Threads)
    # Not added to CTest -- standalone diagnostic, run manually for Murphy E A/B.

    add_executable(test_factor_with_kleinjung
        tests/test_factor_with_kleinjung.cpp
    )
    target_link_libraries(test_factor_with_kleinjung PRIVATE gnfs_core Threads::Threads)
    add_test(NAME FactorWithKleinjung COMMAND test_factor_with_kleinjung)
    # 27/40/50-bit semiprime end-to-end; routinely takes > 5 minutes on CI runners.
    set_tests_properties(FactorWithKleinjung PROPERTIES LABELS "slow" TIMEOUT 900)

    add_executable(test_sqrt_debug
        tests/test_sqrt_debug.cpp
    )
    target_link_libraries(test_sqrt_debug PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SqrtDebug COMMAND test_sqrt_debug)
    set_tests_properties(SqrtDebug PROPERTIES LABELS "instant" TIMEOUT 10)

    # Nguyen hybrid Hensel lift parallel dispatch (GNFS_SQRT_HENSEL_THREADS)
    add_executable(test_hensel_parallel
        tests/test_hensel_parallel.cpp
    )
    target_link_libraries(test_hensel_parallel PRIVATE gnfs_core Threads::Threads)
    add_test(NAME HenselParallel COMMAND test_hensel_parallel)
    set_tests_properties(HenselParallel PROPERTIES LABELS "instant" TIMEOUT 60)

    # Improved Couveignes for large class groups (2026-05-21)
    add_executable(test_couveignes_large_class_group
        tests/test_couveignes_large_class_group.cpp
    )
    target_link_libraries(test_couveignes_large_class_group PRIVATE gnfs_core Threads::Threads)
    add_test(NAME CouveignesLargeClassGroup COMMAND test_couveignes_large_class_group)
    set_tests_properties(CouveignesLargeClassGroup PROPERTIES LABELS "instant" TIMEOUT 60)

    # Couveignes sign-pattern parallel search (GNFS_COUVEIGNES_PARALLEL_THREADS)
    add_executable(test_couveignes_parallel
        tests/test_couveignes_parallel.cpp
    )
    target_link_libraries(test_couveignes_parallel PRIVATE gnfs_core Threads::Threads)
    add_test(NAME CouveignesParallel COMMAND test_couveignes_parallel)
    set_tests_properties(CouveignesParallel PROPERTIES LABELS "instant" TIMEOUT 60)

    # 渐进式测试 — 支持命令行参数选择级别
    add_executable(test_gnfs_progressive
        tests/test_gnfs_progressive.cpp
    )
    target_link_libraries(test_gnfs_progressive PRIVATE gnfs_core Threads::Threads)
    add_test(NAME GNFS_Progressive_L1 COMMAND test_gnfs_progressive 1 1)
    add_test(NAME GNFS_Progressive_L2 COMMAND test_gnfs_progressive 2 2)
    set_tests_properties(GNFS_Progressive_L1 PROPERTIES LABELS "fast" TIMEOUT 60)
    set_tests_properties(GNFS_Progressive_L2 PROPERTIES LABELS "fast" TIMEOUT 120)
    # Level 3+ 不加入 ctest — 手动运行: ./test_gnfs_progressive 3

    # 参数计算器测试
    add_executable(test_params
        tests/test_params.cpp
    )
    target_link_libraries(test_params PRIVATE gnfs_core Threads::Threads)
    add_test(NAME Params COMMAND test_params)
    set_tests_properties(Params PROPERTIES LABELS "instant" TIMEOUT 10)

    # 整系数多项式测试
    add_executable(test_int_polynomial
        tests/test_int_polynomial.cpp
    )
    target_link_libraries(test_int_polynomial PRIVATE gnfs_core Threads::Threads)
    add_test(NAME IntPolynomial COMMAND test_int_polynomial)
    set_tests_properties(IntPolynomial PROPERTIES LABELS "instant" TIMEOUT 10)

    # Polynomial Half-GCD (Knuth-Schönhage HGCD over F_p[x])
    add_executable(test_half_gcd
        tests/test_half_gcd.cpp
    )
    target_link_libraries(test_half_gcd PRIVATE gnfs_core Threads::Threads)
    add_test(NAME HalfGCD COMMAND test_half_gcd)
    set_tests_properties(HalfGCD PROPERTIES LABELS "instant" TIMEOUT 60)

    # Polynomial Karatsuba multiplication helper (sub-quadratic primitive
    # backing Half-GCD; ENV-gated threshold dispatch to schoolbook)
    add_executable(test_poly_karatsuba
        tests/test_poly_karatsuba.cpp
    )
    target_link_libraries(test_poly_karatsuba PRIVATE gnfs_core Threads::Threads)
    add_test(NAME PolyKaratsuba COMMAND test_poly_karatsuba)
    set_tests_properties(PolyKaratsuba PROPERTIES LABELS "instant" TIMEOUT 60)

    # Polynomial multi-point Horner batched evaluation SIMD helper
    # (NEON 2-lane / AVX2 4-lane; ENV-gated; bit-for-bit parity vs scalar)
    add_executable(test_horner_batch_simd
        tests/test_horner_batch_simd.cpp
    )
    target_link_libraries(test_horner_batch_simd PRIVATE gnfs_core)
    add_test(NAME HornerBatchSIMD COMMAND test_horner_batch_simd)
    set_tests_properties(HornerBatchSIMD PROPERTIES LABELS "instant" TIMEOUT 60)

    # Polynomial Newton-reciprocal divrem helper (sub-quadratic primitive,
    # bit-for-bit parity with schoolbook reference; ENV-gated three-state
    # dispatch with default-OFF Auto routing)
    add_executable(test_divrem_subquadratic
        tests/test_divrem_subquadratic.cpp
    )
    target_link_libraries(test_divrem_subquadratic PRIVATE gnfs_core Threads::Threads)
    add_test(NAME DivremSubquadratic COMMAND test_divrem_subquadratic)
    set_tests_properties(DivremSubquadratic PROPERTIES LABELS "instant" TIMEOUT 60)

    # Polynomial 3-prime NTT multiplication helper (O(n log n) primitive
    # via Schönhage-friendly NTT primes + Garner CRT reconstruction;
    # ENV-gated three-state dispatch, schoolbook parity invariant)
    add_executable(test_poly_ntt
        tests/test_poly_ntt.cpp
    )
    target_link_libraries(test_poly_ntt PRIVATE gnfs_core Threads::Threads)
    add_test(NAME PolyNTT COMMAND test_poly_ntt)
    set_tests_properties(PolyNTT PROPERTIES LABELS "instant" TIMEOUT 60)

    # Polynomial modular squaring helper (work-halved schoolbook square +
    # Karatsuba squaring above threshold; bit-for-bit parity with W9
    # karatsuba_mul_mod(a, a, p, out); ENV-gated three-state dispatch)
    add_executable(test_poly_square
        tests/test_poly_square.cpp
    )
    target_link_libraries(test_poly_square PRIVATE gnfs_core Threads::Threads)
    add_test(NAME PolySquare COMMAND test_poly_square)
    set_tests_properties(PolySquare PROPERTIES LABELS "instant" TIMEOUT 60)

    # F_p[x] coefficient batch add / sub mod-p SIMD helper (W14 T2):
    # NEON 4-lane + AVX2 8-lane wide add/sub with conditional reduce
    # against p. ENV-gated three-state dispatch (GNFS_POLY_ADD_MOD_SIMD).
    # SIMD-acceleration window p <= 2^31; falls back to scalar (uint64
    # widening) for larger p.
    add_executable(test_poly_add_mod_simd
        tests/test_poly_add_mod_simd.cpp
    )
    target_link_libraries(test_poly_add_mod_simd PRIVATE gnfs_core)
    add_test(NAME PolyAddModSimd COMMAND test_poly_add_mod_simd)
    set_tests_properties(PolyAddModSimd PROPERTIES LABELS "instant" TIMEOUT 60)

    # F_p[x] batched modular Horner evaluation SIMD helper (W15 T2):
    # NEON 2-lane + AVX2 4-lane consolidated loads of evaluation points
    # with inner uint64 mul-add-reduce per lane. Sibling of W10 T2 (int64
    # Horner) and W14 T2 (F_p[x] add/sub). ENV-gated three-state dispatch
    # (GNFS_POLY_HORNER_MOD_SIMD). SIMD-acceleration window p <= 2^31;
    # falls back to scalar reference for larger p (scalar path supports
    # the full p < 2^32 range via uint64 widening).
    add_executable(test_poly_horner_mod_simd
        tests/test_poly_horner_mod_simd.cpp
    )
    target_link_libraries(test_poly_horner_mod_simd PRIVATE gnfs_core)
    add_test(NAME PolyHornerModSimd COMMAND test_poly_horner_mod_simd)
    set_tests_properties(PolyHornerModSimd PROPERTIES LABELS "instant" TIMEOUT 60)

    # 关系过滤器测试
    add_executable(test_filter
        tests/test_filter.cpp
    )
    target_link_libraries(test_filter PRIVATE gnfs_core Threads::Threads)
    add_test(NAME Filter COMMAND test_filter)
    set_tests_properties(Filter PROPERTIES LABELS "instant" TIMEOUT 10)

    # Canonical full-width odd-large-prime contract and consumer agreement.
    add_executable(test_lp_key_contract
        tests/test_lp_key_contract.cpp
    )
    target_link_libraries(test_lp_key_contract PRIVATE gnfs_core Threads::Threads)
    add_test(NAME LPKeyContract COMMAND test_lp_key_contract)
    set_tests_properties(LPKeyContract PROPERTIES LABELS "instant" TIMEOUT 10)

    # Exact raw (a,b) and merged-source identity regressions.
    add_executable(test_relation_identity
        tests/test_relation_identity.cpp
    )
    target_link_libraries(test_relation_identity PRIVATE gnfs_core Threads::Threads)
    add_test(NAME RelationIdentity COMMAND test_relation_identity)
    set_tests_properties(RelationIdentity PROPERTIES LABELS "instant" TIMEOUT 10)

    # Shared pure-memory legacy reduction engine and explicit strategy contract.
    add_executable(test_relation_reduction_engine
        tests/test_relation_reduction_engine.cpp
    )
    target_link_libraries(test_relation_reduction_engine PRIVATE gnfs_core Threads::Threads)
    add_test(NAME RelationReductionEngine COMMAND test_relation_reduction_engine)
    set_tests_properties(RelationReductionEngine PROPERTIES LABELS "instant" TIMEOUT 10)

    # Sequential structured relation reduction and exact GF(2) oracle.
    add_executable(test_structured_filter
        tests/test_structured_filter.cpp
    )
    target_link_libraries(test_structured_filter PRIVATE gnfs_core Threads::Threads)
    add_test(NAME StructuredFilter COMMAND test_structured_filter)
    set_tests_properties(StructuredFilter PROPERTIES LABELS "instant" TIMEOUT 10)

    # Deterministic weight-[3,8] tree bases and exact dependency-space oracle.
    add_executable(test_structured_tree_basis
        tests/test_structured_tree_basis.cpp
    )
    target_link_libraries(test_structured_tree_basis PRIVATE gnfs_core Threads::Threads)
    add_test(NAME StructuredTreeBasis COMMAND test_structured_tree_basis)
    set_tests_properties(StructuredTreeBasis PROPERTIES LABELS "instant" TIMEOUT 10)

    # Exhaustive/property coverage for structured tree-basis planning.
    add_executable(test_structured_tree_basis_property
        tests/test_structured_tree_basis_property.cpp
    )
    target_link_libraries(test_structured_tree_basis_property PRIVATE gnfs_core Threads::Threads)
    add_test(NAME StructuredTreeBasisProperty COMMAND test_structured_tree_basis_property)
    set_tests_properties(StructuredTreeBasisProperty PROPERTIES LABELS "instant" TIMEOUT 10)

    # Budgeted vector-backed structured driver limits and stop semantics.
    add_executable(test_structured_budgeted_driver
        tests/test_structured_budgeted_driver.cpp
    )
    target_link_libraries(test_structured_budgeted_driver PRIVATE gnfs_core Threads::Threads)
    add_test(NAME StructuredBudgetedDriver COMMAND test_structured_budgeted_driver)
    set_tests_properties(StructuredBudgetedDriver PROPERTIES LABELS "instant" TIMEOUT 10)

    # Immutable-snapshot candidate ordering and conflict-free batch selection.
    add_executable(test_structured_conflict_batch
        tests/test_structured_conflict_batch.cpp
    )
    target_link_libraries(test_structured_conflict_batch PRIVATE gnfs_core Threads::Threads)
    add_test(NAME StructuredConflictBatch COMMAND test_structured_conflict_batch)
    set_tests_properties(StructuredConflictBatch PROPERTIES LABELS "instant" TIMEOUT 10)

    # Drain-all parallel materialization over one immutable conflict-free batch.
    add_executable(test_structured_parallel_prepare
        tests/test_structured_parallel_prepare.cpp
    )
    target_link_libraries(test_structured_parallel_prepare PRIVATE gnfs_core Threads::Threads)
    add_test(NAME StructuredParallelPrepare COMMAND test_structured_parallel_prepare)
    set_tests_properties(StructuredParallelPrepare PROPERTIES LABELS "instant" TIMEOUT 10)

    # Structured source-XOR materialization and persistence contracts.
    add_executable(test_structured_materialization
        tests/test_structured_materialization.cpp
    )
    target_link_libraries(test_structured_materialization PRIVATE gnfs_core Threads::Threads)
    add_test(NAME StructuredMaterialization COMMAND test_structured_materialization)
    set_tests_properties(StructuredMaterialization PROPERTIES LABELS "instant" TIMEOUT 10)

    # Phase 0 radix-sort ENV gate + bit-for-bit parity with std::sort path
    add_executable(test_filter_radix_sort
        tests/test_filter_radix_sort.cpp
    )
    target_link_libraries(test_filter_radix_sort PRIVATE gnfs_core Threads::Threads)
    add_test(NAME FilterRadixSort COMMAND test_filter_radix_sort)
    set_tests_properties(FilterRadixSort PROPERTIES LABELS "instant" TIMEOUT 60)

    # LP key Bloom pre-screen helper (W9 T5)
    add_executable(test_lp_bloom
        tests/test_lp_bloom.cpp
    )
    target_link_libraries(test_lp_bloom PRIVATE gnfs_core)
    add_test(NAME LPBloom COMMAND test_lp_bloom)
    set_tests_properties(LPBloom PROPERTIES LABELS "instant" TIMEOUT 60)

    # LP key splitmix64 hash mixing helper (W11 T5)
    add_executable(test_lp_key_hash
        tests/test_lp_key_hash.cpp
    )
    target_link_libraries(test_lp_key_hash PRIVATE gnfs_core)
    add_test(NAME LPKeyHash COMMAND test_lp_key_hash)
    set_tests_properties(LPKeyHash PROPERTIES LABELS "instant" TIMEOUT 60)

    # Partial relation merger worker-pool dispatcher (GNFS_FILTER_MERGE_THREADS)
    add_executable(test_merger_parallel
        tests/test_merger_parallel.cpp
    )
    target_link_libraries(test_merger_parallel PRIVATE gnfs_core Threads::Threads)
    add_test(NAME MergerParallel COMMAND test_merger_parallel)
    set_tests_properties(MergerParallel PROPERTIES LABELS "instant" TIMEOUT 60)

    # V3 Clique Merge backup (header-only, in include/gnfs/relation/clique_merger.hpp)
    add_executable(test_clique_merger
        tests/test_clique_merger.cpp
    )
    target_link_libraries(test_clique_merger PRIVATE gnfs_core Threads::Threads)
    add_test(NAME CliqueMerger COMMAND test_clique_merger)
    set_tests_properties(CliqueMerger PROPERTIES LABELS "instant" TIMEOUT 10)

    # V3 50d-like synthetic LP distribution test
    add_executable(test_clique_merger_50d_synthetic
        tests/test_clique_merger_50d_synthetic.cpp
    )
    target_link_libraries(test_clique_merger_50d_synthetic PRIVATE gnfs_core Threads::Threads)
    add_test(NAME CliqueMerger50dSynthetic COMMAND test_clique_merger_50d_synthetic)
    set_tests_properties(CliqueMerger50dSynthetic PROPERTIES LABELS "fast" TIMEOUT 60)

    # 3LP cofactor classification + decomposition (GNFS_3LP opt-in path)
    add_executable(test_3lp_cofactor
        tests/test_3lp_cofactor.cpp
    )
    target_link_libraries(test_3lp_cofactor PRIVATE gnfs_core Threads::Threads)
    add_test(NAME ThreeLpCofactor COMMAND test_3lp_cofactor)
    set_tests_properties(ThreeLpCofactor PROPERTIES LABELS "instant" TIMEOUT 30)

    # 3LP merger admission (V0 + V3 cascade BFS handle 3LP relations)
    add_executable(test_3lp_merge
        tests/test_3lp_merge.cpp
    )
    target_link_libraries(test_3lp_merge PRIVATE gnfs_core Threads::Threads)
    add_test(NAME ThreeLpMerge COMMAND test_3lp_merge)
    set_tests_properties(ThreeLpMerge PROPERTIES LABELS "instant" TIMEOUT 10)

    # Batch trial division (T5 Stage 1) — shared small-primes table sweep
    add_executable(test_batch_trial
        tests/test_batch_trial.cpp
    )
    target_link_libraries(test_batch_trial PRIVATE gnfs_core Threads::Threads)
    add_test(NAME BatchTrial COMMAND test_batch_trial)
    set_tests_properties(BatchTrial PROPERTIES LABELS "instant" TIMEOUT 20)

    # ECM Montgomery curve warm-pool (T5 Stage 2) — pre-generated Suyama curves
    add_executable(test_ecm_curve_pool
        tests/test_ecm_curve_pool.cpp
    )
    target_link_libraries(test_ecm_curve_pool PRIVATE gnfs_core Threads::Threads)
    add_test(NAME EcmCurvePool COMMAND test_ecm_curve_pool)
    set_tests_properties(EcmCurvePool PROPERTIES LABELS "instant" TIMEOUT 30)

    # ECM sigma seed warm pool (W10 T3, GNFS_ECM_SIGMA_POOL_SIZE)
    add_executable(test_sigma_seed_pool
        tests/test_sigma_seed_pool.cpp
    )
    target_link_libraries(test_sigma_seed_pool PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SigmaSeedPool COMMAND test_sigma_seed_pool)
    set_tests_properties(SigmaSeedPool PROPERTIES LABELS "instant" TIMEOUT 60)

    # ECM Stage 2 multi-curve parallel dispatcher (GNFS_ECM_STAGE2_PARALLEL)
    add_executable(test_ecm_stage2_parallel
        tests/test_ecm_stage2_parallel.cpp
    )
    target_link_libraries(test_ecm_stage2_parallel PRIVATE gnfs_core Threads::Threads)
    add_test(NAME EcmStage2Parallel COMMAND test_ecm_stage2_parallel)
    set_tests_properties(EcmStage2Parallel PROPERTIES LABELS "fast" TIMEOUT 60)

    # ECM Stage 1 multi-curve parallel dispatcher (GNFS_ECM_STAGE1_PARALLEL_THREADS)
    add_executable(test_ecm_stage1_parallel
        tests/test_ecm_stage1_parallel.cpp
    )
    target_link_libraries(test_ecm_stage1_parallel PRIVATE gnfs_core Threads::Threads)
    add_test(NAME EcmStage1Parallel COMMAND test_ecm_stage1_parallel)
    set_tests_properties(EcmStage1Parallel PROPERTIES LABELS "instant" TIMEOUT 60)

    # Lattice basis reduction multi-basis parallel dispatcher
    # (GNFS_LATTICE_BASIS_PARALLEL_THREADS)
    add_executable(test_lattice_basis_parallel
        tests/test_lattice_basis_parallel.cpp
    )
    target_link_libraries(test_lattice_basis_parallel PRIVATE gnfs_core Threads::Threads)
    add_test(NAME LatticeBasisParallel COMMAND test_lattice_basis_parallel)
    set_tests_properties(LatticeBasisParallel PROPERTIES LABELS "instant" TIMEOUT 60)

    # Sieve apply-tile parallel dispatcher
    # (GNFS_SIEVE_APPLY_TILE_THREADS). Orthogonal to W6
    # GNFS_SIEVE_REGION_TILE_BITS: that gate handles sequential
    # cache-blocking by tile size, this dispatcher handles parallel
    # work distribution across whatever tiles the caller produces.
    add_executable(test_sieve_apply_tile_parallel
        tests/test_sieve_apply_tile_parallel.cpp
    )
    target_link_libraries(test_sieve_apply_tile_parallel PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SieveApplyTileParallel COMMAND test_sieve_apply_tile_parallel)
    set_tests_properties(SieveApplyTileParallel PROPERTIES LABELS "instant" TIMEOUT 60)

    # Lattice coordinate SIMD batch helper (GNFS_LATTICE_COORDS_SIMD).
    # NEON 2-lane / AVX2 4-lane batched (i, j) -> (a, b) projection for
    # the lattice sieve. Helper-only future infrastructure; the bucket
    # sieve main loop does not currently call into it.
    add_executable(test_lattice_coords_simd
        tests/test_lattice_coords_simd.cpp
    )
    target_link_libraries(test_lattice_coords_simd PRIVATE gnfs_core)
    add_test(NAME LatticeCoordsSimd COMMAND test_lattice_coords_simd)
    set_tests_properties(LatticeCoordsSimd PROPERTIES LABELS "instant" TIMEOUT 60)

    # Sieve uint8_t threshold count SIMD helper
    # (GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD). NEON 16-lane / AVX2 32-lane
    # `count(values[i] >= threshold)` over a uint8 span — typical sieve
    # apply-phase candidate count primitive. Helper-only future
    # infrastructure; the bucket sieve main loop does not currently
    # call into it.
    add_executable(test_threshold_scan_simd
        tests/test_threshold_scan_simd.cpp
    )
    target_link_libraries(test_threshold_scan_simd PRIVATE gnfs_core)
    add_test(NAME ThresholdScanSimd COMMAND test_threshold_scan_simd)
    set_tests_properties(ThresholdScanSimd PROPERTIES LABELS "instant" TIMEOUT 60)

    # Sieve uint8_t saturated subtract SIMD helper
    # (GNFS_SIEVE_SATURATED_SUB_SIMD). NEON 16-lane `vqsubq_u8` /
    # AVX2 32-lane `_mm256_subs_epu8` in-place saturating subtract over
    # a uint8 span — typical sieve uniform-bias adjustment primitive
    # (`values[i] = max(0, int(values[i]) - bias)`). Helper-only
    # future infrastructure; the bucket sieve main loop does not
    # currently call into it.
    add_executable(test_saturated_sub_simd
        tests/test_saturated_sub_simd.cpp
    )
    target_link_libraries(test_saturated_sub_simd PRIVATE gnfs_core)
    add_test(NAME SaturatedSubSimd COMMAND test_saturated_sub_simd)
    set_tests_properties(SaturatedSubSimd PROPERTIES LABELS "instant" TIMEOUT 60)

    # Montgomery batch modular inversion helper (GNFS_ECM_BATCH_INV)
    add_executable(test_batch_inversion
        tests/test_batch_inversion.cpp
    )
    target_link_libraries(test_batch_inversion PRIVATE gnfs_core)
    add_test(NAME BatchInversion COMMAND test_batch_inversion)
    set_tests_properties(BatchInversion PROPERTIES LABELS "instant" TIMEOUT 60)

    # Trial division SIMD batch helper (GNFS_TRIAL_DIV_SIMD)
    add_executable(test_trial_div_simd
        tests/test_trial_div_simd.cpp
    )
    target_link_libraries(test_trial_div_simd PRIVATE gnfs_core)
    add_test(NAME TrialDivSimd COMMAND test_trial_div_simd)
    set_tests_properties(TrialDivSimd PROPERTIES LABELS "instant" TIMEOUT 60)

    # Cofactor per-stage timing telemetry helper (GNFS_COFACTOR_TIMING_ENABLE)
    add_executable(test_cofactor_stage_timing
        tests/test_cofactor_stage_timing.cpp
    )
    target_link_libraries(test_cofactor_stage_timing PRIVATE gnfs_core Threads::Threads)
    add_test(NAME CofactorStageTiming COMMAND test_cofactor_stage_timing)
    set_tests_properties(CofactorStageTiming PROPERTIES LABELS "instant" TIMEOUT 60)

    # ECM B1 prime-power expansion cache (W13 T3, GNFS_ECM_B1_CACHE_SIZE)
    add_executable(test_ecm_prime_cache
        tests/test_ecm_prime_cache.cpp
    )
    target_link_libraries(test_ecm_prime_cache PRIVATE gnfs_core Threads::Threads)
    add_test(NAME EcmPrimeCache COMMAND test_ecm_prime_cache)
    set_tests_properties(EcmPrimeCache PROPERTIES LABELS "instant" TIMEOUT 60)

    # Cofactor classification LRU result cache (W14 T3, GNFS_COFACTOR_RESULT_CACHE_SIZE)
    add_executable(test_cofactor_result_cache
        tests/test_cofactor_result_cache.cpp
    )
    target_link_libraries(test_cofactor_result_cache PRIVATE gnfs_core Threads::Threads)
    add_test(NAME CofactorResultCache COMMAND test_cofactor_result_cache)
    set_tests_properties(CofactorResultCache PROPERTIES LABELS "instant" TIMEOUT 60)

    # Brent-Pollard rho multi-config parallel dispatcher
    # (W15 T3, GNFS_BRENT_POLLARD_RHO_THREADS) — opt-in parallel fan-out
    # across independent (c, x0) rho configurations. Helper-only future
    # infrastructure; cofactorizer dispatch does not currently call into it.
    add_executable(test_brent_pollard_rho_parallel
        tests/test_brent_pollard_rho_parallel.cpp
    )
    target_link_libraries(test_brent_pollard_rho_parallel PRIVATE gnfs_core Threads::Threads)
    add_test(NAME BrentPollardRhoParallel COMMAND test_brent_pollard_rho_parallel)
    set_tests_properties(BrentPollardRhoParallel PROPERTIES LABELS "instant" TIMEOUT 60)

    # Cofactor batch micro-benchmark (T5 Stage 4) — invoke manually
    # ./build/test_cofactor_batch_bench [count] [seed]
    add_executable(test_cofactor_batch_bench
        tests/test_cofactor_batch_bench.cpp
    )
    target_link_libraries(test_cofactor_batch_bench PRIVATE gnfs_core Threads::Threads)
    add_test(NAME CofactorBatchBench COMMAND test_cofactor_batch_bench 1000)
    set_tests_properties(CofactorBatchBench PROPERTIES LABELS "bench" TIMEOUT 120)

    # 回归测试
    add_executable(test_regressions
        tests/test_regressions.cpp
    )
    target_link_libraries(test_regressions PRIVATE gnfs_core Threads::Threads)
    add_test(NAME Regressions COMMAND test_regressions)
    set_tests_properties(Regressions PROPERTIES LABELS "instant" TIMEOUT 10)

    # PolynomialContext 单元测试
    add_executable(test_polynomial_context
        tests/test_polynomial_context.cpp
    )
    target_link_libraries(test_polynomial_context PRIVATE gnfs_core Threads::Threads)
    add_test(NAME PolynomialContext COMMAND test_polynomial_context)
    set_tests_properties(PolynomialContext PROPERTIES LABELS "instant" TIMEOUT 10)

    # BaseMSelector 单元测试
    add_executable(test_base_m
        tests/test_base_m.cpp
    )
    target_link_libraries(test_base_m PRIVATE gnfs_core Threads::Threads)
    add_test(NAME BaseM COMMAND test_base_m)
    set_tests_properties(BaseM PROPERTIES LABELS "instant" TIMEOUT 10)

    # PolynomialOptimizer 单元测试
    add_executable(test_polynomial_optimizer
        tests/test_polynomial_optimizer.cpp
    )
    target_link_libraries(test_polynomial_optimizer PRIVATE gnfs_core Threads::Threads)
    add_test(NAME PolynomialOptimizer COMMAND test_polynomial_optimizer)
    set_tests_properties(PolynomialOptimizer PROPERTIES LABELS "instant" TIMEOUT 10)

    # ClassGroup 单元测试
    add_executable(test_class_group
        tests/test_class_group.cpp
    )
    target_link_libraries(test_class_group PRIVATE gnfs_core Threads::Threads)
    add_test(NAME ClassGroup COMMAND test_class_group)
    set_tests_properties(ClassGroup PROPERTIES LABELS "instant" TIMEOUT 10)

    # 边界/极端情况测试
    add_executable(test_edge_cases
        tests/test_edge_cases.cpp
    )
    target_link_libraries(test_edge_cases PRIVATE gnfs_core Threads::Threads)
    add_test(NAME EdgeCases COMMAND test_edge_cases)
    set_tests_properties(EdgeCases PROPERTIES LABELS "instant" TIMEOUT 10)

    # Schirokauer degree-4 diagnostic
    add_executable(test_schirokauer_deg4
        tests/test_schirokauer_deg4.cpp
    )
    target_link_libraries(test_schirokauer_deg4 PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SchirokaureDeg4 COMMAND test_schirokauer_deg4)
    set_tests_properties(SchirokaureDeg4 PROPERTIES LABELS "instant" TIMEOUT 10)

    # Schirokauer strip-ℓ math regression (instant)
    add_executable(test_schirokauer_strip
        tests/test_schirokauer_strip.cpp
    )
    target_link_libraries(test_schirokauer_strip PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SchirokauerStrip COMMAND test_schirokauer_strip)
    set_tests_properties(SchirokauerStrip PROPERTIES LABELS "instant" TIMEOUT 10)

    # Schirokauer per-relation parallel dispatch (GNFS_SCHIROKAUER_THREADS)
    add_executable(test_schirokauer_parallel
        tests/test_schirokauer_parallel.cpp
    )
    target_link_libraries(test_schirokauer_parallel PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SchirokauerParallel COMMAND test_schirokauer_parallel)
    set_tests_properties(SchirokauerParallel PROPERTIES LABELS "instant" TIMEOUT 60)

    # Factor base CZ root-finding per-prime parallel dispatcher (GNFS_FB_ROOTS_THREADS)
    add_executable(test_fb_roots_parallel
        tests/test_fb_roots_parallel.cpp
    )
    target_link_libraries(test_fb_roots_parallel PRIVATE gnfs_core Threads::Threads)
    add_test(NAME FbRootsParallel COMMAND test_fb_roots_parallel)
    set_tests_properties(FbRootsParallel PROPERTIES LABELS "instant" TIMEOUT 60)

    # 跨模块集成测试
    add_executable(test_integration
        tests/test_integration.cpp
    )
    target_link_libraries(test_integration PRIVATE gnfs_core Threads::Threads)
    add_test(NAME Integration COMMAND test_integration)
    set_tests_properties(Integration PROPERTIES LABELS "fast" TIMEOUT 120)

    # 25-digit 性能测试
    add_executable(test_25digit
        tests/test_25digit.cpp
    )
    target_link_libraries(test_25digit PRIVATE gnfs_core Threads::Threads)
    add_test(NAME 25Digit COMMAND test_25digit)
    set_tests_properties(25Digit PROPERTIES DISABLED TRUE
        LABELS "heavy" TIMEOUT 600)

    # Work-stealing ThreadPool 测试
    add_executable(test_work_stealing
        tests/test_work_stealing.cpp
    )
    target_link_libraries(test_work_stealing PRIVATE gnfs_util Threads::Threads)
    add_test(NAME WorkStealing COMMAND test_work_stealing)
    set_tests_properties(WorkStealing PROPERTIES LABELS "instant" TIMEOUT 10)

    # Bucket sieve 单元测试
    add_executable(test_bucket_sieve
        tests/test_bucket_sieve.cpp
    )
    target_link_libraries(test_bucket_sieve PRIVATE gnfs_core Threads::Threads)
    add_test(NAME BucketSieve COMMAND test_bucket_sieve)
    set_tests_properties(BucketSieve PROPERTIES LABELS "fast" TIMEOUT 120)

    # Sieve tiny-prime SIMD / loop-fusion parity + micro-bench
    add_executable(test_sieve_tiny_simd
        tests/test_sieve_tiny_simd.cpp
    )
    target_link_libraries(test_sieve_tiny_simd PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SieveTinySIMD COMMAND test_sieve_tiny_simd)
    set_tests_properties(SieveTinySIMD PROPERTIES LABELS "instant" TIMEOUT 10)

    # Bucket prefetch ENV gate + bit-for-bit parity tests
    add_executable(test_bucket_prefetch
        tests/test_bucket_prefetch.cpp
    )
    target_link_libraries(test_bucket_prefetch PRIVATE gnfs_core Threads::Threads)
    add_test(NAME BucketPrefetch COMMAND test_bucket_prefetch)
    set_tests_properties(BucketPrefetch PROPERTIES LABELS "fast" TIMEOUT 120)

    # Sieve row-tile ENV gate (`GNFS_SIEVE_REGION_TILE_BITS`). Header-only
    # helper, so the test exercises the cached env parser and clamp
    # behaviour in isolation; no sieve fixture needed at this stage.
    add_executable(test_sieve_region_tile
        tests/test_sieve_region_tile.cpp
    )
    target_link_libraries(test_sieve_region_tile PRIVATE gnfs_core)
    add_test(NAME SieveRegionTile COMMAND test_sieve_region_tile)
    set_tests_properties(SieveRegionTile PROPERTIES LABELS "instant" TIMEOUT 60)

    # Sieve norm-precompute row-tile ENV gate (`GNFS_SIEVE_NORM_TILE_BITS`).
    # Header-only helper complementary to the region_tile gate above; this
    # one tiles the norm precomputation phase rather than the apply scan.
    # No sieve fixture needed - the test exercises the cached env parser
    # and clamp behaviour in isolation, matching the region_tile harness.
    add_executable(test_sieve_norm_tile
        tests/test_sieve_norm_tile.cpp
    )
    target_link_libraries(test_sieve_norm_tile PRIVATE gnfs_core)
    add_test(NAME SieveNormTile COMMAND test_sieve_norm_tile)
    set_tests_properties(SieveNormTile PROPERTIES LABELS "instant" TIMEOUT 60)

    # Mmap CSR Matrix 单元测试
    add_executable(test_mmap_csr
        tests/test_mmap_csr.cpp
    )
    target_link_libraries(test_mmap_csr PRIVATE gnfs_core Threads::Threads)
    add_test(NAME MmapCSR COMMAND test_mmap_csr)
    set_tests_properties(MmapCSR PROPERTIES LABELS "instant" TIMEOUT 10)

    # MatrixView concept compile-time + run-time conformance tests.
    # Locks in CSRMatrix / MmapCSRMatrix accessor parity so SpMV
    # templated kernels can stay devirtualised.
    add_executable(test_matrix_view_concept
        tests/test_matrix_view_concept.cpp
    )
    target_link_libraries(test_matrix_view_concept PRIVATE gnfs_core Threads::Threads)
    add_test(NAME MatrixViewConcept COMMAND test_matrix_view_concept)
    set_tests_properties(MatrixViewConcept PROPERTIES LABELS "instant" TIMEOUT 10)

    # save_sparse_as_mmap helper (Phase 5 dual-path entrypoint).
    # Verifies the helper is byte-identical to the explicit
    # SparseMatrix → CSRMatrix → save → MmapCSRMatrix sequence.
    add_executable(test_save_sparse_as_mmap
        tests/test_save_sparse_as_mmap.cpp
    )
    target_link_libraries(test_save_sparse_as_mmap PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SaveSparseAsMmap COMMAND test_save_sparse_as_mmap)
    set_tests_properties(SaveSparseAsMmap PROPERTIES LABELS "instant" TIMEOUT 10)

    # GNFS_LINALG_MMAP three-state policy parser.
    add_executable(test_linalg_mmap_policy
        tests/test_linalg_mmap_policy.cpp
    )
    target_link_libraries(test_linalg_mmap_policy PRIVATE gnfs_core)
    add_test(NAME LinalgMmapPolicy COMMAND test_linalg_mmap_policy)
    set_tests_properties(LinalgMmapPolicy PROPERTIES LABELS "instant" TIMEOUT 10)

    # Metal SpMV (correctness) — runs on every platform. The test itself
    # short-circuits the "Metal unavailable" path on non-Apple/Linux CI,
    # so the binary stays green either way. Tagged "instant" so it joins
    # the smoke set.
    add_executable(test_metal_spmv
        tests/test_metal_spmv.cpp
    )
    target_link_libraries(test_metal_spmv PRIVATE gnfs_core Threads::Threads)
    add_test(NAME MetalSpMV COMMAND test_metal_spmv)
    set_tests_properties(MetalSpMV PROPERTIES LABELS "instant" TIMEOUT 30)

    # SIMD SpMV (correctness) — exercises the NEON / AVX2 inner kernels
    # added in feat(linalg) SIMD SpMV dispatch. The test compares the
    # SIMD helper output to the verbatim scalar kernel bit-for-bit on a
    # battery of matrices and edge cases. On hosts without NEON or AVX2
    # the SIMD helper transparently falls back to scalar, so the binary
    # stays green either way. Tagged "instant" so it joins the smoke set.
    add_executable(test_spmv_simd
        tests/test_spmv_simd.cpp
    )
    target_link_libraries(test_spmv_simd PRIVATE gnfs_core Threads::Threads)
    add_test(NAME LinalgSpmvSimd COMMAND test_spmv_simd)
    set_tests_properties(LinalgSpmvSimd PROPERTIES LABELS "instant" TIMEOUT 60)

    # Cache-blocked GF(2) matrix transpose helper (correctness).
    # Exercises the naive scalar reference path, the 64x64 tile-blocked
    # path, and the ENV-gated dispatcher across a battery of edge sizes
    # (empty, 1x1, 7x3, 63x127, 64x64, 128x64, 1000x1000, 500x800) plus
    # double-transpose round-trip identity. Header-only helper, no extra
    # link dependencies beyond the core library.
    add_executable(test_transpose_blocked
        tests/test_transpose_blocked.cpp
    )
    target_link_libraries(test_transpose_blocked PRIVATE gnfs_core)
    add_test(NAME LinalgTransposeBlocked COMMAND test_transpose_blocked)
    set_tests_properties(LinalgTransposeBlocked PROPERTIES LABELS "instant" TIMEOUT 60)

    # SIMD GF(2) word popcount batch helper (correctness).
    # Exercises the scalar reference, the NEON 2-lane / AVX2 4-lane batch
    # path, and the ENV-gated dispatcher across a battery of edge sizes
    # (empty, single word patterns, aligned/unaligned batches, 1M perf
    # probe). Header-only helper, no extra link dependencies beyond the
    # core library.
    add_executable(test_popcount_simd
        tests/test_popcount_simd.cpp
    )
    target_link_libraries(test_popcount_simd PRIVATE gnfs_core)
    add_test(NAME LinalgPopcountSimd COMMAND test_popcount_simd)
    set_tests_properties(LinalgPopcountSimd PROPERTIES LABELS "instant" TIMEOUT 60)

    # SIMD GF(2) batch "popcount(a & b)" helper (correctness).
    # AND-fused counterpart of test_popcount_simd: exercises the scalar
    # reference, the NEON 2-lane / AVX2 4-lane batch path, and the
    # ENV-gated dispatcher across a battery of edge sizes (empty,
    # single-word bit patterns, aligned/unaligned batches, 1M perf
    # probe). Useful for Block Lanczos / Block Wiedemann orthogonality
    # checks and GF(2) inner-product reductions. Header-only helper,
    # no extra link dependencies beyond the core library.
    add_executable(test_and_popcnt_simd
        tests/test_and_popcnt_simd.cpp
    )
    target_link_libraries(test_and_popcnt_simd PRIVATE gnfs_core)
    add_test(NAME AndPopcntSIMD COMMAND test_and_popcnt_simd)
    set_tests_properties(AndPopcntSIMD PROPERTIES LABELS "instant" TIMEOUT 60)

    # SIMD GF(2) batch row-word XOR helper (correctness).
    # Third SIMD primitive in the popcount / and-popcount family:
    # exercises the scalar reference (`dst[i] ^= src[i]` loop), the
    # NEON 2-lane (`veorq_u64`) / AVX2 4-lane (`_mm256_xor_si256`)
    # batch path, and the ENV-gated dispatcher across edge sizes
    # (empty, single-word patterns, aligned/unaligned batches, length
    # mismatch clamping, 1M perf probe). Useful for Block Lanczos /
    # Block Wiedemann row-block updates, Krylov vector recurrences,
    # and parity sweep hot sites. Header-only helper, no extra link
    # dependencies beyond the core library.
    add_executable(test_xor_words_simd
        tests/test_xor_words_simd.cpp
    )
    target_link_libraries(test_xor_words_simd PRIVATE gnfs_core)
    add_test(NAME RowXorSIMD COMMAND test_xor_words_simd)
    set_tests_properties(RowXorSIMD PROPERTIES LABELS "instant" TIMEOUT 60)

    # SIMD GF(2) batch word-AND helper (correctness, W13 T1).
    # Fourth SIMD primitive in the popcount / and-popcount / xor-words
    # family: exercises the scalar reference (`out[i] = a[i] & b[i]`),
    # the NEON 2-lane (`vandq_u64`) / AVX2 4-lane (`_mm256_and_si256`)
    # batch path, and the ENV-gated dispatcher across edge sizes
    # (empty, single-word patterns, aligned/unaligned batches, length
    # mismatch clamping, identity check via `a == b`, 1M perf probe).
    # Three-argument variant: AND of two `const uint64_t` spans into a
    # third mutable output span (cf. xor_words_simd.hpp's in-place
    # XOR and and_popcnt_simd.hpp's fused AND-then-popcount reduction).
    # Useful for Block Lanczos / Block Wiedemann mask applications,
    # GF(2) row intersection caches, and structured Gaussian elimination
    # active-column projections. Header-only helper, no extra link
    # dependencies beyond the core library.
    add_executable(test_and_words_simd
        tests/test_and_words_simd.cpp
    )
    target_link_libraries(test_and_words_simd PRIVATE gnfs_core)
    add_test(NAME AndWordsSIMD COMMAND test_and_words_simd)
    set_tests_properties(AndWordsSIMD PROPERTIES LABELS "instant" TIMEOUT 60)

    # SIMD GF(2) batch XOR-then-popcount helper (correctness, W14 T1).
    # Fifth SIMD primitive in the popcount / and-popcount / xor-words /
    # and-words family: exercises the scalar reference
    # (`__builtin_popcountll(a ^ b)`), the NEON 2-lane (`veorq_u64 +
    # vcntq_u8 + vaddv_u8`) / AVX2 4-lane (`_mm256_xor_si256 +
    # _mm256_popcnt_epi64` or `_mm_popcnt_u64` fallback) batch path,
    # and the ENV-gated dispatcher across edge sizes (empty,
    # single-word Hamming distance patterns, aligned/unaligned batches,
    # length mismatch clamping, a==b self-XOR zero identity, 1M perf
    # probe). Two reduction entry points: per-word `uint32_t` weights
    # and a `uint64_t` total Hamming distance. Counts differing bits
    # between two packed GF(2) word arrays (sibling of AND-popcount,
    # which counts shared bits). Useful for Block Lanczos / Block
    # Wiedemann dependency drift metrics, GF(2) basis vector distance
    # checks, and parity-difference reductions. Header-only helper, no
    # extra link dependencies beyond the core library.
    add_executable(test_xor_popcnt_simd
        tests/test_xor_popcnt_simd.cpp
    )
    target_link_libraries(test_xor_popcnt_simd PRIVATE gnfs_core)
    add_test(NAME XorPopcntSIMD COMMAND test_xor_popcnt_simd)
    set_tests_properties(XorPopcntSIMD PROPERTIES LABELS "instant" TIMEOUT 60)

    # GF(2) per-row popcount SIMD helper (W15 T1).
    # Per-row Hamming weight over a row-major packed GF(2) matrix laid
    # out as `row_count * row_words` contiguous uint64_t words. NEON
    # 2-word stride (ARM64) or AVX2 4-word stride (x86_64) per row with
    # scalar `__builtin_popcountll` tail. Sixth helper in the linalg
    # SIMD primitive family (joins W9 popcount / W10 and_popcnt / W11
    # xor_words / W13 T1 and_words / W14 T1 xor_popcnt). First family
    # member that respects a 2-D matrix layout rather than a flat span.
    # Useful for column-weight tallies, dependency vector parity,
    # sparsity profiling, and any caller that needs per-row weights as
    # a primitive. Header-only helper, no extra link dependencies
    # beyond the core library.
    add_executable(test_row_popcount_simd
        tests/test_row_popcount_simd.cpp
    )
    target_link_libraries(test_row_popcount_simd PRIVATE gnfs_core)
    add_test(NAME RowPopcountSIMD COMMAND test_row_popcount_simd)
    set_tests_properties(RowPopcountSIMD PROPERTIES LABELS "instant" TIMEOUT 60)

    # Linalg iteration progress telemetry helper (W12 T1).
    # ENV-gated, header-only logger that callers can opt-in wire into
    # Block Lanczos / Block Wiedemann SpMV loops to print iteration
    # progress (iter / total, elapsed wall, throughput rate, ETA) to
    # stderr. Default-off (GNFS_LINALG_PROGRESS_INTERVAL=0 / unset)
    # short-circuits all tick() / finish() calls to no-ops; no clock
    # samples, no string formatting, no stderr writes. Validates ENV
    # parsing matrix, disabled silence, enabled tick gating, finish()
    # DONE summary, multi-logger coexistence, total/iter clamping, rate
    # underflow handling, and move semantics. Header-only helper, no
    # extra link dependencies beyond the core library.
    add_executable(test_linalg_progress
        tests/test_linalg_progress.cpp
    )
    target_link_libraries(test_linalg_progress PRIVATE gnfs_core)
    add_test(NAME LinalgProgress COMMAND test_linalg_progress)
    set_tests_properties(LinalgProgress PROPERTIES LABELS "instant" TIMEOUT 60)

    # Out-of-core Relations 单元测试
    add_executable(test_ooc_relations
        tests/test_ooc_relations.cpp
    )
    target_link_libraries(test_ooc_relations PRIVATE gnfs_core Threads::Threads)
    add_test(NAME OOCRelations COMMAND test_ooc_relations)
    set_tests_properties(OOCRelations PROPERTIES LABELS "instant" TIMEOUT 10)

    # Block Wiedemann 单元测试
    add_executable(test_block_wiedemann
        tests/test_block_wiedemann.cpp
    )
    target_link_libraries(test_block_wiedemann PRIVATE gnfs_core Threads::Threads)
    add_test(NAME BlockWiedemann COMMAND test_block_wiedemann)
    set_tests_properties(BlockWiedemann PROPERTIES LABELS "fast" TIMEOUT 60)

    # 回归门禁测试 (10/15/20/25-digit full pipeline)
    add_executable(test_regression_gate
        tests/test_regression_gate.cpp
    )
    target_link_libraries(test_regression_gate PRIVATE gnfs_core Threads::Threads)
    add_test(NAME RegressionGate COMMAND test_regression_gate)
    set_tests_properties(RegressionGate PROPERTIES LABELS "gate" TIMEOUT 240)

    # API 层测试
    add_executable(test_api
        tests/test_api.cpp
    )
    target_link_libraries(test_api PRIVATE gnfs_core Threads::Threads)
    add_test(NAME API COMMAND test_api)
    set_tests_properties(API PROPERTIES LABELS "slow" TIMEOUT 300)

    # Method selection 测试
    add_executable(test_method_selection
        tests/test_method_selection.cpp
    )
    target_link_libraries(test_method_selection PRIVATE gnfs_core Threads::Threads)
    add_test(NAME MethodSelection COMMAND test_method_selection)
    set_tests_properties(MethodSelection PROPERTIES LABELS "instant" TIMEOUT 60)

    # i18n 测试
    add_executable(test_i18n
        tests/test_i18n.cpp
    )
    target_link_libraries(test_i18n PRIVATE gnfs_core Threads::Threads)
    add_test(NAME I18N COMMAND test_i18n)
    set_tests_properties(I18N PROPERTIES LABELS "instant" TIMEOUT 10)

    # SIQS (Self-Initializing Quadratic Sieve) 测试
    add_executable(test_siqs
        tests/test_siqs.cpp
    )
    target_link_libraries(test_siqs PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SIQS COMMAND test_siqs)
    set_tests_properties(SIQS PROPERTIES LABELS "fast" TIMEOUT 180)

    # SIQS 端到端：100-bit + 180-bit 实测分解
    add_executable(test_siqs_e2e
        tests/test_siqs_e2e.cpp
    )
    target_link_libraries(test_siqs_e2e PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SiqsE2E COMMAND test_siqs_e2e)
    set_tests_properties(SiqsE2E PROPERTIES LABELS "slow" TIMEOUT 700)

    # GNFS 流水线基准测试 (20-35 digit)
    add_executable(test_gnfs_bench
        tests/test_gnfs_bench.cpp
    )
    target_link_libraries(test_gnfs_bench PRIVATE gnfs_core Threads::Threads)
    # 不加入 ctest — 手动运行: ./test_gnfs_bench

    # 性能目标基准测试 (10-70 digit)
    add_executable(test_perf_targets
        tests/test_perf_targets.cpp
    )
    target_link_libraries(test_perf_targets PRIVATE gnfs_core Threads::Threads)
    # 不加入 ctest — 手动运行: ./test_perf_targets

    # ECM 快速诊断测试
    add_executable(test_ecm_quick
        tests/test_ecm_quick.cpp
    )
    target_link_libraries(test_ecm_quick PRIVATE gnfs_core Threads::Threads)

    # 压力测试 (50/80/100-digit)
    add_executable(test_stress
        tests/test_stress.cpp
    )
    target_link_libraries(test_stress PRIVATE gnfs_core Threads::Threads)
    add_test(NAME Stress COMMAND test_stress 1 1)
    set_tests_properties(Stress PROPERTIES DISABLED TRUE
        LABELS "stress" TIMEOUT 43200)

    # ────────────────────────────────────────────────────────────
    # Instrumentation / platform-aware test timeout scaling. Sanitizers and
    # coverage builds instrument hot integer paths heavily, and hosted MSVC
    # runners are slower on several polynomial-selection tests.
    # set_tests_properties TIMEOUT 是 hard limit (优先于 ctest --timeout), 因此
    # CI 下必须放大 TIMEOUT 否则 instant/fast 测试会被 spurious 超时.
    # ────────────────────────────────────────────────────────────
    set(_GNFS_TEST_TIMEOUT_MULT 1)
    if(CMAKE_CXX_FLAGS MATCHES "fsanitize=(address|thread|memory|undefined|leak)")
        set(_GNFS_TEST_TIMEOUT_MULT 10)
        message(STATUS "Sanitizer detected — multiplying test TIMEOUTs by ${_GNFS_TEST_TIMEOUT_MULT}")
    elseif(CMAKE_CXX_FLAGS MATCHES "(^|[ ;])-(-coverage|fprofile-arcs|ftest-coverage)" OR
           CMAKE_EXE_LINKER_FLAGS MATCHES "(^|[ ;])-(-coverage|fprofile-arcs|ftest-coverage)")
        set(_GNFS_TEST_TIMEOUT_MULT 10)
        message(STATUS "Coverage instrumentation detected — multiplying test TIMEOUTs by ${_GNFS_TEST_TIMEOUT_MULT}")
    endif()
    if(MSVC AND _GNFS_TEST_TIMEOUT_MULT LESS 3)
        set(_GNFS_TEST_TIMEOUT_MULT 3)
        message(STATUS "MSVC runner detected — multiplying test TIMEOUTs by ${_GNFS_TEST_TIMEOUT_MULT}")
    endif()
    if(_GNFS_TEST_TIMEOUT_MULT GREATER 1)
        get_property(_all_tests DIRECTORY PROPERTY TESTS)
        foreach(_tn ${_all_tests})
            get_test_property(${_tn} TIMEOUT _curr_to)
            if(_curr_to AND NOT _curr_to STREQUAL "NOTFOUND")
                math(EXPR _new_to "${_curr_to} * ${_GNFS_TEST_TIMEOUT_MULT}")
                set_tests_properties(${_tn} PROPERTIES TIMEOUT ${_new_to})
            endif()
        endforeach()
    endif()
endif()

# ============================================================
# Fuzz targets
# ============================================================

if(GNFS_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "GNFS fuzz targets require Clang/libFuzzer")
    endif()
    include(CheckCXXSourceCompiles)
    set(_GNFS_SAVED_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -fsanitize=fuzzer,address,undefined")
    # libFuzzer supplies main(); probe with the fuzzer entrypoint instead of
    # check_cxx_compiler_flag(), whose generated main conflicts at link time.
    check_cxx_source_compiles([=[
        #include <stddef.h>
        #include <stdint.h>
        extern "C" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) {
            return 0;
        }
    ]=] GNFS_HAS_LIBFUZZER)
    set(CMAKE_REQUIRED_FLAGS "${_GNFS_SAVED_REQUIRED_FLAGS}")
    if(NOT GNFS_HAS_LIBFUZZER)
        message(FATAL_ERROR "GNFS fuzz targets require a working libFuzzer runtime")
    endif()

    add_executable(fuzz_integer
        tests/fuzz/fuzz_integer.cpp
    )
    target_link_libraries(fuzz_integer PRIVATE gnfs_core)
    target_compile_options(fuzz_integer PRIVATE
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer
        -g
    )
    target_link_options(fuzz_integer PRIVATE
        -fsanitize=fuzzer,address,undefined
    )
endif()

# ============================================================
# 安装
# ============================================================

install(TARGETS gnfs gnfs_core gnfs_util
    EXPORT GNFSTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

install(DIRECTORY include/gnfs
    DESTINATION include
)

# ============================================================
# 打印配置摘要
# ============================================================

message(STATUS "")
message(STATUS "GNFS Configuration Summary:")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  GMP: ${GMP_LIBRARY}")
message(STATUS "  NTL: ${HAVE_NTL}")
message(STATUS "  Metal: ${HAVE_METAL}")
message(STATUS "  Tests: ${GNFS_BUILD_TESTS}")
message(STATUS "")

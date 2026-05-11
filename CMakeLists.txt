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
    # 优化 — Apple Silicon 用 -mcpu=native（隐含 -march），x86 用 -march=native
    if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
        set(GNFS_NATIVE_FLAG -mcpu=native)
    else()
        set(GNFS_NATIVE_FLAG -march=native)
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
    add_compile_options(/W4)
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

# 导出编译命令（用于 IDE 和 clangd）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================
# 依赖查找
# ============================================================

# GMP - GNU Multiple Precision Arithmetic Library
find_library(GMP_LIBRARY gmp REQUIRED)
find_path(GMP_INCLUDE_DIR gmp.h REQUIRED)

# NTL - Number Theory Library (可选)
find_library(NTL_LIBRARY ntl)
find_path(NTL_INCLUDE_DIR NTL/ZZ.h)

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

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/cofactor/cofactorizer.cpp")
    target_sources(gnfs_core PRIVATE src/cofactor/cofactorizer.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/relation/collector.cpp")
    target_sources(gnfs_core PRIVATE src/relation/collector.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/relation/filter.cpp")
    target_sources(gnfs_core PRIVATE src/relation/filter.cpp)
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
    ${GMP_INCLUDE_DIR}
)
target_link_libraries(gnfs_core PUBLIC
    ${GMP_LIBRARY}
    gnfs_util
    Threads::Threads
)

if(HAVE_NTL)
    target_include_directories(gnfs_core PUBLIC ${NTL_INCLUDE_DIR})
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

if(GNFS_BUILD_TESTS)
    enable_testing()

    # 简单测试程序
    add_executable(test_small_vector
        tests/test_small_vector.cpp
    )
    target_link_libraries(test_small_vector PRIVATE gnfs_util)
    add_test(NAME SmallVector COMMAND test_small_vector)

    add_executable(test_integer
        tests/test_integer.cpp
    )
    target_link_libraries(test_integer PRIVATE gnfs_core)
    add_test(NAME Integer COMMAND test_integer)

    add_executable(test_thread_pool
        tests/test_thread_pool.cpp
    )
    target_link_libraries(test_thread_pool PRIVATE gnfs_util Threads::Threads)
    add_test(NAME ThreadPool COMMAND test_thread_pool)

    add_executable(test_factor_base
        tests/test_factor_base.cpp
    )
    target_link_libraries(test_factor_base PRIVATE gnfs_core Threads::Threads)
    add_test(NAME FactorBase COMMAND test_factor_base)

    add_executable(test_special_q
        tests/test_special_q.cpp
    )
    target_link_libraries(test_special_q PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SpecialQ COMMAND test_special_q)

    add_executable(test_lattice_sieve
        tests/test_lattice_sieve.cpp
    )
    target_link_libraries(test_lattice_sieve PRIVATE gnfs_core Threads::Threads)
    add_test(NAME LatticeSieve COMMAND test_lattice_sieve)
    set_tests_properties(LatticeSieve PROPERTIES LABELS "slow" TIMEOUT 300)

    add_executable(test_relation_collector
        tests/test_relation_collector.cpp
    )
    target_link_libraries(test_relation_collector PRIVATE gnfs_core Threads::Threads)
    add_test(NAME RelationCollector COMMAND test_relation_collector)

    add_executable(test_sieve_basic
        tests/test_sieve_basic.cpp
    )
    target_link_libraries(test_sieve_basic PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SieveIntegration COMMAND test_sieve_basic)

    add_executable(test_cofactor
        tests/test_cofactor.cpp
    )
    target_link_libraries(test_cofactor PRIVATE gnfs_core Threads::Threads)
    add_test(NAME Cofactor COMMAND test_cofactor)

    add_executable(test_squfof
        tests/test_squfof.cpp
    )
    target_link_libraries(test_squfof PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SQUFOF COMMAND test_squfof)

    add_executable(test_linalg
        tests/test_linalg.cpp
    )
    target_link_libraries(test_linalg PRIVATE gnfs_core Threads::Threads)
    add_test(NAME LinearAlgebra COMMAND test_linalg)

    add_executable(test_sqrt
        tests/test_sqrt.cpp
    )
    target_link_libraries(test_sqrt PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SquareRoot COMMAND test_sqrt)

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

    # 渐进式测试 — 支持命令行参数选择级别
    add_executable(test_gnfs_progressive
        tests/test_gnfs_progressive.cpp
    )
    target_link_libraries(test_gnfs_progressive PRIVATE gnfs_core Threads::Threads)
    add_test(NAME GNFS_Progressive_L1 COMMAND test_gnfs_progressive 1 1)
    add_test(NAME GNFS_Progressive_L2 COMMAND test_gnfs_progressive 2 2)
    set_tests_properties(GNFS_Progressive_L1 PROPERTIES TIMEOUT 60)
    set_tests_properties(GNFS_Progressive_L2 PROPERTIES TIMEOUT 120)
    # Level 3+ 不加入 ctest — 手动运行: ./test_gnfs_progressive 3

    # 参数计算器测试
    add_executable(test_params
        tests/test_params.cpp
    )
    target_link_libraries(test_params PRIVATE gnfs_core Threads::Threads)
    add_test(NAME Params COMMAND test_params)

    # 整系数多项式测试
    add_executable(test_int_polynomial
        tests/test_int_polynomial.cpp
    )
    target_link_libraries(test_int_polynomial PRIVATE gnfs_core Threads::Threads)
    add_test(NAME IntPolynomial COMMAND test_int_polynomial)

    # 关系过滤器测试
    add_executable(test_filter
        tests/test_filter.cpp
    )
    target_link_libraries(test_filter PRIVATE gnfs_core Threads::Threads)
    add_test(NAME Filter COMMAND test_filter)

    # 回归测试
    add_executable(test_regressions
        tests/test_regressions.cpp
    )
    target_link_libraries(test_regressions PRIVATE gnfs_core Threads::Threads)
    add_test(NAME Regressions COMMAND test_regressions)

    # PolynomialContext 单元测试
    add_executable(test_polynomial_context
        tests/test_polynomial_context.cpp
    )
    target_link_libraries(test_polynomial_context PRIVATE gnfs_core Threads::Threads)
    add_test(NAME PolynomialContext COMMAND test_polynomial_context)

    # BaseMSelector 单元测试
    add_executable(test_base_m
        tests/test_base_m.cpp
    )
    target_link_libraries(test_base_m PRIVATE gnfs_core Threads::Threads)
    add_test(NAME BaseM COMMAND test_base_m)

    # PolynomialOptimizer 单元测试
    add_executable(test_polynomial_optimizer
        tests/test_polynomial_optimizer.cpp
    )
    target_link_libraries(test_polynomial_optimizer PRIVATE gnfs_core Threads::Threads)
    add_test(NAME PolynomialOptimizer COMMAND test_polynomial_optimizer)

    # ClassGroup 单元测试
    add_executable(test_class_group
        tests/test_class_group.cpp
    )
    target_link_libraries(test_class_group PRIVATE gnfs_core Threads::Threads)
    add_test(NAME ClassGroup COMMAND test_class_group)

    # 边界/极端情况测试
    add_executable(test_edge_cases
        tests/test_edge_cases.cpp
    )
    target_link_libraries(test_edge_cases PRIVATE gnfs_core Threads::Threads)
    add_test(NAME EdgeCases COMMAND test_edge_cases)

    # Schirokauer degree-4 diagnostic
    add_executable(test_schirokauer_deg4
        tests/test_schirokauer_deg4.cpp
    )
    target_link_libraries(test_schirokauer_deg4 PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SchirokaureDeg4 COMMAND test_schirokauer_deg4)

    # Schirokauer strip-ℓ math regression (instant)
    add_executable(test_schirokauer_strip
        tests/test_schirokauer_strip.cpp
    )
    target_link_libraries(test_schirokauer_strip PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SchirokauerStrip COMMAND test_schirokauer_strip)

    # 跨模块集成测试
    add_executable(test_integration
        tests/test_integration.cpp
    )
    target_link_libraries(test_integration PRIVATE gnfs_core Threads::Threads)
    add_test(NAME Integration COMMAND test_integration)

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

    # Line sieve 单元测试
    add_executable(test_line_sieve
        tests/test_line_sieve.cpp
    )
    target_link_libraries(test_line_sieve PRIVATE gnfs_core Threads::Threads)
    add_test(NAME LineSieve COMMAND test_line_sieve)

    # Bucket sieve 单元测试
    add_executable(test_bucket_sieve
        tests/test_bucket_sieve.cpp
    )
    target_link_libraries(test_bucket_sieve PRIVATE gnfs_core Threads::Threads)
    add_test(NAME BucketSieve COMMAND test_bucket_sieve)

    # Mmap CSR Matrix 单元测试
    add_executable(test_mmap_csr
        tests/test_mmap_csr.cpp
    )
    target_link_libraries(test_mmap_csr PRIVATE gnfs_core Threads::Threads)
    add_test(NAME MmapCSR COMMAND test_mmap_csr)

    # Out-of-core Relations 单元测试
    add_executable(test_ooc_relations
        tests/test_ooc_relations.cpp
    )
    target_link_libraries(test_ooc_relations PRIVATE gnfs_core Threads::Threads)
    add_test(NAME OOCRelations COMMAND test_ooc_relations)

    # Block Wiedemann 单元测试
    add_executable(test_block_wiedemann
        tests/test_block_wiedemann.cpp
    )
    target_link_libraries(test_block_wiedemann PRIVATE gnfs_core Threads::Threads)
    add_test(NAME BlockWiedemann COMMAND test_block_wiedemann)

    # 回归门禁测试 (10/15/20/25-digit full pipeline)
    add_executable(test_regression_gate
        tests/test_regression_gate.cpp
    )
    target_link_libraries(test_regression_gate PRIVATE gnfs_core Threads::Threads)
    add_test(NAME RegressionGate COMMAND test_regression_gate)
    set_tests_properties(RegressionGate PROPERTIES LABELS "gate" TIMEOUT 120)

    # API 层测试
    add_executable(test_api
        tests/test_api.cpp
    )
    target_link_libraries(test_api PRIVATE gnfs_core Threads::Threads)
    add_test(NAME API COMMAND test_api)

    # Method selection 测试
    add_executable(test_method_selection
        tests/test_method_selection.cpp
    )
    target_link_libraries(test_method_selection PRIVATE gnfs_core Threads::Threads)
    add_test(NAME MethodSelection COMMAND test_method_selection)

    # i18n 测试
    add_executable(test_i18n
        tests/test_i18n.cpp
    )
    target_link_libraries(test_i18n PRIVATE gnfs_core Threads::Threads)
    add_test(NAME I18N COMMAND test_i18n)

    # SIQS (Self-Initializing Quadratic Sieve) 测试
    add_executable(test_siqs
        tests/test_siqs.cpp
    )
    target_link_libraries(test_siqs PRIVATE gnfs_core Threads::Threads)
    add_test(NAME SIQS COMMAND test_siqs)

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

#!/usr/bin/env zsh
# ╔══════════════════════════════════════════════════════════════════╗
# ║  GNFS 本地全自动测试工作流                                        ║
# ║  涵盖: 冒烟测试、单元测试、模块测试、集成测试、E2E、渐进、性能基准  ║
# ╚══════════════════════════════════════════════════════════════════╝
#
# ────────────────────────── 用法 ──────────────────────────
#
# 基础模式:
#   ./scripts/test.sh                     # 默认: 编译 + 冒烟测试 (最快)
#   ./scripts/test.sh smoke               # 冒烟测试: instant 层核心测试
#   ./scripts/test.sh unit                # 全部 ctest 单元测试
#   ./scripts/test.sh build               # 仅编译，不跑测试
#
# 模块级测试:
#   ./scripts/test.sh module <mod>        # 指定模块 (core/util/sieve/linalg/...)
#   ./scripts/test.sh module sieve linalg # 多模块同时测
#   ./scripts/test.sh module all          # 逐模块全测
#
# 单个测试:
#   ./scripts/test.sh run <test_name>     # 运行指定测试二进制 (如 test_linalg)
#   ./scripts/test.sh run test_sqrt       # 运行指定测试
#
# 集成 & E2E:
#   ./scripts/test.sh e2e                 # E2E 测试 (N=143 完整流水线)
#   ./scripts/test.sh integration         # 跨模块集成测试
#   ./scripts/test.sh pipeline            # 完整流水线: 每阶段分别验证
#
# 渐进式测试:
#   ./scripts/test.sh progressive         # L1-L5 全部 (可能很慢)
#   ./scripts/test.sh progressive 1       # 仅 L1
#   ./scripts/test.sh progressive 1 3     # L1 到 L3
#   ./scripts/test.sh L1                  # 快捷: progressive 1 1
#   ./scripts/test.sh L2                  # 快捷: progressive 2 2
#   ./scripts/test.sh L3                  # 快捷: progressive 3 3
#   ./scripts/test.sh L4                  # 快捷: progressive 4 4
#   ./scripts/test.sh L5                  # 快捷: progressive 5 5
#
# 合并门禁:
#   ./scripts/test.sh gate                # 二级门禁: smoke + 回归 (17/27/40/81-bit)
#   ./scripts/test.sh gate --quick        # 快速门禁: 仅 smoke
#   ./scripts/test.sh tsan-relation       # 窄 ThreadSanitizer relation 并发门禁
#
# 智能模式:
#   ./scripts/test.sh changed             # 根据 git diff 自动选择受影响模块
#   ./scripts/test.sh changed --deep      # git diff + 含依赖模块的级联测试
#
# 全量测试:
#   ./scripts/test.sh full                # 编译 + ctest + E2E + Progressive L1-L2
#   ./scripts/test.sh thorough            # full + 所有模块逐测 + Progressive L1-L3
#   ./scripts/test.sh nightly             # thorough + L4 + L5 (超长，适合过夜)
#
# 性能 & 基准:
#   ./scripts/test.sh perf                # 25-digit 性能测试
#   ./scripts/test.sh bench               # 基准测试: 全部级别 + 计时对比
#   ./scripts/test.sh bench --save        # 保存基准结果到 benchmarks/
#   ./scripts/test.sh bench --compare     # 与上次保存的基准对比
#   ./scripts/test.sh structured-ooc-rss 50000 4
#                                         # 独立进程 structured OOC RSS 场景
#   ./scripts/test.sh probe-50d-structured-ooc
#                                         # 真实 50 位、有界 production Pipeline 探针
#   ./scripts/test.sh bench-ram <level>   # 后台 RAM baseline: nohup + /usr/bin/time -l
#                                         # level=1 (50d ≈2h) / 2 (60d hours+) / 3-5 (大)
#
# 监视模式:
#   ./scripts/test.sh watch               # 文件变更时自动重新测试 (需 fswatch)
#   ./scripts/test.sh watch smoke         # 监视 + 仅冒烟测试
#   ./scripts/test.sh watch module sieve  # 监视 + 仅筛法模块
#
# 报告:
#   ./scripts/test.sh report              # 生成 JSON 测试报告到 build/test_report.json
#   ./scripts/test.sh list                # 列出所有测试、模块、依赖关系
#   ./scripts/test.sh matrix              # 显示模块→测试映射矩阵
#
# ────────────────────────── 选项 ──────────────────────────
#
#   -j N          并行编译数 (默认: 自动检测 CPU 核心数)
#   -t TYPE       构建类型: Debug / Release / RelWithDebInfo (默认: Debug)
#   -v            详细输出 (显示测试完整 stdout)
#   -q            安静模式 (仅显示失败)
#   --no-build    跳过编译步骤 (直接运行已编译的测试)
#   --no-color    禁用彩色输出
#   --fail-fast   首个失败即停止
#   --timeout N   测试超时秒数 (默认: 300)
#   --retry N     失败重试次数 (默认: 0)
#
# ────────────────────────── 示例 ──────────────────────────
#
#   ./scripts/test.sh module linalg          # 只跑线性代数测试
#   ./scripts/test.sh changed --deep         # 自动检测 + 级联依赖测试
#   ./scripts/test.sh -t Release bench       # Release 模式基准测试
#   ./scripts/test.sh --no-build run test_sqrt  # 不编译，直接跑 sqrt 测试
#   ./scripts/test.sh --fail-fast full       # 全测试，首次失败即停
#   ./scripts/test.sh watch module linalg sqrt  # 监视模式，只跑 linalg+sqrt
#

set -eo pipefail
# Under errexit, `(( counter++ ))` returns status 1 when counter was zero and
# can silently terminate a mode before its first test. Use `+= 1` mutations.

# ============================================================
# 全局配置
# ============================================================

PROJECT_ROOT="${0:A:h:h}"
BUILD_DIR="${PROJECT_ROOT}/build"
BENCH_DIR="${PROJECT_ROOT}/benchmarks"
REPORT_FILE="${BUILD_DIR}/test_report.json"
NCPU=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

BUILD_TYPE="Debug"
BUILD_TYPE_EXPLICIT=0
PARALLEL_JOBS="$NCPU"
VERBOSE=0
QUIET=0
SKIP_BUILD=0
USE_COLOR=1
FAIL_FAST=0
TIMEOUT=300
TIMEOUT_EXPLICIT=0
RETRY_COUNT=0

# 统计变量
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0
SKIPPED_TESTS=0
TOTAL_TIME_MS=0

# JSON 报告条目
typeset -a REPORT_ENTRIES
REPORT_ENTRIES=()

# ============================================================
# 颜色
# ============================================================

setup_colors() {
    if [[ -t 1 ]] && (( USE_COLOR )); then
        RED=$'\033[0;31m'
        GREEN=$'\033[0;32m'
        YELLOW=$'\033[0;33m'
        BLUE=$'\033[0;34m'
        CYAN=$'\033[0;36m'
        MAGENTA=$'\033[0;35m'
        BOLD=$'\033[1m'
        DIM=$'\033[2m'
        RESET=$'\033[0m'
        CHECK='✓'
        CROSS='✗'
        WARN='⚠'
        ARROW='→'
        BULLET='•'
    else
        RED='' GREEN='' YELLOW='' BLUE='' CYAN='' MAGENTA='' BOLD='' DIM='' RESET=''
        CHECK='[PASS]' CROSS='[FAIL]' WARN='[WARN]' ARROW='->' BULLET='*'
    fi
}
setup_colors

# ============================================================
# 模块定义 → 测试映射 (zsh associative arrays)
# ============================================================

# 所有已知测试二进制
typeset -a ALL_TEST_BINARIES
ALL_TEST_BINARIES=(
    test_integer
    test_small_vector
    test_thread_pool
    test_ordered_parallel_map
    test_logger
    test_primes
    test_timer
    test_process_memory
    test_mmap_file
    test_resultant
    test_core_types
    test_safe_math
    test_bit_intrin
    test_ooc_policy
    test_v0_bfs_policy
    test_bw_rank_est
    test_matrix_diagnostics
    test_sge_streaming
    test_factor_base
    test_fb_roots_parallel
    test_special_q
    test_lattice_sieve
    test_lll_lattice
    test_adaptive_lattice
    test_sieve_basic
    test_relation_collector
    test_relation_corpus
    test_relation_sink
    test_ooc_store_integrity
    test_cofactor
    test_batch_ecm
    test_linalg
    test_sge_batch_pivots
    test_sqrt
    test_sqrt_debug
    test_hensel_parallel
    test_couveignes_large_class_group
    test_couveignes_parallel
    test_murphy
    test_kleinjung
    test_kleinjung_large
    test_factor_with_kleinjung
    test_gnfs_e2e
    test_gnfs_progressive
    test_25digit
    test_params
    test_int_polynomial
    test_half_gcd
    test_poly_karatsuba
    test_horner_batch_simd
    test_divrem_subquadratic
    test_poly_ntt
    test_poly_square
    test_poly_add_mod_simd
    test_poly_horner_mod_simd
    test_filter
    test_lp_key_contract
    test_relation_identity
    test_relation_reduction_engine
    test_structured_ooc_scale
    test_structured_ooc_50d_probe
    test_structured_filter
    test_structured_filter_policy
    test_structured_tree_basis
    test_structured_tree_basis_property
    test_structured_budgeted_driver
    test_structured_conflict_batch
    test_structured_parallel_prepare
    test_structured_batch_commit
    test_structured_parallel_driver
    test_structured_parallel_failures
    test_structured_incidence_builder
    test_structured_materialization
    test_regressions
    test_polynomial_context
    test_base_m
    test_polynomial_optimizer
    test_rotation_incremental
    test_class_group
    test_schirokauer_deg4
    test_schirokauer_strip
    test_schirokauer_parallel
    test_edge_cases
    test_integration
    test_squfof
    test_brent_pollard_rho
    test_survival_predictor
    test_bucket_sieve
    test_sieve_tiny_simd
    test_bucket_prefetch
    test_sieve_region_tile
    test_sieve_norm_tile
    test_lattice_basis_parallel
    test_sieve_apply_tile_parallel
    test_lattice_coords_simd
    test_threshold_scan_simd
    test_saturated_sub_simd
    test_regression_gate
    test_stress
    test_3lp_cofactor
    test_3lp_merge
    test_trial_wheel
    test_batch_trial
    test_ecm_curve_pool
    test_sigma_seed_pool
    test_ecm_stage2_parallel
    test_ecm_stage1_parallel
    test_batch_inversion
    test_cofactor_stage_timing
    test_ecm_prime_cache
    test_cofactor_result_cache
    test_brent_pollard_rho_parallel
    test_matrix_view_concept
    test_save_sparse_as_mmap
    test_linalg_mmap_policy
    test_metal_spmv
    test_spmv_simd
    test_transpose_blocked
    test_popcount_simd
    test_and_popcnt_simd
    test_xor_words_simd
    test_and_words_simd
    test_xor_popcnt_simd
    test_row_popcount_simd
    test_krylov_compress
    test_krylov_compression
    test_ecm_brent_suyama
    test_ecm_brent_suyama_bench
    test_poly_checkpoint
    test_fb_checkpoint
    test_bl_checkpoint
    test_memory_pool
    test_relation_pool_integration
    test_integer_scratch_pool
    test_mpz_powm_parallel
    test_linalg_progress
    test_mpz_invert_parallel
    test_mpz_mod_parallel
    test_mpz_gcd_parallel
    test_mpz_mul_parallel
    test_api
    test_bai_brent_poly
    test_batch_ecm_bench
    test_bl_resume_integration
    test_block_wiedemann
    test_bw_krylov_mmap_integration
    test_bw_krylov_parallel
    test_clique_merger
    test_clique_merger_50d_synthetic
    test_cofactor_batch_bench
    test_distributed_sieve
    test_filter_radix_sort
    test_full_resume
    test_i18n
    test_krylov_sequence_mmap
    test_lp_bloom
    test_lp_key_hash
    test_merger_parallel
    test_method_selection
    test_mmap_csr
    test_ooc_relations
    test_root_property_cache
    test_sieve_checkpoint
    test_sieve_ecore_qos
    test_siqs
    test_siqs_e2e
    test_trial_div_simd
    test_trial_wheel_bench
    test_work_stealing
)

# 模块 → 测试二进制映射 (仅 instant+fast 的测试)
typeset -A MODULE_TESTS
MODULE_TESTS=(
    core           "test_integer test_params test_regressions test_edge_cases test_core_types"
    util           "test_small_vector test_thread_pool test_ordered_parallel_map test_logger test_primes test_timer test_process_memory test_mmap_file test_safe_math test_bit_intrin test_memory_pool test_integer_scratch_pool test_mpz_powm_parallel test_mpz_invert_parallel test_mpz_mod_parallel test_mpz_gcd_parallel test_mpz_mul_parallel"
    polynomial     "test_murphy test_root_property_cache test_int_polynomial test_half_gcd test_poly_karatsuba test_horner_batch_simd test_divrem_subquadratic test_poly_ntt test_poly_square test_poly_add_mod_simd test_poly_horner_mod_simd test_regressions test_polynomial_context test_base_m test_polynomial_optimizer test_resultant test_rotation_incremental test_bai_brent_poly test_poly_checkpoint"
    factor_base    "test_factor_base test_fb_checkpoint test_fb_roots_parallel"
    sieve          "test_special_q test_sieve_basic test_sieve_checkpoint test_distributed_sieve test_bucket_sieve test_sieve_ecore_qos test_lll_lattice test_adaptive_lattice test_sieve_tiny_simd test_bucket_prefetch test_sieve_region_tile test_sieve_norm_tile test_lattice_basis_parallel test_sieve_apply_tile_parallel test_lattice_coords_simd test_threshold_scan_simd test_saturated_sub_simd"
    cofactor       "test_cofactor test_squfof test_brent_pollard_rho test_brent_pollard_rho_parallel test_survival_predictor test_batch_ecm test_3lp_cofactor test_trial_wheel test_batch_trial test_ecm_curve_pool test_sigma_seed_pool test_ecm_stage2_parallel test_ecm_stage1_parallel test_batch_inversion test_trial_div_simd test_cofactor_stage_timing test_ecm_prime_cache test_cofactor_result_cache test_integration test_ecm_brent_suyama"
    relation       "test_relation_collector test_relation_corpus test_relation_sink test_ooc_store_integrity test_filter test_lp_key_contract test_relation_identity test_relation_reduction_engine test_structured_filter test_structured_filter_policy test_structured_tree_basis test_structured_tree_basis_property test_structured_budgeted_driver test_structured_conflict_batch test_structured_parallel_prepare test_structured_batch_commit test_structured_parallel_driver test_structured_parallel_failures test_structured_incidence_builder test_structured_materialization test_filter_radix_sort test_lp_bloom test_lp_key_hash test_merger_parallel test_clique_merger test_clique_merger_50d_synthetic test_3lp_merge test_ooc_relations test_ooc_policy test_v0_bfs_policy test_integration test_relation_pool_integration"
    linalg         "test_linalg test_sge_batch_pivots test_block_wiedemann test_bw_rank_est test_matrix_diagnostics test_sge_streaming test_mmap_csr test_schirokauer_deg4 test_schirokauer_strip test_schirokauer_parallel test_edge_cases test_integration test_matrix_view_concept test_save_sparse_as_mmap test_linalg_mmap_policy test_bw_krylov_parallel test_metal_spmv test_spmv_simd test_transpose_blocked test_popcount_simd test_and_popcnt_simd test_xor_words_simd test_and_words_simd test_xor_popcnt_simd test_row_popcount_simd test_krylov_compress test_krylov_compression test_bl_checkpoint test_bl_resume_integration test_linalg_progress"
    integration    "test_integration"
    sqrt           "test_sqrt test_sqrt_debug test_hensel_parallel test_class_group test_couveignes_large_class_group test_couveignes_parallel"
    api            "test_i18n test_method_selection test_relation_reduction_engine"
    siqs           "test_siqs"
)

# 模块 → 慢速测试映射 (slow+heavy, 可选运行)
typeset -A MODULE_SLOW_TESTS
MODULE_SLOW_TESTS=(
    polynomial     "test_kleinjung test_kleinjung_large test_factor_with_kleinjung"
    sieve          "test_lattice_sieve"
    cofactor       "test_ecm_brent_suyama_bench"
    relation       "test_structured_ooc_scale"
    api            "test_api test_full_resume"
    siqs           "test_siqs_e2e"
)

# 冒烟测试子集: instant 层核心测试，绝不包含真实 GNFS pipeline / bench / stress
typeset -a SMOKE_TESTS
SMOKE_TESTS=(
    test_integer
    test_params
    test_int_polynomial
    test_half_gcd
    test_poly_karatsuba
    test_horner_batch_simd
    test_divrem_subquadratic
    test_poly_ntt
    test_poly_square
    test_poly_add_mod_simd
    test_poly_horner_mod_simd
    test_small_vector
    test_thread_pool
    test_ordered_parallel_map
    test_logger
    test_primes
    test_timer
    test_process_memory
    test_mmap_file
    test_resultant
    test_core_types
    test_safe_math
    test_bit_intrin
    test_ooc_policy
    test_v0_bfs_policy
    test_sieve_ecore_qos
    test_lll_lattice
    test_adaptive_lattice
    test_bw_rank_est
    test_matrix_diagnostics
    test_sge_streaming
    test_factor_base
    test_fb_roots_parallel
    test_special_q
    test_relation_collector
    test_relation_corpus
    test_relation_sink
    test_filter
    test_lp_key_contract
    test_relation_identity
    test_relation_reduction_engine
    test_structured_filter
    test_structured_filter_policy
    test_structured_tree_basis
    test_structured_tree_basis_property
    test_structured_budgeted_driver
    test_structured_conflict_batch
    test_structured_parallel_prepare
    test_structured_batch_commit
    test_structured_parallel_driver
    test_structured_parallel_failures
    test_structured_incidence_builder
    test_structured_materialization
    test_filter_radix_sort
    test_lp_bloom
    test_lp_key_hash
    test_merger_parallel
    test_cofactor
    test_linalg
    test_sge_batch_pivots
    test_sqrt
    test_sqrt_debug
    test_hensel_parallel
    test_couveignes_large_class_group
    test_couveignes_parallel
    test_murphy
    test_root_property_cache
    test_regressions
    test_polynomial_context
    test_base_m
    test_polynomial_optimizer
    test_rotation_incremental
    test_class_group
    test_schirokauer_deg4
    test_schirokauer_strip
    test_schirokauer_parallel
    test_edge_cases
    test_squfof
    test_brent_pollard_rho
    test_survival_predictor
    test_i18n
    test_method_selection
    test_clique_merger
    test_3lp_cofactor
    test_3lp_merge
    test_trial_wheel
    test_batch_trial
    test_ecm_curve_pool
    test_sigma_seed_pool
    test_ecm_stage1_parallel
    test_batch_inversion
    test_trial_div_simd
    test_cofactor_stage_timing
    test_ecm_prime_cache
    test_cofactor_result_cache
    test_brent_pollard_rho_parallel
    test_sieve_tiny_simd
    test_sieve_region_tile
    test_sieve_norm_tile
    test_lattice_basis_parallel
    test_sieve_apply_tile_parallel
    test_lattice_coords_simd
    test_threshold_scan_simd
    test_saturated_sub_simd
    test_matrix_view_concept
    test_save_sparse_as_mmap
    test_linalg_mmap_policy
    test_metal_spmv
    test_spmv_simd
    test_transpose_blocked
    test_popcount_simd
    test_and_popcnt_simd
    test_xor_words_simd
    test_and_words_simd
    test_xor_popcnt_simd
    test_row_popcount_simd
    test_krylov_compress
    test_krylov_compression
    test_ecm_brent_suyama
    test_poly_checkpoint
    test_fb_checkpoint
    test_bl_checkpoint
    test_memory_pool
    test_relation_pool_integration
    test_integer_scratch_pool
    test_mpz_powm_parallel
    test_linalg_progress
    test_mpz_invert_parallel
    test_mpz_mod_parallel
    test_mpz_gcd_parallel
    test_mpz_mul_parallel
)

# ThreadSanitizer 窄通道: 只覆盖 structured relation 的有界分片构建、
# 共享 engine dispatch、并发调度、准备、批提交、driver 和故障边界。保持此列表小而明确，
# 避免把完整 instant 层复制到高成本的 sanitizer 构建。
typeset -a TSAN_RELATION_TESTS
TSAN_RELATION_TESTS=(
    test_ordered_parallel_map
    test_relation_collector
    test_relation_reduction_engine
    test_structured_parallel_prepare
    test_structured_batch_commit
    test_structured_parallel_driver
    test_structured_parallel_failures
    test_structured_incidence_builder
)

# ── 每个测试的超时秒数 (基于 2026-06-02 macOS Debug/Release 实测) ──
# instant: 纯单元 / helper correctness，单跑通常 <5s
# fast: 中等集成 / 资源敏感 helper，Debug 单跑通常 <30s
# gate: 多规模正确性门禁，Release PR 跑，Debug 可到 2min
# slow: 真实 GNFS / API pipeline，Debug 可到 5min
# heavy/stress/bench: 手动、nightly 或 informational
typeset -A TEST_TIMEOUT
TEST_TIMEOUT=(
    test_integer             10
    test_small_vector        10
    test_thread_pool         10
    test_ordered_parallel_map 10
    test_logger              10
    test_primes              10
    test_timer               10
    test_process_memory      10
    test_mmap_file           10
    test_resultant           10
    test_core_types          10
    test_safe_math           10
    test_bit_intrin          10
    test_ooc_policy          10
    test_v0_bfs_policy       10
    test_sieve_ecore_qos     10
    test_lll_lattice         10
    test_adaptive_lattice    30
    test_bw_rank_est         10
    test_matrix_diagnostics  10
    test_sge_streaming       30
    test_factor_base         10
    test_fb_roots_parallel   60
    test_special_q           10
    test_relation_collector  10
    test_relation_corpus     10
    test_relation_sink       10
    test_ooc_store_integrity 10
    test_cofactor            10
    test_linalg              10
    test_sge_batch_pivots    60
    test_sqrt                10
    test_sqrt_debug          10
    test_hensel_parallel     60
    test_couveignes_large_class_group 60
    test_couveignes_parallel 60
    test_murphy              10
    test_root_property_cache 60
    test_params              10
    test_int_polynomial      10
    test_half_gcd            60
    test_poly_karatsuba      60
    test_horner_batch_simd   60
    test_divrem_subquadratic 60
    test_poly_ntt            60
    test_poly_square         60
    test_poly_add_mod_simd   60
    test_poly_horner_mod_simd 60
    test_filter              10
    test_lp_key_contract     10
    test_relation_identity   10
    test_relation_reduction_engine 10
    test_structured_ooc_scale 180
    test_structured_ooc_50d_probe 3600
    test_structured_filter   10
    test_structured_filter_policy 10
    test_structured_tree_basis 10
    test_structured_tree_basis_property 10
    test_structured_budgeted_driver 10
    test_structured_conflict_batch 10
    test_structured_parallel_prepare 10
    test_structured_batch_commit 10
    test_structured_parallel_driver 10
    test_structured_parallel_failures 10
    test_structured_incidence_builder 10
    test_structured_materialization 10
    test_filter_radix_sort   60
    test_lp_bloom            60
    test_lp_key_hash         60
    test_merger_parallel     60
    test_regressions         10
    test_polynomial_context  10
    test_base_m              10
    test_polynomial_optimizer 10
    test_rotation_incremental 30
    test_class_group         10
    test_schirokauer_deg4    10
    test_schirokauer_strip   10
    test_schirokauer_parallel 60
    test_edge_cases          10
    test_integration         30
    test_sieve_basic         120
    test_sieve_checkpoint    10
    test_distributed_sieve   180
    test_kleinjung           360
    test_kleinjung_large     600
    test_factor_with_kleinjung 900
    test_lattice_sieve       180
    test_gnfs_e2e            300
    test_squfof              10
    test_brent_pollard_rho   60
    test_survival_predictor  30
    test_batch_ecm           60
    test_batch_ecm_bench     300
    test_block_wiedemann     30
    test_bw_krylov_mmap_integration 60
    test_krylov_sequence_mmap 10
    test_ooc_relations       10
    test_mmap_csr            10
    test_bucket_sieve        120
    test_regression_gate     240
    test_gnfs_progressive    3600
    test_25digit             1800
    test_stress              43200
    test_api                 300
    test_i18n                10
    test_method_selection    60
    test_clique_merger       10
    test_clique_merger_50d_synthetic 60
    test_3lp_cofactor        30
    test_3lp_merge           10
    test_trial_wheel         10
    test_batch_trial         20
    test_ecm_curve_pool      30
    test_sigma_seed_pool     60
    test_ecm_stage2_parallel 60
    test_ecm_stage1_parallel 60
    test_batch_inversion     60
    test_trial_div_simd      60
    test_cofactor_stage_timing 60
    test_ecm_prime_cache     60
    test_cofactor_result_cache 60
    test_brent_pollard_rho_parallel 60
    test_sieve_tiny_simd     10
    test_bucket_prefetch     120
    test_sieve_region_tile   60
    test_sieve_norm_tile     60
    test_lattice_basis_parallel 60
    test_sieve_apply_tile_parallel 60
    test_lattice_coords_simd 60
    test_threshold_scan_simd 60
    test_saturated_sub_simd  60
    test_matrix_view_concept 10
    test_save_sparse_as_mmap 10
    test_linalg_mmap_policy  10
    test_metal_spmv          30
    test_spmv_simd           10
    test_transpose_blocked   10
    test_popcount_simd       60
    test_and_popcnt_simd     60
    test_xor_words_simd      60
    test_and_words_simd      60
    test_xor_popcnt_simd     60
    test_row_popcount_simd   60
    test_krylov_compress     10
    test_krylov_compression  10
    test_bw_krylov_parallel  120
    test_ecm_brent_suyama    30
    test_ecm_brent_suyama_bench 120
    test_bai_brent_poly      60
    test_trial_wheel_bench   120
    test_cofactor_batch_bench 120
    test_poly_checkpoint     10
    test_fb_checkpoint       10
    test_bl_checkpoint       10
    test_bl_resume_integration 60
    test_full_resume         120
    test_siqs_e2e            700
    test_memory_pool         30
    test_relation_pool_integration 60
    test_integer_scratch_pool 60
    test_mpz_powm_parallel   60
    test_linalg_progress     60
    test_mpz_invert_parallel 60
    test_mpz_mod_parallel    60
    test_mpz_gcd_parallel    60
    test_mpz_mul_parallel    60
    test_work_stealing       10
    test_siqs                180
)

# 测试速度分级 (用于 list 显示)
typeset -A TEST_TIER
TEST_TIER=(
    test_integer             "instant"
    test_small_vector        "instant"
    test_thread_pool         "instant"
    test_ordered_parallel_map "instant"
    test_logger              "instant"
    test_primes              "instant"
    test_timer               "instant"
    test_process_memory      "instant"
    test_mmap_file           "instant"
    test_resultant           "instant"
    test_core_types          "instant"
    test_safe_math           "instant"
    test_bit_intrin          "instant"
    test_ooc_policy          "instant"
    test_v0_bfs_policy       "instant"
    test_sieve_ecore_qos     "instant"
    test_lll_lattice         "instant"
    test_adaptive_lattice    "instant"
    test_bw_rank_est         "instant"
    test_matrix_diagnostics  "instant"
    test_sge_streaming       "instant"
    test_factor_base         "instant"
    test_fb_roots_parallel   "instant"
    test_special_q           "instant"
    test_relation_collector  "instant"
    test_relation_corpus     "instant"
    test_relation_sink       "instant"
    test_ooc_store_integrity "instant"
    test_cofactor            "instant"
    test_linalg              "instant"
    test_sge_batch_pivots    "instant"
    test_sqrt                "instant"
    test_sqrt_debug          "instant"
    test_hensel_parallel     "instant"
    test_couveignes_large_class_group "instant"
    test_couveignes_parallel "instant"
    test_murphy              "instant"
    test_root_property_cache "instant"
    test_params              "instant"
    test_int_polynomial      "instant"
    test_half_gcd            "instant"
    test_poly_karatsuba      "instant"
    test_horner_batch_simd   "instant"
    test_divrem_subquadratic "instant"
    test_poly_ntt            "instant"
    test_poly_square         "instant"
    test_poly_add_mod_simd   "instant"
    test_poly_horner_mod_simd "instant"
    test_filter              "instant"
    test_lp_key_contract     "instant"
    test_relation_identity   "instant"
    test_relation_reduction_engine "instant"
    test_structured_ooc_scale "gate"
    test_structured_ooc_50d_probe "stress"
    test_structured_filter   "instant"
    test_structured_filter_policy "instant"
    test_structured_tree_basis "instant"
    test_structured_tree_basis_property "instant"
    test_structured_budgeted_driver "instant"
    test_structured_conflict_batch "instant"
    test_structured_parallel_prepare "instant"
    test_structured_batch_commit "instant"
    test_structured_parallel_driver "instant"
    test_structured_parallel_failures "instant"
    test_structured_incidence_builder "instant"
    test_structured_materialization "instant"
    test_filter_radix_sort   "instant"
    test_lp_bloom            "instant"
    test_lp_key_hash         "instant"
    test_merger_parallel     "instant"
    test_regressions         "instant"
    test_polynomial_context  "instant"
    test_base_m              "instant"
    test_polynomial_optimizer "instant"
    test_rotation_incremental "instant"
    test_class_group         "instant"
    test_schirokauer_deg4    "instant"
    test_schirokauer_strip   "instant"
    test_schirokauer_parallel "instant"
    test_edge_cases          "instant"
    test_integration         "fast"
    test_sieve_basic         "fast"
    test_sieve_checkpoint    "instant"
    test_distributed_sieve   "fast"
    test_kleinjung           "slow"
    test_kleinjung_large     "heavy"
    test_factor_with_kleinjung "slow"
    test_lattice_sieve       "slow"
    test_gnfs_e2e            "slow"
    test_squfof              "instant"
    test_brent_pollard_rho   "instant"
    test_survival_predictor  "instant"
    test_batch_ecm           "fast"
    test_batch_ecm_bench     "bench"
    test_block_wiedemann     "fast"
    test_bw_krylov_mmap_integration "fast"
    test_krylov_sequence_mmap "instant"
    test_ooc_relations       "instant"
    test_mmap_csr            "instant"
    test_bucket_sieve        "fast"
    test_regression_gate     "gate"
    test_gnfs_progressive    "heavy"
    test_25digit             "heavy"
    test_stress              "stress"
    test_api                 "slow"
    test_i18n                "instant"
    test_method_selection    "instant"
    test_siqs                "fast"
    test_clique_merger       "instant"
    test_clique_merger_50d_synthetic "fast"
    test_3lp_cofactor        "instant"
    test_3lp_merge           "instant"
    test_trial_wheel         "instant"
    test_batch_trial         "instant"
    test_ecm_curve_pool      "instant"
    test_sigma_seed_pool     "instant"
    test_ecm_stage2_parallel "fast"
    test_ecm_stage1_parallel "instant"
    test_batch_inversion     "instant"
    test_trial_div_simd      "instant"
    test_cofactor_stage_timing "instant"
    test_ecm_prime_cache     "instant"
    test_cofactor_result_cache "instant"
    test_brent_pollard_rho_parallel "instant"
    test_sieve_tiny_simd     "instant"
    test_bucket_prefetch     "fast"
    test_sieve_region_tile   "instant"
    test_sieve_norm_tile     "instant"
    test_lattice_basis_parallel "instant"
    test_sieve_apply_tile_parallel "instant"
    test_lattice_coords_simd "instant"
    test_threshold_scan_simd "instant"
    test_saturated_sub_simd  "instant"
    test_matrix_view_concept "instant"
    test_save_sparse_as_mmap "instant"
    test_linalg_mmap_policy  "instant"
    test_metal_spmv          "instant"
    test_spmv_simd           "instant"
    test_transpose_blocked   "instant"
    test_popcount_simd       "instant"
    test_and_popcnt_simd     "instant"
    test_xor_words_simd      "instant"
    test_and_words_simd      "instant"
    test_xor_popcnt_simd     "instant"
    test_row_popcount_simd   "instant"
    test_krylov_compress     "instant"
    test_krylov_compression  "instant"
    test_bw_krylov_parallel  "fast"
    test_ecm_brent_suyama    "instant"
    test_ecm_brent_suyama_bench "slow"
    test_bai_brent_poly      "fast"
    test_trial_wheel_bench   "bench"
    test_cofactor_batch_bench "bench"
    test_poly_checkpoint     "instant"
    test_fb_checkpoint       "instant"
    test_bl_checkpoint       "instant"
    test_bl_resume_integration "fast"
    test_full_resume         "slow"
    test_siqs_e2e            "slow"
    test_memory_pool         "instant"
    test_relation_pool_integration "instant"
    test_integer_scratch_pool "instant"
    test_mpz_powm_parallel   "instant"
    test_linalg_progress     "instant"
    test_mpz_invert_parallel "instant"
    test_mpz_mod_parallel    "instant"
    test_mpz_gcd_parallel    "instant"
    test_mpz_mul_parallel    "instant"
    test_work_stealing       "instant"
)

# 模块依赖图 (改了 A 模块 → 需要额外测试的下游模块)
typeset -A MODULE_DEPS
MODULE_DEPS=(
    core           "polynomial factor_base sieve cofactor relation linalg sqrt"
    util           "core polynomial factor_base sieve cofactor relation linalg sqrt"
    polynomial     "factor_base sieve"
    factor_base    "sieve cofactor relation linalg"
    sieve          "relation"
    cofactor       "relation"
    relation       "linalg"
    linalg         "sqrt"
    sqrt           ""
    siqs           ""
)

# 模块描述
typeset -A MODULE_DESC
MODULE_DESC=(
    core           "核心类型 (Integer, Polynomial, Relation)"
    util           "工具库 (SmallVector, ThreadPool, Logger)"
    polynomial     "多项式选择 (Kleinjung, Murphy E, base-m)"
    factor_base    "因子基构建 (Cantor-Zassenhaus)"
    sieve          "格筛法 (Lattice sieve, Special-Q)"
    cofactor       "余因子分解 (ECM, 试除法)"
    relation       "关系收集与过滤"
    linalg         "线性代数 (GF(2) 矩阵, Block Lanczos)"
    sqrt           "平方根 (Hensel, Couveignes, 代数平方根)"
    api            "公共 API (factorize, Pipeline, Config)"
    siqs           "Self-Initializing Quadratic Sieve (25-100d)"
)

# 有序模块列表 (按流水线顺序)
typeset -a MODULE_ORDER
MODULE_ORDER=(core util polynomial factor_base sieve cofactor relation linalg sqrt api)

# 文件路径 → 模块映射
path_to_module() {
    local path="$1"
    case "$path" in
        *core/*)       echo "core" ;;
        *util/*)       echo "util" ;;
        *polynomial/*) echo "polynomial" ;;
        *factor_base*) echo "factor_base" ;;
        *sieve/*)      echo "sieve" ;;
        *cofactor/*)   echo "cofactor" ;;
        *relation/*)   echo "relation" ;;
        *linalg/*)     echo "linalg" ;;
        *sqrt/*)       echo "sqrt" ;;
        *)             echo "" ;;
    esac
}

# ============================================================
# 日志与输出
# ============================================================

log_info()    { echo "${BLUE}[INFO]${RESET} $*"; }
log_success() { echo "${GREEN} ${CHECK}${RESET} $*"; }
log_fail()    { echo "${RED} ${CROSS}${RESET} $*"; }
log_warn()    { echo "${YELLOW} ${WARN}${RESET}  $*"; }
log_skip()    { echo "${DIM} - $*${RESET}"; }

# 计算字符串的终端显示宽度 (CJK 字符占 2 列)
# zsh 内置 $((#ch)) 取 Unicode codepoint，无需子进程
display_width() {
    local str="$1" w=0 ch
    local -i i cp
    for (( i = 1; i <= ${#str}; i++ )); do
        ch="${str[$i]}"
        cp=$(( #ch ))
        # U+3000 以上的 CJK / 全角字符占 2 列
        if (( cp > 0x2FFF )); then
            (( w += 2 ))
        else
            (( w += 1 ))
        fi
    done
    echo $w
}

log_header()  {
    local msg="$*"
    local -i w=$(display_width "$msg")
    local -i pad_len=$(( 48 - w ))
    (( pad_len < 0 )) && pad_len=0
    local padding="${(l:$pad_len:: :)}"
    echo ""
    echo "${BOLD}${CYAN}╔══════════════════════════════════════════════════╗${RESET}"
    echo "${BOLD}${CYAN}║  ${msg}${padding}║${RESET}"
    echo "${BOLD}${CYAN}╚══════════════════════════════════════════════════╝${RESET}"
    echo ""
}
log_section() { echo "\n${BOLD}── $* ──${RESET}\n"; }

# 高精度计时 (毫秒)
timer_start_ms() {
    python3 -c 'import time; print(int(time.time()*1000))'
}

format_duration() {
    local ms=$1
    if (( ms >= 60000 )); then
        printf "%dm %d.%ds" $((ms / 60000)) $((ms % 60000 / 1000)) $((ms % 1000 / 100))
    elif (( ms >= 1000 )); then
        printf "%d.%02ds" $((ms / 1000)) $((ms % 1000 / 10))
    else
        printf "%dms" "$ms"
    fi
}

# ============================================================
# 核心: 编译
# ============================================================

do_build() {
    if (( SKIP_BUILD )); then
        log_info "跳过编译 (--no-build)"
        return 0
    fi

    log_header "编译 ${BUILD_TYPE} -j${PARALLEL_JOBS}"

    # CMake 配置
    if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
        log_info "首次配置 CMake..."
        cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -S "$PROJECT_ROOT" 2>&1 | tail -5
    else
        local cached_type
        cached_type=$(grep -m1 'CMAKE_BUILD_TYPE:' "${BUILD_DIR}/CMakeCache.txt" | cut -d= -f2)
        if [[ "$cached_type" != "$BUILD_TYPE" ]]; then
            log_warn "构建类型变更: ${cached_type} ${ARROW} ${BUILD_TYPE}"
            cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -S "$PROJECT_ROOT" 2>&1 | tail -3
        fi
    fi

    local start_ms
    start_ms=$(timer_start_ms)

    local build_output exit_code=0
    if (( VERBOSE )); then
        cmake --build "$BUILD_DIR" -j"$PARALLEL_JOBS" -- -v 2>&1 || exit_code=$?
    else
        build_output=$(cmake --build "$BUILD_DIR" -j"$PARALLEL_JOBS" 2>&1) || exit_code=$?
    fi

    local end_ms
    end_ms=$(timer_start_ms)
    local elapsed=$((end_ms - start_ms))

    if (( exit_code != 0 )); then
        if (( ! VERBOSE )); then
            echo "$build_output"
        fi
        log_fail "编译失败 ($(format_duration $elapsed))"
        return 1
    fi

    # 统计 warnings
    if (( ! VERBOSE )); then
        local warn_count=0
        local -a build_lines warning_lines
        build_lines=("${(@f)build_output}")
        warning_lines=("${(@M)build_lines:#*warning:*}")
        warn_count=${#warning_lines}

        if (( warn_count > 0 )); then
            log_warn "编译成功 ($(format_duration $elapsed)) — ${warn_count} warnings"
            if (( ! QUIET )); then
                local preview_count=$(( warn_count < 5 ? warn_count : 5 ))
                local i
                for (( i = 1; i <= preview_count; i++ )); do
                    echo "$warning_lines[$i]"
                done
                if (( warn_count > 5 )); then
                    echo "  ${DIM}... 还有 $((warn_count - 5)) 个 warning${RESET}"
                fi
            fi
        else
            log_success "编译成功 ($(format_duration $elapsed))"
        fi
    else
        log_success "编译成功 ($(format_duration $elapsed))"
    fi
}

# ============================================================
# 核心: zsh 原生超时包装器 (不依赖 timeout/gtimeout)
# ============================================================

# run_with_timeout <timeout_secs> <command> [args...]
# 返回: 0=正常退出, 124=超时, 其他=命令退出码
# 输出保存在全局变量 RUN_OUTPUT 中
RUN_OUTPUT=""
run_with_timeout() {
    local timeout_secs=$1
    shift
    local cmd=("$@")

    local tmpfile
    tmpfile=$(mktemp /tmp/gnfs_test.XXXXXX)

    # 启动后台进程
    "${cmd[@]}" > "$tmpfile" 2>&1 &
    local pid=$!

    # 快速轮询: 前 2 秒每 0.1s 检查一次 (覆盖 instant 测试)
    local polls=0
    while (( polls < 20 )); do
        sleep 0.1
        (( polls += 1 ))
        if ! kill -0 $pid 2>/dev/null; then
            wait $pid 2>/dev/null
            local exit_code=$?
            RUN_OUTPUT=$(cat "$tmpfile")
            rm -f "$tmpfile"
            return $exit_code
        fi
    done

    # 慢速轮询: 之后每 1s 检查 + 心跳
    local elapsed_secs=2
    while (( elapsed_secs < timeout_secs )); do
        sleep 1
        (( elapsed_secs += 1 ))

        if ! kill -0 $pid 2>/dev/null; then
            wait $pid 2>/dev/null
            local exit_code=$?
            RUN_OUTPUT=$(cat "$tmpfile")
            rm -f "$tmpfile"
            return $exit_code
        fi

        # 每 10 秒打一个心跳点
        if (( elapsed_secs % 10 == 0 && !QUIET )); then
            printf "${DIM}[%ds]${RESET}" "$elapsed_secs" >&2
        fi
    done

    # 超时: 杀掉进程
    kill $pid 2>/dev/null
    sleep 0.2
    kill -9 $pid 2>/dev/null
    wait $pid 2>/dev/null

    RUN_OUTPUT=$(cat "$tmpfile")
    rm -f "$tmpfile"
    return 124
}

# ============================================================
# 核心: 运行单个测试
# ============================================================

run_single_test() {
    local name="$1"
    shift
    local extra_args=("$@")
    local binary="${BUILD_DIR}/${name}"

    if [[ ! -x "$binary" ]]; then
        log_skip "${name}: 二进制不存在"
        (( SKIPPED_TESTS += 1 ))
        return 2
    fi

    (( TOTAL_TESTS += 1 ))

    # 确定超时: 优先用 --timeout 全局覆盖，否则用每测试分级超时
    local test_timeout=${TIMEOUT}
    if (( ! TIMEOUT_EXPLICIT )); then
        # 用户未指定 --timeout，使用分级默认值
        test_timeout=${TEST_TIMEOUT[$name]:-$TIMEOUT}
    fi

    local tier="${TEST_TIER[$name]:-unknown}"

    # 显示开始运行 (非安静模式下，对慢测试提前告知)
    if (( !QUIET )) && [[ "$tier" == "gate" || "$tier" == "slow" || "$tier" == "heavy" || "$tier" == "stress" ]]; then
        printf "  ${DIM}%s (timeout=%ss, tier=%s)${RESET} " "$name" "$test_timeout" "$tier" >&2
    fi

    local start_ms exit_code=0
    start_ms=$(timer_start_ms)

    # 用 zsh 原生超时运行
    run_with_timeout "$test_timeout" "$binary" "${extra_args[@]}"
    exit_code=$?
    local output="$RUN_OUTPUT"

    local end_ms=$(timer_start_ms)
    local elapsed=$((end_ms - start_ms))
    TOTAL_TIME_MS=$((TOTAL_TIME_MS + elapsed))

    # 清除心跳点的行
    if (( !QUIET )) && [[ "$tier" == "gate" || "$tier" == "slow" || "$tier" == "heavy" || "$tier" == "stress" ]]; then
        printf "\r\033[K" >&2
    fi

    # 超时处理
    if (( exit_code == 124 )); then
        log_fail "${name} ${RED}TIMEOUT${RESET} ${DIM}after ${test_timeout}s (tier=${tier})${RESET}"
        if (( !QUIET )); then
            echo "${DIM}最后输出:${RESET}"
            echo "$output" | tail -10
        fi
        (( FAILED_TESTS += 1 ))
        REPORT_ENTRIES+=("{\"name\":\"${name}\",\"status\":\"timeout\",\"elapsed_ms\":${elapsed},\"detail\":\"timeout=${test_timeout}s\"}")
        if (( FAIL_FAST )); then
            log_fail "首次失败即停 (--fail-fast)"
            show_summary
            exit 1
        fi
        return 3
    fi

    # 重试逻辑
    if (( exit_code != 0 && RETRY_COUNT > 0 )); then
        local retry
        for retry in $(seq 1 "$RETRY_COUNT"); do
            log_warn "${name}: 重试 ${retry}/${RETRY_COUNT}..."
            local retry_start=$(timer_start_ms)
            run_with_timeout "$test_timeout" "$binary" "${extra_args[@]}"
            exit_code=$?
            output="$RUN_OUTPUT"
            local retry_end=$(timer_start_ms)
            elapsed=$((retry_end - retry_start))
            if (( exit_code == 0 )); then break; fi
        done
    fi

    if (( exit_code == 0 )); then
        (( PASSED_TESTS += 1 ))
        if (( QUIET )); then
            : # 安静模式不输出通过的测试
        elif (( VERBOSE )); then
            log_success "${name} ${DIM}($(format_duration $elapsed))${RESET}"
            echo "$output"
        else
            log_success "${name} ${DIM}($(format_duration $elapsed))${RESET}"
        fi
        REPORT_ENTRIES+=("{\"name\":\"${name}\",\"status\":\"pass\",\"elapsed_ms\":${elapsed},\"detail\":\"\"}")
        return 0
    else
        (( FAILED_TESTS += 1 ))
        log_fail "${name} ${DIM}($(format_duration $elapsed), exit=${exit_code})${RESET}"
        local tail_lines=20
        if (( VERBOSE )); then tail_lines=100; fi
        echo "${DIM}"
        echo "$output" | tail -"$tail_lines"
        echo "${RESET}"
        REPORT_ENTRIES+=("{\"name\":\"${name}\",\"status\":\"fail\",\"elapsed_ms\":${elapsed},\"detail\":\"exit_code=$exit_code\"}")
        if (( FAIL_FAST )); then
            log_fail "首次失败即停 (--fail-fast)"
            show_summary
            exit 1
        fi
        return 1
    fi
}

# Capture one machine-readable measurement record from RUN_OUTPUT. Dedicated
# resource modes fail closed if a successful binary emits no record or more
# than one record.
MEASUREMENT_RECORD=""
capture_single_measurement_record() {
    local prefix="$1"
    local label="$2"
    local record_count
    record_count=$(printf '%s\n' "$RUN_OUTPUT" |
        awk -v prefix="$prefix" 'index($0, prefix) == 1 { count += 1 } END { print count + 0 }')
    if [[ "$record_count" != "1" ]]; then
        log_fail "${label} 必须恰好输出 1 条 ${prefix}记录，实际为 ${record_count} 条"
        MEASUREMENT_RECORD=""
        return 1
    fi
    MEASUREMENT_RECORD=$(printf '%s\n' "$RUN_OUTPUT" |
        awk -v prefix="$prefix" 'index($0, prefix) == 1 { print }')
}

# ============================================================
# 报告输出
# ============================================================

write_report() {
    local total_elapsed=$1
    local entries=""
    for entry in "${REPORT_ENTRIES[@]}"; do
        if [[ -n "$entries" ]]; then entries+=","; fi
        entries+="$entry"
    done

    cat > "$REPORT_FILE" <<EOF
{
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "build_type": "${BUILD_TYPE}",
  "total": ${TOTAL_TESTS},
  "passed": ${PASSED_TESTS},
  "failed": ${FAILED_TESTS},
  "skipped": ${SKIPPED_TESTS},
  "total_elapsed_ms": ${total_elapsed},
  "tests": [${entries}]
}
EOF
    log_info "测试报告 ${ARROW} ${REPORT_FILE}"
}

# ============================================================
# 总结输出
# ============================================================

show_summary() {
    local total_elapsed
    total_elapsed=$(format_duration "$TOTAL_TIME_MS")

    # box 内宽 50 列 (║ 与 ║ 之间)
    local -i box_w=50

    # 辅助: 根据纯文本的显示宽度生成右填充空格
    _box_pad() {
        local -i w=$(display_width "$1")
        local -i pad=$(( box_w - w ))
        (( pad < 0 )) && pad=0
        printf '%*s' "$pad" ""
    }

    echo ""
    echo "${BOLD}╔══════════════════════════════════════════════════╗${RESET}"

    # 状态行
    local status_plain pad
    if (( FAILED_TESTS == 0 && TOTAL_TESTS > 0 )); then
        status_plain="  全部通过"
        pad=$(_box_pad "$status_plain")
        echo "${BOLD}║${RESET}  ${GREEN}全部通过${RESET}${BOLD}${pad}║${RESET}"
    elif (( TOTAL_TESTS == 0 )); then
        status_plain="  无测试运行"
        pad=$(_box_pad "$status_plain")
        echo "${BOLD}║${RESET}  ${DIM}无测试运行${RESET}${BOLD}${pad}║${RESET}"
    else
        status_plain="  有失败"
        pad=$(_box_pad "$status_plain")
        echo "${BOLD}║${RESET}  ${RED}有失败${RESET}${BOLD}${pad}║${RESET}"
    fi

    echo "${BOLD}╠──────────────────────────────────────────────────╣${RESET}"

    # 统计行
    local stats_plain
    stats_plain=$(printf '  通过: %d  失败: %d  跳过: %d  总计: %d' \
        "$PASSED_TESTS" "$FAILED_TESTS" "$SKIPPED_TESTS" "$TOTAL_TESTS")
    pad=$(_box_pad "$stats_plain")
    printf '%s  通过: %s%d%s  失败: %s%d%s  跳过: %s%d%s  总计: %d%s%s\n' \
        "${BOLD}║${RESET}" \
        "${GREEN}" "$PASSED_TESTS" "${RESET}" \
        "${RED}" "$FAILED_TESTS" "${RESET}" \
        "${DIM}" "$SKIPPED_TESTS" "${RESET}" \
        "$TOTAL_TESTS" \
        "$pad" "${BOLD}║${RESET}"

    # 耗时行
    local time_plain="  耗时: ${total_elapsed}"
    pad=$(_box_pad "$time_plain")
    echo "${BOLD}║${RESET}  耗时: ${total_elapsed}${pad}${BOLD}║${RESET}"

    echo "${BOLD}╚══════════════════════════════════════════════════╝${RESET}"
}

# ============================================================
# 模式: 冒烟测试
# ============================================================

do_smoke() {
    log_header "冒烟测试 (Smoke)"
    log_info "运行 ${#SMOKE_TESTS[@]} 个 instant 层核心测试"
    echo ""

    for test in "${SMOKE_TESTS[@]}"; do
        run_single_test "$test" || true
    done
}

# ============================================================
# 模式: CTest 单元测试
# ============================================================

do_ctest() {
    log_header "CTest 单元测试"

    local start_ms exit_code=0
    start_ms=$(timer_start_ms)

    local ctest_args=(--test-dir "$BUILD_DIR" --output-on-failure)
    if (( VERBOSE )); then ctest_args+=(--verbose); fi
    if (( QUIET )); then ctest_args+=(--quiet); fi

    ctest "${ctest_args[@]}" 2>&1 || exit_code=$?

    local end_ms=$(timer_start_ms)
    local elapsed=$((end_ms - start_ms))

    if (( exit_code == 0 )); then
        log_success "CTest 全部通过 ($(format_duration $elapsed))"
    else
        log_fail "CTest 有失败项 ($(format_duration $elapsed))"
    fi
    return $exit_code
}

# ============================================================
# 模式: 模块测试
# ============================================================

do_module() {
    local include_slow=0
    local modules=()

    # 解析参数
    for arg in "$@"; do
        case "$arg" in
            --slow) include_slow=1 ;;
            *)      modules+=("$arg") ;;
        esac
    done

    # 支持 "all" 关键字
    if [[ "${modules[1]}" == "all" ]]; then
        modules=("${MODULE_ORDER[@]}")
    fi

    if (( include_slow )); then
        log_header "模块测试 (+slow): ${modules[*]}"
    else
        log_header "模块测试: ${modules[*]}"
    fi

    local module_pass=0 module_fail=0

    for mod in "${modules[@]}"; do
        local tests="${MODULE_TESTS[$mod]:-}"
        if [[ -z "$tests" ]]; then
            log_warn "未知模块: ${mod}"
            log_info "可用模块: ${(k)MODULE_TESTS}"
            continue
        fi

        log_section "${mod} — ${MODULE_DESC[$mod]:-}"

        local mod_total=0 mod_pass=0
        for test in ${(s: :)tests}; do
            (( mod_total += 1 ))
            if run_single_test "$test"; then
                (( mod_pass += 1 ))
            fi
        done

        # 包含慢速测试
        if (( include_slow )); then
            local slow_tests="${MODULE_SLOW_TESTS[$mod]:-}"
            if [[ -n "$slow_tests" ]]; then
                log_info "  ${DIM}(包含慢速测试)${RESET}"
                for test in ${(s: :)slow_tests}; do
                    (( mod_total += 1 ))
                    if run_single_test "$test"; then
                        (( mod_pass += 1 ))
                    fi
                done
            fi
        fi

        if (( mod_pass == mod_total )); then
            (( module_pass += 1 ))
            log_info "${BOLD}${mod}${RESET}: ${mod_pass}/${mod_total} 全部通过"
        else
            (( module_fail += 1 ))
            log_fail "${BOLD}${mod}${RESET}: $((mod_total - mod_pass))/${mod_total} 失败"
        fi
    done

    echo ""
    if (( module_fail == 0 )); then
        log_success "所有模块通过 (${module_pass} modules)"
    else
        log_fail "${module_fail} 个模块有失败"
    fi
    return $module_fail
}

# ============================================================
# 模式: 单个测试
# ============================================================

do_run() {
    local name="$1"
    shift
    local extra_args=("$@")

    # 自动补全前缀
    if [[ "$name" != test_* ]]; then
        name="test_${name}"
    fi

    log_section "运行: ${name} ${extra_args[*]:-}"
    run_single_test "$name" "${extra_args[@]}" || true
}

# ============================================================
# 模式: E2E
# ============================================================

do_e2e() {
    log_header "E2E 端到端测试"
    log_info "完整 GNFS 流水线: 10800, 143, 9991, 10403, 96091"
    log_info "含 5 个子测试，tier=slow, timeout=${TEST_TIMEOUT[test_gnfs_e2e]}s"
    echo ""
    run_single_test test_gnfs_e2e || true
}

# ============================================================
# 模式: 集成测试 (跨模块)
# ============================================================

do_integration() {
    log_header "集成测试 (跨模块)"
    log_info "验证模块间交互正确性"
    echo ""

    log_section "1/4 多项式选择 + 因子基"
    run_single_test test_factor_with_kleinjung || true

    log_section "2/4 筛法 + 关系收集"
    run_single_test test_sieve_basic || true
    run_single_test test_relation_collector || true

    log_section "3/4 线性代数 + 平方根"
    run_single_test test_linalg || true
    run_single_test test_sqrt || true

    log_section "4/4 完整流水线"
    run_single_test test_gnfs_e2e || true
}

# ============================================================
# 模式: 流水线阶段逐步验证
# ============================================================

do_pipeline() {
    log_header "流水线阶段验证"

    local stage_num=0
    for mod in "${MODULE_ORDER[@]}"; do
        (( stage_num += 1 ))

        local tests="${MODULE_TESTS[$mod]:-}"
        if [[ -z "$tests" ]]; then continue; fi

        log_section "Stage ${stage_num}/9: ${MODULE_DESC[$mod]:-} (${mod})"

        local all_pass=1
        for test in ${(s: :)tests}; do
            if ! run_single_test "$test"; then
                all_pass=0
            fi
        done

        if (( ! all_pass && FAIL_FAST )); then
            log_fail "Stage ${stage_num} 失败，中止流水线"
            return 1
        fi
    done

    log_section "Stage 10/9: E2E 完整验证"
    run_single_test test_gnfs_e2e || true

    echo ""
    log_info "流水线验证完成"
}

# ============================================================
# 模式: 渐进式测试
# ============================================================

do_progressive() {
    local min_level="${1:-1}"
    local max_level="${2:-5}"

    log_header "渐进式测试 L${min_level}-L${max_level}"

    typeset -A level_desc
    level_desc=(
        1 "L1: 8-14 bit (< 5s)"
        2 "L2: 17-27 bit (< 30s)"
        3 "L3: 40 bit (< 10min)"
        4 "L4: 50 bit (< 1hr)"
        5 "L5: 61 bit (minutes-hours)"
    )

    local level
    for level in $(seq "$min_level" "$max_level"); do
        log_info "${level_desc[$level]:-Level $level}"
    done

    run_single_test test_gnfs_progressive "$min_level" "$max_level" || true
}

# ============================================================
# 模式: Git 变更检测
# ============================================================

do_changed() {
    local deep=0
    if [[ "${1:-}" == "--deep" ]]; then deep=1; shift; fi

    log_header "Git 变更检测"

    cd "$PROJECT_ROOT"

    # 收集所有变更文件
    local changed_files=""
    changed_files+=$(git diff --name-only HEAD 2>/dev/null || true)
    changed_files+=$'\n'
    changed_files+=$(git diff --name-only --cached 2>/dev/null || true)
    changed_files+=$'\n'
    changed_files+=$(git ls-files --others --exclude-standard 2>/dev/null || true)

    changed_files=$(echo "$changed_files" | sort -u | grep -v '^$')

    if [[ -z "$changed_files" ]]; then
        log_info "没有检测到代码变更"
        log_info "回退到冒烟测试..."
        do_smoke
        return
    fi

    local file_count
    file_count=$(echo "$changed_files" | wc -l | tr -d ' ')
    log_info "检测到 ${file_count} 个变更文件:"
    echo "$changed_files" | head -20 | while read -r f; do
        echo "  ${DIM}${BULLET} ${f}${RESET}"
    done
    if (( file_count > 20 )); then
        echo "  ${DIM}... 还有 $((file_count - 20)) 个文件${RESET}"
    fi
    echo ""

    # 映射到模块
    typeset -A affected_modules
    while IFS= read -r file; do
        [[ -z "$file" ]] && continue
        local mod
        mod=$(path_to_module "$file")
        if [[ -n "$mod" ]]; then
            affected_modules[$mod]=1
        fi
    done <<< "$changed_files"

    # 级联依赖 (--deep 模式)
    if (( deep )); then
        typeset -A cascade_modules
        for mod in "${(k)affected_modules[@]}"; do
            cascade_modules[$mod]=1
            local deps="${MODULE_DEPS[$mod]:-}"
            for dep in ${(s: :)deps}; do
                cascade_modules[$dep]=1
            done
        done

        local new_mods=""
        for mod in "${(k)cascade_modules[@]}"; do
            if [[ -z "${affected_modules[$mod]:-}" ]]; then
                new_mods+=" $mod"
            fi
        done

        if [[ -n "$new_mods" ]]; then
            log_info "级联依赖追加模块:${new_mods}"
        fi

        for mod in "${(k)cascade_modules[@]}"; do
            affected_modules[$mod]=1
        done
    fi

    local modules=("${(k)affected_modules[@]}")

    if (( ${#modules[@]} == 0 )); then
        log_info "变更文件未匹配到已知模块，运行冒烟测试"
        do_smoke
        return
    fi

    log_info "受影响模块: ${modules[*]}"
    echo ""

    do_module "${modules[@]}" || true

    # 核心模块变更 → 额外 E2E
    local needs_e2e=0
    for mod in "${modules[@]}"; do
        case "$mod" in
            core|linalg|sqrt|sieve|cofactor|relation) needs_e2e=1 ;;
        esac
    done

    if (( needs_e2e )); then
        log_section "核心模块变更 ${ARROW} E2E 回归"
        run_single_test test_gnfs_e2e || true
    fi

    if echo "$changed_files" | grep -q "CMakeLists.txt"; then
        log_warn "CMakeLists.txt 变更，建议运行 'full' 模式做完整验证"
    fi
}

# ============================================================
# 模式: 全量测试
# ============================================================

do_full() {
    log_header "全量测试"

    do_ctest || true

    log_section "E2E"
    run_single_test test_gnfs_e2e || true

    log_section "Progressive L1-L2"
    run_single_test test_gnfs_progressive 1 2 || true
}

do_thorough() {
    log_header "彻底测试"

    do_module "${MODULE_ORDER[@]}" || true

    do_integration || true

    log_section "Progressive L1-L3"
    run_single_test test_gnfs_progressive 1 3 || true
}

do_nightly() {
    log_header "夜间完整测试"

    do_thorough || true

    log_section "Progressive L4 (可能很慢)"
    run_single_test test_gnfs_progressive 4 4 || true

    log_section "Progressive L5 (可能非常慢)"
    run_single_test test_gnfs_progressive 5 5 || true

    log_section "Stress L1: 50-digit (可能数小时)"
    run_single_test test_stress 1 1 || true
}

# ============================================================
# 模式: 合并门禁 (Merge Gate)
# ============================================================
# 三级递进式验证，任一级失败立即退出:
#   Level 1: Smoke tests (instant 核心测试)
#   Level 2: RegressionGate (17/27/40/81-bit 全流水线)
#
# 用法:
#   ./scripts/test.sh gate            # 运行完整门禁
#   ./scripts/test.sh gate --quick    # 仅 Level 1 (smoke only)
#
# Exit code: 0=全部通过, 非0=有失败

do_gate() {
    local quick=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --quick) quick=1; shift ;;
            *)       shift ;;
        esac
    done

    log_header "合并门禁 (Merge Gate)"

    local gate_start_ms
    gate_start_ms=$(timer_start_ms)

    # ── Level 1: Smoke ──
    log_section "Gate Level 1: Smoke Tests"
    local pre_fail=$FAILED_TESTS
    for test in "${SMOKE_TESTS[@]}"; do
        run_single_test "$test" || true
        if (( FAIL_FAST && FAILED_TESTS > pre_fail )); then
            log_fail "Gate Level 1 FAILED — 中止门禁"
            return 1
        fi
    done
    if (( FAILED_TESTS > pre_fail )); then
        log_fail "Gate Level 1 FAILED — smoke 测试有失败，中止门禁"
        return 1
    fi
    log_success "Gate Level 1 PASSED — smoke ${#SMOKE_TESTS[@]}/${#SMOKE_TESTS[@]}"

    if (( quick )); then
        log_info "快速模式: 跳过 Level 2"
        return 0
    fi

    # ── Level 2: Regression Gate (multi-N full pipeline) ──
    # Uses test_regression_gate (17/27/40/81-bit) instead of progressive
    # because progressive L1 has known intermittent failures on 14-bit N.
    log_section "Gate Level 2: 回归门禁 (17/27/40/81-bit)"
    pre_fail=$FAILED_TESTS
    run_single_test test_regression_gate || true
    if (( FAILED_TESTS > pre_fail )); then
        log_fail "Gate Level 2 FAILED — 多规模回归失败"
        return 1
    fi
    log_success "Gate Level 2 PASSED — 回归门禁 (4 levels)"

    # ── 门禁通过 ──
    local gate_end_ms
    gate_end_ms=$(timer_start_ms)
    local gate_elapsed=$(format_duration "$((gate_end_ms - gate_start_ms))")
    echo ""
    log_success "${BOLD}合并门禁全部通过${RESET} (${gate_elapsed})"
    return 0
}

# ============================================================
# 模式: 基准测试
# ============================================================

do_bench() {
    local save=0 compare=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --save)    save=1; shift ;;
            --compare) compare=1; shift ;;
            *)         shift ;;
        esac
    done

    log_header "基准测试"

    mkdir -p "$BENCH_DIR"
    local bench_file="${BENCH_DIR}/bench_$(date +%Y%m%d_%H%M%S).json"
    typeset -a bench_results
    bench_results=()

    typeset -A level_names
    level_names=(1 "L1 (8-14 bit)" 2 "L2 (17-27 bit)" 3 "L3 (40 bit)" 4 "L4 (50 bit)" 5 "L5 (61 bit)")

    for level in 1 2 3 4 5; do
        log_section "基准: ${level_names[$level]}"

        local start_ms=$(timer_start_ms)
        local exit_code=0
        "${BUILD_DIR}/test_gnfs_progressive" "$level" "$level" > /dev/null 2>&1 || exit_code=$?
        local end_ms=$(timer_start_ms)
        local elapsed=$((end_ms - start_ms))

        if (( exit_code == 0 )); then
            log_success "${level_names[$level]}: $(format_duration $elapsed)"
            bench_results+=("{\"level\":${level},\"elapsed_ms\":${elapsed},\"status\":\"pass\"}")
        else
            log_fail "${level_names[$level]}: FAILED ($(format_duration $elapsed))"
            bench_results+=("{\"level\":${level},\"elapsed_ms\":${elapsed},\"status\":\"fail\"}")
        fi
    done

    if (( save )); then
        local entries=""
        for entry in "${bench_results[@]}"; do
            if [[ -n "$entries" ]]; then entries+=","; fi
            entries+="$entry"
        done
        cat > "$bench_file" <<EOF
{
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "build_type": "${BUILD_TYPE}",
  "results": [${entries}]
}
EOF
        log_info "基准结果已保存: ${bench_file}"
    fi

    if (( compare )); then
        local latest
        latest=$(ls -t "${BENCH_DIR}"/bench_*.json 2>/dev/null | head -2 | tail -1)
        if [[ -n "$latest" && -f "$latest" ]]; then
            log_section "与上次基准对比"
            log_info "对比文件: ${latest}"
            for level in 1 2 3 4 5; do
                local prev_ms
                prev_ms=$(python3 -c "
import json
with open('${latest}') as f:
    data = json.load(f)
for r in data['results']:
    if r['level'] == ${level}:
        print(r['elapsed_ms'])
        break
" 2>/dev/null || echo "0")
                local curr_entry="${bench_results[$level]}"
                local curr_ms
                curr_ms=$(echo "$curr_entry" | python3 -c "import json,sys; print(json.loads(sys.stdin.read())['elapsed_ms'])" 2>/dev/null || echo "0")

                if (( prev_ms > 0 && curr_ms > 0 )); then
                    local diff_pct=$(( (curr_ms - prev_ms) * 100 / prev_ms ))
                    if (( diff_pct < -5 )); then
                        echo "  L${level}: $(format_duration $prev_ms) ${ARROW} $(format_duration $curr_ms) ${GREEN}(${diff_pct}% 更快)${RESET}"
                    elif (( diff_pct > 5 )); then
                        echo "  L${level}: $(format_duration $prev_ms) ${ARROW} $(format_duration $curr_ms) ${RED}(+${diff_pct}% 更慢)${RESET}"
                    else
                        echo "  L${level}: $(format_duration $prev_ms) ${ARROW} $(format_duration $curr_ms) ${DIM}(相近)${RESET}"
                    fi
                fi
            done
        else
            log_warn "没有找到之前的基准文件进行对比"
        fi
    fi
}

# ============================================================
# 模式: 监视
# ============================================================

do_watch() {
    local watch_mode="${1:-smoke}"
    shift 2>/dev/null || true
    local watch_args=("$@")

    log_header "监视模式"

    if ! command -v fswatch &>/dev/null; then
        log_fail "需要 fswatch。安装: brew install fswatch"
        return 1
    fi

    log_info "监视 include/ src/ tests/ 的变更"
    log_info "模式: ${watch_mode} ${watch_args[*]:-}"
    log_info "按 Ctrl+C 停止"
    echo ""

    # 先运行一次
    SKIP_BUILD=0
    do_build || return 1
    case "$watch_mode" in
        smoke)   do_smoke ;;
        unit)    do_ctest ;;
        module)  do_module "${watch_args[@]}" ;;
        e2e)     do_e2e ;;
        changed) do_changed ;;
        *)       do_smoke ;;
    esac
    show_summary

    # 监视文件变更
    fswatch -o -r \
        --include='\.cpp$' \
        --include='\.hpp$' \
        --include='\.h$' \
        --exclude='.*' \
        "${PROJECT_ROOT}/include" \
        "${PROJECT_ROOT}/src" \
        "${PROJECT_ROOT}/tests" \
    | while read -r _; do
        sleep 1
        while read -r -t 0.1 _; do :; done

        echo ""
        echo "${BOLD}${YELLOW}═══ 检测到文件变更 ($(date +%H:%M:%S)) ═══${RESET}"
        echo ""

        TOTAL_TESTS=0 PASSED_TESTS=0 FAILED_TESTS=0 SKIPPED_TESTS=0 TOTAL_TIME_MS=0
        REPORT_ENTRIES=()

        SKIP_BUILD=0
        do_build || continue
        case "$watch_mode" in
            smoke)   do_smoke ;;
            unit)    do_ctest ;;
            module)  do_module "${watch_args[@]}" ;;
            e2e)     do_e2e ;;
            changed) do_changed ;;
            *)       do_smoke ;;
        esac
        show_summary
    done
}

# ============================================================
# 模式: ThreadSanitizer relation 窄通道
# ============================================================

do_tsan_relation() {
    local host_os
    host_os=$(uname -s)
    case "$host_os" in
        Linux|Darwin) ;;
        *)
            log_skip "tsan-relation 不支持 ${host_os}; 支持的平台为 Linux 和 macOS"
            (( SKIPPED_TESTS += ${#TSAN_RELATION_TESTS[@]} ))
            return 0
            ;;
    esac

    BUILD_DIR="${GNFS_TSAN_BUILD_DIR:-${PROJECT_ROOT}/build-tsan-relation}"
    REPORT_FILE="${BUILD_DIR}/test_report.json"
    BUILD_TYPE="Debug"

    log_header "ThreadSanitizer relation 窄通道"
    log_info "平台: ${host_os}; 构建目录: ${BUILD_DIR}"
    log_info "串行运行 ${#TSAN_RELATION_TESTS[@]} 个并发边界测试"

    if (( SKIP_BUILD )); then
        if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
            log_fail "--no-build 需要已配置的 TSan 构建目录: ${BUILD_DIR}"
            return 1
        fi
        if ! grep -q '^GNFS_ENABLE_TSAN:BOOL=ON$' "${BUILD_DIR}/CMakeCache.txt"; then
            log_fail "拒绝运行未启用 GNFS_ENABLE_TSAN 的缓存: ${BUILD_DIR}/CMakeCache.txt"
            return 1
        fi
        log_info "跳过配置和编译 (--no-build); 已确认 GNFS_ENABLE_TSAN=ON"
    else
        cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
            -DCMAKE_BUILD_TYPE=Debug \
            -DGNFS_BUILD_TESTS=ON \
            -DGNFS_ENABLE_NATIVE_ARCH=OFF \
            -DGNFS_ENABLE_ASAN=OFF \
            -DGNFS_ENABLE_TSAN=ON \
            -DGNFS_ENABLE_UBSAN=OFF
        cmake --build "$BUILD_DIR" \
            --parallel "$PARALLEL_JOBS" \
            --target "${TSAN_RELATION_TESTS[@]}"
    fi

    local test
    for test in "${TSAN_RELATION_TESTS[@]}"; do
        if [[ ! -x "${BUILD_DIR}/${test}" ]]; then
            log_fail "TSan 测试二进制不存在: ${BUILD_DIR}/${test}"
            return 1
        fi
    done

    # Sanitizer instrumentation can be substantially slower than Debug. Respect
    # an explicit --timeout override; otherwise cap every binary at 120 seconds.
    if (( ! TIMEOUT_EXPLICIT )); then
        TIMEOUT=120
    fi
    export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1:second_deadlock_stack=1}"
    log_info "每测试 timeout=${TIMEOUT}s; TSAN_OPTIONS=${TSAN_OPTIONS}"
    echo ""

    for test in "${TSAN_RELATION_TESTS[@]}"; do
        run_single_test "$test" || true
    done
}

# ============================================================
# 模式: 列表
# ============================================================

do_list() {
    log_header "测试全景"

    echo "${BOLD}模块 ${ARROW} 测试映射:${RESET}"
    echo ""
    printf "  ${BOLD}%-15s %-42s %s${RESET}\n" "模块" "描述" "测试"
    echo "  $(printf '%0.s─' {1..90})"
    for mod in "${MODULE_ORDER[@]}"; do
        printf "  ${CYAN}%-15s${RESET} %-42s %s\n" "$mod" "${MODULE_DESC[$mod]}" "${MODULE_TESTS[$mod]}"
    done

    echo ""
    echo "${BOLD}模块依赖图 (A 变更 ${ARROW} 需要额外测试 B):${RESET}"
    echo ""
    for mod in "${MODULE_ORDER[@]}"; do
        local deps="${MODULE_DEPS[$mod]:-}"
        if [[ -n "$deps" ]]; then
            echo "  ${CYAN}${mod}${RESET} ${ARROW} ${deps}"
        else
            echo "  ${CYAN}${mod}${RESET} ${ARROW} ${DIM}(无下游依赖)${RESET}"
        fi
    done

    echo ""
    echo "${BOLD}冒烟测试集 (${#SMOKE_TESTS[@]} 个):${RESET}"
    for test in "${SMOKE_TESTS[@]}"; do
        echo "  ${BULLET} ${test}"
    done

    echo ""
    echo "${BOLD}特殊测试:${RESET}"
    echo "  ${BULLET} ${CYAN}test_gnfs_e2e${RESET}          — 完整 GNFS 流水线 (N=143)"
    echo "  ${BULLET} ${CYAN}test_gnfs_progressive${RESET}  — 渐进式 L1-L5 (8-61 bit)"
    echo "  ${BULLET} ${CYAN}test_25digit${RESET}           — 25-digit 性能基准 (81 bit)"
    echo "  ${BULLET} ${CYAN}test_stress${RESET}            — 压力测试: 50/60-digit (164-197 bit)"
    echo "  ${BULLET} ${CYAN}test_structured_ooc_50d_probe${RESET} — 有界 50 位 production OOC 前缀探针"

    echo ""
    echo "${BOLD}Sanitizer 窄通道:${RESET}"
    echo "  ${BULLET} ${CYAN}tsan-relation${RESET} — ${#TSAN_RELATION_TESTS[@]} 个 structured relation 并发边界测试"
    for test in "${TSAN_RELATION_TESTS[@]}"; do
        echo "      ${DIM}${test}${RESET}"
    done

    echo ""
    echo "${BOLD}所有测试二进制 (${#ALL_TEST_BINARIES[@]}):${RESET}"
    echo ""
    echo "  ${BOLD}     测试名                          分级       超时${RESET}"
    echo "  $(printf '%0.s─' {1..60})"
    local _em _tier _tout _tc
    for t in "${ALL_TEST_BINARIES[@]}"; do
        if [[ -x "${BUILD_DIR}/${t}" ]]; then
            _em="${GREEN}${CHECK}${RESET}"
        else
            _em="${DIM}?${RESET}"
        fi
        _tier="${TEST_TIER[$t]:-unknown}"
        _tout="${TEST_TIMEOUT[$t]:-???}"
        _tc=""
        case "$_tier" in
            instant) _tc="${GREEN}" ;;
            fast)    _tc="${CYAN}" ;;
            gate)    _tc="${MAGENTA}" ;;
            slow)    _tc="${YELLOW}" ;;
            heavy)   _tc="${RED}" ;;
            stress)  _tc="${RED}" ;;
            bench)   _tc="${MAGENTA}" ;;
            *)       _tc="${DIM}" ;;
        esac
        echo "  ${_em} $(printf '%-32s' "$t") ${_tc}$(printf '%-10s' "$_tier")${RESET} ${_tout}s"
    done

    echo ""
    echo "${BOLD}测试层级金字塔:${RESET}"
    echo ""
    echo "           ${DIM}▲ 耗时${RESET}"
    echo "           │"
    echo "           │   ${RED}stress${RESET}         50/60-digit 压力测试"
    echo "           │   ${MAGENTA}bench${RESET}          informational micro-bench"
    echo "           │   ${MAGENTA}nightly${RESET}        thorough + L4 + L5 + stress L1"
    echo "           │   ${MAGENTA}thorough${RESET}       全模块 + 集成 + L1-L3"
    echo "           │   ${YELLOW}full${RESET}           ctest + E2E + L1-L2"
    echo "           │   ${MAGENTA}gate${RESET}           smoke + 17/27/40/81-bit 回归"
    echo "           │   ${CYAN}integration${RESET}    跨模块交互"
    echo "           │   ${CYAN}pipeline${RESET}       流水线阶段逐验证"
    echo "           │   ${GREEN}unit${RESET}           ctest 单元测试"
    echo "           │   ${GREEN}module <m>${RESET}     指定模块"
    echo "           │   ${GREEN}smoke${RESET}          instant 核心子集"
    echo "           └────────────────────────── ${DIM}覆盖范围 ▶${RESET}"
}

do_matrix() {
    log_header "模块 × 测试 映射矩阵"

    # 收集所有测试 (按模块顺序)
    typeset -a all_tests
    for mod in "${MODULE_ORDER[@]}"; do
        for test in ${(s: :)MODULE_TESTS[$mod]}; do
            all_tests+=("$test")
        done
    done

    # 表头
    printf "${BOLD}%-15s${RESET}" "模块"
    for test in "${all_tests[@]}"; do
        local short="${test#test_}"
        printf " %-5s" "${short[1,5]}"
    done
    echo ""
    echo "$(printf '%0.s─' {1..90})"

    for mod in "${MODULE_ORDER[@]}"; do
        printf "${CYAN}%-15s${RESET}" "$mod"
        for test in "${all_tests[@]}"; do
            if [[ "${MODULE_TESTS[$mod]}" == *"$test"* ]]; then
                printf " ${GREEN}  ●  ${RESET}"
            else
                printf " ${DIM}  ·  ${RESET}"
            fi
        done
        echo ""
    done
}

# ============================================================
# 参数解析
# ============================================================

MODE=""
typeset -a MODE_ARGS
MODE_ARGS=()
typeset -a BENCH_EXTRA_ARGS
BENCH_EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -j)          PARALLEL_JOBS="$2"; shift 2 ;;
        -t)          BUILD_TYPE="$2"; BUILD_TYPE_EXPLICIT=1; shift 2 ;;
        -v)          VERBOSE=1; shift ;;
        -q)          QUIET=1; shift ;;
        --no-build)  SKIP_BUILD=1; shift ;;
        --no-color)  USE_COLOR=0; setup_colors; shift ;;
        --fail-fast) FAIL_FAST=1; shift ;;
        --timeout)   TIMEOUT="$2"; TIMEOUT_EXPLICIT=1; shift 2 ;;
        --retry)     RETRY_COUNT="$2"; shift 2 ;;
        --save)      BENCH_EXTRA_ARGS+=(--save); shift ;;
        --compare)   BENCH_EXTRA_ARGS+=(--compare); shift ;;
        --deep)      MODE_ARGS+=(--deep); shift ;;
        --slow)      MODE_ARGS+=(--slow); shift ;;
        -h|--help)
            # 显示帮助 — 用 awk 而非 sed (BSD sed 'bad flag in {...}' 不兼容)
            awk '/^set -eo/{exit} /^#/{sub(/^# ?/, ""); print}' "$0"
            exit 0 ;;
        *)
            if [[ -z "$MODE" ]]; then
                MODE="$1"
            else
                MODE_ARGS+=("$1")
            fi
            shift ;;
    esac
done

# 默认模式
[[ -z "$MODE" ]] && MODE="smoke"

# ============================================================
# 主调度
# ============================================================

OVERALL_START_MS=$(timer_start_ms)

case "$MODE" in
    build)
        do_build
        ;;

    smoke)
        do_build
        do_smoke
        show_summary
        ;;

    unit|ctest)
        do_build
        do_ctest
        ;;

    module|mod)
        if [[ ${#MODE_ARGS[@]} -eq 0 ]]; then
            log_fail "用法: $0 module <模块名> [模块名...]"
            log_info "可用模块: ${(k)MODULE_TESTS}"
            log_info "使用 'module all' 测试全部模块"
            exit 1
        fi
        do_build
        do_module "${MODE_ARGS[@]}"
        show_summary
        ;;

    run)
        if [[ ${#MODE_ARGS[@]} -eq 0 ]]; then
            log_fail "用法: $0 run <test_name> [args...]"
            exit 1
        fi
        do_build
        do_run "${MODE_ARGS[@]}"
        show_summary
        ;;

    e2e)
        do_build
        do_e2e
        show_summary
        ;;

    integration|integ)
        do_build
        do_integration
        show_summary
        ;;

    pipeline|pipe)
        do_build
        do_pipeline
        show_summary
        ;;

    progressive|prog)
        do_build
        do_progressive "${MODE_ARGS[@]}"
        show_summary
        ;;

    L1) do_build; do_progressive 1 1; show_summary ;;
    L2) do_build; do_progressive 2 2; show_summary ;;
    L3) do_build; do_progressive 3 3; show_summary ;;
    L4) do_build; do_progressive 4 4; show_summary ;;
    L5) do_build; do_progressive 5 5; show_summary ;;

    changed|diff)
        do_build
        do_changed "${MODE_ARGS[@]}"
        show_summary
        ;;

    full)
        do_build
        do_full
        show_summary
        ;;

    thorough)
        do_build
        do_thorough
        show_summary
        ;;

    nightly)
        do_build
        do_nightly
        show_summary
        ;;

    gate)
        do_build
        do_gate "${MODE_ARGS[@]}"
        show_summary
        ;;

    tsan-relation)
        do_tsan_relation
        show_summary
        ;;

    structured-ooc-rss)
        if [[ ${#MODE_ARGS[@]} -ne 2 ]]; then
            log_fail "用法: $0 structured-ooc-rss <rows> <workers>"
            log_info "rows=5000|50000|200000; workers=1|2|4"
            exit 1
        fi
        local _rss_rows="${MODE_ARGS[1]}"
        local _rss_workers="${MODE_ARGS[2]}"
        local _rss_valid=1
        case "$_rss_rows" in 5000|50000|200000) ;; *) _rss_valid=0 ;; esac
        case "$_rss_workers" in 1|2|4) ;; *) _rss_valid=0 ;; esac
        if (( ! _rss_valid )); then
            log_fail "非法 RSS 场景: rows=${_rss_rows}, workers=${_rss_workers}"
            exit 1
        fi
        if (( ! BUILD_TYPE_EXPLICIT )); then
            BUILD_TYPE="Release"
        fi
        if (( SKIP_BUILD )); then
            log_fail "structured-ooc-rss 不接受 --no-build；资源证据必须由本次请求的构建生成"
            exit 1
        fi
        do_build
        if [[ ! -x "${BUILD_DIR}/test_structured_ooc_scale" ]]; then
            log_fail "资源测量二进制不存在: ${BUILD_DIR}/test_structured_ooc_scale"
            exit 1
        fi
        log_header "Structured OOC 独立进程 RSS"
        local _rss_status=0
        run_single_test test_structured_ooc_scale --rss-case "$_rss_rows" "$_rss_workers" ||
            _rss_status=$?
        if (( _rss_status == 0 )); then
            if capture_single_measurement_record "GNFS_RESOURCE_V1 " "Structured OOC RSS"; then
                print -r -- "$MEASUREMENT_RECORD"
            else
                (( FAILED_TESTS += 1 ))
            fi
        fi
        show_summary
        ;;

    probe-50d-structured-ooc)
        if [[ ${#MODE_ARGS[@]} -gt 1 ]]; then
            log_fail "用法: $0 probe-50d-structured-ooc [max_special_q]"
            exit 1
        fi
        local _probe_max_special_q="${MODE_ARGS[1]:-4}"
        if [[ ! "$_probe_max_special_q" =~ ^[0-9]+$ ]] ||
           (( _probe_max_special_q < 1 || _probe_max_special_q > 64 )); then
            log_fail "max_special_q 必须在 1..64 (传入: ${_probe_max_special_q})"
            exit 1
        fi
        if (( ! BUILD_TYPE_EXPLICIT )); then
            BUILD_TYPE="Release"
        fi
        if (( SKIP_BUILD )); then
            log_fail "probe-50d-structured-ooc 不接受 --no-build；资源证据必须由本次请求的构建生成"
            exit 1
        fi
        do_build
        if [[ ! -x "${BUILD_DIR}/test_structured_ooc_50d_probe" ]]; then
            log_fail "50 位探针二进制不存在: ${BUILD_DIR}/test_structured_ooc_50d_probe"
            exit 1
        fi
        local _probe_dir
        _probe_dir=$(mktemp -d "${TMPDIR:-/tmp}/gnfs_structured_ooc_50d.XXXXXX")
        local _probe_base="${_probe_dir}/raw"
        log_header "有界 50 位 production structured OOC 探针"
        log_info "max_special_q=${_probe_max_special_q}; 临时目录=${_probe_dir}"
        local _probe_status=0
        run_single_test test_structured_ooc_50d_probe --max-special-q \
            "$_probe_max_special_q" --ooc-base "$_probe_base" || _probe_status=$?
        if (( _probe_status == 0 )); then
            if capture_single_measurement_record "GNFS_EXPERIMENT_V1 " "50 位探针"; then
                print -r -- "$MEASUREMENT_RECORD"
            else
                (( FAILED_TESTS += 1 ))
            fi
            if rmdir "$_probe_dir"; then
                log_success "探针工件已完成生命周期清理"
            else
                log_fail "探针成功但临时目录非空，已保留: ${_probe_dir}"
                (( FAILED_TESTS += 1 ))
            fi
        else
            log_warn "探针失败，保留诊断工件: ${_probe_dir}"
        fi
        show_summary
        ;;

    perf)
        do_build
        log_header "性能测试 (25-digit)"
        log_warn "预计耗时数分钟..."
        run_single_test test_25digit
        show_summary
        ;;

    pgo-train)
        # PGO 训练完整工作流（见 docs/perf/performance-doctrine.md §5.3）
        # 输出: build-pgo-use/test_* (PGO-optimized)
        log_header "PGO 训练 (Profile-Guided Optimization)"
        log_info "四阶段: instrumented build -> training run -> merge -> optimized build"
        log_info "预计耗时 5-15 分钟"
        exec "${PROJECT_ROOT}/scripts/pgo-train.sh" "${MODE_ARGS[@]}"
        ;;

    profile)
        # Instruments + xctrace 抓 CPU PMU trace（见 doctrine §5.4）
        # 用法: ./scripts/test.sh profile <test_name> [args...]
        # 例:   ./scripts/test.sh profile factor_with_kleinjung
        if [[ ${#MODE_ARGS[@]} -eq 0 ]]; then
            log_fail "用法: $0 profile <test_name> [args...]"
            log_info "例: $0 profile factor_with_kleinjung"
            exit 1
        fi
        do_build
        local _test_name="${MODE_ARGS[1]}"
        local _test_bin="${BUILD_DIR}/test_${_test_name}"
        if [[ ! -x "${_test_bin}" ]]; then
            # 允许带或不带 test_ 前缀
            _test_bin="${BUILD_DIR}/${_test_name}"
        fi
        if [[ ! -x "${_test_bin}" ]]; then
            log_fail "测试二进制不存在: ${_test_bin}"
            exit 1
        fi
        log_header "性能采集 (Instruments CPU Counters)"
        log_info "目标: ${_test_bin}"
        # zsh 数组是 1-indexed; [2,-1] 是 "从第 2 个到末尾" 的 slice
        exec "${PROJECT_ROOT}/scripts/perf/profile-cpu.sh" "${_test_bin}" "${MODE_ARGS[@]:1}"
        ;;

    pmu)
        # mperf 直接读 M5 PMU 计数器（10 events，见 doctrine §5 + P1.A 计划）
        # 用法: ./scripts/test.sh pmu <test_name> [args...]
        # 例:   ./scripts/test.sh pmu factor_with_kleinjung
        # 需要 sudo（kpc API 内核限制）。先跑 ./scripts/perf/install-mperf.sh 一次。
        if [[ ${#MODE_ARGS[@]} -eq 0 ]]; then
            log_fail "用法: $0 pmu <test_name> [args...]"
            log_info "例: $0 pmu factor_with_kleinjung"
            log_info "首次需安装: ./scripts/perf/install-mperf.sh"
            exit 1
        fi
        do_build
        local _test_name="${MODE_ARGS[1]}"
        local _test_bin="${BUILD_DIR}/test_${_test_name}"
        if [[ ! -x "${_test_bin}" ]]; then
            _test_bin="${BUILD_DIR}/${_test_name}"
        fi
        if [[ ! -x "${_test_bin}" ]]; then
            log_fail "测试二进制不存在: ${_test_bin}"
            exit 1
        fi
        log_header "PMU 采集 (mperf, GNFS P1.A 10 events)"
        log_info "目标: ${_test_bin}"
        log_warn "需要 sudo (kpc API)"
        exec "${PROJECT_ROOT}/scripts/perf/pmu-stat.sh" "${_test_bin}" "${MODE_ARGS[@]:1}"
        ;;

    stress)
        do_build
        log_header "压力测试 (50/60-digit)"
        log_warn "50-digit 可能需要数小时, 60-digit 可能需要十几小时..."
        local stress_min=${MODE_ARGS[1]:-1}
        local stress_max=${MODE_ARGS[2]:-2}
        run_single_test test_stress "$stress_min" "$stress_max"
        show_summary
        ;;

    bench-ram)
        # 后台启动 test_stress + /usr/bin/time -l, 收集 RAM peak baseline (P3-2 工作流).
        # 用法: ./scripts/test.sh bench-ram <level>
        # 例:   ./scripts/test.sh bench-ram 1   # 50-digit (≈ 2h)
        #       ./scripts/test.sh bench-ram 2   # 60-digit (hours+)
        if [[ ${#MODE_ARGS[@]} -eq 0 ]]; then
            log_fail "用法: $0 bench-ram <level>"
            log_info "level=1: 50-digit (164-bit, ≈ 2h)"
            log_info "level=2: 60-digit (197-bit, hours+)"
            log_info "level=3: 70-digit / level=4: 80-digit / level=5: 90-digit"
            log_info "建议预先 build-release:"
            log_info "  cmake -B build-release -DCMAKE_BUILD_TYPE=Release && make -C build-release test_stress"
            exit 1
        fi
        local _level="${MODE_ARGS[1]}"
        if [[ ! "$_level" =~ ^[1-5]$ ]]; then
            log_fail "level 必须为 1-5 (传入: $_level)"
            exit 1
        fi
        local _bin_dir
        if [[ -x "${PROJECT_ROOT}/build-release/test_stress" ]]; then
            _bin_dir="${PROJECT_ROOT}/build-release"
        elif [[ -x "${BUILD_DIR}/test_stress" ]]; then
            _bin_dir="${BUILD_DIR}"
            log_warn "未找到 build-release/test_stress; 用 ${BUILD_DIR} (建议 Release 减少 wall time)"
        else
            log_fail "找不到 test_stress 二进制. 先 build:"
            log_info "  cmake -B build-release -DCMAKE_BUILD_TYPE=Release"
            log_info "  make -C build-release test_stress"
            exit 1
        fi
        local _digit_name _short
        case "$_level" in
            1) _digit_name="50-digit (L1, 164-bit)"; _short="50d" ;;
            2) _digit_name="60-digit (L2, 197-bit)"; _short="60d" ;;
            3) _digit_name="70-digit (L3, 231-bit)"; _short="70d" ;;
            4) _digit_name="80-digit (L4, 264-bit)"; _short="80d" ;;
            5) _digit_name="90-digit (L5, 298-bit)"; _short="90d" ;;
        esac
        local _log="/tmp/p3_ram_profile_${_short}.log"
        log_header "RAM Baseline 收集 ($_digit_name)"
        log_info "二进制: ${_bin_dir}/test_stress 1 $_level"
        log_info "日志: $_log"
        log_warn "后台启动 (nohup + /usr/bin/time -l), 预计 hours+, 不阻塞此 shell"
        nohup /usr/bin/time -l "${_bin_dir}/test_stress" 1 "$_level" > "$_log" 2>&1 &
        local _pid=$!
        echo "PID=$_pid LOG=$_log task=P3-2_RAM_baseline_${_short} start=$(date '+%Y-%m-%d_%H:%M:%S')" >> /tmp/bg_tasks.txt
        sleep 1
        log_info ""
        log_info "PID=$_pid 已启动 (注: /usr/bin/time 是 wrapper, child test_stress 通过 pgrep -P 查)"
        log_info ""
        log_info "监控命令:"
        log_info "  tail -f $_log"
        log_info "  pgrep -P $_pid && ps -p \$(pgrep -P $_pid) -o pid,%cpu,rss,etime"
        log_info ""
        log_info "完成后解析:"
        log_info "  ./scripts/perf/parse-time-l.sh $_log \"$_digit_name baseline\""
        ;;

    bench|benchmark)
        do_build
        do_bench "${BENCH_EXTRA_ARGS[@]}"
        show_summary
        ;;

    watch)
        do_watch "${MODE_ARGS[@]}"
        ;;

    report)
        do_build
        do_full
        local overall_end_ms=$(timer_start_ms)
        write_report "$((overall_end_ms - OVERALL_START_MS))"
        show_summary
        ;;

    list|ls)
        do_list
        ;;

    matrix)
        do_matrix
        ;;

    *)
        log_fail "未知模式: ${MODE}"
        echo "运行 '$0 --help' 查看完整用法"
        echo ""
        echo "常用模式: smoke | unit | module | e2e | gate | tsan-relation | changed | full | list"
        exit 1
        ;;
esac

# 自动写入报告
case "$MODE" in
    report|list|ls|matrix|watch|build) ;;
    *)
        if (( TOTAL_TESTS > 0 )); then
            local overall_end_ms=$(timer_start_ms)
            write_report "$((overall_end_ms - OVERALL_START_MS))" 2>/dev/null || true
        fi
        ;;
esac

exit $FAILED_TESTS

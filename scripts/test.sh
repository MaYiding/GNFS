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
#   ./scripts/test.sh tsan-relation       # 窄 ThreadSanitizer 并发门禁
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
#   ./scripts/test.sh probe-50d-special-q-workers
#                                         # 真实 50 位，外层 SQ workers=1/2/4 对照
#   ./scripts/test.sh sweep-50d-candidate-batch
#                                         # 固定 4-SQ candidate worker/chunk 扫测
#   ./scripts/test.sh bench-squfof
#                                         # 固定 50 位 SQUFOF multiplier/吞吐基准
#   ./scripts/test.sh bench-siqs-shadow <mode> [options]
#                                         # Release-only SIQS shadow matrix 可复现基准
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
    test_candidate_chunk_plan
    test_candidate_batch
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
    test_structured_filter_pipeline_120bit
    test_structured_ooc_50d_probe
    test_candidate_batch_50d_sweep
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
    test_squfof_budget_corpus
    test_squfof_success_challenge_corpus
    test_squfof_success_challenge_oracle
    test_squfof_budget_oracle
    test_squfof_strategy_oracle
    test_squfof_bench
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
    test_local_sieve_thread_budget
    test_siqs
    test_siqs_2lp
    test_siqs_2lp_graph
    test_siqs_2lp_materializer
    test_siqs_2lp_adapter
    test_siqs_2lp_congruence
    test_siqs_post_merge_row
    test_siqs_shadow_assembly
    test_siqs_shadow_linear_algebra
    test_siqs_shadow_cross_size
    test_siqs_shadow_matrix_bench
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
    sieve          "test_special_q test_sieve_basic test_sieve_checkpoint test_distributed_sieve test_bucket_sieve test_sieve_ecore_qos test_local_sieve_thread_budget test_lll_lattice test_adaptive_lattice test_sieve_tiny_simd test_bucket_prefetch test_sieve_region_tile test_sieve_norm_tile test_lattice_basis_parallel test_sieve_apply_tile_parallel test_lattice_coords_simd test_threshold_scan_simd test_saturated_sub_simd"
    cofactor       "test_cofactor test_candidate_chunk_plan test_candidate_batch test_squfof test_squfof_budget_corpus test_squfof_success_challenge_corpus test_squfof_success_challenge_oracle test_squfof_budget_oracle test_squfof_strategy_oracle test_brent_pollard_rho test_brent_pollard_rho_parallel test_survival_predictor test_batch_ecm test_3lp_cofactor test_trial_wheel test_batch_trial test_ecm_curve_pool test_sigma_seed_pool test_ecm_stage2_parallel test_ecm_stage1_parallel test_batch_inversion test_trial_div_simd test_cofactor_stage_timing test_ecm_prime_cache test_cofactor_result_cache test_integration test_ecm_brent_suyama"
    relation       "test_relation_collector test_relation_corpus test_relation_sink test_ooc_store_integrity test_filter test_lp_key_contract test_relation_identity test_relation_reduction_engine test_structured_filter test_structured_filter_policy test_structured_tree_basis test_structured_tree_basis_property test_structured_budgeted_driver test_structured_conflict_batch test_structured_parallel_prepare test_structured_batch_commit test_structured_parallel_driver test_structured_parallel_failures test_structured_incidence_builder test_structured_materialization test_filter_radix_sort test_lp_bloom test_lp_key_hash test_merger_parallel test_clique_merger test_clique_merger_50d_synthetic test_3lp_merge test_ooc_relations test_ooc_policy test_v0_bfs_policy test_integration test_relation_pool_integration"
    linalg         "test_linalg test_sge_batch_pivots test_block_wiedemann test_bw_rank_est test_matrix_diagnostics test_sge_streaming test_mmap_csr test_schirokauer_deg4 test_schirokauer_strip test_schirokauer_parallel test_edge_cases test_integration test_matrix_view_concept test_save_sparse_as_mmap test_linalg_mmap_policy test_bw_krylov_parallel test_metal_spmv test_spmv_simd test_transpose_blocked test_popcount_simd test_and_popcnt_simd test_xor_words_simd test_and_words_simd test_xor_popcnt_simd test_row_popcount_simd test_krylov_compress test_krylov_compression test_bl_checkpoint test_bl_resume_integration test_linalg_progress"
    integration    "test_integration"
    sqrt           "test_sqrt test_sqrt_debug test_hensel_parallel test_class_group test_couveignes_large_class_group test_couveignes_parallel"
    api            "test_i18n test_method_selection test_relation_reduction_engine"
    siqs           "test_siqs test_siqs_2lp test_siqs_2lp_graph test_siqs_2lp_materializer test_siqs_2lp_adapter test_siqs_2lp_congruence test_siqs_post_merge_row test_siqs_shadow_assembly test_siqs_shadow_linear_algebra test_siqs_shadow_cross_size"
)

# 模块 → 慢速测试映射 (slow+heavy, 可选运行)
typeset -A MODULE_SLOW_TESTS
MODULE_SLOW_TESTS=(
    polynomial     "test_kleinjung test_kleinjung_large test_factor_with_kleinjung"
    sieve          "test_lattice_sieve"
    cofactor       "test_ecm_brent_suyama_bench"
    relation       "test_structured_ooc_scale test_structured_filter_pipeline_120bit"
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
    test_local_sieve_thread_budget
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
    test_candidate_chunk_plan
    test_candidate_batch
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
    test_squfof_budget_corpus
    test_squfof_success_challenge_corpus
    test_brent_pollard_rho
    test_survival_predictor
    test_i18n
    test_method_selection
    test_siqs_2lp
    test_siqs_2lp_graph
    test_siqs_2lp_materializer
    test_siqs_2lp_adapter
    test_siqs_2lp_congruence
    test_siqs_post_merge_row
    test_siqs_shadow_assembly
    test_siqs_shadow_linear_algebra
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

# ThreadSanitizer 窄通道: 覆盖候选级 cofactor 调度、structured relation 有界并发和
# SIQS shadow 持久消元 worker 的发布/收敛/故障边界。保持此列表小而明确，
# 避免把完整 instant 层复制到高成本的 sanitizer 构建。
typeset -a TSAN_RELATION_TESTS
TSAN_RELATION_TESTS=(
    test_ordered_parallel_map
    test_candidate_batch
    test_relation_collector
    test_relation_reduction_engine
    test_structured_parallel_prepare
    test_structured_batch_commit
    test_structured_parallel_driver
    test_structured_parallel_failures
    test_structured_incidence_builder
    test_siqs_shadow_linear_algebra
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
    test_local_sieve_thread_budget 10
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
    test_candidate_chunk_plan 10
    test_candidate_batch     10
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
    test_structured_filter_pipeline_120bit 600
    test_structured_ooc_50d_probe 3600
    test_candidate_batch_50d_sweep 900
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
    test_squfof_budget_corpus 10
    test_squfof_success_challenge_corpus 10
    test_squfof_success_challenge_oracle 120
    test_squfof_budget_oracle 120
    test_squfof_strategy_oracle 60
    test_squfof_bench        120
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
    test_siqs_2lp            10
    test_siqs_2lp_graph      10
    test_siqs_2lp_materializer 10
    test_siqs_2lp_adapter    10
    test_siqs_2lp_congruence 10
    test_siqs_post_merge_row 10
    test_siqs_shadow_assembly 10
    test_siqs_shadow_linear_algebra 10
    test_siqs_shadow_cross_size 60
    test_siqs_shadow_matrix_bench 600
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
    test_local_sieve_thread_budget "instant"
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
    test_candidate_chunk_plan "instant"
    test_candidate_batch     "instant"
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
    test_structured_filter_pipeline_120bit "heavy"
    test_structured_ooc_50d_probe "stress"
    test_candidate_batch_50d_sweep "bench"
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
    test_squfof_budget_corpus "instant"
    test_squfof_success_challenge_corpus "instant"
    test_squfof_success_challenge_oracle "fast"
    test_squfof_budget_oracle "fast"
    test_squfof_strategy_oracle "fast"
    test_squfof_bench        "bench"
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
    test_siqs_2lp            "instant"
    test_siqs_2lp_graph      "instant"
    test_siqs_2lp_materializer "instant"
    test_siqs_2lp_adapter    "instant"
    test_siqs_2lp_congruence "instant"
    test_siqs_post_merge_row "instant"
    test_siqs_shadow_assembly "instant"
    test_siqs_shadow_linear_algebra "instant"
    test_siqs_shadow_cross_size "fast"
    test_siqs_shadow_matrix_bench "bench"
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
MODULE_ORDER=(core util polynomial factor_base sieve cofactor relation linalg sqrt siqs api)

# 文件路径 → 模块映射
path_to_module() {
    local path="$1"
    case "$path" in
        tests/test_squfof*.cpp|tests/support/squfof_*.hpp|tests/fixtures/squfof_*.hpp) echo "cofactor" ;;
        tests/test_structured*.cpp) echo "relation" ;;
        tests/test_siqs*.cpp|*siqs/*) echo "siqs" ;;
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
    if (( !QUIET )) && [[ "$tier" == "gate" || "$tier" == "slow" || "$tier" == "heavy" || "$tier" == "stress" || "$tier" == "bench" ]]; then
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
    if (( !QUIET )) && [[ "$tier" == "gate" || "$tier" == "slow" || "$tier" == "heavy" || "$tier" == "stress" || "$tier" == "bench" ]]; then
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

measurement_record_field() {
    local record="$1"
    local key="$2"
    printf '%s\n' "$record" | awk -v key="$key" '
        {
            for (i = 1; i <= NF; ++i) {
                if (index($i, key "=") == 1) {
                    print substr($i, length(key) + 2)
                    matches += 1
                }
            }
        }
        END { if (matches != 1) exit 1 }
    '
}

# Validate the complete V1 contract emitted by the Release-only SQUFOF strategy
# benchmark. SUMMARY owns the dynamic multiplier count; CASE and MULTIPLIER
# records must agree with its corpus and deterministic identity fields.
validate_squfof_bench_output() {
    local expected_build_type="$1"
    local expected_repetitions="$2"

    printf '%s\n' "$RUN_OUTPUT" | awk \
        -v expected_build_type="$expected_build_type" \
        -v expected_repetitions="$expected_repetitions" '
        function clear_record(key) {
            for (key in record) delete record[key]
            for (key in occurrences) delete occurrences[key]
        }
        function parse_record(token_index, equals, key, value) {
            clear_record()
            for (token_index = 2; token_index <= NF; ++token_index) {
                equals = index($token_index, "=")
                if (equals <= 1) {
                    invalid = 1
                    continue
                }
                key = substr($token_index, 1, equals - 1)
                value = substr($token_index, equals + 1)
                if (key !~ /^[A-Za-z][A-Za-z0-9_]*$/ || value == "") {
                    invalid = 1
                    continue
                }
                record[key] = value
                if (++occurrences[key] != 1) invalid = 1
            }
        }
        function canonical_decimal(text) {
            return text == "0" || text ~ /^[1-9][0-9]*$/
        }
        function positive_decimal(text) {
            return text ~ /^[1-9][0-9]*$/
        }
        function u64(text, limit, digit_index, actual_digit, limit_digit) {
            if (!canonical_decimal(text)) return 0
            if (length(text) < 20) return 1
            if (length(text) > 20) return 0
            limit = "18446744073709551615"
            for (digit_index = 1; digit_index <= 20; ++digit_index) {
                actual_digit = substr(text, digit_index, 1) + 0
                limit_digit = substr(limit, digit_index, 1) + 0
                if (actual_digit < limit_digit) return 1
                if (actual_digit > limit_digit) return 0
            }
            return 1
        }
        function valid_bitmap(text, case_count, failure_count,
                              expected_length, remainder, last_digit,
                              bitmap_index, digit, population) {
            if (text !~ /^[0-9a-f]+$/) return 0
            expected_length = int((case_count + 3) / 4)
            if (length(text) != expected_length) return 0

            population = 0
            for (bitmap_index = 1; bitmap_index <= length(text); ++bitmap_index) {
                digit = substr(text, bitmap_index, 1)
                population += hex_population[digit]
            }
            if (population != failure_count) return 0

            remainder = case_count % 4
            last_digit = substr(text, length(text), 1)
            if (remainder == 1 && last_digit !~ /^[01]$/) return 0
            if (remainder == 2 && last_digit !~ /^[0-3]$/) return 0
            if (remainder == 3 && last_digit !~ /^[0-7]$/) return 0
            return 1
        }
        BEGIN {
            case_prefix = "GNFS_SQUFOF_BENCH_CASE_V1 "
            multiplier_prefix = "GNFS_SQUFOF_BENCH_MULTIPLIER_V1 "
            summary_prefix = "GNFS_SQUFOF_BENCH_SUMMARY_V1 "
            expected_scope = "fixed_50d_strategy_corpus"
            expected_corpus = "fixed_50d_squfof_strategy_v1"
            expected_claim_boundary = "whole_squfof_factor_call"
            expected_timing_scope = "factor_calls_plus_preallocated_result_store"

            case_required_count = split("status scope corpus build_type timing_scope timing_asserted diagnostics_timed repetition cases calls successes failures invalid_factors wall_ns corpus_digest_low corpus_digest_high factor_digest_low factor_digest_high success_digest_low success_digest_high failure_digest_low failure_digest_high failure_bitmap_hex schedule_digest_low schedule_digest_high", case_required, " ")
            multiplier_required_count = split("status scope corpus build_type diagnostics_timed slot multiplier attempts forward_iterations core_hits accepted_hits overflow_skips schedule_digest_low schedule_digest_high", multiplier_required, " ")
            summary_required_count = split("status scope corpus build_type claim_boundary timing_scope timing_asserted diagnostics_timed cases repetitions measured_calls successes_per_repetition failures_per_repetition invalid_factors wall_min_ns wall_median_ns wall_max_ns multiplier_count corpus_digest_low corpus_digest_high factor_digest_low factor_digest_high success_digest_low success_digest_high failure_digest_low failure_digest_high failure_bitmap_hex schedule_digest_low schedule_digest_high", summary_required, " ")
            case_identity_count = split("scope corpus build_type timing_scope timing_asserted diagnostics_timed cases successes failures invalid_factors corpus_digest_low corpus_digest_high factor_digest_low factor_digest_high success_digest_low success_digest_high failure_digest_low failure_digest_high failure_bitmap_hex schedule_digest_low schedule_digest_high", case_identity, " ")
            digest_count = split("corpus_digest_low corpus_digest_high factor_digest_low factor_digest_high success_digest_low success_digest_high failure_digest_low failure_digest_high schedule_digest_low schedule_digest_high", digest_fields, " ")

            hex_population["0"] = 0
            hex_population["1"] = 1
            hex_population["2"] = 1
            hex_population["3"] = 2
            hex_population["4"] = 1
            hex_population["5"] = 2
            hex_population["6"] = 2
            hex_population["7"] = 3
            hex_population["8"] = 1
            hex_population["9"] = 2
            hex_population["a"] = 2
            hex_population["b"] = 3
            hex_population["c"] = 2
            hex_population["d"] = 3
            hex_population["e"] = 3
            hex_population["f"] = 4
        }
        index($0, case_prefix) == 1 {
            if (multiplier_records != 0 || summaries != 0) invalid = 1
            case_records += 1
            parse_record()
            for (field_index = 1; field_index <= case_required_count; ++field_index) {
                if (occurrences[case_required[field_index]] != 1) invalid = 1
            }

            repetition = record["repetition"]
            corpus_cases = record["cases"]
            successes = record["successes"]
            failures = record["failures"]
            if (record["status"] != "pass" || record["scope"] != expected_scope ||
                record["corpus"] != expected_corpus ||
                record["build_type"] != expected_build_type ||
                record["timing_scope"] != expected_timing_scope ||
                record["timing_asserted"] != "false" ||
                record["diagnostics_timed"] != "false" ||
                !positive_decimal(repetition) || repetition + 0 > expected_repetitions ||
                seen_repetition[repetition]++ || !positive_decimal(corpus_cases) ||
                !canonical_decimal(successes) || !canonical_decimal(failures) ||
                successes + failures != corpus_cases + 0 ||
                record["calls"] != corpus_cases || record["invalid_factors"] != "0" ||
                !u64(record["wall_ns"]) || record["wall_ns"] == "0" ||
                !valid_bitmap(record["failure_bitmap_hex"], corpus_cases + 0,
                              failures + 0)) {
                invalid = 1
            }
            for (field_index = 1; field_index <= digest_count; ++field_index) {
                if (!u64(record[digest_fields[field_index]])) invalid = 1
            }

            if (case_records == 1) {
                for (field_index = 1; field_index <= case_identity_count; ++field_index) {
                    field = case_identity[field_index]
                    case_reference[field] = record[field]
                }
            } else {
                for (field_index = 1; field_index <= case_identity_count; ++field_index) {
                    field = case_identity[field_index]
                    if (record[field] != case_reference[field]) invalid = 1
                }
            }
            case_wall[repetition + 0] = record["wall_ns"] + 0
        }
        index($0, multiplier_prefix) == 1 {
            if (summaries != 0) invalid = 1
            multiplier_records += 1
            parse_record()
            for (field_index = 1; field_index <= multiplier_required_count; ++field_index) {
                if (occurrences[multiplier_required[field_index]] != 1) invalid = 1
            }

            slot = record["slot"]
            attempts = record["attempts"]
            core_hits = record["core_hits"]
            accepted_hits = record["accepted_hits"]
            overflow_skips = record["overflow_skips"]
            if (record["status"] != "pass" || record["scope"] != expected_scope ||
                record["corpus"] != expected_corpus ||
                record["build_type"] != expected_build_type ||
                record["diagnostics_timed"] != "false" ||
                !canonical_decimal(slot) || seen_slot[slot]++ ||
                !u64(record["multiplier"]) || record["multiplier"] == "0" ||
                seen_multiplier[record["multiplier"]]++ ||
                !u64(attempts) || !u64(record["forward_iterations"]) ||
                !u64(core_hits) || !u64(accepted_hits) || !u64(overflow_skips) ||
                core_hits + 0 > attempts + 0 || accepted_hits + 0 > core_hits + 0 ||
                overflow_skips + 0 > case_reference["cases"] + 0 ||
                attempts + 0 > case_reference["cases"] + 0 ||
                attempts + overflow_skips > case_reference["cases"] + 0 ||
                !u64(record["schedule_digest_low"]) ||
                !u64(record["schedule_digest_high"])) {
                invalid = 1
            }
            accepted_total += accepted_hits + 0

            if (multiplier_records == 1) {
                multiplier_scope = record["scope"]
                multiplier_corpus = record["corpus"]
                multiplier_build_type = record["build_type"]
                multiplier_diagnostics_timed = record["diagnostics_timed"]
                schedule_digest_low = record["schedule_digest_low"]
                schedule_digest_high = record["schedule_digest_high"]
            } else if (record["scope"] != multiplier_scope ||
                       record["corpus"] != multiplier_corpus ||
                       record["build_type"] != multiplier_build_type ||
                       record["diagnostics_timed"] != multiplier_diagnostics_timed ||
                       record["schedule_digest_low"] != schedule_digest_low ||
                       record["schedule_digest_high"] != schedule_digest_high) {
                invalid = 1
            }
        }
        index($0, summary_prefix) == 1 {
            summaries += 1
            parse_record()
            for (field_index = 1; field_index <= summary_required_count; ++field_index) {
                if (occurrences[summary_required[field_index]] != 1) invalid = 1
            }

            if (summaries != 1 || record["status"] != "pass" ||
                record["scope"] != expected_scope || record["corpus"] != expected_corpus ||
                record["build_type"] != expected_build_type ||
                record["claim_boundary"] != expected_claim_boundary ||
                record["timing_scope"] != expected_timing_scope ||
                record["timing_asserted"] != "false" ||
                record["diagnostics_timed"] != "false" ||
                !positive_decimal(record["cases"]) ||
                record["repetitions"] != expected_repetitions ||
                !positive_decimal(record["multiplier_count"]) ||
                record["multiplier_count"] + 0 > 1024 ||
                !canonical_decimal(record["successes_per_repetition"]) ||
                !canonical_decimal(record["failures_per_repetition"]) ||
                record["successes_per_repetition"] + record["failures_per_repetition"] != record["cases"] + 0 ||
                record["invalid_factors"] != "0" ||
                !u64(record["measured_calls"]) ||
                record["measured_calls"] + 0 != record["cases"] * record["repetitions"] ||
                !u64(record["wall_min_ns"]) || record["wall_min_ns"] == "0" ||
                !u64(record["wall_median_ns"]) || record["wall_median_ns"] == "0" ||
                !u64(record["wall_max_ns"]) || record["wall_max_ns"] == "0" ||
                record["wall_min_ns"] + 0 > record["wall_median_ns"] + 0 ||
                record["wall_median_ns"] + 0 > record["wall_max_ns"] + 0 ||
                !valid_bitmap(record["failure_bitmap_hex"], record["cases"] + 0,
                              record["failures_per_repetition"] + 0) ||
                !u64(record["schedule_digest_low"]) ||
                !u64(record["schedule_digest_high"])) {
                invalid = 1
            }
            for (field_index = 1; field_index <= digest_count; ++field_index) {
                if (!u64(record[digest_fields[field_index]])) invalid = 1
            }
            for (field in record) summary[field] = record[field]
        }
        index($0, "GNFS_SQUFOF_BENCH_") == 1 &&
        index($0, case_prefix) != 1 &&
        index($0, multiplier_prefix) != 1 &&
        index($0, summary_prefix) != 1 {
            invalid = 1
        }
        END {
            if (summaries != 1 || case_records != expected_repetitions ||
                multiplier_records != summary["multiplier_count"] + 0) {
                invalid = 1
            }
            for (repetition = 1; repetition <= expected_repetitions; ++repetition) {
                if (seen_repetition[repetition] != 1) invalid = 1
            }
            for (slot = 0; slot < summary["multiplier_count"] + 0; ++slot) {
                if (seen_slot[slot] != 1) invalid = 1
            }

            if (summary["scope"] != case_reference["scope"] ||
                summary["corpus"] != case_reference["corpus"] ||
                summary["build_type"] != case_reference["build_type"] ||
                summary["timing_scope"] != case_reference["timing_scope"] ||
                summary["timing_asserted"] != case_reference["timing_asserted"] ||
                summary["diagnostics_timed"] != case_reference["diagnostics_timed"] ||
                summary["cases"] != case_reference["cases"] ||
                summary["successes_per_repetition"] != case_reference["successes"] ||
                summary["failures_per_repetition"] != case_reference["failures"] ||
                summary["invalid_factors"] != case_reference["invalid_factors"] ||
                summary["corpus_digest_low"] != case_reference["corpus_digest_low"] ||
                summary["corpus_digest_high"] != case_reference["corpus_digest_high"] ||
                summary["factor_digest_low"] != case_reference["factor_digest_low"] ||
                summary["factor_digest_high"] != case_reference["factor_digest_high"] ||
                summary["success_digest_low"] != case_reference["success_digest_low"] ||
                summary["success_digest_high"] != case_reference["success_digest_high"] ||
                summary["failure_digest_low"] != case_reference["failure_digest_low"] ||
                summary["failure_digest_high"] != case_reference["failure_digest_high"] ||
                summary["failure_bitmap_hex"] != case_reference["failure_bitmap_hex"] ||
                summary["schedule_digest_low"] != case_reference["schedule_digest_low"] ||
                summary["schedule_digest_high"] != case_reference["schedule_digest_high"] ||
                summary["scope"] != multiplier_scope ||
                summary["corpus"] != multiplier_corpus ||
                summary["build_type"] != multiplier_build_type ||
                summary["diagnostics_timed"] != multiplier_diagnostics_timed ||
                summary["schedule_digest_low"] != schedule_digest_low ||
                summary["schedule_digest_high"] != schedule_digest_high ||
                accepted_total != summary["successes_per_repetition"] + 0) {
                invalid = 1
            }

            if (case_records == expected_repetitions && expected_repetitions > 0) {
                wall_min = case_wall[1]
                wall_max = case_wall[1]
                for (repetition = 1; repetition <= expected_repetitions; ++repetition) {
                    sorted_wall[repetition] = case_wall[repetition]
                    if (case_wall[repetition] < wall_min) wall_min = case_wall[repetition]
                    if (case_wall[repetition] > wall_max) wall_max = case_wall[repetition]
                }
                for (sort_index = 2; sort_index <= expected_repetitions; ++sort_index) {
                    value = sorted_wall[sort_index]
                    cursor = sort_index - 1
                    while (cursor >= 1 && sorted_wall[cursor] > value) {
                        sorted_wall[cursor + 1] = sorted_wall[cursor]
                        cursor -= 1
                    }
                    sorted_wall[cursor + 1] = value
                }
                middle = int(expected_repetitions / 2) + 1
                if (expected_repetitions % 2 == 1) {
                    wall_median = sorted_wall[middle]
                } else {
                    lower = sorted_wall[middle - 1]
                    upper = sorted_wall[middle]
                    wall_median = lower + int((upper - lower) / 2)
                }
                if (summary["wall_min_ns"] + 0 != wall_min ||
                    summary["wall_median_ns"] + 0 != wall_median ||
                    summary["wall_max_ns"] + 0 != wall_max) {
                    invalid = 1
                }
            }
            if (invalid) exit 1
        }
    '
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

    # sed keeps a successful exit status for an empty stream. grep -v '^$'
    # returns 1 when the worktree is clean and would terminate this function
    # under set -e before the documented smoke fallback can run.
    changed_files=$(echo "$changed_files" | sed '/^$/d' | sort -u)

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

    log_section "Structured 120-bit route gate"
    run_single_test test_structured_filter_pipeline_120bit || true

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
# 模式: ThreadSanitizer 并发窄通道
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

    log_header "ThreadSanitizer 并发窄通道"
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
    echo "  ${BULLET} ${CYAN}test_candidate_batch_50d_sweep${RESET} — 固定 50 位 4-SQ candidate 调度扫测"
    echo "  ${BULLET} ${CYAN}test_squfof_bench${RESET}     — 固定 50 位 SQUFOF multiplier/吞吐基准"
    echo "  ${BULLET} ${CYAN}test_siqs_shadow_matrix_bench${RESET} — 固定 SIQS shadow matrix 求解/内核/准备基准"

    echo ""
    echo "${BOLD}Sanitizer 窄通道:${RESET}"
    echo "  ${BULLET} ${CYAN}tsan-relation${RESET} — ${#TSAN_RELATION_TESTS[@]} 个 cofactor/relation/SIQS 并发边界测试"
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
        if [[ ${#MODE_ARGS[@]} -gt 3 ]]; then
            log_fail "用法: $0 probe-50d-structured-ooc [max_special_q] [max_batch_workers] [max_local_sieve_threads|auto]"
            exit 1
        fi
        local _probe_max_special_q="${MODE_ARGS[1]:-4}"
        local _probe_max_batch_workers="${MODE_ARGS[2]:-4}"
        local _probe_max_local_sieve_threads="${MODE_ARGS[3]:-auto}"
        if [[ ! "$_probe_max_special_q" =~ ^[0-9]+$ ]] ||
           (( _probe_max_special_q < 1 || _probe_max_special_q > 64 )); then
            log_fail "max_special_q 必须在 1..64 (传入: ${_probe_max_special_q})"
            exit 1
        fi
        if [[ ! "$_probe_max_batch_workers" =~ ^[0-9]+$ ]] ||
           (( _probe_max_batch_workers < 1 || _probe_max_batch_workers > 4 )); then
            log_fail "max_batch_workers 必须在 1..4 (传入: ${_probe_max_batch_workers})"
            exit 1
        fi
        if [[ "$_probe_max_local_sieve_threads" != "auto" ]] &&
           { [[ ! "$_probe_max_local_sieve_threads" =~ ^[0-9]+$ ]] ||
             (( _probe_max_local_sieve_threads < 1 ||
                _probe_max_local_sieve_threads > 4294967295 )); }; then
            log_fail "max_local_sieve_threads 必须是 auto 或 1..4294967295 (传入: ${_probe_max_local_sieve_threads})"
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
        log_info "max_special_q=${_probe_max_special_q}; max_batch_workers=${_probe_max_batch_workers}; max_local_sieve_threads=${_probe_max_local_sieve_threads}; 临时目录=${_probe_dir}"
        local -a _probe_thread_args=()
        if [[ "$_probe_max_local_sieve_threads" != "auto" ]]; then
            _probe_thread_args=(--max-local-sieve-threads "$_probe_max_local_sieve_threads")
        fi
        local _probe_status=0
        run_single_test test_structured_ooc_50d_probe --max-special-q \
            "$_probe_max_special_q" --max-special-q-batch-workers \
            "$_probe_max_batch_workers" "${_probe_thread_args[@]}" \
            --ooc-base "$_probe_base" || _probe_status=$?
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

    probe-50d-special-q-workers)
        if [[ ${#MODE_ARGS[@]} -gt 2 ]]; then
            log_fail "用法: $0 probe-50d-special-q-workers [max_special_q] [max_local_sieve_threads|auto]"
            exit 1
        fi
        local _comparison_max_special_q="${MODE_ARGS[1]:-4}"
        local _comparison_max_local_sieve_threads="${MODE_ARGS[2]:-auto}"
        if [[ ! "$_comparison_max_special_q" =~ ^[0-9]+$ ]] ||
           (( _comparison_max_special_q < 4 || _comparison_max_special_q > 64 )); then
            log_fail "对照 max_special_q 必须在 4..64 (传入: ${_comparison_max_special_q})"
            exit 1
        fi
        if [[ "$_comparison_max_local_sieve_threads" != "auto" ]] &&
           { [[ ! "$_comparison_max_local_sieve_threads" =~ ^[0-9]+$ ]] ||
             (( _comparison_max_local_sieve_threads < 1 ||
                _comparison_max_local_sieve_threads > 4294967295 )); }; then
            log_fail "对照 max_local_sieve_threads 必须是 auto 或 1..4294967295 (传入: ${_comparison_max_local_sieve_threads})"
            exit 1
        fi
        if (( ! BUILD_TYPE_EXPLICIT )); then
            BUILD_TYPE="Release"
        fi
        if (( SKIP_BUILD )); then
            log_fail "probe-50d-special-q-workers 不接受 --no-build；对照证据必须由本次请求的构建生成"
            exit 1
        fi
        do_build
        if [[ ! -x "${BUILD_DIR}/test_structured_ooc_50d_probe" ]]; then
            log_fail "50 位探针二进制不存在: ${BUILD_DIR}/test_structured_ooc_50d_probe"
            exit 1
        fi

        log_header "50 位 Special-Q 外层 worker 独立进程对照"
        local -A _comparison_records
        local -a _comparison_thread_args=()
        if [[ "$_comparison_max_local_sieve_threads" != "auto" ]]; then
            _comparison_thread_args=(--max-local-sieve-threads \
                "$_comparison_max_local_sieve_threads")
        fi
        local _comparison_ready=1
        local _comparison_workers _comparison_dir _comparison_base _comparison_run_status
        local _comparison_effective_budget _comparison_observed_limit
        local _comparison_observed_peak _comparison_candidate_peak _comparison_candidate_chunks
        local _comparison_expected_workers _comparison_expected_candidate_workers
        for _comparison_workers in 1 2 4; do
            _comparison_dir=$(mktemp -d \
                "${TMPDIR:-/tmp}/gnfs_structured_ooc_50d_w${_comparison_workers}.XXXXXX")
            _comparison_base="${_comparison_dir}/raw"
            log_info "workers=${_comparison_workers}; max_special_q=${_comparison_max_special_q}; max_local_sieve_threads=${_comparison_max_local_sieve_threads}; 临时目录=${_comparison_dir}"
            _comparison_run_status=0
            run_single_test test_structured_ooc_50d_probe --max-special-q \
                "$_comparison_max_special_q" --max-special-q-batch-workers \
                "$_comparison_workers" "${_comparison_thread_args[@]}" \
                --ooc-base "$_comparison_base" || \
                _comparison_run_status=$?
            if (( _comparison_run_status != 0 )); then
                _comparison_ready=0
                log_warn "workers=${_comparison_workers} 探针失败，保留诊断工件: ${_comparison_dir}"
                continue
            fi
            if ! capture_single_measurement_record "GNFS_EXPERIMENT_V1 " \
                "50 位 workers=${_comparison_workers} 探针"; then
                (( FAILED_TESTS += 1 ))
                _comparison_ready=0
                log_warn "workers=${_comparison_workers} 记录无效，保留诊断工件: ${_comparison_dir}"
                continue
            fi
            if ! _comparison_effective_budget=$(measurement_record_field \
                "$MEASUREMENT_RECORD" local_sieve_thread_budget) ||
               ! _comparison_observed_limit=$(measurement_record_field \
                "$MEASUREMENT_RECORD" special_q_batch_worker_limit) ||
               ! _comparison_observed_peak=$(measurement_record_field \
                "$MEASUREMENT_RECORD" special_q_batch_peak_workers) ||
               ! _comparison_candidate_peak=$(measurement_record_field \
                "$MEASUREMENT_RECORD" candidate_batch_peak_workers) ||
               ! _comparison_candidate_chunks=$(measurement_record_field \
                "$MEASUREMENT_RECORD" candidate_batch_peak_chunks); then
                log_fail "workers=${_comparison_workers} 记录缺少有效调度拓扑"
                (( FAILED_TESTS += 1 ))
                _comparison_ready=0
                rmdir "$_comparison_dir" 2>/dev/null || true
                break
            fi
            if (( _comparison_workers == 1 && _comparison_effective_budget < 4 )); then
                log_fail "1/2/4 对照要求有效本地筛法预算至少为 4；当前为 ${_comparison_effective_budget}"
                (( FAILED_TESTS += 1 ))
                _comparison_ready=0
                rmdir "$_comparison_dir" 2>/dev/null || true
                break
            fi
            _comparison_expected_workers=$_comparison_workers
            if (( _comparison_expected_workers > _comparison_effective_budget )); then
                _comparison_expected_workers=$_comparison_effective_budget
            fi
            if (( _comparison_expected_workers > 4 )); then
                _comparison_expected_workers=4
            fi
            if (( _comparison_observed_limit != _comparison_expected_workers ||
                  _comparison_observed_peak != _comparison_expected_workers )); then
                log_fail "workers=${_comparison_workers} 未实现声明拓扑：limit=${_comparison_observed_limit}, peak=${_comparison_observed_peak}, expected=${_comparison_expected_workers}"
                (( FAILED_TESTS += 1 ))
                _comparison_ready=0
                rmdir "$_comparison_dir" 2>/dev/null || true
                break
            fi
            _comparison_expected_candidate_workers=$_comparison_effective_budget
            if (( _comparison_expected_candidate_workers > _comparison_candidate_chunks )); then
                _comparison_expected_candidate_workers=$_comparison_candidate_chunks
            fi
            if (( _comparison_candidate_peak != _comparison_expected_candidate_workers )); then
                log_fail "workers=${_comparison_workers} 候选批次拓扑不符：peak=${_comparison_candidate_peak}, expected=${_comparison_expected_candidate_workers}"
                (( FAILED_TESTS += 1 ))
                _comparison_ready=0
                rmdir "$_comparison_dir" 2>/dev/null || true
                break
            fi
            _comparison_records[$_comparison_workers]="$MEASUREMENT_RECORD"
            print -r -- "$MEASUREMENT_RECORD"
            if rmdir "$_comparison_dir"; then
                log_success "workers=${_comparison_workers} 探针工件已完成生命周期清理"
            else
                log_fail "workers=${_comparison_workers} 探针成功但临时目录非空，已保留: ${_comparison_dir}"
                (( FAILED_TESTS += 1 ))
                _comparison_ready=0
            fi
        done

        if (( _comparison_ready )); then
            local -a _comparison_fields=(
                status claim_boundary stop_after pipeline_batch_mode candidate_chunk_size
                candidate_rss_sample_policy cofactor_inner_parallel_policy
                n_digits n_bits max_special_q
                special_q_processed special_q_batch_count special_q_batch_peak_size
                max_local_sieve_threads_requested local_sieve_thread_budget
                special_q_batch_peak_assigned_threads
                candidates_total candidate_batch_total_chunks candidate_batch_peak_chunks
                candidate_batch_peak_candidates candidate_batch_rss_sample_candidates
                rational_fb_columns algebraic_fb_columns base_factor_columns initial_raw_target
                first_round_complete resume_scope attempted_resume attempted_distributed
                sge_attempted solver_attempted sqrt_attempted factorization_attempted
                route_evidence strategy storage generation raw_rows raw_duplicates
                input_lp_columns output_rows output_lp_columns structured_commits
                structured_emitted_rows structured_stop incidence_shards
                incidence_requested_workers incidence_peak_workers raw_digest_low raw_digest_high
                output_digest_low output_digest_high matrix_rows matrix_cols matrix_nonzeros
                matrix_signed_delta matrix_row_mapping_identity thin_guard_proof_satisfied
                structured_filter_records structured_matrix_records raw_pair_observed
                raw_pair_removed output_pair_observed output_pair_retained_by_matrix
                output_pair_removed output_lease_removed
            )
            local _comparison_field _comparison_reference _comparison_value
            for _comparison_field in "${_comparison_fields[@]}"; do
                if ! _comparison_reference=$(measurement_record_field \
                    "${_comparison_records[1]}" "$_comparison_field"); then
                    log_fail "workers=1 记录缺失或重复字段: ${_comparison_field}"
                    (( FAILED_TESTS += 1 ))
                    _comparison_ready=0
                    break
                fi
                for _comparison_workers in 2 4; do
                    if ! _comparison_value=$(measurement_record_field \
                        "${_comparison_records[$_comparison_workers]}" "$_comparison_field"); then
                        log_fail "workers=${_comparison_workers} 记录缺失或重复字段: ${_comparison_field}"
                        (( FAILED_TESTS += 1 ))
                        _comparison_ready=0
                        break 2
                    fi
                    if [[ "$_comparison_value" != "$_comparison_reference" ]]; then
                        log_fail "对照字段漂移: ${_comparison_field}; workers=1 为 ${_comparison_reference}, workers=${_comparison_workers} 为 ${_comparison_value}"
                        (( FAILED_TESTS += 1 ))
                        _comparison_ready=0
                        break 2
                    fi
                done
            done
        fi
        if (( _comparison_ready )); then
            print -r -- "GNFS_EXPERIMENT_COMPARISON_V1 status=pass scope=bounded_50d_special_q_batch_workers max_special_q=${_comparison_max_special_q} max_local_sieve_threads=${_comparison_max_local_sieve_threads} workers=1,2,4 identity_fields=${#_comparison_fields[@]} timing_compared=false rss_compared=false"
            log_success "1/2/4 外层 worker 的 relation、matrix 与生命周期身份一致"
        fi
        show_summary
        ;;

    sweep-50d-candidate-batch)
        if [[ ${#MODE_ARGS[@]} -gt 1 ]]; then
            log_fail "用法: $0 sweep-50d-candidate-batch [repetitions]"
            exit 1
        fi
        local _sweep_repetitions="${MODE_ARGS[1]:-3}"
        if [[ ! "$_sweep_repetitions" =~ ^[0-9]+$ ]] ||
           (( _sweep_repetitions < 1 || _sweep_repetitions > 9 )); then
            log_fail "repetitions 必须在 1..9 (传入: ${_sweep_repetitions})"
            exit 1
        fi
        if (( BUILD_TYPE_EXPLICIT )) && [[ "$BUILD_TYPE" != "Release" ]]; then
            log_fail "sweep-50d-candidate-batch 只接受 Release 构建 (传入: ${BUILD_TYPE})"
            exit 1
        fi
        BUILD_TYPE="Release"
        if (( SKIP_BUILD )); then
            log_fail "sweep-50d-candidate-batch 不接受 --no-build；性能证据必须由本次请求的构建生成"
            exit 1
        fi
        do_build
        if [[ ! -x "${BUILD_DIR}/test_candidate_batch_50d_sweep" ]]; then
            log_fail "50 位 candidate sweep 二进制不存在: ${BUILD_DIR}/test_candidate_batch_50d_sweep"
            exit 1
        fi

        log_header "固定 50 位 CandidateBatch worker/chunk 扫测"
        log_info "4 个 production special-Q；workers=1,2,4,6,8,10；chunk=64,128,256,512,1024；repetitions=${_sweep_repetitions}"
        local _sweep_status=0
        run_single_test test_candidate_batch_50d_sweep "$_sweep_repetitions" ||
            _sweep_status=$?
        if (( _sweep_status == 0 )); then
            if printf '%s\n' "$RUN_OUTPUT" | awk \
                -v expected_build_type="$BUILD_TYPE" \
                -v expected_repetitions="$_sweep_repetitions" '
                function clear_record(key) {
                    for (key in record) delete record[key]
                    for (key in occurrences) delete occurrences[key]
                }
                function parse_record(token_index, equals, key) {
                    clear_record()
                    for (token_index = 2; token_index <= NF; ++token_index) {
                        equals = index($token_index, "=")
                        if (equals <= 1) {
                            invalid = 1
                            continue
                        }
                        key = substr($token_index, 1, equals - 1)
                        record[key] = substr($token_index, equals + 1)
                        if (++occurrences[key] != 1) invalid = 1
                    }
                }
                function decimal(text) {
                    return text ~ /^[0-9]+$/
                }
                BEGIN {
                    expected_scope = "fixed_50d_first_production_batch"
                    expected_call_boundary = "whole_verify_candidate_batch_call"
                    workers[1] = workers[2] = workers[4] = 1
                    workers[6] = workers[8] = workers[10] = 1
                    chunks[64] = chunks[128] = chunks[256] = 1
                    chunks[512] = chunks[1024] = 1

                    consistent_fields[1] = "build_type"
                    consistent_fields[2] = "timing_scope"
                    consistent_fields[3] = "run_fingerprint_low"
                    consistent_fields[4] = "run_fingerprint_high"
                    consistent_fields[5] = "candidate_digest_low"
                    consistent_fields[6] = "candidate_digest_high"
                    consistent_fields[7] = "relation_digest_low"
                    consistent_fields[8] = "relation_digest_high"
                    consistent_fields[9] = "candidates"
                    consistent_fields[10] = "relations"

                    case_required_count = split("status scope build_type timing_scope timing_asserted worker_cap chunk_size planned_chunks workers_used candidates relations repetitions run_fingerprint_low run_fingerprint_high candidate_digest_low candidate_digest_high relation_digest_low relation_digest_high", case_required, " ")
                    summary_required_count = split("status scope build_type claim_boundary timing_scope timing_asserted n_digits n_bits special_q_count candidates relations relations_per_special_q cases repetitions worker_caps chunk_sizes run_fingerprint_low run_fingerprint_high candidate_digest_low candidate_digest_high relation_digest_low relation_digest_high", summary_required, " ")
                }
                index($0, "GNFS_CANDIDATE_SWEEP_CASE_V1 ") == 1 {
                    cases += 1
                    parse_record()
                    for (field_index = 1; field_index <= case_required_count; ++field_index) {
                        if (occurrences[case_required[field_index]] != 1) invalid = 1
                    }
                    worker = record["worker_cap"]
                    chunk = record["chunk_size"]
                    planned_chunks = record["planned_chunks"]
                    workers_used = record["workers_used"]
                    key = worker SUBSEP chunk

                    if (record["status"] != "pass" || record["scope"] != expected_scope ||
                        record["build_type"] != expected_build_type ||
                        record["timing_scope"] != expected_call_boundary ||
                        record["timing_asserted"] != "false" ||
                        record["repetitions"] != expected_repetitions ||
                        !(worker in workers) || !(chunk in chunks) || seen[key]++) {
                        invalid = 1
                    }
                    if (!decimal(planned_chunks) || planned_chunks + 0 < 1 ||
                        !decimal(workers_used) || workers_used + 0 < 1 ||
                        workers_used + 0 > worker + 0 ||
                        workers_used + 0 > planned_chunks + 0 ||
                        !decimal(record["candidates"]) || record["candidates"] + 0 < 1 ||
                        !decimal(record["relations"]) || record["relations"] + 0 < 1) {
                        invalid = 1
                    }
                    for (field_index = 1; field_index <= 10; ++field_index) {
                        field = consistent_fields[field_index]
                        if (cases == 1) {
                            reference[field] = record[field]
                        } else if (record[field] != reference[field]) {
                            invalid = 1
                        }
                    }
                }
                index($0, "GNFS_CANDIDATE_SWEEP_SUMMARY_V1 ") == 1 {
                    summaries += 1
                    parse_record()
                    for (field_index = 1; field_index <= summary_required_count; ++field_index) {
                        if (occurrences[summary_required[field_index]] != 1) invalid = 1
                    }
                    if (record["status"] != "pass" || record["scope"] != expected_scope ||
                        record["build_type"] != expected_build_type ||
                        record["claim_boundary"] != expected_call_boundary ||
                        record["timing_scope"] != expected_call_boundary ||
                        record["timing_asserted"] != "false" || record["n_digits"] != "50" ||
                        record["n_bits"] != "164" || record["special_q_count"] != "4" ||
                        record["cases"] != "30" ||
                        record["repetitions"] != expected_repetitions ||
                        record["worker_caps"] != "1,2,4,6,8,10" ||
                        record["chunk_sizes"] != "64,128,256,512,1024") {
                        invalid = 1
                    }
                    if (!decimal(record["candidates"]) || record["candidates"] + 0 < 1 ||
                        !decimal(record["relations"]) || record["relations"] + 0 < 1) {
                        invalid = 1
                    }

                    relation_parts = split(record["relations_per_special_q"], relation_counts, ",")
                    relation_sum = 0
                    if (relation_parts != 4) invalid = 1
                    for (part = 1; part <= relation_parts; ++part) {
                        if (!decimal(relation_counts[part])) invalid = 1
                        relation_sum += relation_counts[part] + 0
                    }
                    if (relation_sum != record["relations"] + 0) invalid = 1

                    for (field_index = 1; field_index <= 10; ++field_index) {
                        field = consistent_fields[field_index]
                        if (record[field] != reference[field]) invalid = 1
                        if (field_index >= 3 && !decimal(record[field])) invalid = 1
                    }
                }
                END {
                    if (cases != 30 || summaries != 1) invalid = 1
                    for (worker in workers) {
                        for (chunk in chunks) {
                            if (seen[worker SUBSEP chunk] != 1) invalid = 1
                        }
                    }
                    if (invalid) exit 1
                }
            '; then
                printf '%s\n' "$RUN_OUTPUT" | awk '
                    index($0, "GNFS_CANDIDATE_SWEEP_CASE_V1 ") == 1 ||
                    index($0, "GNFS_CANDIDATE_SWEEP_SUMMARY_V1 ") == 1 { print }
                '
                log_success "30 组 worker/chunk 的 Release 构建、计时边界和身份字段均有效"
            else
                log_fail "candidate sweep CASE/SUMMARY 构建、边界、网格或身份字段无效"
                (( FAILED_TESTS += 1 ))
            fi
        fi
        show_summary
        ;;

    bench-squfof)
        if [[ ${#MODE_ARGS[@]} -gt 1 ]]; then
            log_fail "用法: $0 bench-squfof [repetitions]"
            exit 1
        fi
        local _squfof_repetitions="${MODE_ARGS[1]:-3}"
        if [[ ! "$_squfof_repetitions" =~ ^[0-9]+$ ]] ||
           (( _squfof_repetitions < 1 || _squfof_repetitions > 9 )); then
            log_fail "repetitions 必须在 1..9 (传入: ${_squfof_repetitions})"
            exit 1
        fi
        if (( BUILD_TYPE_EXPLICIT )) && [[ "$BUILD_TYPE" != "Release" ]]; then
            log_fail "bench-squfof 只接受 Release 构建 (传入: ${BUILD_TYPE})"
            exit 1
        fi
        BUILD_TYPE="Release"
        if (( SKIP_BUILD )); then
            log_fail "bench-squfof 不接受 --no-build；性能证据必须由本次请求的构建生成"
            exit 1
        fi
        if (( RETRY_COUNT != 0 )); then
            log_fail "bench-squfof 不接受 --retry；自动重试会破坏独立测量证据"
            exit 1
        fi

        do_build
        if [[ ! -x "${BUILD_DIR}/test_squfof_bench" ]]; then
            log_fail "SQUFOF benchmark 二进制不存在: ${BUILD_DIR}/test_squfof_bench"
            exit 1
        fi

        log_header "固定 50 位 SQUFOF multiplier/吞吐基准"
        log_info "Release；corpus=fixed_50d_squfof_strategy_v1；repetitions=${_squfof_repetitions}；墙钟仅报告、不设阈值"
        local _squfof_status=0
        run_single_test test_squfof_bench "$_squfof_repetitions" ||
            _squfof_status=$?
        if (( _squfof_status == 0 )); then
            if validate_squfof_bench_output "$BUILD_TYPE" "$_squfof_repetitions"; then
                printf '%s\n' "$RUN_OUTPUT" | awk '
                    index($0, "GNFS_SQUFOF_BENCH_CASE_V1 ") == 1 ||
                    index($0, "GNFS_SQUFOF_BENCH_MULTIPLIER_V1 ") == 1 ||
                    index($0, "GNFS_SQUFOF_BENCH_SUMMARY_V1 ") == 1 { print }
                '
                capture_single_measurement_record "GNFS_SQUFOF_BENCH_SUMMARY_V1 " \
                    "SQUFOF benchmark" >/dev/null
                local _squfof_multiplier_count
                _squfof_multiplier_count=$(measurement_record_field \
                    "$MEASUREMENT_RECORD" multiplier_count)
                log_success "${_squfof_repetitions} 条 CASE、${_squfof_multiplier_count} 条 MULTIPLIER 与 1 条 SUMMARY 身份一致"
            else
                log_fail "SQUFOF benchmark CASE/MULTIPLIER/SUMMARY schema 或身份字段无效"
                (( FAILED_TESTS += 1 ))
            fi
        fi
        show_summary
        ;;

    bench-siqs-shadow)
        if [[ ${#MODE_ARGS[@]} -eq 0 ]]; then
            log_fail "用法: $0 bench-siqs-shadow <solve|kernel|prepare|fbcheck> [benchmark options]"
            exit 1
        fi
        local _shadow_mode="${MODE_ARGS[1]}"
        local _shadow_workers="1,2,4"
        local _shadow_arg_index
        for (( _shadow_arg_index = 2; _shadow_arg_index <= ${#MODE_ARGS[@]}; ++_shadow_arg_index )); do
            if [[ "${MODE_ARGS[$_shadow_arg_index]}" == "--workers" ]] &&
               (( _shadow_arg_index < ${#MODE_ARGS[@]} )); then
                _shadow_workers="${MODE_ARGS[$((_shadow_arg_index + 1))]}"
            fi
        done
        [[ "$_shadow_workers" == "all" ]] && _shadow_workers="1,2,4"
        case "$_shadow_mode" in
            solve|kernel|prepare|fbcheck) ;;
            *)
                log_fail "SIQS shadow benchmark mode 必须是 solve、kernel、prepare 或 fbcheck"
                exit 1
                ;;
        esac
        if (( BUILD_TYPE_EXPLICIT )) && [[ "$BUILD_TYPE" != "Release" ]]; then
            log_fail "bench-siqs-shadow 只接受 Release 构建 (传入: ${BUILD_TYPE})"
            exit 1
        fi
        BUILD_TYPE="Release"
        if (( SKIP_BUILD )); then
            log_fail "bench-siqs-shadow 不接受 --no-build；性能证据必须由本次请求的构建生成"
            exit 1
        fi
        if (( RETRY_COUNT != 0 )); then
            log_fail "bench-siqs-shadow 不接受 --retry；自动重试会破坏独立测量证据"
            exit 1
        fi

        do_build
        if [[ ! -x "${BUILD_DIR}/test_siqs_shadow_matrix_bench" ]]; then
            log_fail "SIQS shadow benchmark 二进制不存在: ${BUILD_DIR}/test_siqs_shadow_matrix_bench"
            exit 1
        fi

        log_header "固定 SIQS shadow matrix ${_shadow_mode} 基准"
        log_info "Release/NDEBUG；seed=0x53a9f19d97e8c641；steady_clock 墙钟仅报告、不设阈值"
        local _shadow_status=0
        run_single_test test_siqs_shadow_matrix_bench "${MODE_ARGS[@]}" ||
            _shadow_status=$?
        if (( _shadow_status == 0 )); then
            if printf '%s\n' "$RUN_OUTPUT" | awk \
                -v mode="$_shadow_mode" -v requested_workers="$_shadow_workers" '
                function has_field(line, key, value) {
                    return index(" " line " ", " " key "=" value " ") != 0
                }
                function field(line, key,    count, index_, fields, prefix) {
                    count = split(line, fields, " ")
                    prefix = key "="
                    for (index_ = 1; index_ <= count; ++index_) {
                        if (index(fields[index_], prefix) == 1) {
                            return substr(fields[index_], length(prefix) + 1)
                        }
                    }
                    return ""
                }
                index($0, "GNFS_SIQS_SHADOW_MATRIX_BENCH_CONFIG_V1 ") == 1 {
                    configs++
                    if (!has_field($0, "mode", mode) ||
                        !has_field($0, "build_contract", "release_ndebug") ||
                        !has_field($0, "cmake_build_type", "Release") ||
                        !has_field($0, "timing_asserted", "false") ||
                        !has_field($0, "seed", "0x53a9f19d97e8c641")) invalid = 1
                }
                index($0, "GNFS_SIQS_SHADOW_MATRIX_BENCH_RESULT_V1 ") == 1 {
                    results++
                    worker = field($0, "workers")
                    implementation = field($0, "implementation")
                    digest = field($0, "result_digest")
                    dependency_digest = field($0, "dependency_digest")
                    minimum = field($0, "wall_min_ns") + 0
                    median = field($0, "wall_median_ns") + 0
                    maximum = field($0, "wall_max_ns") + 0
                    if (!has_field($0, "status", "ok") ||
                        !has_field($0, "mode", mode) ||
                        !has_field($0, "build_contract", "release_ndebug") ||
                        !has_field($0, "cmake_build_type", "Release") ||
                        !has_field($0, "timing_asserted", "false") ||
                        !has_field($0, "seed", "0x53a9f19d97e8c641") ||
                        $0 !~ / wall_min_ns=[0-9]+ / ||
                        $0 !~ / wall_median_ns=[0-9]+ / ||
                        $0 !~ / wall_max_ns=[0-9]+ / ||
                        $0 !~ / result_digest=0x[0-9a-f]+$/) invalid = 1
                    if (minimum > median || median > maximum) invalid = 1
                    if (reference_digest == "") reference_digest = digest
                    else if (digest != reference_digest) invalid = 1

                    if (mode == "solve") {
                        if (implementation != "public_solver" ||
                            dependency_digest != digest) invalid = 1
                        seen[worker, implementation]++
                    } else if (mode == "kernel") {
                        if (dependency_digest != "na" ||
                            (implementation != "legacy_per_pivot_jthread" &&
                             implementation != "benchmark_only_queued_thread_pool" &&
                             implementation != "production_persistent_worker_team")) invalid = 1
                        seen[worker, implementation]++
                    } else if (mode == "prepare") {
                        if (worker != "1" || dependency_digest != "na" ||
                            (implementation != "public_identity_wrapper" &&
                             implementation != "prevalidated_identity_helper")) invalid = 1
                        seen[worker, implementation]++
                    } else if (mode == "fbcheck") {
                        if (worker != "1" || dependency_digest != "na" ||
                            implementation != "factor_base_validation") invalid = 1
                        seen[worker, implementation]++
                    }
                }
                END {
                    if (configs != 1) invalid = 1
                    expected_count = split(requested_workers, expected, ",")
                    if (mode == "solve") {
                        if (results != expected_count) invalid = 1
                        for (index_ = 1; index_ <= expected_count; ++index_)
                            if (seen[expected[index_], "public_solver"] != 1) invalid = 1
                    } else if (mode == "kernel") {
                        if (results != expected_count * 3) invalid = 1
                        for (index_ = 1; index_ <= expected_count; ++index_) {
                            if (seen[expected[index_], "legacy_per_pivot_jthread"] != 1)
                                invalid = 1
                            if (seen[expected[index_],
                                     "benchmark_only_queued_thread_pool"] != 1)
                                invalid = 1
                            if (seen[expected[index_],
                                     "production_persistent_worker_team"] != 1)
                                invalid = 1
                        }
                    } else if (mode == "prepare") {
                        if (results != 2 || seen["1", "public_identity_wrapper"] != 1 ||
                            seen["1", "prevalidated_identity_helper"] != 1) invalid = 1
                    } else if (mode == "fbcheck") {
                        if (results != 1 || seen["1", "factor_base_validation"] != 1) invalid = 1
                    }
                    if (invalid) exit 1
                }
            '; then
                printf '%s\n' "$RUN_OUTPUT" | awk '
                    index($0, "GNFS_SIQS_SHADOW_MATRIX_BENCH_CONFIG_V1 ") == 1 ||
                    index($0, "GNFS_SIQS_SHADOW_MATRIX_BENCH_RESULT_V1 ") == 1 { print }
                '
                log_success "SIQS shadow ${_shadow_mode} 基准 schema、构建合同与结果身份均有效"
            else
                log_fail "SIQS shadow benchmark CONFIG/RESULT schema 或身份字段无效"
                (( FAILED_TESTS += 1 ))
            fi
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

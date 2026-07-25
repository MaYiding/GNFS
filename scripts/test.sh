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
#   ./scripts/test.sh check-50d-contracts  # 仅 CLI/schema 合同，不运行真实 50 位流水线
#   ./scripts/test.sh compare-50d-bounded-routes
#                                         # 真实 50 位，4-SQ legacy/structured 独立进程对照
#   ./scripts/test.sh compare-50d-first-round
#                                         # 真实 50 位，完整首轮 legacy/structured 独立进程对照
#   ./scripts/test.sh probe-50d-special-q-workers
#                                         # 真实 50 位，外层 SQ workers=1/2/4 对照
#   ./scripts/test.sh sweep-50d-candidate-batch
#                                         # 固定 4-SQ candidate worker/chunk 扫测
#   ./scripts/test.sh bench-squfof
#                                         # 固定 50 位 SQUFOF multiplier/吞吐基准
#   ./scripts/test.sh bench-siqs-shadow <mode> [options]
#                                         # Release-only SIQS shadow matrix 可复现基准
#   ./scripts/test.sh probe-siqs-live-sieve <50|70|90> <1|2|4>
#                                         # Release-only 有界 SIQS live-sieve 单进程探针
#   ./scripts/test.sh compare-siqs-live-sieve <50|70|90>
#                                         # 新构建后 1/2/4 三个独立进程身份对照
#   ./scripts/test.sh profile-siqs-cycle-density <1|2|4>
#                                         # 固定 50 位 1/4/16/64 A cycle-density profile
#   ./scripts/test.sh compare-siqs-cycle-density
#                                         # 同一新构建的 1/2/4 profile 独立进程对照
#   ./scripts/test.sh profile-siqs-256a <1|2|4>
#                                         # 固定 50 位 256x32 A scale 证据
#   ./scripts/test.sh compare-siqs-256a
#                                         # 同一新构建的 1/2/4 scale 独立进程对照
#   ./scripts/test.sh profile-siqs-256a-proof <1|2|4>
#                                         # 固定 256-A gate 后的 proof-shadow 因子证据
#   ./scripts/test.sh compare-siqs-256a-proof
#                                         # 同一新构建的 1/2/4 proof 独立进程对照
#   ./scripts/test.sh probe-siqs-shadow-observe <off|observe>
#                                         # Release-only production factor 双流探针
#   ./scripts/test.sh compare-siqs-shadow-observe
#                                         # 1 次 off + 3 次 observe fresh-process 对照
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
GNFS_TEST_PYTHON="${GNFS_PYTHON_EXECUTABLE:-python3}"
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
RETRY_EXPLICIT=0

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
    test_sha256
    test_thread_pool
    test_ordered_parallel_map
    test_fixed_slot_executor
    test_logger
    test_primes
    test_timer
    test_process_memory
    test_bounded_child_process
    test_durable_immutable_file
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
    test_siqs_live_sieve_capture
    test_siqs_2lp_graph
    test_siqs_2lp_materializer
    test_siqs_2lp_adapter
    test_siqs_2lp_congruence
    test_siqs_post_merge_row
    test_siqs_shadow_assembly
    test_siqs_shadow_linear_algebra
    test_siqs_shadow_proof_runner
    test_siqs_shadow_proof_observe
    test_siqs_shadow_proof_observe_record_codec
    test_siqs_runtime_facts
    test_siqs_shadow_proof_rss_probe_execution_identity
    test_siqs_shadow_observe_rss_holdouts
    test_siqs_shadow_proof_rss_gate
    test_siqs_shadow_proof_rss_terminal_gate_record
    test_siqs_shadow_proof_rss_policy_record
    test_siqs_shadow_proof_rss_campaign
    test_siqs_shadow_proof_rss_campaign_journal
    test_siqs_shadow_proof_rss_campaign_journal_codec
    test_siqs_shadow_proof_rss_campaign_journal_layout
    test_siqs_shadow_proof_rss_campaign_artifact_layout
    test_siqs_shadow_proof_rss_campaign_journal_store
    test_siqs_shadow_proof_rss_campaign_entry
    test_siqs_shadow_proof_rss_holdout_probe_contract
    test_siqs_shadow_proof_rss_holdout_probe_record_codec
    test_siqs_shadow_proof_rss_holdout_stream_join
    test_siqs_shadow_proof_prefer
    test_siqs_shadow_prefer_route
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
    util           "test_small_vector test_sha256 test_thread_pool test_ordered_parallel_map test_fixed_slot_executor test_logger test_primes test_timer test_process_memory test_bounded_child_process test_durable_immutable_file test_mmap_file test_safe_math test_bit_intrin test_memory_pool test_integer_scratch_pool test_mpz_powm_parallel test_mpz_invert_parallel test_mpz_mod_parallel test_mpz_gcd_parallel test_mpz_mul_parallel"
    polynomial     "test_murphy test_root_property_cache test_int_polynomial test_half_gcd test_poly_karatsuba test_horner_batch_simd test_divrem_subquadratic test_poly_ntt test_poly_square test_poly_add_mod_simd test_poly_horner_mod_simd test_regressions test_polynomial_context test_base_m test_polynomial_optimizer test_resultant test_rotation_incremental test_bai_brent_poly test_poly_checkpoint"
    factor_base    "test_factor_base test_fb_checkpoint test_fb_roots_parallel"
    sieve          "test_special_q test_sieve_basic test_sieve_checkpoint test_distributed_sieve test_bucket_sieve test_sieve_ecore_qos test_local_sieve_thread_budget test_lll_lattice test_adaptive_lattice test_sieve_tiny_simd test_bucket_prefetch test_sieve_region_tile test_sieve_norm_tile test_lattice_basis_parallel test_sieve_apply_tile_parallel test_lattice_coords_simd test_threshold_scan_simd test_saturated_sub_simd"
    cofactor       "test_cofactor test_candidate_chunk_plan test_candidate_batch test_squfof test_squfof_budget_corpus test_squfof_success_challenge_corpus test_squfof_success_challenge_oracle test_squfof_budget_oracle test_squfof_strategy_oracle test_brent_pollard_rho test_brent_pollard_rho_parallel test_survival_predictor test_batch_ecm test_3lp_cofactor test_trial_wheel test_batch_trial test_ecm_curve_pool test_sigma_seed_pool test_ecm_stage2_parallel test_ecm_stage1_parallel test_batch_inversion test_trial_div_simd test_cofactor_stage_timing test_ecm_prime_cache test_cofactor_result_cache test_integration test_ecm_brent_suyama"
    relation       "test_relation_collector test_relation_corpus test_relation_sink test_ooc_store_integrity test_filter test_lp_key_contract test_relation_identity test_relation_reduction_engine test_structured_filter test_structured_filter_policy test_structured_tree_basis test_structured_tree_basis_property test_structured_budgeted_driver test_structured_conflict_batch test_structured_parallel_prepare test_structured_batch_commit test_structured_parallel_driver test_structured_parallel_failures test_structured_incidence_builder test_structured_materialization test_filter_radix_sort test_lp_bloom test_lp_key_hash test_merger_parallel test_clique_merger test_clique_merger_50d_synthetic test_3lp_merge test_ooc_relations test_ooc_policy test_v0_bfs_policy test_integration test_relation_pool_integration"
    linalg         "test_linalg test_sge_batch_pivots test_block_wiedemann test_bw_rank_est test_matrix_diagnostics test_sge_streaming test_mmap_csr test_schirokauer_deg4 test_schirokauer_strip test_schirokauer_parallel test_edge_cases test_integration test_matrix_view_concept test_save_sparse_as_mmap test_linalg_mmap_policy test_bw_krylov_parallel test_metal_spmv test_spmv_simd test_transpose_blocked test_popcount_simd test_and_popcnt_simd test_xor_words_simd test_and_words_simd test_xor_popcnt_simd test_row_popcount_simd test_krylov_compress test_krylov_compression test_bl_checkpoint test_bl_resume_integration test_linalg_progress"
    integration    "test_integration"
    sqrt           "test_sqrt test_sqrt_debug test_hensel_parallel test_class_group test_couveignes_large_class_group test_couveignes_parallel"
    api            "test_i18n test_method_selection test_relation_reduction_engine"
    siqs           "test_siqs test_siqs_2lp test_siqs_live_sieve_capture test_siqs_2lp_graph test_siqs_2lp_materializer test_siqs_2lp_adapter test_siqs_2lp_congruence test_siqs_post_merge_row test_siqs_shadow_assembly test_siqs_shadow_linear_algebra test_siqs_shadow_proof_runner test_siqs_shadow_proof_observe test_siqs_shadow_proof_observe_record_codec test_siqs_runtime_facts test_siqs_shadow_proof_rss_probe_execution_identity test_siqs_shadow_observe_rss_holdouts test_siqs_shadow_proof_rss_gate test_siqs_shadow_proof_rss_terminal_gate_record test_siqs_shadow_proof_rss_policy_record test_siqs_shadow_proof_rss_campaign test_siqs_shadow_proof_rss_campaign_journal test_siqs_shadow_proof_rss_campaign_journal_codec test_siqs_shadow_proof_rss_campaign_journal_layout test_siqs_shadow_proof_rss_campaign_artifact_layout test_siqs_shadow_proof_rss_campaign_journal_store test_siqs_shadow_proof_rss_campaign_entry test_siqs_shadow_proof_rss_holdout_probe_contract test_siqs_shadow_proof_rss_holdout_probe_record_codec test_siqs_shadow_proof_rss_holdout_stream_join test_siqs_shadow_proof_prefer test_siqs_shadow_prefer_route test_siqs_shadow_cross_size"
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
    test_sha256
    test_thread_pool
    test_ordered_parallel_map
    test_fixed_slot_executor
    test_logger
    test_primes
    test_timer
    test_process_memory
    test_bounded_child_process
    test_durable_immutable_file
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
    test_siqs_live_sieve_capture
    test_siqs_2lp_graph
    test_siqs_2lp_materializer
    test_siqs_2lp_adapter
    test_siqs_2lp_congruence
    test_siqs_post_merge_row
    test_siqs_shadow_assembly
    test_siqs_shadow_linear_algebra
    test_siqs_shadow_proof_runner
    test_siqs_shadow_proof_observe
    test_siqs_shadow_proof_observe_record_codec
    test_siqs_runtime_facts
    test_siqs_shadow_proof_rss_probe_execution_identity
    test_siqs_shadow_observe_rss_holdouts
    test_siqs_shadow_proof_rss_gate
    test_siqs_shadow_proof_rss_terminal_gate_record
    test_siqs_shadow_proof_rss_policy_record
    test_siqs_shadow_proof_rss_campaign
    test_siqs_shadow_proof_rss_campaign_journal
    test_siqs_shadow_proof_rss_campaign_journal_codec
    test_siqs_shadow_proof_rss_campaign_journal_layout
    test_siqs_shadow_proof_rss_campaign_artifact_layout
    test_siqs_shadow_proof_rss_campaign_entry
    test_siqs_shadow_proof_rss_holdout_probe_contract
    test_siqs_shadow_proof_rss_holdout_probe_record_codec
    test_siqs_shadow_proof_rss_holdout_stream_join
    test_siqs_shadow_proof_prefer
    test_siqs_shadow_prefer_route
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
    test_fixed_slot_executor
    test_candidate_batch
    test_relation_collector
    test_relation_reduction_engine
    test_structured_parallel_prepare
    test_structured_batch_commit
    test_structured_parallel_driver
    test_structured_parallel_failures
    test_structured_incidence_builder
    test_siqs_shadow_linear_algebra
    test_siqs_shadow_proof_runner
    test_siqs_shadow_proof_observe
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
    test_sha256              10
    test_thread_pool         10
    test_ordered_parallel_map 10
    test_fixed_slot_executor 10
    test_logger              10
    test_primes              10
    test_timer               10
    test_process_memory      10
    test_bounded_child_process 30
    test_durable_immutable_file 10
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
    test_siqs_live_sieve_capture 10
    test_siqs_2lp_graph      10
    test_siqs_2lp_materializer 10
    test_siqs_2lp_adapter    10
    test_siqs_2lp_congruence 10
    test_siqs_post_merge_row 10
    test_siqs_shadow_assembly 10
    test_siqs_shadow_linear_algebra 10
    test_siqs_shadow_proof_runner 10
    test_siqs_shadow_proof_observe 10
    test_siqs_shadow_proof_observe_record_codec 10
    test_siqs_runtime_facts 10
    test_siqs_shadow_proof_rss_probe_execution_identity 10
    test_siqs_shadow_observe_rss_holdouts 10
    test_siqs_shadow_proof_rss_gate 10
    test_siqs_shadow_proof_rss_terminal_gate_record 10
    test_siqs_shadow_proof_rss_policy_record 10
    test_siqs_shadow_proof_rss_campaign 10
    test_siqs_shadow_proof_rss_campaign_journal 10
    test_siqs_shadow_proof_rss_campaign_journal_codec 10
    test_siqs_shadow_proof_rss_campaign_journal_layout 10
    test_siqs_shadow_proof_rss_campaign_artifact_layout 10
    test_siqs_shadow_proof_rss_campaign_journal_store 120
    test_siqs_shadow_proof_rss_campaign_entry 10
    test_siqs_shadow_proof_rss_holdout_probe_contract 10
    test_siqs_shadow_proof_rss_holdout_probe_record_codec 10
    test_siqs_shadow_proof_rss_holdout_stream_join 10
    test_siqs_shadow_proof_prefer 10
    test_siqs_shadow_prefer_route 10
    test_siqs_shadow_cross_size 60
    test_siqs_shadow_matrix_bench 600
)

# 测试速度分级 (用于 list 显示)
typeset -A TEST_TIER
TEST_TIER=(
    test_integer             "instant"
    test_small_vector        "instant"
    test_sha256              "instant"
    test_thread_pool         "instant"
    test_ordered_parallel_map "instant"
    test_fixed_slot_executor "instant"
    test_logger              "instant"
    test_primes              "instant"
    test_timer               "instant"
    test_process_memory      "instant"
    test_bounded_child_process "instant"
    test_durable_immutable_file "instant"
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
    test_siqs_live_sieve_capture "instant"
    test_siqs_2lp_graph      "instant"
    test_siqs_2lp_materializer "instant"
    test_siqs_2lp_adapter    "instant"
    test_siqs_2lp_congruence "instant"
    test_siqs_post_merge_row "instant"
    test_siqs_shadow_assembly "instant"
    test_siqs_shadow_linear_algebra "instant"
    test_siqs_shadow_proof_runner "instant"
    test_siqs_shadow_proof_observe "instant"
    test_siqs_shadow_proof_observe_record_codec "instant"
    test_siqs_runtime_facts "instant"
    test_siqs_shadow_proof_rss_probe_execution_identity "instant"
    test_siqs_shadow_observe_rss_holdouts "instant"
    test_siqs_shadow_proof_rss_gate "instant"
    test_siqs_shadow_proof_rss_terminal_gate_record "instant"
    test_siqs_shadow_proof_rss_policy_record "instant"
    test_siqs_shadow_proof_rss_campaign "instant"
    test_siqs_shadow_proof_rss_campaign_journal "instant"
    test_siqs_shadow_proof_rss_campaign_journal_codec "instant"
    test_siqs_shadow_proof_rss_campaign_journal_layout "instant"
    test_siqs_shadow_proof_rss_campaign_artifact_layout "instant"
    test_siqs_shadow_proof_rss_campaign_journal_store "fast"
    test_siqs_shadow_proof_rss_campaign_entry "instant"
    test_siqs_shadow_proof_rss_holdout_probe_contract "instant"
    test_siqs_shadow_proof_rss_holdout_probe_record_codec "instant"
    test_siqs_shadow_proof_rss_holdout_stream_join "instant"
    test_siqs_shadow_proof_prefer "instant"
    test_siqs_shadow_prefer_route "instant"
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
    util           "core polynomial factor_base sieve cofactor relation linalg sqrt siqs"
    polynomial     "factor_base sieve"
    factor_base    "sieve cofactor relation linalg"
    sieve          "relation api"
    cofactor       "relation"
    relation       "linalg api"
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
        tests/test_sha256.cpp) echo "util" ;;
        tests/test_squfof*.cpp|tests/support/squfof_*.hpp|tests/fixtures/squfof_*.hpp) echo "cofactor" ;;
        tests/test_relation_collector.cpp|tests/test_relation_reduction_engine.cpp|tests/test_ooc_store_integrity.cpp) echo "relation" ;;
        tests/test_structured*.cpp) echo "relation" ;;
        tests/test_api.cpp|*api/*) echo "api" ;;
        tests/test_siqs*.cpp|*siqs/*) echo "siqs" ;;
        tests/test_sieve_checkpoint.cpp|tests/test_distributed_sieve.cpp) echo "sieve" ;;
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
    "$GNFS_TEST_PYTHON" -c 'import time; print(int(time.time()*1000))'
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

validate_50d_experiment_v2_schema() {
    local record="$1"
    local label="$2"
    "$GNFS_TEST_PYTHON" - "$record" "$label" <<'PY'
import re
import sys
from decimal import Decimal, InvalidOperation


class SchemaError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise SchemaError(message)


EXPECTED_KEYS = """
scope claim_boundary stop_after pipeline_batch_mode candidate_chunk_size
candidate_rss_sample_policy cofactor_inner_parallel_policy status failure_stage
n n_digits n_bits max_special_q max_special_q_batch_workers special_q_processed
special_q_batch_worker_limit special_q_batch_peak_workers special_q_batch_count
special_q_batch_peak_size max_local_sieve_threads_requested local_sieve_thread_budget
special_q_batch_peak_assigned_threads special_q_worker_peak_sieve_threads candidates_total
candidate_batch_peak_workers candidate_batch_total_chunks candidate_batch_peak_chunks
candidate_batch_peak_candidates candidate_batch_rss_sample_candidates
candidate_batch_after_generation_current_rss_bytes
candidate_batch_after_cofactor_current_rss_bytes
candidate_batch_after_release_current_rss_bytes rational_fb_columns algebraic_fb_columns
base_factor_columns initial_raw_target first_round_complete sieve_rounds_completed
sieve_stop_reason resume_scope attempted_resume attempted_distributed sge_attempted
solver_attempted sqrt_attempted factorization_attempted route route_evidence strategy storage
generation raw_rows raw_duplicates input_lp_columns input_lp_w1 input_lp_w2 input_lp_w3
input_lp_w4plus output_rows output_lp_columns structured_commits structured_emitted_rows
structured_stop incidence_shards incidence_requested_workers incidence_peak_workers
raw_digest_low raw_digest_high output_digest_low output_digest_high matrix_rows matrix_cols
matrix_nonzeros matrix_signed_delta matrix_row_mapping_identity structured_filter_records
structured_matrix_records raw_pair_observed raw_pair_removed output_pair_observed
output_pair_retained_by_matrix output_pair_removed output_lease_removed process_rss_scope
process_rss_backend process_current_rss_supported process_peak_rss_supported
process_current_rss_bytes process_peak_rss_bytes rss_start_current_bytes rss_start_peak_bytes
rss_after_polynomial_current_bytes rss_after_polynomial_peak_bytes
rss_after_factor_base_current_bytes rss_after_factor_base_peak_bytes
rss_after_sieve_current_bytes rss_after_sieve_peak_bytes rss_after_matrix_current_bytes
rss_after_matrix_peak_bytes rss_after_cleanup_current_bytes rss_after_cleanup_peak_bytes
polynomial_ms factor_base_ms sieve_ms candidate_generation_s candidate_cofactor_s matrix_ms
wall_ms error
""".split()

BOOLEAN_FIELDS = {
    "first_round_complete", "attempted_resume", "attempted_distributed", "sge_attempted",
    "solver_attempted", "sqrt_attempted", "factorization_attempted",
    "matrix_row_mapping_identity", "raw_pair_observed", "raw_pair_removed",
    "output_pair_observed", "output_pair_retained_by_matrix", "output_pair_removed",
    "output_lease_removed", "process_current_rss_supported", "process_peak_rss_supported",
}

OPTIONAL_UINT_FIELDS = {
    "candidate_batch_after_generation_current_rss_bytes",
    "candidate_batch_after_cofactor_current_rss_bytes",
    "candidate_batch_after_release_current_rss_bytes",
    "process_current_rss_bytes", "process_peak_rss_bytes",
    "rss_start_current_bytes", "rss_start_peak_bytes",
    "rss_after_polynomial_current_bytes", "rss_after_polynomial_peak_bytes",
    "rss_after_factor_base_current_bytes", "rss_after_factor_base_peak_bytes",
    "rss_after_sieve_current_bytes", "rss_after_sieve_peak_bytes",
    "rss_after_matrix_current_bytes", "rss_after_matrix_peak_bytes",
    "rss_after_cleanup_current_bytes", "rss_after_cleanup_peak_bytes",
}

FLOAT_FIELDS = {"candidate_generation_s", "candidate_cofactor_s"}
SIGNED_OPTIONAL_FIELDS = {"matrix_signed_delta"}
BIG_UINT_FIELDS = {"n"}
STRING_FIELDS = {
    "scope", "claim_boundary", "stop_after", "pipeline_batch_mode",
    "candidate_rss_sample_policy", "cofactor_inner_parallel_policy", "status",
    "failure_stage", "sieve_stop_reason", "resume_scope", "route", "route_evidence",
    "strategy", "storage", "structured_stop", "process_rss_scope", "process_rss_backend",
    "error",
}
UINT_FIELDS = set(EXPECTED_KEYS) - (
    BOOLEAN_FIELDS | OPTIONAL_UINT_FIELDS | FLOAT_FIELDS | SIGNED_OPTIONAL_FIELDS |
    BIG_UINT_FIELDS | STRING_FIELDS
)
require(
    BOOLEAN_FIELDS | OPTIONAL_UINT_FIELDS | FLOAT_FIELDS | SIGNED_OPTIONAL_FIELDS |
    BIG_UINT_FIELDS | STRING_FIELDS | UINT_FIELDS == set(EXPECTED_KEYS),
    "internal schema field coverage is incomplete",
)

UINT_RE = re.compile(r"(?:0|[1-9][0-9]*)\Z")
SIGNED_RE = re.compile(r"(?:0|[1-9][0-9]*|-[1-9][0-9]*)\Z")
FLOAT_RE = re.compile(
    r"(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?\Z"
)
TOKEN_RE = re.compile(r"[A-Za-z0-9_.-]+\Z")
UINT64_MAX = (1 << 64) - 1
INT64_MIN = -(1 << 63)
INT64_MAX = (1 << 63) - 1


def parse_uint(value, field, bounded=True):
    require(UINT_RE.fullmatch(value) is not None, f"{field} is not canonical unsigned decimal")
    number = int(value)
    if bounded:
        require(number <= UINT64_MAX, f"{field} exceeds uint64")
    return number


def parse_optional_uint(value, field):
    if value == "na":
        return None
    return parse_uint(value, field)


def parse_record(line):
    require(line.isascii(), "record is not ASCII")
    require("\n" not in line and "\r" not in line and "\x00" not in line,
            "record contains a forbidden control byte")
    tokens = line.split(" ")
    require(tokens[0] == "GNFS_EXPERIMENT_V2", "record prefix mismatch")
    require(len(tokens) == len(EXPECTED_KEYS) + 1, "field count mismatch")

    fields = {}
    observed_keys = []
    for token in tokens[1:]:
        require(token.count("=") == 1, "field token must contain one equals sign")
        key, value = token.split("=", 1)
        require(key and value, "field key/value must be nonempty")
        require(key not in fields, f"duplicate field {key}")
        fields[key] = value
        observed_keys.append(key)
    require(observed_keys == EXPECTED_KEYS, "field set or order mismatch")

    for field in BOOLEAN_FIELDS:
        require(fields[field] in {"true", "false"}, f"{field} is not canonical boolean")
    for field in UINT_FIELDS:
        parse_uint(fields[field], field)
    for field in BIG_UINT_FIELDS:
        parse_uint(fields[field], field, bounded=False)
    for field in OPTIONAL_UINT_FIELDS:
        parse_optional_uint(fields[field], field)
    for field in SIGNED_OPTIONAL_FIELDS:
        value = fields[field]
        require(value == "na" or SIGNED_RE.fullmatch(value) is not None,
                f"{field} is not canonical signed decimal or na")
        if value != "na":
            number = int(value)
            require(INT64_MIN <= number <= INT64_MAX, f"{field} exceeds int64")
    for field in FLOAT_FIELDS:
        value = fields[field]
        require(FLOAT_RE.fullmatch(value) is not None, f"{field} is not canonical decimal")
        try:
            number = Decimal(value)
        except InvalidOperation as error:
            raise SchemaError(f"{field} is not a finite decimal") from error
        require(number.is_finite() and number >= 0, f"{field} must be finite and nonnegative")
    for field in STRING_FIELDS:
        require(TOKEN_RE.fullmatch(fields[field]) is not None,
                f"{field} contains a non-token value")

    require(fields["scope"] == "bounded_50d_prefix_probe", "scope mismatch")
    require(fields["claim_boundary"] == "relation_reduction_and_matrix_shape_only",
            "claim boundary mismatch")
    require(fields["stop_after"] == "matrix_build", "stop boundary mismatch")
    require(fields["pipeline_batch_mode"] == "two_stage_candidate_batch",
            "pipeline batch mode mismatch")
    require(fields["candidate_rss_sample_policy"] == "first_max_candidates",
            "candidate RSS policy mismatch")
    require(fields["cofactor_inner_parallel_policy"] == "forced_sequential",
            "cofactor inner policy mismatch")
    require(fields["status"] == "pass" and fields["failure_stage"] == "none",
            "successful runner received a non-pass record")
    require(fields["error"] == "none", "pass record carries an error")
    require(fields["resume_scope"] == "none", "probe unexpectedly reports resume scope")
    require(fields["process_rss_scope"] == "self_lifetime", "RSS scope mismatch")
    require(fields["sieve_stop_reason"] in {
        "effective_column_excess", "recovered_finalized_corpus", "insufficient_raw_relations",
        "special_q_budget_reached", "special_q_range_exhausted",
        "adaptive_round_limit_reached", "distributed_wave_complete",
    }, "unknown sieve stop reason")
    require(fields["route"] in {"legacy", "structured"}, "unknown route")
    require(fields["route_evidence"] in {
        "production_legacy_ooc", "production_direct_ooc",
    }, "unknown route evidence")
    require(fields["strategy"] in {"standard_v0", "structured"}, "unknown strategy")
    require(fields["storage"] in {"in_memory", "finalized_ooc"}, "unknown storage")
    require(fields["structured_stop"] in {
        "not_started", "no_candidates", "budget_limit", "persistence_limit",
    }, "unknown structured stop reason")
    require(fields["process_rss_backend"] in {
        "unsupported", "unobserved", "darwin_getrusage", "linux_getrusage", "windows_psapi",
    }, "unknown process RSS backend")

    require(
        parse_uint(fields["base_factor_columns"], "base_factor_columns") ==
        parse_uint(fields["rational_fb_columns"], "rational_fb_columns") +
        parse_uint(fields["algebraic_fb_columns"], "algebraic_fb_columns"),
        "factor-base column total mismatch",
    )
    require(fields["matrix_signed_delta"] != "na", "pass record lacks matrix delta")
    require(
        int(fields["matrix_signed_delta"]) ==
        parse_uint(fields["matrix_rows"], "matrix_rows") -
        parse_uint(fields["matrix_cols"], "matrix_cols"),
        "matrix signed delta mismatch",
    )

    candidate_rss = [
        fields["candidate_batch_after_generation_current_rss_bytes"],
        fields["candidate_batch_after_cofactor_current_rss_bytes"],
        fields["candidate_batch_after_release_current_rss_bytes"],
    ]
    require(all(value == "na" for value in candidate_rss) or
            all(value != "na" for value in candidate_rss),
            "candidate RSS fields are partially populated")

    current_supported = fields["process_current_rss_supported"] == "true"
    peak_supported = fields["process_peak_rss_supported"] == "true"
    current_fields = [
        "process_current_rss_bytes", "rss_start_current_bytes",
        "rss_after_polynomial_current_bytes", "rss_after_factor_base_current_bytes",
        "rss_after_sieve_current_bytes", "rss_after_matrix_current_bytes",
        "rss_after_cleanup_current_bytes",
    ]
    peak_fields = [
        "process_peak_rss_bytes", "rss_start_peak_bytes", "rss_after_polynomial_peak_bytes",
        "rss_after_factor_base_peak_bytes", "rss_after_sieve_peak_bytes",
        "rss_after_matrix_peak_bytes", "rss_after_cleanup_peak_bytes",
    ]
    require(all((fields[key] != "na") == current_supported for key in current_fields),
            "current RSS support/value linkage mismatch")
    require(all((fields[key] != "na") == peak_supported for key in peak_fields),
            "peak RSS support/value linkage mismatch")
    require((candidate_rss[0] != "na") == current_supported,
            "candidate/process current RSS support mismatch")
    require(fields["process_current_rss_bytes"] == fields["rss_after_cleanup_current_bytes"],
            "terminal current RSS mismatch")
    require(fields["process_peak_rss_bytes"] == fields["rss_after_cleanup_peak_bytes"],
            "terminal peak RSS mismatch")
    if fields["process_rss_backend"] in {"unsupported", "unobserved"}:
        require(not current_supported and not peak_supported,
                "unsupported backend reports supported RSS")
    else:
        require(current_supported and peak_supported, "supported backend omitted RSS")
        phase_currents = [int(fields[key]) for key in current_fields[1:]]
        phase_peaks = [int(fields[key]) for key in peak_fields[1:]]
        require(all(current <= peak for current, peak in zip(phase_currents, phase_peaks)),
                "phase current RSS exceeds lifetime peak")
        require(phase_peaks == sorted(phase_peaks), "lifetime peak RSS regressed")
    return fields


def expect_rejected(line, description):
    try:
        parse_record(line)
    except SchemaError:
        return
    raise SchemaError(f"schema self-check accepted {description}")


record = sys.argv[1]
label = sys.argv[2]
try:
    parsed = parse_record(record)
    tokens = record.split(" ")
    status_index = EXPECTED_KEYS.index("status") + 1
    max_q_index = EXPECTED_KEYS.index("max_special_q") + 1
    bool_index = EXPECTED_KEYS.index("first_round_complete") + 1
    rss_index = EXPECTED_KEYS.index("process_current_rss_bytes") + 1
    route_index = EXPECTED_KEYS.index("route") + 1

    expect_rejected(" ".join(tokens[:status_index] + tokens[status_index + 1:]),
                    "a missing field")
    duplicate = tokens.copy()
    duplicate[status_index] = tokens[1]
    expect_rejected(" ".join(duplicate), "a duplicate field")
    unknown = tokens.copy()
    unknown[status_index] = "unknown_field=1"
    expect_rejected(" ".join(unknown), "an unknown field")
    reordered = tokens.copy()
    reordered[1], reordered[2] = reordered[2], reordered[1]
    expect_rejected(" ".join(reordered), "field reordering")
    noncanonical = tokens.copy()
    noncanonical[max_q_index] = "max_special_q=01"
    expect_rejected(" ".join(noncanonical), "a noncanonical integer")
    invalid_bool = tokens.copy()
    invalid_bool[bool_index] = "first_round_complete=TRUE"
    expect_rejected(" ".join(invalid_bool), "a noncanonical boolean")
    invalid_enum = tokens.copy()
    invalid_enum[route_index] = "route=experimental"
    expect_rejected(" ".join(invalid_enum), "an unknown enum value")
    partial_rss = tokens.copy()
    partial_rss[rss_index] = (
        "process_current_rss_bytes=na"
        if parsed["process_current_rss_bytes"] != "na"
        else "process_current_rss_bytes=1"
    )
    expect_rejected(" ".join(partial_rss), "partial RSS linkage")
except SchemaError as error:
    print(f"{label}: GNFS_EXPERIMENT_V2 schema error: {error}", file=sys.stderr)
    sys.exit(1)
PY
}

validate_50d_uint32_argument() {
    local value="$1"
    local label="$2"
    local minimum="$3"
    local maximum="4294967295"
    if [[ ! "$value" =~ ^[1-9][0-9]*$ ]] ||
       (( ${#value} > ${#maximum} )) ||
       (( value < minimum || value > maximum )); then
        log_fail "${label} 必须在 ${minimum}..${maximum}（传入: ${value}）"
        return 1
    fi
}

validate_50d_batch_workers() {
    local value="$1"
    case "$value" in
        1|2|3|4) ;;
        *)
            log_fail "max_batch_workers 必须在 1..4（传入: ${value}）"
            return 1
            ;;
    esac
}

validate_50d_local_threads() {
    local value="$1"
    if [[ "$value" != "auto" ]]; then
        validate_50d_uint32_argument "$value" "max_local_sieve_threads" 1
    fi
}

run_50d_probe_with_timeout() {
    local default_timeout="$1"
    shift
    local saved_timeout="$TIMEOUT"
    local saved_timeout_explicit="$TIMEOUT_EXPLICIT"
    local bounded_timeout="$default_timeout"
    if (( TIMEOUT_EXPLICIT )); then
        bounded_timeout="$TIMEOUT"
    fi
    validate_50d_uint32_argument "$bounded_timeout" "50 位探针 timeout（秒）" 1 ||
        return 1

    TIMEOUT="$bounded_timeout"
    TIMEOUT_EXPLICIT=1
    local probe_status=0
    run_single_test test_structured_ooc_50d_probe "$@" || probe_status=$?
    TIMEOUT="$saved_timeout"
    TIMEOUT_EXPLICIT="$saved_timeout_explicit"
    return "$probe_status"
}

self_check_50d_probe_cli() {
    local executable="${GNFS_50D_PROBE_EXECUTABLE:-${BUILD_DIR}/test_structured_ooc_50d_probe}"
    "$GNFS_TEST_PYTHON" - "$executable" <<'PY'
import subprocess
import sys
import os
import tempfile
from pathlib import Path


executable = sys.argv[1]
invalid_cases = [
    ["--strategy"],
    ["--strategy", "legacy", "--strategy", "structured"],
    ["--strategy", "invalid"],
    ["--max-special-q", "0"],
    ["--max-special-q", "4294967296"],
    ["--max-special-q", "18446744073709551616"],
    ["--max-special-q", "1", "--max-special-q=2"],
    ["--max-special-q-batch-workers", "0"],
    ["--max-special-q-batch-workers", "5"],
    ["--max-local-sieve-threads", "0"],
    ["--max-local-sieve-threads", "4294967296"],
    ["--ooc-base", ""],
    ["--unknown"],
]
valid_help_cases = [
    ["--help"],
    [
        "--strategy", "legacy", "--max-special-q", "1",
        "--max-special-q-batch-workers", "1", "--max-local-sieve-threads", "1",
        "--ooc-base", "unused-help-path", "--help",
    ],
    [
        "--strategy=structured", "--max-special-q=4294967295",
        "--max-special-q-batch-workers=4", "--max-local-sieve-threads=4294967295",
        "--help",
    ],
]

for arguments in invalid_cases:
    with tempfile.TemporaryDirectory(prefix="gnfs_50d_cli_contract_") as temp_dir:
        environment = dict(os.environ)
        environment["TMPDIR"] = temp_dir
        result = subprocess.run(
            [executable, *arguments], capture_output=True, text=True, timeout=5, check=False,
            env=environment,
        )
        if result.returncode == 0:
            raise SystemExit(f"50d CLI self-check accepted invalid arguments: {arguments}")
        records = [
            line for line in result.stdout.splitlines()
            if line.startswith("GNFS_EXPERIMENT_V2 ")
        ]
        if len(records) != 1:
            raise SystemExit(f"50d CLI self-check lost its single failure record: {arguments}")
        fields = dict(token.split("=", 1) for token in records[0].split()[1:])
        if fields.get("status") != "fail" or fields.get("failure_stage") != "cli" or \
                fields.get("error") in {None, "none"}:
            raise SystemExit(
                f"50d CLI self-check crossed the CLI rejection boundary: {arguments}"
            )
        if any(Path(temp_dir).iterdir()):
            raise SystemExit(f"50d CLI self-check created artifacts: {arguments}")

for arguments in valid_help_cases:
    result = subprocess.run(
        [executable, *arguments], capture_output=True, text=True, timeout=5, check=False
    )
    if result.returncode != 0 or not result.stdout.startswith("Usage: ") or \
            "GNFS_EXPERIMENT_V2 " in result.stdout:
        raise SystemExit(f"50d CLI self-check rejected valid help boundary: {arguments}")
PY
}

self_check_50d_probe_contracts() {
    local executable="${GNFS_50D_PROBE_EXECUTABLE:-${BUILD_DIR}/test_structured_ooc_50d_probe}"
    if [[ ! -x "$executable" ]]; then
        log_fail "50 位探针合同二进制不存在: ${executable}"
        return 1
    fi
    self_check_50d_probe_cli || return 1

    local fixture_output fixture_status=0
    fixture_output=$("$executable" --emit-contract-fixture 2>&1) || fixture_status=$?
    if (( fixture_status != 0 )); then
        log_fail "50 位探针合同 fixture 生成失败 (exit=${fixture_status})"
        return 1
    fi

    if [[ "$fixture_output" == *"GNFS_EXPERIMENT_V2 "* ]]; then
        log_fail "50 位探针 synthetic fixture 冒充了 production V2 证据"
        return 1
    fi

    local saved_run_output="$RUN_OUTPUT"
    RUN_OUTPUT="$fixture_output"
    if ! capture_single_measurement_record "GNFS_EXPERIMENT_FIXTURE_V2 " \
        "50 位探针合同 fixture"; then
        RUN_OUTPUT="$saved_run_output"
        return 1
    fi
    local fixture_record="${MEASUREMENT_RECORD/GNFS_EXPERIMENT_FIXTURE_V2/GNFS_EXPERIMENT_V2}"
    RUN_OUTPUT="$saved_run_output"

    validate_50d_route_record "$fixture_record" structured false \
        "50 位探针合同 fixture" 4 4 auto
}

expect_measurement_field() {
    local record="$1"
    local key="$2"
    local expected="$3"
    local label="$4"
    local actual
    if ! actual=$(measurement_record_field "$record" "$key"); then
        log_fail "${label} 缺少或重复字段: ${key}"
        return 1
    fi
    if [[ "$actual" != "$expected" ]]; then
        log_fail "${label} 字段 ${key}=${actual}，预期 ${expected}"
        return 1
    fi
}

validate_50d_route_record() {
    local record="$1"
    local route="$2"
    local expected_complete="$3"
    local label="$4"
    local expected_max_special_q="$5"
    local expected_batch_workers="$6"
    local expected_local_threads="$7"
    if [[ "$expected_local_threads" == "auto" ]]; then
        expected_local_threads=0
    fi
    validate_50d_experiment_v2_schema "$record" "$label" || return 1
    local expected_strategy expected_storage expected_evidence
    case "$route" in
        legacy)
            expected_strategy="standard_v0"
            expected_storage="in_memory"
            expected_evidence="production_legacy_ooc"
            ;;
        structured)
            expected_strategy="structured"
            expected_storage="finalized_ooc"
            expected_evidence="production_direct_ooc"
            ;;
        *)
            log_fail "${label} 使用未知 route: ${route}"
            return 1
            ;;
    esac

    expect_measurement_field "$record" status pass "$label" &&
        expect_measurement_field "$record" failure_stage none "$label" &&
        expect_measurement_field "$record" scope bounded_50d_prefix_probe "$label" &&
        expect_measurement_field "$record" claim_boundary \
            relation_reduction_and_matrix_shape_only "$label" &&
        expect_measurement_field "$record" stop_after matrix_build "$label" &&
        expect_measurement_field "$record" pipeline_batch_mode \
            two_stage_candidate_batch "$label" &&
        expect_measurement_field "$record" n \
            16000000000000004000000216000000000000027000000729 "$label" &&
        expect_measurement_field "$record" n_digits 50 "$label" &&
        expect_measurement_field "$record" n_bits 164 "$label" &&
        expect_measurement_field "$record" max_special_q \
            "$expected_max_special_q" "$label" &&
        expect_measurement_field "$record" max_special_q_batch_workers \
            "$expected_batch_workers" "$label" &&
        expect_measurement_field "$record" max_local_sieve_threads_requested \
            "$expected_local_threads" "$label" &&
        expect_measurement_field "$record" route "$route" "$label" &&
        expect_measurement_field "$record" strategy "$expected_strategy" "$label" &&
        expect_measurement_field "$record" storage "$expected_storage" "$label" &&
        expect_measurement_field "$record" route_evidence "$expected_evidence" "$label" &&
        expect_measurement_field "$record" first_round_complete "$expected_complete" "$label" &&
        expect_measurement_field "$record" sieve_rounds_completed 1 "$label" &&
        expect_measurement_field "$record" resume_scope none "$label" &&
        expect_measurement_field "$record" attempted_resume false "$label" &&
        expect_measurement_field "$record" attempted_distributed false "$label" &&
        expect_measurement_field "$record" sge_attempted false "$label" &&
        expect_measurement_field "$record" solver_attempted false "$label" &&
        expect_measurement_field "$record" sqrt_attempted false "$label" &&
        expect_measurement_field "$record" factorization_attempted false "$label" &&
        expect_measurement_field "$record" raw_pair_observed true "$label" &&
        expect_measurement_field "$record" raw_pair_removed true "$label" &&
        expect_measurement_field "$record" process_rss_scope self_lifetime "$label" ||
        return 1

    local stop_reason
    if ! stop_reason=$(measurement_record_field "$record" sieve_stop_reason); then
        log_fail "${label} 缺少或重复字段: sieve_stop_reason"
        return 1
    fi
    if [[ "$expected_complete" == "false" ]]; then
        case "$stop_reason" in
            special_q_budget_reached|special_q_range_exhausted) ;;
            *)
                log_fail "${label} 短前缀 stop=${stop_reason}；仅接受 special-Q budget/range"
                return 1
                ;;
        esac
    else
        case "$stop_reason" in
            adaptive_round_limit_reached|effective_column_excess) ;;
            *)
                log_fail "${label} 完整首轮 stop=${stop_reason}；仅接受 round limit/effective excess"
                return 1
                ;;
        esac
    fi
}

run_50d_route_comparison() {
    local scope="$1"
    local expected_complete="$2"
    local default_cap="$3"
    local default_timeout="$4"
    shift 4
    if (( $# > 3 )); then
        log_fail "用法: $0 ${MODE} [max_special_q] [max_batch_workers] [max_local_sieve_threads|auto]"
        return 1
    fi

    local max_special_q="${1:-$default_cap}"
    local max_batch_workers="${2:-4}"
    local max_local_sieve_threads="${3:-auto}"
    local route_timeout="$default_timeout"
    if (( TIMEOUT_EXPLICIT )); then
        route_timeout="$TIMEOUT"
    fi
    validate_50d_uint32_argument "$max_special_q" max_special_q 1 || return 1
    validate_50d_batch_workers "$max_batch_workers" || return 1
    validate_50d_local_threads "$max_local_sieve_threads" || return 1
    validate_50d_uint32_argument "$route_timeout" "50 位探针 timeout（秒）" 1 || return 1

    if (( BUILD_TYPE_EXPLICIT )) && [[ "$BUILD_TYPE" != "Release" ]]; then
        log_fail "${MODE} 只接受 Release 构建（传入: ${BUILD_TYPE}）"
        return 1
    fi
    BUILD_TYPE="Release"
    if (( SKIP_BUILD )); then
        log_fail "${MODE} 不接受 --no-build；对照证据必须由本次请求的构建生成"
        return 1
    fi
    if (( RETRY_EXPLICIT )); then
        log_fail "${MODE} 不接受 --retry；自动重试会破坏每条 route 的 fresh-process 证据"
        return 1
    fi
    do_build
    if [[ ! -x "${BUILD_DIR}/test_structured_ooc_50d_probe" ]]; then
        log_fail "50 位探针二进制不存在: ${BUILD_DIR}/test_structured_ooc_50d_probe"
        return 1
    fi
    if ! self_check_50d_probe_cli; then
        log_fail "50 位探针 CLI 边界自检失败"
        return 1
    fi

    log_header "50 位 legacy/structured 独立进程对照"
    log_info "scope=${scope}; max_special_q=${max_special_q}; max_batch_workers=${max_batch_workers}; max_local_sieve_threads=${max_local_sieve_threads}; per_route_timeout=${route_timeout}s"

    local -A route_records
    local route probe_dir probe_base run_status
    local -a thread_args=()
    if [[ "$max_local_sieve_threads" != "auto" ]]; then
        thread_args=(--max-local-sieve-threads "$max_local_sieve_threads")
    fi
    local comparison_ready=1
    for route in legacy structured; do
        probe_dir=$(mktemp -d "${TMPDIR:-/tmp}/gnfs_50d_${route}.XXXXXX")
        probe_base="${probe_dir}/raw"
        log_info "route=${route}; 独占临时目录=${probe_dir}"
        run_status=0
        run_50d_probe_with_timeout "$route_timeout" \
            --strategy "$route" \
            --max-special-q "$max_special_q" \
            --max-special-q-batch-workers "$max_batch_workers" \
            "${thread_args[@]}" \
            --ooc-base "$probe_base" || run_status=$?
        if (( run_status != 0 )); then
            comparison_ready=0
            log_warn "route=${route} 探针失败，保留诊断工件: ${probe_dir}"
            continue
        fi
        if ! capture_single_measurement_record "GNFS_EXPERIMENT_V2 " \
            "50 位 route=${route} 探针"; then
            (( FAILED_TESTS += 1 ))
            comparison_ready=0
            log_warn "route=${route} 记录无效，保留诊断工件: ${probe_dir}"
            continue
        fi
        if ! validate_50d_route_record "$MEASUREMENT_RECORD" "$route" \
            "$expected_complete" "50 位 route=${route}" "$max_special_q" \
            "$max_batch_workers" "$max_local_sieve_threads"; then
            (( FAILED_TESTS += 1 ))
            comparison_ready=0
            log_warn "route=${route} 契约无效，保留诊断工件: ${probe_dir}"
            continue
        fi
        route_records[$route]="$MEASUREMENT_RECORD"
        print -r -- "$MEASUREMENT_RECORD"

        if [[ -e "${probe_base}.reldata" || -L "${probe_base}.reldata" ||
              -e "${probe_base}.relidx" || -L "${probe_base}.relidx" ]]; then
            log_fail "route=${route} 成功后仍残留原始 OOC pair: ${probe_base}"
            (( FAILED_TESTS += 1 ))
            comparison_ready=0
            continue
        fi
        if rmdir "$probe_dir"; then
            log_success "route=${route} 探针工件已完成生命周期清理"
        else
            log_fail "route=${route} 成功但临时目录非空，已保留: ${probe_dir}"
            (( FAILED_TESTS += 1 ))
            comparison_ready=0
        fi
    done

    local -a identity_fields=(
        scope claim_boundary stop_after pipeline_batch_mode candidate_chunk_size
        candidate_rss_sample_policy cofactor_inner_parallel_policy
        n_digits n_bits n max_special_q max_special_q_batch_workers
        special_q_processed special_q_batch_worker_limit special_q_batch_peak_workers
        special_q_batch_count special_q_batch_peak_size
        max_local_sieve_threads_requested local_sieve_thread_budget
        special_q_batch_peak_assigned_threads special_q_worker_peak_sieve_threads
        candidates_total candidate_batch_peak_workers candidate_batch_total_chunks
        candidate_batch_peak_chunks candidate_batch_peak_candidates
        candidate_batch_rss_sample_candidates
        rational_fb_columns algebraic_fb_columns base_factor_columns initial_raw_target
        sieve_rounds_completed first_round_complete resume_scope attempted_resume
        attempted_distributed sge_attempted solver_attempted sqrt_attempted
        factorization_attempted raw_rows raw_duplicates input_lp_columns
        input_lp_w1 input_lp_w2 input_lp_w3 input_lp_w4plus
        raw_digest_low raw_digest_high raw_pair_observed raw_pair_removed
    )
    local field reference value
    if (( comparison_ready )); then
        for field in "${identity_fields[@]}"; do
            if ! reference=$(measurement_record_field "${route_records[legacy]}" "$field"); then
                log_fail "legacy 记录缺少或重复身份字段: ${field}"
                (( FAILED_TESTS += 1 ))
                comparison_ready=0
                break
            fi
            if ! value=$(measurement_record_field "${route_records[structured]}" "$field"); then
                log_fail "structured 记录缺少或重复身份字段: ${field}"
                (( FAILED_TESTS += 1 ))
                comparison_ready=0
                break
            fi
            if [[ "$value" != "$reference" ]]; then
                log_fail "原始身份字段漂移: ${field}; legacy=${reference}, structured=${value}"
                (( FAILED_TESTS += 1 ))
                comparison_ready=0
                break
            fi
        done
    fi

    if (( comparison_ready )); then
        local shared_n shared_special_q shared_raw_rows shared_raw_duplicates
        local shared_lp_columns shared_lp_w1 shared_lp_w2 shared_lp_w3 shared_lp_w4plus
        local shared_raw_digest_low shared_raw_digest_high
        local shared_sq_worker_limit shared_sq_peak_workers shared_local_budget
        local shared_sq_assigned_threads shared_sq_peak_sieve_threads
        local shared_candidates shared_candidate_peak_workers shared_candidate_total_chunks
        local shared_candidate_peak_chunks shared_candidate_peak_candidates
        local legacy_stop structured_stop
        local legacy_output_rows structured_output_rows
        local legacy_output_lp structured_output_lp
        local legacy_output_digest_low legacy_output_digest_high
        local structured_output_digest_low structured_output_digest_high
        local legacy_matrix_rows legacy_matrix_cols legacy_matrix_nonzeros legacy_matrix_delta
        local structured_matrix_rows structured_matrix_cols structured_matrix_nonzeros
        local structured_matrix_delta legacy_matrix_mapping structured_matrix_mapping
        local legacy_wall structured_wall legacy_peak structured_peak
        shared_n=$(measurement_record_field "${route_records[legacy]}" n)
        shared_special_q=$(measurement_record_field "${route_records[legacy]}" special_q_processed)
        shared_raw_rows=$(measurement_record_field "${route_records[legacy]}" raw_rows)
        shared_raw_duplicates=$(measurement_record_field "${route_records[legacy]}" raw_duplicates)
        shared_lp_columns=$(measurement_record_field "${route_records[legacy]}" input_lp_columns)
        shared_lp_w1=$(measurement_record_field "${route_records[legacy]}" input_lp_w1)
        shared_lp_w2=$(measurement_record_field "${route_records[legacy]}" input_lp_w2)
        shared_lp_w3=$(measurement_record_field "${route_records[legacy]}" input_lp_w3)
        shared_lp_w4plus=$(measurement_record_field "${route_records[legacy]}" input_lp_w4plus)
        shared_raw_digest_low=$(measurement_record_field \
            "${route_records[legacy]}" raw_digest_low)
        shared_raw_digest_high=$(measurement_record_field \
            "${route_records[legacy]}" raw_digest_high)
        shared_sq_worker_limit=$(measurement_record_field \
            "${route_records[legacy]}" special_q_batch_worker_limit)
        shared_sq_peak_workers=$(measurement_record_field \
            "${route_records[legacy]}" special_q_batch_peak_workers)
        shared_local_budget=$(measurement_record_field \
            "${route_records[legacy]}" local_sieve_thread_budget)
        shared_sq_assigned_threads=$(measurement_record_field \
            "${route_records[legacy]}" special_q_batch_peak_assigned_threads)
        shared_sq_peak_sieve_threads=$(measurement_record_field \
            "${route_records[legacy]}" special_q_worker_peak_sieve_threads)
        shared_candidates=$(measurement_record_field "${route_records[legacy]}" candidates_total)
        shared_candidate_peak_workers=$(measurement_record_field \
            "${route_records[legacy]}" candidate_batch_peak_workers)
        shared_candidate_total_chunks=$(measurement_record_field \
            "${route_records[legacy]}" candidate_batch_total_chunks)
        shared_candidate_peak_chunks=$(measurement_record_field \
            "${route_records[legacy]}" candidate_batch_peak_chunks)
        shared_candidate_peak_candidates=$(measurement_record_field \
            "${route_records[legacy]}" candidate_batch_peak_candidates)
        legacy_stop=$(measurement_record_field "${route_records[legacy]}" sieve_stop_reason)
        structured_stop=$(measurement_record_field "${route_records[structured]}" sieve_stop_reason)
        legacy_output_rows=$(measurement_record_field "${route_records[legacy]}" output_rows)
        structured_output_rows=$(measurement_record_field "${route_records[structured]}" output_rows)
        legacy_output_lp=$(measurement_record_field "${route_records[legacy]}" output_lp_columns)
        structured_output_lp=$(measurement_record_field "${route_records[structured]}" output_lp_columns)
        legacy_output_digest_low=$(measurement_record_field "${route_records[legacy]}" output_digest_low)
        legacy_output_digest_high=$(measurement_record_field "${route_records[legacy]}" output_digest_high)
        structured_output_digest_low=$(measurement_record_field "${route_records[structured]}" output_digest_low)
        structured_output_digest_high=$(measurement_record_field "${route_records[structured]}" output_digest_high)
        legacy_matrix_rows=$(measurement_record_field "${route_records[legacy]}" matrix_rows)
        legacy_matrix_cols=$(measurement_record_field "${route_records[legacy]}" matrix_cols)
        legacy_matrix_nonzeros=$(measurement_record_field "${route_records[legacy]}" matrix_nonzeros)
        legacy_matrix_delta=$(measurement_record_field "${route_records[legacy]}" matrix_signed_delta)
        legacy_matrix_mapping=$(measurement_record_field \
            "${route_records[legacy]}" matrix_row_mapping_identity)
        structured_matrix_rows=$(measurement_record_field "${route_records[structured]}" matrix_rows)
        structured_matrix_cols=$(measurement_record_field "${route_records[structured]}" matrix_cols)
        structured_matrix_nonzeros=$(measurement_record_field "${route_records[structured]}" matrix_nonzeros)
        structured_matrix_delta=$(measurement_record_field "${route_records[structured]}" matrix_signed_delta)
        structured_matrix_mapping=$(measurement_record_field \
            "${route_records[structured]}" matrix_row_mapping_identity)
        legacy_wall=$(measurement_record_field "${route_records[legacy]}" wall_ms)
        structured_wall=$(measurement_record_field "${route_records[structured]}" wall_ms)
        legacy_peak=$(measurement_record_field "${route_records[legacy]}" process_peak_rss_bytes)
        structured_peak=$(measurement_record_field "${route_records[structured]}" process_peak_rss_bytes)

        print -r -- "GNFS_EXPERIMENT_COMPARISON_V2 status=pass scope=${scope} routes=legacy,structured n=${shared_n} max_special_q=${max_special_q} max_special_q_batch_workers=${max_batch_workers} max_local_sieve_threads=${max_local_sieve_threads} special_q_processed=${shared_special_q} special_q_batch_worker_limit=${shared_sq_worker_limit} special_q_batch_peak_workers=${shared_sq_peak_workers} local_sieve_thread_budget=${shared_local_budget} special_q_batch_peak_assigned_threads=${shared_sq_assigned_threads} special_q_worker_peak_sieve_threads=${shared_sq_peak_sieve_threads} candidates_total=${shared_candidates} candidate_batch_peak_workers=${shared_candidate_peak_workers} candidate_batch_total_chunks=${shared_candidate_total_chunks} candidate_batch_peak_chunks=${shared_candidate_peak_chunks} candidate_batch_peak_candidates=${shared_candidate_peak_candidates} first_round_complete=${expected_complete} legacy_stop=${legacy_stop} structured_stop=${structured_stop} raw_rows=${shared_raw_rows} raw_duplicates=${shared_raw_duplicates} input_lp_columns=${shared_lp_columns} input_lp_w1=${shared_lp_w1} input_lp_w2=${shared_lp_w2} input_lp_w3=${shared_lp_w3} input_lp_w4plus=${shared_lp_w4plus} raw_digest_low=${shared_raw_digest_low} raw_digest_high=${shared_raw_digest_high} raw_identity_fields=${#identity_fields[@]} legacy_output_rows=${legacy_output_rows} structured_output_rows=${structured_output_rows} legacy_output_lp_columns=${legacy_output_lp} structured_output_lp_columns=${structured_output_lp} legacy_output_digest_low=${legacy_output_digest_low} legacy_output_digest_high=${legacy_output_digest_high} structured_output_digest_low=${structured_output_digest_low} structured_output_digest_high=${structured_output_digest_high} legacy_matrix_rows=${legacy_matrix_rows} legacy_matrix_cols=${legacy_matrix_cols} legacy_matrix_nonzeros=${legacy_matrix_nonzeros} legacy_matrix_signed_delta=${legacy_matrix_delta} legacy_matrix_row_mapping_identity=${legacy_matrix_mapping} structured_matrix_rows=${structured_matrix_rows} structured_matrix_cols=${structured_matrix_cols} structured_matrix_nonzeros=${structured_matrix_nonzeros} structured_matrix_signed_delta=${structured_matrix_delta} structured_matrix_row_mapping_identity=${structured_matrix_mapping} legacy_wall_ms=${legacy_wall} structured_wall_ms=${structured_wall} legacy_peak_rss_bytes=${legacy_peak} structured_peak_rss_bytes=${structured_peak} timing_scope=fresh_process_per_route rss_scope=fresh_process_per_route timing_asserted=false rss_asserted=false promotion=false"
        log_success "legacy/structured 原始语料身份一致；策略输出与矩阵结果已分别记录"
    fi
    show_summary
}

# Validate both raw process streams before shell command substitution can strip
# terminators or hide NUL bytes. The protocol is deliberately ASCII-only:
# printable bytes plus one final LF per expected record, with no blank lines.
siqs_shadow_observe_validate_protocol() {
    "$GNFS_TEST_PYTHON" - "$@" <<'PY'
import re
import sys
import tempfile
from pathlib import Path


class ProtocolError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise ProtocolError(message)


def read_closed_stream(path, expected_lines, label):
    data = Path(path).read_bytes()
    if expected_lines == 0:
        require(data == b"", f"{label} must be byte-empty")
        return []
    require(data, f"{label} must not be empty")
    require(data.endswith(b"\n"), f"{label} must end in LF")
    require(data.count(b"\n") == expected_lines,
            f"{label} must contain exactly {expected_lines} LF-terminated record(s)")
    for byte in data:
        require(byte == 10 or 32 <= byte <= 126,
                f"{label} contains a byte outside printable ASCII plus LF")
    records = data[:-1].split(b"\n")
    require(len(records) == expected_lines and all(records),
            f"{label} contains an empty record")
    return [record.decode("ascii") for record in records]


def parse_ordered_record(line, prefix, order, label):
    tokens = line.split(" ")
    require(tokens and tokens[0] == prefix, f"{label} prefix mismatch")
    require(len(tokens) == len(order) + 1, f"{label} field cardinality mismatch")
    fields = {}
    observed_order = []
    for token in tokens[1:]:
        require(token.count("=") == 1, f"{label} token is not one key=value pair")
        key, value = token.split("=", 1)
        require(re.fullmatch(r"[a-z][a-z0-9_]*", key) is not None,
                f"{label} has an invalid key")
        require(value != "", f"{label} has an empty value")
        require(key not in fields, f"{label} repeats field {key}")
        fields[key] = value
        observed_order.append(key)
    require(observed_order == order, f"{label} field set/order is not the frozen schema")
    return fields


def uint(fields, key, positive=False):
    value = fields[key]
    require(re.fullmatch(r"0|[1-9][0-9]*", value) is not None,
            f"{key} is not a canonical unsigned integer")
    number = int(value)
    require(number <= (1 << 64) - 1, f"{key} exceeds uint64")
    if positive:
        require(number > 0, f"{key} must be positive")
    return number


def boolean(fields, key):
    require(fields[key] in ("true", "false"), f"{key} is not a canonical boolean")
    return fields[key] == "true"


BACKENDS = {"unsupported", "darwin_getrusage", "linux_getrusage", "windows_psapi"}


def validate_memory_fields(fields, label):
    for side in ("before", "after"):
        backend = fields[f"{side}_rss_backend"]
        require(backend in BACKENDS, f"{label} has an unknown {side} RSS backend")
        current_supported = boolean(fields, f"{side}_current_rss_supported")
        current = uint(fields, f"{side}_current_rss_bytes")
        peak_supported = boolean(fields, f"{side}_peak_rss_supported")
        peak = uint(fields, f"{side}_peak_rss_bytes")
        require((current > 0) == current_supported,
                f"{label} {side} current RSS support/value mismatch")
        require((peak > 0) == peak_supported,
                f"{label} {side} peak RSS support/value mismatch")
        if backend == "unsupported":
            require(not current_supported and not peak_supported,
                    f"{label} unsupported backend cannot report RSS")

    before_backend = fields["before_rss_backend"]
    after_backend = fields["after_rss_backend"]
    before_peak_supported = boolean(fields, "before_peak_rss_supported")
    after_peak_supported = boolean(fields, "after_peak_rss_supported")
    before_peak = uint(fields, "before_peak_rss_bytes")
    after_peak = uint(fields, "after_peak_rss_bytes")
    growth_supported = boolean(fields, "peak_growth_supported")
    growth = uint(fields, "peak_growth_bytes")
    comparable = (before_backend != "unsupported" and before_backend == after_backend and
                  before_peak_supported and after_peak_supported and after_peak >= before_peak)
    require(growth_supported == comparable, f"{label} peak growth support is inconsistent")
    require(growth == (after_peak - before_peak if comparable else 0),
            f"{label} peak RSS difference is not conserved")


PROBE_PREFIX = "GNFS_SIQS_SHADOW_PROOF_OBSERVE_PROBE_V1"
PROBE_ORDER = [
    "schema_version", "status", "profile_id", "build_type", "ndebug", "scope", "mode",
    "env_value", "sample_ordinal", "band", "digits", "n", "expected_factor",
    "expected_cofactor", "max_seconds", "factor_status", "factor", "cofactor",
    "factor_identity", "relations_found", "polynomials_used", "factor_wall_ns", "rss_scope",
    "before_rss_backend", "before_current_rss_supported", "before_current_rss_bytes",
    "before_peak_rss_supported", "before_peak_rss_bytes", "after_rss_backend",
    "after_current_rss_supported", "after_current_rss_bytes", "after_peak_rss_supported",
    "after_peak_rss_bytes", "peak_growth_supported", "peak_growth_bytes", "route", "promotion",
]

TELEMETRY_PREFIX = "GNFS_SIQS_SHADOW_PROOF_OBSERVE_V1"
TELEMETRY_ORDER = [
    "schema_version", "mode", "route", "rss_scope", "proof_attempted", "terminal", "stage",
    "fallback", "factor_found", "observe_wall_ns", "raw_relations", "raw_payload_supported",
    "raw_payload_bytes", "factor_base_columns", "large_prime_bound", "raw_relation_cap",
    "raw_payload_cap_bytes", "graph_edge_cap", "graph_cycle_cap", "graph_incidence_cap",
    "row_candidate_cap", "pretrim_row_cap", "minimum_row_excess", "trim_excess_rows",
    "assembly_workers", "matrix_max_dependencies", "matrix_workers",
    "matrix_parallel_column_threshold", "matrix_dense_bytes_cap", "matrix_dense_variable_cap",
    "adapter_input_relations", "adapter_full_relations", "adapter_accepted_one_lp",
    "adapter_accepted_two_lp", "adapter_rejected_relations", "adapter_malformed_source_shape",
    "adapter_unsupported_encoding", "adapter_invalid_one_large_prime",
    "adapter_invalid_two_large_prime_split", "adapter_exact_duplicate",
    "graph_evidence_supported", "graph_vertices", "graph_edges", "graph_components",
    "graph_cycles", "graph_cycle_incidences", "graph_max_cycle_length", "row_candidate_upper",
    "assembly_evidence_supported", "assembly_pretrim_rows", "assembly_selected_rows",
    "assembly_selected_full_rows", "assembly_selected_cycle_rows", "assembly_trimmed_rows",
    "assembly_fingerprint_supported", "assembly_source_fingerprint_low",
    "assembly_source_fingerprint_high", "assembly_pretrim_fingerprint_low",
    "assembly_pretrim_fingerprint_high", "assembly_selected_fingerprint_low",
    "assembly_selected_fingerprint_high", "projected_dense_bytes_supported",
    "projected_dense_bytes", "matrix_evidence_supported", "matrix_rows", "matrix_columns",
    "minimum_nullity", "dependencies_returned", "dependencies_examined",
    "dependencies_verified", "no_factor_count", "factor_found_count", "dependency_cap_reached",
    "dependency_fingerprint_supported", "dependency_fingerprint_low",
    "dependency_fingerprint_high", "first_failed_dependency_supported",
    "first_failed_dependency", "winning_dependency_supported", "winning_dependency",
    "winning_dependency_size_supported", "winning_dependency_size", "before_rss_backend",
    "before_current_rss_supported", "before_current_rss_bytes", "before_peak_rss_supported",
    "before_peak_rss_bytes", "after_rss_backend", "after_current_rss_supported",
    "after_current_rss_bytes", "after_peak_rss_supported", "after_peak_rss_bytes",
    "peak_growth_supported", "peak_growth_bytes", "promotion",
]


def validate_probe(line, expected_mode, expected_sample):
    fields = parse_ordered_record(line, PROBE_PREFIX, PROBE_ORDER, "probe stdout")
    exact = {
        "schema_version": "1", "status": "valid",
        "profile_id": "siqs50_production_shadow_observe_v1", "build_type": "Release",
        "ndebug": "true", "scope": "production_factor_fresh_process", "mode": expected_mode,
        "env_value": "0" if expected_mode == "off" else "observe", "band": "50", "digits": "50",
        "n": "18027426610499408447671494571938206274555088868093",
        "expected_factor": "2041646378661656688438487",
        "expected_cofactor": "8829847714527711737483339", "max_seconds": "30",
        "factor_status": "factor_found", "factor": "2041646378661656688438487",
        "cofactor": "8829847714527711737483339", "factor_identity": "pass",
        "relations_found": "1701", "rss_scope": "self_lifetime", "route": "legacy_result",
        "promotion": "false",
    }
    for key, expected in exact.items():
        require(fields[key] == expected, f"probe {key} mismatch")
    require(uint(fields, "sample_ordinal") == expected_sample, "probe sample_ordinal mismatch")
    require(0 <= expected_sample <= 3, "expected sample ordinal is outside 0..3")
    uint(fields, "polynomials_used", positive=True)
    uint(fields, "factor_wall_ns", positive=True)
    validate_memory_fields(fields, "probe")
    return fields


def validate_telemetry(line):
    fields = parse_ordered_record(line, TELEMETRY_PREFIX, TELEMETRY_ORDER, "observe stderr")
    exact = {
        "schema_version": "1", "mode": "observe", "route": "legacy_continue",
        "rss_scope": "self_lifetime", "proof_attempted": "true", "terminal": "factor_found",
        "stage": "factor_extraction", "fallback": "none", "factor_found": "true",
        "raw_payload_supported": "true", "factor_base_columns": "1601",
        "large_prime_bound": "3477480", "raw_relation_cap": "32768",
        "raw_payload_cap_bytes": "67108864", "graph_edge_cap": "16384",
        "graph_cycle_cap": "4096", "graph_incidence_cap": "262144",
        "row_candidate_cap": "4096", "pretrim_row_cap": "4096", "minimum_row_excess": "1",
        "trim_excess_rows": "100", "assembly_workers": "1", "matrix_max_dependencies": "64",
        "matrix_workers": "1", "matrix_parallel_column_threshold": "20000",
        "matrix_dense_bytes_cap": "268435456", "matrix_dense_variable_cap": "100000",
        "adapter_accepted_two_lp": "0", "graph_evidence_supported": "true",
        "assembly_evidence_supported": "true", "assembly_fingerprint_supported": "true",
        "projected_dense_bytes_supported": "true", "projected_dense_bytes": "345816",
        "matrix_evidence_supported": "true", "matrix_rows": "1701", "matrix_columns": "1601",
        "minimum_nullity": "100", "dependencies_returned": "64",
        "dependency_cap_reached": "true", "dependency_fingerprint_supported": "true",
        "first_failed_dependency_supported": "false", "first_failed_dependency": "0",
        "winning_dependency_supported": "true", "winning_dependency_size_supported": "true",
        "promotion": "false",
    }
    for key, expected in exact.items():
        require(fields[key] == expected, f"telemetry {key} mismatch")

    bool_fields = [
        "proof_attempted", "factor_found", "raw_payload_supported", "graph_evidence_supported",
        "assembly_evidence_supported", "assembly_fingerprint_supported",
        "projected_dense_bytes_supported", "matrix_evidence_supported", "dependency_cap_reached",
        "dependency_fingerprint_supported", "first_failed_dependency_supported",
        "winning_dependency_supported", "winning_dependency_size_supported", "promotion",
    ]
    for key in bool_fields:
        boolean(fields, key)

    numeric_fields = [key for key in TELEMETRY_ORDER if key not in {
        "mode", "route", "rss_scope", "proof_attempted", "terminal", "stage", "fallback",
        "factor_found", "raw_payload_supported", "graph_evidence_supported",
        "assembly_evidence_supported", "assembly_fingerprint_supported",
        "projected_dense_bytes_supported", "matrix_evidence_supported", "dependency_cap_reached",
        "dependency_fingerprint_supported", "first_failed_dependency_supported",
        "winning_dependency_supported", "winning_dependency_size_supported", "before_rss_backend",
        "before_current_rss_supported", "before_peak_rss_supported", "after_rss_backend",
        "after_current_rss_supported", "after_peak_rss_supported", "peak_growth_supported",
        "promotion",
    }]
    for key in numeric_fields:
        uint(fields, key)
    uint(fields, "observe_wall_ns", positive=True)
    raw = uint(fields, "raw_relations", positive=True)
    raw_payload = uint(fields, "raw_payload_bytes", positive=True)
    require(raw <= uint(fields, "raw_relation_cap"), "raw relation cap exceeded")
    require(raw_payload <= uint(fields, "raw_payload_cap_bytes"), "raw payload cap exceeded")

    adapter_input = uint(fields, "adapter_input_relations")
    full = uint(fields, "adapter_full_relations")
    one_lp = uint(fields, "adapter_accepted_one_lp")
    two_lp = uint(fields, "adapter_accepted_two_lp")
    rejected = uint(fields, "adapter_rejected_relations")
    require(adapter_input == raw, "adapter input/raw relation conservation failed")
    require(full + one_lp + two_lp + rejected == adapter_input,
            "adapter disposition conservation failed")
    rejection_sum = sum(uint(fields, key) for key in (
        "adapter_malformed_source_shape", "adapter_unsupported_encoding",
        "adapter_invalid_one_large_prime", "adapter_invalid_two_large_prime_split",
        "adapter_exact_duplicate"))
    require(rejection_sum == rejected, "adapter rejection taxonomy conservation failed")

    vertices = uint(fields, "graph_vertices")
    edges = uint(fields, "graph_edges")
    components = uint(fields, "graph_components")
    cycles = uint(fields, "graph_cycles")
    incidences = uint(fields, "graph_cycle_incidences")
    max_cycle = uint(fields, "graph_max_cycle_length")
    require(edges == one_lp + two_lp, "graph edge/accepted partial conservation failed")
    require(edges <= uint(fields, "graph_edge_cap"), "graph edge cap exceeded")
    require(cycles <= uint(fields, "graph_cycle_cap"), "graph cycle cap exceeded")
    require(incidences <= uint(fields, "graph_incidence_cap"), "graph incidence cap exceeded")
    require(edges + components == vertices + cycles, "graph cycle-rank identity failed")
    require((cycles == 0 and incidences == 0 and max_cycle == 0) or
            (cycles > 0 and incidences >= 2 * cycles and 2 <= max_cycle <= incidences),
            "graph cycle incidence/length invariant failed")
    row_upper = uint(fields, "row_candidate_upper")
    require(row_upper == full + cycles, "row candidate upper conservation failed")
    require(row_upper <= uint(fields, "row_candidate_cap"), "row candidate cap exceeded")

    pretrim = uint(fields, "assembly_pretrim_rows")
    selected = uint(fields, "assembly_selected_rows")
    selected_full = uint(fields, "assembly_selected_full_rows")
    selected_cycle = uint(fields, "assembly_selected_cycle_rows")
    trimmed = uint(fields, "assembly_trimmed_rows")
    require(pretrim <= row_upper and pretrim <= uint(fields, "pretrim_row_cap"),
            "assembly pretrim bound failed")
    require(pretrim == selected + trimmed, "assembly trim conservation failed")
    require(selected == selected_full + selected_cycle, "assembly origin conservation failed")
    require(selected_full <= full and selected_cycle <= cycles,
            "assembly selected-source bounds failed")
    require(selected == 1701, "frozen 50-digit selected row count mismatch")

    rows = uint(fields, "matrix_rows")
    columns = uint(fields, "matrix_columns")
    nullity = uint(fields, "minimum_nullity")
    require(rows == selected and rows > columns and nullity == rows - columns,
            "matrix shape/nullity conservation failed")
    projected = uint(fields, "projected_dense_bytes")
    require(projected == columns * ((rows + 63) // 64) * 8,
            "projected dense byte formula failed")
    require(projected <= uint(fields, "matrix_dense_bytes_cap") and
            rows <= uint(fields, "matrix_dense_variable_cap"), "matrix resource cap exceeded")

    returned = uint(fields, "dependencies_returned")
    examined = uint(fields, "dependencies_examined")
    verified = uint(fields, "dependencies_verified")
    no_factor = uint(fields, "no_factor_count")
    found = uint(fields, "factor_found_count")
    winning = uint(fields, "winning_dependency")
    winning_size = uint(fields, "winning_dependency_size")
    require(returned == uint(fields, "matrix_max_dependencies"),
            "dependency cap count mismatch")
    require(0 < examined <= returned and verified == examined,
            "dependency examined/verified conservation failed")
    require(no_factor + found == verified and found == 1,
            "dependency factor outcome conservation failed")
    require(winning == no_factor and winning + 1 == examined and winning < returned,
            "winning dependency ordinal conservation failed")
    require(0 < winning_size <= rows, "winning dependency size is invalid")
    validate_memory_fields(fields, "telemetry")
    return fields


def validate_paths(stdout_path, stderr_path, expected_mode, expected_sample):
    require(expected_mode in ("off", "observe"), "expected mode must be off or observe")
    stdout_records = read_closed_stream(stdout_path, 1, "probe stdout")
    stderr_records = read_closed_stream(stderr_path, 0 if expected_mode == "off" else 1,
                                        "probe stderr")
    validate_probe(stdout_records[0], expected_mode, expected_sample)
    if expected_mode == "observe":
        validate_telemetry(stderr_records[0])


SELF_PROBE = b"GNFS_SIQS_SHADOW_PROOF_OBSERVE_PROBE_V1 schema_version=1 status=valid profile_id=siqs50_production_shadow_observe_v1 build_type=Release ndebug=true scope=production_factor_fresh_process mode=observe env_value=observe sample_ordinal=1 band=50 digits=50 n=18027426610499408447671494571938206274555088868093 expected_factor=2041646378661656688438487 expected_cofactor=8829847714527711737483339 max_seconds=30 factor_status=factor_found factor=2041646378661656688438487 cofactor=8829847714527711737483339 factor_identity=pass relations_found=1701 polynomials_used=7780 factor_wall_ns=688934375 rss_scope=self_lifetime before_rss_backend=darwin_getrusage before_current_rss_supported=true before_current_rss_bytes=6209536 before_peak_rss_supported=true before_peak_rss_bytes=6209536 after_rss_backend=darwin_getrusage after_current_rss_supported=true after_current_rss_bytes=84901888 after_peak_rss_supported=true after_peak_rss_bytes=84901888 peak_growth_supported=true peak_growth_bytes=78692352 route=legacy_result promotion=false\n"
SELF_TELEMETRY = b"GNFS_SIQS_SHADOW_PROOF_OBSERVE_V1 schema_version=1 mode=observe route=legacy_continue rss_scope=self_lifetime proof_attempted=true terminal=factor_found stage=factor_extraction fallback=none factor_found=true observe_wall_ns=345026625 raw_relations=8457 raw_payload_supported=true raw_payload_bytes=14151664 factor_base_columns=1601 large_prime_bound=3477480 raw_relation_cap=32768 raw_payload_cap_bytes=67108864 graph_edge_cap=16384 graph_cycle_cap=4096 graph_incidence_cap=262144 row_candidate_cap=4096 pretrim_row_cap=4096 minimum_row_excess=1 trim_excess_rows=100 assembly_workers=1 matrix_max_dependencies=64 matrix_workers=1 matrix_parallel_column_threshold=20000 matrix_dense_bytes_cap=268435456 matrix_dense_variable_cap=100000 adapter_input_relations=8457 adapter_full_relations=1243 adapter_accepted_one_lp=7214 adapter_accepted_two_lp=0 adapter_rejected_relations=0 adapter_malformed_source_shape=0 adapter_unsupported_encoding=0 adapter_invalid_one_large_prime=0 adapter_invalid_two_large_prime_split=0 adapter_exact_duplicate=0 graph_evidence_supported=true graph_vertices=6641 graph_edges=7214 graph_components=1 graph_cycles=574 graph_cycle_incidences=1148 graph_max_cycle_length=2 row_candidate_upper=1817 assembly_evidence_supported=true assembly_pretrim_rows=1816 assembly_selected_rows=1701 assembly_selected_full_rows=1242 assembly_selected_cycle_rows=459 assembly_trimmed_rows=115 assembly_fingerprint_supported=true assembly_source_fingerprint_low=3951595500518709611 assembly_source_fingerprint_high=3592456487407473912 assembly_pretrim_fingerprint_low=6526707829188145409 assembly_pretrim_fingerprint_high=4365835778597310596 assembly_selected_fingerprint_low=2564175583998209799 assembly_selected_fingerprint_high=330467350989717490 projected_dense_bytes_supported=true projected_dense_bytes=345816 matrix_evidence_supported=true matrix_rows=1701 matrix_columns=1601 minimum_nullity=100 dependencies_returned=64 dependencies_examined=1 dependencies_verified=1 no_factor_count=0 factor_found_count=1 dependency_cap_reached=true dependency_fingerprint_supported=true dependency_fingerprint_low=7822033740691768892 dependency_fingerprint_high=279485519398704092 first_failed_dependency_supported=false first_failed_dependency=0 winning_dependency_supported=true winning_dependency=0 winning_dependency_size_supported=true winning_dependency_size=671 before_rss_backend=darwin_getrusage before_current_rss_supported=true before_current_rss_bytes=26542080 before_peak_rss_supported=true before_peak_rss_bytes=26722304 after_rss_backend=darwin_getrusage after_current_rss_supported=true after_current_rss_bytes=82575360 after_peak_rss_supported=true after_peak_rss_bytes=82575360 peak_growth_supported=true peak_growth_bytes=55853056 promotion=false\n"


def self_check():
    with tempfile.TemporaryDirectory(prefix="gnfs_shadow_observe_validator_") as directory:
        stdout_path = Path(directory) / "stdout"
        stderr_path = Path(directory) / "stderr"

        def validate_bytes(stdout_data, stderr_data, mode="observe", sample=1):
            stdout_path.write_bytes(stdout_data)
            stderr_path.write_bytes(stderr_data)
            validate_paths(stdout_path, stderr_path, mode, sample)

        validate_bytes(SELF_PROBE, SELF_TELEMETRY)
        off_probe = SELF_PROBE.replace(b" mode=observe env_value=observe sample_ordinal=1 ",
                                       b" mode=off env_value=0 sample_ordinal=0 ")
        validate_bytes(off_probe, b"", "off", 0)

        invalid_cases = [
            (SELF_PROBE.replace(b" status=valid", b"", 1), SELF_TELEMETRY),
            (SELF_PROBE.replace(b" status=valid", b" status=valid status=valid", 1), SELF_TELEMETRY),
            (SELF_PROBE.replace(b" status=valid", b" unknown=1 status=valid", 1), SELF_TELEMETRY),
            (SELF_PROBE.replace(b" schema_version=1 status=valid",
                                b" status=valid schema_version=1", 1), SELF_TELEMETRY),
            (SELF_PROBE.replace(b" max_seconds=30", b" max_seconds=31", 1), SELF_TELEMETRY),
            (SELF_PROBE, SELF_TELEMETRY.replace(b" assembly_workers=1", b" assembly_workers=2", 1)),
            (SELF_PROBE, SELF_TELEMETRY.replace(b" matrix_rows=1701", b" matrix_rows=1702", 1)),
            (SELF_PROBE, SELF_TELEMETRY.replace(b" peak_growth_bytes=55853056",
                                                b" peak_growth_bytes=55853057", 1)),
            (SELF_PROBE, SELF_TELEMETRY.replace(b" adapter_input_relations=8457",
                                                b" adapter_input_relations=8458", 1)),
            (SELF_PROBE[:-1], SELF_TELEMETRY),
            (SELF_PROBE.replace(b"\n", b"\r\n"), SELF_TELEMETRY),
            (SELF_PROBE.replace(b" status", b"\x00 status", 1), SELF_TELEMETRY),
            (SELF_PROBE.replace(b" status", b"\xc3\xa9 status", 1), SELF_TELEMETRY),
            (SELF_PROBE + b"\n", SELF_TELEMETRY),
        ]
        for stdout_data, stderr_data in invalid_cases:
            try:
                validate_bytes(stdout_data, stderr_data)
            except ProtocolError:
                continue
            raise ProtocolError("validator self-check accepted an invalid transcript")
        try:
            validate_bytes(off_probe, SELF_TELEMETRY, "off", 0)
        except ProtocolError:
            pass
        else:
            raise ProtocolError("validator self-check accepted off-mode stderr")


try:
    if len(sys.argv) == 2 and sys.argv[1] == "--self-check":
        self_check()
    else:
        require(len(sys.argv) == 5,
                "validator requires stdout path, stderr path, mode, and sample ordinal")
        require(re.fullmatch(r"0|[1-3]", sys.argv[4]) is not None,
                "sample ordinal argument must be canonical 0..3")
        validate_paths(sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]))
except (OSError, ProtocolError) as error:
    print(f"SIQS shadow observe protocol validation failed: {error}", file=sys.stderr)
    sys.exit(1)
PY
}

siqs_shadow_observe_validator_self_check() {
    siqs_shadow_observe_validate_protocol --self-check
}

SIQS_SHADOW_OBSERVE_PROBE_RECORD=""
SIQS_SHADOW_OBSERVE_TELEMETRY_RECORD=""
DUAL_STREAM_TIMED_OUT=0

# Fresh-process evidence needs stdout and stderr as independent byte streams.
# This timeout wrapper never combines them and never stores them in shell
# variables before the protocol validator has inspected the raw bytes.
run_dual_stream_with_timeout() {
    local stdout_file="$1"
    local stderr_file="$2"
    local timeout_seconds="$3"
    shift 3
    local cmd=("$@")

    DUAL_STREAM_TIMED_OUT=0
    "${cmd[@]}" >"$stdout_file" 2>"$stderr_file" &
    local pid=$!
    local elapsed_tenths=0
    local timeout_tenths=$((timeout_seconds * 10))
    local next_heartbeat_tenths=100

    while kill -0 "$pid" 2>/dev/null; do
        if (( elapsed_tenths >= timeout_tenths )); then
            DUAL_STREAM_TIMED_OUT=1
            kill "$pid" 2>/dev/null || true
            sleep 0.2
            kill -9 "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            return 124
        fi
        if (( elapsed_tenths < 20 )); then
            sleep 0.1
            (( elapsed_tenths += 1 ))
        else
            sleep 1
            (( elapsed_tenths += 10 ))
        fi
        if (( elapsed_tenths >= next_heartbeat_tenths && !QUIET )); then
            printf "${DIM}[%ds]${RESET}" "$((elapsed_tenths / 10))" >&2
            (( next_heartbeat_tenths += 100 ))
        fi
    done

    local exit_code=0
    wait "$pid" 2>/dev/null || exit_code=$?
    return "$exit_code"
}

show_siqs_shadow_observe_stream_bytes() {
    local stdout_file="$1"
    local stderr_file="$2"
    local stdout_bytes stderr_bytes
    stdout_bytes=$(wc -c <"$stdout_file" | tr -d '[:space:]')
    stderr_bytes=$(wc -c <"$stderr_file" | tr -d '[:space:]')
    log_info "原始通道字节数: stdout=${stdout_bytes}, stderr=${stderr_bytes}"
    if (( stdout_bytes > 0 )); then
        printf '%s\n' "stdout 前 96 bytes (hex):"
        od -An -v -t x1 -N 96 "$stdout_file"
    fi
    if (( stderr_bytes > 0 )); then
        printf '%s\n' "stderr 前 96 bytes (hex):"
        od -An -v -t x1 -N 96 "$stderr_file"
    fi
}

# Run exactly one production factor process. A successful off run is 1 stdout
# record plus byte-empty stderr; observe is 1 stdout probe plus exactly 1
# existing telemetry record on stderr.
run_siqs_shadow_observe_process() {
    local mode="$1"
    local sample_ordinal="$2"
    local timeout_seconds="$3"
    local binary="${BUILD_DIR}/test_siqs_shadow_proof_observe_probe"
    local stdout_file stderr_file
    stdout_file=$(mktemp "${TMPDIR:-/tmp}/gnfs_siqs_shadow_observe_stdout.XXXXXX") || return 1
    stderr_file=$(mktemp "${TMPDIR:-/tmp}/gnfs_siqs_shadow_observe_stderr.XXXXXX") || {
        rm -f "$stdout_file"
        return 1
    }

    SIQS_SHADOW_OBSERVE_PROBE_RECORD=""
    SIQS_SHADOW_OBSERVE_TELEMETRY_RECORD=""
    local start_ms end_ms elapsed exit_code=0
    start_ms=$(timer_start_ms)
    run_dual_stream_with_timeout "$stdout_file" "$stderr_file" "$timeout_seconds" \
        "$binary" --mode "$mode" --sample-ordinal "$sample_ordinal" || exit_code=$?
    end_ms=$(timer_start_ms)
    elapsed=$((end_ms - start_ms))
    TOTAL_TIME_MS=$((TOTAL_TIME_MS + elapsed))
    (( TOTAL_TESTS += 1 ))

    if (( exit_code == 124 )); then
        log_fail "SIQS shadow observe mode=${mode} sample=${sample_ordinal} TIMEOUT after ${timeout_seconds}s"
        (( FAILED_TESTS += 1 ))
        rm -f "$stdout_file" "$stderr_file"
        return 1
    fi
    if (( exit_code != 0 )); then
        log_fail "SIQS shadow observe mode=${mode} sample=${sample_ordinal} 退出码 ${exit_code}"
        show_siqs_shadow_observe_stream_bytes "$stdout_file" "$stderr_file"
        (( FAILED_TESTS += 1 ))
        rm -f "$stdout_file" "$stderr_file"
        return 1
    fi
    if ! siqs_shadow_observe_validate_protocol \
        "$stdout_file" "$stderr_file" "$mode" "$sample_ordinal"; then
        log_fail "SIQS shadow observe 双流不满足冻结协议"
        show_siqs_shadow_observe_stream_bytes "$stdout_file" "$stderr_file"
        (( FAILED_TESTS += 1 ))
        rm -f "$stdout_file" "$stderr_file"
        return 1
    fi

    SIQS_SHADOW_OBSERVE_PROBE_RECORD=$(LC_ALL=C sed -n '1p' "$stdout_file")
    if [[ "$mode" == "observe" ]]; then
        SIQS_SHADOW_OBSERVE_TELEMETRY_RECORD=$(LC_ALL=C sed -n '1p' "$stderr_file")
    fi
    rm -f "$stdout_file" "$stderr_file"

    (( PASSED_TESTS += 1 ))
    REPORT_ENTRIES+=("{\"name\":\"siqs_shadow_observe_${mode}_sample_${sample_ordinal}\",\"status\":\"pass\",\"elapsed_ms\":${elapsed}}")
    return 0
}

uint_values_min() {
    local minimum="$1"
    shift
    local value
    for value in "$@"; do
        (( value < minimum )) && minimum="$value"
    done
    printf '%s\n' "$minimum"
}

uint_values_max() {
    local maximum="$1"
    shift
    local value
    for value in "$@"; do
        (( value > maximum )) && maximum="$value"
    done
    printf '%s\n' "$maximum"
}

# The live-sieve probe has one stdout record and a separate diagnostic stderr
# stream. These globals carry the validated record and its normalized identity
# between fresh-process runs without writing a comparison artifact.
SIQS_LIVE_CAPTURE_RECORD=""
SIQS_LIVE_CAPTURE_IDENTITY=""
SIQS_LIVE_CAPTURE_STDERR=""

siqs_live_probe_timeout() {
    case "$1" in
        50) echo 900 ;;
        70) echo 1800 ;;
        90) echo 3600 ;;
        *) return 1 ;;
    esac
}

# Validate the narrow key=value line protocol. Worker execution fields are
# checked against the requested process and then omitted from the deterministic
# corpus identity. Wall time and peak RSS are informational by contract.
validate_siqs_live_capture_record() {
    local line="$1"
    local expected_band="$2"
    local expected_workers="$3"
    local -a tokens
    tokens=(${=line})

    SIQS_LIVE_CAPTURE_RECORD=""
    SIQS_LIVE_CAPTURE_IDENTITY=""
    if (( ${#tokens[@]} < 8 )) ||
       [[ "${tokens[1]:-}" != "GNFS_SIQS_LIVE_CAPTURE_V1" ]]; then
        return 1
    fi

    local token key value
    typeset -A fields
    for token in "${tokens[@]:1}"; do
        key="${token%%=*}"
        value="${token#*=}"
        if [[ "$key" == "$token" || -z "$value" ||
              ! "$key" =~ '^[A-Za-z][A-Za-z0-9_]*$' ||
              -n "${fields[$key]+present}" ]]; then
            return 1
        fi
        fields[$key]="$value"
    done

    # Freeze the complete V1 evidence schema. Duplicate keys were rejected
    # above; exact cardinality plus membership rejects both omissions and
    # forward-added/unknown fields until this parser is deliberately revised.
    local required
    local -a schema_fields=(
        schema_version status build_type ndebug scope band digits n p q seed a_planner
        multiplier sieved_n sieved_bits param_fb_size factor_base_columns
        factor_base_last_prime param_sieve_half param_lp_multiplier param_a_factors
        param_sieve_error param_small_prime_cutoff large_prime_bound two_large_prime_bound
        threshold available_b_slots fixture_b_slots polynomial_a
        polynomial_family_digest_low polynomial_family_digest_high
        plan_digest_low plan_digest_high relation_limit_per_slot payload_limit_bytes_per_slot
        planned_slots completed_slots workers resolved_workers peak_workers schedule logical_merge
        worker_independence_premises capture_threshold_candidates
        capture_unrepresentable_residuals capture_rejected_residuals capture_observed_full
        capture_observed_one_lp capture_observed_two_lp capture_relations capture_payload_bytes
        capture_stop_none capture_stop_invalid_limits capture_stop_invalid_relation_kind
        capture_stop_invalid_state capture_stop_relation_limit capture_stop_payload_limit
        capture_stop_size_overflow raw_full raw_one_lp raw_two_lp_candidates adapter_input
        adapter_full adapter_accepted_one_lp adapter_accepted_two_lp adapter_rejected
        adapter_malformed_source_shape adapter_unsupported_encoding
        adapter_invalid_one_large_prime adapter_invalid_two_large_prime_split
        adapter_exact_duplicate graph_vertices graph_edges graph_components graph_cycles
        graph_cycle_rank_identity assembly_status assembly_input_relations assembly_encoded_full
        assembly_valid_full assembly_rejected_full assembly_full_sources
        assembly_duplicate_full_sources assembly_adapter_input assembly_adapter_full
        assembly_adapter_accepted_one_lp assembly_adapter_accepted_two_lp
        assembly_adapter_rejected assembly_adapter_malformed_source_shape
        assembly_adapter_unsupported_encoding assembly_adapter_invalid_one_large_prime
        assembly_adapter_invalid_two_large_prime_split assembly_adapter_exact_duplicate
        assembly_partial_sources assembly_graph_edges assembly_graph_cycles
        assembly_valid_cycle_rows assembly_rejected_cycle_rows assembly_rows_before_dedup
        assembly_arithmetic_duplicates_removed assembly_pretrim_rows assembly_selected_rows
        assembly_selected_full_rows assembly_selected_cycle_rows assembly_trimmed_rows
        source_fingerprint_low source_fingerprint_high pretrim_fingerprint_low
        pretrim_fingerprint_high selected_fingerprint_low selected_fingerprint_high
        logical_raw_digest_low logical_raw_digest_high canonical_raw_digest_low
        canonical_raw_digest_high slot_state_digest_low slot_state_digest_high matrix_rows
        matrix_columns matrix_projected_dense_bytes matrix_default_max_dense_bytes
        matrix_default_max_variables matrix_status_scope matrix_admission_status
        solver_attempted rss_scope rss_backend peak_rss_bytes wall_ns
    )
    (( ${#fields} == ${#schema_fields} )) || return 1
    for required in "${schema_fields[@]}"; do
        [[ -n "${fields[$required]+present}" ]] || return 1
    done

    if [[ "${fields[schema_version]:-}" != "1" ||
          "${fields[status]:-}" != "valid" ||
          "${fields[build_type]:-}" != "Release" ||
          "${fields[ndebug]:-}" != "true" ||
          "${fields[scope]:-}" != "fixed_one_a_family_prefix" ||
          "${fields[a_planner]:-}" != "stable_mpz_root_mt19937_fisher_yates_v1" ||
          "${fields[band]:-}" != "$expected_band" ||
          "${fields[digits]:-}" != "$expected_band" ||
          "${fields[workers]:-}" != "$expected_workers" ||
          "${fields[schedule]:-}" != "static_contiguous" ||
          "${fields[logical_merge]:-}" != "slot_order" ||
          "${fields[worker_independence_premises]:-}" != "pass" ||
          "${fields[graph_cycle_rank_identity]:-}" != "pass" ||
          "${fields[assembly_status]:-}" != "valid" ||
          "${fields[matrix_status_scope]:-}" != "projected_not_run" ||
          "${fields[solver_attempted]:-}" != "false" ]]; then
        return 1
    fi
    case "${fields[matrix_admission_status]}" in
        valid|size_overflow|resource_limit|unsupported_backend) ;;
        *) return 1 ;;
    esac
    if [[ ! "${fields[resolved_workers]:-}" =~ '^[1-9][0-9]*$' ||
          ! "${fields[peak_workers]:-}" =~ '^[1-9][0-9]*$' ]] ||
       (( fields[resolved_workers] != expected_workers ||
          fields[peak_workers] != expected_workers )); then
        return 1
    fi
    if [[ ! "${fields[wall_ns]:-}" =~ '^[1-9][0-9]*$' ||
          ! "${fields[peak_rss_bytes]:-}" =~ '^(na|[1-9][0-9]*)$' ]]; then
        return 1
    fi
    local numeric_field
    local -a required_uint_fields=(
        plan_digest_low plan_digest_high planned_slots completed_slots
        slot_state_digest_low slot_state_digest_high
        capture_relations adapter_input adapter_rejected
        adapter_malformed_source_shape adapter_unsupported_encoding
        adapter_invalid_one_large_prime adapter_invalid_two_large_prime_split
        adapter_exact_duplicate
        source_fingerprint_low source_fingerprint_high
        pretrim_fingerprint_low pretrim_fingerprint_high
        selected_fingerprint_low selected_fingerprint_high
        logical_raw_digest_low logical_raw_digest_high
        canonical_raw_digest_low canonical_raw_digest_high
        matrix_rows matrix_columns
    )
    for numeric_field in "${required_uint_fields[@]}"; do
        [[ "${fields[$numeric_field]}" =~ '^[0-9]+$' ]] || return 1
    done
    [[ "${fields[planned_slots]}" == "${fields[completed_slots]}" ]] || return 1

    local normalized=""
    for key in ${(ok)fields}; do
        case "$key" in
            workers|resolved_workers|peak_workers|wall_ns|peak_rss_bytes) continue ;;
        esac
        normalized+="${key}=${fields[$key]}"$'\n'
    done
    [[ -n "$normalized" ]] || return 1

    SIQS_LIVE_CAPTURE_RECORD="$line"
    SIQS_LIVE_CAPTURE_IDENTITY="$normalized"
}

# Run exactly one fresh probe process. stdout must contain exactly one non-empty
# protocol line. stderr is retained only in memory for a concise failure report.
run_siqs_live_probe_process() {
    local band="$1"
    local workers="$2"
    local timeout_seconds="$3"
    local binary="${BUILD_DIR}/test_siqs_live_sieve_probe"
    local stderr_file
    stderr_file=$(mktemp "${TMPDIR:-/tmp}/gnfs_siqs_live_probe_stderr.XXXXXX")

    local start_ms exit_code=0
    start_ms=$(timer_start_ms)
    run_with_timeout "$timeout_seconds" /bin/sh -c \
        'stderr_path=$1; shift; exec "$@" 2>"$stderr_path"' \
        sh "$stderr_file" "$binary" --band "$band" --workers "$workers" ||
        exit_code=$?
    local end_ms
    end_ms=$(timer_start_ms)
    local elapsed=$((end_ms - start_ms))
    TOTAL_TIME_MS=$((TOTAL_TIME_MS + elapsed))
    (( TOTAL_TESTS += 1 ))

    local stdout="$RUN_OUTPUT"
    SIQS_LIVE_CAPTURE_STDERR=$(<"$stderr_file")
    rm -f "$stderr_file"

    if (( exit_code == 124 )); then
        log_fail "SIQS live-sieve band=${band} workers=${workers} TIMEOUT after ${timeout_seconds}s"
        (( FAILED_TESTS += 1 ))
        return 1
    fi
    if (( exit_code != 0 )); then
        log_fail "SIQS live-sieve band=${band} workers=${workers} 退出码 ${exit_code}"
        [[ -n "$SIQS_LIVE_CAPTURE_STDERR" ]] &&
            printf '%s\n' "$SIQS_LIVE_CAPTURE_STDERR" | tail -10
        (( FAILED_TESTS += 1 ))
        return 1
    fi
    if [[ -n "$SIQS_LIVE_CAPTURE_STDERR" ]]; then
        log_fail "SIQS live-sieve 成功进程不得写入 stderr"
        printf '%s\n' "$SIQS_LIVE_CAPTURE_STDERR" | tail -10
        (( FAILED_TESTS += 1 ))
        return 1
    fi
    if [[ -z "$stdout" || "$stdout" == *$'\n'* ]] ||
       ! validate_siqs_live_capture_record "$stdout" "$band" "$workers"; then
        log_fail "SIQS live-sieve stdout 必须是唯一且合法的 GNFS_SIQS_LIVE_CAPTURE_V1 记录"
        (( FAILED_TESTS += 1 ))
        return 1
    fi

    (( PASSED_TESTS += 1 ))
    REPORT_ENTRIES+=("{\"name\":\"test_siqs_live_sieve_probe_${band}_w${workers}\",\"status\":\"pass\",\"elapsed_ms\":${elapsed}}")
    return 0
}

# The fixed 50-digit multi-A profile emits one CONFIG, four PREFIX, and one
# SUMMARY record. V2 is a separate closed schema; it does not loosen the
# single-record 129-key live-capture V1 contract above.
SIQS_CYCLE_PROFILE_OUTPUT=""
SIQS_CYCLE_PROFILE_IDENTITY=""
SIQS_CYCLE_PROFILE_LINE_IDENTITY=""
SIQS_CYCLE_PROFILE_STDERR=""

validate_siqs_cycle_profile_line() {
    local line="$1"
    local kind="$2"
    local expected_workers="$3"
    local expected_a="${4:-0}"
    local expected_prefix=""
    local -a schema_fields numeric_fields

    case "$kind" in
        config)
            expected_prefix="GNFS_SIQS_MULTI_A_CYCLE_CONFIG_V2"
            schema_fields=(
                schema_version status profile_id build_type ndebug band digits n p q seed
                a_planner max_a unique_a planner_attempts planner_duplicate_draws
                accepted_duplicate_a max_planner_attempts b_per_a available_b_per_a
                complete_b_family capture_stages prefix_a_counts logical_id_schema
                relation_provenance_schema accepted_provenance_policy multiplier sieved_n
                sieved_bits param_fb_size factor_base_columns factor_base_last_prime
                param_sieve_half param_lp_multiplier param_a_factors param_sieve_error
                param_small_prime_cutoff large_prime_bound two_large_prime_bound threshold
                relation_limit_per_slot payload_limit_bytes_per_slot theoretical_relation_cap
                theoretical_payload_cap_bytes shadow_trim_excess first_a last_a
                plan_1_digest_low plan_1_digest_high plan_4_digest_low plan_4_digest_high
                plan_16_digest_low plan_16_digest_high plan_64_digest_low plan_64_digest_high
                promotion solver_attempted
            )
            numeric_fields=(
                n p q seed max_a unique_a planner_attempts planner_duplicate_draws
                accepted_duplicate_a max_planner_attempts b_per_a available_b_per_a multiplier
                sieved_n sieved_bits param_fb_size factor_base_columns factor_base_last_prime
                param_sieve_half param_lp_multiplier param_a_factors param_sieve_error
                param_small_prime_cutoff large_prime_bound two_large_prime_bound threshold
                relation_limit_per_slot payload_limit_bytes_per_slot theoretical_relation_cap
                theoretical_payload_cap_bytes shadow_trim_excess first_a last_a
                plan_1_digest_low plan_1_digest_high plan_4_digest_low plan_4_digest_high
                plan_16_digest_low plan_16_digest_high plan_64_digest_low plan_64_digest_high
            )
            ;;
        prefix)
            expected_prefix="GNFS_SIQS_MULTI_A_CYCLE_PREFIX_V2"
            schema_fields=(
                schema_version status profile_id prefix_a stage_a_begin stage_a_end b_per_a
                planned_slots completed_slots stage_slots plan_digest_low plan_digest_high
                slot_state_digest_low slot_state_digest_high logical_raw_digest_low
                logical_raw_digest_high canonical_raw_digest_low canonical_raw_digest_high
                capture_threshold_candidates capture_unrepresentable_residuals
                capture_rejected_residuals capture_observed_full capture_observed_one_lp
                capture_observed_two_lp capture_relations capture_payload_bytes
                capture_observed_not_captured capture_stop_none capture_stop_relation_limit
                capture_stop_payload_limit capacity_truncated_slots capacity_truncated
                stage_captured_relations stage_capacity_truncated_slots raw_full raw_one_lp
                raw_two_lp_candidates adapter_input adapter_full adapter_accepted_one_lp
                adapter_accepted_two_lp adapter_rejected adapter_malformed_source_shape
                adapter_unsupported_encoding adapter_invalid_one_large_prime
                adapter_invalid_two_large_prime_split adapter_exact_duplicate graph_vertices
                graph_edges graph_components graph_cycles stage_graph_cycles
                graph_cycle_density_ppm graph_cycle_rank_identity cycles_with_accepted_2lp
                cycles_without_accepted_2lp cycles_spanning_multiple_a max_cycle_a_coverage
                cycle_source_a_count two_lp_edge_source_a_count two_lp_bearing_cycle_a_count
                accepted_two_lp_source_a_count cycle_provenance_digest_low
                cycle_provenance_digest_high assembly_status assembly_valid_full
                assembly_full_sources assembly_duplicate_full_sources assembly_partial_sources
                assembly_graph_cycles assembly_valid_cycle_rows assembly_rejected_cycle_rows
                assembly_rows_before_dedup assembly_arithmetic_duplicates_removed
                assembly_pretrim_rows assembly_selected_rows assembly_selected_full_rows
                assembly_selected_cycle_rows assembly_trimmed_rows source_fingerprint_low
                source_fingerprint_high pretrim_fingerprint_low pretrim_fingerprint_high
                selected_fingerprint_low selected_fingerprint_high promotion solver_attempted
            )
            numeric_fields=(
                prefix_a stage_a_begin stage_a_end b_per_a planned_slots completed_slots
                stage_slots plan_digest_low plan_digest_high slot_state_digest_low
                slot_state_digest_high logical_raw_digest_low logical_raw_digest_high
                canonical_raw_digest_low canonical_raw_digest_high capture_threshold_candidates
                capture_unrepresentable_residuals capture_rejected_residuals
                capture_observed_full capture_observed_one_lp capture_observed_two_lp
                capture_relations capture_payload_bytes capture_observed_not_captured
                capture_stop_none capture_stop_relation_limit capture_stop_payload_limit
                capacity_truncated_slots stage_captured_relations
                stage_capacity_truncated_slots raw_full raw_one_lp raw_two_lp_candidates
                adapter_input adapter_full adapter_accepted_one_lp adapter_accepted_two_lp
                adapter_rejected adapter_malformed_source_shape adapter_unsupported_encoding
                adapter_invalid_one_large_prime adapter_invalid_two_large_prime_split
                adapter_exact_duplicate graph_vertices graph_edges graph_components graph_cycles
                stage_graph_cycles graph_cycle_density_ppm cycles_with_accepted_2lp
                cycles_without_accepted_2lp cycles_spanning_multiple_a max_cycle_a_coverage
                cycle_source_a_count two_lp_edge_source_a_count two_lp_bearing_cycle_a_count
                accepted_two_lp_source_a_count cycle_provenance_digest_low
                cycle_provenance_digest_high assembly_valid_full assembly_full_sources
                assembly_duplicate_full_sources assembly_partial_sources assembly_graph_cycles
                assembly_valid_cycle_rows assembly_rejected_cycle_rows
                assembly_rows_before_dedup assembly_arithmetic_duplicates_removed
                assembly_pretrim_rows assembly_selected_rows assembly_selected_full_rows
                assembly_selected_cycle_rows assembly_trimmed_rows source_fingerprint_low
                source_fingerprint_high pretrim_fingerprint_low pretrim_fingerprint_high
                selected_fingerprint_low selected_fingerprint_high
            )
            ;;
        summary)
            expected_prefix="GNFS_SIQS_MULTI_A_CYCLE_SUMMARY_V2"
            schema_fields=(
                schema_version status profile_id stdout_records config_records prefix_records
                summary_records max_a b_per_a planned_slots completed_slots workers
                resolved_workers peak_workers stages_with_full_peak schedule logical_merge
                worker_independence_premises theoretical_relation_cap
                theoretical_payload_cap_bytes any_capacity_truncation first_cycle_prefix
                first_two_lp_cycle_prefix rss_scope rss_backend plan_current_rss_bytes
                plan_peak_rss_bytes capture_current_rss_bytes capture_peak_rss_bytes
                final_current_rss_bytes final_peak_rss_bytes plan_wall_ns capture_wall_ns
                analysis_wall_ns wall_ns promotion solver_attempted
            )
            numeric_fields=(
                stdout_records config_records prefix_records summary_records max_a b_per_a
                planned_slots completed_slots workers resolved_workers peak_workers
                stages_with_full_peak theoretical_relation_cap theoretical_payload_cap_bytes
                plan_wall_ns capture_wall_ns analysis_wall_ns wall_ns
            )
            ;;
        *) return 1 ;;
    esac

    local -a tokens
    tokens=(${=line})
    [[ "${tokens[1]:-}" == "$expected_prefix" ]] || return 1
    local token key value required numeric_field
    typeset -A fields
    for token in "${tokens[@]:1}"; do
        key="${token%%=*}"
        value="${token#*=}"
        if [[ "$key" == "$token" || -z "$value" ||
              ! "$key" =~ '^[A-Za-z][A-Za-z0-9_]*$' ||
              -n "${fields[$key]+present}" ]]; then
            return 1
        fi
        fields[$key]="$value"
    done
    (( ${#fields} == ${#schema_fields} )) || return 1
    for required in "${schema_fields[@]}"; do
        [[ -n "${fields[$required]+present}" ]] || return 1
    done
    for numeric_field in "${numeric_fields[@]}"; do
        [[ "${fields[$numeric_field]}" =~ '^(0|[1-9][0-9]*)$' ]] || return 1
    done

    if [[ "${fields[schema_version]}" != "2" ||
          "${fields[status]}" != "valid" ||
          "${fields[profile_id]}" != "siqs50_multi_a_64x32_v2" ||
          "${fields[promotion]}" != "false" ||
          "${fields[solver_attempted]}" != "false" ]]; then
        return 1
    fi

    case "$kind" in
        config)
            if [[ "${fields[build_type]}" != "Release" || "${fields[ndebug]}" != "true" ||
                  "${fields[band]}" != "50" || "${fields[digits]}" != "50" ||
                  "${fields[a_planner]}" != "stable_mpz_root_mt19937_fisher_yates_unique_v2" ||
                  "${fields[max_a]}" != "64" || "${fields[unique_a]}" != "64" ||
                  "${fields[accepted_duplicate_a]}" != "0" ||
                  "${fields[max_planner_attempts]}" != "4096" ||
                  "${fields[b_per_a]}" != "32" || "${fields[available_b_per_a]}" != "32" ||
                  "${fields[complete_b_family]}" != "true" ||
                  "${fields[capture_stages]}" != "0-1,1-4,4-16,16-64" ||
                  "${fields[prefix_a_counts]}" != "1,4,16,64" ||
                  "${fields[logical_id_schema]}" != "a_ordinal_gray_ordinal" ||
                  "${fields[relation_provenance_schema]}" != "a_ordinal_gray_ordinal_relation_ordinal" ||
                  "${fields[accepted_provenance_policy]}" != "canonical_min_logical_id" ||
                  "${fields[param_fb_size]}" != "1600" ||
                  "${fields[factor_base_columns]}" != "1601" ||
                  "${fields[param_sieve_half]}" != "65536" ||
                  "${fields[param_lp_multiplier]}" != "120" ||
                  "${fields[param_a_factors]}" != "6" ||
                  "${fields[param_sieve_error]}" != "12" ||
                  "${fields[param_small_prime_cutoff]}" != "25" ||
                  "${fields[relation_limit_per_slot]}" != "32" ||
                  "${fields[payload_limit_bytes_per_slot]}" != "65536" ||
                  "${fields[theoretical_relation_cap]}" != "65536" ||
                  "${fields[theoretical_payload_cap_bytes]}" != "134217728" ]]; then
                return 1
            fi
            if (( fields[planner_attempts] != 64 + fields[planner_duplicate_draws] )); then
                return 1
            fi
            local digest_a
            for digest_a in 1 4 16 64; do
                [[ "${fields[plan_${digest_a}_digest_low]}:${fields[plan_${digest_a}_digest_high]}" != "0:0" ]] || return 1
            done
            ;;
        prefix)
            local stage_begin stage_end expected_slots expected_stage_slots
            case "$expected_a" in
                1) stage_begin=0; stage_end=1 ;;
                4) stage_begin=1; stage_end=4 ;;
                16) stage_begin=4; stage_end=16 ;;
                64) stage_begin=16; stage_end=64 ;;
                *) return 1 ;;
            esac
            expected_slots=$(( expected_a * 32 ))
            expected_stage_slots=$(( (stage_end - stage_begin) * 32 ))
            if [[ "${fields[prefix_a]}" != "$expected_a" ||
                  "${fields[stage_a_begin]}" != "$stage_begin" ||
                  "${fields[stage_a_end]}" != "$stage_end" ||
                  "${fields[b_per_a]}" != "32" ||
                  "${fields[planned_slots]}" != "$expected_slots" ||
                  "${fields[completed_slots]}" != "$expected_slots" ||
                  "${fields[stage_slots]}" != "$expected_stage_slots" ||
                  "${fields[graph_cycle_rank_identity]}" != "pass" ||
                  "${fields[assembly_status]}" != "valid" ]]; then
                return 1
            fi
            if (( fields[graph_edges] + fields[graph_components] < fields[graph_vertices] ||
                  fields[graph_cycles] != fields[graph_edges] - fields[graph_vertices] + fields[graph_components] )); then
                return 1
            fi
            local expected_density=0
            (( fields[graph_edges] != 0 )) &&
                expected_density=$(( fields[graph_cycles] * 1000000 / fields[graph_edges] ))
            (( fields[graph_cycle_density_ppm] == expected_density )) || return 1
            (( fields[capture_stop_none] + fields[capture_stop_relation_limit] +
               fields[capture_stop_payload_limit] == fields[completed_slots] )) || return 1
            (( fields[capture_threshold_candidates] ==
               fields[capture_unrepresentable_residuals] +
               fields[capture_rejected_residuals] + fields[capture_observed_full] +
               fields[capture_observed_one_lp] + fields[capture_observed_two_lp] )) || return 1
            (( fields[capture_observed_full] + fields[capture_observed_one_lp] +
               fields[capture_observed_two_lp] == fields[capture_relations] +
               fields[capture_observed_not_captured] )) || return 1
            (( fields[capacity_truncated_slots] == fields[capture_stop_relation_limit] +
               fields[capture_stop_payload_limit] )) || return 1
            if (( fields[capacity_truncated_slots] == 0 )); then
                [[ "${fields[capacity_truncated]}" == "false" ]] || return 1
            else
                [[ "${fields[capacity_truncated]}" == "true" ]] || return 1
            fi
            (( fields[raw_full] + fields[raw_one_lp] + fields[raw_two_lp_candidates] ==
               fields[capture_relations] && fields[adapter_input] == fields[capture_relations] )) || return 1
            (( fields[adapter_full] + fields[adapter_accepted_one_lp] +
               fields[adapter_accepted_two_lp] + fields[adapter_rejected] ==
               fields[adapter_input] )) || return 1
            (( fields[adapter_full] == fields[raw_full] )) || return 1
            (( fields[adapter_malformed_source_shape] + fields[adapter_unsupported_encoding] +
               fields[adapter_invalid_one_large_prime] +
               fields[adapter_invalid_two_large_prime_split] +
               fields[adapter_exact_duplicate] == fields[adapter_rejected] )) || return 1
            (( fields[cycles_with_accepted_2lp] + fields[cycles_without_accepted_2lp] ==
               fields[graph_cycles] && fields[assembly_graph_cycles] == fields[graph_cycles] )) || return 1
            (( fields[assembly_partial_sources] == fields[adapter_accepted_one_lp] +
               fields[adapter_accepted_two_lp] &&
               fields[assembly_partial_sources] == fields[graph_edges] )) || return 1
            (( fields[assembly_valid_full] == fields[assembly_full_sources] +
               fields[assembly_duplicate_full_sources] )) || return 1
            (( fields[assembly_valid_cycle_rows] + fields[assembly_rejected_cycle_rows] ==
               fields[graph_cycles] )) || return 1
            (( fields[assembly_rows_before_dedup] == fields[assembly_full_sources] +
               fields[assembly_valid_cycle_rows] &&
               fields[assembly_rows_before_dedup] == fields[assembly_pretrim_rows] +
               fields[assembly_arithmetic_duplicates_removed] )) || return 1
            (( fields[assembly_pretrim_rows] == fields[assembly_selected_rows] +
               fields[assembly_trimmed_rows] )) || return 1
            (( fields[assembly_selected_rows] == fields[assembly_selected_full_rows] +
               fields[assembly_selected_cycle_rows] )) || return 1
            [[ "${fields[plan_digest_low]}:${fields[plan_digest_high]}" != "0:0" ]] || return 1
            ;;
        summary)
            if [[ "${fields[stdout_records]}" != "6" ||
                  "${fields[config_records]}" != "1" ||
                  "${fields[prefix_records]}" != "4" ||
                  "${fields[summary_records]}" != "1" ||
                  "${fields[max_a]}" != "64" || "${fields[b_per_a]}" != "32" ||
                  "${fields[planned_slots]}" != "2048" ||
                  "${fields[completed_slots]}" != "2048" ||
                  "${fields[workers]}" != "$expected_workers" ||
                  "${fields[resolved_workers]}" != "$expected_workers" ||
                  "${fields[peak_workers]}" != "$expected_workers" ||
                  "${fields[stages_with_full_peak]}" != "4" ||
                  "${fields[schedule]}" != "staged_static_contiguous_logical_slots" ||
                  "${fields[logical_merge]}" != "lexicographic_a_gray_relation" ||
                  "${fields[worker_independence_premises]}" != "pass" ||
                  "${fields[theoretical_relation_cap]}" != "65536" ||
                  "${fields[theoretical_payload_cap_bytes]}" != "134217728" ||
                  "${fields[rss_scope]}" != "self_lifetime" ]]; then
                return 1
            fi
            case "${fields[any_capacity_truncation]}" in true|false) ;; *) return 1 ;; esac
            case "${fields[first_cycle_prefix]}" in none|1|4|16|64) ;; *) return 1 ;; esac
            case "${fields[first_two_lp_cycle_prefix]}" in none|1|4|16|64) ;; *) return 1 ;; esac
            local resource_field
            for resource_field in plan_current_rss_bytes plan_peak_rss_bytes \
                capture_current_rss_bytes capture_peak_rss_bytes \
                final_current_rss_bytes final_peak_rss_bytes; do
                [[ "${fields[$resource_field]}" =~ '^(na|[1-9][0-9]*)$' ]] || return 1
            done
            (( fields[plan_wall_ns] > 0 && fields[capture_wall_ns] > 0 &&
               fields[analysis_wall_ns] > 0 && fields[wall_ns] > 0 )) || return 1
            ;;
    esac

    local normalized="$expected_prefix"$'\n'
    for key in ${(ok)fields}; do
        case "$key" in
            workers|resolved_workers|peak_workers|plan_current_rss_bytes|plan_peak_rss_bytes|capture_current_rss_bytes|capture_peak_rss_bytes|final_current_rss_bytes|final_peak_rss_bytes|plan_wall_ns|capture_wall_ns|analysis_wall_ns|wall_ns) continue ;;
        esac
        normalized+="${key}=${fields[$key]}"$'\n'
    done
    SIQS_CYCLE_PROFILE_LINE_IDENTITY="$normalized"
}

validate_siqs_cycle_profile_output() {
    local stdout="$1"
    local expected_workers="$2"
    local -a lines expected_prefixes=(1 4 16 64)
    lines=("${(@f)stdout}")
    (( ${#lines[@]} == 6 )) || return 1

    local identity="" index digest_key_low digest_key_high
    validate_siqs_cycle_profile_line "${lines[1]}" config "$expected_workers" || return 1
    identity+="$SIQS_CYCLE_PROFILE_LINE_IDENTITY"
    for (( index = 1; index <= 4; ++index )); do
        validate_siqs_cycle_profile_line "${lines[index + 1]}" prefix "$expected_workers" \
            "${expected_prefixes[index]}" || return 1
        digest_key_low="plan_${expected_prefixes[index]}_digest_low"
        digest_key_high="plan_${expected_prefixes[index]}_digest_high"
        [[ "$(measurement_record_field "${lines[1]}" "$digest_key_low")" ==
           "$(measurement_record_field "${lines[index + 1]}" plan_digest_low)" &&
           "$(measurement_record_field "${lines[1]}" "$digest_key_high")" ==
           "$(measurement_record_field "${lines[index + 1]}" plan_digest_high)" ]] || return 1
        identity+="$SIQS_CYCLE_PROFILE_LINE_IDENTITY"
    done
    validate_siqs_cycle_profile_line "${lines[6]}" summary "$expected_workers" || return 1
    identity+="$SIQS_CYCLE_PROFILE_LINE_IDENTITY"

    local previous_relations=0 previous_truncated=0 previous_cycles=0
    local current stage_value expected_first="none" expected_first_two_lp="none"
    local line two_lp_cycles
    local any_truncated="false"
    for (( index = 1; index <= 4; ++index )); do
        line="${lines[index + 1]}"
        current=$(measurement_record_field "$line" capture_relations) || return 1
        stage_value=$(measurement_record_field "$line" stage_captured_relations) || return 1
        (( current >= previous_relations && stage_value == current - previous_relations )) || return 1
        previous_relations=$current
        current=$(measurement_record_field "$line" capacity_truncated_slots) || return 1
        stage_value=$(measurement_record_field "$line" stage_capacity_truncated_slots) || return 1
        (( current >= previous_truncated && stage_value == current - previous_truncated )) || return 1
        (( current != 0 )) && any_truncated="true"
        previous_truncated=$current
        current=$(measurement_record_field "$line" graph_cycles) || return 1
        stage_value=$(measurement_record_field "$line" stage_graph_cycles) || return 1
        (( current >= previous_cycles && stage_value == current - previous_cycles )) || return 1
        if [[ "$expected_first" == "none" ]] && (( current != 0 )); then
            expected_first="${expected_prefixes[index]}"
        fi
        two_lp_cycles=$(measurement_record_field "$line" cycles_with_accepted_2lp) || return 1
        if [[ "$expected_first_two_lp" == "none" ]] && (( two_lp_cycles != 0 )); then
            expected_first_two_lp="${expected_prefixes[index]}"
        fi
        previous_cycles=$current
    done
    local summary="${lines[6]}"
    [[ "$(measurement_record_field "$summary" any_capacity_truncation)" == "$any_truncated" &&
       "$(measurement_record_field "$summary" first_cycle_prefix)" == "$expected_first" &&
       "$(measurement_record_field "$summary" first_two_lp_cycle_prefix)" == "$expected_first_two_lp" ]] || return 1

    SIQS_CYCLE_PROFILE_OUTPUT="$stdout"
    SIQS_CYCLE_PROFILE_IDENTITY="$identity"
}

run_siqs_cycle_profile_process() {
    local workers="$1"
    local timeout_seconds="$2"
    local binary="${BUILD_DIR}/test_siqs_multi_a_cycle_profile"
    local stdout_file stderr_file
    stdout_file=$(mktemp "${TMPDIR:-/tmp}/gnfs_siqs_cycle_profile_stdout.XXXXXX")
    stderr_file=$(mktemp "${TMPDIR:-/tmp}/gnfs_siqs_cycle_profile_stderr.XXXXXX")

    local start_ms exit_code=0
    start_ms=$(timer_start_ms)
    run_with_timeout "$timeout_seconds" /bin/sh -c \
        'stdout_path=$1; stderr_path=$2; shift 2; exec "$@" >"$stdout_path" 2>"$stderr_path"' \
        sh "$stdout_file" "$stderr_file" "$binary" --workers "$workers" || exit_code=$?
    local end_ms
    end_ms=$(timer_start_ms)
    local elapsed=$((end_ms - start_ms))
    TOTAL_TIME_MS=$((TOTAL_TIME_MS + elapsed))
    (( TOTAL_TESTS += 1 ))

    local line_count blank_count last_byte stdout
    line_count=$(awk 'END { print NR + 0 }' "$stdout_file")
    blank_count=$(awk 'NF == 0 { count += 1 } END { print count + 0 }' "$stdout_file")
    last_byte=$(tail -c 1 "$stdout_file" | od -An -tuC | tr -d ' ')
    stdout=$(<"$stdout_file")
    SIQS_CYCLE_PROFILE_STDERR=$(<"$stderr_file")
    rm -f "$stdout_file" "$stderr_file"

    if (( exit_code == 124 )); then
        log_fail "SIQS cycle-density workers=${workers} TIMEOUT after ${timeout_seconds}s"
        (( FAILED_TESTS += 1 ))
        return 1
    fi
    if (( exit_code != 0 )); then
        log_fail "SIQS cycle-density workers=${workers} 退出码 ${exit_code}；拒绝任何部分输出"
        [[ -n "$SIQS_CYCLE_PROFILE_STDERR" ]] &&
            printf '%s\n' "$SIQS_CYCLE_PROFILE_STDERR" | tail -10
        (( FAILED_TESTS += 1 ))
        return 1
    fi
    if [[ -n "$SIQS_CYCLE_PROFILE_STDERR" ]]; then
        log_fail "SIQS cycle-density 成功进程不得写入 stderr"
        printf '%s\n' "$SIQS_CYCLE_PROFILE_STDERR" | tail -10
        (( FAILED_TESTS += 1 ))
        return 1
    fi
    if [[ "$line_count" != "6" || "$blank_count" != "0" || "$last_byte" != "10" ]] ||
       ! validate_siqs_cycle_profile_output "$stdout" "$workers"; then
        log_fail "SIQS cycle-density stdout 必须严格为 CONFIG_V2 + 4 PREFIX_V2 + SUMMARY_V2"
        (( FAILED_TESTS += 1 ))
        return 1
    fi

    (( PASSED_TESTS += 1 ))
    REPORT_ENTRIES+=("{\"name\":\"test_siqs_multi_a_cycle_profile_w${workers}\",\"status\":\"pass\",\"elapsed_ms\":${elapsed}}")
    return 0
}

# Closed V3 parser for the Release-only 256-A scale transcript. The caller
# supplies file-level facts because command substitution intentionally strips
# the final LF. Python is already a Harness dependency and gives exact integer
# arithmetic for byte counters while leaving uint64 digests as canonical text.
SIQS_256A_PROFILE_OUTPUT=""
SIQS_256A_PROFILE_IDENTITY=""
SIQS_256A_PROFILE_STDERR=""
SIQS_256A_SCALE_PASS=0
SIQS_256A_VALIDATOR_BASELINE=""
SIQS_256A_VALIDATOR_NOT_ATTEMPTED=""

validate_siqs_256a_profile_output() {
    local stdout="$1"
    local expected_workers="$2"
    local line_count="$3"
    local blank_count="$4"
    local last_byte="$5"
    local identity

    SIQS_256A_PROFILE_OUTPUT=""
    SIQS_256A_PROFILE_IDENTITY=""
    SIQS_256A_SCALE_PASS=0
    [[ "$line_count" == "6" && "$blank_count" == "0" && "$last_byte" == "10" ]] || return 1

    if ! identity=$("$GNFS_TEST_PYTHON" - "$expected_workers" 3<<<"$stdout" <<'PY'
import os
import re
import sys

def need(condition):
    if not condition:
        raise ValueError

def same(value, expected):
    return value == str(expected)

try:
    expected_workers = int(sys.argv[1])
    need(expected_workers in (1, 2, 4))
    lines = os.fdopen(3, encoding="utf-8").read().splitlines()
    need(len(lines) == 6 and all(lines))

    prefixes = [
        "GNFS_SIQS_256A_CONFIG_V3",
        "GNFS_SIQS_256A_CAPTURE_V3",
        "GNFS_SIQS_256A_GRAPH_V3",
        "GNFS_SIQS_256A_ASSEMBLY_V3",
        "GNFS_SIQS_256A_PROOF_V3",
        "GNFS_SIQS_256A_SUMMARY_V3",
    ]
    schemas = [
        """schema_version status profile_id build_type ndebug band digits n p q seed max_a
        unique_a b_per_a available_b_per_a complete_b_family batch_schedule batch_max_a
        batch_barrier partition admission_order admission_race_first global_cap_boundary
        timeout_seconds planner_attempts planner_duplicate_draws accepted_duplicate_a first_a
        last_a plan_digest_low plan_digest_high multiplier sieved_n sieved_bits
        factor_base_columns factor_base_last_prime param_fb_size param_sieve_half
        param_lp_multiplier param_a_factors param_sieve_error param_small_prime_cutoff threshold
        relation_limit_per_slot payload_limit_bytes_per_slot global_raw_limit
        global_payload_limit_bytes graph_edge_limit graph_cycle_limit graph_incidence_limit
        row_candidate_limit pretrim_limit shadow_trim_excess selected_required min_2lp_cycles
        min_2lp_edge_source_a rss_budget_bytes solver_attempted promotion""".split(),
        """schema_version status profile_id batches completed_batches unstarted_batches planned_a
        completed_a unstarted_a planned_slots completed_slots unstarted_slots produced_relations
        admitted_relations discarded_relations produced_full produced_one_lp produced_two_lp
        admitted_full admitted_one_lp admitted_two_lp admitted_payload_bytes
        discarded_payload_bytes terminal_slot_a terminal_slot_gray first_rejected_a
        first_rejected_gray first_rejected_relation first_rejected_reason global_cap_precedence
        threshold_candidates unrepresentable_residuals rejected_residuals observed_full
        observed_one_lp observed_two_lp produced_payload_bytes slot_stop_none
        slot_stop_relation_limit slot_stop_payload_limit slot_digest_low slot_digest_high
        raw_digest_low raw_digest_high workers resolved_workers peak_workers capture_wall_ns
        solver_attempted promotion""".split(),
        """schema_version status profile_id attempted adapter_input adapter_full
        adapter_accepted_one_lp adapter_accepted_two_lp adapter_rejected adapter_exact_duplicate
        adapter_malformed_source_shape adapter_unsupported_encoding adapter_invalid_one_large_prime
        adapter_invalid_two_large_prime_split graph_status graph_input_edges graph_vertices
        graph_edges graph_components graph_cycles graph_cycle_incidences graph_max_cycle_length
        cycles_with_accepted_2lp cycles_without_accepted_2lp two_lp_edge_source_a_count
        cycle_source_a_count cycle_provenance_digest_low cycle_provenance_digest_high
        row_candidate_upper solver_attempted promotion""".split(),
        """schema_version status profile_id attempted assembly_status graph_edges graph_cycles
        valid_full full_sources partial_sources valid_cycle_rows rejected_cycle_rows
        rows_before_dedup arithmetic_duplicates_removed pretrim_rows required_rows row_deficit
        selected_rows selected_full_rows selected_cycle_rows trimmed_rows source_fingerprint_low
        source_fingerprint_high pretrim_fingerprint_low pretrim_fingerprint_high
        selected_fingerprint_low selected_fingerprint_high solver_attempted promotion""".split(),
        """schema_version attempted status factor cofactor deterministic_terminal
        solver_attempted promotion""".split(),
        """schema_version status profile_id stdout_records config_records capture_records
        graph_records assembly_records proof_records summary_records workers rss_scope rss_backend
        rss_evidence scale_evidence final_current_rss_bytes final_peak_rss_bytes plan_wall_ns
        capture_wall_ns analysis_wall_ns wall_ns solver_attempted proof_status promotion""".split(),
    ]
    nonnumeric = {
        "status", "profile_id", "build_type", "ndebug", "complete_b_family",
        "batch_schedule", "batch_barrier", "partition", "admission_order",
        "admission_race_first", "global_cap_boundary", "solver_attempted", "promotion",
        "first_rejected_reason", "global_cap_precedence", "attempted", "graph_status",
        "assembly_status", "factor", "cofactor", "deterministic_terminal", "rss_scope",
        "rss_backend", "rss_evidence", "scale_evidence", "proof_status",
    }
    optional_index = {
        "terminal_slot_a", "terminal_slot_gray", "first_rejected_a",
        "first_rejected_gray", "first_rejected_relation",
    }
    optional_rss = {"final_current_rss_bytes", "final_peak_rss_bytes"}
    canonical = re.compile(r"0|[1-9][0-9]*\Z")
    positive = re.compile(r"[1-9][0-9]*\Z")
    records = []
    for prefix, schema, line in zip(prefixes, schemas, lines):
        tokens = line.split(" ")
        need(tokens[0] == prefix and len(tokens) == len(schema) + 1)
        record = {}
        for expected_key, token in zip(schema, tokens[1:]):
            need("=" in token)
            key, value = token.split("=", 1)
            need(key == expected_key and value and key not in record)
            if key in optional_index:
                need(value == "none" or canonical.fullmatch(value))
            elif key in optional_rss:
                need(value == "na" or positive.fullmatch(value))
            elif key not in nonnumeric:
                need(canonical.fullmatch(value))
            record[key] = value
        records.append(record)

    c, cap, graph, assembly, proof, summary = records
    I = lambda record, key: int(record[key])
    profile_id = "siqs50_multi_a_256x32_scale_v3"
    terminals = {
        "solver_ready", "slot_relation_limit", "slot_payload_limit",
        "global_relation_limit", "global_payload_limit", "graph_edge_limit",
        "graph_cycle_limit", "graph_incidence_limit", "row_candidate_limit",
        "pretrim_limit", "insufficient_rows", "rejected_cycle_rows",
        "arithmetic_duplicates", "insufficient_two_lp_cycles",
        "insufficient_two_lp_source_a",
    }
    terminal = c["status"]
    need(terminal in terminals)
    for record in (cap, graph, assembly, summary):
        need(record["status"] == terminal and record["profile_id"] == profile_id)
    need(c["profile_id"] == profile_id and proof["deterministic_terminal"] == terminal)
    for record in records:
        need(record["schema_version"] == "3" and record["solver_attempted"] == "false" and
             record["promotion"] == "false")

    need(c["build_type"] == "Release" and c["ndebug"] == "true")
    fixed_config = {
        "band": "50", "digits": "50",
        "n": "18027426610499408447671494571938206274555088868093",
        "p": "2041646378661656688438487", "q": "8829847714527711737483339",
        "seed": "42", "max_a": "256", "unique_a": "256", "b_per_a": "32",
        "available_b_per_a": "32", "complete_b_family": "true",
        "batch_schedule": "0-1,1-4,4-16,16-32,32-48,48-64,64-80,80-96,96-112,112-128,128-144,144-160,160-176,176-192,192-208,208-224,224-240,240-256",
        "batch_max_a": "16", "batch_barrier": "true", "partition": "static_contiguous",
        "admission_order": "a_gray_relation", "admission_race_first": "false",
        "global_cap_boundary": "next_gt_limit", "timeout_seconds": "1800",
        "accepted_duplicate_a": "0", "factor_base_columns": "1601",
        "param_fb_size": "1600", "param_sieve_half": "65536",
        "param_lp_multiplier": "120", "param_a_factors": "6",
        "param_sieve_error": "12", "param_small_prime_cutoff": "25",
        "relation_limit_per_slot": "32", "payload_limit_bytes_per_slot": "65536",
        "global_raw_limit": "32768", "global_payload_limit_bytes": "67108864",
        "graph_edge_limit": "16384", "graph_cycle_limit": "4096",
        "graph_incidence_limit": "262144", "row_candidate_limit": "4096",
        "pretrim_limit": "4096", "shadow_trim_excess": "100",
        "selected_required": "1701", "min_2lp_cycles": "32",
        "min_2lp_edge_source_a": "16", "rss_budget_bytes": "536870912",
    }
    need(all(c[key] == value for key, value in fixed_config.items()))
    need(I(c, "planner_attempts") == 256 + I(c, "planner_duplicate_draws"))
    need(I(c, "first_a") > 0 and I(c, "last_a") > 0 and I(c, "multiplier") > 0)
    need(I(c, "sieved_bits") > 0 and I(c, "factor_base_last_prime") > 0 and
         I(c, "selected_required") == I(c, "factor_base_columns") + I(c, "shadow_trim_excess"))
    need((c["plan_digest_low"], c["plan_digest_high"]) != ("0", "0"))

    need(cap["global_cap_precedence"] == "relation_then_payload")
    need(I(cap, "batches") == 18 and I(cap, "planned_a") == 256 and
         I(cap, "planned_slots") == 8192)
    need(I(cap, "batches") == I(cap, "completed_batches") + I(cap, "unstarted_batches"))
    need(I(cap, "planned_a") == I(cap, "completed_a") + I(cap, "unstarted_a"))
    need(I(cap, "planned_slots") == I(cap, "completed_slots") + I(cap, "unstarted_slots"))
    endpoints = [1, 4, 16] + list(range(32, 257, 16))
    completed_batches = I(cap, "completed_batches")
    need(1 <= completed_batches <= 18 and I(cap, "completed_a") == endpoints[completed_batches - 1])
    completed_batch_begin = 0 if completed_batches == 1 else endpoints[completed_batches - 2]
    need(I(cap, "completed_slots") == I(cap, "completed_a") * 32)
    need(I(cap, "produced_relations") == I(cap, "admitted_relations") + I(cap, "discarded_relations"))
    need(I(cap, "produced_relations") == I(cap, "produced_full") + I(cap, "produced_one_lp") + I(cap, "produced_two_lp"))
    need(I(cap, "admitted_relations") == I(cap, "admitted_full") + I(cap, "admitted_one_lp") + I(cap, "admitted_two_lp"))
    need(I(cap, "produced_payload_bytes") == I(cap, "admitted_payload_bytes") + I(cap, "discarded_payload_bytes"))
    need(I(cap, "produced_relations") <= I(cap, "completed_slots") * 32 and
         I(cap, "produced_payload_bytes") <= I(cap, "completed_slots") * 65536)
    need(I(cap, "admitted_relations") <= 32768 and I(cap, "admitted_payload_bytes") <= 67108864)
    need(I(cap, "admitted_full") <= I(cap, "produced_full") and
         I(cap, "admitted_one_lp") <= I(cap, "produced_one_lp") and
         I(cap, "admitted_two_lp") <= I(cap, "produced_two_lp"))
    need(I(cap, "produced_full") <= I(cap, "observed_full") and
         I(cap, "produced_one_lp") <= I(cap, "observed_one_lp") and
         I(cap, "produced_two_lp") <= I(cap, "observed_two_lp"))
    need((I(cap, "produced_relations") == 0) == (I(cap, "produced_payload_bytes") == 0) and
         (I(cap, "admitted_relations") == 0) == (I(cap, "admitted_payload_bytes") == 0))
    need(I(cap, "threshold_candidates") == I(cap, "unrepresentable_residuals") +
         I(cap, "rejected_residuals") + I(cap, "observed_full") +
         I(cap, "observed_one_lp") + I(cap, "observed_two_lp"))
    need(I(cap, "observed_full") + I(cap, "observed_one_lp") + I(cap, "observed_two_lp") >=
         I(cap, "produced_relations"))
    need(I(cap, "completed_slots") == I(cap, "slot_stop_none") +
         I(cap, "slot_stop_relation_limit") + I(cap, "slot_stop_payload_limit"))
    need(cap["workers"] == str(expected_workers) and cap["resolved_workers"] == str(expected_workers) and
         cap["peak_workers"] == str(expected_workers) and I(cap, "capture_wall_ns") > 0)
    need((cap["slot_digest_low"], cap["slot_digest_high"]) != ("0", "0") and
         (cap["raw_digest_low"], cap["raw_digest_high"]) != ("0", "0"))

    capture_terminals = {"slot_relation_limit", "slot_payload_limit", "global_relation_limit", "global_payload_limit"}
    slot_terminals = {"slot_relation_limit", "slot_payload_limit"}
    global_terminals = {"global_relation_limit", "global_payload_limit"}
    if terminal in slot_terminals:
        need(cap["terminal_slot_a"] != "none" and cap["terminal_slot_gray"] != "none")
        need(completed_batch_begin <= I(cap, "terminal_slot_a") < I(cap, "completed_a") and
             I(cap, "terminal_slot_gray") < 32)
        need(cap["first_rejected_a"] == cap["first_rejected_gray"] ==
             cap["first_rejected_relation"] == "none" and cap["first_rejected_reason"] == "none")
        need(I(cap, "discarded_relations") > 0 and I(cap, "discarded_payload_bytes") > 0)
        stop_key = "slot_stop_relation_limit" if terminal == "slot_relation_limit" else "slot_stop_payload_limit"
        need(I(cap, stop_key) > 0)
    elif terminal in global_terminals:
        need(cap["terminal_slot_a"] == cap["terminal_slot_gray"] == "none")
        need(cap["first_rejected_a"] != "none" and cap["first_rejected_gray"] != "none" and
             cap["first_rejected_relation"] != "none" and cap["first_rejected_reason"] == terminal)
        need(completed_batch_begin <= I(cap, "first_rejected_a") < I(cap, "completed_a") and
             I(cap, "first_rejected_gray") < 32 and I(cap, "first_rejected_relation") < 32)
        need(I(cap, "slot_stop_none") == I(cap, "completed_slots") and
             I(cap, "slot_stop_relation_limit") == 0 and I(cap, "slot_stop_payload_limit") == 0 and
             I(cap, "discarded_relations") > 0 and I(cap, "discarded_payload_bytes") > 0)
        if terminal == "global_relation_limit":
            need(I(cap, "admitted_relations") == 32768 and
                 I(cap, "admitted_payload_bytes") <= 67108864)
        else:
            need(I(cap, "admitted_relations") < 32768 and
                 I(cap, "admitted_payload_bytes") <= 67108864)
    else:
        need(cap["terminal_slot_a"] == cap["terminal_slot_gray"] ==
             cap["first_rejected_a"] == cap["first_rejected_gray"] ==
             cap["first_rejected_relation"] == "none" and cap["first_rejected_reason"] == "none")

    if terminal not in capture_terminals:
        need(completed_batches == 18 and I(cap, "completed_a") == 256 and I(cap, "completed_slots") == 8192)
        need(I(cap, "discarded_relations") == 0 and I(cap, "discarded_payload_bytes") == 0)
        need(I(cap, "slot_stop_none") == 8192 and I(cap, "slot_stop_relation_limit") == 0 and
             I(cap, "slot_stop_payload_limit") == 0)

    graph_caps = {
        "graph_edge_limit": "edge_limit",
        "graph_cycle_limit": "cycle_limit",
        "graph_incidence_limit": "incidence_limit",
    }
    graph_attempted = graph["attempted"] == "true"
    need(graph["attempted"] in {"true", "false"})
    need(graph_attempted == (terminal not in capture_terminals))
    adapter_keys = ["adapter_input", "adapter_full", "adapter_accepted_one_lp",
                    "adapter_accepted_two_lp", "adapter_rejected", "adapter_exact_duplicate",
                    "adapter_malformed_source_shape", "adapter_unsupported_encoding",
                    "adapter_invalid_one_large_prime", "adapter_invalid_two_large_prime_split"]
    graph_value_keys = ["graph_input_edges", "graph_vertices", "graph_edges", "graph_components",
                        "graph_cycles", "graph_cycle_incidences", "graph_max_cycle_length",
                        "cycles_with_accepted_2lp", "cycles_without_accepted_2lp",
                        "two_lp_edge_source_a_count", "cycle_source_a_count",
                        "cycle_provenance_digest_low", "cycle_provenance_digest_high",
                        "row_candidate_upper"]
    if not graph_attempted:
        need(graph["graph_status"] == "not_attempted" and
             all(I(graph, key) == 0 for key in adapter_keys + graph_value_keys))
    else:
        need(I(graph, "adapter_input") == I(cap, "admitted_relations") and
             I(graph, "adapter_full") == I(cap, "admitted_full"))
        need(I(graph, "adapter_accepted_one_lp") <= I(cap, "admitted_one_lp") and
             I(graph, "adapter_accepted_two_lp") <= I(cap, "admitted_two_lp"))
        need(I(graph, "adapter_input") == I(graph, "adapter_full") +
             I(graph, "adapter_accepted_one_lp") + I(graph, "adapter_accepted_two_lp") +
             I(graph, "adapter_rejected"))
        need(I(graph, "adapter_rejected") == I(graph, "adapter_exact_duplicate") +
             I(graph, "adapter_malformed_source_shape") + I(graph, "adapter_unsupported_encoding") +
             I(graph, "adapter_invalid_one_large_prime") + I(graph, "adapter_invalid_two_large_prime_split"))
        need(I(graph, "graph_input_edges") == I(graph, "adapter_accepted_one_lp") +
             I(graph, "adapter_accepted_two_lp"))
        if terminal in graph_caps:
            need(graph["graph_status"] == graph_caps[terminal] and
                 all(I(graph, key) == 0 for key in graph_value_keys[1:]))
            if terminal == "graph_edge_limit":
                need(I(graph, "graph_input_edges") > 16384)
            else:
                need(I(graph, "graph_input_edges") <= 16384)
        else:
            need(graph["graph_status"] == "valid" and I(graph, "graph_edges") == I(graph, "graph_input_edges"))
            need(I(graph, "graph_edges") <= 16384 and I(graph, "graph_cycles") <= 4096 and
                 I(graph, "graph_cycle_incidences") <= 262144)
            need(I(graph, "graph_components") <= I(graph, "graph_vertices") and
                 (I(graph, "graph_edges") != 0 or
                  (I(graph, "graph_vertices") == 0 and I(graph, "graph_components") == 0)))
            need(I(graph, "graph_edges") + I(graph, "graph_components") >= I(graph, "graph_vertices"))
            need(I(graph, "graph_cycles") == I(graph, "graph_edges") +
                 I(graph, "graph_components") - I(graph, "graph_vertices"))
            need(I(graph, "graph_cycles") == I(graph, "cycles_with_accepted_2lp") +
                 I(graph, "cycles_without_accepted_2lp"))
            need(I(graph, "row_candidate_upper") == I(graph, "adapter_full") + I(graph, "graph_cycles"))
            need(I(graph, "two_lp_edge_source_a_count") <= 256 and I(graph, "cycle_source_a_count") <= 256)
            need((graph["cycle_provenance_digest_low"], graph["cycle_provenance_digest_high"]) != ("0", "0"))
            if I(graph, "graph_cycles") == 0:
                need(I(graph, "graph_cycle_incidences") == 0 and I(graph, "graph_max_cycle_length") == 0 and
                     I(graph, "cycles_with_accepted_2lp") == 0 and I(graph, "cycle_source_a_count") == 0)
            else:
                need(I(graph, "graph_cycle_incidences") >= I(graph, "graph_cycles") and
                     0 < I(graph, "graph_max_cycle_length") <= I(graph, "graph_cycle_incidences") and
                     I(graph, "graph_cycle_incidences") <= I(graph, "graph_cycles") * I(graph, "graph_max_cycle_length"))

    assembly_terminals = {"pretrim_limit", "insufficient_rows", "rejected_cycle_rows",
                          "arithmetic_duplicates", "insufficient_two_lp_cycles",
                          "insufficient_two_lp_source_a"}
    assembly_attempted = assembly["attempted"] == "true"
    need(assembly["attempted"] in {"true", "false"})
    need(assembly_attempted == (terminal == "solver_ready" or terminal in assembly_terminals))
    assembly_counter_keys = ["graph_edges", "graph_cycles", "valid_full", "full_sources",
                             "partial_sources", "valid_cycle_rows", "rejected_cycle_rows",
                             "rows_before_dedup", "arithmetic_duplicates_removed", "pretrim_rows",
                             "selected_rows", "selected_full_rows", "selected_cycle_rows", "trimmed_rows",
                             "source_fingerprint_low", "source_fingerprint_high",
                             "pretrim_fingerprint_low", "pretrim_fingerprint_high",
                             "selected_fingerprint_low", "selected_fingerprint_high"]
    need(I(assembly, "required_rows") == 1701)
    need(I(assembly, "row_deficit") == max(1701 - I(assembly, "selected_rows"), 0))
    if not assembly_attempted:
        need(assembly["assembly_status"] == "not_attempted" and
             all(I(assembly, key) == 0 for key in assembly_counter_keys) and
             I(assembly, "row_deficit") == 1701)
    else:
        need(assembly["assembly_status"] == "valid")
        need(I(graph, "row_candidate_upper") <= 4096)
        need(I(assembly, "graph_edges") == I(graph, "graph_edges") and
             I(assembly, "graph_cycles") == I(graph, "graph_cycles") and
             I(assembly, "valid_full") == I(graph, "adapter_full") and
             I(assembly, "full_sources") <= I(assembly, "valid_full") and
             I(assembly, "partial_sources") == I(graph, "graph_edges"))
        need(I(assembly, "valid_cycle_rows") + I(assembly, "rejected_cycle_rows") == I(assembly, "graph_cycles"))
        need(I(assembly, "rows_before_dedup") == I(assembly, "full_sources") + I(assembly, "valid_cycle_rows"))
        need(I(assembly, "rows_before_dedup") == I(assembly, "pretrim_rows") +
             I(assembly, "arithmetic_duplicates_removed"))
        need(I(assembly, "pretrim_rows") == I(assembly, "selected_rows") + I(assembly, "trimmed_rows"))
        need(I(assembly, "selected_rows") == I(assembly, "selected_full_rows") +
             I(assembly, "selected_cycle_rows") and I(assembly, "selected_rows") <= 1701)
        need(I(assembly, "selected_full_rows") <= I(assembly, "full_sources") and
             I(assembly, "selected_cycle_rows") <= I(assembly, "valid_cycle_rows"))
        need(I(assembly, "rows_before_dedup") <= I(graph, "row_candidate_upper"))
        for low, high in (("source_fingerprint_low", "source_fingerprint_high"),
                          ("pretrim_fingerprint_low", "pretrim_fingerprint_high"),
                          ("selected_fingerprint_low", "selected_fingerprint_high")):
            need((assembly[low], assembly[high]) != ("0", "0"))

    if terminal == "row_candidate_limit":
        need(I(graph, "row_candidate_upper") > 4096)
    elif terminal == "pretrim_limit":
        need(I(graph, "row_candidate_upper") <= 4096 and I(assembly, "pretrim_rows") > 4096)
    elif terminal == "insufficient_rows":
        need(I(graph, "row_candidate_upper") <= 4096 and I(assembly, "pretrim_rows") <= 4096 and
             (I(assembly, "pretrim_rows") < 1701 or I(assembly, "selected_rows") != 1701))
    elif terminal == "rejected_cycle_rows":
        need(I(assembly, "pretrim_rows") <= 4096 and I(assembly, "selected_rows") == 1701 and
             I(assembly, "rejected_cycle_rows") > 0)
    elif terminal == "arithmetic_duplicates":
        need(I(assembly, "pretrim_rows") <= 4096 and I(assembly, "selected_rows") == 1701 and
             I(assembly, "rejected_cycle_rows") == 0 and I(assembly, "arithmetic_duplicates_removed") > 0)
    elif terminal == "insufficient_two_lp_cycles":
        need(I(assembly, "pretrim_rows") <= 4096 and I(assembly, "selected_rows") == 1701 and
             I(assembly, "rejected_cycle_rows") == 0 and I(assembly, "arithmetic_duplicates_removed") == 0 and
             I(graph, "cycles_with_accepted_2lp") < 32)
    elif terminal == "insufficient_two_lp_source_a":
        need(I(assembly, "pretrim_rows") <= 4096 and I(assembly, "selected_rows") == 1701 and
             I(assembly, "rejected_cycle_rows") == 0 and I(assembly, "arithmetic_duplicates_removed") == 0 and
             I(graph, "cycles_with_accepted_2lp") >= 32 and I(graph, "two_lp_edge_source_a_count") < 16)
    elif terminal == "solver_ready":
        need(I(assembly, "pretrim_rows") <= 4096 and I(assembly, "pretrim_rows") >= 1701 and
             I(assembly, "selected_rows") == 1701 and I(assembly, "row_deficit") == 0 and
             I(assembly, "rejected_cycle_rows") == 0 and I(assembly, "arithmetic_duplicates_removed") == 0 and
             I(graph, "cycles_with_accepted_2lp") >= 32 and I(graph, "two_lp_edge_source_a_count") >= 16)

    need(proof["attempted"] == "false" and proof["status"] == "not_attempted" and
         proof["factor"] == "none" and proof["cofactor"] == "none")
    fixed_summary = {"stdout_records": "6", "config_records": "1", "capture_records": "1",
                     "graph_records": "1", "assembly_records": "1", "proof_records": "1",
                     "summary_records": "1", "rss_scope": "self_lifetime",
                     "proof_status": "not_attempted"}
    need(all(summary[key] == value for key, value in fixed_summary.items()))
    need(summary["workers"] == str(expected_workers))
    need(summary["rss_backend"] in {"unsupported", "darwin_getrusage", "linux_getrusage", "windows_psapi"})
    peak = summary["final_peak_rss_bytes"]
    expected_rss = "unavailable" if peak == "na" else ("over_budget" if int(peak) > 536870912 else "pass")
    need(summary["rss_evidence"] == expected_rss)
    if summary["rss_backend"] == "unsupported":
        need(summary["final_current_rss_bytes"] == "na" and peak == "na" and
             summary["rss_evidence"] == "unavailable")
    expected_scale = ("terminal" if terminal != "solver_ready" else
                      ("pass" if expected_rss == "pass" else
                       "unavailable" if expected_rss == "unavailable" else "fail"))
    need(summary["scale_evidence"] == expected_scale)
    if summary["final_current_rss_bytes"] != "na" and peak != "na":
        need(int(summary["final_current_rss_bytes"]) <= int(peak))
    need(I(summary, "plan_wall_ns") > 0 and I(summary, "capture_wall_ns") > 0 and
         I(summary, "wall_ns") >= I(summary, "plan_wall_ns") + I(summary, "capture_wall_ns") +
         I(summary, "analysis_wall_ns"))
    need(summary["capture_wall_ns"] == cap["capture_wall_ns"])
    if terminal in capture_terminals:
        need(I(summary, "analysis_wall_ns") == 0)
    else:
        need(I(summary, "analysis_wall_ns") > 0)

    if terminal == "solver_ready":
        frozen_config = {
            "planner_attempts": "256", "planner_duplicate_draws": "0",
            "first_a": "228011737959984857761", "last_a": "235884298804888144139",
            "plan_digest_low": "2132402111948970426",
            "plan_digest_high": "3331495609548214574", "multiplier": "5",
            "sieved_n": "90137133052497042238357472859691031372775444340465",
            "sieved_bits": "166", "factor_base_last_prime": "28979", "threshold": "61",
        }
        frozen_capture = {
            "batches": "18", "completed_batches": "18", "unstarted_batches": "0",
            "planned_a": "256", "completed_a": "256", "unstarted_a": "0",
            "planned_slots": "8192", "completed_slots": "8192", "unstarted_slots": "0",
            "produced_relations": "18008", "admitted_relations": "18008",
            "discarded_relations": "0", "produced_full": "1385", "produced_one_lp": "7420",
            "produced_two_lp": "9203", "admitted_full": "1385", "admitted_one_lp": "7420",
            "admitted_two_lp": "9203", "admitted_payload_bytes": "30050394",
            "discarded_payload_bytes": "0", "terminal_slot_a": "none",
            "terminal_slot_gray": "none", "first_rejected_a": "none",
            "first_rejected_gray": "none", "first_rejected_relation": "none",
            "first_rejected_reason": "none", "threshold_candidates": "164620",
            "unrepresentable_residuals": "36261", "rejected_residuals": "110351",
            "observed_full": "1385", "observed_one_lp": "7420", "observed_two_lp": "9203",
            "produced_payload_bytes": "30050394", "slot_stop_none": "8192",
            "slot_stop_relation_limit": "0", "slot_stop_payload_limit": "0",
            "slot_digest_low": "2675139373410695744",
            "slot_digest_high": "17317603334185087565",
            "raw_digest_low": "5272179497076428132",
            "raw_digest_high": "15848963677271175240",
        }
        frozen_graph = {
            "attempted": "true", "adapter_input": "18008", "adapter_full": "1385",
            "adapter_accepted_one_lp": "7419", "adapter_accepted_two_lp": "4624",
            "adapter_rejected": "4580", "adapter_exact_duplicate": "1",
            "adapter_malformed_source_shape": "0", "adapter_unsupported_encoding": "0",
            "adapter_invalid_one_large_prime": "0",
            "adapter_invalid_two_large_prime_split": "4579", "graph_status": "valid",
            "graph_input_edges": "12043", "graph_vertices": "13581", "graph_edges": "12043",
            "graph_components": "2346", "graph_cycles": "808",
            "graph_cycle_incidences": "1857", "graph_max_cycle_length": "6",
            "cycles_with_accepted_2lp": "191", "cycles_without_accepted_2lp": "617",
            "two_lp_edge_source_a_count": "160", "cycle_source_a_count": "256",
            "cycle_provenance_digest_low": "12977246761921163132",
            "cycle_provenance_digest_high": "12380521641813228706",
            "row_candidate_upper": "2193",
        }
        frozen_assembly = {
            "attempted": "true", "assembly_status": "valid", "graph_edges": "12043",
            "graph_cycles": "808", "valid_full": "1385", "full_sources": "1383",
            "partial_sources": "12043", "valid_cycle_rows": "808",
            "rejected_cycle_rows": "0", "rows_before_dedup": "2191",
            "arithmetic_duplicates_removed": "0", "pretrim_rows": "2191",
            "required_rows": "1701", "row_deficit": "0", "selected_rows": "1701",
            "selected_full_rows": "1383", "selected_cycle_rows": "318", "trimmed_rows": "490",
            "source_fingerprint_low": "13792072274280994075",
            "source_fingerprint_high": "7730575491251011754",
            "pretrim_fingerprint_low": "11231947477657928681",
            "pretrim_fingerprint_high": "13110892638711325127",
            "selected_fingerprint_low": "11745144848901110871",
            "selected_fingerprint_high": "3340986363997617983",
        }
        need(all(c[key] == value for key, value in frozen_config.items()) and
             all(cap[key] == value for key, value in frozen_capture.items()) and
             all(graph[key] == value for key, value in frozen_graph.items()) and
             all(assembly[key] == value for key, value in frozen_assembly.items()))

    dynamic = {"workers", "resolved_workers", "peak_workers", "plan_wall_ns", "capture_wall_ns",
               "analysis_wall_ns", "wall_ns", "final_current_rss_bytes", "final_peak_rss_bytes",
               "rss_evidence", "scale_evidence"}
    normalized = []
    for prefix, schema, record in zip(prefixes, schemas, records):
        normalized.append(" ".join([prefix] + [f"{key}={record[key]}" for key in schema if key not in dynamic]))
    print("\n".join(normalized))
except (ValueError, OSError, UnicodeError):
    sys.exit(1)
PY
    ); then
        return 1
    fi

    SIQS_256A_PROFILE_OUTPUT="$stdout"
    SIQS_256A_PROFILE_IDENTITY="$identity"
    if [[ "$(measurement_record_field "${${(@f)stdout}[6]}" status)" == "solver_ready" &&
          "$(measurement_record_field "${${(@f)stdout}[6]}" rss_evidence)" == "pass" &&
          "$(measurement_record_field "${${(@f)stdout}[6]}" scale_evidence)" == "pass" ]]; then
        SIQS_256A_SCALE_PASS=1
    fi
}

# Apply explicit key substitutions without changing record or field order. This
# helper is used only by the parser self-check fixtures below.
siqs_256a_mutate_transcript() {
    local stdout="$1"
    shift
    "$GNFS_TEST_PYTHON" - "$@" 3<<<"$stdout" <<'PY'
import os
import sys

lines = os.fdopen(3, encoding="utf-8").read().splitlines()
for mutation in sys.argv[1:]:
    record_text, assignment = mutation.split(":", 1)
    key, value = assignment.split("=", 1)
    record_index = int(record_text) - 1
    if record_index < 0 or record_index >= len(lines):
        raise SystemExit(1)
    tokens = lines[record_index].split(" ")
    matches = [index for index, token in enumerate(tokens) if token.startswith(key + "=")]
    if len(matches) != 1:
        raise SystemExit(1)
    tokens[matches[0]] = key + "=" + value
    lines[record_index] = " ".join(tokens)
print("\n".join(lines))
PY
}

# Cheap parser-only regression table. It deliberately never starts the profile
# binary; the real modes run this before their single fresh Release build.
siqs_256a_validator_self_check() {
    local baseline
    baseline=$(<<'EOF'
GNFS_SIQS_256A_CONFIG_V3 schema_version=3 status=solver_ready profile_id=siqs50_multi_a_256x32_scale_v3 build_type=Release ndebug=true band=50 digits=50 n=18027426610499408447671494571938206274555088868093 p=2041646378661656688438487 q=8829847714527711737483339 seed=42 max_a=256 unique_a=256 b_per_a=32 available_b_per_a=32 complete_b_family=true batch_schedule=0-1,1-4,4-16,16-32,32-48,48-64,64-80,80-96,96-112,112-128,128-144,144-160,160-176,176-192,192-208,208-224,224-240,240-256 batch_max_a=16 batch_barrier=true partition=static_contiguous admission_order=a_gray_relation admission_race_first=false global_cap_boundary=next_gt_limit timeout_seconds=1800 planner_attempts=256 planner_duplicate_draws=0 accepted_duplicate_a=0 first_a=228011737959984857761 last_a=235884298804888144139 plan_digest_low=2132402111948970426 plan_digest_high=3331495609548214574 multiplier=5 sieved_n=90137133052497042238357472859691031372775444340465 sieved_bits=166 factor_base_columns=1601 factor_base_last_prime=28979 param_fb_size=1600 param_sieve_half=65536 param_lp_multiplier=120 param_a_factors=6 param_sieve_error=12 param_small_prime_cutoff=25 threshold=61 relation_limit_per_slot=32 payload_limit_bytes_per_slot=65536 global_raw_limit=32768 global_payload_limit_bytes=67108864 graph_edge_limit=16384 graph_cycle_limit=4096 graph_incidence_limit=262144 row_candidate_limit=4096 pretrim_limit=4096 shadow_trim_excess=100 selected_required=1701 min_2lp_cycles=32 min_2lp_edge_source_a=16 rss_budget_bytes=536870912 solver_attempted=false promotion=false
GNFS_SIQS_256A_CAPTURE_V3 schema_version=3 status=solver_ready profile_id=siqs50_multi_a_256x32_scale_v3 batches=18 completed_batches=18 unstarted_batches=0 planned_a=256 completed_a=256 unstarted_a=0 planned_slots=8192 completed_slots=8192 unstarted_slots=0 produced_relations=18008 admitted_relations=18008 discarded_relations=0 produced_full=1385 produced_one_lp=7420 produced_two_lp=9203 admitted_full=1385 admitted_one_lp=7420 admitted_two_lp=9203 admitted_payload_bytes=30050394 discarded_payload_bytes=0 terminal_slot_a=none terminal_slot_gray=none first_rejected_a=none first_rejected_gray=none first_rejected_relation=none first_rejected_reason=none global_cap_precedence=relation_then_payload threshold_candidates=164620 unrepresentable_residuals=36261 rejected_residuals=110351 observed_full=1385 observed_one_lp=7420 observed_two_lp=9203 produced_payload_bytes=30050394 slot_stop_none=8192 slot_stop_relation_limit=0 slot_stop_payload_limit=0 slot_digest_low=2675139373410695744 slot_digest_high=17317603334185087565 raw_digest_low=5272179497076428132 raw_digest_high=15848963677271175240 workers=1 resolved_workers=1 peak_workers=1 capture_wall_ns=20 solver_attempted=false promotion=false
GNFS_SIQS_256A_GRAPH_V3 schema_version=3 status=solver_ready profile_id=siqs50_multi_a_256x32_scale_v3 attempted=true adapter_input=18008 adapter_full=1385 adapter_accepted_one_lp=7419 adapter_accepted_two_lp=4624 adapter_rejected=4580 adapter_exact_duplicate=1 adapter_malformed_source_shape=0 adapter_unsupported_encoding=0 adapter_invalid_one_large_prime=0 adapter_invalid_two_large_prime_split=4579 graph_status=valid graph_input_edges=12043 graph_vertices=13581 graph_edges=12043 graph_components=2346 graph_cycles=808 graph_cycle_incidences=1857 graph_max_cycle_length=6 cycles_with_accepted_2lp=191 cycles_without_accepted_2lp=617 two_lp_edge_source_a_count=160 cycle_source_a_count=256 cycle_provenance_digest_low=12977246761921163132 cycle_provenance_digest_high=12380521641813228706 row_candidate_upper=2193 solver_attempted=false promotion=false
GNFS_SIQS_256A_ASSEMBLY_V3 schema_version=3 status=solver_ready profile_id=siqs50_multi_a_256x32_scale_v3 attempted=true assembly_status=valid graph_edges=12043 graph_cycles=808 valid_full=1385 full_sources=1383 partial_sources=12043 valid_cycle_rows=808 rejected_cycle_rows=0 rows_before_dedup=2191 arithmetic_duplicates_removed=0 pretrim_rows=2191 required_rows=1701 row_deficit=0 selected_rows=1701 selected_full_rows=1383 selected_cycle_rows=318 trimmed_rows=490 source_fingerprint_low=13792072274280994075 source_fingerprint_high=7730575491251011754 pretrim_fingerprint_low=11231947477657928681 pretrim_fingerprint_high=13110892638711325127 selected_fingerprint_low=11745144848901110871 selected_fingerprint_high=3340986363997617983 solver_attempted=false promotion=false
GNFS_SIQS_256A_PROOF_V3 schema_version=3 attempted=false status=not_attempted factor=none cofactor=none deterministic_terminal=solver_ready solver_attempted=false promotion=false
GNFS_SIQS_256A_SUMMARY_V3 schema_version=3 status=solver_ready profile_id=siqs50_multi_a_256x32_scale_v3 stdout_records=6 config_records=1 capture_records=1 graph_records=1 assembly_records=1 proof_records=1 summary_records=1 workers=1 rss_scope=self_lifetime rss_backend=darwin_getrusage rss_evidence=pass scale_evidence=pass final_current_rss_bytes=1000 final_peak_rss_bytes=2000 plan_wall_ns=10 capture_wall_ns=20 analysis_wall_ns=30 wall_ns=70 solver_attempted=false proof_status=not_attempted promotion=false
EOF
    )

    validate_siqs_256a_profile_output "$baseline" 1 6 0 10 || return 1
    (( SIQS_256A_SCALE_PASS == 1 )) || return 1
    local baseline_identity="$SIQS_256A_PROFILE_IDENTITY"
    local unknown missing duplicate field_order line_order conservation attempted forged_scale
    local frozen_drift over_payload over_incidence selected_subtype unsupported_numeric
    local false_row_candidate false_pretrim false_insufficient
    unknown=$(printf '%s\n' "$baseline" | awk 'NR == 1 { $0 = $0 " unknown=1" } { print }')
    missing="${baseline/ build_type=Release/}"
    duplicate=$(printf '%s\n' "$baseline" | awk 'NR == 1 { $0 = $0 " build_type=Release" } { print }')
    field_order="${baseline/profile_id=siqs50_multi_a_256x32_scale_v3 build_type=Release ndebug=true/profile_id=siqs50_multi_a_256x32_scale_v3 ndebug=true build_type=Release}"
    line_order=$(printf '%s\n' "$baseline" | awk 'NR == 1 { first=$0; next } NR == 2 { print; print first; next } { print }')
    conservation="${baseline/discarded_relations=0/discarded_relations=1}"
    attempted=$(siqs_256a_mutate_transcript "$baseline" "3:attempted=false") || return 1
    forged_scale=$(siqs_256a_mutate_transcript "$baseline" \
        "1:status=insufficient_two_lp_cycles" "2:status=insufficient_two_lp_cycles" \
        "3:status=insufficient_two_lp_cycles" "3:cycles_with_accepted_2lp=31" \
        "3:cycles_without_accepted_2lp=777" "4:status=insufficient_two_lp_cycles" \
        "5:deterministic_terminal=insufficient_two_lp_cycles" \
        "6:status=insufficient_two_lp_cycles") || return 1
    frozen_drift=$(siqs_256a_mutate_transcript "$baseline" "1:threshold=62") || return 1
    over_payload=$(siqs_256a_mutate_transcript "$baseline" \
        "2:admitted_payload_bytes=67108865" "2:produced_payload_bytes=67108865") || return 1
    over_incidence=$(siqs_256a_mutate_transcript "$baseline" "3:graph_cycle_incidences=262145") || return 1
    selected_subtype=$(siqs_256a_mutate_transcript "$baseline" \
        "4:selected_full_rows=1384" "4:selected_cycle_rows=317") || return 1
    unsupported_numeric=$(siqs_256a_mutate_transcript "$baseline" "6:rss_backend=unsupported") || return 1
    false_row_candidate=$(siqs_256a_mutate_transcript "$baseline" \
        "1:status=row_candidate_limit" "2:status=row_candidate_limit" \
        "3:status=row_candidate_limit" "4:status=row_candidate_limit" \
        "5:deterministic_terminal=row_candidate_limit" "6:status=row_candidate_limit" \
        "6:scale_evidence=terminal") || return 1
    false_pretrim=$(siqs_256a_mutate_transcript "$baseline" \
        "1:status=pretrim_limit" "2:status=pretrim_limit" "3:status=pretrim_limit" \
        "4:status=pretrim_limit" "5:deterministic_terminal=pretrim_limit" \
        "6:status=pretrim_limit" "6:scale_evidence=terminal") || return 1
    false_insufficient=$(siqs_256a_mutate_transcript "$baseline" \
        "1:status=insufficient_rows" "2:status=insufficient_rows" \
        "3:status=insufficient_rows" "4:status=insufficient_rows" \
        "5:deterministic_terminal=insufficient_rows" "6:status=insufficient_rows" \
        "6:scale_evidence=terminal") || return 1

    local mutation_index mutation
    typeset -a mutation_names=(unknown missing duplicate field_order line_order conservation \
        attempted forged_scale frozen_drift over_payload over_incidence selected_subtype \
        unsupported_numeric false_row_candidate false_pretrim false_insufficient)
    typeset -a mutations=("$unknown" "$missing" "$duplicate" "$field_order" "$line_order" \
        "$conservation" "$attempted" "$forged_scale" "$frozen_drift" "$over_payload" \
        "$over_incidence" "$selected_subtype" "$unsupported_numeric" "$false_row_candidate" \
        "$false_pretrim" "$false_insufficient")
    for (( mutation_index = 1; mutation_index <= ${#mutations[@]}; ++mutation_index )); do
        mutation="${mutations[$mutation_index]}"
        if validate_siqs_256a_profile_output "$mutation" 1 6 0 10 2>/dev/null; then
            log_fail "SIQS 256-A validator self-check 未拒绝 ${mutation_names[$mutation_index]} mutation"
            return 1
        fi
    done
    if validate_siqs_256a_profile_output "$baseline" 1 6 0 9 2>/dev/null; then
        log_fail "SIQS 256-A validator self-check 未拒绝缺失末尾 LF"
        return 1
    fi

    local dynamic="$baseline"
    dynamic="${dynamic//workers=1/workers=4}"
    dynamic="${dynamic//capture_wall_ns=20/capture_wall_ns=21}"
    dynamic="${dynamic/plan_wall_ns=10/plan_wall_ns=11}"
    dynamic="${dynamic/analysis_wall_ns=30/analysis_wall_ns=31}"
    dynamic="${dynamic/wall_ns=70/wall_ns=75}"
    dynamic="${dynamic/final_current_rss_bytes=1000/final_current_rss_bytes=1200}"
    dynamic="${dynamic/final_peak_rss_bytes=2000/final_peak_rss_bytes=2200}"
    validate_siqs_256a_profile_output "$dynamic" 4 6 0 10 || return 1
    [[ "$SIQS_256A_PROFILE_IDENTITY" == "$baseline_identity" && SIQS_256A_SCALE_PASS -eq 1 ]] || {
        log_fail "SIQS 256-A validator self-check 的动态字段 normalization 不闭合"
        return 1
    }

    local unavailable capture_terminal graph_terminal assembly_terminal
    unavailable=$(siqs_256a_mutate_transcript "$baseline" \
        "6:rss_evidence=unavailable" "6:scale_evidence=unavailable" \
        "6:final_current_rss_bytes=na" \
        "6:final_peak_rss_bytes=na") || return 1

    capture_terminal=$(siqs_256a_mutate_transcript "$baseline" \
        "1:status=slot_relation_limit" \
        "2:status=slot_relation_limit" "2:completed_batches=1" "2:unstarted_batches=17" \
        "2:completed_a=1" "2:unstarted_a=255" "2:completed_slots=32" \
        "2:unstarted_slots=8160" "2:produced_relations=10" "2:admitted_relations=0" \
        "2:discarded_relations=10" "2:produced_full=4" "2:produced_one_lp=3" \
        "2:produced_two_lp=3" "2:admitted_full=0" "2:admitted_one_lp=0" \
        "2:admitted_two_lp=0" "2:admitted_payload_bytes=0" \
        "2:discarded_payload_bytes=1000" "2:terminal_slot_a=0" \
        "2:terminal_slot_gray=0" "2:threshold_candidates=10" \
        "2:unrepresentable_residuals=0" "2:rejected_residuals=0" "2:observed_full=4" \
        "2:observed_one_lp=3" "2:observed_two_lp=3" "2:produced_payload_bytes=1000" \
        "2:slot_stop_none=31" "2:slot_stop_relation_limit=1" \
        "3:status=slot_relation_limit" "3:attempted=false" "3:adapter_input=0" \
        "3:adapter_full=0" "3:adapter_accepted_one_lp=0" "3:adapter_accepted_two_lp=0" \
        "3:adapter_rejected=0" "3:adapter_exact_duplicate=0" \
        "3:adapter_invalid_two_large_prime_split=0" "3:graph_status=not_attempted" \
        "3:graph_input_edges=0" "3:graph_vertices=0" "3:graph_edges=0" \
        "3:graph_components=0" "3:graph_cycles=0" "3:graph_cycle_incidences=0" \
        "3:graph_max_cycle_length=0" "3:cycles_with_accepted_2lp=0" \
        "3:cycles_without_accepted_2lp=0" "3:two_lp_edge_source_a_count=0" \
        "3:cycle_source_a_count=0" "3:cycle_provenance_digest_low=0" \
        "3:cycle_provenance_digest_high=0" "3:row_candidate_upper=0" \
        "4:status=slot_relation_limit" "4:attempted=false" "4:assembly_status=not_attempted" \
        "4:graph_edges=0" "4:graph_cycles=0" "4:valid_full=0" "4:full_sources=0" \
        "4:partial_sources=0" "4:valid_cycle_rows=0" "4:rows_before_dedup=0" \
        "4:pretrim_rows=0" "4:row_deficit=1701" "4:selected_rows=0" \
        "4:selected_full_rows=0" "4:selected_cycle_rows=0" "4:trimmed_rows=0" \
        "4:source_fingerprint_low=0" "4:source_fingerprint_high=0" \
        "4:pretrim_fingerprint_low=0" "4:pretrim_fingerprint_high=0" \
        "4:selected_fingerprint_low=0" "4:selected_fingerprint_high=0" \
        "5:deterministic_terminal=slot_relation_limit" "6:status=slot_relation_limit" \
        "6:scale_evidence=terminal" "6:analysis_wall_ns=0") || return 1

    graph_terminal=$(siqs_256a_mutate_transcript "$baseline" \
        "1:status=graph_edge_limit" "2:status=graph_edge_limit" \
        "3:status=graph_edge_limit" "3:adapter_accepted_one_lp=7400" \
        "3:adapter_accepted_two_lp=9000" "3:adapter_rejected=223" \
        "3:adapter_invalid_two_large_prime_split=222" "3:graph_status=edge_limit" \
        "3:graph_input_edges=16400" "3:graph_vertices=0" "3:graph_edges=0" \
        "3:graph_components=0" "3:graph_cycles=0" "3:graph_cycle_incidences=0" \
        "3:graph_max_cycle_length=0" "3:cycles_with_accepted_2lp=0" \
        "3:cycles_without_accepted_2lp=0" "3:two_lp_edge_source_a_count=0" \
        "3:cycle_source_a_count=0" "3:cycle_provenance_digest_low=0" \
        "3:cycle_provenance_digest_high=0" "3:row_candidate_upper=0" \
        "4:status=graph_edge_limit" "4:attempted=false" "4:assembly_status=not_attempted" \
        "4:graph_edges=0" "4:graph_cycles=0" "4:valid_full=0" "4:full_sources=0" \
        "4:partial_sources=0" "4:valid_cycle_rows=0" "4:rows_before_dedup=0" \
        "4:pretrim_rows=0" "4:row_deficit=1701" "4:selected_rows=0" \
        "4:selected_full_rows=0" "4:selected_cycle_rows=0" "4:trimmed_rows=0" \
        "4:source_fingerprint_low=0" "4:source_fingerprint_high=0" \
        "4:pretrim_fingerprint_low=0" "4:pretrim_fingerprint_high=0" \
        "4:selected_fingerprint_low=0" "4:selected_fingerprint_high=0" \
        "5:deterministic_terminal=graph_edge_limit" "6:status=graph_edge_limit" \
        "6:scale_evidence=terminal") || return 1

    assembly_terminal=$(siqs_256a_mutate_transcript "$baseline" \
        "1:status=insufficient_rows" "2:status=insufficient_rows" \
        "3:status=insufficient_rows" "4:status=insufficient_rows" \
        "4:valid_cycle_rows=217" "4:rejected_cycle_rows=591" \
        "4:rows_before_dedup=1600" "4:pretrim_rows=1600" "4:row_deficit=101" \
        "4:selected_rows=1600" "4:selected_full_rows=1383" \
        "4:selected_cycle_rows=217" "4:trimmed_rows=0" \
        "5:deterministic_terminal=insufficient_rows" "6:status=insufficient_rows" \
        "6:scale_evidence=terminal") || return 1

    validate_siqs_256a_profile_output "$unavailable" 1 6 0 10 || return 1
    [[ "$SIQS_256A_PROFILE_IDENTITY" == "$baseline_identity" && SIQS_256A_SCALE_PASS -eq 0 ]] || return 1
    local positive_index positive
    typeset -a positive_names=(capture_terminal graph_terminal assembly_terminal)
    typeset -a positives=("$capture_terminal" "$graph_terminal" "$assembly_terminal")
    for (( positive_index = 1; positive_index <= ${#positives[@]}; ++positive_index )); do
        positive="${positives[$positive_index]}"
        if ! validate_siqs_256a_profile_output "$positive" 1 6 0 10 ||
           (( SIQS_256A_SCALE_PASS != 0 )); then
            log_fail "SIQS 256-A validator self-check 未接受合法 ${positive_names[$positive_index]}"
            return 1
        fi
    done
    SIQS_256A_VALIDATOR_BASELINE="$baseline"
    SIQS_256A_VALIDATOR_NOT_ATTEMPTED="$capture_terminal"
    return 0
}

run_siqs_256a_profile_process() {
    local workers="$1"
    local binary="${BUILD_DIR}/test_siqs_256a_scale_profile"
    local stdout_file stderr_file
    stdout_file=$(mktemp "${TMPDIR:-/tmp}/gnfs_siqs_256a_stdout.XXXXXX")
    stderr_file=$(mktemp "${TMPDIR:-/tmp}/gnfs_siqs_256a_stderr.XXXXXX")

    local start_ms exit_code=0
    start_ms=$(timer_start_ms)
    run_with_timeout 1800 /bin/sh -c \
        'stdout_path=$1; stderr_path=$2; shift 2; exec "$@" >"$stdout_path" 2>"$stderr_path"' \
        sh "$stdout_file" "$stderr_file" "$binary" --workers "$workers" || exit_code=$?
    local end_ms
    end_ms=$(timer_start_ms)
    local elapsed=$((end_ms - start_ms))
    TOTAL_TIME_MS=$((TOTAL_TIME_MS + elapsed))
    (( TOTAL_TESTS += 1 ))

    local line_count blank_count last_byte stdout stderr_bytes stderr_preview=""
    local stdout_file_bytes stdout_payload_bytes
    line_count=$(awk 'END { print NR + 0 }' "$stdout_file")
    blank_count=$(awk 'NF == 0 { count += 1 } END { print count + 0 }' "$stdout_file")
    last_byte=$(tail -c 1 "$stdout_file" | od -An -tuC | tr -d ' ')
    stdout_file_bytes=$(wc -c < "$stdout_file" | tr -d '[:space:]')
    stderr_bytes=$(wc -c < "$stderr_file" | tr -d '[:space:]')
    stdout=$(<"$stdout_file")
    stdout_payload_bytes=$(printf '%s' "$stdout" | wc -c | tr -d '[:space:]')
    if [[ "$stderr_bytes" != "0" ]]; then
        stderr_preview=$(LC_ALL=C tr '\000' '?' < "$stderr_file" | tail -10)
    fi
    SIQS_256A_PROFILE_STDERR="$stderr_preview"
    rm -f "$stdout_file" "$stderr_file"

    SIQS_256A_PROFILE_OUTPUT=""
    SIQS_256A_PROFILE_IDENTITY=""
    SIQS_256A_SCALE_PASS=0
    if (( exit_code == 124 )); then
        log_fail "SIQS 256-A workers=${workers} TIMEOUT after 1800s；拒绝任何部分 stdout"
        (( FAILED_TESTS += 1 ))
        return 1
    fi
    if (( exit_code != 0 )); then
        log_fail "SIQS 256-A workers=${workers} 退出码 ${exit_code}；拒绝任何部分 stdout"
        [[ -n "$SIQS_256A_PROFILE_STDERR" ]] && printf '%s\n' "$SIQS_256A_PROFILE_STDERR"
        (( FAILED_TESTS += 1 ))
        return 1
    fi
    if [[ "$stderr_bytes" != "0" ]]; then
        log_fail "SIQS 256-A 成功进程不得写入 stderr（${stderr_bytes} bytes）"
        [[ -n "$SIQS_256A_PROFILE_STDERR" ]] && printf '%s\n' "$SIQS_256A_PROFILE_STDERR"
        (( FAILED_TESTS += 1 ))
        return 1
    fi
    if [[ "$stdout_file_bytes" != "$(( stdout_payload_bytes + 1 ))" ]] ||
       ! validate_siqs_256a_profile_output \
        "$stdout" "$workers" "$line_count" "$blank_count" "$last_byte"; then
        log_fail "SIQS 256-A stdout 必须是无 NUL/尾随空行且严格有序、闭集、守恒的六行 V3 transcript"
        (( FAILED_TESTS += 1 ))
        return 1
    fi
    if (( ! SIQS_256A_SCALE_PASS )); then
        local summary="${${(@f)stdout}[6]}"
        log_fail "SIQS 256-A 规模证据未通过: status=$(measurement_record_field "$summary" status), rss_evidence=$(measurement_record_field "$summary" rss_evidence), scale_evidence=$(measurement_record_field "$summary" scale_evidence)"
        (( FAILED_TESTS += 1 ))
        return 1
    fi

    (( PASSED_TESTS += 1 ))
    REPORT_ENTRIES+=("{\"name\":\"test_siqs_256a_scale_profile_w${workers}\",\"status\":\"pass\",\"elapsed_ms\":${elapsed}}")
    return 0
}

# V4 reuses the first four V3 gate records. We mechanically downgrade those
# records, synthesize the V3 proof/summary tail, and invoke the single V3 gate
# validator above before validating the V4 proof-specific tail.
SIQS_256A_PROOF_OUTPUT=""
SIQS_256A_PROOF_IDENTITY=""
SIQS_256A_PROOF_STDERR=""
SIQS_256A_PROOF_GATE_SCALE_PASS=0
SIQS_256A_PROOF_PASS=0

validate_siqs_256a_proof_output() {
    local stdout="$1"
    local expected_workers="$2"
    local line_count="$3"
    local blank_count="$4"
    local last_byte="$5"
    local -a lines
    lines=("${(@f)stdout}")

    SIQS_256A_PROOF_OUTPUT=""
    SIQS_256A_PROOF_IDENTITY=""
    SIQS_256A_PROOF_GATE_SCALE_PASS=0
    SIQS_256A_PROOF_PASS=0
    [[ "$line_count" == "6" && "$blank_count" == "0" && "$last_byte" == "10" &&
       ${#lines[@]} -eq 6 ]] || return 1

    local gate_stdout="" gate_line v4_prefix v3_prefix gate_terminal
    local record_index
    for (( record_index = 1; record_index <= 4; ++record_index )); do
        case "$record_index" in
            1) v4_prefix="GNFS_SIQS_256A_CONFIG_V4"; v3_prefix="GNFS_SIQS_256A_CONFIG_V3" ;;
            2) v4_prefix="GNFS_SIQS_256A_CAPTURE_V4"; v3_prefix="GNFS_SIQS_256A_CAPTURE_V3" ;;
            3) v4_prefix="GNFS_SIQS_256A_GRAPH_V4"; v3_prefix="GNFS_SIQS_256A_GRAPH_V3" ;;
            4) v4_prefix="GNFS_SIQS_256A_ASSEMBLY_V4"; v3_prefix="GNFS_SIQS_256A_ASSEMBLY_V3" ;;
        esac
        gate_line="${lines[$record_index]}"
        [[ "${gate_line%% *}" == "$v4_prefix" ]] || return 1
        gate_line="$v3_prefix${gate_line#$v4_prefix}"
        gate_line="${gate_line/ schema_version=4 / schema_version=3 }"
        if [[ "$gate_line" == *" solver_attempted=true "* ]]; then
            gate_line="${gate_line/ solver_attempted=true / solver_attempted=false }"
        elif [[ "$gate_line" != *" solver_attempted=false "* ]]; then
            return 1
        fi
        gate_stdout+="$gate_line"$'\n'
    done
    gate_terminal=$(measurement_record_field "${lines[1]}" status) || return 1

    local proof_line="${lines[5]}" summary_line="${lines[6]}"
    local profile_id workers rss_scope rss_backend rss_evidence scale_evidence
    local current_rss peak_rss plan_wall capture_wall analysis_wall wall
    profile_id=$(measurement_record_field "$summary_line" profile_id) || return 1
    workers=$(measurement_record_field "$summary_line" workers) || return 1
    rss_scope=$(measurement_record_field "$summary_line" rss_scope) || return 1
    rss_backend=$(measurement_record_field "$summary_line" rss_backend) || return 1
    rss_evidence=$(measurement_record_field "$summary_line" rss_evidence) || return 1
    scale_evidence=$(measurement_record_field "$summary_line" scale_evidence) || return 1
    current_rss=$(measurement_record_field "$summary_line" final_current_rss_bytes) || return 1
    peak_rss=$(measurement_record_field "$summary_line" final_peak_rss_bytes) || return 1
    plan_wall=$(measurement_record_field "$summary_line" plan_wall_ns) || return 1
    capture_wall=$(measurement_record_field "$summary_line" capture_wall_ns) || return 1
    analysis_wall=$(measurement_record_field "$summary_line" analysis_wall_ns) || return 1
    wall=$(measurement_record_field "$summary_line" wall_ns) || return 1

    gate_stdout+="GNFS_SIQS_256A_PROOF_V3 schema_version=3 attempted=false status=not_attempted factor=none cofactor=none deterministic_terminal=${gate_terminal} solver_attempted=false promotion=false"$'\n'
    gate_stdout+="GNFS_SIQS_256A_SUMMARY_V3 schema_version=3 status=${gate_terminal} profile_id=${profile_id} stdout_records=6 config_records=1 capture_records=1 graph_records=1 assembly_records=1 proof_records=1 summary_records=1 workers=${workers} rss_scope=${rss_scope} rss_backend=${rss_backend} rss_evidence=${rss_evidence} scale_evidence=${scale_evidence} final_current_rss_bytes=${current_rss} final_peak_rss_bytes=${peak_rss} plan_wall_ns=${plan_wall} capture_wall_ns=${capture_wall} analysis_wall_ns=${analysis_wall} wall_ns=${wall} solver_attempted=false proof_status=not_attempted promotion=false"

    validate_siqs_256a_profile_output "$gate_stdout" "$expected_workers" 6 0 10 || return 1
    local gate_identity="$SIQS_256A_PROFILE_IDENTITY"
    local gate_scale_pass="$SIQS_256A_SCALE_PASS"
    local proof_identity
    if ! proof_identity=$("$GNFS_TEST_PYTHON" - "$expected_workers" 3<<<"$stdout" <<'PY'
import os
import re
import sys

def need(condition):
    if not condition:
        raise ValueError

try:
    expected_workers = int(sys.argv[1])
    need(expected_workers in (1, 2, 4))
    lines = os.fdopen(3, encoding="utf-8").read().splitlines()
    need(len(lines) == 6 and all(lines))
    gate_prefixes = [
        "GNFS_SIQS_256A_CONFIG_V4", "GNFS_SIQS_256A_CAPTURE_V4",
        "GNFS_SIQS_256A_GRAPH_V4", "GNFS_SIQS_256A_ASSEMBLY_V4",
    ]
    gate_records = []
    for prefix, line in zip(gate_prefixes, lines[:4]):
        tokens = line.split(" ")
        need(tokens[0] == prefix)
        record = dict(token.split("=", 1) for token in tokens[1:])
        need(record.get("schema_version") == "4" and record.get("solver_attempted") in {"true", "false"})
        gate_records.append(record)

    proof_prefix = "GNFS_SIQS_256A_PROOF_V4"
    proof_schema = """schema_version attempted status deterministic_terminal matrix_status
        dependency_status factor_status matrix_rows matrix_columns matrix_projected_dense_bytes
        solver_max_dependencies solver_elimination_workers solver_parallel_column_threshold
        solver_max_dense_matrix_bytes solver_max_dense_variable_count elimination_mode
        dependency_search dependency_combinations_attempted dependency_search_complete
        dependency_ordinal_base minimum_nullity dependencies_returned dependencies_examined
        dependencies_verified dependency_cap_reached dependency_digest_low dependency_digest_high
        first_failed_dependency factor_no_factor_count factor_found_count winning_dependency
        winning_dependency_size square_modulus gcd_target factor cofactor solver_wall_ns
        verify_extract_wall_ns solver_attempted promotion""".split()
    summary_prefix = "GNFS_SIQS_256A_SUMMARY_V4"
    summary_schema = """schema_version status profile_id stdout_records config_records capture_records
        graph_records assembly_records proof_records summary_records workers rss_scope rss_backend
        rss_evidence scale_evidence proof_evidence final_current_rss_bytes final_peak_rss_bytes
        plan_wall_ns capture_wall_ns analysis_wall_ns solver_wall_ns verify_extract_wall_ns wall_ns
        solver_attempted proof_status matrix_status dependency_status factor_status
        dependencies_returned dependencies_examined dependency_cap_reached factor cofactor
        promotion""".split()

    def parse_closed(line, prefix, schema):
        tokens = line.split(" ")
        need(tokens[0] == prefix and len(tokens) == len(schema) + 1)
        record = {}
        for expected_key, token in zip(schema, tokens[1:]):
            need("=" in token)
            key, value = token.split("=", 1)
            need(key == expected_key and value and key not in record)
            record[key] = value
        return record

    proof = parse_closed(lines[4], proof_prefix, proof_schema)
    summary = parse_closed(lines[5], summary_prefix, summary_schema)
    canonical = re.compile(r"0|[1-9][0-9]*\Z")
    positive = re.compile(r"[1-9][0-9]*\Z")
    proof_string = {
        "attempted", "status", "deterministic_terminal", "matrix_status", "dependency_status",
        "factor_status", "matrix_projected_dense_bytes", "elimination_mode", "dependency_search",
        "dependency_combinations_attempted", "dependency_search_complete", "dependency_cap_reached",
        "dependency_digest_low", "dependency_digest_high", "first_failed_dependency",
        "winning_dependency", "winning_dependency_size", "square_modulus", "gcd_target", "factor",
        "cofactor", "solver_attempted", "promotion",
    }
    optional_numeric = {
        "matrix_projected_dense_bytes": "not_attempted", "dependency_digest_low": "none",
        "dependency_digest_high": "none", "first_failed_dependency": "none",
        "winning_dependency": "none", "winning_dependency_size": "none", "factor": "none",
        "cofactor": "none",
    }
    for key, value in proof.items():
        if key in optional_numeric:
            need(value == optional_numeric[key] or canonical.fullmatch(value))
        elif key not in proof_string:
            need(canonical.fullmatch(value))
    summary_string = {
        "status", "profile_id", "rss_scope", "rss_backend", "rss_evidence", "scale_evidence",
        "proof_evidence", "final_current_rss_bytes", "final_peak_rss_bytes", "solver_attempted",
        "proof_status", "matrix_status", "dependency_status", "factor_status", "factor",
        "cofactor", "dependency_cap_reached", "promotion",
    }
    for key, value in summary.items():
        if key in {"final_current_rss_bytes", "final_peak_rss_bytes"}:
            need(value == "na" or positive.fullmatch(value))
        elif key in {"factor", "cofactor"}:
            need(value == "none" or positive.fullmatch(value))
        elif key not in summary_string:
            need(canonical.fullmatch(value))

    I = lambda record, key: int(record[key])
    attempted = proof["attempted"] == "true"
    need(proof["attempted"] in {"true", "false"} and proof["solver_attempted"] == proof["attempted"])
    need(all(record["solver_attempted"] == proof["attempted"] for record in gate_records))
    need(proof["schema_version"] == "4" and summary["schema_version"] == "4")
    need(proof["promotion"] == "false" and summary["promotion"] == "false")
    need(summary["profile_id"] == "siqs50_multi_a_256x32_scale_v3")
    need(proof["deterministic_terminal"] == gate_records[0]["status"])
    gate_terminal = proof["deterministic_terminal"]
    need(proof["solver_max_dependencies"] == "64" and
         proof["solver_elimination_workers"] == str(expected_workers) and
         proof["solver_parallel_column_threshold"] == "0" and
         proof["solver_max_dense_matrix_bytes"] == "345816" and
         proof["solver_max_dense_variable_count"] == "1701")
    expected_mode = "not_attempted" if not attempted else ("serial" if expected_workers == 1 else "persistent_parallel")
    need(proof["elimination_mode"] == expected_mode and
         proof["dependency_search"] == "first_free_column_basis_prefix" and
         proof["dependency_combinations_attempted"] == "false" and
         proof["dependency_ordinal_base"] == "0" and proof["square_modulus"] == "sieved_n" and
         proof["gcd_target"] == "n")

    matrix_failures = {"invalid_modulus", "invalid_factor_base", "invalid_options", "size_overflow",
                       "invalid_row", "row_identity_mismatch", "worker_failure",
                       "internal_invariant_failure", "resource_limit", "unsupported_backend"}
    dependency_failures = {"invalid_modulus", "invalid_factor_base", "invalid_row",
                           "row_identity_mismatch", "invalid_dependency", "exponent_overflow",
                           "dependency_not_square", "dependency_mismatch"}
    factor_failures = {"invalid_verified_dependency", "invalid_target", "target_not_divisor"}
    proof_terminals = {"not_attempted", "factor_found", "no_factor", "matrix_failure",
                       "dependency_failure", "factor_failure"}
    need(proof["status"] in proof_terminals)
    if not attempted:
        need(gate_terminal != "solver_ready" and proof["status"] == "not_attempted")
        need(proof["matrix_status"] == proof["dependency_status"] == proof["factor_status"] == "not_attempted")
        need(proof["matrix_rows"] == proof["matrix_columns"] == proof["minimum_nullity"] == "0")
        need(proof["matrix_projected_dense_bytes"] == "not_attempted" and
             proof["dependency_search_complete"] == "not_attempted")
        for key in ("dependencies_returned", "dependencies_examined", "dependencies_verified",
                    "factor_no_factor_count", "factor_found_count", "solver_wall_ns",
                    "verify_extract_wall_ns"):
            need(proof[key] == "0")
        need(proof["dependency_cap_reached"] == "false" and
             proof["dependency_digest_low"] == proof["dependency_digest_high"] == "none" and
             proof["first_failed_dependency"] == proof["winning_dependency"] ==
             proof["winning_dependency_size"] == proof["factor"] == proof["cofactor"] == "none")
    else:
        need(gate_terminal == "solver_ready" and proof["status"] != "not_attempted")
        need(proof["matrix_rows"] == "1701" and proof["matrix_columns"] == "1601" and
             proof["matrix_projected_dense_bytes"] == "345816" and proof["minimum_nullity"] == "100")
        need(I(proof, "solver_wall_ns") > 0)
        if proof["status"] == "matrix_failure":
            need(proof["matrix_status"] in matrix_failures and
                 proof["dependency_status"] == proof["factor_status"] == "not_attempted")
            need(proof["dependency_search_complete"] == "not_attempted" and
                 proof["dependencies_returned"] == proof["dependencies_examined"] ==
                 proof["dependencies_verified"] == "0" and proof["dependency_cap_reached"] == "false")
            need(proof["dependency_digest_low"] == proof["dependency_digest_high"] == "none" and
                 proof["first_failed_dependency"] == proof["winning_dependency"] ==
                 proof["winning_dependency_size"] == proof["factor"] == proof["cofactor"] == "none")
            need(proof["factor_no_factor_count"] == proof["factor_found_count"] == "0" and
                 proof["verify_extract_wall_ns"] == "0")
        else:
            need(proof["matrix_status"] == "valid" and proof["dependencies_returned"] == "64")
            returned = I(proof, "dependencies_returned")
            examined = I(proof, "dependencies_examined")
            verified = I(proof, "dependencies_verified")
            need(proof["dependency_cap_reached"] == "true" and
                 proof["dependency_search_complete"] == "false")
            need(proof["dependency_digest_low"] != "none" and proof["dependency_digest_high"] != "none" and
                 (proof["dependency_digest_low"], proof["dependency_digest_high"]) != ("0", "0"))
            need(0 <= verified <= examined <= returned and I(proof, "verify_extract_wall_ns") > 0)
            no_factor = I(proof, "factor_no_factor_count")
            found = I(proof, "factor_found_count")
            if proof["status"] == "no_factor":
                need(examined == verified == returned and no_factor == returned and found == 0)
                need(proof["first_failed_dependency"] == proof["winning_dependency"] ==
                     proof["winning_dependency_size"] == proof["factor"] == proof["cofactor"] == "none")
                if returned == 0:
                    need(proof["dependency_status"] == proof["factor_status"] == "not_attempted")
                else:
                    need(proof["dependency_status"] == "valid" and proof["factor_status"] == "no_factor")
            elif proof["status"] == "dependency_failure":
                need(proof["dependency_status"] in dependency_failures and examined == verified + 1 and
                     no_factor == verified and found == 0 and
                     proof["first_failed_dependency"] == str(verified))
                need(proof["factor_status"] == ("not_attempted" if verified == 0 else "no_factor") and
                     proof["winning_dependency"] == proof["winning_dependency_size"] ==
                     proof["factor"] == proof["cofactor"] == "none")
            elif proof["status"] == "factor_failure":
                need(proof["dependency_status"] == "valid" and proof["factor_status"] in factor_failures and
                     examined == verified and verified > 0 and no_factor == verified - 1 and found == 0 and
                     proof["first_failed_dependency"] == str(verified - 1))
                need(proof["winning_dependency"] == proof["winning_dependency_size"] ==
                     proof["factor"] == proof["cofactor"] == "none")
            else:
                need(proof["status"] == "factor_found" and proof["dependency_status"] == "valid" and
                     proof["factor_status"] == "factor_found" and examined == verified and verified > 0 and
                     no_factor == verified - 1 and found == 1 and proof["first_failed_dependency"] == "none")
                need(proof["winning_dependency"] == str(no_factor) and
                     proof["winning_dependency_size"] != "none" and I(proof, "winning_dependency_size") > 0)
                need(proof["factor"] != "none" and proof["cofactor"] != "none")
                modulus = 18027426610499408447671494571938206274555088868093
                factor, cofactor = int(proof["factor"]), int(proof["cofactor"])
                need(1 < factor < modulus and 1 < cofactor < modulus and factor * cofactor == modulus)

    expected_overall = gate_terminal if gate_terminal != "solver_ready" else proof["status"]
    expected_evidence = ("not_attempted" if not attempted else
                         "factor_found" if proof["status"] == "factor_found" else
                         "bounded_no_factor" if proof["status"] == "no_factor" else "fail")
    fixed_summary = {"stdout_records": "6", "config_records": "1", "capture_records": "1",
                     "graph_records": "1", "assembly_records": "1", "proof_records": "1",
                     "summary_records": "1", "rss_scope": "self_lifetime", "promotion": "false"}
    need(all(summary[key] == value for key, value in fixed_summary.items()))
    need(summary["status"] == expected_overall and summary["workers"] == str(expected_workers) and
         summary["solver_attempted"] == proof["attempted"] and
         summary["proof_evidence"] == expected_evidence and summary["proof_status"] == proof["status"])
    for key in ("matrix_status", "dependency_status", "factor_status", "dependencies_returned",
                "dependencies_examined", "dependency_cap_reached", "factor", "cofactor"):
        need(summary[key] == proof[key])
    need(summary["solver_wall_ns"] == proof["solver_wall_ns"] and
         summary["verify_extract_wall_ns"] == proof["verify_extract_wall_ns"] and
         summary["capture_wall_ns"] == gate_records[1]["capture_wall_ns"])
    need(I(summary, "analysis_wall_ns") >= I(summary, "solver_wall_ns") + I(summary, "verify_extract_wall_ns"))

    if proof["status"] == "factor_found":
        golden = {
            "matrix_rows": "1701", "matrix_columns": "1601",
            "matrix_projected_dense_bytes": "345816", "minimum_nullity": "100",
            "dependencies_returned": "64", "dependencies_examined": "4",
            "dependencies_verified": "4", "dependency_cap_reached": "true",
            "dependency_digest_low": "10254149926071895734",
            "dependency_digest_high": "17300745096364993287", "first_failed_dependency": "none",
            "factor_no_factor_count": "3", "factor_found_count": "1", "winning_dependency": "3",
            "winning_dependency_size": "703", "factor": "2041646378661656688438487",
            "cofactor": "8829847714527711737483339", "matrix_status": "valid",
            "dependency_status": "valid", "factor_status": "factor_found",
        }
        need(all(proof[key] == value for key, value in golden.items()))

    proof_dynamic = {"solver_elimination_workers", "elimination_mode", "solver_wall_ns",
                     "verify_extract_wall_ns"}
    summary_dynamic = {"workers", "rss_evidence", "scale_evidence", "final_current_rss_bytes",
                       "final_peak_rss_bytes", "plan_wall_ns", "capture_wall_ns", "analysis_wall_ns",
                       "solver_wall_ns", "verify_extract_wall_ns", "wall_ns"}
    proof_identity = " ".join([proof_prefix] +
        [f"{key}={proof[key]}" for key in proof_schema if key not in proof_dynamic])
    summary_identity = " ".join([summary_prefix] +
        [f"{key}={summary[key]}" for key in summary_schema if key not in summary_dynamic])
    print(proof_identity + "\n" + summary_identity)
except (ValueError, OSError, UnicodeError):
    sys.exit(1)
PY
    ); then
        return 1
    fi

    SIQS_256A_PROOF_OUTPUT="$stdout"
    SIQS_256A_PROOF_IDENTITY="$gate_identity"$'\n'"$proof_identity"
    SIQS_256A_PROOF_GATE_SCALE_PASS="$gate_scale_pass"
    if (( gate_scale_pass == 1 )) && [[ "$gate_terminal" == "solver_ready" &&
          "$(measurement_record_field "$proof_line" status)" == "factor_found" &&
          "$(measurement_record_field "$summary_line" status)" == "factor_found" &&
          "$(measurement_record_field "$summary_line" proof_evidence)" == "factor_found" ]]; then
        SIQS_256A_PROOF_PASS=1
    fi
}

siqs_256a_proof_validator_self_check() {
    siqs_256a_validator_self_check || return 1
    local -a gate_lines
    gate_lines=("${(@f)SIQS_256A_VALIDATOR_BASELINE}")
    (( ${#gate_lines[@]} == 6 )) || return 1

    local baseline="" gate_line record_index v3_prefix v4_prefix
    for (( record_index = 1; record_index <= 4; ++record_index )); do
        case "$record_index" in
            1) v3_prefix="GNFS_SIQS_256A_CONFIG_V3"; v4_prefix="GNFS_SIQS_256A_CONFIG_V4" ;;
            2) v3_prefix="GNFS_SIQS_256A_CAPTURE_V3"; v4_prefix="GNFS_SIQS_256A_CAPTURE_V4" ;;
            3) v3_prefix="GNFS_SIQS_256A_GRAPH_V3"; v4_prefix="GNFS_SIQS_256A_GRAPH_V4" ;;
            4) v3_prefix="GNFS_SIQS_256A_ASSEMBLY_V3"; v4_prefix="GNFS_SIQS_256A_ASSEMBLY_V4" ;;
        esac
        gate_line="$v4_prefix${gate_lines[$record_index]#$v3_prefix}"
        gate_line="${gate_line/ schema_version=3 / schema_version=4 }"
        gate_line="${gate_line/ solver_attempted=false / solver_attempted=true }"
        baseline+="$gate_line"$'\n'
    done
    baseline+="GNFS_SIQS_256A_PROOF_V4 schema_version=4 attempted=true status=factor_found deterministic_terminal=solver_ready matrix_status=valid dependency_status=valid factor_status=factor_found matrix_rows=1701 matrix_columns=1601 matrix_projected_dense_bytes=345816 solver_max_dependencies=64 solver_elimination_workers=1 solver_parallel_column_threshold=0 solver_max_dense_matrix_bytes=345816 solver_max_dense_variable_count=1701 elimination_mode=serial dependency_search=first_free_column_basis_prefix dependency_combinations_attempted=false dependency_search_complete=false dependency_ordinal_base=0 minimum_nullity=100 dependencies_returned=64 dependencies_examined=4 dependencies_verified=4 dependency_cap_reached=true dependency_digest_low=10254149926071895734 dependency_digest_high=17300745096364993287 first_failed_dependency=none factor_no_factor_count=3 factor_found_count=1 winning_dependency=3 winning_dependency_size=703 square_modulus=sieved_n gcd_target=n factor=2041646378661656688438487 cofactor=8829847714527711737483339 solver_wall_ns=5 verify_extract_wall_ns=6 solver_attempted=true promotion=false"$'\n'
    baseline+="GNFS_SIQS_256A_SUMMARY_V4 schema_version=4 status=factor_found profile_id=siqs50_multi_a_256x32_scale_v3 stdout_records=6 config_records=1 capture_records=1 graph_records=1 assembly_records=1 proof_records=1 summary_records=1 workers=1 rss_scope=self_lifetime rss_backend=darwin_getrusage rss_evidence=pass scale_evidence=pass proof_evidence=factor_found final_current_rss_bytes=1000 final_peak_rss_bytes=2000 plan_wall_ns=10 capture_wall_ns=20 analysis_wall_ns=30 solver_wall_ns=5 verify_extract_wall_ns=6 wall_ns=70 solver_attempted=true proof_status=factor_found matrix_status=valid dependency_status=valid factor_status=factor_found dependencies_returned=64 dependencies_examined=4 dependency_cap_reached=true factor=2041646378661656688438487 cofactor=8829847714527711737483339 promotion=false"

    validate_siqs_256a_proof_output "$baseline" 1 6 0 10 || return 1
    (( SIQS_256A_PROOF_GATE_SCALE_PASS == 1 && SIQS_256A_PROOF_PASS == 1 )) || return 1
    local baseline_identity="$SIQS_256A_PROOF_IDENTITY"
    local unknown missing order digest factor summary_mismatch
    unknown=$(printf '%s\n' "$baseline" | awk 'NR == 5 { $0 = $0 " unknown=1" } { print }')
    missing="${baseline/ matrix_status=valid/}"
    order="${baseline/ status=factor_found deterministic_terminal=solver_ready/ deterministic_terminal=solver_ready status=factor_found}"
    digest=$(siqs_256a_mutate_transcript "$baseline" \
        "5:dependency_digest_low=10254149926071895735") || return 1
    factor=$(siqs_256a_mutate_transcript "$baseline" "5:factor=2") || return 1
    summary_mismatch=$(siqs_256a_mutate_transcript "$baseline" "6:dependencies_examined=5") || return 1

    local mutation_index mutation
    typeset -a mutation_names=(unknown missing order digest factor summary_mismatch)
    typeset -a mutations=("$unknown" "$missing" "$order" "$digest" "$factor" "$summary_mismatch")
    for (( mutation_index = 1; mutation_index <= ${#mutations[@]}; ++mutation_index )); do
        mutation="${mutations[$mutation_index]}"
        if validate_siqs_256a_proof_output "$mutation" 1 6 0 10 2>/dev/null; then
            log_fail "SIQS 256-A proof validator self-check 未拒绝 ${mutation_names[$mutation_index]} mutation"
            return 1
        fi
    done

    local dynamic retained_backend
    dynamic=$(siqs_256a_mutate_transcript "$baseline" \
        "2:workers=4" "2:resolved_workers=4" "2:peak_workers=4" "2:capture_wall_ns=21" \
        "5:solver_elimination_workers=4" "5:elimination_mode=persistent_parallel" \
        "5:solver_wall_ns=7" "5:verify_extract_wall_ns=8" \
        "6:workers=4" "6:final_current_rss_bytes=1200" "6:final_peak_rss_bytes=2200" \
        "6:plan_wall_ns=11" "6:capture_wall_ns=21" "6:analysis_wall_ns=31" \
        "6:solver_wall_ns=7" "6:verify_extract_wall_ns=8" "6:wall_ns=80") || return 1
    validate_siqs_256a_proof_output "$dynamic" 4 6 0 10 || return 1
    [[ "$SIQS_256A_PROOF_IDENTITY" == "$baseline_identity" &&
       SIQS_256A_PROOF_GATE_SCALE_PASS -eq 1 && SIQS_256A_PROOF_PASS -eq 1 ]] || {
        log_fail "SIQS 256-A proof validator 动态字段 normalization 不闭合"
        return 1
    }
    retained_backend=$(siqs_256a_mutate_transcript "$dynamic" "6:rss_backend=linux_getrusage") || return 1
    validate_siqs_256a_proof_output "$retained_backend" 4 6 0 10 || return 1
    [[ "$SIQS_256A_PROOF_IDENTITY" != "$baseline_identity" ]] || {
        log_fail "SIQS 256-A proof validator 错误剔除了 rss_backend"
        return 1
    }

    local no_factor matrix_failure dependency_failure_zero
    local dependency_failure_after_verified factor_failure gate_not_attempted=""
    no_factor=$(siqs_256a_mutate_transcript "$baseline" \
        "5:status=no_factor" "5:factor_status=no_factor" \
        "5:dependencies_examined=64" "5:dependencies_verified=64" \
        "5:factor_no_factor_count=64" "5:factor_found_count=0" \
        "5:winning_dependency=none" "5:winning_dependency_size=none" \
        "5:factor=none" "5:cofactor=none" \
        "6:status=no_factor" "6:proof_evidence=bounded_no_factor" \
        "6:proof_status=no_factor" "6:factor_status=no_factor" \
        "6:dependencies_examined=64" "6:factor=none" "6:cofactor=none") || return 1
    matrix_failure=$(siqs_256a_mutate_transcript "$baseline" \
        "5:status=matrix_failure" "5:matrix_status=resource_limit" \
        "5:dependency_status=not_attempted" "5:factor_status=not_attempted" \
        "5:dependency_search_complete=not_attempted" "5:dependencies_returned=0" \
        "5:dependencies_examined=0" "5:dependencies_verified=0" \
        "5:dependency_cap_reached=false" "5:dependency_digest_low=none" \
        "5:dependency_digest_high=none" "5:factor_no_factor_count=0" \
        "5:factor_found_count=0" "5:winning_dependency=none" \
        "5:winning_dependency_size=none" "5:factor=none" "5:cofactor=none" \
        "5:verify_extract_wall_ns=0" \
        "6:status=matrix_failure" "6:proof_evidence=fail" \
        "6:verify_extract_wall_ns=0" "6:proof_status=matrix_failure" \
        "6:matrix_status=resource_limit" "6:dependency_status=not_attempted" \
        "6:factor_status=not_attempted" "6:dependencies_returned=0" \
        "6:dependencies_examined=0" "6:dependency_cap_reached=false" \
        "6:factor=none" "6:cofactor=none") || return 1
    dependency_failure_zero=$(siqs_256a_mutate_transcript "$baseline" \
        "5:status=dependency_failure" "5:dependency_status=invalid_dependency" \
        "5:factor_status=not_attempted" "5:dependencies_examined=1" \
        "5:dependencies_verified=0" "5:first_failed_dependency=0" \
        "5:factor_no_factor_count=0" "5:factor_found_count=0" \
        "5:winning_dependency=none" "5:winning_dependency_size=none" \
        "5:factor=none" "5:cofactor=none" \
        "6:status=dependency_failure" "6:proof_evidence=fail" \
        "6:proof_status=dependency_failure" "6:dependency_status=invalid_dependency" \
        "6:factor_status=not_attempted" "6:dependencies_examined=1" \
        "6:factor=none" "6:cofactor=none") || return 1
    dependency_failure_after_verified=$(siqs_256a_mutate_transcript "$baseline" \
        "5:status=dependency_failure" "5:dependency_status=dependency_not_square" \
        "5:factor_status=no_factor" "5:dependencies_verified=3" \
        "5:first_failed_dependency=3" "5:factor_found_count=0" \
        "5:winning_dependency=none" "5:winning_dependency_size=none" \
        "5:factor=none" "5:cofactor=none" \
        "6:status=dependency_failure" "6:proof_evidence=fail" \
        "6:proof_status=dependency_failure" \
        "6:dependency_status=dependency_not_square" "6:factor_status=no_factor" \
        "6:factor=none" "6:cofactor=none") || return 1
    factor_failure=$(siqs_256a_mutate_transcript "$baseline" \
        "5:status=factor_failure" "5:factor_status=invalid_target" \
        "5:first_failed_dependency=3" "5:factor_found_count=0" \
        "5:winning_dependency=none" "5:winning_dependency_size=none" \
        "5:factor=none" "5:cofactor=none" \
        "6:status=factor_failure" "6:proof_evidence=fail" \
        "6:proof_status=factor_failure" "6:factor_status=invalid_target" \
        "6:factor=none" "6:cofactor=none") || return 1

    local -a not_attempted_lines
    not_attempted_lines=("${(@f)SIQS_256A_VALIDATOR_NOT_ATTEMPTED}")
    (( ${#not_attempted_lines[@]} == 6 )) || return 1
    for (( record_index = 1; record_index <= 4; ++record_index )); do
        case "$record_index" in
            1) v3_prefix="GNFS_SIQS_256A_CONFIG_V3"; v4_prefix="GNFS_SIQS_256A_CONFIG_V4" ;;
            2) v3_prefix="GNFS_SIQS_256A_CAPTURE_V3"; v4_prefix="GNFS_SIQS_256A_CAPTURE_V4" ;;
            3) v3_prefix="GNFS_SIQS_256A_GRAPH_V3"; v4_prefix="GNFS_SIQS_256A_GRAPH_V4" ;;
            4) v3_prefix="GNFS_SIQS_256A_ASSEMBLY_V3"; v4_prefix="GNFS_SIQS_256A_ASSEMBLY_V4" ;;
        esac
        gate_line="$v4_prefix${not_attempted_lines[$record_index]#$v3_prefix}"
        gate_line="${gate_line/ schema_version=3 / schema_version=4 }"
        gate_not_attempted+="$gate_line"$'\n'
    done
    gate_not_attempted+="GNFS_SIQS_256A_PROOF_V4 schema_version=4 attempted=false status=not_attempted deterministic_terminal=slot_relation_limit matrix_status=not_attempted dependency_status=not_attempted factor_status=not_attempted matrix_rows=0 matrix_columns=0 matrix_projected_dense_bytes=not_attempted solver_max_dependencies=64 solver_elimination_workers=1 solver_parallel_column_threshold=0 solver_max_dense_matrix_bytes=345816 solver_max_dense_variable_count=1701 elimination_mode=not_attempted dependency_search=first_free_column_basis_prefix dependency_combinations_attempted=false dependency_search_complete=not_attempted dependency_ordinal_base=0 minimum_nullity=0 dependencies_returned=0 dependencies_examined=0 dependencies_verified=0 dependency_cap_reached=false dependency_digest_low=none dependency_digest_high=none first_failed_dependency=none factor_no_factor_count=0 factor_found_count=0 winning_dependency=none winning_dependency_size=none square_modulus=sieved_n gcd_target=n factor=none cofactor=none solver_wall_ns=0 verify_extract_wall_ns=0 solver_attempted=false promotion=false"$'\n'
    gate_not_attempted+="GNFS_SIQS_256A_SUMMARY_V4 schema_version=4 status=slot_relation_limit profile_id=siqs50_multi_a_256x32_scale_v3 stdout_records=6 config_records=1 capture_records=1 graph_records=1 assembly_records=1 proof_records=1 summary_records=1 workers=1 rss_scope=self_lifetime rss_backend=darwin_getrusage rss_evidence=pass scale_evidence=terminal proof_evidence=not_attempted final_current_rss_bytes=1000 final_peak_rss_bytes=2000 plan_wall_ns=10 capture_wall_ns=20 analysis_wall_ns=0 solver_wall_ns=0 verify_extract_wall_ns=0 wall_ns=70 solver_attempted=false proof_status=not_attempted matrix_status=not_attempted dependency_status=not_attempted factor_status=not_attempted dependencies_returned=0 dependencies_examined=0 dependency_cap_reached=false factor=none cofactor=none promotion=false"

    local typed_index typed expected_gate_pass
    typeset -a typed_names=(no_factor matrix_failure dependency_failure_zero \
        dependency_failure_after_verified factor_failure gate_not_attempted)
    typeset -a typed_outputs=("$no_factor" "$matrix_failure" "$dependency_failure_zero" \
        "$dependency_failure_after_verified" "$factor_failure" "$gate_not_attempted")
    typeset -a typed_gate_passes=(1 1 1 1 1 0)
    for (( typed_index = 1; typed_index <= ${#typed_outputs[@]}; ++typed_index )); do
        typed="${typed_outputs[$typed_index]}"
        expected_gate_pass="${typed_gate_passes[$typed_index]}"
        if ! validate_siqs_256a_proof_output "$typed" 1 6 0 10 ||
           (( SIQS_256A_PROOF_GATE_SCALE_PASS != expected_gate_pass ||
              SIQS_256A_PROOF_PASS != 0 )); then
            log_fail "SIQS 256-A proof validator self-check 未接受合法 ${typed_names[$typed_index]}"
            return 1
        fi
    done

    validate_siqs_256a_proof_output "$baseline" 1 6 0 10 || return 1
    [[ "$SIQS_256A_PROOF_IDENTITY" == "$baseline_identity" &&
       SIQS_256A_PROOF_GATE_SCALE_PASS -eq 1 && SIQS_256A_PROOF_PASS -eq 1 ]] || {
        log_fail "SIQS 256-A proof validator typed self-check 污染了 factor-found baseline"
        return 1
    }
    return 0
}

run_siqs_256a_proof_process() {
    local workers="$1"
    local binary="${BUILD_DIR}/test_siqs_256a_proof_shadow"
    local stdout_file stderr_file
    stdout_file=$(mktemp "${TMPDIR:-/tmp}/gnfs_siqs_256a_proof_stdout.XXXXXX")
    stderr_file=$(mktemp "${TMPDIR:-/tmp}/gnfs_siqs_256a_proof_stderr.XXXXXX")

    local start_ms exit_code=0
    start_ms=$(timer_start_ms)
    run_with_timeout 1800 /bin/sh -c \
        'stdout_path=$1; stderr_path=$2; shift 2; exec "$@" >"$stdout_path" 2>"$stderr_path"' \
        sh "$stdout_file" "$stderr_file" "$binary" --workers "$workers" || exit_code=$?
    local end_ms
    end_ms=$(timer_start_ms)
    local elapsed=$((end_ms - start_ms))
    TOTAL_TIME_MS=$((TOTAL_TIME_MS + elapsed))
    (( TOTAL_TESTS += 1 ))

    local line_count blank_count last_byte stdout stderr_bytes stderr_preview=""
    local stdout_file_bytes stdout_payload_bytes
    line_count=$(awk 'END { print NR + 0 }' "$stdout_file")
    blank_count=$(awk 'NF == 0 { count += 1 } END { print count + 0 }' "$stdout_file")
    last_byte=$(tail -c 1 "$stdout_file" | od -An -tuC | tr -d ' ')
    stdout_file_bytes=$(wc -c < "$stdout_file" | tr -d '[:space:]')
    stderr_bytes=$(wc -c < "$stderr_file" | tr -d '[:space:]')
    stdout=$(<"$stdout_file")
    stdout_payload_bytes=$(printf '%s' "$stdout" | wc -c | tr -d '[:space:]')
    if [[ "$stderr_bytes" != "0" ]]; then
        stderr_preview=$(LC_ALL=C tr '\000' '?' < "$stderr_file" | tail -10)
    fi
    SIQS_256A_PROOF_STDERR="$stderr_preview"
    rm -f "$stdout_file" "$stderr_file"

    SIQS_256A_PROOF_OUTPUT=""
    SIQS_256A_PROOF_IDENTITY=""
    SIQS_256A_PROOF_GATE_SCALE_PASS=0
    SIQS_256A_PROOF_PASS=0
    if (( exit_code == 124 )); then
        log_fail "SIQS 256-A proof workers=${workers} TIMEOUT after 1800s；拒绝任何部分 stdout"
        (( FAILED_TESTS += 1 ))
        return 1
    fi
    if (( exit_code != 0 )); then
        log_fail "SIQS 256-A proof workers=${workers} 退出码 ${exit_code}；拒绝任何部分 stdout"
        [[ -n "$SIQS_256A_PROOF_STDERR" ]] && printf '%s\n' "$SIQS_256A_PROOF_STDERR"
        (( FAILED_TESTS += 1 ))
        return 1
    fi
    if [[ "$stderr_bytes" != "0" ]]; then
        log_fail "SIQS 256-A proof 成功进程不得写入 stderr（${stderr_bytes} bytes）"
        [[ -n "$SIQS_256A_PROOF_STDERR" ]] && printf '%s\n' "$SIQS_256A_PROOF_STDERR"
        (( FAILED_TESTS += 1 ))
        return 1
    fi
    if [[ "$stdout_file_bytes" != "$(( stdout_payload_bytes + 1 ))" ]] ||
       ! validate_siqs_256a_proof_output \
        "$stdout" "$workers" "$line_count" "$blank_count" "$last_byte"; then
        log_fail "SIQS 256-A proof stdout 必须是无 NUL/尾随空行且严格闭集的六行 V4 transcript"
        (( FAILED_TESTS += 1 ))
        return 1
    fi
    if (( ! SIQS_256A_PROOF_GATE_SCALE_PASS || ! SIQS_256A_PROOF_PASS )); then
        local proof_line="${${(@f)stdout}[5]}" summary_line="${${(@f)stdout}[6]}"
        log_fail "SIQS 256-A proof 证据未通过: gate=$(measurement_record_field "$proof_line" deterministic_terminal), proof=$(measurement_record_field "$proof_line" status), evidence=$(measurement_record_field "$summary_line" proof_evidence)"
        (( FAILED_TESTS += 1 ))
        return 1
    fi

    (( PASSED_TESTS += 1 ))
    REPORT_ENTRIES+=("{\"name\":\"test_siqs_256a_proof_shadow_w${workers}\",\"status\":\"pass\",\"elapsed_ms\":${elapsed}}")
    return 0
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
    local needs_50d_contract=0
    if echo "$changed_files" |
        grep -Eq '^(CMakeLists\.txt|scripts/test\.sh|tests/test_structured_ooc_50d_probe\.cpp)$'; then
        needs_50d_contract=1
    fi

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
        if (( needs_50d_contract )); then
            log_info "变更仅命中 50 位探针合同，运行 fast 合同"
            do_50d_probe_contracts || true
        else
            log_info "变更文件未匹配到已知模块，运行冒烟测试"
            do_smoke
        fi
        return
    fi

    log_info "受影响模块: ${modules[*]}"
    echo ""

    do_module "${modules[@]}" || true

    if (( needs_50d_contract )); then
        do_50d_probe_contracts || true
    fi

    # Public API contract tests are deliberately slow and therefore live in
    # MODULE_SLOW_TESTS. Deep change validation must still execute them when
    # API headers, implementation, or test_api itself changed.
    if (( deep )) && [[ -n "${affected_modules[api]:-}" ]]; then
        log_section "API 深层合同回归"
        run_single_test test_api || true
    fi

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
                prev_ms=$("$GNFS_TEST_PYTHON" -c "
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
                curr_ms=$(echo "$curr_entry" | "$GNFS_TEST_PYTHON" -c "import json,sys; print(json.loads(sys.stdin.read())['elapsed_ms'])" 2>/dev/null || echo "0")

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
# 模式: 50 位探针合同（不运行真实 50 位流水线）
# ============================================================

do_50d_probe_contracts() {
    log_header "50 位探针 CLI / V2 schema 合同"
    (( TOTAL_TESTS += 1 ))
    local start_ms end_ms elapsed
    start_ms=$(timer_start_ms)
    if self_check_50d_probe_contracts; then
        end_ms=$(timer_start_ms)
        elapsed=$((end_ms - start_ms))
        (( TOTAL_TIME_MS += elapsed ))
        (( PASSED_TESTS += 1 ))
        REPORT_ENTRIES+=(
            "{\"name\":\"test_structured_ooc_50d_contract\",\"status\":\"pass\",\"elapsed_ms\":${elapsed},\"detail\":\"\"}"
        )
        log_success "CLI 负例、fixture emitter、schema synthetic 负例全部通过"
        return 0
    fi

    end_ms=$(timer_start_ms)
    elapsed=$((end_ms - start_ms))
    (( TOTAL_TIME_MS += elapsed ))
    (( FAILED_TESTS += 1 ))
    REPORT_ENTRIES+=(
        "{\"name\":\"test_structured_ooc_50d_contract\",\"status\":\"fail\",\"elapsed_ms\":${elapsed},\"detail\":\"contract failure\"}"
    )
    log_fail "50 位探针合同失败"
    return 1
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
    echo "  ${BULLET} ${CYAN}check-50d-contracts${RESET} — 不运行真实 50 位流水线的 CLI/schema 合同"
    echo "  ${BULLET} ${CYAN}compare-50d-bounded-routes${RESET} — 4-SQ legacy/structured fresh-process 对照"
    echo "  ${BULLET} ${CYAN}compare-50d-first-round${RESET} — 完整首轮 legacy/structured fresh-process 对照"
    echo "  ${BULLET} ${CYAN}test_candidate_batch_50d_sweep${RESET} — 固定 50 位 4-SQ candidate 调度扫测"
    echo "  ${BULLET} ${CYAN}test_squfof_bench${RESET}     — 固定 50 位 SQUFOF multiplier/吞吐基准"
    echo "  ${BULLET} ${CYAN}test_siqs_shadow_matrix_bench${RESET} — 固定 SIQS shadow matrix 求解/内核/准备基准"
    echo "  ${BULLET} ${CYAN}probe-siqs-live-sieve <band> <workers>${RESET} — Release-only 50/70/90 位有界现场探针"
    echo "  ${BULLET} ${CYAN}compare-siqs-live-sieve <band>${RESET} — 同一新构建的 1/2/4 独立进程身份对照"
    echo "  ${BULLET} ${CYAN}profile-siqs-cycle-density <workers>${RESET} — 固定 50 位 1/4/16/64 A cycle-density profile"
    echo "  ${BULLET} ${CYAN}compare-siqs-cycle-density${RESET} — 同一新构建的 1/2/4 profile 独立进程对照"
    echo "  ${BULLET} ${CYAN}profile-siqs-256a <workers>${RESET} — 固定 50 位 256x32 A scale profile"
    echo "  ${BULLET} ${CYAN}compare-siqs-256a${RESET} — 同一新构建的 1/2/4 scale 独立进程对照"
    echo "  ${BULLET} ${CYAN}profile-siqs-256a-proof <workers>${RESET} — 固定 256-A proof-shadow 因子证据"
    echo "  ${BULLET} ${CYAN}compare-siqs-256a-proof${RESET} — 同一新构建的 1/2/4 proof 身份对照"
    echo "  ${BULLET} ${CYAN}probe-siqs-shadow-observe <off|observe>${RESET} — Release-only production factor 双流探针"
    echo "  ${BULLET} ${CYAN}compare-siqs-shadow-observe${RESET} — 1 次 off + 3 次 observe fresh-process 对照"

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
        --retry)     RETRY_COUNT="$2"; RETRY_EXPLICIT=1; shift 2 ;;
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

    check-50d-contracts)
        if [[ ${#MODE_ARGS[@]} -ne 0 ]]; then
            log_fail "用法: $0 check-50d-contracts"
            exit 1
        fi
        do_build
        do_50d_probe_contracts || true
        show_summary
        (( FAILED_TESTS == 0 ))
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
        validate_50d_uint32_argument "$_probe_max_special_q" max_special_q 1 || exit 1
        validate_50d_batch_workers "$_probe_max_batch_workers" || exit 1
        validate_50d_local_threads "$_probe_max_local_sieve_threads" || exit 1
        if (( BUILD_TYPE_EXPLICIT )) && [[ "$BUILD_TYPE" != "Release" ]]; then
            log_fail "probe-50d-structured-ooc 只接受 Release 构建（传入: ${BUILD_TYPE}）"
            exit 1
        fi
        BUILD_TYPE="Release"
        if (( SKIP_BUILD )); then
            log_fail "probe-50d-structured-ooc 不接受 --no-build；资源证据必须由本次请求的构建生成"
            exit 1
        fi
        if (( RETRY_EXPLICIT )); then
            log_fail "probe-50d-structured-ooc 不接受 --retry；自动重试会破坏 fresh-process 证据"
            exit 1
        fi
        do_build
        if [[ ! -x "${BUILD_DIR}/test_structured_ooc_50d_probe" ]]; then
            log_fail "50 位探针二进制不存在: ${BUILD_DIR}/test_structured_ooc_50d_probe"
            exit 1
        fi
        if ! self_check_50d_probe_cli; then
            log_fail "50 位探针 CLI 边界自检失败"
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
        run_single_test test_structured_ooc_50d_probe --strategy structured --max-special-q \
            "$_probe_max_special_q" --max-special-q-batch-workers \
            "$_probe_max_batch_workers" "${_probe_thread_args[@]}" \
            --ooc-base "$_probe_base" || _probe_status=$?
        if (( _probe_status == 0 )); then
            local _probe_contract_valid=1
            if capture_single_measurement_record "GNFS_EXPERIMENT_V2 " "50 位探针"; then
                local _probe_first_round_complete
                if ! _probe_first_round_complete=$(measurement_record_field \
                    "$MEASUREMENT_RECORD" first_round_complete) ||
                   ! validate_50d_route_record "$MEASUREMENT_RECORD" structured \
                    "$_probe_first_round_complete" "50 位 structured 探针" \
                    "$_probe_max_special_q" "$_probe_max_batch_workers" \
                    "$_probe_max_local_sieve_threads"; then
                    (( FAILED_TESTS += 1 ))
                    _probe_contract_valid=0
                else
                    print -r -- "$MEASUREMENT_RECORD"
                fi
            else
                (( FAILED_TESTS += 1 ))
                _probe_contract_valid=0
            fi
            if (( _probe_contract_valid )); then
                if [[ -e "${_probe_base}.reldata" || -L "${_probe_base}.reldata" ||
                      -e "${_probe_base}.relidx" || -L "${_probe_base}.relidx" ]]; then
                    log_fail "探针成功后仍残留原始 OOC pair: ${_probe_base}"
                    (( FAILED_TESTS += 1 ))
                    _probe_contract_valid=0
                fi
            fi
            if (( _probe_contract_valid )) && rmdir "$_probe_dir"; then
                log_success "探针工件已完成生命周期清理"
            else
                if (( _probe_contract_valid )); then
                    log_fail "探针成功但临时目录非空，已保留: ${_probe_dir}"
                    (( FAILED_TESTS += 1 ))
                else
                    log_warn "探针记录或生命周期无效，保留诊断工件: ${_probe_dir}"
                fi
            fi
        else
            log_warn "探针失败，保留诊断工件: ${_probe_dir}"
        fi
        show_summary
        ;;

    compare-50d-bounded-routes|probe-50d-route-comparison)
        run_50d_route_comparison \
            bounded_50d_route_prefix_comparison false 4 900 "${MODE_ARGS[@]}"
        ;;

    compare-50d-first-round|probe-50d-first-round-comparison)
        run_50d_route_comparison \
            bounded_50d_first_round_comparison true 8192 7200 "${MODE_ARGS[@]}"
        ;;

    probe-50d-special-q-workers)
        if [[ ${#MODE_ARGS[@]} -gt 2 ]]; then
            log_fail "用法: $0 probe-50d-special-q-workers [max_special_q] [max_local_sieve_threads|auto]"
            exit 1
        fi
        local _comparison_max_special_q="${MODE_ARGS[1]:-4}"
        local _comparison_max_local_sieve_threads="${MODE_ARGS[2]:-auto}"
        validate_50d_uint32_argument "$_comparison_max_special_q" \
            "对照 max_special_q" 4 || exit 1
        validate_50d_local_threads "$_comparison_max_local_sieve_threads" || exit 1
        if (( BUILD_TYPE_EXPLICIT )) && [[ "$BUILD_TYPE" != "Release" ]]; then
            log_fail "probe-50d-special-q-workers 只接受 Release 构建（传入: ${BUILD_TYPE}）"
            exit 1
        fi
        BUILD_TYPE="Release"
        if (( SKIP_BUILD )); then
            log_fail "probe-50d-special-q-workers 不接受 --no-build；对照证据必须由本次请求的构建生成"
            exit 1
        fi
        if (( RETRY_EXPLICIT )); then
            log_fail "probe-50d-special-q-workers 不接受 --retry；自动重试会破坏每个 worker 的 fresh-process 证据"
            exit 1
        fi
        do_build
        if [[ ! -x "${BUILD_DIR}/test_structured_ooc_50d_probe" ]]; then
            log_fail "50 位探针二进制不存在: ${BUILD_DIR}/test_structured_ooc_50d_probe"
            exit 1
        fi
        if ! self_check_50d_probe_cli; then
            log_fail "50 位探针 CLI 边界自检失败"
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
        local _comparison_first_round_complete
        for _comparison_workers in 1 2 4; do
            _comparison_dir=$(mktemp -d \
                "${TMPDIR:-/tmp}/gnfs_structured_ooc_50d_w${_comparison_workers}.XXXXXX")
            _comparison_base="${_comparison_dir}/raw"
            log_info "workers=${_comparison_workers}; max_special_q=${_comparison_max_special_q}; max_local_sieve_threads=${_comparison_max_local_sieve_threads}; 临时目录=${_comparison_dir}"
            _comparison_run_status=0
            run_single_test test_structured_ooc_50d_probe --strategy structured --max-special-q \
                "$_comparison_max_special_q" --max-special-q-batch-workers \
                "$_comparison_workers" "${_comparison_thread_args[@]}" \
                --ooc-base "$_comparison_base" || \
                _comparison_run_status=$?
            if (( _comparison_run_status != 0 )); then
                _comparison_ready=0
                log_warn "workers=${_comparison_workers} 探针失败，保留诊断工件: ${_comparison_dir}"
                continue
            fi
            if ! capture_single_measurement_record "GNFS_EXPERIMENT_V2 " \
                "50 位 workers=${_comparison_workers} 探针"; then
                (( FAILED_TESTS += 1 ))
                _comparison_ready=0
                log_warn "workers=${_comparison_workers} 记录无效，保留诊断工件: ${_comparison_dir}"
                continue
            fi
            if ! _comparison_first_round_complete=$(measurement_record_field \
                "$MEASUREMENT_RECORD" first_round_complete) ||
               ! validate_50d_route_record "$MEASUREMENT_RECORD" structured \
                "$_comparison_first_round_complete" \
                "50 位 workers=${_comparison_workers} 探针" \
                "$_comparison_max_special_q" "$_comparison_workers" \
                "$_comparison_max_local_sieve_threads"; then
                (( FAILED_TESTS += 1 ))
                _comparison_ready=0
                log_warn "workers=${_comparison_workers} 契约无效，保留诊断工件: ${_comparison_dir}"
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
                n n_digits n_bits max_special_q
                route sieve_rounds_completed sieve_stop_reason
                special_q_processed special_q_batch_count special_q_batch_peak_size
                max_local_sieve_threads_requested local_sieve_thread_budget
                special_q_batch_peak_assigned_threads
                candidates_total candidate_batch_total_chunks candidate_batch_peak_chunks
                candidate_batch_peak_candidates candidate_batch_rss_sample_candidates
                rational_fb_columns algebraic_fb_columns base_factor_columns initial_raw_target
                first_round_complete resume_scope attempted_resume attempted_distributed
                sge_attempted solver_attempted sqrt_attempted factorization_attempted
                route_evidence strategy storage generation raw_rows raw_duplicates
                input_lp_columns input_lp_w1 input_lp_w2 input_lp_w3 input_lp_w4plus
                output_rows output_lp_columns structured_commits
                structured_emitted_rows structured_stop incidence_shards
                incidence_requested_workers incidence_peak_workers raw_digest_low raw_digest_high
                output_digest_low output_digest_high matrix_rows matrix_cols matrix_nonzeros
                matrix_signed_delta matrix_row_mapping_identity
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
            print -r -- "GNFS_EXPERIMENT_COMPARISON_V2 status=pass scope=bounded_50d_special_q_batch_workers max_special_q=${_comparison_max_special_q} max_local_sieve_threads=${_comparison_max_local_sieve_threads} workers=1,2,4 identity_fields=${#_comparison_fields[@]} timing_scope=fresh_process_per_worker rss_scope=fresh_process_per_worker timing_asserted=false rss_asserted=false promotion=false"
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

    probe-siqs-live-sieve)
        if [[ ${#MODE_ARGS[@]} -ne 2 ]]; then
            log_fail "用法: $0 probe-siqs-live-sieve <50|70|90> <1|2|4>"
            exit 1
        fi
        local _live_band="${MODE_ARGS[1]}"
        local _live_workers="${MODE_ARGS[2]}"
        case "$_live_band" in
            50|70|90) ;;
            *) log_fail "band 必须是 50、70 或 90"; exit 1 ;;
        esac
        case "$_live_workers" in
            1|2|4) ;;
            *) log_fail "workers 必须是 1、2 或 4"; exit 1 ;;
        esac
        if (( BUILD_TYPE_EXPLICIT )) && [[ "$BUILD_TYPE" != "Release" ]]; then
            log_fail "probe-siqs-live-sieve 只接受 Release 构建 (传入: ${BUILD_TYPE})"
            exit 1
        fi
        BUILD_TYPE="Release"
        if (( SKIP_BUILD )); then
            log_fail "probe-siqs-live-sieve 不接受 --no-build；证据必须来自本次请求的新构建"
            exit 1
        fi
        if (( RETRY_EXPLICIT )); then
            log_fail "probe-siqs-live-sieve 不接受 --retry；自动重试会破坏独立进程证据"
            exit 1
        fi

        do_build
        if [[ ! -x "${BUILD_DIR}/test_siqs_live_sieve_probe" ]]; then
            log_fail "SIQS live-sieve probe 二进制不存在: ${BUILD_DIR}/test_siqs_live_sieve_probe"
            exit 1
        fi
        local _live_timeout
        _live_timeout=$(siqs_live_probe_timeout "$_live_band")
        (( TIMEOUT_EXPLICIT )) && _live_timeout="$TIMEOUT"
        log_header "SIQS live-sieve ${_live_band} 位有界探针"
        log_info "Release/NDEBUG；fresh process；workers=${_live_workers}；timeout=${_live_timeout}s"
        if run_siqs_live_probe_process "$_live_band" "$_live_workers" "$_live_timeout"; then
            printf '%s\n' "$SIQS_LIVE_CAPTURE_RECORD"
            log_success "唯一 V1 记录、Release 构建合同和 band/worker 字段均有效"
        fi
        show_summary
        ;;

    compare-siqs-live-sieve)
        if [[ ${#MODE_ARGS[@]} -ne 1 ]]; then
            log_fail "用法: $0 compare-siqs-live-sieve <50|70|90>"
            exit 1
        fi
        local _compare_band="${MODE_ARGS[1]}"
        case "$_compare_band" in
            50|70|90) ;;
            *) log_fail "band 必须是 50、70 或 90"; exit 1 ;;
        esac
        if (( BUILD_TYPE_EXPLICIT )) && [[ "$BUILD_TYPE" != "Release" ]]; then
            log_fail "compare-siqs-live-sieve 只接受 Release 构建 (传入: ${BUILD_TYPE})"
            exit 1
        fi
        BUILD_TYPE="Release"
        if (( SKIP_BUILD )); then
            log_fail "compare-siqs-live-sieve 不接受 --no-build；三组证据必须共享本次新构建"
            exit 1
        fi
        if (( RETRY_EXPLICIT )); then
            log_fail "compare-siqs-live-sieve 不接受 --retry；自动重试会破坏独立进程证据"
            exit 1
        fi

        do_build
        if [[ ! -x "${BUILD_DIR}/test_siqs_live_sieve_probe" ]]; then
            log_fail "SIQS live-sieve probe 二进制不存在: ${BUILD_DIR}/test_siqs_live_sieve_probe"
            exit 1
        fi
        local _compare_timeout
        _compare_timeout=$(siqs_live_probe_timeout "$_compare_band")
        (( TIMEOUT_EXPLICIT )) && _compare_timeout="$TIMEOUT"
        log_header "SIQS live-sieve ${_compare_band} 位 1/2/4 worker 对照"
        log_info "单次 Release 构建；3 个 fresh processes；每进程 timeout=${_compare_timeout}s"

        local _compare_ok=1
        local _compare_worker
        typeset -A _compare_identities
        for _compare_worker in 1 2 4; do
            if run_siqs_live_probe_process \
                "$_compare_band" "$_compare_worker" "$_compare_timeout"; then
                _compare_identities[$_compare_worker]="$SIQS_LIVE_CAPTURE_IDENTITY"
                printf '%s\n' "$SIQS_LIVE_CAPTURE_RECORD"
            else
                _compare_ok=0
                (( FAIL_FAST )) && break
            fi
        done

        if (( _compare_ok )); then
            (( TOTAL_TESTS += 1 ))
            if [[ "${_compare_identities[1]}" == "${_compare_identities[2]}" &&
                  "${_compare_identities[1]}" == "${_compare_identities[4]}" ]]; then
                (( PASSED_TESTS += 1 ))
                REPORT_ENTRIES+=("{\"name\":\"compare_siqs_live_sieve_${_compare_band}\",\"status\":\"pass\",\"elapsed_ms\":0}")
                log_success "除已验证的 worker 执行字段、wall_ns 和 peak_rss_bytes 外，全部字段一致"
            else
                (( FAILED_TESTS += 1 ))
                log_fail "1/2/4 worker 的确定性身份字段不一致"
            fi
        fi
        show_summary
        ;;

    profile-siqs-cycle-density)
        if [[ ${#MODE_ARGS[@]} -ne 1 ]]; then
            log_fail "用法: $0 profile-siqs-cycle-density <1|2|4>"
            exit 1
        fi
        local _cycle_workers="${MODE_ARGS[1]}"
        case "$_cycle_workers" in
            1|2|4) ;;
            *) log_fail "workers 必须是 1、2 或 4"; exit 1 ;;
        esac
        if (( BUILD_TYPE_EXPLICIT )) && [[ "$BUILD_TYPE" != "Release" ]]; then
            log_fail "profile-siqs-cycle-density 只接受 Release 构建 (传入: ${BUILD_TYPE})"
            exit 1
        fi
        BUILD_TYPE="Release"
        if (( SKIP_BUILD )); then
            log_fail "profile-siqs-cycle-density 不接受 --no-build；证据必须来自本次请求的新构建"
            exit 1
        fi
        if (( RETRY_EXPLICIT )); then
            log_fail "profile-siqs-cycle-density 不接受 --retry；自动重试会破坏独立进程证据"
            exit 1
        fi

        do_build
        if [[ ! -x "${BUILD_DIR}/test_siqs_multi_a_cycle_profile" ]]; then
            log_fail "SIQS cycle-density profile 二进制不存在: ${BUILD_DIR}/test_siqs_multi_a_cycle_profile"
            exit 1
        fi
        local _cycle_timeout=900
        (( TIMEOUT_EXPLICIT )) && _cycle_timeout="$TIMEOUT"
        log_header "SIQS 固定 50 位 multi-A cycle-density profile"
        log_info "Release/NDEBUG；fresh process；workers=${_cycle_workers}；A=1/4/16/64；timeout=${_cycle_timeout}s"
        if run_siqs_cycle_profile_process "$_cycle_workers" "$_cycle_timeout"; then
            printf '%s\n' "$SIQS_CYCLE_PROFILE_OUTPUT"
            log_success "严格六行 V2 profile、四级 prefix 和 worker 合同均有效"
        fi
        show_summary
        ;;

    compare-siqs-cycle-density)
        if [[ ${#MODE_ARGS[@]} -ne 0 ]]; then
            log_fail "用法: $0 compare-siqs-cycle-density"
            exit 1
        fi
        if (( BUILD_TYPE_EXPLICIT )) && [[ "$BUILD_TYPE" != "Release" ]]; then
            log_fail "compare-siqs-cycle-density 只接受 Release 构建 (传入: ${BUILD_TYPE})"
            exit 1
        fi
        BUILD_TYPE="Release"
        if (( SKIP_BUILD )); then
            log_fail "compare-siqs-cycle-density 不接受 --no-build；三组证据必须共享本次新构建"
            exit 1
        fi
        if (( RETRY_EXPLICIT )); then
            log_fail "compare-siqs-cycle-density 不接受 --retry；自动重试会破坏独立进程证据"
            exit 1
        fi

        do_build
        if [[ ! -x "${BUILD_DIR}/test_siqs_multi_a_cycle_profile" ]]; then
            log_fail "SIQS cycle-density profile 二进制不存在: ${BUILD_DIR}/test_siqs_multi_a_cycle_profile"
            exit 1
        fi
        local _cycle_compare_timeout=900
        (( TIMEOUT_EXPLICIT )) && _cycle_compare_timeout="$TIMEOUT"
        log_header "SIQS 固定 50 位 multi-A 1/2/4 worker 对照"
        log_info "单次 Release 构建；3 个 fresh processes；每进程 timeout=${_cycle_compare_timeout}s"

        local _cycle_compare_ok=1 _cycle_compare_worker
        typeset -A _cycle_identities
        for _cycle_compare_worker in 1 2 4; do
            if run_siqs_cycle_profile_process \
                "$_cycle_compare_worker" "$_cycle_compare_timeout"; then
                _cycle_identities[$_cycle_compare_worker]="$SIQS_CYCLE_PROFILE_IDENTITY"
                printf '%s\n' "$SIQS_CYCLE_PROFILE_OUTPUT"
            else
                _cycle_compare_ok=0
                (( FAIL_FAST )) && break
            fi
        done

        if (( _cycle_compare_ok )); then
            (( TOTAL_TESTS += 1 ))
            if [[ "${_cycle_identities[1]}" == "${_cycle_identities[2]}" &&
                  "${_cycle_identities[1]}" == "${_cycle_identities[4]}" ]]; then
                (( PASSED_TESTS += 1 ))
                REPORT_ENTRIES+=("{\"name\":\"compare_siqs_cycle_density_50\",\"status\":\"pass\",\"elapsed_ms\":0}")
                log_success "除已验证的 worker、wall-time 和 RSS 信息字段外，全部 V2 字段一致"
            else
                (( FAILED_TESTS += 1 ))
                log_fail "1/2/4 worker 的 cycle-density 确定性身份字段不一致"
            fi
        fi
        show_summary
        ;;

    profile-siqs-256a)
        if [[ ${#MODE_ARGS[@]} -ne 1 ]]; then
            log_fail "用法: $0 profile-siqs-256a <1|2|4>"
            exit 1
        fi
        local _scale_workers="${MODE_ARGS[1]}"
        case "$_scale_workers" in
            1|2|4) ;;
            *) log_fail "workers 必须是 1、2 或 4"; exit 1 ;;
        esac
        if (( BUILD_TYPE_EXPLICIT )) && [[ "$BUILD_TYPE" != "Release" ]]; then
            log_fail "profile-siqs-256a 只接受 Release 构建 (传入: ${BUILD_TYPE})"
            exit 1
        fi
        BUILD_TYPE="Release"
        if (( SKIP_BUILD )); then
            log_fail "profile-siqs-256a 不接受 --no-build；证据必须来自本次请求的新构建"
            exit 1
        fi
        if (( RETRY_EXPLICIT )); then
            log_fail "profile-siqs-256a 不接受 --retry；自动重试会破坏独立进程证据"
            exit 1
        fi
        if (( TIMEOUT_EXPLICIT )); then
            log_fail "profile-siqs-256a 的 timeout 固定为 1800s，不接受 --timeout"
            exit 1
        fi
        if ! siqs_256a_validator_self_check; then
            log_fail "SIQS 256-A validator self-check 失败；拒绝启动构建"
            exit 1
        fi

        do_build
        if [[ ! -x "${BUILD_DIR}/test_siqs_256a_scale_profile" ]]; then
            log_fail "SIQS 256-A scale profile 二进制不存在: ${BUILD_DIR}/test_siqs_256a_scale_profile"
            exit 1
        fi
        log_header "SIQS 固定 50 位 256x32 A scale profile"
        log_info "Release/NDEBUG；fresh process；workers=${_scale_workers}；timeout=1800s"
        if run_siqs_256a_profile_process "$_scale_workers"; then
            printf '%s\n' "$SIQS_256A_PROFILE_OUTPUT"
            log_success "严格六行 V3 transcript、全守恒与 solver_ready/RSS scale 证据均有效"
        fi
        show_summary
        ;;

    compare-siqs-256a)
        if [[ ${#MODE_ARGS[@]} -ne 0 ]]; then
            log_fail "用法: $0 compare-siqs-256a"
            exit 1
        fi
        if (( BUILD_TYPE_EXPLICIT )) && [[ "$BUILD_TYPE" != "Release" ]]; then
            log_fail "compare-siqs-256a 只接受 Release 构建 (传入: ${BUILD_TYPE})"
            exit 1
        fi
        BUILD_TYPE="Release"
        if (( SKIP_BUILD )); then
            log_fail "compare-siqs-256a 不接受 --no-build；三组证据必须共享本次新构建"
            exit 1
        fi
        if (( RETRY_EXPLICIT )); then
            log_fail "compare-siqs-256a 不接受 --retry；自动重试会破坏独立进程证据"
            exit 1
        fi
        if (( TIMEOUT_EXPLICIT )); then
            log_fail "compare-siqs-256a 的每进程 timeout 固定为 1800s，不接受 --timeout"
            exit 1
        fi
        if ! siqs_256a_validator_self_check; then
            log_fail "SIQS 256-A validator self-check 失败；拒绝启动构建"
            exit 1
        fi

        do_build
        if [[ ! -x "${BUILD_DIR}/test_siqs_256a_scale_profile" ]]; then
            log_fail "SIQS 256-A scale profile 二进制不存在: ${BUILD_DIR}/test_siqs_256a_scale_profile"
            exit 1
        fi
        log_header "SIQS 固定 50 位 256x32 A 1/2/4 worker 对照"
        log_info "单次 Release 构建；3 个 fresh processes；每进程 timeout=1800s"

        local _scale_compare_ok=1 _scale_compare_worker
        typeset -A _scale_identities
        for _scale_compare_worker in 1 2 4; do
            if run_siqs_256a_profile_process "$_scale_compare_worker"; then
                _scale_identities[$_scale_compare_worker]="$SIQS_256A_PROFILE_IDENTITY"
                printf '%s\n' "$SIQS_256A_PROFILE_OUTPUT"
            else
                _scale_compare_ok=0
                (( FAIL_FAST )) && break
            fi
        done

        if (( _scale_compare_ok )); then
            (( TOTAL_TESTS += 1 ))
            if [[ "${_scale_identities[1]}" == "${_scale_identities[2]}" &&
                  "${_scale_identities[1]}" == "${_scale_identities[4]}" ]]; then
                (( PASSED_TESTS += 1 ))
                REPORT_ENTRIES+=("{\"name\":\"compare_siqs_256a_50\",\"status\":\"pass\",\"elapsed_ms\":0}")
                log_success "三次均为 scale pass；仅 worker、wall、RSS 和派生 scale_evidence 字段被规范化"
            else
                (( FAILED_TESTS += 1 ))
                log_fail "1/2/4 worker 的 256-A 确定性身份字段不一致"
            fi
        fi
        show_summary
        ;;

    profile-siqs-256a-proof)
        if [[ ${#MODE_ARGS[@]} -ne 1 ]]; then
            log_fail "用法: $0 profile-siqs-256a-proof <1|2|4>"
            exit 1
        fi
        local _proof_workers="${MODE_ARGS[1]}"
        case "$_proof_workers" in
            1|2|4) ;;
            *) log_fail "workers 必须是 1、2 或 4"; exit 1 ;;
        esac
        if (( BUILD_TYPE_EXPLICIT )) && [[ "$BUILD_TYPE" != "Release" ]]; then
            log_fail "profile-siqs-256a-proof 只接受 Release 构建 (传入: ${BUILD_TYPE})"
            exit 1
        fi
        BUILD_TYPE="Release"
        if (( SKIP_BUILD )); then
            log_fail "profile-siqs-256a-proof 不接受 --no-build；证据必须来自本次请求的新构建"
            exit 1
        fi
        if (( RETRY_EXPLICIT )); then
            log_fail "profile-siqs-256a-proof 不接受 --retry；自动重试会破坏独立进程证据"
            exit 1
        fi
        if (( TIMEOUT_EXPLICIT )); then
            log_fail "profile-siqs-256a-proof 的 timeout 固定为 1800s，不接受 --timeout"
            exit 1
        fi
        if ! siqs_256a_proof_validator_self_check; then
            log_fail "SIQS 256-A proof validator self-check 失败；拒绝启动构建"
            exit 1
        fi

        do_build
        if [[ ! -x "${BUILD_DIR}/test_siqs_256a_proof_shadow" ]]; then
            log_fail "SIQS 256-A proof profile 二进制不存在: ${BUILD_DIR}/test_siqs_256a_proof_shadow"
            exit 1
        fi
        log_header "SIQS 固定 256-A proof-shadow profile"
        log_info "Release/NDEBUG；fresh process；workers=${_proof_workers}；timeout=1800s"
        if run_siqs_256a_proof_process "$_proof_workers"; then
            printf '%s\n' "$SIQS_256A_PROOF_OUTPUT"
            log_success "V3 gate scale pass 且 V4 proof 返回冻结 factor_found 证据"
        fi
        show_summary
        ;;

    compare-siqs-256a-proof)
        if [[ ${#MODE_ARGS[@]} -ne 0 ]]; then
            log_fail "用法: $0 compare-siqs-256a-proof"
            exit 1
        fi
        if (( BUILD_TYPE_EXPLICIT )) && [[ "$BUILD_TYPE" != "Release" ]]; then
            log_fail "compare-siqs-256a-proof 只接受 Release 构建 (传入: ${BUILD_TYPE})"
            exit 1
        fi
        BUILD_TYPE="Release"
        if (( SKIP_BUILD )); then
            log_fail "compare-siqs-256a-proof 不接受 --no-build；三组证据必须共享本次新构建"
            exit 1
        fi
        if (( RETRY_EXPLICIT )); then
            log_fail "compare-siqs-256a-proof 不接受 --retry；自动重试会破坏独立进程证据"
            exit 1
        fi
        if (( TIMEOUT_EXPLICIT )); then
            log_fail "compare-siqs-256a-proof 的每进程 timeout 固定为 1800s，不接受 --timeout"
            exit 1
        fi
        if ! siqs_256a_proof_validator_self_check; then
            log_fail "SIQS 256-A proof validator self-check 失败；拒绝启动构建"
            exit 1
        fi

        do_build
        if [[ ! -x "${BUILD_DIR}/test_siqs_256a_proof_shadow" ]]; then
            log_fail "SIQS 256-A proof profile 二进制不存在: ${BUILD_DIR}/test_siqs_256a_proof_shadow"
            exit 1
        fi
        log_header "SIQS 固定 256-A proof-shadow 1/2/4 worker 对照"
        log_info "单次 Release 构建；3 个 fresh processes；每进程 timeout=1800s"

        local _proof_compare_ok=1 _proof_compare_worker
        typeset -A _proof_identities
        for _proof_compare_worker in 1 2 4; do
            if run_siqs_256a_proof_process "$_proof_compare_worker"; then
                _proof_identities[$_proof_compare_worker]="$SIQS_256A_PROOF_IDENTITY"
                printf '%s\n' "$SIQS_256A_PROOF_OUTPUT"
            else
                _proof_compare_ok=0
                (( FAIL_FAST )) && break
            fi
        done

        if (( _proof_compare_ok )); then
            (( TOTAL_TESTS += 1 ))
            if [[ "${_proof_identities[1]}" == "${_proof_identities[2]}" &&
                  "${_proof_identities[1]}" == "${_proof_identities[4]}" ]]; then
                (( PASSED_TESTS += 1 ))
                REPORT_ENTRIES+=("{\"name\":\"compare_siqs_256a_proof_50\",\"status\":\"pass\",\"elapsed_ms\":0}")
                log_success "三次 gate/proof 均通过；仅白名单 worker、wall 与 RSS 观测字段被规范化"
            else
                (( FAILED_TESTS += 1 ))
                log_fail "1/2/4 worker 的 256-A proof 确定性身份字段不一致"
            fi
        fi
        show_summary
        ;;

    probe-siqs-shadow-observe)
        if [[ ${#MODE_ARGS[@]} -ne 1 ]]; then
            log_fail "用法: $0 probe-siqs-shadow-observe <off|observe>"
            exit 1
        fi
        local _shadow_observe_mode="${MODE_ARGS[1]}"
        case "$_shadow_observe_mode" in
            off|observe) ;;
            *) log_fail "mode 必须是 off 或 observe"; exit 1 ;;
        esac
        if (( BUILD_TYPE_EXPLICIT )) && [[ "$BUILD_TYPE" != "Release" ]]; then
            log_fail "probe-siqs-shadow-observe 只接受 Release 构建 (传入: ${BUILD_TYPE})"
            exit 1
        fi
        BUILD_TYPE="Release"
        if (( SKIP_BUILD )); then
            log_fail "probe-siqs-shadow-observe 不接受 --no-build；证据必须来自本次请求的新构建"
            exit 1
        fi
        if (( RETRY_EXPLICIT )); then
            log_fail "probe-siqs-shadow-observe 不接受 --retry；自动重试会破坏 fresh-process 证据"
            exit 1
        fi
        local _shadow_observe_timeout=60
        (( TIMEOUT_EXPLICIT )) && _shadow_observe_timeout="$TIMEOUT"
        if [[ ! "$_shadow_observe_timeout" =~ '^[1-9][0-9]*$' ]]; then
            log_fail "probe-siqs-shadow-observe 的 timeout 必须是正整数秒"
            exit 1
        fi
        if ! siqs_shadow_observe_validator_self_check; then
            log_fail "SIQS shadow observe validator self-check 失败；拒绝启动构建"
            exit 1
        fi

        do_build
        if [[ ! -x "${BUILD_DIR}/test_siqs_shadow_proof_observe_probe" ]]; then
            log_fail "SIQS shadow observe probe 二进制不存在: ${BUILD_DIR}/test_siqs_shadow_proof_observe_probe"
            exit 1
        fi
        local _shadow_observe_sample=0
        [[ "$_shadow_observe_mode" == "observe" ]] && _shadow_observe_sample=1
        log_header "SIQS production shadow-proof ${_shadow_observe_mode} 探针"
        log_info "Release/NDEBUG；fresh process；严格双流；timeout=${_shadow_observe_timeout}s"
        if run_siqs_shadow_observe_process \
            "$_shadow_observe_mode" "$_shadow_observe_sample" "$_shadow_observe_timeout"; then
            printf '%s\n' "$SIQS_SHADOW_OBSERVE_PROBE_RECORD"
            if [[ "$_shadow_observe_mode" == "observe" ]]; then
                printf '%s\n' "$SIQS_SHADOW_OBSERVE_TELEMETRY_RECORD"
            fi
            log_success "probe 与 telemetry 的冻结 schema、50 位证据和 RSS 守恒均有效"
        fi
        show_summary
        ;;

    compare-siqs-shadow-observe)
        if [[ ${#MODE_ARGS[@]} -ne 0 ]]; then
            log_fail "用法: $0 compare-siqs-shadow-observe"
            exit 1
        fi
        if (( BUILD_TYPE_EXPLICIT )) && [[ "$BUILD_TYPE" != "Release" ]]; then
            log_fail "compare-siqs-shadow-observe 只接受 Release 构建 (传入: ${BUILD_TYPE})"
            exit 1
        fi
        BUILD_TYPE="Release"
        if (( SKIP_BUILD )); then
            log_fail "compare-siqs-shadow-observe 不接受 --no-build；四组证据必须共享本次新构建"
            exit 1
        fi
        if (( RETRY_EXPLICIT )); then
            log_fail "compare-siqs-shadow-observe 不接受 --retry；自动重试会破坏 fresh-process 证据"
            exit 1
        fi
        local _shadow_compare_timeout=60
        (( TIMEOUT_EXPLICIT )) && _shadow_compare_timeout="$TIMEOUT"
        if [[ ! "$_shadow_compare_timeout" =~ '^[1-9][0-9]*$' ]]; then
            log_fail "compare-siqs-shadow-observe 的 timeout 必须是正整数秒"
            exit 1
        fi
        if ! siqs_shadow_observe_validator_self_check; then
            log_fail "SIQS shadow observe validator self-check 失败；拒绝启动构建"
            exit 1
        fi

        do_build
        if [[ ! -x "${BUILD_DIR}/test_siqs_shadow_proof_observe_probe" ]]; then
            log_fail "SIQS shadow observe probe 二进制不存在: ${BUILD_DIR}/test_siqs_shadow_proof_observe_probe"
            exit 1
        fi
        log_header "SIQS production shadow-proof off/observe fresh-process 对照"
        log_info "单次 Release 构建；1 次 off + 3 次 observe；每进程 timeout=${_shadow_compare_timeout}s"
        log_info "不跨运行比较 raw corpus、指纹、依赖序号或其他 identity 字段"

        local _shadow_compare_ok=1
        local _off_probe="" _off_factor_wall_ns="" _off_process_peak_supported="false"
        local _off_process_peak_bytes="0" _off_backend="unavailable"
        typeset -a _observe_factor_wall_values
        typeset -a _observe_process_peak_values
        typeset -a _shadow_before_peak_values
        typeset -a _shadow_after_peak_values
        typeset -a _shadow_growth_values
        typeset -a _supported_backends
        _observe_factor_wall_values=()
        _observe_process_peak_values=()
        _shadow_before_peak_values=()
        _shadow_after_peak_values=()
        _shadow_growth_values=()
        _supported_backends=()

        if run_siqs_shadow_observe_process off 0 "$_shadow_compare_timeout"; then
            _off_probe="$SIQS_SHADOW_OBSERVE_PROBE_RECORD"
            printf '%s\n' "$_off_probe"
            _off_factor_wall_ns=$(measurement_record_field "$_off_probe" factor_wall_ns)
            _off_process_peak_supported=$(measurement_record_field \
                "$_off_probe" after_peak_rss_supported)
            _off_process_peak_bytes=$(measurement_record_field "$_off_probe" after_peak_rss_bytes)
            if [[ "$_off_process_peak_supported" == "true" ]]; then
                _off_backend=$(measurement_record_field "$_off_probe" after_rss_backend)
                _supported_backends+=("$_off_backend")
            fi
        else
            _shadow_compare_ok=0
        fi

        local _shadow_sample _observe_probe _observe_telemetry
        local _process_peak_supported _process_peak_bytes _process_backend
        local _shadow_growth_supported _shadow_backend
        if (( _shadow_compare_ok || !FAIL_FAST )); then
            for _shadow_sample in 1 2 3; do
                if run_siqs_shadow_observe_process \
                    observe "$_shadow_sample" "$_shadow_compare_timeout"; then
                    _observe_probe="$SIQS_SHADOW_OBSERVE_PROBE_RECORD"
                    _observe_telemetry="$SIQS_SHADOW_OBSERVE_TELEMETRY_RECORD"
                    printf '%s\n' "$_observe_probe"
                    printf '%s\n' "$_observe_telemetry"
                    _observe_factor_wall_values+=("$(measurement_record_field \
                        "$_observe_probe" factor_wall_ns)")
                    _process_peak_supported=$(measurement_record_field \
                        "$_observe_probe" after_peak_rss_supported)
                    if [[ "$_process_peak_supported" == "true" ]]; then
                        _process_peak_bytes=$(measurement_record_field \
                            "$_observe_probe" after_peak_rss_bytes)
                        _process_backend=$(measurement_record_field \
                            "$_observe_probe" after_rss_backend)
                        _observe_process_peak_values+=("$_process_peak_bytes")
                        _supported_backends+=("$_process_backend")
                    fi
                    _shadow_growth_supported=$(measurement_record_field \
                        "$_observe_telemetry" peak_growth_supported)
                    if [[ "$_shadow_growth_supported" == "true" ]]; then
                        _shadow_before_peak_values+=("$(measurement_record_field \
                            "$_observe_telemetry" before_peak_rss_bytes)")
                        _shadow_after_peak_values+=("$(measurement_record_field \
                            "$_observe_telemetry" after_peak_rss_bytes)")
                        _shadow_growth_values+=("$(measurement_record_field \
                            "$_observe_telemetry" peak_growth_bytes)")
                        _shadow_backend=$(measurement_record_field \
                            "$_observe_telemetry" after_rss_backend)
                        _supported_backends+=("$_shadow_backend")
                    fi
                else
                    _shadow_compare_ok=0
                    (( FAIL_FAST )) && break
                fi
            done
        fi

        if (( _shadow_compare_ok )); then
            local _observe_wall_min _observe_wall_max
            _observe_wall_min=$(uint_values_min "${_observe_factor_wall_values[@]}")
            _observe_wall_max=$(uint_values_max "${_observe_factor_wall_values[@]}")

            local _observe_peak_min="na" _observe_peak_max="na"
            local _shadow_before_min="na" _shadow_before_max="na"
            local _shadow_after_min="na" _shadow_after_max="na"
            local _shadow_growth_min="na" _shadow_growth_max="na"
            if (( ${#_observe_process_peak_values[@]} > 0 )); then
                _observe_peak_min=$(uint_values_min "${_observe_process_peak_values[@]}")
                _observe_peak_max=$(uint_values_max "${_observe_process_peak_values[@]}")
            fi
            if (( ${#_shadow_growth_values[@]} > 0 )); then
                _shadow_before_min=$(uint_values_min "${_shadow_before_peak_values[@]}")
                _shadow_before_max=$(uint_values_max "${_shadow_before_peak_values[@]}")
                _shadow_after_min=$(uint_values_min "${_shadow_after_peak_values[@]}")
                _shadow_after_max=$(uint_values_max "${_shadow_after_peak_values[@]}")
                _shadow_growth_min=$(uint_values_min "${_shadow_growth_values[@]}")
                _shadow_growth_max=$(uint_values_max "${_shadow_growth_values[@]}")
            fi

            local _rss_evidence="unavailable" _rss_backend="unavailable"
            local _experiment_eligibility="insufficient_evidence"
            local _backend_consistent=1 _backend_value=""
            if (( ${#_supported_backends[@]} > 0 )); then
                _backend_value="${_supported_backends[1]}"
                local _candidate_backend
                for _candidate_backend in "${_supported_backends[@]}"; do
                    [[ "$_candidate_backend" == "$_backend_value" ]] || _backend_consistent=0
                done
            else
                _backend_consistent=0
            fi
            if [[ "$_off_process_peak_supported" == "true" ]] &&
               (( ${#_observe_process_peak_values[@]} == 3 &&
                  ${#_shadow_growth_values[@]} == 3 && _backend_consistent )); then
                _rss_evidence="available"
                _rss_backend="$_backend_value"
                _experiment_eligibility="candidate"
            fi

            local _comparison_record
            _comparison_record="GNFS_SIQS_SHADOW_PROOF_OBSERVE_COMPARISON_V1"
            _comparison_record+=" schema_version=1 status=valid"
            _comparison_record+=" profile_id=siqs50_production_shadow_observe_v1"
            _comparison_record+=" off_runs=1 observe_runs=3"
            _comparison_record+=" legacy_factor_parity=pass proof_evidence=pass"
            _comparison_record+=" matrix_shape_evidence=pass rss_evidence=${_rss_evidence}"
            _comparison_record+=" rss_backend=${_rss_backend}"
            _comparison_record+=" off_factor_wall_ns=${_off_factor_wall_ns}"
            _comparison_record+=" observe_factor_wall_ns_min=${_observe_wall_min}"
            _comparison_record+=" observe_factor_wall_ns_max=${_observe_wall_max}"
            _comparison_record+=" off_process_peak_rss_supported=${_off_process_peak_supported}"
            _comparison_record+=" off_process_peak_rss_bytes=${_off_process_peak_bytes}"
            _comparison_record+=" observe_process_peak_rss_supported_runs=${#_observe_process_peak_values[@]}"
            _comparison_record+=" observe_process_peak_rss_bytes_min=${_observe_peak_min}"
            _comparison_record+=" observe_process_peak_rss_bytes_max=${_observe_peak_max}"
            _comparison_record+=" shadow_before_peak_rss_bytes_min=${_shadow_before_min}"
            _comparison_record+=" shadow_before_peak_rss_bytes_max=${_shadow_before_max}"
            _comparison_record+=" shadow_after_peak_rss_bytes_min=${_shadow_after_min}"
            _comparison_record+=" shadow_after_peak_rss_bytes_max=${_shadow_after_max}"
            _comparison_record+=" shadow_peak_growth_supported_runs=${#_shadow_growth_values[@]}"
            _comparison_record+=" shadow_peak_growth_bytes_min=${_shadow_growth_min}"
            _comparison_record+=" shadow_peak_growth_bytes_max=${_shadow_growth_max}"
            _comparison_record+=" identity_compared=false timing_threshold_applied=false"
            _comparison_record+=" rss_threshold_applied=false shadow_outcome_routed=false"
            _comparison_record+=" prefer_scope=explicit_experiment_only"
            _comparison_record+=" experiment_eligibility=${_experiment_eligibility} promotion=false"
            printf '%s\n' "$_comparison_record"

            (( TOTAL_TESTS += 1 ))
            (( PASSED_TESTS += 1 ))
            REPORT_ENTRIES+=("{\"name\":\"compare_siqs_shadow_observe_50\",\"status\":\"pass\",\"elapsed_ms\":0}")
            log_success "1+3 fresh processes 均通过；comparison 仅报告范围，不比较跨运行 identity"
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

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
#   ./scripts/test.sh smoke               # 冒烟测试: 仅最快的核心测试 (~5s)
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

# ============================================================
# 全局配置
# ============================================================

PROJECT_ROOT="${0:A:h:h}"
BUILD_DIR="${PROJECT_ROOT}/build"
BENCH_DIR="${PROJECT_ROOT}/benchmarks"
REPORT_FILE="${BUILD_DIR}/test_report.json"
NCPU=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

BUILD_TYPE="Debug"
PARALLEL_JOBS="$NCPU"
VERBOSE=0
QUIET=0
SKIP_BUILD=0
USE_COLOR=1
FAIL_FAST=0
TIMEOUT=300
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
    test_factor_base
    test_special_q
    test_lattice_sieve
    test_sieve_basic
    test_relation_collector
    test_cofactor
    test_linalg
    test_sqrt
    test_sqrt_debug
    test_murphy
    test_kleinjung
    test_kleinjung_large
    test_factor_with_kleinjung
    test_gnfs_e2e
    test_gnfs_progressive
    test_25digit
    test_params
    test_int_polynomial
    test_filter
    test_regressions
    test_polynomial_context
    test_base_m
    test_polynomial_optimizer
    test_class_group
    test_edge_cases
    test_integration
    test_stress
)

# 模块 → 测试二进制映射 (仅 instant+fast 的测试)
typeset -A MODULE_TESTS
MODULE_TESTS=(
    core           "test_integer test_params test_regressions test_edge_cases"
    util           "test_small_vector test_thread_pool"
    polynomial     "test_murphy test_int_polynomial test_regressions test_polynomial_context test_base_m test_polynomial_optimizer"
    factor_base    "test_factor_base"
    sieve          "test_special_q test_sieve_basic"
    cofactor       "test_cofactor test_integration"
    relation       "test_relation_collector test_filter test_integration"
    linalg         "test_linalg test_edge_cases test_integration"
    integration    "test_integration"
    sqrt           "test_sqrt test_sqrt_debug test_class_group"
)

# 模块 → 慢速测试映射 (slow+heavy, 可选运行)
typeset -A MODULE_SLOW_TESTS
MODULE_SLOW_TESTS=(
    polynomial     "test_kleinjung test_kleinjung_large test_factor_with_kleinjung"
    sieve          "test_lattice_sieve"
)

# 冒烟测试子集 (仅 < 1s 的纯单元测试，绝不包含实际 GNFS 运算)
typeset -a SMOKE_TESTS
SMOKE_TESTS=(
    test_integer
    test_params
    test_int_polynomial
    test_small_vector
    test_thread_pool
    test_factor_base
    test_special_q
    test_relation_collector
    test_filter
    test_cofactor
    test_linalg
    test_sqrt
    test_sqrt_debug
    test_murphy
    test_regressions
    test_polynomial_context
    test_base_m
    test_polynomial_optimizer
    test_class_group
    test_edge_cases
)

# ── 每个测试的超时秒数 (基于实测) ──
# instant (<1s): 大部分单元测试
# fast (1-30s): sieve_basic
# slow (30s-5min): kleinjung, lattice_sieve, gnfs_e2e, factor_with_kleinjung
# heavy (5min+): gnfs_progressive, 25digit, kleinjung_large
typeset -A TEST_TIMEOUT
TEST_TIMEOUT=(
    test_integer             10
    test_small_vector        10
    test_thread_pool         10
    test_factor_base         10
    test_special_q           10
    test_relation_collector  10
    test_cofactor            10
    test_linalg              10
    test_sqrt                10
    test_sqrt_debug          10
    test_murphy              10
    test_params              10
    test_int_polynomial      10
    test_filter              10
    test_regressions         10
    test_polynomial_context  10
    test_base_m              10
    test_polynomial_optimizer 10
    test_class_group         10
    test_edge_cases          10
    test_integration         30
    test_sieve_basic         60
    test_kleinjung           180
    test_kleinjung_large     600
    test_factor_with_kleinjung 300
    test_lattice_sieve       180
    test_gnfs_e2e            300
    test_gnfs_progressive    3600
    test_25digit             1800
    test_stress              43200
)

# 测试速度分级 (用于 list 显示)
typeset -A TEST_TIER
TEST_TIER=(
    test_integer             "instant"
    test_small_vector        "instant"
    test_thread_pool         "instant"
    test_factor_base         "instant"
    test_special_q           "instant"
    test_relation_collector  "instant"
    test_cofactor            "instant"
    test_linalg              "instant"
    test_sqrt                "instant"
    test_sqrt_debug          "instant"
    test_murphy              "instant"
    test_params              "instant"
    test_int_polynomial      "instant"
    test_filter              "instant"
    test_regressions         "instant"
    test_polynomial_context  "instant"
    test_base_m              "instant"
    test_polynomial_optimizer "instant"
    test_class_group         "instant"
    test_edge_cases          "instant"
    test_integration         "fast"
    test_sieve_basic         "fast"
    test_kleinjung           "slow"
    test_kleinjung_large     "heavy"
    test_factor_with_kleinjung "slow"
    test_lattice_sieve       "slow"
    test_gnfs_e2e            "slow"
    test_gnfs_progressive    "heavy"
    test_25digit             "heavy"
    test_stress              "heavy"
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
)

# 有序模块列表 (按流水线顺序)
typeset -a MODULE_ORDER
MODULE_ORDER=(core util polynomial factor_base sieve cofactor relation linalg sqrt)

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
        warn_count=$(echo "$build_output" | grep -c "warning:" 2>/dev/null || true)

        if (( warn_count > 0 )); then
            log_warn "编译成功 ($(format_duration $elapsed)) — ${warn_count} warnings"
            if (( ! QUIET )); then
                echo "$build_output" | grep "warning:" | head -5
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
        (( polls++ ))
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
        (( elapsed_secs++ ))

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
        (( SKIPPED_TESTS++ ))
        return 2
    fi

    (( TOTAL_TESTS++ ))

    # 确定超时: 优先用 --timeout 全局覆盖，否则用每测试分级超时
    local test_timeout=${TIMEOUT}
    if (( TIMEOUT == 300 )); then
        # 用户未指定 --timeout，使用分级默认值
        test_timeout=${TEST_TIMEOUT[$name]:-$TIMEOUT}
    fi

    local tier="${TEST_TIER[$name]:-unknown}"

    # 显示开始运行 (非安静模式下，对慢测试提前告知)
    if (( !QUIET )) && [[ "$tier" == "slow" || "$tier" == "heavy" ]]; then
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
    if (( !QUIET )) && [[ "$tier" == "slow" || "$tier" == "heavy" ]]; then
        printf "\r\033[K" >&2
    fi

    # 超时处理
    if (( exit_code == 124 )); then
        log_fail "${name} ${RED}TIMEOUT${RESET} ${DIM}after ${test_timeout}s (tier=${tier})${RESET}"
        if (( !QUIET )); then
            echo "${DIM}最后输出:${RESET}"
            echo "$output" | tail -10
        fi
        (( FAILED_TESTS++ ))
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
        (( PASSED_TESTS++ ))
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
        (( FAILED_TESTS++ ))
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
    log_info "运行最关键的 ${#SMOKE_TESTS[@]} 个测试 (目标 <10s)"
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
            (( mod_total++ ))
            if run_single_test "$test"; then
                (( mod_pass++ ))
            fi
        done

        # 包含慢速测试
        if (( include_slow )); then
            local slow_tests="${MODULE_SLOW_TESTS[$mod]:-}"
            if [[ -n "$slow_tests" ]]; then
                log_info "  ${DIM}(包含慢速测试)${RESET}"
                for test in ${(s: :)slow_tests}; do
                    (( mod_total++ ))
                    if run_single_test "$test"; then
                        (( mod_pass++ ))
                    fi
                done
            fi
        fi

        if (( mod_pass == mod_total )); then
            (( module_pass++ ))
            log_info "${BOLD}${mod}${RESET}: ${mod_pass}/${mod_total} 全部通过"
        else
            (( module_fail++ ))
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
        (( stage_num++ ))

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
    echo "  ${BULLET} ${CYAN}test_stress${RESET}            — 压力测试: 50/80/100-digit (164-330 bit)"

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
            slow)    _tc="${YELLOW}" ;;
            heavy)   _tc="${RED}" ;;
            *)       _tc="${DIM}" ;;
        esac
        echo "  ${_em} $(printf '%-32s' "$t") ${_tc}$(printf '%-10s' "$_tier")${RESET} ${_tout}s"
    done

    echo ""
    echo "${BOLD}测试层级金字塔:${RESET}"
    echo ""
    echo "           ${DIM}▲ 耗时${RESET}"
    echo "           │"
    echo "           │   ${RED}stress${RESET}         50/80/100-digit 压力测试
           │   ${MAGENTA}nightly${RESET}        thorough + L4 + L5 + stress L1"
    echo "           │   ${MAGENTA}thorough${RESET}       全模块 + 集成 + L1-L3"
    echo "           │   ${YELLOW}full${RESET}           ctest + E2E + L1-L2"
    echo "           │   ${CYAN}integration${RESET}    跨模块交互"
    echo "           │   ${CYAN}pipeline${RESET}       流水线阶段逐验证"
    echo "           │   ${GREEN}unit${RESET}           ctest 单元测试"
    echo "           │   ${GREEN}module <m>${RESET}     指定模块"
    echo "           │   ${GREEN}smoke${RESET}          最快核心子集"
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
        -t)          BUILD_TYPE="$2"; shift 2 ;;
        -v)          VERBOSE=1; shift ;;
        -q)          QUIET=1; shift ;;
        --no-build)  SKIP_BUILD=1; shift ;;
        --no-color)  USE_COLOR=0; setup_colors; shift ;;
        --fail-fast) FAIL_FAST=1; shift ;;
        --timeout)   TIMEOUT="$2"; shift 2 ;;
        --retry)     RETRY_COUNT="$2"; shift 2 ;;
        --save)      BENCH_EXTRA_ARGS+=(--save); shift ;;
        --compare)   BENCH_EXTRA_ARGS+=(--compare); shift ;;
        --deep)      MODE_ARGS+=(--deep); shift ;;
        --slow)      MODE_ARGS+=(--slow); shift ;;
        -h|--help)
            # 显示帮助
            sed -n '2,/^set -eo/{ /^#/s/^# \{0,1\}//p }' "$0"
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

    perf)
        do_build
        log_header "性能测试 (25-digit)"
        log_warn "预计耗时数分钟..."
        run_single_test test_25digit
        show_summary
        ;;

    stress)
        do_build
        log_header "压力测试 (50/80/100-digit)"
        log_warn "50-digit 可能需要数小时, 80/100-digit 可能需要数天或超出当前能力..."
        local stress_min=${2:-1}
        local stress_max=${3:-3}
        run_single_test test_stress "$stress_min" "$stress_max"
        show_summary
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
        echo "常用模式: smoke | unit | module | e2e | changed | full | list"
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

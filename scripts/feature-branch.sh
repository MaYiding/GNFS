#!/usr/bin/env zsh
# ╔══════════════════════════════════════════════════════════════════╗
# ║  GNFS 特性分支工作流                                              ║
# ║  创建分支 → 开发 → 门禁验证 → 合并到 main                         ║
# ╚══════════════════════════════════════════════════════════════════╝
#
# 用法:
#   ./scripts/feature-branch.sh create feat bucket-sieve   # 创建 feat/260315-bucket-sieve
#   ./scripts/feature-branch.sh create fix  sqrt-overflow  # 创建 fix/260315-sqrt-overflow
#   ./scripts/feature-branch.sh gate                       # 在当前分支运行合并门禁
#   ./scripts/feature-branch.sh gate --quick               # 快速门禁 (仅 smoke)
#   ./scripts/feature-branch.sh merge                      # 门禁通过后合并到 main
#   ./scripts/feature-branch.sh merge --force              # 跳过门禁直接合并 (慎用)
#   ./scripts/feature-branch.sh status                     # 显示当前分支状态
#   ./scripts/feature-branch.sh list                       # 列出所有特性分支

set -eo pipefail

PROJECT_ROOT="${0:A:h:h}"
SCRIPTS_DIR="${PROJECT_ROOT}/scripts"

# ============================================================
# 颜色
# ============================================================

if [[ -t 1 ]]; then
    RED=$'\033[0;31m'
    GREEN=$'\033[0;32m'
    YELLOW=$'\033[0;33m'
    BLUE=$'\033[0;34m'
    CYAN=$'\033[0;36m'
    BOLD=$'\033[1m'
    DIM=$'\033[2m'
    RESET=$'\033[0m'
else
    RED='' GREEN='' YELLOW='' BLUE='' CYAN='' BOLD='' DIM='' RESET=''
fi

info()    { echo "${BLUE}[INFO]${RESET} $*"; }
success() { echo "${GREEN}[PASS]${RESET} $*"; }
fail()    { echo "${RED}[FAIL]${RESET} $*"; }
warn()    { echo "${YELLOW}[WARN]${RESET} $*"; }

# ============================================================
# 子命令: create
# ============================================================

do_create() {
    local type="${1:-}"
    local desc="${2:-}"

    if [[ -z "$type" || -z "$desc" ]]; then
        fail "用法: $0 create <type> <description>"
        echo "  type: feat | fix | perf | refactor | exp"
        echo "  description: 短横线分隔的描述 (如 bucket-sieve)"
        echo ""
        echo "示例:"
        echo "  $0 create feat bucket-sieve"
        echo "  $0 create fix  sqrt-overflow"
        exit 1
    fi

    # 验证 type
    case "$type" in
        feat|fix|perf|refactor|exp) ;;
        *)
            fail "未知类型: $type (允许: feat, fix, perf, refactor, exp)"
            exit 1
            ;;
    esac

    # 生成分支名: <type>/<YYMMDD>-<desc>
    local date_prefix=$(date +%y%m%d)
    local branch_name="${type}/${date_prefix}-${desc}"

    # 检查是否已存在
    if git rev-parse --verify "$branch_name" >/dev/null 2>&1; then
        fail "分支已存在: $branch_name"
        exit 1
    fi

    # 确认从 main 创建
    local current_branch=$(git branch --show-current)
    if [[ "$current_branch" != "main" ]]; then
        warn "当前不在 main 分支 (当前: $current_branch)"
        echo -n "是否从 main 创建? [Y/n] "
        read -r answer
        if [[ "$answer" =~ ^[Nn] ]]; then
            info "从当前分支 $current_branch 创建"
        else
            git checkout main
        fi
    fi

    git checkout -b "$branch_name"
    success "已创建分支: ${BOLD}${branch_name}${RESET}"
    echo ""
    info "开发完成后运行:"
    echo "  ${CYAN}./scripts/feature-branch.sh gate${RESET}   # 运行合并门禁"
    echo "  ${CYAN}./scripts/feature-branch.sh merge${RESET}  # 合并到 main"
}

# ============================================================
# 子命令: gate
# ============================================================

do_gate() {
    local current_branch=$(git branch --show-current)

    if [[ "$current_branch" == "main" ]]; then
        warn "当前在 main 分支，门禁通常在特性分支上运行"
    fi

    info "在分支 ${BOLD}${current_branch}${RESET} 上运行合并门禁..."
    echo ""

    # 委托给 test.sh gate
    "${SCRIPTS_DIR}/test.sh" gate "$@"
    local exit_code=$?

    if (( exit_code == 0 )); then
        echo ""
        success "分支 ${BOLD}${current_branch}${RESET} 已通过合并门禁"
        info "运行 ${CYAN}./scripts/feature-branch.sh merge${RESET} 合并到 main"
    else
        echo ""
        fail "分支 ${BOLD}${current_branch}${RESET} 未通过合并门禁"
        info "请修复失败的测试后重新运行门禁"
    fi

    return $exit_code
}

# ============================================================
# 子命令: merge
# ============================================================

do_merge() {
    local force=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --force) force=1; shift ;;
            *)       shift ;;
        esac
    done

    local current_branch=$(git branch --show-current)

    # 禁止在 main 上 merge
    if [[ "$current_branch" == "main" ]]; then
        fail "当前在 main 分支，无法合并自身"
        exit 1
    fi

    # 检查工作区干净
    if [[ -n "$(git status --porcelain)" ]]; then
        fail "工作区有未提交的变更，请先 commit 或 stash"
        git status --short
        exit 1
    fi

    # 运行门禁 (除非 --force)
    if (( !force )); then
        info "运行合并门禁..."
        echo ""
        "${SCRIPTS_DIR}/test.sh" gate
        local gate_exit=$?
        if (( gate_exit != 0 )); then
            fail "门禁未通过，中止合并"
            info "使用 ${CYAN}--force${RESET} 跳过门禁 (慎用)"
            exit 1
        fi
        echo ""
    else
        warn "跳过门禁验证 (--force)"
    fi

    # 执行合并
    info "合并 ${BOLD}${current_branch}${RESET} 到 main..."
    git checkout main
    git merge --no-ff "$current_branch"

    local merge_exit=$?
    if (( merge_exit != 0 )); then
        fail "合并失败 (可能有冲突)"
        exit 1
    fi

    success "已合并 ${BOLD}${current_branch}${RESET} 到 main"
    echo ""
    info "分支已保留 (按项目规范不删除)"
    info "如需推送: ${CYAN}git push origin main${RESET}"
}

# ============================================================
# 子命令: status
# ============================================================

do_status() {
    local current_branch=$(git branch --show-current)
    local main_hash=$(git rev-parse --short main 2>/dev/null || echo "N/A")
    local head_hash=$(git rev-parse --short HEAD)

    echo "${BOLD}${CYAN}╔══════════════════════════════════════════════════╗${RESET}"
    echo "${BOLD}${CYAN}║  特性分支状态                                    ║${RESET}"
    echo "${BOLD}${CYAN}╚══════════════════════════════════════════════════╝${RESET}"
    echo ""
    echo "  分支: ${BOLD}${current_branch}${RESET}"
    echo "  HEAD: ${head_hash}"
    echo "  main: ${main_hash}"

    if [[ "$current_branch" != "main" ]]; then
        local ahead=$(git rev-list main..HEAD --count 2>/dev/null || echo 0)
        local behind=$(git rev-list HEAD..main --count 2>/dev/null || echo 0)
        echo "  领先 main: ${GREEN}${ahead}${RESET} commits"
        echo "  落后 main: ${RED}${behind}${RESET} commits"
    fi

    echo ""

    # 工作区状态
    local changes=$(git status --porcelain | wc -l | tr -d ' ')
    if (( changes > 0 )); then
        warn "工作区有 ${changes} 个未提交变更"
    else
        success "工作区干净"
    fi
}

# ============================================================
# 子命令: list
# ============================================================

do_list() {
    echo "${BOLD}${CYAN}╔══════════════════════════════════════════════════╗${RESET}"
    echo "${BOLD}${CYAN}║  特性分支列表                                    ║${RESET}"
    echo "${BOLD}${CYAN}╚══════════════════════════════════════════════════╝${RESET}"
    echo ""

    local current_branch=$(git branch --show-current)
    local has_feature=0

    # 列出所有 feat/fix/perf/refactor/exp 分支
    git branch --list 'feat/*' 'fix/*' 'perf/*' 'refactor/*' 'exp/*' --format='%(refname:short)' 2>/dev/null | while read branch; do
        has_feature=1
        local marker=" "
        if [[ "$branch" == "$current_branch" ]]; then
            marker="${GREEN}*${RESET}"
        fi

        # 获取 commit 数
        local commits=$(git rev-list main.."$branch" --count 2>/dev/null || echo "?")
        local last_date=$(git log -1 --format='%ci' "$branch" 2>/dev/null | cut -d' ' -f1)

        printf "  %s ${BOLD}%-40s${RESET} %s commits  %s\n" "$marker" "$branch" "$commits" "${DIM}${last_date}${RESET}"
    done

    if (( !has_feature )); then
        echo "  ${DIM}(无特性分支)${RESET}"
    fi
}

# ============================================================
# 主入口
# ============================================================

CMD="${1:-}"
shift 2>/dev/null || true

case "$CMD" in
    create)  do_create "$@" ;;
    gate)    do_gate "$@" ;;
    merge)   do_merge "$@" ;;
    status)  do_status ;;
    list|ls) do_list ;;
    *)
        echo "GNFS 特性分支工作流"
        echo ""
        echo "用法: $0 <command> [args]"
        echo ""
        echo "命令:"
        echo "  ${CYAN}create${RESET} <type> <desc>  创建特性分支 (feat/fix/perf/refactor/exp)"
        echo "  ${CYAN}gate${RESET}   [--quick]      运行合并门禁"
        echo "  ${CYAN}merge${RESET}  [--force]      门禁通过后合并到 main"
        echo "  ${CYAN}status${RESET}                显示当前分支状态"
        echo "  ${CYAN}list${RESET}                  列出所有特性分支"
        echo ""
        echo "完整工作流:"
        echo "  1. $0 create feat bucket-sieve"
        echo "  2. (开发 + 提交)"
        echo "  3. $0 gate"
        echo "  4. $0 merge"
        exit 1
        ;;
esac

# P1.A: M5 PMU 事件深化采集 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 M5 上把 PMU 采集从 xctrace "CPU Counters" 4 列聚合升级到 10 列细粒度事件（含 `ARM_STALL_BACKEND/FRONTEND` 等），首次拿到能区分 MemBound vs CoreBound vs BadSpec 的真实数据，据此为 doctrine §6 P1.B 锁定优化方向。

**Architecture:** mperf（外部 MIT 工具）调 Apple 私有 kperf framework 直接读 PMU 计数器；`scripts/perf/pmu-stat.sh` 包装事件集 + JSON 输出；`scripts/perf/pmu-derive.py` 解析 + 派生 IPC / 各类 stall rate / miss rate；test.sh 加 `pmu` 子命令；最终 `bench/results/2026-05-13-pmu-deepening.md` 给出 P1.B 决策。

**Tech Stack:** zsh, Python 3 stdlib, [mperf](https://github.com/tmcgilchrist/mperf), Apple kperf/kpep frameworks (private), as5.plist (M5 event database).

**Branch:** `feat/260513-pmu-events-deepening`（已创建）

---

## 事件集（10 槽满采，无 multiplexing）

> **修订 (Task 1 调查后)**: mperf v0.1 `mperf-stat -l` 在 M5 上**未暴露** as5.plist 中的细分事件 `BRANCH_COND_MISPRED_NONSPEC`、`INST_LDST`、`INST_SIMD_ALU`。已替换为 mperf 暴露的等价物。完整可用事件清单见 `~/.local/bin/mperf-stat -l`。

| 类型 | 名称 | 用途 |
|---|---|---|
| Fixed | `FIXED_CYCLES` | IPC 分母 |
| Fixed | `FIXED_INSTRUCTIONS` | IPC 分子 |
| Config | `ARM_STALL_BACKEND` | **后端 stall 总周期（MemBound + CoreBound 共同贡献）** |
| Config | `ARM_STALL_FRONTEND` | **前端 stall（FrontendBound）** |
| Config | `L1D_CACHE_MISS_LD` | L1D load miss（细分 MemBound） |
| Config | `L1D_TLB_MISS` | TLB miss |
| Config | `ARM_MEM_ACCESS` | 数据访存总数（miss rate 分母，原 INST_LDST 替代） |
| Config | `BRANCH_MISPRED_NONSPEC` | 误判分支（BadSpec 主指标，含 cond+indir，原 BRANCH_COND_MISPRED_NONSPEC 替代） |
| Config | `INST_BRANCH` | 分支总数（mispred rate 分母） |
| Config | `MAP_SIMD_UOP` | SIMD/FP uop 映射数（NEON 利用率，原 INST_SIMD_ALU 替代） |

派生指标：
- `IPC = INSTRUCTIONS / CYCLES`
- `BackendStallRate = STALL_BACKEND / CYCLES`
- `FrontendStallRate = STALL_FRONTEND / CYCLES`
- `L1DMissRate = L1D_CACHE_MISS_LD / ARM_MEM_ACCESS`
- `TLBMissRate = L1D_TLB_MISS / ARM_MEM_ACCESS`
- `BranchMispredRate = BRANCH_MISPRED_NONSPEC / INST_BRANCH`
- `SIMDDensity = MAP_SIMD_UOP / INSTRUCTIONS` (mapped 不是 retired，作为相对指标尚可)

P1.B 决策规则（doctrine §6）：
- `BackendStallRate > 30%` 且 `L1DMissRate > 5%` → **MemBound** → prefetch / 对齐 / SoA
- `BackendStallRate > 30%` 且 `L1DMissRate < 2%` 且 `SIMDDensity < 5%` → **CoreBound** → NEON 全面化 / ILP
- `BranchMispredRate > 5%` → **BadSpec** → `[[likely]]` / branchless

---

## Task 1: 安装 mperf 作为外部依赖

**Files:**
- Create: `scripts/perf/install-mperf.sh`
- Modify: `.gitignore` 增加 mperf 缓存目录

- [ ] **Step 1: 写 install-mperf.sh**

```bash
#!/usr/bin/env zsh
# scripts/perf/install-mperf.sh
# Clone, build, and install mperf (Apple Silicon hardware perf counters CLI)
# into ~/.local/bin (no /usr/local pollution, no brew conflict).
#
# Usage:
#   scripts/perf/install-mperf.sh           # first install
#   scripts/perf/install-mperf.sh --update  # pull + rebuild
#
# Requires: Xcode CLT (make, xcrun), git. mperf itself needs sudo at runtime.

set -euo pipefail

MPERF_REPO="https://github.com/tmcgilchrist/mperf.git"
MPERF_DIR="${HOME}/.cache/mperf-src"
INSTALL_DIR="${HOME}/.local/bin"
UPDATE=0

[[ "${1:-}" == "--update" ]] && UPDATE=1

mkdir -p "${INSTALL_DIR}"

if [[ ! -d "${MPERF_DIR}" ]]; then
    echo "== Cloning mperf into ${MPERF_DIR} =="
    git clone "${MPERF_REPO}" "${MPERF_DIR}"
elif (( UPDATE )); then
    echo "== Updating mperf =="
    git -C "${MPERF_DIR}" pull --ff-only
fi

echo "== Building mperf =="
make -C "${MPERF_DIR}" clean >/dev/null 2>&1 || true
make -C "${MPERF_DIR}"

BIN_SRC="${MPERF_DIR}/mperf-stat"
if [[ ! -x "${BIN_SRC}" ]]; then
    echo "ERROR: mperf-stat binary not produced at ${BIN_SRC}" >&2
    exit 1
fi

ln -sf "${BIN_SRC}" "${INSTALL_DIR}/mperf-stat"
echo ""
echo "== Done =="
echo "Installed:  ${INSTALL_DIR}/mperf-stat -> ${BIN_SRC}"
echo ""
echo "Add to PATH if needed:  export PATH=\"\${HOME}/.local/bin:\${PATH}\""
echo "Test:                   sudo ${INSTALL_DIR}/mperf-stat -- /bin/echo hi"
```

- [ ] **Step 2: 给执行权限**

```bash
chmod +x scripts/perf/install-mperf.sh
```

- [ ] **Step 3: 跑安装**

Run: `./scripts/perf/install-mperf.sh`
Expected: 输出 "Done"，`~/.local/bin/mperf-stat` 存在且可执行

- [ ] **Step 4: 验证 mperf 在 M5 上能跑（最小命令）**

Run: `sudo ~/.local/bin/mperf-stat -- /bin/echo hi`
Expected: 输出 "hi" + cycles/instructions 计数（非零）

如果失败，记录错误到 `findings.md`，回到 Step 3 排查（kperf API 在 M5 上可能需要 entitlement / SIP 配置）。

- [ ] **Step 5: 更新 .gitignore**

`.gitignore` 末尾追加：

```
# PMU 采集 JSON (单次运行结果不入 git; 报告 .md 入 git)
bench/results/*.pmu.json
```

(mperf 源码缓存在 `${HOME}/.cache/mperf-src/`，不在项目内，无需 ignore)

- [ ] **Step 6: Commit**

```bash
git add scripts/perf/install-mperf.sh .gitignore
git commit -m "$(cat <<'EOF'
feat(perf): add scripts/perf/install-mperf.sh — mperf installer

mperf (https://github.com/tmcgilchrist/mperf) is a perf-stat-like CLI
that calls Apple's private kperf framework directly, giving access to
~135 PMU events on M5 (as5.plist). Installed to ~/.local/bin via symlink
to avoid /usr/local pollution and brew conflicts.

Required for P1.A — deepening PMU event collection beyond xctrace's
4-column CPU Bottlenecks aggregation.

Co-Authored-By: Claude <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: 写 pmu-stat.sh 包装 mperf

**Files:**
- Create: `scripts/perf/pmu-stat.sh`

- [ ] **Step 1: 写 pmu-stat.sh**

```bash
#!/usr/bin/env zsh
# scripts/perf/pmu-stat.sh
# Run a target binary under mperf with the GNFS P1.A event set (10 slots),
# emit JSON to bench/results/<timestamp>-<basename>.pmu.json.
#
# Usage:
#   scripts/perf/pmu-stat.sh <path-to-binary> [args...]
#   scripts/perf/pmu-stat.sh --events "alt,set" <bin>  # override default events
#
# Requires: mperf-stat in PATH or ~/.local/bin/mperf-stat. Needs sudo (kperf).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RESULTS_DIR="${ROOT}/bench/results"

# GNFS P1.A event set — see docs/superpowers/plans/2026-05-13-pmu-events-deepening.md
EVENTS=(
    FIXED_CYCLES
    FIXED_INSTRUCTIONS
    ARM_STALL_BACKEND
    ARM_STALL_FRONTEND
    L1D_CACHE_MISS_LD
    L1D_TLB_MISS
    ARM_MEM_ACCESS
    BRANCH_MISPRED_NONSPEC
    INST_BRANCH
    MAP_SIMD_UOP
)

while [[ $# -gt 0 ]]; do
    case "$1" in
        --events) IFS=',' read -rA EVENTS <<< "$2"; shift 2 ;;
        -h|--help)
            awk '/^set -euo/{exit} NR>=2 && /^#/{sub(/^# ?/,""); print}' "$0"
            exit 0 ;;
        *) break ;;
    esac
done

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 [--events <comma-list>] <bin> [args...]" >&2
    exit 1
fi

BIN="$1"; shift
[[ -x "${BIN}" ]] || { echo "ERROR: not executable: ${BIN}" >&2; exit 1; }

# Locate mperf-stat
MPERF=""
if command -v mperf-stat >/dev/null 2>&1; then
    MPERF=$(command -v mperf-stat)
elif [[ -x "${HOME}/.local/bin/mperf-stat" ]]; then
    MPERF="${HOME}/.local/bin/mperf-stat"
else
    echo "ERROR: mperf-stat not found. Run ./scripts/perf/install-mperf.sh first." >&2
    exit 1
fi

mkdir -p "${RESULTS_DIR}"
TS=$(date +%Y-%m-%d-%H%M%S)
BASENAME=$(basename "${BIN}")
JSON_OUT="${RESULTS_DIR}/${TS}-${BASENAME}.pmu.json"

# Build -e args
ARGS=()
for ev in "${EVENTS[@]}"; do
    ARGS+=(-e "${ev}")
done

echo "== Recording PMU =="
echo "  binary:  ${BIN} $*"
echo "  events:  ${(j:,:)EVENTS}"
echo "  out:     ${JSON_OUT}"
echo ""

sudo "${MPERF}" -j "${ARGS[@]}" -- "${BIN}" "$@" > "${JSON_OUT}"

echo ""
echo "== Done =="
echo "JSON: ${JSON_OUT} ($(ls -lh "${JSON_OUT}" | awk '{print $5}'))"
echo ""
echo "Parse: python3 ${SCRIPT_DIR}/pmu-derive.py ${JSON_OUT}"
```

- [ ] **Step 2: chmod +x**

```bash
chmod +x scripts/perf/pmu-stat.sh
```

- [ ] **Step 3: 用 /bin/echo 验证 wrapper 正确性**

Run: `./scripts/perf/pmu-stat.sh /bin/echo "smoke test"`
Expected:
- 提示输入 sudo 密码
- 输出 "smoke test"
- `bench/results/*-echo.pmu.json` 生成
- JSON 含 10 个 event key

如果 mperf JSON schema 与预期不同，记录到 findings.md，调整 EVENTS 名 / 解析方式。

- [ ] **Step 4: 检查 JSON schema**

Run: `python3 -c "import json,sys; print(json.dumps(json.load(open(sys.argv[1])), indent=2)[:500])" bench/results/*-echo.pmu.json | head`
Expected: 看见 `counters` / `time` 或类似顶层 key，含我们 10 个事件

记录实际 schema 到 findings.md，作为 Task 3 实现依据。

- [ ] **Step 5: Commit**

```bash
git add scripts/perf/pmu-stat.sh
git commit -m "$(cat <<'EOF'
feat(perf): add pmu-stat.sh — mperf wrapper with GNFS P1.A event set

Records 10 PMU events covering MemBound/CoreBound/BadSpec discrimination:
- FIXED_CYCLES, FIXED_INSTRUCTIONS (IPC)
- ARM_STALL_BACKEND, ARM_STALL_FRONTEND (top-down split)
- L1D_CACHE_MISS_LD, L1D_TLB_MISS, INST_LDST (memory)
- BRANCH_COND_MISPRED_NONSPEC, INST_BRANCH (bad speculation)
- INST_SIMD_ALU (NEON utilization)

Outputs JSON to bench/results/<timestamp>-<basename>.pmu.json.

Co-Authored-By: Claude <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: 写 pmu-derive.py 解析 + 派生指标

**Files:**
- Create: `scripts/perf/pmu-derive.py`

- [ ] **Step 1: 写 pmu-derive.py（stdlib only）**

```python
#!/usr/bin/env python3
"""
scripts/perf/pmu-derive.py

Parse mperf JSON output and emit markdown summary with derived metrics
for MemBound/CoreBound/BadSpec discrimination.

Usage:
    python3 pmu-derive.py <run.pmu.json>                # single-run summary
    python3 pmu-derive.py <baseline.json> <after.json>  # diff mode

See docs/superpowers/plans/2026-05-13-pmu-events-deepening.md.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Optional


EVENT_KEYS = [
    "FIXED_CYCLES", "FIXED_INSTRUCTIONS",
    "ARM_STALL_BACKEND", "ARM_STALL_FRONTEND",
    "L1D_CACHE_MISS_LD", "L1D_TLB_MISS", "ARM_MEM_ACCESS",
    "BRANCH_MISPRED_NONSPEC", "INST_BRANCH",
    "MAP_SIMD_UOP",
]


def load_counters(p: Path) -> dict[str, int]:
    """Extract event -> count from mperf JSON.

    mperf JSON shape (per Task 2 Step 4 findings.md): TBD at parse time.
    Defensive: walk all dicts, collect uppercase keys whose value is int/str-int.
    """
    raw = json.loads(p.read_text())
    counters: dict[str, int] = {}

    def walk(node):
        if isinstance(node, dict):
            for k, v in node.items():
                if isinstance(v, (int, float)) and k.isupper() and k in EVENT_KEYS:
                    counters[k] = int(v)
                elif isinstance(v, str) and k in EVENT_KEYS:
                    try:
                        counters[k] = int(v.replace(",", ""))
                    except ValueError:
                        pass
                else:
                    walk(v)
        elif isinstance(node, list):
            for x in node:
                walk(x)

    walk(raw)
    return counters


def derive(c: dict[str, int]) -> dict[str, Optional[float]]:
    """Compute derived metrics. Returns None for any metric with missing inputs."""
    def safe_div(num_k: str, den_k: str) -> Optional[float]:
        n, d = c.get(num_k), c.get(den_k)
        if n is None or d is None or d == 0:
            return None
        return n / d

    return {
        "IPC":               safe_div("FIXED_INSTRUCTIONS", "FIXED_CYCLES"),
        "BackendStallRate":  safe_div("ARM_STALL_BACKEND", "FIXED_CYCLES"),
        "FrontendStallRate": safe_div("ARM_STALL_FRONTEND", "FIXED_CYCLES"),
        "L1DMissRate":       safe_div("L1D_CACHE_MISS_LD", "ARM_MEM_ACCESS"),
        "TLBMissRate":       safe_div("L1D_TLB_MISS", "ARM_MEM_ACCESS"),
        "BranchMispredRate": safe_div("BRANCH_MISPRED_NONSPEC", "INST_BRANCH"),
        "SIMDDensity":       safe_div("MAP_SIMD_UOP", "FIXED_INSTRUCTIONS"),
    }


def classify(d: dict[str, Optional[float]]) -> list[str]:
    """Apply doctrine §6 P1 decision rules. Return list of triggered categories."""
    triggers = []
    bsr = d.get("BackendStallRate") or 0
    fsr = d.get("FrontendStallRate") or 0
    l1d = d.get("L1DMissRate") or 0
    bmr = d.get("BranchMispredRate") or 0
    sd  = d.get("SIMDDensity") or 0

    if bsr > 0.30 and l1d > 0.05:
        triggers.append("**MemBound** (BackendStall>30% AND L1DMissRate>5%) → prefetch / 64B align / SoA")
    if bsr > 0.30 and l1d < 0.02 and sd < 0.05:
        triggers.append("**CoreBound** (BackendStall>30% AND L1DMissRate<2% AND SIMDDensity<5%) → NEON / ILP")
    if bmr > 0.05:
        triggers.append("**BadSpec** (BranchMispredRate>5%) → `[[likely]]` / branchless")
    if fsr > 0.20:
        triggers.append("**FrontendBound** (FrontendStall>20%) → code-layout / inline review")
    return triggers


def fmt_int(n: int) -> str:
    return f"{n:,}"


def fmt_pct(v: Optional[float]) -> str:
    return f"{v*100:.2f}%" if v is not None else "n/a"


def fmt_ratio(v: Optional[float]) -> str:
    return f"{v:.3f}" if v is not None else "n/a"


def print_single(p: Path) -> int:
    c = load_counters(p)
    if not c:
        print(f"ERROR: no recognized PMU events in {p}", file=sys.stderr)
        return 1
    d = derive(c)
    triggers = classify(d)

    print(f"# PMU summary: {p.name}\n")
    print("## Raw counters\n")
    print("| Event | Count |\n|---|---:|")
    for ev in EVENT_KEYS:
        v = c.get(ev)
        print(f"| `{ev}` | {fmt_int(v) if v is not None else 'n/a'} |")
    print("\n## Derived metrics\n")
    print("| Metric | Value |\n|---|---:|")
    for k, v in d.items():
        if k == "IPC":
            print(f"| {k} | {fmt_ratio(v)} |")
        else:
            print(f"| {k} | {fmt_pct(v)} |")
    print("\n## doctrine §6 P1 decision\n")
    if triggers:
        for t in triggers:
            print(f"- {t}")
    else:
        print("- _no category exceeds threshold — collect a larger sample or expand events_")
    print()
    return 0


def print_diff(a: Path, b: Path) -> int:
    ca, cb = load_counters(a), load_counters(b)
    if not ca or not cb:
        print(f"ERROR: empty counters. a={len(ca)} b={len(cb)}", file=sys.stderr)
        return 1
    da, db = derive(ca), derive(cb)

    print(f"# PMU diff: {a.name} vs {b.name}\n")
    print("## Raw counter delta\n")
    print("| Event | A | B | Δ% |\n|---|---:|---:|---:|")
    for ev in EVENT_KEYS:
        va, vb = ca.get(ev), cb.get(ev)
        if va is None or vb is None or va == 0:
            ds = "n/a"
        else:
            ds = f"{(vb - va) / va * 100:+.2f}%"
        print(f"| `{ev}` | {fmt_int(va) if va is not None else 'n/a'} | {fmt_int(vb) if vb is not None else 'n/a'} | {ds} |")
    print("\n## Derived metric delta\n")
    print("| Metric | A | B | Δ pp / Δ ratio |\n|---|---:|---:|---:|")
    for k in da:
        va, vb = da[k], db[k]
        if va is None or vb is None:
            ds = "n/a"
        elif k == "IPC":
            ds = f"{vb - va:+.3f}"
        else:
            ds = f"{(vb - va) * 100:+.2f}pp"
        if k == "IPC":
            print(f"| {k} | {fmt_ratio(va)} | {fmt_ratio(vb)} | {ds} |")
        else:
            print(f"| {k} | {fmt_pct(va)} | {fmt_pct(vb)} | {ds} |")
    print("\n## doctrine §6 P1 decision (based on B = after)\n")
    for t in classify(db):
        print(f"- {t}")
    print()
    return 0


def main(argv: list[str]) -> int:
    if len(argv) == 2:
        return print_single(Path(argv[1]))
    if len(argv) == 3:
        return print_diff(Path(argv[1]), Path(argv[2]))
    print("Usage:\n  pmu-derive.py <run.pmu.json>\n  pmu-derive.py <A.json> <B.json>", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
```

- [ ] **Step 2: 给执行权限**

```bash
chmod +x scripts/perf/pmu-derive.py
```

- [ ] **Step 3: 用 Task 2 产生的 echo 测试 JSON 跑解析**

Run: `python3 scripts/perf/pmu-derive.py bench/results/*-echo.pmu.json`
Expected: 输出 markdown，含 raw counters + derived metrics + decision 区块

如果 `load_counters` 拿不到任何 event：根据 Task 2 Step 4 记录的实际 JSON schema 调整 `walk()` 路径（mperf JSON 可能把事件包在 `counters.<event>` 子 dict 而不是顶层）。修复后重跑。

- [ ] **Step 4: Commit**

```bash
git add scripts/perf/pmu-derive.py
git commit -m "$(cat <<'EOF'
feat(perf): add pmu-derive.py — mperf JSON parser + doctrine §6 P1 classifier

Parses mperf JSON output, computes derived metrics (IPC, BackendStallRate,
L1DMissRate, BranchMispredRate, SIMDDensity), and applies doctrine §6 P1
decision rules to suggest next optimization direction (MemBound /
CoreBound / BadSpec / FrontendBound).

Supports single-run summary and diff mode (baseline vs PGO).

Co-Authored-By: Claude <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: 接入 scripts/test.sh — 新增 `pmu` 子命令

**Files:**
- Modify: `scripts/test.sh`（参考已有 `profile` case 实现）

- [ ] **Step 1: 读 test.sh 现有 profile case 定位插入点**

Run: `grep -n '^\s*profile)' scripts/test.sh`
预期：定位到 `profile)` case，pmu 子命令插入其前后（保持字母序：pmu 在 profile 后）

- [ ] **Step 2: 在 profile case 后追加 pmu case**

读 test.sh 中 profile case 完整代码（约 20 行），照样在其后增加：

```bash
pmu)
    if [[ ${#MODE_ARGS[@]} -eq 0 ]]; then
        log_fail "用法: $0 pmu <test_name> [args...]"
        log_info "例: $0 pmu factor_with_kleinjung"
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
    log_header "PMU 采集 (mperf, GNFS P1.A 事件集)"
    exec "${PROJECT_ROOT}/scripts/perf/pmu-stat.sh" "${_test_bin}" "${MODE_ARGS[@]:1}"
    ;;
```

- [ ] **Step 3: 验证 list/help 自动显示 pmu**

如果 test.sh 的 help 文本不是自动生成，找到 list/help 输出位置追加：
- `pmu <test>` 行：`PMU 细粒度采集 (10 events, mperf)`

- [ ] **Step 4: 跑一个 instant 测试做端到端 smoke**

Run: `./scripts/test.sh pmu small_vector`
Expected:
- 编译完成（do_build 没出错）
- mperf 提示 sudo
- 输出 JSON 文件
- 提示 parse 命令

- [ ] **Step 5: Commit**

```bash
git add scripts/test.sh
git commit -m "$(cat <<'EOF'
feat(perf): wire scripts/test.sh pmu subcommand → pmu-stat.sh

Adds `./scripts/test.sh pmu <test_name>` for one-shot M5 PMU capture
with the GNFS P1.A event set (10 events). Output:
bench/results/<timestamp>-test_<name>.pmu.json.

Parse with: python3 scripts/perf/pmu-derive.py <json>.

Co-Authored-By: Claude <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: 首次实测 — baseline + PGO 双向采集

**Files:**
- Use existing: `build-baseline-release/test_factor_with_kleinjung`, `build-pgo-use/test_factor_with_kleinjung`
- Output: `bench/results/<TS>-test_factor_with_kleinjung-baseline.pmu.json`, `.../-pgo.pmu.json`

- [ ] **Step 1: 验证两个 release build 仍存在**

Run: `ls -lh build-baseline-release/test_factor_with_kleinjung build-pgo-use/test_factor_with_kleinjung`
Expected: 两个二进制都在。如缺失，重建（参考 `bench/results/2026-05-12-pgo-impact.md` "Reproduce" 段）。

- [ ] **Step 2: 采集 baseline**

Run: `./scripts/perf/pmu-stat.sh build-baseline-release/test_factor_with_kleinjung`
Expected: 提示 sudo → 完成 → JSON 在 `bench/results/`

记录 JSON 路径。重命名带 `-baseline` 后缀以便识别：

```bash
ts_baseline=$(ls -t bench/results/*-test_factor_with_kleinjung.pmu.json | head -1)
mv "${ts_baseline}" "${ts_baseline%.pmu.json}-baseline.pmu.json"
```

- [ ] **Step 3: 采集 PGO**

Run: `./scripts/perf/pmu-stat.sh build-pgo-use/test_factor_with_kleinjung`
Expected: 同上，新 JSON 文件

```bash
ts_pgo=$(ls -t bench/results/*-test_factor_with_kleinjung.pmu.json | head -1)
mv "${ts_pgo}" "${ts_pgo%.pmu.json}-pgo.pmu.json"
```

- [ ] **Step 4: 跑 diff，得到 markdown**

```bash
python3 scripts/perf/pmu-derive.py \
    bench/results/*test_factor_with_kleinjung-baseline.pmu.json \
    bench/results/*test_factor_with_kleinjung-pgo.pmu.json \
    | tee bench/results/pmu-diff-raw.md
```

Expected: 完整 markdown 报告，含 raw delta + derived delta + P1 decision

- [ ] **Step 5: Commit JSON + raw diff**

```bash
git add bench/results/*test_factor_with_kleinjung-baseline.pmu.json \
        bench/results/*test_factor_with_kleinjung-pgo.pmu.json \
        bench/results/pmu-diff-raw.md
git commit -m "$(cat <<'EOF'
chore(bench): capture baseline + PGO PMU traces (10-event M5 set)

Two raw mperf JSON outputs + auto-generated diff markdown. Source for
the 2026-05-13-pmu-deepening report.

Co-Authored-By: Claude <noreply@anthropic.com>
EOF
)"
```

注：raw JSON 文件大小预计 <20K（mperf 是 summary，不是 sampling），保留入 git 是合理的——便于历史回溯。如果实际 >100K 则改入 .gitignore。

---

## Task 6: 写 P1.A 完整报告 + 给 P1.B 结论

**Files:**
- Create: `bench/results/2026-05-13-pmu-deepening.md`

- [ ] **Step 1: 基于 Task 5 的 diff 撰写完整报告**

模板结构（每节按实测数据填写）：

```markdown
# PMU Deepening Report — test_factor_with_kleinjung (M5)

**Date:** 2026-05-13
**Host:** Apple M5 (4P+6E, 4.61 GHz)
**Tool:** mperf (private kperf framework, MIT)
**Event database:** /usr/share/kpep/as5.plist (M5 native)
**Builds:**
- baseline: `-O3 -mcpu=native -flto=thin`
- PGO:     `-O3 -mcpu=native -flto=thin -fprofile-instr-use=merged.profdata`

## Why deeper events than the 2026-05-12 report

The first PGO measurement used xctrace "CPU Counters" template, which on
M-series aggregates to 4 columns (Discarded/Processing/Delivery/Cycles).
**Processing dominated at 80%+** but that column conflates MemBound and
CoreBound, blocking doctrine §6 P1 direction selection.

Switching to mperf + as5.plist's 135 events lets us separate:
- Backend stall (MemBound + CoreBound joint)
- Frontend stall (取指/解码瓶颈)
- L1D / TLB miss rates
- Branch mispred rate
- SIMD density

## Raw counters

(Table from `pmu-derive.py` diff output — copy/edit Task 5 Step 4 result)

## Derived metrics

(Table from `pmu-derive.py`)

## doctrine §6 P1 decision

**Triggered category:** _MemBound / CoreBound / BadSpec / FrontendBound_ (填实际)

**Rationale:** (基于具体百分比说明为什么这条路径触发)

**P1.B 行动建议:**
- (按触发的类别，从 doctrine §6 P1 列表选 2-3 个具体行动)
- 例：MemBound 触发 → 优先 `__builtin_prefetch` 审计 Block Lanczos SpMV 内层循环
- 例：CoreBound 触发 → 优先 NEON 全面化 lattice_sieve::scatter_bucket

## Comparison with 2026-05-12 PGO report

(交叉验证: wall time 改善 -2% 是否 align with 这次的 derived metric Δ)

## Reproduce

\`\`\`bash
./scripts/perf/install-mperf.sh                                    # 一次
./scripts/perf/pmu-stat.sh build-baseline-release/test_factor_with_kleinjung
./scripts/perf/pmu-stat.sh build-pgo-use/test_factor_with_kleinjung
python3 scripts/perf/pmu-derive.py <baseline.json> <pgo.json>
\`\`\`

## Limitations

- mperf 用 PET sampling 近似，counters always physically active（无 multiplexing）
- 单次运行，未做多轮取中位数（Task 5 是 single-shot；如果需要可加循环）
- factor_with_kleinjung 是异构 pipeline，单一类别触发可能掩盖某模块的特异瓶颈
```

实际写入时把所有 _填实际_ 替换成 Task 5 真实数据，并按触发类别给出明确的下一步行动。

- [ ] **Step 2: 删除 Task 5 的 pmu-diff-raw.md 中间文件**

```bash
git rm bench/results/pmu-diff-raw.md
```

- [ ] **Step 3: Commit**

```bash
git add bench/results/2026-05-13-pmu-deepening.md
git commit -m "$(cat <<'EOF'
docs(perf): P1.A PMU deepening report — first data-driven direction for P1.B

10-event M5 trace (FIXED_CYCLES, ARM_STALL_BACKEND/FRONTEND, L1D_CACHE_MISS_LD,
L1D_TLB_MISS, BRANCH_COND_MISPRED_NONSPEC, INST_SIMD_ALU, ...) on
test_factor_with_kleinjung, baseline vs PGO.

P1.B decision: <填实际触发类别>.

Co-Authored-By: Claude <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: 更新 doctrine §6 + 附录 A

**Files:**
- Modify: `docs/perf/performance-doctrine.md`（§6 P1 区块 + 附录 A）

- [ ] **Step 1: §6 标 P1.A 完成 + 引用报告**

定位 `### P1 — 数据驱动的下一阶段` 这一节，把它分成 P1.A（已完成）和 P1.B（下一步），格式参考已有 P0 区块（带 commit hash + 报告路径）。

P1.A 区块用最新 commit hash 填充，引 `bench/results/2026-05-13-pmu-deepening.md`。

P1.B 区块基于报告结论锁定**具体优化任务**（具体到模块/函数）。

- [ ] **Step 2: 附录 A 表更新 — 用 M5 实际事件名替换占位**

定位附录 A 表（约 line 859），用 `/usr/share/kpep/as5.plist` 实际事件名修正（很多已经一致，少量需改，如 `BRANCH_COND_MISPRED_NONSPEC` 是 as5 上的真实名称）。

加一段 footnote：

```markdown
> **M5 实测**: 上表事件名取自 `/usr/share/kpep/as5.plist`（macOS 25.4 Darwin 25.4.0 自带）。
> 完整 135 事件清单导出: `plutil -convert xml1 -o - /usr/share/kpep/as5.plist | grep -oE '<key>[A-Z][A-Z_0-9]+</key>'`
```

- [ ] **Step 3: Commit**

```bash
git add docs/perf/performance-doctrine.md
git commit -m "$(cat <<'EOF'
docs(perf): mark P1.A complete + lock P1.B specific targets

P1.A delivered: scripts/perf/{install-mperf,pmu-stat}.sh,
scripts/perf/pmu-derive.py, test.sh pmu subcommand, 10-event M5 trace,
2026-05-13-pmu-deepening.md report.

P1.B targets (data-driven from report):
- <具体模块/函数 1>
- <具体模块/函数 2>

Appendix A: PMU event table updated with M5 (as5.plist) actual names.

Co-Authored-By: Claude <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: 验证 + 合并到 main

- [ ] **Step 1: 跑 smoke + gate 确保未回归**

```bash
./scripts/test.sh smoke
./scripts/test.sh gate
```

Expected: 26/26 + 27/27 PASS（基础设施改动不会影响测试代码）

如有失败，停止 + 调查（极不应该发生，因为这次没碰 C++ 代码）。

- [ ] **Step 2: 使用 finishing-a-development-branch skill 完成合并**

按 skill 流程：
1. 验证测试通过 ✅ (Step 1)
2. 检测 repo 状态（normal repo: GIT_DIR == GIT_COMMON）
3. 呈现 4 options → 用户上一阶段已经偏好「本地 merge 到 main」
4. 执行 `git checkout main && git merge --no-ff feat/260513-pmu-events-deepening`
5. **保留分支不删除**（CLAUDE.md 项目规则覆盖 skill 默认）
6. `git push origin main`

- [ ] **Step 3: 验证 push 成功 + 最终 smoke**

```bash
git log --oneline -3 main
./scripts/test.sh smoke
```

Expected: merge commit 出现 + smoke 全过

---

## 风险 / 已知未知

1. **mperf JSON schema** 在 Task 2 之前未实际见过 — `pmu-derive.py` 的 `load_counters` 是防御式 walk，但如果实际 schema 极不规则需要在 Task 2 Step 4 后微调 Task 3。
2. **sudo 频次**：每次跑 pmu 都要 sudo。可以一次性 `sudo -v` 缓存凭证 5 分钟，但 P1.A 只需 2 次采集，可接受。
3. **多轮 vs 单次**：Task 5 是单次采集。如果发现 run-to-run variance 大（看 Cycles 变动 >5%），加 wrapping 3 次取中位数。
4. **M5 是否有 entitlement 限制**：未亲测过 sudo + kperf 在 macOS 25.4 (Darwin 25.4.0) 上是否需要禁用 SIP。如 Task 1 Step 4 失败，需在 findings.md 记录并研究替代（如 hutchison `superpmc` 或自写 wrapper）。
5. **报告样本只有 factor_with_kleinjung**：作为 P1.A 一次性 baseline 够；P1.B 实施时需对具体模块单独采集（如 SpMV 单元测试 / lattice_sieve micro-benchmark）。

---

## Self-Review

- [x] **Spec coverage**: P1.A 全部覆盖 — 安装 (Task 1) → wrapper (Task 2) → 解析 (Task 3) → CLI 集成 (Task 4) → 实测 (Task 5) → 报告 (Task 6) → doctrine 更新 (Task 7) → 合并 (Task 8)
- [x] **No placeholders**: 每步都有完整代码 / 命令 / 预期输出；`<填实际>` 占位仅用于报告中需基于实测填写的部分（已明示）
- [x] **Type consistency**: `EVENTS` 在 pmu-stat.sh 中和 `EVENT_KEYS` 在 pmu-derive.py 中保持一致顺序与名称
- [x] **Bite-sized**: 每个 step 2-5 分钟，含独立的运行 + 验证
- [x] **TDD-ish**: 每个脚本步骤都有验证 step（smoke run + 检查输出）
- [x] **Frequent commits**: 每个 Task 末尾 commit，符合一 task 一 commit

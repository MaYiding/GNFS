#!/usr/bin/env zsh
# parse-time-l.sh — 解析 macOS /usr/bin/time -l 输出, 生成 markdown table
#
# 用法:
#   ./scripts/perf/parse-time-l.sh <log_file> [label]
#
# 把 log 末尾 `time -l` 输出块抽取出来, 转成 markdown table, 加几列
# 派生指标 (GiB / IPC / parallelism). 写到 stdout, 适合 pipe 进 baseline
# report 的 §3.1 section.
#
# 输入示例 (log 末尾应该包含):
#      7142.76 real     30463.25 user        85.05 sys
#           2183331840  maximum resident set size
#              2871200  page reclaims
#                   93  page faults
#                    0  swaps
#                  217  voluntary context switches
#             15140213  involuntary context switches
#      153089526230138  instructions retired
#      115783714680624  cycles elapsed
#           2826258856  peak memory footprint

set -euo pipefail

LOG="${1:-}"
LABEL="${2:-time -l summary}"

if [[ -z "$LOG" ]]; then
    print -u2 "Usage: $0 <log_file> [label]"
    print -u2 "       Parse macOS '/usr/bin/time -l' output to markdown table."
    exit 1
fi

if [[ ! -f "$LOG" ]]; then
    print -u2 "Error: file not found: $LOG"
    exit 1
fi

# Find the LAST `time -l` block. 锚: 含 'real' 'user' 'sys' 的单行,
# 后面跟一系列 `<num>  <metric>` 行. 抽取 real 行 + 后续 20 行已足够.
# grep 无匹配返回 1, 用 || true 避免 set -e 提前 abort, 我们自己报错.
local block_line
block_line=$(grep -nE '^[[:space:]]+[0-9]+\.[0-9]+[[:space:]]+real' "$LOG" 2>/dev/null | tail -1) || true

if [[ -z "$block_line" ]]; then
    print -u2 "Error: no 'time -l' block found in $LOG"
    print -u2 "       expected line matching: '<real> real <user> user <sys> sys'"
    print -u2 "       (macOS /usr/bin/time -l format. GNU 'time -v' 格式尚不支持.)"
    exit 1
fi

# block_line = "linenum:content" — 拿 linenum 抽出 25 行 (cover 全 time -l 输出)
local start_line=${block_line%%:*}
local -a body
body=("${(@f)$(tail -n +"$start_line" "$LOG" | head -25)}")

# Field 1: real/user/sys 同行
local real_user_sys="${body[1]}"
local real_s user_s sys_s
real_s=$(print "$real_user_sys" | awk '{print $1}')
user_s=$(print "$real_user_sys" | awk '{print $3}')
sys_s=$(print "$real_user_sys" | awk '{print $5}')

# 后续行: <value>  <metric...> 用 awk 提 first field + rest
declare -A m
local line value metric
for line in "${body[@]:1}"; do
    [[ "$line" =~ ^[[:space:]]*$ ]] && continue
    value=$(print "$line" | awk '{print $1}')
    metric=$(print "$line" | awk '{$1=""; sub(/^[[:space:]]+/,""); print}')
    [[ -z "$value" || -z "$metric" ]] && continue
    [[ ! "$value" =~ ^[0-9]+$ ]] && continue
    m[$metric]="$value"
done

# 抽具体字段, 缺省 '?'
local max_rss="${m[maximum resident set size]:-?}"
local peak_fp="${m[peak memory footprint]:-?}"
local page_reclaims="${m[page reclaims]:-?}"
local page_faults="${m[page faults]:-?}"
local swaps="${m[swaps]:-?}"
local vol_csw="${m[voluntary context switches]:-?}"
local invol_csw="${m[involuntary context switches]:-?}"
local instructions="${m[instructions retired]:-?}"
local cycles="${m[cycles elapsed]:-?}"

# 派生
local max_rss_gib="?" peak_fp_gib="?" ipc="?" parallelism="?" real_min="?"
[[ "$max_rss" != "?" ]] && max_rss_gib=$(awk -v b="$max_rss" 'BEGIN {printf "%.2f", b/1073741824}')
[[ "$peak_fp" != "?" ]] && peak_fp_gib=$(awk -v b="$peak_fp" 'BEGIN {printf "%.2f", b/1073741824}')
[[ "$instructions" != "?" && "$cycles" != "?" && "$cycles" != "0" ]] && \
    ipc=$(awk -v i="$instructions" -v c="$cycles" 'BEGIN {printf "%.2f", i/c}')
[[ -n "$user_s" && -n "$real_s" ]] && \
    parallelism=$(awk -v u="$user_s" -v r="$real_s" 'BEGIN {printf "%.2fx", u/r}')
[[ -n "$real_s" ]] && \
    real_min=$(awk -v r="$real_s" 'BEGIN {printf "%.1f", r/60}')

# 输出 markdown
cat <<EOF
### $LABEL

\`\`\`
$(print "$real_user_sys" | sed 's/^[[:space:]]*//')
$(for line in "${body[@]:1}"; do
    [[ "$line" =~ ^[[:space:]]*[0-9] ]] && print "$line"
done | sed 's/^[[:space:]]*//')
\`\`\`

| Metric | Value | Note |
|---|---:|---|
| Real time | ${real_s} s | ≈ ${real_min} min wall clock |
| User time | ${user_s} s | ${parallelism} parallelism |
| Sys time | ${sys_s} s | kernel overhead |
| **Max RSS** | **${max_rss_gib} GiB** | ${max_rss} bytes — resident peak |
| **Peak footprint** | **${peak_fp_gib} GiB** | ${peak_fp} bytes — macOS extended |
| Page reclaims | ${page_reclaims} | typical malloc-heavy |
| Page faults | ${page_faults} | hard faults |
| Swaps | ${swaps} | $([[ "$swaps" == "0" ]] && echo "well under RAM limit" || echo "memory pressure detected") |
| Vol context switches | ${vol_csw} | |
| Invol context switches | ${invol_csw} | |
| Instructions retired | ${instructions} | |
| Cycles elapsed | ${cycles} | |
| IPC | ${ipc} | inst/cyc — typical OOO ARM64 |
EOF

#!/bin/bash
# Stop Hook: TODO.md Completion Check
#
# Behavior:
#   - If TODO.md doesn't exist or is empty → allow stop
#   - If all TODO.md items are done → allow stop
#   - If incomplete items exist AND this is the first block → block + inject /compact instruction
#   - If stop_hook_active=true (already blocked once this turn) → allow stop (anti-loop guard)
#
# stop_hook_active semantics:
#   false = first time this hook fires for this stop attempt → OK to block
#   true  = a previous Stop hook already blocked → must allow to prevent infinite loops
#
# Exit codes:
#   0 = allow stop (or JSON decision controls behavior)
#   2 = block stop (reason fed back to Claude)

set -euo pipefail

# Read stdin JSON
INPUT=$(cat)

# ── Anti-loop guard ──────────────────────────────────────────────
STOP_HOOK_ACTIVE=$(echo "$INPUT" | python3 -c "
import json, sys
try:
    data = json.load(sys.stdin)
    print('true' if data.get('stop_hook_active', False) else 'false')
except Exception:
    print('false')
" 2>/dev/null || echo "false")

if [ "$STOP_HOOK_ACTIVE" = "true" ]; then
  # Already blocked once this turn — allow stop to prevent infinite loops
  exit 0
fi

# ── Find project root ────────────────────────────────────────────
CWD=$(echo "$INPUT" | python3 -c "
import json, sys
try:
    data = json.load(sys.stdin)
    print(data.get('cwd', '.'))
except Exception:
    print('.')
" 2>/dev/null || echo ".")

TODO_FILE="${CWD}/TODO.md"

# ── Check TODO.md existence ──────────────────────────────────────
if [ ! -f "$TODO_FILE" ]; then
  exit 0
fi

# ── Parse TODO items ─────────────────────────────────────────────
# Recognized formats:
#   Incomplete: - [ ] text  OR  * [ ] text  OR  1. [ ] text
#   Complete:   - [x] text  OR  * [x] text  OR  1. [x] text  (case-insensitive x)
# Lines starting with # are headers (ignored)
# Empty lines are ignored

INCOMPLETE=$(grep -cE '^\s*[-*+]\s\[[ ]\]' "$TODO_FILE" 2>/dev/null || true)
COMPLETE=$(grep -cE '^\s*[-*+]\s\[[xX]\]' "$TODO_FILE" 2>/dev/null || true)
# Also count numbered list items: 1. [ ] task
INCOMPLETE_NUM=$(grep -cE '^\s*[0-9]+\.\s\[[ ]\]' "$TODO_FILE" 2>/dev/null || true)
COMPLETE_NUM=$(grep -cE '^\s*[0-9]+\.\s\[[xX]\]' "$TODO_FILE" 2>/dev/null || true)

# Ensure numeric (grep -c outputs "0" on no match, || true prevents set -e exit)
[[ "$INCOMPLETE" =~ ^[0-9]+$ ]] || INCOMPLETE=0
[[ "$COMPLETE" =~ ^[0-9]+$ ]] || COMPLETE=0
[[ "$INCOMPLETE_NUM" =~ ^[0-9]+$ ]] || INCOMPLETE_NUM=0
[[ "$COMPLETE_NUM" =~ ^[0-9]+$ ]] || COMPLETE_NUM=0

INCOMPLETE=$((INCOMPLETE + INCOMPLETE_NUM))
COMPLETE=$((COMPLETE + COMPLETE_NUM))

# ── Decision ─────────────────────────────────────────────────────
if [ "$INCOMPLETE" -eq 0 ]; then
  # All items done (or no items) — allow stop
  exit 0
fi

# Extract incomplete item summaries (first 80 chars each, max 10 items)
INCOMPLETE_ITEMS=$(grep -E '^\s*[-*+0-9.]+\s\[[ ]\]' "$TODO_FILE" 2>/dev/null | head -10 | sed 's/^[[:space:]]*//' | cut -c1-100)

REASON="【Stop Hook】TODO.md 中仍有 ${INCOMPLETE} 项未完成（已完成 ${COMPLETE} 项）。

未完成项目：
${INCOMPLETE_ITEMS}

请先使用 /compact 压缩上下文（如果上下文较大），然后继续解决 TODO.md 中的未完成项目。
完成一项后将对应条目标记为 [x]，全部完成后再结束。"

echo "{\"decision\": \"block\", \"reason\": $(echo "$REASON" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))')}"
exit 0

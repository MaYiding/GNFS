#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
HOOK="${PROJECT_ROOT}/.claude/hooks/stop-todo-check.sh"

TMPDIR=$(mktemp -d)
cleanup() {
    rm -rf "${TMPDIR}"
}
trap cleanup EXIT

fail() {
    echo "[FAIL] $*" >&2
    exit 1
}

pass() {
    echo "[PASS] $*"
}

run_hook() {
    local cwd="$1"
    local active="$2"
    local out_file="$3"
    printf '{"cwd": "%s", "stop_hook_active": %s}' "${cwd}" "${active}" | "${HOOK}" > "${out_file}"
}

assert_allow() {
    local name="$1"
    local cwd="$2"
    local active="${3:-false}"
    local out="${TMPDIR}/${name}.out"

    run_hook "${cwd}" "${active}" "${out}" || fail "${name}: hook exited non-zero"
    if [ -s "${out}" ]; then
        fail "${name}: expected allow with empty stdout, got: $(cat "${out}")"
    fi
    pass "${name}"
}

assert_block() {
    local name="$1"
    local cwd="$2"
    local active="${3:-false}"
    local expected_count="$4"
    local out="${TMPDIR}/${name}.out"

    run_hook "${cwd}" "${active}" "${out}" || fail "${name}: hook exited non-zero"
    python3 - "${out}" "${expected_count}" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
expected_count = sys.argv[2]
payload = json.loads(path.read_text())
if payload.get("decision") != "block":
    raise SystemExit(f"expected block decision, got {payload!r}")
reason = payload.get("reason", "")
if f"TODO.md 中仍有 {expected_count} 项未完成" not in reason:
    raise SystemExit(f"missing expected count in reason: {reason!r}")
if "未完成项目" not in reason:
    raise SystemExit(f"missing item list in reason: {reason!r}")
PY
    pass "${name}"
}

mkdir -p "${TMPDIR}/absent"
assert_allow "missing TODO.md allows stop" "${TMPDIR}/absent"

mkdir -p "${TMPDIR}/complete"
cat > "${TMPDIR}/complete/TODO.md" <<'EOF'
# TODO

stop_hook_prevent_infinite_loop: true

- [x] done
1. [X] also done
EOF
assert_allow "complete TODO.md allows stop" "${TMPDIR}/complete"

mkdir -p "${TMPDIR}/default_guard"
cat > "${TMPDIR}/default_guard/TODO.md" <<'EOF'
# TODO

- [ ] finish relation trim audit
1. [ ] run hook tests
- [x] existing finished item
EOF
assert_block "incomplete TODO.md blocks first stop" "${TMPDIR}/default_guard" false 2
assert_allow "default guard allows second stop" "${TMPDIR}/default_guard" true

mkdir -p "${TMPDIR}/strict"
cat > "${TMPDIR}/strict/TODO.md" <<'EOF'
# TODO

stop_hook_prevent_infinite_loop: false

- [ ] finish the user-controlled task
EOF
assert_block "disabled guard blocks second stop" "${TMPDIR}/strict" true 1

mkdir -p "${TMPDIR}/alias"
cat > "${TMPDIR}/alias/TODO.md" <<'EOF'
# TODO

STOP_HOOK_PREVENT_INFINITE_LOOP=false

* [ ] support env-style config aliases
EOF
assert_block "uppercase env-style config disables guard" "${TMPDIR}/alias" true 1

echo "Stop TODO hook tests passed."

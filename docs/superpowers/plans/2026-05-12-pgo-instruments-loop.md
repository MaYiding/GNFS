# PGO + Instruments Closed-Loop Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the project's first profile-guided optimization (PGO) workflow and Instruments-based PMU measurement infrastructure on Apple M5. Expose them as two new subcommands: `./scripts/test.sh pgo-train` and `./scripts/test.sh profile <test>`. Produce a baseline-vs-PGO impact report for `test_factor_with_kleinjung`.

**Architecture:** Three layers — (1) CMake options for Clang PGO generate/use builds, written to `build-pgo-gen/` and `build-pgo-use/` so they coexist with the normal `build/`; (2) Shell scripts orchestrating instrumented build → training runs → `llvm-profdata` merge → optimized build, plus `xctrace record/export` for PMU capture; (3) Python parser that turns xctrace XML into a derived-metric table (IPC, L1D miss rate, branch misprediction rate, iTLB miss rate) with a diff mode. Two new `test.sh` subcommands wire it together. The first measurement is the baseline-vs-PGO diff for `test_factor_with_kleinjung` (~30-bit, ~10 s wall time, full GNFS pipeline).

**Tech Stack:** Clang/LLVM (`-fprofile-instr-generate/use`, `llvm-profdata`), `xctrace` + Instruments "CPU Counters" template, Python 3 stdlib (`xml.etree.ElementTree`), CMake 3.20+, zsh.

**Tracked artifacts (will be created):**
- `bench/results/` — trace outputs (gitignored)
- `build-pgo-gen/`, `build-pgo-use/` — alternate CMake build dirs (gitignored via existing `build-*/`)
- `scripts/pgo-train.sh`
- `scripts/perf/profile-cpu.sh`
- `scripts/perf/parse-trace.py`
- `bench/results/2026-05-12-pgo-impact.md` — first impact report (gitignored binary, but `.md` report tracked)

**Self-check before merging to main:** The whole pipeline must run end-to-end in one command (`./scripts/test.sh pgo-train` succeeds, `./scripts/test.sh profile factor_with_kleinjung` succeeds, `parse-trace.py` emits a non-empty markdown table). PGO impact must be measurable (not crash, not regress wall time more than 2%).

---

## File Structure

Files to create or modify, with the single responsibility of each:

| File | Action | Responsibility |
|------|--------|----------------|
| `.gitignore` | modify | Add `bench/results/*.trace*`, `bench/results/*.xml`, and explicit `pgo-profiles/` entries so PGO and trace artifacts don't pollute git |
| `bench/results/.gitkeep` | create | Anchor the `bench/results/` directory in git so the parser scripts can `mkdir -p` is unnecessary |
| `CMakeLists.txt` | modify | Add `GNFS_ENABLE_PGO_GEN`, `GNFS_ENABLE_PGO_USE`, `GNFS_PGO_PROFILE_DIR` options. Inject `-fprofile-instr-generate` or `-fprofile-instr-use=...` accordingly. Hard-error if both flags are set or if compiler isn't Clang |
| `scripts/pgo-train.sh` | create | Orchestrate the four PGO phases: instrumented build → training run → `llvm-profdata merge` → optimized build |
| `scripts/perf/profile-cpu.sh` | create | Wrap `xctrace record` against the Apple-provided CPU Counters template; export trace to XML |
| `scripts/perf/parse-trace.py` | create | Parse xctrace XML, compute derived metrics, support single-trace summary and two-trace diff |
| `scripts/test.sh` | modify | Add two new subcommands (`pgo-train`, `profile`) to the main `case` block; add them to the `--help` and `list` output |
| `bench/results/2026-05-12-pgo-impact.md` | create | First impact report (baseline vs PGO derived metrics) — produced by running the pipeline in Task 7 |

**Boundary notes:**
- `pgo-train.sh` only does PGO. `profile-cpu.sh` only does xctrace. `parse-trace.py` only does XML→Markdown. Each script has one clear job; they compose via shell pipes and shared filesystem paths.
- `parse-trace.py` is intentionally stdlib-only (no `pandas`/`lxml`). It runs against whatever Python `python3` resolves to on macOS (≥3.9 ships with Xcode CLT).
- `scripts/test.sh` is not refactored — only two case branches added, plus help text.

---

### Task 1: Prep gitignore and bench/results/ skeleton

**Files:**
- Modify: `.gitignore` (append new section)
- Create: `bench/results/.gitkeep` (empty file)

- [ ] **Step 1.1: Inspect current gitignore tail to find insertion point**

Run:
```bash
tail -15 /Users/mayiding/Desktop/GitMy/GNFS/.gitignore
```
Expected: Output ending with the "Obsolete root documentation" block. Confirm `build-*/` is already there (line 3 — it already covers `build-pgo-gen/` and `build-pgo-use/`).

- [ ] **Step 1.2: Add bench/results/ exclusions to .gitignore**

Append this block to `/Users/mayiding/Desktop/GitMy/GNFS/.gitignore`:

```gitignore

# ===== Performance Profiling Artifacts =====
# Trace binaries are large and machine-specific; reports (.md) ARE tracked.
bench/results/*.trace
bench/results/*.trace/
bench/results/*.xml
bench/results/*.profraw
bench/results/*.profdata

# PGO profile data (when produced outside build-*/ dirs)
pgo-profiles/
*.profraw
*.profdata
```

Use the Edit tool with `old_string` matching the final `Testing/` line (last line of the file) and `new_string` adding the block after it.

- [ ] **Step 1.3: Create bench/results/.gitkeep to anchor the directory**

Run:
```bash
mkdir -p /Users/mayiding/Desktop/GitMy/GNFS/bench/results
touch /Users/mayiding/Desktop/GitMy/GNFS/bench/results/.gitkeep
```
Expected: No output, both `bench/` and `bench/results/` directories exist.

- [ ] **Step 1.4: Verify gitignore correctness**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
touch bench/results/test.trace
git check-ignore -v bench/results/test.trace bench/results/.gitkeep
rm bench/results/test.trace
```
Expected: `bench/results/test.trace` is matched by `bench/results/*.trace`, `bench/results/.gitkeep` is NOT ignored (no line printed for it).

- [ ] **Step 1.5: Commit**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
git add .gitignore bench/results/.gitkeep
git status
git commit -m "$(cat <<'EOF'
chore(perf): add bench/results dir and gitignore for PGO/trace artifacts

Prepare directory structure for the upcoming PGO + Instruments closed-loop
infrastructure (see docs/perf/performance-doctrine.md §5).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```
Expected: Commit succeeds with hash printed.

---

### Task 2: Add PGO options to CMakeLists.txt

**Files:**
- Modify: `CMakeLists.txt:58-62` (insert a new PGO block immediately after the existing sanitizer block, before `set(CMAKE_EXPORT_COMPILE_COMMANDS ON)`)

**Background:** The doctrine (`docs/perf/performance-doctrine.md` §5.2) specifies these exact options. Clang on Apple Silicon uses `-fprofile-instr-generate` (frontend, source-based) rather than `-fprofile-generate` (IR-based) because the former integrates cleanly with `xcrun llvm-profdata merge` and survives `-flto=thin`. We keep `-flto=thin` enabled in both PGO phases.

- [ ] **Step 2.1: Verify the sanitizer block location**

Run:
```bash
grep -n "GNFS_ENABLE_UBSAN\|CMAKE_EXPORT_COMPILE_COMMANDS" /Users/mayiding/Desktop/GitMy/GNFS/CMakeLists.txt
```
Expected: `GNFS_ENABLE_UBSAN` near line 56, `CMAKE_EXPORT_COMPILE_COMMANDS` near line 61. The PGO block goes between them (after line 58 ends the UBSAN if-block).

- [ ] **Step 2.2: Insert the PGO option block**

Use the Edit tool. `old_string`:
```
if(GNFS_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined)
    add_link_options(-fsanitize=undefined)
endif()

# 导出编译命令（用于 IDE 和 clangd）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
```

`new_string`:
```
if(GNFS_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined)
    add_link_options(-fsanitize=undefined)
endif()

# ============================================================
# Profile-Guided Optimization (PGO)
# ============================================================
# 两阶段 PGO 工作流（详见 docs/perf/performance-doctrine.md §5.2）:
#   阶段 1 (训练): -DGNFS_ENABLE_PGO_GEN=ON, 跑训练样本生成 .profraw
#   阶段 2 (优化): -DGNFS_ENABLE_PGO_USE=ON, 消费 merged.profdata 重编
# 自动化入口: scripts/pgo-train.sh
option(GNFS_ENABLE_PGO_GEN "Enable PGO instrumentation (training run)"   OFF)
option(GNFS_ENABLE_PGO_USE "Enable PGO optimized build (consume profile)" OFF)
set(GNFS_PGO_PROFILE_DIR "${CMAKE_BINARY_DIR}/pgo-profiles"
    CACHE PATH "Directory for PGO .profraw / merged.profdata")

if(GNFS_ENABLE_PGO_GEN AND GNFS_ENABLE_PGO_USE)
    message(FATAL_ERROR
        "GNFS PGO: cannot enable both GNFS_ENABLE_PGO_GEN and GNFS_ENABLE_PGO_USE. "
        "Use separate build directories for the two phases.")
endif()

if(GNFS_ENABLE_PGO_GEN OR GNFS_ENABLE_PGO_USE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "GNFS PGO requires Clang (got ${CMAKE_CXX_COMPILER_ID})")
    endif()
endif()

if(GNFS_ENABLE_PGO_GEN)
    file(MAKE_DIRECTORY ${GNFS_PGO_PROFILE_DIR})
    add_compile_options(-fprofile-instr-generate)
    add_link_options(-fprofile-instr-generate)
    message(STATUS "GNFS PGO: instrumentation enabled, profiles -> ${GNFS_PGO_PROFILE_DIR}")
endif()

if(GNFS_ENABLE_PGO_USE)
    set(_pgo_data "${GNFS_PGO_PROFILE_DIR}/merged.profdata")
    if(NOT EXISTS ${_pgo_data})
        message(FATAL_ERROR
            "GNFS PGO_USE: profile not found at ${_pgo_data}. "
            "Run 'scripts/pgo-train.sh' first, or set GNFS_PGO_PROFILE_DIR to a populated dir.")
    endif()
    add_compile_options(-fprofile-instr-use=${_pgo_data})
    add_link_options(-fprofile-instr-use=${_pgo_data})
    # 防止小代码改动后强失败 — PGO 数据稍旧仍可用
    add_compile_options(-Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled)
    message(STATUS "GNFS PGO: consuming profile ${_pgo_data}")
endif()

# 导出编译命令（用于 IDE 和 clangd）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
```

- [ ] **Step 2.3: Verify CMake parses cleanly with both options off (no regression)**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
rm -rf build-pgo-test
cmake -B build-pgo-test -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -20
```
Expected: Normal CMake configure output, no FATAL_ERROR, no mention of PGO (because both options default OFF). Last lines should include `-- Configuring done` and `-- Generating done`.

- [ ] **Step 2.4: Verify mutex error fires when both options ON**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
rm -rf build-pgo-test
cmake -B build-pgo-test -DCMAKE_BUILD_TYPE=Release -DGNFS_ENABLE_PGO_GEN=ON -DGNFS_ENABLE_PGO_USE=ON 2>&1 | tail -5
```
Expected: `CMake Error at CMakeLists.txt:...:` containing `cannot enable both GNFS_ENABLE_PGO_GEN and GNFS_ENABLE_PGO_USE`. Exit code non-zero.

- [ ] **Step 2.5: Verify PGO_GEN-only configures cleanly**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
rm -rf build-pgo-test
cmake -B build-pgo-test -DCMAKE_BUILD_TYPE=Release -DGNFS_ENABLE_PGO_GEN=ON 2>&1 | tail -10
```
Expected: `-- GNFS PGO: instrumentation enabled, profiles -> .../build-pgo-test/pgo-profiles`, configure succeeds.

- [ ] **Step 2.6: Verify PGO_USE without profile fires error**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
rm -rf build-pgo-test
cmake -B build-pgo-test -DCMAKE_BUILD_TYPE=Release -DGNFS_ENABLE_PGO_USE=ON 2>&1 | tail -5
```
Expected: `CMake Error` containing `profile not found at .../merged.profdata`. Exit code non-zero.

- [ ] **Step 2.7: Cleanup test build dir and commit**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
rm -rf build-pgo-test
git status
git add CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(cmake): add PGO_GEN/PGO_USE options for Clang profile-guided optimization

- GNFS_ENABLE_PGO_GEN: emit -fprofile-instr-generate for training builds
- GNFS_ENABLE_PGO_USE: consume merged.profdata via -fprofile-instr-use
- Mutex check + Clang-only guard + missing-profile guard
- Profile dir default: ${CMAKE_BINARY_DIR}/pgo-profiles

Wired up to scripts/pgo-train.sh (next commit). See doctrine §5.2.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```
Expected: Commit succeeds.

---

### Task 3: Create scripts/pgo-train.sh

**Files:**
- Create: `scripts/pgo-train.sh` (executable)

**Background:** This script automates the doctrine §5.3 four-phase workflow. Training samples are deliberately chosen to exercise three hot code paths: full GNFS pipeline (`test_factor_with_kleinjung` — polynomial selection, factor base, sieve, cofactor, relations, linalg, sqrt), sieve isolation (`test_lattice_sieve`), and linear algebra isolation (`test_linalg`). All three are `slow` tier per `scripts/test.sh` but each finishes in <2 min on M5.

Key design points:
- Two CMake build dirs (`build-pgo-gen`, `build-pgo-use`) — separate to avoid CMake cache pollution
- `LLVM_PROFILE_FILE` uses `%m-%p.profraw` (module-pid) so parallel runs don't clobber each other
- `set -euo pipefail` for fail-fast; explicit error messages on missing tools
- Diff-friendly output: each phase printed with `==` separator

- [ ] **Step 3.1: Verify required tools exist**

Run:
```bash
which xcrun cmake make
xcrun llvm-profdata --help 2>&1 | head -3
xcrun --find clang
```
Expected: All commands return paths. `llvm-profdata` shows usage. `clang` path is under `/Applications/Xcode.app/...` or similar.

- [ ] **Step 3.2: Write scripts/pgo-train.sh**

Create `/Users/mayiding/Desktop/GitMy/GNFS/scripts/pgo-train.sh` with the following exact content:

```bash
#!/usr/bin/env zsh
# scripts/pgo-train.sh
# Automate Clang PGO (Profile-Guided Optimization) four-phase workflow:
#   1. Instrumented build  (build-pgo-gen/)
#   2. Training runs       (generate .profraw)
#   3. Profile merge       (llvm-profdata merge -> merged.profdata)
#   4. Optimized build     (build-pgo-use/)
#
# Usage:
#   scripts/pgo-train.sh                  # default training samples
#   scripts/pgo-train.sh --keep            # keep .profraw files after merge
#   scripts/pgo-train.sh --clean           # wipe build-pgo-{gen,use} first
#
# See docs/perf/performance-doctrine.md §5.3 for design rationale.

set -euo pipefail

# Resolve project root (scripts/ -> ..)
SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_GEN="${ROOT}/build-pgo-gen"
BUILD_USE="${ROOT}/build-pgo-use"
PROFILE_DIR="${BUILD_GEN}/pgo-profiles"

KEEP_PROFRAW=0
CLEAN_FIRST=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --keep)  KEEP_PROFRAW=1; shift ;;
        --clean) CLEAN_FIRST=1; shift ;;
        -h|--help)
            sed -n '2,/^set -euo/{ /^#/s/^# \{0,1\}//p }' "$0"
            exit 0 ;;
        *)
            echo "Unknown arg: $1" >&2
            exit 1 ;;
    esac
done

JOBS=$(sysctl -n hw.ncpu)

if (( CLEAN_FIRST )); then
    echo "== Cleaning previous PGO build directories =="
    rm -rf "${BUILD_GEN}" "${BUILD_USE}"
fi

# ----- Sanity checks -----
if ! command -v xcrun >/dev/null 2>&1; then
    echo "ERROR: xcrun not found; install Xcode Command Line Tools" >&2
    exit 1
fi
if ! xcrun llvm-profdata --version >/dev/null 2>&1; then
    echo "ERROR: xcrun llvm-profdata not available" >&2
    exit 1
fi

echo ""
echo "==================== Phase 1/4: Instrumented build ===================="
cmake -B "${BUILD_GEN}" -S "${ROOT}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DGNFS_ENABLE_PGO_GEN=ON \
    -DGNFS_PGO_PROFILE_DIR="${PROFILE_DIR}" \
    > "${BUILD_GEN}/cmake-config.log" 2>&1 || {
        echo "CMake configure failed; see ${BUILD_GEN}/cmake-config.log" >&2
        tail -20 "${BUILD_GEN}/cmake-config.log" >&2
        exit 1
    }
echo "  -- configure ok"

# Only build the training-sample binaries (not all 41 tests; save time)
cmake --build "${BUILD_GEN}" -j"${JOBS}" \
    --target test_factor_with_kleinjung test_lattice_sieve test_linalg \
    > "${BUILD_GEN}/cmake-build.log" 2>&1 || {
        echo "Build failed; see ${BUILD_GEN}/cmake-build.log" >&2
        tail -30 "${BUILD_GEN}/cmake-build.log" >&2
        exit 1
    }
echo "  -- build ok (3 training targets)"

echo ""
echo "==================== Phase 2/4: Training runs ===================="
mkdir -p "${PROFILE_DIR}"
# Module-pid pattern: each invocation gets a unique .profraw
export LLVM_PROFILE_FILE="${PROFILE_DIR}/%m-%p.profraw"

run_training() {
    local name="$1"
    local bin="${BUILD_GEN}/${name}"
    if [[ ! -x "${bin}" ]]; then
        echo "ERROR: training binary not built: ${bin}" >&2
        exit 1
    fi
    echo "  -- training: ${name}"
    local t0=$EPOCHREALTIME
    "${bin}" > "${BUILD_GEN}/${name}.train.log" 2>&1 || {
        echo "ERROR: training run failed for ${name}; see ${BUILD_GEN}/${name}.train.log" >&2
        tail -20 "${BUILD_GEN}/${name}.train.log" >&2
        exit 1
    }
    local t1=$EPOCHREALTIME
    printf "     elapsed: %.1fs\n" $(( t1 - t0 ))
}

run_training test_factor_with_kleinjung
run_training test_lattice_sieve
run_training test_linalg

n_profraw=$(ls -1 "${PROFILE_DIR}"/*.profraw 2>/dev/null | wc -l | tr -d ' ')
echo "  -- collected ${n_profraw} .profraw files in ${PROFILE_DIR}"
if [[ "${n_profraw}" -eq 0 ]]; then
    echo "ERROR: no .profraw produced. Check LLVM_PROFILE_FILE and PGO instrumentation." >&2
    exit 1
fi

echo ""
echo "==================== Phase 3/4: Merge profiles ===================="
xcrun llvm-profdata merge \
    -output="${PROFILE_DIR}/merged.profdata" \
    "${PROFILE_DIR}"/*.profraw
ls -lh "${PROFILE_DIR}/merged.profdata"
echo "  -- merge ok"

if (( KEEP_PROFRAW == 0 )); then
    rm -f "${PROFILE_DIR}"/*.profraw
    echo "  -- cleaned up .profraw (use --keep to retain)"
fi

echo ""
echo "==================== Phase 4/4: PGO-optimized build ===================="
cmake -B "${BUILD_USE}" -S "${ROOT}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DGNFS_ENABLE_PGO_USE=ON \
    -DGNFS_PGO_PROFILE_DIR="${PROFILE_DIR}" \
    > "${BUILD_USE}/cmake-config.log" 2>&1 || {
        echo "CMake configure failed; see ${BUILD_USE}/cmake-config.log" >&2
        tail -20 "${BUILD_USE}/cmake-config.log" >&2
        exit 1
    }
echo "  -- configure ok"

cmake --build "${BUILD_USE}" -j"${JOBS}" \
    > "${BUILD_USE}/cmake-build.log" 2>&1 || {
        echo "PGO-optimized build failed; see ${BUILD_USE}/cmake-build.log" >&2
        tail -30 "${BUILD_USE}/cmake-build.log" >&2
        exit 1
    }
echo "  -- build ok (full 41 test targets)"

echo ""
echo "==================== Done ===================="
echo "Baseline binaries:  ${ROOT}/build/test_*           (-O3 -flto, no PGO)"
echo "PGO binaries:       ${BUILD_USE}/test_*           (-O3 -flto -fprofile-instr-use)"
echo ""
echo "Quick wall-time comparison:"
echo "  time ${ROOT}/build/test_factor_with_kleinjung           # baseline"
echo "  time ${BUILD_USE}/test_factor_with_kleinjung            # PGO"
echo ""
echo "Full PMU diff (via Instruments + parse-trace.py):"
echo "  ./scripts/test.sh profile factor_with_kleinjung        # baseline"
echo "  ./scripts/perf/profile-cpu.sh ${BUILD_USE}/test_factor_with_kleinjung    # PGO"
echo "  python3 scripts/perf/parse-trace.py <baseline.xml> <pgo.xml>"
```

- [ ] **Step 3.3: Make executable**

Run:
```bash
chmod +x /Users/mayiding/Desktop/GitMy/GNFS/scripts/pgo-train.sh
```

- [ ] **Step 3.4: Smoke-test help output**

Run:
```bash
/Users/mayiding/Desktop/GitMy/GNFS/scripts/pgo-train.sh --help
```
Expected: A descriptive comment block printed (lines starting with `# scripts/pgo-train.sh` through the usage description).

- [ ] **Step 3.5: Smoke-test full pipeline (DO NOT skip — this is the real validation)**

This actually runs PGO end-to-end. It takes 5-15 min depending on M5 thermal state.

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
./scripts/pgo-train.sh 2>&1 | tee /tmp/pgo-train-first.log
```
Expected output milestones:
- `Phase 1/4: Instrumented build` → `configure ok` → `build ok (3 training targets)`
- `Phase 2/4: Training runs` → 3 `training: test_X` lines with elapsed seconds
- `Phase 3/4: Merge profiles` → `ls -lh` output showing `merged.profdata` ~1-50 MB
- `Phase 4/4: PGO-optimized build` → `configure ok` → `build ok (full 41 test targets)`
- Final `Done` block.

If any phase fails, the script prints the log path and last 20-30 lines; debug from there.

- [ ] **Step 3.6: Verify the optimized binary actually runs and produces a result**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
/usr/bin/time -p ./build-pgo-use/test_factor_with_kleinjung 2>&1 | tail -10
```
Expected: Test PASS, `real` time printed (in seconds). Should be in the same ballpark as the baseline (within ±20%).

- [ ] **Step 3.7: Commit**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
git add scripts/pgo-train.sh
git commit -m "$(cat <<'EOF'
feat(perf): add scripts/pgo-train.sh — four-phase Clang PGO workflow

Phases: instrumented build -> training runs -> llvm-profdata merge ->
optimized build. Training samples chosen to cover GNFS pipeline +
sieve + linalg hot paths (test_factor_with_kleinjung, test_lattice_sieve,
test_linalg).

Default keeps merged.profdata, deletes intermediate .profraw (--keep
to retain). --clean wipes build-pgo-{gen,use} first.

Run validated: full pipeline completes, build-pgo-use/test_* binaries
execute correctly. See doctrine §5.3.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```
Expected: Commit succeeds.

---

### Task 4: Create scripts/perf/profile-cpu.sh

**Files:**
- Create: `scripts/perf/profile-cpu.sh` (executable)

**Background:** macOS's `xctrace` (shipped with Xcode CLT, located at `/usr/bin/xctrace`) drives Instruments from the command line. The Apple-provided "CPU Counters" template at `/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/Library/Instruments/Templates/CPU Counters.tracetemplate` captures M5 PMU events. We use `xctrace record --launch` to spawn the binary under measurement, then `xctrace export` with an XPath to dump the events to XML.

Key design points:
- Trace output goes under `bench/results/<timestamp>-<binary>.trace`
- Companion XML at `<trace_path>.xml`
- Pass through extra args to the binary
- Fail loudly if template doesn't exist (Xcode not installed)

- [ ] **Step 4.1: Verify the Instruments CPU Counters template exists**

Run:
```bash
ls -la "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/Library/Instruments/Templates/CPU Counters.tracetemplate" 2>&1
```
Expected: File exists (size ~few KB). If "No such file", Xcode is missing; install it before continuing.

If the path differs (Xcode beta or custom install), find the template:
```bash
mdfind -name "CPU Counters.tracetemplate" 2>/dev/null
```

- [ ] **Step 4.2: Create the scripts/perf/ directory**

Run:
```bash
mkdir -p /Users/mayiding/Desktop/GitMy/GNFS/scripts/perf
```

- [ ] **Step 4.3: Write scripts/perf/profile-cpu.sh**

Create `/Users/mayiding/Desktop/GitMy/GNFS/scripts/perf/profile-cpu.sh` with this content:

```bash
#!/usr/bin/env zsh
# scripts/perf/profile-cpu.sh
# Record CPU PMU counters for a binary using xctrace + Instruments "CPU Counters" template.
# Outputs:
#   bench/results/<YYYY-MM-DD-HHMMSS>-<basename>.trace        (binary trace)
#   bench/results/<YYYY-MM-DD-HHMMSS>-<basename>.trace.xml    (exported XML)
#
# Usage:
#   scripts/perf/profile-cpu.sh <path-to-binary> [args...]
#
# Then:
#   python3 scripts/perf/parse-trace.py bench/results/<file>.trace.xml
#
# See docs/perf/performance-doctrine.md §5.4.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RESULTS_DIR="${ROOT}/bench/results"

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <path-to-binary> [args...]" >&2
    exit 1
fi

BIN="$1"
shift
if [[ ! -x "${BIN}" ]]; then
    echo "ERROR: not an executable file: ${BIN}" >&2
    exit 1
fi

TEMPLATE="/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/Library/Instruments/Templates/CPU Counters.tracetemplate"
if [[ ! -e "${TEMPLATE}" ]]; then
    # Fallback: search via Spotlight
    TEMPLATE=$(mdfind -name "CPU Counters.tracetemplate" 2>/dev/null | head -1)
    if [[ -z "${TEMPLATE}" || ! -e "${TEMPLATE}" ]]; then
        echo "ERROR: Instruments 'CPU Counters' tracetemplate not found. Install Xcode." >&2
        exit 1
    fi
fi

if ! command -v xctrace >/dev/null 2>&1; then
    echo "ERROR: xctrace not in PATH. Install Xcode Command Line Tools." >&2
    exit 1
fi

mkdir -p "${RESULTS_DIR}"
TS=$(date +%Y-%m-%d-%H%M%S)
BASENAME=$(basename "${BIN}")
TRACE_OUT="${RESULTS_DIR}/${TS}-${BASENAME}.trace"
XML_OUT="${TRACE_OUT}.xml"

echo "== Recording trace =="
echo "  binary:   ${BIN} $*"
echo "  template: ${TEMPLATE}"
echo "  trace:    ${TRACE_OUT}"
xctrace record \
    --template "${TEMPLATE}" \
    --launch "${BIN}" \
    --output "${TRACE_OUT}" \
    --no-prompt \
    -- "$@"

echo ""
echo "== Exporting to XML =="
# XPath captures all instrument data rows (event + value)
xctrace export \
    --input "${TRACE_OUT}" \
    --xpath '/trace-toc/run/data/table[@schema="counters-profile"]' \
    --output "${XML_OUT}"

echo ""
echo "== Done =="
echo "Trace: ${TRACE_OUT}"
echo "XML:   ${XML_OUT}"
echo ""
echo "Parse with: python3 ${ROOT}/scripts/perf/parse-trace.py ${XML_OUT}"
```

- [ ] **Step 4.4: Make executable**

Run:
```bash
chmod +x /Users/mayiding/Desktop/GitMy/GNFS/scripts/perf/profile-cpu.sh
```

- [ ] **Step 4.5: Smoke-test against a small baseline binary**

`test_integer` is an `instant`-tier test (<1 s). It's the cheapest target to validate the trace plumbing without burning time.

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
./scripts/test.sh build 2>&1 | tail -3   # ensure ./build/test_integer exists
./scripts/perf/profile-cpu.sh ./build/test_integer 2>&1 | tee /tmp/profile-smoke.log
```
Expected:
- `Recording trace` section with paths printed
- `Exporting to XML` section
- Final `Done` showing both `Trace:` and `XML:` paths
- Files exist: `bench/results/<timestamp>-test_integer.trace` (directory) and `.trace.xml` (file)

- [ ] **Step 4.6: Inspect the XML format (for the parser in next task)**

Run:
```bash
ls -lh /Users/mayiding/Desktop/GitMy/GNFS/bench/results/*-test_integer.trace.xml
head -100 /Users/mayiding/Desktop/GitMy/GNFS/bench/results/*-test_integer.trace.xml | head -80
```
Expected: XML output. Note the actual element names — the doctrine §5.5 placeholder parser uses `row/event/value` but the real xctrace output may use different tags. **The parser in Task 5 must match whatever xctrace actually emits here.** Save a copy of the head of this file to inform parser implementation:

Run:
```bash
head -200 /Users/mayiding/Desktop/GitMy/GNFS/bench/results/*-test_integer.trace.xml > /tmp/xctrace-xml-schema.txt
cat /tmp/xctrace-xml-schema.txt
```
Note the actual schema (likely `<row><sample><...></row>` with attributes like `event-name="..."` and child `<value>...</value>` or `count="..."`).

- [ ] **Step 4.7: Commit (parser comes next; this is just the recorder)**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
git add scripts/perf/profile-cpu.sh
git commit -m "$(cat <<'EOF'
feat(perf): add scripts/perf/profile-cpu.sh — xctrace CPU PMU recorder

Wraps xctrace record/export against Apple's CPU Counters template.
Outputs <bench/results/<ts>-<bin>.trace> + .trace.xml. Validated against
test_integer.

Parser (parse-trace.py) follows in the next commit.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```
Expected: Commit succeeds.

---

### Task 5: Create scripts/perf/parse-trace.py

**Files:**
- Create: `scripts/perf/parse-trace.py` (executable, Python 3 stdlib only)

**Background:** The xctrace XML schema isn't formally documented; we discovered it empirically in Task 4.6. The parser must be defensive (handle missing keys, multiple element styles) and produce a clean markdown table.

Two modes:
1. **Single trace**: `parse-trace.py <file.xml>` → prints a derived-metrics table
2. **Diff**: `parse-trace.py <baseline.xml> <pgo.xml>` → prints a comparison table with Δ%

**Derived metrics** (per doctrine §2 — TMA-aligned):
- `IPC` = `INST_ALL / CORE_ACTIVE_CYCLE` — overall throughput (M5 P-core peak ~8.0)
- `L1D_miss_rate` = `L1D_CACHE_MISS_LD / INST_INT_LD` — data cache pressure
- `BR_mispred_rate` = `BRANCH_MISPRED_NONSPEC / INST_BRANCH` — control flow penalty
- `iTLB_miss_per_inst` = `L2_TLB_MISS_INST / INST_ALL` — code locality penalty

If certain events aren't present in the trace, the metric prints as `n/a` rather than crashing.

- [ ] **Step 5.1: Inspect /tmp/xctrace-xml-schema.txt (saved in Task 4.6)**

Run:
```bash
cat /tmp/xctrace-xml-schema.txt | head -60
```
Identify the elements:
- The table element wrapping rows
- The row element (each PMU sample)
- The event-name attribute or element
- The value/count attribute or element

If the schema doesn't match the doctrine placeholder, adapt the parser code in Step 5.2 accordingly. Common patterns:
- `<row><sample><event-name>X</event-name><value>123</value></sample></row>`
- `<row event-name="X" value="123"/>`
- `<row><pmc-event ref="..."/><pmc-event-count>...</pmc-event-count></row>`

- [ ] **Step 5.2: Write scripts/perf/parse-trace.py**

Create `/Users/mayiding/Desktop/GitMy/GNFS/scripts/perf/parse-trace.py` with this content:

```python
#!/usr/bin/env python3
"""
scripts/perf/parse-trace.py

Parse xctrace XML export and emit a derived-metric markdown summary.

Usage:
    python3 parse-trace.py <trace.xml>                   # single-trace summary
    python3 parse-trace.py <baseline.xml> <pgo.xml>      # diff mode

Stdlib only. See docs/perf/performance-doctrine.md §5.5.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Dict, Optional
import xml.etree.ElementTree as ET


# Map "well-known PMU event name" -> set of aliases that xctrace may emit.
# M5 P-core inherits A18 Pro / M4 event names per doctrine §4.1.
EVENT_ALIASES = {
    "INST_ALL":              ["INST_ALL", "FIXED_INSTRUCTIONS", "INSTRUCTIONS", "instructions"],
    "CORE_ACTIVE_CYCLE":     ["CORE_ACTIVE_CYCLE", "FIXED_CYCLES", "CYCLES", "cycles"],
    "INST_INT_LD":           ["INST_INT_LD", "INST_LD", "L1D_LOADS"],
    "L1D_CACHE_MISS_LD":     ["L1D_CACHE_MISS_LD", "L1D_LOAD_MISS", "DCACHE_LOAD_MISS"],
    "INST_BRANCH":           ["INST_BRANCH", "BRANCHES", "INST_BR"],
    "BRANCH_MISPRED_NONSPEC":["BRANCH_MISPRED_NONSPEC", "BRANCH_MISPRED", "BR_MIS_PRED"],
    "L2_TLB_MISS_INST":      ["L2_TLB_MISS_INST", "ITLB_MISS", "INST_TLB_MISS"],
}


def parse_counters(xml_path: Path) -> Dict[str, int]:
    """Parse xctrace XML; return {event_name: aggregated_count}.

    Defensive: scans every leaf-ish element, looks for event-name + value pairs
    in attributes OR children. Aggregates across multiple samples if present.
    """
    tree = ET.parse(xml_path)
    root = tree.getroot()
    counters: Dict[str, int] = {}

    # Strategy 1: <row> elements with event-name attr + value attr/child
    for elem in root.iter():
        tag = elem.tag.lower()
        if tag not in ("row", "sample", "counter", "event", "pmc-event"):
            continue

        # Try attribute forms
        ev = elem.attrib.get("event-name") or elem.attrib.get("event") \
            or elem.attrib.get("name") or elem.attrib.get("ref")
        val_s = elem.attrib.get("value") or elem.attrib.get("count") \
            or elem.attrib.get("pmc-event-count")

        if ev is None or val_s is None:
            # Try child elements
            ev_e = elem.find("./event-name") or elem.find("./event") or elem.find("./name")
            val_e = elem.find("./value") or elem.find("./count") or elem.find("./pmc-event-count")
            if ev_e is not None and ev_e.text:
                ev = ev_e.text.strip()
            if val_e is not None and val_e.text:
                val_s = val_e.text.strip()

        if ev is None or val_s is None:
            continue

        try:
            v = int(val_s.replace(",", "").replace(" ", ""))
        except ValueError:
            continue

        counters[ev] = counters.get(ev, 0) + v

    return counters


def resolve(counters: Dict[str, int], canonical: str) -> Optional[int]:
    """Look up a canonical event name via its alias list."""
    for alias in EVENT_ALIASES.get(canonical, [canonical]):
        if alias in counters:
            return counters[alias]
        # case-insensitive fallback
        for k, v in counters.items():
            if k.lower() == alias.lower():
                return v
    return None


def derived_metrics(counters: Dict[str, int]) -> Dict[str, Optional[float]]:
    inst = resolve(counters, "INST_ALL")
    cyc  = resolve(counters, "CORE_ACTIVE_CYCLE")
    ld   = resolve(counters, "INST_INT_LD")
    ld_m = resolve(counters, "L1D_CACHE_MISS_LD")
    br   = resolve(counters, "INST_BRANCH")
    br_m = resolve(counters, "BRANCH_MISPRED_NONSPEC")
    itm  = resolve(counters, "L2_TLB_MISS_INST")

    def safe_div(num, den):
        if num is None or den is None or den == 0:
            return None
        return num / den

    return {
        "IPC":                 safe_div(inst, cyc),
        "L1D_miss_rate":       safe_div(ld_m, ld),
        "BR_mispred_rate":     safe_div(br_m, br),
        "iTLB_miss_per_inst":  safe_div(itm, inst),
    }


def fmt(v: Optional[float]) -> str:
    return f"{v:.4f}" if v is not None else "n/a"


def print_single(xml_path: Path) -> int:
    counters = parse_counters(xml_path)
    if not counters:
        print(f"WARNING: no counters parsed from {xml_path}", file=sys.stderr)
        print("Raw element-tag scan (first 20 unique tags):", file=sys.stderr)
        tree = ET.parse(xml_path)
        seen = set()
        for e in tree.iter():
            seen.add(e.tag)
            if len(seen) >= 20:
                break
        for t in sorted(seen):
            print(f"  - {t}", file=sys.stderr)
        return 1

    metrics = derived_metrics(counters)
    print(f"# Trace summary: {xml_path.name}")
    print()
    print(f"Raw events captured: **{len(counters)}**")
    print()
    print("## Derived metrics\n")
    print("| Metric | Value |")
    print("|---|---|")
    for k, v in metrics.items():
        print(f"| {k} | {fmt(v)} |")
    print()
    print("## Raw counter snapshot (top 30 by magnitude)\n")
    print("| Event | Count |")
    print("|---|---|")
    for ev, cnt in sorted(counters.items(), key=lambda kv: -kv[1])[:30]:
        print(f"| `{ev}` | {cnt:,} |")
    return 0


def print_diff(xml_a: Path, xml_b: Path) -> int:
    ca = parse_counters(xml_a)
    cb = parse_counters(xml_b)
    ma = derived_metrics(ca)
    mb = derived_metrics(cb)
    print(f"# PGO impact: {xml_a.name} -> {xml_b.name}")
    print()
    print("## Derived metrics\n")
    print("| Metric | Baseline | After | Δ% |")
    print("|---|---|---|---|")
    for k in ma:
        a, b = ma[k], mb[k]
        if a is None or b is None or a == 0:
            print(f"| {k} | {fmt(a)} | {fmt(b)} | n/a |")
        else:
            delta = (b - a) / a * 100
            sign = "+" if delta >= 0 else ""
            print(f"| {k} | {a:.4f} | {b:.4f} | {sign}{delta:.2f}% |")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) == 2:
        return print_single(Path(argv[1]))
    if len(argv) == 3:
        return print_diff(Path(argv[1]), Path(argv[2]))
    print("Usage:\n"
          "  parse-trace.py <trace.xml>\n"
          "  parse-trace.py <baseline.xml> <pgo.xml>", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
```

- [ ] **Step 5.3: Make executable**

Run:
```bash
chmod +x /Users/mayiding/Desktop/GitMy/GNFS/scripts/perf/parse-trace.py
```

- [ ] **Step 5.4: Smoke-test single-trace mode against the test_integer XML from Task 4.5**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
ls -1 bench/results/*-test_integer.trace.xml
# pick the first one
XML_FILE=$(ls -1 bench/results/*-test_integer.trace.xml | head -1)
python3 scripts/perf/parse-trace.py "${XML_FILE}"
```
Expected:
- If parser finds counters: markdown table with `# Trace summary` header, derived metrics (some may be `n/a` if event names differ from defaults), top-30 raw counters table.
- If parser finds zero counters: WARNING printed to stderr, plus a list of XML tag names found. **This is the failure mode that requires adjusting the parser's tag/attribute names in Step 5.2** — re-read `/tmp/xctrace-xml-schema.txt`, update `EVENT_ALIASES` and the element scan in `parse_counters()`, repeat this step.

**If parser fails with zero counters:**
1. Look at `/tmp/xctrace-xml-schema.txt` carefully
2. Identify the actual element/attribute structure (e.g., maybe it's `<sample event-id="42" count="12345"/>` with a separate `<event id="42" name="CPU_CYCLES"/>` lookup table)
3. Update the `parse_counters()` function to walk that structure
4. Update `EVENT_ALIASES` to include the canonical names xctrace actually emits
5. Re-run Step 5.4

This is the most likely point where the doctrine's placeholder code needs adjustment. Spend up to 30 min iterating; if it's still failing, fall back to scraping `xctrace export --output -` plain-text format instead of XML.

- [ ] **Step 5.5: Smoke-test diff mode (run two traces against the same binary)**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
./scripts/perf/profile-cpu.sh ./build/test_integer
# pick two most recent
XML_A=$(ls -1t bench/results/*-test_integer.trace.xml | sed -n 1p)
XML_B=$(ls -1t bench/results/*-test_integer.trace.xml | sed -n 2p)
python3 scripts/perf/parse-trace.py "${XML_B}" "${XML_A}"
```
Expected: `# PGO impact` header, `| Metric | Baseline | After | Δ% |` table with small Δ% (run-to-run noise, expect within ±5%).

- [ ] **Step 5.6: Commit**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
git add scripts/perf/parse-trace.py
git commit -m "$(cat <<'EOF'
feat(perf): add scripts/perf/parse-trace.py — xctrace XML -> markdown

Single-trace mode: derived metrics (IPC, L1D miss, branch mispred, iTLB miss)
+ top-30 raw counters.
Diff mode: side-by-side with Δ% column.

Stdlib only (xml.etree.ElementTree). Defensive parsing with EVENT_ALIASES
to cope with xctrace schema variations across Xcode versions.

Validated against test_integer trace from profile-cpu.sh.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```
Expected: Commit succeeds.

---

### Task 6: Wire pgo-train and profile subcommands into scripts/test.sh

**Files:**
- Modify: `scripts/test.sh` (add two new case branches around line 1671, update help/list output)

**Background:** Existing dispatch structure (test.sh:1575-1731) uses a flat `case "$MODE"` block. Insert the two new modes before the catch-all. Help text is autogenerated from the file header comment (test.sh:2-XX), so we update that too.

- [ ] **Step 6.1: Locate the help block at top of test.sh**

Run:
```bash
sed -n '1,90p' /Users/mayiding/Desktop/GitMy/GNFS/scripts/test.sh | head -90
```
Identify the comment block listing all modes (lines starting with `# `). Find the section where modes are documented; insert the new ones there.

- [ ] **Step 6.2: Locate the `case "$MODE"` dispatch block tail**

Run:
```bash
sed -n '1670,1735p' /Users/mayiding/Desktop/GitMy/GNFS/scripts/test.sh
```
Confirm: `gate)` block ends near line 1671, `perf)` around 1673, then `stress)`, `watch)`, `report)`, `matrix)`. Insert `pgo-train)` and `profile)` between `perf)` and `stress)`, since they're perf-related.

- [ ] **Step 6.3: Add the two new case branches**

Use the Edit tool. `old_string`:
```
    perf)
        do_build
        log_header "性能测试 (25-digit)"
        log_warn "预计耗时数分钟..."
        run_single_test test_25digit
        show_summary
        ;;
```

`new_string`:
```
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
        exec "${SCRIPT_DIR}/pgo-train.sh" "${MODE_ARGS[@]}"
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
        exec "${SCRIPT_DIR}/perf/profile-cpu.sh" "${_test_bin}" "${MODE_ARGS[@]:1}"
        ;;
```

Note: `${MODE_ARGS[1]}` is zsh 1-indexed (first element). For the slice "everything except the first element," zsh's `${array[@]:N}` is bash-compatible 0-indexed offset semantics — so `${MODE_ARGS[@]:1}` means "skip element at index 0 (none in zsh, but bash-compat applies)" which actually skips the first element. **Verify this works on actual test.sh:** see Step 6.5 — if behavior is wrong, switch to zsh-native `${MODE_ARGS[2,-1]}` (1-indexed slice).

- [ ] **Step 6.4: Update the help header at top of test.sh**

Find the help block (around lines 2-80; the regex `sed -n '2,/^set -eo/{ /^#/s/^# \{0,1\}//p }'` extracts it as `--help`). Locate the section describing perf/bench commands. Use the Edit tool to add documentation for `pgo-train` and `profile`.

Run:
```bash
grep -n "perf\|bench" /Users/mayiding/Desktop/GitMy/GNFS/scripts/test.sh | head -20
```

Find a line near the top documenting `perf`. Edit `old_string` to that block and `new_string` to add lines like:
```
#   pgo-train               PGO 训练 + 优化构建 (~10min, 见 doctrine §5.3)
#   profile <test>          Instruments CPU PMU trace (见 doctrine §5.4)
```

If you can't find a clean insertion point in the header, just add a `## ` section labelled `性能采集` after the existing `perf` mention.

- [ ] **Step 6.5: Smoke-test the dispatcher (no actual PGO run)**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
./scripts/test.sh profile 2>&1 | tail -5     # no arg -> usage error
```
Expected: `用法: ... profile <test_name>` error message, exit code 1.

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
./scripts/test.sh profile nonexistent 2>&1 | tail -5
```
Expected: `测试二进制不存在: .../test_nonexistent` error.

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
./scripts/test.sh profile integer 2>&1 | head -20
```
Expected: dispatches to `profile-cpu.sh ./build/test_integer`, starts recording. Let it complete (~5 s).

- [ ] **Step 6.6: Commit**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
git add scripts/test.sh
git commit -m "$(cat <<'EOF'
feat(perf): wire pgo-train + profile subcommands into scripts/test.sh

./scripts/test.sh pgo-train            -> scripts/pgo-train.sh
./scripts/test.sh profile <test> [...] -> scripts/perf/profile-cpu.sh

Updates the autogenerated --help block too. Validated dispatch for
profile (no-arg error, missing-binary error, successful integer run).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```
Expected: Commit succeeds.

---

### Task 7: Run first baseline + PGO comparison, write impact report

**Files:**
- Create: `bench/results/2026-05-12-pgo-impact.md` (tracked report)

**Background:** This is the payoff. We capture a baseline trace (current `build/` without PGO), run the full PGO pipeline (which generates `build-pgo-use/`), capture a PGO trace, and diff them. The doctrine §5.7 gives the expected envelope: -5% to -20% wall time, +5% to +15% IPC. If we hit those numbers, PGO is validated as a baseline optimization layer for the project.

Target binary: `test_factor_with_kleinjung` (slow tier, ~30 bit, full GNFS pipeline, ~10-30 s wall time per run).

- [ ] **Step 7.1: Ensure baseline build is fresh and up to date**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
./scripts/test.sh build -t Release 2>&1 | tail -5
ls -la build/test_factor_with_kleinjung
```
Expected: Binary exists, recently built. If `build/` was Debug, rebuild as Release explicitly.

- [ ] **Step 7.2: Capture baseline wall time (3 runs for noise floor)**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
for i in 1 2 3; do
    /usr/bin/time -p ./build/test_factor_with_kleinjung 2>&1 | tail -3 | tee -a /tmp/baseline-times.log
    echo "---" | tee -a /tmp/baseline-times.log
done
cat /tmp/baseline-times.log
```
Expected: 3 `real X.XX` lines. Note the median (call this `T_base`).

- [ ] **Step 7.3: Capture baseline PMU trace**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
./scripts/test.sh profile factor_with_kleinjung 2>&1 | tail -10
ls -1t bench/results/*-test_factor_with_kleinjung.trace.xml | head -1
```
Expected: Trace recorded; XML file path printed. Note the path (call it `XML_BASELINE`).

- [ ] **Step 7.4: Run the PGO pipeline**

If not already done in Task 3.5:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
./scripts/test.sh pgo-train --clean 2>&1 | tee /tmp/pgo-train-final.log
```
Expected: Same milestone output as Task 3.5; produces `build-pgo-use/test_*` binaries.

If PGO was already trained, you can skip this step.

- [ ] **Step 7.5: Capture PGO wall time (3 runs)**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
for i in 1 2 3; do
    /usr/bin/time -p ./build-pgo-use/test_factor_with_kleinjung 2>&1 | tail -3 | tee -a /tmp/pgo-times.log
    echo "---" | tee -a /tmp/pgo-times.log
done
cat /tmp/pgo-times.log
```
Expected: 3 `real X.XX` lines. Note the median (call this `T_pgo`).

Compute speedup: `(T_base - T_pgo) / T_base * 100`. Expected: +5% to +20% improvement.

- [ ] **Step 7.6: Capture PGO PMU trace**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
./scripts/perf/profile-cpu.sh ./build-pgo-use/test_factor_with_kleinjung 2>&1 | tail -10
ls -1t bench/results/*-test_factor_with_kleinjung.trace.xml | head -1
```
Expected: Trace recorded; XML file path printed. Note the path (call it `XML_PGO`).

- [ ] **Step 7.7: Generate diff report**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
# Substitute actual paths from steps 7.3 and 7.6
XML_BASELINE=$(ls -1t bench/results/*-test_factor_with_kleinjung.trace.xml | sed -n 2p)
XML_PGO=$(ls -1t bench/results/*-test_factor_with_kleinjung.trace.xml | sed -n 1p)
echo "baseline: ${XML_BASELINE}"
echo "pgo:      ${XML_PGO}"
python3 scripts/perf/parse-trace.py "${XML_BASELINE}" "${XML_PGO}" > /tmp/pgo-diff.md
cat /tmp/pgo-diff.md
```
Expected: A markdown table with `| Metric | Baseline | After | Δ% |` rows.

- [ ] **Step 7.8: Compose the full impact report**

Create `/Users/mayiding/Desktop/GitMy/GNFS/bench/results/2026-05-12-pgo-impact.md` with this template (fill in actual numbers from steps 7.2, 7.5, 7.7):

```markdown
# PGO Impact Report — test_factor_with_kleinjung

**Date:** 2026-05-12
**Host:** Apple M5 (4P+6E, 4.61 GHz P-core)
**Build flags (baseline):** `-O3 -mcpu=native -flto=thin`
**Build flags (PGO):** `-O3 -mcpu=native -flto=thin -fprofile-instr-use=merged.profdata`
**Target binary:** `test_factor_with_kleinjung` (~30-bit GNFS factorization)

## Wall time (3 runs, median)

| Build | Median wall time (s) | Min | Max |
|---|---|---|---|
| Baseline | TBD | TBD | TBD |
| PGO | TBD | TBD | TBD |
| **Δ** | **TBD%** (sign: negative = PGO faster) | | |

## Derived PMU metrics (Instruments CPU Counters)

[paste output of `python3 scripts/perf/parse-trace.py <baseline.xml> <pgo.xml>` here]

## Verdict

[Fill in one of:]
- ✅ **PGO accepted.** Wall time improved by X%, IPC improved by Y%. Recommend integrating PGO build as a build option for releases.
- ⚠️ **PGO inconclusive.** Improvements within noise floor (<2%). Recommend re-running on cooler thermal state and with more training samples.
- ❌ **PGO rejected.** Wall time regressed by X% or PMU metrics worsened. Likely overfit to training samples; need to broaden training set.

## Training samples used

- `test_factor_with_kleinjung` (full GNFS pipeline, ~30-bit)
- `test_lattice_sieve` (sieve hotpath)
- `test_linalg` (block Lanczos / Wiedemann hotpath)

## Notes

[Any caveats: thermal throttling observed, run-to-run noise, parser warnings, etc.]

## Reproduce

```bash
./scripts/test.sh pgo-train --clean
./scripts/test.sh profile factor_with_kleinjung           # baseline trace
./scripts/perf/profile-cpu.sh ./build-pgo-use/test_factor_with_kleinjung  # pgo trace
python3 scripts/perf/parse-trace.py <baseline.xml> <pgo.xml>
```
```

Use the Write tool to create this file. Fill in the TBD values with the actual numbers you measured.

- [ ] **Step 7.9: Commit the report**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
git add bench/results/2026-05-12-pgo-impact.md
git status
git commit -m "$(cat <<'EOF'
docs(perf): first PGO impact report for test_factor_with_kleinjung

Baseline (T_base) vs PGO (T_pgo) wall time and PMU derived metrics
captured via the new pgo-train + profile pipeline.

Verdict: [accepted/inconclusive/rejected based on actual numbers].

See doctrine §5.7 for expected envelope.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```
Expected: Commit succeeds. **Edit the verdict in the commit message to match the actual outcome.**

- [ ] **Step 7.10: Update doctrine §6 path roadmap entry**

Open `docs/perf/performance-doctrine.md` and update the `§6 路线图` P0 entry to mark "Instruments 闭环 + PGO" as complete (or note actual outcome). Use Edit tool with the existing P0 description.

Run:
```bash
grep -n "P0\|首战" /Users/mayiding/Desktop/GitMy/GNFS/docs/perf/performance-doctrine.md | head -10
```

Adjust the P0 status line accordingly.

- [ ] **Step 7.11: Commit doctrine update**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
git add docs/perf/performance-doctrine.md
git commit -m "docs(perf): mark P0 (Instruments + PGO loop) complete in doctrine roadmap

Refs the impact report at bench/results/2026-05-12-pgo-impact.md.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

- [ ] **Step 7.12: Push (per CLAUDE.md auto-push policy)**

Run:
```bash
cd /Users/mayiding/Desktop/GitMy/GNFS
git status
git log --oneline -10
git push origin main
```
Expected: All 7 (or more) PGO-related commits pushed to remote.

---

## Self-Review Checklist (run after writing the plan)

After completing Tasks 1-7, run a sanity sweep:

- [ ] **Spec coverage:** Every section of doctrine §5 (S1-S6) maps to one or more tasks above. ✓ S1 → Task 2, S2 → Task 3, S3 → Task 4, S4 → Task 5, S5 → Task 6, S6 → Task 7.
- [ ] **No placeholders:** Search the plan for `TODO`, `TBD`, `XXX`, `fixme`. The only `TBD` left is in the impact report template (Task 7.8) where actual measurements are filled in — that's intentional, not a plan defect.
- [ ] **Type/signature consistency:** Script paths used in Task 6 (`pgo-train.sh`, `perf/profile-cpu.sh`) match what's created in Tasks 3 and 4. ✓
- [ ] **Commands runnable:** Every `Run:` block uses absolute paths or anchors via `cd /Users/mayiding/Desktop/GitMy/GNFS`. No relative-path ambiguity. ✓
- [ ] **Commit messages:** All commit messages follow Conventional Commits (`feat(perf):`, `chore(perf):`, `docs(perf):`) per CLAUDE.md. ✓
- [ ] **Cleanup:** Task 2.7 removes the throwaway `build-pgo-test/` dir. No leftover test artifacts. ✓

## Risks and Mitigations

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| xctrace XML schema differs from doctrine placeholder | **High** | Task 4.6 + Task 5.4 explicitly iterate the parser against actual output before commit. Step 5.2 is annotated to expect adjustment. |
| PGO training run fails on test_factor_with_kleinjung (e.g., assertion on instrumented build) | Medium | Task 3.5 logs each phase to disk; if Phase 2 fails, the `.train.log` shows exactly which test broke. Fallback: shrink training to only `test_linalg` (cheapest). |
| `Wno-profile-instr-out-of-date` masks legitimate ABI breakage | Low | Only matters if PGO is enabled in CI long-term. We only run it locally for now (Task 7). |
| Wall-time regression (PGO worse than baseline) | Low | Doctrine §5.7 explicitly notes risk of training overfit. Task 7.8 verdict template includes the "rejected" path. We document & roll back; no harm to baseline build. |
| `bench/results/*.trace` is actually a directory, not a file (Instruments quirk) | Medium | gitignore in Task 1.2 explicitly handles both `*.trace` (file) and `*.trace/` (dir). |

---

## Execution Estimate

| Task | Est. wall time |
|------|----------------|
| 1. gitignore + bench dir | 10 min |
| 2. CMakeLists PGO | 20 min |
| 3. pgo-train.sh + smoke run | 30 min script + 5-15 min PGO run |
| 4. profile-cpu.sh | 20 min |
| 5. parse-trace.py + iteration | 30-60 min (parser may need real-XML adjustment) |
| 6. test.sh integration | 20 min |
| 7. First impact report | 30 min (3 baselines + 3 PGO + traces + writeup) |
| **Total** | **2.5-4 hours** |

This is one focused session.


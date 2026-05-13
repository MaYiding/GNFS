#!/usr/bin/env python3
"""
scripts/perf/pmu-derive.py

Parse mperf JSON output and emit a markdown summary with derived metrics
for MemBound / CoreBound / BadSpec / FrontendBound discrimination.

Usage:
    python3 pmu-derive.py <run.pmu.json>                # single-run summary
    python3 pmu-derive.py <baseline.json> <after.json>  # diff mode

mperf (https://github.com/tmcgilchrist/mperf) JSON shape:
    {
      "counters": {"<event-name>": <int>, ...},
      "time":     {"wall_ns": ..., "user_ns": ..., "sys_ns": ...},
      "derived":  {"ipc": <float>, "cpi": <float>}     # only when cycles+instructions both present
    }

The event names in `counters` match exactly what was passed to mperf via -e.
We pass GNFS P1.A event set (see plan / pmu-stat.sh).

See docs/superpowers/plans/2026-05-13-pmu-events-deepening.md.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Optional


# Must match scripts/perf/pmu-stat.sh EVENTS array order/spelling.
EVENT_KEYS = [
    "FIXED_CYCLES", "FIXED_INSTRUCTIONS",
    "ARM_STALL_BACKEND", "ARM_STALL_FRONTEND",
    "L1D_CACHE_MISS_LD", "L1D_TLB_MISS", "ARM_MEM_ACCESS",
    "BRANCH_MISPRED_NONSPEC", "INST_BRANCH",
    "MAP_SIMD_UOP",
]


def load_counters(p: Path) -> tuple[dict[str, int], dict]:
    """Load mperf JSON. Returns (counters_dict, full_json)."""
    raw = json.loads(p.read_text())
    counters_raw = raw.get("counters", {}) if isinstance(raw, dict) else {}

    counters: dict[str, int] = {}
    for k, v in counters_raw.items():
        if isinstance(v, (int, float)):
            counters[k] = int(v)
        elif isinstance(v, str):
            try:
                counters[k] = int(v.replace(",", "").strip())
            except ValueError:
                pass
    return counters, raw


def derive(c: dict[str, int]) -> dict[str, Optional[float]]:
    """Compute derived metrics; None for any with missing inputs."""
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
    bsr = d.get("BackendStallRate") or 0.0
    fsr = d.get("FrontendStallRate") or 0.0
    l1d = d.get("L1DMissRate") or 0.0
    bmr = d.get("BranchMispredRate") or 0.0
    sd  = d.get("SIMDDensity") or 0.0

    if bsr > 0.30 and l1d > 0.05:
        triggers.append("**MemBound** (BackendStall>30% AND L1DMissRate>5%) "
                        "→ prefetch / 64B align / SoA")
    if bsr > 0.30 and l1d < 0.02 and sd < 0.05:
        triggers.append("**CoreBound** (BackendStall>30% AND L1DMissRate<2% "
                        "AND SIMDDensity<5%) → NEON full coverage / ILP")
    if bmr > 0.05:
        triggers.append("**BadSpec** (BranchMispredRate>5%) "
                        "→ `[[likely]]` / branchless")
    if fsr > 0.20:
        triggers.append("**FrontendBound** (FrontendStall>20%) "
                        "→ code-layout / inline review")
    return triggers


def fmt_int(n: Optional[int]) -> str:
    return f"{n:,}" if n is not None else "n/a"


def fmt_pct(v: Optional[float]) -> str:
    return f"{v*100:.2f}%" if v is not None else "n/a"


def fmt_ratio(v: Optional[float]) -> str:
    return f"{v:.3f}" if v is not None else "n/a"


def print_single(p: Path) -> int:
    counters, raw = load_counters(p)
    if not counters:
        print(f"ERROR: no recognized PMU events in {p}", file=sys.stderr)
        print(f"  Top-level JSON keys: {list(raw.keys()) if isinstance(raw, dict) else type(raw).__name__}", file=sys.stderr)
        return 1
    derived = derive(counters)
    triggers = classify(derived)

    print(f"# PMU summary: {p.name}\n")
    t = raw.get("time", {}) if isinstance(raw, dict) else {}
    if t:
        wall_ns = t.get("wall_ns")
        if wall_ns:
            print(f"- Wall: {wall_ns / 1e9:.3f} s "
                  f"(user {t.get('user_ns',0)/1e9:.3f}s + sys {t.get('sys_ns',0)/1e9:.3f}s)")
    print("\n## Raw counters\n")
    print("| Event | Count |\n|---|---:|")
    for ev in EVENT_KEYS:
        print(f"| `{ev}` | {fmt_int(counters.get(ev))} |")
    print("\n## Derived metrics\n")
    print("| Metric | Value |\n|---|---:|")
    for k, v in derived.items():
        if k == "IPC":
            print(f"| {k} | {fmt_ratio(v)} |")
        else:
            print(f"| {k} | {fmt_pct(v)} |")
    print("\n## doctrine §6 P1 decision\n")
    if triggers:
        for tg in triggers:
            print(f"- {tg}")
    else:
        print("- _no category exceeds threshold — collect larger sample or expand events_")
    print()
    return 0


def print_diff(a_path: Path, b_path: Path) -> int:
    ca, raw_a = load_counters(a_path)
    cb, raw_b = load_counters(b_path)
    if not ca or not cb:
        print(f"ERROR: empty counters. a={len(ca)} b={len(cb)}", file=sys.stderr)
        return 1
    da, db = derive(ca), derive(cb)

    print(f"# PMU diff: A={a_path.name}  vs  B={b_path.name}\n")
    for label, raw in (("A", raw_a), ("B", raw_b)):
        t = raw.get("time", {}) if isinstance(raw, dict) else {}
        wall_ns = t.get("wall_ns")
        if wall_ns:
            print(f"- {label}: wall {wall_ns / 1e9:.3f} s")
    print("\n## Raw counter delta\n")
    print("| Event | A | B | Δ% |\n|---|---:|---:|---:|")
    for ev in EVENT_KEYS:
        va, vb = ca.get(ev), cb.get(ev)
        if va is None or vb is None or va == 0:
            ds = "n/a"
        else:
            ds = f"{(vb - va) / va * 100:+.2f}%"
        print(f"| `{ev}` | {fmt_int(va)} | {fmt_int(vb)} | {ds} |")
    print("\n## Derived metric delta\n")
    print("| Metric | A | B | Δ |\n|---|---:|---:|---:|")
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
    triggers = classify(db)
    if triggers:
        for tg in triggers:
            print(f"- {tg}")
    else:
        print("- _no category exceeds threshold — collect larger sample or expand events_")
    print()
    return 0


def main(argv: list[str]) -> int:
    if len(argv) == 2:
        return print_single(Path(argv[1]))
    if len(argv) == 3:
        return print_diff(Path(argv[1]), Path(argv[2]))
    print("Usage:\n"
          "  pmu-derive.py <run.pmu.json>\n"
          "  pmu-derive.py <A.json> <B.json>", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

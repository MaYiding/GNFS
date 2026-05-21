#!/usr/bin/env python3
"""GNFS sweep aggregator — cross-run summary across multiple sweep markdown files.

Reads one or more `bench/results/sweep_*.md` reports produced by
`scripts/sweep_full.sh` and emits a consolidated cross-run table:

  - Per ENV: best Δ% observed across all runs and all sizes
  - Per N-size: top-3 most consistent winners
  - Trend: which ENVs improve as N grows (positive scaling) vs which
    plateau or regress

Pure standard-library; no third-party deps. Works on Python 3.9+.

Usage:
    scripts/sweep_analyze.py bench/results/sweep_*.md
    scripts/sweep_analyze.py --json bench/results/sweep_*.md
    scripts/sweep_analyze.py --top 5 --out report.md bench/results/*.md
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


# ──────────────────────────────────────────────────────────────────
# Data model
# ──────────────────────────────────────────────────────────────────


@dataclass
class Row:
    file: str
    size_key: str   # "40-bit", "50-digit", etc.
    env_name: str   # "baseline" or "GNFS_…"
    env_value: str
    wall_text: str  # "5.30s" / "120ms" / "1.5m"
    wall_ms: Optional[int]
    status: str     # "PASS" / "FAIL" / "TIMEOUT" / "DRY"
    delta_pct: Optional[float]  # Δ vs baseline within the same size


@dataclass
class Sweep:
    file: str
    metadata: dict = field(default_factory=dict)
    rows: list[Row] = field(default_factory=list)


# ──────────────────────────────────────────────────────────────────
# Parsing
# ──────────────────────────────────────────────────────────────────


WALL_TIME_RE = re.compile(
    r"""
    ^\s*([0-9]+(?:\.[0-9]+)?)\s*(ms|s|m)\s*$
    """,
    re.VERBOSE,
)


def parse_wall_time(text: str) -> Optional[int]:
    """Return milliseconds, or None when unparsable."""
    text = text.strip()
    if not text or text in {"—", "-", "(no PASS)"}:
        return None
    m = WALL_TIME_RE.match(text)
    if not m:
        return None
    val, unit = float(m.group(1)), m.group(2)
    if unit == "ms":
        return int(val)
    if unit == "s":
        return int(val * 1000)
    if unit == "m":
        return int(val * 60_000)
    return None


def parse_delta(text: str) -> Optional[float]:
    text = text.strip().rstrip("%").rstrip("|").strip()
    if not text or text == "—":
        return None
    text = text.replace("−", "-")  # unicode minus
    try:
        return float(text.rstrip("%"))
    except ValueError:
        return None


def parse_sweep_file(path: Path) -> Sweep:
    """Parse one sweep_*.md report."""
    sweep = Sweep(file=str(path))
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()

    # Metadata: first | Key | Value | table.
    in_meta = False
    for ln in lines:
        if ln.strip().startswith("| Key |"):
            in_meta = True
            continue
        if in_meta:
            if not ln.strip().startswith("|"):
                if ln.strip() == "":
                    continue
                in_meta = False
                continue
            cells = [c.strip() for c in ln.strip("|").split("|")]
            if len(cells) >= 2 and "---" not in cells[0]:
                sweep.metadata[cells[0]] = cells[1]

    # Per-size results: sections starting with "## Results: …".
    current_size = None
    in_results = False
    for ln in lines:
        h = ln.strip()
        if h.startswith("## Results:"):
            current_size = h[len("## Results:"):].strip()
            # Extract leading size token, e.g. "40-bit" from "40-bit (N=…)".
            sk_match = re.match(r"([0-9]+-(?:bit|digit))", current_size)
            current_size = sk_match.group(1) if sk_match else current_size
            in_results = True
            continue
        if in_results:
            if h.startswith("##"):
                in_results = False
                continue
            if not h.startswith("|"):
                continue
            cells = [c.strip() for c in h.strip("|").split("|")]
            if len(cells) < 5:
                continue
            if "---" in cells[0]:
                continue
            if cells[0] == "ENV":
                continue
            env_name = cells[0].strip("`")
            env_value = cells[1]
            wall_text = cells[2]
            status = cells[3]
            delta = parse_delta(cells[4])
            sweep.rows.append(
                Row(
                    file=str(path),
                    size_key=current_size or "?",
                    env_name=env_name,
                    env_value=env_value,
                    wall_text=wall_text,
                    wall_ms=parse_wall_time(wall_text),
                    status=status,
                    delta_pct=delta,
                )
            )
    return sweep


# ──────────────────────────────────────────────────────────────────
# Aggregation
# ──────────────────────────────────────────────────────────────────


def aggregate(sweeps: list[Sweep]) -> dict:
    """Build cross-run analysis."""
    rows: list[Row] = []
    for s in sweeps:
        rows.extend(s.rows)

    # Per (env, size) → list of deltas.
    per_env_size: dict[tuple[str, str], list[float]] = defaultdict(list)
    per_env_all_sizes: dict[str, list[float]] = defaultdict(list)
    for r in rows:
        if r.env_name == "baseline":
            continue
        if r.status != "PASS":
            continue
        if r.delta_pct is None:
            continue
        key = f"{r.env_name}={r.env_value}"
        per_env_size[(key, r.size_key)].append(r.delta_pct)
        per_env_all_sizes[key].append(r.delta_pct)

    # Best Δ% per (env, size).
    best_per_env_size = {
        k: min(v) for k, v in per_env_size.items() if v
    }

    # Median Δ% per env over all sizes.
    def median(xs: list[float]) -> float:
        s = sorted(xs)
        n = len(s)
        return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])

    median_per_env = {
        k: median(v) for k, v in per_env_all_sizes.items() if v
    }

    # Sizes seen.
    sizes_seen = sorted({s for (_, s) in per_env_size.keys()})

    return {
        "sweep_count": len(sweeps),
        "row_count": len(rows),
        "sizes": sizes_seen,
        "per_env_size_best": best_per_env_size,
        "per_env_median": median_per_env,
        "per_env_all_sizes": per_env_all_sizes,
    }


# ──────────────────────────────────────────────────────────────────
# Rendering
# ──────────────────────────────────────────────────────────────────


def render_markdown(agg: dict, top_n: int) -> str:
    out: list[str] = []
    out.append("# GNFS Sweep Cross-Run Aggregate")
    out.append("")
    out.append(f"- Sweep files: {agg['sweep_count']}")
    out.append(f"- Row count:   {agg['row_count']}")
    out.append(f"- Sizes seen:  {', '.join(agg['sizes'])}")
    out.append("")

    out.append("## Median Δ% per ENV value across all sizes")
    out.append("")
    out.append("Lower (more negative) = consistent speed-up.")
    out.append("")
    out.append("| Rank | ENV=value | Median Δ% | Samples |")
    out.append("|------|-----------|-----------|---------|")
    ranked = sorted(agg["per_env_median"].items(), key=lambda kv: kv[1])
    for i, (k, v) in enumerate(ranked[:top_n], start=1):
        samples = len(agg["per_env_all_sizes"][k])
        out.append(f"| {i} | `{k}` | {v:+.1f}% | {samples} |")
    out.append("")

    out.append("## Best ENV per size")
    out.append("")
    out.append("Single most negative Δ% achieved at each size band.")
    out.append("")
    out.append("| N size | Best ENV=value | Best Δ% |")
    out.append("|--------|----------------|---------|")
    per_size_best: dict[str, tuple[str, float]] = {}
    for (k, sz), v in agg["per_env_size_best"].items():
        if sz not in per_size_best or v < per_size_best[sz][1]:
            per_size_best[sz] = (k, v)
    for sz in agg["sizes"]:
        if sz in per_size_best:
            k, v = per_size_best[sz]
            out.append(f"| {sz} | `{k}` | {v:+.1f}% |")
        else:
            out.append(f"| {sz} | (none) | — |")
    out.append("")

    out.append("## Scaling trend per ENV value")
    out.append("")
    out.append("How the speed-up evolves from smaller to larger N.")
    out.append("ENVs that improve as N grows are most worth enabling on large jobs.")
    out.append("")
    header = "| ENV=value | " + " | ".join(agg["sizes"]) + " |"
    sep = "|" + "|".join(["---"] * (len(agg["sizes"]) + 1)) + "|"
    out.append(header)
    out.append(sep)
    keys = sorted(
        {k for (k, _) in agg["per_env_size_best"].keys()},
        key=lambda x: agg["per_env_median"].get(x, 0),
    )
    for k in keys:
        cells = []
        for sz in agg["sizes"]:
            v = agg["per_env_size_best"].get((k, sz))
            cells.append(f"{v:+.1f}%" if v is not None else "—")
        out.append(f"| `{k}` | " + " | ".join(cells) + " |")
    out.append("")

    return "\n".join(out)


# ──────────────────────────────────────────────────────────────────
# Main
# ──────────────────────────────────────────────────────────────────


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description="Cross-run aggregator for GNFS perf sweep reports.",
    )
    ap.add_argument(
        "files",
        nargs="+",
        type=Path,
        help="One or more sweep_*.md files produced by sweep_full.sh",
    )
    ap.add_argument(
        "--json",
        action="store_true",
        help="Emit machine-readable JSON instead of markdown",
    )
    ap.add_argument(
        "--top",
        type=int,
        default=10,
        help="Top-N entries to show in 'median Δ%%' table (default 10)",
    )
    ap.add_argument(
        "--out",
        type=Path,
        default=None,
        help="Write report to file (default: stdout)",
    )
    args = ap.parse_args(argv)

    sweeps: list[Sweep] = []
    for f in args.files:
        if not f.is_file():
            print(f"warn: {f} is not a file, skipping", file=sys.stderr)
            continue
        try:
            sweeps.append(parse_sweep_file(f))
        except Exception as exc:  # noqa: BLE001
            print(f"warn: parse {f} failed: {exc}", file=sys.stderr)
    if not sweeps:
        print("error: no parseable sweep files supplied", file=sys.stderr)
        return 2

    agg = aggregate(sweeps)

    if args.json:
        # JSON-friendly form (tuple keys → "env|size" strings).
        json_safe = {
            "sweep_count": agg["sweep_count"],
            "row_count": agg["row_count"],
            "sizes": agg["sizes"],
            "per_env_size_best": {
                f"{k}|{sz}": v for (k, sz), v in agg["per_env_size_best"].items()
            },
            "per_env_median": agg["per_env_median"],
        }
        out_str = json.dumps(json_safe, indent=2, ensure_ascii=False)
    else:
        out_str = render_markdown(agg, args.top)

    if args.out:
        args.out.write_text(out_str + "\n", encoding="utf-8")
        print(f"wrote {args.out}", file=sys.stderr)
    else:
        print(out_str)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

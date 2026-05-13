#!/usr/bin/env python3
"""
scripts/perf/parse-trace.py

Parse xctrace XML export and emit a markdown summary.

Usage:
    python3 parse-trace.py <trace.xml>                   # single-trace summary
    python3 parse-trace.py <baseline.xml> <pgo.xml>      # diff mode

xctrace's "CPU Counters" template on Apple M-series runs in "CPU Bottlenecks"
mode and emits aggregated counters as a 4-element uint64 array per sample.
Per the ToC's CounterMetric metricLegend, the 5 metric names are:
  index 0: Cycles
  index 1: Instruction Delivery Bottleneck
  index 2: Discarded Bottleneck
  index 3: Instruction Processing Bottleneck
  index 4: Useful

The array has 4 slots; sum-to-cycles invariant does not hold across columns
(may represent overlapping/independent attributions). We therefore emit:
  - raw sums of each of the 4 columns
  - relative distribution (each / sum-of-all-4) as a stable cross-build
    comparison metric

For PGO impact analysis the *relative shifts* across the 4 columns are
the actionable signal, not the absolute mapping.

Stdlib only (xml.etree.ElementTree).

See docs/perf/performance-doctrine.md §5.5.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Optional
import xml.etree.ElementTree as ET


# 4 columns in uint64-array (semantic mapping discovered empirically — Apple
# does not document the column order. Labels here are best-effort, but the
# diff output is insensitive to label correctness: only column-wise Δ matters.)
METRIC_NAMES = [
    "col0_Discarded",
    "col1_Processing",
    "col2_Delivery",
    "col3_Cycles",
]


def parse_rows(xml_path: Path) -> list[dict]:
    """Parse xctrace XML rows; return list of {"start_ns": int, "dur_ns": int, "values": [int,int,int,int], "precise": bool}.

    xctrace uses uint64-array elements with text content like "925 1049 4715 3251"
    and an id/ref dedup mechanism: rows can reference a prior element via
    <uint64-array ref="3"/> to reuse the previous value.
    """
    tree = ET.parse(xml_path)
    root = tree.getroot()

    # First pass: build id -> raw-text dictionary for cross-row references
    id_table: dict[str, str] = {}
    for elem in root.iter():
        if "id" in elem.attrib and elem.text:
            id_table[elem.attrib["id"]] = elem.text.strip()

    def resolve(elem: ET.Element) -> Optional[str]:
        if elem is None:
            return None
        if "ref" in elem.attrib:
            return id_table.get(elem.attrib["ref"])
        return elem.text.strip() if elem.text else None

    rows: list[dict] = []
    for row in root.iter("row"):
        st = row.find("start-time")
        du = row.find("duration")
        ua = row.find("uint64-array")
        bo = row.find("boolean")

        st_v = resolve(st)
        du_v = resolve(du)
        ua_v = resolve(ua)
        bo_v = resolve(bo)

        if ua_v is None:
            continue
        try:
            values = [int(x) for x in ua_v.split()]
        except ValueError:
            continue
        if len(values) < 4:
            continue

        rows.append({
            "start_ns": int(st_v) if st_v else 0,
            "dur_ns":   int(du_v) if du_v else 0,
            "values":   values[:4],
            "precise":  (bo_v == "1"),
        })
    return rows


def aggregate(rows: list[dict]) -> dict:
    """Sum each of the 4 metric columns across all rows, separately for precise/imprecise."""
    sums = [0, 0, 0, 0]
    total_dur_ns = 0
    precise_dur_ns = 0
    n_rows = len(rows)
    for r in rows:
        for i in range(4):
            sums[i] += r["values"][i]
        total_dur_ns += r["dur_ns"]
        if r["precise"]:
            precise_dur_ns += r["dur_ns"]

    return {
        "sums":            sums,
        "total":           sum(sums),
        "total_dur_ns":    total_dur_ns,
        "precise_dur_ns":  precise_dur_ns,
        "n_rows":          n_rows,
    }


def derived(agg: dict) -> list[Optional[float]]:
    """Return each column as a % of the 4-column total."""
    total = agg["total"]
    if total == 0:
        return [None] * 4
    return [s / total * 100 for s in agg["sums"]]


def fmt_pct(v: Optional[float]) -> str:
    return f"{v:.2f}%" if v is not None else "n/a"


def fmt_int(n: int) -> str:
    return f"{n:,}"


def print_single(xml_path: Path) -> int:
    rows = parse_rows(xml_path)
    if not rows:
        print(f"WARNING: no counter rows parsed from {xml_path}", file=sys.stderr)
        return 1

    agg = aggregate(rows)
    d = derived(agg)

    print(f"# Trace summary: {xml_path.name}")
    print()
    print(f"- Samples (rows):     **{agg['n_rows']}**")
    print(f"- Total sample dur:   **{agg['total_dur_ns'] / 1e6:.2f} ms**")
    print(f"  (of which precise: {agg['precise_dur_ns'] / 1e6:.2f} ms)")
    print()
    print("## Counter sums (4-column aggregation)")
    print()
    print("> xctrace CPU Bottlenecks emits 4-value uint64 array per sample.")
    print("> Labels are best-effort empirical (col3 typically Cycles); the")
    print("> diff between baseline/PGO is insensitive to label correctness.")
    print()
    print("| Column | Sum | % of total |")
    print("|---|---:|---:|")
    for i, name in enumerate(METRIC_NAMES):
        print(f"| {name} | {fmt_int(agg['sums'][i])} | {fmt_pct(d[i])} |")
    print(f"| **Total (all 4)** | **{fmt_int(agg['total'])}** | **100.00%** |")
    print()
    return 0


def print_diff(xml_a: Path, xml_b: Path) -> int:
    rows_a = parse_rows(xml_a)
    rows_b = parse_rows(xml_b)
    if not rows_a or not rows_b:
        print(f"ERROR: empty rows. a={len(rows_a)} b={len(rows_b)}", file=sys.stderr)
        return 1
    a = aggregate(rows_a)
    b = aggregate(rows_b)
    da = derived(a)
    db = derived(b)

    print(f"# PGO impact: {xml_a.name} vs {xml_b.name}")
    print()
    print(f"- A (baseline) samples: {a['n_rows']}, sample dur: {a['total_dur_ns']/1e6:.2f} ms")
    print(f"- B (after)    samples: {b['n_rows']}, sample dur: {b['total_dur_ns']/1e6:.2f} ms")
    print()
    print("## Absolute counter sums")
    print()
    print("| Column | A (baseline) | B (after) | Δ% |")
    print("|---|---:|---:|---:|")
    for i, name in enumerate(METRIC_NAMES):
        va, vb = a["sums"][i], b["sums"][i]
        if va == 0:
            d = "n/a"
        else:
            delta = (vb - va) / va * 100
            sign = "+" if delta >= 0 else ""
            d = f"{sign}{delta:.2f}%"
        print(f"| {name} | {fmt_int(va)} | {fmt_int(vb)} | {d} |")
    va, vb = a["total"], b["total"]
    d = f"{(vb-va)/va*100:+.2f}%" if va else "n/a"
    print(f"| **Total** | **{fmt_int(va)}** | **{fmt_int(vb)}** | **{d}** |")
    print()
    print("## Distribution shift (each col as % of total)")
    print()
    print("| Column | A% | B% | Δ pp |")
    print("|---|---:|---:|---:|")
    for i, name in enumerate(METRIC_NAMES):
        va, vb = da[i], db[i]
        if va is None or vb is None:
            d = "n/a"
        else:
            delta = vb - va
            sign = "+" if delta >= 0 else ""
            d = f"{sign}{delta:.2f}pp"
        print(f"| {name} | {fmt_pct(va)} | {fmt_pct(vb)} | {d} |")
    print()
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

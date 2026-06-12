#!/usr/bin/env python3
"""Aggregates fc_experiment CSV rows into per-cell statistics.

Standard library only (csv/statistics/math) so it runs anywhere the repo
builds. Output: a markdown table per (cell, solver) with the salvo kill rate,
a per-rocket KILL SHARE with its 95% Wilson interval, miss statistics
(RMS/mean/median), and kill-chain timing (charter §5.7).

Metric honesty note: the world adjudicates at most ONE kill per engagement
(the first kill ends it), so for N>1 the per-rocket share equals
salvo_rate/N by construction — it is a contribution metric, NOT an
independent per-shot probability. Only N=1 cells measure the true per-shot
p1 directly; independence comparisons must be built from an N=1 baseline.

Usage: python3 tools/experiment/aggregate.py results.csv [--md out.md]
"""

import argparse
import csv
import math
import statistics
import sys
from collections import defaultdict

TICK_S = 1.0 / 60.0


def wilson(successes: int, total: int, z: float = 1.96) -> tuple[float, float, float]:
    """Point estimate + 95% Wilson interval for a binomial proportion."""
    if total == 0:
        return 0.0, 0.0, 0.0
    p = successes / total
    denom = 1.0 + z * z / total
    center = (p + z * z / (2 * total)) / denom
    margin = z * math.sqrt(p * (1 - p) / total + z * z / (4 * total * total)) / denom
    return p, max(0.0, center - margin), min(1.0, center + margin)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv_path")
    parser.add_argument("--md", help="write the markdown table here (default: stdout)")
    args = parser.parse_args()

    with open(args.csv_path, newline="") as f:
        reader = csv.DictReader(f)
        fields = reader.fieldnames or []
        fixed = {"solver", "rep", "sim_seed", "gust_seed", "first_track_tick", "confirm_tick",
                 "launch_tick", "track_error_at_launch_m", "solver_tof_s", "fired", "rocket_id",
                 "miss_m", "detonated", "killed", "would_kill", "salvo_killed"}
        axis_names = [name for name in fields if name not in fixed]
        rows = list(reader)
    if not rows:
        print("empty csv", file=sys.stderr)
        return 1

    # Group rocket rows by (cell, solver); runs by (cell, solver, rep).
    cells = defaultdict(list)
    for row in rows:
        key = tuple(row[name] for name in axis_names) + (row["solver"],)
        cells[key].append(row)

    lines = []
    header = axis_names + ["solver", "runs", "fired%", "true p1 [95% CI]",
                           "kill share/rocket", "salvo kill", "rms miss(m)", "mean miss(m)",
                           "median miss(m)", "track err@launch(m)", "confirm(s)", "launch(s)"]
    lines.append("| " + " | ".join(header) + " |")
    lines.append("|" + "---|" * len(header))

    for key in sorted(cells):
        rocket_rows = cells[key]
        runs = defaultdict(list)
        for row in rocket_rows:
            runs[row["rep"]].append(row)
        n_runs = len(runs)
        fired_runs = sum(1 for rep_rows in runs.values() if rep_rows[0]["fired"] == "1")

        fired_rockets = [r for r in rocket_rows if r["fired"] == "1" and r["rocket_id"] != "0"]
        kills = sum(1 for r in fired_rockets if r["killed"] == "1")
        p1, _, _ = wilson(kills, len(fired_rockets))
        # would_kill is the UNCAPPED per-rocket Pk outcome (v4 sim change):
        # the true per-shot probability, valid at any salvo size.
        would_kills = sum(1 for r in fired_rockets if r.get("would_kill") == "1")
        true_p1, lo, hi = wilson(would_kills, len(fired_rockets))
        salvo_kills = sum(1 for rep_rowss in runs.values()
                          if any(r["salvo_killed"] == "1" for r in rep_rowss))
        salvo_rate = salvo_kills / n_runs if n_runs else 0.0

        misses = [float(r["miss_m"]) for r in fired_rockets]
        rms_miss = math.sqrt(sum(m * m for m in misses) / len(misses)) if misses else 0.0
        track_errs = [float(r["track_error_at_launch_m"]) for r in fired_rockets
                      if r["solver"] == "track"]
        confirms = [float(r["confirm_tick"]) * TICK_S for r in fired_rockets
                    if r["confirm_tick"] != "0"]
        launches = [float(r["launch_tick"]) * TICK_S for r in fired_rockets
                    if r["launch_tick"] != "0"]

        cell_axis = list(key[:-1])
        solver = key[-1]
        lines.append("| " + " | ".join(
            cell_axis + [
                solver,
                str(n_runs),
                f"{100.0 * fired_runs / n_runs:.0f}%" if n_runs else "-",
                f"{true_p1:.3f} [{lo:.3f}, {hi:.3f}]" if fired_rockets else "-",
                f"{p1:.3f}" if fired_rockets else "-",
                f"{salvo_rate:.3f}",
                f"{rms_miss:.1f}" if misses else "-",
                f"{statistics.fmean(misses):.1f}" if misses else "-",
                f"{statistics.median(misses):.1f}" if misses else "-",
                f"{statistics.fmean(track_errs):.1f}" if track_errs else "-",
                f"{statistics.fmean(confirms):.1f}" if confirms else "-",
                f"{statistics.fmean(launches):.1f}" if launches else "-",
            ]) + " |")

    output = "\n".join(lines) + "\n"
    if args.md:
        with open(args.md, "w") as f:
            f.write(output)
        print(f"wrote {args.md}")
    else:
        print(output)
    return 0


if __name__ == "__main__":
    sys.exit(main())

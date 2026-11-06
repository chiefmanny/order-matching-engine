#!/usr/bin/env python3
"""Draws results.png and results-dark.png from results/throughput.csv.

Duration uses only runs where the server survived with no client errors,
because failed RPCs return immediately and make a run look fast.
"""
import csv
import statistics
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = Path(__file__).parent
VERSIONS = {
    "coarse": ("Coarse locks (2024)", "-", "o"),
    "fine": ("Fine-grained locks (2024)", "--", "s"),
    "current": ("Current engine", ":", "^"),
}
THEMES = {
    "results.png": {
        "surface": "#fcfcfb", "primary": "#0b0b0b", "secondary": "#52514e",
        "muted": "#898781", "grid": "#e1e0d9", "axis": "#c3c2b7",
        "series": {"coarse": "#2a78d6", "fine": "#eb6834", "current": "#1baf7a"},
    },
    "results-dark.png": {
        "surface": "#1a1a19", "primary": "#f0efec", "secondary": "#c3c2b7",
        "muted": "#898781", "grid": "#2c2c2a", "axis": "#383835",
        "series": {"coarse": "#3987e5", "fine": "#d95926", "current": "#199e70"},
    },
}


def load():
    durations = defaultdict(list)
    aborts = defaultdict(int)
    with open(HERE / "results" / "throughput.csv", newline="") as f:
        for row in csv.DictReader(f):
            key = (row["Version"], int(row["Test Size"]))
            aborts[key] += row["Server"] != "alive"
            if row["Server"] == "alive" and row["Client Errors"] == "0":
                durations[key].append(int(row["Duration (ms)"]))
    return durations, aborts


def draw(filename, theme, durations, aborts):
    sizes = sorted({size for _, size in aborts})
    fig, (left, right) = plt.subplots(
        1, 2, figsize=(11, 4.6), dpi=200, facecolor=theme["surface"]
    )

    for ax in (left, right):
        ax.set_facecolor(theme["surface"])
        ax.spines[["top", "right", "left"]].set_visible(False)
        ax.spines["bottom"].set_color(theme["axis"])
        ax.tick_params(colors=theme["muted"], length=0, labelsize=9)
        ax.grid(axis="y", color=theme["grid"], linewidth=0.8)
        ax.set_axisbelow(True)

    for version, (label, style, marker) in VERSIONS.items():
        points = [s for s in sizes if durations[(version, s)]]
        left.plot(
            points,
            [statistics.median(durations[(version, s)]) / 1000 for s in points],
            label=label, color=theme["series"][version], linestyle=style,
            marker=marker, markersize=6, linewidth=2,
            markeredgecolor=theme["surface"], markeredgewidth=1,
        )
    left.set_xscale("log", base=2)
    left.set_xticks(sizes[::2], [f"{s:,}" for s in sizes[::2]])
    # Log on both axes, so linear growth doesn't look like an exponential blow-up.
    left.set_yscale("log", base=2)
    left.set_yticks([1, 2, 4, 8, 16], ["1", "2", "4", "8", "16"])
    left.minorticks_off()
    left.set_ylim(0.8, 24)
    left.set_xlabel("Orders per client", color=theme["secondary"], fontsize=10)
    left.set_ylabel("Median duration (s)", color=theme["secondary"], fontsize=10)
    left.set_title(
        "Locking strategy made no measurable difference",
        loc="left", color=theme["primary"], fontsize=12, fontweight="bold", pad=12,
    )
    legend = left.legend(frameon=False, fontsize=9, loc="upper left")
    for text in legend.get_texts():
        text.set_color(theme["secondary"])

    positions = range(len(sizes))
    counts = [aborts[("fine", s)] for s in sizes]
    bars = right.bar(positions, counts, width=0.7, color=theme["series"]["fine"])
    right.bar_label(
        bars, labels=[str(c) if c else "" for c in counts],
        color=theme["primary"], fontsize=9, padding=3,
    )
    right.set_xticks(list(positions)[::2], [f"{s:,}" for s in sizes[::2]])
    right.set_ylim(0, 10)
    right.set_yticks([0, 2, 4, 6, 8, 10])
    right.set_xlabel("Orders per client", color=theme["secondary"], fontsize=10)
    right.set_ylabel("Runs aborted, out of 10", color=theme["secondary"], fontsize=10)
    right.set_title(
        "Only the fine-grained server crashed",
        loc="left", color=theme["primary"], fontsize=12, fontweight="bold", pad=12,
    )
    total = sum(counts)
    other = sum(aborts[(v, s)] for v in ("coarse", "current") for s in sizes)
    right.text(
        0.02, 0.92,
        f"Fine-grained: {total} of {len(sizes) * 10} runs\n"
        f"Coarse and current: {other} of {len(sizes) * 20} runs",
        transform=right.transAxes, color=theme["secondary"], fontsize=9, va="top",
    )

    fig.tight_layout(w_pad=3)
    fig.savefig(HERE / filename, facecolor=theme["surface"])
    plt.close(fig)


def main():
    durations, aborts = load()
    for filename, theme in THEMES.items():
        draw(filename, theme, durations, aborts)


if __name__ == "__main__":
    main()

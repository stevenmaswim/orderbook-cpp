"""Draw the README charts from committed benchmark output (upgrade I).

Inputs (all committed, all produced by bench/run_all.py or the trace export):
  bench/results.json                          throughput medians
  bench/data/latency_hist_<book>_shape.csv    per-op latency histogram, last round
  bench/py_parity/py_parity_expected.json     end-state book of the py_parity run
Outputs: bench/plots/*.png. Every number drawn comes from those files.

Usage (needs matplotlib; the Python engine repo's venv has it):
    ../Order_book_project/.venv/bin/python3.11 tools/plot_bench.py
"""
from __future__ import annotations

import csv
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # files only, no window
import matplotlib.pyplot as plt  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "bench" / "plots"

# Validated categorical slots 1-3 (blue, orange, aqua) on a light surface;
# checked with the dataviz palette validator. Aqua is below 3:1 contrast, so
# every line is also labeled directly.
BLUE, ORANGE, AQUA = "#2a78d6", "#eb6834", "#1baf7a"
SURFACE, INK, INK_2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e1e0d9"


def style(ax) -> None:
    ax.set_facecolor(SURFACE)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)
    ax.tick_params(colors=INK_2, labelsize=9)
    ax.grid(color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)


def throughput_chart(results: dict) -> None:
    """One panel per workload: each has its own scale, so they are separate
    panels (never two scales on one axis)."""
    workloads = ["py_parity", "shape", "wide"]
    rows = results["throughput"]
    books = list(dict.fromkeys(r["book"] for r in rows))
    fig, axes = plt.subplots(1, 3, figsize=(12, 3.6), facecolor=SURFACE)
    for ax, wl in zip(axes, workloads):
        style(ax)
        ax.grid(axis="y", visible=False)
        vals = {r["book"]: r["orders_per_sec_median"] / 1e6 for r in rows if r["workload"] == wl}
        ys = range(len(books))
        ax.barh(list(ys), [vals[b] for b in books], height=0.55, color=BLUE)
        ax.set_yticks(list(ys), books)
        ax.invert_yaxis()  # first book on top
        top = max(vals.values())
        for y, b in zip(ys, books):
            ax.text(vals[b] + top * 0.02, y, f"{vals[b]:.1f}M", va="center", color=INK, fontsize=9)
        ax.set_xlim(0, top * 1.25)
        ax.set_title(f"{wl}", color=INK, fontsize=11, loc="left")
        ax.set_xlabel("orders/sec (millions), median", color=INK_2, fontsize=9)
    fig.suptitle("Throughput by book variant (one thread; bench/RESULTS.md)", color=INK, fontsize=12,
                 x=0.01, ha="left")
    fig.tight_layout()
    fig.savefig(OUT / "throughput.png", dpi=150)
    plt.close(fig)


def read_hist(path: Path) -> tuple[list[int], list[float]]:
    xs, counts = [], []
    with path.open() as f:
        for row in csv.DictReader(f):
            xs.append(int(row["latency_ns"]))
            counts.append(int(row["count"]))
    total = sum(counts)
    cum, acc = [], 0
    for c in counts:
        acc += c
        cum.append(acc / total)
    return xs, cum


def latency_chart() -> None:
    """CDF of per-op latency (all op kinds, shape workload) for three books.

    Linear x axis clipped at 600 ns so the body of the distribution is
    readable; the clipped tail (p99.9, max) is in the RESULTS.md table. Drawn
    as steps because the clock itself moves in about 41 ns steps, and a
    sample of 0 ns means "under one clock step"."""
    fig, ax = plt.subplots(figsize=(8, 4.2), facecolor=SURFACE)
    style(ax)
    books = (("baseline", BLUE), ("intrusive_array", ORANGE), ("pool_array", AQUA))
    for row, (book, color) in enumerate(books):
        xs, cum = read_hist(ROOT / f"bench/data/latency_hist_{book}_shape.csv")
        ax.step(xs, cum, where="post", color=color, linewidth=2, label=book)
        p50 = xs[next(k for k, c in enumerate(cum) if c >= 0.50)]
        p99 = xs[next(k for k, c in enumerate(cum) if c >= 0.99)]
        # Legend rows stacked in the empty lower-right area: a colored swatch
        # carries identity, the text stays in ink (text never takes the series
        # color), and the p50/p99 values label each line directly.
        y = 0.30 - row * 0.09
        ax.plot([395, 420], [y, y], color=color, linewidth=2)
        ax.text(428, y, f"{book}: p50 {p50} ns, p99 {p99} ns", va="center", color=INK, fontsize=9)
    ax.set_xlim(0, 600)
    ax.set_ylim(0, 1.02)
    ax.set_xlabel("latency per operation, ns (clipped at 600; samples include one clock read)",
                  color=INK_2, fontsize=9)
    ax.set_ylabel("fraction of operations at or below", color=INK_2, fontsize=9)
    ax.set_title("Per-operation latency CDF, shape workload, last round", color=INK, fontsize=12, loc="left")
    # The swatch + text rows above are the legend; no second legend box.
    fig.tight_layout()
    fig.savefig(OUT / "latency_cdf.png", dpi=150)
    plt.close(fig)


def depth_chart(expected: dict) -> None:
    """Cumulative resting qty away from the touch, py_parity end state (the
    C++ books and the Python engine end in this same state)."""
    bids = expected["bids"]  # best (highest) first
    asks = expected["asks"]  # best (lowest) first

    def cumulative(levels):
        px, qty, acc = [], [], 0
        for p, q in levels:
            acc += q
            px.append(p / 100)
            qty.append(acc)
        return px, qty

    fig, ax = plt.subplots(figsize=(8, 4.2), facecolor=SURFACE)
    style(ax)
    bx, by = cumulative(bids)
    axx, ayy = cumulative(asks)
    ax.step(bx, by, where="post", color=BLUE, linewidth=2)
    ax.step(axx, ayy, where="post", color=ORANGE, linewidth=2)
    ax.annotate(f"best bid {bx[0]:.2f}", (bx[0], by[0]), xytext=(6, 14), textcoords="offset points",
                color=INK, fontsize=9)
    ax.annotate(f"best ask {axx[0]:.2f}", (axx[0], ayy[0]), xytext=(-78, 14), textcoords="offset points",
                color=INK, fontsize=9)
    ax.set_xlabel("price ($, ticks / 100)", color=INK_2, fontsize=9)
    ax.set_ylabel("cumulative resting qty", color=INK_2, fontsize=9)
    ax.set_title("Book depth at the end of the py_parity workload", color=INK, fontsize=12, loc="left")
    ax.legend(["bids", "asks"], frameon=False, fontsize=9, labelcolor=INK, loc="upper center")
    fig.tight_layout()
    fig.savefig(OUT / "depth.png", dpi=150)
    plt.close(fig)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    results = json.loads((ROOT / "bench/results.json").read_text())
    expected = json.loads((ROOT / "bench/py_parity/py_parity_expected.json").read_text())
    throughput_chart(results)
    latency_chart()
    depth_chart(expected)
    print(f"wrote {', '.join(p.name for p in sorted(OUT.glob('*.png')))} to {OUT}")


if __name__ == "__main__":
    main()

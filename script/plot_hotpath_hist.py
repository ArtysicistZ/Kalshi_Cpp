#!/usr/bin/env python3
"""Plot the latency histogram emitted by bench_hotpath_pipe.

Reads:
  /tmp/bench_hotpath_pipe_cycles.bin   — raw uint64 TSC-cycle samples
  /tmp/bench_hotpath_pipe_meta.txt     — key/value: ns_per_cycle, n_samples,
                                         producer_core, consumer_core

Writes:
  --out PNG (default ./hotpath_latency_histogram.png)

Run inside the conda env that has matplotlib + numpy:
    source ~/miniconda3/etc/profile.d/conda.sh && conda activate motus
    python script/plot_hotpath_hist.py --out docs/hotpath_latency_histogram.png
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker


def read_meta(path: Path) -> dict[str, str]:
    meta: dict[str, str] = {}
    with path.open() as fh:
        for line in fh:
            parts = line.strip().split(maxsplit=1)
            if len(parts) == 2:
                meta[parts[0]] = parts[1]
    return meta


def fmt_ns(ns: float) -> str:
    if ns < 1_000:
        return f"{ns:.0f} ns"
    if ns < 1_000_000:
        return f"{ns / 1_000:.2f} µs"
    return f"{ns / 1_000_000:.2f} ms"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--cycles",
        type=Path,
        default=Path("/tmp/bench_hotpath_pipe_cycles.bin"),
        help="raw cycle samples (uint64 little-endian)",
    )
    parser.add_argument(
        "--meta",
        type=Path,
        default=Path("/tmp/bench_hotpath_pipe_meta.txt"),
        help="meta file with ns_per_cycle and run info",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=Path("hotpath_latency_histogram.png"),
        help="output PNG path",
    )
    parser.add_argument(
        "--title",
        type=str,
        default=None,
        help="override plot title",
    )
    args = parser.parse_args()

    meta = read_meta(args.meta)
    ns_per_cycle = float(meta["ns_per_cycle"])
    n_samples = int(meta["n_samples"])
    prod = meta.get("producer_core", "?")
    cons = meta.get("consumer_core", "?")

    cycles = np.fromfile(args.cycles, dtype=np.uint64)
    if cycles.size != n_samples:
        print(f"warn: meta says {n_samples} samples, found {cycles.size}; using found")

    ns = cycles.astype(np.float64) * ns_per_cycle

    p50 = float(np.percentile(ns, 50))
    p90 = float(np.percentile(ns, 90))
    p99 = float(np.percentile(ns, 99))
    p999 = float(np.percentile(ns, 99.9))
    pmax = float(ns.max())

    # Log-spaced bins across the full observed range.
    lo = max(1.0, float(ns.min()))
    hi = pmax
    bins = np.logspace(np.log10(lo), np.log10(hi), 220)

    fig, ax = plt.subplots(figsize=(11.0, 5.5), dpi=160)

    # Solid histogram in the same teal as the README headline badge.
    color_bar = "#2a9d8f"
    ax.hist(
        ns,
        bins=bins,
        color=color_bar,
        alpha=0.92,
        edgecolor="none",
    )

    ax.set_xscale("log")
    ax.set_yscale("log")

    ax.set_xlabel("end-to-end latency per message", fontsize=11)
    ax.set_ylabel("count (log scale)", fontsize=11)

    title = args.title or (
        f"Hot-path pipeline latency  ·  {cycles.size:,} messages"
        f"  ·  AMD EPYC 7V12, cores {prod} / {cons} (same L3 CCX)"
    )
    ax.set_title(title, fontsize=12, pad=12)

    # Percentile vertical lines, labelled via legend (avoid overlap when
    # adjacent percentiles fall in the same log-decade).
    pct_lines = [
        ("p50",   p50,  "#264653"),
        ("p90",   p90,  "#e9c46a"),
        ("p99",   p99,  "#f4a261"),
        ("p99.9", p999, "#e76f51"),
    ]
    for label, value, color in pct_lines:
        ax.axvline(
            value,
            color=color,
            linestyle="--",
            linewidth=1.4,
            alpha=0.9,
            label=f"{label} = {fmt_ns(value)}",
        )
    ax.legend(
        loc="upper right",
        frameon=True,
        framealpha=0.95,
        edgecolor="#cccccc",
        fontsize=10,
        title="percentiles",
        title_fontsize=10,
    )

    # X tick formatting in ns / µs.
    def x_formatter(x: float, _pos: int) -> str:
        if x < 1_000:
            return f"{int(x)} ns"
        if x < 1_000_000:
            return f"{x / 1_000:.0f} µs"
        return f"{x / 1_000_000:.0f} ms"

    ax.xaxis.set_major_formatter(mticker.FuncFormatter(x_formatter))
    ax.xaxis.set_minor_formatter(mticker.NullFormatter())

    ax.grid(True, which="major", linestyle=":", alpha=0.35)
    ax.grid(True, which="minor", linestyle=":", alpha=0.15)

    # Caption: which knobs were on, what was measured.
    caption = (
        f"parse → SPSC handoff → orderbook → signal → reconcile → serialize  ·  "
        f"zero-allocation enforced  ·  rdtsc / rdtscp"
    )
    fig.text(
        0.5,
        0.005,
        caption,
        ha="center",
        fontsize=9,
        color="#555555",
    )

    plt.tight_layout(rect=(0, 0.02, 1, 1))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out, bbox_inches="tight")
    print(
        f"wrote {args.out}  "
        f"(p50={fmt_ns(p50)}  p90={fmt_ns(p90)}  p99={fmt_ns(p99)}  "
        f"p99.9={fmt_ns(p999)}  max={fmt_ns(pmax)})"
    )


if __name__ == "__main__":
    main()

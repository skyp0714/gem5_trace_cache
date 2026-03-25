#!/usr/bin/env python3

import argparse
import csv
import json
import math
import re
import subprocess
import sys
import time
from concurrent.futures import (
    ThreadPoolExecutor,
    as_completed,
)
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

JSON_RE = re.compile(r"JSON:\s+(.+)")


def is_power_of_two(value):
    return value > 0 and (value & (value - 1)) == 0


def channel_mapping_mode(channels):
    if not is_power_of_two(channels):
        raise ValueError(
            f"Unsupported channel count {channels}: only powers of two are allowed"
        )
    return "single" if channels == 1 else "striped"


def parse_args():
    repo_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(
        description=(
            "Sweep memory channels and DRAM types, then generate a heatmap "
            "of the fraction of traces where ZSTD latency is lower than LZ4."
        )
    )
    parser.add_argument(
        "--runner",
        default=str(repo_root / "src/cxl_objects/run_codec_latency_sweep.py"),
        help="Path to run_codec_latency_sweep.py",
    )
    parser.add_argument(
        "--gem5-bin",
        default=str(repo_root / "build/X86/gem5.opt"),
        help="Path to gem5.opt",
    )
    parser.add_argument(
        "--config",
        default=str(repo_root / "src/cxl_objects/config/cxl_trace_replay.py"),
        help="Path to the replay config",
    )
    parser.add_argument(
        "--trace-root",
        default=str(
            Path("~/cold-memory-model/output_traces/reconstruct").expanduser()
        ),
        help="Root containing tr*/reduced trace directories",
    )
    parser.add_argument(
        "--results-root",
        default=str(repo_root / "src/cxl_objects/results/zstd_lz4_heatmap"),
        help="Directory for cell summaries and rendered heatmaps",
    )
    parser.add_argument(
        "--dram-types",
        nargs="+",
        default=["DDR3_1600_8x8", "DDR4_2400_16x4", "DDR5_4400_4x8"],
        help="DRAM models to sweep, ordered from lower to higher bandwidth",
    )
    parser.add_argument(
        "--mem-channels",
        nargs="+",
        type=int,
        default=[2, 4, 8, 16],
        help="Memory-channel counts to sweep",
    )
    parser.add_argument(
        "--max-cores",
        type=int,
        default=72,
        help="Total gem5 worker budget across all concurrent cell sweeps",
    )
    parser.add_argument(
        "--parallel-cells",
        type=int,
        default=None,
        help="Number of cell sweeps to run concurrently; default is derived from --max-cores",
    )
    parser.add_argument(
        "--max-workers-per-cell",
        type=int,
        default=20,
        help="Maximum trace-level workers used inside each cell sweep",
    )
    parser.add_argument(
        "--timeout-seconds",
        type=int,
        default=360,
        help="Per-run timeout forwarded to run_codec_latency_sweep.py",
    )
    parser.add_argument(
        "--num-engines",
        type=int,
        default=1,
        help="Decompression engine count for all cells",
    )
    parser.add_argument(
        "--chunk-send-delay-ticks",
        type=int,
        default=24,
        help="Chunk-send delay for all cells",
    )
    parser.add_argument(
        "--inter-memory-request-delay-ticks",
        type=int,
        default=0,
        help="Inter-memory-request delay for all cells",
    )
    parser.add_argument(
        "--trace-ids",
        nargs="*",
        default=None,
        help="Optional subset of traces, e.g. tr1 tr3",
    )
    args = parser.parse_args()
    invalid_channels = [
        ch for ch in args.mem_channels if not is_power_of_two(ch)
    ]
    if invalid_channels:
        parser.error(
            "--mem-channels must all be powers of two; invalid values: "
            + ", ".join(str(ch) for ch in invalid_channels)
        )
    return args


def cell_command(
    args, cell_results_root, dram_type, mem_channels, workers_per_cell
):
    cmd = [
        sys.executable,
        args.runner,
        "--gem5-bin",
        args.gem5_bin,
        "--config",
        args.config,
        "--trace-root",
        args.trace_root,
        "--results-root",
        str(cell_results_root),
        "--max-workers",
        str(workers_per_cell),
        "--timeout-seconds",
        str(args.timeout_seconds),
        "--dram-type",
        dram_type,
        "--mem-channels",
        str(mem_channels),
        "--num-engines",
        str(args.num_engines),
        "--chunk-send-delay-ticks",
        str(args.chunk_send_delay_ticks),
        "--inter-memory-request-delay-ticks",
        str(args.inter_memory_request_delay_ticks),
    ]
    if args.trace_ids:
        cmd.extend(["--trace-ids", *args.trace_ids])
    return cmd


def run_cell(
    args, base_results_dir, dram_type, mem_channels, workers_per_cell
):
    cell_dir = base_results_dir / "cell_runs" / f"{dram_type}_ch{mem_channels}"
    cell_dir.mkdir(parents=True, exist_ok=True)
    cmd = cell_command(
        args, cell_dir, dram_type, mem_channels, workers_per_cell
    )
    completed = subprocess.run(
        cmd, capture_output=True, text=True, check=False
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"Cell sweep failed for {dram_type} / {mem_channels} channels\n"
            f"stdout:\n{completed.stdout}\n\nstderr:\n{completed.stderr}"
        )
    match = JSON_RE.search(completed.stdout)
    if not match:
        raise RuntimeError(
            f"Could not find summary JSON path for {dram_type} / {mem_channels} channels\n"
            f"stdout:\n{completed.stdout}"
        )
    summary_path = Path(match.group(1).strip())
    summary = json.loads(summary_path.read_text())
    per_trace = summary["per_trace"]
    better_count = sum(float(row["reduction_pct"]) > 0.0 for row in per_trace)
    fraction = better_count / len(per_trace) if per_trace else 0.0
    return {
        "dram_type": dram_type,
        "mem_channels": mem_channels,
        "mapping_mode": channel_mapping_mode(mem_channels),
        "trace_fraction_zstd_better": fraction,
        "better_traces": better_count,
        "total_traces": len(per_trace),
        "weighted_reduction_pct": summary["weighted"]["reduction_pct"],
        "weighted_lz4_avg_latency_ns": summary["weighted"][
            "lz4_avg_latency_ns"
        ],
        "weighted_zstd_avg_latency_ns": summary["weighted"][
            "zstd_avg_latency_ns"
        ],
        "summary_json": str(summary_path),
        "summary_csv": str(summary_path.with_name("per_trace_summary.csv")),
    }


def auto_parallelism(total_cells, max_cores, max_workers_per_cell):
    max_workers_per_cell = min(max_workers_per_cell, 20)
    parallel_cells = max(
        1, min(total_cells, max_cores // max_workers_per_cell)
    )
    if parallel_cells == 0:
        parallel_cells = 1
    workers_per_cell = max(
        1, min(max_workers_per_cell, max_cores // parallel_cells)
    )
    return parallel_cells, workers_per_cell


def write_csv(path, rows):
    fieldnames = [
        "dram_type",
        "mem_channels",
        "mapping_mode",
        "trace_fraction_zstd_better",
        "better_traces",
        "total_traces",
        "weighted_reduction_pct",
        "weighted_lz4_avg_latency_ns",
        "weighted_zstd_avg_latency_ns",
        "summary_json",
        "summary_csv",
    ]
    with path.open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def render_heatmap(
    path,
    dram_types,
    mem_channels,
    rows,
    title,
    value_key,
    value_label,
    vmin,
    vmax,
    cmap,
    formatter,
):
    matrix = np.full((len(dram_types), len(mem_channels)), np.nan)
    for row in rows:
        y = dram_types.index(row["dram_type"])
        x = mem_channels.index(row["mem_channels"])
        matrix[y, x] = row[value_key]

    fig, ax = plt.subplots(
        figsize=(1.4 * len(mem_channels) + 2, 1.2 * len(dram_types) + 2)
    )
    im = ax.imshow(matrix, vmin=vmin, vmax=vmax, cmap=cmap, aspect="auto")
    cbar = fig.colorbar(im, ax=ax)
    cbar.set_label(value_label)

    ax.set_xticks(range(len(mem_channels)))
    ax.set_xticklabels(mem_channels)
    ax.set_xlabel("Memory Channels")
    ax.set_yticks(range(len(dram_types)))
    ax.set_yticklabels(dram_types)
    ax.set_ylabel("DRAM Module (Lower to Higher Bandwidth)")
    ax.set_title(title)

    for y in range(len(dram_types)):
        for x in range(len(mem_channels)):
            value = matrix[y, x]
            if math.isnan(value):
                label = "NA"
                color = "white"
            else:
                label = formatter(value)
                midpoint = (vmin + vmax) / 2.0
                color = (
                    "white"
                    if abs(value - midpoint) > (vmax - vmin) * 0.25
                    else "black"
                )
            ax.text(
                x, y, label, ha="center", va="center", color=color, fontsize=10
            )

    fig.tight_layout()
    fig.savefig(path, dpi=200)
    plt.close(fig)


def main():
    args = parse_args()
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    base_results_dir = (
        Path(args.results_root).expanduser().resolve() / timestamp
    )
    base_results_dir.mkdir(parents=True, exist_ok=True)

    cells = [
        (dram, ch) for dram in args.dram_types for ch in args.mem_channels
    ]
    if args.parallel_cells is None:
        parallel_cells, workers_per_cell = auto_parallelism(
            len(cells), args.max_cores, args.max_workers_per_cell
        )
    else:
        parallel_cells = max(1, min(args.parallel_cells, len(cells)))
        workers_per_cell = max(
            1, min(args.max_workers_per_cell, args.max_cores // parallel_cells)
        )

    metadata = {
        "runner": args.runner,
        "gem5_bin": args.gem5_bin,
        "config": args.config,
        "trace_root": args.trace_root,
        "dram_types": args.dram_types,
        "mem_channels": args.mem_channels,
        "max_cores": args.max_cores,
        "parallel_cells": parallel_cells,
        "workers_per_cell": workers_per_cell,
        "num_engines": args.num_engines,
        "chunk_send_delay_ticks": args.chunk_send_delay_ticks,
        "inter_memory_request_delay_ticks": args.inter_memory_request_delay_ticks,
        "timeout_seconds": args.timeout_seconds,
        "note": (
            "Only power-of-two channel counts are supported. All channel "
            "counts in this sweep use 64B striped interleaving."
        ),
    }
    (base_results_dir / "metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n"
    )

    print(
        f"Running {len(cells)} cells with {parallel_cells} concurrent sweeps."
    )
    print(f"Each cell uses up to {workers_per_cell} trace workers.")
    print(f"Results root: {base_results_dir}")
    sys.stdout.flush()

    rows = []
    with ThreadPoolExecutor(max_workers=parallel_cells) as executor:
        futures = {
            executor.submit(
                run_cell,
                args,
                base_results_dir,
                dram_type,
                mem_channels,
                workers_per_cell,
            ): (dram_type, mem_channels)
            for dram_type, mem_channels in cells
        }
        for future in as_completed(futures):
            dram_type, mem_channels = futures[future]
            row = future.result()
            rows.append(row)
            print(
                f"[done] {dram_type} / ch{mem_channels}: "
                f"fraction={row['trace_fraction_zstd_better']:.2f}, "
                f"weighted_reduction={row['weighted_reduction_pct']:.2f}%"
            )
            sys.stdout.flush()

    rows.sort(
        key=lambda row: (
            args.dram_types.index(row["dram_type"]),
            args.mem_channels.index(row["mem_channels"]),
        )
    )
    csv_path = base_results_dir / "heatmap_cells.csv"
    write_csv(csv_path, rows)

    fraction_title = (
        "ZSTD Better-Than-LZ4 Fraction\n"
        f"engines={args.num_engines}, chunk_delay={args.chunk_send_delay_ticks}, "
        f"inter_req_delay={args.inter_memory_request_delay_ticks}"
    )
    fraction_png_path = base_results_dir / "zstd_lz4_heatmap.png"
    render_heatmap(
        fraction_png_path,
        args.dram_types,
        args.mem_channels,
        rows,
        fraction_title,
        "trace_fraction_zstd_better",
        "Fraction of Traces Where ZSTD < LZ4",
        0.0,
        1.0,
        "viridis",
        lambda value: f"{value:.2f}",
    )

    reduction_title = (
        "Weighted ZSTD Latency Reduction vs LZ4\n"
        f"engines={args.num_engines}, chunk_delay={args.chunk_send_delay_ticks}, "
        f"inter_req_delay={args.inter_memory_request_delay_ticks}"
    )
    reduction_png_path = (
        base_results_dir / "zstd_lz4_weighted_reduction_heatmap.png"
    )
    render_heatmap(
        reduction_png_path,
        args.dram_types,
        args.mem_channels,
        rows,
        reduction_title,
        "weighted_reduction_pct",
        "Weighted Latency Reduction (%)",
        -20.0,
        20.0,
        "coolwarm",
        lambda value: f"{value:.1f}%",
    )

    print()
    print(f"Cell CSV: {csv_path}")
    print(f"Fraction heatmap: {fraction_png_path}")
    print(f"Reduction heatmap: {reduction_png_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

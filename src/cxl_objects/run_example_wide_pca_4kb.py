#!/usr/bin/env python3

import argparse
import csv
import math
import os
import re
import signal
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


TRACE_RE = re.compile(
    r"^ws(?P<ws>\d+)_ac(?P<ac>\d+)_tl(?P<tl>\d+)_sl(?P<sl>\d+)_ts(?P<ts>\d+)_(?P<codec>lz4|zstd)_4096\.txt$"
)
AVG_RE = re.compile(r"Average Latency:\s+([0-9.]+)")
HIT_LAT_RE = re.compile(r"Average Hit Latency:\s+([0-9.]+)")
MISS_LAT_RE = re.compile(r"Average Miss Latency:\s+([0-9.]+)")
HIT_RATE_RE = re.compile(r"Cache Hit Rate:\s+([0-9.]+)")
BYTES_READ_RE = re.compile(
    r"(system\.shared_mem_ctrl_\d+\.dram\.bytesRead::total)\s+([0-9]+)"
)


@dataclass(frozen=True)
class TraceJob:
    trace_path: Path
    trace_name: str
    stem: str
    ws: int
    ac: int
    tl: int
    sl: int
    ts: int
    codec: str
    request_count: int


@dataclass
class TraceResult:
    trace_name: str
    trace_path: str
    stem: str
    ws: int
    ac: int
    tl: int
    sl: int
    ts: int
    codec: str
    request_count: int
    avg_latency_ns: float
    avg_hit_latency_ns: float
    avg_miss_latency_ns: float
    hit_rate_pct: float
    completed_rows: int
    summary_complete: bool
    total_memory_read_bytes: int
    bandwidth_inflation: float
    run_dir: str
    latency_log: str
    stats_file: str
    stdout_file: str
    stderr_file: str
    elapsed_sec: float


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(
        description=(
            "Run all traces in example_wide_pca_4kb, summarize latency and "
            "bandwidth inflation, and generate tl/sl heatmaps."
        )
    )
    parser.add_argument(
        "--gem5-bin",
        default=str(repo_root / "build/X86/gem5.fast"),
        help="Path to gem5 binary",
    )
    parser.add_argument(
        "--config",
        default=str(repo_root / "src/cxl_objects/config/cxl_trace_replay.py"),
        help="Path to cxl_trace_replay.py",
    )
    parser.add_argument(
        "--trace-root",
        default="/home/hnpark2/cold-memory-model/output_traces/example_wide_pca_4kb",
        help="Directory containing input traces",
    )
    parser.add_argument(
        "--results-root",
        default=str(
            repo_root / "src/cxl_objects/results/example_wide_pca_4kb"
        ),
        help="Root directory for logs, run dirs, csv, and plots",
    )
    parser.add_argument(
        "--parallelism",
        type=int,
        default=64,
        help="Maximum number of gem5 simulations to run concurrently",
    )
    parser.add_argument(
        "--block-size",
        type=int,
        default=4096,
        help="Compression block size passed to the simulator",
    )
    parser.add_argument(
        "--num-engines",
        type=int,
        default=8,
        help="Number of decompression engines passed to the simulator",
    )
    parser.add_argument(
        "--mem-channels",
        type=int,
        default=4,
        help="Number of shared DRAM channels passed to the simulator",
    )
    parser.add_argument(
        "--dram-type",
        default="DDR4_2400_16x4",
        help="Shared DRAM interface model passed to the simulator",
    )
    parser.add_argument(
        "--l1-size",
        default="16MB",
        help="L1 cache capacity passed to the simulator config",
    )
    parser.add_argument(
        "--l1-assoc",
        type=int,
        default=8,
        help="L1 cache associativity passed to the simulator config",
    )
    parser.add_argument(
        "--chunk-send-delay-ticks",
        type=int,
        default=3,
        help="Chunk generation delay passed to the simulator",
    )
    parser.add_argument(
        "--inter-memory-request-delay-ticks",
        type=int,
        default=1000,
        help="Inter-memory request delay passed to the simulator",
    )
    parser.add_argument(
        "--timeout-seconds",
        type=int,
        default=None,
        help="Optional timeout per gem5 run",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Rerun traces even if outputs already exist",
    )
    parser.add_argument(
        "--collect-only",
        action="store_true",
        help="Only parse existing outputs; do not launch gem5",
    )
    return parser.parse_args()


def count_requests(trace_path: Path) -> int:
    count = 0
    with trace_path.open() as fh:
        for line in fh:
            stripped = line.strip()
            if stripped and not stripped.startswith("//"):
                count += 1
    return count


def discover_jobs(trace_root: Path) -> list[TraceJob]:
    jobs: list[TraceJob] = []
    for trace_path in sorted(trace_root.iterdir()):
        if not trace_path.is_file():
            continue
        match = TRACE_RE.match(trace_path.name)
        if not match:
            continue
        groups = match.groupdict()
        jobs.append(
            TraceJob(
                trace_path=trace_path,
                trace_name=trace_path.name,
                stem=trace_path.stem,
                ws=int(groups["ws"]),
                ac=int(groups["ac"]),
                tl=int(groups["tl"]),
                sl=int(groups["sl"]),
                ts=int(groups["ts"]),
                codec=groups["codec"],
                request_count=count_requests(trace_path),
            )
        )
    if not jobs:
        raise ValueError(f"No matching traces found in {trace_root}")
    return jobs


def parse_latency_rows(
    latency_log: Path,
) -> tuple[float, float, float, float, int]:
    total_latency = 0.0
    hit_latency = 0.0
    miss_latency = 0.0
    hit_count = 0
    miss_count = 0
    completed_rows = 0
    with latency_log.open() as fh:
        for line in fh:
            stripped = line.strip()
            if not stripped.startswith("0x"):
                continue
            fields = stripped.split()
            if len(fields) < 5:
                continue
            latency_ns = float(fields[3])
            is_hit = fields[4] == "1"
            total_latency += latency_ns
            completed_rows += 1
            if is_hit:
                hit_latency += latency_ns
                hit_count += 1
            else:
                miss_latency += latency_ns
                miss_count += 1

    if completed_rows == 0:
        raise ValueError(f"No completed request rows found in {latency_log}")

    avg_latency = total_latency / completed_rows
    avg_hit_latency = hit_latency / hit_count if hit_count else math.nan
    avg_miss_latency = miss_latency / miss_count if miss_count else math.nan
    hit_rate_pct = 100.0 * hit_count / completed_rows
    return (
        avg_latency,
        avg_hit_latency,
        avg_miss_latency,
        hit_rate_pct,
        completed_rows,
    )


def parse_latency_metrics(
    latency_log: Path,
) -> tuple[float, float, float, float, int, bool]:
    text = latency_log.read_text()
    avg_match = AVG_RE.search(text)
    hit_match = HIT_LAT_RE.search(text)
    miss_match = MISS_LAT_RE.search(text)
    hit_rate_match = HIT_RATE_RE.search(text)
    if avg_match and hit_match and miss_match and hit_rate_match:
        row_metrics = parse_latency_rows(latency_log)
        return (
            float(avg_match.group(1)),
            float(hit_match.group(1)),
            float(miss_match.group(1)),
            float(hit_rate_match.group(1)),
            row_metrics[4],
            True,
        )
    row_metrics = parse_latency_rows(latency_log)
    return (*row_metrics, False)


def parse_total_memory_read_bytes(stats_file: Path) -> int:
    text = stats_file.read_text()
    last_values: dict[str, int] = {}
    for match in BYTES_READ_RE.finditer(text):
        last_values[match.group(1)] = int(match.group(2))
    if not last_values:
        raise ValueError(f"Could not find bytesRead::total in {stats_file}")
    return sum(last_values.values())


def run_one(
    job: TraceJob,
    gem5_bin: Path,
    config: Path,
    results_root: Path,
    block_size: int,
    num_engines: int,
    mem_channels: int,
    dram_type: str,
    l1_size: str,
    l1_assoc: int,
    chunk_send_delay_ticks: int,
    inter_memory_request_delay_ticks: int,
    timeout_seconds: int | None,
    force: bool,
    collect_only: bool,
) -> TraceResult:
    results_root.mkdir(parents=True, exist_ok=True)
    run_dir = results_root / "runs" / job.stem
    run_dir.mkdir(parents=True, exist_ok=True)

    latency_log = results_root / f"{job.stem}_latency.txt"
    stdout_file = run_dir / "stdout.txt"
    stderr_file = run_dir / "stderr.txt"
    stats_file = run_dir / "stats.txt"

    cmd = [
        str(gem5_bin),
        "-d",
        str(run_dir),
        str(config),
        f"--compression-block-size={block_size}",
        f"--num-engines={num_engines}",
        f"--mem-channels={mem_channels}",
        f"--dram-type={dram_type}",
        f"--l1-size={l1_size}",
        f"--l1-assoc={l1_assoc}",
        f"--chunk-send-delay-ticks={chunk_send_delay_ticks}",
        (
            "--inter-memory-request-delay-ticks="
            f"{inter_memory_request_delay_ticks}"
        ),
        f"--trace-file={job.trace_path}",
        f"--output-file={latency_log}",
    ]

    needs_run = (
        force
        or not latency_log.exists()
        or not stats_file.exists()
        or stats_file.stat().st_size == 0
    )

    start = time.monotonic()
    if collect_only and needs_run:
        raise FileNotFoundError(
            f"Missing outputs for collect-only mode: {job.trace_name}"
        )

    if needs_run:
        with stdout_file.open("w") as stdout, stderr_file.open("w") as stderr:
            proc = subprocess.Popen(
                cmd,
                stdout=stdout,
                stderr=stderr,
            )
            salvaged = False
            try:
                returncode = proc.wait(timeout=timeout_seconds)
            except subprocess.TimeoutExpired:
                salvaged = True
                proc.send_signal(signal.SIGUSR1)
                time.sleep(2)
                proc.send_signal(signal.SIGINT)
                returncode = proc.wait(timeout=30)
        if returncode != 0 and not salvaged:
            raise RuntimeError(
                f"gem5 failed for {job.trace_name} with return code {returncode}"
            )

    (
        avg_latency_ns,
        avg_hit_latency_ns,
        avg_miss_latency_ns,
        hit_rate_pct,
        completed_rows,
        summary_complete,
    ) = parse_latency_metrics(latency_log)
    total_memory_read_bytes = parse_total_memory_read_bytes(stats_file)
    bandwidth_inflation = total_memory_read_bytes / (
        job.request_count * 64.0
    )

    return TraceResult(
        trace_name=job.trace_name,
        trace_path=str(job.trace_path),
        stem=job.stem,
        ws=job.ws,
        ac=job.ac,
        tl=job.tl,
        sl=job.sl,
        ts=job.ts,
        codec=job.codec,
        request_count=job.request_count,
        avg_latency_ns=avg_latency_ns,
        avg_hit_latency_ns=avg_hit_latency_ns,
        avg_miss_latency_ns=avg_miss_latency_ns,
        hit_rate_pct=hit_rate_pct,
        completed_rows=completed_rows,
        summary_complete=summary_complete,
        total_memory_read_bytes=total_memory_read_bytes,
        bandwidth_inflation=bandwidth_inflation,
        run_dir=str(run_dir),
        latency_log=str(latency_log),
        stats_file=str(stats_file),
        stdout_file=str(stdout_file),
        stderr_file=str(stderr_file),
        elapsed_sec=time.monotonic() - start,
    )


def write_trace_csv(results_root: Path, results: list[TraceResult]) -> Path:
    out_path = results_root / "trace_summary.csv"
    fieldnames = [
        "trace_name",
        "trace_path",
        "stem",
        "ws",
        "ac",
        "tl",
        "sl",
        "ts",
        "codec",
        "request_count",
        "avg_latency_ns",
        "avg_hit_latency_ns",
        "avg_miss_latency_ns",
        "hit_rate_pct",
        "completed_rows",
        "summary_complete",
        "total_memory_read_bytes",
        "bandwidth_inflation",
        "run_dir",
        "latency_log",
        "stats_file",
        "stdout_file",
        "stderr_file",
        "elapsed_sec",
    ]
    with out_path.open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        for result in sorted(
            results,
            key=lambda r: (r.tl, r.sl, r.ws, r.ac, r.ts, r.codec, r.trace_name),
        ):
            writer.writerow(result.__dict__)
    return out_path


def write_cell_csvs(
    results_root: Path, results: list[TraceResult]
) -> tuple[Path, Path, Path]:
    by_key: dict[tuple[int, int, int, int, int], dict[str, TraceResult]] = {}
    for result in results:
        key = (result.ws, result.ac, result.tl, result.sl, result.ts)
        by_key.setdefault(key, {})[result.codec] = result

    zstd_rows = []
    lz4_bw_rows = []
    speedup_rows = []

    tls = sorted({result.tl for result in results})
    sls = sorted({result.sl for result in results})

    for sl in sls:
        for tl in tls:
            pair_count = 0
            zstd_better_count = 0
            lz4_bw_values = []
            zstd_speedups = []
            for (ws, ac, key_tl, key_sl, ts), codecs in by_key.items():
                if key_tl != tl or key_sl != sl:
                    continue
                lz4 = codecs.get("lz4")
                zstd = codecs.get("zstd")
                if lz4 is not None:
                    lz4_bw_values.append(lz4.bandwidth_inflation)
                if lz4 is not None and zstd is not None:
                    pair_count += 1
                    if zstd.avg_latency_ns < lz4.avg_latency_ns:
                        zstd_better_count += 1
                    if zstd.avg_latency_ns > 0:
                        zstd_speedups.append(
                            lz4.avg_latency_ns / zstd.avg_latency_ns
                        )

            pct = (
                (100.0 * zstd_better_count / pair_count) if pair_count else math.nan
            )
            avg_lz4_bw = (
                sum(lz4_bw_values) / len(lz4_bw_values)
                if lz4_bw_values
                else math.nan
            )
            avg_zstd_speedup = (
                sum(zstd_speedups) / len(zstd_speedups)
                if zstd_speedups
                else math.nan
            )
            zstd_rows.append(
                {
                    "tl": tl,
                    "sl": sl,
                    "pair_count": pair_count,
                    "zstd_better_count": zstd_better_count,
                    "zstd_better_pct": pct,
                }
            )
            lz4_bw_rows.append(
                {
                    "tl": tl,
                    "sl": sl,
                    "lz4_trace_count": len(lz4_bw_values),
                    "avg_lz4_bandwidth_inflation": avg_lz4_bw,
                }
            )
            speedup_rows.append(
                {
                    "tl": tl,
                    "sl": sl,
                    "pair_count": pair_count,
                    "avg_zstd_speedup_over_lz4": avg_zstd_speedup,
                }
            )

    zstd_csv = results_root / "zstd_better_heatmap_cells.csv"
    with zstd_csv.open("w", newline="") as fh:
        writer = csv.DictWriter(
            fh,
            fieldnames=[
                "tl",
                "sl",
                "pair_count",
                "zstd_better_count",
                "zstd_better_pct",
            ],
        )
        writer.writeheader()
        writer.writerows(zstd_rows)

    bw_csv = results_root / "lz4_bandwidth_inflation_heatmap_cells.csv"
    with bw_csv.open("w", newline="") as fh:
        writer = csv.DictWriter(
            fh,
            fieldnames=[
                "tl",
                "sl",
                "lz4_trace_count",
                "avg_lz4_bandwidth_inflation",
            ],
        )
        writer.writeheader()
        writer.writerows(lz4_bw_rows)

    speedup_csv = results_root / "zstd_speedup_heatmap_cells.csv"
    with speedup_csv.open("w", newline="") as fh:
        writer = csv.DictWriter(
            fh,
            fieldnames=[
                "tl",
                "sl",
                "pair_count",
                "avg_zstd_speedup_over_lz4",
            ],
        )
        writer.writeheader()
        writer.writerows(speedup_rows)

    return zstd_csv, bw_csv, speedup_csv


def build_matrix(
    rows: list[dict], value_key: str, tls: list[int], sls: list[int]
) -> np.ndarray:
    matrix = np.full((len(sls), len(tls)), np.nan)
    for row in rows:
        x = tls.index(int(row["tl"]))
        y = sls.index(int(row["sl"]))
        matrix[y, x] = float(row[value_key])
    return matrix


def render_heatmap(
    matrix: np.ndarray,
    tls: list[int],
    sls: list[int],
    out_path: Path,
    title: str,
    cbar_label: str,
    value_format: str,
    cmap: str,
) -> None:
    fig, ax = plt.subplots(
        figsize=(1.2 * len(tls) + 2.0, 1.0 * len(sls) + 2.0)
    )
    im = ax.imshow(matrix, origin="lower", aspect="auto", cmap=cmap)
    cbar = fig.colorbar(im, ax=ax)
    cbar.set_label(cbar_label)

    ax.set_title(title)
    ax.set_xlabel("TL #")
    ax.set_ylabel("SL #")
    ax.set_xticks(range(len(tls)))
    ax.set_xticklabels(tls)
    ax.set_yticks(range(len(sls)))
    ax.set_yticklabels(sls)

    for y in range(matrix.shape[0]):
        for x in range(matrix.shape[1]):
            value = matrix[y, x]
            label = "NA" if math.isnan(value) else format(value, value_format)
            ax.text(
                x,
                y,
                label,
                ha="center",
                va="center",
                color="white" if not math.isnan(value) else "black",
                fontsize=9,
            )

    fig.tight_layout()
    fig.savefig(out_path, dpi=200)
    plt.close(fig)


def main() -> int:
    args = parse_args()

    gem5_bin = Path(args.gem5_bin).resolve()
    config = Path(args.config).resolve()
    trace_root = Path(args.trace_root).resolve()
    results_root = Path(args.results_root).resolve()

    if not gem5_bin.exists():
        raise FileNotFoundError(f"gem5 binary not found: {gem5_bin}")
    if not config.exists():
        raise FileNotFoundError(f"config not found: {config}")
    if not trace_root.exists():
        raise FileNotFoundError(f"trace directory not found: {trace_root}")

    results_root.mkdir(parents=True, exist_ok=True)
    jobs = discover_jobs(trace_root)

    print(
        f"Discovered {len(jobs)} traces in {trace_root}. Running with parallelism={args.parallelism}.",
        flush=True,
    )

    results: list[TraceResult] = []
    with ThreadPoolExecutor(max_workers=args.parallelism) as executor:
        future_to_job = {
            executor.submit(
                run_one,
                job,
                gem5_bin,
                config,
                results_root,
                args.block_size,
                args.num_engines,
                args.mem_channels,
                args.dram_type,
                args.l1_size,
                args.l1_assoc,
                args.chunk_send_delay_ticks,
                args.inter_memory_request_delay_ticks,
                args.timeout_seconds,
                args.force,
                args.collect_only,
            ): job
            for job in jobs
        }
        completed = 0
        for future in as_completed(future_to_job):
            job = future_to_job[future]
            result = future.result()
            results.append(result)
            completed += 1
            print(
                f"[{completed}/{len(jobs)}] {job.trace_name}: "
                f"avg={result.avg_latency_ns:.2f} ns, "
                f"hit_rate={result.hit_rate_pct:.2f}%, "
                f"bw_inflation={result.bandwidth_inflation:.4f}, "
                f"rows={result.completed_rows}/{result.request_count}, "
                f"summary={'yes' if result.summary_complete else 'rows-only'}",
                flush=True,
            )

    trace_csv = write_trace_csv(results_root, results)
    zstd_cells_csv, lz4_bw_cells_csv, zstd_speedup_cells_csv = write_cell_csvs(
        results_root, results
    )

    tls = sorted({result.tl for result in results})
    sls = sorted({result.sl for result in results})

    with zstd_cells_csv.open() as fh:
        zstd_rows = list(csv.DictReader(fh))
    with lz4_bw_cells_csv.open() as fh:
        lz4_bw_rows = list(csv.DictReader(fh))
    with zstd_speedup_cells_csv.open() as fh:
        zstd_speedup_rows = list(csv.DictReader(fh))

    zstd_matrix = build_matrix(zstd_rows, "zstd_better_pct", tls, sls)
    lz4_bw_matrix = build_matrix(
        lz4_bw_rows, "avg_lz4_bandwidth_inflation", tls, sls
    )
    zstd_speedup_matrix = build_matrix(
        zstd_speedup_rows, "avg_zstd_speedup_over_lz4", tls, sls
    )

    zstd_plot = results_root / "zstd_better_pct_heatmap.png"
    render_heatmap(
        zstd_matrix,
        tls,
        sls,
        zstd_plot,
        title="ZSTD Better Than LZ4: Average Latency Comparison",
        cbar_label="% of Traces Where ZSTD Avg Latency < LZ4 Avg Latency",
        value_format=".1f",
        cmap="viridis",
    )

    lz4_bw_plot = results_root / "lz4_bandwidth_inflation_heatmap.png"
    render_heatmap(
        lz4_bw_matrix,
        tls,
        sls,
        lz4_bw_plot,
        title="Average LZ4 Bandwidth Inflation",
        cbar_label="Bandwidth Inflation",
        value_format=".3f",
        cmap="magma",
    )

    zstd_speedup_plot = results_root / "zstd_speedup_heatmap.png"
    render_heatmap(
        zstd_speedup_matrix,
        tls,
        sls,
        zstd_speedup_plot,
        title="Average ZSTD Speedup Over LZ4",
        cbar_label="Speedup (LZ4 Avg Latency / ZSTD Avg Latency)",
        value_format=".3f",
        cmap="cividis",
    )

    print(f"Trace CSV: {trace_csv}")
    print(f"ZSTD heatmap cell CSV: {zstd_cells_csv}")
    print(f"LZ4 BW heatmap cell CSV: {lz4_bw_cells_csv}")
    print(f"ZSTD speedup cell CSV: {zstd_speedup_cells_csv}")
    print(f"ZSTD heatmap image: {zstd_plot}")
    print(f"LZ4 BW heatmap image: {lz4_bw_plot}")
    print(f"ZSTD speedup heatmap image: {zstd_speedup_plot}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

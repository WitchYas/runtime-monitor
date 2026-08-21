#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import math
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


VIOLATION_TYPES = [
    "DEADLOCK_CYCLE",
    "NON_OWNER_UNLOCK",
    "SELF_DEADLOCK_CANDIDATE",
    "LOCK_HELD_AT_THREAD_EXIT",
    "HOLD_TIME_VIOLATION",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Analyze L5 runtime-monitor evaluation CSV files. "
            "High-resolution benchmark target_elapsed_ns is used as the "
            "primary runtime metric."
        )
    )

    parser.add_argument(
        "results_dir",
        nargs="?",
        default="results/l5/latest",
        help=(
            "Directory containing correctness.csv, performance.csv "
            "and stress.csv."
        ),
    )

    return parser.parse_args()


def read_csv(
    path: Path,
) -> List[Dict[str, str]]:

    if not path.exists():
        raise FileNotFoundError(
            f"Missing required file: {path}"
        )

    with path.open(
        "r",
        newline="",
        encoding="utf-8",
    ) as handle:

        return list(
            csv.DictReader(handle)
        )


def write_csv(
    path: Path,
    rows: List[Dict[str, object]],
    fieldnames: List[str],
) -> None:

    path.parent.mkdir(
        parents=True,
        exist_ok=True,
    )

    with path.open(
        "w",
        newline="",
        encoding="utf-8",
    ) as handle:

        writer = csv.DictWriter(
            handle,
            fieldnames=fieldnames,
            extrasaction="ignore",
        )

        writer.writeheader()
        writer.writerows(rows)


def as_float(
    value: object,
) -> Optional[float]:

    if value is None:
        return None

    text = str(value).strip()

    if text == "":
        return None

    try:
        return float(text)

    except ValueError:
        return None


def as_int(
    value: object,
) -> Optional[int]:

    number = as_float(value)

    return (
        None
        if number is None
        else int(number)
    )


def mean(
    values: Iterable[float],
) -> float:

    data = list(values)

    return (
        statistics.mean(data)
        if data
        else math.nan
    )


def stdev(
    values: Iterable[float],
) -> float:

    data = list(values)

    return (
        statistics.stdev(data)
        if len(data) >= 2
        else 0.0
    )


def median(
    values: Iterable[float],
) -> float:

    data = list(values)

    return (
        statistics.median(data)
        if data
        else math.nan
    )


def percentile(
    values: Iterable[float],
    p: float,
) -> float:

    data = sorted(values)

    if not data:
        return math.nan

    if len(data) == 1:
        return data[0]

    position = (
        len(data) - 1
    ) * p

    lower = math.floor(position)
    upper = math.ceil(position)

    if lower == upper:
        return data[lower]

    fraction = (
        position - lower
    )

    return (
        data[lower] * (1.0 - fraction)
        +
        data[upper] * fraction
    )


def fmt(
    value: float,
    digits: int = 3,
) -> str:

    if math.isnan(value):
        return "NA"

    return f"{value:.{digits}f}"


# ============================================================================
# Correctness analysis
# ============================================================================

def expected_behavior_ok(
    row: Dict[str, str],
) -> bool:
    """
    Behavioral correctness is deliberately separated
    from trace authority.

    A run may have the expected behavioral result while
    still being non-authoritative because transport loss
    made the trace incomplete.
    """

    target = row.get(
        "target",
        "",
    )

    expected = row.get(
        "expected_violation",
        "NONE",
    )

    exit_code = as_int(
        row.get(
            "exit_code"
        )
    )

    total_violations = (
        as_int(
            row.get(
                "total_violations"
            )
        )
        or 0
    )

    if target == "circular_deadlock":

        return (
            exit_code == 124
            and
            (
                as_int(
                    row.get(
                        "v_DEADLOCK_CYCLE"
                    )
                )
                or 0
            ) == 1
            and
            total_violations == 1
        )

    if exit_code != 0:
        return False

    if expected == "NONE":

        return (
            total_violations == 0
        )

    expected_count = (
        as_int(
            row.get(
                f"v_{expected}"
            )
        )
        or 0
    )

    return (
        expected_count == 1
        and
        total_violations == 1
    )


def summarize_correctness(
    rows: List[Dict[str, str]],
) -> List[Dict[str, object]]:

    grouped: Dict[
        str,
        List[Dict[str, str]],
    ] = defaultdict(list)

    for row in rows:

        grouped[
            row["target"]
        ].append(row)

    summary: List[
        Dict[str, object]
    ] = []

    for target in sorted(grouped):

        group = grouped[target]

        runs = len(group)

        behavioral_successes = sum(
            1
            for row in group
            if expected_behavior_ok(row)
        )

        authoritative_passes = sum(
            (
                as_int(
                    row.get("pass")
                )
                or 0
            )
            for row in group
        )

        complete_traces = sum(
            1
            for row in group
            if row.get(
                "trace_integrity"
            ) == "COMPLETE"
        )

        zero_false_violation_runs = 0

        if target == "correct_smoke":

            zero_false_violation_runs = sum(
                1
                for row in group
                if (
                    as_int(
                        row.get(
                            "total_violations"
                        )
                    )
                    or 0
                ) == 0
            )

        latency_values = [
            value
            for row in group
            if (
                value :=
                as_float(
                    row.get(
                        "deadlock_latency_ms"
                    )
                )
            ) is not None
        ]

        summary.append(
            {
                "target":
                    target,

                "runs":
                    runs,

                "behavioral_successes":
                    behavioral_successes,

                "behavioral_success_rate":
                    (
                        behavioral_successes / runs
                        if runs
                        else math.nan
                    ),

                "authoritative_passes":
                    authoritative_passes,

                "authoritative_pass_rate":
                    (
                        authoritative_passes / runs
                        if runs
                        else math.nan
                    ),

                "complete_traces":
                    complete_traces,

                "complete_trace_rate":
                    (
                        complete_traces / runs
                        if runs
                        else math.nan
                    ),

                "zero_false_violation_runs":
                    zero_false_violation_runs,

                "mean_deadlock_latency_ms":
                    mean(
                        latency_values
                    ),

                "median_deadlock_latency_ms":
                    median(
                        latency_values
                    ),

                "p95_deadlock_latency_ms":
                    percentile(
                        latency_values,
                        0.95,
                    ),

                "max_deadlock_latency_ms":
                    (
                        max(
                            latency_values
                        )
                        if latency_values
                        else math.nan
                    ),
            }
        )

    return summary


# ============================================================================
# Performance analysis
# ============================================================================

def target_seconds(
    row: Dict[str, str],
) -> Optional[float]:

    elapsed_ns = as_float(
        row.get(
            "target_elapsed_ns"
        )
    )

    if (
        elapsed_ns is None
        or
        elapsed_ns <= 0
    ):
        return None

    return (
        elapsed_ns
        / 1_000_000_000.0
    )


def summarize_performance(
    rows: List[Dict[str, str]],
) -> List[Dict[str, object]]:

    grouped: Dict[
        Tuple[str, int],
        Dict[
            str,
            List[Dict[str, str]],
        ],
    ] = defaultdict(
        lambda: defaultdict(list)
    )

    for row in rows:

        profile = row[
            "profile"
        ]

        threads = int(
            row["threads"]
        )

        mode = row[
            "mode"
        ]

        grouped[
            (
                profile,
                threads,
            )
        ][mode].append(row)

    summary: List[
        Dict[str, object]
    ] = []

    for (
        profile,
        threads,
    ), modes in sorted(
        grouped.items()
    ):

        baseline = modes.get(
            "baseline",
            [],
        )

        monitor = modes.get(
            "monitor",
            [],
        )

        # --------------------------------------------------------------------
        # PRIMARY timing metric:
        # benchmark CLOCK_MONOTONIC_RAW target_elapsed_ns
        # --------------------------------------------------------------------

        baseline_target_s = [
            value
            for row in baseline
            if (
                value :=
                target_seconds(row)
            ) is not None
        ]

        monitor_target_s = [
            value
            for row in monitor
            if (
                value :=
                target_seconds(row)
            ) is not None
        ]

        # --------------------------------------------------------------------
        # Secondary OS-level metrics
        # --------------------------------------------------------------------

        baseline_wall_s = [
            value
            for row in baseline
            if (
                value :=
                as_float(
                    row.get(
                        "wall_seconds"
                    )
                )
            ) is not None
        ]

        monitor_wall_s = [
            value
            for row in monitor
            if (
                value :=
                as_float(
                    row.get(
                        "wall_seconds"
                    )
                )
            ) is not None
        ]

        baseline_rss = [
            value
            for row in baseline
            if (
                value :=
                as_float(
                    row.get(
                        "max_rss_kb"
                    )
                )
            ) is not None
        ]

        monitor_rss = [
            value
            for row in monitor
            if (
                value :=
                as_float(
                    row.get(
                        "max_rss_kb"
                    )
                )
            ) is not None
        ]

        baseline_user_s = [
            value
            for row in baseline
            if (
                value :=
                as_float(
                    row.get(
                        "user_seconds"
                    )
                )
            ) is not None
        ]

        monitor_user_s = [
            value
            for row in monitor
            if (
                value :=
                as_float(
                    row.get(
                        "user_seconds"
                    )
                )
            ) is not None
        ]

        baseline_sys_s = [
            value
            for row in baseline
            if (
                value :=
                as_float(
                    row.get(
                        "sys_seconds"
                    )
                )
            ) is not None
        ]

        monitor_sys_s = [
            value
            for row in monitor
            if (
                value :=
                as_float(
                    row.get(
                        "sys_seconds"
                    )
                )
            ) is not None
        ]

        monitor_loss_ppm = [
            value
            for row in monitor
            if (
                value :=
                as_float(
                    row.get(
                        "loss_ppm"
                    )
                )
            ) is not None
        ]

        generated_values = [
            value
            for row in monitor
            if (
                value :=
                as_float(
                    row.get(
                        "generated"
                    )
                )
            ) is not None
        ]

        # --------------------------------------------------------------------
        # Corrected high-resolution overhead
        # --------------------------------------------------------------------

        baseline_target_mean = mean(
            baseline_target_s
        )

        monitor_target_mean = mean(
            monitor_target_s
        )

        runtime_overhead_pct = (
            math.nan
        )

        added_target_s = (
            math.nan
        )

        if (
            not math.isnan(
                baseline_target_mean
            )
            and
            baseline_target_mean > 0
            and
            not math.isnan(
                monitor_target_mean
            )
        ):

            added_target_s = (
                monitor_target_mean
                -
                baseline_target_mean
            )

            runtime_overhead_pct = (
                added_target_s
                /
                baseline_target_mean
            ) * 100.0

        # --------------------------------------------------------------------
        # Secondary /usr/bin/time overhead
        # --------------------------------------------------------------------

        baseline_wall_mean = mean(
            baseline_wall_s
        )

        monitor_wall_mean = mean(
            monitor_wall_s
        )

        os_wall_overhead_pct = (
            math.nan
        )

        if (
            not math.isnan(
                baseline_wall_mean
            )
            and
            baseline_wall_mean > 0
            and
            not math.isnan(
                monitor_wall_mean
            )
        ):

            os_wall_overhead_pct = (
                (
                    monitor_wall_mean
                    -
                    baseline_wall_mean
                )
                /
                baseline_wall_mean
            ) * 100.0

        # --------------------------------------------------------------------
        # Memory
        # --------------------------------------------------------------------

        baseline_rss_mean = mean(
            baseline_rss
        )

        monitor_rss_mean = mean(
            monitor_rss
        )

        rss_delta_kb = (
            math.nan
        )

        if (
            not math.isnan(
                baseline_rss_mean
            )
            and
            not math.isnan(
                monitor_rss_mean
            )
        ):

            rss_delta_kb = (
                monitor_rss_mean
                -
                baseline_rss_mean
            )

        # --------------------------------------------------------------------
        # Normalized monitor cost
        # --------------------------------------------------------------------

        total_ops = 0

        if baseline:

            total_ops = (
                as_int(
                    baseline[0].get(
                        "total_ops"
                    )
                )
                or 0
            )

        elif monitor:

            total_ops = (
                as_int(
                    monitor[0].get(
                        "total_ops"
                    )
                )
                or 0
            )

        mean_generated = mean(
            generated_values
        )

        added_ns_per_critical_section = (
            math.nan
        )

        if (
            not math.isnan(
                added_target_s
            )
            and
            total_ops > 0
        ):

            added_ns_per_critical_section = (
                added_target_s
                *
                1_000_000_000.0
                /
                total_ops
            )

        effective_added_ns_per_generated_event = (
            math.nan
        )

        if (
            not math.isnan(
                added_target_s
            )
            and
            not math.isnan(
                mean_generated
            )
            and
            mean_generated > 0
        ):

            effective_added_ns_per_generated_event = (
                added_target_s
                *
                1_000_000_000.0
                /
                mean_generated
            )

        # --------------------------------------------------------------------
        # Trace quality
        # --------------------------------------------------------------------

        complete_runs = sum(
            1
            for row in monitor
            if row.get(
                "trace_integrity"
            ) == "COMPLETE"
        )

        zero_false_violation_runs = sum(
            1
            for row in monitor
            if (
                as_int(
                    row.get(
                        "total_violations"
                    )
                )
                or 0
            ) == 0
        )

        summary.append(
            {
                "profile":
                    profile,

                "threads":
                    threads,

                "total_ops":
                    total_ops,

                "baseline_runs":
                    len(baseline),

                "monitor_runs":
                    len(monitor),

                "baseline_target_mean_s":
                    baseline_target_mean,

                "baseline_target_sd_s":
                    stdev(
                        baseline_target_s
                    ),

                "monitor_target_mean_s":
                    monitor_target_mean,

                "monitor_target_sd_s":
                    stdev(
                        monitor_target_s
                    ),

                "added_target_time_ms":
                    (
                        added_target_s
                        * 1000.0
                        if not math.isnan(
                            added_target_s
                        )
                        else math.nan
                    ),

                "runtime_overhead_pct":
                    runtime_overhead_pct,

                "added_ns_per_critical_section":
                    added_ns_per_critical_section,

                "effective_added_ns_per_generated_event":
                    effective_added_ns_per_generated_event,

                "baseline_os_wall_mean_s":
                    baseline_wall_mean,

                "monitor_os_wall_mean_s":
                    monitor_wall_mean,

                "os_wall_overhead_pct":
                    os_wall_overhead_pct,

                "baseline_mean_user_s":
                    mean(
                        baseline_user_s
                    ),

                "monitor_mean_user_s":
                    mean(
                        monitor_user_s
                    ),

                "baseline_mean_sys_s":
                    mean(
                        baseline_sys_s
                    ),

                "monitor_mean_sys_s":
                    mean(
                        monitor_sys_s
                    ),

                "baseline_mean_rss_kb":
                    baseline_rss_mean,

                "monitor_mean_rss_kb":
                    monitor_rss_mean,

                "rss_delta_kb":
                    rss_delta_kb,

                "mean_generated_events":
                    mean_generated,

                "mean_monitor_loss_ppm":
                    mean(
                        monitor_loss_ppm
                    ),

                "complete_monitor_runs":
                    complete_runs,

                "complete_monitor_rate":
                    (
                        complete_runs
                        /
                        len(monitor)
                        if monitor
                        else math.nan
                    ),

                "zero_false_violation_runs":
                    zero_false_violation_runs,
            }
        )

    return summary


# ============================================================================
# Stress analysis
# ============================================================================

def summarize_stress(
    rows: List[Dict[str, str]],
) -> List[Dict[str, object]]:

    grouped: Dict[
        int,
        List[Dict[str, str]],
    ] = defaultdict(list)

    for row in rows:

        grouped[
            int(
                row["threads"]
            )
        ].append(row)

    summary: List[
        Dict[str, object]
    ] = []

    for threads in sorted(grouped):

        group = grouped[
            threads
        ]

        loss_ppm_values = [
            value
            for row in group
            if (
                value :=
                as_float(
                    row.get(
                        "loss_ppm"
                    )
                )
            ) is not None
        ]

        dropped_values = [
            value
            for row in group
            if (
                value :=
                as_float(
                    row.get(
                        "dropped"
                    )
                )
            ) is not None
        ]

        target_s_values = [
            value
            for row in group
            if (
                value :=
                target_seconds(row)
            ) is not None
        ]

        wall_values = [
            value
            for row in group
            if (
                value :=
                as_float(
                    row.get(
                        "wall_seconds"
                    )
                )
            ) is not None
        ]

        rss_values = [
            value
            for row in group
            if (
                value :=
                as_float(
                    row.get(
                        "max_rss_kb"
                    )
                )
            ) is not None
        ]

        gap_values = [
            value
            for row in group
            if (
                value :=
                as_float(
                    row.get(
                        "stream_gaps"
                    )
                )
            ) is not None
        ]

        tainted_values = [
            value
            for row in group
            if (
                value :=
                as_float(
                    row.get(
                        "tainted_streams"
                    )
                )
            ) is not None
        ]

        complete_runs = sum(
            1
            for row in group
            if row.get(
                "trace_integrity"
            ) == "COMPLETE"
        )

        false_violation_runs = sum(
            1
            for row in group
            if (
                as_int(
                    row.get(
                        "total_violations"
                    )
                )
                or 0
            ) != 0
        )

        summary.append(
            {
                "threads":
                    threads,

                "runs":
                    len(group),

                "mean_target_elapsed_s":
                    mean(
                        target_s_values
                    ),

                "mean_os_wall_s":
                    mean(
                        wall_values
                    ),

                "mean_rss_kb":
                    mean(
                        rss_values
                    ),

                "mean_dropped_events":
                    mean(
                        dropped_values
                    ),

                "mean_loss_ppm":
                    mean(
                        loss_ppm_values
                    ),

                "median_loss_ppm":
                    median(
                        loss_ppm_values
                    ),

                "p95_loss_ppm":
                    percentile(
                        loss_ppm_values,
                        0.95,
                    ),

                "mean_stream_gaps":
                    mean(
                        gap_values
                    ),

                "mean_tainted_streams":
                    mean(
                        tainted_values
                    ),

                "complete_runs":
                    complete_runs,

                "complete_rate":
                    (
                        complete_runs
                        /
                        len(group)
                        if group
                        else math.nan
                    ),

                "false_violation_runs":
                    false_violation_runs,
            }
        )

    return summary


# ============================================================================
# Plots
# ============================================================================

def create_plots(
    results_dir: Path,
    correctness_rows: List[Dict[str, str]],
    performance_summary: List[Dict[str, object]],
    stress_summary: List[Dict[str, object]],
) -> bool:

    try:
        import matplotlib.pyplot as plt

    except ImportError:

        print(
            "[L5] matplotlib not available; "
            "skipping PNG plots."
        )

        return False

    plot_dir = (
        results_dir
        / "plots"
    )

    plot_dir.mkdir(
        parents=True,
        exist_ok=True,
    )

    profiles = sorted(
        {
            str(
                row["profile"]
            )
            for row
            in performance_summary
        }
    )

    for profile in profiles:

        rows = [
            row
            for row
            in performance_summary
            if row["profile"]
            == profile
        ]

        rows.sort(
            key=lambda row:
                int(
                    row["threads"]
                )
        )

        x = [
            int(
                row["threads"]
            )
            for row in rows
        ]

        y = [
            float(
                row[
                    "runtime_overhead_pct"
                ]
            )
            for row in rows
        ]

        fig, ax = (
            plt.subplots()
        )

        ax.plot(
            x,
            y,
            marker="o",
        )

        ax.set_xlabel(
            "Application threads"
        )

        ax.set_ylabel(
            "Runtime overhead (%)"
        )

        ax.set_title(
            "High-resolution runtime overhead "
            f"— {profile}"
        )

        ax.grid(
            True,
            alpha=0.25,
        )

        fig.tight_layout()

        fig.savefig(
            plot_dir
            /
            (
                "runtime_overhead_"
                f"{profile}.png"
            ),
            dpi=180,
        )

        plt.close(
            fig
        )

    if stress_summary:

        rows = sorted(
            stress_summary,
            key=lambda row:
                int(
                    row["threads"]
                ),
        )

        x = [
            int(
                row["threads"]
            )
            for row in rows
        ]

        y = [
            float(
                row[
                    "mean_loss_ppm"
                ]
            )
            for row in rows
        ]

        fig, ax = (
            plt.subplots()
        )

        ax.plot(
            x,
            y,
            marker="o",
        )

        ax.set_xlabel(
            "Application threads"
        )

        ax.set_ylabel(
            "Mean transport loss (ppm)"
        )

        ax.set_title(
            "Transport loss vs thread count"
        )

        ax.grid(
            True,
            alpha=0.25,
        )

        fig.tight_layout()

        fig.savefig(
            plot_dir
            /
            "transport_loss_vs_threads.png",
            dpi=180,
        )

        plt.close(
            fig
        )

    latency_values = [
        value
        for row
        in correctness_rows
        if row.get(
            "target"
        ) == "circular_deadlock"
        if (
            value :=
            as_float(
                row.get(
                    "deadlock_latency_ms"
                )
            )
        ) is not None
    ]

    if latency_values:

        fig, ax = (
            plt.subplots()
        )

        ax.plot(
            list(
                range(
                    1,
                    len(
                        latency_values
                    )
                    + 1,
                )
            ),
            latency_values,
            marker="o",
        )

        ax.set_xlabel(
            "Run"
        )

        ax.set_ylabel(
            "Observed detection latency (ms)"
        )

        ax.set_title(
            "Circular-deadlock "
            "detection latency proxy"
        )

        ax.grid(
            True,
            alpha=0.25,
        )

        fig.tight_layout()

        fig.savefig(
            plot_dir
            /
            "deadlock_detection_latency.png",
            dpi=180,
        )

        plt.close(
            fig
        )

    return True


# ============================================================================
# Markdown report
# ============================================================================

def write_markdown_report(
    path: Path,
    correctness_summary: List[Dict[str, object]],
    performance_summary: List[Dict[str, object]],
    stress_summary: List[Dict[str, object]],
) -> None:

    lines: List[str] = []

    lines.append(
        "# L5 Evaluation Summary"
    )

    lines.append("")

    lines.append(
        "Primary runtime overhead is computed from the benchmark's "
        "high-resolution `target_elapsed_ns` measurement obtained with "
        "`CLOCK_MONOTONIC_RAW`. `/usr/bin/time` wall time, CPU time and "
        "maximum RSS are retained as secondary operating-system-level "
        "measurements."
    )

    lines.append("")

    lines.append(
        "The circular-deadlock latency is an observed host-side proxy "
        "measured from the second target request message to the live "
        "`DEADLOCK_CYCLE` report."
    )

    lines.append("")

    # ------------------------------------------------------------------------
    # Correctness
    # ------------------------------------------------------------------------

    lines.append(
        "## Detection correctness"
    )

    lines.append("")

    lines.append(
        "Behavioral success is separated from trace authority: an "
        "execution can produce the expected violation behavior while "
        "still being non-authoritative if transport loss makes the "
        "trace incomplete."
    )

    lines.append("")

    lines.append(
        "| Target | Runs | Behavioral successes | Behavioral success | "
        "Authoritative passes | Authoritative rate | Complete traces | "
        "Mean deadlock latency (ms) |"
    )

    lines.append(
        "|---|---:|---:|---:|---:|---:|---:|---:|"
    )

    for row in correctness_summary:

        lines.append(
            "| "
            f"{row['target']} | "
            f"{row['runs']} | "
            f"{row['behavioral_successes']} | "
            f"{fmt(float(row['behavioral_success_rate']) * 100.0, 2)}% | "
            f"{row['authoritative_passes']} | "
            f"{fmt(float(row['authoritative_pass_rate']) * 100.0, 2)}% | "
            f"{row['complete_traces']}/{row['runs']} | "
            f"{fmt(float(row['mean_deadlock_latency_ms']), 3)} |"
        )

    lines.append("")

    # ------------------------------------------------------------------------
    # Performance
    # ------------------------------------------------------------------------

    lines.append(
        "## Runtime and memory overhead"
    )

    lines.append("")

    lines.append(
        "| Profile | Threads | Baseline target mean (ms) | "
        "Monitor target mean (ms) | Added time (ms) | "
        "Corrected overhead | Added ns / critical section | "
        "Effective added ns / generated event | RSS delta (KiB) | "
        "Mean loss (ppm) | Complete traces |"
    )

    lines.append(
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
    )

    for row in performance_summary:

        lines.append(
            "| "
            f"{row['profile']} | "
            f"{row['threads']} | "
            f"{fmt(float(row['baseline_target_mean_s']) * 1000.0, 3)} | "
            f"{fmt(float(row['monitor_target_mean_s']) * 1000.0, 3)} | "
            f"{fmt(float(row['added_target_time_ms']), 3)} | "
            f"{fmt(float(row['runtime_overhead_pct']), 2)}% | "
            f"{fmt(float(row['added_ns_per_critical_section']), 1)} | "
            f"{fmt(float(row['effective_added_ns_per_generated_event']), 1)} | "
            f"{fmt(float(row['rss_delta_kb']), 1)} | "
            f"{fmt(float(row['mean_monitor_loss_ppm']), 1)} | "
            f"{row['complete_monitor_runs']}/{row['monitor_runs']} |"
        )

    lines.append("")

    lines.append(
        "The per-event value above is an effective elapsed-time "
        "normalization, not the latency of one event; producer and "
        "consumer execution overlap asynchronously."
    )

    lines.append("")

    # ------------------------------------------------------------------------
    # Stress
    # ------------------------------------------------------------------------

    lines.append(
        "## Stress / scalability"
    )

    lines.append("")

    lines.append(
        "| Threads | Runs | Mean target time (s) | Mean loss (ppm) | "
        "P95 loss (ppm) | Mean stream gaps | Mean tainted streams | "
        "Complete traces | False-violation runs |"
    )

    lines.append(
        "|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
    )

    for row in stress_summary:

        lines.append(
            "| "
            f"{row['threads']} | "
            f"{row['runs']} | "
            f"{fmt(float(row['mean_target_elapsed_s']), 4)} | "
            f"{fmt(float(row['mean_loss_ppm']), 1)} | "
            f"{fmt(float(row['p95_loss_ppm']), 1)} | "
            f"{fmt(float(row['mean_stream_gaps']), 2)} | "
            f"{fmt(float(row['mean_tainted_streams']), 2)} | "
            f"{row['complete_runs']}/{row['runs']} | "
            f"{row['false_violation_runs']} |"
        )

    lines.append("")

    # ------------------------------------------------------------------------
    # Interpretation
    # ------------------------------------------------------------------------

    lines.append(
        "## Interpretation rules"
    )

    lines.append("")

    lines.append(
        "- Behavioral correctness and trace authority are reported "
        "separately."
    )

    lines.append(
        "- Normally terminating runs are authoritative only when "
        "transport and verifier state are complete."
    )

    lines.append(
        "- Stress runs with transport loss are expected to become "
        "`DEGRADED` rather than generate false state-dependent violations."
    )

    lines.append(
        "- Primary runtime overhead uses high-resolution in-target timing: "
        "`(mean monitor target time - mean baseline target time) / mean "
        "baseline target time × 100%`."
    )

    lines.append(
        "- `/usr/bin/time` wall time is retained only as a secondary metric "
        "because its resolution is too coarse for the shortest baselines."
    )

    lines.append(
        "- Maximum RSS is an observed process-level metric and must not be "
        "confused with the monitor's statically bounded reserved state."
    )

    path.write_text(
        "\n".join(lines)
        + "\n",
        encoding="utf-8",
    )


# ============================================================================
# Main
# ============================================================================

def main() -> int:

    args = parse_args()

    results_dir = (
        Path(
            args.results_dir
        ).resolve()
    )

    correctness_rows = read_csv(
        results_dir
        / "correctness.csv"
    )

    performance_rows = read_csv(
        results_dir
        / "performance.csv"
    )

    stress_rows = read_csv(
        results_dir
        / "stress.csv"
    )

    correctness_summary = (
        summarize_correctness(
            correctness_rows
        )
    )

    performance_summary = (
        summarize_performance(
            performance_rows
        )
    )

    stress_summary = (
        summarize_stress(
            stress_rows
        )
    )

    # ------------------------------------------------------------------------
    # Correctness summary
    # ------------------------------------------------------------------------

    write_csv(
        results_dir
        / "correctness_summary.csv",

        correctness_summary,

        [
            "target",
            "runs",
            "behavioral_successes",
            "behavioral_success_rate",
            "authoritative_passes",
            "authoritative_pass_rate",
            "complete_traces",
            "complete_trace_rate",
            "zero_false_violation_runs",
            "mean_deadlock_latency_ms",
            "median_deadlock_latency_ms",
            "p95_deadlock_latency_ms",
            "max_deadlock_latency_ms",
        ],
    )

    # ------------------------------------------------------------------------
    # Performance summary
    # ------------------------------------------------------------------------

    write_csv(
        results_dir
        / "performance_summary.csv",

        performance_summary,

        [
            "profile",
            "threads",
            "total_ops",
            "baseline_runs",
            "monitor_runs",
            "baseline_target_mean_s",
            "baseline_target_sd_s",
            "monitor_target_mean_s",
            "monitor_target_sd_s",
            "added_target_time_ms",
            "runtime_overhead_pct",
            "added_ns_per_critical_section",
            "effective_added_ns_per_generated_event",
            "baseline_os_wall_mean_s",
            "monitor_os_wall_mean_s",
            "os_wall_overhead_pct",
            "baseline_mean_user_s",
            "monitor_mean_user_s",
            "baseline_mean_sys_s",
            "monitor_mean_sys_s",
            "baseline_mean_rss_kb",
            "monitor_mean_rss_kb",
            "rss_delta_kb",
            "mean_generated_events",
            "mean_monitor_loss_ppm",
            "complete_monitor_runs",
            "complete_monitor_rate",
            "zero_false_violation_runs",
        ],
    )

    # ------------------------------------------------------------------------
    # Stress summary
    # ------------------------------------------------------------------------

    write_csv(
        results_dir
        / "stress_summary.csv",

        stress_summary,

        [
            "threads",
            "runs",
            "mean_target_elapsed_s",
            "mean_os_wall_s",
            "mean_rss_kb",
            "mean_dropped_events",
            "mean_loss_ppm",
            "median_loss_ppm",
            "p95_loss_ppm",
            "mean_stream_gaps",
            "mean_tainted_streams",
            "complete_runs",
            "complete_rate",
            "false_violation_runs",
        ],
    )

    # ------------------------------------------------------------------------
    # Markdown report
    # ------------------------------------------------------------------------

    write_markdown_report(
        results_dir
        / "report_summary.md",

        correctness_summary,
        performance_summary,
        stress_summary,
    )

    # ------------------------------------------------------------------------
    # Figures
    # ------------------------------------------------------------------------

    plots_created = (
        create_plots(
            results_dir,
            correctness_rows,
            performance_summary,
            stress_summary,
        )
    )

    # ------------------------------------------------------------------------
    # Console summary
    # ------------------------------------------------------------------------

    total_behavioral_runs = sum(
        int(
            row["runs"]
        )
        for row
        in correctness_summary
    )

    total_behavioral_successes = sum(
        int(
            row[
                "behavioral_successes"
            ]
        )
        for row
        in correctness_summary
    )

    total_authoritative_passes = sum(
        int(
            row[
                "authoritative_passes"
            ]
        )
        for row
        in correctness_summary
    )

    total_monitor_runs = sum(
        int(
            row[
                "monitor_runs"
            ]
        )
        for row
        in performance_summary
    )

    total_zero_false_violation_runs = sum(
        int(
            row[
                "zero_false_violation_runs"
            ]
        )
        for row
        in performance_summary
    )

    stress_false_violations = sum(
        int(
            row[
                "false_violation_runs"
            ]
        )
        for row
        in stress_summary
    )

    print(
        "[L5] Analysis complete"
    )

    print(
        "[L5] Correctness behavioral outcomes: "
        f"{total_behavioral_successes}/"
        f"{total_behavioral_runs}"
    )

    print(
        "[L5] Correctness authoritative passes: "
        f"{total_authoritative_passes}/"
        f"{total_behavioral_runs}"
    )

    print(
        "[L5] Performance runs with zero false violations: "
        f"{total_zero_false_violation_runs}/"
        f"{total_monitor_runs}"
    )

    print(
        "[L5] Stress false-violation runs: "
        f"{stress_false_violations}"
    )

    print(
        "[L5] Summary: "
        f"{results_dir / 'report_summary.md'}"
    )

    print(
        "[L5] Plots: "
        +
        (
            str(
                results_dir
                / "plots"
            )
            if plots_created
            else "not generated"
        )
    )

    return 0


if __name__ == "__main__":

    raise SystemExit(
        main()
    )
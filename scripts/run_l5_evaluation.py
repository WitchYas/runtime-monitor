#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


VIOLATION_TYPES = [
    "DEADLOCK_CYCLE",
    "NON_OWNER_UNLOCK",
    "SELF_DEADLOCK_CANDIDATE",
    "LOCK_HELD_AT_THREAD_EXIT",
    "HOLD_TIME_VIOLATION",
]


CORRECTNESS_FIELDS = [
    "target",
    "run",
    "expected_violation",
    "exit_code",
    "wall_ms",
    "pass",
    "deadlock_latency_ms",
    "generated",
    "consumed",
    "dropped",
    "loss_ppm",
    "trace_integrity",
    "verifier_state",
    "interpretation",
    "stream_gaps",
    "tainted_streams",
    "state_overflows",
    "total_violations",
    *[f"v_{name}" for name in VIOLATION_TYPES],
]


PERFORMANCE_FIELDS = [
    "profile",
    "threads",
    "iterations_per_thread",
    "total_ops",
    "work",
    "mode",
    "run",
    "exit_code",
    "wall_seconds",
    "max_rss_kb",
    "user_seconds",
    "sys_seconds",
    "target_elapsed_ns",
    "bench_status",
    "generated",
    "consumed",
    "dropped",
    "loss_ppm",
    "trace_integrity",
    "stream_gaps",
    "tainted_streams",
    "total_violations",
]


STRESS_FIELDS = [
    "threads",
    "iterations_per_thread",
    "total_ops",
    "work",
    "run",
    "exit_code",
    "wall_seconds",
    "max_rss_kb",
    "user_seconds",
    "sys_seconds",
    "target_elapsed_ns",
    "bench_status",
    "generated",
    "consumed",
    "dropped",
    "loss_ppm",
    "trace_integrity",
    "stream_gaps",
    "tainted_streams",
    "total_violations",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the complete L5 evaluation suite for runtime-monitor."
    )

    parser.add_argument(
        "--build-dir",
        default="build-release",
        help="CMake build directory containing bin/ and lib/.",
    )

    parser.add_argument(
        "--output",
        default="results/l5/latest",
        help="Directory where L5 results will be written.",
    )

    parser.add_argument(
        "--correctness-reps",
        type=int,
        default=30,
        help="Repetitions for each correctness target.",
    )

    parser.add_argument(
        "--performance-reps",
        type=int,
        default=10,
        help="Measured repetitions per performance configuration and mode.",
    )

    parser.add_argument(
        "--warmups",
        type=int,
        default=2,
        help="Unrecorded warm-up repetitions per performance configuration.",
    )

    parser.add_argument(
        "--stress-reps",
        type=int,
        default=5,
        help="Repetitions per stress/scalability configuration.",
    )

    parser.add_argument(
        "--deadlock-timeout",
        type=float,
        default=1.0,
        help="Seconds before the circular-deadlock target is killed.",
    )

    parser.add_argument(
        "--quick",
        action="store_true",
        help="Use a short smoke-evaluation configuration.",
    )

    return parser.parse_args()


def clean_environment() -> Dict[str, str]:
    env = os.environ.copy()

    env.pop(
        "LD_PRELOAD",
        None,
    )

    env["LC_ALL"] = "C"

    return env


def run_text(
    cmd: List[str],
    timeout: float = 30.0,
) -> Tuple[int, str, int]:

    start_ns = time.perf_counter_ns()

    try:
        completed = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=clean_environment(),
            timeout=timeout,
            check=False,
        )

        return_code = completed.returncode
        output = completed.stdout

    except subprocess.TimeoutExpired as exc:

        return_code = 124

        partial = exc.stdout or ""

        if isinstance(
            partial,
            bytes,
        ):
            partial = partial.decode(
                errors="replace"
            )

        output = (
            partial
            + "\n[L5] Python timeout expired\n"
        )

    elapsed_ns = (
        time.perf_counter_ns()
        - start_ns
    )

    return (
        return_code,
        output,
        elapsed_ns,
    )


def monitored_command(
    lib: Path,
    command: Iterable[str],
    extra_env: Optional[
        Dict[str, str]
    ] = None,
) -> List[str]:

    result = [
        "env",
        f"LD_PRELOAD={lib}",
    ]

    if extra_env:

        for key, value in extra_env.items():

            result.append(
                f"{key}={value}"
            )

    result.extend(
        str(item)
        for item in command
    )

    return result


def parse_value(
    text: str,
    label: str,
) -> Optional[str]:

    match = re.search(
        rf"^{re.escape(label)}\s*:\s*(.+?)\s*$",
        text,
        re.MULTILINE,
    )

    return (
        match.group(1).strip()
        if match
        else None
    )


def parse_int_value(
    text: str,
    label: str,
) -> Optional[int]:

    value = parse_value(
        text,
        label,
    )

    if value is None:
        return None

    match = re.search(
        r"-?\d+",
        value,
    )

    return (
        int(match.group(0))
        if match
        else None
    )


def parse_violation_counts(
    text: str,
) -> Dict[str, int]:

    counts: Dict[
        str,
        int,
    ] = {}

    for violation in VIOLATION_TYPES:

        summary_match = re.search(
            rf"^\s{{2}}"
            rf"{re.escape(violation)}"
            rf"\s*:\s*(\d+)\s*$",

            text,
            re.MULTILINE,
        )

        if summary_match:

            counts[violation] = int(
                summary_match.group(1)
            )

        else:

            counts[violation] = len(
                re.findall(
                    rf"\[monitor\]\[VIOLATION\]\s+"
                    rf"{re.escape(violation)}\b",

                    text,
                )
            )

    return counts


def parse_monitor_metrics(
    text: str,
) -> Dict[str, object]:

    violations = (
        parse_violation_counts(
            text
        )
    )

    total_violations = (
        parse_int_value(
            text,
            "Total violations",
        )
    )

    if total_violations is None:

        total_violations = sum(
            violations.values()
        )

    return {

        "generated":
            parse_int_value(
                text,
                "Generated events",
            )
            or 0,

        "consumed":
            parse_int_value(
                text,
                "Consumed events",
            )
            or 0,

        "dropped":
            parse_int_value(
                text,
                "Dropped events",
            )
            or 0,

        "loss_ppm":
            parse_int_value(
                text,
                "Loss rate",
            )
            or 0,

        "trace_integrity":
            parse_value(
                text,
                "Trace integrity",
            )
            or "",

        "verifier_state":
            parse_value(
                text,
                "Verifier state",
            )
            or "",

        "interpretation":
            parse_value(
                text,
                "Interpretation",
            )
            or "",

        "stream_gaps":
            parse_int_value(
                text,
                "Detected stream gaps",
            )
            or 0,

        "tainted_streams":
            parse_int_value(
                text,
                "Tainted thread streams",
            )
            or 0,

        "state_overflows":
            parse_int_value(
                text,
                "State overflow events",
            )
            or 0,

        "total_violations":
            total_violations,

        **{
            f"v_{name}":
                violations[name]

            for name
            in VIOLATION_TYPES
        },
    }


def parse_bench_result(
    text: str,
) -> Dict[str, str]:

    match = re.search(
        r"^BENCH_RESULT\s+(.+)$",
        text,
        re.MULTILINE,
    )

    if not match:
        return {}

    result: Dict[
        str,
        str,
    ] = {}

    for token in (
        match.group(1).split()
    ):

        if "=" not in token:
            continue

        key, value = token.split(
            "=",
            1,
        )

        result[key] = value

    return result


def write_csv(
    path: Path,
    rows: List[
        Dict[str, object]
    ],
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

        writer.writerows(
            rows
        )


def save_log(
    log_dir: Path,
    name: str,
    run: int,
    text: str,
) -> None:

    log_dir.mkdir(
        parents=True,
        exist_ok=True,
    )

    (
        log_dir
        / f"{name}_run_{run:03d}.log"
    ).write_text(
        text,
        encoding="utf-8",
    )


def command_version(
    command: List[str],
) -> str:

    try:
        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=clean_environment(),
            timeout=5.0,
            check=False,
        )

        return (
            completed.stdout.strip()
        )

    except Exception as exc:

        return (
            f"unavailable: {exc}"
        )


def git_commit(
    root: Path,
) -> str:

    try:
        completed = subprocess.run(
            [
                "git",
                "rev-parse",
                "HEAD",
            ],
            cwd=root,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=5.0,
            check=False,
        )

        if completed.returncode == 0:

            return (
                completed.stdout.strip()
            )

    except Exception:
        pass

    return "unknown"


def collect_metadata(
    root: Path,
    build_dir: Path,
    args: argparse.Namespace,
) -> Dict[str, object]:

    gcc_version = command_version(
        [
            "gcc",
            "--version",
        ]
    )

    cmake_version = command_version(
        [
            "cmake",
            "--version",
        ]
    )

    return {

        "timestamp_local":
            time.strftime(
                "%Y-%m-%dT%H:%M:%S%z"
            ),

        "project_root":
            str(root),

        "build_dir":
            str(build_dir),

        "git_commit":
            git_commit(root),

        "python":
            sys.version,

        "platform":
            platform.platform(),

        "machine":
            platform.machine(),

        "processor":
            platform.processor(),

        "kernel":
            platform.release(),

        "gcc":
            gcc_version.splitlines()[0]
            if gcc_version
            else "",

        "cmake":
            cmake_version.splitlines()[0]
            if cmake_version
            else "",

        "nproc":
            os.cpu_count(),

        "evaluation": {

            "correctness_reps":
                args.correctness_reps,

            "performance_reps":
                args.performance_reps,

            "warmups":
                args.warmups,

            "stress_reps":
                args.stress_reps,

            "deadlock_timeout_seconds":
                args.deadlock_timeout,

            "quick":
                args.quick,
        },
    }


def run_deadlock_live(
    binary: Path,
    lib: Path,
    timeout_seconds: float,
) -> Tuple[
    int,
    str,
    Optional[float],
]:

    timeout_program = (
        shutil.which("timeout")
        or "timeout"
    )

    stdbuf_program = (
        shutil.which("stdbuf")
        or "stdbuf"
    )

    command = [

        timeout_program,
        f"{timeout_seconds}s",

        "env",
        f"LD_PRELOAD={lib}",

        stdbuf_program,
        "-oL",
        "-eL",

        str(binary),
    ]

    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        env=clean_environment(),
    )

    lines: List[str] = []

    request_count = 0

    cycle_ready_ns: Optional[
        int
    ] = None

    violation_ns: Optional[
        int
    ] = None

    assert process.stdout is not None

    for line in process.stdout:

        observed_ns = (
            time.perf_counter_ns()
        )

        lines.append(
            line
        )

        if (
            "Requesting mutex"
            in line
        ):

            request_count += 1

            if (
                request_count == 2
                and
                cycle_ready_ns is None
            ):

                cycle_ready_ns = (
                    observed_ns
                )

        if (
            "[monitor][VIOLATION] "
            "DEADLOCK_CYCLE"
        ) in line:

            if violation_ns is None:

                violation_ns = (
                    observed_ns
                )

    return_code = (
        process.wait()
    )

    latency_ms: Optional[
        float
    ] = None

    if (
        cycle_ready_ns is not None
        and
        violation_ns is not None
        and
        violation_ns >=
            cycle_ready_ns
    ):

        latency_ms = (
            violation_ns
            - cycle_ready_ns
        ) / 1_000_000.0

    return (
        return_code,
        "".join(lines),
        latency_ms,
    )


def run_correctness_suite(
    bin_dir: Path,
    lib: Path,
    output: Path,
    reps: int,
    deadlock_timeout: float,
) -> List[Dict[str, object]]:

    log_dir = (
        output
        / "logs"
        / "correctness"
    )

    tests = [

        {
            "name":
                "correct_smoke",

            "command": [

                str(
                    bin_dir
                    / "benchmark_mutex"
                ),

                "--threads",
                "2",

                "--iterations",
                "5000",

                "--work",
                "0",
            ],

            "expected":
                None,

            "extra_env":
                {},
        },

        {
            "name":
                "non_owner_unlock",

            "command": [
                str(
                    bin_dir
                    / "non_owner_unlock"
                )
            ],

            "expected":
                "NON_OWNER_UNLOCK",

            "extra_env":
                {},
        },

        {
            "name":
                "self_deadlock",

            "command": [
                str(
                    bin_dir
                    / "self_deadlock"
                )
            ],

            "expected":
                "SELF_DEADLOCK_CANDIDATE",

            "extra_env":
                {},
        },

        {
            "name":
                "held_at_exit",

            "command": [
                str(
                    bin_dir
                    / "held_at_exit"
                )
            ],

            "expected":
                "LOCK_HELD_AT_THREAD_EXIT",

            "extra_env":
                {},
        },

        {
            "name":
                "hold_time",

            "command": [
                str(
                    bin_dir
                    / "hold_time"
                )
            ],

            "expected":
                "HOLD_TIME_VIOLATION",

            "extra_env": {
                "MONITOR_MAX_HOLD_NS":
                    "100000000",
            },
        },
    ]

    rows: List[
        Dict[str, object]
    ] = []

    for test in tests:

        print(
            f"[L5] correctness: "
            f"{test['name']} x {reps}"
        )

        for run in range(
            1,
            reps + 1,
        ):

            command = monitored_command(
                lib,
                test["command"],
                test["extra_env"],
            )

            (
                return_code,
                text,
                wall_ns,
            ) = run_text(
                command,
                timeout=30.0,
            )

            save_log(
                log_dir,
                str(test["name"]),
                run,
                text,
            )

            metrics = (
                parse_monitor_metrics(
                    text
                )
            )

            expected = (
                test["expected"]
            )

            if expected is None:

                correct_violations = (
                    metrics[
                        "total_violations"
                    ] == 0
                )

            else:

                correct_violations = (

                    metrics[
                        f"v_{expected}"
                    ] == 1

                    and

                    metrics[
                        "total_violations"
                    ] == 1
                )

            passed = (

                return_code == 0

                and

                correct_violations

                and

                metrics[
                    "trace_integrity"
                ] == "COMPLETE"

                and

                metrics[
                    "verifier_state"
                ] == "COMPLETE"

                and

                metrics[
                    "interpretation"
                ] == "AUTHORITATIVE"
            )

            rows.append({

                "target":
                    test["name"],

                "run":
                    run,

                "expected_violation":
                    expected
                    or "NONE",

                "exit_code":
                    return_code,

                "wall_ms":
                    wall_ns
                    / 1_000_000.0,

                "pass":
                    int(passed),

                "deadlock_latency_ms":
                    "",

                **metrics,
            })

    print(
        "[L5] correctness: "
        f"circular_deadlock x {reps}"
    )

    for run in range(
        1,
        reps + 1,
    ):

        (
            return_code,
            text,
            latency_ms,
        ) = run_deadlock_live(

            bin_dir
            / "circular_deadlock",

            lib,

            deadlock_timeout,
        )

        save_log(
            log_dir,
            "circular_deadlock",
            run,
            text,
        )

        violations = (
            parse_violation_counts(
                text
            )
        )

        passed = (

            return_code == 124

            and

            violations[
                "DEADLOCK_CYCLE"
            ] == 1

            and

            "cycle_length=2"
            in text
        )

        rows.append({

            "target":
                "circular_deadlock",

            "run":
                run,

            "expected_violation":
                "DEADLOCK_CYCLE",

            "exit_code":
                return_code,

            "wall_ms":
                deadlock_timeout
                * 1000.0,

            "pass":
                int(passed),

            "deadlock_latency_ms":
                (
                    ""
                    if latency_ms is None
                    else latency_ms
                ),

            "generated":
                "",

            "consumed":
                "",

            "dropped":
                "",

            "loss_ppm":
                "",

            "trace_integrity":
                "TIMEOUT_EXPECTED",

            "verifier_state":
                "",

            "interpretation":
                "LIVE_DETECTION",

            "stream_gaps":
                "",

            "tainted_streams":
                "",

            "state_overflows":
                "",

            "total_violations":
                violations[
                    "DEADLOCK_CYCLE"
                ],

            **{
                f"v_{name}":
                    violations[name]

                for name
                in VIOLATION_TYPES
            },
        })

    return rows


def parse_time_file(
    path: Path,
) -> Tuple[
    float,
    int,
    float,
    float,
]:

    text = path.read_text(
        encoding="utf-8"
    ).strip()

    parts = text.split(",")

    if len(parts) != 4:

        raise RuntimeError(
            "Unexpected /usr/bin/time "
            f"output: {text!r}"
        )

    (
        elapsed,
        max_rss,
        user_s,
        sys_s,
    ) = parts

    return (
        float(elapsed),
        int(max_rss),
        float(user_s),
        float(sys_s),
    )


def run_timed_benchmark(
    benchmark: Path,
    lib: Path,
    threads: int,
    iterations: int,
    work: int,
    monitored: bool,
    timeout: float = 120.0,
) -> Tuple[
    int,
    str,
    Dict[str, object],
]:

    time_program = Path(
        "/usr/bin/time"
    )

    if not time_program.exists():

        raise RuntimeError(
            "/usr/bin/time is required "
            "for L5 RSS/CPU measurements. "
            "Install the 'time' package first."
        )

    with tempfile.TemporaryDirectory(
        prefix="runtime-monitor-l5-"
    ) as tmp:

        time_file = (
            Path(tmp)
            / "time.csv"
        )

        inner_command = [

            str(benchmark),

            "--threads",
            str(threads),

            "--iterations",
            str(iterations),

            "--work",
            str(work),
        ]

        if monitored:

            inner_command = (
                monitored_command(
                    lib,
                    inner_command,
                )
            )

        command = [

            str(time_program),

            "-f",
            "%e,%M,%U,%S",

            "-o",
            str(time_file),

            *inner_command,
        ]

        (
            return_code,
            text,
            _,
        ) = run_text(
            command,
            timeout=timeout,
        )

        if (
            time_file.exists()
            and
            time_file.stat().st_size > 0
        ):

            (
                wall_seconds,
                max_rss_kb,
                user_seconds,
                sys_seconds,
            ) = parse_time_file(
                time_file
            )

        else:

            wall_seconds = 0.0
            max_rss_kb = 0
            user_seconds = 0.0
            sys_seconds = 0.0

        bench = parse_bench_result(
            text
        )

        monitor_metrics = (
            parse_monitor_metrics(
                text
            )
            if monitored
            else {}
        )

        result: Dict[
            str,
            object,
        ] = {

            "wall_seconds":
                wall_seconds,

            "max_rss_kb":
                max_rss_kb,

            "user_seconds":
                user_seconds,

            "sys_seconds":
                sys_seconds,

            "target_elapsed_ns":
                int(
                    bench.get(
                        "elapsed_ns",
                        "0",
                    )
                ),

            "bench_status":
                bench.get(
                    "status",
                    "MISSING",
                ),

            "generated":
                monitor_metrics.get(
                    "generated",
                    "",
                ),

            "consumed":
                monitor_metrics.get(
                    "consumed",
                    "",
                ),

            "dropped":
                monitor_metrics.get(
                    "dropped",
                    "",
                ),

            "loss_ppm":
                monitor_metrics.get(
                    "loss_ppm",
                    "",
                ),

            "trace_integrity":
                monitor_metrics.get(
                    "trace_integrity",
                    "",
                ),

            "stream_gaps":
                monitor_metrics.get(
                    "stream_gaps",
                    "",
                ),

            "tainted_streams":
                monitor_metrics.get(
                    "tainted_streams",
                    "",
                ),

            "total_violations":
                monitor_metrics.get(
                    "total_violations",
                    "",
                ),
        }

        return (
            return_code,
            text,
            result,
        )


def run_performance_suite(
    bin_dir: Path,
    lib: Path,
    output: Path,
    reps: int,
    warmups: int,
) -> List[Dict[str, object]]:

    benchmark = (
        bin_dir
        / "benchmark_mutex"
    )

    log_dir = (
        output
        / "logs"
        / "performance"
    )

    profiles = [

        (
            "sync_heavy",
            0,
        ),

        (
            "mixed",
            100,
        ),
    ]

    thread_counts = [
        1,
        2,
        4,
        8,
        16,
    ]

    # Strong-scaling comparison:
    # same total number of critical sections.
    total_ops = 400_000

    rows: List[
        Dict[str, object]
    ] = []

    for profile, work in profiles:

        for threads in thread_counts:

            iterations = max(
                1,
                total_ops
                // threads,
            )

            print(
                "[L5] performance: "
                f"profile={profile} "
                f"threads={threads} "
                f"iterations={iterations}"
            )

            # Warm-up runs are not stored.
            for _ in range(
                warmups
            ):

                for monitored in (
                    False,
                    True,
                ):

                    run_timed_benchmark(
                        benchmark,
                        lib,
                        threads,
                        iterations,
                        work,
                        monitored,
                    )

            for run in range(
                1,
                reps + 1,
            ):

                # Alternate order to reduce
                # systematic temporal bias.
                order = (
                    (False, True)
                    if run % 2
                    else (True, False)
                )

                for monitored in order:

                    (
                        return_code,
                        text,
                        metrics,
                    ) = run_timed_benchmark(
                        benchmark,
                        lib,
                        threads,
                        iterations,
                        work,
                        monitored,
                    )

                    mode = (
                        "monitor"
                        if monitored
                        else "baseline"
                    )

                    save_log(
                        log_dir
                        / profile
                        / f"threads_{threads}",
                        mode,
                        run,
                        text,
                    )

                    rows.append({

                        "profile":
                            profile,

                        "threads":
                            threads,

                        "iterations_per_thread":
                            iterations,

                        "total_ops":
                            iterations
                            * threads,

                        "work":
                            work,

                        "mode":
                            mode,

                        "run":
                            run,

                        "exit_code":
                            return_code,

                        **metrics,
                    })

    return rows


def run_stress_suite(
    bin_dir: Path,
    lib: Path,
    output: Path,
    reps: int,
) -> List[Dict[str, object]]:

    benchmark = (
        bin_dir
        / "benchmark_mutex"
    )

    log_dir = (
        output
        / "logs"
        / "stress"
    )

    thread_counts = [
        1,
        2,
        4,
        8,
        16,
    ]

    iterations = 250_000

    work = 0

    rows: List[
        Dict[str, object]
    ] = []

    for threads in thread_counts:

        print(
            "[L5] stress: "
            f"threads={threads} "
            f"iterations/thread={iterations} "
            f"x {reps}"
        )

        for run in range(
            1,
            reps + 1,
        ):

            (
                return_code,
                text,
                metrics,
            ) = run_timed_benchmark(
                benchmark,
                lib,
                threads,
                iterations,
                work,
                monitored=True,
                timeout=180.0,
            )

            save_log(
                log_dir
                / f"threads_{threads}",
                "monitor",
                run,
                text,
            )

            rows.append({

                "threads":
                    threads,

                "iterations_per_thread":
                    iterations,

                "total_ops":
                    iterations
                    * threads,

                "work":
                    work,

                "run":
                    run,

                "exit_code":
                    return_code,

                **metrics,
            })

    return rows


def verify_required_paths(
    bin_dir: Path,
    lib: Path,
) -> None:

    required_binaries = [

        "benchmark_mutex",
        "circular_deadlock",
        "non_owner_unlock",
        "self_deadlock",
        "held_at_exit",
        "hold_time",
    ]

    missing: List[str] = []

    if not lib.exists():

        missing.append(
            str(lib)
        )

    for binary in required_binaries:

        path = (
            bin_dir
            / binary
        )

        if not path.exists():

            missing.append(
                str(path)
            )

    if missing:

        formatted = "\n  ".join(
            missing
        )

        raise FileNotFoundError(
            "Required L5 build artifacts "
            "are missing:\n"
            f"  {formatted}\n"
            "Build the Release configuration "
            "before running L5."
        )


def main() -> int:

    args = parse_args()

    if args.quick:

        args.correctness_reps = 3
        args.performance_reps = 3
        args.warmups = 1
        args.stress_reps = 2

    if (
        args.correctness_reps <= 0
        or
        args.performance_reps <= 0
        or
        args.warmups < 0
        or
        args.stress_reps <= 0
        or
        args.deadlock_timeout <= 0
    ):

        print(
            "Invalid L5 repetition/"
            "timeout configuration.",
            file=sys.stderr,
        )

        return 2

    root = (
        Path.cwd().resolve()
    )

    build_dir = (
        root
        / args.build_dir
    ).resolve()

    output = (
        root
        / args.output
    ).resolve()

    bin_dir = (
        build_dir
        / "bin"
    )

    lib = (
        build_dir
        / "lib"
        / "libmonitor.so"
    )

    verify_required_paths(
        bin_dir,
        lib,
    )

    output.mkdir(
        parents=True,
        exist_ok=True,
    )

    metadata = collect_metadata(
        root,
        build_dir,
        args,
    )

    (
        output
        / "metadata.json"
    ).write_text(
        json.dumps(
            metadata,
            indent=2,
        ),
        encoding="utf-8",
    )

    correctness_rows = (
        run_correctness_suite(
            bin_dir,
            lib,
            output,
            args.correctness_reps,
            args.deadlock_timeout,
        )
    )

    write_csv(
        output
        / "correctness.csv",
        correctness_rows,
        CORRECTNESS_FIELDS,
    )

    performance_rows = (
        run_performance_suite(
            bin_dir,
            lib,
            output,
            args.performance_reps,
            args.warmups,
        )
    )

    write_csv(
        output
        / "performance.csv",
        performance_rows,
        PERFORMANCE_FIELDS,
    )

    stress_rows = (
        run_stress_suite(
            bin_dir,
            lib,
            output,
            args.stress_reps,
        )
    )

    write_csv(
        output
        / "stress.csv",
        stress_rows,
        STRESS_FIELDS,
    )

    print()

    print(
        "[L5] Raw evaluation complete."
    )

    print(
        f"[L5] Results: {output}"
    )

    print(
        "[L5] Next: "
        "python3 "
        "analysis/analyze_l5.py "
        + str(output)
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(
        main()
    )
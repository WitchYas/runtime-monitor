# L5 Evaluation Summary

Primary runtime overhead is computed from the benchmark's high-resolution `target_elapsed_ns` measurement obtained with `CLOCK_MONOTONIC_RAW`. `/usr/bin/time` wall time, CPU time and maximum RSS are retained as secondary operating-system-level measurements.

The circular-deadlock latency is an observed host-side proxy measured from the second target request message to the live `DEADLOCK_CYCLE` report.

## Detection correctness

Behavioral success is separated from trace authority: an execution can produce the expected violation behavior while still being non-authoritative if transport loss makes the trace incomplete.

| Target | Runs | Behavioral successes | Behavioral success | Authoritative passes | Authoritative rate | Complete traces | Mean deadlock latency (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|
| circular_deadlock | 30 | 30 | 100.00% | 30 | 100.00% | 0/30 | 0.027 |
| correct_smoke | 30 | 30 | 100.00% | 22 | 73.33% | 22/30 | NA |
| held_at_exit | 30 | 30 | 100.00% | 30 | 100.00% | 30/30 | NA |
| hold_time | 30 | 30 | 100.00% | 30 | 100.00% | 30/30 | NA |
| non_owner_unlock | 30 | 30 | 100.00% | 30 | 100.00% | 30/30 | NA |
| self_deadlock | 30 | 30 | 100.00% | 30 | 100.00% | 30/30 | NA |

## Runtime and memory overhead

| Profile | Threads | Baseline target mean (ms) | Monitor target mean (ms) | Added time (ms) | Corrected overhead | Added ns / critical section | Effective added ns / generated event | RSS delta (KiB) | Mean loss (ppm) | Complete traces |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| mixed | 1 | 130.775 | 319.071 | 188.296 | 143.98% | 470.7 | 156.9 | 0.0 | 8615.4 | 1/10 |
| mixed | 2 | 226.765 | 431.362 | 204.597 | 90.22% | 511.5 | 170.5 | 38.4 | 4816.0 | 2/10 |
| mixed | 4 | 246.513 | 537.948 | 291.435 | 118.22% | 728.6 | 242.9 | 38.4 | 645.3 | 4/10 |
| mixed | 8 | 280.421 | 546.728 | 266.307 | 94.97% | 665.8 | 221.9 | 64.0 | 805.1 | 8/10 |
| mixed | 16 | 321.542 | 553.787 | 232.246 | 72.23% | 580.6 | 193.5 | 256.0 | 0.0 | 10/10 |
| sync_heavy | 1 | 10.712 | 202.760 | 192.048 | 1792.88% | 480.1 | 160.0 | 0.0 | 5801.8 | 0/10 |
| sync_heavy | 2 | 31.618 | 220.735 | 189.118 | 598.14% | 472.8 | 157.6 | 25.6 | 6781.9 | 1/10 |
| sync_heavy | 4 | 27.827 | 236.782 | 208.955 | 750.90% | 522.4 | 174.1 | 38.4 | 3629.0 | 3/10 |
| sync_heavy | 8 | 35.538 | 269.713 | 234.175 | 658.94% | 585.4 | 195.1 | 102.4 | 0.0 | 10/10 |
| sync_heavy | 16 | 38.432 | 285.661 | 247.229 | 643.29% | 618.1 | 206.0 | 384.0 | 3808.3 | 9/10 |

The per-event value above is an effective elapsed-time normalization, not the latency of one event; producer and consumer execution overlap asynchronously.

## Stress / scalability

| Threads | Runs | Mean target time (s) | Mean loss (ppm) | P95 loss (ppm) | Mean stream gaps | Mean tainted streams | Complete traces | False-violation runs |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 5 | 0.1160 | 14285.0 | 26555.0 | 9.60 | 0.80 | 1/5 | 0 |
| 2 | 5 | 0.2593 | 6605.6 | 11562.6 | 12.00 | 2.00 | 0/5 | 0 |
| 4 | 5 | 0.5883 | 339.0 | 1129.2 | 2.60 | 1.80 | 2/5 | 0 |
| 8 | 5 | 1.3029 | 3981.8 | 15721.6 | 27.60 | 2.20 | 2/5 | 0 |
| 16 | 5 | 2.7831 | 102.4 | 373.0 | 1.60 | 1.40 | 3/5 | 0 |

## Interpretation rules

- Behavioral correctness and trace authority are reported separately.
- Normally terminating runs are authoritative only when transport and verifier state are complete.
- Stress runs with transport loss are expected to become `DEGRADED` rather than generate false state-dependent violations.
- Primary runtime overhead uses high-resolution in-target timing: `(mean monitor target time - mean baseline target time) / mean baseline target time × 100%`.
- `/usr/bin/time` wall time is retained only as a secondary metric because its resolution is too coarse for the shortest baselines.
- Maximum RSS is an observed process-level metric and must not be confused with the monitor's statically bounded reserved state.

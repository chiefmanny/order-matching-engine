# Benchmarks

These are the raw results behind the numbers in the [main README](../../README.md).

## Versions

| Name | Commit | What it is |
|---|---|---|
| `coarse` | `c8c3aac` | 2024 engine: one lock per side of the book |
| `fine` | `ae5e9de` | 2024 engine: adds a mutex per price level |
| `current` | `10ab4c8` | Today's engine, including the 2026 fixes |

All three use the same client test programs. The 2024 sources are unmodified: `build_version.sh` only edits their build files and force-includes standard headers that newer compilers no longer pull in transitively.

## Environment

- **Machine:** Apple M4, 10 cores, on battery
- **Container:** Docker 28.3.0, Linux VM with 10 CPUs and about 8 GB of memory
- **Toolchain:** Debian bookworm, g++ 12.2, gRPC 1.51.1, protobuf 3.21.12, CMake `Release`
- **Date:** November 6, 2026, 10:17-10:42 local time. The power log shows no sleep or wake events in that window.

The demo web app's three containers were also idling on the same Docker VM, using about 2% of one core in total.

Client and server run in the same container and talk over localhost, so the absolute numbers only apply to this setup. I'd only use them to compare versions.

## What each test measures

**`latency_test_1`:** 10,000 `AddLimitOrder` round trips from one client, with 3 runs per version.
- Buy prices center on 50 and sell prices on 100, so the orders essentially never cross.
- It measures order acknowledgement, not time to match.

**`throughput_test_1`:** two clients each send N orders that all cross, with 10 runs per size and version.
- The duration includes a fixed 1-second sleep before the second client starts, which is why small runs all take about 1 second.
- It stops when the last order is acknowledged and does not wait for matching to finish. The "orders matched" line it prints really counts orders submitted.
- Two clients plus one matcher thread is light contention, so this isn't a strong test of what fine-grained locking could do with many writers.

A failed RPC returns immediately, so a crashed server makes a run look fast. That's why every row records client errors and whether the server survived, and durations only use clean runs.

## Results

Median duration in seconds. Parentheses show clean runs out of 10 where some aborted.

| Orders per client | Coarse | Fine-grained | Current |
|---:|---:|---:|---:|
| 1,600 | 1.18 | 1.17 | 1.17 |
| 12,800 | 2.40 | 2.40 (9) | 2.39 |
| 51,200 | 7.99 | 8.01 (4) | 8.07 |
| 102,400 | 15.44 | 15.45 (1) | 15.57 |

Acknowledgement latency in microseconds, pooled over 30,000 orders per version:

| Version | p50 | p90 | p99 | p99.9 | Max |
|---|---:|---:|---:|---:|---:|
| Coarse | 96 | 111 | 160 | 501 | 1,642 |
| Fine-grained | 97 | 106 | 124 | 159 | 382 |
| Current | 97 | 107 | 125 | 166 | 232 |

I wouldn't read much into the tail differences. Coarse's p99 ranged from 125 to 234 µs across its own three runs, a wider spread than the gap between versions.

## Crash reproduction

`results/asan/summary.csv` records 3 AddressSanitizer runs per version at 51,200 orders per client. All 3 fine-grained runs failed with a heap use-after-free, and coarse and current had no reports. `results/asan/fine_report.txt` is one of those reports, cut down to its three stacks.

## Files

- `results/throughput.csv`, `results/latency_runs.csv`: per-run results and server survival
- `results/latency_<version>_run<n>.txt`: raw latencies in microseconds
- `results/asan/`: the sanitizer summary and report
- `run.sh`, `Dockerfile`, `build_version.sh`, `run_benchmarks.sh`, `asan_repro.sh`: how to reproduce them
- `plot.py`: draws the charts (needs matplotlib)

## Reproducing

From the repository root, with Docker running:

```bash
docs/benchmarks/run.sh                 # about 25 minutes
docs/benchmarks/run.sh asan_repro.sh   # about 4 minutes
python3 docs/benchmarks/plot.py
```

Delete `results/throughput.csv` and `results/latency_runs.csv` first, because the sweep appends to them. On a Mac, prefix the runs with `caffeinate -is` and keep the lid open. I threw out two earlier sweeps because the Mac went to sleep during them. Closing the lid overrides `caffeinate`.

# Nginx worker_processes experiment — October 15, 2026

**In the tested configuration, a single Nginx worker used 40.93% less
container memory — 1.986 MiB saved. A throughput gain was not confirmed:
the median difference of +0.47% is smaller than the observed run-to-run
spread.**

The change is stock Nginx `worker_processes auto` → `1` under a 1-CPU quota.
Server Opt runs the comparison and collects the results; the audit or load
generator by itself does not optimize anything.

## Conditions

- Apple Silicon host, Linux VM via Lima 2.2.0 / Apple Virtualization.
- VM: Ubuntu 26.04 LTS, aarch64, 4 vCPU, 4 GiB RAM.
- Docker Engine 29.9.1, cgroup v2.
- Both containers: Ubuntu 24.04, Nginx `1.24.0-2ubuntu7.18`,
  1-CPU quota, 256 MiB RAM limit.
- Baseline: four workers + master; tuned: one worker + master.
- Same small JSON response, HTTP/1.1 keep-alive, no TLS, no database.
- 32 concurrent requests, 15 s per measurement, six repeats per profile,
  warmup before each measurement.
- Order: baseline/tuned then tuned/baseline, alternating across rounds.
- C++17 generator built with `-O2`, GCC 15.2, libcurl 8.18.0,
  running inside the VM (no load traffic through macOS).
- Every response checked by HTTP status and exact body.

Shared image ID:
`sha256:2f1c6a9a6a70bd14fadf5feb460670673d69c283e2f012be1fac7912784b9fef`.

## Results

Medians of six runs per profile:

| Metric | auto / 4 workers | 1 worker | Change |
|---|---:|---:|---:|
| Container memory under load | 4.852 MiB | 2.866 MiB | −40.93% |
| Successful requests/s | 75,803 | 76,159 | +0.47% |
| p95 of a successful request | 0.4415 ms | 0.4065 ms | −7.93% |
| Errors across all runs | 0 | 0 | none |

The absolute p95 drop is 0.035 ms. This is the median of per-run p95 values
computed on a sample of successful requests, not the p95 of all requests.

Run-to-run spread:

| Metric | auto / 4 workers | 1 worker |
|---|---:|---:|
| requests/s min–max | 70,585–76,503 | 71,614–76,662 |
| memory min–max | 4.777–4.936 MiB | 2.762–2.918 MiB |

In every round, tuned memory was below baseline. The throughput direction
flipped: in round five, tuned was slower. No formal significance test was run.

## What can be claimed

Limiting worker processes reduced the container's RAM by ~2 MiB, reproduced
in all six measurement pairs. Memory came from `docker stats`, which excludes
part of the page cache and rounds values. The percentage refers to a small
Nginx container — not the whole VM, OS, or an app with a database.

The load generator used ~94% of one CPU, so it may itself cap the measured
throughput. The server's maximum capacity and how many more users it can
serve were not determined by this test — that needs an independent,
more powerful generator and a real application workload.

On a multi-CPU server without a quota, one worker may hurt throughput.
If `auto` already picks one worker, the configuration does not change at all.
Percentages do not generalize to all servers. Energy and monetary savings
were not measured.

## Artifacts and reproduction

- `results/comparison-c32.json` — full report, all 12 measurements, raw metrics.
- `results/comparison-summary.json` — aggregates and ranges.
- `results/comparison-c32.log` — run order.
- `results/linux-build-test.log` — Linux build + 17 passing tests.

An earlier run failed while counting processes via `docker top` (missing PID
column); fixed to `ps -eo pid,comm` and the experiment was rerun in full.
Only the clean run is used above.

The measurement command inside the prepared VM:

```sh
DOCKER_BUILDKIT=0 ~/server_opt/build/server-opt experiment \
  --project-root ~/server_opt \
  --duration 15 --repeats 6 --concurrency 32 \
  --output ~/server_opt/results/comparison-c32.json
```

`DOCKER_BUILDKIT=0` was used to reuse a previously built image via the legacy
builder. On another host, plain `./build/server-opt experiment` with the same
load parameters works.

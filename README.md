# server-opt

A native C++17 toolkit for finding and *measuring* wasted resources on
Linux servers: HTTP load generation, container experiments, and a
`pg_stat_statements` analyzer that ranks real PostgreSQL queries by
cumulative time and flags anti-patterns.

Every claim in this repo is backed by a reproducible measurement —
including negative results.

## What it does

| Command | Purpose |
|---|---|
| `server-opt audit` | Read-only snapshot of Linux resources (RAM, cgroup limits, top processes) |
| `server-opt bench URL` | HTTP load via libcurl multi; throughput, latency percentiles, error classes |
| `server-opt experiment` | Controlled Nginx A/B: `worker_processes auto` vs `1` under a 1-CPU container |
| `server-opt postgres` | Disposable PostgreSQL benchmark: index vs no-index with measured costs |
| `server-opt analyze` | Reads `pg_stat_statements` of a running PG container; flags TRUNCATE churn, schema introspection, implicit joins, `LIKE` patterns |

## Measured case studies

| Case | Setup | Result |
|---|---|---|
| [Nginx worker count](experiments/nginx/RESULTS.md) | 1 CPU container, 32 connections, 6 runs | −40.9% container RAM (4.85 → 2.87 MiB); throughput change within noise |
| [Index benchmark](experiments/postgres/RESULTS.md) | 1M-row catalog, range+sort query | ~×299 read throughput; −3.4% insert throughput; +58% storage |
| [Wiki.js](experiments/wikijs/FINDINGS.md) | Real app, 150 pages via API | Finding is architectural: `TRUNCATE pageTree` + full rebuild per page create — not an index problem |
| [Zabbix at 100M rows](experiments/zabbix/FINDINGS.md) | `history` table, 30-day retention cleanup | `DROP PARTITION` vs housekeeper `DELETE`: ~74 s heavy work → ~3 s; reads 3× faster (57 vs 182 ms p50); disk actually freed |

**Honest scope:** these results are specific to the measured workload, schema
and hardware. They do not generalize to every server — that's why the tool
measures instead of guessing.

## Build

Requires a C++17 compiler, Make, and libcurl ≥ 7.85 with headers.

```sh
make          # → build/server-opt
make test     # 18 native tests
make sanitize # ASan/UBSan build, then: make clean && make
```

Ubuntu:

```sh
sudo apt-get install build-essential libcurl4-openssl-dev
```

## Usage

```sh
# Snapshot of the machine
./build/server-opt audit --output results/audit.json

# Load test (your own service only)
./build/server-opt bench http://127.0.0.1:8080/health --duration 30 --concurrency 8

# Nginx experiment — needs Docker with Linux containers
./build/server-opt experiment --duration 15 --repeats 3

# PostgreSQL index benchmark — disposable container, ~1M rows
./build/server-opt postgres --rows 1000000

# Analyze a live PostgreSQL container (read-only)
./build/server-opt analyze --container zbx-db --database zabbix --user zabbix --top 15
```

`analyze` requires `pg_stat_statements` enabled in the target database:

```sql
ALTER SYSTEM SET shared_preload_libraries = 'pg_stat_statements';  -- then restart
CREATE EXTENSION pg_stat_statements;
```

## Methodology rules we follow

- Optimizations are verified on disposable copies, never claimed blind.
- Write-side costs (insert rate, storage) are measured alongside wins.
- `measurement_valid` in reports excludes runs with load or collection errors.
- Memory savings ≠ energy savings: kWh requires separate measurement.
- A bigger relative win on a tiny number is still a tiny number — absolute
  values are always reported.

## Layout

```
src/                 C++17 sources (benchmark, audit, experiments, analyzer)
tests/               native test suite with a local HTTP rig
experiments/
  nginx/             worker_processes A/B: Dockerfile, configs, results
  postgres/          disposable index benchmark: SQL rig, results
  wikijs/            Wiki.js deployment script + analysis findings
  zabbix/            100M-row history partitioning experiment + findings
```

## License

MIT. Dependencies (libcurl, nginx, PostgreSQL, and the measured apps) keep
their own licenses.

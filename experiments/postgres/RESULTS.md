# Searching a million-product catalog — October 15, 2026

**In this SQL scenario, adding an index raised throughput from 221 to
66,112 queries/s: roughly ×299 (+29,763%).** Mean latency dropped from
36.12 ms to 0.1165 ms (−99.68%). Inserts became 3.41% slower and the table
with indexes grew by 58.02%.

The change is a standard PostgreSQL index — nothing novel. Server Opt
builds the isolated rig, verifies results, and measures the effect.
This is not a claim of similar speedup for an arbitrary database,
an OS, or an HTTP API.

## What was compared

One million deterministic products, 100 categories, prices 0–999.99,
in-stock and out-of-stock items in every category. Query:

```sql
SELECT id, name, price
FROM products
WHERE category_id = :category AND in_stock AND price >= :min_price
ORDER BY price, id
LIMIT 20;
```

Both profiles have a primary key and a `category_id` index. Tuned adds:

```sql
CREATE INDEX products_search_idx
ON products (category_id, price, id)
INCLUDE (name)
WHERE in_stock;
```

Tuned data is a copy of baseline; the query's business result is unchanged.
The planner picked plans on its own — no forced index usage, no disabled
seq scans.

## Environment and load

- Linux VM (Lima 2.2.0, Apple Silicon): Ubuntu 26.04, 4 vCPU, 4 GiB RAM.
- PostgreSQL 17.11 on Debian Bookworm image; Docker 29.9.1, cgroup v2.
- DB container: 2-CPU quota, 1 GiB RAM, `shared_buffers=256MB`.
- JIT and single-query parallelism disabled identically in both profiles.
- Load: native `pgbench` in a separate container, 1-CPU quota, 256 MiB RAM,
  two threads, eight clients, prepared statements.
- Traffic over an internal Docker network, no port forwarding through macOS.
- Reads: 15 s per measurement, four repeats per profile.
- Inserts: 10,000 transactions per each of eight clients, four repeats
  per profile, each on a fresh copy of the source database.
- Profiles alternate; a 2-second read warmup precedes each measurement.
- `fsync`, `synchronous_commit`, `full_page_writes` left enabled.

## Measurements

Medians of four runs per load type and profile:

| Metric | Baseline | Tuned | Change |
|---|---:|---:|---:|
| Search queries/s | 221.38 | 66,112.33 | +29,763.13%, ×298.63 |
| Mean search latency | 36.12 ms | 0.1165 ms | −99.68% |
| DB CPU per search query, approx | 9061 µs | 30.30 µs | −99.67% |
| Inserts/s | 21,735.53 | 20,994.97 | −3.41% |
| Mean insert latency | 0.362 ms | 0.375 ms | +3.59% |
| Table incl. indexes | 124.62 MiB | 196.92 MiB | +58.02%, +72.30 MiB |
| Transaction errors | 0 | 0 | — |

Search throughput spread: 219.61–223.17 q/s baseline, 64,499.60–66,767.57
tuned. Search got faster and inserts slower in all four pairs.
Insert spread: 20,962.31–21,936.61 vs 19,934.18–21,153.77.
Descriptive statistics only — no formal significance test.

Index build took ~0.65 s on this rig (including command startup); this does
not predict build time on a busy production database.

## Why the win is large

Saved `EXPLAIN (ANALYZE, BUFFERS)` for one category and min_price=500:

- Baseline: bitmap scan found 10,000 category rows, filtered 6,000,
  then sorted the survivors to take the first 20. Top plan node shows
  10,017 shared-buffer hits.
- Tuned: index-only scan returns the needed 20 rows in the right order.
  Four shared-buffer hits, zero heap fetches (`Heap Fetches: 0`), no sort.

This is removing wasted reading and sorting in a specific query.
Different filtering, ordering, column sets, or a frequently updated table
may produce a different result. After updates, index-only scans may hit the
heap until the next VACUUM.

## Checks and limits

All 16 measurements have `measurement_valid: true`; no query or collection
errors. Fingerprints of 300 control queries match before and after reads.
Every insert run ends at exactly 1,080,000 rows. Control queries are
verified — pgbench does not compare every response body.

During active runs the server used ~2 CPUs and the tuned client ~68–75% of
one CPU. The older single-threaded HTTP generator was not used here.
This is still a shared-VM test on synthetic data, not the maximum capacity
of a physical server.

Per-query CPU is computed from the DB's cgroup and includes background work
and measurement commands — it is not energy consumption. Disk size covers
the table and indexes, not RAM, the whole volume, or WAL. Mean latency does
not describe p95/p99. Updates, deletes, mixed workloads, cold cache, and a
full HTTP API were outside this experiment.

A large win applies to a query that previously lacked a suitable composite
index. Re-adding it on an already well-indexed database changes nothing.

## Reproduction and artifacts

```sh
make
./build/server-opt postgres --rows 1000000 --clients 8 \
  --duration 15 --repeats 4 --inserts-per-client 10000 \
  --output results/postgres-million.json
```

- `results/postgres-million.json` — full report: SQL, parameters, image ID,
  plans, all repeats, raw metrics.
- `results/postgres-million-summary.json` — condensed report.
- `results/postgres-million.log` — run order and pgbench progress.
- `results/postgres-million-instrumentation-error.json` — first big run:
  two Docker stats failures, `measurement_valid: false`. Kept for
  transparency and **not** used in the table above.

The fix: the metrics collector now starts after the client container is
running, not while it is in `created` state. The full experiment was rerun
after the fix. Containers, network, and the test-data volume were removed;
the image was kept for reproduction.

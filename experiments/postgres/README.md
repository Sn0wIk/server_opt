# PostgreSQL product-search benchmark

`server-opt postgres` compares two copies of the same catalog. Both have a
primary key and a plain `category_id` index. Tuned additionally creates a
partial covering index:

```sql
CREATE INDEX products_search_idx
ON products (category_id, price, id)
INCLUDE (name)
WHERE in_stock;
```

It matches the query "20 cheapest in-stock products in a category, priced at
least X" — a standard PostgreSQL capability. The experiment's goal is to
measure both the benefit *and* the cost of a fitting index. The program does
not automatically analyze or modify user databases.

Completed run on a million products: [RESULTS.md](RESULTS.md).

## Running

Requires Linux Docker with cgroup v2. The host tool runs on macOS and Linux,
drives the Docker CLI, and needs no local PostgreSQL or libpq.

From the project root:

```sh
make
./build/server-opt postgres
```

Defaults:

```sh
./build/server-opt postgres --rows 1000000 --clients 8 \
  --duration 10 --repeats 4 --inserts-per-client 1000 \
  --output results/postgres.json
```

Smoke test:

```sh
./build/server-opt postgres --rows 10000 --clients 2 \
  --duration 3 --repeats 2 --inserts-per-client 100 \
  --output results/postgres-smoke.json
```

A full run takes several minutes plus image pull, catalog creation and index
build. Completed stages are written to JSON incrementally; an incomplete
report has no `summary`. `--project-root DIR` lets you run the binary from
another directory.

## Method

- PostgreSQL 17 in its own container: 2-CPU quota, 1 GiB RAM,
  `shared_buffers=256MB`, JIT off, single-query parallelism off.
- Native `pgbench` from the same image, separate container: 1-CPU quota,
  256 MiB RAM, two threads, prepared statements.
- Clients reach PostgreSQL over a private Docker network — no load traffic
  through the host CLI or port forwarding.
- Deterministic catalog, 100 categories, mixed in-stock/out-of-stock items.
- Category and min_price are pseudo-random with a fixed seed; in timed mode
  query counts vary between runs.
- Baseline/tuned alternate; a read warmup precedes each measurement.
- 300 control queries are compared by fingerprint of ordered results before
  and after all read measurements — not a per-response check by pgbench.
- `EXPLAIN (ANALYZE, BUFFERS)` stores real plans of one control query.
- After reads, inserts are measured: fixed transactions per client.
  Each run uses a fresh copy of the source DB; final row count is verified.
- `fsync`, `synchronous_commit`, `full_page_writes` are recorded in the
  report and never disabled for speed.
- Measured: table+index size, index build time, mean latency, TPS, DB CPU
  per transaction, raw `docker stats`.

This measures the SQL layer — **not** HTTP API response time. A real app adds
business logic, serialization, network and pooling on top. Latency here is a
mean, not p95/p99. Fixed concurrency does not model a fixed request rate.

## Reading the report

In `summary.read` / `summary.insert`:

- `throughput_change_percent` — positive means more TPS.
- `latency_reduction_percent` — positive means lower mean latency.
- Both profiles carry medians, TPS range, and error counts.
- `server_cpu_us_per_transaction` includes DB background work and measurement
  commands — an approximate cost, not pure SQL execution time.

`extra_relation_bytes` and `relation_size_increase_percent` show the disk
price (relation sizes, not RAM/volume/WAL). The index may also slow inserts
and updates; updates/deletes are not exercised here. A search win does not
automatically become an app-wide win. Energy is not measured.

## Isolation and cleanup

The test creates fresh containers, a private internal network, and a volume
holding only synthetic data. The PostgreSQL port is never published to the
host. Inside this isolated rig, trust authentication is used — do not carry
that mode onto user or publicly reachable databases.

On completion or Ctrl+C, containers, network and volume are removed. The
`server-opt-postgres:local` image remains. After SIGKILL/host crash, cleanup
may be incomplete. Rig resources are labeled `app=server-opt-postgres`;
foreign databases and Docker resources are never touched.

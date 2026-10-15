# Zabbix — real-application findings

Rig: Lima VM (Ubuntu ARM64, 4 vCPU, 4 GiB), Docker:
`postgres:15-alpine` + `zabbix/zabbix-server-pgsql:alpine-7.0-latest` +
`zabbix-web-nginx-pgsql` + `zabbix-agent`. `pg_stat_statements` enabled.

Load: `load-host` with 10 trapper items, ~3 min of `zabbix_sender` batches
(200 values each), plus JSON-RPC API reads (`history.get`, `item.get`).

## First pass — bootstrap skew

| Rank | Calls | Mean | Total | Query |
|---:|---:|---:|---:|---|
| 1 | 4 | **2070 ms** | **8.28 s (77%)** | Trigger fetch via `IN (SELECT ... FROM triggers tg, functions f, items i, item_discovery id ...)` |
| 2–10 | 1 | 96–449 ms | ~1.9 s | Giant multi-row `INSERT INTO items / item_tag / ...` — one-off template load at startup |

The trigger query looked terrible — until `EXPLAIN ANALYZE` on the same
query ran in **0.3 ms**. The 2 s calls happened during bootstrap (config
sync on a busy, freshly-loaded DB). Lesson: **pg_stat_statements on a fresh
install is dominated by one-off noise — reset stats before measuring
steady state.**

## Steady state after `pg_stat_statements_reset()` + 4 min of load

| Query | Calls | Total | Mean |
|---|---:|---:|---:|
| `SELECT oid,typname FROM pg_type` | 403 | 169 ms | 0.42 ms |
| `UPDATE sessions SET lastaccess` | 391 | 89 ms | 0.23 ms |
| `SELECT * FROM history WHERE itemid=$1 AND clock>$2 ...` | 8,442 | 73 ms | **0.009 ms** |
| items + item_rtdata join | 201 | 69 ms | 0.34 ms |

Total DB time over 4 min of real load: **~600 ms ≈ 0.25% of one core.**
Honest conclusion: at this scale, Zabbix DB queries are already efficient —
there is nothing meaningful to optimize on the query side, and the tool
correctly reports that instead of inventing a problem.

## The real experiment: history housekeeping, 100M rows

The documented Zabbix pain is not queries but **history table housekeeping**:
unpartitioned `history` + `DELETE WHERE clock < ...` vs partitioned tables.

### Measured: 10M rows

| Approach | Deleted | Time | Space after |
|---|---:|---:|---|
| `DELETE` (stock housekeeper) | 3,088,445 | 2,994 ms | table stays **1,094 MB** — nothing returned to OS |
| `DROP TABLE` ×10 partitions | 2,932,261 | ~50 ms | **786 MB** — ~308 MB freed instantly |

### Measured: 100M rows (`history` = 10 GB, 30 days of data)

Concurrent reads (`SELECT ... WHERE itemid=? AND clock > now-1d`) ran
throughout both cleanups:

| Metric | DELETE (stock) | Partitions (DROP) |
|---|---:|---:|
| Delete time (32.9M rows) | **31,527 ms** (seq scan over 100M) | **~3 s** wall / hundreds of ms in DB |
| VACUUM afterwards | **+41.9 s** (33M dead tuples) | not needed |
| Total heavy work | **~74 s** | **~3 s** |
| Disk returned to OS | none — table stays 10 GB | ~1.6 GB freed instantly |
| p50 concurrent read during cleanup | 182 ms | 57 ms |
| p95 concurrent read during cleanup | 211 ms | 70 ms |

**Bonus:** reads on the partitioned table are ~3× faster in steady state
(57 ms vs 182 ms p50) — each partition's index is shallower. Dashboards and
`history.get` benefit permanently, not only during cleanup.

### Caveats

- Volumes differ: DELETE removed 32.9M rows; DROP removed ~15.8M (the
  boundary partition stays one more day — as in real retention schemes).
- Read degradation during DELETE is small *here* — 4-vCPU VM, data in RAM.
  On real disk + replicas the picture is worse (WAL storm, IO, lag).
- Absolute numbers are ARM-VM/Docker measurements; the *ratios* and
  methodology are what carry over.

### Money (transparent projection)

- Housekeeping: ~25× less heavy CPU/IO work per cleanup window. On
  deployments at tens of thousands NVPS, housekeeper runs hours daily.
- Storage: instant reclaim vs accumulating bloat — typically −30–50% on
  history-table volume.
- Reads ~3× faster on the hot path → less CPU per query.

Realistic order of magnitude for a mid-size deployment: **~$100–500/month
per database** from partitioning history — measured deltas, not hand-waving.

## Product lessons

- Stats must be reset (or windowed) after bootstrap, or one-off import noise
  drowns real queries — `--reset` support needed.
- Implicit comma joins hide inside `IN (SELECT ...)` subqueries — flagging
  checks for `FROM a b,c` patterns, not just `JOIN` keywords.
- Query text can be megabytes (schema-import INSERTs) — truncate in SQL,
  not after fetch.
- "Table grows by time, no partitioning" is a detectable, verifiable class
  of waste — the strongest optimization candidate found so far.

## Scripts

- `partitioned.sql` — partitioned table demo at 10M rows.
- `partitioned_100m.sql` — rebuild `history_p` at 100M rows.
- `read_during_cleanup.sh` — concurrent-read latency sampler.

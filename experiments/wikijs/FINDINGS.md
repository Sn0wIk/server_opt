# Wiki.js — real-application findings

Rig: Lima VM (Ubuntu ARM64, 4 vCPU), Docker, `postgres:15-alpine` +
`ghcr.io/requarks/wiki:2`. `pg_stat_statements` enabled. 150 pages created
via the GraphQL API, then read-side page load via the C++ generator.

## pg_stat_statements top by cumulative time

| Calls | Total, ms | Query |
|---:|---:|---|
| 152 | 648.1 | `truncate "pageTree" restart identity` |
| 306 | 433.9 | `select * from information_schema.columns where table_name = ...` |
| 51 | 164.3 | `insert into "pageTree" (...)` — 5,100 rows total |
| 1,734 | 133.4 | `select pages.id, pages.path, ... where localeCode=$1 and path=$2` — 0.077 ms, already fast |

## Findings

1. **Full page-tree rebuild on every write.** Creating a page makes Wiki.js
   `TRUNCATE pageTree` and re-insert all nodes — O(n) per write, growing
   roughly quadratically with page count. On 150 pages: 648 ms total just
   for truncate. An index cannot fix this — it is an application
   architecture pattern (known Wiki.js 2 behaviour: tree rebuild on change).

2. **ORM schema introspection per operation.** 306 queries against
   `information_schema.columns` — 434 ms. Objection.js/Knex re-reads table
   schema; a candidate for schema caching in the app.

3. **Page reads are already fast.** The main page SELECT runs ~0.077 ms —
   the `(localeCode, path)` index exists; nothing to add.

## Product lesson

The real culprit was not a missing index but an **application pattern**:
frequent DDL/TRUNCATE and schema introspection under load. An analyzer that
only suggests indexes would miss it. The tool therefore also flags:

- frequent `TRUNCATE` / DDL under load;
- repeated `information_schema` / `pg_catalog` queries;
- "insert + full rebuild of a related table" patterns;
- high `total_exec_time` with low `mean_exec_time` — a frequency problem,
  not a single-call problem.

GraphQL search returned 0 hits — the `db` search engine requires index setup
that was not configured; search queries were not profiled in this session.

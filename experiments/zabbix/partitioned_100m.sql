DROP TABLE IF EXISTS history_p;
CREATE TABLE history_p (
  itemid bigint NOT NULL,
  clock int NOT NULL,
  value double precision,
  ns int NOT NULL DEFAULT 0,
  PRIMARY KEY (itemid, clock, ns)
) PARTITION BY RANGE (clock);

DO $$
DECLARE base int := extract(epoch from date_trunc('day', now() - interval '31 days'))::int;
BEGIN
  FOR d IN 0..31 LOOP
    EXECUTE format('CREATE TABLE history_p_%s PARTITION OF history_p FOR VALUES FROM (%s) TO (%s)',
                   d, base + d*86400, base + (d+1)*86400);
  END LOOP;
END $$;

INSERT INTO history_p (itemid, clock, ns, value) SELECT itemid, clock, ns, value FROM history;

SELECT count(*) AS rows_loaded,
       pg_size_pretty(sum(pg_total_relation_size(inhrelid))) AS size
FROM history_p h JOIN pg_inherits i ON h.tableoid = i.inhrelid
WHERE i.inhparent = 'history_p'::regclass;

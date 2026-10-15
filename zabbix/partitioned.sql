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

INSERT INTO history_p (itemid, clock, ns, value)
SELECT 50740 + (i % 10),
       extract(epoch from now())::int - (i % 2592000),
       i,
       random()*100
FROM generate_series(1,10000000) i
ON CONFLICT DO NOTHING;

SELECT count(*) AS rows_loaded, pg_size_pretty(pg_total_relation_size('history_p')) AS size
FROM history_p;

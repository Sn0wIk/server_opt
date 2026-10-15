#!/bin/bash
# Samples a concurrent SELECT latency while a cleanup operation runs.
# Usage: read_during_cleanup.sh SECONDS
# Prints one "elapsed_ms" per line.
END=$((SECONDS + ${1:-60}))
while [ $SECONDS -lt $END ]; do
  t0=$(date +%s%N)
  docker exec zbx-db psql -U zabbix -d zabbix -XqAt -c \
    "SELECT count(*) FROM history WHERE itemid=50741 AND clock > extract(epoch from now())::int - 86400;" >/dev/null 2>&1
  t1=$(date +%s%N)
  echo $(( (t1 - t0) / 1000000 ))
done

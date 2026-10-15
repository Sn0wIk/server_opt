#!/bin/bash
# Prepare Wiki.js content + generate read load. Run inside the Lima VM.
# Credentials come from env vars — never commit real passwords/tokens:
#   WIKI_ADMIN_USER, WIKI_ADMIN_PASSWORD, WIKI_BASE
set -euo pipefail
BASE=${WIKI_BASE:-http://127.0.0.1:3000}
WIKI_USER=${WIKI_ADMIN_USER:?'set WIKI_ADMIN_USER'}
WIKI_PASS=${WIKI_ADMIN_PASSWORD:?'set WIKI_ADMIN_PASSWORD'}

TOKEN=$(curl -s "$BASE/graphql" -H 'Content-Type: application/json' \
  -d "{\"query\":\"mutation { authentication { login(username: \\\"$WIKI_USER\\\", password: \\\"$WIKI_PASS\\\", strategy: \\\"local\\\") { responseResult { succeeded } jwt } } }\"}" \
  | sed -n 's/.*"jwt":"\([^"]*\)".*/\1/p')
[ -n "$TOKEN" ] || { echo "login failed"; exit 1; }

WORDS="server optimization database index query cache latency throughput memory container benchmark kubernetes docker linux ubuntu postgresql nginx"
create_page() {
  local i=$1
  local title="Test page $i"
  local body
  body=$(for w in $WORDS; do printf '%s %s %s ' "$w" "$w$i" "$i$w"; done)
  curl -s "$BASE/graphql" -H 'Content-Type: application/json' \
    -H "Authorization: Bearer $TOKEN" \
    -d "{\"query\":\"mutation { pages { create(content: \\\"# $title\\\\n$body\\\", description: \\\"test $i\\\", editor: \\\"markdown\\\", isPublished: true, isPrivate: false, locale: \\\"en\\\", path: \\\"p$i\\\", tags: [], title: \\\"$title\\\") { responseResult { succeeded errorCode } } } }\"}" \
    | grep -q '"succeeded":true' || echo "page $i failed"
}

for i in $(seq 1 "${1:-150}"); do create_page "$i"; done
echo "pages created"

for i in 1 2 3 4 5; do
  curl -s -o /dev/null "$BASE/"
  curl -s -o /dev/null "$BASE/en/p$((i * 7))"
  curl -s -o /dev/null "$BASE/login"
done
echo "warmup done"

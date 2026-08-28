#!/usr/bin/env bash
# SessionStart hook: print what is top of mind, from label truth. Never fails the session.
cd "$(dirname "$0")/.." || exit 0
echo "== rete session start =="
sed -n '/^## Current focus/,/^## /p' CLAUDE.md | sed '$d' | grep -v '^## Current focus'
echo "-- open P0/P1 (label truth):"
timeout 8 gh issue list --state open --label P0 --json number,title --jq '.[] | "  P0 #\(.number) \(.title)"' 2>/dev/null
timeout 8 gh issue list --state open --label P1 --json number,title --jq '.[] | "  P1 #\(.number) \(.title)"' 2>/dev/null
echo "-- git: $(git branch --show-current), $(git status --short | wc -l) uncommitted, $(git log --oneline @{u}.. 2>/dev/null | wc -l) unpushed"
echo "-- status page stamp: $(tr -d "\n" < docs/status/dev-status.html | grep -o "Status synthesized [^<]*<[^>]*>[^<]*" | sed "s/<[^>]*>//g" | head -1)"
exit 0

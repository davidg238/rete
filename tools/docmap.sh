#!/usr/bin/env bash
# Regenerates the generated-inventory section of review/DOCMAP.md.
#
# The map has two layers: a hand-curated "documents of record" header (everything
# above the BEGIN marker — edit by hand, keep it short) and a generated inventory
# (everything below — never edit by hand, rerun this script instead). Generated,
# not curated, because a hand-maintained full index rots, and a stale index is a
# silent gate: it looks authoritative precisely when it is wrong.
set -euo pipefail

MAP="$(cd "$(dirname "$0")/.." && pwd)/review/DOCMAP.md"
MARKER='<!-- BEGIN GENERATED INVENTORY — do not edit below; rerun tools/docmap.sh -->'

# The five doc homes of the program (two workspace roots).
ROOTS=(
  "$HOME/workspace/tuvm/review"
  "$HOME/workspace/tuvm/docs"
  "$HOME/workspace/tuvm/nodus/nsl/docs"
  "$HOME/workspace/porta/docs"
  "$HOME/workspaceToit/nodus/docs"
)

kind_of() {
  case "$1" in
    */superpowers/plans/*) echo plan ;;
    */design/*)            echo design ;;
    */superpowers/*)       echo note ;;
    *)                     echo record ;;
  esac
}

title_of() {
  local f="$1"
  case "$f" in
    *.html) grep -oPm1 '(?<=<title>).*?(?=</title>)' "$f" 2>/dev/null || true ;;
    *)      grep -m1 '^# ' "$f" 2>/dev/null | sed 's/^# *//' || true ;;
  esac
}

[ -f "$MAP" ] || { echo "missing $MAP — write the curated header first" >&2; exit 1; }
grep -qF "$MARKER" "$MAP" || { echo "missing marker in $MAP" >&2; exit 1; }

tmp="$(mktemp)"
sed "/$(printf '%s' "$MARKER" | sed 's/[^^]/[&]/g; s/\^/\\^/g')/q" "$MAP" > "$tmp"

{
  echo
  echo "_Regenerated $(date +%F) by \`tools/docmap.sh\`. Legend: **record** = mainline doc of record · **design** = dated decision record (\`docs/design/\`) · **plan** = execution scaffolding (\`docs/superpowers/plans/\`) · **note** = superpowers notes/findings._"
  for root in "${ROOTS[@]}"; do
    [ -d "$root" ] || continue
    repo_dir="$(git -C "$root" rev-parse --show-toplevel 2>/dev/null || echo "$root")"
    echo
    echo "### ~${root#"$HOME"}"
    echo
    find "$root" \( -name '*.md' -o -name '*.html' \) -not -path '*/.superpowers/*' | sort | while read -r f; do
      kind="$(kind_of "$f")"
      title="$(title_of "$f")"
      date="$(git -C "$repo_dir" log -1 --format=%as -- "$f" 2>/dev/null || true)"
      rel="~${f#"$HOME"}"
      printf -- '- `%s` — %s _(%s%s)_\n' "$rel" "${title:-${f##*/}}" "$kind" "${date:+, $date}"
    done
  done
} >> "$tmp"

mv "$tmp" "$MAP"
echo "regenerated: $MAP"

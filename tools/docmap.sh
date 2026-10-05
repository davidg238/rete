#!/usr/bin/env bash
# Regenerates the generated-inventory section of review/DOCMAP.md.
#
# The map has two layers: a hand-curated "documents of record" header (everything
# above the BEGIN marker — edit by hand, keep it short) and a generated inventory
# (everything below — never edit by hand, rerun this script instead). Generated,
# not curated, because a hand-maintained full index rots, and a stale index is a
# silent gate: it looks authoritative precisely when it is wrong.
set -euo pipefail

export LC_ALL=C  # the inventory sorts; a locale collation reorders every line (2026-10-05)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MAP="$ROOT/review/DOCMAP.md"
MARKER='<!-- BEGIN GENERATED INVENTORY — do not edit below; rerun tools/docmap.sh -->'

# The doc homes of the program. porta and nsl are in-repo now (rete
# consolidation Task 4); the two ~/workspaceToit entries are genuinely separate
# repos living outside this tree — nodus is the frozen Toit v1 implementation
# (GitHub: nodus-v1), and stic is the cross-VM benchmark harness, which stays
# outside rete on purpose (design D12): a harness that measures rete against
# other VMs does not belong inside rete.
#
# $ROOT/nodus/oevm/docs exists since 2026-08-29 (#79): the consolidation design
# (2026-08-02, fate table row "tuvm docs") called for VM-specific documents to
# live with the VM and program-level ones at the root, and Task 1 had left them
# all at the root. Reference docs live with their subsystem; dated records
# (design/, results/, plans) stay at the program level where the citations point.
ROOTS=(
  "$ROOT/review"
  "$ROOT/docs"
  "$ROOT/nodus/oevm/docs"
  "$ROOT/protocol"
  "$ROOT/nodus/nsl/docs"
  "$ROOT/nodus/oevm/zephyr/docs"
  "$ROOT/porta/docs"
  "$HOME/workspaceToit/nodus/docs"
  "$HOME/workspaceToit/stic"
)

# In-repo entries are emitted repo-relative, outside ones as ~-absolute. The
# repo's own absolute path is not stable — it was ~/workspace/tuvm, it is
# ~/workspace/rete, and a branch worktree is somewhere else again — so an
# absolute in-repo path would bake a citation that is wrong the next time the
# checkout moves.
label_of() {
  case "$1" in
    "$ROOT")   echo "." ;;
    "$ROOT"/*) echo "${1#"$ROOT"/}" ;;
    "$HOME"/*) echo "~${1#"$HOME"}" ;;
    *)         echo "$1" ;;
  esac
}

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
    echo "### $(label_of "$root")"
    echo
    find "$root" \( -name '*.md' -o -name '*.html' \) -not -path '*/.superpowers/*' | sort | while read -r f; do
      kind="$(kind_of "$f")"
      title="$(title_of "$f")"
      date="$(git -C "$repo_dir" log -1 --format=%as -- "$f" 2>/dev/null || true)"
      rel="$(label_of "$f")"
      printf -- '- `%s` — %s _(%s%s)_\n' "$rel" "${title:-${f##*/}}" "$kind" "${date:+, $date}"
    done
  done
} >> "$tmp"

mv "$tmp" "$MAP"
echo "regenerated: $MAP"

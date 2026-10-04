// Copyright (c) 2026 Ekorau LLC

package space

// IsStale answers whether a cell predates its node's current boot — the
// rete#210 mixed-read row. It is deliberately narrow, because every widening of
// it would be a way to call a truthful row a lie:
//
//   - only inbound rows can be stale. An 'out' row is porta's own intent, which
//     no node reboot invalidates, and a 'gw' row is porta's verdict.
//   - a row with boot_id 0 is UNKNOWN, not stale — it is either a node with no
//     persistent identity (posix without OEVM_BOOTID_FILE, a board without
//     CONFIG_SETTINGS) or a row written before the column existed.
//   - if the node's current boot is unknown there is nothing to compare against,
//     so nothing is marked.
//
// One rule, read by both `porta get` and custos's cells.stale column.
func IsStale(direction string, rowBoot, currentBoot int64) bool {
	return direction == "in" && rowBoot != 0 && currentBoot != 0 && rowBoot != currentBoot
}

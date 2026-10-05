// Copyright (c) 2026 Ekorau LLC

// Package store is the vocabulary of porta's store that its readers share:
// the gateway's own direction and path, and the one rule for a stale row.
// porta writes the store; custos reads it. Both import this, so neither
// restates it (rete#338).
package store

// DirectionGateway is the third value of the direction column: a row porta
// wrote ABOUT a node rather than TO it. The two wire directions are 'in'
// (the node's own authority) and 'out' (porta's intent).
const DirectionGateway = "gw"

// PathLiveness is the one gw/** path today: porta's verdict on whether a node
// is still making progress. One of the five verdicts in porta's sweep.go
// (#live, #stalled, #silent, #neverSeen, #unknown), as a Symbol.
const PathLiveness = "gw/liveness"

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

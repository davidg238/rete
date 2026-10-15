# Patch through porta (rete#432)

_DRAFT, 2026-10-15, for iteration with David. Branch `432-patch-porta`._

## Where it stands

0.1 is *one node, every verb — load, debug, patch, profile — through porta, over the radio, while it sleeps*. Load
(#233, #398), debug (#375–#380) and profile (#415) are built through porta on both bells. Patch is built over the cable
only (#313). David, 2026-10-14: patch through porta is in 0.1. David, 2026-10-15: (1) assume a patch must be split;
(2) if debug sessions survive deep sleep, patches probably should too, for full debug capability.

## Findings

- **F1. A patch is one debug line, and it does not fit porta's value.** `patch_method.py` writes the VM's patch text,
  which is sent as `dbg:patch <text>` (`supervisor.nsl` `doPatch:`, `vm.c` `primitive_patch`). The cable's line buffer
  caps it at 511 bytes (`patch_method.py` `LINE_BYTES`). porta's limit is 256 bytes per encoded value
  (`MAX_VALUE_BYTES`, RECORDS §1). The header alone is about 60 characters and each instruction or constant 9 more, so
  a method of more than about 20 words does not fit. David's (1) holds.
- **F2. Breakpoints survive deep sleep because they are a cell, and a patch today is a line.** `goal/debug/main` =
  `Debug{sha, breaks}` is the whole breakpoint set for one image. porta replays a cell to a fresh boot, and the node
  arms it in the first check-in of every wake, before the program's work (`DebugLink>>acceptGoal:`). A `dbg/req` row
  is not replayed (#262). A patch sent as a line lasts until the next cold boot, which on the MagTag is the next wake.
  So for (2), the patch must be in the cell.
- **F3. The split already exists: porta's blob store and the node's block fetch.** Program OTA moves an image as
  content-addressed 256-byte blocks (`porta/internal/blockserv`; the node's `fetch_block`, `nodus/oevm/src/ota.c:169`),
  each block retried 5 times, the whole checked by its SHA-256. A patch set is a small blob. Using that path means no
  second way of splitting.
- **F4. The VM cannot remove a patch.** `primitive_patch` installs a replacement and records it in `method_patches`
  (64 entries, `oevm.h:569`); the old method stays reachable, since it may still be running, and there is no
  uninstall. A breakpoint set can shrink in place (`dbg:clear`); a patch set can only shrink at a boot.
- **F5. custos has no debug verbs on a porta node.** It serves `log runs panics health load`, and `profile` with a map
  (`custos/internal/custos/board.go:273`). The debug session speaks through one interface, `debug.Line` (`Send`), and
  is fed replies by `OnLine`. Remote debug on the boards was driven by hand with `porta put` and `porta log`. porta's
  HTTP has `POST /blob`, `PUT` and `GET` of one cell (#398), and `PUT dbg/req` (#415 T2), but no route that reads
  `dbg/resp` rows.
- **F6. A breakpoint inside a patched method has no blob offset.** Breakpoints are `[blob, off]` in the loaded image,
  and a patch's method is not in the blob. #313 does not support it over the cable either. Out of scope here.

## Proposal

**One rule: what must survive a sleep is in the cell; everything else is a line.** The cell is
`Debug{sha, breaks, patches}`. `patches` is the SHA-256 of a blob holding the whole patch set for the image `sha`,
one patch text per line, applied in order. An empty `patches` means no patches. A session command (`step`, `bt`,
`inspect`, `continue`, `restart`) stays a `dbg/req` line, as today.

**Node (kernel and VM).**
- `DebugLink>>acceptGoal:` compares the cell's `patches` with the set it has applied this boot.
  - The same set: nothing to do.
  - A longer set that begins with the applied set (the usual case, one more patch): fetch the blob, apply the new
    lines.
  - Anything else (a patch removed or changed): the set cannot shrink in place (F4), so the node reboots. A
    software reset keeps RTC memory, and the next boot applies the new set from the start.
- The fetch is a new primitive over `fetch_block` that reads a blob into a ByteArray, checking the hash. A patch set
  is a few blocks; nothing is written to flash.
- Patches are applied before breakpoints are armed and before `main:`, so a wake runs the patched program from its
  first line.
- Debug images only, like every patch (#313). A standard image ignores `patches`.

**custos.**
- A porta node gains the debug verbs, through a porta `debug.Line`: `Send` is `PUT dbg/req`, and replies come from
  `dbg/resp` rows.
- `break` and `clear` rewrite the cell's whole breakpoint set, instead of sending a line, so a breakpoint set from
  custos survives a sleep as it does when written with `porta put`.
- `POST /board/<n>/debug/patch?method=…` runs `patch_method.py` as on a cable. custos then adds the text to the node's
  patch set (a new patch of the same method replaces the old one), puts the set as a blob, and rewrites the cell. It
  answers at once, `Pending`, and the node's `dbg:ok patch` or `dbg:error patch` arrives on `dbg/resp`. The patch set
  is rebuilt from the cell and the blob store whenever custos restarts, so custos keeps no state of its own.
- A `load` (a new image) leaves the old patch set naming the old `sha`, and the node refuses it, as it refuses
  breakpoints for another image today. custos clears both when it loads.

**porta.** One general route: `GET /node/{id}/log?path=dbg/resp&after=<seq>`, the twin of `porta log`, so custos
can read replies. No patch-specific code in porta (D1–D2: porta stays language-neutral).

## Alternatives not taken

- **Split the text across `dbg/req` lines** (`dbg:patch+ <part>` …). It needs a reassembly protocol of its own, and
  lines are not replayed, so the patch dies at the next wake. Fails (2).
- **One cell per patch** (`goal/debug/patch/<n>`). A cell cannot be removed, so a dropped patch needs a tombstone, and
  the number of cells is an invented bound. It also still needs splitting for a patch over 256 bytes.

## Gates

- **Host:** `porta_sync_host.sh` gets a leg in which an agent patches a method through custos and a real porta. The
  node's output changes; then the node restarts and still runs the patched method. A second patch extends the set
  with no reboot, and removing one reboots the node into the smaller set.
- **Falsify first:** with the replay of `patches` at boot removed, the restart part goes red.
- **Boards:** the wall bell (always on, Thread) and the MagTag (deep sleep, WiFi), each patched through custos, with
  the patch still in force after a wake.

## Questions for David

1. **The cell, or a cell of its own?** `patches` as a field of `Debug{sha, breaks, patches}` (one record holds
   everything a debug image is told to be, and the sha check covers both), or a separate `goal/patch/main` cell.
   _Recommend the field._
2. **Removing a patch reboots the node** (F4). On the wall bell that is a restart with no ring missed unless it falls
   on the hour. Accept, or keep patches additive until the next load? _Recommend the reboot: the cell then always
   says what runs._
3. **Signed or not?** Program OTA checks porta's ed25519 signature because a standard image takes it. A patch reaches
   only a debug image, which already obeys unsigned `dbg/req` lines and breakpoints. _Recommend unsigned, said
   plainly in `limits.md`._
4. **Fetch at every wake.** A deep-sleep node re-fetches its patch set at each wake (a few 256-byte blocks over WiFi,
   in the check-in). Measure it first rather than keep a copy in RTC memory? _Recommend measure first._
5. **custos's debug verbs over porta come with this issue** (F5): without them no agent can patch through porta.
   That makes #432 the remote debug front as well (`break`, `step`, `bt` …, through custos). In scope?

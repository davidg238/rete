# Node id = factory link address (#118/#242) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `platform nodeId` answers the board's factory link address read from silicon (lowercase hex), the build-time `OEVM_NODE_ID` plumbing is deleted, and the old build-time board name moves to a new optional `board` field on `obs/build`.

**Architecture:** One backend seam pair in `src/platform_backend.h` (`platform_backend_node_id`, `platform_backend_board_name`) — posix answers from env, zephyr answers from silicon / a generated C string — so `platform.c` loses its `#ifdef` id blocks. Formatting is one header-only function (`src/link_addr.h`), golden-tested as a C unit test. The board name is a new primitive (159) plus an optional manifest field.

**Tech Stack:** C99 (`nodus/oevm/src`, `src/backend/{posix,zephyr}`), Zephyr (ESP32 tree `~/zephyr-esp32`, NCS tree `~/ncs`), CMake, bash (`zephyr/run.sh`), nsl (`nodus/nsl/lib/kernel.nsl`), CDDL/zcbor (`protocol/MANIFEST.cddl`), Go (`porta`, fixtures only).

**Spec:** `docs/design/2026-09-11-node-identity-hardware-rooted-design.md` — the decision AND its "Addendum — 2026-10-02" (which this plan implements exactly; neither is to be revisited).

> **EVERY CODE SNIPPET BELOW IS A SKETCH THAT HAS NEVER RUN.** Trust the tree over the snippet: names, signatures, Kconfig symbols and include paths are to be verified (`rg`, read the neighbour) before use. Where a snippet and the tree disagree, the tree wins and the task's *test* is the arbiter. Cite symbols, not line numbers.

## Global Constraints

- Branch `118-node-id-factory-address` (already checked out). One commit per task, message ends `refs #118 #242` (NEVER a closing keyword), then the Co-Authored-By line from the session attribution.
- **No backward compatibility**: no dual-read, no deprecation, no versioned field. Both ends are reflashed together. Dead code is deleted in the task that kills it.
- **No invented tunables.** `PLATFORM_NODE_ID_MAX` is the only new constant; it is the old host buffer size.
- **Never touch `review/`.** Its hits are listed for David in Task 7.
- **Never edit dated records** (`docs/design/**` except the addendum commit in Task 0, `docs/results/**`, `docs/superpowers/plans/**` other than this file, `docs/status/focus-history.md`).
- nsl tests: inline `(* expect: *)` header at the TOP, `println`, no `.expected` files. Single test: `cd nodus/nsl && python3 tools/run-one.py <file.nsl> B` — **always pass `B`**. Path-A bakes clobber the tracked `nodus/oevm/src/solidified_image.c`; if `git status` shows it modified, `git checkout -- nodus/oevm/src/solidified_image.c`.
- The full nsl suite (~20 min) runs only in the controller, one at a time, in the background with `-u` to a file, **never under `timeout`, never through `tail`**, and nothing under `kernel/porta/manifest` is touched while it runs. Never run it beside `porta_sync_host.sh` (udp 16969/16970).
- Model A builds go to `z170` over ssh; every build in this plan is Model B (the default) and stays local.
- Subagents do NOT run: the nsl suite, any build under `zephyr/`, any flash/ssh/board command. Those are controller steps (Tasks 8–12).
- `MANIFEST.cddl` rules: named `sym-*` keys only; key order IS wire order (shorter encoded key first, then lexicographic); zcbor 0.9.1.

## Review Focus

1. **Env set but empty** (`OEVM_NODE_ID=`): must be "unset" → primitive fails (Task 2 test).
2. **Id longer than the buffer**: must fail, never silently truncate into a colliding id (Task 1/2 tests).
3. **A build told no board name**: `obs/build` carries no `board` key at all (not an empty string) (Task 6 test).
4. **A board name containing odd characters** (it is a file name from `~/.config/rete/console-<board>.env`): CMake must not mangle or inject it into C (Task 5 step: quote-safe generation + assertion).
5. **A stale shell with `OEVM_NODE_ID` exported building a Zephyr image**: now silently ignored — run.sh's `[node]`/`[board]` line must make the missing board name loud (Task 5).

## File Structure

| File | Change |
|---|---|
| `nodus/oevm/src/link_addr.h` | NEW, header-only `link_addr_hex` (the one formatting rule) |
| `nodus/oevm/tests/test_link_addr.c` | NEW C unit test (golden) |
| `nodus/nsl/tools/run_tests_oevm.py` | one row in `C_UNIT_TESTS` |
| `nodus/oevm/src/platform_backend.h` | declare the two seam functions |
| `nodus/oevm/src/backend/posix/platform_posix.c` | host answers from env |
| `nodus/oevm/src/backend/zephyr/platform_zephyr.c` | ESP hwinfo / nRF FICR / RP fail; board name from generated C |
| `nodus/oevm/src/platform.c` | delete both `#ifdef` id blocks and `oevm_host_node_id`; `PRIM_pf_nodeId` uses the seam; new `PRIM_pf_boardName` case |
| `nodus/oevm/src/{prim_ids.h,dispatch.c,prim_names.def}` | prim 159 `boardName` |
| `nodus/nsl/lib/kernel.nsl` | `boardName` accessor; `publishBuild:` gains `board` |
| `nodus/oevm/zephyr/{CMakeLists.txt,prj_esp32.conf,prj_esp32s2.conf,prj_esp32c6.conf,run.sh}` | HWINFO; board-name plumbing; assertion retarget |
| `nodus/oevm/zephyr/cmake/gen_node_id.cmake` | `git mv` → `gen_board_name.cmake`, fallback chain deleted |
| `nodus/oevm/zephyr/NodeIdProbe.nsl` | NEW 6-line board-proof node |
| `protocol/{MANIFEST.cddl,descriptors/records.yaml,PROTOCOL-NSL-RECORDS.md}` | `board` field |
| `nodus/nsl/tools/fixtures/manifest_goldens.nsl` | one more `obs/build` golden |
| `nodus/tests/23_mprof/{05_node_id,06_node_id_unset,07_node_id_toolong}.nsl`, `nodus/tests/22_spacesync/{25_board_name,26_board_name_unset,27_build_board,28_build_board_absent}.nsl` | tests |
| docs (Task 7) | see its list |

---

### Task 0: Commit the addendum (the spec travels with the plan)

**Files:** `docs/design/2026-09-11-node-identity-hardware-rooted-design.md` (already modified in the working tree, uncommitted), this plan.

- [ ] **Step 1:** `git status --short` must show exactly the design doc modified (and this plan untracked). If anything else is dirty, stop and report.
- [ ] **Step 2:** `git add docs/design/2026-09-11-node-identity-hardware-rooted-design.md docs/superpowers/plans/2026-10-02-node-id-factory-address.md && git commit -m "docs(#118): node id addendum 2026-10-02 and the implementation plan, refs #118 #242"`

---

### Task 1: The one formatting rule, golden-tested on the host

**Files:** Create `nodus/oevm/src/link_addr.h`, `nodus/oevm/tests/test_link_addr.c`; Modify `nodus/nsl/tools/run_tests_oevm.py` (`C_UNIT_TESTS`).

**Why a header, not a `.c`:** a new VM `.c` costs nine enumerated lists (`tuvm-vm-source-lists`); a header costs none. A C unit test is one registry row.

**Interfaces — Produces:** `static inline size_t link_addr_hex(const uint8_t *bytes, size_t n, char *out, size_t cap)` — writes lowercase hex, most significant byte first, no separators, NUL-terminated; returns `2*n`, or `0` (and `out[0]='\0'` when `cap>0`) if `n==0` or `cap < 2*n+1`.

- [ ] **Step 1: Write the failing test** `nodus/oevm/tests/test_link_addr.c` (model the header comment and `main` on `tests/test_reboot_note.c`):

```c
#include "../src/link_addr.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    char out[32];
    const uint8_t a[6] = {0xde,0xad,0xbe,0xef,0x00,0x0a};
    const uint8_t z[6] = {0x00,0x01,0x02,0x03,0x04,0x05};
    assert(link_addr_hex(a, 6, out, sizeof out) == 12);
    assert(strcmp(out, "deadbeef000a") == 0);            /* lowercase, MSB first, no colons */
    assert(link_addr_hex(z, 6, out, sizeof out) == 12);
    assert(strcmp(out, "000102030405") == 0);            /* a leading zero byte is KEPT */
    assert(link_addr_hex(a, 6, out, 12) == 0 && out[0] == '\0');   /* no room for NUL: refuse, never truncate */
    assert(link_addr_hex(a, 0, out, sizeof out) == 0);
    printf("test_link_addr OK\n");
    return 0;
}
```
- [ ] **Step 2:** Add the row to `C_UNIT_TESTS` beside `test_reboot_note`: `("test_link_addr", ENG_A, [], None),` with a one-line comment `# rete#118: the node id's one formatting rule — golden`. Run the test the way the registry does: `cd nodus/oevm && gcc -std=c99 -Isrc tests/test_link_addr.c -o /tmp/tla && /tmp/tla` (it needs only the header). Expected now: compile error `link_addr.h: No such file`.
- [ ] **Step 3:** Create `src/link_addr.h` with the function (sketch):

```c
#ifndef OEVM_LINK_ADDR_H
#define OEVM_LINK_ADDR_H
#include <stddef.h>
#include <stdint.h>
static inline size_t link_addr_hex(const uint8_t *b, size_t n, char *out, size_t cap) {
    static const char d[] = "0123456789abcdef";
    size_t i;
    if (n == 0 || cap < 2 * n + 1) { if (cap > 0) out[0] = '\0'; return 0; }
    for (i = 0; i < n; i++) { out[2*i] = d[b[i] >> 4]; out[2*i + 1] = d[b[i] & 15]; }
    out[2 * n] = '\0';
    return 2 * n;
}
#endif
```
- [ ] **Step 4:** Re-run Step 2's command. Expected: `test_link_addr OK`.
- [ ] **Step 5 (falsify):** change `"0123456789abcdef"` to uppercase; the test must abort on the first `strcmp`. Revert.
- [ ] **Step 6:** commit `feat(#118): link_addr_hex — the one id formatting rule, golden-tested`.

---

### Task 2: Host `platform nodeId` answers the env value or FAILS

**Files:** Modify `nodus/oevm/src/platform_backend.h`, `src/backend/posix/platform_posix.c`, `src/platform.c`; Modify `nodus/tests/23_mprof/05_node_id.nsl`; Create `nodus/tests/23_mprof/06_node_id_unset.nsl`.

**What failure looks like (verified in the tree):** `platform_dispatch` returning `0` falls through `send_resolve` to `does_not_understand`, which prints `VM Error: Message 'nodeId' not understood by receiver … ` on stderr and terminates non-zero (catchable by `on:do:` like any DNU). There is no nil-answer and no dedicated error primitive; this plan keeps it so (mechanism minimization) and adds ONE console line before the return so the cause is not a misleading DNU. A bridge node's `nodeId = ( ^ platform nodeId )` is evaluated at setup, so a board without an address stops at boot — the decision's "does not boot with a made-up one". The harness check is `expect-runtime-fail: nodeId` (`_check_runtime_fail`: substring of stderr, plus any `expect:` lines against stdout before the crash).

**Interfaces — Produces:** in `platform_backend.h`:
```c
#define PLATFORM_NODE_ID_MAX 96
size_t platform_backend_node_id(char *out, size_t cap);     /* strlen of the id written, 0 = this board has none */
size_t platform_backend_board_name(char *out, size_t cap);  /* same contract; Task 4 uses it */
```
(declare both now; define `platform_backend_board_name` in Task 4. Until then nothing calls it.)

- [ ] **Step 1: Write the failing tests.**
  `05_node_id.nsl` rewritten (keep its explanation of why the `nodeId`/`buildId` comparison matters; delete every sentence about `<host>-NNNN` and `gen_node_id.cmake`):
```
(* expect:
nodeId is the env value: true
nodeId is not the buildId: true
*)
```
  with `env: OEVM_NODE_ID=a1b2c3d4e5f6` as its own line INSIDE that header comment, after the expect lines. Body: `n:: platform nodeId. ('nodeId is the env value: ', (n = 'a1b2c3d4e5f6') printString) println.` plus the buildId comparison. (Header grammar: see `parse_test_metadata` in `run_tests_oevm.py`; `env:` lines end an expect block.)
  `06_node_id_unset.nsl`: header `expect-runtime-fail: nodeId` and `env: OEVM_NODE_ID=` (an EMPTY value — this is Review Focus 1, and it also defeats a developer shell that exports the variable); body `(platform nodeId) println.` Add a second file `07_node_id_toolong.nsl` with `env: OEVM_NODE_ID=<100 'x' characters>` and the same `expect-runtime-fail: nodeId` (Review Focus 2).
- [ ] **Step 2: Run to see them fail:** `cd nodus/nsl && python3 tools/run-one.py ../tests/23_mprof/05_node_id.nsl B` (expect a hostname-shaped answer → FAIL), `…/06_node_id_unset.nsl B` (expect "Expected runtime failure but ran successfully" → FAIL). Check `git status` for a clobbered `solidified_image.c`.
- [ ] **Step 3: Implement.**
  `platform_posix.c` (beside `platform_backend_boot_id`, same file, includes `<stdlib.h> <string.h>` already or add):
```c
size_t platform_backend_node_id(char *out, size_t cap) {
    const char *e = getenv("OEVM_NODE_ID");
    size_t n;
    if (e == NULL || e[0] == '\0') return 0;
    n = strlen(e);
    if (n + 1 > cap) return 0;                 /* never truncate an id into a collision */
    memcpy(out, e, n + 1);
    return n;
}
```
  Add a comment: this is the host's stand-in for silicon; `host_ota` runs several VMs per machine so an interface MAC or fixed value would give them one id (addendum point 4).
  `platform.c`: **delete** the whole `#ifdef __ZEPHYR__ extern const char oevm_node_id[]; … #define OEVM_NODE_ID oevm_host_node_id()` block including `oevm_host_node_id()` and, if nothing else uses it, `#include <time.h>` (check with `rg "time\(|clock_gettime" platform.c` first). Replace the `PRIM_pf_nodeId` case body:
```c
case PRIM_pf_nodeId: {
    char id[PLATFORM_NODE_ID_MAX];
    size_t n = platform_backend_node_id(id, sizeof id);
    if (n == 0) {
        static const char m[] = "VM: nodeId: this board has no factory link address\n";
        platform_console_write(m, sizeof m - 1);
        return 0;                    /* falls through to doesNotUnderstand: — the primitive FAILS */
    }
    *out = space_make_string_pub(id, n);   /* GC-safety: bytes copied into a C local first, one allocation */
    return 1;
}
```
  Update the `PRIM_pf_nodeId` comment in `prim_ids.h` (it says host answers "host" and describes the build-time define — rewrite to the new contract; it is the mainline description of the prim).
- [ ] **Step 4: Run to pass:** the three single-test commands. Expected `PASS` for 05, 06, 07.
- [ ] **Step 5 (falsify):** run `OEVM_NODE_ID=zz python3 tools/run-one.py ../tests/23_mprof/06_node_id_unset.nsl B` is NOT a falsification (per-test env overrides). Instead temporarily make `platform_backend_node_id` return `1` always: 06 must FAIL for "ran successfully". Revert.
- [ ] **Step 6:** host callers that must still work were audited: `nodus/oevm/tests/host_ota/run.sh` (`--env OEVM_NODE_ID=$NID-$n`), `nodus/oevm/tests/porta_sync_host.sh` (`OEVM_NODE_ID="$NODE"`), `docs/tutorials/03-porta.md` (`OEVM_NODE_ID=tut-node`) all set it. Confirm with `rg -n "OEVM_NODE_ID" nodus/oevm/tests docs/tutorials`; change nothing in them except comments that say it "overrides" a default (Task 7).
- [ ] **Step 7:** commit `feat(#118): host platform nodeId answers OEVM_NODE_ID or fails; hostname fallback deleted`.

---

### Task 3: Zephyr reads the factory address (ESP via hwinfo, nRF via FICR, RP fails)

**Files:** Modify `nodus/oevm/src/backend/zephyr/platform_zephyr.c`, `nodus/oevm/zephyr/prj_esp32.conf`, `prj_esp32s2.conf`, `prj_esp32c6.conf`.

**Verified in the tree:** `hwinfo_get_device_id` (`~/zephyr-esp32/zephyr/drivers/hwinfo/hwinfo_esp32.c`) returns the 6-byte eFuse MAC, MSB first, on every ESP32 family; on nRF it returns `DEVICEID` (the wrong register — do not use); on RP it returns the flash serial (not a link address — do not use). The Thread/other `prj_*` ESP confs overlay one of the three base confs (`CONF_FILE` in `run.sh` profiles), so three edits suffice. Existing guards use `CONFIG_SOC_FAMILY_ESPRESSIF_ESP32` (see `zephyr/src/main.c`); the nRF symbol is `CONFIG_SOC_SERIES_NRF52X` and the RP one is `CONFIG_SOC_FAMILY_RPI_PICO` (both read from existing `build-*/zephyr/.config` files on 2026-10-02). **"No id" is a per-family statement, never the `#else`:** an unknown family is a compile error, so a future board cannot inherit "has no address" by omission.

**Interfaces — Consumes:** `link_addr_hex` (Task 1), `platform_backend_node_id` declaration (Task 2). **Produces:** the zephyr definition of `platform_backend_node_id`.

- [ ] **Step 1 (test = compile gate, controller runs it in Task 8):** there is no host test for this file (`host-suite-blind-to-isr-seam`: no host gate compiles `zephyr/src` or `src/backend/zephyr`). The guard is the `#error` below plus Task 8's per-family builds and Task 10's board golden. State this in the commit body.
- [ ] **Step 2: Implement** at the end of `platform_zephyr.c` (sketch; verify include names against neighbours):

```c
#include "link_addr.h"
#if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)
#  ifndef CONFIG_HWINFO
#    error "node id: ESP32 ports read the eFuse MAC through hwinfo — set CONFIG_HWINFO=y in the prj_*.conf"
#  endif
#  include <zephyr/drivers/hwinfo.h>
static size_t link_addr_read(uint8_t b[6]) { return hwinfo_get_device_id(b, 6) == 6 ? 6 : 0; }
#elif defined(CONFIG_SOC_SERIES_NRF52X)
#  include <nrf.h>      /* FICR DEVICEADDR, NOT hwinfo: hwinfo answers DEVICEID there */
static size_t link_addr_read(uint8_t b[6]) {
    uint32_t lo = NRF_FICR->DEVICEADDR[0], hi = NRF_FICR->DEVICEADDR[1] & 0xFFFFu;
    b[0] = hi >> 8;  b[1] = hi;  b[2] = lo >> 24;  b[3] = lo >> 16;  b[4] = lo >> 8;  b[5] = lo;
    return 6;
}
#elif defined(CONFIG_SOC_FAMILY_RPI_PICO)
/* RP2040 / RP2350: no radio, so no link address (addendum point 3). Flash serial is NOT one. */
static size_t link_addr_read(uint8_t b[6]) { (void)b; return 0; }
#else
#  error "node id: a new board port must say where its factory link address is, or that it has none"
#endif
size_t platform_backend_node_id(char *out, size_t cap) {
    uint8_t b[6];
    size_t n = link_addr_read(b);
    return n == 0 ? 0 : link_addr_hex(b, n, out, cap);
}
```
  Add `CONFIG_HWINFO=y` (with a two-line comment: "rete#118: node id = eFuse base MAC via hwinfo") to the three ESP base confs. Do NOT add it to nRF or RP confs.
- [ ] **Step 3:** `rg -n "hwinfo" nodus/oevm/zephyr/prj*.conf` must show exactly the three; commit `feat(#118): zephyr node id — eFuse MAC via hwinfo, nRF FICR DEVICEADDR, RP fails`.

---

### Task 4: `platform boardName` — primitive 159 and the host seam

**Files:** Modify `nodus/oevm/src/{prim_names.def,prim_ids.h,dispatch.c,platform.c,platform_backend.h}`, `src/backend/posix/platform_posix.c`, `src/backend/zephyr/platform_zephyr.c`, `nodus/nsl/lib/kernel.nsl`; Create `nodus/tests/22_spacesync/25_board_name.nsl` and `26_board_name_unset.nsl`.

**Every list a new primitive touches** (from `git show a3eed7c` #243's `manifestPin` and `git show 00c1470` #273's `appLiveness:`; confirmed by `rg -l appLiveness`): (1) `src/prim_names.def` — append `PRIM_NAME("boardName")` (the python bakers `nodus/nsl/tools/prim_ids.py`, `solidify.py`, `oimg_serialize.py` and `src/kernel_boot.c` read this same file — no edit there); (2) `src/dispatch.c` — append `PRIM_pf_boardName` to the enum after `PRIM_pf_appLiveness` AND advance `prim_names_enum_parity_check` to `PRIM_pf_boardName`; (3) `src/prim_ids.h` — `#define PRIM_pf_boardName 159` with the comment style of its neighbours; (4) `src/platform.c` — the `case`; (5) `kernel.nsl` — the accessor. **Count, do not add:** `grep -c '^PRIM_NAME' nodus/oevm/src/prim_names.def` is 158 today; it must read 159 after. `BUILTIN_REGISTRY_VERSION` / `builtins_registry.py` are the *class* registry, unrelated — do not bump.

- [ ] **Step 1: Write the failing test** `25_board_name.nsl` (header: `expect:` three lines + `env: OEVM_BOARD_NAME=bench-c6`; the unset case is Task 6's test):
```
(* expect:
board is the env value: true
board unset is nil: true
*)
```
  Two runs of one file cannot differ in env, so make this file the SET case only (`board is the env value: true`) and put the unset case in `26_board_name_unset.nsl` (`env: OEVM_BOARD_NAME=` → `platform boardName == nil` prints `true`; use `== nil`, the prim answers a raw nil/leaf — see `23_build_man.nsl`'s note). Body: `platform boardName`.
- [ ] **Step 2:** run both with `run-one.py … B`; expected FAIL (selector not understood).
- [ ] **Step 3: Implement.** posix backend: `platform_backend_board_name` = same body as `platform_backend_node_id` but `getenv("OEVM_BOARD_NAME")`. zephyr backend: 
```c
extern const char oevm_board_name[];     /* generated by cmake/gen_board_name.cmake (Task 5) */
size_t platform_backend_board_name(char *out, size_t cap) {
    size_t n = strlen(oevm_board_name);
    if (n == 0 || n + 1 > cap) return 0;
    memcpy(out, oevm_board_name, n + 1);
    return n;
}
```
  `platform.c` case (after `PRIM_pf_nodeId`):
```c
case PRIM_pf_boardName: {          /* rete#118: the build-time board name; nil = "this build was not told" */
    char nm[PLATFORM_NODE_ID_MAX];
    size_t n = platform_backend_board_name(nm, sizeof nm);
    *out = (n == 0) ? NULL : space_make_string_pub(nm, n);
    return 1;
}
```
  `kernel.nsl`: next to `nodeId`/`manifestPin`: `public boardName = ( ^ platform boardName )` with a comment (answers a String or nil; nil means obs/build omits `board`).
- [ ] **Step 4:** both tests PASS; also re-run `05`, `06`, `23_build_man.nsl` (`run-one.py … B`) — unchanged. `grep -c '^PRIM_NAME' prim_names.def` → 159. Compile the C unit tests that include the parity guard: `cd nodus/oevm && gcc -std=c99 -Isrc tests/test_symbol_prim_id.c <ENG_A files from run_tests_oevm.py> -lm` is easier done by the controller's suite in Task 9; subagent runs `python3 -c "import sys; sys.path.insert(0,'nodus/nsl/tools'); import prim_ids; print(len(prim_ids.PRIM_IDS), prim_ids.PRIM_IDS['boardName'])"` → `159 159`.
- [ ] **Step 5 (falsify):** advance only `prim_names.def` and not the enum: the parity typedef must fail to compile (any `gcc -Isrc -c src/dispatch.c`). Revert.
- [ ] **Step 6:** commit `feat(#118): platform boardName (prim 159) — build-time board name, nil when not told`.

---

### Task 5: Zephyr board-name plumbing; `run.sh` loses the id, keeps the name; probe node

**Files:** `git mv nodus/oevm/zephyr/cmake/gen_node_id.cmake nodus/oevm/zephyr/cmake/gen_board_name.cmake` and rewrite; Modify `nodus/oevm/zephyr/CMakeLists.txt`, `nodus/oevm/zephyr/run.sh`; Create `nodus/oevm/zephyr/NodeIdProbe.nsl`.

**Rules:** one variable, `OEVM_BOARD_NAME` (env, per the #281 sysbuild rule — the same env route `OEVM_BLOB` uses; `-D` is dropped under sysbuild). **No fallback chain**: delete the tailscale → hostname → `unnamed` logic entirely. Unset → bake an empty string → the primitive answers nil → `obs/build` has no `board`. The name is a file name from the bench, so write it into C through CMake's `file(WRITE …)` after rejecting anything outside `[A-Za-z0-9._-]` with `message(FATAL_ERROR …)` (Review Focus 4).

- [ ] **Step 1: Write the failing check** (shell, the test of this task): `rg -n "OEVM_NODE_ID|oevm_node_id|NODE_ID_ARG|gen_node_id" nodus/oevm/zephyr` currently lists the sites; after this task it must print nothing. Also create `NodeIdProbe.nsl` (modelled on `TimerNode.nsl`'s `packageUsing: manifest` / `main: platform args:` shape; no capabilities):
```
class NodeIdProbe packageUsing: manifest = () (
  public main: platform args: args = (
    ('board ' , platform boardName printString) println.
    ('node ' , platform nodeId) println.
  )
) : ()
```
  (board first, so an RP board that fails on `nodeId` still shows its board line.) Verify it compiles on host: `cd nodus/nsl && OEVM_NODE_ID=ab12 OEVM_BOARD_NAME=probe python3 tools/ns_compile_oevm.py ../oevm/zephyr/NodeIdProbe.nsl /tmp/probe.oimg --format=oimg --layout=/tmp/oevm_layout.json` (mirror the invocation in `docs/tutorials/03-porta.md`; the layout JSON comes from a prior suite run — if absent leave this check to the controller).
- [ ] **Step 2: Implement.**
  `gen_board_name.cmake`: header comment rewritten (board name, build-time, configure-time, why no default: a guessed board name is the #186 failure with a friendlier spelling); body:
```cmake
if(NOT BOARD_NAME)
  set(BOARD_NAME "")
endif()
if(NOT BOARD_NAME STREQUAL "" AND NOT BOARD_NAME MATCHES "^[A-Za-z0-9._-]+$")
  message(FATAL_ERROR "OEVM_BOARD_NAME '${BOARD_NAME}': only [A-Za-z0-9._-]")
endif()
file(WRITE ${OUT_C}
  "/* generated by cmake/gen_board_name.cmake — do not edit, do not commit */\n"
  "const char oevm_board_name[] = \"${BOARD_NAME}\";\n")
```
  `CMakeLists.txt`: rename the `OEVM_NODE_ID` env/`execute_process` block to `OEVM_BOARD_NAME` / `OEVM_BOARD_NAME_C` / `oevm_board_name.c` / `message(STATUS "oevm board name: …")`; rewrite the two surrounding comments (the #281 sysbuild paragraph keeps its lesson, names `OEVM_BOARD_NAME`).
  `run.sh` (read the whole region first; there are four sites): (a) replace the `NODE_ID_ARG` block with `BOARD_NAME_ARG` using the same two doors — explicit `OEVM_BOARD_NAME` env wins (announced on stderr), else `bench::board_for_port "$CONSOLE_PORT"`, else a `[board] WARNING: no collector env names $CONSOLE_PORT — this image publishes NO board name in obs/build` (delete the tailscale paragraph); (b) the `export` after the sysbuild paragraph exports `OEVM_BOARD_NAME` only when resolved (and update that paragraph's list: "OEVM_MODEL, OEVM_BLOB and OEVM_BOARD_NAME"); (c) the post-build assertion greps `oevm_board_name.c` for `"$WANT"` and on mismatch prints `[build] FATAL: board name '…' is not in the generated oevm_board_name.c.` + `exit 3`; (d) fix the comment in the `rpi_pico2` profile ("the node id then falls back with the warning below") and grep for any other `node id` comment (`rg -n "node id|nodeId" run.sh`). Keep the stderr-not-stdout rule noted in the old comment.
- [ ] **Step 3:** the Step 1 `rg` prints nothing; `bash -n nodus/oevm/zephyr/run.sh` clean.
- [ ] **Step 4:** commit `refactor(#118): zephyr board name replaces the node id plumbing; fallback chain deleted`. Real builds happen in Task 8 (controller).

---

### Task 6: `obs/build.board` — publisher, manifest, descriptor, goldens, catalogue

**Files:** Modify `nodus/nsl/lib/kernel.nsl` (`publishBuild:`), `protocol/MANIFEST.cddl`, `protocol/descriptors/records.yaml`, `protocol/PROTOCOL-NSL-RECORDS.md`, `nodus/nsl/tools/fixtures/manifest_goldens.nsl`; Create `nodus/tests/22_spacesync/27_build_board.nsl` and `28_build_board_absent.nsl`.

**Key position, verified:** encoded keys are `d827` + text head + bytes, ordered bytewise: head `0x63` (3 chars: `man`, `sdk`) < `0x64` (`chip`) < `0x65` (5 chars: `board` < `build` < `image`, since `o` < `u` < `i`… compare `b-o` vs `b-u`: `o`(0x6f) < `u`(0x75); `image` starts `i`(0x69) > `b`). Wire order is therefore **man, sdk, chip, board, build, image** — `board` sits between `chip` and `build`, matching the addendum. Re-verify with the golden (below), not by eye.

- [ ] **Step 1: Write the failing tests.**
  (a) `27_build_board.nsl`: build a host bridge as `24_liveness_app_cad.nsl` does (`platform PortaBridge socket: nil space: platform space id: 'test' peer: '::1' port: 1 spans: platform Spans new`), call `b publishBuild: #host`, read the row back as `01_seq_and_scan.nsl` does (`space changedSince: 0 max: 8` → rows `path seq value`; take the `obs/build` row's value, a Record) and print `board present: <bool>` and, if present, the value. Header `env: OEVM_BOARD_NAME=bench-c6` → `expect: board present: true / board value: bench-c6`. Add `28_build_board_absent.nsl` with `env: OEVM_BOARD_NAME=` → `board present: false` (Review Focus 3: absent, not empty).
  (b) `manifest_goldens.nsl`: add a third `obs/build` emission (`#chip -> #esp32c6, #board -> 'bench-c6', #build -> 'host-test'`, via the same `OC`/`M fromPairs:` idiom as the 5-field one) and ALSO add `oc add: #board -> 'bench-c6'.` to the existing 5-field emission so the full-record golden covers all six keys. Re-pin the `expect:` block by running it once (`python3 tools/run-one.py tools/fixtures/manifest_goldens.nsl B`, copy its output lines into the header) and inspect the new GOLDEN hex: the key bytes must appear in the order `63 'man'`, `63 'sdk'`, `64 'chip'`, `65 'board'`, `65 'build'`, `65 'image'`.
- [ ] **Step 2: run to see failures:** 27/28 fail (no `board` key published); the golden gate fails until the CDDL has `board`: `cd nodus/nsl && python3 -m pytest tools/test_manifest_gate.py -x -q` (RED: zcbor rejects the unknown key).
- [ ] **Step 3: Implement.**
  `kernel.nsl` `publishBuild:` — replace the two-way `pin == nil ifTrue/ifFalse` with one build-up (the 2×2 branch is the "added beside, not instead of" smell). Smallest form that exists in the kernel: look at how `Record` is constructed elsewhere in `kernel.nsl` (`rg -n "Record tag:.*fields:" nodus/nsl/lib/kernel.nsl`); if only `tag:with:` forms exist, build the pair list with the `OrderedCollection`+`fromPairs:` idiom used in `manifest_goldens.nsl` (key order is the encoder's job, not the caller's). Fields: `chip`, `build` always; `man` iff `outer manifestPin ~= nil` (`== nil` test, raw leaf); `board` iff `outer boardName ~= nil`. Update the comment above `publishBuild:` (#259/#243 text) to mention `board`.
  `MANIFEST.cddl`: add `sym-board = #6.39("board")` to the key list (alphabetical, between `sym-bootId`… check: `sym-bid`, `sym-board`, `sym-bootId`) and in `build`:
```
  ? sym-man   => bstr,
  ? sym-sdk   => tstr,
    sym-chip  => anysym,
  ? sym-board => tstr,         ; build-time board name (#118); absent when the build was not told
    sym-build => tstr,
  ? sym-image => bstr,
```
  `records.yaml` `build:` — add the `board` field between `chip` and `build` (key `board`, name `board`, unit `text`, kind `identity`, meaning "the bench board the image was built for (the collector env file name); the node asserts it on every boot", validity "Optional: absent when the build was not told a board name. A name, not a credential. `refs: [rete#118]`"); also fix the stale summary sentence "A node id is a board since rete#186" → "A node id is the board's factory link address since rete#118; a space still carries many builds …".
  `PROTOCOL-NSL-RECORDS.md`: the catalogue row (`| obs/build | R | …`) → `Build { man?: bytes, sdk?: str, chip: sym, board?: str, build: str, image?: bytes }` (this also fixes the row omitting `man`); add a `board` bullet after the `chip` bullet in the prose list ("**Omitted when the build was told no board name**"); note in the "82 B encoded" paragraph that the figure is for a record without `board`/`man`.
- [ ] **Step 4: pass, in the manifest memory's order:** `python3 protocol/tools/manifest_lint.py protocol/MANIFEST.cddl` (path per its usage) → clean; `cd nodus/nsl && python3 -m pytest tools/test_manifest_gate.py -q` → green; `cd porta && go test ./internal/manifest/ ./internal/gate/ ./internal/space/ ./internal/portacli/` → green (`TestDescriptorsCoverManifest` fails if `records.yaml` order/fields disagree with the CDDL); 27/28 and `22_spacesync/23_build_man.nsl` via `run-one.py … B`.
- [ ] **Step 5 (falsify):** move `? sym-board` after `sym-build` in the CDDL: `test_manifest_gate.py` must FAIL on the six-key golden (correct wire bytes rejected). Revert. Then delete the `board` row from `records.yaml`: `TestDescriptorsCoverManifest` must FAIL. Revert.
- [ ] **Step 6:** `git status` — if `solidified_image.c` is modified, checkout. Commit `feat(#118): obs/build gains optional board; manifest, descriptor, goldens, catalogue`. NOTE: this moves the manifest SHA-256 — record the new value in Task 12.

---

### Task 7: Docs sweep

**Files (mainline only):** `docs/tutorials/02-board.md`, `docs/tutorials/03-porta.md`, `nodus/oevm/zephyr/docs/BOARD_PORTS.md`, `porta/docs/GETTING-STARTED.md`, comments in `nodus/oevm/zephyr/PortaBridgeNode.nsl`, `PortaBridgeNodeNrf.nsl`, `OtaBridgeNode.nsl.tmpl`, `nodus/oevm/tests/host_ota/run.sh`, `nodus/oevm/tests/porta_sync_host.sh`, `nodus/oevm/tests/target/bridge-health.env`, `bridge-forensics.env`, `nodus/oevm/zephyr/tools/console/README.md` (line citing `nodeId = 'c6-bench'`), `docs/status/dev-status.html` (the stale "build-time define" sentence — controller refreshes the page in Task 12; here only the sentence), `nodus/tests/23_mprof/05_node_id.nsl` (done in Task 2).

**What to say:** `platform nodeId` is the factory link address, 12 lowercase hex chars on ESP and nRF boards; RP boards have none and the primitive fails; the HUMAN name is `goal/name`. `OEVM_NODE_ID` survives **only as the host VM's stand-in for silicon** (host gates and the host tutorial set it; unset → fails). `OEVM_BOARD_NAME` is the build-time board name (run.sh resolves it from the collector env file name, or takes it explicitly), published at `obs/build.board`. The two `*.env` precondition comments that say `porta put <board> goal/name <board>` become `<id>` — "the id the node prints in its 'no goal/name arrived' line, or read from the sticker" — because the porta space is now the hex address, not the bench name. Leave `porta/docs/CLI.md` examples (illustrative node names; its gate checks command names only).

- [ ] **Step 1: the end-state check, written first (it fails now):**
```
rg -n "OEVM_NODE_ID|oevm_node_id|gen_node_id|oevm_host_node_id|NODE_ID_ARG|<hostname>-NNNN|<host>-NNNN" ~/workspace/rete --glob '!review/**'
```
  **Expected remaining hits and nothing else:** (1) `nodus/oevm/src/backend/posix/platform_posix.c` (`OEVM_NODE_ID` getenv, host stand-in) and its header comment; (2) `nodus/tests/23_mprof/05_node_id.nsl`, `06_node_id_unset.nsl`, `07_node_id_toolong.nsl` (`env: OEVM_NODE_ID=`); (3) `nodus/oevm/tests/host_ota/run.sh` and `nodus/oevm/tests/porta_sync_host.sh` (the env they set, and comments describing it); (4) `docs/tutorials/03-porta.md` (the host env knob, reworded); (5) `nodus/oevm/src/prim_ids.h` (comment naming the host stand-in); (6) dated records: `docs/design/**`, `docs/results/**`, `docs/status/focus-history.md`, `docs/superpowers/plans/**` (including this plan). Any other hit is a miss.
  Also `rg -n "OEVM_NODE_ID" ~/workspace/rete/review` → currently **no hits** (counted `rg -c`); if that changes, LIST them for David and do not edit.
- [ ] **Step 2:** edit until Step 1's output matches the list. `02-board.md` §5/§7 narrative about "`OEVM_NODE_ID`, the one door" is rewritten to `OEVM_BOARD_NAME` for the board name and drops the claim that the id "takes its space name from the machine that built it" (false after this change); keep the #181/#186 history as history.
- [ ] **Step 3:** `bash tools/docmap.sh` (regenerates `review/DOCMAP.md` — it is a generated file the project says to regenerate; if it dirties `review/`, report that to David rather than committing it).
- [ ] **Step 4:** commit `docs(#118): node id and board name — the sweep`.

---

### Task 8 (controller): compile check, one build per board family

No host gate compiles `src/backend/zephyr/*.c`; this is the only pre-board proof. Model B only, local, no flash. Per board (7): 
```
cd ~/workspace/rete/nodus/oevm && OEVM_BOARD_NAME=probe-<board> bash zephyr/run.sh --board <board> --node NodeIdProbe.nsl --pristine --build-dir build-nodeid-<board>
```
for `<board>` in `esp32 esp32s2 esp32c6 nrf52840dk nrf52840dongle rpi_pico2 feather_rp2040`. Expected per build: exit 0 and the line `[build] board name probe-<board> confirmed in the generated source`; for the three ESP boards additionally `grep -x CONFIG_HWINFO=y zephyr/build-nodeid-<board>/zephyr/.config`. Also one build with **no** `OEVM_BOARD_NAME` and no resolvable port: expected the `[board] WARNING … NO board name` line and `oevm_board_name.c` containing `""`. Any `#error` or undefined `NRF_FICR`/`hwinfo_get_device_id` is a Task 3 defect: fix in a follow-up commit, re-run that board.
- [ ] **Falsify** (each reverted after): (0) rename the `CONFIG_SOC_FAMILY_RPI_PICO` guard to a symbol that does not exist → the `rpi_pico2` build must stop at the unknown-family `#error`; (1) delete `CONFIG_HWINFO=y` from `prj_esp32c6.conf` → the C6 build must stop at the `#error`; (2) append an `x` in `gen_board_name.cmake`'s written string → run.sh must `exit 3` with the FATAL line; (3) export `OEVM_NODE_ID=c6bench` only → build must still print the no-board-name WARNING (the stale variable does nothing, loudly).

### Task 9 (controller): gates, one at a time

In this order, each to completion before the next, none concurrent:
1. `cd ~/workspace/rete/nodus/nsl && python3 -u tools/run_tests_oevm.py > /tmp/rete118-suite.log 2>&1` (background; no `timeout`, no `tail`). Read **both** the `Passed: N / Failed: 0` line (N was 281 at 2026-10-01; this plan adds 06, 07, 25/26 or 27/28 and a C unit test — expect more, "a mismatch is staleness in CLAUDE.md, never a missing test") **and the aux line**: `c-units` (now includes `test_link_addr`), `manifest-py`/`nsl-tools-py`, `tutorial-host`, `tutorial-porta`, `tutorial-board`, `porta-cli`, `footprint`. Any SKIP or red aux entry is a finding.
2. `cd ~/workspace/rete/porta && go test ./...`
3. `cd ~/workspace/rete/nodus/oevm && bash tests/porta_sync_host.sh` → `PORTA SYNC OK`
4. `bash nodus/oevm/tests/host_ota/run.sh` (standalone, ~4 min, ports 26969+1) — it sets `OEVM_NODE_ID` per VM; green proves the host stand-in still gives distinct ids.
5. Check `docs/tutorials/03-porta.md`'s printed `sha256 96e29343…` line is not asserted by the gate (it was stale before this change); if the gate compares it, update it to the new manifest hash.
- [ ] `git status`: `solidified_image.c` must be unmodified.

### Task 10 (controller + David): board proof

Per family, flash `NodeIdProbe.nsl` and read the console (collector env files resolve boards; never a remembered table; the bench inventory in `FOCUS.md` is unverified — enumerate first):
| family | board | expected console | golden source |
|---|---|---|---|
| ESP32 C6 | `c6spare` (disposable; JTAG node here, CP2102N absent — flash via the node that can) | `board "c6spare"` then `node <12 hex>` | the sticker (WiFi MAC) |
| nRF | `nrf4ec0` or `nrf92c2` dongle / DK | `node <12 hex>` | sticker value; **this is also the check that `DEVICEADDR` in this byte order is what the sticker prints** |
| RP2350 | RP2350-Zero on this desktop (USB-CDC; BOOT+RUN = BOOTSEL) | `board …` then `VM: nodeId: this board has no factory link address` then `VM Error: Message 'nodeId' not understood …` | the primitive FAILING is the proof |
ESP32 (WROOM) and S2 (MagTag) get the same read if a board is present; absent boards are listed as unproven, not skipped silently. Record each captured id beside its sticker in `docs/results/2026-10-02-118-node-id-board-proof.md` (new results file, dated, with the raw console excerpts).
- [ ] **Falsify each:** (a) compare the C6 golden against a *different* board's sticker — must mismatch (proves the comparison can fail); (b) reflash the same board twice and show the id is identical across a full erase (`esptool erase-flash` / `nrfutil` recover) — the property that distinguishes it from the OpenThread extended address; (c) the RP2350 result must also be shown with a node that does NOT read `nodeId` booting normally (e.g. the #133 gate image), so "fails" is not "board is dead".
- [ ] **STOP and ask David (yes/no) if the nRF sticker differs from the raw `DEVICEADDR` bytes** — the BLE controller forces the top two bits (`|0xC0`) of the static address (`hci_vendor.c`), so a sticker that shows them set means the formula needs `b[0] |= 0xC0`. That is a one-line change to `link_addr_read` and a re-golden, but it is the addendum's explicit hand-check and not the implementer's call.

### Task 11 (DAVID PRESENT): deploy — every step is physical or an outage

Mark each of these as requiring David; none is run by an agent unprompted.
- [ ] **Before anything:** check the dev-status page, the soak dashboard (:8085/:54361) and collector for consumers that key a porta space by bench name (`c6bench`, …) — `ssh gw85224-01` read-only `rg` of `~/` tooling. Those views will show the old spaces as historical and the new hex spaces as new nodes; confirm David accepts that.
- [ ] **Gateway restart with the new manifest** (OUTAGE, ~35 s last time): the **three-call rule** from `porta-gateway-operational-state` — ssh call 1 = find the PID and `kill <pid>` and nothing else; call 2 = verify it is down; call 3 = install the new binary if porta changed (porta has NO code change here, only its manifest file), write the restart to a file and `setsid nohup ./porta serve --db <read /proc/<pid>/cmdline first> --manifest <new protocol/MANIFEST.cddl>` from that file. Verify `porta: manifest gate ON (… sha256 <new hash>…)` in the log. The manifest persists in the store: confirm with `porta get` against the stored copy.
- [ ] **Reflash every node** (`c6bench`, `c6spare`, `nrf4ec0`, `nrf92c2`, `7adf`, the bell), each with `OEVM_BOARD_NAME` resolved by `run.sh` from its collector env. **The bell (`ShipsBellAlarmNodeC6`) is an actuator: use `zephyr/flash-bell.sh`, in daylight, saying out loud that it will ring for the whole flash** (`flashing-an-actuator-node`). Order: disposable boards first.
- [ ] After each flash: `porta get <hexid> obs/build` shows `board`, `man` = the new pin, and `chip`; `gw/liveness` `#live`; `porta put <hexid> goal/name <role>` (the precondition the `bridge-health`/`bridge-forensics` scenarios print). The old spaces stay in the db and are historical from this date (same treatment as #186's 2026-08-22 cut); analysis that crosses the date reads both.

### Task 12 (controller): close-out and promotion

- [ ] **Promote** (the issue is not closed until this lands): `porta/docs/ARCHITECTURE.md` (identity section: id = factory link address, hex format, RP has none, board name at `obs/build.board`, host env stand-in) and `protocol/PROTOCOL-NSL.md` (the node-id field's definition; replace the "Manifest sha256 `844ba659b3a9a3b0…`" mention in the mainline text with the new value and the #118 line — do NOT edit `docs/design/2026-09-14-liveness-two-cadences-design.md`, a dated record). `nodus/oevm/zephyr/docs/BOARD_PORTS.md` is done in Task 7. Append a one-paragraph close-out to the design record's addendum naming what absorbed it (the only edit to a dated record, as the decision's own closing note).
- [ ] **Status:** rewrite `FOCUS.md` (<15 lines; next item per the burn list), refresh `docs/status/dev-status.html` and scp it (recipe in `.claude/commands/conclude.md`), run `/conclude`.
- [ ] **Issues:** comments (≤5 lines each) on #118 and #242 with the proof file path, the commit range and the new manifest hash, using `refs`/never a closing keyword; close #118/#242 by hand only after verifying the comment landed. File **found-during #118** P3 items for anything surfaced (candidate: a collector/dashboard consumer keyed on board-name spaces). #305 (id ingest gate) stays open, now with the shape fixed: 12 lowercase hex.
- [ ] `git push -u origin HEAD` at `/conclude`; never force; never push `master`.

---

## Self-review

- **Spec coverage:** decision 1 → Tasks 2–3; format (addendum 1) → Task 1; read mechanism (2) → Task 3; RP fails (3) → Task 3/10; host (4) → Task 2; board name → Tasks 4–6; removal list → Tasks 3, 5, 7; golden per family → Task 10; manifest/descriptors → Task 6; gateway/reflash → Task 11; promotion → Task 12; #305 and `OtaBridgeNode.nsl.tmpl` left alone (it gains no `board`; only its comment changes).
- **Naming consistency:** `link_addr_hex`, `platform_backend_node_id`, `platform_backend_board_name`, `PLATFORM_NODE_ID_MAX`, `PRIM_pf_boardName`/`boardName` (159), `OEVM_BOARD_NAME`, `oevm_board_name`, `gen_board_name.cmake`, `NodeIdProbe.nsl` are used identically in every task.
- **Known sequencing hazard:** Zephyr builds are broken between Task 2 and Task 3 (platform.c calls a seam the zephyr backend has not defined yet) and the host gates must not be run mid-way; the controller builds only after Task 5.

# Host arena floor — the smallest `OEVM_HEAP_SIZE` that boots, and why 131072 did not

`tools/host-arena-floor.sh` bisects the smallest `OEVM_HEAP_SIZE` that **boots** the host
image VM. It is the floor counterpart to `tools/heap-ceiling.sh`, which bisects the largest
that **links** on a board (`docs/results/2026-07-20-c6-heap-ceiling.md`). Ceiling is a
link-time property and needs no hardware; floor is a run-time property and so the script
actually boots what it builds.

Closes rete#179, whose title asserted that *one boot-path allocation asks for the whole
arena, so no host run reproduces device memory conditions.* Both halves are wrong.

## The allocation is the VM stack

`gdb`, breakpoint on `default_oom`, running the reproduction in #179's body — the host VM
built `-DOEVM_HEAP_SIZE=131072` and handed `01_addition.oimg`:

```
#0  default_oom (size=131096) at src/gc.c:162
#1  gc_allocate (vtable=Vtable_StackVtable, size=131096) at src/gc.c:321
#2  stack_alloc (capacity=16384) at src/dispatch.c:59
#3  run_interpreter (...) at src/vm.c:713
#4  send / oimg_bootstrap / main
```

It is the **VM execution stack**: `vm.c:713` allocates `g_active_stack` once, as a
fixed-capacity heap `Stack` of `VM_STACK_INIT_WORDS` slots, and never grows or moves it
(the deliberate design — raw frame pointers stay valid for the life of the run, `vm.c:54-59`).
The default is **16384 words**, and on a 64-bit host a word is 8 B:

```
16384 x 8 B + 24 B header = 131096
```

`131072` is exactly `16384 x 8`. **The allocation does not scale with the arena.** It
happens to equal the C6's arena because the C6's arena is 128 KiB and the host's default
stack is 128 KiB, and that coincidence is the entire content of #179's "asks for the whole
arena". Nothing on the boot path reads `OEVM_HEAP_SIZE`; the `+24` that read as a header on
an arena-sized indexable is the header on the stack.

The device never meets this, because `zephyr/run.sh` exports `VM_STACK_INIT_WORDS=4096` for
every board and its word is 4 B. **The C6 spends 16 408 B of its 131 072 arena on the stack
— 12.5%. The 64-bit host default spends 100%.**

The knob was already there and already documented at the point of definition (`vm.c:59`:
*"-D lower on embedded to shrink the fixed footprint"*). What was missing was that a *host*
build ever wants it.

## The floor is a formula

Bisected to 8 B. Boot = compile, run `01_addition.oimg`, print `7`, exit 0.

| build  | `VM_STACK_INIT_WORDS` | stack bytes | smallest booting `OEVM_HEAP_SIZE` | residual |
|--------|-----------------------|-------------|-----------------------------------|----------|
| 64-bit | 16384 (host default)  | 131 096     | **134 127**                       | 3 031    |
| 64-bit | 4096 (device value)   | 32 792      | **35 821**                        | 3 029    |
| `-m32` | 16384 (host default)  | 65 552      | **67 075**                        | 1 523    |
| `-m32` | 4096 (device value)   | 16 400      | **17 918**                        | 1 518    |

```
floor  ~=  VM_STACK_INIT_WORDS x oop_size  +  header  +  ~1520 B x (oop_size / 4)
```

The residual is 1 520 B at `oop_size` 4 and 3 030 B at 8 — it **doubles with the word**, so
the whole boot footprint is slot-dominated and there is no fixed-size boot allocation hiding
inside it. That is a stronger statement than the table: it says the model is complete.

This also explains #179's *"smallest size observed to boot: 262144. 131072 fails."* Both
probes were powers of two. The real floor at the host default is **134 127** — between them,
and 2.3% above the value that failed.

**Caveat on what the floor measures.** `01_addition.nsl` is the smallest thing that exercises
the boot path; a real node needs headroom for its own live set on top of the floor. The floor
is the point below which the VM cannot start, not a size at which anything useful runs.

## A host run does reproduce device memory conditions — at `-m32`

```
gcc -m32 -DOEVM_HEAP_SIZE=131072 -DVM_STACK_INIT_WORDS=4096 \
         -DOEVM_MARK_STACK_SIZE=512 -DMETHOD_CACHE_SIZE=256 -DPRIM_CACHE_SIZE=256 ...
$ ./vm32 tiny32.oimg
7
```

Not an approximation. `oop_size` is 4, the arena is the same 131 072 bytes, the stack is the
same 16 400, and every object has the same slot width — byte-for-byte and object-for-object
what the C6 runs. A 64-bit host at the same arena is not merely inexact, it cannot boot.

**The recipe already existed.** `tests/test_portability_32.sh` builds a 32-bit image VM on
every suite run (the `portability-32` aux gate), and `zephyr/docs/BOARD_PORTS.md` documents
the sequence: build `dump_layout` `-m32` -> layout with `oop_size: 4` -> recompile the
`.oimg` against **that** layout -> build the image VM `-m32`. The `.oimg` recompile is not
optional; a 64-bit image will not load into a 32-bit VM. All that was missing were the two
`-D`s, and the knowledge that they *are* `-D`s.

## Both knobs are compile-time macros. This is the expensive half.

`gc.c:18-19` is an `#ifndef` with a 16 MB host default and is **never `getenv`'d**:

```c
#ifndef OEVM_HEAP_SIZE
#define OEVM_HEAP_SIZE (16u * 1024u * 1024u)   /* host default; -D override for embedded */
```

Setting `OEVM_HEAP_SIZE` in the environment therefore does nothing, silently. A run that
does so measures the 16 MB default while reporting itself as a measurement of the device —
which is exactly what invalidated rete#199's first host reproduction and forced a retraction.

Set against that, #179's own loud failure (an abort at boot, with the requested size in the
message) cost minutes. **The failure mode this issue described was the cheap one; the failure
mode it caused was the expensive one.** Both trace to one missing sentence, which is why that
sentence is now here rather than in a comment on an issue.

## Nothing in the VM needs to change

The default is right for a host that wants a deep stack; the knob is right for a host that
wants a small arena. Lowering the host default would trade a documented knob for a new one.

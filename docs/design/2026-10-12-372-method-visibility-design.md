# Method visibility at no cost per send (rete#372)

> Status: **DRAFT, answered by David 2026-10-12:** visibility is ultimately mandatory (#37 Q2) and is built now, ahead
> of the network reflash; three levels with `protected` the default, which follows Newspeak's intent (Q1–Q2); the
> private send uncached until the board says otherwise (Q3); no compile-time rule, because a `doesNotUnderstand:`
> handler can make any send legal (Q4); an ordinary send that finds `protected` answers DNU at once, as Soup does (Q5).

## Where it stands

Dialect row 25 (`nodus/nsl/docs/NEWSPEAK_DIALECT.md`): the grammar accepts `public`/`protected`/`private` on methods,
slots and classes. Only slots act on it: a `public` or `protected` slot gets accessors, while a `private` or unmarked
slot gets none. A method's keyword is parsed and dropped. Today's L1 (Opt #12) deleted the dead run-time check and
the `access` fields along with it. So a reference to an object reaches every method it has, and a private method
that answers a private slot hands that slot to anyone. `private` in the source is a claim nothing keeps.

## What Newspeak does (Primordial Soup, `~/workspace/primordialsoup/vm/interpreter.cc`)

| send kind | finds `public` | finds `protected` | finds `private` |
|---|---|---|---|
| ordinary (explicit receiver, `perform:`) | yes | DNU | skipped, so the lookup goes on up |
| `self`, implicit, `outer`, `super` | yes, virtual | yes, virtual | only in the **sender's own class** (lexical, not virtual) |

An unmarked member is `protected` (`newspeak/MirrorTesting.ns:412`). Soup's cache for self-ish sends keys on the
sending method as well, because the private rule depends on where the send sits.

## The data (a corpus spike, 372 files, parse-only, `vis_spike.py` in the session scratchpad)

- Definitions: 737 `public` and 473 unmarked methods, **88 `private`, 4 `protected`**; 315 public slots and 11
  protected slots.
- Every send to a selector that is only ever private is a self send (151), an implicit send (61) or an outer send (1).
  **No explicit send reaches a private method** (the 14 `n:` hits are a class factory, which is a false positive). So
  enforcing `private` breaks nothing that exists.
- `protected` has 15 uses. Nine are implicit sends; the 14 explicit sends are the same false positive.
- 15 selectors are private in one class and public or unmarked in another (`indexOf:`, `bar`, `release:`…).
- With Newspeak's default (unmarked = protected), **130 explicit sends** across 51 selectors would break: 92 in
  `nsl/lib`, mostly `setX:` initialisers sent to a fresh instance (`setPairs:values:`, `setRoot:`, `setPort:`…).

## The decision (David, 2026-10-12)

**Three levels, as Newspeak has them, and unmarked = `protected` for every member.** That follows the language's
intent instead of departing from it, and gives one default for every member. An unmarked slot gets accessors only
the object itself can reach, so capabilities stay closed. An unmarked method stays virtual, so overriding keeps
working. The price is call-site churn: the 130 explicit sends above get marked `public`. An earlier draft of this
record dropped `protected` to save a mechanism. That left two defaults in conflict (unmarked methods public,
unmarked slots private), which is the worse trade.

| send kind | reaches `public` | reaches `protected` | reaches `private` |
|---|---|---|---|
| ordinary (explicit receiver, `perform:`) | yes | DNU at once | no (skipped, so the lookup goes on up) |
| `self`, implicit, `outer`, `super` | yes, virtual | yes, virtual | only in the sender's class or a lexically enclosing one, settled by the compiler |

1. **Bindings are ordered by level.** The image writer emits each vtable's bindings public, then protected, then
   private, and the vtable records `n_public` and `n_visible` (public + protected). Every run-time lookup loops to
   `n_visible` instead of `size`, so it never sees a private binding. An ordinary send (`OP_SEND`, the fast ops'
   fallback, `perform:`) that matches at an index at or past `n_public` has found a protected method, and stops there
   with DNU, as Soup does (Q5): a subclass that narrows a public selector to `protected` hides it, and does not
   quietly hand outsiders its superclass's version. The self-ish sends (`OP_SEND_IMPLICIT`'s walk, `super`) take the
   match. That index compare runs once per cache fill, never on a hit, so **a cache hit is unchanged**.
2. **An explicit send to `self` gets its own opcode, `OP_SEND_SELF`.** `self foo` compiles to `OP_SEND` today and
   shares the method cache with ordinary sends, so a protected method cached by `self foo` would be handed to an
   outside send of `foo` to the same class. `OP_SEND_SELF` has `OP_SEND`'s layout and handler, but looks up to
   `n_visible` and keys the cache on `selector | 1` (symbols are aligned, so the bit is free). The tag is a constant
   of the opcode, so it costs nothing per send. This is the one mechanism `protected` costs.
3. **Private is resolved at compile time.** A self, implicit or outer send whose selector is private in the
   sender's class, or in a lexically enclosing one, is known to the compiler, along with the defining class and the
   number of outer hops. It emits `OP_SEND_PRIVATE`, with `OP_SEND_SUPER`'s layout (`[c]` = the defining vtable
   constant, `[c+1]` = the receiver reached by the existing outer-slot walk). The VM looks the selector up in that one
   vtable, across all its bindings, and does not walk the delegate chain. This is Soup's `LexicalSend`, done once,
   ahead of time. A private method never reaches a run-time lookup, so the method cache can never hold one.
4. **The private send is uncached at first** (Q3). It costs a scan of one vtable, like `OP_SEND_SUPER`. The 212
   private sites go through the cached `OP_SEND` today, so the c6spare A/B measures the difference. What would earn
   a cache: a measurable loss on the board. Then the tagged key from step 2 extends to private entries.
5. **No compile-time rule** (Q4). nsl is dynamically typed, so a compiler check could only refuse an explicit send to
   a selector that has no public definition anywhere in the image. Even that would be wrong: a class that defines
   `doesNotUnderstand:` (a forwarding proxy, `tests/04_somns_basic/03_does_not_understand.nsl`) answers any selector,
   so the send is legal. The run-time lookup is the one guarantee. The same scan, run once as a throwaway migration
   tool, lists the ~130 sites to be marked `public`.
6. **Hot patch and mirrors.** The override table (`method_override_install`) gains the binding's level, so a patched
   method keeps its reach. Mirrors and the debugger read every binding. The patch format gets back the `access`
   field L1 deleted, now with three values.
7. **Slots and classes follow the same rule.** A slot's accessors take the slot's level, and so does a nested class's
   synthesised accessor. Internal slot access stays `OP_LD_SLOT`/`OP_ST_SLOT`.

What this costs per send: **nothing on the cached path.** Two opcodes (`OP_SEND_SELF`, `OP_SEND_PRIVATE`), two vtable
counts, one ordering rule in the image writer. The VM never checks access, so there is nothing to bypass.

## Found during this (to be filed P3 `found-during #372`)

- `OP_SEND_IMPLICIT`'s lexical walk starts at the **receiver's** class (`vm.c:1563`). Soup starts at the **sending
  method's** class (`FindApplicationOf`). They differ when an inherited method makes an implicit send and the
  subclass sits under different outers. Suspected, not reproduced. Step 3 takes private out of this path, but the
  walk still differs for public and protected methods.

## Steps

1. Grammar and parser: unmarked = `protected`; the level reaches the image for methods, slots and classes.
2. Compiler: bindings ordered by level, `n_public`/`n_visible` written; `OP_SEND_SELF` for `self foo`;
   `OP_SEND_PRIVATE` for private self, implicit and outer sends. Then the migration: the throwaway scan (step 5)
   lists the explicit sends to selectors with no public definition, each target is marked `public`, and the suite
   is the check.
3. VM: the loop bounds, the two handlers, the tagged key; `oevm_inst_check` and the image loader taught both opcodes.
4. Tests: an explicit send to a private method and to a protected one → DNU; a protected method overridden in a
   subclass and reached through `self`; a subclass that narrows a public selector to `protected` → DNU; a private method that shadows a public one in a superclass; a private send
   through `outer`; a `doesNotUnderstand:` proxy; a hot patch to a private method; a mirror listing every level.
5. Gates: the suite, the Go modules, PORTA SYNC; a c6spare A/B (the uncached private send, and `OP_SEND_SELF`).
6. Close-out: dialect row 25 turns green and `nsl-design-paper.md`'s row is struck; this record stays as the why.

## Questions for David

1. Answered: three levels, `protected` the default (follows Newspeak's intent).
2. Answered with 1.
3. Answered: the private send ships uncached and is measured on the board.
4. Answered: no compile-time rule (a `doesNotUnderstand:` handler makes any send legal); the run-time lookup is the guarantee.
5. Answered: an ordinary send that finds `protected` answers DNU at once, as Soup does (closer to the intent).

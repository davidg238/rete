# rete#37 — L4(b): a method frame's home is NULL (2026-10-12, night)

**Verdict: kept (Opt #19) for simplicity; speed level. Geomean AWFY C6 0.992, WeAct 0.998–1.000; STIC C6 0.986, WeAct
0.994 against master `d9c4fdae` (`run1/`, each pair of boards within 0.002), but the gain is code placement, not the
lever:** the rows that move most barely send (C6 arrayWrite 0.928, Sieve 0.946; arrayWrite was level through every
send lever), while the frame-heavy rows the change touches are level or slightly worse (C6 Richards 1.007, CD 1.007;
WeAct Towers 1.018–1.020). MCU text −28 B (46,808 → 46,780).

The change (`ef855730`, `vm.c`): `push_frame` and `run_interpreter` stored `cl->home_ctx ? cl->home_ctx :
home_link_encode_relative(vm_stack, ctx)`, a branch and an encode per frame. A frame's `home_ctx` is read only by the
mirror's `homeOf:` (`primitive_closure_home`; the GC's mark of a relative SMI is a no-op), so the frame now stores the
closure's home as-is — NULL for a method — and `homeOf:` answers the frame itself for NULL. One encode site fewer.

**L4(a) not built.** Leaving the frame's `vtable` unset is unsafe: `primitive_restart_frame` reads `vtable ==
MethodContextVtable` as "stopped", so a stale word from an earlier debugger-stopped frame at the same stack address
would make a running frame restartable, and the GC marks the field. Its safe form clears the field, which is the store
it meant to save.

Gates (msi, `ef855730`): nsl 298/0, every aux gate PASS, PORTA SYNC OK; custos red only on #414.
`tables.md` (`../2026-10-12-37-send-frame/boardtable.py`; all four boards).

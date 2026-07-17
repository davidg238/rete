import sys
import os
import json
import subprocess
from solidify import SolidifyCompiler
from stmap import build_stmap


class CompileError(Exception):
    """Raised for a source-level error the compiler detects at compile time
    (e.g. '^' inside an escaping closure under the no-NLR model). Caught at
    the top-level driver (main()) and reported cleanly — never surfaces as a
    raw Python traceback."""

# Opcode constants
# 2 retired: the dead IC-send path (never emitted; deleted in de-recursify
# Phase 2 Task 3) and the no-NLR-model non-local-return op ('^' in an escaping
# closure is a CompileError instead) — both fully compacted out in Task 9b's
# renumber (no reserved slots, no dead constants).
OP_LD_CONST = 0
OP_LD_REG = 1
OP_SEND = 2
OP_RET = 3
OP_ADD = 4
OP_SUB = 5
OP_MUL = 6
OP_DIV = 7
OP_JMP = 8
OP_JMP_IF_FALSE = 9
OP_LD_SLOT = 10
OP_ST_SLOT = 11
OP_NEW = 12
OP_SEND_IMPLICIT = 13
# 0x0E is intentionally not assigned here: it is OP_BREAK, a C-only live opcode
# (oevm/src/oevm.h) patched into a method's instruction stream by the debugger
# for breakpoints — never emitted by this compiler, so this compiler's opcode
# table has no OP_BREAK constant at all.
OP_SEND_SUPER = 0x0F
OP_LD_LEXICAL = 0x10
OP_ST_LEXICAL = 0x11
OP_CLOSURE    = 0x12
OP_LD_SENDER  = 0x13
OP_BLOCK_INVOKE = 0x14
# De-recursify deferred lift, Phase 1 (temp-vectors). Mirrors oevm/src/oevm.h.
# A frame's own Context lives in the fixed register `num_arguments + 1` (there
# is no free 4th operand byte in the 32-bit ISA), so LD/ST_CTX take no ctxreg
# operand — the VM reads it from that fixed register.
OP_MK_CONTEXT = 0x15   # dest=ctx_reg, src1=num_slots, src2=outer (0xFF => closure.captured_env; else NULL)
OP_LD_CTX     = 0x16   # dest, src1=depth (Context->outer hops), src2=slot_idx
OP_ST_CTX     = 0x17   # dest=value_reg, src1=depth, src2=slot_idx
OP_LD_SELF    = 0x18   # dest=reg; load running block closure's captured_self (self-migration)
# perf/op-cmp-at: send-layout fast-path opcodes (src/oevm.h). Identical operand
# layout to OP_SEND; a fast-path miss in the VM falls INTO the OP_SEND handler,
# so lowering any explicit-receiver send of these selectors is semantics-free.
OP_LT     = 0x19
OP_LE     = 0x1A
OP_GT     = 0x1B
OP_GE     = 0x1C
OP_AT     = 0x1D       # basicAt:
OP_AT_PUT = 0x1E       # basicAt:put:
FASTPATH_SEND_OPS = {"<": OP_LT, "<=": OP_LE, ">": OP_GT, ">=": OP_GE,
                     "basicAt:": OP_AT, "basicAt:put:": OP_AT_PUT}
def send_op_for(selector):
    return FASTPATH_SEND_OPS.get(selector, OP_SEND)


def pack_abc(opcode, a, b, c):
    """Encode a dest/src1/src2 instruction word (mirrors MethodCompiler.emit_ABC).
    Used by the deferred-lift Context post-pass to rewrite already-emitted words
    in place."""
    return ((c & 0xFF) << 24) | ((b & 0xFF) << 16) | ((a & 0xFF) << 8) | (opcode & 0xFF)


class Scope:
    """A lexical scope: name -> register bindings, plus the frame that owns
    those registers. Resolution walks enclosing scopes and counts how many real
    frame boundaries are crossed. See oevm/docs/topics.md#why-standalone-scope-class."""

    def __init__(self, frame, parent):
        self.frame = frame        # owning MethodCompiler (where regs live)
        self.parent = parent      # enclosing Scope, or None
        self.vars = {}            # name -> register index within self.frame

    def declare(self, name, reg):
        self.vars[name] = reg

    def resolve(self, name):
        """Return (reg, frame_depth) or None. frame_depth counts real frame
        boundaries crossed from this scope to the defining scope (0 = same frame)."""
        scope = self
        cur_frame = self.frame
        depth = 0
        while scope is not None:
            if scope.frame is not cur_frame:
                depth += 1
                cur_frame = scope.frame
            if name in scope.vars:
                return (scope.vars[name], depth)
            scope = scope.parent
        return None

    def defining_frame_of(self, name):
        """Mirror resolve()'s walk, but return the MethodCompiler frame that
        declares `name` (i.e. owns its register), or None if not found. Used
        to attribute a captured name to the enclosing frame that defines it,
        regardless of how many frame boundaries the reference itself crosses."""
        scope = self
        while scope is not None:
            if name in scope.vars:
                return scope.frame
            scope = scope.parent
        return None

class MethodCompiler:
    def __init__(self, name, selector, args, temps, parent_compiler, outer_compiler=None):
        self.name = name
        self.selector = selector
        self.args = args
        self.temps = temps
        self.parent = parent_compiler
        self.outer = outer_compiler
        # Mirror the target oop width down from the OevmCompiler (self.parent
        # is always the OevmCompiler instance, for both top-level methods and
        # nested block compilers — see the MethodCompiler( ) call sites).
        self.oop_bytes = parent_compiler.oop_bytes
        # When compiling a class-side (metaclass) method, this is the qname of
        # the class whose class object is `self`. Lets `self new` inside the
        # body instantiate that class. None for ordinary instance methods.
        # Inherited by nested block compilers so blocks in a class-side method
        # keep the same `self`-is-the-class meaning.
        self.class_side_qname = outer_compiler.class_side_qname if outer_compiler is not None else None

        # Register mapping: self (0), args (1..n), Context (n+1), temps (n+2..).
        # De-recursify deferred lift Phase 1: register `num_arguments + 1` is
        # RESERVED in every frame to hold that frame's own heap Context (the
        # temp-vector for captured locals). The VM hard-codes this slot
        # (OP_MK/LD/ST_CTX read `regs[num_arguments + 1]`; OP_CLOSURE reads it as
        # the new closure's captured_env). Reserving it uniformly — even in
        # capture-free frames, where it stays NULL (frame regs are nil-seeded) —
        # keeps OP_CLOSURE's unconditional read GC-safe.
        self.local_vars = {"self": 0}
        for i, arg in enumerate(args):
            self.local_vars[arg] = i + 1

        self.ctx_reg = len(args) + 1
        self.first_temp_reg = len(args) + 2
        for i, temp in enumerate(temps):
            self.local_vars[temp] = self.first_temp_reg + i

        self.next_free_reg = self.first_temp_reg + len(temps)
        self.max_regs_used = self.next_free_reg
        self.constants = []
        self.instructions = []
        # Source map: (bytecode_offset, source_line) recorded at statement
        # boundaries, mirroring st-berry/transpiler/st_funcctx.py. Pure
        # bookkeeping — never affects emitted bytecode.
        self.source_map = []

        # De-recursify deferred lift, Task 3 (analysis only — no codegen change
        # yet): the set of this frame's args/temps (or `self`) that are
        # referenced from a nested block at frame_depth > 0, i.e. candidates
        # for indirect Context-slot storage instead of a plain register.
        # Populated incrementally as nested blocks are compiled (see
        # resolve_name); `capture_slot` is only assigned once this frame's
        # whole body — including all descendant blocks — has finished
        # compiling (see finalize_jumps).
        self.captured_locals = set()
        self.capture_slot = {}
        # De-recursify deferred lift Phase 1 (Task 4 codegen): captured-local
        # access sites recorded during body codegen for the post-pass rewrite.
        # capture_slot is only complete after the whole body (incl. every nested
        # block) finishes compiling, so we emit a provisional register/lexical
        # access, remember where, and rewrite the ones that turned out captured to
        # OP_LD_CTX/OP_ST_CTX once slots are assigned (see finalize_contexts()).
        # Each entry: (instr_index, is_write, defining_frame, name, depth, val_reg).
        self._ctx_patch = []
        # Internal: name -> original register, recorded at the point of capture
        # (see resolve_name). NOT every captured name lives in local_vars — an
        # inlined literal-block's params/temps (to:do:/ifTrue:/... induction
        # vars, declared straight into a child Scope by compile_inlined_block)
        # share this frame but never populate local_vars, so the register must
        # be captured from the resolve() call site itself, not looked up after
        # the fact.
        self._capture_regs = {}

        # Lexical scope (see oevm/docs/topics.md#scope-vs-frame). Phase A: one
        # root scope per frame, mirroring local_vars. `self` lives only in
        # non-block frames so it resolves lexically (via home/regAt:) inside
        # blocks, matching prior behaviour.
        is_block = "_block" in name
        parent_scope = outer_compiler.current_scope if outer_compiler is not None else None
        self.root_scope = Scope(self, parent_scope)
        self.current_scope = self.root_scope
        if not is_block:
            self.root_scope.declare("self", 0)
        for i, arg in enumerate(args):
            self.root_scope.declare(arg, i + 1)
        for i, temp in enumerate(temps):
            self.root_scope.declare(temp, self.first_temp_reg + i)

        self._label_counter = 0
        self._label_targets = {}     # label id -> instruction index
        self._pending_jumps = []     # (placeholder_index, op, cond_reg, label)

    @staticmethod
    def _is_block_value_selector(selector):
        # "value", "value:", "value:value:", ... — the fixed-arity block-invoke
        # protocol. Matches "value" followed by zero or more ":" and nothing else.
        # (valueWithArguments: deliberately does NOT match — it spreads an array
        #  and stays an ordinary send for now.)
        if not selector.startswith("value"):
            return False
        rest = selector[len("value"):]
        return rest == "" or all(ch == ":" for ch in rest)

    def add_constant(self, val):
        if val not in self.constants:
            self.constants.append(val)
        return self.constants.index(val)

    def add_string_constant(self, val):
        # std-lib 3c-1: string literals materialize as `String` instances, NOT
        # interned Symbols. Tag the pool entry with a typed tuple ("STRING", val)
        # so the constant emitter routes it to a StaticString_ / String (Path A)
        # instead of get_symbol_ref. A tuple (vs a "STRING:"-content prefix) can
        # never collide with arbitrary user string content (e.g. a literal whose
        # text is itself 'SMI:5' or 'STRING:x'), mirroring how floats are
        # distinguished by isinstance(const, float).
        marker = ("STRING", val)
        if marker not in self.constants:
            self.constants.append(marker)
        return self.constants.index(marker)

    def alloc_reg(self):
        reg = self.next_free_reg
        self.next_free_reg += 1
        if self.next_free_reg > self.max_regs_used:
            self.max_regs_used = self.next_free_reg
        return reg

    def free_reg(self):
        self.next_free_reg -= 1

    def emit(self, inst):
        self.instructions.append(inst)

    def emit_ABC(self, opcode, a, b, c):
        inst = ((c & 0xFF) << 24) | ((b & 0xFF) << 16) | ((a & 0xFF) << 8) | (opcode & 0xFF)
        self.emit(inst)

    def emit_ABx(self, opcode, a, bx):
        inst = ((bx & 0xFFFF) << 16) | ((a & 0xFF) << 8) | (opcode & 0xFF)
        self.emit(inst)

    def emit_sBx(self, opcode, sbx):
        # sBx has offset biased by 0x8000 or signed 16-bit
        # For simplicity, OEVM JMP uses standard signed 16-bit offset directly in inst >> 8 or inst >> 16
        if opcode == OP_JMP:
            inst = ((sbx & 0xFFFF) << 8) | (opcode & 0xFF)
        elif opcode == OP_JMP_IF_FALSE:
            # For JMP_IF_FALSE, it is (offset << 16) | (cond_reg << 8) | OP_JMP_IF_FALSE
            # We handle it separately in emit_jump
            raise ValueError("Use emit_jump for JMP_IF_FALSE")
        self.emit(inst)

    def new_label(self):
        self._label_counter += 1
        return self._label_counter

    def bind_label(self, label):
        self._label_targets[label] = len(self.instructions)

    def emit_jump(self, op, cond_reg, label):
        # Reserve a slot; real offset is patched in finalize_jumps().
        idx = len(self.instructions)
        self.instructions.append(None)
        self._pending_jumps.append((idx, op, cond_reg, label))

    def finalize_jumps(self):
        # Offsets are relative to the instruction AFTER the jump, because the
        # interpreter does pc++ before decoding (see oevm/src/vm.c OP_JMP).
        for idx, op, cond_reg, label in self._pending_jumps:
            target = self._label_targets[label]
            offset = target - (idx + 1)
            if op == OP_JMP:
                inst = ((offset & 0xFFFF) << 8) | (op & 0xFF)
            elif op == OP_JMP_IF_FALSE:
                inst = ((offset & 0xFFFF) << 16) | ((cond_reg & 0xFF) << 8) | (op & 0xFF)
            else:
                raise ValueError(f"finalize_jumps: unexpected op {op}")
            self.instructions[idx] = inst
        self._pending_jumps = []

        # De-recursify deferred lift, Task 3: this frame's whole body —
        # including every descendant block, which are compiled to completion
        # before their enclosing frame reaches this point (see the "block"
        # case in compile_expr) — has now finished compiling, so
        # captured_locals is complete. Assign dense, deterministic slots
        # ordered by original register (stable across runs — matters for
        # .oimg reproducibility and the differential oracle).
        # Tie-break on the name so the order is a deterministic TOTAL order:
        # two captured names can share a register (e.g. inlined-block induction
        # vars from two sequential `to:do:` loops both reuse the same reg), and
        # a plain register key would then fall back to hash-randomized set
        # iteration order — different slots per process, breaking .oimg
        # reproducibility once Task 4 bakes capture_slot into codegen.
        #
        # Task 4 fix (David's option b): ONLY real frame args/temps get a
        # Context slot. An INLINED-block-managed local — a to:do:/to:by:do:
        # induction var, or an inlined ifTrue:/whileTrue: block param/temp —
        # lives in an inlined-block child Scope and is NOT in this real frame's
        # local_vars (the same class that caused the Task-3 KeyError). The inline
        # loop codegen writes/reads it via a plain register (i_reg), never the
        # Context, so Context-homing it would rewrite the loop's OWN same-frame
        # reads to OP_LD_CTX against a slot nobody writes -> nil -> crash. Keep
        # these register+home_ctx-homed: excluded from capture_slot, their
        # reads/writes stay on OP_LD_REG (same frame) / OP_LD_LEXICAL (nested
        # block via home_ctx), i.e. exactly pre-Phase-1 behavior. Mixed homing
        # in one nested block is fine: real captures via OP_LD_CTX (Context->outer),
        # inlined-loop captures via OP_LD_LEXICAL (home_ctx) — both reach the
        # defining frame. num_slots (finalize_contexts) then counts only the
        # Context-homed captures, and the Context->outer depth still equals
        # frame_depth (every block still allocates a Context link).
        # LIMITATION (deferred): inlined-loop/inlined-block captured locals remain
        # NOT upward/cross-fiber-safe (same as pre-phase).
        real_captures = [n for n in self.captured_locals if n in self.local_vars]
        for i, name in enumerate(sorted(real_captures,
                                        key=lambda n: (self._capture_regs[n], n))):
            self.capture_slot[name] = i

    def is_block_frame(self):
        return "_block" in self.name

    def finalize_contexts(self):
        """De-recursify deferred lift Phase 1 (Task 4): the frame's body + all
        descendant blocks are compiled and every frame's capture_slot is
        assigned. (1) Rewrite each recorded captured-local access to its
        OP_LD_CTX/OP_ST_CTX form; (2) prepend the frame's OP_MK_CONTEXT prologue.
        MUST run after this frame AND every frame it lexically references have
        finalized (the compile() emit-loops guarantee this — see call sites).
        Patch first (indices are body-relative), then prepend (jumps are already
        relative offsets, so a uniform prepend leaves them valid; only the
        absolute source_map offsets need shifting)."""
        # (1) Rewrite captured accesses.
        for (idx, is_write, frame, name, depth, val_reg) in self._ctx_patch:
            if frame is None or name not in frame.capture_slot:
                continue  # not captured -> leave the provisional register/lexical op
            slot = frame.capture_slot[name]
            op = OP_ST_CTX if is_write else OP_LD_CTX
            self.instructions[idx] = pack_abc(op, val_reg, depth, slot)

        # (2) Prepend the prologue. Every BLOCK frame allocates a Context (even
        # 0-slot: a pass-through link so Context->outer depth matches frame_depth);
        # a METHOD frame allocates one only when it actually has captures.
        need_ctx = self.is_block_frame() or bool(self.capture_slot)
        if not need_ctx:
            return
        num_slots = len(self.capture_slot)
        # 0xFF => outer is the running closure's captured_env (block chains to its
        # lexically-enclosing frame's Context); a method root has no outer (NULL).
        outer_sentinel = 0xFF if self.is_block_frame() else 0x00
        prologue = [pack_abc(OP_MK_CONTEXT, self.ctx_reg, num_slots, outer_sentinel)]
        # Captured ARGS / block-params arrive in their incoming registers; copy
        # them into their Context slots so all later access reads the live value
        # from the Context. Captured TEMPS start nil (context_alloc nils slots)
        # and are first written through OP_ST_CTX, so they need no init-copy.
        for name, slot in sorted(self.capture_slot.items(), key=lambda kv: kv[1]):
            if name in self.args:
                prologue.append(pack_abc(OP_ST_CTX, self.local_vars[name], 0, slot))
        n = len(prologue)
        self.instructions = prologue + self.instructions
        self.source_map = [(off + n, line) for (off, line) in self.source_map]

    def compile_inlined_block(self, block, target_reg, param_regs=None, init_temps=False):
        # Compile a literal block's body directly into THIS frame (no closure).
        # Push a child Scope sharing this frame (so its vars and the enclosing
        # frame's vars resolve as plain registers, frame_depth 0), declare the
        # block's temps as fresh registers, compile the body so its value lands
        # in target_reg, then pop the scope and reclaim the temp registers.
        # `^` inside the body is compiled by compile_statement against THIS
        # frame: since THIS frame is never a block frame here (inlining always
        # targets the lexically-enclosing non-inlined frame), it becomes
        # OP_RET. See oevm/docs/topics.md#block-inlining and the no-NLR model
        # (spec §2): '^' only becomes a CompileError when THIS frame IS a
        # block frame, i.e. a genuinely escaping (non-inlined) closure.
        saved_free = self.next_free_reg
        saved_scope = self.current_scope
        self.current_scope = Scope(self, saved_scope)
        # Bind block params to caller-owned registers (e.g. a loop induction
        # variable). The caller allocated them below saved_free and keeps them
        # live across iterations; we only name them in the child scope.
        if param_regs is not None:
            for name, reg in zip(block.get("params", []), param_regs):
                self.current_scope.declare(name, reg)
        # Declare block temps as fresh registers. When this body is re-run in a
        # loop, init_temps=True nil-resets them each iteration to match real
        # block-activation semantics (a fresh closure gets nil temps).
        for t in block.get("temps", []):
            reg = self.alloc_reg()
            self.current_scope.declare(t, reg)
            if init_temps:
                self.emit_ABx(OP_LD_CONST, reg, self.add_constant(None))
        body = block["body"]
        if not body:
            self.emit_ABx(OP_LD_CONST, target_reg, self.add_constant(None))
        else:
            for stmt in body[:-1]:
                self.compile_statement(stmt)
            last = body[-1]
            if last["type"] == "return":
                self.compile_statement(last)        # OP_RET, or CompileError if this is a block frame
            else:
                self.compile_expr(last, target_reg)  # value falls through
        self.current_scope = saved_scope
        self.next_free_reg = saved_free

    def emit_block_value(self, block_arg, target_reg, param_regs=None, init_temps=False):
        # Put the VALUE of a combinator's block argument into target_reg.
        #  - literal block of matching arity -> inline its body (fast path).
        #  - any other expression (block held in a var/slot/param) -> evaluate to
        #    a closure and OP_BLOCK_INVOKE it; param_regs become value: arguments.
        want_params = len(param_regs) if param_regs else 0
        if (block_arg["type"] == "block"
                and len(block_arg.get("params", [])) == want_params):
            self.compile_inlined_block(block_arg, target_reg,
                                       param_regs=param_regs, init_temps=init_temps)
            return
        saved_free = self.next_free_reg
        base = self.alloc_reg()                       # receiver = the closure
        self.compile_expr(block_arg, base)
        self.next_free_reg = base + 1
        argc = 0
        if param_regs:
            for pr in param_regs:
                areg = self.alloc_reg()               # contiguous value: args
                self.emit_ABC(OP_LD_REG, areg, pr, 0)
                self.next_free_reg = areg + 1
                argc += 1
        cidx = self.add_constant("value" + ":" * argc)
        self.emit_ABC(OP_BLOCK_INVOKE, argc, cidx, base)
        self.emit_ABC(OP_LD_REG, target_reg, base, 0)  # result lands in `base`
        self.next_free_reg = saved_free

    def _emit_cond_jmp_false(self, cond_expr, cond_reg, label):
        # Compile `cond_expr` into cond_reg and branch to `label` when falsy.
        # Single-sources the condition+JMP_IF_FALSE skeleton shared by the
        # if-family, and:/or:, and the loop lowerings. Truthiness lives solely
        # in OP_JMP_IF_FALSE (the VM's is_truthy).
        self.compile_expr(cond_expr, cond_reg)
        self.emit_jump(OP_JMP_IF_FALSE, cond_reg, label)

    def compile_if_inline(self, selector, receiver, args, target_reg):
        # Lower an if-family send to jumps. `target_reg` holds the result.
        # An absent arm yields the `false` symbol (matches the primitives).
        if selector == "ifTrue:":
            true_block, false_block = args[0], None
        elif selector == "ifFalse:":
            true_block, false_block = None, args[0]
        elif selector == "ifTrue:ifFalse:":
            true_block, false_block = args[0], args[1]
        else:  # "ifFalse:ifTrue:"
            true_block, false_block = args[1], args[0]

        cond_reg = self.alloc_reg()
        l_false = self.new_label()
        l_end = self.new_label()
        self._emit_cond_jmp_false(receiver, cond_reg, l_false)
        # condition truthy:
        if true_block is not None:
            self.emit_block_value(true_block, target_reg)
        else:
            self.emit_ABx(OP_LD_CONST, target_reg, self.add_constant("false"))
        self.emit_jump(OP_JMP, 0, l_end)
        # condition falsy:
        self.bind_label(l_false)
        if false_block is not None:
            self.emit_block_value(false_block, target_reg)
        else:
            self.emit_ABx(OP_LD_CONST, target_reg, self.add_constant("false"))
        self.bind_label(l_end)
        self.free_reg()  # cond_reg

    def compile_logic_inline(self, selector, receiver, block, target_reg):
        # Short-circuit `and:` / `or:` with a literal zero-arg block, matching
        # PRIM_and / PRIM_or exactly (oevm/src/dispatch.c):
        #   and: truthy -> block value;     falsy -> the `false` symbol
        #   or:  truthy -> the `true` symbol; falsy -> block value
        # The condition test uses OP_JMP_IF_FALSE (is_truthy), same as the
        # primitive. See oevm/docs/topics.md#block-inlining.
        cond_reg = self.alloc_reg()
        l_false = self.new_label()
        l_end = self.new_label()
        self._emit_cond_jmp_false(receiver, cond_reg, l_false)
        # condition truthy:
        if selector == "and:":
            self.emit_block_value(block, target_reg)
        else:  # "or:"
            self.emit_ABx(OP_LD_CONST, target_reg, self.add_constant("true"))
        self.emit_jump(OP_JMP, 0, l_end)
        # condition falsy:
        self.bind_label(l_false)
        if selector == "and:":
            self.emit_ABx(OP_LD_CONST, target_reg, self.add_constant("false"))
        else:  # "or:"
            self.emit_block_value(block, target_reg)
        self.bind_label(l_end)
        self.free_reg()  # cond_reg

    def compile_to_do_inline(self, start_expr, end_expr, step_node, block, target_reg):
        # `start to: end do: [:i | body]` (and to:by:do: with a literal step).
        # Result value = the receiver (start), matching PRIM_to_do / PRIM_to_by_do.
        # i lives in i_reg (the block's single param); end in end_reg. The
        # condition `i <= end` (or `i >= end` for a negative step) is a normal
        # comparison send; the increment uses the OP_ADD fast path.
        step_val = 1 if step_node is None else int(step_node["value"])
        cmp_sel = ">=" if step_val < 0 else "<="

        self.compile_expr(start_expr, target_reg)        # result = receiver = start
        i_reg = self.alloc_reg()
        self.emit_ABC(OP_LD_REG, i_reg, target_reg, 0)   # i := start
        end_reg = self.alloc_reg()
        self.compile_expr(end_expr, end_reg)

        l_start = self.new_label()
        l_end = self.new_label()
        self.bind_label(l_start)
        # cond: (i <cmp> end) -> result lands in rc
        rc = self.alloc_reg()
        self.emit_ABC(OP_LD_REG, rc, i_reg, 0)
        ec = self.alloc_reg()                            # contiguous arg = rc + 1
        self.emit_ABC(OP_LD_REG, ec, end_reg, 0)
        self.emit_ABC(send_op_for(cmp_sel), 1, self.add_constant(cmp_sel), rc)
        self.emit_jump(OP_JMP_IF_FALSE, rc, l_end)
        self.free_reg()  # ec
        self.free_reg()  # rc
        # body: literal block inlines with param bound to i_reg; a non-literal
        # block is OP_BLOCK_INVOKE'd as `body value: i`.
        body_reg = self.alloc_reg()
        self.emit_block_value(block, body_reg, param_regs=[i_reg], init_temps=True)
        self.free_reg()  # body_reg
        # i := i + step
        step_reg = self.alloc_reg()
        self.emit_ABx(OP_LD_CONST, step_reg, self.add_constant(f"SMI:{step_val}"))
        self.emit_ABC(OP_ADD, i_reg, i_reg, step_reg)
        self.free_reg()  # step_reg
        self.emit_jump(OP_JMP, 0, l_start)
        self.bind_label(l_end)
        self.free_reg()  # end_reg
        self.free_reg()  # i_reg
        # target_reg already holds the start value (the receiver) = result

    def compile_to_by_do_runtime(self, start_expr, end_expr, step_expr, block, target_reg):
        # `start to: end by: step do: [:i | body]` when `step` is not a
        # literal nonzero integer, so compile_to_do_inline's compile-time
        # direction fast path doesn't apply. Direction is resolved ONCE at
        # runtime via a sign check on step, then one of two counted loops
        # runs (identical but for the comparison selector), matching
        # PRIM_to_by_do: i = start, start+step, ... while i <= end (step > 0)
        # or i >= end (step < 0); step = 0 does zero iterations. Result value
        # = the receiver (start).
        self.compile_expr(start_expr, target_reg)        # result = receiver = start
        i_reg = self.alloc_reg()
        self.emit_ABC(OP_LD_REG, i_reg, target_reg, 0)    # i := start
        end_reg = self.alloc_reg()
        self.compile_expr(end_expr, end_reg)
        step_reg = self.alloc_reg()
        self.compile_expr(step_expr, step_reg)

        l_neg = self.new_label()
        l_pos = self.new_label()
        l_done = self.new_label()

        def emit_counted_loop(cmp_sel):
            # Shared counted-loop skeleton (mirrors compile_to_do_inline):
            # cond `i <cmp_sel> end`, body via emit_block_value, i += step.
            l_start = self.new_label()
            l_loop_end = self.new_label()
            self.bind_label(l_start)
            rc = self.alloc_reg()
            self.emit_ABC(OP_LD_REG, rc, i_reg, 0)
            ec = self.alloc_reg()                         # contiguous arg = rc + 1
            self.emit_ABC(OP_LD_REG, ec, end_reg, 0)
            self.emit_ABC(send_op_for(cmp_sel), 1, self.add_constant(cmp_sel), rc)
            self.emit_jump(OP_JMP_IF_FALSE, rc, l_loop_end)
            self.free_reg()  # ec
            self.free_reg()  # rc
            body_reg = self.alloc_reg()
            self.emit_block_value(block, body_reg, param_regs=[i_reg], init_temps=True)
            self.free_reg()  # body_reg
            self.emit_ABC(OP_ADD, i_reg, i_reg, step_reg)
            self.emit_jump(OP_JMP, 0, l_start)
            self.bind_label(l_loop_end)

        # step = 0 -> zero iterations (skip both direction loops entirely,
        # matching PRIM_to_by_do's step==0 no-op).
        zc = self.alloc_reg()
        self.emit_ABC(OP_LD_REG, zc, step_reg, 0)
        zec = self.alloc_reg()                            # contiguous arg = zc + 1
        self.emit_ABx(OP_LD_CONST, zec, self.add_constant("SMI:0"))
        self.emit_ABC(OP_SEND, 1, self.add_constant("="), zc)
        l_nonzero = self.new_label()
        self.emit_jump(OP_JMP_IF_FALSE, zc, l_nonzero)
        self.emit_jump(OP_JMP, 0, l_done)
        self.bind_label(l_nonzero)
        self.free_reg()  # zec
        self.free_reg()  # zc

        # step < 0 -> the `i >= end` loop (l_neg); else (step > 0, zero
        # already excluded above) -> the `i <= end` loop (l_pos).
        sc = self.alloc_reg()
        self.emit_ABC(OP_LD_REG, sc, step_reg, 0)
        sec = self.alloc_reg()                            # contiguous arg = sc + 1
        self.emit_ABx(OP_LD_CONST, sec, self.add_constant("SMI:0"))
        self.emit_ABC(send_op_for("<"), 1, self.add_constant("<"), sc)
        self.emit_jump(OP_JMP_IF_FALSE, sc, l_pos)
        self.free_reg()  # sec
        self.free_reg()  # sc

        self.bind_label(l_neg)
        emit_counted_loop(">=")
        self.emit_jump(OP_JMP, 0, l_done)

        self.bind_label(l_pos)
        emit_counted_loop("<=")

        self.bind_label(l_done)
        self.free_reg()  # step_reg
        self.free_reg()  # end_reg
        self.free_reg()  # i_reg
        # target_reg already holds the start value (the receiver) = result

    def compile_times_repeat(self, receiver, block, target_reg):
        # `n timesRepeat: aBlock` — evaluate the 0-arg block `n` times; result = n
        # (the receiver). Matches PRIM_timesRepeat. Loop counter i: 0..<n.
        self.compile_expr(receiver, target_reg)        # result = receiver = n
        n_reg = self.alloc_reg()
        self.emit_ABC(OP_LD_REG, n_reg, target_reg, 0) # n := receiver
        i_reg = self.alloc_reg()
        self.emit_ABx(OP_LD_CONST, i_reg, self.add_constant("SMI:0"))  # i := 0
        l_start = self.new_label()
        l_end = self.new_label()
        self.bind_label(l_start)
        # cond: (i < n) -> rc
        rc = self.alloc_reg()
        self.emit_ABC(OP_LD_REG, rc, i_reg, 0)
        ec = self.alloc_reg()                          # contiguous arg = rc + 1
        self.emit_ABC(OP_LD_REG, ec, n_reg, 0)
        self.emit_ABC(send_op_for("<"), 1, self.add_constant("<"), rc)
        self.emit_jump(OP_JMP_IF_FALSE, rc, l_end)
        self.free_reg()  # ec
        self.free_reg()  # rc
        # body: 0-arg block (literal inlines; non-literal OP_BLOCK_INVOKE'd)
        body_reg = self.alloc_reg()
        self.emit_block_value(block, body_reg, init_temps=True)
        self.free_reg()  # body_reg
        # i := i + 1
        step_reg = self.alloc_reg()
        self.emit_ABx(OP_LD_CONST, step_reg, self.add_constant("SMI:1"))
        self.emit_ABC(OP_ADD, i_reg, i_reg, step_reg)
        self.free_reg()  # step_reg
        self.emit_jump(OP_JMP, 0, l_start)
        self.bind_label(l_end)
        self.free_reg()  # i_reg
        self.free_reg()  # n_reg
        # target_reg already holds n (the receiver) = result

    def compile_loop_inline(self, selector, cond_block, body_block, target_reg):
        # `[cond] whileTrue: [body]` / whileFalse:. Result = nil (matches the
        # primitive). cond_reg and body_reg persist across iterations (allocated
        # once, below the scratch the inlined blocks reuse each pass). Truthiness
        # goes through OP_JMP_IF_FALSE only. cond_block/body_block may be literal
        # zero-arg blocks (inlined by emit_block_value) or any other expression
        # (block held in a var/slot/param, OP_BLOCK_INVOKE'd as `value`).
        cond_reg = self.alloc_reg()
        body_reg = self.alloc_reg()          # body value, discarded
        l_start = self.new_label()
        l_end = self.new_label()
        self.bind_label(l_start)
        self.emit_block_value(cond_block, cond_reg, init_temps=True)
        if selector == "whileTrue:":
            self.emit_jump(OP_JMP_IF_FALSE, cond_reg, l_end)   # falsy -> exit
            self.emit_block_value(body_block, body_reg, init_temps=True)
            self.emit_jump(OP_JMP, 0, l_start)
        else:  # "whileFalse:" — continue while falsy, exit when truthy
            l_body = self.new_label()
            self.emit_jump(OP_JMP_IF_FALSE, cond_reg, l_body)  # falsy -> body
            self.emit_jump(OP_JMP, 0, l_end)                   # truthy -> exit
            self.bind_label(l_body)
            self.emit_block_value(body_block, body_reg, init_temps=True)
            self.emit_jump(OP_JMP, 0, l_start)
        self.bind_label(l_end)
        self.emit_ABx(OP_LD_CONST, target_reg, self.add_constant(None))  # nil
        self.free_reg()  # body_reg
        self.free_reg()  # cond_reg

    def compile_while_unary(self, selector, cond_block, target_reg):
        # `[cond] whileTrue` / `[cond] whileFalse` — no body. Loop evaluating the
        # condition block until it goes falsy (whileTrue) / truthy (whileFalse).
        # Result = nil. Condition invoked via emit_block_value (literal inlines,
        # non-literal OP_BLOCK_INVOKE'd).
        cond_reg = self.alloc_reg()
        l_start = self.new_label()
        l_end = self.new_label()
        self.bind_label(l_start)
        self.emit_block_value(cond_block, cond_reg, init_temps=True)
        if selector == "whileTrue":
            self.emit_jump(OP_JMP_IF_FALSE, cond_reg, l_end)   # falsy -> exit
            self.emit_jump(OP_JMP, 0, l_start)
        else:  # "whileFalse"
            l_again = self.new_label()
            self.emit_jump(OP_JMP_IF_FALSE, cond_reg, l_again) # falsy -> loop
            self.emit_jump(OP_JMP, 0, l_end)                   # truthy -> exit
            self.bind_label(l_again)
            self.emit_jump(OP_JMP, 0, l_start)
        self.bind_label(l_end)
        self.emit_ABx(OP_LD_CONST, target_reg, self.add_constant(None))  # nil
        self.free_reg()  # cond_reg

    def resolve_name(self, name):
        # 1+2. Lexical resolution via the scope chain (this frame's scopes,
        # then enclosing frames). frame_depth 0 == same frame == a register.
        found = self.current_scope.resolve(name)
        if found is not None:
            reg, frame_depth = found
            if frame_depth == 0:
                return ("local", reg)
            # Crosses >=1 frame boundary: mark `name` captured in the frame
            # that actually declares it (not this referencing frame). See
            # Task 3/4 (de-recursify deferred lift). `self` is EXCLUDED from the
            # heap-Context temp-vector: it is snapshotted onto the closure
            # (captured_self) and read via OP_LD_SELF — both the implicit path
            # (get_self_reg) and the explicit varRef path (below) — NOT the
            # home_ctx chain (Phase 2 self-migration). Only ordinary args/temps
            # move to the temp-vector; inlined-loop vars stay OP_LD_LEXICAL.
            if name != "self":
                defining_frame = self.current_scope.defining_frame_of(name)
                if defining_frame is not None:
                    defining_frame.captured_locals.add(name)
                    defining_frame._capture_regs[name] = reg
            return ("lexical", reg, frame_depth)

        # 2. Slot in the current class or enclosing classes
        # Walk parent scopes
        class_stack = self.parent.class_stack
        for depth, cls_name in enumerate(reversed(class_stack)):
            slots = self.parent.class_slots[cls_name]
            if name in slots:
                return ("slot", slots.index(name), depth)
                
        # 3. Nested class visible in the enclosing lexical scope
        ref = self.parent.resolve_class_ref(name)
        if ref is not None:
            return ("nested_class", ref)

        # 4. A capitalised name with no lexical/slot/nested-class binding is a
        #    library global (Array, OrderedCollection, ...). Route it through the
        #    handed `platform` capability when one is in lexical scope — reached
        #    as a threaded param/slot, never a VM global (capability-pure). The
        #    "platform" lookup is lowercase, so it cannot re-enter this branch.
        #    Falls back to the legacy implicit-send when no platform is in scope,
        #    keeping pre-std-lib programs green during the migration.
        if name[:1].isupper():
            plat = self.resolve_name("platform")
            if plat[0] in ("local", "lexical", "slot"):
                return ("platform_global", name)

        # 5. Implicit send fallback (lowercase self-sends; or no platform handed)
        return ("implicit_send", name)
            
    def get_self_reg(self):
        if "_block" in self.name:
            # De-recursify Phase 2 (self-migration, Approach B): a block's self is its
            # home method's receiver, snapshotted onto the closure at creation
            # (captured_self). Read it fiber-independently via OP_LD_SELF instead of
            # walking the home_ctx chain (OP_LD_LEXICAL) — the latter is not
            # cross-fiber safe once home_ctx is relativized (Task 2).
            base_reg = self.alloc_reg()
            self.emit_ABC(OP_LD_SELF, base_reg, 0, 0)
            return base_reg
        else:
            return 0

    def free_self_reg(self, reg):
        if reg != 0:
            self.free_reg()

    def record_line(self, node):
        # Record the source line for the next instruction at the current offset.
        # Guarded: synthetic nodes without a span (e.g. generated accessors) are
        # simply not mapped.
        span = node.get("span") if isinstance(node, dict) else None
        if span and "startLine" in span:
            self.source_map.append((len(self.instructions), span["startLine"]))

    def compile_statement(self, stmt):
        self.record_line(stmt)
        if stmt["type"] == "return":
            if "_block" in self.name:
                # no-NLR model (spec §2): '^' lexically inside an escaping
                # closure (a block compiled as its own method — this frame's
                # name contains "_block") has no runtime mechanism to unwind
                # to its home method context anymore. '^' inside an INLINED
                # control-flow block (ifTrue:/whileTrue:/to:do:) never reaches
                # here as a block frame — compile_inlined_block compiles it
                # directly into the enclosing (non-block) frame instead.
                raise CompileError(
                    "non-local return: '^' inside an escaping closure is not "
                    "allowed (no-NLR model). Use a collection combinator or a "
                    "sentinel-and-return.")
            reg = self.alloc_reg()
            self.compile_expr(stmt["value"], reg)
            self.emit_ABC(OP_RET, reg, 0, 0)
            self.free_reg()
        else:
            reg = self.alloc_reg()
            self.compile_expr(stmt, reg)
            self.free_reg()

    def compile_expr(self, expr, target_reg):
        et = expr["type"]
        if et == "literal":
            val = expr["value"]
            kind = expr["litKind"]
            if kind == "int":
                ival = int(val)
                # Integer literals that overflow the tagged SmallInteger range
                # become boxed LargeInteger constants. RefWord.smi(v) packs
                # (v << 2) | TAG_SMI into an oop*8-bit word: 2 tag bits on top
                # of a signed payload, so the true capacity is magnitude
                # < 2^(oop_bytes*8-3) — the SAME bound oimg_serialize.py's
                # _smi_fits() enforces. The old bound (2^(oop_bytes*8-2), i.e.
                # a fixed 1<<62 on a 64-bit host) was one bit too lenient and
                # let gap-band literals get labeled "SMI:" here even though
                # they don't fit the ref-word (nsl-tuvm#4).
                smi_limit = 1 << (self.oop_bytes * 8 - 3)
                if -smi_limit <= ival < smi_limit:
                    cidx = self.add_constant(f"SMI:{ival}")
                else:
                    cidx = self.add_constant(f"LARGEINT:{ival}")
                self.emit_ABx(OP_LD_CONST, target_reg, cidx)
            elif kind == "float":
                cidx = self.add_constant(float(val))
                self.emit_ABx(OP_LD_CONST, target_reg, cidx)
            elif kind == "symbol":
                cidx = self.add_constant(str(val))
                self.emit_ABx(OP_LD_CONST, target_reg, cidx)
            elif kind == "string":
                cidx = self.add_string_constant(str(val))   # String constant, not interned Symbol
                self.emit_ABx(OP_LD_CONST, target_reg, cidx)
            elif kind == "bool":
                cidx = self.add_constant("true" if val else "false")
                self.emit_ABx(OP_LD_CONST, target_reg, cidx)
            elif kind == "nil":
                cidx = self.add_constant(None)
                self.emit_ABx(OP_LD_CONST, target_reg, cidx)
        
        elif et == "varRef":
            name = expr["name"]
            res = self.resolve_name(name)
            if res[0] == "local":
                local_reg = res[1]
                # Always emit (even target==local_reg -> a harmless self-move):
                # if this own-frame local turns out captured, the post-pass
                # rewrites this word to an OP_LD_CTX read from the heap Context
                # (the value no longer lives in the register). See finalize_contexts.
                idx = len(self.instructions)
                self.emit_ABC(OP_LD_REG, target_reg, local_reg, 0)
                self._ctx_patch.append((idx, False, self, name, 0, target_reg))
            elif res[0] == "lexical":
                reg_idx, depth = res[1], res[2]
                assert depth < 256 and reg_idx < 256
                if name == "self":
                    # self-migration (Approach B): explicit `self` used as a value or
                    # send-receiver in a block reads the closure's captured_self
                    # (fiber-independent) via OP_LD_SELF, NOT the home_ctx chain — the
                    # same treatment get_self_reg gives implicit receivers. Not
                    # Context-homed, so no ctx-patch. (Completes the self-migration:
                    # get_self_reg was the implicit path; this is the explicit path.)
                    self.emit_ABC(OP_LD_SELF, target_reg, 0, 0)
                else:
                    idx = len(self.instructions)
                    self.emit_ABC(OP_LD_LEXICAL, target_reg, depth, reg_idx)
                    # A cross-frame read is captured (except `self`, handled above,
                    # and inlined-loop vars, which stay lexical/home_ctx-homed — a
                    # documented deferred limitation); the post-pass rewrites captured
                    # ones to OP_LD_CTX.
                    self._ctx_patch.append(
                        (idx, False, self.current_scope.defining_frame_of(name),
                         name, depth, target_reg))
            elif res[0] == "slot":
                slot_idx, depth = res[1], res[2]
                base_reg = self.get_self_reg()
                if depth == 0:
                    self.emit_ABC(OP_LD_SLOT, target_reg, slot_idx, base_reg)
                else:
                    # Walk outer chain. A class-side `self` is a BoundClass whose
                    # enclosing instance is at slot 1, not slot 0 — so the FIRST hop
                    # out of a class-side self reads slot 1; every later hop (and all
                    # instance-side hops) read slot 0. Mirrors the `outer C` path.
                    prev_reg = base_reg
                    for w in range(depth):
                        next_reg = self.alloc_reg()
                        outer_slot = 1 if (w == 0 and self.class_side_qname is not None) else 0
                        self.emit_ABC(OP_LD_SLOT, next_reg, outer_slot, prev_reg)
                        prev_reg = next_reg
                    self.emit_ABC(OP_LD_SLOT, target_reg, slot_idx, prev_reg)
                    # Reclaim outer walk registers
                    self.next_free_reg -= depth
                self.free_self_reg(base_reg)
            elif res[0] == "nested_class":
                # A nested class is stored in a slot of its enclosing class.
                # Walk `hops` outer links to reach that class, then read its slot.
                child_qname, hops, container_qname = res[1]
                base_reg = self.get_self_reg()
                prev_reg = base_reg
                for _ in range(hops):
                    next_reg = self.alloc_reg()
                    self.emit_ABC(OP_LD_SLOT, next_reg, 0, prev_reg)  # outer = slot 0
                    prev_reg = next_reg
                bare = child_qname.rsplit(".", 1)[-1]
                slot_idx = self.parent.class_slots[container_qname].index(bare)
                self.emit_ABC(OP_LD_SLOT, target_reg, slot_idx, prev_reg)
                self.next_free_reg -= hops
                self.free_self_reg(base_reg)
            elif res[0] == "implicit_send":
                cidx = self.add_constant(name)
                rcvr_reg = self.alloc_reg()
                # Inside a block, the implicit receiver is the HOME method's self,
                # not the running block closure (whose R0 is the closure itself).
                # get_self_reg() walks the home chain; copy its result into our
                # receiver register.
                self_reg = self.get_self_reg()
                self.emit_ABC(OP_LD_REG, rcvr_reg, self_reg, 0)
                self.free_self_reg(self_reg)
                self.emit_ABC(OP_SEND_IMPLICIT, 0, cidx, rcvr_reg) # dest=0 (argc), src1=cidx, src2=rcvr_reg
                self.emit_ABC(OP_LD_REG, target_reg, rcvr_reg, 0) # Copy result from rcvr_reg
                self.free_reg()
            elif res[0] == "platform_global":
                # `Name` (capitalised library global) => `platform Name`: a unary
                # send to the handed platform capability. platform is loaded via
                # the ordinary varRef path (local/lexical/slot), so it is reached
                # through the lexically-threaded slot, not a VM global. send()
                # intercepts the capitalised selector (PRIM_Array etc.) and
                # returns the class object (mock today; real .ns class in step 3).
                plat_reg = self.alloc_reg()
                self.compile_expr({"type": "varRef", "name": "platform"}, plat_reg)
                sel_idx = self.add_constant(name)
                self.emit_ABC(OP_SEND, 0, sel_idx, plat_reg)
                self.emit_ABC(OP_LD_REG, target_reg, plat_reg, 0)
                self.free_reg()
                
        elif et == "assign":
            target = expr["target"]
            self.compile_expr(expr["value"], target_reg)
            res = self.resolve_name(target)
            if res[0] == "local":
                local_reg = res[1]
                # If captured, the post-pass rewrites this to OP_ST_CTX
                # (store into the heap Context slot instead of the register).
                idx = len(self.instructions)
                self.emit_ABC(OP_LD_REG, local_reg, target_reg, 0)
                self._ctx_patch.append((idx, True, self, target, 0, target_reg))
            elif res[0] == "slot":
                slot_idx, depth = res[1], res[2]
                base_reg = self.get_self_reg()
                if depth == 0:
                    self.emit_ABC(OP_ST_SLOT, target_reg, slot_idx, base_reg)
                else:
                    prev_reg = base_reg
                    for w in range(depth):
                        next_reg = self.alloc_reg()
                        # class-side self: first hop reads slot 1 (see slot-read above)
                        outer_slot = 1 if (w == 0 and self.class_side_qname is not None) else 0
                        self.emit_ABC(OP_LD_SLOT, next_reg, outer_slot, prev_reg)
                        prev_reg = next_reg
                    self.emit_ABC(OP_ST_SLOT, target_reg, slot_idx, prev_reg)
                    self.next_free_reg -= depth
                self.free_self_reg(base_reg)
            elif res[0] == "nested_class":
                # Storing into a nested class slot is not allowed in Newspeak,
                # but handle it symmetrically with the read path if needed.
                child_qname, hops, container_qname = res[1]
                base_reg = self.get_self_reg()
                prev_reg = base_reg
                for _ in range(hops):
                    next_reg = self.alloc_reg()
                    self.emit_ABC(OP_LD_SLOT, next_reg, 0, prev_reg)
                    prev_reg = next_reg
                bare = child_qname.rsplit(".", 1)[-1]
                slot_idx = self.parent.class_slots[container_qname].index(bare)
                self.emit_ABC(OP_ST_SLOT, target_reg, slot_idx, prev_reg)
                self.next_free_reg -= hops
                self.free_self_reg(base_reg)
            elif res[0] == "lexical":
                reg_idx, depth = res[1], res[2]
                assert depth < 256 and reg_idx < 256
                idx = len(self.instructions)
                self.emit_ABC(OP_ST_LEXICAL, target_reg, depth, reg_idx)
                # Cross-frame store -> captured; post-pass rewrites to OP_ST_CTX.
                self._ctx_patch.append(
                    (idx, True, self.current_scope.defining_frame_of(target),
                     target, depth, target_reg))

        elif et == "dynArray":
            # `{ e1. e2. ... }` builds an Array, evaluating each element.
            # Lowered to `(Array new: n) at: i put: ei` for each element.
            elements = expr["elements"]
            n = len(elements)
            arr_reg = self.alloc_reg()
            # Acquire the Array class the same way a bare `Array` reference does:
            # through the handed platform (platform_global) when one is in scope,
            # else the legacy implicit-send. Keeps `{ ... }` literals consistent
            # with explicit `Array new:` resolution.
            self.compile_expr({"type": "varRef", "name": "Array"}, arr_reg)
            size_reg = self.alloc_reg()
            size_cidx = self.add_constant(f"SMI:{n}")
            self.emit_ABx(OP_LD_CONST, size_reg, size_cidx)
            new_idx = self.add_constant("new:")
            self.emit_ABC(OP_SEND, 1, new_idx, arr_reg)                  # arr_reg = Array new: n
            self.free_reg()  # size_reg
            # at: i put: ei  (1-based). Use a scratch receiver copy each time so
            # the array stays in arr_reg (at:put: returns the stored value).
            atput_idx = self.add_constant("at:put:")
            for i, elem in enumerate(elements):
                rcvr_reg = self.alloc_reg()
                self.emit_ABC(OP_LD_REG, rcvr_reg, arr_reg, 0)
                idx_reg = self.alloc_reg()
                idx_cidx = self.add_constant(f"SMI:{i + 1}")
                self.emit_ABx(OP_LD_CONST, idx_reg, idx_cidx)
                elem_reg = self.alloc_reg()
                self.compile_expr(elem, elem_reg)
                # Receiver is the fresh `Array new:` above, so OP_AT_PUT's fast
                # path always applies; the selector const keeps the fallback a
                # correct plain `at:put:` send regardless.
                self.emit_ABC(OP_AT_PUT, 2, atput_idx, rcvr_reg)
                self.free_reg(); self.free_reg(); self.free_reg()
            self.emit_ABC(OP_LD_REG, target_reg, arr_reg, 0)
            self.free_reg()  # arr_reg

        elif et == "send":
            receiver = expr["receiver"]
            selector = expr["selector"]
            args = expr["args"]

            # `->` is reserved compile-time sugar (Milestone B Task 5), not an
            # Object method: NO builtin receiver delegates to kernel Object
            # (Task 2 finding — every builtin vtable hardcodes delegate=NULL),
            # so `a -> b` can never be an ordinary send if it is to work for
            # SMI/Symbol/String/etc. receivers. Rewrite the AST node in place
            # to a keyword send `Association key: <receiver> value: <arg>` and
            # recurse — receiver-agnostic, since it never dispatches `->` on
            # anything. `Association` is a bare capitalised varRef, so it goes
            # through the ordinary library-global lookup (resolve_name step 4:
            # `platform Association`, the same reachability path already used
            # for Array/OrderedCollection/Dictionary/etc. from arbitrary user
            # code) — no new VM machinery needed. Evaluation order is
            # preserved: the synthetic receiver (`Association`, no user-visible
            # side effects) resolves first, then the original left operand,
            # then the original right operand, each exactly once, matching the
            # normal send/keyword-args compilation below.
            if selector == "->" and len(args) == 1:
                rewritten = {
                    "type": "send",
                    "receiver": {"type": "varRef", "name": "Association", "span": expr.get("span")},
                    "selector": "key:value:",
                    "args": [receiver, args[0]],
                    "span": expr.get("span"),
                }
                self.compile_expr(rewritten, target_reg)
                return

            # Outer-send target: `outer C` (a unary send whose receiver is the
            # bare identifier `outer` and whose selector C is a nested class in
            # lexical scope) yields the lexically-enclosing INSTANCE of class C.
            # The grammar parses `outer C sel` as `((outer C) sel)`, so the inner
            # `outer C` lands here and `sel` is then a normal send to the result.
            # resolve_class_ref returns `hops` = walks from self to the CONTAINER
            # of C; the enclosing C instance is one level inside that, i.e.
            # hops-1 outer-slot (slot 0) walks from self. (hops==1 -> C is self's
            # own class -> 0 walks -> self.)
            if (receiver["type"] == "varRef" and receiver.get("name") == "outer"
                    and len(args) == 0):
                cref = self.parent.resolve_class_ref(selector)
                if cref is not None:
                    _child_qname, hops, _container_qname = cref
                    walks = hops - 1 if hops > 0 else 0
                    base_reg = self.get_self_reg()
                    prev_reg = base_reg
                    for w in range(walks):
                        nxt = self.alloc_reg()
                        # In a class-side method invoked via a BoundClass, `self`
                        # is the BoundClass whose enclosing instance is in slot 1
                        # (not slot 0). So the FIRST hop out of a class-side self
                        # reads slot 1; every subsequent hop walks ordinary
                        # instance outers (slot 0). Instance-side selves are
                        # ordinary instances: all hops read slot 0.
                        outer_slot = 1 if (w == 0 and self.class_side_qname is not None) else 0
                        self.emit_ABC(OP_LD_SLOT, nxt, outer_slot, prev_reg)
                        prev_reg = nxt
                    self.emit_ABC(OP_LD_REG, target_reg, prev_reg, 0)
                    if walks:
                        self.next_free_reg -= walks
                    self.free_self_reg(base_reg)
                    return

            # Super-send: `super sel: args` dispatches `sel` starting at the
            # DEFINING class's delegate (superclass), not the receiver's own
            # vtable, with self unchanged. The defining class is the one whose
            # body lexically contains this send (class_stack top, preserved
            # while compiling blocks), so its delegate is a compile-time VTABLE
            # constant. Emitted as OP_SEND_SUPER with register layout
            # [base]=start vtable, [base+1]=self, [base+2..]=args.
            if receiver["type"] == "varRef" and receiver.get("name") == "super":
                defining = self.parent.class_stack[-1] if self.parent.class_stack else None
                deleg = self.parent.delegate_qname(defining) if defining else None
                if deleg is not None:
                    saved_free = self.next_free_reg
                    self_src = self.get_self_reg()
                    base = self.alloc_reg()
                    vt_cidx = self.add_constant(f"VTABLE:{self.parent.vt_id(deleg)}")
                    self.emit_ABx(OP_LD_CONST, base, vt_cidx)       # [base] = start vtable
                    self_reg = self.alloc_reg()
                    self.emit_ABC(OP_LD_REG, self_reg, self_src, 0)  # [base+1] = self
                    self.next_free_reg = self_reg + 1
                    for arg in args:
                        areg = self.alloc_reg()
                        self.compile_expr(arg, areg)
                        self.next_free_reg = areg + 1                # keep args contiguous
                    cidx = self.add_constant(selector)
                    self.emit_ABC(OP_SEND_SUPER, len(args), cidx, base)
                    self.emit_ABC(OP_LD_REG, target_reg, base, 0)    # result is in [base]
                    self.next_free_reg = saved_free
                    return

            # Phase B: if-family and and:/or: are ALWAYS lowered to branch +
            # OP_BLOCK_INVOKE. Literal zero-arg block arms inline (fast path);
            # any other arm is invoked via OP_BLOCK_INVOKE inside
            # emit_block_value. The recursive PRIM_ifTrue/PRIM_ifFalse/
            # PRIM_ifTrueIfFalse/PRIM_and/PRIM_or paths are retired (Phase 1
            # de-recursify).
            if selector in ("ifTrue:", "ifFalse:", "ifTrue:ifFalse:", "ifFalse:ifTrue:"):
                self.compile_if_inline(selector, receiver, args, target_reg)
                return

            # Phase C: short-circuit `and:` / `or:`.
            if selector in ("and:", "or:") and len(args) == 1:
                self.compile_logic_inline(selector, receiver, args[0], target_reg)
                return

            # to:do: — always lowered. Literal one-param body inlines; any other
            # body is OP_BLOCK_INVOKE'd as `body value: i`.
            if selector == "to:do:" and len(args) == 2:
                self.compile_to_do_inline(receiver, args[0], None, args[1], target_reg)
                return

            # to:by:do: — literal nonzero-int step keeps the compile-time-direction
            # fast path; a non-literal (or non-int) step uses the runtime-direction
            # lowering. `by: 0` still yields zero iterations (guarded below).
            if selector == "to:by:do:" and len(args) == 3:
                step_arg = args[1]
                if (step_arg["type"] == "literal"
                        and step_arg.get("litKind") == "int"
                        and int(step_arg["value"]) != 0):
                    self.compile_to_do_inline(receiver, args[0], step_arg, args[2], target_reg)
                else:
                    self.compile_to_by_do_runtime(receiver, args[0], step_arg, args[2], target_reg)
                return

            # whileTrue:/whileFalse: — always lowered to a bytecode loop. Literal
            # zero-arg cond/body blocks inline; non-literal ones are OP_BLOCK_INVOKE'd.
            if selector in ("whileTrue:", "whileFalse:") and len(args) == 1:
                self.compile_loop_inline(selector, receiver, args[0], target_reg)
                return

            # timesRepeat: — always lowered to a counting loop driving OP_BLOCK_INVOKE.
            if selector == "timesRepeat:" and len(args) == 1:
                self.compile_times_repeat(receiver, args[0], target_reg)
                return

            # Unary whileTrue/whileFalse (no body) — always lowered to a
            # bytecode loop over the condition block alone.
            if selector in ("whileTrue", "whileFalse") and len(args) == 0:
                self.compile_while_unary(selector, receiver, target_reg)
                return

            # Implicit-receiver send (no explicit receiver in source, e.g. `foo: x`).
            # Resolved lexically/by inheritance at runtime via OP_SEND_IMPLICIT,
            # with `self` as the starting receiver.
            if receiver["type"] == "implicitReceiver":
                rcvr_reg = self.alloc_reg()
                # Inside a block, the implicit receiver is the HOME method's self,
                # not the running block closure (whose R0 is the closure itself).
                # get_self_reg() walks the home chain back to the method's self.
                self_reg = self.get_self_reg()
                self.emit_ABC(OP_LD_REG, rcvr_reg, self_reg, 0)
                self.free_self_reg(self_reg)
                old_free = self.next_free_reg
                self.next_free_reg = rcvr_reg + 1
                for arg in args:
                    areg = self.alloc_reg()
                    self.compile_expr(arg, areg)
                    self.next_free_reg = areg + 1  # keep args contiguous (see normal send)
                cidx = self.add_constant(selector)
                self.emit_ABC(OP_SEND_IMPLICIT, len(args), cidx, rcvr_reg)
                self.emit_ABC(OP_LD_REG, target_reg, rcvr_reg, 0)
                self.next_free_reg = old_free
                return

            # Optimization: Direct arithmetic opcodes for +, -, *, /
            if selector in ["+", "-", "*", "/"] and len(args) == 1:
                rcvr_reg = self.alloc_reg()
                self.compile_expr(receiver, rcvr_reg)
                arg_reg = self.alloc_reg()
                self.compile_expr(args[0], arg_reg)
                
                op_map = {"+": OP_ADD, "-": OP_SUB, "*": OP_MUL, "/": OP_DIV}
                self.emit_ABC(op_map[selector], target_reg, rcvr_reg, arg_reg)
                
                self.free_reg()
                self.free_reg()
                return

            # Special case: a send whose receiver is a class. This is either a
            # class-side (metaclass) method call or an instantiation.
            #   - A bare class name resolved in lexical scope is the class.
            #   - Inside a class-side method, `self` IS the class object, so
            #     `self new` instantiates the enclosing class.
            class_ref = self.parent.resolve_class_ref(receiver["name"]) if receiver["type"] == "varRef" else None
            cls_qname = None
            hops = 0
            self_is_class = False
            if class_ref is not None:
                cls_qname = class_ref[0]
                hops = class_ref[1]
            elif (receiver["type"] == "varRef" and receiver["name"] == "self"
                  and self.class_side_qname is not None):
                cls_qname = self.class_side_qname
                self_is_class = True

            # Class-side method dispatch: `Class classSideSelector` (or
            # `self classSideSelector` within a class-side method) sends to the
            # class OBJECT, whose header vtable is the metaclass. Distinct from
            # the factory/instantiation path below (e.g. `Packet create:...`).
            if cls_qname is not None and self.parent.class_side_has(cls_qname, selector):
                rcvr_reg = self.alloc_reg()
                if self_is_class:
                    self.emit_ABC(OP_LD_REG, rcvr_reg, 0, 0)  # self (the class)
                elif class_ref is not None and cls_qname in self.parent.nested_classes_flat:
                    # Nested class: send to the BoundClass stored in the enclosing
                    # instance's slot, NOT the raw vtable. The BoundClass carries the
                    # outer reference at slot 1, which class-side methods use to walk
                    # back to enclosing-instance slots (e.g. `^ secret` at depth 1).
                    # Sending to a raw vtable instead would put a Vtable struct in R0,
                    # making slot[1] = vtable.bindings (garbage), not the outer instance.
                    _child_qname, bc_hops, bc_container = class_ref
                    bare = cls_qname.rsplit(".", 1)[-1]
                    bc_slot_idx = self.parent.class_slots[bc_container].index(bare)
                    base_reg = self.get_self_reg()
                    prev_reg = base_reg
                    for w in range(bc_hops):
                        nxt = self.alloc_reg()
                        outer_slot = 1 if (w == 0 and self.class_side_qname is not None) else 0
                        self.emit_ABC(OP_LD_SLOT, nxt, outer_slot, prev_reg)
                        prev_reg = nxt
                    self.emit_ABC(OP_LD_SLOT, rcvr_reg, bc_slot_idx, prev_reg)
                    if bc_hops:
                        self.next_free_reg -= bc_hops
                    self.free_self_reg(base_reg)
                else:
                    # Non-nested (root-level) class: load raw vtable. No outer
                    # reference is needed since root classes have no enclosing instance.
                    vtcidx = self.add_constant(f"VTABLE:{self.parent.vt_id(cls_qname)}")
                    self.emit_ABx(OP_LD_CONST, rcvr_reg, vtcidx)
                old_free = self.next_free_reg
                self.next_free_reg = rcvr_reg + 1
                for arg in args:
                    areg = self.alloc_reg()
                    self.compile_expr(arg, areg)
                    self.next_free_reg = areg + 1
                cidx = self.add_constant(selector)
                self.emit_ABC(OP_SEND, len(args), cidx, rcvr_reg)
                self.emit_ABC(OP_LD_REG, target_reg, rcvr_reg, 0)
                self.next_free_reg = old_free
                return

            if cls_qname is not None:
                # It is a class construction!
                # Allocate using OP_NEW
                cls_slots = self.parent.class_slots[cls_qname]
                cidx = self.add_constant(f"VTABLE:{self.parent.vt_id(cls_qname)}")
                vreg = self.alloc_reg()
                self.emit_ABx(OP_LD_CONST, vreg, cidx)
                self.emit_ABC(OP_NEW, target_reg, vreg, len(cls_slots))
                self.free_reg()

                # If the class is nested, set its outer (slot 0) to the instance
                # of its lexically-enclosing class. That enclosing instance is
                # reached by walking `hops` outer links from self — NOT always
                # self itself. resolve_class_ref returns hops = how many levels
                # up from the current class to the container that defines the
                # nested class. hops==0 means it is defined in the current class,
                # so self IS the enclosing instance. hops>0 happens when the
                # nested class lives in an ANCESTOR (e.g. instantiating a sibling
                # class that shares the same container): we must walk up to that
                # container's instance, else the new object's outer points at the
                # wrong frame and outer-slot access reads garbage.
                if cls_qname in self.parent.nested_classes_flat:
                    if self_is_class:
                        # Instantiating from a class-side method: `self` (R0) is
                        # the BoundClass object. BoundClass.slot[1] = the enclosing
                        # instance (outer) threaded at access time. Read it and store
                        # into the new object's outer slot (slot 0), so that the
                        # initializer can walk the outer chain to reach threaded params
                        # such as `platform`. (Previously this stored nil, which worked
                        # only because no initializer needed to walk the outer chain.)
                        outer_reg = self.alloc_reg()
                        self.emit_ABC(OP_LD_SLOT, outer_reg, 1, 0)  # BoundClass.slot[1] = outer
                        self.emit_ABC(OP_ST_SLOT, outer_reg, 0, target_reg)  # new_obj.outer = outer
                        self.free_reg()
                    elif hops == 0:
                        self.emit_ABC(OP_ST_SLOT, 0, 0, target_reg) # self (0) -> slot 0
                    else:
                        prev_reg = self.alloc_reg()
                        self.emit_ABC(OP_LD_REG, prev_reg, 0, 0)  # self
                        for _ in range(hops):
                            nxt = self.alloc_reg()
                            self.emit_ABC(OP_LD_SLOT, nxt, 0, prev_reg)  # outer = slot 0
                            prev_reg = nxt
                        self.emit_ABC(OP_ST_SLOT, prev_reg, 0, target_reg)
                        self.next_free_reg -= (hops + 1)

                # Call initializer
                params = self.parent.all_classes[cls_qname].get("params", [])
                init_sel = "".join(f"{p}:" for p in params)
                if not init_sel:
                    init_sel = "new"

                # Prepare arguments starting at target_reg + 1
                old_free = self.next_free_reg
                self.next_free_reg = target_reg + 1
                for arg in args:
                    areg = self.alloc_reg()
                    self.compile_expr(arg, areg)
                    self.next_free_reg = areg + 1  # keep args contiguous (see normal send)

                # Send initialize message
                init_selector = f"init_{init_sel}"
                init_cidx = self.add_constant(init_selector)
                self.emit_ABC(OP_SEND, len(args), init_cidx, target_reg)
                self.next_free_reg = old_free
                return
            
            # Normal send: evaluate receiver
            rcvr_reg = self.alloc_reg()
            self.compile_expr(receiver, rcvr_reg)
            
            # Evaluate arguments contiguously starting at rcvr_reg + 1.
            # After each arg, reset next_free_reg to areg+1: if the arg was
            # itself a send it left next_free_reg above its own scratch, which
            # would push the NEXT arg to a non-contiguous register and feed the
            # VM a stale scratch value in the gap. The arg's result sits in areg
            # (below the reclaimed scratch), so reclaiming is safe.
            old_free = self.next_free_reg
            self.next_free_reg = rcvr_reg + 1
            for arg in args:
                areg = self.alloc_reg()
                self.compile_expr(arg, areg)
                self.next_free_reg = areg + 1

            cidx = self.add_constant(selector)
            send_op = OP_BLOCK_INVOKE if self._is_block_value_selector(selector) else send_op_for(selector)
            self.emit_ABC(send_op, len(args), cidx, rcvr_reg)
            self.emit_ABC(OP_LD_REG, target_reg, rcvr_reg, 0)
            self.next_free_reg = old_free
            
        elif et == "block":
            # 1. Compile block as a separate method
            blk_idx = self.parent.alloc_block_id()
            blk_name = f"{self.name}_block{blk_idx}"
            blk_selector = f"value" + "".join(":" for _ in expr["params"])
            
            # Block parameters are arguments
            blk_compiler = MethodCompiler(blk_name, blk_selector, expr["params"], expr.get("temps", []), self.parent, outer_compiler=self)
            
            # Add implicit NLR return if needed
            for stmt in expr["body"][:-1]:
                blk_compiler.compile_statement(stmt)
            if expr["body"]:
                last_stmt = expr["body"][-1]
                if last_stmt["type"] == "return":
                    blk_compiler.compile_statement(last_stmt)
                else:
                    ret_reg = blk_compiler.alloc_reg()
                    blk_compiler.compile_expr(last_stmt, ret_reg)
                    blk_compiler.emit_ABC(OP_RET, ret_reg, 0, 0)
                    blk_compiler.free_reg()
            else:
                ret_reg = blk_compiler.alloc_reg()
                cidx = blk_compiler.add_constant(None)
                blk_compiler.emit_ABx(OP_LD_CONST, ret_reg, cidx)
                blk_compiler.emit_ABC(OP_RET, ret_reg, 0, 0)
                blk_compiler.free_reg()

            blk_compiler.finalize_jumps()

            # Register block method in parent compiler
            self.parent.block_methods.append(blk_compiler)
            
            # 2. Emit instantiating the block closure
            cidx = self.add_constant(f"METHOD:{blk_name}:{blk_selector}")
            assert cidx < 0x10000, "closure method-const index exceeds 16-bit ABx field"
            self.emit_ABx(OP_CLOSURE, target_reg, cidx)


class OevmCompiler:
    def __init__(self, ast, oop_bytes=8):
        self.ast = ast
        # Target oop (ref-word) width in bytes: the same source of truth
        # oimg_serialize.py's OimgSerializer reads (`self.oop = layout["oop_size"]`).
        # Path A (--format=c, no --layout) runs as native C on the host, so it
        # defaults to the host's own width (8). Path B (--format=oimg) passes
        # layout["oop_size"] in from main() (nsl-tuvm#4: compile_expr's
        # SMI/LARGEINT split must agree with the ref-word's true capacity).
        self.oop_bytes = oop_bytes
        self.solidify = SolidifyCompiler()
        # Per-method source maps, keyed "Vtable>>selector". Collected during
        # compile(); serialized to a .stmap sidecar by main().
        self.source_maps = {}
        # Per-frame MethodCompiler objects, keyed "Vtable>>selector" (same
        # keying as source_maps). Pure bookkeeping for introspection (e.g. the
        # de-recursify Task 3 capture analysis, tests) — never consulted by
        # codegen, and not part of the emitted .oimg.
        self.method_compilers = {}

        # Patch get_symbol_ref first so all symbols get StaticSymbol_ prefix
        def patched_get_symbol_ref(name):
            if name.startswith("VTABLE:"):
                vt_name = name.split(":")[1]
                return f"(oop)&Vtable_{vt_name}"
            if name.startswith("METHOD:"):
                parts = name.split(":")
                blk_name = parts[1]
                selector = ":".join(parts[2:])
                sanitized_sel = self.solidify.sanitize_symbol_name(selector)
                mid = f"Method_Block_{blk_name}_{sanitized_sel}"
                return f"(oop)&{mid}_compiled"
            # For standard symbols, map them to StaticSymbol_xxx
            if name not in self.solidify.symbols:
                sanitized = self.solidify.sanitize_symbol_name(name)
                self.solidify.symbols[name] = f"StaticSymbol_{sanitized}"
            return f"(oop)&{self.solidify.symbols[name]}"
        self.solidify.get_symbol_ref = patched_get_symbol_ref

        self.solidify.add_vtable("IntegerVtable")
        self.solidify.add_vtable("FloatVtable")
        self.solidify.add_vtable("LargeIntegerVtable")
        self.solidify.add_vtable("MethodContextVtable")
        self.solidify.get_symbol_ref("value")
        self.class_slots = {}
        # qname -> own (pre-inheritance-merge) slot layout, and qname -> resolved
        # superclass qname. Captured by collect_slots; consumed by
        # finalize_slot_layouts to re-merge inherited layouts order-independently.
        self.class_own_slots = {}
        self.class_parent = {}
        self.nested_classes = {}
        self.nested_classes_flat = set()
        # qname -> set of class-side (metaclass) method selectors. Lets the send
        # compiler distinguish `Class classSideMethod` (dispatch to the class
        # object) from `Class factorySelector` (instantiate).
        self.class_side_methods = {}
        self.class_stack = []
        self.block_count = 0
        self.block_methods = []
        self.all_classes = {}

        # First pass: map class hierarchies and fields
        self.map_classes(self.ast)
        # Re-merge inherited slot layouts order-independently (collect_slots'
        # in-line merge only works when the superclass is declared first).
        self.finalize_slot_layouts()

    def alloc_block_id(self):
        self.block_count += 1
        return self.block_count

    def register_source_map(self, name, source_map):
        # Store only non-empty maps; synthetic methods (accessors, default init)
        # have no source statements and are intentionally omitted.
        if source_map:
            self.source_maps[name] = source_map

    def register_method_compiler(self, name, compiler):
        self.method_compilers[name] = compiler

    def map_classes(self, node):
        # Classes are keyed by a qualified, dotted name (the lexical path from
        # the root module) so that same-named classes in different scopes — e.g.
        # an outer `Sieve` and an inner `Sieve` — never collide. Bare names are
        # only used for scope-aware reference resolution (resolve_class_ref).
        if node["type"] == "program":
            for child in node["body"]:
                self.map_classes(child)
        elif node["type"] == "module":
            node["qname"] = node["name"]
            self.all_classes[node["qname"]] = node
            self.class_side_methods[node["qname"]] = {
                m["selector"] for m in node.get("classMethods", [])}
            self.collect_slots(node, is_root=True)
            for child in node.get("classes", []):
                self.map_nested(child, node["qname"])

    def map_nested(self, node, parent_qname):
        qname = f"{parent_qname}.{node['name']}"
        node["qname"] = qname
        self.all_classes[qname] = node
        self.class_side_methods[qname] = {
            m["selector"] for m in node.get("classMethods", [])}
        self.nested_classes_flat.add(qname)
        self.nested_classes.setdefault(parent_qname, []).append(qname)
        self.collect_slots(node, is_root=False, container_qname=parent_qname)
        for child in node.get("classes", []):
            self.map_nested(child, qname)

    def collect_slots(self, node, is_root, container_qname=None):
        slots = []
        if not is_root:
            slots.append("outer")
        slots += node.get("params", [])
        slots += [s["name"] for s in node.get("slots", [])]
        # Nested classes also occupy slots of their outer class (keyed by their
        # bare name, since that is how they are referenced from source).
        for nested in node.get("classes", []):
            slots.append(nested["name"])
        # Record the OWN (pre-merge) layout so finalize_slot_layouts can re-merge
        # the inherited prefix in dependency order (the in-line merge below is
        # order-dependent and silently skipped when the parent is declared later).
        self.class_own_slots[node["qname"]] = list(slots)

        # If this class inherits from a sibling (via superInit), the child must
        # use the same slot layout as the parent so that inherited methods —
        # which index slots by position — see the correct slot indices.
        # The parent must already have been processed (it appears earlier in the
        # class body, which map_nested processes in order).
        #
        # NOTE — two known limitations of this merge:
        #
        # (a) Source-order requirement: the parent class must be declared
        #     TEXTUALLY BEFORE the child within the same container class.
        #     map_nested processes siblings in declaration order, so if the
        #     parent appears later the parent's entry in class_slots will not
        #     yet exist when the child is processed and the merge is silently
        #     skipped.  The child then keeps its own (potentially narrower)
        #     slot layout.  This is intentional: superclasses resolved outside
        #     the sibling set (e.g. from an enclosing scope) are never found
        #     here and must still compile without error.  Do NOT turn the
        #     "not found" case into a hard error — that would break classes
        #     whose superclass lives outside this container.
        #
        # (b) Single-level merge only: we copy the immediate parent's layout.
        #     If the parent itself inherited from a grandparent the grandparent's
        #     layout is already baked into the parent's recorded slots, so
        #     transitive inheritance is handled correctly *for slots* — but see
        #     the matching limitation in compile_initializer for init-chain depth.
        super_name = node.get("super")
        if super_name and container_qname is not None:
            parent_qname_candidate = f"{container_qname}.{super_name}"
            self.class_parent[node["qname"]] = parent_qname_candidate
            parent_slots = self.class_slots.get(parent_qname_candidate)
            if parent_slots is not None:
                # Start from the parent layout and append any child-only names
                # (nested class refs, extra own slots) that aren't in the parent.
                merged = list(parent_slots)
                for s in slots:
                    if s not in merged:
                        merged.append(s)
                slots = merged

        # Navigating superclass (super/self/outer head) -> a possibly
        # cross-container delegate. Resolve it once here and cache it on the node
        # (compile_class reuses it), and inherit the delegate's slot layout so
        # super-dispatched methods see the inherited slots at the same indices.
        super_expr = node.get("superExpr")
        if super_expr and container_qname is not None:
            target = self.resolve_super_expr(container_qname, super_expr)
            node["_superTarget"] = target
            if target is not None:
                self.class_parent[node["qname"]] = target
                parent_slots = self.class_slots.get(target)
                if parent_slots is not None:
                    merged = list(parent_slots)
                    for s in slots:
                        if s not in merged:
                            merged.append(s)
                    slots = merged

        self.class_slots[node["qname"]] = slots

    def finalize_slot_layouts(self):
        # Re-merge every class's slot layout from its superclass's, to a fixpoint.
        # The in-line merge in collect_slots is order-dependent: a class declared
        # textually BEFORE its superclass (e.g. deltablue's BinaryConstraint,
        # declared before its parent Constraint) misses the merge and keeps a
        # narrower layout, so inherited slots land at the wrong indices. Here we
        # recompute layout(C) = layout(super(C)) + C's own-only names until stable,
        # which is order-independent and propagates multi-level chains.
        changed = True
        guard = 0
        while changed and guard < 1000:
            changed = False
            guard += 1
            for qname, parent_qname in self.class_parent.items():
                parent_layout = self.class_slots.get(parent_qname)
                if not parent_layout:
                    continue
                merged = list(parent_layout)
                for s in self.class_own_slots.get(qname, []):
                    if s not in merged:
                        merged.append(s)
                if merged != self.class_slots.get(qname):
                    self.class_slots[qname] = merged
                    changed = True

    @staticmethod
    def vt_id(qname):
        """C-safe vtable/method identifier for a qualified class name."""
        return qname.replace(".", "_")

    def resolve_class_ref(self, name):
        """Resolve a bare class name against the current lexical class scope.

        Returns (child_qname, hops, container_qname) where `hops` is the number
        of `outer` (slot 0) walks from the current `self` out to the enclosing
        class that *contains* the referenced nested class. Returns None if the
        name is not a user class visible in scope."""
        stack = self.class_stack
        for hops, container_qname in enumerate(reversed(stack)):
            for child_qname in self.nested_classes.get(container_qname, []):
                if child_qname.rsplit(".", 1)[-1] == name:
                    return (child_qname, hops, container_qname)
        return None

    def super_qname(self, cls_qname):
        """Resolve a class's superclass to a qname, or None. Only resolves
        sibling supers (same container), mirroring compile_initializer — that
        is the layout the rest of the compiler relies on. `Object` (the VM
        root) and out-of-container supers return None."""
        node = self.all_classes.get(cls_qname)
        if not node:
            return None
        sup = node.get("super")
        if not sup or sup == "Object":
            return None
        container = cls_qname.rsplit(".", 1)[0] if "." in cls_qname else ""
        cand = f"{container}.{sup}" if container else sup
        return cand if cand in self.all_classes else None

    def delegate_qname(self, cls_qname):
        """The resolved superclass (delegate) qname of a class, or None for no
        user superclass (or `Object`). Handles both a navigating superExpr
        (resolved to `_superTarget`) and a same-container bare-name super. Used
        to bind `super` sends to the defining class's delegate vtable."""
        node = self.all_classes.get(cls_qname)
        if not node:
            return None
        if node.get("superExpr"):
            return node.get("_superTarget")
        super_name = node.get("super")
        if not super_name or super_name == "Object":
            return None
        container = cls_qname.rsplit(".", 1)[0] if "." in cls_qname else ""
        cand = f"{container}.{super_name}" if container else super_name
        return cand if cand in self.all_classes else None

    def resolve_super_expr(self, enclosing_qname, tokens):
        """Resolve a navigating superclass expression to its delegate qname.

        `tokens` is the chain from the source clause, e.g. ['super','Inner1'],
        ['outer','InnerHierarchy','Inner1'], ['self','Inner0']. Resolution runs
        in the scope of `enclosing_qname` (the container of the class being
        defined): `super` -> that container's superclass, `self` -> the
        container, `outer X` -> the enclosing class named X (walking up). The
        remaining unary tokens then navigate into nested classes. Returns the
        delegate qname, or None if it cannot be resolved. The target may live in
        a different container than the class being defined (cross-container)."""
        head = tokens[0]
        if head == 'self':
            current, idx = enclosing_qname, 1
        elif head == 'super':
            current, idx = self.super_qname(enclosing_qname), 1
            if current is None:
                return None
        elif head == 'outer':
            if len(tokens) < 2:
                return None
            target, idx = tokens[1], 2
            current = None
            q = enclosing_qname
            while q:
                if q.rsplit(".", 1)[-1] == target:
                    current = q
                    break
                q = q.rsplit(".", 1)[0] if "." in q else None
            if current is None:
                return None
        else:
            return None
        for t in tokens[idx:]:
            cand = f"{current}.{t}"
            if cand not in self.all_classes:
                return None
            current = cand
        return current

    def class_side_has(self, cls_qname, selector):
        """True if `selector` is a class-side method of cls_qname or any
        ancestor (class-side methods are inherited via the metaclass chain)."""
        seen = set()
        q = cls_qname
        while q and q not in seen:
            seen.add(q)
            if selector in self.class_side_methods.get(q, ()):
                return True
            q = self.super_qname(q)
        return False

    def class_side_any(self, cls_qname):
        """True if cls_qname or any ancestor declares any class-side method, so
        the class needs a metaclass vtable (for inherited class-side dispatch)."""
        seen = set()
        q = cls_qname
        while q and q not in seen:
            seen.add(q)
            if self.class_side_methods.get(q):
                return True
            q = self.super_qname(q)
        return False

    def compile(self):
        # We need to compile every class starting from the root modules
        for node in self.ast.get("body", []):
            self.compile_class(node)
            
        # Add all methods (including block methods) to solidify compiler
        for blk in self.block_methods:
            # Bind blocks to a special "BlockVtable" so they can be dispatched on value
            # Wait, block closure methods are dispatched on the BytecodeClosureVtable!
            # So all blocks' value methods should be bound to BytecodeClosureVtable!
            # Since multiple blocks might have value / value: value:: methods,
            # wait! If they all bind to BytecodeClosureVtable under the same selector, they will overwrite each other!
            # Ah!
            # In OEVM/Smalltalk, block closures contain the entry_point pointer to their CompiledMethod!
            # And when a closure is dispatched, the interpreter executes the CompiledMethod directly!
            # So we don't bind block methods to a Vtable selector! Instead, they are referenced directly by their CompiledMethod pointer!
            # Yes!
            # So we can just add block methods to SolidifyCompiler as global methods (or bind them to a dummy Vtable) so they get compiled and solidified, but not bound to any class dispatch bindings!
            # Let's add them to a dummy vtable "BlockMethods" without bindings, or just add them.
            self.solidify.add_vtable(f"Block_{blk.name}")
            # Deferred lift Phase 1: safe to run now — every block is compiled
            # (self.block_methods holds them all) and every enclosing method
            # finalized during compile_class, so all referenced frames' capture
            # slots are assigned.
            blk.finalize_contexts()
            self.solidify.add_method(
                vtable_name=f"Block_{blk.name}",
                selector=blk.selector,
                num_registers=blk.max_regs_used,
                num_arguments=len(blk.args),
                constants=blk.constants,
                instructions=blk.instructions
            )
            self.register_source_map(f"{blk.name}>>{blk.selector}", blk.source_map)
            self.register_method_compiler(f"{blk.name}>>{blk.selector}", blk)

    def compile_class(self, node):
        qname = node["qname"]
        self.class_stack.append(qname)
        vt = self.vt_id(qname)

        # Add Vtable to solidify compiler. The superclass clause is a bare name
        # resolved against the enclosing lexical scope to its qualified vtable.
        is_nested = 1 if "." in qname else 0
        super_name = node.get("super")
        delegate = None
        # std-lib step 3a: `Object` is now a REAL kernel class (lib/kernel.ns).
        #   - the class named `Object` is the root: delegate NULL.
        #   - every other rootless or `= Object` heap class delegates to Vtable_Object
        #     (the universal root), so all heap objects get Object.ns protocol.
        #   - a named non-Object super resolves as before.
        if qname.rsplit(".", 1)[-1] == "Object":
            delegate = None
        elif super_name and super_name != "Object":
            sup = self.resolve_class_ref(super_name)
            delegate = self.vt_id(sup[0]) if sup else super_name
        elif super_name == "Object" or (super_name is None and not node.get("superExpr")):
            # std-lib step 3b: `Object` now lives inside the Kernel module
            # (lib/kernel.ns), so its vtable is Vtable_Kernel_Object. The simple-name
            # check above still identifies the root (delegate NULL); every other
            # rootless / `= Object` heap class delegates to the nested root.
            delegate = "Kernel_Object"
        elif node.get("superExpr"):
            # Navigating superclass (super/self/outer head) -> a possibly
            # cross-container delegate, resolved during collect_slots.
            target = node.get("_superTarget")
            if target:
                delegate = self.vt_id(target)

        # Class-side (metaclass) methods, if any, live on a separate metaclass
        # vtable `Meta_<vt>`; the class vtable's header points at it so sends to
        # the class OBJECT dispatch there. The metaclass delegates to the
        # superclass's metaclass when that superclass also has class-side
        # methods (class-side inheritance), else to the VM root.
        class_methods = node.get("classMethods", []) or []
        # A class gets a metaclass when it — or any ancestor — declares a
        # class-side method, so inherited class-side sends still dispatch.
        meta_vt = f"Meta_{vt}" if self.class_side_any(qname) else None
        self.solidify.add_vtable(vt, delegate_name=delegate, is_nested=is_nested,
                                 meta=meta_vt)
        if meta_vt:
            sup_qname = self.super_qname(qname)
            meta_delegate = (f"Meta_{self.vt_id(sup_qname)}"
                             if sup_qname and self.class_side_any(sup_qname) else None)
            self.solidify.add_vtable(meta_vt, delegate_name=meta_delegate, is_nested=0)

        # Compile all methods
        for m in node.get("methods", []):
            selector = m["selector"]
            compiler = MethodCompiler(vt, selector, m["params"], m.get("temps", []), self)

            for stmt in m["body"][:-1]:
                compiler.compile_statement(stmt)
            if m["body"]:
                compiler.compile_statement(m["body"][-1])

            compiler.finalize_jumps()

            # If method doesn't end with a return, emit return self (R0)
            if not compiler.instructions or (compiler.instructions[-1] & 0xFF) != OP_RET:
                compiler.emit_ABC(OP_RET, 0, 0, 0)

            compiler.finalize_contexts()  # deferred lift Phase 1: patch + prologue
            self.solidify.add_method(
                vtable_name=vt,
                selector=selector,
                num_registers=compiler.max_regs_used,
                num_arguments=len(m["params"]),
                constants=compiler.constants,
                instructions=compiler.instructions
            )
            self.register_source_map(f"{vt}>>{selector}", compiler.source_map)
            self.register_method_compiler(f"{vt}>>{selector}", compiler)

        # Compile class-side methods onto the metaclass vtable. `self` (R0) is
        # the class object, so `self new` inside a body instantiates this class.
        for m in class_methods:
            selector = m["selector"]
            compiler = MethodCompiler(meta_vt, selector, m["params"], m.get("temps", []), self)
            compiler.class_side_qname = qname

            for stmt in m["body"][:-1]:
                compiler.compile_statement(stmt)
            if m["body"]:
                compiler.compile_statement(m["body"][-1])

            compiler.finalize_jumps()
            if not compiler.instructions or (compiler.instructions[-1] & 0xFF) != OP_RET:
                compiler.emit_ABC(OP_RET, 0, 0, 0)

            compiler.finalize_contexts()  # deferred lift Phase 1: patch + prologue
            self.solidify.add_method(
                vtable_name=meta_vt,
                selector=selector,
                num_registers=compiler.max_regs_used,
                num_arguments=len(m["params"]),
                constants=compiler.constants,
                instructions=compiler.instructions
            )
            self.register_source_map(f"{meta_vt}>>{selector}", compiler.source_map)
            self.register_method_compiler(f"{meta_vt}>>{selector}", compiler)

        # Synthesise accessor methods for public and protected slots: a getter
        # `name`, plus a setter `name:` for mutable slots. Internal slot access
        # goes straight through OP_LD/ST_SLOT; these accessors back explicit
        # sends — `public` for any caller, `protected` for self/subclass sends
        # (e.g. deltablue's `protected satisfied ::= false.` set via
        # `satisfied: false`). The VM does not enforce send-visibility; `private`
        # slots get no accessor (direct slot ops only).
        defined = {m["selector"] for m in node.get("methods", [])}
        slots = self.class_slots[qname]
        for s in node.get("slots", []):
            if s.get("visibility") not in ("public", "protected"):
                continue
            slot_idx = slots.index(s["name"])
            getter = s["name"]
            if getter not in defined:
                mc = MethodCompiler(vt, getter, [], [], self)
                r = mc.alloc_reg()
                mc.emit_ABC(OP_LD_SLOT, r, slot_idx, 0)
                mc.emit_ABC(OP_RET, r, 0, 0)
                self.solidify.add_method(vt, getter, mc.max_regs_used, 0,
                                         mc.constants, mc.instructions)
                defined.add(getter)
            if s.get("mutable"):
                setter = f"{getter}:"
                if setter not in defined:
                    mc = MethodCompiler(vt, setter, ["value"], [], self)
                    mc.emit_ABC(OP_ST_SLOT, 1, slot_idx, 0)  # self.slots[idx] = arg
                    mc.emit_ABC(OP_RET, 0, 0, 0)             # return self
                    self.solidify.add_method(vt, setter, mc.max_regs_used, 1,
                                             mc.constants, mc.instructions)
                    defined.add(setter)

        # Synthesise nested-class accessors: for each directly-nested class C,
        # emit a method `C` on this vtable returning a BoundClass that pairs C's
        # vtable with `self` (the enclosing instance). `recv C` then resolves by
        # normal dispatch to whichever accessor sits on recv's actual vtable,
        # disambiguating same-named nested classes across containers for free.
        # The BoundClass is built by the `__boundOuter:slotCount:` primitive
        # (receiver = C's vtable, args = [self, C's slot count]); send() then
        # intercepts BoundClass receivers and performs construction.
        for nested in node.get("classes", []):
            bare = nested["name"]
            if bare in defined:
                continue
            nested_qname = nested["qname"]
            nslot_count = len(self.class_slots[nested_qname])
            mc = MethodCompiler(vt, bare, [], [], self)
            rcvr = mc.alloc_reg()
            vt_cidx = mc.add_constant(f"VTABLE:{self.vt_id(nested_qname)}")
            mc.emit_ABx(OP_LD_CONST, rcvr, vt_cidx)        # rcvr = C's vtable
            a0 = mc.alloc_reg()
            mc.emit_ABC(OP_LD_REG, a0, 0, 0)               # arg0 = self (outer)
            a1 = mc.alloc_reg()
            sc_cidx = mc.add_constant(f"SMI:{nslot_count}")
            mc.emit_ABx(OP_LD_CONST, a1, sc_cidx)          # arg1 = slot_count
            sel_cidx = mc.add_constant("__boundOuter:slotCount:")
            mc.emit_ABC(OP_SEND, 2, sel_cidx, rcvr)        # rcvr <- BoundClass
            mc.emit_ABC(OP_RET, rcvr, 0, 0)
            self.solidify.add_method(vt, bare, mc.max_regs_used, 0,
                                     mc.constants, mc.instructions)
            defined.add(bare)

        # Compile initializer method `init_<ctor_sig>`
        self.compile_initializer(node)

        # Recurse on nested classes
        for child in node.get("classes", []):
            self.compile_class(child)

        self.class_stack.pop()

    def compile_initializer(self, node):
        qname = node["qname"]
        vt = self.vt_id(qname)
        # Intializer name is init_<params_sig>
        params = node.get("params", [])
        init_sel = "".join(f"{p}:" for p in params)
        if not init_sel:
            init_sel = "new"
        selector = f"init_{init_sel}"

        # Initializer parameters are the class parameters
        compiler = MethodCompiler(vt, selector, params, [], self)

        # 0. If there is a superInit clause, initialize inherited slots inline by
        # walking the ENTIRE super chain (parent, grandparent, ...). We cannot use
        # SEND (it would recurse through the child's own vtable), so we emit each
        # ancestor's initialization logic directly: bind its params (from the
        # subclass-below's superInit args) and evaluate its own slot defaults.
        #
        # collect_slots already merged the whole chain's slot LAYOUT, so every
        # ancestor slot sits at a consistent index (each ancestor's layout is a
        # prefix of its subclass's). Args are evaluated in the child's context:
        # the child's own params are live registers, and an intermediate
        # ancestor's params have been stored to their slots by the previous
        # iteration, so name resolution finds them there. (Deltablue needs this:
        # ScaleConstraint -> BinaryConstraint -> Constraint, where Constraint's
        # `strength` slot would otherwise never be initialized.)
        def _parent_qname_of(n):
            sn = n.get("super")
            if sn:
                return f"{'.'.join(n['qname'].split('.')[:-1])}.{sn}"
            if n.get("superExpr"):
                return n.get("_superTarget")
            return None

        subclass = node
        while subclass.get("superInit"):
            parent_qname = _parent_qname_of(subclass)
            if parent_qname is None:
                break
            parent_node = self.all_classes.get(parent_qname)
            if parent_node is None:
                break
            parent_slots_list = self.class_slots.get(parent_qname, [])
            # Bind this ancestor's params from the subclass-below's superInit args.
            # NOTE: step 1 below will also store any params shared between an
            # ancestor and the child (same name, same slot index) — that re-store
            # is redundant but harmless; both writes produce the same value.
            si_args = subclass["superInit"].get("args", [])
            parent_params_list = parent_node.get("params", [])
            for pp, si_arg in zip(parent_params_list, si_args):
                slot_idx = parent_slots_list.index(pp)
                reg = compiler.alloc_reg()
                compiler.compile_expr(si_arg, reg)
                compiler.emit_ABC(OP_ST_SLOT, reg, slot_idx, 0)
                compiler.free_reg()
            # Initialize this ancestor's own slots (not params).
            for s in parent_node.get("slots", []):
                if s["name"] in parent_params_list:
                    continue  # Already initialized above as param
                slot_idx = parent_slots_list.index(s["name"])
                reg = compiler.alloc_reg()
                if s.get("init"):
                    compiler.compile_expr(s["init"], reg)
                else:
                    cidx = compiler.add_constant(None)
                    compiler.emit_ABx(OP_LD_CONST, reg, cidx)
                compiler.emit_ABC(OP_ST_SLOT, reg, slot_idx, 0)
                compiler.free_reg()
            # Ascend, using THIS ancestor's own superInit for the next level up.
            subclass = parent_node

        # 1. Initialize parameters slots
        slots = self.class_slots[qname]
        for i, p in enumerate(params):
            slot_idx = slots.index(p)
            reg = compiler.local_vars[p]
            compiler.emit_ABC(OP_ST_SLOT, reg, slot_idx, 0) # store param to slot of self (0)

        # 2. Initialize nested-class slots with a BoundClass (vtable + outer +
        #    slot count), mirroring the synthesised `C` accessor above. Storing a
        #    bare vtable would only support class-side methods (dispatched via its
        #    metaclass), NOT instance construction: construction is handled by
        #    boundclass_construct, which a raw vtable receiver never reaches. So an
        #    alias `private List = Vec.` reading this slot must capture a
        #    construction-capable value, else `List new: n` is "not understood".
        #    The outer is `self` (R0), the enclosing instance being built.
        #
        #    This MUST precede the user slot initializers below: a slot
        #    initializer may read this slot by name (deltablue's
        #    `private List = Vector.`). If it were still nil there, the alias
        #    would capture nil. (Method bodies are unaffected — a literal class
        #    name compiles to a direct OP_NEW, not a slot read.)
        for nested in node.get("classes", []):
            slot_idx = slots.index(nested["name"])
            nslot_count = len(self.class_slots[nested["qname"]])
            rcvr = compiler.alloc_reg()
            vt_cidx = compiler.add_constant(f"VTABLE:{self.vt_id(nested['qname'])}")
            compiler.emit_ABx(OP_LD_CONST, rcvr, vt_cidx)   # rcvr = C's vtable
            a0 = compiler.alloc_reg()
            compiler.emit_ABC(OP_LD_REG, a0, 0, 0)          # arg0 = self (outer)
            a1 = compiler.alloc_reg()
            sc_cidx = compiler.add_constant(f"SMI:{nslot_count}")
            compiler.emit_ABx(OP_LD_CONST, a1, sc_cidx)     # arg1 = slot count
            sel_cidx = compiler.add_constant("__boundOuter:slotCount:")
            compiler.emit_ABC(OP_SEND, 2, sel_cidx, rcvr)   # rcvr <- BoundClass
            compiler.emit_ABC(OP_ST_SLOT, rcvr, slot_idx, 0)
            compiler.free_reg()
            compiler.free_reg()
            compiler.free_reg()

        # 3. Evaluate and initialize user-declared slots
        for s in node.get("slots", []):
            slot_idx = slots.index(s["name"])
            if s.get("init"):
                reg = compiler.alloc_reg()
                compiler.compile_expr(s["init"], reg)
                compiler.emit_ABC(OP_ST_SLOT, reg, slot_idx, 0)
                compiler.free_reg()
            else:
                # Initialize to nil
                cidx = compiler.add_constant(None)
                reg = compiler.alloc_reg()
                compiler.emit_ABx(OP_LD_CONST, reg, cidx)
                compiler.emit_ABC(OP_ST_SLOT, reg, slot_idx, 0)
                compiler.free_reg()

        # 4. Run the primary-factory body statements (executable code written
        #    after the slot section, e.g. richards' `1 to: 4 do: [...]` or
        #    deltablue's bare `addConstraint` self-send). They execute at
        #    instantiation, after all slots are initialized, with self in R0 and
        #    slots/params in scope exactly as in a method body.
        for stmt in node.get("initBody", []) or []:
            compiler.compile_statement(stmt)

        # Return self (R0)
        compiler.finalize_jumps()
        compiler.emit_ABC(OP_RET, 0, 0, 0)

        compiler.finalize_contexts()  # deferred lift Phase 1: patch + prologue
        self.solidify.add_method(
            vtable_name=vt,
            selector=selector,
            num_registers=compiler.max_regs_used,
            num_arguments=len(params),
            constants=compiler.constants,
            instructions=compiler.instructions
        )
        self.register_source_map(f"{vt}>>{selector}", compiler.source_map)
        self.register_method_compiler(f"{vt}>>{selector}", compiler)

def main():
    if len(sys.argv) < 2:
        print("Usage: python ns_compile_oevm.py <src.ns> [output.c]")
        sys.exit(1)
        
    # Parse optional flags (--format=c|oimg, --layout=<layout.json>) out of argv.
    fmt = "c"
    layout_path = None
    positional = []
    for a in sys.argv[1:]:
        if a.startswith("--format="):
            fmt = a.split("=", 1)[1]
        elif a.startswith("--layout="):
            layout_path = a.split("=", 1)[1]
        else:
            positional.append(a)

    src_ns = positional[0]
    out_c = positional[1] if len(positional) > 1 else "src/solidified_image.c"

    # 1. Parse using Ohm parser
    parse_script = os.path.join(os.path.dirname(__file__), "..", "grammar", "ns-parse.js")
    proc = subprocess.run(
        ["node", parse_script, src_ns],
        capture_output=True, text=True
    )
    if proc.returncode != 0:
        print("Parsing failed:")
        print(proc.stderr or proc.stdout)
        sys.exit(1)
        
    ast = json.loads(proc.stdout)
    
    # Check if this is a script or a module
    is_script = True
    if ast["type"] == "program":
        for child in ast["body"]:
            if child["type"] == "module":
                is_script = False
                break
                
    if is_script:
        # Wrap the script statements in a synthetic module `ScriptApp`. Once
        # wrapped, a top-level script's statements live in an ordinary method
        # body (`main:args:`), so '^' is handled exactly like in any other
        # method: OP_RET when lexically inside an INLINED control-flow block,
        # or a CompileError at the compile_statement emit site when lexically
        # inside an ESCAPING closure (no-NLR model, spec §2).
        script_body = ast["body"]
        script_temps = ast.get("temps", [])
        ast = {
            "type": "program",
            "body": [
                {
                    "type": "module",
                    "name": "ScriptApp",
                    "params": ["platform", "args"],
                    "slots": [],
                    "methods": [
                        {
                            "type": "method",
                            "selector": "main:args:",
                            "params": ["platform", "args"],
                            "temps": script_temps,
                            "body": script_body
                        }
                    ],
                    "classes": []
                }
            ]
        }

    # std-lib step 3a: concatenate the kernel library AFTER the app so the app's
    # first class stays the entry (entry = next(iter(all_classes))). Kernel classes
    # are top-level siblings: not nested in the app, so resolve_class_ref never
    # resolves them in app scope — app code still reaches them via `platform`
    # (step-2 routing). Reached only through platform → source-compatible with the
    # future "kernel as outer module" form.
    kernel_path = os.path.join(os.path.dirname(__file__), "..", "lib", "kernel.ns")
    kproc = subprocess.run(["node", parse_script, kernel_path],
                           capture_output=True, text=True)
    if kproc.returncode != 0:
        print("Kernel parse failed:")
        print(kproc.stderr or kproc.stdout)
        sys.exit(1)
    kernel_ast = json.loads(kproc.stdout)
    ast["body"].extend(kernel_ast["body"])

    # in-image debugger step 1: compile the Supervisor module in as a top-level
    # sibling of Kernel (same load-after-app rationale: never resolved in app scope).
    supervisor_path = os.path.join(os.path.dirname(__file__), "..", "lib", "supervisor.ns")
    sproc = subprocess.run(["node", parse_script, supervisor_path],
                           capture_output=True, text=True)
    if sproc.returncode != 0:
        print("Supervisor parse failed:")
        print(sproc.stderr or sproc.stdout)
        sys.exit(1)
    supervisor_ast = json.loads(sproc.stdout)
    ast["body"].extend(supervisor_ast["body"])

    # Determine the target oop width BEFORE compiling: Path A (--format=c, no
    # --layout) runs as native C on the host, so it stays at the host's own
    # width (8); Path B (--format=oimg) supplies layout.json, whose oop_size
    # is the same source of truth oimg_serialize.py's OimgSerializer reads
    # (self.oop = layout["oop_size"]). Loading it here lets compile_expr's
    # SMI/LARGEINT split (nsl-tuvm#4) use the real target width instead of a
    # hardcoded host-64-bit assumption.
    layout = None
    oop_bytes = 8
    if layout_path:
        import json as _json
        layout = _json.load(open(layout_path))
        oop_bytes = layout["oop_size"]

    # Compile
    compiler = OevmCompiler(ast, oop_bytes=oop_bytes)
    try:
        compiler.compile()
    except CompileError as e:
        print(f"Compile error: {e}")
        sys.exit(1)

    # Emit the source-map sidecar (.stmap) next to the image, for both the
    # Path A (.c) and Path B (.oimg) backends. Format matches st-berry so the
    # jast-gw debug viewer consumes ns-oevm and st-berry maps identically.
    stmap_path = os.path.splitext(out_c)[0] + ".stmap"
    with open(stmap_path, "w", encoding="utf-8") as f:
        f.write(build_stmap(os.path.basename(src_ns), compiler.source_maps))

    # Execute-in-place image backend: serialize the app graph to a .oimg blob
    # instead of emitting fused C (Model A). The entry class is the first
    # declared class, matching the C path's run_solidified_entry_point.
    if fmt == "oimg":
        from oimg_serialize import OimgSerializer
        from builtins_registry import REGISTRY, VERSION
        if not layout_path:
            print("--format=oimg requires --layout=<layout.json>")
            sys.exit(1)
        # layout already loaded above (before compiling) to size compile_expr's
        # SMI/LARGEINT split correctly.
        entry_class = next(iter(compiler.all_classes.keys()))

        class _Graph:
            symbols = compiler.solidify.symbols
            vtables = compiler.solidify.vtables
            methods = compiler.solidify.methods

        # Entry descriptor — mirror run_solidified_entry_point (Path A) EXACTLY so
        # Path A and Path B agree on init selector, argc and slot count.
        _params = compiler.all_classes[entry_class].get("params", [])
        _init_sel = "".join(f"{p}:" for p in _params)
        if not _init_sel:
            _init_sel = "new"
        _init_selector = f"init_{_init_sel}"
        _init_argc = len(_params)
        _entry_nslots = len(compiler.class_slots[entry_class])
        # std-lib step 3b: pass the Kernel module descriptor (vtable name, init selector,
        # slot count) so the serializer stores it in the header (format_version 3). The
        # loader constructs `Kernel usingPlatform: <native>` and hands it to the app.
        _kparams = compiler.all_classes["Kernel"].get("params", [])
        _kinit_selector = f"init_{''.join(f'{p}:' for p in _kparams) or 'new'}"
        _kernel_nslots = len(compiler.class_slots["Kernel"])
        # std-lib step 3c-3b: emit serial_on_sel_ref when the entry declares `serial:`.
        # The loader will send #serialOn: to kernel_inst to mint the Serial wrapper.
        _serial_on_sel_name = "serialOn:" if "serial" in _params else None
        _net_on_sel_name = "netOn:" if "net" in _params else None
        # `concurrency:` capability retired with Concurrency/Fiber (Milestone A Task 4);
        # its oimg header word was compacted away in the Task 9b format-v8 header.
        # in-image debugger step 1: Supervisor descriptor. The
        # init selector is derived from Supervisor's param variable names, matching
        # the synthesized initializer (e.g. init_mirror: from param `mirror`).
        _sparams = compiler.all_classes["Supervisor"].get("params", [])
        _sinit_selector = f"init_{''.join(f'{p}:' for p in _sparams) or 'new'}"
        _supervisor_nslots = len(compiler.class_slots["Supervisor"])
        blob = OimgSerializer(layout, REGISTRY, registry_version=VERSION).generate(
            _Graph, entry_class,
            init_sel_name=_init_selector, init_argc=_init_argc,
            main_sel_name="main:args:", entry_nslots=_entry_nslots,
            kernel_vtable_name="Kernel", kernel_init_sel_name=_kinit_selector,
            kernel_nslots=_kernel_nslots,
            serial_on_sel_name=_serial_on_sel_name,
            net_on_sel_name=_net_on_sel_name,
            supervisor_vtable_name="Supervisor",
            supervisor_init_sel_name=_sinit_selector,
            supervisor_run_sel_name="run:platform:args:debug:",
            supervisor_nslots=_supervisor_nslots)
        with open(out_c, "wb") as f:
            f.write(blob)
        print(f"OIMG written to {out_c} ({len(blob)} bytes, entry={entry_class})")
        return

    # Solidify custom additions
    # Extend SolidifyCompiler logic to handle VTABLE: and METHOD: prefixes in constants
    orig_generate_c = compiler.solidify.generate_c
    
    def patched_generate_c():
        # Prepend forward declarations for all compiled methods
        fwd_decls = []
        for m in compiler.solidify.methods:
            mid = m['id']
            fwd_decls.append(f"static const struct CompiledMethod {mid}_compiled;")
        # getenv is from <stdlib.h>, not pulled in by oevm.h; declare it explicitly
        # so the solidified image compiles cleanly with -std=c99 -Wall.
        fwd_decls.append("extern char *getenv(const char *);")

        c_code = "\n".join(fwd_decls) + "\n\n" + orig_generate_c()
        
        # Link global VM symbols to the static solidified symbols
        c_code = c_code.replace(
            "void init_solidified_vtables(void) {",
            "void init_solidified_vtables(void) {\n"
            "    extern oop Symbol_value;\n"
            "    Symbol_value = (oop)&StaticSymbol_value;"
        )
        
        # Add patch for VTABLE: and METHOD: tags in PATTERNS
        # We can just replace the string constants with direct symbol/Vtable pointers in C code
        # Wait, since solidify.py already writes Vtable_xxx and Method_xxx declarations,
        # we can replace our placeholders after generation!
        
        # Let's inspect the symbols and replace:
        lines = c_code.splitlines()
        for i, line in enumerate(lines):
            # Replace SMI:val with TAG_SMI(val)
            # Actually, solidify.py already does this!
            # Replace VTABLE:xxx placeholder
            if "Symbol_VTABLE_" in line:
                # Find the symbol definition and replace it
                pass
        
        # Find entry class
        entry_class = next(iter(compiler.all_classes.keys()))
        
        # Append run_solidified_entry_point function
        c_code += f"\n\noop run_solidified_entry_point(void) {{\n"
        c_code += "    // Calculate Flash ROM boundaries\n"
        c_code += "    extern uintptr_t flash_rom_start, flash_rom_end;\n"
        c_code += "    uintptr_t min_addr = (uintptr_t)-1;\n"
        c_code += "    uintptr_t max_addr = 0;\n\n"
        c_code += "    #define UPDATE_LIMITS(ptr, sz) do { \\\n"
        c_code += "        uintptr_t start = (uintptr_t)(ptr); \\\n"
        c_code += "        uintptr_t end = start + (sz); \\\n"
        c_code += "        if (start < min_addr) min_addr = start; \\\n"
        c_code += "        if (end > max_addr) max_addr = end; \\\n"
        c_code += "    } while(0)\n\n"
        
        # Update limits for every static structure in the C file
        # Scan Vtables and Methods to output UPDATE_LIMITS
        for vt in compiler.solidify.vtables:
            c_code += f"    UPDATE_LIMITS(&Vtable_{vt}, sizeof(struct Vtable));\n"
        for m in compiler.solidify.methods:
            mid = m['id']
            c_code += f"    UPDATE_LIMITS({mid}_insts, sizeof({mid}_insts));\n"
            if len(m['constants']) > 0:
                c_code += f"    UPDATE_LIMITS({mid}_consts, sizeof({mid}_consts));\n"
            c_code += f"    UPDATE_LIMITS(&{mid}_compiled, sizeof(struct CompiledMethod));\n"
            c_code += f"    UPDATE_LIMITS(&{mid}_closure, sizeof(struct Closure));\n"
        for sym in compiler.solidify.symbols.values():
            c_code += f"    UPDATE_LIMITS(&{sym}, sizeof(struct Symbol));\n"
        # std-lib 3c-1: mark String literal constants read-only (data segment) so
        # GC's is_read_only skips them. sizeof(the anon struct) is exact.
        for sstr in compiler.solidify.string_consts.values():
            c_code += f"    UPDATE_LIMITS(&{sstr}, sizeof({sstr}));\n"
            
        c_code += "\n    flash_rom_start = min_addr;\n"
        c_code += "    flash_rom_end = max_addr;\n\n"
        
        # std-lib step 3b: two-stage boot. (1) native C platform; (2) construct the
        # Kernel module `Kernel usingPlatform: <native>`; (3) hand the Kernel
        # INSTANCE to the app as its `platform`.
        c_code += "    extern oop gc_allocate(oop vtable, size_t size);\n"
        # oevm#21: pin boot instances as PRECISE GC roots. They are held in C locals
        # across the send sequence below; the conservative C-stack scan that would
        # otherwise root them is defeated by ASan's fake-stack, so a GC_STRESS
        # collection mid-boot can sweep them and corrupt dispatch. Cleared before return.
        c_code += "    extern void gc_temp_root_push(oop o);\n"
        c_code += "    extern void gc_temp_roots_clear(void);\n"
        c_code += f"    oop native_platform = platform_object();\n"
        c_code += "    gc_temp_root_push(native_platform);\n"
        c_code += f"    oop fake_args = TAG_SMI(0);\n"

        # (2) construct Kernel usingPlatform: native
        kparams = compiler.all_classes["Kernel"].get("params", [])
        kinit_sel = "".join(f"{p}:" for p in kparams) or "new"
        kinit_selector = f"init_{kinit_sel}"
        kinit_sym = compiler.solidify.symbols[kinit_selector]
        c_code += f"    oop kernel_inst = gc_allocate((oop)&Vtable_Kernel, sizeof(struct ObjectInstance) + {len(compiler.class_slots['Kernel'])} * sizeof(oop));\n"
        c_code += "    gc_temp_root_push(kernel_inst);\n"
        c_code += f"    oop kernel_init_args[1] = {{ native_platform }};\n"
        c_code += f"    send(kernel_inst, (oop)&{kinit_sym}, {len(kparams)}, kernel_init_args);\n\n"

        # (3) instantiate the entry class, threading capabilities by declared params.
        # Map canonical capability names -> the C expression that yields them.
        params = compiler.all_classes[entry_class].get("params", [])
        cap_expr = {"platform": "kernel_inst"}
        if "serial" in params:
            serial_on_sym = compiler.solidify.symbols["serialOn:"]
            c_code += "    extern oop serial_object(void);\n"
            c_code += "    oop serial_port = serial_object();\n"
            c_code += f"    oop serial_args[1] = {{ serial_port }};\n"
            c_code += f"    oop serial_inst = send(kernel_inst, (oop)&{serial_on_sym}, 1, serial_args);\n"
            c_code += "    gc_temp_root_push(serial_inst);\n"
            cap_expr["serial"] = "serial_inst"
        if "net" in params:
            net_on_sym = compiler.solidify.symbols["netOn:"]
            c_code += "    extern oop udp_object(void);\n"
            c_code += "    oop net_port = udp_object();\n"
            c_code += "    oop net_args[1] = { net_port };\n"
            c_code += f"    oop net_inst = send(kernel_inst, (oop)&{net_on_sym}, 1, net_args);\n"
            c_code += "    gc_temp_root_push(net_inst);\n"
            cap_expr["net"] = "net_inst"
        # `concurrency:` capability injection retired with Concurrency/Fiber (Task 4):
        # Kernel>>concurrency no longer exists, so no capability is minted. An entry
        # declaring `concurrency:` (all such tests are skip-oevm) falls through to the
        # fake_args default below, as with any unrecognized capability param.
        # Build init_args in the entry's declared param order.
        argv = ", ".join(cap_expr.get(p, "fake_args") for p in params) or "kernel_inst"
        init_sel = "".join(f"{p}:" for p in params) or "new"
        init_selector = f"init_{init_sel}"
        c_code += f"    // Instantiate entry class {entry_class}\n"
        c_code += f"    oop entry_inst = gc_allocate((oop)&Vtable_{entry_class}, sizeof(struct ObjectInstance) + {len(compiler.class_slots[entry_class])} * sizeof(oop));\n"
        c_code += "    gc_temp_root_push(entry_inst);\n"
        c_code += f"    oop init_args[{max(len(params),1)}] = {{ {argv} }};\n"
        init_sym_var = compiler.solidify.symbols[init_selector]
        c_code += f"    send(entry_inst, (oop)&{init_sym_var}, {len(params)}, init_args);\n\n"

        # in-image debugger step 1: mint the Supervisor, grant it the native
        # mirror, and route the launch through it (gated by OEVM_DEBUG).
        sup_params = compiler.all_classes["Supervisor"].get("params", [])
        sup_init_sel = "".join(f"{p}:" for p in sup_params) or "new"
        sup_init_selector = f"init_{sup_init_sel}"
        sinit_sym = compiler.solidify.symbols[sup_init_selector]
        srun_sym = compiler.solidify.symbols["run:platform:args:debug:"]
        sup_nslots = len(compiler.class_slots["Supervisor"])
        # Supervisor is a SINGLE-slot grant: the one capability we hand it is the
        # native mirror. Derive the argc from len(sup_params) so it stays honest
        # with the class definition, and assert the mirror-only invariant so a
        # future multi-capability Supervisor fails LOUD here (wrong contents)
        # instead of silently passing a stale argc.
        assert len(sup_params) == 1, (
            f"Supervisor grant emit expects exactly one capability param (the mirror), "
            f"got params={sup_params!r}; update the grant contents below before changing argc.")
        sup_argc = len(sup_params)
        c_code += "    extern oop mirror_object(void);\n"
        c_code += f"    oop supervisor_inst = gc_allocate((oop)&Vtable_Supervisor, sizeof(struct ObjectInstance) + {sup_nslots} * sizeof(oop));\n"
        c_code += "    gc_temp_root_push(supervisor_inst);\n"
        c_code += f"    oop sup_init_args[{sup_argc}] = {{ mirror_object() }};\n"
        c_code += f"    send(supervisor_inst, (oop)&{sinit_sym}, {sup_argc}, sup_init_args);\n"
        c_code += "    oop dbg_flag = getenv(\"OEVM_DEBUG\") ? TAG_SMI(1) : TAG_SMI(0);\n"
        c_code += "    oop run_args[4] = { entry_inst, kernel_inst, fake_args, dbg_flag };\n"
        c_code += f"    oop res = send(supervisor_inst, (oop)&{srun_sym}, 4, run_args);\n"
        c_code += "    gc_temp_roots_clear();\n"
        c_code += "    return res;\n"
        c_code += "}\n"
        
        # Expose a symbol helper function to assign SymbolVtable, etc.
        # This is already generated in init_solidified_vtables!
        # Let's add init_reflection_symbols to build standard symbols
        c_code += "\nvoid init_reflection_symbols(void) {\n"
        c_code += "    // Expose reflection symbols to the interpreter\n"
        c_code += "}\n"
        
        return c_code
        
    compiler.solidify.generate_c = patched_generate_c

    
    output = compiler.solidify.generate_c()
    with open(out_c, "w") as f:
        f.write(output)
        
    print(f"Compilation and solidification successful: {out_c}")

if __name__ == "__main__":
    main()

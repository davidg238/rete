> **HISTORICAL — archived 2026-08-29.** The genesis spec (pre-git). §2–3 describe Non-Local Return via lazy stack reification; NLR was DELETED in Milestone A (2026-07-04, `nodus/oevm/docs/MILESTONE_A.md` §1) and `^` out of an escaping closure is now a compile error. §1 (the three-object universe, bytecodes as sends) and the MIC idea still describe the VM. Current architecture: `nodus/oevm/docs/vm.md`.

# Architectural Specification: Open, Lazy-Reified Virtual Machine
**Target Architecture:** Berry VM Compatible (32-bit fixed-length iABC/iABx/iAsBx instruction set)
**Design Philosophy:** Ian Piumarta's Open Extensible Object Model (VPRI, c. 2007-2010) + Newspeak Late-Bound Class/Non-Local Return Semantics.

---

## 1. Core Paradigm & Micro-Kernel Dispatch
The execution engine is stripped of hard-coded C-level type rules, switch-matrix arithmetic coercions, and inheritance lookups. The VM acts strictly as a **Message Dispatch Accelerator**.

### The 3-Object Primitive Universe
1. **Object:** Root structural cell. Structural layout begins with a `vtable` pointer.
2. **Symbol:** Unique, interned string tokens representing message selectors (e.g., `#+`, `#lookup`, `#unwindTo:value:`).
3. **Vtable:** Virtual Method Table objects mapping Symbols to compiled closures. The `vtable` is itself an Object pointing to a meta-vtable.

### Bytecode Deconstruction
Standard arithmetic and access bytecodes are desugared internally into semantic message sends.
* `ADD R(A), R(B), R(C)` $\implies$ `send(Regs[B], #+, Regs[C])`
* `GETFIELD R(A), R(B), C(idx)` $\implies$ `send(Regs[B], #getField:, ConstantPool[idx])`

---

## 2. Execution Engine & Lazy Stack Reification
To execute Newspeak-style Non-Local Returns (NLR) without exception overhead or heap-allocation thrashing on resource-constrained embedded hardware (e.g., ESP32/Zephyr), the VM utilizes a hybrid dynamic stack model.

### Double-Agent Frame Layout
By default, function activation records are allocated sequentially on a raw, contiguous stack array for performance. However, their memory footprint is meticulously engineered to mirror an Object payload:

[ Offset 0: vtable_ptr ] -> Initialized to NULL / Dummy Token (Fast Path)
[ Offset 1: prev_frame ] -> Dynamic Link (Pointer to Caller Context)
[ Offset 2: saved_pc   ] -> Instruction Pointer return address
[ Offset 3: home_ctx   ] -> Lexical Link (Pointer to defining MethodContext)
[ Offset 4: gen_tag    ] -> 32-bit Lifecycle Generation Counter
[ Offset 5: cleanup_cl ] -> Pointer to optional resource deallocation closure
[ Offset 6...: regs    ] -> Local Register Array (Virtual Address Space)


### The Unwinding Trigger
When a closure executes a return instruction that targets a lexical scope higher than its immediate caller:
1. **Stack Reification:** The VM sweeps up the active physical stack frames and patches their `vtable_ptr` slots to point to a globally shared, static `MethodContextVtable`.
2. **Polymorphic Promotion:** The frames are now legally recognized as Open Model Objects without any heap allocation or copying.
3. **The Unwind Message:** The VM transforms the return execution into an explicit late-bound message send:
   `send(current_frame, #unwindTo:value:, closure->home_ctx, return_value)`

### Unwinding Semantics (Pseudo-Code)
The stack-walking, resource cleanup, and target discovery are handled natively via the open object model:
```newspeak
MethodContextVtable >> unwindTo: targetHome value: rVal [
    (this == targetHome) ifTrue: [
        this executeCleanup.
        this sender resumeWith: rVal.
        ^ nil
    ].
    this executeCleanup.
    this sender unwindTo: targetHome value: rVal.
]

## 3. Critical Constraints & Mitigations
### The Dangling Return Trap
- Problem: A Newspeak block closure can outlive its defining lexical scope. Executing an NLR to a dead stack frame results in undefined behavior.
- Mitigation: Every closure captures its home frame pointer and the home frame's unique gen_tag. Before unwinding, the VM asserts frame->gen_tag == closure->home_generation. On a mismatch, the unwind aborts and vectors to send(closure, #cannotReturn:, rVal).

### Optimization: Monomorphic Inline Caching (MIC)
- Problem: Pure user-space vtable lookups for every primitive operation introduce severe performance degradation.
- Mitigation: Reserve a mutable 64-bit cache slot directly within the bytecode instruction stream for every SEND operation:
  [ Opcode: SEND ] [ Selector ] [ Cache: Last_Seen_Vtable ] [ Cache: Target_Func_Ptr ]
  If object->vtable == Last_Seen_Vtable, bypass lookup entirely and execute an indirect jump to Target_Func_Ptr (Fast-Path hit). Fallback to open user-space lookup only on a cache miss.
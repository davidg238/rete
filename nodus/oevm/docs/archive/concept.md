> **HISTORICAL — archived 2026-08-29.** The genesis concept note. Part 1 §2A (NLR) and §2C (fibers) describe mechanisms DELETED in Milestone A (2026-07-04, `nodus/oevm/docs/MILESTONE_A.md` §1); the rest of Part 1 still holds and Part 2 (why keyword syntax suits LLM generation) is the rationale that started the program. Current architecture: `nodus/oevm/docs/vm.md`.

# OEVM Concept & Genesis: From LLM-Driven Titelang to Custom Micro-Kernel VM

This document merges the original design discussions (exploring Smalltalk-style syntax for LLM code generation) with the updated architectural concepts of the **Open Extensible Virtual Machine (OEVM)**.

---

# Part 1: The OEVM Architecture (The Custom Micro-Kernel)

The **Open Extensible Virtual Machine (OEVM)** is an experimental, lightweight, register-based VM. It uses a 32-bit fixed-length instruction layout compatible with the Berry VM, but internally operates on **Ian Piumarta's Open Extensible Object Model (OEOM)**.

## 1. Core Principles & Unified Message Dispatch
Instead of hardcoding C-level type rules, switch-case type checks, or class inheritance trees inside the interpreter, the VM is stripped down to a pure **Message Dispatch Accelerator**. All operations are resolved dynamically via late-bound message sends.

### The 3-Object Primitive Universe
Every value in the VM is a uniform reference type called an `oop` (Object-Oriented Pointer).
1. **Object:** The base cell of all structures. Every `Object` payload starts with a `vtable` pointer.
2. **Symbol:** An interned string token representing method selectors and variable names.
3. **Vtable:** A lookup table mapping `Symbol` keys to method `Closures`.

### Monomorphic Inline Caching (MIC)
To bypass slow, open-ended Vtable lookups, a mutable 64-bit cache slot is reserved directly within the bytecode stream for each `SEND` instruction. 
On the first lookup, the resolved closure is cached. On subsequent runs, if the receiver's Vtable matches the cached Vtable, the VM executes a fast-path direct call, achieving performance close to native C function pointers.

---

## 2. The "Double-Agent" Design Patterns
To resolve the performance constraints of dynamic execution on resource-constrained embedded targets (like the ESP32), OEVM applies the "Double-Agent" pattern (lazy reification / polymorphic promotion) in key areas:

### A. Double-Agent Stack Frames (MethodContext)
* **Fast Path (Stack Allocation):** Function activation records are allocated sequentially on a raw, contiguous stack array to minimize allocation overhead.
* **Polymorphic Promotion:** The stack frame layout is engineered to mirror a standard `Object` structure. When a Non-Local Return (NLR) is triggered, the VM sweeps up the active physical frames, patches their `vtable_ptr` from `NULL` to a shared static `MethodContextVtable`, and promotes them to first-class heap-like objects.
* **NLR via Message Passing:** Stack unwinding transitions from rigid C-level frame adjustments to a polymorphic message send:
  `send(current_frame, #unwindTo:value:, home_ctx, return_val)`

### B. Pointer Tagging & Immediate Values
* **Untagged objects** reside in the heap, and their `vtable_ptr` is retrieved by dereferencing offset 0.
* **Immediate values** (like Small Integers / Smis) are tagged directly in the `oop` pointer representation (e.g., LSB=1).
* **Virtual Vtables:** The dispatch engine inspects pointer tags. If it detects a tagged integer, it intercepts dereferencing and maps it to a virtual/static `IntegerVtable`. To the VM, integers behave like first-class objects, but cost zero allocation.

### C. Fiber / Task Scheduling
Cooperative tasks are represented as `Fiber` objects. A fiber's state is controlled by dynamically changing its Vtable pointer (e.g., `RunnableFiberVtable`, `SuspendedFiberVtable`, `DeadFiberVtable`). Cooperative scheduling is handled entirely via late-bound message passing rather than hardcoded C state machines.

### D. Unified Stack/Heap Garbage Collection
Since stack frames are layout-compatible with heap objects and linked via `prev_frame` pointers, the garbage collector treats the active stack as a standard object graph, walking it using the same tracing code used for heap allocations.

---

# Part 2: Genesis & The Smalltalk/LLM Rationale

This section captures the initial discussion exploring why Smalltalk-style syntax is uniquely suited for LLM code generation on embedded platforms.

## 1. Smalltalk as a "Cheat Code" for LLM Accuracy
Keyword syntax provides structural anchors in the token stream that drastically reduce common AI coding hallucinations compared to C-style languages.

### Self-Documenting Call Sites (Parameter Safety)
The most common LLM bugs in C-style languages stem from argument transposition (e.g. passing `width` instead of `height`).
* **Python/C:** `rect.draw(10, 20, 100, 50)` — The LLM must recall the exact positional order.
* **Smalltalk:** `rect x: 10 y: 20 width: 100 height: 50.` — The syntax forces interleaved labels, ensuring the transformer attends to the correct slot.

### Eliminating Indentation Hallucinations
LLMs often struggle to maintain correct block structures in indentation-sensitive languages (like Python or Toit) due to invisible whitespace tokens.
* Smalltalk's use of explicit statement terminators (`.`) and block brackets (`[...]`) provides hard anchors in the token stream, preventing structural indentation mistakes.

### Reduced Token Complexity
By removing redundant delimiters (parentheses, commas, semicolons), keyword messaging uses fewer tokens, reducing generation error probability and context window consumption.

---

## 2. Language Definition Card (titelang / OEVM Dialect)

Use these rules to prompt LLMs for code generation targeting the VM:

### Syntax Rules
* **Assignment:** `variable := value.`
* **Statement terminator:** Every statement MUST end with a period `.`
* **Messaging:**
  * Unary: `receiver method.`
  * Binary: `receiver + 5.`
  * Keyword: `receiver at: 1 put: 'data'.`
* **Blocks:** `[ :arg1 :arg2 | code. ]`
* **Cascades:** Use `;` to send multiple messages to the same receiver.
  * Example: `motor speed: 50; start.`
* **Comments:** Double quotes `"comment"`.

### Control Flow (Smalltalk Style)
* **If:** `(condition) ifTrue: [ ... ].`
* **If/Else:** `(condition) ifTrue: [ ... ] ifFalse: [ ... ].`
* **Loops:** `[ condition ] whileTrue: [ ... ].` or `10 timesRepeat: [ ... ].`

### Example: CANbus Sniffer
```smalltalk
| bus led |
bus := CANbus open: 'can0' speed: 500000.
led := GPIO pin: 2 mode: #output.

bus onID: 0x620 do: [ :frame |
    ((frame at: 0) == 0x80) ifTrue: [
        led toggle.
        Console print: 'Signal Detected'.
    ].
].
```

---

## 3. Custom Binary Selectors: Grammar and Optimization
In titelang/OEVM, standard binary operators (like `+`, `-`, `<<`, `&`) are treated as messages, but optimized at the compiler/transpiler level to compile directly to VM opcodes.

### Operator Precedence (High to Low)
1. **Unary:** `myObj abs`
2. **Multiplicative:** `* / %`
3. **Additive:** `+ -`
4. **Bitwise:** `<< >> & |`
5. **Relational:** `== != < > <= >=`
6. **Keyword:** `at:put:`

Custom binary operators (e.g. `@` or `!!`) map to normal dynamic message sends (e.g. `@` mangles to `_at:` method call).

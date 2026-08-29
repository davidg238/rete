> **HISTORICAL — archived 2026-08-29.** A pre-git phase walkthrough of the original oevm build-up (2026-06). Phases 4 (fibers) and 5 (NLR) were DELETED in Milestone A (2026-07-04, `nodus/oevm/docs/MILESTONE_A.md`). Current architecture: `nodus/oevm/docs/vm.md`.

# Walkthrough - Phase 2: Instruction Set Expansion & XIP Solidification

We completed the implementation of Phase 2, enabling the execution of compiled bytecodes directly from simulated Flash ROM (Read-Only memory) using static, immutable structures.

## Changes Made

### 1. Expanded VM Instruction Set (`src/oevm.h`, `src/vm.c`)
- Expanded the VM register decoding loop with:
  - **Arithmetic:** `OP_ADD`, `OP_SUB`, `OP_MUL`, `OP_DIV`. Operands dynamically switch between unboxed fast-path Smis and heap-allocated boxed Floats.
  - **Control Flow:** `OP_JMP`, `OP_JMP_IF_FALSE` allowing loops and branches.
  - **Slot/Property Access:** `OP_LD_SLOT` and `OP_ST_SLOT` to read and write dynamic instance properties.
  - **Instantiation:** `OP_NEW` allocating user-defined dynamic structures of a given class vtable.

### 2. Solidification Utility (`tools/solidify.py`)
- Programmed a Python-based assembler and C code generator that:
  - Scans user methods, compiles instructions, and structures constants.
  - Pre-registers referenced selector strings to global Symbols.
  - Generates immutable `.rodata` static C structures for Symbol, Closure, Method, and Vtable layouts.
  - Emits the boot initialization hook `init_solidified_vtables()` assigning static Flash structure addresses to runtime global variables.

### 3. XIP & GC Verification (`tests/test_solidified.c`)
- Formulated a verification program that:
  - Initializes the solidified image environment.
  - Allocates a dynamic `Counter` object instance on the garbage-collected heap.
  - Sends `#increment` to the counter, executing bytecode instructions entirely from simulated Flash ROM (`.rodata`).
  - Traces execution and runs the GC sweep phase to prove that Flash ROM boundaries are correctly skipped by the GC mark phase.

## Verification Results

### 1. VM Integration and Execution Tests (`tests/test_oevm.c`)

Both 32-bit and 64-bit execution tests compile and run with exit code `0` and **zero warnings**:

```
Setting up OEVM test environment...
Running pointer tagging and routing test...
Pointer tagging test passed successfully!
Running Monomorphic Inline Cache (MIC) test...
Inline cache test passed successfully!
Running Garbage Collector test...
Garbage Collector test passed successfully!
Running Non-Local Return (NLR) test...
Non-Local Return (NLR) test passed successfully!

All OEVM integration tests PASSED successfully!
```

### 2. XIP Solidification Verification (`tests/test_solidified.c`)

Both 32-bit and 64-bit builds of the solidification test pass successfully:

#### 64-bit Output:
```
Initializing solidified VM environment...
Simulated Flash ROM: 0x5673f7d01c60 - 0x5673f7d01cc8
Created dynamic Counter instance at 0x5673f7d726c8, slot[0] = 10
Sending 'increment' to Counter instance...
Result: 11
Increment method executed successfully from Flash ROM!
Running garbage collector sweep...
Garbage collection with Flash boundaries passed successfully!

All OEVM Execute-in-Place (XIP) Solidification tests PASSED!
```

#### 32-bit Output (`-m32`):
```
Initializing solidified VM environment...
Simulated Flash ROM: 0x58577e58 - 0x58577e80
Created dynamic Counter instance at 0x58e105bc, slot[0] = 10
Sending 'increment' to Counter instance...
Result: 11
Increment method executed successfully from Flash ROM!
Running garbage collector sweep...
Garbage collection with Flash boundaries passed successfully!

All OEVM Execute-in-Place (XIP) Solidification tests PASSED!
```

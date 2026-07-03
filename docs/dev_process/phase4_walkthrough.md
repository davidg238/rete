# Walkthrough - Phase 4: Fiber Scheduling Architecture

We have successfully completed **Phase 4** of the OEVM micro-kernel design, establishing first-class **Fiber** objects, cooperative stack-swapping primitives, GC tracing across multiple heap-allocated stacks, and a bytecode-resilient cooperative context switcher.

## Changes Made

### 1. Fiber Object Definition & Layout (`src/oevm.h`)
- Reified fibers via `struct Fiber` containing pointers to its private heap stack (`stack`), allocation capacity, saved execution context (`current_context`), private stack pointer register (`stack_ptr`), and execution status metadata.
- Declared `current_fiber` register to track the active thread of execution, along with three fiber state class vtables (`RunnableFiberVtable`, `SuspendedFiberVtable`, `DeadFiberVtable`).

### 2. Cooperative Stack Swapping & Primitives (`src/vm.c`)
- Transitioned the VM's active evaluation stack from a static buffer to a dynamic pointer (`oop *vm_stack`), defaulting to a boot-time stack buffer.
- Implemented `primitive_fiber_create` to allocate a `Fiber` object and its private stack from the heap, and build a dummy method context frame on the target stack to boot the fiber's entry closure.
- Implemented `primitive_fiber_switch` to exchange the active `current_fiber` register, swap `vm_stack` pointer regions, restore execution context markers (`current_frame`, `vm_stack_ptr`), and dynamically transition fiber vtables to reflect scheduling state changes.

### 3. Interpreter Loop Resiliency & Value Passing (`src/vm.c`)
- Updated `OP_SEND` and `OP_SEND_IC` dispatch handlers to intercept context switches.
- Implemented a post-dispatch check: if a context switch occurred (`ctx != current_frame`), the VM dynamically reloads loop variables (`ctx`, `pc`, `pc_end`, `method`) for the resuming fiber.
- Added a suspended instruction lookup step: if resuming a fiber previously suspended at an `OP_SEND`/`OP_SEND_IC`, the VM decodes the exact register destination of that suspended send instruction to write the resumed yield value. For new fibers, it bypasses the register overwrite to keep the initial boot state pristine.

### 4. GC Stack Scanning & Root Tracing (`src/gc.c`)
- Guarded system vtable matches (`FloatVtable`, `FiberVtable`, `RunnableFiberVtable`, `SuspendedFiberVtable`, `DeadFiberVtable`) against `NULL` to avoid matching uninitialized type markers during GC sweeps.
- Configured `gc_collect()` to mark all fiber global state vtables and the `current_fiber` object.
- Customized the mark phase (`mark()`) to traverse and trace all objects on private stacks of suspended fibers, preventing GC-induced collection of living registers.
- Configured the sweep phase to automatically reclaim and `free()` heap-allocated stack arrays of any garbage collected `Fiber` objects.

### 5. Solidification Support (`tools/solidify.py`)
- Forward-declared and registered `RunnableFiberVtable`, `SuspendedFiberVtable`, and `DeadFiberVtable` as system root meta-vtables in the solidification compiler pipeline.

### 6. Fiber Scheduling Integration Test (`tests/test_fibers.c`)
- Built a comprehensive test validating fiber creation, stack initialization, cooperative context switching passing values, and GC stack tracing.
- Utilized a guest bytecode launcher to execute the context switch from within the interpreter loop, confirming total VM register, frame, and instruction stream integrity across switches.

## Verification Results

All tests compile and run successfully with zero warnings under both 64-bit and 32-bit (`-m32`) compilation targets.

### 64-Bit Target Run
```bash
$ gcc -Wall -Wextra -std=c99 src/vm.c src/gc.c src/dispatch.c src/solidified_image.c tests/test_fibers.c -o tests/test_fibers && ./tests/test_fibers
--- Running Fiber Creation and Context Switching Test ---
Successfully created suspended fiber at 0x5b6e235de7f8
Main fiber initialized at 0x5b6e235fe8e8
Executing bytecode launcher to switch to new fiber...
Returned back to C with result: 123 (expected 123)
Running garbage collector tracing test...
Garbage collection test passed!
All fiber scheduling tests PASSED successfully!
```

### 32-Bit Target Run
```bash
$ gcc -m32 -Wall -Wextra -std=c99 src/vm.c src/gc.c src/dispatch.c src/solidified_image.c tests/test_fibers.c -o tests/test_fibers_32 && ./tests/test_fibers_32
--- Running Fiber Creation and Context Switching Test ---
Successfully created suspended fiber at 0x5d2bc66c
Main fiber initialized at 0x5d2cc6fc
Executing bytecode launcher to switch to new fiber...
Returned back to C with result: 123 (expected 123)
Running garbage collector tracing test...
Garbage collection test passed!
All fiber scheduling tests PASSED successfully!
```

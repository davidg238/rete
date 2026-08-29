> **HISTORICAL — archived 2026-08-29.** A pre-git phase walkthrough of the original oevm build-up (2026-06). Phases 4 (fibers) and 5 (NLR) were DELETED in Milestone A (2026-07-04, `nodus/oevm/docs/MILESTONE_A.md`). Current architecture: `nodus/oevm/docs/vm.md`.

# Walkthrough - Core C Structures for OEVM

We have implemented the core header file and a validation test suite to verify the layout of the Open Extensible Object Model (OEVM) Virtual Machine structures.

## Changes Made

### OEVM Core
- Created [oevm.h](file:///home/david/workspace/oevm/src/oevm.h) containing definition of:
  - `oop`: Object-Oriented Pointer, pointing to `struct Object`.
  - `struct Object`: Base structure starting with a `vtable` pointer.
  - `struct Symbol`: Interned symbol identifier mapping to a selector string.
  - `struct Binding`: Individual mapping entry within a virtual table.
  - `struct Vtable`: Method lookup structure mapping selectors to compiled methods or closures.
  - `struct MethodContext`: The "Double-Agent" stack frame layout, structured using a union for `gen_tag` to ensure that offset index alignment is preserved across 32-bit and 64-bit systems.
  - `struct Closure`: Represents a block closure capturing its defining lexical environment frame (`home_ctx`), generation lifecycle tag (`home_generation`), and runtime entry point.
  - `struct InlineCache`: Designed to fit the instruction stream's Monomorphic Inline Cache (MIC) slots for acceleration of late-bound sends.

### Validation Test Suite
- Updated [test_layout.c](file:///home/david/workspace/oevm/tests/test_layout.c) to also verify layout compatibility of `struct Closure` and size alignment of `struct InlineCache`.

## Verification Results

We compiled and ran the layout validation test on both 64-bit and 32-bit architectures, confirming that all offset assertions and sizes successfully matched the specifications.

### 64-bit Output:
```
Host oop size: 8 bytes (64-bit architecture)
Checking MethodContext offset layout...
Offset 0 (vtable): Expected 0, Got 0
Offset 1 (prev_frame): Expected 8, Got 8
Offset 2 (saved_pc): Expected 16, Got 16
Offset 3 (home_ctx): Expected 24, Got 24
Offset 4 (gen_tag): Expected 32, Got 32
Offset 5 (cleanup_cl): Expected 40, Got 40
Offset 6 (regs): Expected 48, Got 48
Verifying layout compatibility between Object and MethodContext...
Verifying layout compatibility between Object and Closure...
Checking InlineCache size...
InlineCache size: 16 bytes
All layout assertions PASSED successfully!
```

### 32-bit Output (`-m32`):
```
Host oop size: 4 bytes (32-bit architecture)
Checking MethodContext offset layout...
Offset 0 (vtable): Expected 0, Got 0
Offset 1 (prev_frame): Expected 4, Got 4
Offset 2 (saved_pc): Expected 8, Got 8
Offset 3 (home_ctx): Expected 12, Got 12
Offset 4 (gen_tag): Expected 16, Got 16
Offset 5 (cleanup_cl): Expected 20, Got 20
Offset 6 (regs): Expected 24, Got 24
Verifying layout compatibility between Object and MethodContext...
Verifying layout compatibility between Object and Closure...
Checking InlineCache size...
InlineCache size: 8 bytes
All layout assertions PASSED successfully!
```

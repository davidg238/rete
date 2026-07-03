# OP_SEND_IMPLICIT: Lexical Implicit-Receiver Message Sends

To support Newspeak-style nested classes and lexical scopes, the OEVM micro-kernel implements a dedicated message dispatch opcode: `OP_SEND_IMPLICIT`.

## Semantic Purpose & Newspeak Compatibility

In traditional Smalltalk systems, an implicit send `foo` is just syntactic sugar for `self foo`. In Newspeak, however, they are semantically distinct to support a global-free namespace and the Object Capability model.

| Send Syntax | Target Scope Search Order |
| :--- | :--- |
| **Explicit Send** (`self foo`) | Searches only the inheritance chain of `self` (its delegates/superclasses). Enclosing lexical scopes are ignored. |
| **Implicit Send** (`foo`) | 1. Walks the **lexical enclosure chain** (`self`, `self outer`, `self outer outer`...) checking for local method definitions in each class.<br>2. If not found in any enclosure, falls back to the **inheritance chain** of `self`. Enclosing classes' inheritance chains are *never* searched. |

By prioritizing local lexical outer definitions over self-inheritance, Newspeak prevents subclasses from accidentally shadowing or hijacking lexically bound services and enclosing classes when superclass libraries are upgraded.

## VM Opcode Design

Because nested classes can be subclassed and overridden dynamically, the target receiver of an implicit send cannot always be resolved statically by the compiler. The VM must resolve the correct receiver dynamically at runtime.

### Instruction Layout
`OP_SEND_IMPLICIT` shares the same instruction layout as the standard `OP_SEND`:
- `dest`: Argument count (argc).
- `src1`: Constant pool index of the selector symbol.
- `src2`: Register containing the starting receiver (`self`).

### Resolution Algorithm
When the interpreter executes `OP_SEND_IMPLICIT`:
1. It initializes `current = regs[src2]`.
2. While `current` represents a nested class instance (indicated by `vtable->is_nested == 1`):
   - It searches `current`'s local Vtable bindings directly (without following the `delegate` inheritance pointer).
   - If found, it halts the search; `current` is the resolved receiver.
   - If not found, it traverses to the enclosing outer instance: `current = current->slots[0]`.
3. If the selector is not found in the lexical chain, it falls back to standard message lookup (`lookup()`) starting at the inheritance chain of the original receiver (`regs[src2]`).
4. Once the target method is located on the resolved receiver, it is executed.

## Introspection Support
To expose this lexical outer relationship to guest-level tools (such as reflection mirrors or debuggers), the VM registers the `outer` primitive on the `Object` class:
- Calling `obj outer` returns `obj->slots[0]` if `obj` belongs to a nested class, and `nil`/`NULL` otherwise.

#include <stdio.h>
#include <stddef.h>
#include <assert.h>
#include "../src/oevm.h"

int main(void) {
    size_t oop_size = sizeof(oop);
    printf("Host oop size: %zu bytes (%zu-bit architecture)\n", oop_size, oop_size * 8);

    // Assert the exact offsets in MethodContext (relative to word size)
    printf("Checking MethodContext offset layout...\n");
    
    printf("Offset 0 (vtable): Expected %zu, Got %zu\n", 0 * oop_size, offsetof(struct MethodContext, vtable));
    assert(offsetof(struct MethodContext, vtable) == 0 * oop_size);

    printf("Offset 1 (prev_frame): Expected %zu, Got %zu\n", 1 * oop_size, offsetof(struct MethodContext, prev_frame));
    assert(offsetof(struct MethodContext, prev_frame) == 1 * oop_size);

    printf("Offset 2 (saved_pc): Expected %zu, Got %zu\n", 2 * oop_size, offsetof(struct MethodContext, saved_pc));
    assert(offsetof(struct MethodContext, saved_pc) == 2 * oop_size);

    printf("Offset 3 (home_ctx): Expected %zu, Got %zu\n", 3 * oop_size, offsetof(struct MethodContext, home_ctx));
    assert(offsetof(struct MethodContext, home_ctx) == 3 * oop_size);

    printf("Offset 4 (closure): Expected %zu, Got %zu\n", 4 * oop_size, offsetof(struct MethodContext, closure));
    assert(offsetof(struct MethodContext, closure) == 4 * oop_size);

    printf("Offset 5 (gen_tag): Expected %zu, Got %zu\n", 5 * oop_size, offsetof(struct MethodContext, gen_tag));
    assert(offsetof(struct MethodContext, gen_tag) == 5 * oop_size);

    printf("Offset 6 (cleanup_cl): Expected %zu, Got %zu\n", 6 * oop_size, offsetof(struct MethodContext, cleanup_cl));
    assert(offsetof(struct MethodContext, cleanup_cl) == 6 * oop_size);

    printf("Offset 7 (regs): Expected %zu, Got %zu\n", 7 * oop_size, offsetof(struct MethodContext, regs));
    assert(offsetof(struct MethodContext, regs) == 7 * oop_size);

    // Layout compatibility checks
    printf("Verifying layout compatibility between Object and MethodContext...\n");
    
    // An instance of MethodContext cast to Object* must have its vtable pointer at the same offset.
    assert(offsetof(struct Object, vtable) == offsetof(struct MethodContext, vtable));
    assert(sizeof(((struct Object*)0)->vtable) == sizeof(((struct MethodContext*)0)->vtable));

    printf("Verifying layout compatibility between Object and Closure...\n");
    assert(offsetof(struct Object, vtable) == offsetof(struct Closure, base.vtable));
    assert(sizeof(((struct Object*)0)->vtable) == sizeof(((struct Closure*)0)->base.vtable));

    // Verify InlineCache size
    printf("Checking InlineCache size...\n");
    printf("InlineCache size: %zu bytes\n", sizeof(struct InlineCache));
    assert(sizeof(struct InlineCache) == 2 * oop_size);

    // Parametric check: all oop-typed fields sit at index * oop_size.
    // This verifies that the ABI is self-consistent at both 32-bit and 64-bit.
    printf("Checking parametric oop-field-at-index*oop_size invariant (%zu-bit)...\n",
           oop_size * 8);

    // Symbol: base.vtable at index 0, name (const char*) at index 1
    assert(offsetof(struct Symbol, base.vtable) == 0 * oop_size);
    assert(offsetof(struct Symbol, name)        == 1 * oop_size);
    // Vtable: base.vtable at index 0, delegate at index 1
    assert(offsetof(struct Vtable, base.vtable) == 0 * oop_size);
    assert(offsetof(struct Vtable, delegate)    == 1 * oop_size);
    // Binding: symbol at index 0, method at index 1
    assert(offsetof(struct Binding, symbol) == 0 * oop_size);
    assert(offsetof(struct Binding, method) == 1 * oop_size);
    // Closure: base.vtable at index 0, home_ctx at index 1
    assert(offsetof(struct Closure, base.vtable) == 0 * oop_size);
    assert(offsetof(struct Closure, home_ctx)     == 1 * oop_size);
    // BoxedBinding: base.vtable at index 0, symbol at index 1, method at index 2
    assert(offsetof(struct BoxedBinding, base.vtable) == 0 * oop_size);
    assert(offsetof(struct BoxedBinding, symbol)      == 1 * oop_size);
    assert(offsetof(struct BoxedBinding, method)      == 2 * oop_size);
    // ObjectInstance: base.vtable at index 0
    assert(offsetof(struct ObjectInstance, base.vtable) == 0 * oop_size);
    // Float: base.vtable at index 0, value (double) at index 1
    assert(offsetof(struct Float, base.vtable) == 0 * oop_size);
    assert(offsetof(struct Float, value)       == 1 * oop_size);

    printf("Parametric oop-field offset invariant PASSED (%zu-bit).\n", oop_size * 8);

    printf("All layout assertions PASSED successfully!\n");
    return 0;
}

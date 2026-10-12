/* oevm/tools/dump_layout.c — emit native struct layout for the target ABI. */
#include <stdio.h>
#include <stddef.h>
#include "oevm.h"

/* S(struct)  starts a struct block.
 * F(struct, "field-path", is_ref, is_ptr, offset)  emits one field.
 *   is_ref: field holds an oop (a tagged ref-word in the blob).
 *   is_ptr: field holds an internal C pointer to other blob bytes.
 */
#define FIELDS \
  S(Symbol) \
    F(Symbol, "base.vtable", 1, 0, offsetof(struct Symbol, base.vtable)) \
    F(Symbol, "length",      0, 0, offsetof(struct Symbol, length)) \
    F(Symbol, "prim_id",     0, 0, offsetof(struct Symbol, prim_id)) \
    F(Symbol, "name",        0, 0, offsetof(struct Symbol, name)) \
  S(Vtable) \
    F(Vtable, "base.vtable", 1, 0, offsetof(struct Vtable, base.vtable)) \
    F(Vtable, "delegate",    1, 0, offsetof(struct Vtable, delegate)) \
    F(Vtable, "bindings",    0, 1, offsetof(struct Vtable, bindings)) \
    F(Vtable, "capacity",    0, 0, offsetof(struct Vtable, capacity)) \
    F(Vtable, "size",        0, 0, offsetof(struct Vtable, size)) \
    F(Vtable, "is_nested",   0, 0, offsetof(struct Vtable, is_nested)) \
  S(Binding) \
    F(Binding, "symbol",     1, 0, offsetof(struct Binding, symbol)) \
    F(Binding, "method",     1, 0, offsetof(struct Binding, method)) \
  S(CompiledMethod) \
    F(CompiledMethod, "instructions",     0, 1, offsetof(struct CompiledMethod, instructions)) \
    F(CompiledMethod, "instruction_count",0, 0, offsetof(struct CompiledMethod, instruction_count)) \
    F(CompiledMethod, "num_registers",    0, 0, offsetof(struct CompiledMethod, num_registers)) \
    F(CompiledMethod, "num_arguments",    0, 0, offsetof(struct CompiledMethod, num_arguments)) \
    F(CompiledMethod, "shape",            0, 0, offsetof(struct CompiledMethod, shape)) \
    F(CompiledMethod, "constants",        0, 1, offsetof(struct CompiledMethod, constants)) \
    F(CompiledMethod, "constant_count",   0, 0, offsetof(struct CompiledMethod, constant_count)) \
  S(Closure) \
    F(Closure, "base.vtable",     1, 0, offsetof(struct Closure, base.vtable)) \
    F(Closure, "home_ctx",        1, 0, offsetof(struct Closure, home_ctx)) \
    F(Closure, "entry_point",     0, 1, offsetof(struct Closure, entry_point)) \
  S(Float) \
    F(Float, "base.vtable", 1, 0, offsetof(struct Float, base.vtable)) \
    F(Float, "value",       0, 0, offsetof(struct Float, value)) \
  S(LargeInteger) \
    F(LargeInteger, "base.vtable", 1, 0, offsetof(struct LargeInteger, base.vtable)) \
    F(LargeInteger, "sign",        0, 0, offsetof(struct LargeInteger, sign)) \
    F(LargeInteger, "mag",         0, 0, offsetof(struct LargeInteger, mag)) \
  S(String) \
    F(String, "base.vtable", 1, 0, offsetof(struct String, base.vtable)) \
    F(String, "length",      0, 0, offsetof(struct String, length)) \
    F(String, "bytes",       0, 0, offsetof(struct String, bytes)) \
  S(ObjectInstance) \
    F(ObjectInstance, "base.vtable", 1, 0, offsetof(struct ObjectInstance, base.vtable)) \
  S(BoxedBinding) \
    F(BoxedBinding, "base.vtable", 1, 0, offsetof(struct BoxedBinding, base.vtable)) \
    F(BoxedBinding, "symbol",      1, 0, offsetof(struct BoxedBinding, symbol)) \
    F(BoxedBinding, "method",      1, 0, offsetof(struct BoxedBinding, method))

int main(void) {
    printf("{\n  \"oop_size\": %zu,\n  \"structs\": {\n", sizeof(oop));
    int first_struct = 1, first_field = 1;
    #define S(name) \
        if (!first_struct) printf("\n    }},\n"); first_struct = 0; first_field = 1; \
        printf("    \"%s\": {\"size\": %zu, \"fields\": {", #name, sizeof(struct name));
    #define F(st, path, is_ref, is_ptr, off) \
        printf("%s\n      \"%s\": {\"offset\": %zu, \"is_ref\": %s, \"is_ptr\": %s}", \
               first_field ? "" : ",", path, (size_t)(off), \
               (is_ref) ? "true" : "false", (is_ptr) ? "true" : "false"); first_field = 0;
    FIELDS
    #undef S
    #undef F
    printf("\n    }}\n  }\n}\n");
    return 0;
}

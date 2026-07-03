/* oevm/src/oimg.h */
#ifndef OIMG_H
#define OIMG_H
#include <stdint.h>
#include <stddef.h>

extern const uint8_t *oimg_blob_base;
extern const uint8_t *oimg_blob_end;
extern oop builtin_registry[];
extern oop oimg_singleton[3];

/* S0 (R6): no-POSIX byte-array loader for embedded targets. Validates and
   sets oimg_blob_base/end + the header refs from an in-memory image; does not
   own `base`. oimg_load(path) is a mmap wrapper over this (desktop only). */
int oimg_load_blob(const uint8_t *base, size_t size);

/* BUILTIN_COUNT: derived from builtins.def via X-macro counter so it stays
 * in lockstep automatically when new rows are appended to the .def file.
 * Defined as an enum constant so it can be used as a C array size. */
#define BUILTIN(_idx, _kind, _name) +1
enum { BUILTIN_COUNT = (0
#include "builtins.def"
) };
#undef BUILTIN

static inline int is_in_blob(const void *p) {
    return (const uint8_t *)p >= oimg_blob_base && (const uint8_t *)p < oimg_blob_end;
}

#define OIMG_TAG(w)     ((w) & 0x3u)
#define OIMG_PAYLOAD(w) ((intptr_t)(w) >> 2)   /* arithmetic shift: sign-extends SMI */
#define OIMG_TAG_APP 0u
#define OIMG_TAG_BUILTIN 1u
#define OIMG_TAG_SMI 2u
#define OIMG_TAG_SINGLETON 3u

static inline oop oimg_resolve(uintptr_t word) {
    switch (OIMG_TAG(word)) {
        /* app_offset encodes (file_offset << 2) | 0 (Task 3); file_offset = word >> 2.
         * word == 0 is the reserved NULL sentinel (no object lives at offset 0). */
        case OIMG_TAG_APP:       return word ? (oop)(oimg_blob_base + (word >> 2)) : NULL;
        case OIMG_TAG_BUILTIN:   { uintptr_t i = word >> 2; return i < BUILTIN_COUNT ? builtin_registry[i] : NULL; }
        case OIMG_TAG_SMI:       return TAG_SMI(OIMG_PAYLOAD(word));
        default:                 { uintptr_t i = word >> 2; return i < 3 ? oimg_singleton[i] : NULL; } /* singleton */
    }
}

/* Field-read accessors: identity for RAM objects, resolve for blob objects. */
static inline oop obj_vtable(oop o) {
    if (IS_SMI(o) || o == NULL) return NULL;
    struct Object *ob = (struct Object *)o;
    return is_in_blob(o) ? oimg_resolve((uintptr_t)ob->vtable) : ob->vtable;
}
static inline const char *symbol_name(struct Symbol *s) {
    return is_in_blob(s) ? (const char *)(oimg_blob_base + ((uintptr_t)s->name >> 2))
                         : s->name;
}
static inline oop vtable_delegate(oop vt) {
    struct Vtable *v = (struct Vtable *)vt;
    return is_in_blob(vt) ? oimg_resolve((uintptr_t)v->delegate) : v->delegate;
}
static inline struct Binding *vtable_bindings(oop vt) {
    struct Vtable *v = (struct Vtable *)vt;
    return is_in_blob(vt) ? (struct Binding *)(oimg_blob_base + ((uintptr_t)v->bindings >> 2))
                          : v->bindings;
}
static inline oop binding_symbol(struct Binding *b) {
    return is_in_blob(b) ? oimg_resolve((uintptr_t)b->symbol) : b->symbol;
}
static inline oop binding_method(struct Binding *b) {
    return is_in_blob(b) ? oimg_resolve((uintptr_t)b->method) : b->method;
}
static inline const uint32_t *cm_instructions(struct CompiledMethod *m) {
    return is_in_blob(m) ? (const uint32_t *)(oimg_blob_base + ((uintptr_t)m->instructions >> 2))
                         : m->instructions;
}
static inline oop cm_const_at(struct CompiledMethod *m, size_t i) {
    if (!is_in_blob(m)) return m->constants[i];
    const uintptr_t *c = (const uintptr_t *)(oimg_blob_base + ((uintptr_t)m->constants >> 2));
    return oimg_resolve(c[i]);
}
static inline void *closure_entry(struct Closure *c) {
    return is_in_blob(c) ? (void *)(oimg_blob_base + ((uintptr_t)c->entry_point >> 2))
                         : c->entry_point;
}
static inline oop instance_slot(struct ObjectInstance *o, size_t i) {
    return is_in_blob(o) ? oimg_resolve((uintptr_t)o->slots[i]) : o->slots[i];
}
#endif

/* oevm/tests/test_oimg.c */
#include "../src/oevm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/*
 * Stubs so gc.c links standalone (no image/VM required).
 * All vtable globals are NULL; mark(NULL) returns immediately.
 * scan_vm_stack() with current_frame=NULL / vm_stack_ptr=0 is a no-op.
 */
oop IntegerVtable           = NULL;
oop FloatVtable             = NULL;
oop MethodContextVtable     = NULL;
oop FiberVtable             = NULL;
oop SymbolVtable            = NULL;
oop VtableVtable            = NULL;
oop PrimitiveClosureVtable  = NULL;
oop BytecodeClosureVtable   = NULL;
oop RunnableFiberVtable     = NULL;
oop SuspendedFiberVtable    = NULL;
oop DeadFiberVtable         = NULL;
oop current_fiber           = NULL;
oop Symbol_value            = NULL;
oop *vm_stack               = NULL;
size_t vm_stack_ptr         = 0;
struct MethodContext *current_frame = NULL;
oop boot_stack[1]           = { NULL }; /* array expected by gc.c sweep */

/* GC function declarations (gc.c does not have a separate header) */
oop gc_allocate(oop vtable, size_t size);
void gc_collect(void);
void mark_object(oop ptr);
extern void (*gc_additional_roots_hook)(void);

/* Forward declaration for the GC root hook (defined at file scope below main) */
static struct ObjectInstance *gc_test_root = NULL;
static void gc_blob_root_hook(void);

int main(void) {
    uint8_t fake[64] = {0};
    oimg_blob_base = fake; oimg_blob_end = fake + sizeof(fake);
    assert(is_in_blob(fake + 8));
    assert(!is_in_blob(fake - 1));
    assert(!is_in_blob(fake + 64));
    /* smi ref-word resolves to a tagged SMI */
    uintptr_t w = ((uintptr_t)7 << 2) | 0b10;
    assert(IS_SMI(oimg_resolve(w)) && UNTAG_SMI(oimg_resolve(w)) == 7);
    /* app ref-word with offset 0 resolves to NULL (header sentinel) */
    assert(oimg_resolve(0) == NULL);
    /* app ref-word resolves to base+offset */
    uintptr_t a = ((uintptr_t)8 << 2) | 0b00;
    assert((const uint8_t*)oimg_resolve(a) == fake + 8);
    printf("test_oimg basic PASS\n");
    {
        /* Hand-lay a Symbol whose vtable field is a builtin ref (index 7=SymbolVtable)
           and whose name bytes are INLINE.
           rete#190 stage 2 (.oimg format_version 9): a Symbol record is
           { base.vtable ref-word, size_t length, NUL-terminated bytes }, with
           the payload at offsetof(struct Symbol, name) INSIDE the record. There
           is no separate strbytes allocation and no `name` ref-word any more, so
           this test writes the bytes where the serializer writes them
           (oimg_serialize.py's Symbol emit block) rather than writing an
           app-offset. That is also why symbol_name() no longer resolves: the
           bytes are at a fixed offset in RAM and in the blob alike. */
        static uint8_t blob[64] = {0};
        oimg_blob_base = blob; oimg_blob_end = blob + sizeof(blob);
        struct Symbol *s = (struct Symbol *)blob;
        /* base.vtable = builtin(7) */
        *(uintptr_t*)((uint8_t*)s + offsetof(struct Symbol, base.vtable)) = ((uintptr_t)7 << 2) | OIMG_TAG_BUILTIN;
        /* length, then the inline NUL-terminated payload */
        const char *txt = "hi"; size_t txtlen = 2;
        *(size_t*)((uint8_t*)s + offsetof(struct Symbol, length)) = txtlen;
        memcpy((uint8_t*)s + offsetof(struct Symbol, name), txt, txtlen + 1);
        extern oop builtin_registry[]; oop marker = (oop)0xABCD; builtin_registry[7] = marker;
        assert(obj_vtable((oop)s) == marker);
        assert(s->length == txtlen);
        assert(symbol_name(s)[0] == 'h' && symbol_name(s)[1] == 'i' && symbol_name(s)[2] == 0);
        printf("test_oimg accessors PASS\n");
    }

    /*
     * GC test: RAM object referencing a fake blob survives gc_collect() with slot intact.
     *
     * Setup:
     *   - A 64-byte "fake blob" sits in a static array.
     *   - oimg_blob_base/end span the fake blob.
     *   - A fake object is placed at blob+8 (its address is the "blob oop").
     *   - A real RAM ObjectInstance is gc_allocate'd with vtable=NULL.
     *     Its slot[0] holds the blob-object pointer.
     *   - gc_blob_root_hook is registered so the RAM object is reachable during
     *     the mark phase (otherwise it would be swept as unreachable).
     *   - gc_collect() is called.
     *   - After collection the RAM object must still exist (no crash) and
     *     ram_obj->slots[0] must still point into the fake blob.
     *
     * Honesty note on RED/GREEN:
     *   The test PASSES even WITHOUT the explicit is_in_blob() guard we added
     *   to mark().  The reason: mark() already contained is_valid_heap_object(),
     *   which walks heap_list and returns 0 for the blob pointer (which was never
     *   gc_allocate'd), causing an early return before any dereference.
     *   The explicit guard is belt-and-suspenders: it makes blob-skipping
     *   unmistakably documented and stays correct if is_valid_heap_object is
     *   ever replaced by a range check.  The test's value is as a regression
     *   guard for the end-to-end property: "RAM->blob reference survives GC
     *   with slot intact".
     */
    {
        static uint8_t blob2[64] = {0};
        oimg_blob_base = blob2; oimg_blob_end = blob2 + sizeof(blob2);

        /* A fake blob-resident object at offset 8 within the blob */
        oop blob_obj = (oop)(blob2 + 8);

        /* Allocate a RAM ObjectInstance with one slot pointing into the blob */
        size_t inst_size = sizeof(struct ObjectInstance) + sizeof(oop);
        struct ObjectInstance *ram_obj =
            (struct ObjectInstance *)gc_allocate(NULL, inst_size);
        ram_obj->slots[0] = blob_obj;

        /* Register root hook so ram_obj survives the mark phase */
        gc_test_root = ram_obj;
        gc_additional_roots_hook = gc_blob_root_hook;

        /* Run the collector */
        gc_collect();

        /* After collection: RAM object still reachable, slot still points into blob */
        assert(ram_obj->slots[0] == blob_obj);
        assert(is_in_blob(ram_obj->slots[0]));

        /* Clean up */
        gc_additional_roots_hook = NULL;
        gc_test_root = NULL;

        printf("test_oimg gc_blob_ref_survives PASS\n");
    }

    /* Bounds-check guard: out-of-range builtin and singleton indices return NULL. */
    {
        uintptr_t oob_builtin   = ((uintptr_t)100 << 2) | OIMG_TAG_BUILTIN;
        uintptr_t oob_singleton = ((uintptr_t)5   << 2) | OIMG_TAG_SINGLETON;
        assert(oimg_resolve(oob_builtin)   == NULL);
        assert(oimg_resolve(oob_singleton) == NULL);
        printf("test_oimg oob_bounds_check PASS\n");
    }

    /* Immutability guard precondition: is_in_blob() correctly identifies blob
     * objects so the write-guard in OP_ST_SLOT and set_instruction_at can fire.
     * (The abort() branch itself cannot be tested in-process; this test verifies
     * the predicate is_in_blob() behaves correctly for both blob and non-blob
     * pointers, giving confidence the guard will trigger on a real mutation.) */
    {
        static uint8_t guard_blob[64] = {0};
        oimg_blob_base = guard_blob;
        oimg_blob_end  = guard_blob + sizeof(guard_blob);

        /* Any pointer inside the region must be detected as a blob object. */
        void *inside_ptr = (void *)(guard_blob + 8);
        assert(is_in_blob(inside_ptr));

        /* A RAM-allocated object must NOT be detected as a blob object. */
        struct ObjectInstance *ram_obj = (struct ObjectInstance *)
            gc_allocate(NULL, sizeof(struct ObjectInstance) + sizeof(oop));
        assert(!is_in_blob(ram_obj));

        printf("test_oimg immutability_guard_precondition PASS\n");
        oimg_blob_base = NULL; oimg_blob_end = NULL;
    }

    return 0;
}

/*
 * Root hook used by the GC test above.
 * Called by gc_collect() during the mark phase to keep gc_test_root alive.
 */
static void gc_blob_root_hook(void) {
    if (gc_test_root != NULL) {
        mark_object((oop)gc_test_root);
    }
}

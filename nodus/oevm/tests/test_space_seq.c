/* Unit test for the space-sync seam (src/space.h "Space-sync" section) — the
   per-space monotonic counter (porta space-sync design D5), the per-channel
   last_seq, and the prefix-scoped changed-since scan that backs the nsl verb
   `Channel>>changedSince:max:`.

   Every property below calls the space_* functions DIRECTLY — no send(), no
   primitive dispatch anywhere — the same discipline tests/test_space.c states
   as its property 6: the C seam must be real, not an artifact of going through
   the VM.

   Build (Model-B link set + small capacity overrides for determinism — see
   tools/run_c_tests.sh):
     gcc -std=c99 -Isrc -DOEVM_SPACE_MAX_CHANNELS=16 -DOEVM_SPACE_QUEUE_DEPTH=8 \
         tests/test_space_seq.c <ENG_B> -lm -o /tmp/tss && /tmp/tss

   Properties:
     1. the counter starts at 0 and every write advances it by exactly one — a
        cell write and a stream post alike. Exactly one matters: space_put fans
        its write out through space_post_id, so a stamp applied in both places
        would double-count a cell.
     2. last_seq is per channel and equals the counter at that channel's last
        write; an untouched channel keeps its old value.
     3. space_stamp() is strictly increasing and interleaves with writes in ONE
        sequence — which is what lets a captured stream entry share a number
        space with a cell write without ever colliding.
     4. the scan is CELLS ONLY and cursor-exclusive.
     5. the scan is prefix-scoped and SEGMENT-ALIGNED: "obs" matches "obs" and
        "obs/health", never "obscure" (the A1 rule Channel>>resolve: obeys for
        whole paths, restated for prefixes).
     6. the scan is ascending by last_seq and honours max — which is what makes
        "advance the cursor to the last entry sent" safe under trimming.
     7. a shed write (full ring) still advances the counter: declaring the write
        is not contingent on delivery, exactly as the kind fix already is.
     8. the row accessors answer the live row the scan named.
*/
#define _POSIX_C_SOURCE 200112L
#include "../src/oevm.h"
#include "../src/space.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

extern void heap_init(void);
extern void kernel_boot(void);

#define P(s) (s), strlen(s)

int main(void) {
    heap_init();
    kernel_boot();

    /* --- Property 1: the counter starts at 0 and advances by one per write --- */
    assert(space_seq() == 0 && "no write has happened yet");
    space_put(P("obs/health"), TAG_SMI(1));
    uint32_t s1 = space_seq();
    assert(s1 == 1 && "a cell write advances the counter by exactly one");
    space_post(P("tel/log"), TAG_SMI(2));
    assert(space_seq() == 2 && "a stream post advances it too, by exactly one");

    /* --- Property 2: last_seq is per channel --- */
    int obs = space_intern(P("obs/health"));
    int tel = space_intern(P("tel/log"));
    assert(space_last_seq_of(obs) == 1);
    assert(space_last_seq_of(tel) == 2);
    space_put(P("obs/health"), TAG_SMI(3));
    assert(space_last_seq_of(obs) == 3 && "rewriting advances this channel");
    assert(space_last_seq_of(tel) == 2 && "and leaves the other alone");

    /* --- Property 3: stamp interleaves in the one sequence --- */
    uint32_t a = space_stamp();
    uint32_t b = space_stamp();
    assert(b == a + 1 && "two stamps never collide");
    space_put(P("obs/name"), TAG_SMI(4));
    assert(space_seq() == b + 1 && "a write after a stamp continues the sequence");

    /* --- Property 4: cells only, cursor-exclusive --- */
    {
        int ids[OEVM_SPACE_MAX_CHANNELS];
        int n = space_changed_since(0, P(""), ids, OEVM_SPACE_MAX_CHANNELS);
        int saw_stream = 0;
        for (int i = 0; i < n; i++) {
            if (space_kind_of(ids[i]) != SPACE_KIND_CELL) saw_stream = 1;
        }
        assert(!saw_stream && "a stream must never ride the cell scan");
        assert(n == 2 && "obs/health and obs/name, not tel/log");

        n = space_changed_since(space_seq(), P(""), ids, OEVM_SPACE_MAX_CHANNELS);
        assert(n == 0 && "the cursor is exclusive: nothing is newer than now");
    }

    /* --- Property 5: prefix scoping is segment-aligned --- */
    space_put(P("obscure"), TAG_SMI(5));
    {
        int ids[OEVM_SPACE_MAX_CHANNELS];
        int n = space_changed_since(0, P("obs"), ids, OEVM_SPACE_MAX_CHANNELS);
        assert(n == 2 && "obs/health + obs/name; obscure is a different name");
        for (int i = 0; i < n; i++) {
            size_t len = 0;
            const char *path = space_path_of(ids[i], &len);
            assert(!(len == 7 && memcmp(path, "obscure", 7) == 0));
        }
        n = space_changed_since(0, P("obs/health"), ids, OEVM_SPACE_MAX_CHANNELS);
        assert(n == 1 && "a prefix equal to the whole path matches that path");
    }

    /* --- Property 6: ascending, and max is honoured --- */
    {
        int ids[OEVM_SPACE_MAX_CHANNELS];
        int n = space_changed_since(0, P("obs"), ids, 1);
        assert(n == 1 && "max caps the answer");
        assert(space_last_seq_of(ids[0]) == 3 && "and it is the OLDEST pending write");

        n = space_changed_since(0, P("obs"), ids, OEVM_SPACE_MAX_CHANNELS);
        assert(n == 2);
        assert(space_last_seq_of(ids[0]) < space_last_seq_of(ids[1]) && "ascending");
    }

    /* --- Property 7: a shed write still advances the counter --- */
    {
        uint32_t before = space_seq();
        int floods = OEVM_SPACE_QUEUE_DEPTH + 4;
        for (int i = 0; i < floods; i++) space_post(P("tel/flood"), TAG_SMI(i));
        assert(space_seq() == before + (uint32_t)floods &&
               "every write is stamped, delivered or shed");
        assert(space_overruns(P("tel/flood")) > 0 && "and the shedding really happened");
    }

    /* --- Property 8: the row accessors answer the live row --- */
    {
        int ids[OEVM_SPACE_MAX_CHANNELS];
        int n = space_changed_since(0, P("obs/name"), ids, OEVM_SPACE_MAX_CHANNELS);
        assert(n == 1);
        size_t len = 0;
        const char *path = space_path_of(ids[0], &len);
        assert(len == 8 && memcmp(path, "obs/name", 8) == 0);
        assert(space_kind_of(ids[0]) == SPACE_KIND_CELL);
        assert(space_value_of(ids[0]) == TAG_SMI(4));
    }

    printf("test_space_seq: all properties passed\n");
    return 0;
}

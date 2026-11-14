#ifndef OEVM_JOURNAL_BACKEND_H
#define OEVM_JOURNAL_BACKEND_H
/* The outbox journal seam (rete#443; design docs/design/2026-11-14-443-outbox-journal-design.md).
   Exactly one impl is linked per OEVM_TARGET: backend/posix/journal_posix.c (a file, so
   the nsl suite can reboot a host VM over the same journal) and
   backend/zephyr/journal_zephyr.c (FCB over the `outbox` fixed partition).

   The journal is a linear ring of OPAQUE records: the caller (PortaBridge) owns the
   row encoding; this seam owns flash. Every data record gets a position `pos`, which
   only grows for the life of the journal, so "every record <= pos is acked" is one
   number. A retire appends a small MARKER record carrying that number; FCB cannot
   delete one entry, so retirement is a later record, never an edit.

   When the ring is full the next append erases the OLDEST sector, and the answer is
   how many of that sector's data records were still unacked (they are lost, and the
   caller counts them into `sdr`). */
#include <stdint.h>

/* Answers 1 when this board has a journal (opening it, and erasing it if its sector
   headers are of another format — a reflash that changed the journal), 0 when it has
   none: no `outbox` partition on target, no OEVM_JOURNAL path on the host. */
int journal_open(void);

/* Releases the journal; the next journal_open reads it back from storage. Used by the
   host test to stand in for a reboot. */
void journal_close(void);

/* Appends one data record (len bytes) and sets *pos. Answers the number of unacked
   records dropped by a rotation this call (>= 0), or < 0 when there is no journal or
   the record cannot fit a sector. */
int journal_append(const uint8_t *buf, int len, uint32_t *pos);

/* Marks every data record <= pos acked by appending a marker. A pos at or below the
   current one writes nothing. Answers like journal_append: unacked records the
   marker's own rotation dropped (>= 0), or < 0 when there is no journal. */
int journal_retire(uint32_t pos);

/* The newest retired pos (0 when nothing was ever retired). */
uint32_t journal_last_retired(void);

/* Copies the oldest data record with pos > after into buf (cap bytes) and sets *pos.
   Answers its length (>= 0), -1 when there is none (or no journal), -2 when cap is too
   small. Markers are skipped. The seam remembers where the last answer was, so walking
   the journal by feeding each answer's pos back in costs one record per call, not a
   scan from the oldest sector: the caller refills a small RAM window this way, because
   a full journal (~20,000 rows) does not fit a node's RAM. */
int journal_next(uint32_t after, uint32_t *pos, uint8_t *buf, int cap);

/* A lower bound on the data records lost when journal_open erased a journal of
   another format: one per non-blank sector erased (their rows cannot be parsed, so
   they cannot be counted exactly). 0 when nothing was erased. */
int journal_lost_at_open(void);

#endif /* OEVM_JOURNAL_BACKEND_H */

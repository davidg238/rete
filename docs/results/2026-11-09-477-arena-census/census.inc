/* rete#477 census (throwaway): allocations and peak survivors by vtable. */
#include <stdio.h>
#include "oimg.h"
#include <time.h>
/* CPU time (clock(): standard C, so -std=c99 keeps it; µs resolution on Linux). */
static clock_t cen_c0, cen_c1;
static double cen_tot_sum = 0, cen_tot_max = 0, cen_mark_sum = 0, cen_sweep_sum = 0, cen_live_objs_sum = 0;
static size_t cen_timed = 0, cen_live_objs = 0;
static double cen_us(clock_t a, clock_t b) { return (double)(b - a) * 1e6 / CLOCKS_PER_SEC; }
static void census_gc_begin(void) { cen_c0 = clock(); cen_live_objs = 0; }
static void census_sweep_begin(void) { cen_c1 = clock(); }
static void census_gc_end(void) {
    clock_t c2 = clock(); double tot = cen_us(cen_c0, c2), mk = cen_us(cen_c0, cen_c1);
    cen_timed++; cen_tot_sum += tot; cen_mark_sum += mk; cen_sweep_sum += tot - mk; cen_live_objs_sum += cen_live_objs;
    if (tot > cen_tot_max) cen_tot_max = tot;
}
#define CN 512
typedef struct { oop vt; size_t n, b; } cen_row;
static cen_row cen_alloc[CN], cen_live[CN], cen_peak[CN];
static size_t cen_peak_bytes = 0, cen_gcs = 0;
typedef struct { oop vt; size_t sz; } cen_big;
static cen_big cen_bigs_cur[256], cen_bigs_peak[256]; static int cen_nb_cur = 0, cen_nb_peak = 0;
static void cen_add(cen_row *t, oop vt, size_t b) {
    size_t i = ((uintptr_t)vt >> 3) % CN;
    for (size_t k = 0; k < CN; k++, i = (i + 1) % CN) {
        if (t[i].vt == vt || t[i].vt == NULL) { t[i].vt = vt; t[i].n++; t[i].b += b; return; }
    }
}
static const char *cen_name(oop vt) {
#define K(v) if (vt == v) return #v;
    K(IntegerVtable) K(FloatVtable) K(LargeIntegerVtable) K(MethodContextVtable) K(StackVtable) K(ContextVtable)
    K(SymbolVtable) K(StringVtable) K(RawBytesVtable) K(VtableVtable) K(PrimitiveClosureVtable)
    K(BytecodeClosureVtable) K(BoxedBindingVtable)
#undef K
    const char *n = oimg_vtable_name(vt); if (n) return n;
    if (vt && *(oop *)vt == VtableVtable) { static char buf[32]; snprintf(buf, sizeof buf, "vt@%p", (void *)vt); return buf; }
    return "?";
}
static oop cen_last_vt; static void census_alloc(oop vt, size_t need) { cen_last_vt = vt; cen_add(cen_alloc, vt, need); }
static void census_live(struct HeapHeader *h) {
    oop o = (oop)(h + 1); oop vt = *(oop *)o; size_t b = sizeof(struct HeapHeader) + hh_size(h);
    cen_add(cen_live, vt, b); cen_live_objs++;
    if (b >= 1024 && cen_nb_cur < 256) { cen_bigs_cur[cen_nb_cur].vt = vt; cen_bigs_cur[cen_nb_cur++].sz = b; }
}
#define SEG_CENSUS_LIVE(h) census_live(h)
static int cen_cmp(const void *a, const void *b) { const cen_row *x = a, *y = b; return (x->b < y->b) - (x->b > y->b); }
static void census_end(size_t live) {
    cen_gcs++;
    if (live > cen_peak_bytes) { cen_peak_bytes = live; memcpy(cen_peak, cen_live, sizeof cen_live);
        memcpy(cen_bigs_peak, cen_bigs_cur, sizeof cen_bigs_cur); cen_nb_peak = cen_nb_cur; }
    memset(cen_live, 0, sizeof cen_live); cen_nb_cur = 0;
}
static void cen_print(const char *title, cen_row *t, size_t total) {
    qsort(t, CN, sizeof *t, cen_cmp);
    fprintf(stderr, "CENSUS %s (total %zu B)\n", title, total);
    for (int i = 0; i < 25 && t[i].vt; i++)
        fprintf(stderr, "  %-34s %9zu objs %11zu B %5.1f%%\n", cen_name(t[i].vt), t[i].n, t[i].b, 100.0 * t[i].b / (total ? total : 1));
}
extern oop g_active_stack;
void census_report(void) {
    if (cen_timed) fprintf(stderr, "CENSUS TIMING gcs=%zu mean %.1f us (mark+roots %.1f, sweep %.1f) max %.1f us; mean live objs %.0f; arena %u B\n",
        cen_timed, cen_tot_sum / cen_timed, cen_mark_sum / cen_timed, cen_sweep_sum / cen_timed, cen_tot_max, cen_live_objs_sum / cen_timed, (unsigned)OEVM_HEAP_SIZE);
    if (g_active_stack) { struct Stack *st=(struct Stack*)g_active_stack; uint32_t hw=0;
      for (uint32_t i=0;i<st->capacity;i++) if (st->data[i]) hw=i+1;
      fprintf(stderr,"CENSUS stack capacity=%u words, high-water(nonzero)=%u words = %u B\n",st->capacity,hw,hw*(unsigned)sizeof(oop)); }
    size_t at = 0; for (int i = 0; i < CN; i++) at += cen_alloc[i].b;
    fprintf(stderr, "CENSUS gcs=%zu peak_live=%zu high_water=%zu\n", cen_gcs, cen_peak_bytes, heap_high_water);
    cen_print("PEAK LIVE by class", cen_peak, cen_peak_bytes);
    fprintf(stderr, "CENSUS big objects (>=1KB) at peak: %d\n", cen_nb_peak);
    for (int i = 0; i < cen_nb_peak; i++) fprintf(stderr, "  %-34s %7zu B\n", cen_name(cen_bigs_peak[i].vt), cen_bigs_peak[i].sz);
    cen_print("ALLOCATED by class", cen_alloc, at);
}
#include <signal.h>
#include <unistd.h>
static void census_on_term(int sig) { (void)sig; census_report(); _exit(0); }
static oop census_oom(size_t size) {
    size_t fsum = 0, fmax = 0, fn = 0;
    for (int b = 0; b < FREE_BIN_COUNT; b++) for (struct HeapHeader *h = free_bins[b]; h; h = *hh_next(h)) { fn++; fsum += hh_size(h); if (hh_size(h) > fmax) fmax = hh_size(h); }
    if (big_free) { fn++; fsum += hh_size(big_free); if (hh_size(big_free) > fmax) fmax = hh_size(big_free); }
#ifdef OEVM_SEG_SPIKE
    for (struct HeapHeader *h = large_free; h; h = *hh_next(h)) { fn++; fsum += hh_size(h); if (hh_size(h) > fmax) fmax = hh_size(h); }
    size_t tail = (size_t)(large_lo - heap_top);
#else
    size_t tail = (size_t)(heap_end - heap_top);
#endif
    fprintf(stderr, "CENSUS OOM: request %zu B (%s); live after GC %zu B; free %zu B in %zu blocks, largest %zu B; bump tail %zu B; arena %u\n",
        size, cen_name(cen_last_vt), heap_live_bytes, fsum, fn, fmax, tail, (unsigned)OEVM_HEAP_SIZE);
#ifdef OEVM_SEG_SPIKE
    fprintf(stderr, "CENSUS SEG small [0,%td) large [%td,%u) gap %td\n", heap_top-heap_base, large_lo-heap_base, (unsigned)OEVM_HEAP_SIZE, large_lo-heap_top);
    for (uint8_t *c = large_lo; c < heap_end; ) { struct HeapHeader *h = (struct HeapHeader *)c;
        fprintf(stderr, "  L @%6td %5zu %s %s\n", c-heap_base, sizeof *h + hh_size(h), hh_marked(h) ? "LIVE" : "free", hh_marked(h) ? cen_name(*(oop *)(h+1)) : "");
        c += sizeof *h + hh_size(h); }
    { size_t sl=0, sf=0, nl=0; for (uint8_t *c = heap_base; c < heap_top; ) { struct HeapHeader *h=(struct HeapHeader*)c; if (hh_marked(h)) { sl+=sizeof *h+hh_size(h); nl++; } else sf+=sizeof *h+hh_size(h); c+=sizeof *h+hh_size(h); }
      fprintf(stderr, "  small region: live %zu B in %zu objs, dead/free %zu B\n", sl, nl, sf); }
#endif
    census_report(); abort(); return NULL;
}
void census_install(void) { oevm_oom_handler = census_oom; atexit(census_report); signal(SIGTERM, census_on_term); signal(SIGINT, census_on_term); }

/* sp_slab.c -- size-class slab allocation for GC objects and heap strings,
 * with the generations and the mark kept in bitmaps beside the chunks.
 *
 * Every object and every heap string used to be one malloc and, when it
 * died, one free. On a server that allocates nine thousand of them per
 * request from thirty workers, that was 40% of the CPU: glibc's arena locks
 * and consolidation on the way in, and a sweep that handed every dead block
 * back through free() on the way out, cold, from whichever thread ran the
 * collection. Nothing a faster general allocator fixes -- tcmalloc measured
 * the same -- because the cost is the per-block call and the cross-thread
 * free, not the arithmetic.
 *
 * Here a block is a slot in a 16 KB chunk of one size class, and what the
 * collector knows about a slot lives in seven bitmaps kept in the arena's
 * metadata chunks, one bit per slot each, never in the slot itself:
 *
 *   young[0], young[1]  allocated in the epoch of that parity (see below)
 *   old                 promoted, or allocated as a container payload
 *   mark                reached by this cycle's mark
 *   fin                 an object whose death runs a finalizer or recycler
 *   str                 a heap string (its bytes count on the string side)
 *   pin                 never freed by a sweep (a frozen heap string, a
 *                       payload: both die by an explicit free)
 *
 * A slot is free when it is in no generation (young[0], young[1], old all
 * clear), and allocation is a find-first-zero over those three words and
 * one atomic or into the current epoch's young word. That replaced the
 * per-chunk free list linked through the blocks, and the reason is the
 * sweep: with the list, a sweep touched the header of every dead object to
 * unlink it and to thread the free list through it, and on a server that
 * churns a hundred megabytes of small blocks between two collections that
 * was a stream of cache misses worth a fifth of the process. A sweep here
 * reads the bitmaps of a chunk -- a few cache lines for sixteen kilobytes of
 * slots -- and never the slots, except the dead ones whose `fin` bit says a
 * finalizer has to run.
 *
 * The EPOCH is what tells the generation a concurrent sweep is reclaiming
 * from what the running program has allocated since the barrier: the
 * collector flips the parity under the barrier, so everything allocated
 * from then on goes into the other young bitmap, and the sweep frees from
 * the one that stopped growing. The mark promotes what it reaches -- it
 * sets `old` on a young survivor, as the header's `old` bit is set there
 * too -- so after the mark the swept young word is exactly the dead plus
 * the promoted, and clearing it whole is the sweep's write.
 *
 * Every chunk lives inside one address range reserved at startup (16 GB of
 * untouched, uncommitted pages; smaller where the system refuses), so a free
 * tells a slab block from a malloc'd one by a range check, and a slot finds
 * its chunk by masking its address. The range is handed out in 4 MB arenas
 * whose first chunk holds the arena's chunk headers and whose next seven
 * hold the bitmaps, so a released chunk keeps no resident page at all. At
 * the end of a full cycle every chunk that is entirely free, beyond a small
 * reserve per worker, is given back to the OS with madvise and re-carved
 * from scratch when next used; that is what malloc_trim did for these
 * blocks, at a syscall per idle chunk instead of a walk of thirty arenas.
 *
 * A chunk belongs to one worker for allocation and no other worker
 * allocates from it, and every chunk a worker owns is on its owned list,
 * which is what its sweep walks. Frees come from anywhere -- a sweeper
 * thread, a finalizer on another worker, the explicit free of a payload --
 * and are atomic bit clears; the one structure they share with the owner
 * beyond the bitmaps is the owner's available list, pushed with the same
 * compare-and-swap the owner pops with.
 *
 * SPINEL_GC_SLAB=0 turns this off and every block is a malloc again, which
 * is the configuration ASAN wants: a slab hides a use-after-free from it.
 * With jemalloc as the process's malloc it is off by default (see
 * sp_slab_jemalloc_present); SPINEL_GC_SLAB=1 turns it on there. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <time.h>
#include "sp_gc.h"
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#define SP_SLAB_ARENA   ((size_t)4 << 20)
#define SP_SLAB_CHUNK   ((size_t)16 << 10)
#define SP_SLAB_NCHUNK  (SP_SLAB_ARENA / SP_SLAB_CHUNK)   /* 256 */
#define SP_SLAB_NCLS    27
#define SP_SLAB_MAX     2048
/* chunk 0 is the header table (256 x 64 B); chunks 1..SP_SLAB_META hold the
   bitmaps (256 x 448 B = 112 KB = 7 chunks); the rest are carved */
#define SP_SLAB_META    7
#define SP_SLAB_FIRST   (1 + SP_SLAB_META)
/* fully free chunks a worker keeps resident across a full cycle, at least (256 KB) */
#define SP_SLAB_RESERVE 16
/* the most slots a chunk holds (16 KB / 32 B), in 64-bit words */
#define SP_SLAB_NW      8

#ifdef SP_THREADS
#define SP_SLAB_NWK SP_MAX_WORKERS
#define SP_SLAB_WID() (sp_worker_id)
static inline int sp_worker_id_shadow(void) { return sp_worker_id; }
#else
#define SP_SLAB_NWK 1
#define SP_SLAB_WID() 0
static inline int sp_worker_id_shadow(void) { return 0; }
#endif
void sp_slab_note(const void *p, int what);
extern int sp_slab_verify_on;

/* 16-byte steps to 256, then coarser: an object header is 48 bytes and a
   string header 24, and most blocks are a header plus a few words, so the
   fine steps are where the population is. Past SP_SLAB_MAX a block is
   malloc'd as before. */
static const uint16_t sp_slab_csize[SP_SLAB_NCLS] = {
  32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 256,
  320, 384, 448, 512, 640, 768, 896, 1024, 1280, 1536, 1792, 2048 };
static uint8_t sp_slab_cls_of[SP_SLAB_MAX / 16 + 1];   /* (need+15)/16 -> class */
/* offset-in-chunk / csize as a multiply: (off * recip) >> 32 is exact for
   every off < 16 KB, checked at init */
static uint32_t sp_slab_recip[SP_SLAB_NCLS];
static uint16_t sp_slab_nslots_of[SP_SLAB_NCLS];

typedef struct sp_slab_chunk {
  struct sp_slab_chunk *next_avail;   /* the owner's available list, per class */
  struct sp_slab_chunk *own_next, *own_prev;   /* the owner's owned list, every chunk it carved */
  uint32_t nslots;
  uint16_t hint;                      /* the word the last allocation came from */
  uint16_t cls, wid;
  uint8_t on_avail;                   /* listed on the owner's available list */
  uint8_t in_use;                     /* holds a class; 0 = empty, on the global pool */
  uint8_t touched;                    /* has resident pages since the last release */
  uint8_t _pad[64 - 8 * 3 - 4 - 2 * 3 - 3];
} sp_slab_chunk;
typedef char sp_slab_chunk_is_one_line[sizeof(sp_slab_chunk) == 64 ? 1 : -1];

/* one slot's worth of state per chunk: seven bitmaps of SP_SLAB_NW words */
typedef struct sp_slab_bm {
  uint64_t young[2][SP_SLAB_NW];
  uint64_t old[SP_SLAB_NW];
  uint64_t mark[SP_SLAB_NW];
  uint64_t fin[SP_SLAB_NW];
  uint64_t str[SP_SLAB_NW];
  uint64_t pin[SP_SLAB_NW];
} sp_slab_bm;
typedef char sp_slab_bm_is_448[sizeof(sp_slab_bm) == 448 ? 1 : -1];
typedef char sp_slab_meta_fits[SP_SLAB_NCHUNK * sizeof(sp_slab_bm) <= SP_SLAB_META * SP_SLAB_CHUNK ? 1 : -1];

typedef struct sp_slab_arena {
  sp_slab_chunk ch[SP_SLAB_NCHUNK];   /* ch[0] is this table itself, never carved */
} sp_slab_arena;

typedef struct {
  sp_slab_chunk *cur[SP_SLAB_NCLS];
  sp_slab_chunk *avail[SP_SLAB_NCLS];
  sp_slab_chunk *owned;              /* every chunk this worker carved, doubly linked */
  long taken;        /* chunks this worker started allocating into since the last release */
  int sweeping;      /* a sweep of this worker's chunks is running (the release waits it out) */
  char _pad[64 - ((2 * SP_SLAB_NCLS * sizeof(void *) + sizeof(void *) + sizeof(long) + sizeof(int)) % 64)];
} sp_slab_worker;

static sp_slab_worker sp_slab_wk[SP_SLAB_NWK];
uintptr_t sp_slab_base = 0;   /* the reservation (read inline by sp_slab_owns) */
static uintptr_t sp_slab_brk = 0;   /* how much of it is in use */
size_t sp_slab_cap = 0;
static sp_slab_chunk *sp_slab_empty = NULL;   /* chunks holding no class */
int sp_slab_on = -1;                          /* decided once from the environment */
/* The parity of the epoch new allocations join. The collector flips it under
   the barrier (sp_slab_epoch_flip), where no mutator runs. */
unsigned sp_slab_epoch = 0;
#ifdef SP_THREADS
#include <pthread.h>
static pthread_mutex_t sp_slab_lock = PTHREAD_MUTEX_INITIALIZER;
#define SP_SLAB_LOCK()   pthread_mutex_lock(&sp_slab_lock)
#define SP_SLAB_UNLOCK() pthread_mutex_unlock(&sp_slab_lock)
#else
#define SP_SLAB_LOCK()   ((void)0)
#define SP_SLAB_UNLOCK() ((void)0)
#endif

/* One reservation, aligned to the arena size so a slot's arena and chunk are
   address arithmetic. Mapped twice the size and trimmed to the aligned
   middle; the pages are untouched until a chunk is carved, so the size costs
   nothing but address space. */
static void sp_slab_reserve(void) {
  size_t want = (size_t)16 << 30;
  while (want >= ((size_t)256 << 20)) {
    size_t len = want + SP_SLAB_ARENA;
    void *m = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m != MAP_FAILED) {
      uintptr_t lo = (uintptr_t)m, base = (lo + SP_SLAB_ARENA - 1) & ~(SP_SLAB_ARENA - 1);
      if (base > lo) munmap((void *)lo, base - lo);
      uintptr_t hi = lo + len, top = base + want;
      if (hi > top) munmap((void *)top, hi - top);
      sp_slab_base = sp_slab_brk = base;
      sp_slab_cap = want;
      /* Keep the untouched reservation out of a core dump: the kernel writes a
         mapping's every page, zero or not, and 16 GB of them took a crashing
         program 20 s to die (a minute and more on CI). The arenas actually
         carved are put back in, one at a time, as they are handed out. */
#ifdef MADV_DONTDUMP
      madvise((void *)base, want, MADV_DONTDUMP);
#endif
      return;
    }
    want >>= 2;
  }
  sp_slab_on = 0;   /* no range at all: every block is a malloc */
}

/* Is jemalloc the process's malloc? Its thread caches already do what the
   slab does, and measured beside them the slab is a loss (campfire's room
   page 2,680 req/s with jemalloc alone against 2,470 with the slab on top:
   the per-free atomics land on caches that were already free-list pops),
   where on glibc it is a gain (2,140 against 1,940). So the slab defaults
   off when jemalloc is present, and SPINEL_GC_SLAB=1 asks for it anyway.
   Detection is jemalloc's own mallctl symbol, resolved by the dynamic linker
   from a linked or preloaded jemalloc; nothing else defines it. */
#if defined(__APPLE__)
#include <dlfcn.h>
static int sp_slab_jemalloc_present(void) { return dlsym(RTLD_DEFAULT, "mallctl") != NULL; }
#else
extern int mallctl(const char *, void *, size_t *, void *, size_t) __attribute__((weak));
static int sp_slab_jemalloc_present(void) { return mallctl != NULL; }
#endif

static void sp_slab_init(void) {
  const char *e = getenv("SPINEL_GC_SLAB");
  int c = 0;
  for (unsigned i = 0; i <= SP_SLAB_MAX / 16; i++) {
    while (c < SP_SLAB_NCLS - 1 && sp_slab_csize[c] < i * 16) c++;
    sp_slab_cls_of[i] = (uint8_t)c;
  }
  for (int k = 0; k < SP_SLAB_NCLS; k++) {
    unsigned cs = sp_slab_csize[k];
    sp_slab_nslots_of[k] = (uint16_t)(SP_SLAB_CHUNK / cs);
    /* the smallest reciprocal that divides every in-chunk offset exactly */
    uint32_t r = (uint32_t)((((uint64_t)1 << 32) + cs - 1) / cs);
    for (unsigned off = 0; off < SP_SLAB_CHUNK; off += 16)
      if ((unsigned)(((uint64_t)off * r) >> 32) != off / cs) { r = 0; break; }
    sp_slab_recip[k] = r;   /* 0: divide (never, checked here) */
  }
  if (e && *e) sp_slab_on = (*e != '0');
  else sp_slab_on = !sp_slab_jemalloc_present();
  if (sp_slab_on) sp_slab_reserve();
}

static inline sp_slab_chunk *sp_slab_chunk_of(const void *p) {
  uintptr_t a = (uintptr_t)p;
  sp_slab_arena *ar = (sp_slab_arena *)(a & ~(SP_SLAB_ARENA - 1));
  return &ar->ch[(a & (SP_SLAB_ARENA - 1)) / SP_SLAB_CHUNK];
}
static inline char *sp_slab_chunk_base(sp_slab_chunk *ch) {
  sp_slab_arena *ar = (sp_slab_arena *)((uintptr_t)ch & ~(SP_SLAB_ARENA - 1));
  return (char *)ar + (size_t)(ch - ar->ch) * SP_SLAB_CHUNK;
}
static inline sp_slab_bm *sp_slab_bm_of(sp_slab_chunk *ch) {
  sp_slab_arena *ar = (sp_slab_arena *)((uintptr_t)ch & ~(SP_SLAB_ARENA - 1));
  return (sp_slab_bm *)((char *)ar + SP_SLAB_CHUNK) + (ch - ar->ch);
}
/* a pointer anywhere inside a slot names it: the chunk, its bitmaps, and the
   slot's word and bit */
typedef struct { sp_slab_chunk *ch; sp_slab_bm *bm; unsigned w; uint64_t bit; unsigned idx; } sp_slab_loc;
static inline void sp_slab_locate(const void *p, sp_slab_loc *l) {
  uintptr_t a = (uintptr_t)p;
  l->ch = sp_slab_chunk_of(p);
  l->bm = sp_slab_bm_of(l->ch);
  unsigned off = (unsigned)(a & (SP_SLAB_CHUNK - 1));
  unsigned cls = l->ch->cls;
  unsigned idx = sp_slab_recip[cls] ? (unsigned)(((uint64_t)off * sp_slab_recip[cls]) >> 32) : off / sp_slab_csize[cls];
  l->idx = idx;
  l->w = idx >> 6;
  l->bit = (uint64_t)1 << (idx & 63);
}

/* The next arena of the reservation: its header table and bitmaps are zero
   already (an untouched page reads as zero), so only the empty list needs
   writing. */
static int sp_slab_next_arena(void) {
  if (sp_slab_brk + SP_SLAB_ARENA > sp_slab_base + sp_slab_cap) return 0;
  sp_slab_arena *ar = (sp_slab_arena *)sp_slab_brk;
  sp_slab_brk += SP_SLAB_ARENA;
#ifdef MADV_DODUMP
  madvise((void *)ar, SP_SLAB_ARENA, MADV_DODUMP);   /* this arena holds objects: dump it */
#endif
  for (int i = (int)SP_SLAB_NCHUNK - 1; i >= SP_SLAB_FIRST; i--) {   /* pops then ascend in address */
    ar->ch[i].next_avail = sp_slab_empty;
    sp_slab_empty = &ar->ch[i];
  }
  return 1;
}

/* A free slot of the chunk, claimed into the current epoch's young word
   (or, for a payload, into old and pin). The owner is the only claimer, so
   the search reads the three generation words plainly: a bit a sweeper
   clears meanwhile is a slot it may find next time, and no bit it reads as
   clear can be set by anyone but itself. The claim is an atomic or all the
   same, because a sweep carrying an aged survivor into the current epoch
   writes the same word. NULL when the chunk has no free slot. */
static inline void *sp_slab_take(sp_slab_chunk *ch, unsigned csize, int payload) {
  sp_slab_bm *bm = sp_slab_bm_of(ch);
  unsigned nw = (ch->nslots + 63) >> 6;
  unsigned w = ch->hint;
  unsigned e = sp_slab_epoch & 1;
  for (unsigned n = 0; n < nw; n++, w = (w + 1 == nw) ? 0 : w + 1) {
    uint64_t tail = (w == nw - 1 && (ch->nslots & 63)) ? ~(uint64_t)0 << (ch->nslots & 63) : 0;
    for (;;) {
      uint64_t used = __atomic_load_n(&bm->young[0][w], __ATOMIC_RELAXED) |
                      __atomic_load_n(&bm->young[1][w], __ATOMIC_RELAXED) |
                      __atomic_load_n(&bm->old[w], __ATOMIC_RELAXED) | tail;
      if (used == ~(uint64_t)0) break;
      unsigned i = (unsigned)__builtin_ctzll(~used);
      uint64_t bit = (uint64_t)1 << i;
      /* The three words are not one snapshot: a pool handing a parked header
         back (sp_slab_relive, from any thread) sets its young bit and then
         clears its old bit, and a read that took young before and old after
         sees a free slot that is not. So the claim is checked: the bit must
         not have been set already, and the other generations must still be
         clear once it is; otherwise the claim is undone and the word read
         again. A payload's claim (pin, then old) is checked the same way. */
      uint64_t *claim = payload ? &bm->old[w] : &bm->young[e][w];
      if (payload) __atomic_fetch_or(&bm->pin[w], bit, __ATOMIC_RELAXED);   /* pin before old: see sp_slab_pin */
      uint64_t was = __atomic_fetch_or(claim, bit, __ATOMIC_ACQ_REL);
      uint64_t others = __atomic_load_n(&bm->young[e ^ 1][w], __ATOMIC_RELAXED) |
                        (payload ? __atomic_load_n(&bm->young[e][w], __ATOMIC_RELAXED)
                                 : __atomic_load_n(&bm->old[w], __ATOMIC_RELAXED));
      if ((was & bit) || (others & bit)) {
        if (!(was & bit)) __atomic_fetch_and(claim, ~bit, __ATOMIC_RELAXED);
        if (payload && !(was & bit)) __atomic_fetch_and(&bm->pin[w], ~bit, __ATOMIC_RELAXED);
        continue;
      }
      ch->hint = (uint16_t)w;
      if (sp_slab_verify_on) sp_slab_note(sp_slab_chunk_base(ch) + (size_t)((w << 6) + i) * csize, 40 + (int)e + (payload ? 2 : 0));
      return sp_slab_chunk_base(ch) + (size_t)((w << 6) + i) * csize;
    }
  }
  return NULL;
}

/* The slow path: the worker's current chunk for this class has no slot. NULL
   when the reservation is exhausted, and the caller mallocs. */
static SP_NOINLINE void *sp_slab_refill(sp_slab_worker *wk, int cls, unsigned csize, int payload) {
  sp_slab_chunk *ch;
  void *p = NULL;
  for (;;) {
#ifdef SP_THREADS
    /* pushed by a sweeper beside the running program (a compare-and-swap),
       popped only by the owner: the same exchange, with no ABA to lose to */
    ch = __atomic_load_n(&wk->avail[cls], __ATOMIC_ACQUIRE);
    while (ch) {
      sp_slab_chunk *nx = ch->next_avail;
      if (__atomic_compare_exchange_n(&wk->avail[cls], &ch, nx, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
    }
#else
    ch = wk->avail[cls];
    if (ch) wk->avail[cls] = ch->next_avail;
#endif
    if (!ch) break;
    ch->next_avail = NULL;
    __atomic_store_n(&ch->on_avail, 0, __ATOMIC_RELEASE);
    /* A sweeper decides "not the current chunk" from a read that races with
       this function installing exactly that chunk, so the chunk we are
       allocating from can land on the list too. It has already been used up
       from the front by then: take a slot if one is left, else drop it and
       move on. */
    ch->hint = 0;
    p = sp_slab_take(ch, csize, payload);
    if (p) break;
  }
  if (!ch) {
    SP_SLAB_LOCK();
    if (!sp_slab_empty && !sp_slab_next_arena()) { SP_SLAB_UNLOCK(); return NULL; }
    ch = sp_slab_empty;
    sp_slab_empty = ch->next_avail;
    SP_SLAB_UNLOCK();
    ch->next_avail = NULL;
    ch->nslots = sp_slab_nslots_of[cls];
    ch->hint = 0;
    ch->cls = (uint16_t)cls;
    ch->wid = (uint16_t)(wk - sp_slab_wk);
    ch->on_avail = 0;
    ch->in_use = 1;
    /* onto the owner's owned list: the sweep of this worker walks it */
    ch->own_prev = NULL;
    ch->own_next = wk->owned;
    if (wk->owned) wk->owned->own_prev = ch;
    __atomic_store_n(&wk->owned, ch, __ATOMIC_RELEASE);
    p = sp_slab_take(ch, csize, payload);
  }
  __atomic_store_n(&wk->cur[cls], ch, __ATOMIC_RELEASE);
  wk->taken++;
  ch->touched = 1;
  return p;
}

static inline void *sp_slab_alloc_in(size_t need, int payload) {
  if (__builtin_expect(sp_slab_on < 0, 0)) sp_slab_init();
  if (sp_slab_on && need <= SP_SLAB_MAX) {
    int cls = sp_slab_cls_of[(need + 15) >> 4];
    unsigned csize = sp_slab_csize[cls];
    sp_slab_worker *wk = &sp_slab_wk[SP_SLAB_WID()];
    sp_slab_chunk *ch = wk->cur[cls];
    void *p;
    if (ch && (p = sp_slab_take(ch, csize, payload)) != NULL) return p;
    p = sp_slab_refill(wk, cls, csize, payload);
    if (p) return p;
  }
  void *p = malloc(need);
  if (!p) sp_oom_die();
  return p;
}
/* A block the collector will sweep: an object (zeroed) or a heap string */
void *sp_slab_alloc(size_t need) {
  void *p = sp_slab_alloc_in(need, 0);
  memset(p, 0, need);
  if (sp_slab_verify_on) sp_slab_note(p, 1);
  return p;
}
void *sp_slab_alloc_str(size_t need) {
  void *p = sp_slab_alloc_in(need, 0);
  if (sp_slab_on > 0 && sp_slab_owns(p)) {
    sp_slab_loc l; sp_slab_locate(p, &l);
    __atomic_fetch_or(&l.bm->str[l.w], l.bit, __ATOMIC_RELAXED);
    if (sp_slab_verify_on) sp_slab_note(p, 2);
  }
  return p;
}
/* A block no sweep frees: a container's payload, dying by an explicit free */
void *sp_slab_alloc_raw(size_t need) {
  void *p = sp_slab_alloc_in(need, 1);
  if (sp_slab_verify_on) sp_slab_note(p, 3);
  return p;
}
/* the header gained a finalizer or a recycler after its allocation */
void sp_slab_set_fin(void *h) {
  if (sp_slab_on <= 0 || !sp_slab_owns(h)) return;
  sp_slab_loc l; sp_slab_locate(h, &l);
  __atomic_fetch_or(&l.bm->fin[l.w], l.bit, __ATOMIC_RELAXED);
  if (sp_slab_verify_on) sp_slab_note(h, 4);
}
/* a frozen heap string: kept until the program ends, as a static literal is */
void sp_slab_pin(const void *p) {
  if (sp_slab_on <= 0 || !sp_slab_owns(p)) return;
  sp_slab_loc l; sp_slab_locate(p, &l);
  __atomic_fetch_or(&l.bm->pin[l.w], l.bit, __ATOMIC_RELAXED);
  __atomic_fetch_or(&l.bm->old[l.w], l.bit, __ATOMIC_RELEASE);
  if (sp_slab_verify_on) sp_slab_note(p, 9);
}
/* A dead header about to be handed to its pool: parked (old and pin, out
   of every generation's reach, like a payload) until the pool hands it out
   again or frees it. Its finalizer bit stays: a parked slot is never dead,
   and the bit is what makes its next death run the recycler again. */
void sp_slab_park(void *h) {
  if (sp_slab_on <= 0 || !sp_slab_owns(h)) return;
  sp_slab_loc l; sp_slab_locate(h, &l);
  __atomic_fetch_or(&l.bm->pin[l.w], l.bit, __ATOMIC_RELAXED);
  __atomic_fetch_or(&l.bm->old[l.w], l.bit, __ATOMIC_RELEASE);
}
/* A pooled header handed out again: back into the current epoch, its
   finalizer bit with it (idempotent: parking left it set). */
void sp_slab_relive(void *h) {
  if (sp_slab_on <= 0 || !sp_slab_owns(h)) return;
  sp_slab_loc l; sp_slab_locate(h, &l);
  __atomic_fetch_or(&l.bm->young[sp_slab_epoch & 1][l.w], l.bit, __ATOMIC_RELAXED);
  __atomic_fetch_or(&l.bm->fin[l.w], l.bit, __ATOMIC_RELAXED);
  __atomic_fetch_and(&l.bm->old[l.w], ~l.bit, __ATOMIC_RELAXED);
  __atomic_fetch_and(&l.bm->pin[l.w], ~l.bit, __ATOMIC_RELAXED);
  if (sp_slab_verify_on) sp_slab_note(h, 5 + 10 * (int)(sp_slab_epoch & 1));
}
int sp_slab_is_str(const void *p) {
  sp_slab_loc l; sp_slab_locate(p, &l);
  return (l.bm->str[l.w] & l.bit) != 0;
}
/* the verifiers' report: what the bitmaps say about a slot */
void sp_slab_describe(const void *p) {
  if (sp_slab_on <= 0 || !sp_slab_owns(p)) { fprintf(stderr, "  slab: not a slab block\n"); return; }
  sp_slab_loc l; sp_slab_locate(p, &l);
  fprintf(stderr, "  slab: chunk wid=%u cls=%u (%u B) in_use=%d slot=%u epoch=%u young0=%d young1=%d old=%d mark=%d fin=%d str=%d pin=%d\n",
          l.ch->wid, l.ch->cls, sp_slab_csize[l.ch->cls], l.ch->in_use, l.idx, sp_slab_epoch,
          !!(l.bm->young[0][l.w] & l.bit), !!(l.bm->young[1][l.w] & l.bit), !!(l.bm->old[l.w] & l.bit),
          !!(l.bm->mark[l.w] & l.bit), !!(l.bm->fin[l.w] & l.bit), !!(l.bm->str[l.w] & l.bit), !!(l.bm->pin[l.w] & l.bit));
}
/* SPINEL_GC_VERIFY: every chunk's bitmaps against their invariants -- a
   finalizer, string or pin bit on a slot in no generation is a slot that was
   freed without being cleared, or claimed without being free. */
int sp_slab_verify_on = 0;   /* declared above */
/* under the verifier: who last set a slot's bits (a byte per 32-byte slot
   over the whole reservation, touched only where slots are) */
typedef struct { unsigned char what, wid; unsigned short cycle; } sp_slab_shadow_ev;
typedef struct { unsigned char n; sp_slab_shadow_ev ev[8]; } sp_slab_shadow_rec;
static sp_slab_shadow_rec *sp_slab_shadow = NULL;
extern int sp_gc_cycle;
#define SP_SHADOW(p) (sp_slab_shadow ? &sp_slab_shadow[((uintptr_t)(p) - sp_slab_base) >> 5] : NULL)
void sp_slab_note(const void *p, int what) {
  if (!sp_slab_verify_on || !sp_slab_owns(p)) return;
  if (!sp_slab_shadow) {
    void *m = mmap(NULL, (sp_slab_cap >> 5) * sizeof(sp_slab_shadow_rec), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) return;
    sp_slab_shadow = (sp_slab_shadow_rec *)m;
  }
  sp_slab_shadow_rec *sh = SP_SHADOW(p);
  if (!sh) return;
  sp_slab_shadow_ev *e = &sh->ev[sh->n++ & 7];
  e->what = (unsigned char)what; e->wid = (unsigned char)sp_worker_id_shadow(); e->cycle = (unsigned short)sp_gc_cycle;
}
static void sp_slab_verify_chunk(sp_slab_chunk *ch, const char *when) {
  sp_slab_bm *bm = sp_slab_bm_of(ch);
  unsigned nw = (ch->nslots + 63) >> 6, csize = sp_slab_csize[ch->cls];
  for (unsigned w = 0; w < nw; w++) {
    uint64_t gen = bm->young[0][w] | bm->young[1][w] | bm->old[w];
    uint64_t stray = (bm->fin[w] | bm->str[w] | bm->pin[w] | bm->mark[w]) & ~gen;
    uint64_t both = bm->young[0][w] & bm->young[1][w];
    uint64_t finstr = bm->fin[w] & bm->str[w];
    uint64_t ymark = (bm->young[0][w] | bm->young[1][w]) & bm->mark[w] & (when[0] == 'a' && when[1] == 't' ? ~(uint64_t)0 : 0);   /* at the barrier: no young slot carries a mark */
    uint64_t bad = stray | both | finstr | ymark;
    while (bad) {
      unsigned b = (unsigned)__builtin_ctzll(bad); bad &= bad - 1;
      void *slot = sp_slab_chunk_base(ch) + (size_t)((w << 6) + b) * csize;
      fprintf(stderr, "*** SPINEL_GC_VERIFY: slab invariant broken at %p (%s), %s:\n", slot,
              (stray >> b) & 1 ? "bits on a free slot" : (both >> b) & 1 ? "in both epochs" : (finstr >> b) & 1 ? "finalizer bit on a string" : "a mark on a young slot", when);
      sp_slab_describe(slot);
      { const uint64_t *q = (const uint64_t *)slot;
        fprintf(stderr, "  slot words: %016llx %016llx %016llx %016llx %016llx %016llx\n",
                (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2],
                (unsigned long long)q[3], (unsigned long long)q[4], (unsigned long long)q[5]);
        sp_slab_shadow_rec *sh = SP_SHADOW(slot);
        fprintf(stderr, "  history (1 obj alloc, 2 str alloc, 3 raw alloc, 4 set_fin, 5/15 relive into y0/y1, 6 free, 7 swept dead, 8/18 parked sweeping y0/y1, 9 pin, 30/31 survived sweep of y0/y1, 40-43 take into y0/y1/(+2 payload)), oldest first; now cycle %d:\n", sp_gc_cycle);
        if (sh) for (unsigned k = 0; k < 8; k++) { sp_slab_shadow_ev *e = &sh->ev[(sh->n + k) & 7]; if (e->what) fprintf(stderr, "    what=%d worker=%d cycle=%d\n", e->what, e->wid, e->cycle); } }
      abort();
    }
  }
}
void sp_slab_verify_all(void) {
  if (sp_slab_on <= 0) return;
  sp_slab_verify_on = 1;
  for (uintptr_t a = sp_slab_base; a < sp_slab_brk; a += SP_SLAB_ARENA) {
    sp_slab_arena *ar = (sp_slab_arena *)a;
    for (int i = SP_SLAB_FIRST; i < (int)SP_SLAB_NCHUNK; i++) {
      sp_slab_chunk *ch = &ar->ch[i];
      if (!ch->in_use) continue;
      sp_slab_verify_chunk(ch, "at the barrier");
    }
  }
}
/* allocated at all: in some generation (a verifier's membership test) */
int sp_slab_is_live(const void *p) {
  if (sp_slab_on <= 0 || !sp_slab_owns(p)) return 0;
  sp_slab_loc l; sp_slab_locate(p, &l);
  return ((l.bm->young[0][l.w] | l.bm->young[1][l.w] | l.bm->old[l.w]) & l.bit) != 0;
}
int sp_slab_is_old(const void *p) {
  sp_slab_loc l; sp_slab_locate(p, &l);
  return (l.bm->old[l.w] & l.bit) != 0;
}

/* A chunk a free put a slot back into rejoins its owner's available list,
   unless it is there already or is the chunk the owner allocates from. The
   owner's current chunk is read racily: when it is mid-refill the chunk it
   is installing can be pushed here too, and refill drops it again when it
   finds it empty. */
static void sp_slab_avail_push(sp_slab_chunk *ch) {
#ifdef SP_THREADS
  if (!__atomic_load_n(&ch->on_avail, __ATOMIC_RELAXED) &&
      __atomic_load_n(&sp_slab_wk[ch->wid].cur[ch->cls], __ATOMIC_RELAXED) != ch) {
    unsigned char was = __atomic_exchange_n(&ch->on_avail, 1, __ATOMIC_ACQ_REL);
    if (!was) {
      sp_slab_chunk **head = &sp_slab_wk[ch->wid].avail[ch->cls];
      sp_slab_chunk *oh;
      do { oh = __atomic_load_n(head, __ATOMIC_ACQUIRE); ch->next_avail = oh;
      } while (!__atomic_compare_exchange_n(head, &oh, ch, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    }
  }
#else
  if (!ch->on_avail && sp_slab_wk[0].cur[ch->cls] != ch) {
    ch->on_avail = 1;
    ch->next_avail = sp_slab_wk[0].avail[ch->cls];
    sp_slab_wk[0].avail[ch->cls] = ch;
  }
#endif
}

/* An explicit free, from anywhere: a payload its container dropped, a pooled
   header over the pool's cap, a malloc'd block. The slot leaves every
   generation and loses its marks; the chunk goes back on the owner's list. */
void sp_slab_free(void *p) {
  if (!sp_slab_owns(p)) { free(p); return; }
  sp_slab_loc l; sp_slab_locate(p, &l);
  uint64_t nb = ~l.bit;
  __atomic_fetch_and(&l.bm->young[0][l.w], nb, __ATOMIC_RELAXED);
  __atomic_fetch_and(&l.bm->young[1][l.w], nb, __ATOMIC_RELAXED);
  __atomic_fetch_and(&l.bm->old[l.w], nb, __ATOMIC_RELAXED);
  __atomic_fetch_and(&l.bm->pin[l.w], nb, __ATOMIC_RELAXED);
  __atomic_fetch_and(&l.bm->fin[l.w], nb, __ATOMIC_RELAXED);
  __atomic_fetch_and(&l.bm->str[l.w], nb, __ATOMIC_RELAXED);
  __atomic_fetch_and(&l.bm->mark[l.w], nb, __ATOMIC_RELAXED);
  if (sp_slab_verify_on) sp_slab_note(p, 6);
  sp_slab_avail_push(l.ch);
}

/* A container payload resized: a slab block's capacity is its size class,
   so a smaller or equal request keeps the block, a larger one moves to a
   fresh block (slab or malloc, by size) and frees the old; a malloc block
   is realloc'd, which for the largest is an mremap with no copy. */
void *sp_pl_realloc(void *p, size_t newn) {
  if (!p) return sp_slab_alloc_raw(newn);
  if (!sp_slab_owns(p)) {
    void *q = realloc(p, newn);
    if (!q) sp_oom_die();
    return q;
  }
  sp_slab_chunk *ch = sp_slab_chunk_of(p);
  size_t have = sp_slab_csize[ch->cls];
  if (newn <= have) return p;
  void *q = sp_slab_alloc_raw(newn);
  memcpy(q, p, have);
  sp_slab_free(p);
  return q;
}

/* ---- the mark ----
   The mark bit of a slot, set by whichever marker reaches it first, and
   promotion: a young slot the mark reaches becomes old here, which is what
   the concurrent sweep needs (it will not write the bit, and it frees what
   is young and unmarked). `aging` keeps a first-time survivor young: the
   sweep carries it into the next epoch instead. Returns 0 when the slot
   was already marked this cycle, else 1, with *was_young telling a promotion
   (or an aged survivor) from an old object reached again. */
int sp_slab_mark(const void *p, int aging, int *was_young) {
  sp_slab_loc l; sp_slab_locate(p, &l);
  uint64_t o = __atomic_fetch_or(&l.bm->mark[l.w], l.bit, __ATOMIC_ACQ_REL);
  if (o & l.bit) { *was_young = 0; return 0; }
  uint64_t ow = __atomic_load_n(&l.bm->old[l.w], __ATOMIC_RELAXED);
  if (ow & l.bit) { *was_young = 0; return 1; }
  *was_young = 1;
  if (!aging) __atomic_fetch_or(&l.bm->old[l.w], l.bit, __ATOMIC_RELAXED);
  return 1;
}
/* is the slot marked this cycle? (the string side's "already marked" test) */
int sp_slab_is_marked(const void *p) {
  sp_slab_loc l; sp_slab_locate(p, &l);
  return (__atomic_load_n(&l.bm->mark[l.w], __ATOMIC_RELAXED) & l.bit) != 0;
}
/* the verifiers' probe unmarks a slot to see who marks it again */
void sp_slab_unmark(const void *p) {
  sp_slab_loc l; sp_slab_locate(p, &l);
  __atomic_fetch_and(&l.bm->mark[l.w], ~l.bit, __ATOMIC_RELAXED);
}

/* ---- the sweep ----
   Under the barrier, before the sweep of the epoch that just closed starts:
   new allocations go to the other parity from here. */
void sp_slab_epoch_flip(void) {
  __atomic_store_n(&sp_slab_epoch, sp_slab_epoch + 1, __ATOMIC_RELEASE);
}
/* Every chunk one worker owns, in one pass over the bitmaps. The epoch to
   reclaim from is the one before the current; `full` frees the old
   generation too; `aging` carries young survivors the mark did not promote
   into the current epoch. What dies with a finalizer bit set has its
   finalizer (or recycler) run, from this thread. The caller says which
   thread is sweeping: an owner beside the program, a sweeper thread, or the
   collector under the barrier -- the bit operations are the same. */
void sp_slab_sweep_worker(int wid, int full, int aging, int (*die)(void *hdr), sp_slab_sweep_stats *st) {
  sp_slab_sweep_stats acc; memset(&acc, 0, sizeof acc);
  if (sp_slab_on <= 0 || wid < 0 || wid >= SP_SLAB_NWK) { if (st) *st = acc; return; }
  sp_slab_worker *wk = &sp_slab_wk[wid];
  unsigned e = sp_slab_epoch & 1, pe = e ^ 1;
  __atomic_store_n(&wk->sweeping, 1, __ATOMIC_RELEASE);
  for (sp_slab_chunk *ch = __atomic_load_n(&wk->owned, __ATOMIC_ACQUIRE); ch; ch = ch->own_next) {
    if (!ch->in_use) continue;
    sp_slab_bm *bm = sp_slab_bm_of(ch);
    unsigned nw = (ch->nslots + 63) >> 6;
    unsigned csize = sp_slab_csize[ch->cls];
    size_t freed = 0;
    for (unsigned w = 0; w < nw; w++) {
      uint64_t yv = __atomic_load_n(&bm->young[pe][w], __ATOMIC_RELAXED);
      uint64_t ov = __atomic_load_n(&bm->old[w], __ATOMIC_RELAXED);
      uint64_t mv = __atomic_load_n(&bm->mark[w], __ATOMIC_RELAXED);
      /* a word with nothing to reclaim is skipped, unless it carries marks:
         a minor's mark reaches old strings too, and a mark left behind would
         read as "already marked" next cycle and keep a dead string */
      if (!yv && !mv && !(full && ov)) continue;
      acc.swept += (size_t)__builtin_popcountll(yv) + (full ? (size_t)__builtin_popcountll(ov) : 0);
      uint64_t dead = yv & ~mv & ~ov;
      if (full) dead |= ov & ~mv & ~__atomic_load_n(&bm->pin[w], __ATOMIC_RELAXED);
      if (aging) {
        uint64_t carry = yv & mv & ~ov;
        if (carry) { __atomic_fetch_or(&bm->young[e][w], carry, __ATOMIC_RELAXED); acc.kept_young += (size_t)__builtin_popcountll(carry) * csize; }
      }
      /* a dead object with a finalizer or a recycler: the callback runs it
         and says whether the slot is freed or kept. A pooled header stays
         allocated: the callback parks it (old and pin, out of every
         generation's reach, as a payload is: sp_slab_park) BEFORE handing it
         to its pool, since the pool may hand it out again on another thread
         at once, and that thread's relive undoes the parking. So nothing here
         writes a kept slot's bits after the callback: it is either still
         parked or already alive again, and either way it is not dead. */
      uint64_t fdead = dead & __atomic_load_n(&bm->fin[w], __ATOMIC_RELAXED);
      uint64_t parked = 0;
      while (fdead) {
        unsigned i = (unsigned)__builtin_ctzll(fdead);
        fdead &= fdead - 1;
        if (!die(sp_slab_chunk_base(ch) + (size_t)((w << 6) + i) * csize)) parked |= (uint64_t)1 << i;
      }
      if (parked) {
        dead &= ~parked;
        if (sp_slab_verify_on) { uint64_t v = parked; while (v) { unsigned b = (unsigned)__builtin_ctzll(v); v &= v - 1; sp_slab_note(sp_slab_chunk_base(ch) + (size_t)((w << 6) + b) * csize, 8 + 10 * (int)pe); } }
      }
      if (sp_slab_verify_on) { uint64_t v = yv & ~dead; while (v) { unsigned b = (unsigned)__builtin_ctzll(v); v &= v - 1; sp_slab_note(sp_slab_chunk_base(ch) + (size_t)((w << 6) + b) * csize, 30 + (int)pe); } }
      if (sp_slab_verify_on && dead) { uint64_t v = dead; while (v) { unsigned b = (unsigned)__builtin_ctzll(v); v &= v - 1; sp_slab_note(sp_slab_chunk_base(ch) + (size_t)((w << 6) + b) * csize, 7); } }
      if (dead) {
        uint64_t sd = dead & __atomic_load_n(&bm->str[w], __ATOMIC_RELAXED);
        acc.freed_str += (size_t)__builtin_popcountll(sd) * csize;
        acc.freed_obj += (size_t)__builtin_popcountll(dead & ~sd) * csize;
        __atomic_fetch_and(&bm->fin[w], ~dead, __ATOMIC_RELAXED);
        __atomic_fetch_and(&bm->str[w], ~dead, __ATOMIC_RELAXED);
        if (full) __atomic_fetch_and(&bm->old[w], ~dead, __ATOMIC_RELAXED);
        freed += (size_t)__builtin_popcountll(dead);
      }
      /* the epoch's word is spent: dead, promoted or carried, every bit is
         accounted for. So are the marks: the next cycle starts clean. */
      __atomic_store_n(&bm->young[pe][w], 0, __ATOMIC_RELAXED);
      __atomic_store_n(&bm->mark[w], 0, __ATOMIC_RELAXED);
    }
    acc.slots += ch->nslots;
    if (freed) { acc.freed_slots += freed; sp_slab_avail_push(ch); }
    if (sp_slab_verify_on) sp_slab_verify_chunk(ch, "after its sweep");
  }
  __atomic_store_n(&wk->sweeping, 0, __ATOMIC_RELEASE);
  if (st) *st = acc;
}
/* The verifiers walk every allocated slot of every chunk: objects (not
   strings) whose generation is young, old, or either. */
void sp_slab_each_object(int young, int old, void (*fn)(void *hdr, void *arg), void *arg) {
  if (sp_slab_on <= 0) return;
  for (uintptr_t a = sp_slab_base; a < sp_slab_brk; a += SP_SLAB_ARENA) {
    sp_slab_arena *ar = (sp_slab_arena *)a;
    for (int i = SP_SLAB_FIRST; i < (int)SP_SLAB_NCHUNK; i++) {
      sp_slab_chunk *ch = &ar->ch[i];
      if (!ch->in_use) continue;
      sp_slab_bm *bm = sp_slab_bm_of(ch);
      unsigned nw = (ch->nslots + 63) >> 6, csize = sp_slab_csize[ch->cls];
      for (unsigned w = 0; w < nw; w++) {
        uint64_t v = 0;
        if (young) v |= bm->young[0][w] | bm->young[1][w];
        if (old) v |= bm->old[w];
        v &= ~bm->str[w] & ~(bm->pin[w] & bm->old[w]);   /* not strings, not payloads */
        while (v) {
          unsigned b = (unsigned)__builtin_ctzll(v); v &= v - 1;
          fn(sp_slab_chunk_base(ch) + (size_t)((w << 6) + b) * csize, arg);
        }
      }
    }
  }
}
/* the same over heap strings (the slot's start; the body follows the header) */
void sp_slab_each_string(int young, int old, void (*fn)(void *hdr, void *arg), void *arg) {
  if (sp_slab_on <= 0) return;
  for (uintptr_t a = sp_slab_base; a < sp_slab_brk; a += SP_SLAB_ARENA) {
    sp_slab_arena *ar = (sp_slab_arena *)a;
    for (int i = SP_SLAB_FIRST; i < (int)SP_SLAB_NCHUNK; i++) {
      sp_slab_chunk *ch = &ar->ch[i];
      if (!ch->in_use) continue;
      sp_slab_bm *bm = sp_slab_bm_of(ch);
      unsigned nw = (ch->nslots + 63) >> 6, csize = sp_slab_csize[ch->cls];
      for (unsigned w = 0; w < nw; w++) {
        uint64_t v = 0;
        if (young) v |= bm->young[0][w] | bm->young[1][w];
        if (old) v |= bm->old[w];
        v &= bm->str[w];
        while (v) {
          unsigned b = (unsigned)__builtin_ctzll(v); v &= v - 1;
          fn(sp_slab_chunk_base(ch) + (size_t)((w << 6) + b) * csize, arg);
        }
      }
    }
  }
}

/* ---- release: fully free chunks back to the OS ---- */
/* The available lists are rebuilt in ADDRESS order. A chunk joins the list
   whenever a sweep frees its first slot, in whatever order the sweep meets
   them, and after a few cycles consecutive refills of one class hopped
   across the heap: a 72 MB tree benchmark (gcbench) ran 30% slower on
   16 KB chunks than on glibc, whose coalescing hands back one run of
   addresses, and 15% faster once the refills walked the chunks in order. */
static int sp_slab_chunk_cmp(const void *a, const void *b) {
  uintptr_t x = (uintptr_t)*(sp_slab_chunk *const *)a, y = (uintptr_t)*(sp_slab_chunk *const *)b;
  return x < y ? -1 : x > y;
}
/* The sort buffer is per thread: the owners release their own lists beside
   the program, at the same time, and a shared buffer would need a lock every
   one of them queued on. */
static SP_TLS sp_slab_chunk **sp_slab_sortbuf = NULL;
static SP_TLS size_t sp_slab_sortcap = 0;
static sp_slab_chunk *sp_slab_sort_avail(sp_slab_chunk *head) {
  size_t n = 0;
  for (sp_slab_chunk *ch = head; ch; ch = ch->next_avail) {
    if (n == sp_slab_sortcap) {
      size_t c = sp_slab_sortcap ? sp_slab_sortcap * 2 : 256;
      sp_slab_chunk **nb = (sp_slab_chunk **)realloc(sp_slab_sortbuf, c * sizeof *nb);
      if (!nb) return head;   /* unsorted is still correct */
      sp_slab_sortbuf = nb; sp_slab_sortcap = c;
    }
    sp_slab_sortbuf[n++] = ch;
  }
  if (n < 2) return head;
  qsort(sp_slab_sortbuf, n, sizeof *sp_slab_sortbuf, sp_slab_chunk_cmp);
  for (size_t i = 0; i + 1 < n; i++) sp_slab_sortbuf[i]->next_avail = sp_slab_sortbuf[i + 1];
  sp_slab_sortbuf[n - 1]->next_avail = NULL;
  return sp_slab_sortbuf[0];
}
static inline int sp_slab_chunk_empty(sp_slab_chunk *ch) {
  sp_slab_bm *bm = sp_slab_bm_of(ch);
  unsigned nw = (ch->nslots + 63) >> 6;
  for (unsigned w = 0; w < nw; w++)
    if (__atomic_load_n(&bm->young[0][w], __ATOMIC_ACQUIRE) | __atomic_load_n(&bm->young[1][w], __ATOMIC_ACQUIRE) |
        __atomic_load_n(&bm->old[w], __ATOMIC_ACQUIRE)) return 0;
  return 1;
}

/* SPINEL_GC_PHASES: what a release spends its time on -- chunks walked,
   chunks handed back (each one an madvise), and the madvise time itself. */
unsigned long long sp_slab_rel_calls = 0, sp_slab_rel_walked = 0, sp_slab_rel_madv = 0;
double sp_slab_rel_madv_t = 0, sp_slab_rel_sort_t = 0;
static double sp_slab_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
/* One worker's lists, released by their OWNER (or, for a slot no worker
   runs, by the collector under the barrier), right after its own sweep: the
   walk, the sort and the madvise calls that the collector used to do for
   every worker in turn under the barrier are spread over the workers
   instead, each on its own lists, while the others run. Only the owner pops
   its available lists, so detaching a list whole (an exchange with NULL)
   makes it private for the walk; a sweeper thread pushing a chunk meanwhile
   lands it on the fresh head, and the kept chunks are merged back under it
   with the same compare-and-swap the sweepers push with. A fully free chunk
   is stable once seen: every slot is out of every generation, so nothing can
   free into it, and only its owner could allocate from it. Never the chunk
   the class allocates from, and never while a sweep of this worker's chunks
   is still running on another thread: that sweep clears bits of the very
   chunks the pool would carve for someone else. The empty pool is shared,
   so it takes the slab lock. */
void sp_slab_release_worker(int wid) {
  if (sp_slab_on <= 0 || wid < 0 || wid >= SP_SLAB_NWK) return;
  sp_slab_worker *wk = &sp_slab_wk[wid];
  if (__atomic_load_n(&wk->sweeping, __ATOMIC_ACQUIRE)) return;
  long reserve = wk->taken;
  if (reserve < SP_SLAB_RESERVE) reserve = SP_SLAB_RESERVE;
  wk->taken = 0;
  if (sp_gc_ph_on) sp_slab_rel_calls++;
  for (int cls = 0; cls < SP_SLAB_NCLS; cls++) {
    sp_slab_chunk *ch = __atomic_exchange_n(&wk->avail[cls], NULL, __ATOMIC_ACQ_REL);
    if (!ch) continue;
    sp_slab_chunk *keep = NULL, *give = NULL;
    while (ch) {
      sp_slab_chunk *nx = ch->next_avail;
      if (sp_gc_ph_on) sp_slab_rel_walked++;
      int empty = sp_slab_chunk_empty(ch);
      if (empty && reserve <= 0 && ch != __atomic_load_n(&wk->cur[cls], __ATOMIC_RELAXED)) {
        ch->next_avail = give; give = ch;   /* handed back below, in address order */
      }
      else {
        if (empty) reserve--;
        ch->next_avail = keep; keep = ch;
      }
      ch = nx;
    }
    if (give) {
      /* the chunks handed back, in address order, so neighbours go to the
         kernel as one madvise: a call per 16 KB chunk was most of the
         release's time, and half of them sit next to each other */
      give = sp_slab_sort_avail(give);
      sp_slab_chunk *gt = give;
      while (gt) {
        char *lo = sp_slab_chunk_base(gt), *hi = lo + SP_SLAB_CHUNK;
        int touched = gt->touched;
        sp_slab_chunk *run_end = gt;
        while (run_end->next_avail && sp_slab_chunk_base(run_end->next_avail) == hi &&
               run_end->next_avail->touched == touched) {
          run_end = run_end->next_avail; hi += SP_SLAB_CHUNK;
        }
        if (touched) {
          double mt0 = sp_gc_ph_on ? sp_slab_now() : 0;
          madvise(lo, (size_t)(hi - lo), MADV_DONTNEED);
          if (sp_gc_ph_on) { sp_slab_rel_madv++; sp_slab_rel_madv_t += sp_slab_now() - mt0; }
        }
        sp_slab_chunk *nx = run_end->next_avail;
        /* on_avail stays SET on a chunk handed back: a sweeper whose free
           landed before the walk read the chunk as empty can still be short
           of its own "put it on the list" step, and that step's exchange
           must find the chunk spoken for, or the owner would pop a chunk the
           pool has since carved for someone else. The carve (sp_slab_refill)
           is what clears it. Off the owned list here, before the pool can
           hand it to a worker that links it into its own. */
        for (sp_slab_chunk *c2 = gt;; c2 = c2->next_avail) {
          if (c2->own_prev) c2->own_prev->own_next = c2->own_next;
          else wk->owned = c2->own_next;
          if (c2->own_next) c2->own_next->own_prev = c2->own_prev;
          c2->own_next = c2->own_prev = NULL;
          memset(sp_slab_bm_of(c2), 0, sizeof(sp_slab_bm));
          c2->in_use = 0; c2->on_avail = 1; c2->touched = 0;
          c2->nslots = 0; c2->hint = 0;
          if (c2 == run_end) break;
        }
        gt = nx;
      }
      gt = give; while (gt->next_avail) gt = gt->next_avail;
      SP_SLAB_LOCK();
      gt->next_avail = sp_slab_empty; sp_slab_empty = give;
      SP_SLAB_UNLOCK();
    }
    if (keep) {
      double st0 = sp_gc_ph_on ? sp_slab_now() : 0;
      keep = sp_slab_sort_avail(keep);
      if (sp_gc_ph_on) sp_slab_rel_sort_t += sp_slab_now() - st0;
      sp_slab_chunk *kt = keep; while (kt->next_avail) kt = kt->next_avail;
#ifdef SP_THREADS
      sp_slab_chunk *oh;
      do { oh = __atomic_load_n(&wk->avail[cls], __ATOMIC_ACQUIRE); kt->next_avail = oh;
      } while (!__atomic_compare_exchange_n(&wk->avail[cls], &oh, keep, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
#else
      kt->next_avail = wk->avail[cls]; wk->avail[cls] = keep;
#endif
    }
  }
}
/* The collector's part once the owners release their own: the slots no
   active worker owns (a worker that has exited, the sweeper slot), which
   nobody else will walk. Under the barrier. */
void sp_slab_release_from(int first) {
  if (sp_slab_on <= 0) return;
  for (int w = first; w < SP_SLAB_NWK; w++) {
    sp_slab_worker *wk = &sp_slab_wk[w];
    for (int cls = 0; cls < SP_SLAB_NCLS; cls++) {
      if (!wk->avail[cls]) continue;
      sp_slab_release_worker(w);
      break;
    }
  }
}
/* every worker's, from one thread with nothing else running (the
   single-threaded build, or a stop-the-world sweep) */
void sp_slab_release(void) {
  if (sp_slab_on <= 0) return;
  /* SPINEL_GC_PHASES: the slab's footprint every 64th release -- chunks in
     use, of which fully free (the reserve), and the bytes their live slots
     hold -- so the resident set can be read against what is live. */
  if (sp_gc_ph_on) {
    static int tick = 0;
    if ((++tick & 63) == 0) {
      size_t nuse = 0, nempty = 0, live = 0, untouched = 0;
      for (uintptr_t a = sp_slab_base; a < sp_slab_brk; a += SP_SLAB_ARENA) {
        sp_slab_arena *ar = (sp_slab_arena *)a;
        for (int i = SP_SLAB_FIRST; i < (int)SP_SLAB_NCHUNK; i++) {
          sp_slab_chunk *ch = &ar->ch[i];
          if (!ch->in_use) { if (!ch->touched) untouched++; continue; }
          nuse++;
          sp_slab_bm *bm = sp_slab_bm_of(ch);
          size_t used = 0;
          for (unsigned w = 0; w < (ch->nslots + 63u) / 64u; w++)
            used += (size_t)__builtin_popcountll(bm->young[0][w] | bm->young[1][w] | bm->old[w]);
          if (!used) nempty++;
          live += used * sp_slab_csize[ch->cls];
        }
      }
      fprintf(stderr, "[slab] arenas %zu  chunks in use %zu (fully free %zu)  live in slots %.1f MB  resident chunks %.1f MB\n",
              (size_t)((sp_slab_brk - sp_slab_base) / SP_SLAB_ARENA), nuse, nempty,
              live / 1048576.0, nuse * (SP_SLAB_CHUNK / 1048576.0));
    }
  }
  for (int w = 0; w < SP_SLAB_NWK; w++) sp_slab_release_worker(w);
}

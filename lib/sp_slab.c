/* sp_slab.c -- size-class slab allocation for GC objects and heap strings.
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
 * Here a block is a slot in a 16 KB chunk of one size class. Allocation
 * pops the worker's current chunk, or carves the next slot from it; both are
 * a few instructions with no lock, since a chunk belongs to one worker for
 * allocation and no other worker allocates from it. A dead block goes back
 * onto its chunk's free list: the sweep runs stop-the-world, so the pushes
 * race only each other (the parallel young sweep frees on every worker at
 * once) and never a pop, and a compare-exchange settles that. When a chunk
 * has no free slot it is simply dropped by its owner, and the first free into
 * it puts it back on the owner's available list.
 *
 * Every chunk lives inside one address range reserved at startup (16 GB of
 * untouched, uncommitted pages; smaller where the system refuses), so a free
 * tells a slab block from a malloc'd one by a range check, and a slot finds
 * its chunk by masking its address. The range is handed out in 4 MB arenas
 * whose first chunk holds the arena's chunk headers, so a released chunk
 * keeps no resident page at all. At the end of a full cycle every chunk that
 * is entirely free, beyond a small reserve per worker, is given back to the
 * OS with madvise and re-carved from scratch when next used; that is what
 * malloc_trim did for these blocks, at a syscall per idle chunk instead of a
 * walk of thirty arenas.
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
#include "sp_gc.h"
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#define SP_SLAB_ARENA   ((size_t)4 << 20)
#define SP_SLAB_CHUNK   ((size_t)16 << 10)
#define SP_SLAB_NCHUNK  (SP_SLAB_ARENA / SP_SLAB_CHUNK)   /* 256; chunk 0 is the header table (256 x 64 B) */
#define SP_SLAB_NCLS    27
#define SP_SLAB_MAX     2048
/* fully free chunks a worker keeps resident across a full cycle, at least (256 KB) */
#define SP_SLAB_RESERVE 16

#ifdef SP_THREADS
#define SP_SLAB_NWK SP_MAX_WORKERS
#define SP_SLAB_WID() (sp_worker_id)
#else
#define SP_SLAB_NWK 1
#define SP_SLAB_WID() 0
#endif

/* 16-byte steps to 256, then coarser: an object header is 48 bytes and a
   string header 24, and most blocks are a header plus a few words, so the
   fine steps are where the population is. Past SP_SLAB_MAX a block is
   malloc'd as before. */
static const uint16_t sp_slab_csize[SP_SLAB_NCLS] = {
  32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 256,
  320, 384, 448, 512, 640, 768, 896, 1024, 1280, 1536, 1792, 2048 };
static uint8_t sp_slab_cls_of[SP_SLAB_MAX / 16 + 1];   /* (need+15)/16 -> class */

typedef struct sp_slab_chunk {
  struct sp_slab_chunk *next_avail;   /* the owner's available list, per class */
  void *free;                         /* freed slots, linked through their first word */
  char *bump, *end;                   /* not yet carved: [bump, end) */
  uint32_t nfree;                     /* free slots, carved or not */
  uint32_t nslots;
  uint16_t cls, wid;
  uint8_t on_avail;                   /* listed on the owner's available list */
  uint8_t in_use;                     /* holds a class; 0 = empty, on the global pool */
  uint8_t touched;                    /* has resident pages since the last release */
  uint8_t _pad[64 - 8 * 4 - 4 * 2 - 2 * 2 - 3];
} sp_slab_chunk;
typedef char sp_slab_chunk_is_one_line[sizeof(sp_slab_chunk) == 64 ? 1 : -1];

typedef struct sp_slab_arena {
  sp_slab_chunk ch[SP_SLAB_NCHUNK];   /* ch[0] is this table itself, never carved */
} sp_slab_arena;

typedef struct {
  sp_slab_chunk *cur[SP_SLAB_NCLS];
  sp_slab_chunk *avail[SP_SLAB_NCLS];
  long taken;        /* chunks this worker started allocating into since the last release */
  char _pad[64 - ((2 * SP_SLAB_NCLS * sizeof(void *) + sizeof(long)) % 64)];
} sp_slab_worker;

static sp_slab_worker sp_slab_wk[SP_SLAB_NWK];
static uintptr_t sp_slab_base = 0, sp_slab_brk = 0;   /* the reservation, and how much is in use */
static size_t sp_slab_cap = 0;
static sp_slab_chunk *sp_slab_empty = NULL;   /* chunks holding no class */
int sp_slab_on = -1;                          /* decided once from the environment */
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
  if (e && *e) sp_slab_on = (*e != '0');
  else sp_slab_on = !sp_slab_jemalloc_present();
  if (sp_slab_on) sp_slab_reserve();
}

static inline int sp_slab_owns(const void *p) {
  return (uintptr_t)p - sp_slab_base < sp_slab_cap;
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

/* The next arena of the reservation: its header table is zero already (an
   untouched page reads as zero), so only the empty list needs writing. */
static int sp_slab_next_arena(void) {
  if (sp_slab_brk + SP_SLAB_ARENA > sp_slab_base + sp_slab_cap) return 0;
  sp_slab_arena *ar = (sp_slab_arena *)sp_slab_brk;
  sp_slab_brk += SP_SLAB_ARENA;
  for (int i = (int)SP_SLAB_NCHUNK - 1; i >= 1; i--) {   /* pops then ascend in address */
    ar->ch[i].next_avail = sp_slab_empty;
    sp_slab_empty = &ar->ch[i];
  }
  return 1;
}

/* Pop a freed slot off a chunk. The owner is the only popper, so the
   compare-and-swap has no ABA case to lose to; what it races is a sweeper
   pushing beside the running program (the concurrent sweep, sp_sched.c),
   whose push is the same exchange. Single-threaded, the plain pop. */
static inline void *sp_slab_pop(sp_slab_chunk *ch) {
#ifdef SP_THREADS
  void *p = __atomic_load_n(&ch->free, __ATOMIC_ACQUIRE);
  while (p) {
    void *nx = *(void **)p;
    if (__atomic_compare_exchange_n(&ch->free, &p, nx, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
  }
  return p;
#else
  void *p = ch->free;
  if (p) ch->free = *(void **)p;
  return p;
#endif
}

/* The slow path: the worker's current chunk for this class has no slot. NULL
   when the reservation is exhausted, and the caller mallocs. */
static SP_NOINLINE void *sp_slab_refill(sp_slab_worker *wk, int cls, size_t csize) {
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
       move on. Bumping past `end` here handed out the neighbouring chunk's
       memory. */
    p = sp_slab_pop(ch);
    if (!p && ch->bump < ch->end) { p = ch->bump; ch->bump += csize; }
    if (p) break;
  }
  if (!ch) {
    SP_SLAB_LOCK();
    if (!sp_slab_empty && !sp_slab_next_arena()) { SP_SLAB_UNLOCK(); return NULL; }
    ch = sp_slab_empty;
    sp_slab_empty = ch->next_avail;
    SP_SLAB_UNLOCK();
    char *base = sp_slab_chunk_base(ch);
    ch->next_avail = NULL;
    ch->free = NULL;
    ch->nslots = (uint32_t)(SP_SLAB_CHUNK / csize);
    ch->nfree = ch->nslots;
    ch->bump = base;
    ch->end = base + (size_t)ch->nslots * csize;
    ch->cls = (uint16_t)cls;
    ch->wid = (uint16_t)(wk - sp_slab_wk);
    ch->on_avail = 0;
    ch->in_use = 1;
    p = ch->bump; ch->bump += csize;
  }
  __atomic_store_n(&wk->cur[cls], ch, __ATOMIC_RELEASE);
  wk->taken++;
  __atomic_fetch_sub(&ch->nfree, 1, __ATOMIC_RELAXED);
  ch->touched = 1;
  return p;
}

void *sp_slab_alloc_raw(size_t need) {
  if (__builtin_expect(sp_slab_on < 0, 0)) sp_slab_init();
  if (sp_slab_on && need <= SP_SLAB_MAX) {
    int cls = sp_slab_cls_of[(need + 15) >> 4];
    size_t csize = sp_slab_csize[cls];
    sp_slab_worker *wk = &sp_slab_wk[SP_SLAB_WID()];
    sp_slab_chunk *ch = wk->cur[cls];
    void *p;
    if (ch && (p = sp_slab_pop(ch)) != NULL) { __atomic_fetch_sub(&ch->nfree, 1, __ATOMIC_RELAXED); return p; }
    if (ch && ch->bump < ch->end) { p = ch->bump; ch->bump += csize; __atomic_fetch_sub(&ch->nfree, 1, __ATOMIC_RELAXED); ch->touched = 1; return p; }
    p = sp_slab_refill(wk, cls, csize);
    if (p) return p;
  }
  void *p = malloc(need);
  if (!p) sp_oom_die();
  return p;
}

void *sp_slab_alloc(size_t need) {
  void *p = sp_slab_alloc_raw(need);
  memset(p, 0, need);
  return p;
}

/* Only from a sweep. Under the stop-the-world sweep the only concurrency is
   between sweeping workers freeing into the same chunk; under the concurrent
   sweep the chunk's owner is running and allocating from it at the same time,
   which is why the free list, the chunk's presence on the available list and
   the owner's current chunk are all exchanged atomically. */
/* Frees are batched per chunk: a sweep walks its list in allocation order,
   which is bump order within a chunk, so runs of dead slots from one chunk
   are long, and the run goes onto the chunk's free list with ONE exchange.
   The exchange lands on the line the owner pops from; under the concurrent
   sweep the owner is allocating from that very chunk, and an exchange per
   slot had the line bouncing between the two cores on every allocation
   (the mutators measured 30% slower while a sweep ran). */
static SP_TLS struct { sp_slab_chunk *ch; void *head, *tail; uint32_t n; } sp_slab_fb;

void sp_slab_free_flush(void) {
  sp_slab_chunk *ch = sp_slab_fb.ch;
  if (!ch) return;
  sp_slab_fb.ch = NULL;
#ifdef SP_THREADS
  void *old;
  do { old = __atomic_load_n(&ch->free, __ATOMIC_ACQUIRE); *(void **)sp_slab_fb.tail = old;
  } while (!__atomic_compare_exchange_n(&ch->free, &old, sp_slab_fb.head, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
  __atomic_fetch_add(&ch->nfree, sp_slab_fb.n, __ATOMIC_RELAXED);
  /* A chunk its owner dropped as full comes back onto the available list on
     its first free. The owner's current chunk is read racily: when it is
     mid-refill the chunk it is installing can be pushed here too, and refill
     drops it again when it finds it empty. */
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
  *(void **)sp_slab_fb.tail = ch->free; ch->free = sp_slab_fb.head; ch->nfree += sp_slab_fb.n;
  if (!ch->on_avail && sp_slab_wk[0].cur[ch->cls] != ch) {
    ch->on_avail = 1;
    ch->next_avail = sp_slab_wk[0].avail[ch->cls];
    sp_slab_wk[0].avail[ch->cls] = ch;
  }
#endif
}

void sp_slab_free(void *p) {
  if (!sp_slab_owns(p)) { free(p); return; }
  sp_slab_chunk *ch = sp_slab_chunk_of(p);
  if (ch != sp_slab_fb.ch) {
    sp_slab_free_flush();
    sp_slab_fb.ch = ch; sp_slab_fb.head = sp_slab_fb.tail = p; sp_slab_fb.n = 1;
    return;
  }
  *(void **)p = sp_slab_fb.head; sp_slab_fb.head = p; sp_slab_fb.n++;
}

/* End of a full cycle, stop-the-world, one thread: give the OS every chunk
   that is entirely free beyond each worker's reserve. The reserve is what the
   worker went through since the last release, with a margin: a young
   generation is by construction empty again after every cycle, and releasing
   it each time only had the kernel fault and zero the same pages back in a
   moment later (a quarter of the collector's time in page faults). What is
   kept is the working set; what is released is the excess after a burst.
   The available lists are rebuilt rather than edited in place, which is also
   where a fully free chunk leaves its class and returns to the global pool,
   so a burst of one size does not hold chunks another size needs later. */
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
static sp_slab_chunk **sp_slab_sortbuf = NULL;
static size_t sp_slab_sortcap = 0;
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

void sp_slab_release(void) {
  sp_slab_free_flush();
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
        for (int i = 1; i < (int)SP_SLAB_NCHUNK; i++) {
          sp_slab_chunk *ch = &ar->ch[i];
          if (!ch->in_use) { if (!ch->touched) untouched++; continue; }
          nuse++;
          if (ch->nfree == ch->nslots) nempty++;
          live += (size_t)(ch->nslots - ch->nfree) * sp_slab_csize[ch->cls];
        }
      }
      fprintf(stderr, "[slab] arenas %zu  chunks in use %zu (fully free %zu)  live in slots %.1f MB  resident chunks %.1f MB\n",
              (size_t)((sp_slab_brk - sp_slab_base) / SP_SLAB_ARENA), nuse, nempty,
              live / 1048576.0, nuse * (SP_SLAB_CHUNK / 1048576.0));
    }
  }
  for (int w = 0; w < SP_SLAB_NWK; w++) {
    sp_slab_worker *wk = &sp_slab_wk[w];
    long reserve = wk->taken;
    if (reserve < SP_SLAB_RESERVE) reserve = SP_SLAB_RESERVE;
    wk->taken = 0;
    for (int cls = 0; cls < SP_SLAB_NCLS; cls++) {
      sp_slab_chunk *keep = NULL, *ch = wk->avail[cls];
      while (ch) {
        sp_slab_chunk *nx = ch->next_avail;
        if (ch->nfree == ch->nslots && reserve <= 0) {
          char *base = sp_slab_chunk_base(ch);
          if (ch->touched) madvise(base, SP_SLAB_CHUNK, MADV_DONTNEED);
          ch->in_use = 0; ch->on_avail = 0; ch->touched = 0;
          ch->free = NULL; ch->bump = ch->end = NULL; ch->nfree = ch->nslots = 0;
          ch->next_avail = sp_slab_empty;
          sp_slab_empty = ch;
        }
        else {
          if (ch->nfree == ch->nslots) reserve--;
          ch->next_avail = keep;
          keep = ch;
        }
        ch = nx;
      }
      wk->avail[cls] = sp_slab_sort_avail(keep);
    }
  }
}

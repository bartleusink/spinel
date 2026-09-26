/*
** re_spinel.c - the regexp engine's entry points for spinel
**
** The engine in this directory is mruby-regexp's, taken from mruby as mruby
** carries it (see shim/mruby.h). This file is the part that is spinel's own:
** the mruby API the engine calls, answered with the C library, and the
** re_compile / re_exec / re_free surface the runtime (lib/sp_re.c, the
** strscan package) and the compiler's literal check (src/re_lit_check.c)
** have always called. A compiled pattern is handed out as the engine's
** mrb_regexp_pattern inside a record that also keeps the pattern text, for
** Regexp#source and #inspect.
*/
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include "re_spinel.h"
#include <mruby/string.h>
#include <mruby/error.h>

#ifdef SP_THREADS
# define RE_TLS _Thread_local
#else
# define RE_TLS
#endif

static mrb_state re_mrb;   /* nothing reads it; the engine passes it along */

/* ---- memory ---- */

static __attribute__((noreturn)) void re_nomem(void) {
  fputs("[FATAL] regexp: failed to allocate memory\n", stderr);
  abort();
}
void *mrb_malloc(mrb_state *mrb, size_t len) {
  (void)mrb;
  void *p = malloc(len ? len : 1);
  if (!p) re_nomem();
  return p;
}
void *mrb_malloc_simple(mrb_state *mrb, size_t len) {
  (void)mrb;
  return malloc(len ? len : 1);
}
void *mrb_calloc(mrb_state *mrb, size_t nelem, size_t len) {
  (void)mrb;
  void *p = calloc(nelem ? nelem : 1, len ? len : 1);
  if (!p) re_nomem();
  return p;
}
void *mrb_realloc(mrb_state *mrb, void *p, size_t len) {
  (void)mrb;
  void *q = realloc(p, len ? len : 1);
  if (!q) re_nomem();
  return q;
}
void *mrb_realloc_simple(mrb_state *mrb, void *p, size_t len) {
  (void)mrb;
  return realloc(p, len ? len : 1);
}
void mrb_free(mrb_state *mrb, void *p) { (void)mrb; free(p); }

/* ---- the strings a compile makes ----
   mruby's GC collects these; here every one made while a compile runs is on
   one list, and the compile releases the list when it returns or raises. */

static RE_TLS struct sp_re_str *re_strs;

static mrb_value re_str_alloc(mrb_int capa) {
  struct sp_re_str *s = (struct sp_re_str *)mrb_malloc(NULL, sizeof *s);
  s->capa = capa < 16 ? 16 : capa;
  s->ptr = (char *)mrb_malloc(NULL, (size_t)s->capa + 1);
  s->len = 0;
  s->ptr[0] = 0;
  s->next = re_strs;
  re_strs = s;
  return s;
}
static void re_strs_release(void) {
  while (re_strs) {
    struct sp_re_str *n = re_strs->next;
    free(re_strs->ptr);
    free(re_strs);
    re_strs = n;
  }
}
static void re_str_reserve(mrb_value s, mrb_int need) {
  if (need <= s->capa) return;
  mrb_int capa = s->capa;
  while (capa < need) capa *= 2;
  s->ptr = (char *)mrb_realloc(NULL, s->ptr, (size_t)capa + 1);
  s->capa = capa;
}
mrb_value mrb_str_cat(mrb_state *mrb, mrb_value str, const char *p, size_t len) {
  (void)mrb;
  re_str_reserve(str, str->len + (mrb_int)len);
  memcpy(str->ptr + str->len, p, len);
  str->len += (mrb_int)len;
  str->ptr[str->len] = 0;
  return str;
}
mrb_value mrb_str_new(mrb_state *mrb, const char *p, size_t len) {
  mrb_value s = re_str_alloc((mrb_int)len);
  if (p) mrb_str_cat(mrb, s, p, len);
  else { s->len = (mrb_int)len; memset(s->ptr, 0, len + 1); }
  return s;
}
mrb_value mrb_str_new_cstr(mrb_state *mrb, const char *p) {
  return mrb_str_new(mrb, p, strlen(p));
}
mrb_value mrb_str_resize(mrb_state *mrb, mrb_value str, mrb_int len) {
  (void)mrb;
  re_str_reserve(str, len);
  if (len > str->len) memset(str->ptr + str->len, 0, (size_t)(len - str->len));
  str->len = len;
  str->ptr[len] = 0;
  return str;
}
void *mrb_temp_alloc(mrb_state *mrb, size_t size) {
  return RSTRING_PTR(mrb_str_new(mrb, NULL, size));
}

/* mruby's format, for the directives the engine uses: %l (a pointer and a
   size_t length), %v (a String), %c (a char, passed as an int), %d (an int),
   and a backslash that takes the next character literally */
mrb_value mrb_format(mrb_state *mrb, const char *fmt, ...) {
  mrb_value s = re_str_alloc(64);
  va_list ap;
  va_start(ap, fmt);
  for (const char *p = fmt; *p; p++) {
    /* a backslash takes the next character as it is, as mruby's does */
    if (*p == '\\' && p[1]) { p++; mrb_str_cat(mrb, s, p, 1); continue; }
    if (*p != '%' || !p[1]) { mrb_str_cat(mrb, s, p, 1); continue; }
    p++;
    switch (*p) {
      case 'l': {
        const char *q = va_arg(ap, const char *);
        size_t n = va_arg(ap, size_t);
        mrb_str_cat(mrb, s, q, n);
        break;
      }
      case 'v': {
        mrb_value v = va_arg(ap, mrb_value);
        if (v) mrb_str_cat(mrb, s, v->ptr, (size_t)v->len);
        break;
      }
      case 'c': { char ch = (char)va_arg(ap, int); mrb_str_cat(mrb, s, &ch, 1); break; }
      case 'd': {
        char buf[24];
        int n = snprintf(buf, sizeof buf, "%d", va_arg(ap, int));
        mrb_str_cat(mrb, s, buf, (size_t)n);
        break;
      }
      default: mrb_str_cat(mrb, s, p, 1); break;
    }
  }
  va_end(ap);
  return s;
}

/* ---- the one exception the engine raises ---- */

/* Issue #781: the program installs a handler at startup that raises its
   RegexpError (sp_raise_cls is per translation unit, so the library reaches
   it through this pointer); it does not return. Without one the message is
   printed and the process exits, as it was. */
static void (*sp_re_error_handler)(const char *msg) = NULL;
void sp_re_set_error_handler(void (*fn)(const char *msg)) {
  sp_re_error_handler = fn;
}

static RE_TLS char re_errbuf[1024];

static __attribute__((noreturn)) void re_raise_msg(const char *msg, size_t len) {
  if (len >= sizeof re_errbuf) len = sizeof re_errbuf - 1;
  memcpy(re_errbuf, msg, len);
  re_errbuf[len] = 0;
  re_strs_release();   /* the message is copied out: nothing reads the list now */
  if (sp_re_error_handler) sp_re_error_handler(re_errbuf);
  fprintf(stderr, "RegexpError: %s\n", re_errbuf);
  exit(1);
}

struct RClass *mrb_exc_get_id(mrb_state *mrb, mrb_sym name) {
  (void)mrb; (void)name;
  return NULL;
}
mrb_value mrb_exc_new_str(mrb_state *mrb, struct RClass *c, mrb_value str) {
  (void)mrb; (void)c;
  return str;
}
void mrb_exc_raise(mrb_state *mrb, mrb_value exc) {
  (void)mrb;
  re_raise_msg(exc ? exc->ptr : "invalid pattern", exc ? (size_t)exc->len : 15);
}

/* A compile error quotes the pattern as Regexp#inspect would, `/src/flags`.
   Ruby's `m` is RE_FLAG_DOTALL to spinel, which is the bit spinel passes. */
void mrb_re_flags_cat(mrb_state *mrb, mrb_value str, uint32_t flags) {
  if (flags & RE_FLAG_DOTALL)     mrb_str_cat(mrb, str, "m", 1);
  if (flags & RE_FLAG_IGNORECASE) mrb_str_cat(mrb, str, "i", 1);
  if (flags & RE_FLAG_EXTENDED)   mrb_str_cat(mrb, str, "x", 1);
}

/* ---- UTF-8 ---- */

#include "re_spinel_utf8.inc"

int re_utf8_charlen(const char *s, const char *end) {
  return (int)mrb_re_charlen(s, end, FALSE);
}

/* ---- \p{...} ----
   The engine refuses a character property; spinel answers the ones it carries
   tables for (spinel #4143: once-campfire's String#all_emoji? asks
   \p{Emoji}). Rather than a matcher of its own inside the engine, a property
   is spelled out, before the engine reads the pattern, as a set the engine
   already reads: a POSIX-named one as the bracket it is ([[:alpha:]]), a
   general category or an emoji property as a class of its codepoint ranges.
   Everything the engine does with a set then applies to it unchanged --
   negation, a class it sits in, `&&`, and the /i closure, which CRuby
   applies to a property too (/\p{Lu}/i matches "a"). A name it has no table
   for is left as written, and the engine refuses it. */

#include "re_uniprop.h"

static char re_prop_lc(char ch) { return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch; }

/* The name as CRuby compares it: case, `_`, `-` and spaces do not count. */
static size_t re_prop_norm(const char *name, size_t len, char *norm, size_t cap) {
  size_t n = 0;
  for (size_t i = 0; i < len && n + 1 < cap; i++) {
    char ch = name[i];
    if (ch == '_' || ch == '-' || ch == ' ') continue;
    norm[n++] = re_prop_lc(ch);
  }
  norm[n] = 0;
  return n;
}

static const char *const re_prop_posix[] = {
  "alpha", "alnum", "upper", "lower", "digit", "xdigit", "word", "space",
  "blank", "punct", "cntrl", "graph", "print", "ascii", NULL
};

/* What a general category or emoji name is, as a test on one run of the
   table: `kind` 0 a two-letter category (arg its id), 1 a one-letter one
   (arg its letter), 2 an emoji property (arg its bit). -1 for no such name. */
static int re_prop_lookup(const char *norm, size_t n, int *arg) {
  if (n == 1) {
    for (int i = 0; i < RE_GC_COUNT; i++)
      if (re_prop_lc(re_gc_names[i][0]) == norm[0]) { *arg = norm[0]; return 1; }
    return -1;
  }
  if (n == 2) {
    for (int i = 0; i < RE_GC_COUNT; i++) {
      const char *g = re_gc_names[i];
      if (re_prop_lc(g[0]) == norm[0] && re_prop_lc(g[1]) == norm[1]) { *arg = i; return 0; }
    }
  }
  static const struct { const char *name; int bit; } emo[] = {
    { "emoji", RE_EMOJI_EMOJI },
    { "emojipresentation", RE_EMOJI_EMOJI_PRESENTATION },
    { "extendedpictographic", RE_EMOJI_EXTENDED_PICTOGRAPHIC },
  };
  for (size_t i = 0; i < sizeof(emo) / sizeof(emo[0]); i++)
    if (strcmp(norm, emo[i].name) == 0) { *arg = emo[i].bit; return 2; }
  return -1;
}

static void re_cat_u(mrb_value out, uint32_t cp) {
  char buf[16];
  int k = snprintf(buf, sizeof buf, "\\u{%X}", (unsigned)cp);
  mrb_str_cat(NULL, out, buf, (size_t)k);
}

/* Append [lo, hi] as class members, leaving out the surrogates, which no
   UTF-8 string holds and the engine refuses to spell. */
static void re_cat_range(mrb_value out, uint32_t lo, uint32_t hi, mrb_bool *any) {
  if (lo <= 0xDFFF && hi >= 0xD800) {
    if (lo < 0xD800) re_cat_range(out, lo, 0xD7FF, any);
    if (hi > 0xDFFF) re_cat_range(out, 0xE000, hi, any);
    return;
  }
  *any = TRUE;
  re_cat_u(out, lo);
  if (hi > lo) { mrb_str_cat(NULL, out, "-", 1); re_cat_u(out, hi); }
}

/* The class of every codepoint the property holds (or, `negated`, lacks). */
static void re_cat_prop_class(mrb_value out, int kind, int arg, mrb_bool negated) {
  mrb_str_cat(NULL, out, "[", 1);
  mrb_bool any = FALSE;
  uint32_t open = 0;           /* where the current run of holders began */
  mrb_bool in = FALSE;
  uint32_t nruns = kind == 2 ? RE_EMOJI_RUNS : RE_GC_RUNS;
  uint32_t prev_end = 0;       /* codepoints below the first emoji run hold nothing */
  for (uint32_t i = 0; i <= nruns; i++) {
    uint32_t start, has;
    if (i == nruns) { start = 0x110000; has = !in; }   /* close the last run */
    else if (kind == 2) {
      start = RE_EMOJI_RUN_START(re_emoji_table[i]);
      has = (RE_EMOJI_RUN_SET(re_emoji_table[i]) & (uint32_t)arg) != 0;
    }
    else {
      start = RE_GC_RUN_START(re_gc_table[i]);
      uint32_t cat = RE_GC_RUN_CAT(re_gc_table[i]);
      has = kind == 0 ? cat == (uint32_t)arg : re_prop_lc(re_gc_names[cat][0]) == (char)arg;
    }
    if (negated && i < nruns) has = !has;
    /* the stretch before the first emoji run holds no emoji property */
    if (i == 0 && start > 0) {
      if (negated) { in = TRUE; open = 0; }
    }
    if (i == nruns) {
      if (in) re_cat_range(out, open, 0x10FFFF, &any);
      break;
    }
    if (has && !in) { in = TRUE; open = start; }
    else if (!has && in) { in = FALSE; re_cat_range(out, open, start - 1, &any); }
    prev_end = start;
  }
  (void)prev_end;
  /* a property that holds nothing: a class that matches nothing */
  if (!any) {
    static const char none[] = "&&[^\\x00-\\x7F\\u{80}-\\u{10FFFF}]";
    mrb_str_cat(NULL, out, none, strlen(none));
  }
  mrb_str_cat(NULL, out, "]", 1);
}

/* The pattern with each property spelled as a set, or NULL when it names
   none. `\\` pairs are stepped over whole, so `\\p{L}` is a backslash and
   letters, and inside a class a property is written as a class nested in
   it, which the class reads as a union. */
static mrb_value re_expand_props(const char *pat, mrb_int len) {
  const char *end = pat + len;
  const char *q;
  for (q = pat; q + 2 < end; q++) {
    if (*q == '\\') {
      if ((q[1] == 'p' || q[1] == 'P') && q[2] == '{') break;
      q++;
    }
  }
  if (q + 2 >= end) return NULL;
  mrb_value out = mrb_str_new(NULL, NULL, 0);
  int depth = 0;           /* how deep in classes */
  const char *p = pat;
  while (p < end) {
    char ch = *p;
    if (ch == '\\' && p + 1 < end) {
      if ((p[1] == 'p' || p[1] == 'P') && p + 2 < end && p[2] == '{') {
        const char *name = p + 3;
        const char *close = name;
        while (close < end && *close != '}') close++;
        if (close < end) {
          mrb_bool negated = p[1] == 'P';
          const char *nm = name;
          if (nm < close && *nm == '^') { negated = !negated; nm++; }
          char norm[64];
          size_t n = re_prop_norm(nm, (size_t)(close - nm), norm, sizeof norm);
          int posix = -1;
          for (int i = 0; n && re_prop_posix[i]; i++)
            if (strcmp(norm, re_prop_posix[i]) == 0) { posix = i; break; }
          int arg = 0, kind = posix < 0 && n ? re_prop_lookup(norm, n, &arg) : -1;
          if (posix >= 0) {
            mrb_str_cat(NULL, out, depth ? "[:" : "[[:", depth ? 2 : 3);
            if (negated) mrb_str_cat(NULL, out, "^", 1);
            mrb_str_cat(NULL, out, re_prop_posix[posix], strlen(re_prop_posix[posix]));
            mrb_str_cat(NULL, out, depth ? ":]" : ":]]", depth ? 2 : 3);
            p = close + 1;
            continue;
          }
          if (kind >= 0) {
            re_cat_prop_class(out, kind, arg, negated);
            p = close + 1;
            continue;
          }
        }
      }
      mrb_str_cat(NULL, out, p, 2);
      p += 2;
      continue;
    }
    if (ch == '[') {
      /* a POSIX bracket inside a class is its own [ and ] */
      if (depth && p + 1 < end && p[1] == ':') {
        const char *e2 = p + 2;
        while (e2 + 1 < end && !(e2[0] == ':' && e2[1] == ']')) e2++;
        if (e2 + 1 < end) {
          mrb_str_cat(NULL, out, p, (size_t)(e2 + 2 - p));
          p = e2 + 2;
          continue;
        }
      }
      depth++;
      mrb_str_cat(NULL, out, p, 1);
      p++;
      /* a `]` right after the opening (or after `^`) is a member */
      if (p < end && *p == '^') { mrb_str_cat(NULL, out, p, 1); p++; }
      if (p < end && *p == ']') { mrb_str_cat(NULL, out, p, 1); p++; }
      continue;
    }
    if (ch == ']' && depth) depth--;
    mrb_str_cat(NULL, out, p, 1);
    p++;
  }
  return out;
}

/* ---- spinel's surface ---- */

/* The engine's pattern and the text it was compiled from. The pattern is the
   first member, so a pattern pointer is the record's. */
typedef struct {
  mrb_regexp_pattern pat;
  char *source;
  uint32_t source_len;
  /* the group names as C strings: the engine keeps each as a pointer and a
     length into one arena, with nothing between them, and spinel's side
     reads a name up to its NUL */
  char **names;
} sp_re_record;

mrb_regexp_pattern *re_compile(const char *pattern, mrb_int len, uint32_t flags) {
  sp_re_record *r = (sp_re_record *)mrb_calloc(NULL, 1, sizeof *r);
  r->source = (char *)mrb_malloc(NULL, (size_t)len + 1);
  memcpy(r->source, pattern, (size_t)len);
  r->source[len] = 0;
  r->source_len = (uint32_t)len;
  mrb_value expanded = re_expand_props(pattern, len);
  if (expanded) mrb_re_compile(&re_mrb, &r->pat, RSTRING_PTR(expanded), RSTRING_LEN(expanded), flags, FALSE);
  else mrb_re_compile(&re_mrb, &r->pat, pattern, len, flags, FALSE);
  re_strs_release();
  if (r->pat.num_named) {
    r->names = (char **)mrb_calloc(NULL, r->pat.num_named, sizeof(char *));
    for (uint16_t i = 0; i < r->pat.num_named; i++) {
      const re_named_capture *nc = &r->pat.named_captures[i];
      r->names[i] = (char *)mrb_malloc(NULL, (size_t)nc->name_len + 1);
      memcpy(r->names[i], nc->name, nc->name_len);
      r->names[i][nc->name_len] = 0;
    }
  }
  return &r->pat;
}

void re_free(mrb_regexp_pattern *pat) {
  if (!pat) return;
  sp_re_record *r = (sp_re_record *)pat;
  free(r->source);
  if (r->names) {
    for (uint16_t i = 0; i < pat->num_named; i++) free(r->names[i]);
    free(r->names);
  }
  /* mrb_re_free() releases the pattern block too, which is the record */
  mrb_re_free(&re_mrb, pat);
}

/* A search that stops at one of the engine's limits has no answer to give:
   what it found by then is neither a shorter nor a later match. mruby raises
   RegexpError there, and so does this; the engine used to answer no match. */
int re_exec(const mrb_regexp_pattern *pat, const char *str, mrb_int len, mrb_int start,
            int *captures, int captures_size, int binary) {
  int n = mrb_re_exec(&re_mrb, pat, str, len, start, captures, captures_size, (mrb_bool)(binary != 0));
  if (n >= 0) return n;
  if (n == RE_NOMEM) re_nomem();
  const char *m = n == RE_OVER_STEP_LIMIT ? "step limit over (MRB_REGEXP_STEP_LIMIT)"
                                           : "stack limit over (MRB_REGEXP_STACK_LIMIT)";
  re_raise_msg(m, strlen(m));
}

/* ---- named captures ---- */

int re_num_named(const mrb_regexp_pattern *pat) {
  return pat ? (int)pat->num_named : 0;
}
const char *re_named_name(const mrb_regexp_pattern *pat, int i, int *group_out) {
  if (!pat || i < 0 || i >= (int)pat->num_named) return NULL;
  if (group_out) *group_out = (int)pat->named_captures[i].group;
  return ((const sp_re_record *)pat)->names[i];
}
/* group index -> its name (NUL-terminated copy in a static rotating buffer),
   or NULL when the group is positional. For MatchData#inspect. */
const char *re_group_name(const mrb_regexp_pattern *pat, int group) {
  static RE_TLS char buf[4][64];
  static RE_TLS int rot = 0;
  if (!pat) return NULL;
  for (int k = 0; k < pat->num_named; k++) {
    if (pat->named_captures[k].group == group) {
      size_t n = pat->named_captures[k].name_len;
      if (n > 63) n = 63;
      char *o = buf[rot = (rot + 1) & 3];
      memcpy(o, pat->named_captures[k].name, n);
      o[n] = 0;
      return o;
    }
  }
  return NULL;
}
/* A name may repeat across alternation branches; CRuby resolves to the last
   declared group, so keep scanning and take the final match. */
int re_named_group(const mrb_regexp_pattern *pat, const char *name) {
  if (!pat || !name) return -1;
  size_t nlen = strlen(name);
  int group = -1;
  for (uint16_t i = 0; i < pat->num_named; i++) {
    const re_named_capture *nc = &pat->named_captures[i];
    if (nc->name_len == nlen && memcmp(nc->name, name, nlen) == 0)
      group = (int)nc->group;
  }
  return group;
}

/* ---- Regexp's readers ---- */

const char *sp_re_source(void *vpat) {
  sp_re_record *r = (sp_re_record *)vpat;
  return (r && r->source) ? r->source : "";
}
/* ...and its byte length: the text may hold a NUL, which a strlen stops at */
uint32_t sp_re_source_len(void *vpat) {
  sp_re_record *r = (sp_re_record *)vpat;
  return (r && r->source) ? r->source_len : 0;
}
/* Regexp#options: CRuby's public option bits IGNORECASE=1, EXTENDED=2,
   MULTILINE=4 (the /m "dot matches newline", RE_FLAG_DOTALL here). Returns
   the runtime's sp_int (intptr_t), as sp_re.h declares it. */
intptr_t sp_re_options(void *vpat) {
  mrb_regexp_pattern *pat = (mrb_regexp_pattern *)vpat;
  uint32_t f = pat ? pat->flags : 0;
  intptr_t o = 0;
  if (f & RE_FLAG_IGNORECASE) o |= 1;
  if (f & RE_FLAG_EXTENDED)   o |= 2;
  if (f & RE_FLAG_DOTALL)     o |= 4;
  return o;
}
/* Regexp#== / #eql? is source AND options: /ab/ and /ab/i differ (#3631).
   Declared sp_bool (a C bool) by the runtime. */
bool sp_re_eq(void *a, void *b) {
  if (a == b) return 1;
  if (!a || !b) return 0;
  uint32_t la = sp_re_source_len(a), lb = sp_re_source_len(b);
  return la == lb && memcmp(sp_re_source(a), sp_re_source(b), la) == 0 &&
         sp_re_options(a) == sp_re_options(b);
}
bool sp_re_casefold_p(void *vpat) {
  mrb_regexp_pattern *pat = (mrb_regexp_pattern *)vpat;
  return (pat && (pat->flags & RE_FLAG_IGNORECASE)) ? true : false;
}
/* The engine's own flag word, for re-compiling a copy (Regexp.new(re)) with
   the source pattern's exact options. */
uint32_t sp_re_raw_flags(void *vpat) {
  mrb_regexp_pattern *pat = (mrb_regexp_pattern *)vpat;
  return pat ? pat->flags : 0;
}
/* The public Regexp option bits Regexp.new's second argument carries, as the
   flag bits re_compile takes (#3055). */
uint32_t sp_re_opts_to_flags(intptr_t o) {
  uint32_t f = 0;
  if (o & 1) f |= RE_FLAG_IGNORECASE;
  if (o & 2) f |= RE_FLAG_EXTENDED;
  if (o & 4) f |= RE_FLAG_DOTALL;   /* Ruby MULTILINE == "dot matches newline" */
  return f;
}

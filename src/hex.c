/*
 * hex.c — binary files, shown and edited as hex + ASCII.
 *
 * A file with a NUL byte in it is not text, and the editor widget holds a C
 * string: opening one used to show an empty buffer (everything after the first
 * NUL was simply not there), and saving it would have written that emptiness
 * back over the file. So a binary file comes up in *hex mode* instead — the
 * gtcaca `hexview` widget, rows of
 *
 *     00000000  7f 45 4c 46 02 01 01 00  00 00 00 00 00 00 00 00  |.ELF............|
 *
 * with the bytes themselves as the document. It is an ordinary ccm buffer in
 * every other way: it sits in a pane, C-x o and C-x b reach it, the modeline
 * describes it, C-x C-s writes it, C-x C-c asks about it if it is unsaved.
 *
 * ── how it is wired ─────────────────────────────────────────────────────────
 *
 * The hexview widget draws itself (the toolkit's main loop paints it like any
 * other widget) but it is not focusable, so it never receives a key or a click.
 * A `custom` widget — the toolkit's own escape hatch, which *is* focusable and
 * does nothing at all unless given a draw callback — is laid over it with the
 * same geometry and no drawing of its own, and takes the input. It is created
 * after the view so that it is later in the widget list, which is what makes it
 * win the "topmost widget under the cursor" hit test for the mouse.
 *
 * The widget never owns or mutates the bytes: typing reports the intended
 * change through edit_cb / splice_cb and this file applies it to the block it
 * owns. That is what leaves room for an undo history of our own, for marking
 * the bytes that changed, and for knowing exactly what C-x C-s has to write.
 *
 * ── what it is not ──────────────────────────────────────────────────────────
 *
 * It is not a viewer for files too big for memory: searching and editing both
 * want the whole file at once, so the file is read in full and anything past
 * CCM_HEX_MAX_BYTES is turned down with a message rather than opened as
 * something it could not then edit or save.
 */

#include "cacamacs.h"
#include "hex.h"
#include <gtcaca/hexview.h>
#include <gtcaca/custom.h>

/* ── the document ────────────────────────────────────────────────────────────*/

#define HEX_UNDO_MAX   4096      /* undo/redo records kept per buffer */
#define HEX_NEEDLE_MAX 64        /* longest search pattern */

/* One undoable change, in terms of the bytes it covers. `v` holds the bytes
   that were there before (WRITE) or the bytes that were taken out (DELETE);
   INSERT needs none, since undoing it only has to take them back out. */
enum { HU_WRITE, HU_INSERT, HU_DELETE };
typedef struct { int op; long off, n; uint8_t *v; } hu_t;

typedef struct ccm_hex_t {
  gtcaca_hexview_widget_t *hv;    /* the view the toolkit draws */
  gtcaca_custom_widget_t  *in;    /* the transparent layer that takes the input */
  int       bi;                   /* the buffer this belongs to */

  uint8_t  *b;                    /* the bytes — this is the document */
  long      len, cap;
  int       dirty;                /* changed since visiting or saving */
  int       ro;                   /* nothing to write back to (archive member…) */

  /* Which bytes differ from the file on disk, so an edit is visible as an edit
     and not just as a number that happens to be wrong. A splice shifts
     everything after it, so from `mark_from` on every byte is "changed" and
     there is no point tracking them one at a time. */
  uint8_t  *mark;
  long      markcap;              /* bytes allocated for the bitmap */
  long      mark_from;            /* -1 = nothing spliced yet */

  hu_t     *un; int nun;
  hu_t     *re; int nre;

  uint8_t   needle[HEX_NEEDLE_MAX];
  int       nlen;                 /* 0 = nothing searched for yet */
  int       ndir;                 /* the direction C-s/C-r last went */
  char      ntext[96];            /* the pattern as the echo line describes it */
  char      ntyped[96];           /* and exactly as it was typed, to ask again */
} ccm_hex_t;

/* Hex mode hangs off the buffer, so a buffer index is all anyone needs to hold.
   hex_current() is for the minibuffer's answers — a search pattern, an offset to
   go to — which arrive through a callback of their own long after the keystroke
   that asked for them returned, and so have to find the buffer in front again
   rather than carry a pointer to it. */
static ccm_hex_t *hex_of(int bi)
{
  if (bi < 0 || bi >= g_nbuf) return NULL;
  return (ccm_hex_t *)g_buffers[bi].hex;
}
static ccm_hex_t *hex_current(void) { return hex_of(g_cur_buf); }

/* ── is this file binary? ────────────────────────────────────────────────────
 *
 * One test: does it hold a NUL byte. That is exactly the thing the editor
 * cannot carry (it would end the text there), so it is exactly the line between
 * a file ccm can show as text and one it has to show as bytes — the same test
 * ccm_is_text applies to what comes out of a decompressor.
 *
 * The whole file is scanned when it is small enough to open as hex. Past that
 * only the head is: a file that big is not going to open either way, and the
 * answer is only needed to say so. */
int ccm_file_is_binary(const char *path)
{
  FILE *f;
  unsigned char buf[65536];
  size_t got;
  unsigned long long scanned = 0, limit = CCM_HEX_MAX_BYTES;
  struct stat st;
  int found = 0;

  if (!path || !path[0]) return 0;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
  if ((unsigned long long)st.st_size > CCM_HEX_MAX_BYTES) limit = sizeof buf;

  f = fopen(path, "rb");
  if (!f) return 0;
  while (!found && scanned < limit && (got = fread(buf, 1, sizeof buf, f)) > 0) {
    if (memchr(buf, 0, got)) found = 1;
    scanned += got;
  }
  fclose(f);
  return found;
}

/* ── the changed-byte bitmap ─────────────────────────────────────────────────*/

static void mark_fit(ccm_hex_t *x, long len)
{
  long need = len / 8 + 1;
  if (need <= x->markcap) return;
  { uint8_t *m = realloc(x->mark, (size_t)need);
    if (!m) return;                       /* marks are cosmetic: carry on without */
    memset(m + x->markcap, 0, (size_t)(need - x->markcap));
    x->mark = m; x->markcap = need; }
}
static void mark_set(ccm_hex_t *x, long off)
{
  if (off < 0) return;
  mark_fit(x, off + 1);
  if (off / 8 < x->markcap) x->mark[off / 8] |= (uint8_t)(1u << (off % 8));
}
static int mark_get(ccm_hex_t *x, long off)
{
  if (x->mark_from >= 0 && off >= x->mark_from) return 1;
  if (off < 0 || off / 8 >= x->markcap) return 0;
  return (x->mark[off / 8] >> (off % 8)) & 1;
}
static void mark_clear_all(ccm_hex_t *x)
{
  if (x->mark) memset(x->mark, 0, (size_t)x->markcap);
  x->mark_from = -1;
}

/* ── undo ────────────────────────────────────────────────────────────────────*/

static void hu_free(hu_t *r) { if (r) { free(r->v); r->v = NULL; } }

static void hu_clear(hu_t **stack, int *n)
{
  int i;
  for (i = 0; i < *n; i++) hu_free(&(*stack)[i]);
  free(*stack); *stack = NULL; *n = 0;
}

/* Push one record, dropping the oldest once the history is full — an editor
   that runs out of memory because somebody held a key down is worse than one
   that cannot undo the first few thousand keystrokes of a long session. */
static void hu_push(hu_t **stack, int *n, int op, long off, long len, const uint8_t *v)
{
  hu_t r;
  r.op = op; r.off = off; r.n = len; r.v = NULL;
  if (v && len > 0) {
    r.v = malloc((size_t)len);
    if (!r.v) return;                      /* no record is better than a wrong one */
    memcpy(r.v, v, (size_t)len);
  }
  if (!*stack) {
    *stack = malloc(sizeof(hu_t) * HEX_UNDO_MAX);
    if (!*stack) { free(r.v); return; }
    *n = 0;
  }
  if (*n == HEX_UNDO_MAX) {
    hu_free(&(*stack)[0]);
    memmove(*stack, *stack + 1, sizeof(hu_t) * (HEX_UNDO_MAX - 1));
    (*n)--;
  }
  (*stack)[(*n)++] = r;
}

/* ── the bytes ───────────────────────────────────────────────────────────────*/

/* Hand the block to the widget again: splicing moves bytes and may move the
   block itself, and the widget holds a borrowed pointer and the length. */
static void hex_sync(ccm_hex_t *x)
{
  long cur = x->hv->cursor, top = x->hv->top, anchor = x->hv->anchor;
  gtcaca_hexview_set_data(x->hv, x->b, x->len);
  /* set_data only clamps what no longer exists; keeping the rest means an edit
     in the middle of a file does not throw the view back to the top. */
  if (cur < x->len)    x->hv->cursor = cur;
  if (anchor < x->len) x->hv->anchor = anchor;
  x->hv->top = top;
  if (x->hv->top * gtcaca_hexview_bytes_per_row(x->hv) >= x->len) x->hv->top = 0;
}

static int hex_room(ccm_hex_t *x, long need)
{
  long cap;
  uint8_t *nb;
  if (need <= x->cap) return 1;
  cap = x->cap ? x->cap : 64;
  while (cap < need) cap += cap / 2 + 64;
  nb = realloc(x->b, (size_t)cap);
  if (!nb) { snprintf(g_message, sizeof g_message, "Out of memory"); return 0; }
  x->b = nb; x->cap = cap;
  return 1;
}

/* Overwrite `n` bytes at `off`, recording what was there — unless `record` says
   the change already belongs to the record on top of the undo stack. */
static int hex_write_bytes(ccm_hex_t *x, long off, const uint8_t *v, long n, int record)
{
  if (x->ro || off < 0 || n <= 0 || off + n > x->len) return 0;
  if (!memcmp(x->b + off, v, (size_t)n)) return 1;      /* nothing changed */
  if (record) hu_push(&x->un, &x->nun, HU_WRITE, off, n, x->b + off);
  hu_clear(&x->re, &x->nre);                            /* a new edit ends the redo line */
  memcpy(x->b + off, v, (size_t)n);
  { long i; for (i = off; i < off + n; i++) mark_set(x, i); }
  x->dirty = 1;
  return 1;
}

static int hex_insert_bytes(ccm_hex_t *x, long off, const uint8_t *v, long n)
{
  if (x->ro || off < 0 || off > x->len || n <= 0) return 0;
  if (!hex_room(x, x->len + n)) return 0;
  memmove(x->b + off + n, x->b + off, (size_t)(x->len - off));
  if (v) memcpy(x->b + off, v, (size_t)n);
  else   memset(x->b + off, 0, (size_t)n);
  x->len += n;
  hu_push(&x->un, &x->nun, HU_INSERT, off, n, NULL);
  hu_clear(&x->re, &x->nre);
  if (x->mark_from < 0 || off < x->mark_from) x->mark_from = off;
  x->dirty = 1;
  hex_sync(x);
  return 1;
}

static int hex_delete_bytes(ccm_hex_t *x, long off, long n)
{
  if (x->ro || off < 0 || n <= 0 || off + n > x->len) return 0;
  hu_push(&x->un, &x->nun, HU_DELETE, off, n, x->b + off);
  hu_clear(&x->re, &x->nre);
  memmove(x->b + off, x->b + off + n, (size_t)(x->len - off - n));
  x->len -= n;
  if (x->mark_from < 0 || off < x->mark_from) x->mark_from = off;
  x->dirty = 1;
  hex_sync(x);
  return 1;
}

/* Undo (or redo) one record, pushing its inverse on the other stack so the two
   directions are the same code read twice. */
static void hex_undo_step(ccm_hex_t *x, int redo)
{
  hu_t **from = redo ? &x->re : &x->un;
  int   *nfrom = redo ? &x->nre : &x->nun;
  hu_t **to   = redo ? &x->un : &x->re;
  int   *nto  = redo ? &x->nun : &x->nre;
  hu_t   r;

  if (x->ro) { snprintf(g_message, sizeof g_message, "Buffer is read-only"); return; }
  if (*nfrom == 0) {
    snprintf(g_message, sizeof g_message, redo ? "No further redo information"
                                               : "No further undo information");
    return;
  }
  r = (*from)[--(*nfrom)];

  switch (r.op) {
  case HU_WRITE:
    if (r.off + r.n <= x->len) {
      hu_push(to, nto, HU_WRITE, r.off, r.n, x->b + r.off);
      memcpy(x->b + r.off, r.v, (size_t)r.n);
      { long i; for (i = r.off; i < r.off + r.n; i++) mark_set(x, i); }
      gtcaca_hexview_set_cursor(x->hv, r.off);
    }
    break;
  case HU_INSERT:                                  /* it was put in: take it out */
    if (r.off + r.n <= x->len) {
      hu_push(to, nto, HU_DELETE, r.off, r.n, x->b + r.off);
      memmove(x->b + r.off, x->b + r.off + r.n, (size_t)(x->len - r.off - r.n));
      x->len -= r.n;
      if (x->mark_from < 0 || r.off < x->mark_from) x->mark_from = r.off;
      hex_sync(x);
      gtcaca_hexview_set_cursor(x->hv, r.off);
    }
    break;
  case HU_DELETE:                                  /* it was taken out: put it back */
    if (hex_room(x, x->len + r.n)) {
      hu_push(to, nto, HU_INSERT, r.off, r.n, NULL);
      memmove(x->b + r.off + r.n, x->b + r.off, (size_t)(x->len - r.off));
      memcpy(x->b + r.off, r.v, (size_t)r.n);
      x->len += r.n;
      if (x->mark_from < 0 || r.off < x->mark_from) x->mark_from = r.off;
      hex_sync(x);
      gtcaca_hexview_set_cursor(x->hv, r.off);
    }
    break;
  }
  hu_free(&r);
  x->dirty = 1;   /* back where it started is still a buffer that was written to */
  snprintf(g_message, sizeof g_message, redo ? "Redo" : "Undo");
}

/* ── the widget's callbacks ──────────────────────────────────────────────────*/

static int edit_cb(gtcaca_hexview_widget_t *h, long off, uint8_t v, void *ud)
{
  ccm_hex_t *x = (ccm_hex_t *)ud;
  /* Typing a byte on the hex side is two keystrokes and one change: the first
     nibble recorded what was there, so the second must not stack the
     half-finished value on top of it — undoing `41` has to give the byte back,
     not leave 0x40 behind. The widget is on the low nibble of this very byte
     exactly when that is what is going on. */
  int half = h->nibble == 1 && !h->ascii_pane && x->nun > 0 &&
             x->un[x->nun - 1].op == HU_WRITE &&
             x->un[x->nun - 1].off == off && x->un[x->nun - 1].n == 1;
  return hex_write_bytes(x, off, &v, 1, !half);
}

/* A byte is to appear at (or disappear from) `off`. The widget has already
   decided that is what the keystroke meant; all that is left is to do it and to
   hand the block over again, since it has very likely moved. */
static int splice_cb(gtcaca_hexview_widget_t *h, long off, int is_insert, void *ud)
{
  ccm_hex_t *x = (ccm_hex_t *)ud;
  (void)h;
  if (is_insert) return hex_insert_bytes(x, off, NULL, 1);
  return hex_delete_bytes(x, off, 1);
}

/* 12-bit 0xRGB for one of the sixteen ANSI colours — what the widget's own
   background is given as, and what a cell callback has to answer in. */
static uint16_t rgb12_of_ansi(int a)
{
  static const uint16_t t[16] = {
    0x000, 0x00a, 0x0a0, 0x0aa, 0xa00, 0xa0a, 0xa50, 0xaaa,
    0x555, 0x55f, 0x5f5, 0x5ff, 0xf55, 0xf5f, 0xff5, 0xfff
  };
  return (a >= 0 && a < 16) ? t[a] : 0x000;
}

/* Paint the bytes that differ from the file on disk, so an edit can be seen
   before C-x C-s goes anywhere near the file. The cursor and the region are
   left to the widget — it is asked first, and answering for the cell under the
   cursor would paint over it. */
static int cell_cb(gtcaca_hexview_widget_t *h, long off, int is_ascii,
                   uint16_t *fg, uint16_t *bg, void *ud)
{
  ccm_hex_t *x = (ccm_hex_t *)ud;
  long lo, hi;
  (void)is_ascii;
  if (off == h->cursor && h->has_focus) return 0;
  if (gtcaca_hexview_selection(h, &lo, &hi) && off >= lo && off <= hi) return 0;
  if (!mark_get(x, off)) return 0;
  *fg = 0xf55;                                   /* light red: this byte is yours */
  *bg = rgb12_of_ansi(h->has_focus ? h->color_focus_bg : h->color_nonfocus_bg);
  return 1;
}

/* ── patterns: bytes, or text ────────────────────────────────────────────────
 *
 * A hex editor is searched both ways, so the one prompt takes both:
 *
 *     de ad be ef   deadbeef   0xDE,0xAD   -> those four bytes
 *     "GET /"       'ELF'                  -> the characters between the quotes
 *     hello                                 -> the characters, there being no
 *                                              reading of that as hex
 *
 * A bare `cafe` is both at once, and is read as hex — this is a hex editor, and
 * quotes are there to say otherwise. Either way the echo line reports which it
 * was and how many bytes it came to, so it is never a guess. */
static int hex_digit(int c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int parse_pattern(const char *s, uint8_t *out, int max, int *outlen,
                         int *as_hex, int *full)
{
  const char *p = s;
  int n = 0, hi = -1, digits = 0, ok = 1, total = 0;

  while (*p == ' ' || *p == '\t') p++;

  if (*p == '"' || *p == '\'') {                 /* quoted: take it as it reads */
    char q = *p++;
    for (; *p && *p != q; p++, total++)
      if (n < max) out[n++] = (uint8_t)*p;
    *outlen = n; *as_hex = 0; *full = total;
    return n > 0;
  }

  for (; *p && ok; p++) {
    int d;
    if (*p == ' ' || *p == '\t' || *p == ',' || *p == ':' || *p == '-' || *p == '_') continue;
    if ((*p == '0') && (p[1] == 'x' || p[1] == 'X') && hex_digit(p[2]) >= 0) { p++; continue; }
    d = hex_digit((unsigned char)*p);
    if (d < 0) { ok = 0; break; }
    digits++;
    if (hi < 0) hi = d;
    else { if (n < max) out[n++] = (uint8_t)((hi << 4) | d); hi = -1; }
  }
  if (ok && digits > 0 && (digits % 2) == 0 && n > 0) {
    *outlen = n; *as_hex = 1; *full = digits / 2;
    return 1;
  }

  /* Not hex (or an odd number of digits, which is half a byte): the literal
     characters, which is what somebody searching for `hello` meant. */
  p = s;
  while (*p == ' ' || *p == '\t') p++;
  for (n = 0, total = 0; *p; p++, total++) if (n < max) out[n++] = (uint8_t)*p;
  *outlen = n; *as_hex = 0; *full = total;
  return n > 0;
}

/* The bytes as a hex string ("de ad be ef"), for the kill buffer and messages. */
static void bytes_to_hex(const uint8_t *v, long n, char *out, size_t outsz)
{
  size_t at = 0;
  long i;
  if (outsz < 4) { if (outsz) out[0] = '\0'; return; }
  for (i = 0; i < n; i++) {
    size_t need = i ? 4 : 3;                    /* " hh" (or "hh") and the NUL */
    if (at + need > outsz) break;
    at += (size_t)snprintf(out + at, outsz - at, i ? " %02x" : "%02x", v[i]);
  }
  out[at] = '\0';
  if (i < n && at + 4 <= outsz) strcpy(out + at, " ...");   /* there was more */
}

/* ── search ──────────────────────────────────────────────────────────────────*/

static void hex_find_from(ccm_hex_t *x, long from, int back)
{
  long at;
  int wrapped = 0;

  if (x->nlen <= 0) { snprintf(g_message, sizeof g_message, "Nothing to search for yet"); return; }
  at = gtcaca_hexview_find(x->hv, x->needle, x->nlen, from, back);
  if (at < 0) {                                  /* round the end, as C-s does */
    at = gtcaca_hexview_find(x->hv, x->needle, x->nlen,
                             back ? x->len - 1 : 0, back);
    wrapped = 1;
  }
  if (at < 0) {
    gtcaca_hexview_set_highlight(x->hv, -1, 0);
    snprintf(g_message, sizeof g_message, "Failing search: %s", x->ntext);
    /* Repeating a search that just failed would only fail again, so the next
       C-s asks instead — with this pattern already at the prompt, which is
       what you want when it was a typo. */
    x->nlen = 0;
    return;
  }
  gtcaca_hexview_set_cursor(x->hv, at);
  gtcaca_hexview_set_highlight(x->hv, at, x->nlen);
  snprintf(g_message, sizeof g_message, "%s%s at %08lx  (C-s next, C-r previous)",
           wrapped ? "Wrapped: " : "", x->ntext, (unsigned long)at);
}

static void hex_search_done(const char *input)
{
  ccm_hex_t *x = hex_current();
  uint8_t pat[HEX_NEEDLE_MAX];
  int n = 0, as_hex = 0, full = 0;
  char shown[96];

  if (!x) return;
  if (!input[0]) {                               /* bare Enter: repeat the last one */
    if (x->nlen > 0) hex_find_from(x, x->hv->cursor + (x->ndir < 0 ? -1 : 1), x->ndir < 0);
    else snprintf(g_message, sizeof g_message, "Nothing to search for yet");
    return;
  }
  if (!parse_pattern(input, pat, (int)sizeof pat, &n, &as_hex, &full) || n <= 0) {
    snprintf(g_message, sizeof g_message, "Nothing to search for in that");
    return;
  }
  memcpy(x->needle, pat, (size_t)n);
  x->nlen = n;
  snprintf(x->ntyped, sizeof x->ntyped, "%s", input);
  bytes_to_hex(pat, n, shown, sizeof shown);
  /* A pattern longer than the needle can hold is searched for as far as it goes
     and says so — quietly looking for a prefix of what was asked would report
     matches that are not there. */
  /* Quote it the way it reads, but only once — a pattern typed *with* quotes
     already carries them. */
  { const char *q = (as_hex || input[0] == '"' || input[0] == '\'') ? "" : "\"";
    char count[64];
    if (n == full) snprintf(count, sizeof count, "%d byte%s", n, n == 1 ? "" : "s");
    else           snprintf(count, sizeof count, "the first %d of %d bytes", n, full);
    snprintf(x->ntext, sizeof x->ntext, "%s%s%s (%s)", q, as_hex ? shown : input, q, count); }
  hex_find_from(x, x->hv->cursor, x->ndir < 0);
}

/* Ask for a pattern, starting from the last one typed — so searching for the
   same thing again is one Enter, and changing it is an edit rather than a
   retype. M-s comes straight here; C-s only when it has nothing to repeat. */
static void hex_prompt_search(ccm_hex_t *x, int back)
{
  x->ndir = back ? -1 : 1;
  start_minibuffer_init(back ? "Hex search backward (bytes or \"text\"): "
                             : "Hex search (bytes or \"text\"): ",
                        hex_search_done, 0, x->ntyped[0] ? x->ntyped : NULL);
}

static void hex_start_search(ccm_hex_t *x, int back)
{
  x->ndir = back ? -1 : 1;
  /* A pattern is already in hand: C-s means "the next one", not "ask again" —
     which is what makes C-s C-s C-s walk a file the way it does in a buffer.
     C-g drops it, and M-s asks for another without waiting for that. */
  if (x->nlen > 0) {
    hex_find_from(x, x->hv->cursor + (back ? -1 : 1), back);
    return;
  }
  hex_prompt_search(x, back);
}

/* ── go to an offset ─────────────────────────────────────────────────────────*/

static void hex_goto_done(const char *input)
{
  ccm_hex_t *x = hex_current();
  const char *p = input;
  long off = 0;
  int digits = 0, dec = 0;

  if (!x || !input[0]) return;
  while (*p == ' ' || *p == '\t') p++;
  if (*p == '+' || *p == '-') p++;               /* a sign is not an offset */
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
  /* Only `#` means decimal: `d` is a hex digit, so `d10` has to stay 0xd10. */
  else if (*p == '#') { dec = 1; p++; }

  for (; *p; p++) {
    int d = dec ? ((*p >= '0' && *p <= '9') ? *p - '0' : -1) : hex_digit((unsigned char)*p);
    if (d < 0) {
      if (*p == ' ' || *p == '\t') break;
      snprintf(g_message, sizeof g_message, "Not an offset: %s", input);
      return;
    }
    off = off * (dec ? 10 : 16) + d;
    digits++;
    if (off > x->len) break;                     /* no point running up a huge number */
  }
  if (!digits) { snprintf(g_message, sizeof g_message, "Not an offset: %s", input); return; }
  if (off >= x->len) {
    snprintf(g_message, sizeof g_message, "%s is past the end (%08lx bytes)",
             input, (unsigned long)x->len);
    off = x->len - 1;
  }
  gtcaca_hexview_set_cursor(x->hv, off);
}

static void hex_start_goto(void)
{
  start_minibuffer("Go to offset (hex, or #decimal): ", hex_goto_done);
}

/* ── the region ──────────────────────────────────────────────────────────────*/

static void hex_set_mark(ccm_hex_t *x)
{
  gtcaca_hexview_set_selection(x->hv, x->hv->cursor, x->hv->cursor);
  g_mark_active = 1;
  snprintf(g_message, sizeof g_message, "Mark set at %08lx", (unsigned long)x->hv->cursor);
}

static void hex_clear_mark(ccm_hex_t *x)
{
  gtcaca_hexview_clear_selection(x->hv);
  g_mark_active = 0;
}

/* The region, if there is one, as [lo, hi) — the half-open form every byte
   operation here takes, rather than the inclusive pair the widget reports. */
static int hex_region(ccm_hex_t *x, long *lo, long *n)
{
  long a, b;
  if (!g_mark_active || !gtcaca_hexview_selection(x->hv, &a, &b)) return 0;
  *lo = a; *n = b - a + 1;
  return *n > 0;
}

static void hex_copy_region(ccm_hex_t *x)
{
  long lo, n;
  char *text;
  if (!hex_region(x, &lo, &n)) { snprintf(g_message, sizeof g_message, "No region — C-Space sets the mark"); return; }
  text = malloc((size_t)n * 3 + 2);
  if (!text) return;
  bytes_to_hex(x->b + lo, n, text, (size_t)n * 3 + 2);
  kill_set(text, (int)strlen(text));
  free(text);
  hex_clear_mark(x);
  snprintf(g_message, sizeof g_message, "Copied %ld byte%s as hex", n, n == 1 ? "" : "s");
}

static void hex_kill_region(ccm_hex_t *x)
{
  long lo, n;
  if (x->ro) { snprintf(g_message, sizeof g_message, "Buffer is read-only"); return; }
  if (!hex_region(x, &lo, &n)) { snprintf(g_message, sizeof g_message, "No region — C-Space sets the mark"); return; }
  { char *text = malloc((size_t)n * 3 + 2);
    if (text) { bytes_to_hex(x->b + lo, n, text, (size_t)n * 3 + 2);
                kill_set(text, (int)strlen(text)); free(text); } }
  hex_clear_mark(x);
  if (hex_delete_bytes(x, lo, n)) {
    gtcaca_hexview_set_cursor(x->hv, lo);
    snprintf(g_message, sizeof g_message, "Killed %ld byte%s", n, n == 1 ? "" : "s");
  }
}

/* C-y. The kill buffer holds text, and what to make of it is the same question
   the search prompt answers: hex if it reads as hex, the characters otherwise.
   The echo line says which, because "de ad" is two bytes one way and five the
   other and nobody should have to guess. */
/* C-y. The kill buffer holds text, and what to make of it is the same question
   the search prompt answers: hex if it reads as hex, the characters otherwise.
   The echo line says which, because "de ad" is two bytes one way and five the
   other and nobody should have to guess. */
static void hex_yank(ccm_hex_t *x)
{
  uint8_t *pat;
  size_t room;
  int n = 0, as_hex = 0, full = 0;
  long at;

  if (x->ro) { snprintf(g_message, sizeof g_message, "Buffer is read-only"); return; }
  if (!g_killbuf || !g_killbuf[0]) { snprintf(g_message, sizeof g_message, "Kill ring is empty"); return; }
  /* No pattern is longer than the text it was written as, so this is room
     enough whichever way the text is read — a region killed with C-w yanks
     back whole, however big it was. */
  room = strlen(g_killbuf) + 1;
  pat = malloc(room);
  if (!pat) { snprintf(g_message, sizeof g_message, "Out of memory"); return; }
  if (!parse_pattern(g_killbuf, pat, (int)room, &n, &as_hex, &full) || n <= 0) {
    snprintf(g_message, sizeof g_message, "Nothing in the kill ring to yank as bytes");
    free(pat);
    return;
  }
  at = x->hv->cursor;
  if (hex_insert_bytes(x, at, pat, n)) {
    gtcaca_hexview_set_cursor(x->hv, at);
    snprintf(g_message, sizeof g_message, "Yanked %d byte%s (%s)", n, n == 1 ? "" : "s",
             as_hex ? "read as hex" : "read as text");
  }
  free(pat);
}

/* ── the file ────────────────────────────────────────────────────────────────*/

int ccm_hex_modified(int bi)
{
  ccm_hex_t *x = hex_of(bi);
  return x ? x->dirty : 0;
}

static int hex_write_to(ccm_hex_t *x, const char *path)
{
  FILE *f;
  size_t put;

  f = fopen(path, "wb");
  if (!f) { snprintf(g_message, sizeof g_message, "Cannot write %s", path); return -1; }
  put = x->len > 0 ? fwrite(x->b, 1, (size_t)x->len, f) : 0;
  if (fclose(f) != 0 || put != (size_t)x->len) {
    snprintf(g_message, sizeof g_message, "Wrote only %lu of %ld bytes to %s",
             (unsigned long)put, x->len, path);
    return -1;
  }
  x->dirty = 0;
  mark_clear_all(x);
  return 0;
}

int ccm_hex_write(int bi, const char *path)
{
  ccm_hex_t *x = hex_of(bi);
  if (!x) return -1;
  if (x->ro) {
    snprintf(g_message, sizeof g_message,
             "%s is read-only — C-x C-w writes it out under another name",
             ccm_base_name(path));
    return -1;
  }
  return hex_write_to(x, path);
}

/* C-x C-w: the bytes under a name of their own. This is the way an archive
   member — or anything else hex mode could only read — becomes a file that can
   be edited, so it writes whether or not the buffer was read-only, and the
   buffer stops being read-only once it has. */
int ccm_hex_write_as(int bi, const char *path)
{
  ccm_hex_t *x = hex_of(bi);
  if (!x) return -1;
  if (hex_write_to(x, path) != 0) return -1;
  x->ro = 0;
  gtcaca_hexview_set_editable(x->hv, 1);
  return 0;
}

void ccm_hex_revert(int bi)
{
  ccm_hex_t *x = hex_of(bi);
  buffer_t *b;
  char err[256] = "";
  char *data;
  size_t len = 0;
  long cur;

  if (!x) return;
  b = &g_buffers[bi];
  data = ccm_slurp(b->path, &len, err, sizeof err);
  if (!data) {
    snprintf(g_message, sizeof g_message, "Cannot read %s: %s", ccm_base_name(b->path),
             err[0] ? err : "cannot read it");
    return;
  }
  cur = x->hv->cursor;
  free(x->b);
  x->b = (uint8_t *)data; x->len = (long)len; x->cap = (long)len + 1;
  x->dirty = 0;
  mark_clear_all(x);
  hu_clear(&x->un, &x->nun);
  hu_clear(&x->re, &x->nre);
  hex_clear_mark(x);
  hex_sync(x);
  gtcaca_hexview_set_cursor(x->hv, cur < x->len ? cur : (x->len ? x->len - 1 : 0));
  b->was_modified = 0;
  ccm_stamp_buffer(bi);
  ccm_log_file('O', b->path);
  snprintf(g_message, sizeof g_message, "Reverted %s", b->path);
}

void ccm_hex_undo(int bi)
{
  ccm_hex_t *x = hex_of(bi);
  if (x) hex_undo_step(x, 0);
}

void ccm_hex_redo(int bi)
{
  ccm_hex_t *x = hex_of(bi);
  if (x) hex_undo_step(x, 1);
}

static void hex_save(int bi)
{
  buffer_t *b = &g_buffers[bi];
  if (!b->has_file || !b->path[0]) { start_save_as(); return; }
  if (b->arc_member) {
    snprintf(g_message, sizeof g_message,
             "%s is inside an archive — C-x C-w writes it out as a file of its own",
             ccm_base_name(b->path));
    return;
  }
  /* The same question Emacs asks before writing over a file that moved on
     underneath the buffer; answering it runs the write (see lock.c). */
  if (!ccm_confirm_save(bi)) return;
  write_buffer_file(b->ed);          /* the one choke point for every write */
}

/* ── keys ────────────────────────────────────────────────────────────────────*/

static const char *HEX_HELP =
  "Hex mode — a binary file, as bytes\n"
  "\n"
  "A file with a NUL byte in it cannot be held as text, so ccm shows it as\n"
  "hex with the ASCII beside it, and the bytes are what you edit and save.\n"
  "\n"
  "Moving\n"
  "  C-f C-b C-n C-p   next/previous byte, next/previous row (arrows too)\n"
  "  C-a C-e           start / end of the row\n"
  "  C-v M-v           page down / page up (PageDown, PageUp)\n"
  "  M-< M->           first / last byte\n"
  "  M-g               go to an offset — hex, or #1024 for decimal\n"
  "  C-l               recentre the view on the cursor\n"
  "  Tab               move between the hex side and the ASCII side\n"
  "  wheel / click     scroll, and put the cursor on a byte\n"
  "\n"
  "Repeating\n"
  "  C-u N <key>       do it N times — C-u 16 C-f, C-u 8 C-o, C-u 4 C-d\n"
  "                    (the digits after C-u are the count, not hex input)\n"
  "  C-x ( … C-x )     record a keyboard macro; C-x e replays it\n"
  "\n"
  "Editing\n"
  "  0-9 a-f           type over the byte under the cursor, a nibble at a time\n"
  "  any character     on the ASCII side, type over the byte with that character\n"
  "  Insert            insert rather than overwrite (the file grows as you type)\n"
  "  C-o / M-o         insert a 00 byte at the cursor / append one at the end\n"
  "  C-d / Backspace   delete the byte at / before the cursor\n"
  "  C-_  C-/  C-x u   undo     (C-x C-_ redoes)\n"
  "\n"
  "Region\n"
  "  C-Space           set the mark, then move to select bytes\n"
  "  M-w / C-w         copy the region as hex / cut it\n"
  "  C-y               yank — hex in the kill ring goes in as bytes, text as text\n"
  "  C-g               cancel the mark, a prompt, or a search\n"
  "\n"
  "Searching\n"
  "  C-s / C-r         search forward / backward; C-s again finds the next one\n"
  "  M-s / M-r         search for something else (C-g also ends the search)\n"
  "                    de ad be ef, deadbeef, 0xDE,0xAD   bytes\n"
  "                    \"GET /\"                            those characters\n"
  "                    hello                              those characters too\n"
  "\n"
  "The file\n"
  "  C-x C-s           save   C-x C-w  save as   C-x C-q  read-only on/off\n"
  "  C-x C-f  C-x b  C-x d  C-x o  C-x 2  C-x 3  C-x 1  C-x 0   as anywhere else\n"
  "  C-x C-c           quit (it asks about unsaved bytes)\n"
  "  M-x hex-mode-exit show this file as text instead\n"
  "\n"
  "Bytes you have changed are shown in red until the buffer is saved. Inserting\n"
  "or deleting marks everything after it, because that is what moved.\n"
  "(q or Esc Esc closes this window.)\n";

void ccm_hex_help(void) { show_help_with("Hex mode", HEX_HELP); }

/* The C-x chords that mean something to a buffer of bytes. The ones that do not
   — folding, the rectangle commands, pretty-printing — say so rather than
   quietly doing nothing, which is how the browser's own C-x table reads. */
static int hex_key_once(ccm_hex_t *x, int key);

static int hex_ctrl_x(ccm_hex_t *x, int key)
{
  buffer_t *b = &g_buffers[x->bi];
  int reps = take_prefix();           /* consumed here; only C-x e uses it */

  switch (key) {
  case '(':                                               /* C-x ( start macro */
    g_macro_len = 0; g_macro_recording = 1;
    snprintf(g_message, sizeof g_message, "Defining keyboard macro...");
    return 1;
  case ')':                                               /* C-x ) end macro */
    if (g_macro_recording) {
      if (g_macro_len >= 2) g_macro_len -= 2;             /* drop the trailing C-x ) */
      g_macro_recording = 0;
      snprintf(g_message, sizeof g_message, "Keyboard macro defined (%d keys)", g_macro_len);
    }
    return 1;
  case 'e': {                                             /* C-x e replay it */
    int i, k;
    if (g_macro_len == 0) {
      snprintf(g_message, sizeof g_message, "No keyboard macro defined");
      return 1;
    }
    g_macro_executing = 1;
    for (k = 0; k < reps; k++)
      for (i = 0; i < g_macro_len; i++) hex_key_once(x, g_macro[i]);
    g_macro_executing = 0;
    return 1;
  }
  case KEY_CTRL_S:      hex_save(x->bi);     return 1;   /* C-x C-s */
  case CACA_KEY_CTRL_W: start_save_as();     return 1;   /* C-x C-w */
  case CACA_KEY_CTRL_F: start_find_file();   return 1;   /* C-x C-f */
  case CACA_KEY_CTRL_C: quit_cacamacs();     return 1;   /* C-x C-c */
  case 'u':             hex_undo_step(x, 0); return 1;   /* C-x u */
  case KEY_UNDO:        hex_undo_step(x, 1); return 1;   /* C-x C-_ redo */
  case 'd':             show_browser();      return 1;   /* C-x d */
  case 'b':             pane_switch_buffer();return 1;   /* C-x b */
  case 'o':             pane_other_window(); return 1;   /* C-x o */
  case '2':             pane_split(1);       return 1;   /* C-x 2 */
  case '3':             pane_split(0);       return 1;   /* C-x 3 */
  case '1':             pane_unsplit_one();  return 1;   /* C-x 1 */
  case '0':             pane_delete_current();return 1;  /* C-x 0 */
  case CACA_KEY_CTRL_X:                                   /* C-x C-x exchange */
    if (g_mark_active) {
      long a, bb;
      if (gtcaca_hexview_selection(x->hv, &a, &bb)) {
        long other = x->hv->cursor == a ? bb : a;
        gtcaca_hexview_set_selection(x->hv, x->hv->cursor, other);
      }
    } else snprintf(g_message, sizeof g_message, "No mark set in this buffer");
    return 1;
  case CACA_KEY_CTRL_Q:                                   /* C-x C-q read-only */
    if (b->arc_member || b->comp_ro) {
      snprintf(g_message, sizeof g_message,
               "%s has nowhere to be written back to — C-x C-w saves it elsewhere",
               ccm_base_name(b->path));
      return 1;
    }
    x->ro = !x->ro;
    gtcaca_hexview_set_editable(x->hv, !x->ro);
    snprintf(g_message, sizeof g_message, "Read-only %s", x->ro ? "on" : "off");
    return 1;
  default:
    snprintf(g_message, sizeof g_message,
             "C-x %c does nothing in hex mode — ? lists what does",
             key >= 32 ? key : '?');
    return 1;
  }
}

static int hex_meta(ccm_hex_t *x, int key)
{
  switch (key) {
  case 'x': case 'X':  start_mx();        return 1;   /* M-x */
  case 'w': case 'W':  hex_copy_region(x);return 1;   /* M-w */
  case 'g': case 'G':  g_goto_prefix = 1;             /* M-g … (M-g g too) */
                       snprintf(g_message, sizeof g_message, "M-g-"); return 1;
  case 'o': case 'O':                                  /* M-o append a byte */
    if (hex_insert_bytes(x, x->len, NULL, 1)) {
      gtcaca_hexview_set_cursor(x->hv, x->len - 1);
      snprintf(g_message, sizeof g_message, "Appended a 00 byte at %08lx",
               (unsigned long)(x->len - 1));
    } else if (x->ro) snprintf(g_message, sizeof g_message, "Buffer is read-only");
    return 1;
  case 'v': case 'V':  gtcaca_hexview_key(x->hv, CACA_KEY_PAGEUP, NULL); return 1;
  case '<':            gtcaca_hexview_set_cursor(x->hv, 0); return 1;
  case '>':            gtcaca_hexview_set_cursor(x->hv, x->len ? x->len - 1 : 0); return 1;
  case 's': case 'S':  hex_prompt_search(x, 0); return 1;   /* M-s a new pattern */
  case 'r': case 'R':  hex_prompt_search(x, 1); return 1;   /* M-r the same, backward */
  case CACA_KEY_ESCAPE:                                 /* Esc Esc cancels */
    hex_clear_mark(x);
    gtcaca_hexview_set_highlight(x->hv, -1, 0);
    x->nlen = 0;
    snprintf(g_message, sizeof g_message, "Quit");
    return 1;
  default:
    snprintf(g_message, sizeof g_message, "M-%c is undefined in hex mode",
             key >= 32 ? key : '?');
    return 1;
  }
}

/* Keep the region on the cursor after a motion key, so moving with the mark set
   selects bytes the way moving with the mark set selects text. */
static void hex_after_move(ccm_hex_t *x, long anchor)
{
  if (g_mark_active && anchor >= 0) x->hv->anchor = anchor;
}

static int hex_key_once(ccm_hex_t *x, int key)
{
  buffer_t *b = &g_buffers[x->bi];
  long anchor = x->hv->anchor;

  if (g_ctrl_x) { g_ctrl_x = 0; return hex_ctrl_x(x, key); }
  if (g_goto_prefix && key != CACA_KEY_ESCAPE) {        /* M-g g / M-g M-g */
    g_goto_prefix = 0; g_meta = 0;
    if (key == 'g' || key == 'G') { hex_start_goto(); return 1; }
    snprintf(g_message, sizeof g_message, "M-g %c is undefined", key >= 32 ? key : '?');
    return 1;
  }
  if (g_meta) { g_meta = 0; return hex_meta(x, key); }

  g_message[0] = '\0';

  switch (key) {
  case CACA_KEY_CTRL_X: g_ctrl_x = 1; return 1;
  case CACA_KEY_ESCAPE: g_meta = 1; snprintf(g_message, sizeof g_message, "ESC-"); return 1;

  /* motion */
  case CACA_KEY_CTRL_F: gtcaca_hexview_key(x->hv, CACA_KEY_RIGHT, NULL); hex_after_move(x, anchor); return 1;
  case CACA_KEY_CTRL_B: gtcaca_hexview_key(x->hv, CACA_KEY_LEFT, NULL);  hex_after_move(x, anchor); return 1;
  case CACA_KEY_CTRL_N: gtcaca_hexview_key(x->hv, CACA_KEY_DOWN, NULL);  hex_after_move(x, anchor); return 1;
  case CACA_KEY_CTRL_P: gtcaca_hexview_key(x->hv, CACA_KEY_UP, NULL);    hex_after_move(x, anchor); return 1;
  case CACA_KEY_CTRL_A: gtcaca_hexview_key(x->hv, CACA_KEY_HOME, NULL);  hex_after_move(x, anchor); return 1;
  case CACA_KEY_CTRL_E: gtcaca_hexview_key(x->hv, CACA_KEY_END, NULL);   hex_after_move(x, anchor); return 1;
  case CACA_KEY_CTRL_V: gtcaca_hexview_key(x->hv, CACA_KEY_PAGEDOWN, NULL); hex_after_move(x, anchor); return 1;
  case CACA_KEY_CTRL_L: {
    int rows = x->hv->height > 0 ? x->hv->height : 1;
    int bpr  = gtcaca_hexview_bytes_per_row(x->hv);
    long row = x->hv->cursor / (bpr > 0 ? bpr : 16);
    gtcaca_hexview_set_top(x->hv, row - rows / 2 > 0 ? row - rows / 2 : 0);
    snprintf(g_message, sizeof g_message, "Recentered");
    return 1;
  }

  /* searching */
  case KEY_CTRL_S:      hex_start_search(x, 0); return 1;   /* C-s */
  case CACA_KEY_CTRL_R: hex_start_search(x, 1); return 1;   /* C-r */

  /* the region */
  case KEY_CTRL_SPACE:  hex_set_mark(x);        return 1;   /* C-Space */
  case CACA_KEY_CTRL_W: hex_kill_region(x);     return 1;   /* C-w */
  case CACA_KEY_CTRL_Y: hex_yank(x);            return 1;   /* C-y */
  case CACA_KEY_CTRL_G:                                      /* C-g */
    hex_clear_mark(x);
    gtcaca_hexview_set_highlight(x->hv, -1, 0);
    x->nlen = 0;                 /* the search is over: C-s asks again */
    snprintf(g_message, sizeof g_message, "Quit");
    return 1;

  /* editing */
  case KEY_UNDO:        hex_undo_step(x, 0);    return 1;   /* C-_ and C-/ */
  case CACA_KEY_CTRL_O:                                      /* C-o insert a byte */
    if (hex_insert_bytes(x, x->hv->cursor, NULL, 1))
      snprintf(g_message, sizeof g_message, "Inserted a 00 byte at %08lx",
               (unsigned long)x->hv->cursor);
    else if (x->ro) snprintf(g_message, sizeof g_message, "Buffer is read-only");
    return 1;
  case CACA_KEY_CTRL_D:                                      /* C-d delete forward */
    if (x->ro) { snprintf(g_message, sizeof g_message, "Buffer is read-only"); return 1; }
    gtcaca_hexview_key(x->hv, CACA_KEY_DELETE, NULL);
    return 1;

  /* help */
  case '?':
    /* Only where it cannot be a byte: on the ASCII side `?` is a character to
       type, and in the hex pane `?` is not a digit, so it is free. */
    if (!gtcaca_hexview_pane(x->hv)) { ccm_hex_help(); return 1; }
    break;
  case CACA_KEY_F1:  ccm_hex_help(); return 1;

  default:
    break;
  }

  /* Everything else is the view's: arrows, Home/End, PageUp/PageDown, Tab
     between the panes, Insert, Backspace/Delete, and the hex digits or ASCII
     characters that are the actual typing. */
  if (gtcaca_hexview_key(x->hv, key, NULL)) {
    hex_after_move(x, anchor);
    return 1;
  }

  /* The view turned it down. If it was an edit, say why — a read-only buffer
     that silently ignores typing is indistinguishable from a broken one. */
  if (x->ro && (hex_digit(key) >= 0 || key == CACA_KEY_BACKSPACE ||
                key == CACA_KEY_DELETE || (key >= 32 && key <= 126)))
    snprintf(g_message, sizeof g_message, "%s is read-only (C-x C-q to change that)",
             b->has_file ? ccm_base_name(b->path) : "This buffer");
  else if (x->len == 0)
    snprintf(g_message, sizeof g_message, "%s is empty", ccm_base_name(b->path));
  return 1;   /* hex mode owns every key: see the header of this file */
}

/* The outer layer: the modal questions, the C-u count, and the macro replay —
   everything that decides *whether* hex_key_once runs and how many times. */
static int hex_key(ccm_hex_t *x, int key)
{
  /* The modal questions come first, exactly as they do in on_key: nothing else
     happens until "changed on disk; really edit?" is answered, and while a
     prompt is up the prompt has the keys. */
  if (g_super_active) return supersession_key(key);
  if (g_mb_active)    return minibuffer_key(key);

  /* Mid-chord, the second key belongs to the chord — which consumes any armed
     count itself (C-u 3 C-x e). Repeating it out here would feed `e` to the
     buffer as a hex digit instead, which is what on_key avoids by taking its
     chords before its counts. */
  if (g_ctrl_x || g_goto_prefix || g_meta) return hex_key_once(x, key);

  /* C-u [digits] — a count for the next command. The digits are the count, so
     they are not hex input while one is being read; that is the same bargain
     C-u makes with self-inserting digits in a text buffer. */
  if (g_prefix_reading) {
    if (key >= '0' && key <= '9') {
      g_prefix_arg = (g_prefix_have ? g_prefix_arg : 0) * 10 + (key - '0');
      g_prefix_have = 1;
      snprintf(g_message, sizeof g_message, "C-u %ld-", g_prefix_arg);
      return 1;
    }
    if (key == CACA_KEY_CTRL_U) {
      g_prefix_arg = (g_prefix_have ? g_prefix_arg : 1) * 4; g_prefix_have = 1;
      snprintf(g_message, sizeof g_message, "C-u %ld-", g_prefix_arg);
      return 1;
    }
    g_prefix_reading = 0; g_prefix_pending = 1;
  }
  if (key == CACA_KEY_CTRL_U && !g_prefix_pending) {
    g_prefix_reading = 1; g_prefix_have = 0; g_prefix_arg = 4;
    snprintf(g_message, sizeof g_message, "C-u-");
    return 1;
  }
  /* A count is armed: C-u 16 C-f moves sixteen bytes, C-u 8 C-o inserts eight.
     The chord starters pass through and consume it in their own handlers. */
  if (g_prefix_pending && key != CACA_KEY_CTRL_X && key != CACA_KEY_ESCAPE) {
    int n = take_prefix(), i, r = 1;
    for (i = 0; i < n; i++) r = hex_key_once(x, key);
    return r;
  }
  return hex_key_once(x, key);
}

static int key_cb(gtcaca_custom_widget_t *w, int key, void *ud)
{
  ccm_hex_t *x = (ccm_hex_t *)ud;
  int r;
  (void)w;
  if (!x || g_buffers[x->bi].hex != (struct ccm_hex_t *)x) return 0;
  if (g_macro_recording && !g_macro_executing && g_macro_len < MACRO_MAX)
    g_macro[g_macro_len++] = key;
  r = hex_key(x, key);
  /* The modeline is painted before the pane is, so text set from a draw
     callback would always be a keystroke behind. Setting it here, as the key is
     handled, is what keeps the offset and the byte under the cursor honest. */
  if (g_ed) refresh_modeline(g_ed, NULL);
  return r;
}

/* ── the mouse ───────────────────────────────────────────────────────────────
 *
 * Where a byte is drawn is arithmetic: the address column, two spaces, then
 * three columns per byte with a blank every group, and the ASCII one column per
 * byte after that. The widget lays it out that way and pins the address to
 * eight digits here (see hex_build), so the sum below is exact. */
static long hex_at_xy(ccm_hex_t *x, int mx, int my, int *ascii)
{
  gtcaca_hexview_widget_t *h = x->hv;
  int bpr = gtcaca_hexview_bytes_per_row(h);
  int row = my - h->y, i, hx0, ax0, gaps;
  long base;

  if (bpr <= 0 || row < 0 || row >= h->height) return -1;
  base = (gtcaca_hexview_top(h) + row) * bpr;
  hx0  = h->x + 8 + 2;                       /* after "00000000  " */
  gaps = bpr > 8 ? (bpr - 1) / 8 : 0;
  ax0  = hx0 + 3 * bpr + gaps + 2;           /* after the bytes and " |" */

  if (mx >= ax0 && mx < ax0 + bpr) { *ascii = 1; i = mx - ax0; }
  else {
    for (i = 0; i < bpr; i++) {
      int cx = hx0 + 3 * i + i / 8;
      if (mx >= cx && mx < cx + 2) break;
    }
    if (i >= bpr) return -1;
    *ascii = 0;
  }
  return base + i < x->len ? base + i : -1;
}

static int mouse_cb(gtcaca_custom_widget_t *w, gtcaca_mouse_event_t ev,
                    int mx, int my, int button, void *ud)
{
  ccm_hex_t *x = (ccm_hex_t *)ud;
  int ascii = 0;
  long at;
  (void)w;
  if (!x || g_buffers[x->bi].hex != (struct ccm_hex_t *)x) return 0;

  if (ev == GTCACA_MOUSE_WHEEL) {
    gtcaca_hexview_scroll(x->hv, button == 4 ? 3 : -3);
    if (g_ed) refresh_modeline(g_ed, NULL);
    return 1;
  }
  if (ev != GTCACA_MOUSE_PRESS && ev != GTCACA_MOUSE_MOTION) return 0;

  at = hex_at_xy(x, mx, my, &ascii);
  if (at < 0) return 0;
  if (ev == GTCACA_MOUSE_PRESS) {
    /* A click puts the cursor down and drops any region; dragging from there
       selects, which is the one gesture a hex dump really wants. */
    hex_clear_mark(x);
    gtcaca_hexview_set_pane(x->hv, ascii);
    gtcaca_hexview_set_cursor(x->hv, at);
  } else {
    if (!g_mark_active) {
      gtcaca_hexview_set_selection(x->hv, x->hv->cursor, at);
      g_mark_active = 1;
    } else {
      long a, b;
      if (gtcaca_hexview_selection(x->hv, &a, &b))
        gtcaca_hexview_set_selection(x->hv, x->hv->cursor == b ? a : b, at);
      else gtcaca_hexview_set_selection(x->hv, at, at);
    }
  }
  if (g_ed) refresh_modeline(g_ed, NULL);
  return 1;
}

/* ── the modeline ────────────────────────────────────────────────────────────*/

void ccm_hex_modeline(int bi)
{
  ccm_hex_t *x = hex_of(bi);
  buffer_t *b;
  char text[512], namebuf[128];
  const char *name, *mod;
  char what[64];
  int w = caca_get_canvas_width(gmo.cv), budget = w / 3;
  long cur;
  unsigned char byte = 0;

  if (!x) return;
  b = &g_buffers[bi];
  mod = x->ro ? "%%" : x->dirty ? "**" : "--";
  if (budget < 12) budget = 12;
  if (budget > 40) budget = 40;
  name = compact_path(b->has_file && b->path[0] ? b->path : "*scratch*",
                      namebuf, sizeof namebuf, budget);

  /* "(hex)" for a plain binary; a compressed or archived one says so too, since
     that is why it cannot be written back. */
  if (b->arc_member)   snprintf(what, sizeof what, "hex, in archive");
  else if (b->comp)    snprintf(what, sizeof what, "hex, %s", ccm_comp_name(b->comp));
  else                 snprintf(what, sizeof what, "hex");

  cur = x->hv->cursor;
  if (cur >= 0 && cur < x->len) byte = x->b[cur];

  /* A question owns the whole bar, as it does for a text buffer. */
  if (g_super_active || g_mb_active) {
    snprintf(text, sizeof text, " %s", g_message);
    gtcaca_statusbar_set_text(g_modeline, text);
    return;
  }
  if (g_message[0])
    snprintf(text, sizeof text, " %s %s  (%s)  %08lx/%08lx  %02x %s  %s",
             mod, name, what, (unsigned long)cur, (unsigned long)x->len, byte,
             gtcaca_hexview_insert_mode(x->hv) ? "ins" : "ovw", g_message);
  else
    snprintf(text, sizeof text,
             " %s %s  (%s)  %08lx/%08lx  %02x %s   C-s find  C-x C-s save  ? help",
             mod, name, what, (unsigned long)cur, (unsigned long)x->len, byte,
             gtcaca_hexview_insert_mode(x->hv) ? "ins" : "ovw");
  gtcaca_statusbar_set_text(g_modeline, text);
}

/* ── building and placing ────────────────────────────────────────────────────
 *
 * Both widgets are recycled rather than freed. gtcaca_hexview_new appends the
 * widget to the toolkit's widget list — that is how it comes to be drawn — but
 * gtcaca_hexview_free does not take it out again, so freeing one while the UI
 * is running would leave the next frame walking into freed memory. A hidden
 * widget draws nothing, takes no keys and is skipped by the mouse hit test, so
 * closing a hex buffer hides its pair, strips them and puts them aside for the
 * next one. There are never more than MAXBUF buffers, so the pool is bounded.
 * (Keeping the input layer too means window->focused_child can never be left
 * pointing at freed memory either.) */
static gtcaca_hexview_widget_t *g_hv_pool[MAXBUF];
static gtcaca_custom_widget_t  *g_in_pool[MAXBUF];
static int g_hv_npool, g_in_npool;

static gtcaca_hexview_widget_t *hv_acquire(void)
{
  if (g_hv_npool > 0) return g_hv_pool[--g_hv_npool];
  return gtcaca_hexview_new(NULL, 0, 0, 1, 1);
}
static gtcaca_custom_widget_t *in_acquire(void)
{
  if (g_in_npool > 0) return g_in_pool[--g_in_npool];
  return gtcaca_custom_new(NULL, 0, 0, 1, 1);
}
static void hv_release(gtcaca_hexview_widget_t *h)
{
  if (!h) return;
  gtcaca_widget_hide(GTCACA_WIDGET(h));
  gtcaca_hexview_clear_tags(h);
  gtcaca_hexview_set_highlight(h, -1, 0);
  gtcaca_hexview_set_edit_cb(h, NULL, NULL);
  gtcaca_hexview_set_splice_cb(h, NULL, NULL);
  gtcaca_hexview_set_cell_cb(h, NULL, NULL);
  gtcaca_hexview_set_data(h, NULL, 0);     /* above all: stop pointing at the bytes */
  h->cursor = h->top = h->nibble = h->ascii_pane = 0;
  h->anchor = -1;
  h->has_focus = 0;
  h->parent = NULL;
  if (g_hv_npool < MAXBUF) g_hv_pool[g_hv_npool++] = h;
}
static void in_release(gtcaca_custom_widget_t *c)
{
  if (!c) return;
  gtcaca_widget_hide(GTCACA_WIDGET(c));
  gtcaca_custom_set_key_cb(c, NULL, NULL);
  gtcaca_custom_set_mouse_cb(c, NULL, NULL);
  c->has_focus = 0;
  c->parent = NULL;
  if (g_in_npool < MAXBUF) g_in_pool[g_in_npool++] = c;
}

static int hex_build(ccm_hex_t *x)
{
  /* The view first and the input layer second, so the layer is later in the
     widget list: the mouse hit test takes the last visible widget under the
     cursor, and the layer has to be the one that gets the click. A recycled
     view keeps its old place in that list, so it pairs with a recycled layer;
     a fresh view goes on the end and needs a fresh layer after it. */
  int recycled = g_hv_npool > 0;
  x->hv = hv_acquire();
  if (!x->hv) return -1;
  gtcaca_hexview_set_box(x->hv, 0);            /* the pane's window is the frame */
  gtcaca_hexview_set_group_size(x->hv, 8);
  gtcaca_hexview_set_bytes_per_row(x->hv, 0);  /* as many as the pane is wide */
  gtcaca_hexview_set_addr_digits(x->hv, 8);    /* xxd's eight, and fixed: hex_at_xy
                                                  counts columns from it */
  gtcaca_hexview_set_show_ascii(x->hv, 1);
  /* ~/.cacamacs/theme, as far as a widget that paints in the sixteen ANSI
     colours can carry it — the toolkit's own theme is already in these fields,
     so an untouched ccm needs nothing here. */
  { int fg = x->hv->color_focus_fg, bg = x->hv->color_focus_bg;
    if (ccm_theme_ansi(&fg, &bg)) {
      x->hv->color_focus_fg = x->hv->color_nonfocus_fg = (unsigned char)fg;
      x->hv->color_focus_bg = x->hv->color_nonfocus_bg = (unsigned char)bg;
    } }
  gtcaca_hexview_set_editable(x->hv, !x->ro);
  gtcaca_hexview_set_data(x->hv, x->b, x->len);
  gtcaca_hexview_set_edit_cb(x->hv, edit_cb, x);
  gtcaca_hexview_set_splice_cb(x->hv, splice_cb, x);
  gtcaca_hexview_set_cell_cb(x->hv, cell_cb, x);
  gtcaca_widget_hide(GTCACA_WIDGET(x->hv));

  x->in = recycled ? in_acquire() : gtcaca_custom_new(NULL, 0, 0, 1, 1);
  if (!x->in) return -1;
  /* No draw callback: the layer is there for the keys and the mouse, and a
     custom widget with nothing to draw draws nothing at all. */
  gtcaca_custom_set_key_cb(x->in, key_cb, x);
  gtcaca_custom_set_mouse_cb(x->in, mouse_cb, x);
  gtcaca_custom_set_focusable(x->in, 1);
  gtcaca_widget_hide(GTCACA_WIDGET(x->in));
  return 0;
}

int ccm_hex_adopt(int bi, uint8_t *bytes, long len, int ro, const char *note)
{
  buffer_t *b;
  ccm_hex_t *x;

  if (bi < 0 || bi >= g_nbuf) { free(bytes); return -1; }
  b = &g_buffers[bi];
  if (b->hex) ccm_hex_close(bi);

  x = calloc(1, sizeof *x);
  if (!x) { free(bytes); snprintf(g_message, sizeof g_message, "Out of memory"); return -1; }
  x->bi = bi;
  x->b = bytes; x->len = len; x->cap = len;
  x->ro = ro ? 1 : 0;
  x->mark_from = -1;
  x->ndir = 1;

  if (hex_build(x) != 0) {
    hv_release(x->hv);
    in_release(x->in);
    free(x->b); free(x);
    snprintf(g_message, sizeof g_message, "Cannot create the hex view");
    return -1;
  }
  b->hex = (struct ccm_hex_t *)x;
  strncpy(b->langname, "hex", sizeof b->langname - 1);
  b->langname[sizeof b->langname - 1] = '\0';
  /* The editor widget stays, unseen and read-only: it is what buf_index_of
     identifies the buffer by, and a text command that found its way here has to
     be a no-op rather than a write to a hidden document. */
  if (b->ed) {
    gtcaca_editor_set_text(b->ed, "");
    gtcaca_editor_set_read_only(b->ed, 1);
    gtcaca_editor_empty_undo_buffer(b->ed);
    gtcaca_editor_set_save_point(b->ed);
  }
  if (note && note[0]) {
    snprintf(b->comp_note, sizeof b->comp_note, "%s", note);
    snprintf(g_message, sizeof g_message, "%s", note);
  }
  return 0;
}

int ccm_hex_open_file(int bi, int ro, const char *note)
{
  buffer_t *b;
  struct stat st;
  char err[256] = "";
  char *data;
  size_t len = 0;

  if (bi < 0 || bi >= g_nbuf) return -1;
  b = &g_buffers[bi];
  if (stat(b->path, &st) != 0) {
    snprintf(g_message, sizeof g_message, "Cannot read %s", ccm_base_name(b->path));
    return -1;
  }
  if ((unsigned long long)st.st_size > CCM_HEX_MAX_BYTES) {
    snprintf(g_message, sizeof g_message,
             "%s is %.1f MB — too large for hex mode, which holds the whole file (limit %llu MB)",
             ccm_base_name(b->path), (double)st.st_size / (1024.0 * 1024.0),
             CCM_HEX_MAX_BYTES / (1024ull * 1024));
    return -1;
  }
  data = ccm_slurp(b->path, &len, err, sizeof err);
  if (!data) {
    snprintf(g_message, sizeof g_message, "%s", err[0] ? err : "cannot read it");
    return -1;
  }
  return ccm_hex_adopt(bi, (uint8_t *)data, (long)len, ro, note);
}

void ccm_hex_close(int bi)
{
  buffer_t *b;
  ccm_hex_t *x = hex_of(bi);
  if (!x) return;
  b = &g_buffers[bi];
  in_release(x->in);                 /* put aside, never freed: see hex_build */
  hv_release(x->hv);
  hu_clear(&x->un, &x->nun);
  hu_clear(&x->re, &x->nre);
  free(x->mark);
  free(x->b);
  free(x);
  b->hex = NULL;
}

gtcaca_widget_t *ccm_hex_view(int bi)
{
  ccm_hex_t *x = hex_of(bi);
  return x ? GTCACA_WIDGET(x->hv) : NULL;
}
gtcaca_widget_t *ccm_hex_input(int bi)
{
  ccm_hex_t *x = hex_of(bi);
  return x ? GTCACA_WIDGET(x->in) : NULL;
}

void ccm_hex_place(int bi, int px, int py, int pw, int ph)
{
  ccm_hex_t *x = hex_of(bi);
  if (!x) return;
  /* Both widgets take the pane's whole rectangle: the view to draw in and the
     layer to be clicked in. (position_leaf puts a box around the editor to the
     same end; one expanded child of a marginless box is just the rectangle.) */
  x->hv->x = px; x->hv->y = py; x->hv->width = pw; x->hv->height = ph;
  x->in->x = px; x->in->y = py; x->in->width = pw; x->in->height = ph;
}

/* ── M-x hex-mode / hex-mode-exit ────────────────────────────────────────────*/

void ccm_hex_mode(void)
{
  int bi = g_cur_buf;
  buffer_t *b;

  if (bi < 0) return;
  b = &g_buffers[bi];
  if (b->hex) { snprintf(g_message, sizeof g_message, "Already in hex mode"); return; }
  if (!b->has_file || !b->path[0]) {
    snprintf(g_message, sizeof g_message, "Hex mode shows a file's bytes, and this buffer has no file");
    return;
  }
  if (b->locked) { snprintf(g_message, sizeof g_message, "%s is still encrypted", ccm_base_name(b->path)); return; }
  if (gtcaca_editor_get_modify(b->ed)) {
    snprintf(g_message, sizeof g_message,
             "%s has unsaved changes — save it (C-x C-s) before showing it as hex",
             ccm_base_name(b->path));
    return;
  }
  if (b->is_view) {
    snprintf(g_message, sizeof g_message, "This window is a second view of the file; C-x 1 first");
    return;
  }
  /* An archive member or a one-way compressed file has no file of its own to
     read the bytes from, so hex mode would have nothing to show. */
  if (b->arc_member || b->comp) {
    snprintf(g_message, sizeof g_message,
             "%s is read through another file — hex mode shows files as they are on disk",
             ccm_base_name(b->path));
    return;
  }
  if (ccm_hex_open_file(bi, 0, NULL) != 0) return;
  pane_show_buffer(g_focus_leaf, bi);
  relayout();
  focus_pane(g_focus_leaf);
  snprintf(g_message, sizeof g_message, "%s as hex — ? for the keys, M-x hex-mode-exit to go back",
           ccm_base_name(b->path));
}

void ccm_hex_mode_exit(void)
{
  int bi = g_cur_buf;
  buffer_t *b;
  char *c;

  if (bi < 0) return;
  b = &g_buffers[bi];
  if (!b->hex) { snprintf(g_message, sizeof g_message, "This buffer is not in hex mode"); return; }
  if (ccm_hex_modified(bi)) {
    snprintf(g_message, sizeof g_message,
             "%s has unsaved bytes — save it (C-x C-s) or revert it (M-x revert-buffer) first",
             ccm_base_name(b->path));
    return;
  }
  ccm_hex_close(bi);
  c = b->has_file ? read_file(b->path) : NULL;
  if (b->ed) {
    gtcaca_editor_set_read_only(b->ed, 0);
    gtcaca_editor_set_text(b->ed, c ? c : "");
    gtcaca_editor_empty_undo_buffer(b->ed);
    gtcaca_editor_set_save_point(b->ed);
    gtcaca_editor_goto_pos(b->ed, 0);
  }
  free(c);
  strcpy(b->langname, "fundamental");
  { gtcaca_editor_widget_t *sed = g_ed; const char *sfn = g_filename;
    g_ed = b->ed; g_filename = b->path;
    g_langcfg = NULL; g_grammar = NULL; g_keywords = NULL; g_n_keywords = 0;
    setup_language(b->ed, b->path, NULL);
    buffer_store_globals(bi);
    g_ed = sed; g_filename = sfn; }
  pane_show_buffer(g_focus_leaf, bi);
  relayout();
  focus_pane(g_focus_leaf);
  snprintf(g_message, sizeof g_message,
           "%s as text — anything past a NUL byte is not shown, and saving would drop it",
           ccm_base_name(b->path));
}

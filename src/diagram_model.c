/* diagram_model.c — the diagram document: objects in, ASCII art out, and the
   recogniser that turns ASCII art back into objects.

   Deliberately free of the UI (no caca, no ccm globals) so the round-trip that
   matters — draw, save, reopen, drag the same boxes again — can be reasoned
   about, and tested, on its own. See diagram.h for the model and diagram.c for
   the mode built on it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "diagram.h"

/* Per-shape geometry. `minw/minh` is what the shape needs to be legible at all;
   `defw/defh` is what it gets when dropped from the palette. `odd_h` forces an
   odd height so a diamond's two diagonals meet exactly at the side vertices. */
static const struct {
  const char *name, *sample, *usample;
  int minw, minh, defw, defh, odd_h, odd_w;
} SHAPES[DGM_NSHAPES] = {
  /*  name        ascii    unicode                     min    default  odd h/w */
  { "Box",      "+--+",  "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",  3, 3, 14, 3, 0, 0 },
  { "Rounded",  ".--.",  "\xe2\x95\xad\xe2\x94\x80\xe2\x95\xae",  3, 3, 14, 3, 0, 0 },
  { "Diamond",  "/  \\",  "/  \\",                       5, 5, 13, 5, 1, 0 },
  { "Circle",   "(  )",  "(  )",                       7, 5, 11, 5, 0, 0 },
  { "Ellipse",  "(--)",  "(--)",                       5, 3, 13, 3, 0, 0 },
  { "Cloud",    ".~~.",  ".~~.",                       5, 3, 13, 3, 0, 0 },
  { "Cylinder", "|=|",   "|=|",                        5, 4, 13, 5, 0, 0 },
  { "Hexagon",  "/--\\",  "/--\\",                      5, 3, 14, 3, 0, 0 },
  { "Actor",    " O ",   " O ",                        3, 4,  3, 4, 0, 1 },
};

/* ── UTF-8 ──────────────────────────────────────────────────────────────────
   The art is written out as UTF-8 because the Unicode style draws with box
   characters; an ASCII-style diagram never produces a byte above 0x7f. */

static int utf8_encode(uint32_t cp, char *out)
{
  if (cp < 0x80)     { out[0] = (char)cp; return 1; }
  if (cp < 0x800)    { out[0] = (char)(0xc0 | (cp >> 6));  out[1] = (char)(0x80 | (cp & 0x3f)); return 2; }
  if (cp < 0x10000)  { out[0] = (char)(0xe0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
                       out[2] = (char)(0x80 | (cp & 0x3f)); return 3; }
  out[0] = (char)(0xf0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); out[3] = (char)(0x80 | (cp & 0x3f)); return 4;
}

/* Decode one codepoint; returns the bytes consumed (1 for anything malformed,
   which then round-trips as the raw byte). */
static int utf8_decode(const char *s, uint32_t *cp)
{
  const unsigned char *u = (const unsigned char *)s;
  if (u[0] < 0x80)                       { *cp = u[0]; return 1; }
  if ((u[0] & 0xe0) == 0xc0 && u[1])     { *cp = ((uint32_t)(u[0] & 0x1f) << 6) | (u[1] & 0x3f); return 2; }
  if ((u[0] & 0xf0) == 0xe0 && u[1] && u[2])
    { *cp = ((uint32_t)(u[0] & 0x0f) << 12) | ((uint32_t)(u[1] & 0x3f) << 6) | (u[2] & 0x3f); return 3; }
  if ((u[0] & 0xf8) == 0xf0 && u[1] && u[2] && u[3])
    { *cp = ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3f) << 12)
          | ((uint32_t)(u[2] & 0x3f) << 6) | (u[3] & 0x3f); return 4; }
  *cp = u[0];
  return 1;
}

/* ── glyphs ─────────────────────────────────────────────────────────────────
   Arrowheads stay ASCII in both styles: the Unicode triangles are double-width
   in most terminals and would push a diagram's columns out of alignment. */

typedef struct { uint32_t tl, tr, bl, br, hz, vt; } glyphs_t;

static glyphs_t style_glyphs(int style, int rounded)
{
  glyphs_t g;
  if (style == DGM_STYLE_UNICODE) {
    g.hz = 0x2500; g.vt = 0x2502;
    if (rounded) { g.tl = 0x256d; g.tr = 0x256e; g.bl = 0x2570; g.br = 0x256f; }
    else         { g.tl = 0x250c; g.tr = 0x2510; g.bl = 0x2514; g.br = 0x2518; }
  } else {
    g.hz = '-'; g.vt = '|';
    if (rounded) { g.tl = '.'; g.tr = '.'; g.bl = '\''; g.br = '\''; }
    else         { g.tl = g.tr = g.bl = g.br = '+'; }
  }
  return g;
}

/* The elbow where a connector turns, given the two directions it joins. */
static uint32_t elbow(int style, int from_dx, int from_dy, int to_dx, int to_dy)
{
  if (style != DGM_STYLE_UNICODE) return '+';
  { int up    = (from_dy < 0 || to_dy < 0);
    int down  = (from_dy > 0 || to_dy > 0);
    int left  = (from_dx < 0 || to_dx < 0);
    int right = (from_dx > 0 || to_dx > 0);
    if (down && right) return 0x250c;   /* ┌ */
    if (down && left)  return 0x2510;   /* ┐ */
    if (up   && right) return 0x2514;   /* └ */
    if (up   && left)  return 0x2518;   /* ┘ */
  }
  return 0x253c;                        /* ┼ */
}

/* ── document ───────────────────────────────────────────────────────────────*/

dgm_doc_t *dgm_new(void)
{
  dgm_doc_t *d = calloc(1, sizeof *d);
  if (d) d->style = DGM_STYLE_ASCII;
  return d;
}

void dgm_free(dgm_doc_t *d) { free(d); }

void dgm_clear(dgm_doc_t *d)
{
  int style = d->style;
  memset(d, 0, sizeof *d);
  d->style = style;
}

static void clampf(int *v, int lo, int hi) { if (*v < lo) *v = lo; if (*v > hi) *v = hi; }

static void label_extent(const char *label, int *w, int *h);

static int shape_ok(int shape) { return shape >= 0 && shape < DGM_NSHAPES; }

void dgm_shape_min(int shape, int *w, int *h)
{
  if (!shape_ok(shape)) shape = DGM_RECT;
  *w = SHAPES[shape].minw; *h = SHAPES[shape].minh;
}

void dgm_shape_default(int shape, int *w, int *h)
{
  if (!shape_ok(shape)) shape = DGM_RECT;
  *w = SHAPES[shape].defw; *h = SHAPES[shape].defh;
}

const char *dgm_shape_name(int shape)   { return shape_ok(shape) ? SHAPES[shape].name : "Box"; }
const char *dgm_shape_sample(int shape, int style)
{
  if (!shape_ok(shape)) return "+--+";
  return style == DGM_STYLE_UNICODE ? SHAPES[shape].usample : SHAPES[shape].sample;
}

/* Pull a node's rectangle up to what its shape needs. */
static void fit_shape(dgm_shape_t *s)
{
  int minw, minh;
  if (s->kind != DGM_NODE) return;
  if (!shape_ok(s->shape)) s->shape = DGM_RECT;
  dgm_shape_min(s->shape, &minw, &minh);
  if (s->w < minw) s->w = minw;
  if (s->h < minh) s->h = minh;
  if (SHAPES[s->shape].odd_h && !(s->h & 1)) s->h++;
  if (SHAPES[s->shape].odd_w && !(s->w & 1)) s->w++;    /* keeps the arms symmetric */
  /* An actor is dropped at its smallest — a bare stick figure — and grows only
     as far as its caption needs, so a name is never silently clipped. Resizing
     from there is up to you. */
  if (s->shape == DGM_ACTOR) {
    int lw, lh;
    label_extent(s->label, &lw, &lh);
    if (s->w < lw) s->w = lw + ((lw & 1) ? 0 : 1);
  }
  /* A diamond's diagonals span (h-1)/2 columns on each side, so it can never be
     narrower than it is tall. */
  if (s->shape == DGM_DIAMOND && s->w < s->h) s->w = s->h;
  if (s->x + s->w > DGM_W) s->x = DGM_W - s->w;
  if (s->y + s->h > DGM_H) s->y = DGM_H - s->h;
  if (s->x < 0) s->x = 0;
  if (s->y < 0) s->y = 0;
}

int dgm_add_node(dgm_doc_t *d, int shape, int x, int y, int w, int h, const char *label)
{
  dgm_shape_t *s;
  if (d->n >= DGM_MAX_SHAPES) return -1;
  if (!shape_ok(shape)) shape = DGM_RECT;
  clampf(&x, 0, DGM_W - 1); clampf(&y, 0, DGM_H - 1);
  s = &d->s[d->n];
  memset(s, 0, sizeof *s);
  s->kind = DGM_NODE; s->shape = shape;
  s->x = x; s->y = y; s->w = w; s->h = h;
  s->from = s->to = -1;
  if (label) { strncpy(s->label, label, DGM_LABEL_MAX - 1); s->label[DGM_LABEL_MAX - 1] = '\0'; }
  fit_shape(s);
  return d->n++;
}

void dgm_set_shape(dgm_doc_t *d, int idx, int shape)
{
  if (idx < 0 || idx >= d->n || d->s[idx].kind != DGM_NODE || !shape_ok(shape)) return;
  d->s[idx].shape = shape;
  fit_shape(&d->s[idx]);
}

/* Width/height of a (possibly multi-line) label, in cells. */
static void label_extent(const char *label, int *w, int *h)
{
  int lw = 0, lh = 0, cur = 0;
  const char *p;
  for (p = label; *p; p++) {
    if (*p == '\n') { if (cur > lw) lw = cur; cur = 0; lh++; }
    else if (((unsigned char)*p & 0xc0) != 0x80) cur++;   /* count codepoints */
  }
  if (cur > lw) lw = cur;
  if (cur > 0 || lh == 0) lh++;
  *w = lw; *h = lh;
}

int dgm_add_text(dgm_doc_t *d, int x, int y, const char *text)
{
  dgm_shape_t *s;
  int lw, lh;
  if (d->n >= DGM_MAX_SHAPES) return -1;
  label_extent(text ? text : "", &lw, &lh);
  clampf(&x, 0, DGM_W - 1); clampf(&y, 0, DGM_H - 1);
  s = &d->s[d->n];
  memset(s, 0, sizeof *s);
  s->kind = DGM_TEXT; s->x = x; s->y = y; s->w = lw > 0 ? lw : 1; s->h = lh;
  s->from = s->to = -1;
  if (text) { strncpy(s->label, text, DGM_LABEL_MAX - 1); s->label[DGM_LABEL_MAX - 1] = '\0'; }
  return d->n++;
}

int dgm_find_conn(const dgm_doc_t *d, int a, int b)
{
  int i;
  if (a < 0 || b < 0) return -1;
  for (i = 0; i < d->n; i++) {
    if (d->s[i].kind != DGM_CONN) continue;
    if ((d->s[i].from == a && d->s[i].to == b) ||
        (d->s[i].from == b && d->s[i].to == a)) return i;
  }
  return -1;
}

int dgm_add_conn(dgm_doc_t *d, int from, int to)
{
  dgm_shape_t *s;
  int existing;
  if (d->n >= DGM_MAX_SHAPES) return -1;
  if (from < 0 || to < 0 || from >= d->n || to >= d->n || from == to) return -1;
  /* Anything with a rectangle can be an endpoint: every node shape, and a free
     text label too. Only a connector cannot end on a connector. */
  if (d->s[from].kind == DGM_CONN || d->s[to].kind == DGM_CONN) return -1;
  /* One link per pair, in either direction: a second one would take the same
     route, so it would be invisible on screen but real in the document. */
  existing = dgm_find_conn(d, from, to);
  if (existing >= 0) return existing;
  s = &d->s[d->n];
  memset(s, 0, sizeof *s);
  s->kind = DGM_CONN; s->from = from; s->to = to; s->arrow_to = 1;
  return d->n++;
}

int dgm_duplicate(dgm_doc_t *d, int idx)
{
  dgm_shape_t src;
  if (idx < 0 || idx >= d->n || d->n >= DGM_MAX_SHAPES) return -1;
  src = d->s[idx];
  if (src.kind == DGM_CONN) return -1;
  if (src.kind == DGM_NODE)
    return dgm_add_node(d, src.shape, src.x + 2, src.y + 1, src.w, src.h, src.label);
  return dgm_add_text(d, src.x + 2, src.y + 1, src.label);
}

/* Erase one slot and renumber the connector endpoints that pointed past it. */
static void erase_slot(dgm_doc_t *d, int idx)
{
  int i;
  memmove(&d->s[idx], &d->s[idx + 1], (size_t)(d->n - idx - 1) * sizeof d->s[0]);
  d->n--;
  for (i = 0; i < d->n; i++) {
    if (d->s[i].kind != DGM_CONN) continue;
    if (d->s[i].from > idx) d->s[i].from--;
    if (d->s[i].to   > idx) d->s[i].to--;
  }
}

void dgm_delete(dgm_doc_t *d, int idx)
{
  int i;
  if (idx < 0 || idx >= d->n) return;
  if (d->s[idx].kind != DGM_CONN)     /* an object takes its connectors with it */
    for (i = d->n - 1; i >= 0; i--)
      if (d->s[i].kind == DGM_CONN && (d->s[i].from == idx || d->s[i].to == idx)) {
        erase_slot(d, i);
        if (i < idx) idx--;
      }
  erase_slot(d, idx);
}

void dgm_set_label(dgm_doc_t *d, int idx, const char *label)
{
  dgm_shape_t *s;
  if (idx < 0 || idx >= d->n) return;
  s = &d->s[idx];
  strncpy(s->label, label ? label : "", DGM_LABEL_MAX - 1);
  s->label[DGM_LABEL_MAX - 1] = '\0';
  if (s->kind == DGM_TEXT) {
    int lw, lh;
    label_extent(s->label, &lw, &lh);
    s->w = lw > 0 ? lw : 1; s->h = lh;
  }
  fit_shape(s);          /* an actor's width follows its caption */
}

void dgm_move(dgm_doc_t *d, int idx, int dx, int dy)
{
  dgm_shape_t *s;
  if (idx < 0 || idx >= d->n) return;
  s = &d->s[idx];
  if (s->kind == DGM_CONN) return;
  s->x += dx; s->y += dy;
  clampf(&s->x, 0, DGM_W - s->w);
  clampf(&s->y, 0, DGM_H - s->h);
}

void dgm_resize(dgm_doc_t *d, int idx, int dw, int dh)
{
  dgm_shape_t *s;
  if (idx < 0 || idx >= d->n) return;
  s = &d->s[idx];
  if (s->kind != DGM_NODE) return;
  s->w += dw; s->h += dh;
  clampf(&s->w, 1, DGM_W - s->x);
  clampf(&s->h, 1, DGM_H - s->y);
  fit_shape(s);
}

void dgm_place(dgm_doc_t *d, int idx, int x, int y, int w, int h)
{
  dgm_shape_t *s;
  if (idx < 0 || idx >= d->n) return;
  s = &d->s[idx];
  if (s->kind == DGM_CONN) return;
  if (s->kind == DGM_NODE) {
    clampf(&w, 1, DGM_W); clampf(&h, 1, DGM_H);
    s->w = w; s->h = h;
  }
  s->x = x; s->y = y;
  clampf(&s->x, 0, DGM_W - s->w);
  clampf(&s->y, 0, DGM_H - s->h);
  fit_shape(s);
}

/* ── hit testing ────────────────────────────────────────────────────────────*/

int dgm_hit(const dgm_doc_t *d, int x, int y)
{
  int i, hit = -1;
  for (i = 0; i < d->n; i++) {            /* later shapes draw on top */
    const dgm_shape_t *s = &d->s[i];
    if (s->kind == DGM_CONN) continue;
    if (x >= s->x && x < s->x + s->w && y >= s->y && y < s->y + s->h) hit = i;
  }
  return hit;
}

int dgm_hit_conn(const dgm_doc_t *d, int x, int y)
{
  int i, px[4], py[4], n, k;
  for (i = d->n - 1; i >= 0; i--) {
    if (d->s[i].kind != DGM_CONN) continue;
    n = dgm_route(d, i, px, py, 4);
    for (k = 0; k + 1 < n; k++) {
      int lo, hi;
      if (py[k] == py[k + 1] && y == py[k]) {
        lo = px[k] < px[k + 1] ? px[k] : px[k + 1];
        hi = px[k] < px[k + 1] ? px[k + 1] : px[k];
        if (x >= lo && x <= hi) return i;
      } else if (px[k] == px[k + 1] && x == px[k]) {
        lo = py[k] < py[k + 1] ? py[k] : py[k + 1];
        hi = py[k] < py[k + 1] ? py[k + 1] : py[k];
        if (y >= lo && y <= hi) return i;
      }
    }
  }
  return -1;
}

int dgm_hit_part(const dgm_doc_t *d, int idx, int x, int y)
{
  const dgm_shape_t *s;
  int on_edge;
  if (idx < 0 || idx >= d->n) return DGM_HIT_NONE;
  s = &d->s[idx];
  if (x < s->x || x >= s->x + s->w || y < s->y || y >= s->y + s->h) return DGM_HIT_NONE;
  if (s->kind != DGM_NODE) return DGM_HIT_INSIDE;
  if (x == s->x + s->w - 1 && y == s->y + s->h - 1) return DGM_HIT_CORNER;
  on_edge = (x == s->x || x == s->x + s->w - 1 || y == s->y || y == s->y + s->h - 1);
  return on_edge ? DGM_HIT_BORDER : DGM_HIT_INSIDE;
}

void dgm_extent(const dgm_doc_t *d, int *w, int *h)
{
  int i, x, y, mw = 0, mh = 0;
  for (i = 0; i < d->n; i++) {
    const dgm_shape_t *s = &d->s[i];
    if (s->kind == DGM_CONN) continue;
    if (s->x + s->w > mw) mw = s->x + s->w;
    if (s->y + s->h > mh) mh = s->y + s->h;
  }
  if (d->has_raw)
    for (y = 0; y < DGM_H; y++)
      for (x = 0; x < DGM_W; x++)
        if (d->raw[y][x] && d->raw[y][x] != ' ') {
          if (x + 1 > mw) mw = x + 1;
          if (y + 1 > mh) mh = y + 1;
        }
  *w = mw; *h = mh;
}

/* ── rendering ──────────────────────────────────────────────────────────────*/

static void gput(dgm_grid_t *g, int x, int y, uint32_t ch)
{
  if (x < 0 || y < 0 || x >= g->w || y >= g->h) return;
  g->cell[y * g->w + x] = ch;
}

static uint32_t gget(const dgm_grid_t *g, int x, int y)
{
  if (x < 0 || y < 0 || x >= g->w || y >= g->h) return ' ';
  return g->cell[y * g->w + x];
}

/* One label line: the bytes between `from` and the next '\n' (or the end). */
static const char *label_line(const char *p, char *out, size_t outsz)
{
  size_t n = 0;
  while (*p && *p != '\n') { if (n + 1 < outsz) out[n++] = *p; p++; }
  out[n] = '\0';
  return *p == '\n' ? p + 1 : p;
}

static int cp_count(const char *s)
{
  int n = 0;
  for (; *s; s++) if (((unsigned char)*s & 0xc0) != 0x80) n++;
  return n;
}

/* Put a UTF-8 string starting at (x, y), one codepoint per cell. */
static void gput_str(dgm_grid_t *g, int x, int y, const char *s)
{
  while (*s) {
    uint32_t cp;
    s += utf8_decode(s, &cp);
    gput(g, x++, y, cp);
  }
}

/* Where a label may be written on row `row` of node `s`: the inclusive column
   span [l, r], or 0 if that row is all outline. This is what lets a diamond's
   text follow its taper and an actor's caption sit under the figure. */
static int label_span(const dgm_shape_t *s, int row, int *l, int *r)
{
  int rel = row - s->y;
  if (rel <= 0 || rel >= s->h - 1) {
    /* Only the actor writes on its bottom row — the figure is above it. */
    if (s->shape == DGM_ACTOR && rel == s->h - 1) { *l = s->x; *r = s->x + s->w - 1; return 1; }
    return 0;
  }
  switch (s->shape) {
  case DGM_DIAMOND: {
    int mid = (s->h - 1) / 2;
    int inset = rel <= mid ? mid - rel : rel - mid;      /* distance from the waist */
    *l = s->x + inset + 1;
    *r = s->x + s->w - 1 - inset - 1;
    return *r >= *l;
  }
  case DGM_CIRCLE:
    if (rel == 1 || rel == s->h - 2) { *l = s->x + 2; *r = s->x + s->w - 3; }
    else                             { *l = s->x + 1; *r = s->x + s->w - 2; }
    return *r >= *l;
  case DGM_CYLINDER:
    if (rel == 1) return 0;                              /* the disc line */
    *l = s->x + 1; *r = s->x + s->w - 2;
    return *r >= *l;
  case DGM_ACTOR:
    return 0;                                            /* handled above */
  default:
    *l = s->x + 1; *r = s->x + s->w - 2;
    return *r >= *l;
  }
}

/* Clip a label line to `keep` cells, on a codepoint boundary. */
static void clip_cells(char *line, int keep)
{
  int n = 0;
  char *q = line;
  if (keep < 0) keep = 0;
  while (*q && n < keep) {
    q++;
    while (((unsigned char)*q & 0xc0) == 0x80) q++;
    n++;
  }
  *q = '\0';
}

/* Centre the label over the rows of `s` that can hold text. */
static void draw_label(dgm_grid_t *g, const dgm_shape_t *s)
{
  int rows[DGM_H], nrows = 0, nlines = 0, i, row;
  const char *p;
  char line[DGM_LABEL_MAX];

  if (!s->label[0]) return;
  for (row = s->y; row < s->y + s->h; row++) {
    int l, r;
    if (label_span(s, row, &l, &r) && nrows < DGM_H) rows[nrows++] = row;
  }
  if (nrows == 0) return;

  for (p = s->label; ; ) {                               /* count label lines */
    p = label_line(p, line, sizeof line);
    nlines++;
    if (!*p) break;
  }
  if (nlines > nrows) nlines = nrows;

  for (p = s->label, i = 0; i < nlines; i++) {
    int l, r, lw;
    row = rows[(nrows - nlines) / 2 + i];                /* vertically centred */
    p = label_line(p, line, sizeof line);
    label_span(s, row, &l, &r);
    lw = cp_count(line);
    if (lw > r - l + 1) { clip_cells(line, r - l + 1); lw = cp_count(line); }
    gput_str(g, l + (r - l + 1 - lw) / 2, row, line);    /* horizontally centred */
  }
}

/* Blank the cells a node covers, so it sits *on top of* whatever is behind it
   rather than tangling with it — the same opacity draw.io gives a shape. */
static void clear_node(dgm_grid_t *g, const dgm_shape_t *s)
{
  int x, y;
  for (y = s->y; y < s->y + s->h; y++)
    for (x = s->x; x < s->x + s->w; x++)
      gput(g, x, y, ' ');
}

static void draw_rect_like(const dgm_doc_t *d, dgm_grid_t *g, const dgm_shape_t *s)
{
  glyphs_t gl = style_glyphs(d->style, s->shape != DGM_RECT);
  int x, y;
  for (x = s->x + 1; x < s->x + s->w - 1; x++) {
    gput(g, x, s->y, gl.hz);
    gput(g, x, s->y + s->h - 1, gl.hz);
  }
  for (y = s->y + 1; y < s->y + s->h - 1; y++) {
    gput(g, s->x, y, gl.vt);
    gput(g, s->x + s->w - 1, y, gl.vt);
  }
  gput(g, s->x, s->y, gl.tl);
  gput(g, s->x + s->w - 1, s->y, gl.tr);
  gput(g, s->x, s->y + s->h - 1, gl.bl);
  gput(g, s->x + s->w - 1, s->y + s->h - 1, gl.br);

  /* A cylinder is a rounded box with the rim of its top disc drawn inside it. */
  if (s->shape == DGM_CYLINDER) {
    glyphs_t r = style_glyphs(d->style, 1);
    for (x = s->x + 2; x < s->x + s->w - 2; x++) gput(g, x, s->y + 1, r.hz);
    gput(g, s->x + 1, s->y + 1, r.bl);
    gput(g, s->x + s->w - 2, s->y + 1, r.br);
  }
}

/* (   ) — a rounded cap top and bottom, parentheses down the sides. A cloud is
   the same body with a wavy cap, which is both fluffier to look at and enough
   for the recogniser to tell the two apart. */
static void draw_ellipse(const dgm_doc_t *d, dgm_grid_t *g, const dgm_shape_t *s)
{
  glyphs_t gl = style_glyphs(d->style, 1);
  uint32_t cap = s->shape == DGM_CLOUD ? '~' : gl.hz;
  int x, y;
  for (x = s->x + 2; x < s->x + s->w - 2; x++) {
    gput(g, x, s->y, cap);
    gput(g, x, s->y + s->h - 1, cap);
  }
  gput(g, s->x + 1, s->y, gl.tl);
  gput(g, s->x + s->w - 2, s->y, gl.tr);
  gput(g, s->x + 1, s->y + s->h - 1, gl.bl);
  gput(g, s->x + s->w - 2, s->y + s->h - 1, gl.br);
  for (y = s->y + 1; y < s->y + s->h - 1; y++) {
    gput(g, s->x, y, '(');
    gput(g, s->x + s->w - 1, y, ')');
  }
}

/* A round node. Terminal cells are about twice as tall as they are wide, so a
   circle is drawn wide: a short cap, diagonal shoulders, straight sides, then
   the same again mirrored. */
static void draw_circle(const dgm_doc_t *d, dgm_grid_t *g, const dgm_shape_t *s)
{
  glyphs_t gl = style_glyphs(DGM_STYLE_ASCII, 1);        /* ditto: ASCII shoulders */
  (void)d;
  int x = s->x, y = s->y, w = s->w, h = s->h, i;

  for (i = x + 3; i < x + w - 3; i++) {
    gput(g, i, y, gl.hz);
    gput(g, i, y + h - 1, gl.hz);
  }
  gput(g, x + 2, y, gl.tl);
  gput(g, x + w - 3, y, gl.tr);
  gput(g, x + 2, y + h - 1, gl.bl);
  gput(g, x + w - 3, y + h - 1, gl.br);
  gput(g, x + 1, y + 1, '/');
  gput(g, x + w - 2, y + 1, '\\');
  gput(g, x + 1, y + h - 2, '\\');
  gput(g, x + w - 2, y + h - 2, '/');
  for (i = y + 2; i < y + h - 2; i++) {
    gput(g, x, i, gl.vt);
    gput(g, x + w - 1, i, gl.vt);
  }
}

/* /----\  <    >  \----/ */
static void draw_hexagon(const dgm_doc_t *d, dgm_grid_t *g, const dgm_shape_t *s)
{
  glyphs_t gl = style_glyphs(DGM_STYLE_ASCII, 0);        /* its corners are ASCII slashes */
  (void)d;
  int x, y;
  for (x = s->x + 1; x < s->x + s->w - 1; x++) {
    gput(g, x, s->y, gl.hz);
    gput(g, x, s->y + s->h - 1, gl.hz);
  }
  gput(g, s->x, s->y, '/');
  gput(g, s->x + s->w - 1, s->y, '\\');
  gput(g, s->x, s->y + s->h - 1, '\\');
  gput(g, s->x + s->w - 1, s->y + s->h - 1, '/');
  for (y = s->y + 1; y < s->y + s->h - 1; y++) {
    gput(g, s->x, y, '<');
    gput(g, s->x + s->w - 1, y, '>');
  }
}

/* A lozenge with exact 45° diagonals: the waist vertices sit at mid height, and
   a wider diamond simply grows a flat top and bottom between its corners. */
static void draw_diamond(const dgm_doc_t *d, dgm_grid_t *g, const dgm_shape_t *s)
{
  glyphs_t gl = style_glyphs(DGM_STYLE_ASCII, 0);        /* ditto: 45° ASCII diagonals */
  (void)d;
  int mid = (s->h - 1) / 2, k, x;
  int tl = s->x + mid, tr = s->x + s->w - 1 - mid;

  for (x = tl + 1; x < tr; x++) {                        /* flat top and bottom */
    gput(g, x, s->y, gl.hz);
    gput(g, x, s->y + s->h - 1, gl.hz);
  }
  gput(g, tl, s->y, '+'); gput(g, tr, s->y, '+');
  gput(g, tl, s->y + s->h - 1, '+'); gput(g, tr, s->y + s->h - 1, '+');
  gput(g, s->x, s->y + mid, '+');                        /* the two waist points */
  gput(g, s->x + s->w - 1, s->y + mid, '+');
  for (k = 1; k < mid; k++) {                            /* the four diagonals */
    gput(g, tl - k,          s->y + k,              '/');
    gput(g, tr + k,          s->y + k,              '\\');
    gput(g, tl - k,          s->y + s->h - 1 - k,   '\\');
    gput(g, tr + k,          s->y + s->h - 1 - k,   '/');
  }
}

/* A stick figure that grows with its rectangle: the arms span the full width,
   the body fills the rows between arms and legs, and the caption sits on the
   last row. Everything about the rectangle is therefore visible in the art,
   which is what lets the recogniser hand back the same rectangle — and what
   makes resizing an actor mean something.

       O
    /--|--\
       |
      / \
     caption   */
static void draw_actor(dgm_grid_t *g, const dgm_shape_t *s)
{
  int cx = s->x + s->w / 2, legs = s->y + s->h - 2, i;

  gput(g, cx, s->y, 'O');                                /* head */
  gput(g, s->x, s->y + 1, '/');                          /* arms */
  gput(g, s->x + s->w - 1, s->y + 1, '\\');
  for (i = s->x + 1; i < s->x + s->w - 1; i++)
    if (i != cx) gput(g, i, s->y + 1, '-');
  gput(g, cx, s->y + 1, '|');
  for (i = s->y + 2; i < legs; i++) gput(g, cx, i, '|');  /* body */
  gput(g, cx - 1, legs, '/');                            /* legs */
  gput(g, cx + 1, legs, '\\');
}

static void draw_node(const dgm_doc_t *d, dgm_grid_t *g, const dgm_shape_t *s)
{
  clear_node(g, s);
  switch (s->shape) {
  case DGM_ELLIPSE:
  case DGM_CLOUD:    draw_ellipse(d, g, s); break;
  case DGM_CIRCLE:   draw_circle(d, g, s);  break;
  case DGM_HEXAGON:  draw_hexagon(d, g, s); break;
  case DGM_DIAMOND:  draw_diamond(d, g, s); break;
  case DGM_ACTOR:    draw_actor(g, s);      break;
  default:           draw_rect_like(d, g, s); break;
  }
  draw_label(g, s);
}

static void draw_text(dgm_grid_t *g, const dgm_shape_t *s)
{
  const char *p = s->label;
  int row = 0;
  char line[DGM_LABEL_MAX];
  if (!p[0]) { gput(g, s->x, s->y, ' '); return; }
  for (;;) {
    p = label_line(p, line, sizeof line);
    gput_str(g, s->x, s->y + row++, line);
    if (!*p) break;
  }
}

int dgm_route(const dgm_doc_t *d, int conn, int *px, int *py, int max)
{
  const dgm_shape_t *c, *a, *b;
  int ax, ay, bx, by, gapx = -1, gapy = -1, n = 0, horizontal;

  if (conn < 0 || conn >= d->n || max < 2) return 0;
  c = &d->s[conn];
  if (c->kind != DGM_CONN) return 0;
  if (c->from < 0 || c->to < 0 || c->from >= d->n || c->to >= d->n) return 0;
  a = &d->s[c->from]; b = &d->s[c->to];

  if (b->x > a->x + a->w - 1)      gapx = b->x - (a->x + a->w);
  else if (a->x > b->x + b->w - 1) gapx = a->x - (b->x + b->w);
  if (b->y > a->y + a->h - 1)      gapy = b->y - (a->y + a->h);
  else if (a->y > b->y + b->h - 1) gapy = a->y - (b->y + b->h);

  horizontal = (gapx >= 0 && (gapy < 0 || gapx >= gapy)) || (gapx < 0 && gapy < 0);

  if (horizontal) {
    int left_to_right = b->x >= a->x;
    ay = a->y + a->h / 2; by = b->y + b->h / 2;
    ax = left_to_right ? a->x + a->w : a->x - 1;
    bx = left_to_right ? b->x - 1     : b->x + b->w;
    px[n] = ax; py[n] = ay; n++;
    if (ay != by && n + 2 < max) {
      int mx = (ax + bx) / 2;
      px[n] = mx; py[n] = ay; n++;
      px[n] = mx; py[n] = by; n++;
    }
    px[n] = bx; py[n] = by; n++;
  } else {
    int top_to_bottom = b->y >= a->y;
    ax = a->x + a->w / 2; bx = b->x + b->w / 2;
    ay = top_to_bottom ? a->y + a->h : a->y - 1;
    by = top_to_bottom ? b->y - 1     : b->y + b->h;
    px[n] = ax; py[n] = ay; n++;
    if (ax != bx && n + 2 < max) {
      int my = (ay + by) / 2;
      px[n] = ax; py[n] = my; n++;
      px[n] = bx; py[n] = my; n++;
    }
    px[n] = bx; py[n] = by; n++;
  }
  return n;
}

static uint32_t arrow_glyph(int dx, int dy)
{
  if (dx > 0) return '>';
  if (dx < 0) return '<';
  if (dy > 0) return 'v';
  return '^';
}

static void draw_conn(const dgm_doc_t *d, dgm_grid_t *g, int conn)
{
  int px[4], py[4], n, i;
  glyphs_t gl = style_glyphs(d->style, 0);
  const dgm_shape_t *c = &d->s[conn];

  n = dgm_route(d, conn, px, py, 4);
  if (n < 2) return;

  for (i = 0; i + 1 < n; i++) {                          /* the straight runs */
    int x = px[i], y = py[i], tx = px[i + 1], ty = py[i + 1];
    int sx = (tx > x) - (tx < x), sy = (ty > y) - (ty < y);
    while (x != tx || y != ty) {
      gput(g, x, y, sx ? gl.hz : gl.vt);
      x += sx; y += sy;
    }
    gput(g, tx, ty, sx ? gl.hz : gl.vt);
  }
  for (i = 1; i + 1 < n; i++) {                          /* the turns */
    int idx = (px[i] > px[i - 1]) - (px[i] < px[i - 1]);
    int idy = (py[i] > py[i - 1]) - (py[i] < py[i - 1]);
    int odx = (px[i + 1] > px[i]) - (px[i + 1] < px[i]);
    int ody = (py[i + 1] > py[i]) - (py[i + 1] < py[i]);
    gput(g, px[i], py[i], elbow(d->style, -idx, -idy, odx, ody));
  }
  if (c->arrow_to) {
    int dx = (px[n - 1] > px[n - 2]) - (px[n - 1] < px[n - 2]);
    int dy = (py[n - 1] > py[n - 2]) - (py[n - 1] < py[n - 2]);
    gput(g, px[n - 1], py[n - 1], arrow_glyph(dx, dy));
  }
  if (c->arrow_from) {
    int dx = (px[0] > px[1]) - (px[0] < px[1]);
    int dy = (py[0] > py[1]) - (py[0] < py[1]);
    gput(g, px[0], py[0], arrow_glyph(dx, dy));
  }
}

void dgm_render(const dgm_doc_t *d, dgm_grid_t *g)
{
  int i, x, y;

  for (i = 0; i < g->w * g->h; i++) g->cell[i] = ' ';

  if (d->has_raw)                              /* whatever the recogniser kept */
    for (y = 0; y < DGM_H && y < g->h; y++)
      for (x = 0; x < DGM_W && x < g->w; x++)
        if (d->raw[y][x] && d->raw[y][x] != ' ') gput(g, x, y, d->raw[y][x]);

  for (i = 0; i < d->n; i++) if (d->s[i].kind == DGM_CONN) draw_conn(d, g, i);
  for (i = 0; i < d->n; i++) if (d->s[i].kind == DGM_NODE) draw_node(d, g, &d->s[i]);
  for (i = 0; i < d->n; i++) if (d->s[i].kind == DGM_TEXT) draw_text(g, &d->s[i]);
}

int dgm_flatten(dgm_doc_t *d, int idx, dgm_cell_t *undo, int max, int *nundo)
{
  dgm_doc_t *with, *without;
  dgm_grid_t ga, gb;
  int x, y;

  if (nundo) *nundo = 0;
  if (idx < 0 || idx >= d->n) return -1;

  /* Draw the document twice — as it is, and with this object gone — and keep
     the cells that differ. That is exactly what the object was contributing,
     links included, so the picture is untouched by the swap. */
  with = dgm_new();
  without = dgm_new();
  ga.w = gb.w = DGM_W; ga.h = gb.h = DGM_H;
  ga.cell = malloc((size_t)ga.w * ga.h * sizeof *ga.cell);
  gb.cell = malloc((size_t)gb.w * gb.h * sizeof *gb.cell);
  if (!with || !without || !ga.cell || !gb.cell) {
    free(ga.cell); free(gb.cell); dgm_free(with); dgm_free(without);
    return -1;
  }
  with->style = without->style = d->style;
  memcpy(with->s, d->s, (size_t)d->n * sizeof d->s[0]);
  memcpy(without->s, d->s, (size_t)d->n * sizeof d->s[0]);
  with->n = without->n = d->n;
  dgm_delete(without, idx);
  dgm_render(with, &ga);
  dgm_render(without, &gb);

  for (y = 0; y < DGM_H; y++)
    for (x = 0; x < DGM_W; x++) {
      uint32_t a = ga.cell[y * ga.w + x], b = gb.cell[y * gb.w + x];
      if (a == b || a == ' ' || !a) continue;
      if (undo && nundo && *nundo < max) {
        undo[*nundo].x = x; undo[*nundo].y = y; undo[*nundo].ch = d->raw[y][x];
        (*nundo)++;
      }
      d->raw[y][x] = a;
      d->has_raw = 1;
    }

  free(ga.cell); free(gb.cell);
  dgm_free(with); dgm_free(without);
  dgm_delete(d, idx);
  return 0;
}

uint32_t dgm_raw_get(const dgm_doc_t *d, int x, int y)
{
  if (x < 0 || y < 0 || x >= DGM_W || y >= DGM_H) return 0;
  return d->raw[y][x];
}

uint32_t dgm_raw_put(dgm_doc_t *d, int x, int y, uint32_t ch)
{
  uint32_t was;
  if (x < 0 || y < 0 || x >= DGM_W || y >= DGM_H) return 0;
  was = d->raw[y][x];
  d->raw[y][x] = ch;
  if (ch) d->has_raw = 1;
  return was;
}

/* ── ASCII out ──────────────────────────────────────────────────────────────*/

int dgm_to_text(const dgm_doc_t *d, char *out, size_t outsz)
{
  dgm_grid_t g;
  size_t used = 0;
  int x, y, last_row = -1, rc = 0;

  g.w = DGM_W; g.h = DGM_H;
  g.cell = malloc((size_t)g.w * g.h * sizeof *g.cell);
  if (!g.cell) return -1;
  dgm_render(d, &g);

  for (y = 0; y < g.h; y++)                              /* drop trailing blank rows */
    for (x = 0; x < g.w; x++)
      if (gget(&g, x, y) != ' ') { last_row = y; break; }

  for (y = 0; y <= last_row; y++) {
    int last_col = -1;
    for (x = 0; x < g.w; x++) if (gget(&g, x, y) != ' ') last_col = x;
    for (x = 0; x <= last_col; x++) {
      char buf[4];
      int nb = utf8_encode(gget(&g, x, y), buf), k;
      if (used + (size_t)nb + 2 > outsz) { rc = -1; goto done; }
      for (k = 0; k < nb; k++) out[used++] = buf[k];
    }
    if (used + 2 > outsz) { rc = -1; goto done; }
    out[used++] = '\n';
  }
  if (used < outsz) out[used] = '\0'; else rc = -1;
done:
  free(g.cell);
  return rc == 0 ? (int)used : -1;
}

/* ── ASCII in: the recogniser ───────────────────────────────────────────────
 *
 * Four passes over the art, each consuming the cells it claims:
 *   1. rectangles      → boxes (their interior text becomes the label)
 *   2. runs with a letter or digit in them → free text
 *   3. connected runs of line characters touching two boxes → connectors
 *   4. everything still unclaimed → the raw layer, drawn and saved verbatim
 * A pass never claims a cell an earlier pass took, so the worst case is a
 * diagram that comes back as one big raw layer — still pixel-identical on
 * save, just not draggable. */

typedef struct {
  uint32_t      *c;
  unsigned char *used;
  int           *owner;      /* index of the box occupying a cell, else -1 */
  int            w, h;
} pgrid_t;

static uint32_t pget(const pgrid_t *p, int x, int y)
{
  if (x < 0 || y < 0 || x >= p->w || y >= p->h) return ' ';
  return p->c[y * p->w + x];
}
static int pused(const pgrid_t *p, int x, int y)
{
  if (x < 0 || y < 0 || x >= p->w || y >= p->h) return 1;
  return p->used[y * p->w + x];
}
static void puse(pgrid_t *p, int x, int y, int owner)
{
  if (x < 0 || y < 0 || x >= p->w || y >= p->h) return;
  p->used[y * p->w + x] = 1;
  if (owner >= 0) p->owner[y * p->w + x] = owner;
}
static int powner(const pgrid_t *p, int x, int y)
{
  if (x < 0 || y < 0 || x >= p->w || y >= p->h) return -1;
  return p->owner[y * p->w + x];
}

static int is_corner_ch(uint32_t c)
{
  return c == '+' || c == '.' || c == '\'' || c == '`' ||
         c == 0x250c || c == 0x2510 || c == 0x2514 || c == 0x2518 ||
         c == 0x256d || c == 0x256e || c == 0x2570 || c == 0x256f ||
         c == 0x253c;
}
static int is_hz_ch(uint32_t c) { return c == '-' || c == '=' || c == 0x2500 || is_corner_ch(c); }
static int is_vt_ch(uint32_t c) { return c == '|' || c == 0x2502 || is_corner_ch(c); }
static int is_arrow_ch(uint32_t c)
{
  return c == '>' || c == '<' || c == '^' || c == 'v' || c == 'V' ||
         c == 0x25b6 || c == 0x25c0 || c == 0x25b2 || c == 0x25bc;
}
static int is_line_ch(uint32_t c)
{
  return is_hz_ch(c) || is_vt_ch(c) || is_arrow_ch(c);
}
/* Distinguishes a label from a line: any letter or digit makes it text. */
static int is_wordy(uint32_t c)
{
  return c > 0x7f ? !is_line_ch(c) : isalnum((int)c) != 0;
}

/* Try to read a rectangle whose top-left corner is (x0, y0). */
static int find_box(const pgrid_t *p, int x0, int y0, int *out_w, int *out_h)
{
  int x1, y1, i, j;
  for (x1 = x0 + 2; x1 < p->w; x1++) {
    if (!is_hz_ch(pget(p, x1, y0))) break;              /* top edge broke */
    if (!is_corner_ch(pget(p, x1, y0))) continue;
    for (y1 = y0 + 2; y1 < p->h; y1++) {
      int ok = 1;
      if (!is_vt_ch(pget(p, x0, y1)) || !is_vt_ch(pget(p, x1, y1))) break;
      if (!is_corner_ch(pget(p, x0, y1)) || !is_corner_ch(pget(p, x1, y1))) continue;
      for (i = x0 + 1; i < x1 && ok; i++) if (!is_hz_ch(pget(p, i, y1))) ok = 0;
      for (j = y0 + 1; j < y1 && ok; j++)
        if (!is_vt_ch(pget(p, x0, j)) || !is_vt_ch(pget(p, x1, j))) ok = 0;
      if (ok) { *out_w = x1 - x0 + 1; *out_h = y1 - y0 + 1; return 1; }
    }
  }
  return 0;
}

/* Append a codepoint to a UTF-8 buffer, keeping it NUL-terminated. */
static void str_append(char *buf, size_t bufsz, size_t *len, uint32_t cp)
{
  char tmp[4];
  int n = utf8_encode(cp, tmp), i;
  if (*len + (size_t)n + 1 >= bufsz) return;
  for (i = 0; i < n; i++) buf[(*len)++] = tmp[i];
  buf[*len] = '\0';
}

static void trim_trailing(char *s)
{
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
}

/* Read a node's label back out of the art, using the very spans the renderer
   writes into — so a diamond's tapered rows and an actor's caption come back
   the same way they went in. Lines are trimmed and blank top/bottom lines
   dropped, which is what makes save → open → save idempotent. */
static void extract_label(const pgrid_t *p, const dgm_shape_t *node, char *out, size_t outsz)
{
  char rows[24][DGM_LABEL_MAX];
  size_t len = 0;
  int first = -1, last = -1, i, row, nrows = 0;

  out[0] = '\0';
  for (row = node->y; row < node->y + node->h && nrows < 24; row++) {
    size_t rl = 0;
    int l, r, c, leading = 1;
    if (!label_span(node, row, &l, &r)) continue;
    rows[nrows][0] = '\0';
    for (c = l; c <= r; c++) {
      uint32_t ch = pget(p, c, row);
      if (leading && ch == ' ') continue;
      leading = 0;
      str_append(rows[nrows], DGM_LABEL_MAX, &rl, ch);
    }
    trim_trailing(rows[nrows]);
    if (rows[nrows][0]) { if (first < 0) first = nrows; last = nrows; }
    nrows++;
  }
  if (first < 0) return;
  for (i = first; i <= last; i++) {
    size_t need = strlen(rows[i]) + (i > first ? 1 : 0);
    if (len + need + 1 >= outsz) break;
    if (i > first) out[len++] = '\n';
    memcpy(out + len, rows[i], strlen(rows[i]));
    len += strlen(rows[i]);
    out[len] = '\0';
  }
}

/* ── shape recognisers ───────────────────────────────────────────────────────
   Each one is seeded on a character only that shape draws, and each verifies
   the whole outline before claiming anything: a near-miss falls through to the
   next recogniser and, failing all of them, to the raw layer. */

static int is_rounded_corner(uint32_t c)
{
  return c == '.' || c == '\'' || c == '`' || c == 0x256d || c == 0x256e ||
         c == 0x2570 || c == 0x256f;
}

/* `.-----.` / `|'---'|` — a rounded box whose first interior row is the rim of
   the top disc. */
static int is_cylinder(const pgrid_t *p, int x, int y, int w, int h)
{
  int c;
  if (h < 4 || w < 5) return 0;
  if (!is_rounded_corner(pget(p, x, y))) return 0;
  if (!is_rounded_corner(pget(p, x + 1, y + 1)) ||
      !is_rounded_corner(pget(p, x + w - 2, y + 1))) return 0;
  for (c = x + 2; c < x + w - 2; c++)
    if (!is_hz_ch(pget(p, c, y + 1))) return 0;
  return 1;
}

/* `(` down the left, `)` down the right, rounded caps above and below. */
static int find_ellipse(const pgrid_t *p, int x, int y, int *ox, int *oy, int *ow, int *oh,
                        int *wavy)
{
  int x2, c, r;
  *wavy = 0;
  if (pget(p, x, y) != '(') return 0;
  if (!is_rounded_corner(pget(p, x + 1, y - 1))) return 0;
  for (x2 = x + 3; x2 < p->w; x2++) {                    /* find the cap's far corner */
    uint32_t cap = pget(p, x2 - 1, y - 1);
    if (is_rounded_corner(cap)) break;
    if (cap == '~') { *wavy = 1; continue; }
    if (!is_hz_ch(cap)) return 0;
  }
  if (x2 >= p->w || pget(p, x2, y) != ')') return 0;
  for (c = x + 2; c < x2 - 1; c++) {
    uint32_t cap = pget(p, c, y - 1);
    if (cap == '~') { *wavy = 1; continue; }
    if (!is_hz_ch(cap)) return 0;
  }
  for (r = y; r < p->h; r++) {                           /* how far the sides run */
    if (pget(p, r == y ? x : x, r) == '(' && pget(p, x2, r) == ')') continue;
    break;
  }
  if (r - y < 1) return 0;
  if (!is_rounded_corner(pget(p, x + 1, r)) || !is_rounded_corner(pget(p, x2 - 1, r))) return 0;
  for (c = x + 2; c < x2 - 1; c++) {
    uint32_t cap = pget(p, c, r);
    if (cap == '~') { *wavy = 1; continue; }
    if (!is_hz_ch(cap)) return 0;
  }
  *ox = x; *oy = y - 1; *ow = x2 - x + 1; *oh = r - y + 2;
  return 1;
}

/* A round node: `.--.` cap, `/` `\` shoulders, straight sides, mirrored below.
   Seeded on the top-left shoulder, which only this shape draws. */
static int find_circle(const pgrid_t *p, int sx, int sy, int *ox, int *oy, int *ow, int *oh)
{
  int x = sx - 1, y = sy - 1, c, r, w = 0, h;
  if (pget(p, sx, sy) != '/') return 0;
  if (!is_rounded_corner(pget(p, sx + 1, y))) return 0;      /* the cap starts above-right */
  for (c = sx + 2; c < p->w; c++) {                          /* … and ends at its far corner */
    uint32_t ch = pget(p, c, y);
    if (is_rounded_corner(ch)) { w = c - x + 3; break; }
    if (!is_hz_ch(ch)) return 0;
  }
  if (w < 7) return 0;
  if (pget(p, x + w - 2, y + 1) != '\\') return 0;            /* the other shoulder */
  if (!is_vt_ch(pget(p, x, y + 2)) || !is_vt_ch(pget(p, x + w - 1, y + 2))) return 0;
  for (r = y + 2; r < p->h; r++) {                           /* how far the sides run */
    if (is_vt_ch(pget(p, x, r)) && is_vt_ch(pget(p, x + w - 1, r))) continue;
    break;
  }
  if (pget(p, x + 1, r) != '\\' || pget(p, x + w - 2, r) != '/') return 0;
  if (!is_rounded_corner(pget(p, x + 2, r + 1)) ||
      !is_rounded_corner(pget(p, x + w - 3, r + 1))) return 0;
  for (c = x + 3; c < x + w - 3; c++)
    if (!is_hz_ch(pget(p, c, r + 1))) return 0;
  h = r + 1 - y + 1;
  if (h < 5) return 0;
  *ox = x; *oy = y; *ow = w; *oh = h;
  return 1;
}

/* `/---\` over `<   >` over `\---/`. */
static int find_hexagon(const pgrid_t *p, int x, int y, int *ow, int *oh)
{
  int x2, c, r;
  if (pget(p, x, y) != '/' || pget(p, x, y + 1) != '<') return 0;
  for (x2 = x + 1; x2 < p->w; x2++) {
    uint32_t ch = pget(p, x2, y);
    if (ch == '\\') break;
    if (!is_hz_ch(ch)) return 0;
  }
  if (x2 >= p->w || x2 - x < 4) return 0;
  for (r = y + 1; r < p->h; r++) {
    if (pget(p, x, r) == '<' && pget(p, x2, r) == '>') continue;
    break;
  }
  if (pget(p, x, r) != '\\' || pget(p, x2, r) != '/') return 0;
  for (c = x + 1; c < x2; c++)
    if (!is_hz_ch(pget(p, c, r))) return 0;
  *ow = x2 - x + 1; *oh = r - y + 1;
  return 1;
}

/* A lozenge: `+` corners, 45° `/` and `\` diagonals, `+` waist points. */
static int find_diamond(const pgrid_t *p, int x, int y, int *ox, int *ow, int *oh)
{
  int mid, tr, k;
  if (pget(p, x, y) != '+' || pget(p, x - 1, y + 1) != '/') return 0;
  for (mid = 1; x - mid >= 0 && y + mid < p->h; mid++) {  /* walk down-left to the waist */
    uint32_t ch = pget(p, x - mid, y + mid);
    if (ch == '+') break;
    if (ch != '/') return 0;
  }
  if (mid < 2 || x - mid < 0) return 0;
  for (tr = x; tr + 1 < p->w; tr++) {                     /* along any flat top */
    uint32_t ch = pget(p, tr + 1, y);
    if (ch == '+') { tr++; break; }
    if (!is_hz_ch(ch)) break;
  }
  if (tr > x && pget(p, tr, y) != '+') return 0;
  for (k = 1; k <= mid; k++) {                            /* the other three diagonals */
    /* top-right, down from the top corner; then the two that climb back from
       the waist vertices to the bottom corners. */
    if (pget(p, tr + k, y + k)             != (k == mid ? '+' : '\\')) return 0;
    if (pget(p, x - mid + k, y + mid + k)  != (k == mid ? '+' : '\\')) return 0;
    if (pget(p, tr + mid - k, y + mid + k) != (k == mid ? '+' : '/'))  return 0;
  }
  for (k = x + 1; k < tr; k++)                            /* the flat top and bottom */
    if (!is_hz_ch(pget(p, k, y)) || !is_hz_ch(pget(p, k, y + 2 * mid))) return 0;
  *ox = x - mid; *ow = (tr + mid) - (x - mid) + 1; *oh = 2 * mid + 1;
  return 1;
}

/* The stick figure. Its arms give the width, its body the height, so the
   rectangle it was drawn in comes straight back out of the art. */
static int find_actor(const pgrid_t *p, int cx, int cy, int *ox, int *ow, int *oh)
{
  int left, right, w, legs;
  if (pget(p, cx, cy) != 'O') return 0;
  if (pget(p, cx, cy + 1) != '|') return 0;
  for (left = cx - 1; left >= 0; left--) {               /* out along the arms */
    uint32_t ch = pget(p, left, cy + 1);
    if (ch == '/') break;
    if (ch != '-') return 0;
  }
  if (left < 0) return 0;
  for (right = cx + 1; right < p->w; right++) {
    uint32_t ch = pget(p, right, cy + 1);
    if (ch == '\\') break;
    if (ch != '-') return 0;
  }
  if (right >= p->w) return 0;
  w = right - left + 1;
  if (w < 3 || left + w / 2 != cx) return 0;             /* the figure must be centred */
  for (legs = cy + 2; legs < p->h; legs++)               /* down the body to the legs */
    if (pget(p, cx, legs) != '|') break;
  if (legs >= p->h) return 0;
  if (pget(p, cx - 1, legs) != '/' || pget(p, cx + 1, legs) != '\\') return 0;
  *ox = left; *ow = w; *oh = legs - cy + 2;              /* + the caption row */
  return 1;
}

/* Try every recogniser at (x, y); on a hit, report the shape and rectangle. */
static int detect_node(const pgrid_t *p, int x, int y, int *shape, int *ox, int *oy, int *ow, int *oh)
{
  int w, h;
  *ox = x; *oy = y;
  if (is_corner_ch(pget(p, x, y)) && find_box(p, x, y, &w, &h)) {
    *ow = w; *oh = h;
    if (is_cylinder(p, x, y, w, h))          *shape = DGM_CYLINDER;
    else if (is_rounded_corner(pget(p, x, y))) *shape = DGM_ROUNDED;
    else                                     *shape = DGM_RECT;
    return 1;
  }
  { int wavy;
    if (find_ellipse(p, x, y, ox, oy, ow, oh, &wavy)) {
      *shape = wavy ? DGM_CLOUD : DGM_ELLIPSE;
      return 1;
    } }
  if (find_circle(p, x, y, ox, oy, ow, oh))  { *shape = DGM_CIRCLE;  return 1; }
  if (find_hexagon(p, x, y, ow, oh))         { *shape = DGM_HEXAGON; return 1; }
  if (find_diamond(p, x, y, ox, ow, oh))     { *shape = DGM_DIAMOND; return 1; }
  if (find_actor(p, x, y, ox, ow, oh))       { *shape = DGM_ACTOR; return 1; }
  return 0;
}

/* Pass 3 helper: flood a component of line characters, noting which boxes it
   touches and where its arrowheads sit. */
typedef struct {
  int  boxes[16], nboxes;
  int  arrow_at[16];        /* parallel to boxes: an arrowhead points into it */
  int *stack, nstack;
} comp_t;

static void comp_note_box(comp_t *cm, int box, int arrow)
{
  int i;
  for (i = 0; i < cm->nboxes; i++)
    if (cm->boxes[i] == box) { if (arrow) cm->arrow_at[i] = 1; return; }
  if (cm->nboxes >= 16) return;
  cm->boxes[cm->nboxes] = box;
  cm->arrow_at[cm->nboxes] = arrow;
  cm->nboxes++;
}

static void flood_component(pgrid_t *p, int sx, int sy, comp_t *cm, int *cells, int *ncells, int maxcells)
{
  static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
  int top = 0;
  cm->stack[top++] = sy * p->w + sx;
  p->used[sy * p->w + sx] = 1;
  while (top > 0) {
    int idx = cm->stack[--top];
    int x = idx % p->w, y = idx / p->w, k;
    int arrow = is_arrow_ch(pget(p, x, y));
    if (*ncells < maxcells) cells[(*ncells)++] = idx;
    for (k = 0; k < 4; k++) {
      int nx = x + dx[k], ny = y + dy[k], owner;
      if (nx < 0 || ny < 0 || nx >= p->w || ny >= p->h) continue;
      owner = powner(p, nx, ny);
      if (owner >= 0) { comp_note_box(cm, owner, arrow); continue; }
      if (p->used[ny * p->w + nx]) continue;
      if (!is_line_ch(pget(p, nx, ny))) continue;
      p->used[ny * p->w + nx] = 1;
      cm->stack[top++] = ny * p->w + nx;
    }
  }
}

int dgm_parse_text(dgm_doc_t *d, const char *text)
{
  pgrid_t p;
  const char *s = text;
  int x = 0, y = 0, i, found = 0, unicode = 0;
  int *cells = NULL;
  unsigned char *rejected = NULL;
  comp_t cm;

  dgm_clear(d);
  p.w = DGM_W; p.h = DGM_H;
  p.c     = malloc((size_t)p.w * p.h * sizeof *p.c);
  p.used  = calloc((size_t)p.w * p.h, 1);
  p.owner = malloc((size_t)p.w * p.h * sizeof *p.owner);
  cm.stack = malloc((size_t)p.w * p.h * sizeof *cm.stack);
  cells    = malloc((size_t)p.w * p.h * sizeof *cells);
  rejected = calloc((size_t)p.w * p.h, 1);
  if (!p.c || !p.used || !p.owner || !cm.stack || !cells || !rejected) goto done;
  for (i = 0; i < p.w * p.h; i++) { p.c[i] = ' '; p.owner[i] = -1; }

  while (*s && y < p.h) {                                /* text → grid */
    uint32_t cp;
    s += utf8_decode(s, &cp);
    if (cp == '\n')      { y++; x = 0; continue; }
    if (cp == '\r')      continue;
    if (cp == '\t')      { x = (x + 8) & ~7; continue; }
    if (x < p.w) p.c[y * p.w + x] = cp;
    if (cp > 0x7f) unicode = 1;
    x++;
  }
  d->style = unicode ? DGM_STYLE_UNICODE : DGM_STYLE_ASCII;

  /* 1. shapes — boxes, rounded boxes, cylinders, ellipses, hexagons,
        diamonds and actors, each with the text inside it as its label */
  for (y = 0; y < p.h; y++)
    for (x = 0; x < p.w; x++) {
      int shape, bx, by, bw, bh, r, c, idx;
      dgm_shape_t probe;
      char label[DGM_LABEL_MAX];
      if (pused(&p, x, y) || pget(&p, x, y) == ' ') continue;
      if (!detect_node(&p, x, y, &shape, &bx, &by, &bw, &bh)) continue;
      memset(&probe, 0, sizeof probe);
      probe.kind = DGM_NODE; probe.shape = shape;
      probe.x = bx; probe.y = by; probe.w = bw; probe.h = bh;
      extract_label(&p, &probe, label, sizeof label);
      idx = dgm_add_node(d, shape, bx, by, bw, bh, label);
      if (idx < 0) continue;
      for (r = by; r < by + bh; r++)
        for (c = bx; c < bx + bw; c++) puse(&p, c, r, idx);
      found++;
    }

  /* 2. free text — a run needs a letter or digit, so "--->" stays a line */
  for (y = 0; y < p.h; y++)
    for (x = 0; x < p.w; x++) {
      char run[DGM_LABEL_MAX];
      size_t len = 0;
      int end = x, wordy = 0, c;
      if (pused(&p, x, y) || pget(&p, x, y) == ' ') continue;
      for (c = x; c < p.w; c++) {
        uint32_t ch = pget(&p, c, y);
        if (pused(&p, c, y)) break;
        if (ch == ' ') {                                 /* one space still joins */
          if (c + 1 >= p.w || pget(&p, c + 1, y) == ' ' || pused(&p, c + 1, y)) break;
          continue;
        }
        if (is_wordy(ch)) wordy = 1;
        end = c;
      }
      if (!wordy) continue;
      for (c = x; c <= end; c++) str_append(run, sizeof run, &len, pget(&p, c, y));
      trim_trailing(run);
      if (run[0] && dgm_add_text(d, x, y, run) >= 0) found++;
      for (c = x; c <= end; c++) puse(&p, c, y, -1);
      x = end;
    }

  /* 3. lines that join two boxes */
  for (y = 0; y < p.h; y++)
    for (x = 0; x < p.w; x++) {
      int ncells = 0, k;
      if (pused(&p, x, y) || rejected[y * p.w + x]) continue;
      if (!is_line_ch(pget(&p, x, y))) continue;
      cm.nboxes = 0;
      flood_component(&p, x, y, &cm, cells, &ncells, p.w * p.h);
      if (cm.nboxes < 2) {          /* joins nothing: hand it to the raw layer */
        for (k = 0; k < ncells; k++) { p.used[cells[k]] = 0; rejected[cells[k]] = 1; }
        continue;
      }
      for (k = 1; k < cm.nboxes; k++) {
        int ci = dgm_add_conn(d, cm.boxes[0], cm.boxes[k]);
        if (ci < 0) continue;
        d->s[ci].arrow_to   = cm.arrow_at[k];
        d->s[ci].arrow_from = cm.arrow_at[0];
        found++;
      }
    }

  /* 4. the leftovers, kept verbatim */
  for (y = 0; y < p.h; y++)
    for (x = 0; x < p.w; x++) {
      uint32_t ch = pget(&p, x, y);
      if (pused(&p, x, y) || ch == ' ') continue;
      d->raw[y][x] = ch;
      d->has_raw = 1;
    }

done:
  free(p.c); free(p.used); free(p.owner); free(cm.stack); free(cells); free(rejected);
  return found;
}

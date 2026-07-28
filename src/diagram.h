#ifndef CCM_DIAGRAM_H
#define CCM_DIAGRAM_H

#include <stdint.h>
#include <stddef.h>

/* ── the diagram document model ──────────────────────────────────────────────
 *
 * A diagram is a small list of *objects* — nodes (boxes, diamonds, cylinders,
 * …), free text and the connectors joining them — over a grid of character
 * cells. Objects are what you drag, resize and re-route; the ASCII art is only
 * ever their rendering.
 *
 * The file on disk is that rendering and nothing else: plain ASCII (or the
 * box-drawing glyphs, if the diagram is in Unicode style), with no sidecar and
 * no embedded metadata. Reopening a file runs the art back through
 * dgm_parse_text(), which recognises each shape, its label and the lines
 * joining them, and rebuilds the objects. Whatever it cannot account for is kept
 * verbatim in the `raw` layer, so a diagram drawn by hand — or by another tool
 * — never loses characters by passing through here.
 *
 * Everything in this header is free of the UI: no caca, no gtcaca, no globals.
 * diagram.c drives it; diagram_model.c implements it. */

#define DGM_MAX_SHAPES 512
#define DGM_LABEL_MAX  256
#define DGM_W          512      /* canvas width  in cells */
#define DGM_H          256      /* canvas height in cells */

/* What an object *is*: a node (a shape with a label), a free-floating label, or
   a connector between two of the others. */
enum { DGM_NODE = 0, DGM_TEXT, DGM_CONN };

/* Which shape a node is drawn as. Every one of them lives in the same x/y/w/h
   rectangle, so moving, resizing, hit testing and connecting are shape-blind —
   only drawing and recognising differ. */
enum {
  DGM_RECT = 0,     /* +----+   process, component, plain box   */
  DGM_ROUNDED,      /* .----.   start / end, terminator         */
  DGM_DIAMOND,      /*  /  \    decision                        */
  DGM_CIRCLE,       /*  .--.    a round node: junction, hub     */
  DGM_ELLIPSE,      /* (    )   state, event                    */
  DGM_CLOUD,        /* .~~~~.   the internet, anything external */
  DGM_CYLINDER,     /*          database, queue, any store      */
  DGM_HEXAGON,      /* /----\   preparation, gateway            */
  DGM_ACTOR,        /*   O      a person: user, service owner   */
  DGM_NSHAPES
};

enum { DGM_STYLE_ASCII = 0, DGM_STYLE_UNICODE };

typedef struct {
  int  kind;                    /* DGM_NODE / DGM_TEXT / DGM_CONN             */
  int  shape;                   /* node: DGM_RECT, DGM_DIAMOND, …             */
  int  x, y, w, h;              /* node: outer rectangle; text: origin + span */
  char label[DGM_LABEL_MAX];    /* node/text content ('\n' separates lines)   */
  int  from, to;                /* connector: the two objects it joins        */
  int  arrow_from, arrow_to;    /* connector: arrowhead at either end         */
} dgm_shape_t;

typedef struct {
  dgm_shape_t s[DGM_MAX_SHAPES];
  int      n;
  int      style;               /* DGM_STYLE_ASCII / DGM_STYLE_UNICODE */
  uint32_t raw[DGM_H][DGM_W];   /* characters carried over verbatim from a file */
  int      has_raw;
} dgm_doc_t;

/* A rendered page of cells: `cell` is w*h codepoints, row-major, ' ' for empty. */
typedef struct { uint32_t *cell; int w, h; } dgm_grid_t;

/* ── document ──────────────────────────────────────────────────────────────── */
dgm_doc_t *dgm_new(void);                     /* zeroed doc, ASCII style */
void       dgm_free(dgm_doc_t *d);
void       dgm_clear(dgm_doc_t *d);

/* Add a node of `shape`; w/h are grown to that shape's minimum if need be. */
int  dgm_add_node(dgm_doc_t *d, int shape, int x, int y, int w, int h, const char *label);
int  dgm_add_text(dgm_doc_t *d, int x, int y, const char *text);
/* Join any two objects — nodes of any shape, and free text too. Two objects
   are either linked or not: asking again, in either direction, returns the link
   that is already there rather than stacking a second one on the same route. */
int  dgm_add_conn(dgm_doc_t *d, int from, int to);
/* The link between `a` and `b` whichever way round it points, or -1. */
int  dgm_find_conn(const dgm_doc_t *d, int a, int b);
/* Redraw an existing node as a different shape (its rectangle is kept, grown
   to the new shape's minimum if that shape needs more room). */
void dgm_set_shape(dgm_doc_t *d, int idx, int shape);
/* A shape's smallest sensible rectangle, and the size a fresh one gets when it
   is dropped from the palette. */
void dgm_shape_min(int shape, int *w, int *h);
void dgm_shape_default(int shape, int *w, int *h);
/* Display name ("Diamond") and a small preview for the palette, drawn with the
   same glyphs `style` would use on the canvas. */
const char *dgm_shape_name(int shape);
const char *dgm_shape_sample(int shape, int style);
int  dgm_duplicate(dgm_doc_t *d, int idx);   /* copy of a node/text, offset by 2,1 */
/* Delete an object (and the connectors hanging off it). Indices of later
   objects shift down by one, exactly as in an array erase. */
void dgm_delete(dgm_doc_t *d, int idx);
void dgm_set_label(dgm_doc_t *d, int idx, const char *label);
/* Move/resize with the model's own limits applied (never below the shape's
   minimum, never off the canvas). */
void dgm_move(dgm_doc_t *d, int idx, int dx, int dy);
void dgm_resize(dgm_doc_t *d, int idx, int dw, int dh);
void dgm_place(dgm_doc_t *d, int idx, int x, int y, int w, int h);

/* ── hit testing (all in canvas cells) ─────────────────────────────────────── */
/* Topmost node/text covering (x, y), or -1 (connectors: dgm_hit_conn). */
int dgm_hit(const dgm_doc_t *d, int x, int y);
/* Topmost connector whose route passes through (x, y), or -1 — so a line can
   be clicked, deleted and re-pointed like any other object. */
int dgm_hit_conn(const dgm_doc_t *d, int x, int y);
enum { DGM_HIT_NONE = 0, DGM_HIT_INSIDE, DGM_HIT_BORDER, DGM_HIT_CORNER };
/* Where (x, y) falls on shape `idx`: its interior (drag to move), its border
   (drag to draw a connector) or its bottom-right corner (drag to resize). */
int dgm_hit_part(const dgm_doc_t *d, int idx, int x, int y);
/* Bounding box of everything drawn, including the raw layer. */
void dgm_extent(const dgm_doc_t *d, int *w, int *h);

/* ── rendering ─────────────────────────────────────────────────────────────── */
/* Paint the document into `g` (which the caller sizes and owns). Cells outside
   the grid are dropped. The grid is cleared to spaces first. */
void dgm_render(const dgm_doc_t *d, dgm_grid_t *g);
/* The four corner points of a connector's elbow route: writes up to `max`
   points into px/py and returns how many. Used by the renderer and by the mode
   to draw a live route while dragging. */
int  dgm_route(const dgm_doc_t *d, int conn, int *px, int *py, int max);

/* ── files ─────────────────────────────────────────────────────────────────── */
/* Render to ASCII: trailing blanks trimmed, one '\n' per line, no trailing
   blank lines. Returns the byte length written (excluding the NUL), or -1 if
   `out` is too small. */
int dgm_to_text(const dgm_doc_t *d, char *out, size_t outsz);
/* Rebuild a document from ASCII art: recognised shapes, their labels and the
   lines joining them become objects; anything left over is kept in the raw
   layer. Returns the number of objects recognised. */
int dgm_parse_text(dgm_doc_t *d, const char *text);
/* ── the raw layer ─────────────────────────────────────────────────────────
   Characters the recogniser could not account for are kept verbatim. They are
   not objects, so they are edited a cell at a time — that is what the eraser
   works on. */
/* One character cell, used to hand back what a change overwrote. */
typedef struct { int x, y; uint32_t ch; } dgm_cell_t;

/* Turn one object back into plain characters: whatever it draws (and, for a
   node, whatever its links draw) is written into the raw layer, and the object
   is removed. The picture does not change — only what is an object does. That
   is the escape hatch from the object model: afterwards any single character of
   it can be erased.

   Every raw cell the call overwrites is appended to `undo` (up to `max`) with
   the value it had before, so the caller can put it back. Returns 0 on
   success. */
int dgm_flatten(dgm_doc_t *d, int idx, dgm_cell_t *undo, int max, int *nundo);
/* The raw character at (x, y), or 0 if that cell holds nothing. */
uint32_t dgm_raw_get(const dgm_doc_t *d, int x, int y);
/* Set (or, with 0, clear) one raw cell. Returns what was there before. */
uint32_t dgm_raw_put(dgm_doc_t *d, int x, int y, uint32_t ch);

/* ── the mode (diagram.c) ──────────────────────────────────────────────────── */
/* M-x diagram: open the current buffer's text as a diagram, full screen. */
void run_diagram(void);

#endif /* CCM_DIAGRAM_H */

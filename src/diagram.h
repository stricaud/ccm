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
 * dgm_parse_text(), which recognises each shape, its label, the shapes nested
 * inside it and the lines joining them, and rebuilds the objects. Whatever it cannot account for is kept
 * verbatim in the `raw` layer, so a diagram drawn by hand — or by another tool
 * — never loses characters by passing through here.
 *
 * Everything in this header is free of the UI: no caca, no gtcaca, no globals.
 * diagram.c drives it; diagram_model.c implements it. */

#define DGM_MAX_SHAPES 512
#define DGM_LABEL_MAX  256
#define DGM_W          512      /* canvas width  in cells */
#define DGM_H          256      /* canvas height in cells */

/* What an object *is*: a node (a shape with a label), a free-floating label, a
   connector between two of the others, or a plain line standing on its own. */
enum { DGM_NODE = 0, DGM_TEXT, DGM_CONN, DGM_LINE };

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

/* Which edge of a node a connector leaves by. AUTO lets the router work it out
   from where the two objects sit — what a link gets when it was not started
   from a particular handle, and what every link used to do. Naming the sides is
   what keeps a link drawn out of the right-hand side on the right-hand side,
   and what lets two links between the same pair take different routes. */
enum { DGM_SIDE_AUTO = 0, DGM_SIDE_TOP, DGM_SIDE_BOTTOM, DGM_SIDE_LEFT, DGM_SIDE_RIGHT };

/* How a line or a link is stroked. Dashes and dots are drawn with the
   characters themselves, so they survive into the saved ASCII; colour cannot,
   and lives only on screen and in a .drawio export. */
enum { DGM_SOLID = 0, DGM_DASHED, DGM_DOTTED, DGM_NSTROKES };

#define DGM_COLOR_DEFAULT 0xff   /* "whatever the theme uses" */

typedef struct {
  int  kind;                    /* DGM_NODE / DGM_TEXT / DGM_CONN             */
  int  shape;                   /* node: DGM_RECT, DGM_DIAMOND, …             */
  int  x, y, w, h;              /* node: outer rectangle; text: origin + span */
  char label[DGM_LABEL_MAX];    /* node/text content ('\n' separates lines)   */
  int  from, to;                /* connector: the two objects it joins        */
  int  from_side, to_side;      /* connector: DGM_SIDE_* each end attaches to */
  int  arrow_from, arrow_to;    /* connector: arrowhead at either end         */
  int  dir;                     /* line: 0 runs '\', 1 runs '/' across x/y/w/h */
  int  fg, bg;                  /* ANSI colours, or DGM_COLOR_DEFAULT          */
  int  stroke;                  /* DGM_SOLID / DGM_DASHED / DGM_DOTTED         */
} dgm_shape_t;

typedef struct {
  dgm_shape_t s[DGM_MAX_SHAPES];
  int      n;
  int      style;               /* DGM_STYLE_ASCII / DGM_STYLE_UNICODE */
  uint32_t raw[DGM_H][DGM_W];   /* characters carried over verbatim from a file */
  int      has_raw;
} dgm_doc_t;

/* A rendered page of cells: `cell` is w*h codepoints, row-major, ' ' for empty.
   `fg`/`bg`, when non-NULL, are filled in alongside with each cell's colour (or
   DGM_COLOR_DEFAULT) — the ASCII output ignores them, the screen does not. */
typedef struct { uint32_t *cell; uint8_t *fg, *bg; int w, h; } dgm_grid_t;

/* ── document ──────────────────────────────────────────────────────────────── */
dgm_doc_t *dgm_new(void);                     /* zeroed doc, ASCII style */
void       dgm_free(dgm_doc_t *d);
void       dgm_clear(dgm_doc_t *d);

/* Add a node of `shape`; w/h are grown to that shape's minimum if need be. */
int  dgm_add_node(dgm_doc_t *d, int shape, int x, int y, int w, int h, const char *label);
int  dgm_add_text(dgm_doc_t *d, int x, int y, const char *text);
/* A plain line between two cells — an object like any other, so it can be
   selected, moved, linked to and deleted. The far end is snapped to the
   nearest horizontal, vertical or 45° run, which is what keeps it a clean
   line of characters (and lets the recogniser find it again). */
int  dgm_add_line(dgm_doc_t *d, int x0, int y0, int x1, int y1);
/* The two ends of a line object, in drawing order. */
void dgm_line_ends(const dgm_shape_t *s, int *x0, int *y0, int *x1, int *y1);
/* Snap a far end to the nearest clean run — what dgm_add_line would do, so a
   preview can show exactly the line that will be drawn. */
void dgm_line_snap(int x0, int y0, int *x1, int *y1);
/* Re-place an existing line's ends (used when dragging one of them). */
void dgm_line_set(dgm_doc_t *d, int idx, int x0, int y0, int x1, int y1);
/* The character a run in this direction is drawn with, for previews. */
uint32_t dgm_line_glyph(int dx, int dy, int style);
/* Join any two objects — nodes of any shape, and free text too — leaving the
   router to choose where the link attaches. */
int  dgm_add_conn(dgm_doc_t *d, int from, int to);
/* The same, naming the edge each end leaves by. Two links between one pair are
   allowed as long as they do not take the same route: asking again for a link
   that already exists with the same two sides returns the one already there,
   rather than stacking an invisible second copy on top of it. */
int  dgm_add_conn_sides(dgm_doc_t *d, int from, int to, int from_side, int to_side);
/* The link between `a` and `b` whichever way round it points, or -1. */
int  dgm_find_conn(const dgm_doc_t *d, int a, int b);
/* The link between `a` and `b` leaving by these sides, or -1. */
int  dgm_find_conn_sides(const dgm_doc_t *d, int a, int b, int from_side, int to_side);
/* Which handle of `idx` covers (x, y) — the side a link dragged from there
   leaves by — or DGM_SIDE_AUTO when that cell is not one of the handles. */
int  dgm_handle_side(const dgm_doc_t *d, int idx, int x, int y);
/* The edge of `idx` nearest (x, y): where a link let go there should arrive. */
int  dgm_nearest_side(const dgm_doc_t *d, int idx, int x, int y);
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
/* "solid" / "dashed" / "dotted". */
const char *dgm_stroke_name(int stroke);
/* Restyle an object: pass DGM_COLOR_DEFAULT to clear a colour, or -1 to leave
   that attribute alone. */
void dgm_set_style(dgm_doc_t *d, int idx, int fg, int bg, int stroke);
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
/* Where (x, y) falls on object `idx`, and so what dragging there does:
     DGM_HIT_CORNER  the bottom-right cell — resize (for a line, either end)
     DGM_HIT_BORDER  one of the four edge-midpoint handles — draw a link
     DGM_HIT_INSIDE  anywhere else on it — move the whole thing
   Only the handles link, so dragging a side moves the object like any other
   part of it. */
int dgm_hit_part(const dgm_doc_t *d, int idx, int x, int y);
/* Bounding box of everything drawn, including the raw layer. */
void dgm_extent(const dgm_doc_t *d, int *w, int *h);

/* ── containers, a.k.a. subgraphs ───────────────────────────────────────────
   A node whose rectangle encloses other objects is a frame around them rather
   than a box in its own right. Nothing marks it as one: being drawn around
   things is what makes it one, which is the only thing that could survive a
   file that is nothing but the art. A container is not filled in (so what is
   inside stays visible however the two were drawn), its label sits on its top
   row instead of the middle, and the Mermaid export writes it as a subgraph. */
int dgm_contains(const dgm_doc_t *d, int outer, int inner);
int dgm_is_container(const dgm_doc_t *d, int idx);
/* The innermost container holding `idx`, or -1 for something at the top level. */
int dgm_parent(const dgm_doc_t *d, int idx);

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
/* Render the same objects as a Mermaid `graph`, which Markdown viewers draw for
   real. The ASCII is the picture; this is the graph behind it, laid out by
   Mermaid rather than by hand — so it keeps shapes, labels, arrows and colours,
   and loses the hand-placed geometry along with anything that is not a node or
   a link. Returns the byte length written (excluding the NUL), or -1 if `out`
   is too small. */
int dgm_to_mermaid(const dgm_doc_t *d, char *out, size_t outsz);
/* How much of the diagram Mermaid cannot carry: free-standing lines (an edge
   needs two ends to join) plus one for a non-empty raw layer. 0 means the
   Mermaid version says everything the ASCII does. */
int dgm_mermaid_dropped(const dgm_doc_t *d);
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

/* ── draw.io ───────────────────────────────────────────────────────────────
   The ASCII is the diagram's real home, but it has no colour and no notion of
   a rhombus. Writing the same objects out as a .drawio file keeps all of it —
   shapes, labels, links, colours, dashes — and opens in app.diagrams.net.
   Returns 0 on success, filling `err` on failure. */
int dgm_export_drawio(const dgm_doc_t *d, const char *path, char *err, size_t errsz);

/* ── the mode (diagram.c) ──────────────────────────────────────────────────── */
/* M-x diagram: open the current buffer's text as a diagram, full screen. */
void run_diagram(void);
/* Offer a pasted block to the diagram mode. Returns 1 if it took it (it only
   wants text while a label or a prompt is being typed). */
int  diagram_paste(const char *text, int len);

#endif /* CCM_DIAGRAM_H */

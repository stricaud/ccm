/* diagram.c — M-x diagram: a drag-and-drop canvas for ASCII diagrams.
 *
 * Artist-mode draws characters; this draws *objects*. A box is a thing you
 * grab, drag, resize and label; a connector is a thing that stays attached to
 * the two boxes it joins and re-routes itself when either moves. The file on
 * disk is still nothing but the ASCII art (see diagram_model.c), so a diagram
 * drops straight into a README, a comment block or an email.
 *
 * The canvas is a gtcaca custom widget: we paint every cell ourselves and take
 * the mouse press → motion → release gesture that the toolkit hands us, which
 * is what makes dragging work in a terminal. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <caca.h>
#include <gtcaca/main.h>
#include <gtcaca/window.h>
#include <gtcaca/custom.h>
#include <gtcaca/box.h>
#include <gtcaca/colordialog.h>
#include <gtcaca/dialog.h>

#include "cacamacs.h"
#include "diagram.h"

enum { TOOL_SELECT = 0, TOOL_SHAPE, TOOL_TEXT, TOOL_CONN, TOOL_ERASE,
       TOOL_PENCIL, TOOL_LINE };
enum { DRAG_NONE = 0, DRAG_MOVE, DRAG_RESIZE, DRAG_CONNECT, DRAG_NEWSHAPE, DRAG_PAN,
       DRAG_PALETTE, DRAG_ERASE, DRAG_PENCIL, DRAG_LINE, DRAG_LINE_END, DRAG_BAND };

/* Characters the freehand pen cycles through with Space. */
static const char PEN_CHARS[] = "*#+.oxX@=~";

#define UNDO_MAX 64
#define PALETTE_W 20        /* columns the shape pane takes on the left */

/* The palette, top to bottom. Every entry is reachable three ways: click it,
   press its digit, or drag it onto the canvas. */
typedef struct { char key; int tool, shape; const char *name; } palette_entry_t;

static const palette_entry_t PALETTE[] = {
  { '1', TOOL_SELECT, -1,           "Select"   },
  { '2', TOOL_SHAPE,  DGM_RECT,     "Box"      },
  { '3', TOOL_SHAPE,  DGM_ROUNDED,  "Rounded"  },
  { '4', TOOL_SHAPE,  DGM_DIAMOND,  "Diamond"  },
  { '5', TOOL_SHAPE,  DGM_CIRCLE,   "Circle"   },
  { '6', TOOL_SHAPE,  DGM_ELLIPSE,  "Ellipse"  },
  { '7', TOOL_SHAPE,  DGM_CLOUD,    "Cloud"    },
  { '8', TOOL_SHAPE,  DGM_CYLINDER, "Cylinder" },
  { '9', TOOL_SHAPE,  DGM_HEXAGON,  "Hexagon"  },
  { '0', TOOL_SHAPE,  DGM_ACTOR,    "Actor"    },
  { 't', TOOL_TEXT,   -1,           "Text"     },
  { 'l', TOOL_CONN,   -1,           "Link"     },
  { 'e', TOOL_ERASE,  -1,           "Eraser"   },
  { 'f', TOOL_PENCIL, -1,           "Freehand" },
  { '-', TOOL_LINE,   -1,           "Line"     },
};
#define PALETTE_N ((int)(sizeof PALETTE / sizeof PALETTE[0]))

/* An undo step carries a snapshot of the object list, a patch of raw cells, or
   both — an eraser stroke that cuts into a shape changes both at once. The raw
   layer is a grid, not a list, so patching cells beats snapshotting half a
   megabyte per stroke. */
typedef struct { dgm_shape_t *s; int n; dgm_cell_t *cells; int ncells; } undo_t;

#define ERASE_MAX 8192
static dgm_cell_t   g_stroke[ERASE_MAX];  /* raw cells the stroke in flight changed */
static int          g_nstroke;
static dgm_shape_t *g_stroke_shapes;      /* objects as they were when it started */
static int          g_stroke_nshapes;

static gtcaca_window_widget_t *g_dw;
static gtcaca_custom_widget_t *g_dview;
static dgm_doc_t              *g_doc;
static dgm_grid_t              g_grid;          /* scratch render target */
static int  g_dgm_open;

static int  g_sel = -1;                         /* the primary selection, -1 for none */
/* More than one object can be selected at once: g_multi says which, g_sel is
   the one picked last. Commands that only make sense on a single object — edit
   the label, flip a link's arrows, drag a handle — use g_sel; the ones that
   generalise (move, restyle, delete, duplicate) run over the whole set. Keeping
   a primary rather than a bare set is what lets the two live side by side:
   the handles and the label editor always have exactly one object to point at. */
static unsigned char g_multi[DGM_MAX_SHAPES];
static int  g_nmulti;
static int  g_band_add;                         /* the rubber band adds rather than replaces */
static int  g_click_sel = -1;                   /* pressed inside a group: the one to keep if
                                                   the press turns out to be a click, not a drag */
static int  g_tool = TOOL_SELECT;
static int  g_shape = DGM_RECT;                 /* which shape the shape tool draws */
static int  g_pal = 1;                          /* the palette pane is shown         */
static int  g_pal_sel;                          /* highlighted palette row           */
static int  g_cur_x, g_cur_y;                   /* keyboard cursor, in canvas cells  */
static int  g_conn_from = -1;                   /* "now pick the other end"          */
static int  g_conn_from_side = DGM_SIDE_AUTO;   /* the handle that started the link  */
static int  g_pen = 0;                          /* index into PEN_CHARS              */
static int  g_new_fg = DGM_COLOR_DEFAULT;       /* style the next object gets        */
static int  g_new_bg = DGM_COLOR_DEFAULT;
static int  g_new_stroke = DGM_SOLID;
/* What `q` drops in the buffer. The ASCII *is* the picture; the Mermaid is the
   graph behind it, which a Markdown viewer draws for real. Which one a diagram
   wants depends on where it is going, so the choice is a per-session setting:
   it starts from "diagram-mermaid-default" in config.json and C-x q changes it
   on the spot. */
enum { OUT_ASCII = 0, OUT_MERMAID };
static int  g_out;
static int  g_cx;                               /* C-x seen, waiting for the next key */
static int  g_prompt;                           /* the one-line "export to:" question */
static char g_prompt_label[48];
static char g_prompt_buf[PATH_MAX];
static int  g_prompt_len;
static int  g_vx, g_vy;                         /* top-left canvas cell on screen */
static int  g_modified;
static int  g_ins_start, g_ins_end;             /* buffer range the drawing replaces */
static char g_dgm_msg[192];
static int  g_show_help;

static int  g_drag = DRAG_NONE;
static int  g_drag_shape = -1;
static int  g_drag_ox, g_drag_oy;               /* grab point inside the shape */
static int  g_drag_x0, g_drag_y0;               /* where the press landed (canvas) */
static int  g_drag_x1, g_drag_y1;               /* where the pointer is now  */

static int  g_editing;                          /* typing a label */
static char g_edit_buf[DGM_LABEL_MAX];
static int  g_edit_len;

static int  g_confirm_quit;                     /* y/n/C-g answer wanted */

static undo_t g_undo[UNDO_MAX];
static int    g_undo_n;

static void dgm_status(void);
static void sel_validate(void);   /* undo swaps the object list wholesale */
static const char *out_name(void);

/* ── undo ────────────────────────────────────────────────────────────────────
   Snapshots are of the object list only: the raw layer is set once, when a
   file is parsed, and never edited. */

/* Remember what a finished eraser stroke changed: the characters it rubbed out
   and, if it cut into a shape, the object list from before. */
static void undo_push_cells(void)
{
  undo_t u;
  if (g_nstroke <= 0 && !g_stroke_shapes) return;
  u.s = g_stroke_shapes; u.n = g_stroke_nshapes;
  g_stroke_shapes = NULL; g_stroke_nshapes = 0;
  u.ncells = g_nstroke;
  if (g_nstroke <= 0) { u.cells = NULL; g_nstroke = 0;
                        if (g_undo_n < UNDO_MAX) { g_undo[g_undo_n++] = u; } else free(u.s);
                        return; }
  u.cells = malloc((size_t)g_nstroke * sizeof *u.cells);
  if (!u.cells) { g_nstroke = 0; return; }
  memcpy(u.cells, g_stroke, (size_t)g_nstroke * sizeof *u.cells);
  if (g_undo_n == UNDO_MAX) {
    free(g_undo[0].s); free(g_undo[0].cells);
    memmove(&g_undo[0], &g_undo[1], (size_t)(UNDO_MAX - 1) * sizeof g_undo[0]);
    g_undo_n--;
  }
  g_undo[g_undo_n++] = u;
  g_nstroke = 0;
}

static void undo_push(void)
{
  undo_t u;
  u.cells = NULL; u.ncells = 0;
  u.n = g_doc->n;
  u.s = malloc((size_t)(u.n ? u.n : 1) * sizeof *u.s);
  if (!u.s) return;
  memcpy(u.s, g_doc->s, (size_t)u.n * sizeof *u.s);
  if (g_undo_n == UNDO_MAX) {                   /* drop the oldest */
    free(g_undo[0].s); free(g_undo[0].cells);
    memmove(&g_undo[0], &g_undo[1], (size_t)(UNDO_MAX - 1) * sizeof g_undo[0]);
    g_undo_n--;
  }
  g_undo[g_undo_n++] = u;
}

static void undo_pop(void)
{
  undo_t u;
  if (g_undo_n == 0) { snprintf(g_dgm_msg, sizeof g_dgm_msg, "Nothing to undo"); return; }
  u = g_undo[--g_undo_n];
  if (u.cells) {                                /* put erased characters back */
    int i;
    for (i = 0; i < u.ncells; i++)
      dgm_raw_put(g_doc, u.cells[i].x, u.cells[i].y, u.cells[i].ch);
    free(u.cells);
  }
  if (u.s) {                                    /* and any objects it dissolved */
    memcpy(g_doc->s, u.s, (size_t)u.n * sizeof *u.s);
    g_doc->n = u.n;
    free(u.s);
    sel_validate();
  }
  snprintf(g_dgm_msg, sizeof g_dgm_msg, u.cells ? "Undo — %d character%s back" : "Undo",
           u.ncells, u.ncells == 1 ? "" : "s");
  g_modified = 1;
}

static void undo_clear(void)
{
  while (g_undo_n > 0) {
    g_undo_n--;
    free(g_undo[g_undo_n].s);
    free(g_undo[g_undo_n].cells);
  }
  g_nstroke = 0;
}

/* ── the selection ───────────────────────────────────────────────────────────*/

static int sel_has(int i) { return i >= 0 && i < g_doc->n && g_multi[i]; }

static void sel_clear(void)
{
  memset(g_multi, 0, sizeof g_multi);
  g_nmulti = 0;
  g_sel = -1;
}

/* The selection becomes exactly {idx}, or empty for idx < 0. */
static void sel_set(int idx)
{
  sel_clear();
  if (idx < 0 || idx >= g_doc->n) return;
  g_multi[idx] = 1;
  g_nmulti = 1;
  g_sel = idx;
}

static void sel_add(int idx)
{
  if (idx < 0 || idx >= g_doc->n) return;
  if (!g_multi[idx]) { g_multi[idx] = 1; g_nmulti++; }
  g_sel = idx;
}

/* Shift-click: in if it was out, out if it was in. Dropping the primary hands
   that role to whatever is still selected, so the handles never vanish while
   objects remain picked. */
static void sel_toggle(int idx)
{
  int i;
  if (idx < 0 || idx >= g_doc->n) return;
  if (!g_multi[idx]) { sel_add(idx); return; }
  g_multi[idx] = 0;
  g_nmulti--;
  if (g_sel != idx) return;
  g_sel = -1;
  for (i = 0; i < g_doc->n; i++) if (g_multi[i]) { g_sel = i; return; }
}

/* Objects come and go underneath the selection — an undo, a delete, a reparse —
   so anything that is no longer there is dropped before the set is used. */
static void sel_validate(void)
{
  int i;
  g_nmulti = 0;
  for (i = 0; i < DGM_MAX_SHAPES; i++) {
    if (i >= g_doc->n) g_multi[i] = 0;
    if (g_multi[i]) g_nmulti++;
  }
  if (g_sel >= g_doc->n || (g_sel >= 0 && !g_multi[g_sel])) g_sel = -1;
  if (g_sel < 0 && g_nmulti)
    for (i = 0; i < g_doc->n; i++) if (g_multi[i]) { g_sel = i; break; }
}

/* ── screen ↔ canvas ─────────────────────────────────────────────────────────
   The widget's last row is the hint bar and, when it is shown, its leftmost
   PALETTE_W columns are the shape pane; the canvas viewport is what is left. */

static int pal_w(void)   { return g_pal ? PALETTE_W : 0; }
static int view_x0(void) { return g_dview->x + pal_w(); }
static int view_w(void)  { return g_dview ? g_dview->width - pal_w() : 0; }
static int view_h(void)  { return g_dview ? g_dview->height - 1 : 0; }
static int scr_x(int cx) { return view_x0() + (cx - g_vx); }
static int scr_y(int cy) { return g_dview->y + (cy - g_vy); }
static int canvas_x(int sx) { return sx - view_x0() + g_vx; }
static int canvas_y(int sy) { return sy - g_dview->y + g_vy; }
static int on_canvas(int sx, int sy)
{
  return sx >= view_x0() && sx < view_x0() + view_w() &&
         sy >= g_dview->y && sy < g_dview->y + view_h();
}
/* Which palette row a click landed on, or -1. */
static int palette_row_at(int sx, int sy)
{
  int row = sy - (g_dview->y + 2);
  if (!g_pal || sx < g_dview->x || sx >= g_dview->x + PALETTE_W) return -1;
  return (row >= 0 && row < PALETTE_N) ? row : -1;
}

/* Keep the selected shape (or a point of interest) in view. */
static void scroll_to(int cx, int cy)
{
  if (cx < g_vx) g_vx = cx;
  if (cy < g_vy) g_vy = cy;
  if (cx >= g_vx + view_w())  g_vx = cx - view_w() + 1;
  if (cy >= g_vy + view_h())  g_vy = cy - view_h() + 1;
  if (g_vx < 0) g_vx = 0;
  if (g_vy < 0) g_vy = 0;
}

static void scroll_to_sel(void)
{
  const dgm_shape_t *s;
  if (g_sel < 0 || g_sel >= g_doc->n) return;
  s = &g_doc->s[g_sel];
  if (s->kind == DGM_CONN) return;
  scroll_to(s->x, s->y);
  scroll_to(s->x + s->w - 1, s->y + s->h - 1);
}

/* ── painting ────────────────────────────────────────────────────────────────*/

static void put(int x, int y, uint32_t ch, uint8_t fg, uint8_t bg)
{
  caca_set_color_ansi(gmo.cv, fg, bg);
  caca_set_attr(gmo.cv, 0);
  caca_put_char(gmo.cv, x, y, ch);
}

/* Draw a UTF-8 string, one codepoint per cell, stopping after `maxw` cells. */
static void put_str_clipped(int x, int y, const char *s, int maxw, uint8_t fg, uint8_t bg)
{
  int n = 0;
  caca_set_color_ansi(gmo.cv, fg, bg);
  caca_set_attr(gmo.cv, 0);
  while (*s && n < maxw) {
    const unsigned char *u = (const unsigned char *)s;
    uint32_t cp = *u;
    int len = 1;
    if      ((*u & 0xe0) == 0xc0 && u[1])               { cp = ((uint32_t)(*u & 0x1f) << 6) | (u[1] & 0x3f); len = 2; }
    else if ((*u & 0xf0) == 0xe0 && u[1] && u[2])       { cp = ((uint32_t)(*u & 0x0f) << 12) | ((uint32_t)(u[1] & 0x3f) << 6) | (u[2] & 0x3f); len = 3; }
    else if ((*u & 0xf8) == 0xf0 && u[1] && u[2] && u[3]) { cp = ((uint32_t)(*u & 0x07) << 18) | ((uint32_t)(u[1] & 0x3f) << 12) | ((uint32_t)(u[2] & 0x3f) << 6) | (u[3] & 0x3f); len = 4; }
    caca_put_char(gmo.cv, x + n, y, cp);
    s += len; n++;
  }
}

/* What the renderer put in a canvas cell. A connector's endpoint sits one cell
   outside the box it leaves, which is off-canvas for a box at column 0, so
   every read goes through here. */
static uint32_t grid_at(int cx, int cy)
{
  if (cx < 0 || cy < 0 || cx >= g_grid.w || cy >= g_grid.h) return ' ';
  return g_grid.cell[cy * g_grid.w + cx];
}

/* The rectangle a drag is currently describing (new box / rubber band). */
static void drag_rect(int *x, int *y, int *w, int *h)
{
  int x0 = g_drag_x0 < g_drag_x1 ? g_drag_x0 : g_drag_x1;
  int y0 = g_drag_y0 < g_drag_y1 ? g_drag_y0 : g_drag_y1;
  int x1 = g_drag_x0 > g_drag_x1 ? g_drag_x0 : g_drag_x1;
  int y1 = g_drag_y0 > g_drag_y1 ? g_drag_y0 : g_drag_y1;
  *x = x0; *y = y0; *w = x1 - x0 + 1; *h = y1 - y0 + 1;
}

static const char *tool_name(void)
{
  switch (g_tool) {
  case TOOL_SHAPE:  return dgm_shape_name(g_shape);
  case TOOL_TEXT:   return "text";
  case TOOL_CONN:   return "link";
  case TOOL_ERASE:  return "eraser";
  case TOOL_PENCIL: return "freehand";
  case TOOL_LINE:   return "line";
  default:        return "select";
  }
}

static const char *const help_lines[] = {
  "  Diagram mode — draw.io in a terminal, saved as plain ASCII",
  "",
  "  Getting started",
  "     Pick a shape in the pane on the left, then click on the canvas.",
  "     Or drag the shape straight from the pane to where you want it.",
  "     Type its label, press Enter, and you have your first object.",
  "",
  "  Shapes    1 Select   2 Box      3 Rounded   4 Diamond   5 Circle",
  "            6 Ellipse  7 Cloud    8 Cylinder  9 Hexagon   0 Actor",
  "            t Text     l Link     e Eraser    f Freehand   - Line",
  "            p hides / shows the shape pane",
  "",
  "  Freehand  drag to draw characters yourself; Space picks the character.",
  "  Line      drag from one end to the other — a plain line, no arrows. It is",
  "            an object: click it, drag it, drag an end, Del removes it.",
  "            (l links two objects instead, and keeps them joined.)",
  "",
  "  Style     C colour   B background   S solid / dashed / dotted",
  "            With something selected these restyle it; with nothing selected",
  "            they arm the style everything you draw next will get.",
  "            Dashes and dots are drawn with characters, so the saved ASCII",
  "            keeps them. Colour cannot survive plain text —",
  "            C-w writes a copy that keeps colours, shapes and all: name it",
  "            .drawio for app.diagrams.net, or .mmd for Mermaid.",
  "",
  "  ASCII or Mermaid",
  "            The ASCII art is the picture itself — it drops into a README, a",
  "            comment or an email and needs nothing to read it. A Mermaid",
  "            graph is the graph behind the picture: GitHub, GitLab and most",
  "            Markdown viewers lay it out and draw it for real, so it keeps",
  "            colours and true shapes, but loses the hand-placed geometry,",
  "            free-standing lines and any loose characters.",
  "            C-x q picks which one q inserts; in a .md file the Mermaid goes",
  "            in a ```mermaid fence. \"diagram-mermaid-default\": true in",
  "            ~/.cacamacs/config.json makes Mermaid the one you start on.",
  "",
  "  Eraser    drag rubs out loose characters one cell at a time; a click",
  "            removes the whole object under it.  u undoes either.",
  "            x turns the selected object into plain characters first, so",
  "            the eraser can rub out any part of it.",
  "",
  "  Mouse     drag anywhere on an object   move it (or all that are selected)",
  "            drag its # bottom-right     resize it",
  "            drag an o edge handle       link it to another object",
  "            click a link or a line      select it",
  "            double-click an object      type its label, centred in it",
  "            drag empty space            a band: everything it touches is",
  "                                        selected (Shift keeps what was)",
  "            middle/right drag           pan       wheel  scroll",
  "",
  "  Several at once",
  "            Shift-click adds an object to the selection, or takes it out —",
  "            where the terminal passes Shift-click through, that is. Many",
  "            keep it for their own text selection, so the band and m below",
  "            do the same job without it.",
  "            m adds the next object      A selects everything",
  "            Tab starts again from one   C-g clears the selection",
  "            With several picked, moving, colouring, resizing, r, x, D and",
  "            Del act on all of them at once.",
  "",
  "  Linking   click one object, then click the other (with Link armed) —",
  "            or drag from a selected object's  o  handle onto another —",
  "            or select one, press c, Tab to the other, press Enter.",
  "            Any two objects link, whatever their shapes.",
  "            The handle you drag from is the side the link leaves by, and",
  "            where you let go is the side it arrives at — so a link dragged",
  "            out of the right-hand side stays on the right-hand side, going",
  "            round the boxes if it has to. Clicking instead of dragging lets",
  "            the router pick the sides for you, as it always did.",
  "            Two objects may be joined more than once (a request and a reply",
  "            want two arrows), as long as the two links use different sides;",
  "            and one handle can feed as many links as you like.",
  "",
  "  Keys      Tab      select the next object      Ret   edit its label",
  "            arrows   move the selection, or the cursor when nothing is picked",
  "            Ret      with nothing selected, places the armed shape at the cursor",
  "            < >      narrower / wider            [ ]   shorter / taller",
  "            r        change the shape            a     flip a link's arrows",
  "            Del / d  delete      D duplicate     u     undo",
  "            s        ASCII / Unicode line drawing",
  "            q insert the diagram at the cursor and leave — it asks whether",
  "              to insert the ASCII art or a Mermaid graph first (C-x C-s",
  "              then saves the file, C-/ undoes the insert)",
  "              Q leaves the drawing behind   C-x q is the same question",
  "            C-g cancel   ? this help",
  "",
  "  While typing a label: Ret commits, C-o starts a second line, C-g cancels.",
  NULL
};

static void draw_help_overlay(void)
{
  int n = 0, w = 0, i, x, y;
  for (i = 0; help_lines[i]; i++) {
    int l = (int)strlen(help_lines[i]);
    if (l > w) w = l;
    n++;
  }
  w += 4;
  if (w > view_w()) w = view_w();
  if (n + 2 > view_h()) n = view_h() - 2;        /* a short terminal gets what fits */
  if (n < 1) return;
  x = view_x0() + (view_w() - w) / 2;
  y = g_dview->y + (view_h() - (n + 2)) / 2;
  if (x < view_x0()) x = view_x0();
  if (y < g_dview->y) y = g_dview->y;
  for (i = 0; i < n + 2; i++) {
    int k;
    for (k = 0; k < w; k++) put(x + k, y + i, ' ', CACA_WHITE, CACA_BLUE);
  }
  for (i = 0; i < n; i++)
    put_str_clipped(x + 1, y + 1 + i, help_lines[i], w - 2, CACA_WHITE, CACA_BLUE);
}

/* Activate a palette row: the Select/Text/Link rows switch tool, a shape row
   switches tool *and* arms that shape. */
static void palette_pick(int row)
{
  if (row < 0 || row >= PALETTE_N) return;
  g_pal_sel = row;
  g_tool = PALETTE[row].tool;
  if (PALETTE[row].shape >= 0) g_shape = PALETTE[row].shape;
  if (g_tool == TOOL_SELECT)
    snprintf(g_dgm_msg, sizeof g_dgm_msg,
             "Select: click an object to pick it, drag it to move it");
  else if (g_tool == TOOL_CONN)
    snprintf(g_dgm_msg, sizeof g_dgm_msg,
             "Link: click the first object, then click the second");
  else if (g_tool == TOOL_TEXT)
    snprintf(g_dgm_msg, sizeof g_dgm_msg, "Text: click where the label should start");
  else if (g_tool == TOOL_ERASE)   /* checked before the shape case below */
    snprintf(g_dgm_msg, sizeof g_dgm_msg,
             "Eraser: drag to rub out characters one cell at a time");
  else if (g_tool == TOOL_PENCIL)
    snprintf(g_dgm_msg, sizeof g_dgm_msg,
             "Freehand: drag to draw with '%c' — Space picks another character",
             PEN_CHARS[g_pen]);
  else if (g_tool == TOOL_LINE)
    snprintf(g_dgm_msg, sizeof g_dgm_msg,
             "Line: drag from one end to the other — a plain line, no arrows");
  else
    snprintf(g_dgm_msg, sizeof g_dgm_msg,
             "%s: click on the canvas to place one (drag to size it), or press Enter",
             dgm_shape_name(g_shape));
}

/* The pen's current character, for the palette row. */
static const char *pen_sample(void)
{
  static char buf[8];
  snprintf(buf, sizeof buf, "%c %c %c", PEN_CHARS[g_pen], PEN_CHARS[g_pen], PEN_CHARS[g_pen]);
  return buf;
}

/* The shape pane: one row per entry, its digit, its name and a small preview,
   with the active row highlighted. */
static void draw_palette(void)
{
  int x0 = g_dview->x, y0 = g_dview->y, i, x;
  int h = g_dview->height - 1;

  for (i = 0; i < h; i++)
    for (x = 0; x < PALETTE_W; x++)
      put(x0 + x, y0 + i, ' ', CACA_WHITE, CACA_BLUE);

  put_str_clipped(x0 + 1, y0, "Shapes", PALETTE_W - 2, CACA_YELLOW, CACA_BLUE);
  for (x = 0; x < PALETTE_W - 2; x++) put(x0 + 1 + x, y0 + 1, '-', CACA_WHITE, CACA_BLUE);

  for (i = 0; i < PALETTE_N && 2 + i < h; i++) {
    int active = (i == g_pal_sel);
    uint8_t fg = active ? CACA_BLACK : CACA_WHITE;
    uint8_t bg = active ? CACA_CYAN  : CACA_BLUE;
    char row[PALETTE_W + 8];
    const char *sample = PALETTE[i].shape >= 0 ? dgm_shape_sample(PALETTE[i].shape, g_doc->style)
                       : PALETTE[i].tool == TOOL_CONN  ? "-->"
                       : PALETTE[i].tool == TOOL_TEXT  ? "abc"
                       : PALETTE[i].tool == TOOL_ERASE  ? "[x]"
                       : PALETTE[i].tool == TOOL_LINE   ? "\\ | /"
                       : PALETTE[i].tool == TOOL_PENCIL ? pen_sample() : "[ ]";
    for (x = 0; x < PALETTE_W; x++) put(x0 + x, y0 + 2 + i, ' ', fg, bg);
    snprintf(row, sizeof row, "%c %-9s%s", PALETTE[i].key, PALETTE[i].name, sample);
    put_str_clipped(x0 + 1, y0 + 2 + i, row, PALETTE_W - 2, fg, bg);
  }

  if (2 + PALETTE_N + 2 < h) {
    put_str_clipped(x0 + 1, y0 + PALETTE_N + 3, "click or drag one",
                    PALETTE_W - 2, CACA_LIGHTGRAY, CACA_BLUE);
    put_str_clipped(x0 + 1, y0 + PALETTE_N + 4, "onto the canvas",
                    PALETTE_W - 2, CACA_LIGHTGRAY, CACA_BLUE);
    put_str_clipped(x0 + 1, y0 + PALETTE_N + 6, "p  hide this pane",
                    PALETTE_W - 2, CACA_LIGHTGRAY, CACA_BLUE);
    put_str_clipped(x0 + 1, y0 + PALETTE_N + 7, "?  all the keys",
                    PALETTE_W - 2, CACA_LIGHTGRAY, CACA_BLUE);
  }
}

/* The outline a shape would have if dropped right now, so a drag from the
   palette shows what is coming before the button is released. */
static void draw_ghost(int shape, int x, int y, int w, int h)
{
  int i, j;
  for (j = 0; j < h; j++)
    for (i = 0; i < w; i++) {
      int edge = (i == 0 || i == w - 1 || j == 0 || j == h - 1);
      if (!edge || !on_canvas(scr_x(x + i), scr_y(y + j))) continue;
      put(scr_x(x + i), scr_y(y + j), shape == DGM_ACTOR ? 'O' : '.',
          CACA_BLACK, CACA_YELLOW);
    }
}

/* Paint one selected object over the rendered art: a box gets its border marked,
   a line and a connector their whole run. `primary` adds the o link handles and
   the # resize corner, which belong to a single object at a time. */
static void draw_selected(int idx, int primary)
{
  const dgm_shape_t *s;
  /* The primary is washed brighter than the rest of the group: it is the one
     carrying the handles, so which object a drag from a handle would resize or
     link has to be visible at a glance. */
  uint8_t wash = primary ? CACA_CYAN : CACA_BLUE;
  int x, y;

  if (idx < 0 || idx >= g_doc->n) return;
  s = &g_doc->s[idx];
  if (s->kind == DGM_CONN) {
    int px[8], py[8], n = dgm_route(g_doc, idx, px, py, 8), k;
    for (k = 0; k + 1 < n; k++) {
      int cx = px[k], cy = py[k];
      int sx = (px[k + 1] > cx) - (px[k + 1] < cx), sy = (py[k + 1] > cy) - (py[k + 1] < cy);
      for (;;) {
        if (on_canvas(scr_x(cx), scr_y(cy)))
          put(scr_x(cx), scr_y(cy), grid_at(cx, cy), CACA_BLACK, wash);
        if (cx == px[k + 1] && cy == py[k + 1]) break;
        cx += sx; cy += sy;
      }
    }
    return;
  }
  if (s->kind == DGM_LINE) {
    int x0, y0, x1, y1, dx, dy, ax, ay, steps, i;
    dgm_line_ends(s, &x0, &y0, &x1, &y1);
    dx = x1 - x0; dy = y1 - y0;
    ax = dx < 0 ? -dx : dx; ay = dy < 0 ? -dy : dy;
    steps = ax > ay ? ax : ay;
    for (i = 0; i <= steps; i++) {
      int lx = steps ? x0 + dx * i / steps : x0, ly = steps ? y0 + dy * i / steps : y0;
      if (on_canvas(scr_x(lx), scr_y(ly)))
        put(scr_x(lx), scr_y(ly), grid_at(lx, ly), CACA_BLACK, wash);
    }
    if (!primary) return;
    if (on_canvas(scr_x(x0), scr_y(y0)))         /* the ends are the handles */
      put(scr_x(x0), scr_y(y0), 'o', CACA_BLACK, CACA_MAGENTA);
    if (on_canvas(scr_x(x1), scr_y(y1)))
      put(scr_x(x1), scr_y(y1), 'o', CACA_BLACK, CACA_MAGENTA);
    return;
  }

  for (y = s->y; y < s->y + s->h; y++)
    for (x = s->x; x < s->x + s->w; x++) {
      int edge = (x == s->x || x == s->x + s->w - 1 || y == s->y || y == s->y + s->h - 1);
      if (s->kind == DGM_NODE && !edge) continue;
      if (!on_canvas(scr_x(x), scr_y(y))) continue;
      put(scr_x(x), scr_y(y), grid_at(x, y), CACA_BLACK, wash);
    }
  /* The link handles and the resize corner. Which cells those are is asked of
     dgm_hit_part rather than worked out again here: a handle you can see but
     not grab (or grab but not see) is worse than no handle at all, so the
     drawing and the hit test have to be the same rule. */
  if (primary && s->kind == DGM_NODE) {
    for (y = s->y; y < s->y + s->h; y++)
      for (x = s->x; x < s->x + s->w; x++) {
        int part = dgm_hit_part(g_doc, idx, x, y);
        if (part != DGM_HIT_BORDER && part != DGM_HIT_CORNER) continue;
        if (!on_canvas(scr_x(x), scr_y(y))) continue;
        put(scr_x(x), scr_y(y), part == DGM_HIT_CORNER ? '#' : 'o', CACA_BLACK,
            part == DGM_HIT_CORNER ? CACA_YELLOW : CACA_MAGENTA);
      }
  }
}

static void draw_cb(gtcaca_custom_widget_t *w, void *ud)
{
  int x, y, vw = view_w(), vh = view_h();
  (void)w; (void)ud;

  dgm_render(g_doc, &g_grid);

  for (y = 0; y < vh; y++)
    for (x = 0; x < vw; x++) {
      int cx = g_vx + x, cy = g_vy + y;
      uint32_t ch = grid_at(cx, cy);
      uint8_t  cfg = CACA_LIGHTGRAY, cbg = CACA_BLACK;
      if (cx >= 0 && cy >= 0 && cx < g_grid.w && cy < g_grid.h) {
        uint8_t f = g_grid.fg[cy * g_grid.w + cx], b = g_grid.bg[cy * g_grid.w + cx];
        if (f != DGM_COLOR_DEFAULT) cfg = f;
        if (b != DGM_COLOR_DEFAULT) cbg = b;
      }
      put(view_x0() + x, g_dview->y + y, ch ? ch : ' ', cfg, cbg);
    }

  /* The selection, repainted on top in colour. Every member of the set gets the
     wash; only the primary carries the handles, so there is never any doubt
     about which object a drag from one of them would act on. */
  {
    int si;
    for (si = 0; si < g_doc->n; si++)
      if (g_multi[si]) draw_selected(si, si == g_sel);
  }

  /* The keyboard cursor: where Enter would put the next shape. Only shown when
     nothing is selected, so it never competes with the selection highlight. */
  if (g_sel < 0 && !g_editing && on_canvas(scr_x(g_cur_x), scr_y(g_cur_y)))
    put(scr_x(g_cur_x), scr_y(g_cur_y), grid_at(g_cur_x, g_cur_y), CACA_BLACK, CACA_WHITE);

  /* Live feedback for whatever is being dragged right now. */
  if (g_drag == DRAG_NEWSHAPE) {
    int bx, by, bw, bh;
    drag_rect(&bx, &by, &bw, &bh);
    for (y = by; y < by + bh; y++)
      for (x = bx; x < bx + bw; x++) {
        int edge = (x == bx || x == bx + bw - 1 || y == by || y == by + bh - 1);
        if (!edge || !on_canvas(scr_x(x), scr_y(y))) continue;
        put(scr_x(x), scr_y(y), '.', CACA_BLACK, CACA_YELLOW);
      }
  } else if (g_drag == DRAG_BAND) {
    int bx, by, bw, bh;
    drag_rect(&bx, &by, &bw, &bh);
    for (y = by; y < by + bh; y++)
      for (x = bx; x < bx + bw; x++) {
        int edge = (x == bx || x == bx + bw - 1 || y == by || y == by + bh - 1);
        if (!edge || !on_canvas(scr_x(x), scr_y(y))) continue;
        put(scr_x(x), scr_y(y), grid_at(x, y), CACA_BLACK, CACA_GREEN);
      }
  } else if (g_drag == DRAG_PALETTE) {
    if (g_tool == TOOL_SHAPE || g_tool == TOOL_TEXT) {
      int gw, gh;
      dgm_shape_default(g_shape, &gw, &gh);
      if (g_tool == TOOL_TEXT) { gw = 4; gh = 1; }
      draw_ghost(g_shape, g_drag_x1 - gw / 2, g_drag_y1 - gh / 2, gw, gh);
    } else if (g_tool == TOOL_CONN) {
      /* Dragging the arrow out: mark the object it is over, since that is the
         end the link will start from. */
      int over = dgm_hit(g_doc, g_drag_x1, g_drag_y1);
      if (over >= 0) draw_selected(over, 1);
    }
  } else if (g_drag == DRAG_LINE) {
    int ex = g_drag_x1, ey = g_drag_y1, dx, dy, ax, ay, steps, i;
    uint32_t ch;
    dgm_line_snap(g_drag_x0, g_drag_y0, &ex, &ey);
    dx = ex - g_drag_x0; dy = ey - g_drag_y0;
    ax = dx < 0 ? -dx : dx; ay = dy < 0 ? -dy : dy;
    steps = ax > ay ? ax : ay;
    ch = dgm_line_glyph(dx, dy, g_doc->style);
    for (i = 0; i <= steps; i++) {
      int lx = steps ? g_drag_x0 + dx * i / steps : g_drag_x0;
      int ly = steps ? g_drag_y0 + dy * i / steps : g_drag_y0;
      if (on_canvas(scr_x(lx), scr_y(ly)))
        put(scr_x(lx), scr_y(ly), ch, CACA_BLACK, CACA_YELLOW);
    }
  } else if (g_drag == DRAG_CONNECT) {
    int cx = g_drag_x0, cy = g_drag_y0;
    int steps = 0;
    while (steps++ < 4096 && (cx != g_drag_x1 || cy != g_drag_y1)) {
      int dx = (g_drag_x1 > cx) - (g_drag_x1 < cx);
      int dy = (g_drag_y1 > cy) - (g_drag_y1 < cy);
      if (dx && cx != g_drag_x1) cx += dx; else if (dy) cy += dy;
      if (on_canvas(scr_x(cx), scr_y(cy)))
        put(scr_x(cx), scr_y(cy), '*', CACA_BLACK, CACA_YELLOW);
    }
  }

  /* A label being typed is drawn where it will land, with a caret. */
  if (g_editing && g_sel >= 0 && g_sel < g_doc->n) {
    const dgm_shape_t *s = &g_doc->s[g_sel];
    int is_node = (s->kind == DGM_NODE);
    int maxw = is_node ? s->w - 2 : view_w();
    int ly = is_node ? s->y + s->h / 2 : s->y;
    const char *shown = g_edit_buf;
    const char *nl = strrchr(g_edit_buf, '\n');
    int n, off = 0, lx;
    if (nl) shown = nl + 1;
    n = (int)strlen(shown);
    if (maxw < 1) maxw = 1;
    if (n > maxw - 1) off = n - (maxw - 1);      /* keep the caret in view */
    /* Centre it in the shape, the way it will be drawn once committed — typing
       into a box should look like the finished label, not like a field. */
    lx = is_node ? s->x + 1 + (s->w - 2 - (n - off) - 1) / 2 : s->x;
    if (lx < s->x + (is_node ? 1 : 0)) lx = s->x + (is_node ? 1 : 0);
    if (on_canvas(scr_x(lx), scr_y(ly))) {
      put_str_clipped(scr_x(lx), scr_y(ly), shown + off, maxw, CACA_BLACK, CACA_WHITE);
      put(scr_x(lx) + (n - off), scr_y(ly), '_', CACA_BLACK, CACA_WHITE);
    }
  }

  /* Bottom row: the prompt if one is up, otherwise the key legend. */
  {
    int ly = g_dview->y + g_dview->height - 1;
    char line[512];
    for (x = 0; x < g_dview->width; x++) put(g_dview->x + x, ly, ' ', CACA_BLACK, CACA_LIGHTGRAY);
    if (g_prompt)
      snprintf(line, sizeof line, "%s%s_", g_prompt_label, g_prompt_buf);
    else if (g_cx)
      snprintf(line, sizeof line, "C-x -   (q picks ASCII or Mermaid for the insert)");
    else if (g_confirm_quit)
      snprintf(line, sizeof line,
               "Leave the drawing behind?  y = insert it after all   n = discard it   C-g = stay");
    else if (g_dgm_msg[0])
      snprintf(line, sizeof line, "%s", g_dgm_msg);
    else if (g_editing)
      snprintf(line, sizeof line, "Type the label:  Ret commits   C-o adds a line   C-g cancels");
    else if (g_conn_from >= 0)
      snprintf(line, sizeof line,
               "Link from \"%s\": click the other object, or Tab to it and press Enter  (C-g cancels)",
               g_doc->s[g_conn_from].label[0] ? g_doc->s[g_conn_from].label : "that object");
    else if (g_tool == TOOL_SHAPE)
      snprintf(line, sizeof line,
               "%s: click to place · drag to size · Enter places at the cursor | 1 back to Select  ? help",
               dgm_shape_name(g_shape));
    else if (g_tool == TOOL_CONN)
      snprintf(line, sizeof line,
               "Link: click one object, then click the other (dragging between them works too)"
               " | 1 back to Select   C-g cancels");
    else if (g_tool == TOOL_TEXT)
      snprintf(line, sizeof line, "Text: click where the label starts | 1 back to Select   ? help");
    else if (g_tool == TOOL_ERASE)
      snprintf(line, sizeof line,
               "Eraser: drag rubs out characters one cell at a time (a shape becomes "
               "characters first) | u undoes   1 back to Select");
    else if (g_tool == TOOL_PENCIL)
      snprintf(line, sizeof line,
               "Freehand: drag to draw with '%c' · Space picks another character "
               "| e erases   u undoes   1 back to Select", PEN_CHARS[g_pen]);
    else if (g_tool == TOOL_LINE)
      snprintf(line, sizeof line,
               "Line: drag from one end to the other — plain, no arrowheads "
               "| l links objects instead   u undoes   1 back to Select");
    else if (g_nmulti > 1)
      snprintf(line, sizeof line,
               "%d selected: drag any of them to move them all · click one to pick just it · "
               "C colour  S dashed  Del removes them  C-g clears", g_nmulti);
    else if (g_sel >= 0)
      snprintf(line, sizeof line,
               "Drag anywhere on it to move · # corner resizes · o handles link · "
               "Ret label  r shape  C colour  B background  S dashed  Del  u undo");
    else
      snprintf(line, sizeof line,
               "Pick a shape on the left (or press its digit), then click the canvas | "
               "Tab/m select  C colour  C-w export  q insert %s (C-x q switches)  ? help",
               out_name());
    put_str_clipped(g_dview->x, ly, line, g_dview->width, CACA_BLACK, CACA_LIGHTGRAY);
  }

  /* An empty canvas explains itself rather than sitting there black. */
  if (g_doc->n == 0 && !g_editing && !g_show_help) {
    static const char *const start[] = {
      " Start here ",
      "",
      " 1.  Pick a shape on the left — or press its digit",
      " 2.  Click on the canvas to place it, then type its label",
      " 3.  To link two: click one, press l, then click the other",
      "     (or drag from a selected object's  o  handle onto another)",
      "",
      " q puts the diagram in the buffer at your cursor · ? shows every key",
      NULL
    };
    int n = 0, wd = 0, i, bx, by;
    for (i = 0; start[i]; i++) {
      int l = (int)strlen(start[i]);
      if (l > wd) wd = l;
      n++;
    }
    wd += 2;
    bx = view_x0() + (view_w() - wd) / 2;
    by = g_dview->y + (view_h() - n) / 2;
    if (bx < view_x0()) bx = view_x0();
    if (by < g_dview->y) by = g_dview->y;
    for (i = 0; i < n; i++) {
      int k;
      for (k = 0; k < wd; k++) put(bx + k, by + i, ' ', CACA_WHITE, CACA_BLUE);
      put_str_clipped(bx + 1, by + i, start[i], wd - 2,
                      i == 0 ? CACA_YELLOW : CACA_WHITE, CACA_BLUE);
    }
  }

  if (g_pal) draw_palette();
  if (g_show_help) draw_help_overlay();
  dgm_status();
}

static void dgm_status(void)
{
  char buf[256];
  const char *base = (g_cur_buf >= 0 && g_cur_buf < g_nbuf && g_buffers[g_cur_buf].has_file)
                   ? g_buffers[g_cur_buf].path : "*scratch*";
  const char *slash = strrchr(base, '/');
  char sel[24];
  int nb = 0, nc = 0, nt = 0, nl = 0, i;
  if (g_nmulti > 1) snprintf(sel, sizeof sel, " %d selected", g_nmulti);
  else sel[0] = '\0';                            /* one object needs no count */
  for (i = 0; i < g_doc->n; i++) {
    if (g_doc->s[i].kind == DGM_NODE)      nb++;
    else if (g_doc->s[i].kind == DGM_CONN) nc++;
    else if (g_doc->s[i].kind == DGM_LINE) nl++;
    else nt++;
  }
  /* The count of what is selected and the form q will insert both belong near
     the front: this line is routinely wider than the terminal, and anything
     appended to the end of it is simply never seen. */
  snprintf(buf, sizeof buf,
           " %s%s  diagram  %s%s  %d shape%s %d link%s %d line%s %d label%s  %s  "
           "q %s %s at the cursor",
           g_modified ? "**  " : "--  ", slash ? slash + 1 : base, tool_name(),
           sel, nb, nb == 1 ? "" : "s", nc, nc == 1 ? "" : "s",
           nl, nl == 1 ? "" : "s", nt, nt == 1 ? "" : "s",
           g_doc->style == DGM_STYLE_UNICODE ? "unicode" : "ascii",
           g_ins_end > g_ins_start ? "replaces the selection with" : "inserts",
           out_name());
  {                                            /* what the next object will look like */
    char style[96];
    int fg = g_sel >= 0 ? g_doc->s[g_sel].fg : g_new_fg;
    int st = g_sel >= 0 ? g_doc->s[g_sel].stroke : g_new_stroke;
    snprintf(style, sizeof style, "  [%s %s]",
             fg == DGM_COLOR_DEFAULT ? "default" : gtcaca_color_name(fg), dgm_stroke_name(st));
    strncat(buf, style, sizeof buf - strlen(buf) - 1);
  }
  if (g_modeline) gtcaca_statusbar_set_text(g_modeline, buf);
}

/* ── editing helpers ─────────────────────────────────────────────────────────*/

static void begin_edit(int idx)
{
  if (idx < 0 || idx >= g_doc->n || g_doc->s[idx].kind == DGM_CONN) return;
  sel_set(idx);
  strncpy(g_edit_buf, g_doc->s[idx].label, sizeof g_edit_buf - 1);
  g_edit_buf[sizeof g_edit_buf - 1] = '\0';
  g_edit_len = (int)strlen(g_edit_buf);
  g_editing = 1;
  g_dgm_msg[0] = '\0';
}

static void commit_edit(int keep)
{
  if (!g_editing) return;
  g_editing = 0;
  if (!keep || g_sel < 0 || g_sel >= g_doc->n) return;
  if (!strcmp(g_doc->s[g_sel].label, g_edit_buf)) return;   /* typed nothing new */
  undo_push();
  dgm_set_label(g_doc, g_sel, g_edit_buf);
  /* An empty text object would be invisible and unselectable — drop it. */
  if (g_doc->s[g_sel].kind == DGM_TEXT && !g_edit_buf[0]) {
    dgm_delete(g_doc, g_sel);
    sel_clear();
  }
  g_modified = 1;
}

static const char *out_name(void) { return g_out == OUT_MERMAID ? "mermaid" : "ascii"; }

/* Ask which form to insert. The configured default is the button that starts
   highlighted, so "diagram-mermaid-default": true still decides the answer you
   get by just pressing Enter. Returns 0 if the question was cancelled, in which
   case nothing should be inserted and the drawing stays open. */
static int ask_out_format(void)
{
  const char *mermaid_first[2] = { "mermaid", "ascii" };
  const char *ascii_first[2]   = { "ascii", "mermaid" };
  int def_mermaid = (g_out == OUT_MERMAID);
  int r = gtcaca_dialog_run("Insert diagram",
                            "Which diagram format would you like to insert?",
                            def_mermaid ? mermaid_first : ascii_first, 2);
  gtcaca_redraw();
  if (r < 0) return 0;                           /* cancelled: stay where we are */
  g_out = (r == 0) == def_mermaid ? OUT_MERMAID : OUT_ASCII;
  return 1;
}

/* Does this buffer take a Mermaid graph as a fenced block? Markdown renderers
   draw ```mermaid fences and show the source of anything else, so the fence is
   the difference between a diagram and a wall of text — while in a .c file the
   same fence would just be three stray backticks. */
static int buffer_is_markdown(void)
{
  const char *path, *dot;
  if (g_cur_buf < 0 || g_cur_buf >= g_nbuf || !g_buffers[g_cur_buf].has_file) return 0;
  path = g_buffers[g_cur_buf].path;
  dot = strrchr(path, '.');
  if (!dot) return 0;
  return !strcasecmp(dot, ".md") || !strcasecmp(dot, ".markdown") ||
         !strcasecmp(dot, ".mdx") || !strcasecmp(dot, ".mdown");
}

/* Put the drawing into the buffer where it came from: over the region it was
   started from, or at the cursor if there was none. The mode never writes a
   file — the buffer is saved the ordinary way, with C-x C-s. */
static void apply_to_buffer(void)
{
  size_t cap = (size_t)DGM_W * DGM_H * 4 + DGM_H + 2;
  char *art, *text;
  int n, at_line_start, pos;

  if (!g_ed) return;
  art = malloc(cap);
  text = malloc(cap + 2);
  if (!art || !text) { free(art); free(text); return; }

  if (g_out == OUT_MERMAID) {
    char *body = malloc(cap);
    if (!body) { free(art); free(text); return; }
    n = dgm_to_mermaid(g_doc, body, cap);
    if (n >= 0)
      n = snprintf(art, cap, "%s%s%s", buffer_is_markdown() ? "```mermaid\n" : "",
                   body, buffer_is_markdown() ? "```\n" : "");
    free(body);
    if (n < 0 || (size_t)n >= cap) n = -1;
  } else
    n = dgm_to_text(g_doc, art, cap);
  if (n < 0) {
    free(art); free(text);
    snprintf(g_message, sizeof g_message, "Diagram too large to insert");
    return;
  }

  if (g_ins_end > g_ins_start)                   /* it came from a region */
    gtcaca_editor_delete_range(g_ed, g_ins_start, g_ins_end - g_ins_start);

  /* Art is made of whole lines, so it must start on one: if the cursor sat
     mid-line, break the line first rather than wrapping the first box around
     whatever was already there. */
  pos = g_ins_start;
  at_line_start = (pos == 0) ||
                  (pos == gtcaca_editor_position_from_line(g_ed,
                            gtcaca_editor_line_from_position(g_ed, pos)));
  snprintf(text, cap + 2, "%s%s", at_line_start ? "" : "\n", art);

  gtcaca_editor_insert_text(g_ed, pos, text);
  gtcaca_editor_set_empty_selection(g_ed, pos + (int)strlen(text));
  free(art); free(text);
}

static void close_diagram(int keep)
{
  int inserted = keep && g_modified;
  int dropped  = (g_out == OUT_MERMAID) ? dgm_mermaid_dropped(g_doc) : 0;

  if (inserted) apply_to_buffer();

  gtcaca_widget_hide(GTCACA_WIDGET(g_dview));
  gtcaca_widget_hide(GTCACA_WIDGET(g_dw));
  g_dview->has_focus = 0;
  g_dw->has_focus = 0;
  g_dgm_open = 0;
  g_show_help = 0;
  g_confirm_quit = 0;
  undo_clear();
  if (inserted && g_out == OUT_MERMAID)
    snprintf(g_message, sizeof g_message,
             "Mermaid graph inserted%s — C-x C-s saves the file, C-/ undoes the insert",
             dropped ? " (free lines and loose characters do not travel — C-x q for ASCII)" : "");
  else
    snprintf(g_message, sizeof g_message,
             inserted ? "Diagram inserted — C-x C-s saves the file, C-/ undoes the insert"
                      : g_modified ? "Left the diagram behind (nothing inserted)"
                                   : "Left diagram mode");
  pane_show_all();
}

/* Record one raw cell's previous value, so the stroke can be undone whole. */
static void stroke_record(int x, int y, uint32_t was)
{
  if (g_nstroke >= ERASE_MAX) return;
  g_stroke[g_nstroke].x = x;
  g_stroke[g_nstroke].y = y;
  g_stroke[g_nstroke].ch = was;
  g_nstroke++;
}

/* Put one character on the background layer, remembering what it replaced.
   Objects are drawn over this layer, so a stroke that runs under a shape is
   kept but hidden — the message below says so rather than leaving you
   wondering. */
static int draw_at(int x, int y, uint32_t ch)
{
  uint32_t was;
  if (x < 0 || y < 0 || x >= DGM_W || y >= DGM_H) return 0;
  was = dgm_raw_get(g_doc, x, y);
  if (was == ch) return 1;
  stroke_record(x, y, was);
  dgm_raw_put(g_doc, x, y, ch);
  g_modified = 1;
  if (dgm_hit(g_doc, x, y) >= 0)
    snprintf(g_dgm_msg, sizeof g_dgm_msg,
             "Drawing behind a shape — shapes cover the background layer");
  return 1;
}

/* Rub out exactly one character — never a whole object; Del does that.
   A character inside a shape belongs to that shape rather than to the canvas,
   so the shape is first turned back into plain characters (which changes
   nothing on screen), and then the one cell goes. */
static int erase_at(int x, int y)
{
  uint32_t was = dgm_raw_get(g_doc, x, y);
  int owner;

  if (!was || was == ' ') {
    owner = dgm_hit(g_doc, x, y);
    if (owner < 0) owner = dgm_hit_conn(g_doc, x, y);
    if (owner < 0) return 0;                     /* genuinely empty cell */
    if (!g_stroke_shapes) {                      /* remember the objects, once */
      g_stroke_shapes = malloc((size_t)(g_doc->n ? g_doc->n : 1) * sizeof *g_stroke_shapes);
      if (g_stroke_shapes) {
        memcpy(g_stroke_shapes, g_doc->s, (size_t)g_doc->n * sizeof *g_doc->s);
        g_stroke_nshapes = g_doc->n;
      }
    }
    {
      dgm_cell_t made[ERASE_MAX];
      int nmade = 0, i;
      const char *what = g_doc->s[owner].kind == DGM_CONN ? "link"
                       : g_doc->s[owner].kind == DGM_TEXT ? "label"
                       : dgm_shape_name(g_doc->s[owner].shape);
      if (dgm_flatten(g_doc, owner, made, ERASE_MAX, &nmade) != 0) return 0;
      for (i = 0; i < nmade; i++) stroke_record(made[i].x, made[i].y, made[i].ch);
      /* Flattening renumbers every object after it, so the marks no longer
         point where they did — drop the selection rather than keep a wrong one. */
      sel_clear();
      snprintf(g_dgm_msg, sizeof g_dgm_msg,
               "That %s is plain characters now, so it can be rubbed out (u undoes)", what);
    }
    was = dgm_raw_get(g_doc, x, y);
    if (!was || was == ' ') return 0;
  }

  dgm_raw_put(g_doc, x, y, 0);
  stroke_record(x, y, was);
  g_modified = 1;
  return 1;
}

/* Write the objects out to a file of their own. The name says which kind: .mmd
   (or .mermaid) writes the Mermaid graph, anything else the .drawio that keeps
   the colours. Either way the buffer is untouched — that is what q is for. */
static void export_file(const char *path)
{
  char full[PATH_MAX], err[160];
  const char *dot;

  if (!path || !path[0]) { snprintf(g_dgm_msg, sizeof g_dgm_msg, "Export cancelled"); return; }
  expand_tilde(path, full, sizeof full);
  dot = strrchr(full, '.');
  if (dot && (!strcasecmp(dot, ".mmd") || !strcasecmp(dot, ".mermaid"))) {
    size_t cap = (size_t)DGM_W * DGM_H * 4 + DGM_H + 2;
    char *body = malloc(cap);
    FILE *f;
    int n, dropped;
    if (!body) { snprintf(g_dgm_msg, sizeof g_dgm_msg, "Out of memory"); return; }
    n = dgm_to_mermaid(g_doc, body, cap);
    if (n < 0) { free(body); snprintf(g_dgm_msg, sizeof g_dgm_msg, "Diagram too large"); return; }
    f = fopen(full, "wb");
    if (!f) { free(body); snprintf(g_dgm_msg, sizeof g_dgm_msg, "cannot write %s", full); return; }
    fwrite(body, 1, (size_t)n, f);
    dropped = dgm_mermaid_dropped(g_doc);
    if (ferror(f) || fclose(f) != 0)
      snprintf(g_dgm_msg, sizeof g_dgm_msg, "short write on %s", full);
    else
      snprintf(g_dgm_msg, sizeof g_dgm_msg, "Wrote %s%s", full,
               dropped ? " — free lines and loose characters are not in it" : "");
    free(body);
    return;
  }
  if (dgm_export_drawio(g_doc, full, err, sizeof err) != 0)
    snprintf(g_dgm_msg, sizeof g_dgm_msg, "%s", err);
  else
    snprintf(g_dgm_msg, sizeof g_dgm_msg,
             "Wrote %s — colours and all. q still inserts the %s here.", full, out_name());
}

/* Offer a sensible name next to the file being edited, in whichever form the
   mode is set to write — so C-w and q agree about what you are making. */
static void start_export(void)
{
  char suggestion[PATH_MAX];
  const char *base = (g_cur_buf >= 0 && g_cur_buf < g_nbuf && g_buffers[g_cur_buf].has_file)
                   ? g_buffers[g_cur_buf].path : "diagram";
  const char *dot = strrchr(base, '.');
  int keep = dot && dot != base ? (int)(dot - base) : (int)strlen(base);
  snprintf(suggestion, sizeof suggestion, "%.*s%s", keep, base,
           g_out == OUT_MERMAID ? ".mmd" : ".drawio");
  snprintf(g_prompt_label, sizeof g_prompt_label, "Export to (.mmd or .drawio): ");
  snprintf(g_prompt_buf, sizeof g_prompt_buf, "%s", suggestion);
  g_prompt_len = (int)strlen(g_prompt_buf);
  g_prompt = 1;
  g_dgm_msg[0] = '\0';
}

/* Give a freshly made object the style currently armed in the mode, so a run of
   red dashed links does not have to be restyled one by one. */
static void apply_new_style(int idx)
{
  if (idx < 0 || idx >= g_doc->n) return;
  dgm_set_style(g_doc, idx, g_new_fg, g_new_bg, g_new_stroke);
}

/* Pick a colour with the toolkit's swatch dialog. Returns -2 if cancelled. */
static int ask_colour(const char *what, int current)
{
  int c = gtcaca_colordialog_run(what, current == DGM_COLOR_DEFAULT ? -1 : current);
  gtcaca_redraw();
  return c < 0 ? -2 : c;
}

/* Everything the rubber band touched. A link comes along when both of its ends
   do — that is what makes a band drawn round a cluster pick up the cluster
   whole rather than leaving its arrows behind. The primary stays a node where
   there is one, so the handles land somewhere useful. */
static void select_in_rect(int bx, int by, int bw, int bh, int add)
{
  int i, first = -1;
  if (!add) sel_clear();
  for (i = 0; i < g_doc->n; i++) {
    const dgm_shape_t *s = &g_doc->s[i];
    if (s->kind == DGM_CONN) continue;
    if (s->x >= bx + bw || s->x + s->w <= bx) continue;
    if (s->y >= by + bh || s->y + s->h <= by) continue;
    sel_add(i);
    if (first < 0) first = i;
  }
  for (i = 0; i < g_doc->n; i++) {
    const dgm_shape_t *s = &g_doc->s[i];
    if (s->kind == DGM_CONN && sel_has(s->from) && sel_has(s->to)) sel_add(i);
  }
  if (first >= 0) g_sel = first;
}

/* Move everything selected by (dx, dy), clamped so the set as a whole stays on
   the canvas — clamping each object on its own would shear the group apart. */
static void move_selection(int dx, int dy)
{
  int i, minx = DGM_W, miny = DGM_H, maxx = 0, maxy = 0;
  if (!g_nmulti) return;
  for (i = 0; i < g_doc->n; i++) {
    const dgm_shape_t *s = &g_doc->s[i];
    if (!g_multi[i] || s->kind == DGM_CONN) continue;
    if (s->x < minx) minx = s->x;
    if (s->y < miny) miny = s->y;
    if (s->x + s->w > maxx) maxx = s->x + s->w;
    if (s->y + s->h > maxy) maxy = s->y + s->h;
  }
  if (minx > maxx) return;                       /* only links are selected */
  if (minx + dx < 0)     dx = -minx;
  if (miny + dy < 0)     dy = -miny;
  if (maxx + dx > DGM_W) dx = DGM_W - maxx;
  if (maxy + dy > DGM_H) dy = DGM_H - maxy;
  if (!dx && !dy) return;
  for (i = 0; i < g_doc->n; i++)
    if (g_multi[i] && g_doc->s[i].kind != DGM_CONN) dgm_move(g_doc, i, dx, dy);
  g_modified = 1;
}

/* Delete the whole selection in one pass.
 *
 * Calling dgm_delete once per marked object does not work: deleting a node
 * takes its connectors with it, and a connector can sit at a *lower* index than
 * the node, so even working from the back renumbers marks that have not been
 * used yet. Compacting the list here instead means every index moves exactly
 * once, and the map says where to. A connector also goes when either of its
 * ends does — deleting a box must not leave an arrow pointing at nothing. */
static void delete_selection(void)
{
  int map[DGM_MAX_SHAPES];
  int i, keep = 0, removed = 0;

  if (!g_nmulti) return;
  undo_push();
  for (i = 0; i < g_doc->n; i++) {
    const dgm_shape_t *s = &g_doc->s[i];
    int drop = g_multi[i];
    if (!drop && s->kind == DGM_CONN)
      drop = (s->from >= 0 && s->from < g_doc->n && g_multi[s->from]) ||
             (s->to   >= 0 && s->to   < g_doc->n && g_multi[s->to]);
    map[i] = drop ? -1 : keep++;
    if (drop) removed++;
  }
  keep = 0;
  for (i = 0; i < g_doc->n; i++) {
    dgm_shape_t *s;
    if (map[i] < 0) continue;
    if (keep != i) g_doc->s[keep] = g_doc->s[i];
    s = &g_doc->s[keep];
    if (s->kind == DGM_CONN) {                   /* renumber the ends it still has */
      if (s->from >= 0 && s->from < g_doc->n) s->from = map[s->from];
      if (s->to   >= 0 && s->to   < g_doc->n) s->to   = map[s->to];
    }
    keep++;
  }
  g_doc->n = keep;
  sel_clear();
  if (removed) g_modified = 1;
}

/* Turn every selected shape and label into plain characters. Only nodes and
   text are listed: a connector's characters are written out along with the node
   it hangs off, and dgm_flatten deletes those connectors as it goes — which is
   what shifts the indices still to come, so each one is walked back past them. */
static int flatten_selection(void)
{
  int idx[DGM_MAX_SHAPES], n = 0, done = 0, i, k;

  for (i = 0; i < g_doc->n; i++)
    if (g_multi[i] && g_doc->s[i].kind != DGM_CONN) idx[n++] = i;
  if (!n) return 0;
  undo_push();
  for (k = n - 1; k >= 0; k--) {                 /* back to front: only lower marks shift */
    int gone[DGM_MAX_SHAPES], ng = 0, c, j;
    for (c = 0; c < idx[k]; c++)                 /* its connectors that sit below it */
      if (g_doc->s[c].kind == DGM_CONN &&
          (g_doc->s[c].from == idx[k] || g_doc->s[c].to == idx[k])) gone[ng++] = c;
    if (dgm_flatten(g_doc, idx[k], NULL, 0, NULL) != 0) continue;
    done++;
    for (j = 0; j < k; j++)
      for (c = 0; c < ng; c++) if (idx[j] > gone[c]) idx[j]--;
  }
  if (done) { sel_clear(); g_modified = 1; }
  return done;
}

static void resize_selection(int dw, int dh)
{
  int i;
  if (!g_nmulti) return;
  undo_push();
  for (i = 0; i < g_doc->n; i++)
    if (g_multi[i] && g_doc->s[i].kind != DGM_CONN) dgm_resize(g_doc, i, dw, dh);
  g_modified = 1;
}

/* Restyle the selection, or arm the style for what comes next. */
static void style_selection(int fg, int bg, int stroke)
{
  if (g_nmulti) {
    int i;
    undo_push();
    for (i = 0; i < g_doc->n; i++)
      if (g_multi[i]) dgm_set_style(g_doc, i, fg, bg, stroke);
    g_modified = 1;
  } else {
    if (fg >= 0)     g_new_fg = fg;
    if (bg >= 0)     g_new_bg = bg;
    if (stroke >= 0) g_new_stroke = stroke;
  }
}

/* Join two objects, or explain why nothing happened. Two objects are either
   linked or not — asking for a link that already exists (in either direction)
   selects it instead of stacking an invisible second one on the same route. */
static const char *side_name(int side)
{
  switch (side) {
  case DGM_SIDE_TOP:    return "top";
  case DGM_SIDE_BOTTOM: return "bottom";
  case DGM_SIDE_LEFT:   return "left";
  case DGM_SIDE_RIGHT:  return "right";
  default:              return "";
  }
}

/* Join two objects, leaving by the given sides (DGM_SIDE_AUTO lets the router
   choose, which is what the click-then-click gesture and the keyboard use).
   Two objects may be joined more than once — a request and a reply want to be
   two arrows — but not twice by the same route, since the second would land
   exactly on the first. Asking for a route that already exists selects it. */
static void link_objects_sides(int from, int to, int from_side, int to_side)
{
  int existing, c;

  if (from < 0 || to < 0 || from >= g_doc->n || to >= g_doc->n) return;
  if (from == to) {
    snprintf(g_dgm_msg, sizeof g_dgm_msg, "An object cannot link to itself");
    return;
  }
  existing = dgm_find_conn_sides(g_doc, from, to, from_side, to_side);
  if (existing >= 0) {
    sel_set(existing);
    snprintf(g_dgm_msg, sizeof g_dgm_msg,
             "That link is already there — drag from another handle for a second "
             "route, a flips the arrows, Del removes it");
    return;
  }
  undo_push();
  c = dgm_add_conn_sides(g_doc, from, to, from_side, to_side);
  if (c < 0) { snprintf(g_dgm_msg, sizeof g_dgm_msg, "Cannot link those two"); return; }
  apply_new_style(c);
  sel_set(c);
  g_modified = 1;
  if (from_side)
    snprintf(g_dgm_msg, sizeof g_dgm_msg, "Linked from the %s side to the %s",
             side_name(from_side), side_name(to_side));
  else
    snprintf(g_dgm_msg, sizeof g_dgm_msg, "Linked");
}

static void link_objects(int from, int to)
{
  link_objects_sides(from, to, DGM_SIDE_AUTO, DGM_SIDE_AUTO);
}

/* Cycle the selection through the objects, connectors included. */
static void select_next(int dir)
{
  int n = g_doc->n, i;
  if (n == 0) { sel_clear(); return; }
  i = g_sel < 0 ? (dir > 0 ? -1 : 0) : g_sel;
  i = (i + dir + n) % n;
  sel_set(i);
  scroll_to_sel();
}

/* ── mouse ───────────────────────────────────────────────────────────────────*/

static int mouse_cb_inner(gtcaca_custom_widget_t *w, gtcaca_mouse_event_t ev,
                          int mx, int my, int button, void *ud)
{
  int cx, cy;
  /* Shift extends the selection where the terminal forwards it — many keep
     Shift-click for their own text selection, which is why the rubber band and
     m do the same job without it. Older toolkits cannot report a modifier at
     all (see the check in CMakeLists.txt); there it simply never fires, and
     those other two gestures carry the feature on their own. */
#ifdef CCM_HAVE_MOUSE_MODIFIERS
  int shift = (gtcaca_mouse_modifiers() & GTCACA_MOD_SHIFT) != 0;
#else
  int shift = 0;
#endif
  (void)w; (void)ud;
  if (!g_dgm_open) return 0;

  if (ev == GTCACA_MOUSE_WHEEL) {
    g_vy += (button == 4) ? 3 : -3;
    if (g_vy < 0) g_vy = 0;
    if (g_vy > DGM_H - 1) g_vy = DGM_H - 1;
    return 1;
  }

  cx = canvas_x(mx);
  cy = canvas_y(my);
  if (cx < 0) cx = 0;
  if (cy < 0) cy = 0;
  if (cx >= DGM_W) cx = DGM_W - 1;
  if (cy >= DGM_H) cy = DGM_H - 1;

  /* CCM_DGMLOG=1 traces gestures, the way CCM_EVLOG traces gtcaca's events —
     the only practical way to see what a drag did, since the screen it draws
     on is the thing being debugged. */
  if (getenv("CCM_DGMLOG"))
    fprintf(stderr, "[dgm] ev=%d mx=%d my=%d btn=%d -> canvas(%d,%d) tool=%d drag=%d sel=%d\n",
            (int)ev, mx, my, button, canvas_x(mx), canvas_y(my), g_tool, g_drag, g_sel);
  if (ev == GTCACA_MOUSE_PRESS) {
    int hit, row;
    if (g_show_help) { g_show_help = 0; return 1; }
    if (g_editing) commit_edit(1);

    row = palette_row_at(mx, my);
    if (row >= 0) {                              /* a palette entry: click or drag it */
      palette_pick(row);
      g_drag_x0 = g_drag_x1 = cx;
      g_drag_y0 = g_drag_y1 = cy;
      /* The pane says "click or drag one onto the canvas", so every row has to
         mean something when it is dragged out — dragging the Link row used to
         arm the tool and then silently do nothing on release. */
      g_drag = DRAG_PALETTE;
      return 1;
    }
    if (!on_canvas(mx, my)) return 1;            /* the pane edge or the hint bar */
    g_dgm_msg[0] = '\0';
    g_drag_x0 = g_drag_x1 = cx;
    g_drag_y0 = g_drag_y1 = cy;
    g_cur_x = cx; g_cur_y = cy;                  /* the keyboard cursor follows clicks */

    /* Waiting for the far end of a link: this click picks it. */
    if (g_conn_from >= 0) {
      int target = dgm_hit(g_doc, cx, cy);
      if (target < 0)
        snprintf(g_dgm_msg, sizeof g_dgm_msg, "Nothing there to link to — cancelled");
      else
        link_objects(g_conn_from, target);
      g_conn_from = -1;
      return 1;
    }

    if (g_tool == TOOL_ERASE) {
      g_nstroke = 0;
      g_drag = DRAG_ERASE;
      erase_at(cx, cy);
      return 1;
    }
    if (g_tool == TOOL_PENCIL) {
      g_nstroke = 0;
      g_drag = DRAG_PENCIL;
      draw_at(cx, cy, (uint32_t)(unsigned char)PEN_CHARS[g_pen]);
      return 1;
    }
    if (g_tool == TOOL_LINE) {                   /* anchor one end; drag out the other */
      g_nstroke = 0;
      g_drag = DRAG_LINE;
      return 1;
    }
    if (g_tool == TOOL_SHAPE) { g_drag = DRAG_NEWSHAPE; return 1; }
    if (g_tool == TOOL_TEXT) {
      undo_push();
      sel_set(dgm_add_text(g_doc, cx, cy, ""));
      apply_new_style(g_sel);
      g_modified = 1;
      palette_pick(0);                           /* one placement, then back to Select */
      begin_edit(g_sel);
      return 1;
    }

    hit = dgm_hit(g_doc, cx, cy);
    if (hit < 0) {                               /* nothing here */
      int conn = dgm_hit_conn(g_doc, cx, cy);
      if (conn >= 0 && g_tool == TOOL_SELECT) {
        if (shift) sel_toggle(conn); else sel_set(conn);
        return 1;
      }
      /* Left on empty space pulls a rubber band, the way it does in draw.io;
         the other buttons pan, which is what the left one used to do. Holding
         Shift keeps whatever was already picked and adds to it. */
      if (button != 1 || g_tool != TOOL_SELECT) { g_drag = DRAG_PAN; return 1; }
      g_band_add = shift;
      if (!shift) sel_clear();
      g_drag = DRAG_BAND;
      return 1;
    }

    /* Shift-click on an object adds it to the selection (or takes it out) and
       starts nothing — the click is the whole gesture. */
    if (shift && g_tool == TOOL_SELECT) { sel_toggle(hit); return 1; }
    /* Pressing on one of several already-picked objects keeps the group, so a
       drag from here moves all of them. But a press that turns out to be a
       plain click means "just this one" — otherwise there is no way out of a
       group except C-g, and every drag afterwards moves everything. Which of
       the two it was is only known on release, so record the candidate now. */
    g_click_sel = -1;
    if (sel_has(hit)) { g_sel = hit; if (g_nmulti > 1) g_click_sel = hit; }
    else sel_set(hit);
    if (g_tool == TOOL_CONN) {
      /* Arm the link now: a second click finishes it, and dragging straight to
         the other object finishes it too. Either gesture works. */
      g_conn_from = hit;
      g_conn_from_side = DGM_SIDE_AUTO;
      g_drag = DRAG_CONNECT;
      g_drag_shape = hit;
      snprintf(g_dgm_msg, sizeof g_dgm_msg, "Now click the object to link to");
      return 1;
    }
    if (g_doc->s[hit].kind == DGM_LINE &&
        dgm_hit_part(g_doc, hit, cx, cy) == DGM_HIT_CORNER) {
      int x0, y0, x1, y1;                        /* grab the end that was clicked */
      dgm_line_ends(&g_doc->s[hit], &x0, &y0, &x1, &y1);
      undo_push();
      g_drag = DRAG_LINE_END;
      g_drag_shape = hit;
      g_drag_ox = (cx == x0 && cy == y0) ? x1 : x0;   /* the end that stays put */
      g_drag_oy = (cx == x0 && cy == y0) ? y1 : y0;
      return 1;
    }
    switch (dgm_hit_part(g_doc, hit, cx, cy)) {
    case DGM_HIT_CORNER:
      /* Resizing pins the opposite corner and stretches to the pointer, so the
         box tracks the mouse wherever it goes — including back past the anchor,
         which used to jam it at its minimum size and look like the drag had
         been dropped. */
      g_drag = DRAG_RESIZE;
      g_drag_shape = hit;
      g_drag_ox = g_doc->s[hit].x;
      g_drag_oy = g_doc->s[hit].y;
      undo_push();
      break;
    case DGM_HIT_BORDER:
      /* Which handle you grab is which edge the link leaves by — drag from the
         right-hand side and the arrow stays on the right-hand side. */
      g_drag = DRAG_CONNECT;
      g_drag_shape = hit;
      g_conn_from_side = dgm_handle_side(g_doc, hit, cx, cy);
      break;
    default:
      g_drag = DRAG_MOVE; g_drag_shape = hit; undo_push();
      g_drag_ox = cx - g_doc->s[hit].x;
      g_drag_oy = cy - g_doc->s[hit].y;
      break;
    }
    return 1;
  }

  if (ev == GTCACA_MOUSE_DOUBLE) {
    /* The toolkit spotted a second click on the same cell. It comes after the
       press that started a move, so call that off and edit the label instead. */
    int hit = dgm_hit(g_doc, cx, cy);
    g_drag = DRAG_NONE;
    if (hit < 0 || g_tool != TOOL_SELECT) return 1;
    if (g_doc->s[hit].kind == DGM_CONN || g_doc->s[hit].kind == DGM_LINE) {
      snprintf(g_dgm_msg, sizeof g_dgm_msg, "A line carries no text — put a label beside it (t)");
      return 1;
    }
    sel_set(hit);
    begin_edit(hit);
    return 1;
  }

  if (ev == GTCACA_MOUSE_MOTION) {
    int pdx = g_drag_x1 - cx, pdy = g_drag_y1 - cy;
    g_drag_x1 = cx; g_drag_y1 = cy;
    switch (g_drag) {
    case DRAG_MOVE: {
      dgm_shape_t *s = &g_doc->s[g_drag_shape];
      int nx = cx - g_drag_ox, ny = cy - g_drag_oy;
      if (g_nmulti > 1 && sel_has(g_drag_shape))
        move_selection(nx - s->x, ny - s->y);    /* drag one, they all come */
      else {
        dgm_place(g_doc, g_drag_shape, nx, ny, s->w, s->h);
        g_modified = 1;
      }
      break;
    }
    case DRAG_RESIZE: {
      int ax = g_drag_ox, ay = g_drag_oy;        /* the corner that stays put */
      int nx = cx < ax ? cx : ax, ny = cy < ay ? cy : ay;
      int nw = (cx > ax ? cx - ax : ax - cx) + 1;
      int nh = (cy > ay ? cy - ay : ay - cy) + 1;
      dgm_place(g_doc, g_drag_shape, nx, ny, nw, nh);
      g_modified = 1;
      break;
    }
    case DRAG_PENCIL: {
      /* Same interpolation as the eraser: a quick sweep must not leave gaps. */
      int px = g_drag_x1 + pdx, py = g_drag_y1 + pdy;
      int dx = cx - px, dy = cy - py;
      int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
                ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy), i;
      uint32_t ch = (uint32_t)(unsigned char)PEN_CHARS[g_pen];
      if (steps > 512) steps = 512;
      for (i = 0; i <= steps; i++)
        draw_at(steps ? px + dx * i / steps : cx, steps ? py + dy * i / steps : cy, ch);
      break;
    }
    case DRAG_LINE:
    case DRAG_BAND:
      break;                                     /* drawn as a preview until released */
    case DRAG_LINE_END:
      dgm_line_set(g_doc, g_drag_shape, g_drag_ox, g_drag_oy, cx, cy);
      g_modified = 1;
      break;
    case DRAG_ERASE: {
      /* Terminals report motion in jumps, so rub out every cell between the
         last report and this one — otherwise a quick sweep leaves gaps. */
      int px = g_drag_x1 + pdx, py = g_drag_y1 + pdy;   /* where we were */
      int dx = cx - px, dy = cy - py;
      int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
                ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy), i;
      if (steps > 512) steps = 512;
      for (i = 0; i <= steps; i++)
        erase_at(steps ? px + dx * i / steps : cx,
                 steps ? py + dy * i / steps : cy);
      break;
    }
    case DRAG_PAN:
      g_vx += pdx; g_vy += pdy;
      if (g_vx < 0) g_vx = 0;
      if (g_vy < 0) g_vy = 0;
      g_drag_x1 = canvas_x(mx); g_drag_y1 = canvas_y(my);
      break;
    default:
      break;                                     /* NEWBOX / CONNECT: drawn as preview */
    }
    return 1;
  }

  /* release */
  if (g_drag == DRAG_BAND) {
    int bx, by, bw, bh;
    g_drag_x1 = cx; g_drag_y1 = cy;
    drag_rect(&bx, &by, &bw, &bh);
    g_drag = DRAG_NONE;
    select_in_rect(bx, by, bw, bh, g_band_add);
    if (g_nmulti > 1)
      snprintf(g_dgm_msg, sizeof g_dgm_msg,
               "%d objects selected — drag any of them to move them all, "
               "or click one to go back to just that one", g_nmulti);
    else if (g_nmulti == 0 && (bw > 1 || bh > 1))
      snprintf(g_dgm_msg, sizeof g_dgm_msg, "Nothing inside the band");
    return 1;
  }
  if (g_drag == DRAG_PENCIL) {
    int n = g_nstroke;
    g_drag = DRAG_NONE;
    undo_push_cells();
    if (n && !g_dgm_msg[0])
      snprintf(g_dgm_msg, sizeof g_dgm_msg, "Drew %d character%s  (u undoes)",
               n, n == 1 ? "" : "s");
    return 1;
  }
  if (g_drag == DRAG_LINE) {
    int idx;
    g_drag = DRAG_NONE;
    undo_push();
    idx = dgm_add_line(g_doc, g_drag_x0, g_drag_y0, cx, cy);
    apply_new_style(idx);
    if (idx >= 0) {
      sel_set(idx);
      g_modified = 1;
      snprintf(g_dgm_msg, sizeof g_dgm_msg,
               "Line drawn — drag it to move, drag an end to re-aim, Del removes it");
      palette_pick(0);
    } else snprintf(g_dgm_msg, sizeof g_dgm_msg, "Drag from one end of the line to the other");
    return 1;
  }
  if (g_drag == DRAG_LINE_END) {
    g_drag = DRAG_NONE;
    return 1;
  }
  if (g_drag == DRAG_ERASE) {
    int rubbed = 0, i;
    g_drag = DRAG_NONE;
    for (i = 0; i < g_nstroke; i++)              /* cells cleared, not ones flatten made */
      if (g_stroke[i].ch) rubbed++;
    undo_push_cells();
    if (rubbed)
      snprintf(g_dgm_msg, sizeof g_dgm_msg, "Erased %d character%s  (u puts them back)",
               rubbed, rubbed == 1 ? "" : "s");
    else if (!g_dgm_msg[0])
      snprintf(g_dgm_msg, sizeof g_dgm_msg, "Nothing to erase there");
    return 1;
  }
  if (g_drag == DRAG_NEWSHAPE) {
    int bx, by, bw, bh, dw, dh;
    g_drag_x1 = cx; g_drag_y1 = cy;
    drag_rect(&bx, &by, &bw, &bh);
    dgm_shape_default(g_shape, &dw, &dh);
    /* A click (no real drag) places the shape at its natural size — that is the
       friendly reading of "click here", and dragging still sizes it. */
    if (bw < 3 && bh < 3) { bw = dw; bh = dh; }
    undo_push();
    sel_set(dgm_add_node(g_doc, g_shape, bx, by, bw, bh, ""));
      apply_new_style(g_sel);
    g_modified = 1;
    g_drag = DRAG_NONE;
    palette_pick(0);                             /* one placement, then back to Select */
    if (g_sel >= 0) begin_edit(g_sel);
    return 1;
  }
  if (g_drag == DRAG_PALETTE) {
    int dw, dh;
    g_drag = DRAG_NONE;
    if (!on_canvas(mx, my)) return 1;            /* let go back over the pane */
    if (g_tool == TOOL_CONN) {
      /* Dropped on an object: that is the end the link starts from, so arm it
         and wait for the second click. Dropped on empty space it just leaves
         the tool armed, which is what the click on the row already did. */
      int from = dgm_hit(g_doc, cx, cy);
      if (from >= 0) {
        sel_set(from);
        g_conn_from = from;
        snprintf(g_dgm_msg, sizeof g_dgm_msg,
                 "Link from \"%s\": now click the object to link to  (C-g cancels)",
                 g_doc->s[from].label[0] ? g_doc->s[from].label : "that object");
      }
      return 1;
    }
    if (g_tool != TOOL_SHAPE && g_tool != TOOL_TEXT)
      return 1;                                  /* eraser, pen, line: drawn on the canvas */
    undo_push();
    if (g_tool == TOOL_TEXT) {
      sel_set(dgm_add_text(g_doc, cx, cy, ""));
    } else {
      dgm_shape_default(g_shape, &dw, &dh);
      sel_set(dgm_add_node(g_doc, g_shape, cx - dw / 2, cy - dh / 2, dw, dh, ""));
    }
    apply_new_style(g_sel);
    g_modified = 1;
    palette_pick(0);
    if (g_sel >= 0) begin_edit(g_sel);
    return 1;
  }
  if (g_drag == DRAG_CONNECT) {
    int target = dgm_hit(g_doc, cx, cy);
    int was_click = (cx == g_drag_x0 && cy == g_drag_y0);
    g_drag = DRAG_NONE;
    if (target >= 0 && target != g_drag_shape) {
      /* Where you let go picks the arriving edge, but only when the drag named
         a leaving edge too: a link made by clicking is still routed for you. */
      int to_side = g_conn_from_side ? dgm_nearest_side(g_doc, target, cx, cy)
                                     : DGM_SIDE_AUTO;
      link_objects_sides(g_drag_shape, target, g_conn_from_side, to_side);
      g_conn_from = -1;
      g_conn_from_side = DGM_SIDE_AUTO;
    } else if (was_click) {
      /* Not a drag but a click on the source: stay armed and wait for the
         second click, which is the easier gesture in a terminal. */
      g_conn_from = g_drag_shape;
      snprintf(g_dgm_msg, sizeof g_dgm_msg, "Now click the object to link to  (C-g cancels)");
    } else {
      g_conn_from = -1;
      snprintf(g_dgm_msg, sizeof g_dgm_msg,
               "Let go on top of another object to link the two");
    }
    return 1;
  }
  if (g_drag == DRAG_MOVE && g_click_sel >= 0 &&
      cx == g_drag_x0 && cy == g_drag_y0) {       /* a click inside the group */
    sel_set(g_click_sel);
    snprintf(g_dgm_msg, sizeof g_dgm_msg,
             "Just this one now — drag it to move it on its own");
  }
  g_click_sel = -1;
  g_drag = DRAG_NONE;
  return 1;
}

/* ── keys ────────────────────────────────────────────────────────────────────*/

/* Typing into the label editor or a prompt. Returns 1 if the key was eaten. */
static int typing_key(int key)
{
  char  *buf = g_editing ? g_edit_buf : g_prompt_buf;
  int   *len = g_editing ? &g_edit_len : &g_prompt_len;
  size_t cap = g_editing ? sizeof g_edit_buf : sizeof g_prompt_buf;

  switch (key) {
  case CACA_KEY_RETURN:
  case 10:
    if (g_editing) commit_edit(1);
    else { char tmp[PATH_MAX]; g_prompt = 0;
           snprintf(tmp, sizeof tmp, "%s", g_prompt_buf); export_file(tmp); }
    return 1;
  case CACA_KEY_ESCAPE:
  case 7:                                        /* C-g */
    if (g_editing) commit_edit(0);
    else { g_prompt = 0; snprintf(g_dgm_msg, sizeof g_dgm_msg, "Export cancelled"); }
    return 1;
  case CACA_KEY_BACKSPACE:
  case CACA_KEY_DELETE:
    while (*len > 0 && ((unsigned char)buf[*len - 1] & 0xc0) == 0x80) buf[--(*len)] = '\0';
    if (*len > 0) buf[--(*len)] = '\0';
    return 1;
  case 15:                                       /* C-o: a second label line */
    if (g_editing && (size_t)*len + 2 < cap) { buf[(*len)++] = '\n'; buf[*len] = '\0'; }
    return 1;
  default:
    break;
  }
  if (GTCACA_KEY_IS_UNICODE(key)) {              /* accented input */
    char utf[8];
    int n = key_to_utf8(key, utf), i;
    if (n > 0 && (size_t)(*len + n + 1) < cap)
      for (i = 0; i < n; i++) { buf[(*len)++] = utf[i]; buf[*len] = '\0'; }
    return 1;
  }
  if (key >= 32 && key < 127 && (size_t)*len + 2 < cap) {
    buf[(*len)++] = (char)key;
    buf[*len] = '\0';
    return 1;
  }
  return 1;                                      /* swallow everything while typing */
}

static int key_cb_inner(gtcaca_custom_widget_t *w, int key, void *ud)
{
  (void)w; (void)ud;
  if (!g_dgm_open) return 0;

  if (g_editing || g_prompt) return typing_key(key);

  if (g_confirm_quit) {
    switch (key) {
    case 'y': case 'Y': g_confirm_quit = 0; close_diagram(1); return 1;
    case 'n': case 'N': g_confirm_quit = 0; close_diagram(0); return 1;
    default:            g_confirm_quit = 0; g_dgm_msg[0] = '\0'; return 1;
    }
  }

  if (g_cx) {                                    /* the key after C-x */
    g_cx = 0;
    if (key == 'q' || key == 'Q') {              /* C-x q: the same question */
      if (ask_out_format()) close_diagram(1);
      else g_dgm_msg[0] = '\0';
      return 1;
    }
    snprintf(g_dgm_msg, sizeof g_dgm_msg, "C-x %c is undefined here — C-x q picks ASCII or Mermaid",
             key >= 32 && key < 127 ? key : '?');
    return 1;
  }
  if (key == 0x18) {                             /* C-x: a prefix in here too */
    g_cx = 1;
    return 1;
  }

  if (g_show_help && key != '?') { g_show_help = 0; return 1; }

  switch (key) {
  case 'q': case 3:                              /* q / C-c: put it in the buffer */
    if (!ask_out_format()) { g_dgm_msg[0] = '\0'; return 1; }
    close_diagram(1);
    return 1;
  case 'Q':                                      /* leave the drawing behind */
    if (g_modified) { g_confirm_quit = 1; return 1; }
    close_diagram(0);
    return 1;
  case CACA_KEY_ESCAPE:
  case 7:                                        /* C-g — the universal "never mind" */
    if (g_drag != DRAG_NONE) { g_drag = DRAG_NONE; return 1; }
    if (g_nmulti > 1) { sel_clear(); snprintf(g_dgm_msg, sizeof g_dgm_msg, "Selection cleared"); return 1; }
    if (g_conn_from >= 0) {
      g_conn_from = -1;
      snprintf(g_dgm_msg, sizeof g_dgm_msg, "Link cancelled");
      return 1;
    }
    if (g_tool != TOOL_SELECT) { palette_pick(0); return 1; }
    sel_clear();
    g_dgm_msg[0] = '\0';
    return 1;
  case '?': g_show_help = !g_show_help; return 1;
  case 'p':
    g_pal = !g_pal;
    snprintf(g_dgm_msg, sizeof g_dgm_msg,
             g_pal ? "Shape pane shown" : "Shape pane hidden — p brings it back");
    return 1;

  case 'e': palette_pick(12); return 1;          /* the eraser */
  case 'f': palette_pick(13); return 1;          /* freehand */
  case '-': palette_pick(14); return 1;          /* a plain line */
  case ' ':                                      /* pick the pen's character */
    if (g_tool == TOOL_PENCIL) {
      g_pen = (g_pen + 1) % (int)(sizeof PEN_CHARS - 1);
      snprintf(g_dgm_msg, sizeof g_dgm_msg, "Drawing with '%c'", PEN_CHARS[g_pen]);
    } else palette_pick(13);                     /* Space also reaches for the pen */
    return 1;
  case 'v': palette_pick(0); return 1;
  case 'b': palette_pick(1); return 1;           /* the plain box, the common case */
  case 't': palette_pick(10); return 1;
  case 'c': case 'l':
    /* With something selected, linking is a two-step you can do from the
       keyboard: press c, then Tab to the other object and press Enter. */
    if (g_sel >= 0 && g_doc->s[g_sel].kind != DGM_CONN) {
      g_conn_from = g_sel;
      snprintf(g_dgm_msg, sizeof g_dgm_msg,
               "Link: click the other object, or Tab to it and press Enter");
    } else palette_pick(11);
    return 1;

  case '0': case '1': case '2': case '3': case '4':
  case '5': case '6': case '7': case '8': case '9': {
    int i;
    for (i = 0; i < PALETTE_N; i++)
      if (PALETTE[i].key == key) { palette_pick(i); break; }
    return 1;
  }

  case CACA_KEY_TAB: select_next(1);  return 1;
  case 'm': {                                    /* add the next object to the set */
    int n = g_doc->n, i;
    if (n == 0) { snprintf(g_dgm_msg, sizeof g_dgm_msg, "Nothing to select yet"); return 1; }
    i = ((g_sel < 0 ? -1 : g_sel) + 1) % n;
    sel_add(i);
    scroll_to_sel();
    snprintf(g_dgm_msg, sizeof g_dgm_msg,
             "%d object%s selected — m adds the next, Tab starts again from one", g_nmulti,
             g_nmulti == 1 ? "" : "s");
    return 1;
  }
  case 'A': {                                    /* everything at once */
    int i;
    sel_clear();
    for (i = 0; i < g_doc->n; i++) sel_add(i);
    if (g_doc->n) g_sel = 0;
    snprintf(g_dgm_msg, sizeof g_dgm_msg, g_nmulti
             ? "All %d objects selected — arrows move them, C colours them, Del removes them"
             : "Nothing to select yet", g_nmulti);
    return 1;
  }
  case CACA_KEY_BACKSPACE:
  case CACA_KEY_DELETE: case 'd': {
    int n = g_nmulti;
    delete_selection();
    if (n > 1) snprintf(g_dgm_msg, sizeof g_dgm_msg, "Deleted %d objects  (u puts them back)", n);
    return 1;
  }
  case 'D':
    if (g_nmulti) {
      int i, last = -1, upto = g_doc->n;        /* copies land at the end: stop there */
      undo_push();
      for (i = 0; i < upto; i++)
        if (g_multi[i]) { int c = dgm_duplicate(g_doc, i); if (c >= 0) last = c; }
      if (last >= 0) {                          /* select the copies, not the originals */
        sel_clear();
        for (i = upto; i < g_doc->n; i++) sel_add(i);
        g_sel = upto;
        g_modified = 1;
        scroll_to_sel();
      }
    }
    return 1;
  case CACA_KEY_RETURN: case 10:
    if (g_conn_from >= 0) {                      /* the far end of a keyboard link */
      if (g_sel >= 0 && g_sel != g_conn_from) link_objects(g_conn_from, g_sel);
      else snprintf(g_dgm_msg, sizeof g_dgm_msg, "Tab to the object to link to first");
      g_conn_from = -1;
      return 1;
    }
    if (g_tool == TOOL_SHAPE || g_tool == TOOL_TEXT || g_sel < 0) {
      int dw, dh;                                /* place the armed shape at the cursor */
      undo_push();
      if (g_tool == TOOL_TEXT) {
        sel_set(dgm_add_text(g_doc, g_cur_x, g_cur_y, ""));
      } else {
        dgm_shape_default(g_shape, &dw, &dh);
        sel_set(dgm_add_node(g_doc, g_shape, g_cur_x, g_cur_y, dw, dh, ""));
      }
      apply_new_style(g_sel);
      g_modified = 1;
      palette_pick(0);
      if (g_sel >= 0) begin_edit(g_sel);
      return 1;
    }
    if (g_doc->s[g_sel].kind != DGM_CONN) begin_edit(g_sel);
    return 1;
  case CACA_KEY_INSERT: {                        /* same, but never edits a label */
    int dw, dh;
    undo_push();
    dgm_shape_default(g_shape, &dw, &dh);
    sel_set(dgm_add_node(g_doc, g_shape, g_cur_x, g_cur_y, dw, dh, ""));
      apply_new_style(g_sel);
    g_modified = 1;
    if (g_sel >= 0) begin_edit(g_sel);
    return 1;
  }
  case 'C': {                                    /* colour, from the swatch dialog */
    int cur = g_sel >= 0 ? g_doc->s[g_sel].fg : g_new_fg;
    int c = ask_colour("Colour", cur);
    if (c == -2) { snprintf(g_dgm_msg, sizeof g_dgm_msg, "Colour unchanged"); return 1; }
    style_selection(c, -1, -1);
    snprintf(g_dgm_msg, sizeof g_dgm_msg, "%s in %s%s", g_sel >= 0 ? "Drawn" : "New objects",
             gtcaca_color_name(c), g_sel >= 0 ? "" : " from now on");
    return 1;
  }
  case 'B': {                                    /* background behind it */
    int cur = g_sel >= 0 ? g_doc->s[g_sel].bg : g_new_bg;
    int c = ask_colour("Background", cur);
    if (c == -2) { snprintf(g_dgm_msg, sizeof g_dgm_msg, "Background unchanged"); return 1; }
    style_selection(-1, c, -1);
    snprintf(g_dgm_msg, sizeof g_dgm_msg, "Background %s", gtcaca_color_name(c));
    return 1;
  }
  case 'S': {                                    /* solid → dashed → dotted */
    int cur = (g_sel >= 0 ? g_doc->s[g_sel].stroke : g_new_stroke);
    int next = (cur + 1) % DGM_NSTROKES;
    style_selection(-1, -1, next);
    snprintf(g_dgm_msg, sizeof g_dgm_msg, "%s stroke%s", dgm_stroke_name(next),
             g_sel >= 0 && g_doc->s[g_sel].kind == DGM_NODE
               ? " (shapes stay solid — it shows on lines and links)" : "");
    return 1;
  }
  case 'u': undo_pop(); return 1;
  case 'x':                                      /* an object → plain characters */
    if (g_sel >= 0 && g_doc->s[g_sel].kind != DGM_CONN) {
      const char *what = g_doc->s[g_sel].kind == DGM_TEXT ? "label"
                       : dgm_shape_name(g_doc->s[g_sel].shape);
      int n = flatten_selection();
      if (n) {
        if (n > 1)
          snprintf(g_dgm_msg, sizeof g_dgm_msg,
                   "%d objects are plain characters now — the eraser can rub bits out (u undoes)", n);
        else
          snprintf(g_dgm_msg, sizeof g_dgm_msg,
                   "That %s is plain characters now — the eraser can rub bits out (u undoes)", what);
      }
    } else snprintf(g_dgm_msg, sizeof g_dgm_msg,
                    "Select a shape or label first: x turns it into plain characters");
    return 1;

  case 'a':
    if (g_sel >= 0 && g_doc->s[g_sel].kind == DGM_CONN) {
      dgm_shape_t *s = &g_doc->s[g_sel];
      undo_push();
      /* to → both → from → none → to */
      if (s->arrow_to && !s->arrow_from)      s->arrow_from = 1;
      else if (s->arrow_to && s->arrow_from)  s->arrow_to = 0;
      else if (s->arrow_from)                 s->arrow_from = 0;
      else                                    s->arrow_to = 1;
      g_modified = 1;
    } else snprintf(g_dgm_msg, sizeof g_dgm_msg, "Select a link first (click its line)");
    return 1;
  case 'r':                                      /* redraw the selection as another shape */
    if (g_sel >= 0 && g_doc->s[g_sel].kind == DGM_NODE) {
      /* The primary picks the next shape and the rest of the set follows it, so
         a group of mixed shapes ends up as one shape rather than each drifting
         a step along on its own. */
      int next = (g_doc->s[g_sel].shape + 1) % DGM_NSHAPES, i;
      undo_push();
      for (i = 0; i < g_doc->n; i++)
        if (g_multi[i] && g_doc->s[i].kind == DGM_NODE) dgm_set_shape(g_doc, i, next);
      g_modified = 1;
      snprintf(g_dgm_msg, sizeof g_dgm_msg, g_nmulti > 1 ? "All now %ss" : "Now a %s",
               dgm_shape_name(next));
    } else snprintf(g_dgm_msg, sizeof g_dgm_msg, "Select an object first, then r changes its shape");
    return 1;
  case 's':
    g_doc->style = g_doc->style == DGM_STYLE_ASCII ? DGM_STYLE_UNICODE : DGM_STYLE_ASCII;
    g_modified = 1;
    snprintf(g_dgm_msg, sizeof g_dgm_msg, "%s style",
             g_doc->style == DGM_STYLE_UNICODE ? "Unicode" : "ASCII");
    return 1;

  case 0x17:                                     /* C-w: a copy in a file of its own */
    start_export();
    return 1;
  case KEY_CTRL_S:                               /* the buffer is saved the usual way */
    snprintf(g_dgm_msg, sizeof g_dgm_msg,
             "q puts the diagram in the buffer — then C-x C-s saves the file as usual");
    return 1;

  case '<': case ',': resize_selection(-1, 0); return 1;
  case '>': case '.': resize_selection(1, 0);  return 1;
  case '[':           resize_selection(0, -1); return 1;
  case ']':           resize_selection(0, 1);  return 1;

  case CACA_KEY_LEFT: case CACA_KEY_RIGHT: case CACA_KEY_UP: case CACA_KEY_DOWN: {
    int dx = (key == CACA_KEY_RIGHT) - (key == CACA_KEY_LEFT);
    int dy = (key == CACA_KEY_DOWN)  - (key == CACA_KEY_UP);
    if (g_nmulti) {
      undo_push();
      move_selection(dx, dy);
      scroll_to_sel();
    } else {                                     /* drive the keyboard cursor instead */
      g_cur_x += dx; g_cur_y += dy;
      if (g_cur_x < 0) g_cur_x = 0;
      if (g_cur_y < 0) g_cur_y = 0;
      if (g_cur_x >= DGM_W) g_cur_x = DGM_W - 1;
      if (g_cur_y >= DGM_H) g_cur_y = DGM_H - 1;
      scroll_to(g_cur_x, g_cur_y);
    }
    return 1;
  }
  case CACA_KEY_HOME: g_vx = g_vy = 0; return 1;
  default:
    break;
  }
  return 1;                                      /* the canvas owns every key */
}

/* The modeline is a widget of its own, and gtcaca paints it before the canvas —
   so text set from inside the canvas's draw callback only reaches the screen on
   the *following* frame, leaving the counts (and "3 selected") a keystroke
   behind whatever you just did. Setting it here, as the event is handled and
   before anything is painted, is what keeps it telling the truth. */
static int key_cb(gtcaca_custom_widget_t *w, int key, void *ud)
{
  int r = key_cb_inner(w, key, ud);
  if (g_dgm_open) dgm_status();
  return r;
}

static int mouse_cb(gtcaca_custom_widget_t *w, gtcaca_mouse_event_t ev,
                    int mx, int my, int button, void *ud)
{
  int r = mouse_cb_inner(w, ev, mx, my, button, ud);
  if (g_dgm_open) dgm_status();
  return r;
}

/* ── entry point ─────────────────────────────────────────────────────────────*/

/* Can the terminal show box-drawing glyphs? The only real constraint is a
   UTF-8 locale: ─ │ ╭ ╮ have been in terminal fonts for decades, unlike the
   fractional-block glyphs gtcaca_terminal_supports_blocks() gates on, so that
   check is too strict here — it would leave a "rounded" box drawn as `.--.`
   on a perfectly capable terminal. `s` overrides either way. */
static int locale_is_utf8(void)
{
  const char *loc = getenv("LC_ALL");
  if (!loc || !*loc) loc = getenv("LC_CTYPE");
  if (!loc || !*loc) loc = getenv("LANG");
  if (!loc) return 0;
  return strstr(loc, "UTF-8") || strstr(loc, "utf-8") ||
         strstr(loc, "UTF8")  || strstr(loc, "utf8") ? 1 : 0;
}

/* Does this text fit on the canvas, in both directions? Columns count
   codepoints, matching how the parser lays bytes out in cells. */
static int text_fits(const char *text)
{
  int col = 0, row = 0;
  const char *p;
  for (p = text; *p; p++) {
    if (*p == '\n') { row++; col = 0; if (row >= DGM_H) return 0; continue; }
    if (*p == '\r') continue;
    if (*p == '\t') { col = (col + 8) & ~7; }
    else if (((unsigned char)*p & 0xc0) != 0x80) col++;
    if (col >= DGM_W) return 0;
  }
  return 1;
}

static int build_widgets(void)
{
  int cw = caca_get_canvas_width(gmo.cv);
  int ch = caca_get_canvas_height(gmo.cv);

  if (g_dw) return 0;
  g_dw = gtcaca_window_new(NULL, "Diagram", 0, 0, cw, ch - 1);
  if (!g_dw) return -1;
  g_dview = gtcaca_custom_new(GTCACA_WIDGET(g_dw), 0, 0, 1, 1);
  if (!g_dview) return -1;
  gtcaca_custom_set_draw_cb(g_dview, draw_cb, NULL);
  gtcaca_custom_set_key_cb(g_dview, key_cb, NULL);
  gtcaca_custom_set_mouse_cb(g_dview, mouse_cb, NULL);
  { gtcaca_box_t *b = gtcaca_vbox_new();
    gtcaca_box_set_margin(b, 0);
    gtcaca_box_add_expand(b, GTCACA_WIDGET(g_dview));
    gtcaca_box_apply(b, g_dw->x, g_dw->y, g_dw->width, g_dw->height);
    gtcaca_box_free(b); }
  gtcaca_widget_hide(GTCACA_WIDGET(g_dview));
  gtcaca_widget_hide(GTCACA_WIDGET(g_dw));
  return 0;
}

int diagram_paste(const char *text, int len)
{
  int i;
  if (!g_dgm_open) return 0;
  if (!g_editing && !g_prompt) {                 /* nothing here takes free text */
    snprintf(g_dgm_msg, sizeof g_dgm_msg,
             "Nothing to paste into — press Ret on an object to type its label");
    return 1;
  }
  for (i = 0; i < len; i++) {
    unsigned char c = (unsigned char)text[i];
    if (c == '\n' || c == '\r') continue;        /* labels are typed a line at a time */
    typing_key(c);
  }
  return 1;
}

void run_diagram(void)
{
  char *text = NULL;
  int i;

  if (g_dgm_open) return;
  if (!g_doc) {
    g_doc = dgm_new();
    g_grid.w = DGM_W; g_grid.h = DGM_H;
    g_grid.cell = malloc((size_t)g_grid.w * g_grid.h * sizeof *g_grid.cell);
    g_grid.fg   = malloc((size_t)g_grid.w * g_grid.h);
    g_grid.bg   = malloc((size_t)g_grid.w * g_grid.h);
    if (!g_doc || !g_grid.cell || !g_grid.fg || !g_grid.bg) {
      snprintf(g_message, sizeof g_message, "Diagram: out of memory");
      return;
    }
  }
  if (build_widgets() != 0) {
    snprintf(g_message, sizeof g_message, "Diagram: cannot create the canvas");
    return;
  }

  /* Where the drawing will land, and what it starts from. A selection is taken
     as an existing diagram to edit — mark it with C-Space, then M-x diagram —
     and is replaced when the mode closes. With no selection the canvas starts
     empty and the drawing is inserted at the cursor, leaving the rest of the
     buffer alone. */
  dgm_clear(g_doc);
  g_ins_start = g_ins_end = 0;
  if (g_ed) {
    int sel_start = gtcaca_editor_get_selection_start(g_ed);
    int sel_end   = gtcaca_editor_get_selection_end(g_ed);
    g_ins_start = g_ins_end = gtcaca_editor_get_current_pos(g_ed);
    if (sel_end > sel_start) {
      int len = sel_end - sel_start;
      if ((size_t)len >= (size_t)DGM_W * DGM_H * 4) {
        snprintf(g_message, sizeof g_message,
                 "Selection is larger than the %dx%d diagram canvas", DGM_W, DGM_H);
        return;
      }
      text = malloc((size_t)len + 1);
      if (text) {
        gtcaca_editor_get_text_range(g_ed, sel_start, sel_end, text, len + 1);
        /* Anything past the canvas would be dropped on the way in and lost on
           the way out, so decline instead. */
        if (!text_fits(text)) {
          snprintf(g_message, sizeof g_message,
                   "Selection is larger than the %dx%d diagram canvas", DGM_W, DGM_H);
          free(text);
          return;
        }
        dgm_parse_text(g_doc, text);
        free(text);
        g_ins_start = sel_start;
        g_ins_end   = sel_end;
      }
    }
  }
  g_mark_active = 0;

  /* A fresh diagram draws with box-drawing glyphs where the terminal can show
     them — that is what makes a "rounded" box actually look rounded. Art read
     from a file keeps whatever it was drawn with, and `s` flips between the
     two at any time. */
  if (g_doc->n == 0 && !g_doc->has_raw)
    g_doc->style = locale_is_utf8() ? DGM_STYLE_UNICODE : DGM_STYLE_ASCII;

  sel_clear();
  g_tool = TOOL_SELECT;
  g_pal_sel = 0;
  g_shape = DGM_RECT;
  g_conn_from = -1;
  g_cur_x = g_cur_y = 2;
  g_vx = g_vy = 0;
  g_drag = DRAG_NONE;
  g_editing = g_prompt = g_confirm_quit = g_show_help = 0;
  g_cx = 0;
  g_out = g_cfg_dgm_mermaid ? OUT_MERMAID : OUT_ASCII;
  g_modified = 0;
  undo_clear();
  if (g_doc->n)
    snprintf(g_dgm_msg, sizeof g_dgm_msg,
             "Editing the %d object%s in your selection — q puts them back when you are done",
             g_doc->n, g_doc->n == 1 ? "" : "s");
  else
    snprintf(g_dgm_msg, sizeof g_dgm_msg,
             "Empty canvas — draw, then q drops it into the buffer at your cursor (? for help)");

  pane_hide_all();
  for (i = 0; i < g_nbuf; i++) if (g_buffers[i].ed) g_buffers[i].ed->has_focus = 0;
  for (i = 0; i < MAXLEAVES; i++) if (g_winpool[i]) g_winpool[i]->has_focus = 0;
  if (g_browser_win) g_browser_win->has_focus = 0;
  if (g_browser_ed)  g_browser_ed->has_focus = 0;
  if (g_help_win)    g_help_win->has_focus = 0;
  if (g_help_ed)     g_help_ed->has_focus = 0;

  gtcaca_widget_show(GTCACA_WIDGET(g_dw));
  gtcaca_widget_show(GTCACA_WIDGET(g_dview));
  widget_to_front(GTCACA_WIDGET(g_dw));
  widget_to_front(GTCACA_WIDGET(g_dview));
  g_dw->has_focus = 1;
  g_dw->focused_child = GTCACA_WIDGET(g_dview);
  g_dview->has_focus = 1;
  g_dgm_open = 1;
  dgm_status();
}

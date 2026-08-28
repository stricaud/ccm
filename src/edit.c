#include "cacamacs.h"
/* ── kill ring (single register) ───────────────────────────────────────────── */

/* The kill ring, and the system clipboard with it: killing or copying here
   should be pasteable in a browser, and vice versa. gtcaca picks whichever
   clipboard the machine actually has (pbcopy, wl-copy, xclip, the Win32 API)
   and falls back to OSC 52 so it still works over ssh. */
void kill_set(const char *text, int len)
{
  free(g_killbuf);
  g_killbuf = malloc((size_t)len + 1);
  if (g_killbuf) { memcpy(g_killbuf, text, (size_t)len); g_killbuf[len] = '\0'; }
  gtcaca_clipboard_set(text, len);
}

/* ── commands ──────────────────────────────────────────────────────────────── */


/* Move, extending the selection instead when the mark is active. */
void move(gtcaca_editor_widget_t *ed, mv_t plain, mv_t extend)
{
  if (g_mark_active) extend(ed);
  else               plain(ed);
}

void set_mark(gtcaca_editor_widget_t *ed)
{
  gtcaca_editor_set_rectangular_selection(ed, 0);   /* plain mark = linear region */
  gtcaca_editor_set_empty_selection(ed, gtcaca_editor_get_current_pos(ed));
  g_mark_active = 1;
  snprintf(g_message, sizeof(g_message), "Mark set");
}

/* C-x SPC — like set-mark, but the region is a rectangular column box */
void set_rectangle_mark(gtcaca_editor_widget_t *ed)
{
  gtcaca_editor_set_empty_selection(ed, gtcaca_editor_get_current_pos(ed));
  gtcaca_editor_set_rectangular_selection(ed, 1);
  g_mark_active = 1;
  snprintf(g_message, sizeof(g_message), "Rectangle mark set — move, then C-x r t/k/y/d/c");
}

void keyboard_quit(gtcaca_editor_widget_t *ed)
{
  gtcaca_editor_set_rectangular_selection(ed, 0);
  gtcaca_editor_set_empty_selection(ed, gtcaca_editor_get_current_pos(ed));
  g_mark_active = 0;
  snprintf(g_message, sizeof(g_message), "Quit");
}

void kill_region(gtcaca_editor_widget_t *ed)
{
  int a, b;
  if (gtcaca_editor_get_rectangular_selection(ed)) {   /* C-w on a rectangle */
    gtcaca_editor_rect_kill(ed);
    g_mark_active = 0;
    snprintf(g_message, sizeof g_message, "Killed rectangle");
    return;
  }
  a = gtcaca_editor_get_selection_start(ed);
  b = gtcaca_editor_get_selection_end(ed);
  if (a == b) { snprintf(g_message, sizeof(g_message), "No region"); return; }
  {
    int n = b - a;
    char *buf = malloc((size_t)n + 1);
    if (buf) { gtcaca_editor_get_text_range(ed, a, b, buf, n + 1); kill_set(buf, n); free(buf); }
  }
  gtcaca_editor_delete_range(ed, a, b - a);
  gtcaca_editor_set_empty_selection(ed, a);
  g_mark_active = 0;
}

/* M-w: copy the region to the kill ring without deleting it. */
void copy_region(gtcaca_editor_widget_t *ed)
{
  int a, b;
  if (gtcaca_editor_get_rectangular_selection(ed)) {   /* M-w on a rectangle */
    gtcaca_editor_rect_copy(ed);                        /* -> rectangle clipboard (C-x r y) */
    g_mark_active = 0;
    snprintf(g_message, sizeof g_message, "Copied rectangle");
    return;
  }
  a = gtcaca_editor_get_selection_start(ed);
  b = gtcaca_editor_get_selection_end(ed);
  if (a == b) { snprintf(g_message, sizeof g_message, "No region"); return; }
  {
    int n = b - a;
    char *buf = malloc((size_t)n + 1);
    if (buf) { gtcaca_editor_get_text_range(ed, a, b, buf, n + 1); kill_set(buf, n); free(buf); }
  }
  gtcaca_editor_set_empty_selection(ed, gtcaca_editor_get_current_pos(ed));
  g_mark_active = 0;
  snprintf(g_message, sizeof g_message, "Saved to kill ring");
}

/* M-d / M-Backspace: kill the word forward/back, saving it to the kill ring. */
void kill_word_right(gtcaca_editor_widget_t *ed)
{
  int from = gtcaca_editor_get_current_pos(ed);
  int to   = gtcaca_editor_word_right_position(ed, from);
  if (to > from) { int n = to - from; char *b = malloc((size_t)n + 1); if (b) { gtcaca_editor_get_text_range(ed, from, to, b, n + 1); kill_set(b, n); free(b); } gtcaca_editor_del_word_right(ed); }
}
void kill_word_left(gtcaca_editor_widget_t *ed)
{
  int to   = gtcaca_editor_get_current_pos(ed);
  int from = gtcaca_editor_word_left_position(ed, to);
  if (from < to) { int n = to - from; char *b = malloc((size_t)n + 1); if (b) { gtcaca_editor_get_text_range(ed, from, to, b, n + 1); kill_set(b, n); free(b); } gtcaca_editor_del_word_left(ed); }
}

/* M-u / M-l: upcase/downcase the region, or the word forward from point. */
void case_word(gtcaca_editor_widget_t *ed, int upper)
{
  if (gtcaca_editor_get_selection_start(ed) == gtcaca_editor_get_selection_end(ed)) {
    int from = gtcaca_editor_get_current_pos(ed);
    int to   = gtcaca_editor_word_right_position(ed, from);
    if (to <= from) return;
    gtcaca_editor_set_selection(ed, to, from);
    if (upper) gtcaca_editor_upper_case(ed); else gtcaca_editor_lower_case(ed);
    gtcaca_editor_set_empty_selection(ed, to);
  } else {
    if (upper) gtcaca_editor_upper_case(ed); else gtcaca_editor_lower_case(ed);
  }
}

/* C-t: transpose the two characters around point (Emacs transpose-chars). */
void transpose_chars(gtcaca_editor_widget_t *ed)
{
  int p = gtcaca_editor_get_current_pos(ed), len = gtcaca_editor_get_length(ed), a, b;
  char ca, cb, two[3];
  if (len < 2) return;
  if (p >= len) { a = len - 2; b = len - 1; } else if (p == 0) { a = 0; b = 1; } else { a = p - 1; b = p; }
  ca = gtcaca_editor_get_char_at(ed, a); cb = gtcaca_editor_get_char_at(ed, b);
  if (ca == '\n' || cb == '\n') return;
  two[0] = cb; two[1] = ca; two[2] = '\0';
  gtcaca_editor_delete_range(ed, a, 2);
  gtcaca_editor_insert_text(ed, a, two);
  gtcaca_editor_set_empty_selection(ed, (p >= len) ? p : p + 1);
}

/* ── comment / uncomment (M-;), Emacs comment-dwim ─────────────────────────── */

/* The line-comment prefix for the current file (from the language config, with
   a small fallback by extension). NULL if unknown. */
const char *line_comment_for_ext(const char *ext)
{
  if (!ext) return NULL;
  if (!strcmp(ext, ".c") || !strcmp(ext, ".h") || !strcmp(ext, ".cpp") || !strcmp(ext, ".cc") ||
      !strcmp(ext, ".hpp") || !strcmp(ext, ".js") || !strcmp(ext, ".mjs") || !strcmp(ext, ".ts") ||
      !strcmp(ext, ".java") || !strcmp(ext, ".go") || !strcmp(ext, ".rs") ||
      !strcmp(ext, ".json") || !strcmp(ext, ".jsonc")) return "//";
  if (!strcmp(ext, ".py") || !strcmp(ext, ".pyw") || !strcmp(ext, ".sh") || !strcmp(ext, ".rb") ||
      !strcmp(ext, ".yaml") || !strcmp(ext, ".yml") || !strcmp(ext, ".toml") ||
      !strcmp(ext, ".cfg") || !strcmp(ext, ".conf")) return "#";
  if (!strcmp(ext, ".lua") || !strcmp(ext, ".sql")) return "--";
  if (!strcmp(ext, ".el") || !strcmp(ext, ".lisp") || !strcmp(ext, ".scm") || !strcmp(ext, ".clj")) return ";;";
  return NULL;
}

const char *current_line_comment(void)
{
  const char *lc = g_langcfg ? gtcaca_editor_langcfg_get_line_comment(g_langcfg) : NULL;
  if (lc && lc[0]) return lc;
  return line_comment_for_ext(g_filename ? strrchr(g_filename, '.') : NULL);
}

int line_first_nonws(gtcaca_editor_widget_t *ed, int line)
{
  int p = gtcaca_editor_position_from_line(ed, line);
  int e = gtcaca_editor_get_line_end_position(ed, line);
  while (p < e) { char c = gtcaca_editor_get_char_at(ed, p); if (c != ' ' && c != '\t') break; p++; }
  return p;
}
int line_is_blank(gtcaca_editor_widget_t *ed, int line)
{
  return line_first_nonws(ed, line) >= gtcaca_editor_get_line_end_position(ed, line);
}
int line_is_commented(gtcaca_editor_widget_t *ed, int line, const char *lc, int lclen)
{
  int p = line_first_nonws(ed, line), i;
  if (p + lclen > gtcaca_editor_get_line_end_position(ed, line)) return 0;
  for (i = 0; i < lclen; i++) if (gtcaca_editor_get_char_at(ed, p + i) != lc[i]) return 0;
  return 1;
}

void comment_dwim(gtcaca_editor_widget_t *ed)
{
  const char *lc = current_line_comment();
  int lclen, first, last, L, any_content = 0, any_uncommented = 0, do_comment;
  char cstr[40];

  if (!lc || !lc[0]) { snprintf(g_message, sizeof g_message, "No comment syntax for this file"); return; }
  lclen = (int)strlen(lc);

  if (gtcaca_editor_get_selection_start(ed) != gtcaca_editor_get_selection_end(ed)) {
    int a = gtcaca_editor_get_selection_start(ed), b = gtcaca_editor_get_selection_end(ed);
    first = gtcaca_editor_line_from_position(ed, a);
    last  = gtcaca_editor_line_from_position(ed, b);
    if (last > first && b == gtcaca_editor_position_from_line(ed, last)) last--;  /* trailing line at col 0 */
  } else {
    first = last = gtcaca_editor_get_current_line(ed);
  }

  /* comment unless every non-blank line in the range is already commented */
  for (L = first; L <= last; L++) {
    if (line_is_blank(ed, L)) continue;
    any_content = 1;
    if (!line_is_commented(ed, L, lc, lclen)) any_uncommented = 1;
  }
  do_comment = !any_content || any_uncommented;

  snprintf(cstr, sizeof cstr, "%s ", lc);

  /* edit bottom-to-top so earlier line positions stay valid */
  for (L = last; L >= first; L--) {
    int p = line_first_nonws(ed, L);
    if (do_comment) {
      if (line_is_blank(ed, L)) continue;
      gtcaca_editor_insert_text(ed, p, cstr);
    } else if (line_is_commented(ed, L, lc, lclen)) {
      int del = lclen;
      if (gtcaca_editor_get_char_at(ed, p + lclen) == ' ') del++;   /* eat one following space */
      gtcaca_editor_delete_range(ed, p, del);
    }
  }
  snprintf(g_message, sizeof g_message, "%s %d line%s",
           do_comment ? "Commented" : "Uncommented", last - first + 1, last == first ? "" : "s");
}

/* ── M-x insert-date / insert-time ─────────────────────────────────────────────
 *
 * ISO 8601 order, "2026-08-11" and "2026-08-11 14:03:27", in local time. Sorts
 * correctly as text, which is the point of writing a date this way round.
 * Inserted at point; a region is left alone, only deselected.
 */
static void insert_stamp(gtcaca_editor_widget_t *ed, const char *fmt)
{
  char stamp[64];
  time_t now = time(NULL);
  struct tm tmv;
  int pos;
  size_t n;

#ifdef _WIN32
  { struct tm *p = localtime(&now); if (!p) return; tmv = *p; }
#else
  if (!localtime_r(&now, &tmv)) return;
#endif
  n = strftime(stamp, sizeof stamp, fmt, &tmv);
  if (n == 0) return;

  pos = gtcaca_editor_get_current_pos(ed);
  gtcaca_editor_insert_text(ed, pos, stamp);
  gtcaca_editor_set_empty_selection(ed, pos + (int)n);
  g_mark_active = 0;
  snprintf(g_message, sizeof g_message, "Inserted %s", stamp);
}

void insert_date(gtcaca_editor_widget_t *ed) { insert_stamp(ed, "%Y-%m-%d"); }
void insert_time(gtcaca_editor_widget_t *ed) { insert_stamp(ed, "%Y-%m-%d %H:%M:%S"); }

/* show-paren: highlight the brace under/just-before the caret and its match. */
void show_paren(gtcaca_editor_widget_t *ed)
{
  int pos = gtcaca_editor_get_current_pos(ed), bp = -1;
  char c0 = gtcaca_editor_get_char_at(ed, pos);
  char c1 = pos > 0 ? gtcaca_editor_get_char_at(ed, pos - 1) : 0;
  if (c0 && strchr("()[]{}", c0))      bp = pos;
  else if (c1 && strchr("()[]{}", c1)) bp = pos - 1;
  if (bp >= 0) {
    int m = gtcaca_editor_brace_match(ed, bp);
    if (m >= 0) gtcaca_editor_brace_highlight(ed, bp, m);
    else        gtcaca_editor_brace_badlight(ed, bp);
  } else gtcaca_editor_brace_highlight(ed, -1, -1);
}

void kill_line(gtcaca_editor_widget_t *ed)
{
  int cur  = gtcaca_editor_get_current_pos(ed);
  int line = gtcaca_editor_get_current_line(ed);
  int eol  = gtcaca_editor_get_line_end_position(ed, line);
  int len  = gtcaca_editor_get_length(ed);
  int from = cur, to;

  if (cur < eol)        to = eol;        /* kill to end of line  */
  else if (cur < len)   to = cur + 1;    /* at EOL: kill newline */
  else                  return;          /* end of buffer        */

  {
    int n = to - from;
    char *buf = malloc((size_t)n + 1);
    if (buf) { gtcaca_editor_get_text_range(ed, from, to, buf, n + 1); kill_set(buf, n); free(buf); }
  }
  gtcaca_editor_delete_range(ed, from, to - from);
}

/* Yank what was last copied — from the system clipboard when that holds
   something newer than our own kill ring, so copying in another application and
   pressing C-y here does what you meant. */
void yank(gtcaca_editor_widget_t *ed)
{
  char *outside = gtcaca_clipboard_get();
  const char *text = g_killbuf;
  int pos;

  if (outside && (!g_killbuf || strcmp(outside, g_killbuf) != 0)) text = outside;
  if (!text || !text[0]) {
    free(outside);
    snprintf(g_message, sizeof(g_message), "Nothing to yank");
    return;
  }
  pos = gtcaca_editor_get_current_pos(ed);
  gtcaca_editor_insert_text(ed, pos, text);
  gtcaca_editor_goto_pos(ed, pos + (int)strlen(text));
  g_mark_active = 0;
  if (text == outside)
    snprintf(g_message, sizeof(g_message), "Yanked from the system clipboard");
  free(outside);
}

void exchange_point_and_mark(gtcaca_editor_widget_t *ed)
{
  int point  = gtcaca_editor_get_current_pos(ed);
  int anchor = gtcaca_editor_get_anchor(ed);
  gtcaca_editor_set_selection(ed, anchor, point);  /* caret <- anchor, anchor <- point */
  g_mark_active = 1;
}

void recenter(gtcaca_editor_widget_t *ed)
{
  int line = gtcaca_editor_get_current_line(ed);
  /* rows of text in the view; fall back to the widget height if we have not
     been drawn yet, so the very first C-l still centres. */
  int vh   = ed->view_lines > 0 ? ed->view_lines : ed->height - 2;
  int top  = line - (vh > 0 ? vh / 2 : 0);
  ed->first_visible_line = top < 0 ? 0 : top;   /* near the top it can't centre */
  snprintf(g_message, sizeof g_message, "Recentered");
}

/* ── view toggles (line numbers, folding, annotations) ─────────────────────── */

void toggle_wrap(gtcaca_editor_widget_t *ed)
{
  int on = !gtcaca_editor_get_wrap(ed);
  gtcaca_editor_set_wrap(ed, on);
  snprintf(g_message, sizeof g_message, "Line wrap %s", on ? "on" : "off (lines scroll)");
}

void toggle_line_numbers(gtcaca_editor_widget_t *ed)
{
  int on = !gtcaca_editor_get_line_numbers(ed);
  gtcaca_editor_set_line_numbers(ed, on);
  snprintf(g_message, sizeof g_message, "Line numbers %s", on ? "on" : "off");
}

void toggle_folding(gtcaca_editor_widget_t *ed)
{
  g_folding = !g_folding;
  gtcaca_editor_set_fold_margin(ed, g_folding);
  if (g_folding) gtcaca_editor_fold_by_indentation(ed);
  else           gtcaca_editor_fold_all(ed, 1);   /* reveal everything */
  snprintf(g_message, sizeof g_message, "Folding %s", g_folding ? "on" : "off");
}

void toggle_fold_here(gtcaca_editor_widget_t *ed)
{
  if (!g_folding) { toggle_folding(ed); return; }
  gtcaca_editor_toggle_fold(ed, gtcaca_editor_get_current_line(ed));
}

void toggle_annotation_here(gtcaca_editor_widget_t *ed)
{
  int line = gtcaca_editor_get_current_line(ed);
  char buf[8];
  if (gtcaca_editor_annotation_get_text(ed, line, buf, sizeof buf) > 0) {
    gtcaca_editor_annotation_set_text(ed, line, NULL);
    snprintf(g_message, sizeof g_message, "Annotation removed");
  } else {
    gtcaca_editor_annotation_set_text(ed, line, "note: this line is annotated");
    gtcaca_editor_annotation_set_style(ed, line, GTCACA_EDITOR_STYLE_COMMENT);
    snprintf(g_message, sizeof g_message, "Annotation added");
  }
}

static int xml_prettify(const char *src, char **out);   /* defined below */

/* Is the whole text a single JSON value or a well-formed XML document? Used to
   decide whether C-x p treats the buffer as one document (vs the current line). */
static int whole_is_doc(const char *all)
{
  gtcaca_json_value *j = gtcaca_json_parse(all);
  if (j) { gtcaca_json_free(j); return 1; }
  { char *x = NULL; if (xml_prettify(all, &x)) { free(x); return 1; } }
  return 0;
}

/* C-x p: pretty-print JSON *or* XML, auto-detecting which. Operates on the
   selection, else the whole buffer when it is a single JSON value or a
   well-formed XML document, else the current line (handy for a JSONL/one-line
   record). Errors (no edit) if the target is neither JSON nor XML. */
void pretty_print_auto(gtcaca_editor_widget_t *ed)
{
  int a, b, n;
  char *src, *pretty = NULL;
  const char *kind = NULL;
  gtcaca_json_value *v;

  if (!ed) { snprintf(g_message, sizeof g_message, "No buffer"); return; }

  if (gtcaca_editor_get_selection_start(ed) != gtcaca_editor_get_selection_end(ed)) {
    a = gtcaca_editor_get_selection_start(ed);
    b = gtcaca_editor_get_selection_end(ed);
  } else {
    int len = gtcaca_editor_get_length(ed);
    char *all = malloc((size_t)len + 1);
    int whole = 0;
    if (all) { gtcaca_editor_get_text(ed, all, len + 1); whole = whole_is_doc(all); free(all); }
    if (whole) { a = 0; b = len; }
    else {
      int line = gtcaca_editor_get_current_line(ed);
      a = gtcaca_editor_position_from_line(ed, line);
      b = gtcaca_editor_get_line_end_position(ed, line);
    }
  }

  n = b - a;
  if (n <= 0) { snprintf(g_message, sizeof g_message, "Nothing to pretty-print"); return; }
  src = malloc((size_t)n + 1);
  if (!src) return;
  gtcaca_editor_get_text_range(ed, a, b, src, n + 1);

  /* JSON first (its parser is stricter, so it won't misread markup), then XML. */
  v = gtcaca_json_parse(src);
  if (v) { pretty = gtcaca_json_stringify(v, 2); gtcaca_json_free(v); if (pretty) kind = "JSON"; }
  if (!kind && xml_prettify(src, &pretty)) kind = "XML";
  free(src);

  if (!kind) { free(pretty); snprintf(g_message, sizeof g_message, "Not valid JSON or XML"); return; }

  gtcaca_editor_delete_range(ed, a, b - a);
  gtcaca_editor_insert_text(ed, a, pretty);
  gtcaca_editor_set_empty_selection(ed, a + (int)strlen(pretty));
  free(pretty);

  if (gtcaca_editor_get_json_mode(ed)) gtcaca_editor_fold_json(ed);
  snprintf(g_message, sizeof g_message, "Pretty-printed %s", kind);
}

/* M-x json-pretty-print: pretty-print the JSON value on the *current line* in
   place, expanding it across lines. Unlike C-x p (pretty_print_json) this always
   targets the current line — never the selection or whole buffer — and reports
   an error, making no edit, when that line is not valid JSON. */
void pretty_print_json_line(gtcaca_editor_widget_t *ed)
{
  int line, a, b, n;
  char *src, *pretty;
  gtcaca_json_value *v;

  if (!ed) { snprintf(g_message, sizeof g_message, "No buffer"); return; }
  line = gtcaca_editor_get_current_line(ed);
  a = gtcaca_editor_position_from_line(ed, line);
  b = gtcaca_editor_get_line_end_position(ed, line);
  n = b - a;
  if (n <= 0) { snprintf(g_message, sizeof g_message, "Current line is empty — not valid JSON"); return; }

  src = malloc((size_t)n + 1);
  if (!src) return;
  gtcaca_editor_get_text_range(ed, a, b, src, n + 1);
  v = gtcaca_json_parse(src);
  free(src);
  if (!v) { snprintf(g_message, sizeof g_message, "Current line is not valid JSON"); return; }

  pretty = gtcaca_json_stringify(v, 2);
  gtcaca_json_free(v);
  if (!pretty) return;

  gtcaca_editor_delete_range(ed, a, b - a);
  gtcaca_editor_insert_text(ed, a, pretty);
  gtcaca_editor_set_empty_selection(ed, a + (int)strlen(pretty));
  free(pretty);

  if (gtcaca_editor_get_json_mode(ed)) gtcaca_editor_fold_json(ed);
  snprintf(g_message, sizeof g_message, "Pretty-printed JSON line");
}

/* ── minimal XML pretty-printer (M-x xml-pretty-print) ───────────────────────
   No external XML library, so the dependency set — and the pip bundle — stays
   at libcaca + oniguruma. Tokenises a one-line XML fragment, verifies it is
   well-formed (tags balanced and correctly nested), and re-emits it indented.
   An element whose only child is text is kept on one line (<a>x</a>). Returns a
   malloc'd string via *out on success, or 0 (no output) when the input is not
   well-formed XML. */

typedef struct { char *p; size_t len, cap; } sbuf;
static int sb_put(sbuf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t nc = b->cap ? b->cap : 256;
    char *np;
    while (nc < b->len + n + 1) nc *= 2;
    np = realloc(b->p, nc); if (!np) return 0;
    b->p = np; b->cap = nc;
  }
  memcpy(b->p + b->len, s, n); b->len += n; b->p[b->len] = '\0';
  return 1;
}
static int sb_puts(sbuf *b, const char *s) { return sb_put(b, s, strlen(s)); }

enum { X_OPEN, X_CLOSE, X_SELF, X_TEXT, X_MISC };  /* MISC: comment/PI/CDATA/decl */
typedef struct { int type; const char *s; int len; char name[64]; } xtok;

/* read an element name (after '<' or '</') into `out` */
static void xml_name(const char *p, const char *end, char *out, int outsz) {
  int i = 0;
  while (p < end && (isalnum((unsigned char)*p) || *p=='_' || *p=='-' || *p==':' || *p=='.'))
    { if (i < outsz-1) out[i++] = *p; p++; }
  out[i] = '\0';
}

/* end of a normal tag: the char just past '>', honouring quoted attributes
   (a '>' inside "…"/'…' does not close the tag). NULL if unterminated. */
static const char *xml_tag_end(const char *p, const char *end) {
  char q = 0;
  p++;                                   /* skip '<' */
  for (; p < end; p++) {
    if (q)                    { if (*p == q) q = 0; }
    else if (*p=='"'||*p=='\'') q = *p;
    else if (*p == '>')        return p + 1;
  }
  return NULL;
}

static int xml_prettify(const char *src, char **out) {
  const char *p = src, *end = src + strlen(src);
  xtok *tk = NULL; int ntk = 0, cap = 0, i, depth = 0, ok = 1, saw_markup = 0;
  char *stack[256]; int sp = 0;               /* open-tag names, for nesting */
  sbuf b = {0,0,0};

  #define PUSHTOK(T,S,L) do { \
      if (ntk == cap) { int nc = cap ? cap*2 : 32; xtok *nt = realloc(tk, (size_t)nc*sizeof*tk); \
        if (!nt) { ok = 0; goto done; } tk = nt; cap = nc; } \
      tk[ntk].type=(T); tk[ntk].s=(S); tk[ntk].len=(L); tk[ntk].name[0]='\0'; ntk++; } while (0)

  while (p < end && ok) {
    if (*p == '<') {
      const char *te;
      if (!strncmp(p, "<!--", 4)) {
        te = strstr(p, "-->"); if (!te) { ok = 0; break; } te += 3;
        PUSHTOK(X_MISC, p, (int)(te-p)); saw_markup = 1; p = te;
      } else if (!strncmp(p, "<![CDATA[", 9)) {
        te = strstr(p, "]]>"); if (!te) { ok = 0; break; } te += 3;
        PUSHTOK(X_MISC, p, (int)(te-p)); saw_markup = 1; p = te;
      } else if (!strncmp(p, "<?", 2)) {
        te = strstr(p, "?>"); if (!te) { ok = 0; break; } te += 2;
        PUSHTOK(X_MISC, p, (int)(te-p)); saw_markup = 1; p = te;
      } else if (!strncmp(p, "<!", 2)) {
        te = xml_tag_end(p, end); if (!te) { ok = 0; break; }
        PUSHTOK(X_MISC, p, (int)(te-p)); saw_markup = 1; p = te;
      } else if (!strncmp(p, "</", 2)) {
        te = xml_tag_end(p, end); if (!te) { ok = 0; break; }
        PUSHTOK(X_CLOSE, p, (int)(te-p)); xml_name(p+2, te, tk[ntk-1].name, 64);
        /* must match the innermost open tag */
        if (sp == 0 || strcmp(stack[sp-1], tk[ntk-1].name) != 0) { ok = 0; break; }
        sp--; p = te;
      } else {
        const char *q;
        te = xml_tag_end(p, end); if (!te) { ok = 0; break; }
        q = te - 2; while (q > p && isspace((unsigned char)*q)) q--;
        if (*q == '/') { PUSHTOK(X_SELF, p, (int)(te-p)); xml_name(p+1, te, tk[ntk-1].name, 64); }
        else {
          PUSHTOK(X_OPEN, p, (int)(te-p)); xml_name(p+1, te, tk[ntk-1].name, 64);
          if (sp >= 256) { ok = 0; break; }
          stack[sp++] = tk[ntk-1].name;
        }
        saw_markup = 1; p = te;
      }
    } else {
      const char *t = p, *e;
      while (p < end && *p != '<') p++;
      /* trim surrounding whitespace; drop whitespace-only inter-tag text */
      while (t < p && isspace((unsigned char)*t)) t++;
      e = p; while (e > t && isspace((unsigned char)e[-1])) e--;
      if (e > t) PUSHTOK(X_TEXT, t, (int)(e-t));
    }
  }
  /* well-formed ⇒ scanned cleanly, every tag closed, and at least one markup
     token (a line of plain text is not XML). */
  if (!ok || sp != 0 || !saw_markup) { ok = 0; goto done; }

  for (i = 0; i < ntk; i++) {
    int d;
    if (tk[i].type == X_CLOSE) depth--;
    for (d = 0; d < depth; d++) sb_puts(&b, "  ");
    if (tk[i].type == X_OPEN) {
      /* keep <a></a> and <a>text</a> on one line */
      if (i+1 < ntk && tk[i+1].type == X_CLOSE && !strcmp(tk[i+1].name, tk[i].name)) {
        sb_put(&b, tk[i].s, (size_t)tk[i].len); sb_put(&b, tk[i+1].s, (size_t)tk[i+1].len); i++;
      } else if (i+2 < ntk && tk[i+1].type == X_TEXT
                 && tk[i+2].type == X_CLOSE && !strcmp(tk[i+2].name, tk[i].name)) {
        sb_put(&b, tk[i].s, (size_t)tk[i].len); sb_put(&b, tk[i+1].s, (size_t)tk[i+1].len);
        sb_put(&b, tk[i+2].s, (size_t)tk[i+2].len); i += 2;
      } else {
        sb_put(&b, tk[i].s, (size_t)tk[i].len); depth++;
      }
    } else {
      sb_put(&b, tk[i].s, (size_t)tk[i].len);
    }
    if (i+1 < ntk) sb_puts(&b, "\n");
  }

done:
  free(tk);
  if (ok && b.p) { *out = b.p; return 1; }
  free(b.p);
  return 0;
  #undef PUSHTOK
}

/* M-x xml-pretty-print: pretty-print the XML on the current line in place.
   Like json-pretty-print, but for XML — errors, making no edit, when the line
   is not well-formed XML. */
void pretty_print_xml_line(gtcaca_editor_widget_t *ed)
{
  int line, a, b, n;
  char *src, *pretty = NULL;

  if (!ed) { snprintf(g_message, sizeof g_message, "No buffer"); return; }
  line = gtcaca_editor_get_current_line(ed);
  a = gtcaca_editor_position_from_line(ed, line);
  b = gtcaca_editor_get_line_end_position(ed, line);
  n = b - a;
  if (n <= 0) { snprintf(g_message, sizeof g_message, "Current line is empty — not valid XML"); return; }

  src = malloc((size_t)n + 1);
  if (!src) return;
  gtcaca_editor_get_text_range(ed, a, b, src, n + 1);
  if (!xml_prettify(src, &pretty)) {
    free(src);
    snprintf(g_message, sizeof g_message, "Current line is not valid XML");
    return;
  }
  free(src);

  gtcaca_editor_delete_range(ed, a, b - a);
  gtcaca_editor_insert_text(ed, a, pretty);
  gtcaca_editor_set_empty_selection(ed, a + (int)strlen(pretty));
  free(pretty);

  snprintf(g_message, sizeof g_message, "Pretty-printed XML line");
}

/* The write itself. Split out from save_file because "the file changed on disk,
   save anyway?" is answered in the minibuffer, long after the C-x C-s that
   asked it: the answer calls back in here (see lock.c). */
void write_buffer_file(gtcaca_editor_widget_t *ed)
{
  FILE *f;
  int len, bi;
  char *buf;
  const char *path;

  if (!ed) return;
  bi = buf_index_of(ed);
  path = (bi >= 0 && g_buffers[bi].has_file) ? g_buffers[bi].path : g_filename;
  if (!path) return;

  len = gtcaca_editor_get_length(ed);
  buf = malloc((size_t)len + 1);
  if (!buf) return;
  gtcaca_editor_get_text(ed, buf, len + 1);

  f = fopen(path, "w");
  if (!f) { snprintf(g_message, sizeof(g_message), "Cannot write %s", path); free(buf); return; }
  fwrite(buf, 1, (size_t)len, f);
  fclose(f);
  free(buf);

  gtcaca_editor_set_save_point(ed);
  /* Every buffer visiting this file is now as fresh as the file itself. A
     second view of it (C-x 2) is a buffer of its own with its own stamp, and
     would otherwise ask whether the file had changed underneath it — it had,
     but we are the ones who changed it. */
  { int i;
    for (i = 0; i < g_nbuf; i++)
      if (g_buffers[i].has_file && !strcmp(g_buffers[i].path, path)) ccm_stamp_buffer(i);
    if (bi >= 0 && !g_buffers[bi].has_file) ccm_stamp_buffer(bi); }
  ccm_log_file('W', path);
  snprintf(g_message, sizeof(g_message), "Wrote %s", path);
}

void save_file(gtcaca_editor_widget_t *ed)
{
  if (!g_filename) { start_save_as(); return; }   /* scratch buffer: ask for a name */
  /* Emacs never writes over a file that moved on under the buffer without
     asking first; the answer does the write. */
  if (!ccm_confirm_save(buf_index_of(ed))) return;
  write_buffer_file(ed);
}


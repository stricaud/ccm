#include "cacamacs.h"

/* ── markdown helpers (M-x md-…) ───────────────────────────────────────────────
 *
 * Small editing commands for prose, all named md-* so that `M-x md-` and Tab
 * lists the lot. They follow the same two habits as the rest of cacamacs'
 * editing commands: a command acts on the region when there is one and on the
 * current line or word otherwise, and every one of them is its own inverse —
 * running it twice takes the markup back off. Nothing here knows how to parse
 * markdown; these only add and remove the punctuation you would type anyway.
 */

/* Characters, not bytes: a title of accented text must get an underline of the
   same visible length, and every byte of "é" would make it twice too long.
   (Double-width CJK is still counted as one, which is the usual approximation.) */
static int utf8_len(const char *s, int nbytes)
{
  int i, n = 0;
  for (i = 0; i < nbytes; i++)
    if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
  return n;
}

/* The line range a command applies to: the selected lines, else the caret's.
   A selection ending at column 0 does not drag in the line it merely touches. */
static void md_line_range(gtcaca_editor_widget_t *ed, int *first, int *last)
{
  int a = gtcaca_editor_get_selection_start(ed);
  int b = gtcaca_editor_get_selection_end(ed);
  if (a != b) {
    *first = gtcaca_editor_line_from_position(ed, a);
    *last  = gtcaca_editor_line_from_position(ed, b);
    if (*last > *first && b == gtcaca_editor_position_from_line(ed, *last)) (*last)--;
  } else {
    *first = *last = gtcaca_editor_get_current_line(ed);
  }
}

/* Copy line `L` without its indentation or trailing blanks. Returns its length
   in bytes, and reports where the text starts through `start`. */
static int md_line_text(gtcaca_editor_widget_t *ed, int L, char *buf, int bufsz, int *start)
{
  int p = line_first_nonws(ed, L);
  int e = gtcaca_editor_get_line_end_position(ed, L);
  int n;
  while (e > p) {
    char c = gtcaca_editor_get_char_at(ed, e - 1);
    if (c != ' ' && c != '\t' && c != '\r') break;
    e--;
  }
  n = e - p;
  if (n < 0) n = 0;
  if (n > bufsz - 1) n = bufsz - 1;
  gtcaca_editor_get_text_range(ed, p, p + n, buf, bufsz);
  buf[n] = '\0';
  if (start) *start = p;
  return n;
}

/* Is line `L` nothing but `ch` repeated (a setext underline)? */
static int md_is_rule_line(gtcaca_editor_widget_t *ed, int L, char ch)
{
  char buf[512];
  int n = md_line_text(ed, L, buf, sizeof buf, NULL), i;
  if (n < 1) return 0;
  for (i = 0; i < n; i++) if (buf[i] != ch) return 0;
  return 1;
}

/* ── setext headings: md-title (===) and md-subtitle (---) ──────────────────── */

static void md_underline(gtcaca_editor_widget_t *ed, char ch, const char *what)
{
  char text[512], rule[520];
  int L, first, last, start, n, width, i, eol;

  md_line_range(ed, &first, &last);
  L = first;                       /* a heading is one line; take the first */
  n = md_line_text(ed, L, text, sizeof text, &start);
  if (n == 0) { snprintf(g_message, sizeof g_message, "Nothing on this line to underline"); return; }

  width = utf8_len(text, n);
  if (width > (int)sizeof rule - 2) width = (int)sizeof rule - 2;
  for (i = 0; i < width; i++) rule[i] = ch;
  rule[width] = '\0';

  /* Replace an underline that is already there rather than stacking another,
     so md-title and md-subtitle can be used to change a heading's level. */
  if (L + 1 < gtcaca_editor_get_line_count(ed) &&
      (md_is_rule_line(ed, L + 1, '=') || md_is_rule_line(ed, L + 1, '-'))) {
    int s = gtcaca_editor_position_from_line(ed, L + 1);
    int e = gtcaca_editor_get_line_end_position(ed, L + 1);
    gtcaca_editor_delete_range(ed, s, e - s);
    gtcaca_editor_insert_text(ed, s, rule);
  } else {
    char withnl[522];
    eol = gtcaca_editor_get_line_end_position(ed, L);
    snprintf(withnl, sizeof withnl, "\n%s", rule);
    gtcaca_editor_insert_text(ed, eol, withnl);
  }
  gtcaca_editor_set_empty_selection(ed, gtcaca_editor_get_line_end_position(ed, L));
  g_mark_active = 0;
  snprintf(g_message, sizeof g_message, "%s underlined with %d '%c'", what, width, ch);
}

void md_title(gtcaca_editor_widget_t *ed)    { md_underline(ed, '=', "Title"); }
void md_subtitle(gtcaca_editor_widget_t *ed) { md_underline(ed, '-', "Subtitle"); }

/* ── ATX headings: md-h1 … md-h6 ───────────────────────────────────────────── */

/* Bytes of leading '#'s plus one following space, if the line starts a heading. */
static int md_atx_prefix_len(gtcaca_editor_widget_t *ed, int L, int *level)
{
  int p = line_first_nonws(ed, L);
  int e = gtcaca_editor_get_line_end_position(ed, L);
  int n = 0;
  while (p + n < e && gtcaca_editor_get_char_at(ed, p + n) == '#' && n < 6) n++;
  if (level) *level = n;
  if (n == 0) return 0;
  if (p + n < e && gtcaca_editor_get_char_at(ed, p + n) == ' ') return n + 1;
  return n;
}

void md_heading(gtcaca_editor_widget_t *ed, int level)
{
  char pre[10];
  int first, last, L, p, had, old = 0, i;

  md_line_range(ed, &first, &last);
  for (i = 0; i < level; i++) pre[i] = '#';
  pre[level] = ' '; pre[level + 1] = '\0';

  for (L = last; L >= first; L--) {
    if (line_is_blank(ed, L)) continue;
    p = line_first_nonws(ed, L);
    had = md_atx_prefix_len(ed, L, &old);
    if (had) gtcaca_editor_delete_range(ed, p, had);
    if (old != level) gtcaca_editor_insert_text(ed, p, pre);   /* same level again = off */
  }
  g_mark_active = 0;
  snprintf(g_message, sizeof g_message,
           old == level ? "Heading removed" : "Heading level %d", level);
}

/* ── line prefixes: md-list, md-ordered, md-quote, md-task ─────────────────── */

/* How many bytes of `pre` line L already carries, or 0. `pre` for an ordered
   list is matched loosely — any run of digits then ". ". */
static int md_prefix_len(gtcaca_editor_widget_t *ed, int L, const char *pre, int ordered)
{
  int p = line_first_nonws(ed, L);
  int e = gtcaca_editor_get_line_end_position(ed, L);
  int i, n;

  if (ordered) {
    n = 0;
    while (p + n < e && gtcaca_editor_get_char_at(ed, p + n) >= '0'
                     && gtcaca_editor_get_char_at(ed, p + n) <= '9') n++;
    if (n == 0 || p + n + 1 >= e) return 0;
    if (gtcaca_editor_get_char_at(ed, p + n) != '.') return 0;
    return gtcaca_editor_get_char_at(ed, p + n + 1) == ' ' ? n + 2 : n + 1;
  }
  n = (int)strlen(pre);
  if (p + n > e) return 0;
  for (i = 0; i < n; i++) if (gtcaca_editor_get_char_at(ed, p + i) != pre[i]) return 0;
  return n;
}

static void md_line_prefix(gtcaca_editor_widget_t *ed, const char *pre, int ordered, const char *what)
{
  int first, last, L, any_content = 0, any_plain = 0, add, num;

  md_line_range(ed, &first, &last);
  for (L = first; L <= last; L++) {
    if (line_is_blank(ed, L)) continue;
    any_content = 1;
    if (!md_prefix_len(ed, L, pre, ordered)) any_plain = 1;
  }
  add = !any_content || any_plain;      /* only strip when every line has it */

  /* Numbering counts from the top, but the edits run bottom-up so that the
     positions of the lines still to be touched stay valid. */
  num = 0;
  for (L = first; L <= last; L++) if (!line_is_blank(ed, L)) num++;

  for (L = last; L >= first; L--) {
    int p, had;
    if (line_is_blank(ed, L)) continue;
    p = line_first_nonws(ed, L);
    had = md_prefix_len(ed, L, pre, ordered);
    if (add) {
      if (had) gtcaca_editor_delete_range(ed, p, had);   /* re-number / re-mark */
      if (ordered) { char b[16]; snprintf(b, sizeof b, "%d. ", num); gtcaca_editor_insert_text(ed, p, b); }
      else         gtcaca_editor_insert_text(ed, p, pre);
      num--;
    } else if (had) {
      gtcaca_editor_delete_range(ed, p, had);
    }
  }
  g_mark_active = 0;
  snprintf(g_message, sizeof g_message, "%s %s", add ? what : "Removed", add ? "" : what);
}

void md_list(gtcaca_editor_widget_t *ed)    { md_line_prefix(ed, "- ",     0, "Bullet list"); }
void md_ordered(gtcaca_editor_widget_t *ed) { md_line_prefix(ed, "1. ",    1, "Numbered list"); }
void md_quote(gtcaca_editor_widget_t *ed)   { md_line_prefix(ed, "> ",     0, "Block quote"); }
void md_task(gtcaca_editor_widget_t *ed)    { md_line_prefix(ed, "- [ ] ", 0, "Task list"); }

/* ── inline spans: md-bold, md-italic, md-code, md-strike ──────────────────── */

/* get_char_at outside the document reads as 0, so the run tests below can look
   one character past either end without a bounds check at every call. */
static char md_char(gtcaca_editor_widget_t *ed, int p)
{
  if (p < 0 || p >= gtcaca_editor_get_length(ed)) return '\0';
  return gtcaca_editor_get_char_at(ed, p);
}

/* Word constituents, for deciding whether point is on a word at all. Bytes
   above ASCII count, so an accented word is one word rather than three. */
static int md_is_word(char c)
{
  return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80;
}

static int md_marks_at(gtcaca_editor_widget_t *ed, int p, const char *mark, int n)
{
  int i;
  for (i = 0; i < n; i++) if (md_char(ed, p + i) != mark[i]) return 0;
  return 1;
}

/* Is [a,b) already marked up? Two ways it can be: the markers are inside the
   range, because they were selected along with the text, or immediately
   outside it, because only the text was selected — or because we picked the
   bare word at point, which is what makes running a command twice undo it.
   `*sa`/`*sb` come back as the whole span, markers included.

   The character beyond each marker has to differ from it, so md-italic does not
   mistake the outer half of a "**bold**" run for a "*" span of its own. */
static int md_span(gtcaca_editor_widget_t *ed, int a, int b, const char *mark, int *sa, int *sb)
{
  int n = (int)strlen(mark);
  char c = mark[0];

  if (b - a >= 2 * n && md_marks_at(ed, a, mark, n) && md_marks_at(ed, b - n, mark, n) &&
      md_char(ed, a - 1) != c && md_char(ed, b) != c) {
    *sa = a; *sb = b; return 1;
  }
  if (a - n >= 0 && md_marks_at(ed, a - n, mark, n) && md_marks_at(ed, b, mark, n) &&
      md_char(ed, a - n - 1) != c && md_char(ed, b + n) != c) {
    *sa = a - n; *sb = b + n; return 1;
  }
  return 0;
}

/* The `mark` … `mark` span enclosing `pos` on its line, if there is one, as
   absolute positions covering the markers too.

   This is what makes the command its own inverse with no region: after wrapping,
   the caret sits on the span's edge, where asking for "the word at point" would
   answer with something else entirely. Marker runs are matched exactly — a
   search for "*" steps over "**" — so italic and bold never claim each other's
   punctuation. */
static int md_enclosing(gtcaca_editor_widget_t *ed, int pos, const char *mark, int *sa, int *sb)
{
  int L  = gtcaca_editor_line_from_position(ed, pos);
  int ls = gtcaca_editor_position_from_line(ed, L);
  int le = gtcaca_editor_get_line_end_position(ed, L);
  int n  = (int)strlen(mark);
  char c = mark[0];
  int i, open = -1;

  for (i = ls; i + n <= le; i++) {
    if (!md_marks_at(ed, i, mark, n)) continue;
    if (md_char(ed, i - 1) == c || md_char(ed, i + n) == c) {   /* a longer run: not ours */
      while (i + 1 < le && md_char(ed, i + 1) == c) i++;
      continue;
    }
    if (open < 0) {
      open = i;
    } else {
      if (pos >= open && pos <= i + n) { *sa = open; *sb = i + n; return 1; }
      open = -1;
    }
    i += n - 1;
  }
  return 0;
}

/* The word around `pos`, bounded by its line. Not gtcaca's word_left/right:
   those step over bytes, so on "Précis" they stop between the two bytes of the
   'é' and a marker inserted there splits the character in half — the file comes
   back out invalid UTF-8. Continuation bytes count as word constituents here
   (see md_is_word), which keeps every boundary on a character. */
static void md_word_at(gtcaca_editor_widget_t *ed, int pos, int *a, int *b)
{
  int L  = gtcaca_editor_line_from_position(ed, pos);
  int ls = gtcaca_editor_position_from_line(ed, L);
  int le = gtcaca_editor_get_line_end_position(ed, L);
  int s = pos, e = pos;

  while (s > ls && md_is_word(md_char(ed, s - 1))) s--;
  while (e < le && md_is_word(md_char(ed, e)))     e++;
  *a = s; *b = e;
}

static void md_wrap(gtcaca_editor_widget_t *ed, const char *mark, const char *what)
{
  int a = gtcaca_editor_get_selection_start(ed);
  int b = gtcaca_editor_get_selection_end(ed);
  int n = (int)strlen(mark);
  int sa, sb, marked;

  if (a != b) {
    marked = md_span(ed, a, b, mark, &sa, &sb);
  } else {                            /* no region: the span at point, else the word */
    int pos = gtcaca_editor_get_current_pos(ed);
    marked = md_enclosing(ed, pos, mark, &sa, &sb);
    if (!marked) {
      /* Only claim a word when point is actually on one. Off a word — on the
         space after a span just written, say — the word functions happily
         reach across the whole line, so insert an empty pair at point instead
         and let the next thing typed land inside it. */
      if (md_is_word(md_char(ed, pos)) || md_is_word(md_char(ed, pos - 1)))
        md_word_at(ed, pos, &a, &b);
      else
        a = b = pos;
    }
  }

  if (marked) {                                       /* already marked: take it off */
    gtcaca_editor_delete_range(ed, sb - n, n);
    gtcaca_editor_delete_range(ed, sa, n);
    gtcaca_editor_set_empty_selection(ed, sb - 2 * n);
    g_mark_active = 0;
    snprintf(g_message, sizeof g_message, "%s removed", what);
    return;
  }
  gtcaca_editor_insert_text(ed, b, mark);            /* end first: `a` stays valid */
  gtcaca_editor_insert_text(ed, a, mark);
  /* Caret between the markers when there was no word, else after the span. */
  gtcaca_editor_set_empty_selection(ed, a == b ? a + n : b + 2 * n);
  g_mark_active = 0;
  snprintf(g_message, sizeof g_message, "%s", what);
}

void md_bold(gtcaca_editor_widget_t *ed)   { md_wrap(ed, "**", "Bold"); }
void md_italic(gtcaca_editor_widget_t *ed) { md_wrap(ed, "*",  "Italic"); }
void md_strike(gtcaca_editor_widget_t *ed) { md_wrap(ed, "~~", "Strikethrough"); }

/* Inline `code` for a span inside one line; a fenced block when the region
   covers whole lines, which is what you want for a snippet. */
void md_code(gtcaca_editor_widget_t *ed)
{
  int a = gtcaca_editor_get_selection_start(ed);
  int b = gtcaca_editor_get_selection_end(ed);

  if (a != b && gtcaca_editor_line_from_position(ed, a) != gtcaca_editor_line_from_position(ed, b)) {
    int first, last, s, e;
    md_line_range(ed, &first, &last);
    s = gtcaca_editor_position_from_line(ed, first);
    e = gtcaca_editor_get_line_end_position(ed, last);
    gtcaca_editor_insert_text(ed, e, "\n```");
    gtcaca_editor_insert_text(ed, s, "```\n");
    gtcaca_editor_set_empty_selection(ed, s + 3);   /* on the opening fence: name the language */
    g_mark_active = 0;
    snprintf(g_message, sizeof g_message, "Fenced code block — type the language after ```");
    return;
  }
  md_wrap(ed, "`", "Code");
}

/* ── md-link and md-hr ─────────────────────────────────────────────────────── */

void md_link(gtcaca_editor_widget_t *ed)
{
  int a = gtcaca_editor_get_selection_start(ed);
  int b = gtcaca_editor_get_selection_end(ed);

  if (a == b) {
    gtcaca_editor_insert_text(ed, a, "[]()");
    gtcaca_editor_set_empty_selection(ed, a + 1);          /* inside the [] */
    snprintf(g_message, sizeof g_message, "Link — type the text, then the URL between the parens");
  } else {
    gtcaca_editor_insert_text(ed, b, "]()");
    gtcaca_editor_insert_text(ed, a, "[");
    gtcaca_editor_set_empty_selection(ed, b + 3);          /* inside the () */
    snprintf(g_message, sizeof g_message, "Link — type the URL");
  }
  g_mark_active = 0;
}

void md_hr(gtcaca_editor_widget_t *ed)
{
  int L = gtcaca_editor_get_current_line(ed);
  int e = gtcaca_editor_get_line_end_position(ed, L);
  int blank = line_is_blank(ed, L), last;

  /* On a blank line the rule goes right there; otherwise it gets a blank line
     of its own above, which is what makes it a rule and not a setext heading
     underlining whatever happened to be on the line before it. */
  gtcaca_editor_insert_text(ed, e, blank ? "---" : "\n\n---\n");
  last = gtcaca_editor_get_line_count(ed) - 1;
  L = blank ? L : L + 2;
  if (L > last) L = last;
  gtcaca_editor_set_empty_selection(ed, gtcaca_editor_get_line_end_position(ed, L));
  g_mark_active = 0;
  snprintf(g_message, sizeof g_message, "Horizontal rule");
}

/* ── tables ────────────────────────────────────────────────────────────────
 *
 * Two shapes, because the two readers that matter disagree about what a table
 * with a multi-line cell looks like:
 *
 *   md-table         a pandoc grid table. Cells hold real block content, so a
 *                    fenced graph or a diagram goes in as itself and comes out
 *                    of pandoc as a verbatim block.
 *   md-github-table  a GFM pipe table. One line per row, so a multi-line
 *                    selection has to be folded with <br> — which GitHub draws
 *                    and pandoc, whose LaTeX writer drops raw HTML, does not.
 *
 * md-table-row, md-table-col and md-table-align read whichever kind point is
 * in and do the right thing, so there is only one set to remember.
 */

#define MDT_MAXCOL   32
#define MDT_LINE   8192      /* a folded cell is long: a whole graph on one line */
#define MDT_TABLE 32768      /* a grid table, padding and borders included */
#define MDT_PADCAP   48      /* wider than this and a pipe column is left unpadded */
#define MDT_GRIDMIN  12      /* an empty grid column, wide enough to type into */

/* The whole of line `L`, indentation included, unlike md_line_text. */
static int md_raw_line(gtcaca_editor_widget_t *ed, int L, char *buf, int bufsz)
{
  int s = gtcaca_editor_position_from_line(ed, L);
  int e = gtcaca_editor_get_line_end_position(ed, L);
  int n = e - s;
  if (n < 0) n = 0;
  if (n > bufsz - 1) n = bufsz - 1;
  gtcaca_editor_get_text_range(ed, s, s + n, buf, bufsz);
  buf[n] = '\0';
  return n;
}

static void md_put(char *out, size_t outsz, size_t *n, const char *s)
{
  size_t len = strlen(s);
  if (*n + len < outsz) { memcpy(out + *n, s, len); *n += len; }
  out[*n < outsz ? *n : outsz - 1] = '\0';
}

static void md_putc(char *out, size_t outsz, size_t *n, char c, int times)
{
  while (times-- > 0 && *n + 1 < outsz) out[(*n)++] = c;
  out[*n] = '\0';
}

/* ── telling the two shapes apart ──────────────────────────────────────────── */

/* `+---+===+` and friends: a grid table's rules. Nothing but `+-=:` between
   the first `+` and the last, so a line of box art — `+----+   +----+`, which
   is exactly what you might be putting *into* a table — is not taken for one. */
static int md_is_grid_border(gtcaca_editor_widget_t *ed, int L)
{
  int p = line_first_nonws(ed, L);
  int e = gtcaca_editor_get_line_end_position(ed, L);
  int i, plus = 0, rule = 0;

  while (e > p) {
    char c = gtcaca_editor_get_char_at(ed, e - 1);
    if (c != ' ' && c != '\t' && c != '\r') break;
    e--;
  }
  if (p >= e || gtcaca_editor_get_char_at(ed, p) != '+') return 0;
  for (i = p; i < e; i++) {
    char c = gtcaca_editor_get_char_at(ed, i);
    if (c == '+') { plus++; continue; }
    if (c == '-' || c == '=') { rule++; continue; }
    if (c == ':') continue;
    return 0;
  }
  return plus >= 2 && rule > 0;
}

/* Part of a table: a row of either shape, or a grid rule. */
static int md_is_table_line(gtcaca_editor_widget_t *ed, int L)
{
  int p = line_first_nonws(ed, L);
  int e = gtcaca_editor_get_line_end_position(ed, L);
  if (p < e && gtcaca_editor_get_char_at(ed, p) == '|') return 1;
  return md_is_grid_border(ed, L);
}

/* The run of table lines around `L`. 0 when `L` is not in a table. */
static int md_table_range(gtcaca_editor_widget_t *ed, int L, int *first, int *last)
{
  int n = gtcaca_editor_get_line_count(ed);
  if (!md_is_table_line(ed, L)) return 0;
  *first = *last = L;
  while (*first > 0 && md_is_table_line(ed, *first - 1)) (*first)--;
  while (*last < n - 1 && md_is_table_line(ed, *last + 1)) (*last)++;
  return 1;
}

/* A rule anywhere in the run makes it a grid table. */
static int md_table_is_grid(gtcaca_editor_widget_t *ed, int first, int last)
{
  int L;
  for (L = first; L <= last; L++) if (md_is_grid_border(ed, L)) return 1;
  return 0;
}

/* ── splitting rows into cells ─────────────────────────────────────────────── */

/* Copy `s`..`e` into `buf`, and record it in `cell`. Trailing blanks always go;
   how much of the leading run goes depends on the shape. A pipe cell is prose,
   so all of it does. A grid cell is a *block* — the indent of a line inside a
   fenced graph is content, and losing it would flatten the thing we went to the
   grid shape to keep — so only the one space that separates it from the wall
   comes off. */
static int md_cell_push(const char *s, const char *e, char *buf, size_t bufsz,
                        size_t *off, char **cell, int n, int keep_indent)
{
  if (keep_indent) { if (s < e && (*s == ' ' || *s == '\t')) s++; }
  else while (s < e && (*s == ' ' || *s == '\t')) s++;
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
  if (*off + (size_t)(e - s) + 1 > bufsz) return 0;
  cell[n] = buf + *off;
  memcpy(buf + *off, s, (size_t)(e - s));
  *off += (size_t)(e - s);
  buf[(*off)++] = '\0';
  return 1;
}

/* A GFM row: every unescaped `|` is a wall, which is how GFM itself reads it.
   The empty segment after a trailing `|` is not a cell. */
static int md_cells(const char *line, char *buf, size_t bufsz, char **cell, int maxcells)
{
  const char *p = line;
  size_t off = 0;
  int n = 0;

  while (*p == ' ' || *p == '\t') p++;
  if (*p == '|') p++;

  while (n < maxcells) {
    const char *s = p, *e;
    int closed = 0;
    while (*p && !(*p == '|' && (p == s || p[-1] != '\\'))) p++;
    e = p;
    if (*p == '|') { p++; closed = 1; }
    if (!closed) {
      const char *t = s, *u = e;
      while (t < u && (*t == ' ' || *t == '\t')) t++;
      while (u > t && (u[-1] == ' ' || u[-1] == '\t' || u[-1] == '\r')) u--;
      if (u == t) break;                       /* the tail after a trailing `|` */
    }
    if (!md_cell_push(s, e, buf, bufsz, &off, cell, n, 0)) break;
    n++;
    if (!closed) break;
  }
  return n;
}

/* A GFM separator row: every cell is dashes, with optional alignment colons. */
static int md_is_sep_line(gtcaca_editor_widget_t *ed, int L)
{
  char line[MDT_LINE], buf[MDT_LINE], *cell[MDT_MAXCOL];
  int n, i, j;

  if (!md_is_table_line(ed, L) || md_is_grid_border(ed, L)) return 0;
  md_raw_line(ed, L, line, sizeof line);
  n = md_cells(line, buf, sizeof buf, cell, MDT_MAXCOL);
  if (n == 0) return 0;
  for (i = 0; i < n; i++) {
    int dashes = 0;
    if (!cell[i][0]) return 0;
    for (j = 0; cell[i][j]; j++) {
      if (cell[i][j] == '-') dashes++;
      else if (cell[i][j] != ':') return 0;
    }
    if (!dashes) return 0;
  }
  return 1;
}

/* The character columns of a rule's `+`s — where the walls of every row in
   that table have to stand, which is how pandoc itself reads a grid. */
static int md_grid_bounds(const char *rule, int *col, int maxn)
{
  const char *p = rule;
  int c = 0, n = 0;
  for (; *p && n < maxn; p++) {
    if (*p == '+') col[n++] = c;
    if (((unsigned char)*p & 0xC0) != 0x80) c++;
  }
  return n;
}

/* Byte offsets of the `|`s that stand on `bound`'s columns, or 0 if the line
   does not have one at every last of them. */
static int md_grid_walls(const char *line, const int *bound, int nbound, int *at)
{
  const char *p = line;
  int c = 0, k = 0;
  for (; *p && k < nbound; p++) {
    if (c == bound[k]) { if (*p != '|') return 0; at[k++] = (int)(p - line); }
    if (((unsigned char)*p & 0xC0) != 0x80) c++;
  }
  return k == nbound;
}

/* A grid row, split into cells.

   Where the row still lines up with its rule we split on the rule's columns,
   exactly as pandoc does. That matters as soon as a cell holds a drawing: the
   walls of an ASCII box are `|` characters with spaces round them and are
   indistinguishable from cell walls by any local test.

   A row that no longer lines up — the one you are typing into — falls back to
   reading a `|` as a wall at the line's two edges or after a space, which is
   wrong only for art that has been typed out of alignment and is right for
   everything else. A Mermaid edge label's `-->|"yes"|` survives either way. */
static int md_grid_cells(const char *line, const int *bound, int nbound,
                         char *buf, size_t bufsz, char **cell, int maxcells)
{
  int wall[MDT_MAXCOL];
  int len = (int)strlen(line);
  int s = 0, e, i, prev, n = 0;
  size_t off = 0;

  if (nbound > 1 && nbound <= MDT_MAXCOL && md_grid_walls(line, bound, nbound, wall)) {
    for (i = 0; i + 1 < nbound && n < maxcells; i++) {
      if (!md_cell_push(line + wall[i] + 1, line + wall[i + 1], buf, bufsz, &off, cell, n, 1))
        return n;
      n++;
    }
    return n;
  }

  while (s < len && (line[s] == ' ' || line[s] == '\t')) s++;
  if (s >= len || line[s] != '|') return 0;
  e = len;
  while (e > s && (line[e - 1] == ' ' || line[e - 1] == '\t' || line[e - 1] == '\r')) e--;
  if (e - 1 > s && line[e - 1] == '|') e--;    /* the closing wall */

  prev = s + 1;
  for (i = prev; i < e && n < maxcells; i++) {
    if (line[i] != '|') continue;
    if (line[i - 1] != ' ' && line[i - 1] != '\t') continue;
    if (!md_cell_push(line + prev, line + i, buf, bufsz, &off, cell, n, 1)) return n;
    n++;
    prev = i + 1;
  }
  if (n < maxcells && md_cell_push(line + prev, line + e, buf, bufsz, &off, cell, n, 1)) n++;
  return n;
}

/* A grid rule's segments: how many, and for each one its alignment colons and
   whether it is drawn with `=` (the header rule) or `-`. */
static int md_grid_segments(const char *line, int *lcol, int *rcol, char *fill, int maxseg)
{
  const char *p = line, *s;
  int n = 0;

  while (*p == ' ' || *p == '\t') p++;
  if (*p != '+') return 0;
  p++;
  while (*p && n < maxseg) {
    int eq = 0;
    s = p;
    while (*p && *p != '+') { if (*p == '=') eq = 1; p++; }
    if (p == s) break;
    lcol[n] = *s == ':';
    rcol[n] = p[-1] == ':';
    fill[n] = eq ? '=' : '-';
    n++;
    if (*p == '+') p++; else break;
  }
  return n;
}

/* ── md-github-table: folding a selection into one pipe-table cell ─────────
 *
 * Lines join with `<br>`; art and code get a `<pre>` as well, because a cell
 * is prose otherwise and runs of spaces collapse. A pipe cannot appear raw in
 * a row whatever we do, so it goes in as `&#124;` — `\|` would show its
 * backslash inside the `<pre>`.
 */

/* Does this block need its spacing kept? A fence says so outright; otherwise
   indentation or an inner run of spaces means it is lined up on purpose. */
static int md_block_is_preformatted(const char *text)
{
  const char *p = text;
  int bol = 1;
  for (; *p; p++) {
    if (*p == '\n') { bol = 1; continue; }
    if (bol && (*p == ' ' || *p == '\t')) return 1;
    if (bol && !strncmp(p, "```", 3)) return 1;
    bol = 0;
    if (*p == ' ' && p[1] == ' ' && p[2] != '\n' && p[2]) return 1;
  }
  return 0;
}

/* Is this line a ``` (or ~~~) fence? */
static int md_is_fence(const char *s, int n)
{
  int i = 0;
  while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
  return n - i >= 3 && (!strncmp(s + i, "```", 3) || !strncmp(s + i, "~~~", 3));
}

/* Does the block already carry its own fence? */
static int md_block_is_fenced(const char *text)
{
  const char *p = text;
  while (*p) {
    const char *eol = strchr(p, '\n');
    int len = eol ? (int)(eol - p) : (int)strlen(p);
    int i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
    if (i < len) return md_is_fence(p, len);        /* the first line with anything on it */
    if (!eol) break;
    p = eol + 1;
  }
  return 0;
}

/* `text` (the raw selection) as a single pipe-table cell. */
static void md_fold_cell(const char *text, char *out, size_t outsz)
{
  int pre = md_block_is_preformatted(text);
  const char *p = text;
  size_t n = 0;
  int first = 1;

  out[0] = '\0';
  if (pre) md_put(out, outsz, &n, "<pre>");

  while (*p) {
    const char *eol = strchr(p, '\n');
    int len = eol ? (int)(eol - p) : (int)strlen(p);
    int i;
    while (len > 0 && p[len - 1] == '\r') len--;

    if (md_is_fence(p, len)) { p = eol ? eol + 1 : p + strlen(p); continue; }
    /* Blank lines at either end are the selection's edges, not content; one in
       the middle is a real gap and comes through as an empty <br> row. */
    if (!first || len > 0) {
      if (!first) md_put(out, outsz, &n, "<br>");
      for (i = 0; i < len; i++) {
        char c = p[i];
        if      (c == '|')        md_put(out, outsz, &n, "&#124;");
        else if (c == '&')        md_put(out, outsz, &n, "&amp;");
        else if (c == '<' && pre) md_put(out, outsz, &n, "&lt;");
        else if (c == '>' && pre) md_put(out, outsz, &n, "&gt;");
        else if (c == '\t')       md_put(out, outsz, &n, "    ");
        else if (n + 1 < outsz)   { out[n++] = c; out[n] = '\0'; }
      }
      first = 0;
    }
    if (!eol) break;
    p = eol + 1;
  }

  while (n >= 4 && !strcmp(out + n - 4, "<br>")) { n -= 4; out[n] = '\0'; }
  if (pre) md_put(out, outsz, &n, "</pre>");
}

/* ── where a new table goes ────────────────────────────────────────────────
 *
 * A table has to be a block of its own: one that carries straight on from the
 * paragraph above it is read as more of that paragraph and never drawn. So it
 * gets a blank line either side — splitting the current line where point is
 * mid-way through it, and leaving alone whichever side is blank already.
 */
static void md_block_gap(gtcaca_editor_widget_t *ed, int pos, int *lead, int *trail)
{
  int L    = gtcaca_editor_line_from_position(ed, pos);
  int last = gtcaca_editor_get_line_count(ed) - 1;

  if      (pos > gtcaca_editor_position_from_line(ed, L)) *lead = 2;
  else if (L > 0 && !line_is_blank(ed, L - 1))            *lead = 1;
  else                                                    *lead = 0;

  if      (pos < gtcaca_editor_get_line_end_position(ed, L)) *trail = 2;
  else if (L < last && !line_is_blank(ed, L + 1))            *trail = 1;
  else                                                       *trail = 0;
}

/* The fenced block point is standing in, if there is one. Diagram mode leaves
   point just past the ```mermaid graph it has dropped in, and that graph is
   plainly the thing you meant to put in the cell — selecting it by hand first
   is a step for nothing. Fences pair from the top of the buffer, so that is
   where the counting starts. */
static int md_fence_at_point(gtcaca_editor_widget_t *ed, int *first, int *last)
{
  int n   = gtcaca_editor_get_line_count(ed);
  int cur = gtcaca_editor_get_current_line(ed);
  int L, open = -1;

  /* Point resting on the blank line after a block still counts as being in it:
     that is exactly where diagram mode leaves you. */
  if (cur > 0 && line_is_blank(ed, cur)) cur--;

  for (L = 0; L < n; L++) {
    char line[512];
    int len = md_raw_line(ed, L, line, sizeof line);
    if (!md_is_fence(line, len)) continue;
    if (open < 0) { open = L; continue; }
    if (cur >= open && cur <= L) { *first = open; *last = L; return 1; }
    open = -1;
  }
  return 0;
}

/* The selection, as bytes, with whole lines taken when it spans more than one
   so a block is never half-caught. With no selection, the fenced block point
   is in. Deletes what it took and answers where it was. */
static int md_take_region(gtcaca_editor_widget_t *ed, char *sel, size_t selsz, int *took_fence)
{
  int a = gtcaca_editor_get_selection_start(ed);
  int b = gtcaca_editor_get_selection_end(ed);
  int ffirst, flast;

  sel[0] = '\0';
  *took_fence = 0;
  if (a == b && md_fence_at_point(ed, &ffirst, &flast)) {
    *took_fence = 1;
    a = gtcaca_editor_position_from_line(ed, ffirst);
    b = gtcaca_editor_get_line_end_position(ed, flast);
  } else if (a == b) {
    return a;
  } else if (gtcaca_editor_line_from_position(ed, b) > gtcaca_editor_line_from_position(ed, a)) {
    int first, last;
    md_line_range(ed, &first, &last);
    a = gtcaca_editor_position_from_line(ed, first);
    b = gtcaca_editor_get_line_end_position(ed, last);
  }
  if (a == b) return a;
  if ((size_t)(b - a) > selsz - 1) return -1;
  gtcaca_editor_get_text_range(ed, a, b, sel, (int)selsz);
  sel[b - a] = '\0';
  gtcaca_editor_delete_range(ed, a, b - a);
  return a;
}

/* ── md-github-table ───────────────────────────────────────────────────────
 *
 * Two columns, because that is the shape you are after when you put a diagram
 * on the left and say what it means on the right. The selection becomes the
 * first body cell and point lands in the header above it.
 */
void md_github_table(gtcaca_editor_widget_t *ed)
{
  char sel[MDT_LINE], cell[MDT_LINE], table[MDT_LINE + 128];
  int pos, lead, trail, hdr, fenced;

  pos = md_take_region(ed, sel, sizeof sel, &fenced);
  if (pos < 0) { snprintf(g_message, sizeof g_message, "Selection too big for one cell"); return; }
  md_fold_cell(sel, cell, sizeof cell);
  md_block_gap(ed, pos, &lead, &trail);

  snprintf(table, sizeof table, "%s|  |  |\n| --- | --- |\n| %s |  |%s",
           lead == 2 ? "\n\n" : lead == 1 ? "\n" : "",
           cell,
           trail == 2 ? "\n\n" : trail == 1 ? "\n" : "");
  gtcaca_editor_insert_text(ed, pos, table);

  hdr = gtcaca_editor_line_from_position(ed, pos) + lead;
  gtcaca_editor_set_empty_selection(ed, gtcaca_editor_position_from_line(ed, hdr) + 2);
  g_mark_active = 0;
  snprintf(g_message, sizeof g_message,
           "GitHub table%s — the cell is folded with <br>, which pandoc drops",
           fenced ? " round the block at point" : "");
}

/* ── md-table: a pandoc grid table ─────────────────────────────────────────
 *
 * The point of the grid shape is that a cell is a block, so a selection goes
 * in line for line and keeps its own line breaks. It is fenced on the way if
 * it needs to be: pandoc runs unfenced lines together into one paragraph, and
 * a graph or a diagram loses its shape the moment that happens. A fence the
 * selection already has is left alone — it also names the language.
 */

/* The selection as the body lines of a cell, `\n`-separated, with the widest
   line reported. Blank lines at either end are the selection's edges. */
static int md_grid_body(const char *text, char *out, size_t outsz, int *maxw)
{
  int fence = !md_block_is_fenced(text) && md_block_is_preformatted(text);
  const char *p = text;
  size_t n = 0;
  int lines = 0, blanks = 0;

  out[0] = '\0';
  *maxw = 0;
  if (fence) { md_put(out, outsz, &n, "```"); lines = 1; }

  while (*p) {
    const char *eol = strchr(p, '\n');
    int len = eol ? (int)(eol - p) : (int)strlen(p);
    int i, blank = 1;
    while (len > 0 && p[len - 1] == '\r') len--;
    for (i = 0; i < len; i++) if (p[i] != ' ' && p[i] != '\t') { blank = 0; break; }

    if (blank && !lines) { if (!eol) break; p = eol + 1; continue; }   /* leading */
    if (blank) { blanks++; if (!eol) break; p = eol + 1; continue; }   /* held back */

    while (blanks-- > 0) { md_put(out, outsz, &n, "\n"); lines++; }
    blanks = 0;
    if (lines) md_put(out, outsz, &n, "\n");
    { size_t start = n;
      for (i = 0; i < len; i++) {
        if (p[i] == '\t') md_put(out, outsz, &n, "    ");
        else if (n + 1 < outsz) { out[n++] = p[i]; out[n] = '\0'; }
      }
      { int w = utf8_len(out + start, (int)(n - start)); if (w > *maxw) *maxw = w; }
    }
    lines++;
    if (!eol) break;
    p = eol + 1;
  }

  if (fence) {
    if (lines) md_put(out, outsz, &n, "\n");
    md_put(out, outsz, &n, "```");
    lines++;
    if (*maxw < 3) *maxw = 3;
  }
  return lines;
}

/* `+---+---+` for the given widths, drawn with `fill`. */
static void md_grid_rule(char *out, size_t outsz, size_t *n, const int *width, int ncol, char fill)
{
  int i;
  md_put(out, outsz, n, "+");
  for (i = 0; i < ncol; i++) { md_putc(out, outsz, n, fill, width[i] + 2); md_put(out, outsz, n, "+"); }
}

void md_table(gtcaca_editor_widget_t *ed)
{
  char sel[MDT_LINE], body[MDT_LINE + 16], table[MDT_TABLE];
  int width[2];
  int pos, lead, trail, hdr, nline, fenced, maxw = 0;
  size_t n = 0;
  const char *p;

  pos = md_take_region(ed, sel, sizeof sel, &fenced);
  if (pos < 0) { snprintf(g_message, sizeof g_message, "Selection too big for one cell"); return; }
  nline = md_grid_body(sel, body, sizeof body, &maxw);
  if (nline == 0) { body[0] = '\0'; nline = 1; }

  width[0] = maxw > MDT_GRIDMIN ? maxw : MDT_GRIDMIN;
  width[1] = MDT_GRIDMIN;
  md_block_gap(ed, pos, &lead, &trail);

  table[0] = '\0';
  md_putc(table, sizeof table, &n, '\n', lead);
  md_grid_rule(table, sizeof table, &n, width, 2, '-');
  md_put(table, sizeof table, &n, "\n|");
  md_putc(table, sizeof table, &n, ' ', width[0] + 2);
  md_put(table, sizeof table, &n, "|");
  md_putc(table, sizeof table, &n, ' ', width[1] + 2);
  md_put(table, sizeof table, &n, "|\n");
  md_grid_rule(table, sizeof table, &n, width, 2, '=');

  for (p = body; ; ) {                              /* the cell, a line at a time */
    const char *eol = strchr(p, '\n');
    int len = eol ? (int)(eol - p) : (int)strlen(p);
    char line[MDT_LINE + 16];
    if (len > (int)sizeof line - 1) len = (int)sizeof line - 1;
    memcpy(line, p, (size_t)len); line[len] = '\0';
    md_put(table, sizeof table, &n, "\n| ");
    md_put(table, sizeof table, &n, line);
    md_putc(table, sizeof table, &n, ' ', width[0] - utf8_len(line, len) + 1);
    md_put(table, sizeof table, &n, "|");
    md_putc(table, sizeof table, &n, ' ', width[1] + 2);
    md_put(table, sizeof table, &n, "|");
    if (!eol) break;
    p = eol + 1;
  }
  md_put(table, sizeof table, &n, "\n");
  md_grid_rule(table, sizeof table, &n, width, 2, '-');
  md_putc(table, sizeof table, &n, '\n', trail);

  if (n >= sizeof table - 1) {
    snprintf(g_message, sizeof g_message, "Selection too big for one cell");
    return;
  }
  gtcaca_editor_insert_text(ed, pos, table);

  hdr = gtcaca_editor_line_from_position(ed, pos) + lead + 1;   /* past the top rule */
  gtcaca_editor_set_empty_selection(ed, gtcaca_editor_position_from_line(ed, hdr) + 2);
  g_mark_active = 0;
  snprintf(g_message, sizeof g_message,
           "Grid table%s — name the columns, then M-x md-table-row",
           fenced ? " round the block at point" : "");
}

/* ── md-table-row ─────────────────────────────────────────────────────────── */

/* An empty row shaped like `ref`, so a table that was lined up stays lined up
   without reflowing the rest of it. */
static void md_empty_row_like(const char *ref, char *out, size_t outsz)
{
  char buf[MDT_LINE], *cell[MDT_MAXCOL];
  int n = md_cells(ref, buf, sizeof buf, cell, MDT_MAXCOL);
  int i;
  size_t o = 0;

  out[0] = '\0';
  while (ref[o] == ' ' || ref[o] == '\t') { if (o + 1 < outsz) out[o] = ref[o]; o++; }
  if (o >= outsz) o = 0;
  out[o] = '\0';
  md_put(out, outsz, &o, "|");
  for (i = 0; i < n; i++) {
    int w = utf8_len(cell[i], (int)strlen(cell[i]));
    if (w > MDT_PADCAP) w = 0;               /* a folded cell: do not pad to it */
    md_putc(out, outsz, &o, ' ', w + 2);
    md_put(out, outsz, &o, "|");
  }
}

/* The grid rule closing the row point is on — and from the header or a rule,
   the header rule, so the new row is the first body one. */
static int md_grid_row_end(gtcaca_editor_widget_t *ed, int first, int last, int L)
{
  int i, head = -1;
  for (i = first; i <= last; i++) {
    char line[MDT_LINE];
    md_raw_line(ed, i, line, sizeof line);
    if (md_is_grid_border(ed, i) && strchr(line, '=')) { head = i; break; }
  }
  if (head >= 0 && L <= head) return head;
  for (i = (L > first ? L : first + 1); i <= last; i++) if (md_is_grid_border(ed, i)) return i;
  return last;
}

void md_table_row(gtcaca_editor_widget_t *ed)
{
  char ref[MDT_LINE], row[MDT_LINE + 8], rule[MDT_LINE + 8];
  int L = gtcaca_editor_get_current_line(ed);
  int first, last, at, pos, grid, i;

  if (!md_table_range(ed, L, &first, &last)) {
    snprintf(g_message, sizeof g_message, "Not in a table — M-x md-table makes one");
    return;
  }
  grid = md_table_is_grid(ed, first, last);

  if (grid) {
    at = md_grid_row_end(ed, first, last, L);
    md_raw_line(ed, at, ref, sizeof ref);
    /* The row is a blank content line shaped like the rule that closes it,
       plus a copy of that rule — as `-`, since only the header wears `=`. */
    { char cells[MDT_LINE]; size_t o = 0;
      int lcol[MDT_MAXCOL], rcol[MDT_MAXCOL]; char fill[MDT_MAXCOL];
      int ncol = md_grid_segments(ref, lcol, rcol, fill, MDT_MAXCOL);
      const char *q = ref;
      cells[0] = '\0';
      md_put(cells, sizeof cells, &o, "|");
      for (i = 0, q = ref; i < ncol; i++) {
        const char *s = strchr(q, '+');
        const char *e = s ? strchr(s + 1, '+') : NULL;
        md_putc(cells, sizeof cells, &o, ' ', e && s ? (int)(e - s - 1) : MDT_GRIDMIN + 2);
        md_put(cells, sizeof cells, &o, "|");
        q = s ? s + 1 : q;
      }
      snprintf(row, sizeof row, "%s", cells);
    }
    { size_t o = 0; int j;
      rule[0] = '\0';
      for (j = 0; ref[j]; j++) {
        char c = ref[j] == '=' ? '-' : ref[j];
        if (o + 1 < sizeof rule) rule[o++] = c;
      }
      rule[o] = '\0';
    }
    pos = gtcaca_editor_get_line_end_position(ed, at);
    gtcaca_editor_insert_text(ed, pos, "\n");
    gtcaca_editor_insert_text(ed, pos + 1, row);
    gtcaca_editor_insert_text(ed, pos + 1 + (int)strlen(row), "\n");
    gtcaca_editor_insert_text(ed, pos + 2 + (int)strlen(row), rule);
  } else {
    /* From the header or the separator the new row goes below the separator:
       the first body row is what you meant, not a second header. */
    at = L;
    if (at == first || md_is_sep_line(ed, at)) {
      at = first;
      while (at < last && !md_is_sep_line(ed, at)) at++;
    }
    md_raw_line(ed, at, ref, sizeof ref);
    md_empty_row_like(ref, row, sizeof row);
    pos = gtcaca_editor_get_line_end_position(ed, at);
    gtcaca_editor_insert_text(ed, pos, "\n");
    gtcaca_editor_insert_text(ed, pos + 1, row);
  }
  gtcaca_editor_set_empty_selection(ed, pos + 1 + (int)strspn(row, " \t") + 2);
  g_mark_active = 0;
  snprintf(g_message, sizeof g_message, "Row added");
}

/* ── md-table-col ─────────────────────────────────────────────────────────── */

void md_table_col(gtcaca_editor_widget_t *ed)
{
  int L = gtcaca_editor_get_current_line(ed);
  int first, last, pos, grid;

  if (!md_table_range(ed, L, &first, &last)) {
    snprintf(g_message, sizeof g_message, "Not in a table — M-x md-table makes one");
    return;
  }
  grid = md_table_is_grid(ed, first, last);

  /* Bottom-up: appending to a line moves every line after it. */
  for (L = last; L >= first; L--) {
    char line[MDT_LINE], add[MDT_GRIDMIN + 8];
    int n = md_raw_line(ed, L, line, sizeof line);
    int border = md_is_grid_border(ed, L), e;
    size_t o = 0;
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t' || line[n - 1] == '\r')) n--;
    if (n == 0) continue;

    /* A row with no closing wall needs one before a cell can be added to it. */
    if (line[n - 1] != (border ? '+' : '|')) {
      gtcaca_editor_insert_text(ed, gtcaca_editor_position_from_line(ed, L) + n,
                                border ? "+" : " |");
    }
    add[0] = '\0';
    if (border)     { md_putc(add, sizeof add, &o, strchr(line, '=') ? '=' : '-', MDT_GRIDMIN + 2);
                      md_put(add, sizeof add, &o, "+"); }
    else if (grid)  { md_putc(add, sizeof add, &o, ' ', MDT_GRIDMIN + 2);
                      md_put(add, sizeof add, &o, "|"); }
    else            md_put(add, sizeof add, &o, md_is_sep_line(ed, L) ? " --- |" : "   |");

    e = gtcaca_editor_get_line_end_position(ed, L);
    gtcaca_editor_insert_text(ed, e, add);
  }
  /* Into the new header cell — just past its `| `, so what gets typed there
     does not come out carrying a stray space. On a grid the header is the row
     below the top rule; on a pipe table it is the first line. */
  L = grid ? first + 1 : first;
  pos = gtcaca_editor_get_line_end_position(ed, L);
  gtcaca_editor_set_empty_selection(ed, pos - (grid ? MDT_GRIDMIN + 2 : 3));
  g_mark_active = 0;
  snprintf(g_message, sizeof g_message, "Column added — M-x md-table-align lines it up");
}

/* ── md-table-align ───────────────────────────────────────────────────────── */

/* A grid table reflowed: every cell padded to its column's widest line and the
   rules redrawn to match. No cell is ever narrowed below its content — in a
   grid the walls have to meet the rules, so unlike a pipe table there is no
   column to leave unpadded. Alignment colons and the `=` header rule survive. */
static void md_grid_align(gtcaca_editor_widget_t *ed, int first, int last, int *ncolp)
{
  int width[MDT_MAXCOL] = { 0 };
  int bound[MDT_MAXCOL];
  int L, i, ncol = 0, nbound = 0;

  for (L = first; L <= last; L++) {                     /* the table's own walls */
    char rule[MDT_LINE];
    if (!md_is_grid_border(ed, L)) continue;
    md_raw_line(ed, L, rule, sizeof rule);
    nbound = md_grid_bounds(rule, bound, MDT_MAXCOL);
    break;
  }

  for (L = first; L <= last; L++) {
    char line[MDT_LINE], buf[MDT_LINE], *cell[MDT_MAXCOL];
    int lcol[MDT_MAXCOL], rcol[MDT_MAXCOL]; char fill[MDT_MAXCOL];
    int n;
    md_raw_line(ed, L, line, sizeof line);
    if (md_is_grid_border(ed, L)) {
      n = md_grid_segments(line, lcol, rcol, fill, MDT_MAXCOL);
      if (n > ncol) ncol = n;
      continue;
    }
    n = md_grid_cells(line, bound, nbound, buf, sizeof buf, cell, MDT_MAXCOL);
    if (n > ncol) ncol = n;
    for (i = 0; i < n; i++) {
      int w = utf8_len(cell[i], (int)strlen(cell[i]));
      if (w > width[i]) width[i] = w;
    }
  }
  for (i = 0; i < ncol; i++) if (width[i] < 3) width[i] = 3;
  *ncolp = ncol;

  for (L = last; L >= first; L--) {
    char line[MDT_LINE], buf[MDT_LINE], out[MDT_LINE + 256], *cell[MDT_MAXCOL];
    int lcol[MDT_MAXCOL], rcol[MDT_MAXCOL]; char fill[MDT_MAXCOL];
    int n, s, e;
    size_t o = 0;
    md_raw_line(ed, L, line, sizeof line);
    out[0] = '\0';

    if (md_is_grid_border(ed, L)) {
      n = md_grid_segments(line, lcol, rcol, fill, MDT_MAXCOL);
      md_put(out, sizeof out, &o, "+");
      for (i = 0; i < ncol; i++) {
        char f = i < n ? fill[i] : '-';
        int lc = i < n && lcol[i], rc = i < n && rcol[i];
        if (lc) md_put(out, sizeof out, &o, ":");
        md_putc(out, sizeof out, &o, f, width[i] + 2 - lc - rc);
        if (rc) md_put(out, sizeof out, &o, ":");
        md_put(out, sizeof out, &o, "+");
      }
    } else {
      n = md_grid_cells(line, bound, nbound, buf, sizeof buf, cell, MDT_MAXCOL);
      if (n == 0) continue;
      md_put(out, sizeof out, &o, "|");
      for (i = 0; i < ncol; i++) {
        const char *t = i < n ? cell[i] : "";
        md_put(out, sizeof out, &o, " ");
        md_put(out, sizeof out, &o, t);
        md_putc(out, sizeof out, &o, ' ', width[i] - utf8_len(t, (int)strlen(t)) + 1);
        md_put(out, sizeof out, &o, "|");
      }
    }

    if (strcmp(line, out)) {
      s = gtcaca_editor_position_from_line(ed, L);
      e = gtcaca_editor_get_line_end_position(ed, L);
      gtcaca_editor_delete_range(ed, s, e - s);
      gtcaca_editor_insert_text(ed, s, out);
    }
  }
}

/* A pipe table reflowed. A column wider than MDT_PADCAP is left unpadded: one
   folded diagram would otherwise push every other row out to its width, which
   is the opposite of the point. */
static void md_pipe_align(gtcaca_editor_widget_t *ed, int first, int last, int *ncolp)
{
  int width[MDT_MAXCOL] = { 0 };
  int L, i, ncol = 0;

  for (L = first; L <= last; L++) {
    char line[MDT_LINE], buf[MDT_LINE], *cell[MDT_MAXCOL];
    int n;
    md_raw_line(ed, L, line, sizeof line);
    n = md_cells(line, buf, sizeof buf, cell, MDT_MAXCOL);
    if (n > ncol) ncol = n;
    if (md_is_sep_line(ed, L)) continue;         /* dashes stretch to fit */
    for (i = 0; i < n; i++) {
      int w = utf8_len(cell[i], (int)strlen(cell[i]));
      if (w > width[i]) width[i] = w;
    }
  }
  for (i = 0; i < ncol; i++) {
    if (width[i] > MDT_PADCAP) width[i] = 0;
    else if (width[i] < 3)     width[i] = 3;
  }
  *ncolp = ncol;

  for (L = last; L >= first; L--) {
    char line[MDT_LINE], buf[MDT_LINE], out[MDT_LINE + 256], *cell[MDT_MAXCOL];
    int n, sep = md_is_sep_line(ed, L), s, e;
    size_t o = 0;
    md_raw_line(ed, L, line, sizeof line);
    n = md_cells(line, buf, sizeof buf, cell, MDT_MAXCOL);
    if (n == 0) continue;

    out[0] = '\0';
    md_put(out, sizeof out, &o, "|");
    for (i = 0; i < n; i++) {
      int w = utf8_len(cell[i], (int)strlen(cell[i]));
      if (sep) {
        /* Keep the colons where they are: they are the column's alignment. */
        int lc = cell[i][0] == ':';
        int rc = w > 0 && cell[i][strlen(cell[i]) - 1] == ':';
        int dashes = (width[i] ? width[i] : 3) - lc - rc;
        if (dashes < 1) dashes = 1;
        md_put(out, sizeof out, &o, " ");
        if (lc) md_put(out, sizeof out, &o, ":");
        md_putc(out, sizeof out, &o, '-', dashes);
        if (rc) md_put(out, sizeof out, &o, ":");
        md_put(out, sizeof out, &o, " |");
        continue;
      }
      md_put(out, sizeof out, &o, " ");
      md_put(out, sizeof out, &o, cell[i]);
      md_putc(out, sizeof out, &o, ' ', width[i] - w);
      md_put(out, sizeof out, &o, " |");
    }

    if (strcmp(line, out)) {
      s = gtcaca_editor_position_from_line(ed, L);
      e = gtcaca_editor_get_line_end_position(ed, L);
      gtcaca_editor_delete_range(ed, s, e - s);
      gtcaca_editor_insert_text(ed, s, out);
    }
  }
}

void md_table_align(gtcaca_editor_widget_t *ed)
{
  int first, last, ncol = 0, grid;

  if (!md_table_range(ed, gtcaca_editor_get_current_line(ed), &first, &last)) {
    snprintf(g_message, sizeof g_message, "Not in a table — M-x md-table makes one");
    return;
  }
  grid = md_table_is_grid(ed, first, last);
  if (grid) md_grid_align(ed, first, last, &ncol);
  else      md_pipe_align(ed, first, last, &ncol);

  gtcaca_editor_set_empty_selection(ed, gtcaca_editor_position_from_line(ed, first));
  g_mark_active = 0;
  snprintf(g_message, sizeof g_message, "%s table aligned — %d columns",
           grid ? "Grid" : "Pipe", ncol);
}

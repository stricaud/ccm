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

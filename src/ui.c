#include "cacamacs.h"
#include "hex.h"
/* ── file browser (open a directory to navigate it) ────────────────────────── */

typedef struct { char name[256]; int isdir; } bent_t;

int _cmp_bent(const void *a, const void *b)
{
  const bent_t *x = a, *y = b;
  if (x->isdir != y->isdir) return y->isdir - x->isdir;   /* directories first */
  return strcmp(x->name, y->name);
}

/* Is a browser entry a directory? Prefer dirent's d_type (fast, no stat) and
   fall back to stat() when the filesystem reports DT_UNKNOWN. MinGW-w64's
   struct dirent has no d_type field at all, so it always takes the stat path;
   macOS/Linux have d_type but macOS omits glibc's _DIRENT_HAVE_D_TYPE, hence the
   guard is simply "not Windows". */
static int entry_is_dir(const char *dir, const struct dirent *de)
{
  char p[PATH_MAX]; struct stat st; int need;
#ifndef _WIN32
  if (de->d_type == DT_DIR)      return 1;
  if (de->d_type != DT_UNKNOWN)  return 0;
#endif
  need = snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
  return (need > 0 && need < (int)sizeof p && stat(p, &st) == 0) ? S_ISDIR(st.st_mode) : 0;
}

/* Where the browser is: a real directory, or inside the archive archive.c has
   cached. g_arcsub is how deep inside ("" at its root). See populate_archive. */
static int  g_in_arc = 0;
static char g_arcsub[PATH_MAX] = "";

/* Render the directory listing into the read-only browser editor. */
void populate_browser(const char *dir)
{
  DIR *d = opendir(dir);
  struct dirent *de;
  bent_t ents[2048];
  int n = 0, i, total, len;
  char *listing;
  char resolved[PATH_MAX];

  /* Canonicalise into a SEPARATE buffer first: `dir` may alias g_curdir
     (show_browser passes g_curdir), and realpath() with src==dst is UB. */
  if (ccm_realpath(dir, resolved)) { strncpy(g_curdir, resolved, sizeof g_curdir - 1); g_curdir[sizeof g_curdir - 1] = '\0'; }
  else if (dir != g_curdir)    { strncpy(g_curdir, dir, sizeof g_curdir - 1);      g_curdir[sizeof g_curdir - 1] = '\0'; }

  if (d) {
    while ((de = readdir(d)) != NULL && n < 2048) {
      int isdir;
      if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
      isdir = entry_is_dir(g_curdir, de);
      strncpy(ents[n].name, de->d_name, sizeof ents[n].name - 1);
      ents[n].name[sizeof ents[n].name - 1] = '\0';
      ents[n].isdir = isdir;
      n++;
    }
    closedir(d);
    qsort(ents, (size_t)n, sizeof(bent_t), _cmp_bent);
  }

  /* one entry per line; ".." first so you can always go up */
  total = 3;  /* "../" */
  for (i = 0; i < n; i++) total += 1 + (int)strlen(ents[i].name) + (ents[i].isdir ? 1 : 0);
  listing = malloc((size_t)total + 1);
  if (!listing) return;
  strcpy(listing, "../"); len = 3;
  for (i = 0; i < n; i++) {
    listing[len++] = '\n';
    strcpy(listing + len, ents[i].name); len += (int)strlen(ents[i].name);
    if (ents[i].isdir) listing[len++] = '/';
  }
  listing[len] = '\0';

  gtcaca_editor_set_read_only(g_browser_ed, 0);
  gtcaca_editor_set_text(g_browser_ed, listing);
  gtcaca_editor_set_read_only(g_browser_ed, 1);
  gtcaca_editor_goto_pos(g_browser_ed, 0);
  free(listing);

  g_in_arc = 0;                 /* a real directory: out of any archive */
  g_browser_win->window_title = g_curdir;
  if (!d) snprintf(g_message, sizeof g_message, "Cannot read %s", dir);
  else    snprintf(g_message, sizeof g_message, "%d item%s", n, n == 1 ? "" : "s");
  /* The status bar is shared with the editor behind us, and whoever wrote it
     last wins — so the listing refreshes it rather than leaving the bar
     describing a buffer that is not on screen, or a directory we have since
     left for an archive. */
  browser_modeline(g_browser_ed, NULL);
}

/* ── the browser inside an archive ────────────────────────────────────────────
 *
 * The listing is the same read-only editor; only where the names come from
 * changes. `g_in_arc` says which, and `g_arcsub` is how deep we are inside the
 * archive ("" at its root) — the archive itself is whichever one archive.c has
 * cached, so there is one piece of state and no way for the two to disagree.
 *
 * A member is addressed as "<archive>!<member>" once it is in a buffer. That is
 * not a path anything can open, which is the point: the buffer carries
 * arc_member and never writes.
 */
void populate_archive(const char *sub)
{
  ccm_arc_child_t *kids = NULL;
  char *listing;
  int nk = 0, i, total;
  size_t len;

  if (sub != g_arcsub) {
    strncpy(g_arcsub, sub ? sub : "", sizeof g_arcsub - 1);
    g_arcsub[sizeof g_arcsub - 1] = '\0';
  }
  g_in_arc = 1;

  if (ccm_archive_children(g_arcsub, &kids, &nk) != 0) {
    snprintf(g_message, sizeof g_message, "No archive is open");
    g_in_arc = 0;
    return;
  }

  total = 3;  /* "../" */
  for (i = 0; i < nk; i++) total += 1 + (int)strlen(kids[i].name) + (kids[i].isdir ? 1 : 0);
  listing = malloc((size_t)total + 1);
  if (!listing) { free(kids); return; }
  strcpy(listing, "../"); len = 3;
  for (i = 0; i < nk; i++) {
    listing[len++] = '\n';
    strcpy(listing + len, kids[i].name); len += strlen(kids[i].name);
    if (kids[i].isdir) listing[len++] = '/';
  }
  listing[len] = '\0';

  gtcaca_editor_set_read_only(g_browser_ed, 0);
  gtcaca_editor_set_text(g_browser_ed, listing);
  gtcaca_editor_set_read_only(g_browser_ed, 1);
  gtcaca_editor_goto_pos(g_browser_ed, 0);
  free(listing);

  /* The window title is the one place the whole location fits, so it carries
     the archive and the path inside it; g_curdir still names the directory the
     archive sits in, which is where ".." at the root goes back to. */
  { static char title[PATH_MAX + 64];
    snprintf(title, sizeof title, "%s!/%s", ccm_archive_path(), g_arcsub);
    g_browser_win->window_title = title; }

  snprintf(g_message, sizeof g_message, "%d item%s (read-only, inside %s)",
           nk, nk == 1 ? "" : "s", ccm_base_name(ccm_archive_path()));
  browser_modeline(g_browser_ed, NULL);
  free(kids);
}

/* Step into `path` as an archive and show its root. 0 if we did. */
int browse_archive(const char *path)
{
  char err[256] = "";
  if (ccm_archive_enter(path, err, sizeof err) != 0) {
    snprintf(g_message, sizeof g_message, "%s", err[0] ? err : "cannot read that archive");
    /* The listing behind us was drawn before this failed, and its "N items"
       went to the bar with it; without this the refusal never appears. */
    browser_modeline(g_browser_ed, NULL);
    return -1;
  }
  /* Where ".." at the archive root returns to. */
  { char dir[PATH_MAX]; const char *slash = strrchr(path, '/');
    if (slash && slash != path) {
      size_t n = (size_t)(slash - path);
      if (n >= sizeof dir) n = sizeof dir - 1;
      memcpy(dir, path, n); dir[n] = '\0';
      strncpy(g_curdir, dir, sizeof g_curdir - 1); g_curdir[sizeof g_curdir - 1] = '\0';
    } }
  populate_archive("");
  return 0;
}

static int g_browser_open = 0;
int browser_is_open(void) { return g_browser_open; }

/* The editor a command should act on.
 *
 * `g_ed` is the buffer in the focused window, which is not the same thing as
 * the editor in front of you: the browser and the help window are modal panes
 * over it with editors of their own. A key handler is handed the right one, but
 * an M-x command is run from a minibuffer callback long after the key that
 * started it, and has only the globals to go on — which is how M-x set-mark in
 * the browser came to set a mark in the file behind it and report "Mark set",
 * and M-x copy-region then found no region anywhere. */
gtcaca_editor_widget_t *ccm_active_editor(void)
{
  if (g_browser_open) return g_browser_ed;
  if (g_help_open)    return g_help_ed;
  return g_ed;
}

void show_browser(void)
{
  if (!g_curdir[0]) { if (!ccm_realpath(".", g_curdir)) strcpy(g_curdir, "."); }
  pane_hide_all();                 /* modal: only the browser is visible */
  gtcaca_widget_show(GTCACA_WIDGET(g_browser_win));
  gtcaca_widget_show(GTCACA_WIDGET(g_browser_ed));
  g_browser_open = 1;           /* before the listing: browser_modeline checks it */
  populate_browser(g_curdir);
  gtcaca_window_set_focus(g_browser_win);
  gtcaca_window_set_focused_child(g_browser_win, GTCACA_WIDGET(g_browser_ed));
}

void hide_browser(void)
{
  gtcaca_widget_hide(GTCACA_WIDGET(g_browser_ed));
  gtcaca_widget_hide(GTCACA_WIDGET(g_browser_win));
  g_browser_open = 0;
  pane_show_all();                 /* restore the editor panes and focus */
}

/* ── help viewer (M-x help) ─────────────────────────────────────────────────── */

const char *help_text(void)
{
  return
  "cacamacs key bindings        (Up/Down/PageUp/PageDown or C-v scroll, C-s search,\n"
  "                              q or Esc Esc closes this window)\n"
  "\n"
  "Motion   C-f C-b C-n C-p   C-a C-e   C-v   arrows / Home / End / PageUp / PageDown\n"
  "         M-< / M->   beginning / end of buffer   M-g M-g (or M-x goto-line) jump to line\n"
  "         C-n / C-p step through wrapped rows when line wrap is on\n"
  "Edit     C-d or Del delete-fwd   Backspace delete-back   C-k kill-line\n"
"         Tab indent / complete   M-Del / M-Backspace kill word fwd / back\n"
  "Words    M-f / M-b move    M-d / M-DEL kill    M-u / M-l upcase / downcase\n"
  "Region   C-Space mark   C-w kill   M-w (Esc w) copy   C-y yank   C-g cancel\n"
  "Rect     C-x SPC rectangle mark, move to opposite corner, then:\n"
  "         C-x r t insert string per line   C-x r k kill   C-x r y yank\n"
  "         C-x r d delete   C-x r c blank   (C-w / M-w act on the box too)\n"
  "Comment  M-;  comment / uncomment the line or region\n"
  "Lines    C-x C-t transpose lines   C-t transpose chars\n"
  "Search   C-s isearch fwd   C-r isearch back\n"
  "Replace  M-% query-replace: y replace, n skip, ! all, . last, q quit\n"
  "         M-x query-replace-regexp (regex)   M-x replace-string (all)\n"
  "Spell    M-$ check word at point   (0-9 pick a suggestion, SPC skip)\n"
  "Mouse    click to move point   drag to select   wheel to scroll\n"
  "Macros   C-x ( record   C-x ) finish   C-x e replay   (C-u N C-x e = N times)\n"
  "Repeat   C-u N <command>   runs the next command N times (bare C-u = 4)\n"
  "Windows  C-x 2 split below   C-x 3 split right   C-x 1 one   C-x 0 close\n"
  "         C-x o other window   C-x b switch buffer\n"
  "         (a split shows the same file, same place, in both windows)\n"
  "Files    C-x C-f find file   C-x C-s save   C-x C-w write-as   C-x C-c quit\n"
  "         C-x d directory browser   M-x revert-buffer re-read from disk\n"
  "         in the browser: Enter opens, M-< / M-> jump to either end, q or\n"
  "         Esc Esc closes, C-x C-c quits ccm, C-x C-f opens by name,\n"
  "         C-x b switches buffer, C-x d re-reads it\n"
  "         a name can be lifted out of it like any other text: C-space (or\n"
  "         M-x set-mark), C-e, M-w — then q and C-y where you want it\n"
  "Packed   a .gz opens as its text and saves back compressed; .bz2 and .xz\n"
  "         open read-only (ccm only writes gzip)\n"
  "         a .zip or .tar[.gz|.bz2|.xz] opens as a folder to walk: Enter\n"
  "         descends, Enter on a file shows it read-only, .. at the top comes\n"
  "         back out — C-x C-w writes a member out to a file you can edit\n"
  "         a file changed by someone else is never overwritten or discarded\n"
  "         silently: editing, saving or re-opening it asks first\n"
  "         the prompt starts in the current file's directory: type a name or\n"
  "         Tab to complete; Enter on a directory lists it; C-a C-e C-k M-DEL edit\n"
  "View     C-x l line numbers   C-x f folding   C-x t toggle fold   C-x a annotate\n"
  "         C-x C-l line wrap (on by default)   C-x w whitespace   C-l recenter\n"
  "Pretty   C-x p pretty-print JSON/XML (auto: selection / buffer / line)\n"
  "         M-x json-pretty-print · xml-pretty-print (current line)\n"
  "Complete Tab at a prompt completes; candidates that do not fit on the echo\n"
  "         line open a *Completions* window — an ordinary buffer, so C-x o\n"
  "         steps into it, C-s searches it and C-v scrolls it. Enter there runs\n"
  "         the M-x command under the caret (a filename goes to the prompt\n"
  "         instead); q or Esc Esc closes it\n"
  "Insert   M-x insert-date  2026-08-11      M-x insert-time  + 14:03:27\n"
  "Markdown M-x md-<Tab> lists them all. On the region, else the line or word;\n"
  "         each one run twice takes the markup back off.\n"
  "         md-title / md-subtitle  underline the line with === or ---\n"
  "         md-h1 … md-h6  # headings    md-hr  horizontal rule\n"
  "         md-bold  md-italic  md-code  md-strike  md-link\n"
  "         md-list  md-ordered  md-task  md-quote\n"
  "         md-table  a two-column pandoc grid table; the region becomes\n"
  "         the left cell, keeping its line breaks (a graph, a diagram)\n"
  "         with nothing selected it takes the ``` block point is in, so\n"
  "         M-x diagram then M-x md-table needs no selecting in between\n"
  "         md-github-table  the GFM pipe form — GitHub draws it, pandoc\n"
  "         drops the <br> fold it needs\n"
  "         md-table-row  md-table-col  md-table-align  (either shape)\n"
  "Binary   a file with a NUL byte in it opens as hex with the ASCII beside it,\n"
  "         and the bytes are what you edit and save — 0-9 a-f type over the\n"
  "         byte, Tab crosses to the ASCII side, C-s searches for bytes\n"
  "         (de ad be ef) or for text (\"GET /\"), M-g goes to an offset, and\n"
  "         C-x C-s writes the file back. ? inside the mode lists every key.\n"
  "         M-x hex-mode shows any file that way; M-x hex-mode-exit goes back\n"
  "Diagram  M-x diagram  draw an ASCII diagram and drop it in at the cursor\n"
  "         (select existing art first to edit it; q inserts, then C-x C-s)\n"
  "         shapes, links, an eraser — ? inside the mode lists every key\n"
  "Encrypt  ccm -k FILE  keeps FILE encrypted with GnuPG. On its own, -k asks\n"
  "         for a passphrase in a box and again to confirm it — no keyring\n"
  "         needed. -kKEY (or --key=KEY) encrypts to that public key instead.\n"
  "         Opening an encrypted file needs no -k: ccm knows one and asks for\n"
  "         the passphrase, and every C-x C-s writes it back encrypted. The\n"
  "         plain text is never written to disk, and a buffer still locked is\n"
  "         never saved over it\n"
  "         M-x set-encryption-key  start encrypting an open file, or change\n"
  "         what it goes back to   \"gpg-program\" in config.json picks the\n"
  "         binary to run\n"
  "Undo     C-_   (also C-/, C-x u, M-x undo)  Redo   C-x C-_  (or M-x redo)\n"
  "Paste    the terminal's paste goes in as one edit — C-_ takes it back whole\n"
  "Help     M-x help\n";
}

/* The help window showing any text — the key bindings, or the explanation
   behind a question (C-h at the "changed on disk" prompt; see lock.c). */
void show_help_with(const char *title, const char *text)
{
  pane_hide_all();                 /* modal: only the help window is visible */
  g_help_win->window_title = (char *)title;   /* borrowed, as everywhere else */
  gtcaca_widget_show(GTCACA_WIDGET(g_help_win));
  gtcaca_widget_show(GTCACA_WIDGET(g_help_ed));
  gtcaca_editor_set_read_only(g_help_ed, 0);
  gtcaca_editor_set_text(g_help_ed, text);
  gtcaca_editor_set_read_only(g_help_ed, 1);
  gtcaca_editor_goto_pos(g_help_ed, 0);
  gtcaca_window_set_focus(g_help_win);
  gtcaca_window_set_focused_child(g_help_win, GTCACA_WIDGET(g_help_ed));
  g_help_open = 1;
}

void show_help(void) { show_help_with("Help — key bindings", help_text()); }

void hide_help(void)
{
  gtcaca_widget_hide(GTCACA_WIDGET(g_help_ed));
  gtcaca_widget_hide(GTCACA_WIDGET(g_help_win));
  g_help_open = 0;
  pane_show_all();
}

/* ── completions buffer (Tab at a prompt) ──────────────────────────────────
 * Emacs' *Completions*: when a Tab has more candidates than the echo line can
 * hold, they go into an ordinary buffer shown in a split window. Ordinary is
 * the point — C-x o walks into it, C-s searches it, C-x 0 closes it, and a
 * terminal resize relayouts it, all through the machinery the editor already
 * has. What is left here is only what is specific to completing: filling the
 * buffer, and taking the candidate under the caret when Enter is pressed.
 *
 * COMPLETE_RUN candidates (M-x) run on Enter; COMPLETE_INSERT ones (filenames)
 * only go to the prompt, since choosing a name is not agreeing to write to it. */
static int g_comp_buf = -1;        /* its buffer, kept between uses */
static int g_comp_mode = COMPLETE_RUN;

/* "Open" is not a flag to keep in step — it is simply whether the buffer is on
   screen. So C-x 0 and C-x 1 close the completions window like any other, and
   nothing here has to know they happened. */
int completions_open(void)
{
  return g_comp_buf >= 0 && g_comp_buf < g_nbuf && g_buffers[g_comp_buf].pane >= 0;
}

/* Is the caret in the completions window? Then it takes the keys, not the
   prompt — which is otherwise swallowing everything while it is active. */
int completions_focused(void)
{
  return completions_open() && g_cur_buf == g_comp_buf;
}

/* Lay the candidates out in columns that fit the window, like Emacs does: a
   long M-x list is then readable in a three-line window. */
static char *comp_layout(const char *const *items, int n, int width)
{
  int i, longest = 0, cols, col = 0;
  size_t cap = 1, off = 0;
  char *text;

  for (i = 0; i < n; i++) {
    int l = (int)strlen(items[i]);
    if (l > longest) longest = l;
    cap += (size_t)l + 3;
  }
  cols = (width - 2) / (longest + 2);
  if (cols < 1) cols = 1;
  cap += (size_t)n * (size_t)(longest + 2) + 8;

  text = malloc(cap);
  if (!text) return NULL;
  text[0] = '\0';
  for (i = 0; i < n; i++) {
    int last = (col == cols - 1) || (i == n - 1);
    off += (size_t)snprintf(text + off, cap - off, "%-*s%s",
                            last ? 0 : longest + 2, items[i], last ? "\n" : "");
    col = last ? 0 : col + 1;
  }
  return text;
}

void completions_show(const char *const *items, int n, int mode)
{
  int width = 60;

  if (n <= 0) return;
  if (g_comp_buf < 0 || g_comp_buf >= g_nbuf) {
    g_comp_buf = buffer_create(NULL);
    if (g_comp_buf < 0) return;
    snprintf(g_buffers[g_comp_buf].path, sizeof g_buffers[g_comp_buf].path, "*Completions*");
    gtcaca_editor_set_line_numbers(g_buffers[g_comp_buf].ed, 0);
    gtcaca_editor_set_caret_line_visible(g_buffers[g_comp_buf].ed, 1);
  }
  g_comp_mode = mode;

  /* Give it a window before filling it, so the layout can use the real width. */
  if (g_buffers[g_comp_buf].pane < 0) pane_split_with(g_comp_buf, 1);
  if (g_buffers[g_comp_buf].pane >= 0) {
    int w = g_nodes[g_buffers[g_comp_buf].pane].w;
    if (w > 4) width = w;
  }

  {
    char *text = comp_layout(items, n, width);
    gtcaca_editor_widget_t *ed = g_buffers[g_comp_buf].ed;
    if (!text) return;
    gtcaca_editor_set_read_only(ed, 0);
    gtcaca_editor_set_text(ed, text);
    gtcaca_editor_set_read_only(ed, 1);
    gtcaca_editor_goto_pos(ed, 0);
    free(text);
  }
}

void completions_hide(void)
{
  int comp_pane, keep;
  if (!completions_open()) return;
  comp_pane = g_buffers[g_comp_buf].pane;
  keep = g_focus_leaf;                             /* the pane to come back to */
  focus_pane(comp_pane);
  pane_delete_current();
  if (keep != comp_pane && g_nodes[keep].used && g_nodes[keep].is_leaf) focus_pane(keep);
}

/* Enter in the completions window: run (or insert) the candidate the caret is
   sitting on — the word under point, since the layout puts several per line. */
void completions_pick_at_point(void)
{
  gtcaca_editor_widget_t *ed = g_buffers[g_comp_buf].ed;
  int line = gtcaca_editor_get_current_line(ed);
  int s = gtcaca_editor_position_from_line(ed, line);
  int e = gtcaca_editor_get_line_end_position(ed, line);
  int pos = gtcaca_editor_get_current_pos(ed);
  char row[512], pick[PATH_MAX];
  int i, a, b, mode = g_comp_mode;

  if (e - s <= 0 || e - s >= (int)sizeof row) return;
  gtcaca_editor_get_text_range(ed, s, e, row, sizeof row);
  i = pos - s;
  if (i < 0) i = 0;
  if (i > (int)strlen(row)) i = (int)strlen(row);
  if (row[i] == ' ' && i > 0) i--;                 /* caret in the padding: take the word left */
  a = i; while (a > 0 && row[a - 1] != ' ') a--;
  b = i; while (row[b] && row[b] != ' ') b++;
  if (b <= a) return;
  snprintf(pick, (size_t)(b - a) + 1 < sizeof pick ? (size_t)(b - a) + 1 : sizeof pick, "%s", row + a);

  completions_hide();
  if (mode == COMPLETE_RUN) minibuffer_accept(pick);
  else                      minibuffer_replace_tail(pick);
}

/* Open one member of the cached archive into a read-only buffer.
 *
 * The bytes go from the archive straight into the editor, so nothing is
 * unpacked to disk. The buffer's name is "<archive>!<member>", which no
 * filesystem call will ever resolve — deliberately, since the buffer must not
 * be writable: arc_member makes write_buffer_file refuse, and the editor is set
 * read-only so it never comes up. The name still ends in the member's own
 * extension, which is what gives it the right language. */
static void open_archive_member(const char *member)
{
  char err[256] = "", pseudo[PATH_MAX];
  char *text;
  size_t len = 0;
  int bi, need;

  text = ccm_archive_extract(member, &len, err, sizeof err);
  if (!text) {
    snprintf(g_message, sizeof g_message, "%s", err[0] ? err : "cannot read that member");
    return;
  }
  /* A member may be compressed in its own right — doc/changelog.gz inside a
     package is the everyday case — and what the archive hands back is then
     still the compressed bytes. Peel that layer too, so it reads as text like
     the same file would outside the archive. If it will not decompress, keep
     the bytes and let the text check below turn it down. */
  { int mfmt = ccm_comp_format(member);
    if (mfmt != CCM_COMP_NONE && ccm_comp_supported(mfmt)) {
      size_t dlen = 0;
      char *inner = ccm_comp_read_mem(text, len, mfmt, &dlen, err, sizeof err);
      if (inner) { free(text); text = inner; len = dlen; }
    } }
  need = snprintf(pseudo, sizeof pseudo, "%s!%s", ccm_archive_path(), member);
  if (need < 0 || need >= (int)sizeof pseudo) {
    free(text);
    snprintf(g_message, sizeof g_message, "Name too long");
    return;
  }

  /* There is no file at that name, so the buffer comes up empty and we fill it;
     the language was already set from the extension on the way through. */
  bi = buffer_create(pseudo);
  if (bi < 0) { free(text); snprintf(g_message, sizeof g_message, "Too many buffers"); return; }
  g_buffers[bi].arc_member = 1;
  /* A member called `changelog.gz` would have had buffer_create take the name
     for a compressed file (it found nothing at it, so the buffer is simply
     empty). It is a member, and the archive has already decompressed it. */
  g_buffers[bi].comp = CCM_COMP_NONE;
  g_buffers[bi].comp_ro = 0;
  g_buffers[bi].comp_note[0] = '\0';
  if (!ccm_is_text(text, len)) {
    /* An icon, a .so, a font: bytes, so the member is shown as bytes. It used
       to be turned away with "holds binary data, not text", which left no way
       to look inside an archive at the very files you open an archive for.
       Read-only like every member, and ccm_hex_adopt takes the block over. */
    if (ccm_hex_adopt(bi, (uint8_t *)text, (long)len, 1, NULL) != 0) return;
  } else {
    gtcaca_editor_set_read_only(g_buffers[bi].ed, 0);
    gtcaca_editor_set_text(g_buffers[bi].ed, text);
    gtcaca_editor_empty_undo_buffer(g_buffers[bi].ed);
    gtcaca_editor_set_save_point(g_buffers[bi].ed);
    gtcaca_editor_set_read_only(g_buffers[bi].ed, 1);
    free(text);
  }

  /* Before focus_pane, not after: focus_pane refreshes the modeline, and a
     message set afterwards would not appear until the next keystroke. */
  snprintf(g_message, sizeof g_message, "%s (read-only, inside %s)",
           member, ccm_base_name(ccm_archive_path()));
  hide_browser();
  pane_show_buffer(g_focus_leaf, bi);
  relayout();
  focus_pane(g_focus_leaf);
}

/* Enter on an entry while the listing is showing the inside of an archive. */
static void archive_open_current(const char *entry)
{
  char sub[PATH_MAX];
  int need;

  if (!strcmp(entry, "../")) {
    /* At the root this leaves the archive for the directory holding it; the
       cached archive goes with it, so a second visit re-reads a file that may
       have changed in the meantime. */
    if (!g_arcsub[0]) {
      char dir[PATH_MAX];
      snprintf(dir, sizeof dir, "%s", g_curdir);
      ccm_archive_close();
      g_in_arc = 0;
      populate_browser(dir);
      return;
    }
    { char *slash = strrchr(g_arcsub, '/');
      if (slash) *slash = '\0';
      else g_arcsub[0] = '\0'; }
    populate_archive(g_arcsub);
    return;
  }

  { size_t l = strlen(entry);
    int isdir = (l && entry[l - 1] == '/');
    char name[NAME_MAX + 2];
    snprintf(name, sizeof name, "%s", entry);
    if (isdir) name[strlen(name) - 1] = '\0';

    need = g_arcsub[0] ? snprintf(sub, sizeof sub, "%s/%s", g_arcsub, name)
                       : snprintf(sub, sizeof sub, "%s", name);
    if (need < 0 || need >= (int)sizeof sub) {
      snprintf(g_message, sizeof g_message, "Name too long");
      return;
    }
    if (isdir) populate_archive(sub);
    else       open_archive_member(sub);
  }
}

/* Open whatever entry the caret is on: descend into a directory or an archive,
   or open a file. */
void browser_open_current(void)
{
  gtcaca_editor_widget_t *ed = g_browser_ed;
  int line = gtcaca_editor_get_current_line(ed);
  int s = gtcaca_editor_position_from_line(ed, line);
  int e = gtcaca_editor_get_line_end_position(ed, line);
  char entry[NAME_MAX + 2], path[PATH_MAX];
  struct stat st;
  size_t l;
  int need;

  gtcaca_editor_get_text_range(ed, s, e, entry, sizeof entry);
  if (entry[0] == '\0') return;

  if (g_in_arc) { archive_open_current(entry); return; }

  if (!strcmp(entry, "../")) need = snprintf(path, sizeof path, "%s/..", g_curdir);
  else {
    l = strlen(entry); if (l && entry[l - 1] == '/') entry[l - 1] = '\0';
    need = snprintf(path, sizeof path, "%s/%s", g_curdir, entry);
  }
  if (need < 0 || need >= (int)sizeof path) { snprintf(g_message, sizeof g_message, "Path too long"); return; }
  if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) populate_browser(path);
  /* A .zip or .tar.gz is a directory as far as the browser is concerned: step
     into it rather than opening the container as text. */
  else if (ccm_is_archive(path))                   browse_archive(path);
  else                                             open_path_in_editor(path);
}

/* Modeline shown while the browser is focused. */
void browser_modeline(gtcaca_editor_widget_t *ed, void *ud)
{
  char t[512], dirbuf[128];
  /* Keep the command hints visible: cap the directory to half the bar (the
     path is the browser's main content, so it gets a bigger budget than the
     editor's file name). */
  int w = caca_get_canvas_width(gmo.cv), budget = w / 2;
  const char *dir;
  (void)ed; (void)ud;
  /* gtcaca calls this after every key the browser's editor has handled — which
     includes the Enter that just closed the browser to open a file. Writing the
     bar then would bury the modeline focus_pane has only now set for the file,
     so the browser stops claiming the bar the moment it is no longer in front. */
  if (!g_browser_open) return;
  if (budget < 16) budget = 16;
  if (budget > 60) budget = 60;
  /* Inside an archive the location is the archive plus how deep we are in it —
     g_curdir only names the directory the archive sits in, which is where ".."
     eventually leads but not where the listing is. */
  if (g_in_arc) {
    char where[PATH_MAX + 64];
    snprintf(where, sizeof where, "%s!/%s", ccm_base_name(ccm_archive_path()), g_arcsub);
    dir = compact_path(where, dirbuf, sizeof dirbuf, budget);
  } else {
    dir = compact_path(g_curdir, dirbuf, sizeof dirbuf, budget);
  }
  if (g_message[0]) snprintf(t, sizeof t, "  Browser  %s   %s", dir, g_message);
  else snprintf(t, sizeof t, "  Browser  %s   arrows move  C-s search  Enter open  q quit", dir);
  gtcaca_statusbar_set_text(g_modeline, t);
}

/* ── buffers & split panes ─────────────────────────────────────────────────── */

int buf_index_of(gtcaca_editor_widget_t *ed)
{
  int i; for (i = 0; i < g_nbuf; i++) if (g_buffers[i].ed == ed) return i; return -1;
}

/* The focused buffer's per-file state lives in the globals while focused; these
   move it to/from the buffer record when focus changes. */
void buffer_store_globals(int bi)
{
  buffer_t *b;
  if (bi < 0 || bi >= g_nbuf) return;
  b = &g_buffers[bi];
  b->langcfg = g_langcfg; b->grammar = g_grammar;
  b->keywords = g_keywords; b->n_keywords = g_n_keywords; b->folding = g_folding;
  strncpy(b->langname, g_langname, sizeof b->langname - 1); b->langname[sizeof b->langname - 1] = '\0';
}
void buffer_load_globals(int bi)
{
  buffer_t *b;
  if (bi < 0 || bi >= g_nbuf) return;
  b = &g_buffers[bi];
  g_ed = b->ed; g_langcfg = b->langcfg; g_grammar = b->grammar;
  g_keywords = b->keywords; g_n_keywords = b->n_keywords; g_folding = b->folding;
  strncpy(g_langname, b->langname, sizeof g_langname - 1); g_langname[sizeof g_langname - 1] = '\0';
  /* A named buffer with no file (*Completions*) still shows its name on the
     modeline; a plain scratch buffer has none and falls back to *scratch*. */
  g_filename = (b->has_file || b->path[0]) ? b->path : NULL;
}

/* Create a buffer for `path` (NULL = scratch); dedups by path. Sets up the
   buffer's language without disturbing the currently-focused globals. */
int buffer_create(const char *path)
{
  buffer_t *b;
  int bi, i;
  char abs[PATH_MAX];

  /* Buffers hold absolute paths: `ccm file.txt` must still be able to show the
     file it opened in the Find-file prompt, and it makes the reuse test below
     see "file.txt" and "$PWD/file.txt" as the one buffer they are. */
  if (path) {
    ccm_abs_path(path, abs, sizeof abs);
    path = abs;
    for (i = 0; i < g_nbuf; i++)
      if (g_buffers[i].has_file && !strcmp(g_buffers[i].path, path)) return i;
  }
  if (g_nbuf >= MAXBUF) return -1;
  bi = g_nbuf++;
  b = &g_buffers[bi];
  memset(b, 0, sizeof *b);
  b->pane = -1;
  b->view_of = -1;
  strcpy(b->langname, "fundamental");
  b->ed = gtcaca_editor_new(NULL, 0, 0, 1, 1);
  gtcaca_editor_set_line_numbers(b->ed, 1);
  gtcaca_editor_set_wrap(b->ed, 1);              /* wrap long lines by default */
  gtcaca_editor_key_cb_register(b->ed, on_key, NULL);
  gtcaca_editor_set_update_cb(b->ed, refresh_modeline, NULL);
  gtcaca_editor_set_caret_line_visible(b->ed, 1);
  gtcaca_editor_set_caret_line_back(b->ed, CACA_BLACK);
  gtcaca_editor_set_edge_column(b->ed, g_cfg_edge);
  gtcaca_editor_set_tab_width(b->ed, g_cfg_tab > 0 ? g_cfg_tab : 8);
  ccm_theme_apply_editor(b->ed);                 /* ~/.cacamacs/theme colours, if any */
  gtcaca_widget_hide(GTCACA_WIDGET(b->ed));
  if (path) {
    strncpy(b->path, path, sizeof b->path - 1); b->path[sizeof b->path - 1] = '\0';
    b->has_file = 1;
    if (ccm_file_is_encrypted(path)) {
      /* Nothing is read here. The passphrase is asked for in the minibuffer,
         which answers through a callback well after this returns, so the
         buffer starts empty and read-only and ccm_crypt_begin takes it from
         there — see crypt.c. */
      ccm_crypt_lock(bi);
    } else if (ccm_comp_format(path) != CCM_COMP_NONE) {
      /* .gz and friends: the buffer gets the text, not the compressed bytes,
         and a save puts the bytes back (compress.c). An archive never reaches
         here — open_path_in_editor sends those to the browser instead. */
      ccm_comp_open(bi, ccm_comp_format(path));
    } else if (ccm_file_is_binary(path)) {
      /* A NUL byte is the end of a C string, so the editor cannot hold this
         file: it would come up empty from the first NUL on, and saving it
         would write that emptiness back. It opens as bytes instead — hex with
         the ASCII beside it, which is also something you can edit (hex.c).
         If even that will not have it (too large to hold), the buffer comes up
         empty and the reason has to survive setup_language's own message,
         which is what comp_note is for. */
      if (ccm_hex_open_file(bi, 0, NULL) != 0)
        snprintf(b->comp_note, sizeof b->comp_note, "%s", g_message);
    } else {
      char *c = read_file(path);
      gtcaca_editor_set_text(b->ed, c ? c : ""); free(c);
    }
    ccm_stamp_buffer(bi);   /* what the file looked like when we visited it */
    /* Here rather than in the callers: every route to a file — the command
       line, C-x C-f, the browser, a pane split — lands in buffer_create, and
       the early return above means a buffer that is merely revisited is not
       logged a second time. */
    ccm_log_file('O', b->path);
  }
  gtcaca_editor_empty_undo_buffer(b->ed);
  gtcaca_editor_set_save_point(b->ed);

  /* A hex buffer has no language: it is bytes, and its modeline says "hex". */
  if (b->has_file && !b->hex) {
    /* run language setup with globals temporarily pointed at this buffer */
    gtcaca_editor_widget_t *sed = g_ed; const char *sfn = g_filename;
    gtcaca_editor_langcfg_t *slc = g_langcfg; gtcaca_editor_grammar_t *sgr = g_grammar;
    const char **skw = g_keywords; int snk = g_n_keywords, sfold = g_folding; char sln[64];
    strncpy(sln, g_langname, sizeof sln - 1); sln[sizeof sln - 1] = '\0';

    g_ed = b->ed; g_filename = b->path; g_langcfg = NULL; g_grammar = NULL;
    g_keywords = NULL; g_n_keywords = 0; strcpy(g_langname, "fundamental"); g_folding = 0;
    /* secrets.txt.gpg is a text file; the .gpg says how it is stored, not what
       is in it. So the language comes from the name underneath. */
    /* notes.txt.gz is a text file too: the .gz (like the .gpg) says how it is
       stored, not what is in it, so the language comes from the name
       underneath. Only when there *is* a name underneath — bare `archive.gz`
       keeps its own, there being no inner extension to find. */
    { char inner[PATH_MAX]; const char *dot;
      strncpy(inner, b->path, sizeof inner - 1); inner[sizeof inner - 1] = '\0';
      dot = strrchr(inner, '.');
      if ((b->crypt || b->comp) && dot && strchr(inner, '.') != dot &&
          (!strcasecmp(dot, ".gpg") || !strcasecmp(dot, ".pgp") || !strcasecmp(dot, ".asc") ||
           !strcasecmp(dot, ".gz")  || !strcasecmp(dot, ".bz2") || !strcasecmp(dot, ".xz")))
        inner[dot - inner] = '\0';
      setup_language(b->ed, inner, NULL); }
    buffer_store_globals(bi);

    g_ed = sed; g_filename = sfn; g_langcfg = slc; g_grammar = sgr;
    g_keywords = skw; g_n_keywords = snk; g_folding = sfold;
    strncpy(g_langname, sln, sizeof g_langname - 1); g_langname[sizeof g_langname - 1] = '\0';
  }
  /* Last, so it is not overwritten: setup_language above reports a name it has
     no language for, and that would otherwise bury why this buffer is empty or
     read-only (see ccm_comp_open). */
  if (b->comp_note[0]) snprintf(g_message, sizeof g_message, "%s", b->comp_note);
  return bi;
}

/* A second buffer onto the file `src` is visiting, its editor a *view* of
   src's: one document — same text, same undo history, same modified flag —
   with a caret and a scroll position of its own. That is what lets one file be
   open in two windows at once, since a buffer owns exactly one editor widget
   and an editor widget can only be in one pane.

   Everything the language and the look depend on is copied, and so is the
   file's stamp: the two buffers visit the same file and must agree about what
   is on disk, or saving through one would have the other asking whether the
   file changed underneath it. */
int buffer_create_view(int src)
{
  buffer_t *b, *sb;
  int bi, root;

  if (src < 0 || src >= g_nbuf) return -1;
  sb = &g_buffers[src];
  if (!sb->ed) return -1;
  /* A view is a second *editor* onto one document; the hex view holds the one
     cursor into its bytes and cannot be in two panes at once. Splitting a hex
     buffer therefore gives the other pane something else (see pane_split). */
  if (sb->hex) return -1;

  /* Views are cheap to keep and dear to make: one left over from an earlier
     split of the same document is reused rather than a second one allocated,
     which is what stops C-x 2 / C-x 1 from filling the buffer list with
     identical spares. */
  root = sb->is_view && sb->view_of >= 0 ? sb->view_of : src;
  for (bi = 0; bi < g_nbuf; bi++) {
    b = &g_buffers[bi];
    if (b->is_view && b->pane < 0 && b->view_of == root && bi != src) {
      gtcaca_editor_goto_pos(b->ed, gtcaca_editor_get_current_pos(sb->ed));
      return bi;
    }
  }

  if (g_nbuf >= MAXBUF) return -1;
  bi = g_nbuf++;
  b = &g_buffers[bi];
  memset(b, 0, sizeof *b);
  b->pane = -1;
  b->is_view = 1;
  b->view_of = root;
  b->ed = gtcaca_editor_new_view(sb->ed, NULL, 0, 0, 1, 1);
  if (!b->ed) { g_nbuf--; return -1; }
  gtcaca_editor_key_cb_register(b->ed, on_key, NULL);
  gtcaca_editor_set_update_cb(b->ed, refresh_modeline, NULL);
  gtcaca_widget_hide(GTCACA_WIDGET(b->ed));

  strncpy(b->path, sb->path, sizeof b->path - 1);
  b->has_file = sb->has_file;
  b->langcfg = sb->langcfg; b->grammar = sb->grammar;
  b->keywords = sb->keywords; b->n_keywords = sb->n_keywords;
  b->folding = sb->folding;
  strncpy(b->langname, sb->langname, sizeof b->langname - 1);
  b->stamp = sb->stamp;
  return bi;
}

/* The buffers sharing one document are one file shown several times, so when
   the focused one takes on a new file and a new language — C-x C-w is the only
   way that happens — the others have to follow, or they would go on claiming a
   path and a language config that is no longer anyone's.

   The language belongs to the root of the family; the views hold the same
   pointer but do not own it, which is why the cleanup at exit frees a view's
   language config no more than once (see main()). */
void buffer_family_sync(int bi)
{
  buffer_t *src;
  int root, i;

  if (bi < 0 || bi >= g_nbuf) return;
  src = &g_buffers[bi];
  root = src->is_view ? src->view_of : bi;
  for (i = 0; i < g_nbuf; i++) {
    buffer_t *b = &g_buffers[i];
    if (i == bi || !(i == root || (b->is_view && b->view_of == root))) continue;
    strncpy(b->path, src->path, sizeof b->path - 1); b->path[sizeof b->path - 1] = '\0';
    b->has_file = src->has_file;
    b->stamp    = src->stamp;
    b->langcfg  = src->langcfg;
    b->grammar  = src->grammar;
    b->keywords = src->keywords;
    b->n_keywords = src->n_keywords;
    b->folding  = src->folding;
    strncpy(b->langname, src->langname, sizeof b->langname - 1);
    b->langname[sizeof b->langname - 1] = '\0';
    gtcaca_editor_set_langcfg(b->ed, b->langcfg);
    gtcaca_editor_set_grammar(b->ed, b->grammar);
  }
}

/* Position a pane's buffer editor to fill its slot window. */
/* ── split-tree node pool ──────────────────────────────────────────────────── */

int node_alloc(void)
{
  int i;
  for (i = 0; i < MAXNODES; i++) if (!g_nodes[i].used) {
    memset(&g_nodes[i], 0, sizeof g_nodes[i]);
    g_nodes[i].used = 1; g_nodes[i].parent = -1; g_nodes[i].buf = -1; g_nodes[i].win = -1;
    g_nodes[i].child[0] = g_nodes[i].child[1] = -1;
    return i;
  }
  return -1;
}
int win_alloc(void) { int i; for (i = 0; i < MAXLEAVES; i++) if (!g_win_used[i]) { g_win_used[i] = 1; return i; } return -1; }

/* Collect leaf node indices, left-to-right (in the order they tile). */
void collect_leaves(int n, int *out, int *cnt)
{
  if (n < 0 || !g_nodes[n].used) return;
  if (g_nodes[n].is_leaf) { if (*cnt < MAXLEAVES) out[(*cnt)++] = n; return; }
  collect_leaves(g_nodes[n].child[0], out, cnt);
  collect_leaves(g_nodes[n].child[1], out, cnt);
}
int leftmost_leaf(int n) { while (n >= 0 && !g_nodes[n].is_leaf) n = g_nodes[n].child[0]; return n; }

/* What a buffer shows in its pane. A hex buffer shows two widgets, not one:
   the view the toolkit draws and the input layer that takes the keys and the
   mouse over it (see hex.c), so showing and hiding a buffer means both. */
void buffer_pane_hide(int bi)
{
  if (bi < 0 || bi >= g_nbuf) return;
  if (g_buffers[bi].hex) {
    gtcaca_widget_hide(ccm_hex_input(bi));
    gtcaca_widget_hide(ccm_hex_view(bi));
  }
  if (g_buffers[bi].ed) gtcaca_widget_hide(GTCACA_WIDGET(g_buffers[bi].ed));
}

void position_leaf(int n)
{
  node_t *nd = &g_nodes[n];
  gtcaca_window_widget_t *win = g_winpool[nd->win];
  gtcaca_box_t *bx;
  win->x = nd->x; win->y = nd->y; win->width = nd->w; win->height = nd->h;
  gtcaca_widget_show(GTCACA_WIDGET(win));
  if (nd->buf < 0) return;
  if (g_buffers[nd->buf].hex) {
    buffer_t *b = &g_buffers[nd->buf];
    gtcaca_widget_t *view = ccm_hex_view(nd->buf), *in = ccm_hex_input(nd->buf);
    b->pane = n;
    view->parent = GTCACA_WIDGET(win);
    in->parent   = GTCACA_WIDGET(win);
    win->focused_child = in;                    /* the view is not focusable */
    gtcaca_widget_hide(GTCACA_WIDGET(b->ed));   /* its editor is never shown */
    gtcaca_widget_show(view);
    gtcaca_widget_show(in);
    ccm_hex_place(nd->buf, nd->x, nd->y, nd->w, nd->h);
    win->window_title = (b->has_file || b->path[0]) ? b->path : "*scratch*";
    return;
  }
  {
    buffer_t *b = &g_buffers[nd->buf];
    b->ed->parent = GTCACA_WIDGET(win); b->pane = n;
    win->focused_child = GTCACA_WIDGET(b->ed);   /* never leave it stale */
    gtcaca_widget_show(GTCACA_WIDGET(b->ed));
    bx = gtcaca_vbox_new(); gtcaca_box_set_margin(bx, 0);
    gtcaca_box_add_expand(bx, GTCACA_WIDGET(b->ed));
    gtcaca_box_apply(bx, nd->x, nd->y, nd->w, nd->h);
    gtcaca_box_free(bx);
    /* A buffer with no file still gets a name when it has one (*Completions*). */
    win->window_title = (b->has_file || b->path[0]) ? b->path : "*scratch*";
  }
}

void layout_node(int n, int x, int y, int w, int h)
{
  node_t *nd = &g_nodes[n];
  nd->x = x; nd->y = y; nd->w = w; nd->h = h;
  if (nd->is_leaf) { position_leaf(n); return; }
  if (nd->orient) { int h0 = h / 2; layout_node(nd->child[0], x, y, w, h0); layout_node(nd->child[1], x, y + h0, w, h - h0); }
  else            { int w0 = w / 2; layout_node(nd->child[0], x, y, w0, h); layout_node(nd->child[1], x + w0, y, w - w0, h); }
}

void relayout(void)
{
  int cw = caca_get_canvas_width(gmo.cv), ch = caca_get_canvas_height(gmo.cv), i;
  if (g_root >= 0) layout_node(g_root, 0, 0, cw, ch - 1 - g_bottom_reserve);
  for (i = 0; i < MAXLEAVES; i++) if (!g_win_used[i] && g_winpool[i]) gtcaca_widget_hide(GTCACA_WIDGET(g_winpool[i]));
}

/* Show buffer bi in leaf node n. */
/* Show buffer `bi` in pane `n`.

   A buffer owns one editor widget, so it can only be in one pane at a time.
   When it is already in another pane the two panes trade buffers rather than
   the old one being emptied: asking for a file that is already on screen used
   to leave the pane it came from blank (and focusing a blank pane read
   g_buffers[-1]). Trading keeps every pane showing something. */
void pane_show_buffer(int n, int bi)
{
  int prev = g_nodes[n].buf;              /* what this pane is showing now */
  int from = g_buffers[bi].pane;          /* where bi is showing, if anywhere */

  if (from >= 0 && from != n && prev >= 0) {
    g_nodes[from].buf = prev; g_buffers[prev].pane = from;
    g_nodes[n].buf = bi;      g_buffers[bi].pane = n;
    position_leaf(from);
    position_leaf(n);
    return;
  }
  if (from >= 0 && from != n) g_nodes[from].buf = -1;
  if (prev >= 0 && prev != bi) {
    buffer_pane_hide(prev);
    g_buffers[prev].pane = -1;
  }
  g_nodes[n].buf = bi; g_buffers[bi].pane = n;
  position_leaf(n);
}

void focus_pane(int n)   /* focus a leaf node */
{
  int bi, i;
  if (n < 0 || !g_nodes[n].used || !g_nodes[n].is_leaf) return;
  buffer_store_globals(g_cur_buf);
  bi = g_nodes[n].buf;
  if (bi < 0) {                  /* an empty pane: give it a scratch buffer rather
                                    than reading g_buffers[-1] below */
    bi = buffer_create(NULL);
    if (bi < 0) return;
    pane_show_buffer(n, bi);
  }
  g_focus_leaf = n; g_cur_buf = bi;
  buffer_load_globals(bi);
  g_win = g_winpool[g_nodes[n].win]; g_ed = g_buffers[bi].ed;
  apply_indent_settings(g_buffers[bi].has_file ? strrchr(g_buffers[bi].path, '.') : NULL);

  /* Focus exactly one window + editor. We manage this directly rather than via
     gtcaca_window_set_focus: window->focused_child pointers go stale when buffer
     editors are reparented across the pool, which can leave two editors focused
     (so a key reaches both — e.g. C-x gets set then consumed). */
  for (i = 0; i < g_nbuf; i++) {
    g_buffers[i].ed->has_focus = 0;
    /* A hex buffer's input layer is a focused widget like any other, and the
       toolkit delivers a key to *every* focused widget — so one left focused
       in a pane nobody is looking at would go on eating keystrokes. */
    if (g_buffers[i].hex) {
      ccm_hex_input(i)->has_focus = 0;
      ccm_hex_view(i)->has_focus  = 0;
    }
  }
  for (i = 0; i < MAXLEAVES; i++) if (g_winpool[i]) g_winpool[i]->has_focus = 0;
  if (g_browser_win) g_browser_win->has_focus = 0;
  if (g_browser_ed)  g_browser_ed->has_focus = 0;
  g_win->has_focus = 1;
  if (g_buffers[bi].hex) {
    /* The input layer takes the keys; the view is told it has the focus too,
       which is what makes it draw the byte cursor rather than a flat dump. */
    g_win->focused_child = ccm_hex_input(bi);
    ccm_hex_input(bi)->has_focus = 1;
    ccm_hex_view(bi)->has_focus  = 1;
  } else {
    g_win->focused_child = GTCACA_WIDGET(g_ed);
    g_ed->has_focus = 1;
  }
  refresh_modeline(g_ed, NULL);
}

void pane_hide_all(void)
{
  int leaves[MAXLEAVES], cnt = 0, i;
  collect_leaves(g_root, leaves, &cnt);
  for (i = 0; i < cnt; i++) {
    node_t *nd = &g_nodes[leaves[i]];
    gtcaca_widget_hide(GTCACA_WIDGET(g_winpool[nd->win]));
    if (nd->buf >= 0) buffer_pane_hide(nd->buf);
  }
}
void pane_show_all(void) { relayout(); focus_pane(g_focus_leaf); }

/* Open `path` into the focused window (creating/reusing a buffer). */
void open_path_in_editor(const char *path)
{
  int bi;

  /* An archive is somewhere to go, not something to read: C-x C-f on a .zip or
     a .tar.gz opens the browser inside it. Checked here rather than in
     buffer_create because the answer is a listing, not a buffer.
     Only one that is actually there: `C-x C-f notes.zip` on a name that does
     not exist yet means a new file, not a listing of nothing. */
  { struct stat ast;
    if (stat(path, &ast) == 0 && S_ISREG(ast.st_mode) && ccm_is_archive(path)) {
      if (!browser_is_open()) show_browser();
      if (browse_archive(path) == 0) return;
      /* Not readable as an archive after all — fall through to the file. */
    } }

  bi = buffer_create(path);
  if (bi < 0) { snprintf(g_message, sizeof g_message, "Too many buffers"); return; }
  if (g_browser_open) {
    gtcaca_widget_hide(GTCACA_WIDGET(g_browser_ed));
    gtcaca_widget_hide(GTCACA_WIDGET(g_browser_win));
    g_browser_open = 0;
  }
  pane_show_buffer(g_focus_leaf, bi);
  relayout();
  focus_pane(g_focus_leaf);
  /* Encrypted and not open yet: ask for the passphrase now. C-x C-f and the
     browser land here, so this is the one place the question has to be raised
     for every route into a file that is not the command line. */
  if (g_buffers[bi].locked) { ccm_crypt_begin(bi, NULL); return; }
  /* Already had a buffer for it, and the file has moved on since? Emacs offers
     to reread it rather than showing you a stale copy without a word. */
  ccm_maybe_reread(bi);
}

/* Split the focused leaf into two; the original stays as the first child. */
/* Split, showing `newbuf` in the new window — or, with newbuf < 0, whichever
   buffer is not on screen yet (what C-x 2 / C-x 3 do). Focus stays where it
   was, as in Emacs. */
void pane_split_with(int newbuf, int rows)
{
  int L = g_focus_leaf, c0, c1, wi, i;
  if (L < 0 || !g_nodes[L].is_leaf) return;

  if (newbuf < 0)
    for (i = 0; i < g_nbuf; i++) if (g_buffers[i].pane < 0 && i != g_cur_buf) { newbuf = i; break; }
  if (newbuf < 0) { newbuf = buffer_create(NULL); if (newbuf < 0) return; }

  c0 = node_alloc(); c1 = node_alloc(); wi = win_alloc();
  if (c0 < 0 || c1 < 0 || wi < 0) {
    if (c0 >= 0) g_nodes[c0].used = 0; if (c1 >= 0) g_nodes[c1].used = 0; if (wi >= 0) g_win_used[wi] = 0;
    snprintf(g_message, sizeof g_message, "Too many windows"); return;
  }
  /* c0 inherits the original window + buffer */
  g_nodes[c0].is_leaf = 1; g_nodes[c0].parent = L;
  g_nodes[c0].buf = g_nodes[L].buf; g_nodes[c0].win = g_nodes[L].win;
  if (g_nodes[c0].buf >= 0) g_buffers[g_nodes[c0].buf].pane = c0;
  /* c1 is the new window with another buffer */
  g_nodes[c1].is_leaf = 1; g_nodes[c1].parent = L; g_nodes[c1].buf = newbuf; g_nodes[c1].win = wi;
  g_buffers[newbuf].pane = c1;
  /* L becomes the split */
  g_nodes[L].is_leaf = 0; g_nodes[L].orient = rows; g_nodes[L].child[0] = c0; g_nodes[L].child[1] = c1;
  g_nodes[L].buf = -1; g_nodes[L].win = -1;

  g_focus_leaf = c0;
  relayout();
  focus_pane(c0);
}

/* C-x 2 / C-x 3. Emacs shows the same buffer in the new window, at the place
   you were looking; here that is a second view of the current buffer. Focus
   stays where it was. Should the view not be had, fall back to whatever buffer
   is not on screen — the old behaviour. */
void pane_split(int rows)
{
  int v = buffer_create_view(g_cur_buf);
  pane_split_with(v, rows);
}

void pane_unsplit_one(void)
{
  int leaves[MAXLEAVES], cnt = 0, i, L = g_focus_leaf, buf, wi;
  if (L < 0) return;
  buffer_store_globals(g_cur_buf);
  buf = g_nodes[L].buf; wi = g_nodes[L].win;
  collect_leaves(g_root, leaves, &cnt);
  if (cnt <= 1) return;
  for (i = 0; i < cnt; i++) if (leaves[i] != L) {
    node_t *nd = &g_nodes[leaves[i]];
    if (nd->buf >= 0) { buffer_pane_hide(nd->buf); g_buffers[nd->buf].pane = -1; }
    if (nd->win >= 0) { g_win_used[nd->win] = 0; gtcaca_widget_hide(GTCACA_WIDGET(g_winpool[nd->win])); }
  }
  memset(g_nodes, 0, sizeof g_nodes);
  for (i = 0; i < MAXLEAVES; i++) g_win_used[i] = 0;
  g_win_used[wi] = 1;
  g_root = 0;
  g_nodes[0].used = 1; g_nodes[0].is_leaf = 1; g_nodes[0].parent = -1;
  g_nodes[0].buf = buf; g_nodes[0].win = wi; g_nodes[0].child[0] = g_nodes[0].child[1] = -1;
  if (buf >= 0) g_buffers[buf].pane = 0;
  g_focus_leaf = 0;
  relayout();
  focus_pane(0);
}

void pane_delete_current(void)
{
  int L = g_focus_leaf, P, sib, GP, f;
  node_t s;
  if (L < 0) return;
  P = g_nodes[L].parent;
  if (P < 0) { snprintf(g_message, sizeof g_message, "Only one window"); return; }
  sib = (g_nodes[P].child[0] == L) ? g_nodes[P].child[1] : g_nodes[P].child[0];
  buffer_store_globals(g_cur_buf);
  if (g_nodes[L].buf >= 0) { buffer_pane_hide(g_nodes[L].buf); g_buffers[g_nodes[L].buf].pane = -1; }
  if (g_nodes[L].win >= 0) { g_win_used[g_nodes[L].win] = 0; gtcaca_widget_hide(GTCACA_WIDGET(g_winpool[g_nodes[L].win])); }
  g_nodes[L].used = 0;
  /* collapse: the sibling replaces the parent in place */
  GP = g_nodes[P].parent;
  s = g_nodes[sib];
  g_nodes[P] = s; g_nodes[P].used = 1; g_nodes[P].parent = GP;
  if (g_nodes[P].is_leaf) { if (g_nodes[P].buf >= 0) g_buffers[g_nodes[P].buf].pane = P; }
  else { g_nodes[g_nodes[P].child[0]].parent = P; g_nodes[g_nodes[P].child[1]].parent = P; }
  g_nodes[sib].used = 0;
  f = leftmost_leaf(P);
  g_focus_leaf = f;
  relayout();
  focus_pane(f);
}

void pane_other_window(void)
{
  int leaves[MAXLEAVES], cnt = 0, i, idx = 0;
  collect_leaves(g_root, leaves, &cnt);
  if (cnt <= 1) { snprintf(g_message, sizeof g_message, "Only one window"); return; }
  for (i = 0; i < cnt; i++) if (leaves[i] == g_focus_leaf) { idx = i; break; }
  focus_pane(leaves[(idx + 1) % cnt]);
}

void pane_switch_buffer(void)
{
  int k, bi = -1;
  for (k = 1; k <= g_nbuf; k++) {
    int c = (g_cur_buf + k) % g_nbuf;
    /* a spare view is the document you are already looking at, not another
       buffer to switch to */
    if (c != g_cur_buf && g_buffers[c].pane < 0 && !g_buffers[c].is_view) { bi = c; break; }
  }
  if (bi < 0) { snprintf(g_message, sizeof g_message, "No other buffer"); return; }
  pane_show_buffer(g_focus_leaf, bi);
  focus_pane(g_focus_leaf);
  snprintf(g_message, sizeof g_message, "%s",
           (g_buffers[bi].has_file || g_buffers[bi].path[0]) ? g_buffers[bi].path : "*scratch*");
}

/* ── save-as (C-x C-w, and C-x C-s on a scratch buffer) ────────────────────── */

void save_as_done(const char *input)
{
  char path[PATH_MAX];
  FILE *f;
  int len;
  char *buf;
  buffer_t *b;

  if (!input[0]) return;
  if (input[0] == '~') expand_tilde(input, path, sizeof path);
  else if (input[0] == '/') snprintf(path, sizeof path, "%s", input);
  else if (g_curdir[0]) snprintf(path, sizeof path, "%s/%s", g_curdir, input);
  else snprintf(path, sizeof path, "%s", input);
  /* Canonicalise before it becomes the buffer's path: buffers carry absolute
     paths (see buffer_create), and the Find-file prompt, the modeline and the
     file log all read them back. */
  { char abs[PATH_MAX]; ccm_abs_path(path, abs, sizeof abs); snprintf(path, sizeof path, "%s", abs); }

  /* A hex buffer holds bytes, so there is nothing here to compress or to take
     from the editor: write the bytes, and let the buffer visit the new name.
     This is also the way out of a read-only hex buffer — an archive member you
     want to change — so ccm_hex_write_as writes regardless and clears it. */
  if (g_cur_buf >= 0 && g_buffers[g_cur_buf].hex) {
    b = &g_buffers[g_cur_buf];
    if (ccm_hex_write_as(g_cur_buf, path) != 0) return;
    b->arc_member = 0; b->comp = CCM_COMP_NONE; b->comp_ro = 0; b->comp_note[0] = '\0';
    strncpy(b->path, path, sizeof b->path - 1); b->path[sizeof b->path - 1] = '\0';
    b->has_file = 1;
    g_filename = b->path;
    ccm_stamp_buffer(g_cur_buf);
    ccm_log_file('W', b->path);
    snprintf(g_message, sizeof g_message, "Wrote %s", path);
    return;
  }

  len = gtcaca_editor_get_length(g_ed);
  buf = malloc((size_t)len + 1);
  if (!buf) return;
  gtcaca_editor_get_text(g_ed, buf, len + 1);

  /* The new name decides how it is stored: writing a buffer out as notes.txt.gz
     compresses it, and the buffer keeps compressing on every save after. */
  { int fmt = ccm_comp_format(path);
    if (fmt != CCM_COMP_NONE && ccm_comp_writable(fmt)) {
      char err[256] = "";
      if (ccm_comp_write_file(path, fmt, buf, (size_t)len, err, sizeof err) != 0) {
        snprintf(g_message, sizeof g_message, "Not saved: %s", err[0] ? err : "compression failed");
        free(buf); return;
      }
      g_buffers[g_cur_buf].comp = fmt;
      g_buffers[g_cur_buf].comp_ro = 0;
    } else if (fmt != CCM_COMP_NONE) {
      snprintf(g_message, sizeof g_message, "Not saved: ccm cannot write %s files", ccm_comp_name(fmt));
      free(buf); return;
    } else {
      f = fopen(path, "w");
      if (!f) { snprintf(g_message, sizeof g_message, "Cannot write %s", path); free(buf); return; }
      fwrite(buf, 1, (size_t)len, f);
      fclose(f);
      g_buffers[g_cur_buf].comp = CCM_COMP_NONE;
      g_buffers[g_cur_buf].comp_ro = 0;
    } }
  free(buf);

  /* Writing it out under a real name is how a member (or a format ccm can only
     read) stops being one — C-x C-w is the way to get an archive member or a
     .xz out into a file you can edit, so the buffer must not stay read-only. */
  if (g_buffers[g_cur_buf].arc_member || g_buffers[g_cur_buf].comp_ro)
    gtcaca_editor_set_read_only(g_ed, 0);
  g_buffers[g_cur_buf].arc_member = 0;
  g_buffers[g_cur_buf].comp_ro = 0;

  /* attach the file to the focused buffer and re-detect its language */
  b = &g_buffers[g_cur_buf];
  strncpy(b->path, path, sizeof b->path - 1); b->path[sizeof b->path - 1] = '\0';
  b->has_file = 1;
  g_filename = b->path;
  if (g_langcfg) { gtcaca_editor_set_langcfg(g_ed, NULL); gtcaca_editor_langcfg_free(g_langcfg); g_langcfg = NULL; }
  if (g_grammar) { gtcaca_editor_set_grammar(g_ed, NULL); gtcaca_editor_grammar_free(g_grammar); g_grammar = NULL; }
  g_keywords = NULL; g_n_keywords = 0; strcpy(g_langname, "fundamental");
  gtcaca_editor_set_json_mode(g_ed, 0);
  setup_language(g_ed, g_filename, NULL);
  buffer_store_globals(g_cur_buf);

  gtcaca_editor_set_save_point(g_ed);
  ccm_stamp_buffer(g_cur_buf);  /* the buffer now visits this file, as of now */
  buffer_family_sync(g_cur_buf);   /* other windows on this document, too */
  ccm_log_file('W', b->path);   /* b->path, not `path`: the canonical absolute form */
  snprintf(g_message, sizeof g_message, "Wrote %s", path);
}

/* Bring a widget (and so its window) to the front of the draw order. */
void widget_to_front(gtcaca_widget_t *w)
{
  CDL_DELETE(gmo.widgets_list, w);
  CDL_APPEND(gmo.widgets_list, w);
}

void hide_save_dialog(void)
{
  gtcaca_widget_hide(GTCACA_WIDGET(g_save_win));
  gtcaca_widget_hide(GTCACA_WIDGET(g_save_label));
  gtcaca_widget_hide(GTCACA_WIDGET(g_save_entry));
  gtcaca_widget_hide(GTCACA_WIDGET(g_save_ok));
  gtcaca_widget_hide(GTCACA_WIDGET(g_save_cancel));
  g_save_open = 0;
  focus_pane(g_focus_leaf);           /* return focus to the editor */
}

int dlg_ok(gtcaca_button_widget_t *btn, int key, void *ud)
{
  char tmp[PATH_MAX];
  const char *t;
  (void)btn; (void)ud;
  if (key != CACA_KEY_RETURN && key != ' ') return 0;
  t = gtcaca_entry_get_text(g_save_entry);
  strncpy(tmp, t ? t : "", sizeof tmp - 1); tmp[sizeof tmp - 1] = '\0';
  save_as_done(tmp);     /* sets g_message = "Wrote <path>" before we redraw */
  hide_save_dialog();    /* focus_pane → refresh_modeline shows that message */
  return 0;
}
int dlg_cancel(gtcaca_button_widget_t *btn, int key, void *ud)
{
  (void)btn; (void)ud;
  if (key != CACA_KEY_RETURN && key != ' ') return 0;
  hide_save_dialog();
  snprintf(g_message, sizeof g_message, "Cancelled");
  return 0;
}
/* C-g inside the entry cancels the dialog too. */
int dlg_entry_key(gtcaca_entry_widget_t *e, int key, void *ud)
{
  (void)e; (void)ud;
  if (key == CACA_KEY_CTRL_G) { hide_save_dialog(); snprintf(g_message, sizeof g_message, "Cancelled"); }
  return 0;
}

void start_save_as(void)
{
  char defdir[PATH_MAX];
  /* An archive member's "path" is <archive>!<member>, which is no use as a
     name to save under — offering it would make C-x C-w (the way to get a
     member out into a file you can edit) start by having to clear the line.
     The member's own base name next to the archive is what was meant. */
  if (g_cur_buf >= 0 && g_buffers[g_cur_buf].arc_member) {
    const char *bang = strrchr(g_buffers[g_cur_buf].path, '!');
    snprintf(defdir, sizeof defdir, "%s/%s", g_curdir,
             ccm_base_name(bang ? bang + 1 : g_buffers[g_cur_buf].path));
  } else if (g_cur_buf >= 0 && g_buffers[g_cur_buf].has_file) {
    strncpy(defdir, g_buffers[g_cur_buf].path, sizeof defdir - 1); defdir[sizeof defdir - 1] = '\0';
  } else if (g_curdir[0]) {
    snprintf(defdir, sizeof defdir, "%s/", g_curdir);
  } else {
    if (getcwd(defdir, sizeof defdir - 1)) strncat(defdir, "/", sizeof defdir - strlen(defdir) - 1);
    else defdir[0] = '\0';
  }
  gtcaca_entry_set_text(g_save_entry, defdir);
  widget_to_front(GTCACA_WIDGET(g_save_win));
  widget_to_front(GTCACA_WIDGET(g_save_label));
  widget_to_front(GTCACA_WIDGET(g_save_entry));
  widget_to_front(GTCACA_WIDGET(g_save_ok));
  widget_to_front(GTCACA_WIDGET(g_save_cancel));
  gtcaca_widget_show(GTCACA_WIDGET(g_save_win));
  gtcaca_widget_show(GTCACA_WIDGET(g_save_label));
  gtcaca_widget_show(GTCACA_WIDGET(g_save_entry));
  gtcaca_widget_show(GTCACA_WIDGET(g_save_ok));
  gtcaca_widget_show(GTCACA_WIDGET(g_save_cancel));
  gtcaca_window_set_focus(g_save_win);
  gtcaca_window_set_focused_child(g_save_win, GTCACA_WIDGET(g_save_entry));
  g_save_open = 1;
  snprintf(g_message, sizeof g_message, "Write file — Enter/OK to save, Cancel or C-g to abort");
}


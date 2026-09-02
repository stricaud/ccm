/*
 * cacamacs.c — a tiny text editor built on the gtcaca `editor` widget, driven
 *           with Emacs key bindings.
 *
 * Usage:
 *   cacamacs [options] [file | directory] [+N]      (see --help, or usage())
 *   (+N opens the file at line N, e.g.  ccm file.c +123)
 *
 * Given a directory (or C-x d), a file browser opens: Up/Down to move, Enter to
 * descend into a folder ("../" goes up) or open a file in the editor.
 *
 * The editor widget provides the document model, caret/selection, undo and
 * the default key bindings (arrows, Home/End, PageUp/Down, typing, Backspace,
 * Enter). This app layers an Emacs keymap on top through the widget's key
 * callback: keys it handles are consumed, everything else falls through to the
 * widget's own editing keys.
 *
 * Bindings:
 *   Motion : C-f C-b C-n C-p  C-a C-e  C-v (page down)  arrows  M-f/M-b word
 *   Edit   : C-d (delete fwd)  C-k (kill line)  Backspace  C-t transpose chars
 *            M-d / M-DEL kill word  M-u/M-l upcase/downcase word  Insert overtype
 *   Lines  : C-x C-t transpose lines   M-; comment/uncomment line or region
 *   Modes  : C-x C-q read-only   C-x w show whitespace   (matching braces glow)
 *   Region : C-Space set mark  C-w kill  M-w (Esc w) copy  C-y yank  C-g cancel
 *   Search : C-s incremental search forward, C-r backward (Enter accepts,
 *            C-g cancels); M-% (Esc %) or C-x r replace-all
 *   Meta   : Esc acts as the Meta prefix (Esc then a key = M-key)
 *   Undo   : C-/  (also  C-x u)
 *   Files  : C-x C-s save      C-x C-c quit       C-x C-x exchange point/mark
 *   Complete: Tab — complete the word before the caret (else insert a soft tab).
 *             In the list: Up/Down choose, Enter/Tab accept, Esc cancel.
 *   View   : C-x l line numbers   C-x f folding mode   C-x t toggle fold
 *            C-x a toggle annotation on the current line
 *   Files  : C-x d open the file browser (navigate the current directory)
 *   JSON   : .json/.jsonl open in JSON mode (colour + fold).
 *   Pretty : C-x p pretty-prints JSON or XML (auto-detected) — the selection,
 *            else the whole buffer if it is one document, else the current line.
 *            M-x json-pretty-print / xml-pretty-print force a format on the line.
 */

#include "cacamacs.h"
#include <getopt.h>   /* getopt_long — POSIX systems and mingw-w64 alike */
#include <errno.h>    /* strerror, for --configure's failure messages */

/* global definitions (declared extern in cacamacs.h) */
char g_message[320] = "";   /* status-line message; shared with games.c.
                               Wide enough for a completion candidate list — the
                               point of the shared md- prefix is that `M-x md-`
                               Tab shows the whole set. */
gtcaca_editor_widget_t    *g_ed       = NULL;   /* focused editor       */
gtcaca_statusbar_widget_t *g_modeline = NULL;
gtcaca_window_widget_t    *g_win      = NULL;   /* focused pane window  */
const char                *g_filename = NULL;
char                       g_open_path[PATH_MAX] = "";
buffer_t g_buffers[MAXBUF];
int      g_nbuf = 0;
int      g_cur_buf = -1;           /* focused buffer index */
node_t g_nodes[MAXNODES];
int    g_root = -1, g_focus_leaf = -1;
gtcaca_window_widget_t *g_winpool[MAXLEAVES];
int    g_win_used[MAXLEAVES];
gtcaca_window_widget_t    *g_browser_win = NULL;
gtcaca_editor_widget_t    *g_browser_ed  = NULL;
char                       g_curdir[PATH_MAX] = "";
gtcaca_window_widget_t    *g_help_win = NULL;
gtcaca_editor_widget_t    *g_help_ed  = NULL;
int                        g_help_open = 0;
gtcaca_window_widget_t    *g_save_win    = NULL;
gtcaca_label_widget_t     *g_save_label  = NULL;
gtcaca_entry_widget_t     *g_save_entry  = NULL;
gtcaca_button_widget_t    *g_save_ok     = NULL;
gtcaca_button_widget_t    *g_save_cancel = NULL;
int                        g_save_open   = 0;
gtcaca_editor_langcfg_t   *g_langcfg  = NULL;
gtcaca_editor_grammar_t   *g_grammar  = NULL;
char                       g_langname[64] = "fundamental";
const char               **g_keywords = NULL;   /* language keywords, for completion */
int                        g_n_keywords = 0;
int                        g_folding = 0;         /* folding mode on? */
int             g_cfg_tab = 8, g_cfg_spaces = 1, g_cfg_indent = 2;   /* global defaults */
int             g_cfg_edge = 0;                                       /* edge/ruler column */
int             g_cfg_logfiles = 0;   /* off unless config.json says otherwise */
int             g_cfg_dgm_mermaid = 0;   /* diagram mode writes ASCII unless told otherwise */
lang_override_t g_overrides[32];
int             g_noverrides = 0;
int             g_tab_size = 8, g_insert_spaces = 1, g_indent_size = 2; /* effective */
int   g_mark_active   = 0;
int   g_ctrl_x        = 0;   /* C-x prefix pending */
int   g_rect_prefix   = 0;   /* C-x r rectangle sub-prefix pending */
int   g_goto_prefix   = 0;   /* M-g … (goto) prefix pending */
int   g_meta          = 0;   /* Meta (Esc) prefix pending */
char *g_killbuf       = NULL;
int   g_macro[MACRO_MAX];
int   g_macro_len       = 0;
int   g_macro_recording = 0;
int   g_macro_executing = 0;
int   g_prefix_reading  = 0; /* C-u seen, accumulating an optional count */
int   g_prefix_pending  = 0; /* a count is armed for the next command    */
int   g_prefix_have     = 0; /* digits were actually typed (else default) */
long  g_prefix_arg      = 0;

int g_bottom_reserve = 0;   /* rows kept clear above the status bar */

/* `ccm --test`: a terminal diagnostic. Numbers every row and marks the last two
   rows in colour, so we can see which rows your terminal actually renders (and
   prints the canvas vs terminal size). Screenshot it; key to quit. */
static void run_test(void)
{
  int W = caca_get_canvas_width(gmo.cv), H = caca_get_canvas_height(gmo.cv), y, x;
  const char *drv = caca_get_display_driver(gmo.dp);
  int have = 0, tw = 0, th = 0;   /* terminal size from the OS, if available */
  caca_event_t ev;
#ifndef _WIN32
  { struct winsize ws;            /* TIOCGWINSZ is POSIX-only */
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) { have = 1; tw = ws.ws_col; th = ws.ws_row; } }
#endif

  caca_set_color_ansi(gmo.cv, CACA_LIGHTGRAY, CACA_BLACK); caca_clear_canvas(gmo.cv);
  for (y = 0; y < H; y++) {
    caca_set_color_ansi(gmo.cv, (y & 1) ? CACA_LIGHTGRAY : CACA_WHITE, CACA_BLACK);
    caca_printf(gmo.cv, 0, y, "row %2d", y);
    if (W > 8) caca_printf(gmo.cv, W - 7, y, "row %2d", y);
  }
  caca_set_color_ansi(gmo.cv, CACA_WHITE, CACA_BLACK); caca_set_attr(gmo.cv, CACA_BOLD);
  caca_printf(gmo.cv, 8, 1, "ccm --test   libcaca canvas = %d cols x %d rows", W, H);
  if (have) caca_printf(gmo.cv, 8, 2, "terminal (ioctl)            = %d cols x %d rows", tw, th);
  caca_printf(gmo.cv, 8, 3, "libcaca driver              = %s", drv ? drv : "(unknown)");
  caca_printf(gmo.cv, 8, 5, "Do you see the CYAN bar on the very last row (row %d)?", H - 1);
  caca_printf(gmo.cv, 8, 6, "And the YELLOW bar just above it (row %d)?", H - 2);
  caca_printf(gmo.cv, 8, 7, "Screenshot this screen, then press any key to quit.");
  caca_set_attr(gmo.cv, 0);
  caca_set_color_ansi(gmo.cv, CACA_BLACK, CACA_YELLOW);
  for (x = 0; x < W; x++) caca_put_char(gmo.cv, x, H - 2, ' ');
  caca_printf(gmo.cv, 1, H - 2, " <<< SECOND-TO-LAST ROW (row %d) >>> ", H - 2);
  caca_set_color_ansi(gmo.cv, CACA_BLACK, CACA_CYAN);
  for (x = 0; x < W; x++) caca_put_char(gmo.cv, x, H - 1, ' ');
  caca_printf(gmo.cv, 1, H - 1, " <<< LAST ROW (row %d) — the status bar belongs here >>> ", H - 1);
  caca_refresh_display(gmo.dp);
  caca_get_event(gmo.dp, CACA_EVENT_KEY_PRESS, &ev, -1);

  /* also to stderr, so the numbers survive even if those rows don't render */
  fprintf(stderr, "ccm --test:\n  libcaca canvas : %d cols x %d rows\n", W, H);
  if (have) fprintf(stderr, "  terminal ioctl : %d cols x %d rows\n", tw, th);
  fprintf(stderr, "  libcaca driver : %s\n", drv ? drv : "(unknown)");
  fprintf(stderr, "  If the status bar is missing, run with CCM_BOTTOM_MARGIN=1 (or 2).\n");
}

#ifndef CCM_VERSION_STR
#define CCM_VERSION_STR "0.0.0"
#endif

/* ── startup diagnostics ──────────────────────────────────────────────────────
 *
 * gtcaca_init() greets every run with
 *
 *     No theme file found at <prefix>/share/gtcaca//themes/default, using
 *     built-in defaults.
 *
 * because gtcaca resolves its theme against the GTCACA_DATA_DIR that was baked
 * in when the *library* was compiled. For a relocatable build — the pip wheel,
 * the .pkg — that directory is the machine it was built on, so the line names a
 * CI runner's work tree and the file is never there. cacamacs does not use
 * gtcaca's ini themes at all (its own live in ~/.cacamacs/theme, see theme.c),
 * so the note is noise on a screen that is about to be cleared anyway.
 *
 * Mute fd 2 for the duration of init unless --warnings asked to see it. The
 * only other thing gtcaca writes there is its "cannot create display" error,
 * which always comes with a negative return, so restore the descriptor first
 * and report that failure ourselves — nothing diagnostic is lost.
 */
static int g_warnings = 0;

static int init_gtcaca(int *argc, char ***argv)
{
  int saved = -1, rc;

  if (!g_warnings) {
#ifdef _WIN32
    int null_fd = open("NUL", O_WRONLY);
#else
    int null_fd = open("/dev/null", O_WRONLY);
#endif
    if (null_fd >= 0) { saved = dup(2); dup2(null_fd, 2); close(null_fd); }
  }

  rc = gtcaca_init(argc, argv);

  if (saved >= 0) { dup2(saved, 2); close(saved); }
  if (rc < 0 && !g_warnings)
    fprintf(stderr, "ccm: cannot start the display — re-run with --warnings to see why\n");
  return rc;
}

/* --configure: drop a fully populated config.json into the configuration
   directory, so there is something to edit rather than a blank page and a
   README.

   Every value is read out of the live globals rather than restated as a literal
   here. Restating them is exactly how a "default configuration" file drifts
   away from the actual defaults: this runs before load_config(), so the globals
   still hold what cacamacs starts from.

   An existing file is never overwritten — a hand-tuned configuration is not
   something to lose to a mistyped flag. */
static int write_default_config(void)
{
  char path[PATH_MAX];
  const char *dir = ccm_config_dir();
  struct stat st;
  FILE *f;

  if (!dir[0]) {
    fprintf(stderr, "ccm: no home directory to write a configuration into\n");
    return 1;
  }
  if (stat(dir, &st) != 0 && ccm_mkdir(dir) != 0) {
    fprintf(stderr, "ccm: cannot create %s: %s\n", dir, strerror(errno));
    return 1;
  }
  ccm_config_path("config.json", path, sizeof path);
  if (stat(path, &st) == 0) {
    fprintf(stderr, "ccm: %s already exists — move it aside to write a fresh one\n", path);
    return 1;
  }
  f = fopen(path, "w");
  if (!f) {
    fprintf(stderr, "ccm: cannot write %s: %s\n", path, strerror(errno));
    return 1;
  }

  /* JSON has no comments, so the descriptions ride along as "//<key>" entries;
     load_config only looks up the keys it knows, and ignores these. */
  fprintf(f,
    "{\n"
    "  \"//\": \"cacamacs configuration — every setting at its built-in default\",\n"
    "\n"
    "  \"//tabSize\": \"display width of a tab\",\n"
    "  \"tabSize\": %d,\n"
    "\n"
    "  \"//insertSpaces\": \"Tab inserts spaces (true) or a real tab (false)\",\n"
    "  \"insertSpaces\": %s,\n"
    "\n"
    "  \"//indentSize\": \"spaces inserted per level when insertSpaces is true\",\n"
    "  \"indentSize\": %d,\n"
    "\n"
    "  \"//edgeColumn\": \"column to mark with the edge ruler; 0 turns it off\",\n"
    "  \"edgeColumn\": %d,\n"
    "\n"
    "  \"//logFiles\": \"append every file opened (O) and written (W) to files.log\",\n"
    "  \"logFiles\": %s,\n"
    "\n"
    "  \"//diagram-mermaid-default\": \"M-x diagram writes a Mermaid graph rather than\",\n"
    "  \"//diagram-mermaid-default2\": \"ASCII art; C-x q switches either way while drawing\",\n"
    "  \"diagram-mermaid-default\": %s,\n"
    "\n"
    "  \"//languages\": \"per-extension overrides of the three indent settings,\",\n"
    "  \"//languages ex\": \"e.g. \\\".c\\\": { \\\"insertSpaces\\\": false, \\\"tabSize\\\": 8 }\",\n"
    "  \"languages\": {}\n"
    "}\n",
    g_cfg_tab, g_cfg_spaces ? "true" : "false", g_cfg_indent,
    g_cfg_edge, g_cfg_logfiles ? "true" : "false",
    g_cfg_dgm_mermaid ? "true" : "false");

  if (ferror(f) || fclose(f) != 0) {
    fprintf(stderr, "ccm: failed writing %s\n", path);
    return 1;
  }
  printf("Wrote %s\n", path);
  return 0;
}

static void usage(FILE *out)
{
  const char *cfg = ccm_config_dir();
  fprintf(out,
    "Usage: ccm [options] [file | directory] [+LINE]\n"
    "\n"
    "A small terminal text editor with Emacs key bindings. Given a directory\n"
    "(or C-x d once running) it opens a file browser instead.\n"
    "\n"
    "  -h, --help       show this help and exit\n"
    "  -v, --version    show the version and exit\n"
    "  -w, --warnings   let library start-up warnings through to stderr\n"
    "      --configure  write a default config.json and exit (never overwrites)\n"
    "      --diag       report terminal/library facts, probe mouse and keys, exit\n"
    "      --test       run the built-in self-test and exit\n"
    "\n"
    "  +LINE            open the file at that line, e.g.  ccm file.c +123\n"
    "\n"
    "Per-user configuration lives in %s:\n"
    "  theme            colours — see docs/ccm-theme.example\n"
    "  config.json      tab size, indentation, per-language overrides;\n"
    "                   \"logFiles\": true also records every file opened and\n"
    "                   written to files.log in the same directory\n"
    "  extensions/      VSCode-style grammars driving syntax colourization\n"
    "\n"
    "Inside the editor, M-x help lists every key binding.\n",
    cfg[0] ? cfg : "~/.cacamacs");
}

int main(int argc, char **argv)
{
  static const struct option LONGOPTS[] = {
    { "help",     no_argument, NULL, 'h' },
    { "version",  no_argument, NULL, 'v' },
    { "warnings",  no_argument, NULL, 'w' },
    { "configure", no_argument, NULL, 'C' },  /* long-form only */
    { "diag",      no_argument, NULL, 'D' },  /* long-form only */
    { "test",      no_argument, NULL, 't' },  /* long-form only */
    { NULL,       0,           NULL,  0  }
  };
  int cw, ch, i, b0, c, selftest = 0;
  const char *arg = NULL;
  const char *open_file_path = NULL;
  int open_dir = 0;
  long goto_line_arg = 0;

  /* Options are settled before any display is opened, so --version and --help
     print and exit cleanly. --version is also the packaging smoke test's entry
     point: the OS resolves ccm's whole dylib/DLL closure (gtcaca, libcaca,
     oniguruma) at load, before main runs, so merely reaching it proves the
     libraries resolved — and it leaves no TUI behind to orphan a CI runner. */
  while ((c = getopt_long(argc, argv, "hvw", LONGOPTS, NULL)) != -1) {
    switch (c) {
    case 'h': usage(stdout); return 0;
    case 'v': printf("ccm (cacamacs) %s\n", CCM_VERSION_STR); return 0;
    case 'w': g_warnings = 1; break;
    case 'C': return write_default_config();
    case 'D': return ccm_run_diag(argv[0]);   /* before any display is opened */
    case 't': selftest = 1;   break;
    default:  usage(stderr);  return 2;
    }
  }

  /* What is left is a path plus an optional "+N" line number (vim/less style);
     the +N may come before or after the path. */
  for (i = optind; i < argc; i++) {
    if (argv[i][0] == '+' && argv[i][1] >= '0' && argv[i][1] <= '9')
      goto_line_arg = strtol(argv[i] + 1, NULL, 10);
    else if (!arg)
      arg = argv[i];
  }

  /* cacamacs ships its own default theme (themes/default, installed as
     <data dir>/themes/default). Registering the directory before init makes
     gtcaca layer it over gtcaca's own default, so ours only has to name the
     colours that differ — and the user's ~/.config/gtcaca theme still wins over
     both. Must come before gtcaca_init(), which is where themes are read. */
#ifdef GTCACA_HAVE_THEME_SEARCH
  gtcaca_theme_add_search_dir(ccm_data_dir());
  if (g_warnings) gtcaca_theme_set_verbose(1);
#endif

  if (init_gtcaca(&argc, &argv) < 0) return 1;
  if (selftest) { run_test(); caca_free_display(gmo.dp); return 0; }
  { const char *m = getenv("CCM_BOTTOM_MARGIN"); int mv = m ? atoi(m) : 0;
    if (mv > 0) g_bottom_reserve = mv; }
  /* Fallback for an install whose themes/default is missing (a hand-copied
     binary, a stripped package): keep the Emacs-like editing surface — near
     black with light-grey text, unchanged by focus — that the theme file
     otherwise supplies. Skipped when the file was there, so a user editing it
     is not silently overruled a moment later. */
  if (!ccm_default_theme_present()) {
    gmo.theme.textviewfocus.bg = CACA_BLACK;
    gmo.theme.textviewfocus.fg = CACA_LIGHTGRAY;
    gmo.theme.textview.bg      = CACA_BLACK;
    gmo.theme.textview.fg      = CACA_LIGHTGRAY;
  }
  gtcaca_application_new("cacamacs");
  load_config();
  ccm_theme_load();          /* ~/.cacamacs/theme overrides the defaults above */
  ccm_theme_apply_global();  /* tint the editor surface before editors exist */

  if (arg) {
    struct stat st;
    if (stat(arg, &st) == 0 && S_ISDIR(st.st_mode)) open_dir = 1;
    else open_file_path = arg;
  }

  cw = caca_get_canvas_width(gmo.cv);
  ch = caca_get_canvas_height(gmo.cv);

  /* pane window pool (hidden); a leaf uses one of these by index */
  for (i = 0; i < MAXLEAVES; i++) {
    g_winpool[i] = gtcaca_window_new(NULL, "", 0, 0, cw, ch - 1);
    gtcaca_widget_hide(GTCACA_WIDGET(g_winpool[i]));
    g_win_used[i] = 0;
  }

  /* browser pop-up (created after the panes; modal, so order is fine) */
  {
    int bw = cw * 4 / 5, bh = (ch - 1) * 4 / 5;
    if (bw < 24) bw = cw > 24 ? cw - 2 : cw;
    if (bh < 6)  bh = ch - 2;
    g_browser_win = gtcaca_window_new_centered(NULL, "Browser", bw, bh);
    g_browser_ed = gtcaca_editor_new(GTCACA_WIDGET(g_browser_win), 0, 0, 1, 1);
    gtcaca_editor_set_line_numbers(g_browser_ed, 0);
    gtcaca_editor_set_caret_line_visible(g_browser_ed, 1);
    gtcaca_editor_set_caret_line_back(g_browser_ed, CACA_BLUE);
    gtcaca_editor_set_read_only(g_browser_ed, 1);
    gtcaca_editor_key_cb_register(g_browser_ed, on_key, NULL);
    gtcaca_editor_set_update_cb(g_browser_ed, browser_modeline, NULL);
    { gtcaca_box_t *bb = gtcaca_vbox_new(); gtcaca_box_set_margin(bb, 0);
      gtcaca_box_add_expand(bb, GTCACA_WIDGET(g_browser_ed));
      gtcaca_box_apply(bb, g_browser_win->x, g_browser_win->y,
                       g_browser_win->width, g_browser_win->height);
      gtcaca_box_free(bb); }
    gtcaca_widget_hide(GTCACA_WIDGET(g_browser_ed));
    gtcaca_widget_hide(GTCACA_WIDGET(g_browser_win));
  }

  /* help viewer pop-up (M-x help) — a modal, scrollable, read-only editor */
  {
    int hw = cw * 4 / 5, hh = (ch - 1) * 4 / 5;
    if (hw < 24) hw = cw > 24 ? cw - 2 : cw;
    if (hh < 6)  hh = ch - 2;
    g_help_win = gtcaca_window_new_centered(NULL, "Help — key bindings", hw, hh);
    g_help_ed  = gtcaca_editor_new(GTCACA_WIDGET(g_help_win), 0, 0, 1, 1);
    gtcaca_editor_set_line_numbers(g_help_ed, 0);
    gtcaca_editor_set_read_only(g_help_ed, 1);
    gtcaca_editor_key_cb_register(g_help_ed, on_key, NULL);
    { gtcaca_box_t *hb = gtcaca_vbox_new(); gtcaca_box_set_margin(hb, 0);
      gtcaca_box_add_expand(hb, GTCACA_WIDGET(g_help_ed));
      gtcaca_box_apply(hb, g_help_win->x, g_help_win->y,
                       g_help_win->width, g_help_win->height);
      gtcaca_box_free(hb); }
    gtcaca_widget_hide(GTCACA_WIDGET(g_help_ed));
    gtcaca_widget_hide(GTCACA_WIDGET(g_help_win));
  }

  /* save-as dialog (created hidden; brought to front when shown) */
  {
    g_save_win   = gtcaca_window_new_centered(NULL, "Write file", 56, 10);
    g_save_label = gtcaca_label_new(GTCACA_WIDGET(g_save_win), "File name:", 2, 1);
    g_save_entry = gtcaca_entry_new(GTCACA_WIDGET(g_save_win), 2, 3, 52);
    g_save_ok    = gtcaca_button_new(GTCACA_WIDGET(g_save_win), "[ OK ]", 4, 5);
    g_save_cancel= gtcaca_button_new(GTCACA_WIDGET(g_save_win), "[ Cancel ]", 16, 5);
    gtcaca_button_key_cb_register(g_save_ok, dlg_ok);
    gtcaca_button_key_cb_register(g_save_cancel, dlg_cancel);
    gtcaca_entry_key_cb_register(g_save_entry, dlg_entry_key, NULL);
    gtcaca_window_set_default(g_save_win, GTCACA_WIDGET(g_save_ok));  /* Enter = OK */
    gtcaca_widget_hide(GTCACA_WIDGET(g_save_win));
    gtcaca_widget_hide(GTCACA_WIDGET(g_save_label));
    gtcaca_widget_hide(GTCACA_WIDGET(g_save_entry));
    gtcaca_widget_hide(GTCACA_WIDGET(g_save_ok));
    gtcaca_widget_hide(GTCACA_WIDGET(g_save_cancel));
  }

  g_modeline = gtcaca_statusbar_new("");
  if (g_bottom_reserve > 0) gtcaca_statusbar_set_rows_from_bottom(g_modeline, 1 + g_bottom_reserve);

  /* initial buffer in a single (root) leaf window */
  g_cur_buf = -1;
  b0 = buffer_create(open_file_path);   /* NULL => scratch */
  if (b0 < 0) b0 = buffer_create(NULL);
  memset(g_nodes, 0, sizeof g_nodes);
  g_root = 0;
  g_nodes[0].used = 1; g_nodes[0].is_leaf = 1; g_nodes[0].parent = -1;
  g_nodes[0].buf = b0; g_nodes[0].win = win_alloc();
  g_nodes[0].child[0] = g_nodes[0].child[1] = -1;
  g_buffers[b0].pane = 0;
  g_focus_leaf = 0;
  relayout();
  focus_pane(0);

  /* `ccm <file> +N` — jump to line N once the buffer is open */
  if (goto_line_arg > 0 && !open_dir && g_ed) {
    int nl = gtcaca_editor_get_line_count(g_ed);
    long ln = goto_line_arg > nl ? nl : goto_line_arg;
    gtcaca_editor_goto_line(g_ed, (int)(ln - 1));
    refresh_modeline(g_ed, NULL);   /* update L/C so the modeline isn't stale */
  }

  if (open_dir) {
    if (!ccm_realpath(arg, g_curdir)) { strncpy(g_curdir, arg, sizeof g_curdir - 1); g_curdir[sizeof g_curdir - 1] = '\0'; }
    show_browser();
  }

  gtcaca_set_paste_cb(ccm_paste, NULL);   /* a paste is one edit, not 150k keys */
  ccm_theme_show_warning();   /* surface any ~/.cacamacs/theme mistake (last, so it sticks) */
  gtcaca_main();

  buffer_store_globals(g_cur_buf);
  for (i = 0; i < g_nbuf; i++) {
    /* A view borrows its language config from the buffer it was split from
       (see buffer_create_view): freeing it here as well would free it twice. */
    if (g_buffers[i].is_view) continue;
    gtcaca_editor_langcfg_free(g_buffers[i].langcfg);
    gtcaca_editor_grammar_free(g_buffers[i].grammar);
  }
  free(g_killbuf);
  return 0;
}


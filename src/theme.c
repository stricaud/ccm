#include "cacamacs.h"
#include <strings.h>   /* strcasecmp */

/* ── where per-user configuration lives ─────────────────────────────────────────
 *
 * `~/.cacamacs/` on Unix, `%APPDATA%\cacamacs\` on Windows (falling back to
 * `%USERPROFILE%` when APPDATA is unset, as it is for a service account).
 * The theme, config.json and extensions/ all live there.
 */
#ifdef _WIN32
#define CFG_SEP  "\\"
#define CFG_DIR  "cacamacs"      /* under %APPDATA%, so no leading dot */
#else
#define CFG_SEP  "/"
#define CFG_DIR  ".cacamacs"     /* a dotfile directory in $HOME */
#endif

const char *ccm_config_dir(void)
{
  static char dir[PATH_MAX];
  static int  resolved = 0;
  const char *base;

  if (resolved) return dir;
  resolved = 1;
#ifdef _WIN32
  base = getenv("APPDATA");
  if (!base || !*base) base = getenv("USERPROFILE");
#else
  base = getenv("HOME");
#endif
  if (base && *base) snprintf(dir, sizeof dir, "%s" CFG_SEP CFG_DIR, base);
  return dir;
}

/* Where cacamacs' *shipped* data lives — the sample extensions and the default
   theme it installs alongside its binary, as opposed to the user's own files.
   $CCM_DATA_DIR wins; otherwise the parent of $CCM_BUILTIN_EXTENSIONS (the pip
   wheel's launcher already points that at the bundle); otherwise it is derived
   from the running executable, which is what makes a relocatable bundle find
   its own data instead of a prefix frozen in at build time. */
const char *ccm_data_dir(void)
{
  static char dir[PATH_MAX];
  static int  resolved = 0;
  const char *env;

  if (resolved) return dir;
  resolved = 1;

  env = getenv("CCM_DATA_DIR");
  if (env && *env) { snprintf(dir, sizeof dir, "%s", env); return dir; }

  env = getenv("CCM_BUILTIN_EXTENSIONS");
  if (env && *env) {                       /* .../share/ccm/extensions -> .../share/ccm */
    const char *cut = strrchr(env, '/');
#ifdef _WIN32
    const char *bs = strrchr(env, '\\');
    if (bs && (!cut || bs > cut)) cut = bs;
#endif
    if (cut && cut != env) { snprintf(dir, sizeof dir, "%.*s", (int)(cut - env), env); return dir; }
  }

#ifdef GTCACA_HAVE_THEME_SEARCH
  env = gtcaca_executable_dir();
  if (env && *env) {
    char cand[PATH_MAX];
    struct stat st;
    /* The Windows bundle is flat — data sits beside the exe; everywhere else it
       is the usual bin/ + share/ pair. */
    snprintf(cand, sizeof cand, "%s" CFG_SEP "themes", env);
    if (stat(cand, &st) == 0 && S_ISDIR(st.st_mode)) { snprintf(dir, sizeof dir, "%s", env); return dir; }
    snprintf(dir, sizeof dir, "%s" CFG_SEP ".." CFG_SEP "share" CFG_SEP "ccm", env);
  }
#endif
  return dir;
}

/* Absolute path of the configuration file `leaf`. Returned whether or not it
   exists — a caller that finds nothing there names it in its "not found"
   message, which is also where the file should be created. */
const char *ccm_config_path(const char *leaf, char *out, size_t outsz)
{
  snprintf(out, outsz, "%s" CFG_SEP "%s", ccm_config_dir(), leaf);
  return out;
}

/* Did the shipped themes/default actually make it into this install? */
int ccm_default_theme_present(void)
{
  char path[PATH_MAX];
  struct stat st;
  const char *data = ccm_data_dir();
  if (!data[0]) return 0;
  snprintf(path, sizeof path, "%s" CFG_SEP "themes" CFG_SEP "default", data);
  return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

/* ── per-user cacamacs colour theme (~/.cacamacs/theme) ─────────────────────────
 *
 * A small INI-ish file:  key = colour   (# starts a comment)
 *
 * Colours may be names (the 16 terminal colours), or #rrggbb / #rgb hex, or
 * rgb(r,g,b). On a normal terminal gtcaca's truecolour presenter renders them in
 * up to 4096 colours, so e.g. "background = #301934" is a real dark purple; on a
 * plain 16-colour libcaca terminal each colour falls back to the nearest of 16.
 *
 * Keys: background/bg, foreground/fg, comment, string, number, keyword,
 *       bracket, operator, selection_fg, selection_bg, current_line.
 */

typedef struct { int set; uint8_t a; uint16_t c12; } col_t;   /* ANSI fallback + 12-bit */

static struct {
  int   loaded;
  col_t bg, fg, comment, string, number, keyword, bracket, op, sel_fg, sel_bg, curline;
} T;

static char g_theme_warn[128] = "";   /* first parse mistake, shown at startup */

/* the 16 ANSI colours as 12-bit (0x0RGB) */
static const uint16_t ANSI12[16] = {
  0x000,0x00a,0x0a0,0x0aa,0xa00,0xa0a,0xa50,0xaaa,
  0x555,0x55f,0x5f5,0x5ff,0xf55,0xf5f,0xff5,0xfff
};

/* nearest of the 16 terminal colours for an r,g,b triple */
static uint8_t nearest_ansi(int r, int g, int b)
{
  static const struct { uint8_t c; int r, g, b; } P[16] = {
    {CACA_BLACK,0,0,0},       {CACA_BLUE,0,0,170},      {CACA_GREEN,0,170,0},      {CACA_CYAN,0,170,170},
    {CACA_RED,170,0,0},       {CACA_MAGENTA,170,0,170}, {CACA_BROWN,170,85,0},     {CACA_LIGHTGRAY,170,170,170},
    {CACA_DARKGRAY,85,85,85}, {CACA_LIGHTBLUE,85,85,255},{CACA_LIGHTGREEN,85,255,85},{CACA_LIGHTCYAN,85,255,255},
    {CACA_LIGHTRED,255,85,85},{CACA_LIGHTMAGENTA,255,85,255},{CACA_YELLOW,255,255,85},{CACA_WHITE,255,255,255}
  };
  int i, best = 0; long bd = -1;
  for (i = 0; i < 16; i++) {
    long d = (long)(r-P[i].r)*(r-P[i].r) + (long)(g-P[i].g)*(g-P[i].g) + (long)(b-P[i].b)*(b-P[i].b);
    if (bd < 0 || d < bd) { bd = d; best = i; }
  }
  return P[best].c;
}

static uint16_t rgb_to_12(int r, int g, int b) { return (uint16_t)(((r*15/255)<<8) | ((g*15/255)<<4) | (b*15/255)); }

/* a colour name, #rgb/#rrggbb hex, or rgb(r,g,b). Fills both the ANSI fallback
   and the 12-bit (4096) value. */
static int parse_color(const char *s, col_t *out)
{
  static const struct { const char *name; uint8_t c; } N[] = {
    {"black",CACA_BLACK},{"red",CACA_RED},{"green",CACA_GREEN},{"yellow",CACA_YELLOW},
    {"brown",CACA_BROWN},{"orange",CACA_BROWN},{"blue",CACA_BLUE},{"magenta",CACA_MAGENTA},
    {"purple",CACA_MAGENTA},{"violet",CACA_MAGENTA},{"cyan",CACA_CYAN},{"white",CACA_WHITE},
    {"gray",CACA_LIGHTGRAY},{"grey",CACA_LIGHTGRAY},{"lightgray",CACA_LIGHTGRAY},{"lightgrey",CACA_LIGHTGRAY},
    {"darkgray",CACA_DARKGRAY},{"darkgrey",CACA_DARKGRAY},{"brightblack",CACA_DARKGRAY},
    {"lightred",CACA_LIGHTRED},{"brightred",CACA_LIGHTRED},{"lightgreen",CACA_LIGHTGREEN},{"brightgreen",CACA_LIGHTGREEN},
    {"lightblue",CACA_LIGHTBLUE},{"brightblue",CACA_LIGHTBLUE},{"lightcyan",CACA_LIGHTCYAN},{"brightcyan",CACA_LIGHTCYAN},
    {"lightmagenta",CACA_LIGHTMAGENTA},{"brightmagenta",CACA_LIGHTMAGENTA},{"lightpurple",CACA_LIGHTMAGENTA},
    {"brightyellow",CACA_YELLOW},{NULL,0}
  };
  int i;
  unsigned int r, g, b;
  if (!s || !*s) return 0;

  if (*s == '#') {                                   /* HTML hex */
    const char *h = s + 1; size_t l = strlen(h);
    if      (l == 3 && sscanf(h, "%1x%1x%1x", &r, &g, &b) == 3) { r *= 17; g *= 17; b *= 17; }
    else if (l == 6 && sscanf(h, "%2x%2x%2x", &r, &g, &b) == 3) { /* as-is */ }
    else return 0;
    out->a = nearest_ansi((int)r, (int)g, (int)b); out->c12 = rgb_to_12((int)r, (int)g, (int)b); out->set = 1;
    return 1;
  }
  if (!strncasecmp(s, "rgb(", 4)) {                  /* rgb(r, g, b) */
    int rr, gg, bb;
    if (sscanf(s + 4, "%d , %d , %d", &rr, &gg, &bb) != 3) return 0;
    if (rr<0) rr=0; if (rr>255) rr=255; if (gg<0) gg=0; if (gg>255) gg=255; if (bb<0) bb=0; if (bb>255) bb=255;
    out->a = nearest_ansi(rr, gg, bb); out->c12 = rgb_to_12(rr, gg, bb); out->set = 1;
    return 1;
  }
  for (i = 0; N[i].name; i++)
    if (!strcasecmp(s, N[i].name)) { out->a = N[i].c; out->c12 = ANSI12[N[i].c & 15]; out->set = 1; return 1; }
  return 0;
}

static char *trim(char *s)
{
  char *e;
  while (*s && isspace((unsigned char)*s)) s++;
  e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
  return s;
}

/* Is `k` one of gtcaca's chrome keys — <widget>_bg / <widget>_fg?
   The two theme layers are one file each, read twice: gtcaca takes the chrome
   keys out of them and cacamacs takes the syntax colours, each ignoring what
   the other owns. Without this test every chrome line in the file would be
   reported as an unknown setting. cacamacs' own selection_bg / selection_fg
   end the same way, but they are matched before this is ever consulted. */
static int is_chrome_key(const char *k)
{
  size_t n = strlen(k);
  if (n < 4) return 0;
  return !strcasecmp(k + n - 3, "_bg") || !strcasecmp(k + n - 3, "_fg");
}

/* Apply one theme file over whatever is already in T; keys it does not mention
   keep their current value, which is what makes the layers stack. Returns 0 if
   the file is not there. */
static int theme_parse_file(const char *path)
{
  char line[256], badbuf[120] = "";
  int  nbad = 0, lineno = 0;
  FILE *f = fopen(path, "r");

  if (!f) return 0;

  while (fgets(line, sizeof line, f)) {
    char *p = line, *eq, *k, *v, *c;
    col_t col = {0,0,0};
    int known = 1;
    lineno++;
    /* Skip full-line comments and blanks: a '#' as the first non-blank char is
       a comment, even when the line contains an '=' (e.g. a "key = colour"
       example in the header). A '#' on the value side is still a hex colour —
       that is handled after the '=' split below. */
    { char *q = p; while (*q && isspace((unsigned char)*q)) q++;
      if (*q == '#' || *q == '\0') continue; }
    eq = strchr(p, '=');
    if (!eq) continue;
    *eq = '\0'; k = trim(p); v = trim(eq + 1);   /* trim first: a leading '#' is a hex value, not a comment */
    for (c = v; *c; c++)
      if (*c == '#' && c > v && isspace((unsigned char)c[-1])) { *c = '\0'; break; }
    v = trim(v);
    if (!*k || !*v) continue;
    if (!parse_color(v, &col)) {                 /* e.g. a typo like #00gg00 (g isn't hex) */
      if (!nbad++) snprintf(badbuf, sizeof badbuf, "line %d: invalid colour \"%s\" for \"%s\"", lineno, v, k);
      continue;
    }

    if      (!strcasecmp(k, "background") || !strcasecmp(k, "bg"))        T.bg = col;
    else if (!strcasecmp(k, "foreground") || !strcasecmp(k, "fg") || !strcasecmp(k, "default")) T.fg = col;
    else if (!strcasecmp(k, "comment"))                                  T.comment = col;
    else if (!strcasecmp(k, "string"))                                   T.string = col;
    else if (!strcasecmp(k, "number") || !strcasecmp(k, "constant"))     T.number = col;
    else if (!strcasecmp(k, "keyword"))                                  T.keyword = col;
    else if (!strcasecmp(k, "bracket"))                                  T.bracket = col;
    else if (!strcasecmp(k, "operator"))                                 T.op = col;
    else if (!strcasecmp(k, "selection_fg") || !strcasecmp(k, "selectionfg")) T.sel_fg = col;
    else if (!strcasecmp(k, "selection_bg") || !strcasecmp(k, "selectionbg") || !strcasecmp(k, "selection")) T.sel_bg = col;
    else if (!strcasecmp(k, "current_line") || !strcasecmp(k, "currentline") || !strcasecmp(k, "cursorline")) T.curline = col;
    else known = 0;
    if (!known && !is_chrome_key(k) && !nbad++)
      snprintf(badbuf, sizeof badbuf, "line %d: unknown setting \"%s\"", lineno, k);
  }
  fclose(f);

  /* First mistake wins: an earlier layer's warning is not worth losing to a
     later file that happens to be clean. */
  if (nbad && !g_theme_warn[0])
    snprintf(g_theme_warn, sizeof g_theme_warn, "%s: %s%s", path, badbuf, nbad > 1 ? "  (+ more)" : "");
  return 1;
}

/* The colours cacamacs draws with, in layers — the theme cacamacs ships, then
   the user's own file over it, so a personal theme only has to name what it
   changes. Both are read by the same parser; the shipped one is the same file
   gtcaca reads its chrome out of, which is what lets a single themes/default
   carry the window colours and the syntax colours together. */
void ccm_theme_load(void)
{
  char path[PATH_MAX];
  int  got = 0;

  memset(&T, 0, sizeof T);
  g_theme_warn[0] = '\0';

  if (ccm_data_dir()[0]) {
    snprintf(path, sizeof path, "%s" CFG_SEP "themes" CFG_SEP "default", ccm_data_dir());
    got |= theme_parse_file(path);
  }
  if (ccm_config_dir()[0]) {
    ccm_config_path("theme", path, sizeof path);
    got |= theme_parse_file(path);
  }
  T.loaded = got;
}

/* Put any theme-file warning on the status line. Call it last in startup, after
   the buffer/language messages, so the warning isn't immediately overwritten. */
void ccm_theme_show_warning(void)
{
  if (!g_theme_warn[0]) return;
  snprintf(g_message, sizeof g_message, "%s", g_theme_warn);
  if (g_ed) refresh_modeline(g_ed, NULL);   /* rebuild the status text with the warning */
}

/* run before any editors are created: tints the shared editor surface (ANSI) */
void ccm_theme_apply_global(void)
{
  if (!T.loaded) return;
  if (T.bg.set) { gmo.theme.textview.bg = T.bg.a; gmo.theme.textviewfocus.bg = T.bg.a; }
  if (T.fg.set) { gmo.theme.textview.fg = T.fg.a; gmo.theme.textviewfocus.fg = T.fg.a; }
}

/* run per editor: ANSI fallback + the 12-bit (4096-colour) surface and palette */
void ccm_theme_apply_editor(gtcaca_editor_widget_t *ed)
{
  if (!T.loaded || !ed) return;
  if (T.bg.set) {
    ed->color_focus_bg = ed->color_nonfocus_bg = T.bg.a;
    gtcaca_editor_set_bg_rgb12(ed, T.bg.c12);
  }
  if (T.fg.set) {
    ed->color_focus_fg = ed->color_nonfocus_fg = T.fg.a;
    gtcaca_editor_set_fg_rgb12(ed, T.fg.c12);
    gtcaca_editor_style_set_fore(ed, GTCACA_EDITOR_STYLE_DEFAULT, T.fg.a);
    gtcaca_editor_style_set_fore_rgb12(ed, GTCACA_EDITOR_STYLE_DEFAULT, T.fg.c12);
  }
  #define STY(field, S) do { if ((field).set) { \
      gtcaca_editor_style_set_fore(ed, S, (field).a); \
      gtcaca_editor_style_set_fore_rgb12(ed, S, (field).c12); } } while (0)
  STY(T.comment,  GTCACA_EDITOR_STYLE_COMMENT);
  STY(T.string,   GTCACA_EDITOR_STYLE_STRING);
  STY(T.number,   GTCACA_EDITOR_STYLE_NUMBER);
  STY(T.keyword,  GTCACA_EDITOR_STYLE_KEYWORD);
  STY(T.bracket,  GTCACA_EDITOR_STYLE_BRACKET);
  STY(T.op,       GTCACA_EDITOR_STYLE_OPERATOR);
  #undef STY
  if (T.sel_fg.set || T.sel_bg.set)
    gtcaca_editor_set_selection_colors(ed, T.sel_fg.set ? T.sel_fg.a : CACA_WHITE,
                                           T.sel_bg.set ? T.sel_bg.a : CACA_BLUE);
  if (T.curline.set) gtcaca_editor_set_caret_line_back(ed, T.curline.a);
}

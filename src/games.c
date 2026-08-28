/* games.c — snake, sokoban, bird and wordguess, split out of cacamacs.c. They are
   self-contained game loops; the only shared symbol is g_message. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <caca.h>
#include <gtcaca/main.h>
#include "cacamacs.h"

/* ── snake / nibbles (M-x snake) ────────────────────────────────────────────
 * A self-contained worm game in the spirit of the classic Nibbles: steer the
 * worm to eat the numbered targets 1..9, growing each time; clear all nine to
 * advance a level (faster, new walls). Hitting a wall or yourself costs a life.
 * Original implementation with its own level layouts. */

#define SNAKE_GRID(g, W, x, y) (g)[(y) * (W) + (x)]
enum { SC_EMPTY = 0, SC_WALL = 1, SC_BODY = 2 };

/* the play field is a fixed 80x25 (classic Nibbles), centred on the canvas; all
   grid coordinates are offset by (snake_ox, snake_oy) when drawn to the screen */
static int snake_ox = 0, snake_oy = 0;

/* paint a solid cell with a full-block glyph █ in `colour`. Using a glyph (not a
   space with a coloured background) guarantees the display repaints the cell —
   some drivers don't repaint space cells whose only change is the background. */
static void snake_cell(int x, int y, uint8_t colour)
{
  caca_set_color_ansi(gmo.cv, colour, CACA_BLACK);
  caca_set_attr(gmo.cv, 0);
  caca_put_char(gmo.cv, snake_ox + x, snake_oy + y, 0x2588);   /* █ */
}

/* fill the whole canvas with solid black blocks (a clean, always-repainted bg) */
static void game_fill_bg(void)
{
  int x, y, CW = caca_get_canvas_width(gmo.cv), CH = caca_get_canvas_height(gmo.cv);
  caca_set_color_ansi(gmo.cv, CACA_BLACK, CACA_BLACK);
  caca_set_attr(gmo.cv, 0);
  for (y = 0; y < CH; y++)
    for (x = 0; x < CW; x++)
      caca_put_char(gmo.cv, x, y, 0x2588);
}

/* lay out the walls for `level` into the grid (border + an original pattern) */
static void snake_build_level(unsigned char *g, int W, int H, int level)
{
  int x, y, ix0 = 1, iy0 = 2, ix1 = W - 2, iy1 = H - 2;
  int cx = W / 2, cy = (iy0 + iy1) / 2;
  memset(g, SC_EMPTY, (size_t)W * H);
  /* outer border (row 0 is the status line; the field starts at row 1) */
  for (x = 0; x < W; x++) { SNAKE_GRID(g, W, x, 1) = SC_WALL; SNAKE_GRID(g, W, x, H - 1) = SC_WALL; }
  for (y = 1; y < H; y++) { SNAKE_GRID(g, W, 0, y) = SC_WALL; SNAKE_GRID(g, W, W - 1, y) = SC_WALL; }

  switch (level % 5) {
  case 0: /* open field */
    break;
  case 1: /* two horizontal bars */
    for (x = ix0 + W / 6; x <= ix1 - W / 6; x++) {
      SNAKE_GRID(g, W, x, iy0 + (iy1 - iy0) / 3) = SC_WALL;
      SNAKE_GRID(g, W, x, iy1 - (iy1 - iy0) / 3) = SC_WALL;
    }
    break;
  case 2: /* a central cross */
    for (x = ix0 + W / 8; x <= ix1 - W / 8; x++) SNAKE_GRID(g, W, x, cy) = SC_WALL;
    for (y = iy0 + 1; y <= iy1 - 1; y++) SNAKE_GRID(g, W, cx, y) = SC_WALL;
    SNAKE_GRID(g, W, cx, cy) = SC_EMPTY;            /* gap at the centre */
    SNAKE_GRID(g, W, cx, cy - 1) = SC_EMPTY;
    SNAKE_GRID(g, W, cx, cy + 1) = SC_EMPTY;
    break;
  case 3: /* four corner blocks */
    for (y = 0; y < (iy1 - iy0) / 3; y++)
      for (x = 0; x < W / 5; x++) {
        SNAKE_GRID(g, W, ix0 + 2 + x, iy0 + 1 + y) = SC_WALL;
        SNAKE_GRID(g, W, ix1 - 2 - x, iy0 + 1 + y) = SC_WALL;
        SNAKE_GRID(g, W, ix0 + 2 + x, iy1 - 1 - y) = SC_WALL;
        SNAKE_GRID(g, W, ix1 - 2 - x, iy1 - 1 - y) = SC_WALL;
      }
    break;
  default: /* zig-zag rails */
    for (y = iy0 + 2; y <= iy1 - 2; y += 4) {
      int x0 = (y % 8 == iy0 + 2 % 8) ? ix0 + 1 : cx;
      for (x = x0; x < x0 + (ix1 - ix0) / 2; x++)
        if (x > ix0 && x < ix1) SNAKE_GRID(g, W, x, y) = SC_WALL;
    }
    break;
  }

  /* Always keep the worm's spawn runway clear. The worm starts at
     (cx-3..cx, H/2) heading right (see snake_reset_worm), so open that row
     around the centre — otherwise a pattern crossing the middle (e.g. the
     central cross) buries the snake the instant the level starts. */
  { int ry = H / 2, rx;
    for (rx = cx - 3; rx <= cx + 6; rx++)
      if (rx > 0 && rx < W - 1) SNAKE_GRID(g, W, rx, ry) = SC_EMPTY; }
}

static void snake_place_food(unsigned char *g, int W, int H, int *fx, int *fy)
{
  int tries;
  for (tries = 0; tries < W * H * 4; tries++) {
    int x = 1 + rand() % (W - 2);
    int y = 2 + rand() % (H - 3);
    if (SNAKE_GRID(g, W, x, y) == SC_EMPTY) { *fx = x; *fy = y; return; }
  }
  *fx = W / 2; *fy = H / 2;
}

/* centre a fresh 4-cell worm heading right; returns via the deque state */
static void snake_reset_worm(unsigned char *g, int W, int H, int *body, int *head, int *tail,
                             int *len, int *dx, int *dy)
{
  int cx = W / 2, cy = H / 2, i;
  /* clear any previous body marks */
  for (i = 0; i < W * H; i++) if (g[i] == SC_BODY) g[i] = SC_EMPTY;
  *head = 0; *tail = 0; *len = 0; *dx = 1; *dy = 0;
  for (i = 0; i < 4; i++) {
    int x = cx - 3 + i, y = cy;
    body[i * 2] = x; body[i * 2 + 1] = y;
    SNAKE_GRID(g, W, x, y) = SC_BODY;
    *head = i; (*len)++;
  }
  *tail = 0;
}

void run_snake(void)
{
  int CW = caca_get_canvas_width(gmo.cv);
  int CH = caca_get_canvas_height(gmo.cv);
  int W = CW < 80 ? CW : 80;        /* fixed 80x25 play field (classic Nibbles), */
  int H = CH < 25 ? CH : 25;        /* shrunk only if the terminal is smaller     */
  unsigned char *g;
  int *body;                       /* ring of (x,y) pairs, cap W*H            */
  int cap = W * H, head = 0, tail = 0, len = 0, dx = 1, dy = 0, ndx = 1, ndy = 0;
  int fx = 0, fy = 0, level = 0, number = 1, score = 0, lives = 5;
  int running = 1, started = 0, x, y;
  caca_event_t ev;

  if (W < 20 || H < 8) { snprintf(g_message, sizeof g_message, "Window too small for snake"); return; }
  snake_ox = (CW - W) / 2;          /* centre the play field on the canvas */
  snake_oy = (CH - H) / 2;
  g = malloc((size_t)cap);
  body = malloc((size_t)cap * 2 * sizeof(int));
  if (!g || !body) { free(g); free(body); return; }
  srand((unsigned)time(NULL));

  /* start screen: choose a starting level (as Nibbles prompts for skill) */
  {
    int chosen = 0, ox = snake_ox, oy = snake_oy;
    game_fill_bg();
    caca_set_color_ansi(gmo.cv, CACA_WHITE, CACA_BLACK); caca_set_attr(gmo.cv, CACA_BOLD);
    caca_printf(gmo.cv, ox + W / 2 - 4, oy + H / 2 - 3, "S N A K E");
    caca_set_attr(gmo.cv, 0);
    caca_printf(gmo.cv, ox + W / 2 - 18, oy + H / 2 - 1, "Start at which level (1-9)?  Enter = 1");
    caca_printf(gmo.cv, ox + W / 2 - 18, oy + H / 2 + 1, "Eat 1..9 to clear a level; it speeds up each level.");
    caca_printf(gmo.cv, ox + W / 2 - 18, oy + H / 2 + 2, "Steer with the arrows.  p pauses, q quits.");
    caca_refresh_display(gmo.dp);
    while (!chosen) {
      if (caca_get_event(gmo.dp, CACA_EVENT_KEY_PRESS, &ev, -1)) {
        int k = caca_get_event_key_ch(&ev);
        if (k >= '1' && k <= '9') { level = k - '1'; chosen = 1; }
        else if (k == CACA_KEY_RETURN || k == 10 || k == ' ') { level = 0; chosen = 1; }
        else if (k == 'q' || k == 'Q' || k == CACA_KEY_ESCAPE) { free(g); free(body); gtcaca_redraw(); return; }
      }
    }
  }

  snake_build_level(g, W, H, level);
  snake_reset_worm(g, W, H, body, &head, &tail, &len, &dx, &dy);
  ndx = dx; ndy = dy;
  snake_place_food(g, W, H, &fx, &fy);

  while (running) {
    int nx, ny, ate, tx, ty, idx;

    /* ── draw the whole board ── */
    game_fill_bg();                           /* solid black field */
    caca_set_color_ansi(gmo.cv, CACA_WHITE, CACA_BLACK); caca_set_attr(gmo.cv, 0);
    caca_printf(gmo.cv, snake_ox + 1, snake_oy + 0, "SNAKE  level %d   eat: %d   score: %d   lives: %d   (p pause, q quit)",
                level + 1, number, score, lives);
    for (y = 1; y < H; y++)
      for (x = 0; x < W; x++)
        if (SNAKE_GRID(g, W, x, y) == SC_WALL) snake_cell(x, y, CACA_BLUE);
    for (idx = 0; idx < len; idx++) {
      int bi = (tail + idx) % cap;
      snake_cell(body[bi * 2], body[bi * 2 + 1], bi == head ? CACA_LIGHTGREEN : CACA_GREEN);
    }
    caca_set_color_ansi(gmo.cv, CACA_YELLOW, CACA_BLACK); caca_set_attr(gmo.cv, CACA_BOLD);
    caca_put_char(gmo.cv, snake_ox + fx, snake_oy + fy, '0' + number);
    if (!started) {                            /* keep clear of the worm's row */
      caca_set_color_ansi(gmo.cv, CACA_WHITE, CACA_BLACK); caca_set_attr(gmo.cv, CACA_BOLD);
      caca_printf(gmo.cv, snake_ox + W / 2 - 12, snake_oy + H / 2 - 4, " press an arrow to start ");
    }
    caca_refresh_display(gmo.dp);

    /* ── input (block until the game starts, else tick).  caca_get_event's
       timeout is in MICROSECONDS, so a playable ~8 moves/sec is ~120000us,
       getting a little faster each level. ── */
    int tick_ms = 130 - level * 8; if (tick_ms < 70) tick_ms = 70;
    if (caca_get_event(gmo.dp, CACA_EVENT_KEY_PRESS, &ev, started ? tick_ms * 1000 : -1)) {
      int k = caca_get_event_key_ch(&ev);
      if (k == 'q' || k == 'Q' || k == CACA_KEY_ESCAPE) { running = 0; continue; }
      if (k == 'p' || k == 'P') {            /* pause */
        caca_set_color_ansi(gmo.cv, CACA_WHITE, CACA_BLACK); caca_set_attr(gmo.cv, CACA_BOLD);
        caca_printf(gmo.cv, snake_ox + W / 2 - 5, snake_oy + H / 2 - 4, " paused ");
        caca_refresh_display(gmo.dp);
        caca_get_event(gmo.dp, CACA_EVENT_KEY_PRESS, &ev, -1);
        continue;
      }
      /* any arrow starts the game; the direction is taken unless it reverses */
      {
        int dir = 1;
        if      (k == CACA_KEY_UP    || k == CACA_KEY_CTRL_P) { if (dy == 0) { ndx = 0; ndy = -1; } }
        else if (k == CACA_KEY_DOWN  || k == CACA_KEY_CTRL_N) { if (dy == 0) { ndx = 0; ndy = 1; } }
        else if (k == CACA_KEY_LEFT  || k == CACA_KEY_CTRL_B) { if (dx == 0) { ndx = -1; ndy = 0; } }
        else if (k == CACA_KEY_RIGHT || k == CACA_KEY_CTRL_F) { if (dx == 0) { ndx = 1; ndy = 0; } }
        else dir = 0;
        if (dir) started = 1;
      }
    }
    if (!started) continue;

    /* ── advance one step ── */
    dx = ndx; dy = ndy;
    nx = body[head * 2] + dx; ny = body[head * 2 + 1] + dy;
    tx = body[tail * 2]; ty = body[tail * 2 + 1];
    ate = (nx == fx && ny == fy);
    /* moving into the tail cell is fine unless we are about to grow into it */
    if (SNAKE_GRID(g, W, nx, ny) == SC_WALL ||
        (SNAKE_GRID(g, W, nx, ny) == SC_BODY && !(nx == tx && ny == ty && !ate))) {
      if (--lives <= 0) {                    /* game over */
        caca_set_color_ansi(gmo.cv, CACA_WHITE, CACA_RED); caca_set_attr(gmo.cv, CACA_BOLD);
        caca_printf(gmo.cv, snake_ox + W / 2 - 14, snake_oy + H / 2, "  GAME OVER  -  score %d  -  r restart, q quit  ", score);
        caca_refresh_display(gmo.dp);
        for (;;) {
          caca_get_event(gmo.dp, CACA_EVENT_KEY_PRESS, &ev, -1);
          { int k = caca_get_event_key_ch(&ev);
            if (k == 'r' || k == 'R') { level = 0; number = 1; score = 0; lives = 5; started = 0;
              snake_build_level(g, W, H, level); snake_reset_worm(g, W, H, body, &head, &tail, &len, &dx, &dy);
              ndx = dx; ndy = dy; snake_place_food(g, W, H, &fx, &fy); break; }
            if (k == 'q' || k == 'Q' || k == CACA_KEY_ESCAPE) { running = 0; break; } }
        }
      } else {                               /* lose a life, keep the level */
        snake_reset_worm(g, W, H, body, &head, &tail, &len, &dx, &dy); ndx = dx; ndy = dy; started = 0;
      }
      continue;
    }

    /* add the new head */
    head = (head + 1) % cap; body[head * 2] = nx; body[head * 2 + 1] = ny;
    SNAKE_GRID(g, W, nx, ny) = SC_BODY; len++;

    if (ate) {
      score += number * (level + 1) * 10;
      number++;
      if (number > 9) {                      /* level complete */
        level++; number = 1; started = 0;
        snake_build_level(g, W, H, level);
        snake_reset_worm(g, W, H, body, &head, &tail, &len, &dx, &dy); ndx = dx; ndy = dy;
      }
      snake_place_food(g, W, H, &fx, &fy);
    } else {                                 /* pop the tail */
      SNAKE_GRID(g, W, tx, ty) = SC_EMPTY;
      tail = (tail + 1) % cap; len--;
    }
  }

  free(g); free(body);
  snprintf(g_message, sizeof g_message, "Snake: final score %d", score);
  gtcaca_redraw();
  caca_refresh_display(gmo.dp);
}

/* ── sokoban (M-x sokoban) ──────────────────────────────────────────────────
 * Levels are loaded from files at runtime (one level per N.txt), so we never
 * embed a level set. Legend: X wall, * player, # box, . goal, $ box-on-goal,
 * % player-on-goal, space floor. Each cell is drawn as a multi-cell sprite. */

#define SOK_MAXW 96
#define SOK_MAXH 48
enum { SOKS_FLOOR = 0, SOKS_WALL, SOKS_GOAL };

typedef struct {
  int w, h, pr, pc;
  unsigned char st[SOK_MAXH][SOK_MAXW];    /* SOKS_* (walls/goals are static) */
  unsigned char box[SOK_MAXH][SOK_MAXW];   /* movable boxes */
} sok_level_t;

typedef struct { signed char dr, dc; unsigned char pushed; } sok_move_t;

/* Optional override: drop your own N.txt levels in ~/.cacamacs/sokoban and the
   game uses those instead of the built-in set below. */
static int sok_find_dir(char *out, size_t n)
{
  const char *home = getenv("HOME");
  struct stat s; char p[PATH_MAX];
  if (!home) return 0;
  snprintf(p, sizeof p, "%s/.cacamacs/sokoban", home);
  if (stat(p, &s) == 0 && S_ISDIR(s.st_mode)) { snprintf(out, n, "%s", p); return 1; }
  return 0;
}

static int sok_load(const char *dir, int idx, sok_level_t *L)
{
  char path[PATH_MAX]; FILE *f; char line[256]; int r = 0, maxw = 0;
  snprintf(path, sizeof path, "%s/%d.txt", dir, idx);
  f = fopen(path, "r");
  if (!f) return 0;
  memset(L, 0, sizeof *L);
  while (fgets(line, sizeof line, f) && r < SOK_MAXH) {
    int len = (int)strlen(line), c;
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
    for (c = 0; c < len && c < SOK_MAXW; c++) {
      switch (line[c]) {
      case 'X': L->st[r][c] = SOKS_WALL; break;
      case '.': L->st[r][c] = SOKS_GOAL; break;
      case '#': L->box[r][c] = 1; break;
      case '%': L->st[r][c] = SOKS_GOAL; L->box[r][c] = 1; break;  /* box on target */
      case '*': L->pr = r; L->pc = c; break;
      case '$': L->st[r][c] = SOKS_GOAL; L->pr = r; L->pc = c; break;  /* player on target */
      default: break;
      }
    }
    if (len > maxw) maxw = len;
    r++;
  }
  fclose(f);
  L->h = r; L->w = maxw;
  return r > 0;
}

/* Built-in level set, embedded as text (legend: X wall, * player, # box,
   . target, % box-on-target, $ player-on-target). Each level is a
   NULL-terminated list of rows; add more by extending the array. */
static const char *const g_sok_levels[][16] = {
  { "     XXXXX",
    "XXXXXX . X",
    "X   XX#. X",
    "X*###  . X",
    "XX  X  . X",
    " X  XXXXXX",
    " XXXX     ",
    NULL },
  { "XXXXXX",
    "X    X",
    "X### X",
    "X....X",
    "X ##.X",
    "X  * X",
    "XXXXXX",
    NULL },
  { "XXXXXX  ",
    "X *  X  ",
    "X XX#XXX",
    "X  . . X",
    "XXX #  X",
    "  XXX  X",
    "    XXXX",
    NULL },
  { "XXXXX",
    "X * X",
    "X X XXXX",
    "X ...  X",
    "XX ### X",
    " XXX   X",
    "   XXXXX",
    NULL },
  { "XXXXXX",
    "X.   X",
    "X.   X",
    "XX . X",
    "X  XXX",
    "X ## X",
    "X  # X",
    "X  * X",
    "XXXXXX",
    NULL },
  { "XXXXXXXXX",
    "X   X   X",
    "X %. .. X",
    "XX#X X#XX",
    "X #     X",
    "X   X * X",
    "XXXXXXXXX",
    NULL },
  { "     XXXX",
    "     X  X",
    "     X  X",
    "XXXXXX  X",
    "X*   XX X",
    "X ###X .X",
    "X # #  .X",
    "X   X #.X",
    "XXXXX...X",
    "    XXXXX",
    NULL },
  { " XXXXXX ",
    " X    X ",
    " X X##XX",
    " X     X",
    "XX#XXX X",
    "X  X . X",
    "X    . X",
    "XXX  $ X",
    "  XXXXXX",
    NULL },
  { " XXXXXX ",
    " X    X ",
    " X XX X ",
    "XX .#.X ",
    "X  #*#XX",
    "X X.#. X",
    "X X XX X",
    "X      X",
    "XXXXXXXX",
    NULL },
  { "  XXXX ",
    " XX  X ",
    "XX   XX",
    "X .#. X",
    "X X#X X",
    "X  *  X",
    "XXXXXXX",
    NULL },
  { " XXXX  ",
    " X  XX ",
    "XX # X",
    "X .%.XX",
    "X X#  X",
    "X  *  X",
    "XXXXXXX",
    NULL },
  { "XXXXXXXXX",
    "X   ... X",
    "X   XXX X",
    "XXX #   X",
    "X  #X X X",
    "X # X * X",
    "X   XXXXX",
    "XXXXX    ",
    NULL },
  { "XXXXXXXXX",
    "X.....  X",
    "X   . # X",
    "XXXX XXXX",
    "X #     X",
    "X ### # X",
    "X * X   X",
    "XXXXXXXXX",
    NULL },
  { " XXXX     ",
    " X  X     ",
    " X  XXXXX ",
    " X      XX",
    " XX XX # X",
    "XXX X. #*X",
    "X    .X#XX",
    "X   X.  X ",
    "XXXXXXXXX",
    NULL },
  { "  XXXXX ",
    "  X  .XX",
    "XXX.#. X",
    "X  ##X X",
    "X   *  X",
    "XXXXXXXX",
    NULL },
  { "  XXXXX",
    "XXX  .X",
    "X   X X",
    "X*###.X",
    "X   X X",
    "X  XX X",
    "X    .X",
    "XXXXXXX",
    NULL },
  { "XXXXXXXX",
    "X      X",
    "X ##   X",
    "XX*XX  X",
    " X# X#XX",
    " X.... X",
    " X     X",
    " XXXXXXX",
    NULL },
  { "  XXXXXXX",
    " XX     X",
    " X    # X",
    " X  X #XX",
    "XXX#XX  X",
    "X   ..%.X",
    "X   X * X",
    "XXXXXXXXX",
    NULL },
  { " XXXX     ",
    " X  XXXXXX",
    "XX #X  . X",
    "X #*# .%.X",
    "X  # X . X",
    "X   XXXXXX",
    "XXXXX     ",
    NULL },
  { "XXXXXXX",
    "X     X",
    "X % % X",
    "XX. #XX",
    "X % % X",
    "X     X",
    "X *X  X",
    "XXXXXXX",
    NULL },
  { "   XXXXX",
    "   X   X",
    "XXXX#  X",
    "X   # XX",
    "X X X  X",
    "X * . .X",
    "XXXXXXXX",
    NULL },
  { "  XXXXXXX",
    "  X  .  X",
    " XXX#X  X",
    " X  . . X",
    "XXX#X# XX",
    "X  . . .X",
    "X #X#X# X",
    "X  * X  X",
    "XXXXXXXXX",
    NULL },
  { "  XXXXX   ",
    "XXX   XXXX",
    "X  ..##  X",
    "X  ##..  X",
    "XXXX * XXX",
    "   XXXXX  ",
    NULL },
  { "XXXXXXXX",
    "X ...  X",
    "X  XX# X",
    "X  #   X",
    "XX#XX#XX",
    "X   #  X",
    "X #XX  X",
    "X  ...*X",
    "XXXXXXXX",
    NULL },
  { "XXXXXX",
    "X    X",
    "X### X",
    "X....X",
    "X ##.X",
    "X  * X",
    "XXXXXX",
    NULL },
  { "   XXXXX",
    " XXX   X",
    " X   # X",
    "XX#..#XX",
    "X #.. X ",
    "X * XXX ",
    "XXXXX    ",
    NULL },
  { " XXXXXX   ",
    " X    X   ",
    " X XX#XXXX",
    " X X #   X",
    " X X * # X",
    "XX X#XX XX",
    "X ..    X ",
    "X ..X   X ",
    "XXXXXXXXX ",
    NULL },
  { " XXXXXX   ",
    " X    X   ",
    " X XX XXXX",
    " X X ##  X",
    " X X     X",
    "XX X XX#XX",
    "X ....# X ",
    "X   X * X ",
    "XXXXXXXXX ",
    NULL },
  { " XXXXX    ",
    " X   XXXXX",
    " X X ..  X",
    " X X ..  X",
    "XX#X#XX XX",
    "X # #   X ",
    "X * X   X ",
    "XXXXXXXXX ",
    NULL },
  { "XXXXXXXXX",
    "X       X",
    "X # # # X",
    "XX#XX XXX",
    " X X. . X",
    " X  . . X",
    " X * X  X",
    " XXXXXXXX",
    NULL },
  { " XXXXX ",
    " X   X ",
    "XX X XX",
    "X # # X",
    "X.. ..X",
    "X #X# X",
    "XX * XX",
    " XXXXX ",
    NULL },
  { " XXXXX ",
    " X   X ",
    "XX X XX",
    "X # # X",
    "X.....X",
    "X ### X",
    "XX * XX",
    " XXXXX ",
    NULL },
  { "XXXXX ",
    "X   X ",
    "X # X ",
    "X*##XX",
    "XX#..X",
    " X ..X",
    " X   X",
    " XXXXX",
    NULL },
  { "XXXXXXXXXXXXXXXX",
    "X              X",
    "X              X",
    "X              X",
    "X              X",
    "X * #   ..     X",
    "X              X",
    "X              X",
    "X              X",
    "X              X",
    "XXXXXXXXXXXXXXXX",
    NULL },
};
#define SOK_NLEVELS ((int)(sizeof g_sok_levels / sizeof g_sok_levels[0]))

static int sok_load_embedded(int idx, sok_level_t *L)
{
  const char *const *rows;
  int r, c;
  if (idx < 1 || idx > SOK_NLEVELS) return 0;
  rows = g_sok_levels[idx - 1];
  memset(L, 0, sizeof *L);
  for (r = 0; rows[r]; r++) {
    for (c = 0; rows[r][c]; c++) {
      char ch = rows[r][c];
      if (ch == 'X') L->st[r][c] = SOKS_WALL;
      else if (ch == '.') L->st[r][c] = SOKS_GOAL;
      else if (ch == '#') L->box[r][c] = 1;
      else if (ch == '%') { L->st[r][c] = SOKS_GOAL; L->box[r][c] = 1; }   /* box on target */
      else if (ch == '*') { L->pr = r; L->pc = c; }
      else if (ch == '$') { L->st[r][c] = SOKS_GOAL; L->pr = r; L->pc = c; } /* player on target */
    }
    if ((int)strlen(rows[r]) > L->w) L->w = (int)strlen(rows[r]);
  }
  L->h = r;
  return 1;
}

/* load level `idx` from files if a level dir was found, else from the built-in set */
static int sok_get(int have_dir, const char *dir, int idx, sok_level_t *L)
{
  return have_dir ? sok_load(dir, idx, L) : sok_load_embedded(idx, L);
}

static int sok_won(const sok_level_t *L)
{
  int r, c, boxes = 0;
  for (r = 0; r < L->h; r++)              /* solved when every box sits on a target */
    for (c = 0; c < L->w; c++)
      if (L->box[r][c]) { boxes++; if (L->st[r][c] != SOKS_GOAL) return 0; }
  return boxes > 0;
}

/* draw one cell as a sprite of tw x th terminal cells */
static void sok_tile(int sx, int sy, int tw, int th, int kind)
{
  int i, j;
  switch (kind) {
  case 1:                                   /* wall: solid grey blocks */
    caca_set_color_ansi(gmo.cv, CACA_LIGHTGRAY, CACA_BLACK); caca_set_attr(gmo.cv, 0);
    for (j = 0; j < th; j++) for (i = 0; i < tw; i++) caca_put_char(gmo.cv, sx + i, sy + j, 0x2588);
    break;
  case 3: case 4: {                         /* box / box-on-goal */
    uint8_t col = (kind == 4) ? CACA_GREEN : CACA_BROWN;
    caca_set_color_ansi(gmo.cv, col, CACA_BLACK); caca_set_attr(gmo.cv, 0);
    for (j = 0; j < th; j++) for (i = 0; i < tw; i++) caca_put_char(gmo.cv, sx + i, sy + j, 0x2588);
    if (tw >= 4 && th >= 2) {                /* crate cross-braces */
      caca_set_color_ansi(gmo.cv, CACA_BLACK, col); caca_set_attr(gmo.cv, CACA_BOLD);
      caca_put_char(gmo.cv, sx + 1, sy,        'X');
      caca_put_char(gmo.cv, sx + tw - 2, sy,   'X');
    }
    break;
  }
  case 2:                                   /* goal marker */
    caca_set_color_ansi(gmo.cv, CACA_RED, CACA_BLACK); caca_set_attr(gmo.cv, CACA_BOLD);
    if (tw >= 4 && th >= 2) {
      caca_put_char(gmo.cv, sx + 1, sy, '.');         caca_put_char(gmo.cv, sx + tw - 2, sy, '.');
      caca_put_char(gmo.cv, sx + 1, sy + th - 1, '.'); caca_put_char(gmo.cv, sx + tw - 2, sy + th - 1, '.');
    } else {
      caca_put_char(gmo.cv, sx + tw / 2, sy + th / 2, 'o');
    }
    break;
  case 5: case 6:                           /* player */
    caca_set_color_ansi(gmo.cv, CACA_LIGHTCYAN, CACA_BLACK); caca_set_attr(gmo.cv, CACA_BOLD);
    if (tw >= 4 && th >= 2) {
      caca_put_char(gmo.cv, sx + tw / 2 - 1, sy, 0x2588);   /* head */
      caca_put_char(gmo.cv, sx + tw / 2,     sy, 0x2588);
      for (i = 1; i < tw - 1; i++) caca_put_char(gmo.cv, sx + i, sy + 1, 0x2588); /* body+arms */
    } else {
      for (j = 0; j < th; j++) for (i = 0; i < tw; i++) caca_put_char(gmo.cv, sx + i, sy + j, 0x2588);
    }
    break;
  default: break;                           /* floor: black background */
  }
}

void run_sokoban(void)
{
  int W = caca_get_canvas_width(gmo.cv), H = caca_get_canvas_height(gmo.cv);
  char dir[PATH_MAX];
  int have_dir = sok_find_dir(dir, sizeof dir);
  int idx = 1, moves = 0, running = 1, undo_n = 0;
  sok_level_t L;
  sok_move_t *undo = malloc(sizeof(sok_move_t) * 8192);
  caca_event_t ev;

  if (!undo) return;
  if (!sok_get(have_dir, dir, idx, &L)) { have_dir = 0; sok_get(0, dir, idx, &L); }

  while (running) {
    int tw, th, ox, oy, r, c, k, dr = 0, dc = 0;

    if      (L.w * 4 <= W - 2 && L.h * 2 <= H - 3) { tw = 4; th = 2; }
    else if (L.w * 2 <= W - 2 && L.h     <= H - 3) { tw = 2; th = 1; }
    else                                           { tw = 1; th = 1; }
    ox = (W - L.w * tw) / 2;       if (ox < 0) ox = 0;
    oy = 1 + ((H - 1) - L.h * th) / 2; if (oy < 1) oy = 1;

    game_fill_bg();                         /* solid black background */
    caca_set_color_ansi(gmo.cv, CACA_WHITE, CACA_BLACK); caca_set_attr(gmo.cv, 0);
    caca_printf(gmo.cv, 1, 0, "SOKOBAN  level %d   moves %d   (arrows move, u undo, r reset, n/p level, q quit)%s",
                idx, moves, have_dir ? "" : "   [built-in]");
    for (r = 0; r < L.h; r++)
      for (c = 0; c < L.w; c++) {
        int on_goal = (L.st[r][c] == SOKS_GOAL), kind;
        if (L.st[r][c] == SOKS_WALL)         kind = 1;
        else if (r == L.pr && c == L.pc)     kind = on_goal ? 6 : 5;
        else if (L.box[r][c])                kind = on_goal ? 4 : 3;
        else if (on_goal)                    kind = 2;
        else                                 kind = 0;
        sok_tile(ox + c * tw, oy + r * th, tw, th, kind);
      }
    if (sok_won(&L)) {
      caca_set_color_ansi(gmo.cv, CACA_BLACK, CACA_GREEN); caca_set_attr(gmo.cv, CACA_BOLD);
      caca_printf(gmo.cv, W / 2 - 24, H - 1, "  SOLVED in %d moves!   press n for the next level  ", moves);
    }
    caca_refresh_display(gmo.dp);

    if (!caca_get_event(gmo.dp, CACA_EVENT_KEY_PRESS, &ev, -1)) continue;
    k = caca_get_event_key_ch(&ev);

    if (k == 'q' || k == 'Q' || k == CACA_KEY_ESCAPE) { running = 0; continue; }
    if (k == 'r' || k == 'R') { sok_get(have_dir, dir, idx, &L); moves = 0; undo_n = 0; continue; }
    if (k == 'u' || k == 'U') {
      if (undo_n > 0) {
        sok_move_t m = undo[--undo_n];
        if (m.pushed) { L.box[L.pr + m.dr][L.pc + m.dc] = 0; L.box[L.pr][L.pc] = 1; }
        L.pr -= m.dr; L.pc -= m.dc; if (moves > 0) moves--;
      }
      continue;
    }
    if (k == 'n' || k == 'N') { sok_level_t T; if (sok_get(have_dir, dir, idx + 1, &T)) { idx++; L = T; moves = 0; undo_n = 0; } continue; }
    if (k == 'p' || k == 'P') { sok_level_t T; if (idx > 1 && sok_get(have_dir, dir, idx - 1, &T)) { idx--; L = T; moves = 0; undo_n = 0; } continue; }

    if      (k == CACA_KEY_UP    || k == CACA_KEY_CTRL_P) dr = -1;
    else if (k == CACA_KEY_DOWN  || k == CACA_KEY_CTRL_N) dr = 1;
    else if (k == CACA_KEY_LEFT  || k == CACA_KEY_CTRL_B) dc = -1;
    else if (k == CACA_KEY_RIGHT || k == CACA_KEY_CTRL_F) dc = 1;
    else continue;

    if (sok_won(&L)) continue;               /* level done: wait for n */
    {
      int nr = L.pr + dr, nc = L.pc + dc;
      if (nr < 0 || nr >= L.h || nc < 0 || nc >= L.w) continue;
      if (L.st[nr][nc] == SOKS_WALL) continue;
      if (L.box[nr][nc]) {                    /* push a box */
        int br = nr + dr, bc = nc + dc;
        if (br < 0 || br >= L.h || bc < 0 || bc >= L.w) continue;
        if (L.st[br][bc] == SOKS_WALL || L.box[br][bc]) continue;
        L.box[nr][nc] = 0; L.box[br][bc] = 1;
        L.pr = nr; L.pc = nc;
        if (undo_n < 8192) { undo[undo_n].dr = (signed char)dr; undo[undo_n].dc = (signed char)dc; undo[undo_n].pushed = 1; undo_n++; }
        moves++;
      } else {                                /* just walk */
        L.pr = nr; L.pc = nc;
        if (undo_n < 8192) { undo[undo_n].dr = (signed char)dr; undo[undo_n].dc = (signed char)dc; undo[undo_n].pushed = 0; undo_n++; }
        moves++;
      }
    }
  }
  free(undo);
  gtcaca_redraw();
  caca_refresh_display(gmo.dp);
  snprintf(g_message, sizeof g_message, "Sokoban: level %d", idx);
}


/* ── bird (M-x bird) ────────────────────────────────────────────────────────
 * A one-key flyer in the spirit of Flappy Bird: gravity pulls the bird down
 * every tick and a key beats its wings. The whole game is the gaps — they
 * narrow and crowd together as the score climbs. Original implementation.
 *
 * The bird's height is kept in hundredths of a row so gravity accumulates
 * smoothly instead of snapping a whole row per tick; only the drawing rounds
 * to a cell. */

#define BIRD_MAXPIPES 24
#define BIRD_PIPE_W   2                      /* columns per pipe, for visibility */
/* The dial runs 1..9 and starts at 1 — but 1 is brisk, not the crawl it used to
   be, and 9 is genuinely quick. The whole scale was pulled down: 48ms a frame at
   the bottom, 16ms at the top. */
#define BIRD_SPEED_START 1
#define BIRD_SPEED_MAX   9

typedef struct { int x, gap_y, gap_h, scored; } bird_pipe_t;

static int bird_ox = 0, bird_oy = 0;

static void bird_put(int x, int y, uint8_t colour, uint32_t ch, int bold)
{
  caca_set_color_ansi(gmo.cv, colour, CACA_BLACK);
  caca_set_attr(gmo.cv, bold ? CACA_BOLD : 0);
  caca_put_char(gmo.cv, bird_ox + x, bird_oy + y, ch);
}

/* The bird is three cells wide and two tall — body and beak on its own row,
   a wing above that beats as it climbs and spreads as it falls, which is the
   only animation the game needs to read as alive. Only the body row counts for
   collisions: a hitbox as big as the drawing would feel unfair. */
static void bird_draw(int x, int r, int rising, int top)
{
  if (r - 1 >= top) bird_put(x, r - 1, CACA_WHITE, rising ? '^' : '~', 1);
  bird_put(x - 1, r, CACA_YELLOW,   '(', 1);      /* body  */
  bird_put(x,     r, CACA_WHITE,    'o', 1);      /* eye   */
  bird_put(x + 1, r, CACA_LIGHTRED, '>', 1);      /* beak  */
}

/* A fresh pipe at the right edge: the gap goes anywhere that leaves a lip of
   solid pipe above and below, so the bird always has somewhere to aim. */
static void bird_new_pipe(bird_pipe_t *p, int x, int top, int bottom, int gap_h)
{
  int span = bottom - top + 1;
  int room = span - gap_h - 2;               /* 1 row of pipe top and bottom */
  p->x = x;
  p->gap_h = gap_h;
  p->gap_y = top + 1 + (room > 0 ? rand() % room : 0);
  p->scored = 0;
}

/* Speed is the game's single difficulty dial: it sets the tick, how tight the
   gaps are, and how close together they come. Both the automatic ramp and the
   player's '+' go through here, so they cannot disagree. */
static void bird_apply_speed(int speed, int span, int *tick_ms, int *gap_h, int *spacing, int W)
{
  *tick_ms = 52 - speed * 4;                 /* speed 1 = 48ms … speed 9 = 16ms */
  if (*tick_ms < 16) *tick_ms = 16;          /* a full repaint per tick needs the room */
  /* Gaps are measured from the starting speed, so speed 5 is the honest
     baseline and every step above it genuinely tightens the world. */
  *gap_h   = span / 3 - (speed - BIRD_SPEED_START) / 2;
  if (*gap_h < 4) *gap_h = 4;
  *spacing = W / 3 - (speed - BIRD_SPEED_START);
  if (*spacing < 8) *spacing = 8;
}

void run_bird(void)
{
  int CW = caca_get_canvas_width(gmo.cv);
  int CH = caca_get_canvas_height(gmo.cv);
  int W = CW, H = CH;
  int top = 1, bottom, ground;               /* row 0 is the status line */
  int bird_x, y100, vel100;
  int score = 0, gaps = 0, running = 1, started = 0, dead = 0, autostart = 0;
  int gap_h, spacing, tick_ms = 48, speed = BIRD_SPEED_START, span;
  bird_pipe_t pipes[BIRD_MAXPIPES];
  int npipe = 0, i, x, y;
  static int best = 0;                       /* best of this session */
  caca_event_t ev;

  if (W < 24 || H < 10) { snprintf(g_message, sizeof g_message, "Window too small for bird"); return; }
  bird_ox = 0; bird_oy = 0;
  ground = H - 1;
  bottom = ground - 1;                       /* last flyable row */
  bird_x = W / 4;
  srand((unsigned)time(NULL));

  /* start screen */
  {
    int chosen = 0;
    game_fill_bg();
    caca_set_color_ansi(gmo.cv, CACA_YELLOW, CACA_BLACK); caca_set_attr(gmo.cv, CACA_BOLD);
    caca_printf(gmo.cv, W / 2 - 4, H / 2 - 3, "B I R D");
    caca_set_color_ansi(gmo.cv, CACA_WHITE, CACA_BLACK); caca_set_attr(gmo.cv, 0);
    caca_printf(gmo.cv, W / 2 - 20, H / 2 - 1, "Space or Up flaps — that is the whole game.");
    caca_printf(gmo.cv, W / 2 - 20, H / 2 + 0, "Fly through the gaps. Every 5 gaps the world speeds");
    caca_printf(gmo.cv, W / 2 - 20, H / 2 + 1, "up on its own; + speeds it up now, and there is no");
    caca_printf(gmo.cv, W / 2 - 20, H / 2 + 2, "way back down. A gap pays its speed in points.");
    caca_printf(gmo.cv, W / 2 - 20, H / 2 + 4, "Speed 1 is already brisk; 9 is as fast as it gets.");
    caca_printf(gmo.cv, W / 2 - 20, H / 2 + 5, "Space starts.  p pauses, q quits.");
    caca_refresh_display(gmo.dp);
    while (!chosen) {
      if (caca_get_event(gmo.dp, CACA_EVENT_KEY_PRESS, &ev, -1)) {
        int k = caca_get_event_key_ch(&ev);
        if (k == 'q' || k == 'Q' || k == CACA_KEY_ESCAPE) { gtcaca_redraw(); return; }
        /* Space here means "go" — asking for it twice (once to leave the title,
           once to take off) reads as the game ignoring the first press. */
        if (k == ' ' || k == CACA_KEY_UP || k == 'w' || k == 'W') autostart = 1;
        chosen = 1;
      }
    }
  }

restart:
  span = bottom - top + 1;
  speed = BIRD_SPEED_START;
  bird_apply_speed(speed, span, &tick_ms, &gap_h, &spacing, W);
  y100 = ((top + bottom) / 2) * 100;
  vel100 = 0;
  score = 0; gaps = 0; npipe = 0; started = 0; dead = 0;
  bird_new_pipe(&pipes[npipe++], W - 1, top, bottom, gap_h);
  if (autostart) { started = 1; vel100 = -115; autostart = 0; }

  while (running) {
    int r = y100 / 100;

    /* ── draw ── */
    game_fill_bg();
    caca_set_color_ansi(gmo.cv, CACA_WHITE, CACA_BLACK); caca_set_attr(gmo.cv, 0);
    caca_printf(gmo.cv, 1, 0, "BIRD   score %d   best %d   speed %d/9   (space/Up flap, + faster, p pause, q quit)",
                score, best, speed);

    for (i = 0; i < npipe; i++) {             /* pipes, drawn as solid blocks */
      for (x = pipes[i].x; x < pipes[i].x + BIRD_PIPE_W; x++) {
        if (x < 0 || x >= W) continue;
        for (y = top; y <= bottom; y++)
          if (y < pipes[i].gap_y || y >= pipes[i].gap_y + pipes[i].gap_h)
            bird_put(x, y, CACA_GREEN, 0x2588, 0);   /* █ */
      }
    }
    for (x = 0; x < W; x++) bird_put(x, ground, CACA_BROWN, 0x2588, 0);
    bird_draw(bird_x, r, vel100 < 0, top);

    if (!started) {
      caca_set_color_ansi(gmo.cv, CACA_WHITE, CACA_BLACK); caca_set_attr(gmo.cv, CACA_BOLD);
      caca_printf(gmo.cv, W / 2 - 12, top + 1, " press space to fly ");
    }
    caca_refresh_display(gmo.dp);

    /* ── input; the timeout is what makes gravity tick (microseconds) ── */
    if (caca_get_event(gmo.dp, CACA_EVENT_KEY_PRESS, &ev, started ? tick_ms * 1000 : -1)) {
      int k = caca_get_event_key_ch(&ev);
      if (k == 'q' || k == 'Q' || k == CACA_KEY_ESCAPE) { running = 0; continue; }
      if (k == 'p' || k == 'P') {
        caca_set_color_ansi(gmo.cv, CACA_WHITE, CACA_BLACK); caca_set_attr(gmo.cv, CACA_BOLD);
        caca_printf(gmo.cv, W / 2 - 5, top + 1, " paused ");
        caca_refresh_display(gmo.dp);
        caca_get_event(gmo.dp, CACA_EVENT_KEY_PRESS, &ev, -1);
        continue;
      }
      if (k == '+' || k == '=') {           /* faster, and worth more — no way back */
        if (speed < BIRD_SPEED_MAX) {
          speed++;
          bird_apply_speed(speed, span, &tick_ms, &gap_h, &spacing, W);
        }
        continue;
      }
      if (k == ' ' || k == CACA_KEY_UP || k == 'w' || k == 'W' || k == CACA_KEY_CTRL_P) {
        started = 1;
        vel100 = -115;                        /* a beat cancels the fall outright */
      }
    }
    if (!started) continue;

    /* ── gravity ── */
    vel100 += 20;
    if (vel100 > 140) vel100 = 140;           /* terminal velocity, so a dive stays readable */
    y100 += vel100;
    if (y100 < top * 100) { y100 = top * 100; vel100 = 0; }   /* the ceiling holds it, not kills it */
    r = y100 / 100;

    /* ── scroll the pipes, spawn and retire ── */
    for (i = 0; i < npipe; i++) pipes[i].x--;
    if (npipe && pipes[0].x + BIRD_PIPE_W < 0) {              /* the leftmost left the screen */
      for (i = 1; i < npipe; i++) pipes[i - 1] = pipes[i];
      npipe--;
    }
    if (npipe < BIRD_MAXPIPES && (npipe == 0 || pipes[npipe - 1].x <= W - 1 - spacing))
      bird_new_pipe(&pipes[npipe++], W - 1, top, bottom, gap_h);

    /* ── score, and tighten the game as it goes ── */
    for (i = 0; i < npipe; i++)
      if (!pipes[i].scored && pipes[i].x + BIRD_PIPE_W < bird_x) {
        pipes[i].scored = 1;
        score += speed;                       /* the faster it is, the more a gap pays */
        gaps++;
        if (score > best) best = score;
        if (gaps % 5 == 0 && speed < BIRD_SPEED_MAX) {   /* it also speeds up on its own */
          speed++;
          bird_apply_speed(speed, span, &tick_ms, &gap_h, &spacing, W);
        }
      }

    /* CCM_BIRDLOG=1 traces the physics — the same trick CCM_EVLOG and
       CCM_DGMLOG use, and the only way to check a real-time game from a test
       without racing its repaints. */
    if (getenv("CCM_BIRDLOG")) {
      int aim = -1;                          /* centre of the next gap ahead */
      for (i = 0; i < npipe; i++)
        if (pipes[i].x + BIRD_PIPE_W >= bird_x) { aim = pipes[i].gap_y + pipes[i].gap_h / 2; break; }
      fprintf(stderr, "[bird] r=%d vel=%d speed=%d gap_h=%d score=%d pipes=%d aim=%d\n",
              r, vel100, speed, gap_h, score, npipe, aim);
    }

    /* ── collide ── */
    if (r >= ground) dead = 1;
    for (i = 0; i < npipe && !dead; i++)
      if (bird_x + 1 >= pipes[i].x && bird_x - 1 < pipes[i].x + BIRD_PIPE_W &&
          (r < pipes[i].gap_y || r >= pipes[i].gap_y + pipes[i].gap_h))
        dead = 1;

    if (dead) {
      if (getenv("CCM_BIRDLOG")) fprintf(stderr, "[bird] dead at r=%d score=%d\n", r, score);
      caca_set_color_ansi(gmo.cv, CACA_WHITE, CACA_RED); caca_set_attr(gmo.cv, CACA_BOLD);
      caca_printf(gmo.cv, W / 2 - 22, H / 2, "  DOWN  -  score %d  -  best %d  -  r restart, q quit  ", score, best);
      caca_refresh_display(gmo.dp);
      for (;;) {
        caca_get_event(gmo.dp, CACA_EVENT_KEY_PRESS, &ev, -1);
        { int k = caca_get_event_key_ch(&ev);
          if (k == 'r' || k == 'R') goto restart;
          if (k == 'q' || k == 'Q' || k == CACA_KEY_ESCAPE) { running = 0; break; } }
      }
    }
  }

  snprintf(g_message, sizeof g_message, "Bird: final score %d (best %d)", score, best);
  gtcaca_redraw();
  caca_refresh_display(gmo.dp);
}


/* ── wordguess (M-x wordguess) ──────────────────────────────────────────────
 * Letters are drawn from a 5x5 bitmap font, a cell to the pixel, in tiles
 * five columns wide. 
 *
 * The answer comes from a built-in list of everyday words, so the game stays
 * winnable; `M-x wordguess-wild` draws it from the whole of the system
 * dictionary instead, obscurities and all. A guess only has to be a word: the
 * built-in list, or anything M-$ would spell-check as correct. */

#define WG_ROWS 6
#define WG_COLS 5
#define WG_DICT_PATH "/usr/share/dict/words"

enum { WG_EMPTY = 0, WG_PENDING, WG_ABSENT, WG_PRESENT, WG_CORRECT };

/* 5x5 glyphs, a..z, one string per pixel row; '#' is ink. */
static const char *const wg_font[26][5] = {
  { ".###.", "#...#", "#####", "#...#", "#...#" },   /* a */
  { "####.", "#...#", "####.", "#...#", "####." },   /* b */
  { ".####", "#....", "#....", "#....", ".####" },   /* c */
  { "####.", "#...#", "#...#", "#...#", "####." },   /* d */
  { "#####", "#....", "####.", "#....", "#####" },   /* e */
  { "#####", "#....", "####.", "#....", "#...." },   /* f */
  { ".####", "#....", "#..##", "#...#", ".###." },   /* g */
  { "#...#", "#...#", "#####", "#...#", "#...#" },   /* h */
  { "#####", "..#..", "..#..", "..#..", "#####" },   /* i */
  { "..###", "...#.", "...#.", "#..#.", ".##.." },   /* j */
  { "#...#", "#..#.", "###..", "#..#.", "#...#" },   /* k */
  { "#....", "#....", "#....", "#....", "#####" },   /* l */
  { "#...#", "##.##", "#.#.#", "#...#", "#...#" },   /* m */
  { "#...#", "##..#", "#.#.#", "#..##", "#...#" },   /* n */
  { ".###.", "#...#", "#...#", "#...#", ".###." },   /* o */
  { "####.", "#...#", "####.", "#....", "#...." },   /* p */
  { ".###.", "#...#", "#.#.#", "#..#.", ".##.#" },   /* q */
  { "####.", "#...#", "####.", "#..#.", "#...#" },   /* r */
  { ".####", "#....", ".###.", "....#", "####." },   /* s */
  { "#####", "..#..", "..#..", "..#..", "..#.." },   /* t */
  { "#...#", "#...#", "#...#", "#...#", ".###." },   /* u */
  { "#...#", "#...#", "#...#", ".#.#.", "..#.." },   /* v */
  { "#...#", "#...#", "#.#.#", "##.##", "#...#" },   /* w */
  { "#...#", ".#.#.", "..#..", ".#.#.", "#...#" },   /* x */
  { "#...#", ".#.#.", "..#..", "..#..", "..#.." },   /* y */
  { "#####", "...#.", "..#..", ".#...", "#####" }    /* z */
};

/* Everyday five-letter words: the pool answers are drawn from, and always
   accepted as a guess even when there is no system dictionary to check
   against (Windows has none). */
static const char *const wg_common[] = {
  "about", "above", "abuse", "actor", "acute", "admit", "adopt", "adult", "after", "again",
  "agent", "agree", "ahead", "alarm", "album", "alert", "alike", "alive", "allow", "alone",
  "along", "alter", "among", "anger", "angle", "angry", "apart", "apple", "apply", "arena",
  "argue", "arise", "armor", "array", "arrow", "aside", "asset", "audio", "audit", "avoid",
  "awake", "award", "aware", "badly", "baker", "basic", "basis", "beach", "beard", "beast",
  "begin", "being", "below", "bench", "birth", "black", "blade", "blame", "blank", "blast",
  "blend", "bless", "blind", "block", "blood", "board", "boast", "bonus", "boost", "booth",
  "bound", "brain", "brand", "brave", "bread", "break", "breed", "brick", "bride", "brief",
  "bring", "broad", "broke", "brown", "brush", "build", "built", "bunch", "burnt", "burst",
  "buyer", "cabin", "cable", "camel", "canal", "candy", "cargo", "carry", "carve", "catch",
  "cause", "cease", "chain", "chair", "chalk", "charm", "chart", "chase", "cheap", "cheat",
  "check", "cheek", "cheer", "chess", "chest", "chief", "child", "chill", "china", "choir",
  "chose", "chunk", "cider", "civil", "claim", "clash", "class", "clean", "clear", "clerk",
  "click", "cliff", "climb", "clock", "close", "cloth", "cloud", "clown", "coach", "coast",
  "cobra", "color", "comic", "coral", "couch", "cough", "could", "count", "court", "cover",
  "crack", "craft", "crane", "crash", "crawl", "crazy", "cream", "creek", "crest", "crime",
  "crisp", "cross", "crowd", "crown", "crude", "cruel", "crush", "crust", "curve", "cycle",
  "daily", "dairy", "dance", "dated", "dealt", "death", "debut", "decay", "decor", "delay",
  "delta", "dense", "depth", "devil", "diary", "dirty", "ditch", "diver", "dizzy", "dodge",
  "doing", "donor", "doubt", "dozen", "draft", "drain", "drama", "drank", "dream", "dress",
  "dried", "drift", "drill", "drink", "drive", "drove", "drown", "drunk", "dryer", "eager",
  "eagle", "early", "earth", "eight", "elbow", "elder", "elect", "elite", "empty", "enemy",
  "enjoy", "enter", "entry", "equal", "error", "essay", "event", "every", "exact", "exile",
  "exist", "extra", "fable", "faint", "fairy", "faith", "false", "fancy", "fatal", "fault",
  "favor", "feast", "fence", "fever", "fewer", "fiber", "field", "fiery", "fifth", "fifty",
  "fight", "final", "first", "flame", "flash", "fleet", "flesh", "flick", "fling", "flint",
  "float", "flock", "flood", "floor", "flour", "fluid", "flush", "focal", "focus", "force",
  "forge", "forth", "forty", "forum", "found", "frame", "fraud", "fresh", "fried", "front",
  "frost", "fruit", "fully", "funny", "giant", "giver", "glass", "gleam", "globe", "gloom",
  "glory", "glove", "going", "grace", "grade", "grain", "grand", "grant", "grape", "graph",
  "grasp", "grass", "grave", "great", "greed", "green", "greet", "grief", "grill", "grind",
  "groan", "groom", "group", "grove", "growl", "guard", "guess", "guest", "guide", "guilt",
  "habit", "handy", "happy", "harsh", "haunt", "heart", "heavy", "hedge", "hello", "hence",
  "hobby", "honey", "honor", "horse", "hotel", "house", "hover", "human", "humor", "hurry",
  "ideal", "image", "imply", "index", "inner", "input", "irony", "issue", "ivory", "jelly",
  "jewel", "joint", "jolly", "judge", "juice", "jumbo", "kneel", "knife", "knock", "known",
  "label", "labor", "lance", "large", "laser", "later", "laugh", "layer", "learn", "lease",
  "least", "leave", "legal", "lemon", "level", "lever", "light", "limit", "linen", "liner",
  "liver", "lobby", "local", "lodge", "logic", "loose", "loser", "lover", "lower", "loyal",
  "lucky", "lunar", "lunch", "lying", "magic", "major", "maker", "mango", "maple", "march",
  "marsh", "match", "mayor", "meant", "medal", "media", "mercy", "merge", "merit", "metal",
  "meter", "midst", "might", "minor", "minus", "mixed", "model", "moist", "money", "month",
  "moral", "motor", "mount", "mouse", "mouth", "movie", "music", "naive", "nasty", "naval",
  "nerve", "never", "newly", "night", "noble", "noise", "north", "notch", "novel", "nurse",
  "ocean", "offer", "often", "olive", "onion", "opera", "orbit", "order", "organ", "other",
  "otter", "ought", "ounce", "outer", "owner", "ozone", "paint", "panel", "panic", "paper",
  "party", "pasta", "patch", "pause", "peace", "peach", "pearl", "pedal", "penny", "perch",
  "phase", "phone", "photo", "piano", "piece", "pilot", "pinch", "pitch", "pivot", "pixel",
  "pizza", "place", "plain", "plane", "plant", "plate", "plaza", "pluck", "point", "polar",
  "porch", "pound", "power", "press", "price", "pride", "prime", "print", "prior", "prize",
  "probe", "prone", "proof", "proud", "prove", "prune", "pulse", "punch", "pupil", "puppy",
  "purse", "queen", "query", "quest", "queue", "quick", "quiet", "quilt", "quite", "quota",
  "radar", "radio", "raise", "rally", "ranch", "range", "rapid", "ratio", "razor", "reach",
  "react", "ready", "realm", "rebel", "refer", "reign", "relax", "relay", "renew", "repay",
  "reply", "rider", "ridge", "rifle", "right", "rigid", "rinse", "risen", "risky", "rival",
  "river", "roast", "robot", "rocky", "rogue", "rotor", "rough", "round", "route", "royal",
  "rugby", "ruler", "rumor", "rural", "sadly", "saint", "salad", "salon", "sauce", "scale",
  "scare", "scarf", "scene", "scent", "scope", "score", "scout", "scrap", "screw", "sense",
  "serve", "seven", "shade", "shaft", "shake", "shall", "shame", "shape", "share", "shark",
  "sharp", "sheep", "sheet", "shelf", "shell", "shift", "shine", "shirt", "shock", "shoot",
  "shore", "short", "shout", "shown", "shrug", "siege", "sight", "sigma", "silly", "since",
  "siren", "sixth", "skill", "skirt", "skull", "slate", "sleep", "slice", "slide", "slope",
  "small", "smart", "smash", "smell", "smile", "smoke", "snack", "snake", "sneak", "solar",
  "solid", "solve", "sorry", "sound", "south", "space", "spare", "spark", "speak", "speed",
  "spell", "spend", "spent", "spice", "spike", "spill", "spine", "spite", "split", "spoil",
  "spoke", "spoon", "sport", "spray", "squad", "stack", "staff", "stage", "stain", "stair",
  "stake", "stall", "stamp", "stand", "stare", "start", "state", "steak", "steal", "steam",
  "steel", "steep", "steer", "stern", "stick", "stiff", "still", "sting", "stock", "stone",
  "stood", "stool", "store", "storm", "story", "stove", "strap", "straw", "strip", "stuck",
  "study", "stuff", "style", "sugar", "suite", "sunny", "super", "surge", "sweat", "sweep",
  "sweet", "swift", "swing", "sword", "syrup", "table", "taken", "tally", "tango", "taste",
  "teach", "tempo", "tenth", "thank", "theft", "their", "theme", "there", "these", "thick",
  "thief", "thigh", "thing", "think", "third", "those", "three", "throw", "thumb", "tiger",
  "tight", "timer", "tired", "title", "toast", "today", "token", "tonic", "tooth", "topic",
  "torch", "total", "touch", "tough", "towel", "tower", "toxic", "trace", "track", "trade",
  "trail", "train", "trait", "trash", "treat", "trend", "trial", "tribe", "trick", "tried",
  "trout", "truck", "truly", "trunk", "trust", "truth", "tulip", "tumor", "tutor", "twice",
  "twist", "ultra", "uncle", "under", "union", "unite", "unity", "until", "upper", "upset",
  "urban", "usage", "usual", "vague", "valid", "value", "valve", "vapor", "vault", "venue",
  "verse", "video", "vigor", "villa", "vinyl", "viral", "virus", "visit", "vital", "vivid",
  "vocal", "vodka", "voice", "voter", "wagon", "waist", "waste", "watch", "water", "weary",
  "weave", "wedge", "weigh", "weird", "whale", "wheat", "wheel", "where", "which", "while",
  "white", "whole", "whose", "widow", "width", "witch", "woman", "world", "worry", "worse",
  "worst", "worth", "would", "wound", "wrist", "write", "wrong", "yacht", "yeast", "yield",
  "young", "yours", "youth", "zebra"
};
#define WG_NCOMMON ((int)(sizeof wg_common / sizeof *wg_common))

/* ── word lists ─────────────────────────────────────────────────────────────
 * Guesses are checked by the spell checker (see wg_is_word). What is read here
 * is only the pool a *wild* answer is drawn from: the system dictionary
 * filtered down to its five-letter lowercase words, read once and kept for the
 * session. It is sorted here rather than trusted to be sorted — the file's own
 * order is case-insensitive and varies between systems. */
static char **wg_dict = NULL;
static int    wg_dict_n = 0;
static int    wg_dict_tried = 0;

static int wg_cmp(const void *a, const void *b)
{
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void wg_load_dict(void)
{
  FILE *f;
  char line[128];
  int cap = 0;

  if (wg_dict_tried) return;
  wg_dict_tried = 1;

  f = fopen(WG_DICT_PATH, "r");
  if (!f) return;
  while (fgets(line, sizeof line, f)) {
    char *w;
    int n = 0;
    while (line[n] && line[n] >= 'a' && line[n] <= 'z') n++;
    if (n != WG_COLS || (line[n] && line[n] != '\n' && line[n] != '\r')) continue;
    if (wg_dict_n == cap) {
      int ncap = cap ? cap * 2 : 1024;
      char **t = realloc(wg_dict, sizeof *t * (size_t)ncap);
      if (!t) break;
      wg_dict = t; cap = ncap;
    }
    w = malloc(WG_COLS + 1);
    if (!w) break;
    memcpy(w, line, WG_COLS); w[WG_COLS] = '\0';
    wg_dict[wg_dict_n++] = w;
  }
  fclose(f);
  if (wg_dict_n) qsort(wg_dict, (size_t)wg_dict_n, sizeof *wg_dict, wg_cmp);
}

/* Is `w` a word we accept as a guess? The built-in list first — it has the
   everyday words web2 is missing (pasta, pixel, rugby) — and then the spell
   checker, which is the same question M-$ answers and knows that a base-word
   dictionary holding "lawn" but not "lawns" still makes "lawns" a word. */
static int wg_is_word(const char *w)
{
  int lo = 0, hi = WG_NCOMMON - 1;
  while (lo <= hi) {                                   /* wg_common is sorted */
    int mid = (lo + hi) / 2, c = strcmp(wg_common[mid], w);
    if (!c) return 1;
    if (c < 0) lo = mid + 1; else hi = mid - 1;
  }
  return ccm_word_is_correct(w);
}

/* the answer: an everyday word, or - wild - anything the dictionary holds */
static void wg_pick(char *out, int wild)
{
  const char *w;
  if (wild && wg_dict_n) w = wg_dict[rand() % wg_dict_n];
  else                   w = wg_common[rand() % WG_NCOMMON];
  memcpy(out, w, WG_COLS); out[WG_COLS] = '\0';
}

/* ── drawing ────────────────────────────────────────────────────────────────
 * Every cell of the board is a full block █ drawn in its own colour on that
 * same colour, so it comes out solid however the display renders the glyph:
 * libcaca's ncurses driver has its own idea of what a block element looks
 * like and substitutes ▮ for every one of them. Painting a space on a coloured
 * background would come out solid too, but some drivers do not repaint a cell
 * whose only change is its background — hence a real glyph. */

typedef struct {
  int tw, th;        /* tile size, in cells */
  int rows;          /* rows of the font a letter gets; 0 = one character */
  int vgap;          /* blank rows between one row of tiles and the next */
  int ox, oy;        /* top-left corner of the board */
  int kbd, ky;       /* draw the on-screen keyboard, and the row it starts on */
  int msgy;          /* the row messages are written on */
  int cx;            /* centre column of the window */
} wg_layout_t;

static void wg_fill(int x, int y, int w, int h, uint8_t colour)
{
  int i, j;
  caca_set_color_ansi(gmo.cv, colour, colour);
  caca_set_attr(gmo.cv, 0);
  for (j = 0; j < h; j++)
    for (i = 0; i < w; i++)
      caca_put_char(gmo.cv, x + i, y + j, 0x2588);          /* █ */
}

/* Empty slot: a thin frame */
static void wg_frame(int x, int y, int w, int h, uint8_t colour)
{
  int i, j;
  caca_set_color_ansi(gmo.cv, colour, CACA_BLACK);
  caca_set_attr(gmo.cv, 0);
  for (i = 1; i < w - 1; i++) {
    caca_put_char(gmo.cv, x + i, y, 0x2500);                /* ─ */
    caca_put_char(gmo.cv, x + i, y + h - 1, 0x2500);
  }
  for (j = 1; j < h - 1; j++) {
    caca_put_char(gmo.cv, x, y + j, 0x2502);                /* │ */
    caca_put_char(gmo.cv, x + w - 1, y + j, 0x2502);
  }
  caca_put_char(gmo.cv, x,         y,         0x250C);      /* ┌ */
  caca_put_char(gmo.cv, x + w - 1, y,         0x2510);      /* ┐ */
  caca_put_char(gmo.cv, x,         y + h - 1, 0x2514);      /* └ */
  caca_put_char(gmo.cv, x + w - 1, y + h - 1, 0x2518);      /* ┘ */
}

/* one glyph, a cell to the pixel. `rows` is 5, or 4 to leave out the font's
   fourth row — the crossbar sits above it, so it is the row a letter can lose
   and still be read. */
static void wg_glyph(int x, int y, int ch, int rows, uint8_t ink, uint8_t bg)
{
  const char *const *g = wg_font[ch - 'a'];
  int col, row;
  caca_set_attr(gmo.cv, 0);
  for (row = 0; row < rows; row++) {
    const char *px = g[rows == 5 ? row : (row < 3 ? row : 4)];
    for (col = 0; col < WG_COLS; col++) {
      uint8_t c = px[col] == '#' ? ink : bg;
      caca_set_color_ansi(gmo.cv, c, c);
      caca_put_char(gmo.cv, x + col, y + row, 0x2588);      /* █ */
    }
  }
}

static void wg_colours(int state, uint8_t *bg, uint8_t *ink)
{
  switch (state) {
  case WG_CORRECT: *bg = CACA_GREEN;    *ink = CACA_WHITE; break;
  case WG_PRESENT: *bg = CACA_BROWN;    *ink = CACA_WHITE; break;
  case WG_ABSENT:  *bg = CACA_DARKGRAY; *ink = CACA_WHITE; break;
  default:         *bg = CACA_BLACK;    *ink = CACA_WHITE; break;   /* typed, not yet guessed */
  }
}

static void wg_tile(const wg_layout_t *L, int r, int c, int ch, int state)
{
  int x = L->ox + c * (L->tw + 1);
  int y = L->oy + r * (L->th + L->vgap);
  uint8_t bg, ink;

  if (!ch) {                                        /* an empty slot: an outline */
    if (L->th > 1) wg_frame(x, y, L->tw, L->th, CACA_DARKGRAY);
    else {
      caca_set_color_ansi(gmo.cv, CACA_DARKGRAY, CACA_BLACK);
      caca_set_attr(gmo.cv, 0);
      caca_put_char(gmo.cv, x + L->tw / 2, y, '.');
    }
    return;
  }
  wg_colours(state, &bg, &ink);
  wg_fill(x, y, L->tw, L->th, bg);
  if (L->rows) {
    wg_glyph(x + (L->tw - WG_COLS) / 2, y, ch, L->rows, ink, bg);
  } else {
    caca_set_color_ansi(gmo.cv, ink, bg);
    caca_set_attr(gmo.cv, CACA_BOLD);
    caca_put_char(gmo.cv, x + L->tw / 2, y, (uint32_t)(ch - 'a' + 'A'));
  }
}

static const char *const wg_keyrows[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };

/* the keyboard, coloured with what has been ruled in or out so far */
static void wg_draw_keyboard(const wg_layout_t *L, const char *seen)
{
  int kw = L->rows ? 3 : 1, row, i;
  for (row = 0; row < 3; row++) {
    int n = (int)strlen(wg_keyrows[row]);
    int x = L->cx - (n * (kw + 1) - 1) / 2;
    for (i = 0; i < n; i++, x += kw + 1) {
      int ch = wg_keyrows[row][i];
      uint8_t bg, ink;
      switch (seen[ch - 'a']) {
      case WG_CORRECT: bg = CACA_GREEN;     ink = CACA_BLACK; break;
      case WG_PRESENT: bg = CACA_BROWN;     ink = CACA_BLACK; break;
      case WG_ABSENT:  bg = CACA_DARKGRAY;  ink = CACA_LIGHTGRAY; break;
      default:         bg = CACA_LIGHTGRAY; ink = CACA_BLACK; break;
      }
      wg_fill(x, L->ky + row, kw, 1, bg);
      caca_set_color_ansi(gmo.cv, ink, bg);
      caca_set_attr(gmo.cv, CACA_BOLD);
      caca_put_char(gmo.cv, x + kw / 2, L->ky + row, (uint32_t)(ch - 'a' + 'A'));
    }
  }
}

/* ── scoring ────────────────────────────────────────────────────────────────
 * Two passes, because a letter of the answer can only be claimed once: the
 * greens are taken first, and only what is left over can turn a later
 * duplicate amber. Guessing "sassy" against "basil" greens the middle S and
 * greys the other two — the answer's only S has already been claimed. */
static void wg_score(const char *guess, const char *answer, char *out)
{
  int used[WG_COLS] = { 0 }, i, j;
  for (i = 0; i < WG_COLS; i++)
    if (guess[i] == answer[i]) { out[i] = WG_CORRECT; used[i] = 1; }
    else                         out[i] = WG_ABSENT;
  for (i = 0; i < WG_COLS; i++) {
    if (out[i] == WG_CORRECT) continue;
    for (j = 0; j < WG_COLS; j++)
      if (!used[j] && guess[i] == answer[j]) { out[i] = WG_PRESENT; used[j] = 1; break; }
  }
}

/* the biggest board that fits, and where everything around it goes. The sizes
   are tried in order: five-row letters, four-row letters, four-row letters with
   the rows of tiles touching, then one character to a tile. */
static int wg_layout(wg_layout_t *L, int W, int H)
{
  static const struct { int rows, th, tw, vgap; } sizes[] = {
    { 5, 5, 7, 1 }, { 4, 4, 7, 1 }, { 4, 4, 7, 0 }, { 0, 1, 3, 1 }
  };
  int i;

  L->cx = W / 2;
  for (i = 0; i < (int)(sizeof sizes / sizeof *sizes); i++) {
    int bw = WG_COLS * (sizes[i].tw + 1) - 1;
    int bh = WG_ROWS * (sizes[i].th + sizes[i].vgap) - sizes[i].vgap;
    if (W < bw + 2 || H < bh + 3) continue;        /* + a title, a gap, a message */
    L->rows = sizes[i].rows; L->th = sizes[i].th;
    L->tw = sizes[i].tw;     L->vgap = sizes[i].vgap;
    L->ox = L->cx - bw / 2;
    L->kbd = H >= bh + 7;                          /* room for three rows of keys */
    L->oy = 2 + ((H - 2) - bh - (L->kbd ? 4 : 0) - 1) / 2;
    if (L->oy < 2) L->oy = 2;
    L->ky = L->oy + bh + 2;
    L->msgy = L->kbd ? L->ky + 3 : L->oy + bh + 1;
    if (L->msgy > H - 1) L->msgy = H - 1;
    return 1;
  }
  return 0;
}

static void wg_centre(int y, uint8_t fg, uint8_t bg, int bold, int cx, const char *s)
{
  caca_set_color_ansi(gmo.cv, fg, bg);
  caca_set_attr(gmo.cv, bold ? CACA_BOLD : 0);
  caca_printf(gmo.cv, cx - (int)strlen(s) / 2, y, "%s", s);
}

/* how well you did, in the spirit of the original */
static const char *wg_praise(int tries)
{
  static const char *const p[WG_ROWS] =
    { "Genius", "Magnificent", "Impressive", "Splendid", "Great", "Phew" };
  return p[tries - 1];
}

void run_wordguess(int wild)
{
  int W = caca_get_canvas_width(gmo.cv), H = caca_get_canvas_height(gmo.cv);
  static int seeded = 0;
  wg_layout_t L;
  char answer[WG_COLS + 1];
  char guess[WG_ROWS][WG_COLS + 1];
  char mark[WG_ROWS][WG_COLS];
  char seen[26];
  char msg[80];
  int row = 0, col = 0, over = 0, won = 0, running = 1, played = 0, wins = 0;
  caca_event_t ev;

  if (!wg_layout(&L, W, H)) {
    snprintf(g_message, sizeof g_message, "Window too small for wordguess");
    return;
  }
  if (!seeded) { srand((unsigned)time(NULL)); seeded = 1; }
  if (wild) wg_load_dict();

new_game:
  memset(guess, 0, sizeof guess);
  memset(mark, 0, sizeof mark);
  memset(seen, WG_EMPTY, sizeof seen);
  row = col = over = won = 0;
  msg[0] = '\0';
  {
    /* CCM_WORDGUESS fixes the answer, so a test can play a whole game — the
       same trick CCM_EVLOG and CCM_BIRDLOG use to make a game reproducible. */
    const char *forced = getenv("CCM_WORDGUESS");
    if (forced && strlen(forced) == WG_COLS) {
      memcpy(answer, forced, WG_COLS); answer[WG_COLS] = '\0';
    } else wg_pick(answer, wild);
  }

  while (running) {
    int r, c, k, nw = caca_get_canvas_width(gmo.cv), nh = caca_get_canvas_height(gmo.cv);

    if (nw != W || nh != H) {                 /* the window was resized under us */
      wg_layout_t n;
      if (wg_layout(&n, nw, nh)) { L = n; W = nw; H = nh; }
    }

    /* ── draw ── */
    game_fill_bg();
    caca_set_color_ansi(gmo.cv, CACA_WHITE, CACA_BLACK);
    caca_set_attr(gmo.cv, 0);
    caca_printf(gmo.cv, 1, 0, "WORDGUESS%s   try %d/%d   won %d/%d   (type a word, Enter guesses, Esc quits)",
                wild ? " (wild)" : "", over ? row : row + 1, WG_ROWS, wins, played);

    for (r = 0; r < WG_ROWS; r++)
      for (c = 0; c < WG_COLS; c++) {
        int ch = guess[r][c];
        int state = r < row ? mark[r][c] : WG_PENDING;
        wg_tile(&L, r, c, ch, state);
      }
    if (L.kbd) wg_draw_keyboard(&L, seen);

    if (over) {
      char line[80];
      /* ASCII only: the ncurses driver has no glyph for an em dash and draws
         a question mark in its place. */
      if (won) snprintf(line, sizeof line, " %s in %d/%d  (Enter plays again, q quits) ",
                        wg_praise(row), row, WG_ROWS);
      else     snprintf(line, sizeof line, " The word was %s  (Enter plays again, q quits) ", answer);
      if ((int)strlen(line) > W) {                /* narrow window: say it shorter */
        if (won) snprintf(line, sizeof line, " %s %d/%d  (Enter, q) ", wg_praise(row), row, WG_ROWS);
        else     snprintf(line, sizeof line, " It was %s  (Enter, q) ", answer);
      }
      wg_centre(L.msgy, won ? CACA_BLACK : CACA_WHITE, won ? CACA_GREEN : CACA_RED, 1, L.cx, line);
    } else if (msg[0]) {
      wg_centre(L.msgy, CACA_YELLOW, CACA_BLACK, 1, L.cx, msg);
    }
    caca_refresh_display(gmo.dp);

    /* ── input ── */
    if (!caca_get_event(gmo.dp, CACA_EVENT_KEY_PRESS, &ev, -1)) continue;
    k = caca_get_event_key_ch(&ev);
    if (k >= 'A' && k <= 'Z') k = k - 'A' + 'a';
    msg[0] = '\0';

    if (k == CACA_KEY_ESCAPE || k == CACA_KEY_CTRL_G) break;

    if (over) {
      if (k == 'q') break;
      if (k == '\n' || k == CACA_KEY_RETURN || k == ' ' || k == 'n') goto new_game;
      continue;
    }

    if (k >= 'a' && k <= 'z') {
      if (col < WG_COLS) guess[row][col++] = (char)k;
      continue;
    }
    if (k == CACA_KEY_BACKSPACE || k == CACA_KEY_DELETE) {
      if (col > 0) guess[row][--col] = '\0';
      continue;
    }
    if (k != '\n' && k != CACA_KEY_RETURN) continue;

    /* ── a guess ── */
    if (col < WG_COLS) { snprintf(msg, sizeof msg, " Not enough letters "); continue; }
    if (!wg_is_word(guess[row])) {
      snprintf(msg, sizeof msg, " %s is not a word I know ", guess[row]);
      continue;
    }
    wg_score(guess[row], answer, mark[row]);
    for (c = 0; c < WG_COLS; c++) {                  /* the keyboard only ever improves */
      int i = guess[row][c] - 'a';
      if (mark[row][c] > seen[i]) seen[i] = mark[row][c];
    }
    won = !strcmp(guess[row], answer);
    row++; col = 0;
    if (won || row == WG_ROWS) { over = 1; played++; if (won) wins++; }
  }

  if (played)
    snprintf(g_message, sizeof g_message, "Wordguess: %d of %d", wins, played);
  else
    snprintf(g_message, sizeof g_message, "Wordguess: the word was %s", answer);
  gtcaca_redraw();
  caca_refresh_display(gmo.dp);
}

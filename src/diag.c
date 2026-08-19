/* diag.c — `ccm --diag`: report what the editor sees before it opens a display,
 * then watch the terminal live.
 *
 * Written after a bug that took a machine-to-machine comparison to pin down:
 * on one mac selection and diagram drawing were dead while the same cacamacs
 * build worked elsewhere. Everything needed to explain it — which libgtcaca the
 * binary actually loaded, whether fd 1 was a tty, whether the terminal answered
 * a drag with motion reports, whether Ctrl-Space reached the process at all —
 * was knowable in a second, but only by hand-running pty experiments. This does
 * that for the user, on their machine, and prints it in a form they can paste.
 *
 * It never opens a display: no libcaca, no alternate screen. The probes talk to
 * the controlling terminal directly, exactly the way gtcaca does at runtime, so
 * what they measure is what the editor would get.
 */
/* dladdr()/Dl_info are GNU extensions: glibc's <dlfcn.h> keeps them behind
   __USE_GNU, and -std=gnu99 (what this project builds with) does not turn that
   on — so the define has to come before any header is pulled in, including
   cacamacs.h. macOS and the BSDs declare both unconditionally and ignore it. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cacamacs.h"

#include <errno.h>
#ifndef _WIN32
#include <termios.h>
#include <dlfcn.h>
#endif

#define PROBE_SECONDS 8

static void row(const char *name, const char *value)
{
  printf("  %-22s %s\n", name, value && *value ? value : "(unset)");
}

static void env_row(const char *name)
{
  row(name, getenv(name));
}

/* Which library file did the loader actually pick? The wrong libgtcaca — a
   stale keg, another install ahead on the search path — looks identical from
   the outside, so print the resolved path rather than a version string. */
static void lib_row(const char *name, void *sym)
{
#ifndef _WIN32
  Dl_info info;
  if (sym && dladdr(sym, &info) && info.dli_fname) { row(name, info.dli_fname); return; }
  row(name, "(unresolved)");
#else
  (void)sym;
  row(name, "(DLL path not reported on Windows)");
#endif
}

#ifndef _WIN32
/* ── raw-mode probes ───────────────────────────────────────────────────────
 * Both probes read the controlling terminal, not stdin: a user who pipes the
 * output ("ccm --diag | tee") would otherwise be measuring the pipe. */
static int tty_fd = -1;
static struct termios saved_tio;

static int probe_begin(void)
{
  struct termios raw;
  tty_fd = open("/dev/tty", O_RDWR);
  if (tty_fd < 0) return -1;
  if (tcgetattr(tty_fd, &saved_tio) < 0) { close(tty_fd); tty_fd = -1; return -1; }
  raw = saved_tio;
  raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);   /* keys as they are typed */
  raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);           /* keep C-s and C-m intact */
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;                                /* 100 ms read timeout */
  if (tcsetattr(tty_fd, TCSANOW, &raw) < 0) { close(tty_fd); tty_fd = -1; return -1; }
  return 0;
}

static void probe_end(void)
{
  if (tty_fd < 0) return;
  tcsetattr(tty_fd, TCSANOW, &saved_tio);
  close(tty_fd);
  tty_fd = -1;
}

static void tty_write(const char *s)
{
  size_t n = strlen(s), off = 0;
  while (off < n) {
    ssize_t w = write(tty_fd, s + off, n - off);
    if (w > 0) { off += (size_t)w; continue; }
    if (w < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    break;
  }
}

/* Read one byte, or -1 once `until` has passed. */
static int probe_getc(time_t until)
{
  unsigned char b;
  for (;;) {
    ssize_t n = read(tty_fd, &b, 1);
    if (n == 1) return b;
    if (n < 0 && errno != EINTR && errno != EAGAIN) return -1;
    if (time(NULL) >= until) return -1;
  }
}

/* ── mouse ─────────────────────────────────────────────────────────────────
 * Enables exactly what gtcaca enables (1002 button-event tracking + 1006 SGR)
 * and counts what comes back. The distinction that matters: a terminal doing
 * plain click tracking answers presses but never motion, which is precisely
 * the state in which typing works, the wheel works, and both text selection
 * and diagram drawing are impossible. */
static void probe_mouse(void)
{
  int press = 0, motion = 0, release = 0, wheel = 0, other = 0, c;
  char first[64] = "";
  time_t until = time(NULL) + PROBE_SECONDS;

  printf("\nMouse probe — click and drag inside this window (%d seconds)…\n",
         PROBE_SECONDS);
  fflush(stdout);
  tty_write("\033[?1002h\033[?1006h");

  while ((c = probe_getc(until)) >= 0) {
    int b = 0, x = 0, y = 0, field = 0, fin = 0, i;
    if (c != 0x1b) continue;
    if (probe_getc(until) != '[') continue;
    if (probe_getc(until) != '<') continue;
    for (i = 0; i < 32; i++) {
      c = probe_getc(until);
      if (c < 0) break;
      if (c >= '0' && c <= '9') {
        if (field == 0) b = b*10 + (c-'0'); else if (field == 1) x = x*10 + (c-'0');
        else y = y*10 + (c-'0');
      } else if (c == ';') field++;
      else if (c == 'M' || c == 'm') { fin = c; break; }
      else break;
    }
    if (!fin) continue;
    if (!first[0])
      snprintf(first, sizeof first, "ESC[<%d;%d;%d%c", b, x, y, fin);
    if      (b & 0x40) wheel++;
    else if (b & 0x20) motion++;
    else if (fin == 'M') press++;
    else if (fin == 'm') release++;
    else other++;
  }

  tty_write("\033[?1002l\033[?1006l");
  printf("  press %d · motion %d · release %d · wheel %d · other %d\n",
         press, motion, release, wheel, other);
  if (first[0]) printf("  first report          %s\n", first);

  if (!press && !motion && !release && !wheel)
    puts("  VERDICT: the terminal sent nothing. Mouse reporting is off "
         "(Terminal.app: View > Allow Mouse Reporting; iTerm2: Settings > "
         "Advanced > Enable mouse reporting), or a multiplexer is eating it.");
  else if (!motion)
    puts("  VERDICT: presses arrive but motion does not — the terminal is doing "
         "click-only tracking, so text selection and diagram drawing cannot "
         "work. Something re-set mouse mode after 1002 was enabled.");
  else
    puts("  VERDICT: ok — press, drag and release all reach the editor.");
}

/* ── keyboard ──────────────────────────────────────────────────────────────
 * Chords the editor needs are the ones a desktop quietly steals: macOS binds
 * Ctrl-Space to Input Sources, and Meta only reaches the terminal when the
 * profile maps Option to Esc+. Show the raw bytes so there is nothing to
 * argue about. */
static void probe_keys(void)
{
  int c, seen_nul = 0, seen_meta = 0, count = 0;
  time_t until = time(NULL) + PROBE_SECONDS;

  printf("\nKey probe — press Ctrl-Space, then M-w, then a letter (%d seconds)…\n",
         PROBE_SECONDS);
  fflush(stdout);

  while ((c = probe_getc(until)) >= 0) {
    count++;
    if (c == 0x00)      { seen_nul = 1;  printf("  0x00  Ctrl-Space (set-mark)\n"); }
    else if (c == 0x1b) { seen_meta = 1; printf("  0x1b  Esc / Meta prefix\n"); }
    else if (c < 0x20)  printf("  0x%02x  Ctrl-%c\n", c, '@' + c);
    else if (c < 0x7f)  printf("  0x%02x  '%c'\n", c, c);
    else                printf("  0x%02x\n", c);
    fflush(stdout);
  }

  if (!count)
    puts("  VERDICT: no keys arrived at all.");
  else if (!seen_nul)
    puts("  VERDICT: Ctrl-Space never reached the terminal — macOS is most "
         "likely holding it for Input Sources (System Settings > Keyboard > "
         "Keyboard Shortcuts > Input Sources). Until that is freed, set the "
         "mark with M-x set-mark (C-x SPC starts a rectangle mark, not a "
         "plain one).");
  else if (!seen_meta)
    puts("  VERDICT: Ctrl-Space arrives but no Esc prefix was seen — Option is "
         "not sending Meta (Terminal.app: Profiles > Keyboard > Use Option as "
         "Meta key). Esc followed by w works in the meantime.");
  else
    puts("  VERDICT: ok — Ctrl-Space and Meta both reach the editor.");
}
#endif /* !_WIN32 */

int ccm_run_diag(const char *argv0)
{
  char buf[64];
#ifndef _WIN32
  struct winsize ws;
  int have_tty = 0;
#endif

  puts("cacamacs diagnostics");
  puts("\nbuild");
  row("ccm version", CCM_VERSION_STR);
  row("invoked as", argv0);
  row("libcaca version", caca_get_version());
  lib_row("libgtcaca loaded", (void *)(size_t)gtcaca_init);
  lib_row("libcaca loaded", (void *)(size_t)caca_get_version);

  puts("\nterminal");
  env_row("TERM");
  env_row("COLORTERM");
  env_row("TERM_PROGRAM");
  env_row("TERM_PROGRAM_VERSION");
  snprintf(buf, sizeof buf, "stdin %s · stdout %s · stderr %s",
           isatty(0) ? "tty" : "NOT a tty",
           isatty(1) ? "tty" : "NOT a tty",
           isatty(2) ? "tty" : "NOT a tty");
  row("file descriptors", buf);

#ifndef _WIN32
  { int fd = open("/dev/tty", O_WRONLY);
    have_tty = fd >= 0;
    row("/dev/tty", have_tty ? "open ok" : "unavailable");
    if (have_tty) {
      if (ioctl(fd, TIOCGWINSZ, &ws) == 0) {
        snprintf(buf, sizeof buf, "%d x %d", ws.ws_col, ws.ws_row);
        row("size", buf);
      }
      close(fd);
    }
  }
#endif

  puts("\nenvironment");
  env_row("LANG");
  env_row("LC_ALL");
  env_row("LC_CTYPE");
  env_row("TMUX");
  env_row("STY");
  env_row("SSH_TTY");
  env_row("CACA_DRIVER");
  env_row("GTCACA_NO_TRUECOLOR");
  env_row("CCM_BUILTIN_EXTENSIONS");

  if (!isatty(1))
    puts("\nNOTE: stdout is not a terminal. Run --diag without a pipe or "
         "redirection — piping it changes what is being measured.");

#ifndef _WIN32
  if (!isatty(0) || !have_tty) {
    puts("\nSkipping the live probes: no terminal on stdin.");
    return 0;
  }
  if (probe_begin() < 0) {
    puts("\nSkipping the live probes: could not put the terminal in raw mode.");
    return 0;
  }
  probe_mouse();
  probe_keys();
  probe_end();
  puts("");
#else
  puts("\nLive probes are POSIX-only.");
#endif
  return 0;
}

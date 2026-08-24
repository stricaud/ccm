/* lock.c — "the file changed on disk" questions, the way Emacs asks them.
 *
 * Every buffer visiting a file remembers the modification time that file had
 * when it was read or last written (Emacs calls it buffer-file-modtime). Three
 * things compare that stamp against the file itself, so nothing you or anyone
 * else wrote is ever quietly overwritten or thrown away:
 *
 *   - the first change to an otherwise unmodified buffer whose file has moved
 *     on underneath it (ask-user-about-supersession-threat, userlock.el):
 *         myfile.md changed on disk; really edit the buffer? (y, n, r or C-h)
 *   - saving such a buffer (basic-save-buffer, files.el):
 *         myfile.md has changed since visited or saved.  Save anyway? (yes or no)
 *   - re-visiting the file with C-x C-f, or from the browser, when a buffer for
 *     it is already open (find-file-noselect):
 *         File myfile.md changed on disk.  Reread from disk? (yes or no)
 *
 * and M-x revert-buffer throws the buffer away for the file on purpose.
 *
 * The stamp is (re)taken exactly where Emacs takes it: on visiting a file, on
 * writing one, and on reverting.
 */

#include "cacamacs.h"

int g_super_active = 0;       /* the y/n/r question is up */
static int g_super_buf  = -1; /* … about this buffer */
static int g_super_help = 0;  /* … and C-h put the explanation on screen */

static int g_ask_buf = -1;    /* buffer a yes-or-no question is about */

static const char *base_name(const char *p)
{
  const char *s = p ? strrchr(p, '/') : NULL;
  return s ? s + 1 : (p ? p : "");
}

/* The file's modification stamp. A file that is not there gets exists = 0 —
   the "no modtime yet" state Emacs records for a buffer visiting a new file.
   Size rides along with the time because a second is a long while: two writes
   within the same second are common when a formatter or a `git checkout` is
   what changed the file, and sub-second resolution is not everywhere. */
static void stamp_read(const char *path, ccm_stamp_t *s)
{
  struct stat st;
  memset(s, 0, sizeof *s);
  if (!path || !path[0] || stat(path, &st) != 0) return;
  s->exists = 1;
  s->sec    = st.st_mtime;
  s->size   = (long long)st.st_size;
#if defined(__APPLE__)
  s->nsec = (long)st.st_mtimespec.tv_nsec;
#elif defined(_WIN32)
  s->nsec = 0;
#else
  s->nsec = (long)st.st_mtim.tv_nsec;
#endif
}

void ccm_stamp_buffer(int bi)
{
  if (bi < 0 || bi >= g_nbuf) return;
  stamp_read(g_buffers[bi].has_file ? g_buffers[bi].path : NULL, &g_buffers[bi].stamp);
}

/* Has the file changed since we visited or last saved it?  A file that has been
   *deleted* is not "changed": there is nothing left to overwrite and nothing to
   reread, so Emacs stays quiet about it too, and so do we. */
int ccm_buffer_stale(int bi)
{
  ccm_stamp_t now;
  buffer_t *b;

  if (bi < 0 || bi >= g_nbuf) return 0;
  b = &g_buffers[bi];
  if (!b->has_file) return 0;
  stamp_read(b->path, &now);
  if (!now.exists) return 0;
  return !(b->stamp.exists && now.sec == b->stamp.sec &&
           now.nsec == b->stamp.nsec && now.size == b->stamp.size);
}

/* Replace the buffer with what is on disk, keeping point where it was (as
   revert-buffer does). set_text drops the undo history and the save point, so
   the buffer comes back exactly as freshly visited. */
void ccm_revert_buffer(int bi)
{
  buffer_t *b;
  char *c;
  int pos, len;

  if (bi < 0 || bi >= g_nbuf) return;
  b = &g_buffers[bi];
  if (!b->has_file) { snprintf(g_message, sizeof g_message, "Buffer does not seem to be associated with any file"); return; }
  c = read_file(b->path);
  if (!c) { snprintf(g_message, sizeof g_message, "Cannot read %s", b->path); return; }

  pos = gtcaca_editor_get_current_pos(b->ed);
  gtcaca_editor_set_text(b->ed, c);
  free(c);
  len = gtcaca_editor_get_length(b->ed);
  gtcaca_editor_goto_pos(b->ed, pos > len ? len : pos);
  gtcaca_editor_empty_undo_buffer(b->ed);
  gtcaca_editor_set_save_point(b->ed);
  b->was_modified = 0;
  ccm_stamp_buffer(bi);
  ccm_log_file('O', b->path);
  snprintf(g_message, sizeof g_message, "Reverted %s", b->path);
}

/* ── the supersession threat: y, n, r or C-h ───────────────────────────────── */

static const char *SUPER_HELP =
  "You want to modify a buffer whose contents have changed\n"
  "since you last visited or saved it.\n"
  "\n"
  "If you say `y' to go ahead and modify this buffer, you risk ruining\n"
  "the work of whoever rewrote the file.\n"
  "If you say `r' to revert, the contents of the buffer are refreshed\n"
  "from the file on disk.\n"
  "If you say `n', the change you started to make will be aborted.\n"
  "\n"
  "Usage:\n"
  "y  --  go ahead and modify the buffer\n"
  "n  --  abort — don't make the change\n"
  "r  --  revert the buffer from the file on disk\n"
  "C-h --  show this help\n"
  "\n"
  "(q or Esc closes this window and leaves the question standing.)\n";

static void super_prompt(void)
{
  snprintf(g_message, sizeof g_message, "%s changed on disk; really edit the buffer? (y, n, r or C-h)",
           base_name(g_buffers[g_super_buf].path));
}

/* Called from refresh_modeline, i.e. after every key the editor has handled:
   the moment an unmodified buffer becomes modified is the moment Emacs asks. */
void ccm_check_supersession(gtcaca_editor_widget_t *ed)
{
  buffer_t *b;
  int bi, now;

  if (!ed) return;
  bi = buf_index_of(ed);
  if (bi < 0) return;
  b = &g_buffers[bi];
  now = gtcaca_editor_get_modify(ed) ? 1 : 0;

  if (now && !b->was_modified && !g_super_active && b->has_file && ccm_buffer_stale(bi)) {
    g_super_active = 1; g_super_buf = bi; g_super_help = 0;
    super_prompt();
  }
  b->was_modified = now;     /* only the 0 → 1 transition asks, and asks once */
}

static void super_done(void)
{
  g_super_active = 0;
  if (g_super_help) { g_super_help = 0; hide_help(); }
}

/* Every key goes here while the question is up (see on_key). y/n/r answer it,
   ? and C-h explain it, anything else is ignored — as read-char-choice does. */
int supersession_key(int key)
{
  gtcaca_editor_widget_t *ed;
  int bi = g_super_buf;

  if (bi < 0 || bi >= g_nbuf) { super_done(); return 1; }
  ed = g_buffers[bi].ed;

  switch (key) {
  case 'y': case 'Y':
    super_done();
    g_message[0] = '\0';            /* go ahead: the edit stands */
    break;
  case 'n': case 'N':
  case CACA_KEY_CTRL_G:
  case CACA_KEY_ESCAPE:
    super_done();                   /* abort: take the change back out again */
    gtcaca_editor_undo(ed);
    gtcaca_editor_set_save_point(ed);
    g_buffers[bi].was_modified = 0;
    snprintf(g_message, sizeof g_message, "File changed on disk: %s", g_buffers[bi].path);
    break;
  case 'r': case 'R':
    super_done();
    ccm_revert_buffer(bi);
    break;
  /* C-h *is* ASCII 0x08, which libcaca names CACA_KEY_BACKSPACE — and ncurses
     hands the tty's erase key over under the same name, so Backspace answers
     here too. The same split terminal Emacs lives with, which is why C-h is its
     help key and DEL erases (see ccm_del_deletes_forward in keymap.c). */
  case '?': case CACA_KEY_BACKSPACE:
    if (!g_super_help) { g_super_help = 1; show_help_with("Changed on disk", SUPER_HELP); }
    super_prompt();                 /* the question stays under the window */
    break;
  default:
    super_prompt();                 /* not one of the choices: ask again */
    break;
  }
  if (g_ed) refresh_modeline(g_ed, NULL);
  return 1;
}

/* ── yes-or-no questions (reread, revert) ──────────────────────────────────── */

/* Emacs' yes-or-no-p: nothing but a typed-out "yes" or "no" will do, and
   anything else is met with "Please answer yes or no." and the same question. */
static int yes_or_no(const char *answer, void (*again)(const char *lead))
{
  if (!strcmp(answer, "yes")) return 1;
  if (!strcmp(answer, "no"))  return 0;
  again("Please answer yes or no.  ");
  return -1;
}

static void reread_done(const char *answer);

static void ask_reread(const char *lead)
{
  char prompt[256];
  buffer_t *b = &g_buffers[g_ask_buf];
  snprintf(prompt, sizeof prompt, "%sFile %s changed on disk.  %s (yes or no) ",
           lead, base_name(b->path),
           gtcaca_editor_get_modify(b->ed) ? "Discard your edits?" : "Reread from disk?");
  start_minibuffer(prompt, reread_done);
}

static void reread_done(const char *answer)
{
  int yes = yes_or_no(answer, ask_reread);
  if (yes < 0) return;
  if (yes) ccm_revert_buffer(g_ask_buf);
  else     g_message[0] = '\0';
}

/* C-x C-f (or the browser) on a file that is already open in a buffer: if the
   file has moved on since, offer to reread it. */
void ccm_maybe_reread(int bi)
{
  if (bi < 0 || bi >= g_nbuf || g_super_active || g_mb_active) return;
  if (!ccm_buffer_stale(bi)) return;
  g_ask_buf = bi;
  ask_reread("");
}

static void revert_done(const char *answer);

static void ask_revert(const char *lead)
{
  char prompt[256];
  snprintf(prompt, sizeof prompt, "%sRevert buffer from file %s? (yes or no) ",
           lead, base_name(g_buffers[g_ask_buf].path));
  start_minibuffer(prompt, revert_done);
}

static void revert_done(const char *answer)
{
  int yes = yes_or_no(answer, ask_revert);
  if (yes < 0) return;
  if (yes) ccm_revert_buffer(g_ask_buf);
  else     g_message[0] = '\0';
}

void start_revert_buffer(void)   /* M-x revert-buffer */
{
  if (g_cur_buf < 0 || !g_buffers[g_cur_buf].has_file) {
    snprintf(g_message, sizeof g_message, "Buffer does not seem to be associated with any file");
    return;
  }
  g_ask_buf = g_cur_buf;
  ask_revert("");
}

/* ── saving over a file that changed ───────────────────────────────────────── */

static void save_anyway_done(const char *answer);

static void ask_save_anyway(const char *lead)
{
  char prompt[256];
  snprintf(prompt, sizeof prompt, "%s%s has changed since visited or saved.  Save anyway? (yes or no) ",
           lead, base_name(g_buffers[g_ask_buf].path));
  start_minibuffer(prompt, save_anyway_done);
}

static void save_anyway_done(const char *answer)
{
  int yes = yes_or_no(answer, ask_save_anyway);
  if (yes < 0) return;
  if (yes) write_buffer_file(g_buffers[g_ask_buf].ed);
  else     snprintf(g_message, sizeof g_message, "Save not confirmed");
}

/* C-x C-s: 1 if the write may go ahead now, 0 if a question was put up instead
   (the answer runs the write itself). */
int ccm_confirm_save(int bi)
{
  if (bi < 0 || bi >= g_nbuf) return 1;
  if (!ccm_buffer_stale(bi)) return 1;
  g_ask_buf = bi;
  ask_save_anyway("");
  return 0;
}

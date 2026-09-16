#include "cacamacs.h"

/* ── GnuPG-encrypted files ────────────────────────────────────────────────────
 *
 * `ccm -k file` keeps `file` encrypted to a public key: ccm asks GnuPG to
 * decrypt it on the way in and to encrypt it again on every save, so what the
 * disk ever holds is ciphertext and what the editor holds is the plain text.
 * Opening an already-encrypted file needs no -k — ccm recognises one and asks
 * for the passphrase.
 *
 * Two rules shape everything below.
 *
 * The plain text never touches the disk. gpg is run as a child with pipes on
 * both ends and the bytes go through memory, never through a temporary file —
 * a file ccm cannot unlink if it is killed between writing and removing it.
 *
 * The passphrase never appears in an argument. `--passphrase-fd` reads it from
 * a pipe of its own, so it is never in /proc, never in `ps`, and never in a
 * shell history. It is wiped as soon as gpg has taken it: encryption is to a
 * *public* key and needs no secret, so nothing has to be held for the save.
 */

#ifndef _WIN32
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#endif

/* "gpg-program" in config.json — any binary that speaks gpg's command line.
   gpg2 under another name, a wrapper that picks a card reader, a stub in a
   test. Not a free-form encryption command: the arguments below are gpg's. */
char g_cfg_gpg[PATH_MAX] = "gpg";

/* Overwrite and do it for real — a plain memset on a buffer that is about to
   die is exactly what a compiler is entitled to drop. */
void ccm_wipe(void *p, size_t n)
{
  volatile unsigned char *q = (volatile unsigned char *)p;
  while (n--) *q++ = 0;
}

/* ── is this file encrypted? ─────────────────────────────────────────────────
 *
 * Asked of every file opened, so it has to be sure. A false positive locks a
 * perfectly readable file behind a passphrase prompt that can never succeed.
 *
 * Three things say yes: the armour header, which is unambiguous; one of the
 * extensions that mean it by convention; or an OpenPGP packet tag opening the
 * file. The last is the loosest, so only the four tags that can legitimately
 * *begin* an encrypted message count — a session key or the encrypted data
 * itself — in both the old and the new packet format. */
int ccm_file_is_encrypted(const char *path)
{
  static const unsigned char TAGS[] = {
    0x85, 0x84,   /* old format, tag 1 (public-key session key), 2- and 1-byte length */
    0x8c, 0x8d,   /* old format, tag 3 (symmetric session key) */
    0xc1, 0xc3,   /* new format, tags 1 and 3 */
    0xa3, 0xc8,   /* old and new tag 8 — compressed, what -z leaves on top */
    0xa6, 0xd2,   /* old tag 9 / new tag 18 — the encrypted data packet itself */
  };
  unsigned char head[32];
  const char *dot;
  size_t n, i;
  FILE *f;

  if (!path || !*path) return 0;
  dot = strrchr(path, '.');
  if (dot && (!strcasecmp(dot, ".gpg") || !strcasecmp(dot, ".pgp") || !strcasecmp(dot, ".asc")))
    return 1;

  f = fopen(path, "rb");
  if (!f) return 0;
  n = fread(head, 1, sizeof head, f);
  fclose(f);
  if (n == 0) return 0;                       /* an empty file is not a locked one */

  if (n >= 27 && !memcmp(head, "-----BEGIN PGP MESSAGE-----", 27)) return 1;
  for (i = 0; i < sizeof TAGS; i++) if (head[0] == TAGS[i]) return 1;
  return 0;
}

#ifdef _WIN32

/* Running gpg needs pipes on four descriptors at once, which is fork/exec
   work; CreateProcess can do it but nothing here has been tested against it.
   Rather than half-support it, say so — and say so before anything has been
   read or written, so no file is ever left in a state ccm cannot undo. */
int ccm_crypt_supported(void) { return 0; }

static int gpg_run(const char *const *argv, const char *pass,
                   const char *in, size_t inlen,
                   char **out, size_t *outlen, char *err, size_t errsz)
{
  (void)argv; (void)pass; (void)in; (void)inlen; (void)out; (void)outlen;
  snprintf(err, errsz, "encrypted files are not supported on this platform");
  return -1;
}

#else

int ccm_crypt_supported(void) { return 1; }

/* Run gpg with `in` on its standard input and its output collected.
 *
 * Four pipes, and every one of them has to be pumped at once: gpg will not
 * finish reading the plain text until something is draining the ciphertext it
 * has already produced, and a loop that wrote everything before reading
 * anything would sit forever on a full pipe with a large file. So the loop
 * below moves whichever end is ready and closes each as it runs out.
 *
 * Returns gpg's exit status, or -1 if it could not be run at all. */
static int gpg_run(const char *const *argv, const char *pass,
                   const char *in, size_t inlen,
                   char **out, size_t *outlen, char *err, size_t errsz)
{
  enum { RD, WR };
  int pin[2] = { -1, -1 }, pout[2] = { -1, -1 }, perr[2] = { -1, -1 }, ppass[2] = { -1, -1 };
  size_t sent = 0, ocap = 0, olen = 0, elen = 0;
  char *obuf = NULL;
  char ebuf[512];
  pid_t pid;
  int status = -1, i;
  void (*oldpipe)(int);

  if (err && errsz) err[0] = '\0';
  if (out) *out = NULL;
  if (outlen) *outlen = 0;
  ebuf[0] = '\0';

  if (pipe(pin) < 0 || pipe(pout) < 0 || pipe(perr) < 0) goto done;
  if (pass && pipe(ppass) < 0) goto done;

  /* gpg exiting early — a wrong passphrase, a missing key — closes its end of
     the input pipe while we are still writing to it. That is an EPIPE to
     handle, not a signal to die from. */
  oldpipe = signal(SIGPIPE, SIG_IGN);

  pid = fork();
  if (pid < 0) { signal(SIGPIPE, oldpipe); goto done; }

  if (pid == 0) {                              /* child */
    dup2(pin[RD], STDIN_FILENO);
    dup2(pout[WR], STDOUT_FILENO);
    dup2(perr[WR], STDERR_FILENO);
    if (pass) {
      /* fd 3 by agreement with the --passphrase-fd 3 in the argv below. dup2
         to itself would leave FD_CLOEXEC set, so clear it explicitly. */
      if (ppass[RD] != 3) dup2(ppass[RD], 3);
      else fcntl(3, F_SETFD, 0);
    }
    for (i = 3 + (pass ? 1 : 0); i < 256; i++) close(i);
    close(pin[WR]); close(pout[RD]); close(perr[RD]);
    execvp(argv[0], (char *const *)argv);
    _exit(127);                                /* execvp failed: 127, as a shell says it */
  }

  close(pin[RD]);  pin[RD]  = -1;
  close(pout[WR]); pout[WR] = -1;
  close(perr[WR]); perr[WR] = -1;

  if (pass) {
    /* Small enough to fit a pipe buffer whole, so this cannot block. */
    size_t plen = strlen(pass);
    close(ppass[RD]); ppass[RD] = -1;
    while (plen) {
      ssize_t w = write(ppass[WR], pass, plen);
      if (w <= 0) break;
      pass += w; plen -= (size_t)w;
    }
    write(ppass[WR], "\n", 1);
    close(ppass[WR]); ppass[WR] = -1;
  }

  while (pin[WR] >= 0 || pout[RD] >= 0 || perr[RD] >= 0) {
    fd_set rd, wr;
    int nfds = 0;
    FD_ZERO(&rd); FD_ZERO(&wr);
    if (pin[WR]  >= 0) { FD_SET(pin[WR],  &wr); if (pin[WR]  >= nfds) nfds = pin[WR]  + 1; }
    if (pout[RD] >= 0) { FD_SET(pout[RD], &rd); if (pout[RD] >= nfds) nfds = pout[RD] + 1; }
    if (perr[RD] >= 0) { FD_SET(perr[RD], &rd); if (perr[RD] >= nfds) nfds = perr[RD] + 1; }
    if (select(nfds, &rd, &wr, NULL, NULL) < 0) {
      if (errno == EINTR) continue;
      break;
    }

    if (pin[WR] >= 0 && FD_ISSET(pin[WR], &wr)) {
      if (sent >= inlen) { close(pin[WR]); pin[WR] = -1; }   /* EOF: gpg can finish */
      else {
        ssize_t w = write(pin[WR], in + sent, inlen - sent);
        if (w > 0) sent += (size_t)w;
        else if (w < 0 && (errno == EINTR || errno == EAGAIN)) { /* again */ }
        else { close(pin[WR]); pin[WR] = -1; }
      }
    }
    if (pout[RD] >= 0 && FD_ISSET(pout[RD], &rd)) {
      ssize_t r;
      if (olen + 65536 + 1 > ocap) {
        size_t want = ocap ? ocap * 2 : 131072;
        char *bigger;
        while (want < olen + 65536 + 1) want *= 2;
        bigger = realloc(obuf, want);
        if (!bigger) { close(pout[RD]); pout[RD] = -1; continue; }
        obuf = bigger; ocap = want;
      }
      r = read(pout[RD], obuf + olen, 65536);
      if (r > 0) olen += (size_t)r;
      else if (r == 0 || (errno != EINTR && errno != EAGAIN)) { close(pout[RD]); pout[RD] = -1; }
    }
    if (perr[RD] >= 0 && FD_ISSET(perr[RD], &rd)) {
      ssize_t r = read(perr[RD], ebuf + elen, sizeof ebuf - 1 - elen);
      if (r > 0) { elen += (size_t)r; ebuf[elen] = '\0'; }
      else if (r == 0 || (errno != EINTR && errno != EAGAIN)) { close(perr[RD]); perr[RD] = -1; }
      if (elen >= sizeof ebuf - 1) { close(perr[RD]); perr[RD] = -1; }
    }
  }

  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { /* again */ }
  status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  signal(SIGPIPE, oldpipe);

  if (obuf) obuf[olen] = '\0';
  if (status == 0 && out) { *out = obuf; obuf = NULL; if (outlen) *outlen = olen; }

  if (status != 0 && err && errsz) {
    /* gpg says a great deal; the last non-empty line is the part that names
       what went wrong ("decryption failed: Bad session key"). */
    char *line = ebuf, *p;
    while (elen > 0 && (ebuf[elen - 1] == '\n' || ebuf[elen - 1] == '\r')) ebuf[--elen] = '\0';
    p = strrchr(ebuf, '\n');
    if (p) line = p + 1;
    if (status == 127) snprintf(err, errsz, "cannot run %s — is GnuPG installed?", argv[0]);
    else if (*line)    snprintf(err, errsz, "%s", line);
    else               snprintf(err, errsz, "%s failed (status %d)", argv[0], status);
  }

done:
  free(obuf);
  for (i = 0; i < 2; i++) {
    if (pin[i]   >= 0) close(pin[i]);
    if (pout[i]  >= 0) close(pout[i]);
    if (perr[i]  >= 0) close(perr[i]);
    if (ppass[i] >= 0) close(ppass[i]);
  }
  if (status < 0 && err && errsz && !err[0])
    snprintf(err, errsz, "could not start %s", argv[0]);
  return status;
}

#endif /* _WIN32 */

/* ── the three things ccm asks gpg to do ─────────────────────────────────── */

/* Decrypt `path` into a NUL-terminated buffer the caller frees.
 *
 * `pass` NULL asks gpg-agent to do it on its own. That succeeds when the agent
 * already holds the secret key unlocked, and fails at once when it does not —
 * loopback mode makes gpg answer rather than open a pinentry of its own, which
 * is what keeps a second program off a terminal ccm is drawing on. */
char *ccm_gpg_decrypt(const char *path, const char *pass, size_t *outlen,
                      char *err, size_t errsz, int *fatal)
{
  const char *with[] = {
    g_cfg_gpg, "--batch", "--yes", "--quiet", "--no-tty",
    "--pinentry-mode", "loopback", "--passphrase-fd", "3",
    "--decrypt", path, NULL
  };
  const char *without[] = {
    g_cfg_gpg, "--batch", "--yes", "--quiet", "--no-tty",
    "--pinentry-mode", "loopback",
    "--decrypt", path, NULL
  };
  char *out = NULL;
  int rc = gpg_run(pass ? with : without, pass, NULL, 0, &out, outlen, err, errsz);
  /* 127 is "could not exec" and -1 "could not start": the passphrase was never
     the problem, so asking for it again would only ask three times. */
  if (fatal) *fatal = (rc == 127 || rc < 0);
  if (rc != 0) {
    free(out);
    return NULL;
  }
  return out ? out : calloc(1, 1);
}

/* Encrypt `data` to `key` and write the result over `path`.
 *
 * gpg writes the file itself (`--output`), so the plain text goes down a pipe
 * and only ciphertext is ever handed to the filesystem. */
int ccm_gpg_encrypt(const char *path, const char *key, const char *data, size_t len,
                    char *err, size_t errsz)
{
  const char *argv[] = {
    g_cfg_gpg, "--batch", "--yes", "--quiet", "--no-tty",
    "--output", path, "--recipient", key, "--encrypt", NULL
  };
  return gpg_run(argv, NULL, data, len, NULL, NULL, err, errsz) == 0 ? 0 : -1;
}

/* Who is `path` encrypted to? Read off the file rather than remembered, so a
 * file opened without -k still saves back to the same key.
 *
 * --list-only stops before any secret is needed, so this asks for no
 * passphrase; it prints one ENC_TO line per recipient and we keep the first. */
int ccm_gpg_recipient(const char *path, char *out, size_t outsz)
{
  const char *argv[] = {
    g_cfg_gpg, "--batch", "--quiet", "--no-tty",
    "--status-fd", "1", "--list-only", "--decrypt", path, NULL
  };
  char *status = NULL, *p;
  char err[256];
  int found = 0;

  out[0] = '\0';
  gpg_run(argv, NULL, NULL, 0, &status, NULL, err, sizeof err);
  if (!status) return 0;

  for (p = strstr(status, "ENC_TO "); p; p = strstr(p + 1, "ENC_TO ")) {
    const char *id = p + 7;
    size_t n = strcspn(id, " \r\n");
    if (n == 0 || n >= outsz) continue;
    /* gpg reports an unstated recipient as all zeroes (`--throw-keyids`);
       that names nobody, so keep looking. */
    if (strspn(id, "0") >= n) continue;
    memcpy(out, id, n); out[n] = '\0';
    found = 1;
    break;
  }
  free(status);
  return found;
}

/* ── the editor side ──────────────────────────────────────────────────────────
 *
 * Both questions — the passphrase to get in, the key to save back to — are
 * asked in the minibuffer, which answers through a callback long after the
 * call that started it. So the state of a half-finished unlock lives here:
 * which buffer is waiting, and whether a key still has to be asked for once it
 * is open.
 *
 * A locked buffer is held read-only and empty. That is not a display nicety:
 * an empty buffer that believes it holds the file is one C-x C-s away from
 * replacing the ciphertext with nothing.
 */

#define CRYPT_TRIES 3                    /* wrong passphrases before giving up */

static int g_crypt_buf   = -1;           /* buffer the open question belongs to */
static int g_crypt_askey = 0;            /* -k: ask for the recipient after unlocking */
static int g_crypt_tries = 0;

static void crypt_ask_pass(const char *lead);
static void crypt_ask_key(void);
static int  crypt_unlock(const char *pass, int *fatal);

/* Mark `bi` as holding an encrypted file that has not been decrypted yet. */
void ccm_crypt_lock(int bi)
{
  buffer_t *b = &g_buffers[bi];
  b->crypt = 1;
  b->locked = 1;
  gtcaca_editor_set_text(b->ed, "");
  gtcaca_editor_set_read_only(b->ed, 1);
  ccm_gpg_recipient(b->path, b->crypt_key, sizeof b->crypt_key);
}

/* Start the questions for buffer `bi`. `key` is what -k was given, "" if -k was
   given bare (ask), NULL if it was not given at all. */
void ccm_crypt_begin(int bi, const char *key)
{
  buffer_t *b;

  if (bi < 0 || bi >= g_nbuf) return;
  b = &g_buffers[bi];

  if (key && *key) {                     /* -kKEYID: nothing to ask about it */
    strncpy(b->crypt_key, key, sizeof b->crypt_key - 1);
    b->crypt_key[sizeof b->crypt_key - 1] = '\0';
    b->crypt = 1;
  }

  g_crypt_buf   = bi;
  g_crypt_askey = (key && !*key);
  g_crypt_tries = 0;

  if (b->locked) {
    /* Ask gpg-agent first. If it is already holding the key unlocked — you
       decrypted something a minute ago — there is nothing to ask for, and a
       prompt whose answer is not checked is worse than no prompt at all. */
    if (crypt_unlock(NULL, NULL) != 0) crypt_ask_pass(NULL);
    return;
  }
  if (g_crypt_askey) { crypt_ask_key(); return; }
  if (b->crypt)
    snprintf(g_message, sizeof g_message,
             "Encrypted to %s — C-x C-s writes it back encrypted", b->crypt_key);
}

/* ── the passphrase ──────────────────────────────────────────────────────── */

/* One attempt at opening the locked buffer. 0 when the text is in it, -1 when
   it is still locked, with g_message left saying why. */
static int crypt_unlock(const char *pass, int *fatal)
{
  char err[256] = "";
  buffer_t *b = &g_buffers[g_crypt_buf];
  size_t len = 0;
  char *plain = ccm_gpg_decrypt(b->path, pass, &len, err, sizeof err, fatal);

  if (!plain) {
    snprintf(g_message, sizeof g_message, "%s", err[0] ? err : "Decryption failed");
    return -1;
  }

  gtcaca_editor_set_read_only(b->ed, 0);
  gtcaca_editor_set_text(b->ed, plain);
  ccm_wipe(plain, len);                    /* our copy; the editor has its own */
  free(plain);
  gtcaca_editor_empty_undo_buffer(b->ed);  /* no undo back to the locked state */
  gtcaca_editor_set_save_point(b->ed);
  gtcaca_editor_goto_pos(b->ed, 0);
  b->locked = 0;
  ccm_stamp_buffer(g_crypt_buf);

  if (g_crypt_askey) { crypt_ask_key(); return 0; }
  if (b->crypt_key[0])
    snprintf(g_message, sizeof g_message,
             "Decrypted — saves go back encrypted to %s", b->crypt_key);
  else
    snprintf(g_message, sizeof g_message,
             "Decrypted — M-x set-encryption-key names who to save it back to");
  return 0;
}

static void crypt_pass_done(const char *pass)
{
  int fatal = 0, ok;
  char why[256];

  if (g_crypt_buf < 0 || g_crypt_buf >= g_nbuf) return;

  ok = crypt_unlock(pass, &fatal) == 0;
  ccm_wipe((void *)pass, strlen(pass));    /* the copy the minibuffer handed us */
  if (ok) return;

  snprintf(why, sizeof why, "%s", g_message);
  /* Nearly always a typo, so ask again rather than making the user find the
     command to retry — but not forever, and not at all when gpg itself is what
     is wrong, where the answer would never change however often it is asked. */
  if (!fatal && ++g_crypt_tries < CRYPT_TRIES) { crypt_ask_pass(why); return; }
  /* The state first, the reason after it: the echo line shares its row with
     the modeline and the tail is what gets cut. What matters is that the
     buffer is not the file — C-x C-s says the rest if it is ever tried. */
  snprintf(g_message, sizeof g_message, "Still locked — %s", why);
}

/* `lead` names the last attempt's failure, so the reason and the retry share
   the one echo line rather than the prompt wiping the explanation. */
static void crypt_ask_pass(const char *lead)
{
  buffer_t *b = &g_buffers[g_crypt_buf];
  char prompt[240];
  if (lead) snprintf(prompt, sizeof prompt, "%s — passphrase for %s: ", lead, ccm_base_name(b->path));
  else      snprintf(prompt, sizeof prompt, "Passphrase for %s: ", ccm_base_name(b->path));
  start_minibuffer_secret(prompt, crypt_pass_done);
}

/* ── the recipient ───────────────────────────────────────────────────────── */

static void crypt_key_done(const char *key)
{
  buffer_t *b;

  if (g_crypt_buf < 0 || g_crypt_buf >= g_nbuf) return;
  b = &g_buffers[g_crypt_buf];
  g_crypt_askey = 0;

  while (*key == ' ') key++;
  if (!*key) {
    if (b->crypt_key[0])
      snprintf(g_message, sizeof g_message, "Still encrypting to %s", b->crypt_key);
    else
      snprintf(g_message, sizeof g_message,
               "No key given — the file is left as it is");
    return;
  }

  strncpy(b->crypt_key, key, sizeof b->crypt_key - 1);
  b->crypt_key[sizeof b->crypt_key - 1] = '\0';
  b->crypt = 1;
  snprintf(g_message, sizeof g_message,
           "Encrypting to %s — C-x C-s writes it back encrypted", b->crypt_key);
}

static void crypt_ask_key(void)
{
  /* Pre-filled with whoever the file is already encrypted to, so the common
     answer to "which key?" on an existing file is Enter. */
  start_minibuffer_init("Encrypt to key (name, email or key id): ",
                        crypt_key_done, 0, g_buffers[g_crypt_buf].crypt_key);
}

/* M-x set-encryption-key — the same question, for a buffer already open. */
void ccm_crypt_set_key(int bi)
{
  if (bi < 0 || bi >= g_nbuf) return;
  if (!g_buffers[bi].has_file) {
    snprintf(g_message, sizeof g_message, "This buffer has no file to encrypt");
    return;
  }
  g_crypt_buf = bi;
  g_crypt_askey = 1;
  crypt_ask_key();
}

/* ── saving ──────────────────────────────────────────────────────────────────
 *
 * Called from write_buffer_file for a buffer that is encrypted. Returns 0 when
 * it has written the file, and -1 when the caller must not: a buffer still
 * locked, or a key nobody has named. Refusing is the whole point — a plain
 * write here would put the file on disk in the clear, which is the one thing
 * the user asked for it never to be. */
int ccm_crypt_write(int bi, const char *path, const char *text, size_t len)
{
  buffer_t *b = &g_buffers[bi];
  char err[256] = "";

  if (b->locked) {
    snprintf(g_message, sizeof g_message,
             "%s is still encrypted — nothing to save (M-x revert-buffer to try the passphrase again)",
             ccm_base_name(path));
    return -1;
  }
  if (!b->crypt_key[0]) {
    snprintf(g_message, sizeof g_message,
             "No key to encrypt to — M-x set-encryption-key names one");
    return -1;
  }
  if (ccm_gpg_encrypt(path, b->crypt_key, text, len, err, sizeof err) != 0) {
    snprintf(g_message, sizeof g_message, "Not saved: %s", err[0] ? err : "gpg failed");
    return -1;
  }
  return 0;
}

#include "cacamacs.h"
#include <gtcaca/dialog.h>

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

/* Encrypt `data` and write the result over `path`.
 *
 * Two ways, and which one is in use is decided by whether there is a key:
 * to a public key (`--recipient`), or to a passphrase the user chose
 * (`--symmetric`), which needs no keyring at all.
 *
 * gpg writes the file itself (`--output`), so the plain text goes down a pipe
 * and only ciphertext is ever handed to the filesystem. */
int ccm_gpg_encrypt(const char *path, const char *key, const char *pass,
                    const char *data, size_t len, char *err, size_t errsz)
{
  const char *to_key[] = {
    g_cfg_gpg, "--batch", "--yes", "--quiet", "--no-tty",
    "--output", path, "--recipient", key, "--encrypt", NULL
  };
  const char *to_pass[] = {
    g_cfg_gpg, "--batch", "--yes", "--quiet", "--no-tty",
    "--pinentry-mode", "loopback", "--passphrase-fd", "3",
    "--output", path, "--symmetric", NULL
  };
  if (key && *key)
    return gpg_run(to_key, NULL, data, len, NULL, NULL, err, errsz) == 0 ? 0 : -1;
  if (!pass || !*pass) { snprintf(err, errsz, "no key and no passphrase"); return -1; }
  return gpg_run(to_pass, pass, data, len, NULL, NULL, err, errsz) == 0 ? 0 : -1;
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

/* Which question crypt_choose_passphrase is asking — the three read very
   differently to someone who has just watched a file open. */
enum { CRYPT_ASK_NEW_FILE, CRYPT_ASK_EXISTING, CRYPT_ASK_CHANGE };

static int g_crypt_buf   = -1;           /* buffer the open question belongs to */
static int g_crypt_askey = 0;            /* -k: ask how to encrypt, after unlocking */

static void crypt_ask_pass(const char *lead);
static void crypt_ask_key(void);
static void crypt_ask_setup(void);
static void crypt_say_how(buffer_t *b);
static int  crypt_unlock(const char *pass, int *fatal);
static int  crypt_choose_passphrase(const char *name, int how, char *out, size_t outsz);

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

/* The last word on what happened has to reach the modeline itself.
 *
 * g_message is only *read* when the modeline is rebuilt, and the rebuild that
 * matters here already happened — gtcaca_editor_set_text() fires it while the
 * buffer is being filled, before there is anything to report. Without this the
 * echo line keeps whatever was there when the file was opened, which is how a
 * file that decrypted perfectly came to sit under a stale complaint about
 * syntax colouring. */
static void crypt_flush_message(void)
{
  if (g_ed) refresh_modeline(g_ed, NULL);
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

  if (b->locked) {
    /* Ask gpg-agent first. If it is already holding the key unlocked — you
       decrypted something a minute ago — there is nothing to ask for, and a
       prompt whose answer is not checked is worse than no prompt at all. */
    if (crypt_unlock(NULL, NULL) != 0) crypt_ask_pass(NULL);
    crypt_flush_message();
    return;
  }
  if (g_crypt_askey) { crypt_ask_setup(); crypt_flush_message(); return; }
  if (b->crypt) { crypt_say_how(b); crypt_flush_message(); }
}

/* What this buffer will do on the next save, in one line. */
static void crypt_say_how(buffer_t *b)
{
  if (b->crypt_key[0])
    snprintf(g_message, sizeof g_message,
             "Encrypted to %s — C-x C-s writes it back encrypted", b->crypt_key);
  else if (b->crypt_pass[0])
    snprintf(g_message, sizeof g_message,
             "Encrypted with your passphrase — C-x C-s writes it back encrypted");
  else
    snprintf(g_message, sizeof g_message,
             "Nothing to encrypt with yet — M-x set-encryption-key asks");
}

/* -k with no key named. A file already encrypted to somebody's public key is
   asked about as a key, since that is what it has; anything else — a new file,
   a plain one, one already encrypted to a passphrase — is asked for a
   passphrase, which needs no keyring at all. */
static void crypt_ask_setup(void)
{
  buffer_t *b = &g_buffers[g_crypt_buf];
  struct stat st;
  char pass[256];
  int how;

  g_crypt_askey = 0;
  if (b->crypt_key[0]) { crypt_ask_key(); return; }

  /* Three things to say, and they are not interchangeable: a file that does not
     exist, one that is about to start being encrypted, and one that already is
     and is having its passphrase changed. */
  how = b->crypt_pass[0] ? CRYPT_ASK_CHANGE
      : stat(b->path, &st) != 0 ? CRYPT_ASK_NEW_FILE
      : CRYPT_ASK_EXISTING;

  if (!crypt_choose_passphrase(ccm_base_name(b->path), how, pass, sizeof pass)) {
    if (b->crypt_pass[0]) crypt_say_how(b);
    else snprintf(g_message, sizeof g_message,
                  "No passphrase — %s is left as it is  (M-x set-encryption-key asks again)",
                  ccm_base_name(b->path));
    return;
  }
  snprintf(b->crypt_pass, sizeof b->crypt_pass, "%s", pass);
  ccm_wipe(pass, sizeof pass);
  b->crypt = 1;
  crypt_say_how(b);
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
  /* A file encrypted to a public key can be written back with the public half
     alone, so nothing has to be kept. One encrypted to a passphrase cannot:
     saving it means encrypting it again, and the passphrase is the only way.
     So it is held for the session — and only for the session. */
  if (!b->crypt_key[0] && pass) snprintf(b->crypt_pass, sizeof b->crypt_pass, "%s", pass);

  /* -k on a file that was already encrypted has nothing to set up: it has just
     been opened, so both what it is encrypted with and how to write it back are
     known. Asking anyway is what made opening one look like it had gone wrong —
     the text was on screen and a box was still standing there wanting a key,
     for a file that has no key at all. Changing either is M-x
     set-encryption-key, which is a different question asked on purpose. */
  g_crypt_askey = 0;
  if (b->crypt_key[0])
    snprintf(g_message, sizeof g_message,
             "Decrypted — C-x C-s writes it back encrypted to %s", b->crypt_key);
  else
    snprintf(g_message, sizeof g_message,
             "Decrypted — C-x C-s writes it back with the same passphrase"
             "  (M-x set-encryption-key changes it)");
  return 0;
}

/* Ask for the passphrase until the file opens, the user gives up, or gpg says
 * something no passphrase would fix.
 *
 * A box rather than the echo line, and for the same reason the key question is
 * one: this is asked while ccm is starting, before anything has been drawn, and
 * a prompt on the bottom row can be lost under whatever the language setup or
 * the theme has to say. It also masks what is typed, which is the point. */
static void crypt_ask_pass(const char *lead)
{
  buffer_t *b = &g_buffers[g_crypt_buf];
  const char *name = ccm_base_name(b->path);
  char pass[256], msg[PATH_MAX + 320], why[256];
  int tries, fatal = 0, ok;

  for (tries = 0; tries < CRYPT_TRIES; tries++) {
    if (lead) snprintf(msg, sizeof msg, "%s\n\n%s is encrypted.\nPassphrase to open it:", lead, name);
    else      snprintf(msg, sizeof msg, "%s is encrypted.\nPassphrase to open it:", name);

    if (!gtcaca_dialog_input("Encrypted file", msg, NULL, 1, pass, sizeof pass)) {
      snprintf(g_message, sizeof g_message,
               "%s left locked — C-x C-s will not write over it  (M-x revert-buffer asks again)", name);
      return;
    }
    ok = crypt_unlock(pass, &fatal) == 0;
    ccm_wipe(pass, sizeof pass);
    if (ok) return;

    snprintf(why, sizeof why, "%s", g_message);
    /* A wrong passphrase is nearly always a typo, so ask again — but not when
       gpg itself is what is wrong, where the answer would never change however
       often it is asked. */
    if (fatal) break;
    lead = why;
  }
  snprintf(g_message, sizeof g_message, "Still locked — %s", lead ? lead : "wrong passphrase");
}

/* ── the recipient ───────────────────────────────────────────────────────── */

/* What the box came back with. Split out from the box itself so the empty
   answer and the cancelled one read the same way. */
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
               "No key given — the file is left as it is  (M-x set-encryption-key asks again)");
    return;
  }

  strncpy(b->crypt_key, key, sizeof b->crypt_key - 1);
  b->crypt_key[sizeof b->crypt_key - 1] = '\0';
  b->crypt = 1;
  snprintf(g_message, sizeof g_message,
           "Encrypting to %s — C-x C-s writes it back encrypted", b->crypt_key);
}

/* Ask for a passphrase to encrypt with — twice, and keep it only if the two
 * agree.
 *
 * This is the half of the feature that has no safety net anywhere else. A
 * public key can always be used again because the secret half is in a keyring
 * that outlives the session; a passphrase typed once, masked, into a file that
 * is about to become ciphertext exists nowhere but in the user's head. A typo
 * in it is not an inconvenience, it is the file gone. So it is asked twice and
 * a mismatch simply asks again.
 *
 * Returns 1 with the agreed passphrase in `out`, 0 if the user backed out. */
static int crypt_choose_passphrase(const char *name, int how, char *out, size_t outsz)
{
  char first[256], again[256], msg[PATH_MAX + 200];
  const char *lead = "";
  int tries;

  for (tries = 0; tries < 4; tries++) {
    snprintf(msg, sizeof msg, "%s%s %s\nChoose a %spassphrase for it:", lead, name,
             how == CRYPT_ASK_NEW_FILE ? "does not exist yet — it will be created encrypted."
             : how == CRYPT_ASK_CHANGE ? "is already encrypted."
                                       : "will be written back encrypted from now on.",
             how == CRYPT_ASK_CHANGE ? "new " : "");
    if (!gtcaca_dialog_input("Encrypt with GnuPG", msg, NULL, 1, first, sizeof first))
      return 0;                                  /* cancelled */
    if (!first[0]) { lead = "An empty passphrase encrypts nothing.\n"; continue; }

    snprintf(msg, sizeof msg, "Type the passphrase again to confirm it.\n"
                              "There is no way to recover it if it is wrong.");
    if (!gtcaca_dialog_input("Encrypt with GnuPG", msg, NULL, 1, again, sizeof again)) {
      ccm_wipe(first, sizeof first);
      return 0;
    }
    if (!strcmp(first, again)) {
      snprintf(out, outsz, "%s", first);
      ccm_wipe(first, sizeof first);
      ccm_wipe(again, sizeof again);
      return 1;
    }
    ccm_wipe(first, sizeof first);
    ccm_wipe(again, sizeof again);
    lead = "The two did not match.\n";
  }
  return 0;
}

/* Ask which key to encrypt to, in a box in the middle of the screen.
 *
 * A dialog rather than the echo line because of when it is asked: `ccm -k
 * newfile` puts the question up before anything has been typed, and a prompt
 * on the bottom row is easy to start typing straight past — the answer decides
 * whether the file can be read again at all. It is also the one question here
 * that is not a secret, so the typing is not masked: a key id or an address is
 * meant to be checked before Enter.
 *
 * The box is pre-filled with whoever the file is already encrypted to, so the
 * common answer on an existing file is Enter. */
static void crypt_ask_key(void)
{
  buffer_t *b = &g_buffers[g_crypt_buf];
  const char *name = ccm_base_name(b->path);
  char msg[PATH_MAX + 160], key[sizeof b->crypt_key];
  struct stat st;

  g_crypt_askey = 0;
  if (stat(b->path, &st) != 0)
    snprintf(msg, sizeof msg,
             "%s does not exist yet — it will be created encrypted.\n"
             "Which key should it be encrypted to?", name);
  else
    snprintf(msg, sizeof msg,
             "%s will be written back encrypted from now on.\n"
             "Which key should it be encrypted to?", name);

  if (!gtcaca_dialog_input("Encrypt with GnuPG", msg, b->crypt_key, 0, key, sizeof key)) {
    if (b->crypt_key[0])
      snprintf(g_message, sizeof g_message, "Still encrypting to %s", b->crypt_key);
    else
      snprintf(g_message, sizeof g_message,
               "No key given — %s stays as it is  (M-x set-encryption-key asks again)", name);
    return;
  }
  crypt_key_done(key);
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
  crypt_ask_setup();
  crypt_flush_message();
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
  if (!b->crypt_key[0] && !b->crypt_pass[0]) {
    snprintf(g_message, sizeof g_message,
             "Nothing to encrypt with — M-x set-encryption-key asks for a key or a passphrase");
    return -1;
  }
  if (ccm_gpg_encrypt(path, b->crypt_key, b->crypt_pass, text, len, err, sizeof err) != 0) {
    snprintf(g_message, sizeof g_message, "Not saved: %s", err[0] ? err : "gpg failed");
    return -1;
  }
  return 0;
}

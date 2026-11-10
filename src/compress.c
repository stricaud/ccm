#include "cacamacs.h"
#include "hex.h"

/* ── compressed files ─────────────────────────────────────────────────────────
 *
 * Opening `notes.txt.gz` puts the *text* in the buffer and saving it puts the
 * compressed bytes back, the way Emacs' jka-compr does: the compression is how
 * the file is stored, not what is in it. The language still comes from the name
 * underneath (notes.txt.gz colours as notes.txt — see buffer_create).
 *
 * Three formats, each behind its own library and each optional at build time:
 * gzip (zlib), bzip2 (libbz2) and xz (liblzma). A build without one still opens
 * every other file; it just says so when asked for that format, before anything
 * has been read, so no file is left in a state ccm cannot undo.
 *
 * Everything here works on whole files in memory. Editor-sized files are small,
 * and one in-memory path serves both a compressed file opened directly and a
 * member decompressed out of an archive (see archive.c) — which has no file of
 * its own to stream from.
 *
 * Only gzip can be written. Reading the other two is what the archive browser
 * needs (.tar.bz2, .tar.xz), and a read-only format cannot round-trip, so a
 * .bz2 or .xz opens read-only rather than pretending it could be saved.
 */

#ifdef CCM_HAVE_ZLIB
#include <zlib.h>
#endif
#ifdef CCM_HAVE_BZ2
#include <bzlib.h>
#endif
#ifdef CCM_HAVE_LZMA
#include <lzma.h>
#endif

/* Grown by doubling from here; also the read chunk each library is given. */
#define COMP_CHUNK (128 * 1024)


/* ── which format? ──────────────────────────────────────────────────────────
 *
 * The extension decides, and the magic number is the fallback — a file called
 * `dump` that is really gzip still opens as text, and a *new* `notes.txt.gz`
 * that does not exist yet is still known to be gzip, which is what makes the
 * first save of one compress instead of writing plain text.
 *
 * Deliberately not consulted for a name ending in a format's extension twice
 * over (`.gz.gz`): one layer is all the editor peels, and guessing at more
 * would make the save ambiguous. */
int ccm_comp_format(const char *path)
{
  static const struct { const char *ext; int fmt; } EXTS[] = {
    { ".gz",   CCM_COMP_GZ  }, { ".tgz",  CCM_COMP_GZ  },
    { ".bz2",  CCM_COMP_BZ2 }, { ".tbz2", CCM_COMP_BZ2 }, { ".tbz", CCM_COMP_BZ2 },
    { ".xz",   CCM_COMP_XZ  }, { ".txz",  CCM_COMP_XZ  },
  };
  unsigned char head[6];
  const char *dot;
  size_t n, i;
  FILE *f;

  if (!path || !*path) return CCM_COMP_NONE;

  dot = strrchr(path, '.');
  if (dot) for (i = 0; i < sizeof EXTS / sizeof *EXTS; i++)
    if (!strcasecmp(dot, EXTS[i].ext)) return EXTS[i].fmt;

  f = fopen(path, "rb");
  if (!f) return CCM_COMP_NONE;
  n = fread(head, 1, sizeof head, f);
  fclose(f);

  if (n >= 2 && head[0] == 0x1f && head[1] == 0x8b) return CCM_COMP_GZ;
  if (n >= 3 && !memcmp(head, "BZh", 3))            return CCM_COMP_BZ2;
  if (n >= 6 && !memcmp(head, "\xfd" "7zXZ\0", 6))  return CCM_COMP_XZ;
  return CCM_COMP_NONE;
}

const char *ccm_comp_name(int fmt)
{
  switch (fmt) {
    case CCM_COMP_GZ:  return "gzip";
    case CCM_COMP_BZ2: return "bzip2";
    case CCM_COMP_XZ:  return "xz";
  }
  return "uncompressed";
}

int ccm_comp_supported(int fmt)
{
  switch (fmt) {
#ifdef CCM_HAVE_ZLIB
    case CCM_COMP_GZ:  return 1;
    case CCM_COMP_RAW: return 1;
#endif
#ifdef CCM_HAVE_BZ2
    case CCM_COMP_BZ2: return 1;
#endif
#ifdef CCM_HAVE_LZMA
    case CCM_COMP_XZ:  return 1;
#endif
    case CCM_COMP_NONE: return 1;
  }
  return 0;
}

/* Only gzip round-trips; the rest open read-only (see the note at the top). */
int ccm_comp_writable(int fmt)
{
  return fmt == CCM_COMP_GZ && ccm_comp_supported(fmt);
}

/* ── the growing output buffer every decompressor below writes into ────────── */

typedef struct { char *p; size_t len, cap; int capped; } obuf_t;

static int ob_room(obuf_t *o, size_t need)
{
  char *p;
  size_t want, min = o->len + need + 1;
  if (min <= o->cap) return 1;
  /* Refused before the allocation, not after: doubling onto a 512MB buffer asks
     the system for a gigabyte, so checking once the buffer is already too big
     would cost twice the ceiling in peak memory to enforce it. */
  if (min > CCM_COMP_MAX_BYTES) { o->capped = 1; return 0; }
  want = o->cap ? o->cap : COMP_CHUNK;
  while (want < min) {
    if (want > (size_t)-1 / 2) return 0;
    want *= 2;
  }
  if (want > CCM_COMP_MAX_BYTES) want = CCM_COMP_MAX_BYTES;   /* and no further */
  p = realloc(o->p, want);
  if (!p) return 0;
  o->p = p; o->cap = want;
  return 1;
}

/* ── per-format decompression of a whole block ─────────────────────────────── */

#ifdef CCM_HAVE_ZLIB
/* `raw` selects headerless deflate, which is what a zip member holds; otherwise
   a gzip (or zlib) wrapper is expected and auto-detected.
 *
 * Concatenated members are one stream as far as gunzip is concerned
 * (`gzip -c a b > both.gz`), so Z_STREAM_END with input still to come resets
 * the decoder and keeps going rather than stopping at the first member. */
static int inflate_block(const void *in, size_t inlen, int raw, obuf_t *o,
                         char *err, size_t errsz)
{
  z_stream zs;
  int rc;

  memset(&zs, 0, sizeof zs);
  if (inflateInit2(&zs, raw ? -MAX_WBITS : (16 + MAX_WBITS)) != Z_OK) {
    snprintf(err, errsz, "zlib would not start"); return -1;
  }
  zs.next_in  = (Bytef *)(void *)in;
  zs.avail_in = (uInt)inlen;

  for (;;) {
    if (!ob_room(o, COMP_CHUNK)) {
      inflateEnd(&zs); snprintf(err, errsz, "out of memory"); return -1;
    }
    zs.next_out  = (Bytef *)(o->p + o->len);
    zs.avail_out = (uInt)(o->cap - o->len - 1);
    rc = inflate(&zs, Z_NO_FLUSH);
    o->len = o->cap - 1 - zs.avail_out;

    if (rc == Z_STREAM_END) {
      if (zs.avail_in == 0) break;              /* done */
      if (inflateReset2(&zs, raw ? -MAX_WBITS : (16 + MAX_WBITS)) != Z_OK) break;
      continue;                                 /* another concatenated member */
    }
    if (rc == Z_BUF_ERROR && zs.avail_in == 0) break;  /* truncated but usable */
    if (rc != Z_OK) {
      /* A raw member ends when its bytes do, with no end marker to find. */
      if (raw && rc == Z_BUF_ERROR) break;
      inflateEnd(&zs);
      snprintf(err, errsz, "%s", zs.msg ? zs.msg : "not valid compressed data");
      return -1;
    }
  }
  inflateEnd(&zs);
  return 0;
}
#endif

#ifdef CCM_HAVE_BZ2
static int bunzip_block(const void *in, size_t inlen, obuf_t *o, char *err, size_t errsz)
{
  bz_stream bs;
  int rc;

  memset(&bs, 0, sizeof bs);
  if (BZ2_bzDecompressInit(&bs, 0, 0) != BZ_OK) {
    snprintf(err, errsz, "libbz2 would not start"); return -1;
  }
  bs.next_in  = (char *)(void *)in;
  bs.avail_in = (unsigned int)inlen;

  for (;;) {
    if (!ob_room(o, COMP_CHUNK)) {
      BZ2_bzDecompressEnd(&bs); snprintf(err, errsz, "out of memory"); return -1;
    }
    bs.next_out  = o->p + o->len;
    bs.avail_out = (unsigned int)(o->cap - o->len - 1);
    rc = BZ2_bzDecompress(&bs);
    o->len = o->cap - 1 - bs.avail_out;

    if (rc == BZ_STREAM_END) {
      if (bs.avail_in == 0) break;
      /* bzip2 concatenates too, and the API has no reset: start a new stream. */
      { char *rest = bs.next_in; unsigned int n = bs.avail_in;
        BZ2_bzDecompressEnd(&bs);
        memset(&bs, 0, sizeof bs);
        if (BZ2_bzDecompressInit(&bs, 0, 0) != BZ_OK) return 0;
        bs.next_in = rest; bs.avail_in = n; }
      continue;
    }
    if (rc != BZ_OK) {
      BZ2_bzDecompressEnd(&bs);
      snprintf(err, errsz, "not valid bzip2 data");
      return -1;
    }
    if (bs.avail_in == 0 && bs.avail_out != 0) break;   /* truncated but usable */
  }
  BZ2_bzDecompressEnd(&bs);
  return 0;
}
#endif

#ifdef CCM_HAVE_LZMA
static int unxz_block(const void *in, size_t inlen, obuf_t *o, char *err, size_t errsz)
{
  lzma_stream ls = LZMA_STREAM_INIT;
  lzma_ret rc;

  /* LZMA_CONCATENATED is what makes `xz -d` of a multi-stream file whole. */
  if (lzma_stream_decoder(&ls, (uint64_t)-1, LZMA_CONCATENATED) != LZMA_OK) {
    snprintf(err, errsz, "liblzma would not start"); return -1;
  }
  ls.next_in  = (const uint8_t *)in;
  ls.avail_in = inlen;

  for (;;) {
    if (!ob_room(o, COMP_CHUNK)) {
      lzma_end(&ls); snprintf(err, errsz, "out of memory"); return -1;
    }
    ls.next_out  = (uint8_t *)(o->p + o->len);
    ls.avail_out = o->cap - o->len - 1;
    rc = lzma_code(&ls, ls.avail_in ? LZMA_RUN : LZMA_FINISH);
    o->len = o->cap - 1 - ls.avail_out;

    if (rc == LZMA_STREAM_END) break;
    if (rc != LZMA_OK) {
      if (ls.avail_in == 0) break;              /* truncated but usable */
      lzma_end(&ls);
      snprintf(err, errsz, "not valid xz data");
      return -1;
    }
  }
  lzma_end(&ls);
  return 0;
}
#endif

/* Decompress `in` as `fmt`. Returns a malloc'd NUL-terminated block (the NUL is
 * past *outlen, so text goes straight into the editor) or NULL with `err` set.
 *
 * A short or truncated stream gives back what did decode rather than nothing: a
 * half-written .gz is exactly when you most want to see the beginning of it. */
char *ccm_comp_read_mem(const void *in, size_t inlen, int fmt, size_t *outlen,
                        char *err, size_t errsz)
{
  obuf_t o;
  int rc = -1;

  o.p = NULL; o.len = 0; o.cap = 0; o.capped = 0;
  if (outlen) *outlen = 0;
  if (err && errsz) err[0] = '\0';

  if (!ccm_comp_supported(fmt)) {
    snprintf(err, errsz, "this build of ccm has no %s support", ccm_comp_name(fmt));
    return NULL;
  }

  switch (fmt) {
    case CCM_COMP_NONE:
      if (!ob_room(&o, inlen)) { snprintf(err, errsz, "out of memory"); return NULL; }
      memcpy(o.p, in, inlen); o.len = inlen; rc = 0;
      break;
#ifdef CCM_HAVE_ZLIB
    case CCM_COMP_GZ:  rc = inflate_block(in, inlen, 0, &o, err, errsz); break;
    case CCM_COMP_RAW: rc = inflate_block(in, inlen, 1, &o, err, errsz); break;
#endif
#ifdef CCM_HAVE_BZ2
    case CCM_COMP_BZ2: rc = bunzip_block(in, inlen, &o, err, errsz); break;
#endif
#ifdef CCM_HAVE_LZMA
    case CCM_COMP_XZ:  rc = unxz_block(in, inlen, &o, err, errsz); break;
#endif
    default:
      snprintf(err, errsz, "unknown compression");
      break;
  }

  if (rc != 0) {
    if (o.capped)
      snprintf(err, errsz, "decompresses to more than ccm will hold in memory");
    free(o.p); return NULL;
  }
  if (!ob_room(&o, 0)) { free(o.p); snprintf(err, errsz, "out of memory"); return NULL; }
  o.p[o.len] = '\0';
  if (outlen) *outlen = o.len;
  return o.p;
}

/* Read `path` whole, with no decompression. Shared with archive.c, which needs
 * the raw container before it can know what is inside it. */
char *ccm_slurp(const char *path, size_t *outlen, char *err, size_t errsz)
{
  FILE *f;
  char *buf;
  long n;
  size_t got;

  if (outlen) *outlen = 0;
  f = fopen(path, "rb");
  if (!f) { snprintf(err, errsz, "cannot read %s", ccm_base_name(path)); return NULL; }
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); snprintf(err, errsz, "%s is not a seekable file", ccm_base_name(path)); return NULL; }
  n = ftell(f);
  if (n < 0) n = 0;
  rewind(f);
  buf = malloc((size_t)n + 1);
  if (!buf) { fclose(f); snprintf(err, errsz, "out of memory"); return NULL; }
  got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = '\0';
  if (outlen) *outlen = got;
  return buf;
}

char *ccm_comp_read_file(const char *path, int fmt, size_t *outlen, char *err, size_t errsz)
{
  char *raw, *out;
  size_t rawlen;

  raw = ccm_slurp(path, &rawlen, err, errsz);
  if (!raw) return NULL;
  out = ccm_comp_read_mem(raw, rawlen, fmt, outlen, err, errsz);
  free(raw);
  return out;
}

/* ── writing ────────────────────────────────────────────────────────────────── */

#ifdef CCM_HAVE_ZLIB
/* Compress to a sibling temp file and rename it over `path`.
 *
 * Not for tidiness: compressing straight into the file would truncate it first,
 * and a failure anywhere after that (a full disk, most likely, since compressed
 * output is written as it is produced) would leave a half-finished .gz in place
 * of the original — unrecoverable, because the original is the only copy of what
 * the half-file was supposed to contain. rename() is atomic within a
 * filesystem, so the file is either the old one or the whole new one. */
static int gz_write_file(const char *path, const void *data, size_t len,
                         char *err, size_t errsz)
{
  char tmp[PATH_MAX];
  gzFile z;
  const char *p = (const char *)data;
  size_t sent = 0;
  int need;

  need = snprintf(tmp, sizeof tmp, "%s.ccm-tmp", path);
  if (need < 0 || need >= (int)sizeof tmp) { snprintf(err, errsz, "path too long"); return -1; }

  z = gzopen(tmp, "wb");
  if (!z) { snprintf(err, errsz, "cannot write %s", ccm_base_name(tmp)); return -1; }

  /* gzwrite takes an unsigned int and treats a 0 length as an error, so an
     empty buffer writes nothing at all and still produces a valid empty .gz. */
  while (sent < len) {
    unsigned int chunk = (unsigned int)(len - sent > COMP_CHUNK ? COMP_CHUNK : len - sent);
    if (gzwrite(z, p + sent, chunk) != (int)chunk) {
      int e; const char *m = gzerror(z, &e);
      snprintf(err, errsz, "%s", (m && *m) ? m : "write failed");
      gzclose(z); remove(tmp);
      return -1;
    }
    sent += chunk;
  }
  if (gzclose(z) != Z_OK) {
    snprintf(err, errsz, "could not finish writing %s", ccm_base_name(tmp));
    remove(tmp);
    return -1;
  }

  /* Windows will not rename onto an existing file. */
#ifdef _WIN32
  remove(path);
#endif
  if (rename(tmp, path) != 0) {
    snprintf(err, errsz, "cannot replace %s", ccm_base_name(path));
    remove(tmp);
    return -1;
  }
  return 0;
}
#endif

int ccm_comp_write_file(const char *path, int fmt, const void *data, size_t len,
                        char *err, size_t errsz)
{
  if (err && errsz) err[0] = '\0';
  if (!ccm_comp_writable(fmt)) {
    snprintf(err, errsz, "ccm cannot write %s files", ccm_comp_name(fmt));
    return -1;
  }
#ifdef CCM_HAVE_ZLIB
  return gz_write_file(path, data, len, err, errsz);
#else
  (void)path; (void)data; (void)len;
  snprintf(err, errsz, "this build of ccm has no gzip support");
  return -1;
#endif
}

/* ── is this text? ──────────────────────────────────────────────────────────
 *
 * The editor holds a C string, so a NUL byte in the content would end the text
 * early — and saving that back would write a *shorter* file than was opened,
 * destroying everything past the NUL. For a plain file that is the user's own
 * doing; for a compressed one it is a trap, because `notes.tar.gz` looks like
 * something to open and decompresses to a tar archive full of NULs. So content
 * with a NUL in it is never writable, and the caller opens it read-only. */
int ccm_is_text(const char *p, size_t n)
{
  size_t i;
  if (!p) return 0;
  for (i = 0; i < n; i++) if (p[i] == '\0') return 0;
  return 1;
}

/* ── the buffer side ───────────────────────────────────────────────────────── */

/* Fill buffer `bi` from a compressed file. Called from buffer_create in place
 * of the plain read, having already decided the file is compressed.
 *
 * Anything to tell the user goes in b->comp_note rather than straight to
 * g_message: buffer_create still has the language to set up after this, and
 * setup_language has a message of its own for a name it does not recognise
 * ("No language for '.gz'"), which would land on top of this one.
 *
 * Failure leaves the buffer empty and read-only rather than falling back to the
 * compressed bytes: showing a screenful of binary that a later C-x C-s would
 * write back as text is worse than showing nothing. */
void ccm_comp_open(int bi, int fmt)
{
  buffer_t *b = &g_buffers[bi];
  char err[256] = "";
  char *text;
  size_t len = 0;
  struct stat st;

  b->comp = fmt;

  /* A name that is not there yet, or is there but empty: this is a new file,
     not a broken one. `ccm notes.txt.gz` on a fresh name has to come up as an
     ordinary empty buffer that compresses on its first save — treating the
     missing file as a failed decompression would open it read-only and there
     would be no way to create it. */
  if (stat(b->path, &st) != 0 || st.st_size == 0) {
    gtcaca_editor_set_text(b->ed, "");
    return;
  }

  text = ccm_comp_read_file(b->path, fmt, &len, err, sizeof err);

  if (!text) {
    gtcaca_editor_set_text(b->ed, "");
    gtcaca_editor_set_read_only(b->ed, 1);
    b->comp_ro = 1;
    snprintf(b->comp_note, sizeof b->comp_note, "%s: %s",
             ccm_base_name(b->path), err[0] ? err : "cannot decompress");
    return;
  }

  if (!ccm_is_text(text, len)) {
    gtcaca_editor_set_text(b->ed, "");
    gtcaca_editor_set_read_only(b->ed, 1);
    b->comp_ro = 1;
    /* A .tar.gz is somewhere to go, not something to read: say so, since the
       browser can step inside it and a hex dump of a tar is nobody's answer. */
    if (ccm_is_archive(b->path)) {
      free(text);
      snprintf(b->comp_note, sizeof b->comp_note,
               "%s holds binary data, not text — C-x d browses it as an archive",
               ccm_base_name(b->path));
      return;
    }
    /* Anything else — data.bin.gz, an image, a compressed core — is bytes, and
       bytes are something ccm can show: hex mode, read-only, because what ccm
       writes back through the compressor is text (see ccm_comp_write). C-x C-w
       puts the bytes themselves in a file of their own. */
    { char note[192];
      snprintf(note, sizeof note,
               "%s is %s binary data — shown as hex, read-only (C-x C-w writes the bytes out)",
               ccm_base_name(b->path), ccm_comp_name(fmt));
      if (ccm_hex_adopt(bi, (uint8_t *)text, (long)len, 1, note) == 0) return; }
    /* Hex mode would not have it either (out of memory): leave the buffer as
       it is, empty and read-only, and say what it holds. */
    snprintf(b->comp_note, sizeof b->comp_note,
             "%s holds binary data, not text", ccm_base_name(b->path));
    return;
  }

  gtcaca_editor_set_text(b->ed, text);
  free(text);

  if (!ccm_comp_writable(fmt)) {
    gtcaca_editor_set_read_only(b->ed, 1);
    b->comp_ro = 1;
    snprintf(b->comp_note, sizeof b->comp_note,
             "%s is %s — ccm only writes gzip, so this is read-only (C-x C-w saves it elsewhere)",
             ccm_base_name(b->path), ccm_comp_name(fmt));
  }
}

/* Called from write_buffer_file for a compressed buffer. 0 when it has written
 * the file, -1 when the caller must not — a plain write here would drop a text
 * file where a compressed one belongs, under a name that still says .gz. */
int ccm_comp_write(int bi, const char *path, const char *text, size_t len)
{
  buffer_t *b = &g_buffers[bi];
  char err[256] = "";

  if (b->comp_ro) {
    snprintf(g_message, sizeof g_message, "%s was opened read-only — not saved",
             ccm_base_name(path));
    return -1;
  }
  if (!ccm_comp_writable(b->comp)) {
    snprintf(g_message, sizeof g_message, "Not saved: ccm cannot write %s files",
             ccm_comp_name(b->comp));
    return -1;
  }
  if (ccm_comp_write_file(path, b->comp, text, len, err, sizeof err) != 0) {
    snprintf(g_message, sizeof g_message, "Not saved: %s", err[0] ? err : "compression failed");
    return -1;
  }
  return 0;
}

#include "cacamacs.h"

/* ── archives browsed as directories ──────────────────────────────────────────
 *
 * Opening a .zip or a .tar.gz with C-x C-f (or walking onto one in the C-x d
 * browser) steps *into* it: the listing shows what is inside, Enter on a folder
 * descends, Enter on a file opens it, and ".." at the top comes back out to the
 * directory the archive sits in. A member opens read-only — ccm reads archives
 * and does not rewrite them, so there is nothing a save could do but mislead.
 *
 * Two containers, and the difference between them shapes everything here.
 *
 * A zip has an index. The central directory at the end of the file lists every
 * member with the offset of its bytes, so the archive is read once for the
 * listing and each member is inflated on demand, out of the raw file kept in
 * memory.
 *
 * A tar has none: it is a stream of 512-byte headers each followed by its data,
 * so the only way to find the last member is to walk past all the others. And
 * when it is compressed (.tar.gz and friends) that walk has to happen on the
 * *decompressed* bytes — there is no seeking into a gzip stream. So a tar is
 * decompressed whole into memory once, and the entry list points into it.
 *
 * Either way one archive is cached at a time, which is all the browser ever
 * looks at, and entering another drops the previous one.
 *
 * Nothing here extracts to disk. A member goes from the cached bytes straight
 * into a buffer, so no temporary file is left behind if ccm is killed, and an
 * archive is never partly unpacked somewhere the user did not ask for.
 */

/* An archive this big is not something to pull into memory; the number is a
   guard against a mistake (a sparse image, a disk dump) rather than a limit
   anyone should meet. The same ceiling compress.c puts on one file, for the
   same reason — a .tar.gz is a decompression bomb like any other. */
#define ARC_MAX_BYTES CCM_COMP_MAX_BYTES
#define ARC_MAX_ENTRIES 65536
#define TAR_BLOCK 512

enum { ARC_NONE = 0, ARC_ZIP, ARC_TAR };

typedef struct {
  char      name[PATH_MAX];  /* full path inside the archive, no leading '/' */
  long long size;            /* uncompressed */
  long long csize;           /* zip: compressed size */
  long long offset;          /* zip: local header offset; tar: data offset */
  int       method;          /* zip: 0 stored, 8 deflated */
  int       isdir;
} arc_entry_t;

/* The one cached archive. `blob` is the raw file for a zip and the decompressed
   tar stream for a tar; either way every entry offset points into it. */
static struct {
  char         path[PATH_MAX];
  int          kind;            /* ARC_ZIP / ARC_TAR */
  char        *blob;
  size_t       bloblen;
  arc_entry_t *ents;
  int          nents;
} g_arc;

/* ── little-endian reads out of the blob ───────────────────────────────────── */

static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned long long rd32(const unsigned char *p)
{
  return (unsigned long long)p[0] | ((unsigned long long)p[1] << 8)
       | ((unsigned long long)p[2] << 16) | ((unsigned long long)p[3] << 24);
}

/* ── which container? ───────────────────────────────────────────────────────── */

/* The name decides, and the bytes are the fallback, same as ccm_comp_format.
 * Returns ARC_ZIP / ARC_TAR and sets *comp to the compression wrapped around it
 * (CCM_COMP_NONE for a plain .tar or .zip — a zip compresses its members
 * individually, so the container itself is never wrapped). */
static int archive_kind(const char *path, int *comp)
{
  static const struct { const char *ext; int kind; int comp; } EXTS[] = {
    { ".zip",    ARC_ZIP, CCM_COMP_NONE },
    { ".jar",    ARC_ZIP, CCM_COMP_NONE },   /* and every other zip in disguise */
    { ".whl",    ARC_ZIP, CCM_COMP_NONE },
    { ".apk",    ARC_ZIP, CCM_COMP_NONE },
    { ".tar",    ARC_TAR, CCM_COMP_NONE },
    { ".tgz",    ARC_TAR, CCM_COMP_GZ   },
    { ".tbz",    ARC_TAR, CCM_COMP_BZ2  },
    { ".tbz2",   ARC_TAR, CCM_COMP_BZ2  },
    { ".txz",    ARC_TAR, CCM_COMP_XZ   },
  };
  static const struct { const char *ext; int comp; } TARBALLS[] = {
    { ".tar.gz",  CCM_COMP_GZ  },
    { ".tar.bz2", CCM_COMP_BZ2 },
    { ".tar.xz",  CCM_COMP_XZ  },
  };
  unsigned char head[264];
  const char *dot;
  size_t n, i, plen;
  FILE *f;

  *comp = CCM_COMP_NONE;
  if (!path || !*path) return ARC_NONE;
  plen = strlen(path);

  /* The two-part ones first: ".tar.gz" has to beat the ".gz" that strrchr sees. */
  for (i = 0; i < sizeof TARBALLS / sizeof *TARBALLS; i++) {
    size_t el = strlen(TARBALLS[i].ext);
    if (plen > el && !strcasecmp(path + plen - el, TARBALLS[i].ext)) {
      *comp = TARBALLS[i].comp;
      return ARC_TAR;
    }
  }
  dot = strrchr(path, '.');
  if (dot) for (i = 0; i < sizeof EXTS / sizeof *EXTS; i++)
    if (!strcasecmp(dot, EXTS[i].ext)) { *comp = EXTS[i].comp; return EXTS[i].kind; }

  /* Unhelpfully named: ask the bytes. A compressed tar cannot be recognised
     this cheaply (the tar magic is 257 bytes into the *decompressed* stream),
     so a plain .gz is left to compress.c and only revealed as a tarball if
     decompressing it turns up a tar header — see ccm_archive_enter. */
  f = fopen(path, "rb");
  if (!f) return ARC_NONE;
  n = fread(head, 1, sizeof head, f);
  fclose(f);
  if (n >= 4 && !memcmp(head, "PK\x03\x04", 4)) return ARC_ZIP;
  if (n >= 4 && !memcmp(head, "PK\x05\x06", 4)) return ARC_ZIP;   /* empty zip */
  if (n >= 262 && !memcmp(head + 257, "ustar", 5)) return ARC_TAR;
  return ARC_NONE;
}

int ccm_is_archive(const char *path)
{
  int comp;
  return archive_kind(path, &comp) != ARC_NONE;
}

/* ── entry list ─────────────────────────────────────────────────────────────── */

static int arc_push(arc_entry_t **ents, int *n, int *cap, const arc_entry_t *e)
{
  if (*n >= ARC_MAX_ENTRIES) return 0;
  if (*n >= *cap) {
    int want = *cap ? *cap * 2 : 128;
    arc_entry_t *p = realloc(*ents, (size_t)want * sizeof **ents);
    if (!p) return 0;
    *ents = p; *cap = want;
  }
  (*ents)[(*n)++] = *e;
  return 1;
}

/* Strip the leading "./" and any leading '/' that a tar may carry, so every
   name in the list is relative to the archive root and comparable. */
static void arc_clean_name(char *s)
{
  char *p = s;
  while (*p == '/') p++;
  while (p[0] == '.' && p[1] == '/') { p += 2; while (*p == '/') p++; }
  if (p != s) memmove(s, p, strlen(p) + 1);
}

/* ── tar ────────────────────────────────────────────────────────────────────── */

/* Tar stores numbers as octal in a fixed field, usually NUL- or space-padded.
   GNU's base-256 extension (high bit set on the first byte) turns up for sizes
   over 8GB, which nothing here will be reading, so it is simply refused rather
   than silently misread. */
static long long tar_octal(const unsigned char *p, size_t n)
{
  long long v = 0;
  size_t i = 0;
  if (n && (p[0] & 0x80)) return -1;             /* base-256: not supported */
  while (i < n && (p[i] == ' ' || p[i] == '0')) {
    if (p[i] == '0') break;
    i++;
  }
  for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (p[i] - '0');
  return v;
}

static int tar_scan(const char *blob, size_t len, arc_entry_t **out, int *nout,
                    char *err, size_t errsz)
{
  arc_entry_t *ents = NULL;
  int n = 0, cap = 0;
  size_t pos = 0;
  char longname[PATH_MAX] = "";

  while (pos + TAR_BLOCK <= len) {
    const unsigned char *h = (const unsigned char *)blob + pos;
    arc_entry_t e;
    long long size;
    int type;
    size_t i;

    /* Two zero blocks end the archive; one is enough to stop on. */
    for (i = 0; i < TAR_BLOCK && h[i] == 0; i++) ;
    if (i == TAR_BLOCK) break;

    size = tar_octal(h + 124, 12);
    if (size < 0) { snprintf(err, errsz, "unsupported tar field (archive too large?)"); break; }
    type = h[156];
    pos += TAR_BLOCK;

    /* GNU long name: this header carries the *next* entry's name as its data. */
    if (type == 'L') {
      size_t want = (size_t)size < sizeof longname - 1 ? (size_t)size : sizeof longname - 1;
      if (pos + (size_t)size > len) break;
      memcpy(longname, blob + pos, want);
      longname[want] = '\0';
      pos += ((size_t)size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
      continue;
    }
    /* pax/vendor metadata ('x', 'g') and the GNU long *link* name ('K'):
       nothing the listing needs, so step over the data and move on. */
    if (type == 'x' || type == 'g' || type == 'K') {
      pos += ((size_t)size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
      continue;
    }

    memset(&e, 0, sizeof e);
    if (longname[0]) {
      snprintf(e.name, sizeof e.name, "%s", longname);
      longname[0] = '\0';
    } else {
      /* ustar splits a long name into prefix + name; POSIX says join with '/'. */
      char nm[101], pre[156];
      memcpy(nm, h, 100); nm[100] = '\0';
      memcpy(pre, h + 345, 155); pre[155] = '\0';
      if (!memcmp(h + 257, "ustar", 5) && pre[0])
        snprintf(e.name, sizeof e.name, "%s/%s", pre, nm);
      else
        snprintf(e.name, sizeof e.name, "%s", nm);
    }
    arc_clean_name(e.name);

    e.size   = size;
    e.offset = (long long)pos;
    e.method = 0;
    e.csize  = size;
    /* '5' is a directory; so is any name ending in '/'. Hard and symbolic links
       ('1', '2') have no data of their own — listed, but empty when opened. */
    e.isdir  = (type == '5') || (e.name[0] && e.name[strlen(e.name) - 1] == '/');
    if (e.isdir) { size_t l = strlen(e.name); while (l && e.name[l - 1] == '/') e.name[--l] = '\0'; }

    if (e.name[0] && !arc_push(&ents, &n, &cap, &e)) break;

    /* Data is padded out to a whole number of blocks. */
    pos += ((size_t)size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
  }

  if (n == 0) {
    free(ents);
    snprintf(err, errsz, "no entries found — not a tar archive?");
    return -1;
  }
  *out = ents; *nout = n;
  return 0;
}

/* ── zip ────────────────────────────────────────────────────────────────────── */

static int zip_scan(const char *blob, size_t len, arc_entry_t **out, int *nout,
                    char *err, size_t errsz)
{
  const unsigned char *b = (const unsigned char *)blob;
  arc_entry_t *ents = NULL;
  int n = 0, cap = 0, i, count;
  size_t eocd, cd, pos, back;
  unsigned long long cdoff;

  /* The End of Central Directory record is last, but a trailing comment of up
     to 64K may follow it, so it has to be searched for backwards. */
  if (len < 22) { snprintf(err, errsz, "too short to be a zip"); return -1; }
  back = len < 22 + 65535 ? len : 22 + 65535;
  eocd = (size_t)-1;
  for (pos = len - 22 + 1; pos-- > len - back; )
    if (!memcmp(b + pos, "PK\x05\x06", 4)) { eocd = pos; break; }
  if (eocd == (size_t)-1) { snprintf(err, errsz, "no zip directory found"); return -1; }

  count = (int)rd16(b + eocd + 10);
  cdoff = rd32(b + eocd + 16);

  /* Both fields saturate at 0xffffffff/0xffff in a zip64 archive, where the
     real values live in a separate record this does not read. Saying so beats
     listing a wrong subset of a 5GB archive as though it were all of it. */
  if (cdoff == 0xffffffffull || rd16(b + eocd + 10) == 0xffff) {
    snprintf(err, errsz, "zip64 archives are not supported");
    return -1;
  }
  if (cdoff >= len) { snprintf(err, errsz, "zip directory is out of range"); return -1; }

  cd = (size_t)cdoff;
  for (i = 0; i < count && cd + 46 <= len; i++) {
    arc_entry_t e;
    size_t namelen, extralen, commentlen;
    unsigned long long usize, csize, lho;

    if (memcmp(b + cd, "PK\x01\x02", 4)) break;
    e.method   = (int)rd16(b + cd + 10);
    csize      = rd32(b + cd + 20);
    usize      = rd32(b + cd + 24);
    namelen    = rd16(b + cd + 28);
    extralen   = rd16(b + cd + 30);
    commentlen = rd16(b + cd + 32);
    lho        = rd32(b + cd + 42);

    if (cd + 46 + namelen > len) break;
    if (namelen >= sizeof e.name) namelen = sizeof e.name - 1;
    memcpy(e.name, b + cd + 46, namelen);
    e.name[namelen] = '\0';

    e.size   = (long long)usize;
    e.csize  = (long long)csize;
    e.offset = (long long)lho;
    e.isdir  = (namelen > 0 && e.name[namelen - 1] == '/');
    arc_clean_name(e.name);
    if (e.isdir) { size_t l = strlen(e.name); while (l && e.name[l - 1] == '/') e.name[--l] = '\0'; }

    if (e.name[0] && !arc_push(&ents, &n, &cap, &e)) break;
    cd += 46 + namelen + extralen + commentlen;
  }

  *out = ents; *nout = n;
  return 0;
}

/* Inflate one zip member out of the cached file. The central directory gives
 * the *local* header's offset, and the data starts past that header — whose
 * name and extra fields may differ in length from the central copy, so they are
 * read again here rather than assumed. */
static char *zip_member(const arc_entry_t *e, size_t *outlen, char *err, size_t errsz)
{
  const unsigned char *b = (const unsigned char *)g_arc.blob;
  size_t off = (size_t)e->offset, namelen, extralen, data;

  if (off + 30 > g_arc.bloblen || memcmp(b + off, "PK\x03\x04", 4)) {
    snprintf(err, errsz, "member header is missing or damaged");
    return NULL;
  }
  namelen  = rd16(b + off + 26);
  extralen = rd16(b + off + 28);
  data     = off + 30 + namelen + extralen;
  if (data + (size_t)e->csize > g_arc.bloblen) {
    snprintf(err, errsz, "member runs past the end of the archive");
    return NULL;
  }

  if (e->method == 0)                 /* stored */
    return ccm_comp_read_mem(b + data, (size_t)e->csize, CCM_COMP_NONE, outlen, err, errsz);
  if (e->method == 8)                 /* deflated */
    return ccm_comp_read_mem(b + data, (size_t)e->csize, CCM_COMP_RAW, outlen, err, errsz);

  snprintf(err, errsz, "member uses compression method %d, which ccm cannot read", e->method);
  return NULL;
}

/* ── the cached archive ─────────────────────────────────────────────────────── */

void ccm_archive_close(void)
{
  free(g_arc.blob);
  free(g_arc.ents);
  memset(&g_arc, 0, sizeof g_arc);
}

const char *ccm_archive_path(void) { return g_arc.path; }
int ccm_archive_is_open(void) { return g_arc.path[0] != '\0'; }

/* Load `path` as the cached archive. 0 on success, -1 with `err` set otherwise.
 * Re-entering the archive already loaded is free. */
int ccm_archive_enter(const char *path, char *err, size_t errsz)
{
  int kind, comp, rc;
  char *raw = NULL, *blob = NULL;
  size_t rawlen = 0, bloblen = 0;
  arc_entry_t *ents = NULL;
  int nents = 0;
  struct stat st;

  if (err && errsz) err[0] = '\0';
  if (g_arc.path[0] && !strcmp(g_arc.path, path)) return 0;

  kind = archive_kind(path, &comp);
  if (kind == ARC_NONE) {
    /* A plain .gz may still hold a tar (`gzip` of a .tar, renamed). */
    if (ccm_comp_format(path) != CCM_COMP_NONE) { kind = ARC_TAR; comp = ccm_comp_format(path); }
    else { snprintf(err, errsz, "%s is not an archive ccm can read", ccm_base_name(path)); return -1; }
  }
  if (comp != CCM_COMP_NONE && !ccm_comp_supported(comp)) {
    snprintf(err, errsz, "this build of ccm has no %s support", ccm_comp_name(comp));
    return -1;
  }
  if (kind == ARC_ZIP && !ccm_comp_supported(CCM_COMP_RAW)) {
    snprintf(err, errsz, "this build of ccm has no zlib, so it cannot read zip files");
    return -1;
  }

  if (stat(path, &st) == 0 && (unsigned long long)st.st_size > ARC_MAX_BYTES) {
    snprintf(err, errsz, "%s is too large to browse in memory", ccm_base_name(path));
    return -1;
  }

  raw = ccm_slurp(path, &rawlen, err, errsz);
  if (!raw) return -1;

  if (comp == CCM_COMP_NONE) {
    blob = raw; bloblen = rawlen; raw = NULL;
  } else {
    blob = ccm_comp_read_mem(raw, rawlen, comp, &bloblen, err, errsz);
    free(raw); raw = NULL;
    if (!blob) return -1;
    if (bloblen > ARC_MAX_BYTES) {
      free(blob);
      snprintf(err, errsz, "%s decompresses to more than ccm will hold in memory", ccm_base_name(path));
      return -1;
    }
  }

  /* The cache has to be live before scanning a zip: zip_member reads the blob
     back through it, and a zip's listing is built from offsets into the file. */
  ccm_archive_close();
  g_arc.blob = blob; g_arc.bloblen = bloblen; g_arc.kind = kind;
  snprintf(g_arc.path, sizeof g_arc.path, "%s", path);

  rc = (kind == ARC_ZIP) ? zip_scan(blob, bloblen, &ents, &nents, err, errsz)
                         : tar_scan(blob, bloblen, &ents, &nents, err, errsz);
  if (rc != 0) { ccm_archive_close(); return -1; }

  g_arc.ents = ents; g_arc.nents = nents;
  return 0;
}

/* ── listing one level ──────────────────────────────────────────────────────── */

static int cmp_child(const void *a, const void *b)
{
  const ccm_arc_child_t *x = a, *y = b;
  if (x->isdir != y->isdir) return y->isdir - x->isdir;   /* directories first */
  return strcasecmp(x->name, y->name);
}

/* The entries directly under `sub` ("" is the archive root).
 *
 * An archive is a flat list of paths, and plenty of them name no directories at
 * all — a zip written by some tools has `src/main.c` with no `src/` entry. So
 * the intermediate directories are synthesised here: anything under `sub` whose
 * remainder still contains a '/' contributes its first component as a folder,
 * listed once however many members are under it. */
int ccm_archive_children(const char *sub, ccm_arc_child_t **out, int *nout)
{
  ccm_arc_child_t *kids = NULL;
  int nk = 0, cap = 0, i, j;
  size_t sublen;

  *out = NULL; *nout = 0;
  if (!g_arc.path[0]) return -1;
  sublen = sub ? strlen(sub) : 0;

  for (i = 0; i < g_arc.nents; i++) {
    const char *name = g_arc.ents[i].name;
    const char *rest;
    const char *slash;
    ccm_arc_child_t k;

    if (sublen) {
      if (strncmp(name, sub, sublen) != 0 || name[sublen] != '/') continue;
      rest = name + sublen + 1;
    } else {
      rest = name;
    }
    if (!*rest) continue;

    memset(&k, 0, sizeof k);
    slash = strchr(rest, '/');
    if (slash) {
      size_t l = (size_t)(slash - rest);
      if (l >= sizeof k.name) l = sizeof k.name - 1;
      memcpy(k.name, rest, l); k.name[l] = '\0';
      k.isdir = 1;
      k.size = 0;
    } else {
      snprintf(k.name, sizeof k.name, "%s", rest);
      k.isdir = g_arc.ents[i].isdir;
      k.size  = g_arc.ents[i].size;
    }

    /* One line per name: a synthesised folder is reached from every member
       under it, and a zip may list a directory entry as well as its contents. */
    for (j = 0; j < nk; j++) if (!strcmp(kids[j].name, k.name)) break;
    if (j < nk) { if (k.isdir) kids[j].isdir = 1; continue; }

    if (nk >= cap) {
      int want = cap ? cap * 2 : 64;
      ccm_arc_child_t *p = realloc(kids, (size_t)want * sizeof *p);
      if (!p) break;
      kids = p; cap = want;
    }
    kids[nk++] = k;
  }

  if (nk > 1) qsort(kids, (size_t)nk, sizeof *kids, cmp_child);
  *out = kids; *nout = nk;
  return 0;
}

/* ── one member's bytes ─────────────────────────────────────────────────────── */

char *ccm_archive_extract(const char *member, size_t *outlen, char *err, size_t errsz)
{
  int i;

  if (outlen) *outlen = 0;
  if (err && errsz) err[0] = '\0';
  if (!g_arc.path[0]) { snprintf(err, errsz, "no archive is open"); return NULL; }

  for (i = 0; i < g_arc.nents; i++) {
    const arc_entry_t *e = &g_arc.ents[i];
    if (strcmp(e->name, member)) continue;
    if (e->isdir) { snprintf(err, errsz, "%s is a folder", member); return NULL; }

    if (g_arc.kind == ARC_ZIP) return zip_member(e, outlen, err, errsz);

    /* tar: the data is already sitting in the decompressed blob. */
    if ((size_t)e->offset + (size_t)e->size > g_arc.bloblen) {
      snprintf(err, errsz, "%s runs past the end of the archive", member);
      return NULL;
    }
    return ccm_comp_read_mem(g_arc.blob + e->offset, (size_t)e->size,
                             CCM_COMP_NONE, outlen, err, errsz);
  }
  snprintf(err, errsz, "%s is not in this archive", member);
  return NULL;
}

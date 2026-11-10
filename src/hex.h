#ifndef CCM_HEX_H
#define CCM_HEX_H

#include <stdint.h>
#include <stddef.h>

/* ── hex mode ────────────────────────────────────────────────────────────────
 *
 * A buffer visiting a binary file shows the gtcaca `hexview` widget — rows of
 * "OFFSET  hh hh … hh  |ascii|" — instead of the editor widget, and the bytes
 * themselves are the document. Everything else about it is an ordinary ccm
 * buffer: it lives in a pane, C-x o and C-x b reach it, the modeline describes
 * it, C-x C-s writes it and C-x C-c asks about it if it is unsaved.
 *
 * hex.c holds the whole implementation; what the rest of ccm needs of it is
 * here. A buffer is in hex mode exactly when `b->hex` is non-NULL.
 */

/* What hex mode will hold in memory. A hex buffer *is* its bytes: searching and
   editing both want the whole file at once, so the file is read in full rather
   than paged, and a file past this is turned down with a message instead of
   being opened as something it cannot edit. */
#define CCM_HEX_MAX_BYTES (256ull * 1024 * 1024)

/* Does this file have a NUL byte in it — i.e. would the editor, which holds a C
   string, truncate it? That is the whole test for "binary" here, and it is the
   same one ccm_is_text applies to what comes out of a decompressor. */
int  ccm_file_is_binary(const char *path);

/* Turn buffer `bi` into a hex buffer over `bytes` (taking ownership of the
   malloc'd block; freed by ccm_hex_close). `ro` opens it read-only — an archive
   member, or anything else there is no way to write back. `note`, when given,
   is why, and is shown once the buffer is up. Returns 0 on success. */
int  ccm_hex_adopt(int bi, uint8_t *bytes, long len, int ro, const char *note);
/* The same, reading b->path. Reports why in g_message and returns -1 if it
   cannot (too large, unreadable). */
int  ccm_hex_open_file(int bi, int ro, const char *note);
void ccm_hex_close(int bi);                 /* drop the bytes and the widgets */

int  ccm_hex_modified(int bi);              /* edited since visiting/saving */
int  ccm_hex_write(int bi, const char *path);/* the write itself; 0 on success */
/* C-x C-w: write the bytes to `path` even if the buffer was read-only, which is
   how a hex buffer with nowhere to go gets a file of its own. */
int  ccm_hex_write_as(int bi, const char *path);
void ccm_hex_revert(int bi);                /* re-read the file (M-x revert-buffer) */
/* Take the last change back out — what answering `n` to "really edit the
   buffer?" has to do, the edit having already happened. */
void ccm_hex_undo(int bi);
void ccm_hex_redo(int bi);

/* The two widgets a hex buffer shows in its pane: the view, which the toolkit
   draws, and a transparent input layer over it that takes the keys and the
   mouse (the hexview widget is not focusable, so it never sees either). Both
   get the pane's geometry; only the input layer is ever the focused child. */
struct _gtcaca_widget_t *ccm_hex_view(int bi);
struct _gtcaca_widget_t *ccm_hex_input(int bi);
void ccm_hex_place(int bi, int x, int y, int w, int h);

void ccm_hex_modeline(int bi);              /* from refresh_modeline */
void ccm_hex_help(void);                    /* the ? / C-h window */

/* M-x hex-mode / M-x hex-mode-exit: show the file this buffer visits as hex,
   or go back to showing it as text. */
void ccm_hex_mode(void);
void ccm_hex_mode_exit(void);

#endif /* CCM_HEX_H */

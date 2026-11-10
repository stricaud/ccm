# ccm — cacamacs

A small terminal text editor with Emacs key bindings, built on the
[gtcaca](https://github.com/stricaud/gtcaca) libcaca TUI toolkit and its
`editor` widget. It shows off the widget's colourization, folding, annotations,
autocompletion, search/replace and a VSCode-style language-extension model.

```sh
ccm [file | directory] [language-configuration.json]
```

- A **file** opens in the editor.
- A **directory** opens the file browser (also `C-x d`).
- An optional explicit `language-configuration.json` forces that config.

It also has a **diagram mode** (`M-x diagram`): a drag-and-drop canvas for ASCII
diagrams. Pick a shape from the pane on the left — box, rounded, diamond,
circle, ellipse, cloud, cylinder, hexagon, actor or free text — drop it on the
canvas with the mouse or the keyboard, link any two by clicking one then the
other, draw plain lines and freehand strokes, rub out single characters with the
eraser, and colour or dash anything. Drag a band round several objects (or
Shift-click them) and they move, restyle and delete together. Leaving drops the
finished art into your document at the cursor — plain characters, saved with
`C-x C-s` like anything else — or, with `C-x q`, a **Mermaid graph** that
GitHub and most Markdown viewers draw for real. `C-w` keeps a `.drawio` or
`.mmd` copy that preserves the colours.

```
                         +-------+              .-----------.
     O                  /         \             |'---------'|
    /|\        +------>+   ok?     +----------->|   store   |
    / \   -----+        \         /             |           |
   user                  +-------+              '-----------'
```

See [docs/README-cacamacs.md](docs/README-cacamacs.md) for the full key-binding
reference and feature tour.

> **History.** ccm started life as `apps/cacamacs` inside the gtcaca repo, where
> a copy remains as a worked example of building an app on the toolkit. Active
> development now happens here, against gtcaca as an ordinary external
> dependency.

Copy and paste are the system's: `C-w`/`M-w` put text on the macOS, Windows or
Linux clipboard (and on the terminal's, via OSC 52, so it works over ssh), and
`C-y` yanks whatever another application copied. Large pastes arrive as a single
edit rather than as thousands of keystrokes.

## Install with pip

Prebuilt binaries are published to PyPI for macOS (Apple Silicon), Linux
(x86_64) and Windows (x86_64) — no compiler or Homebrew needed:

```sh
pip install cacamacs
ccm            # or: cacamacs
```

PyPI is used purely as a binary channel (as `ruff`/`cmake`/`ninja` do): the
wheel carries the compiled `ccm` and the `libgtcaca`/`libcaca`/`oniguruma`
libraries it needs, made relocatable, plus the sample language extensions so
syntax highlighting works out of the box. See [packaging/](packaging/) for how
the per-platform wheels are built (`packaging/build.sh` → `packaging/wheel.py`,
wired up in `.github/workflows/release.yml`, which publishes on a `v*` tag).

## Building

ccm consumes gtcaca like any third party would: install gtcaca first, then
build ccm against it. gtcaca ships a `gtcaca.pc`, so pkg-config finds it.

```sh
# 1. install gtcaca (any one of these):
brew install stricaud/tap/gtcaca         # Homebrew tap
#   …or from a gtcaca checkout:  cmake -S . -B build && cmake --build build && sudo cmake --install build

# 2. build ccm
cmake -S . -B build
cmake --build build

# 3. run it
./run.sh              # builds if needed, then runs build/ccm
./run.sh path/to/file
```

Requirements: a C compiler, CMake ≥ 3.20, pkg-config, and an installed
`gtcaca` (which pulls in `libcaca`).

## Installing

```sh
sudo cmake --install build        # → /usr/local/bin/ccm, share/ccm/ccm-theme.example
```

### Self-contained macOS package

`scripts/package-macos.sh` builds a `.pkg` that bundles `libgtcaca` and
`libcaca` alongside `ccm` (rewritten to `@rpath`), so the installed editor needs
neither Homebrew nor a system gtcaca:

```sh
scripts/package-macos.sh          # → ccm-<version>.pkg
```

Its postinstall seeds `~/.cacamacs/theme` from the bundled sample (never clobbering
an existing one).

## When the file changes on disk

Edit, save or reopen a file that something else rewrote underneath you and
cacamacs asks first, in Emacs' own words —
`TIDAL.md changed on disk; really edit the buffer? (y, n, r or C-h)` on the
first keystroke, `… has changed since visited or saved.  Save anyway?` on
`C-x C-s`, `Reread from disk?` on `C-x C-f`. `M-x revert-buffer` rereads on
purpose. See
[docs/README-cacamacs.md](docs/README-cacamacs.md#when-the-file-changes-on-disk).

## Binary files — hex mode

A file with a NUL byte in it is not text, so cacamacs shows it as bytes:

```
00000000  7f 45 4c 46 02 01 01 00  00 00 00 00 00 00 00 00  |.ELF............|
00000010  03 00 3e 00 01 00 00 00  40 1c 00 00 00 00 00 00  |..>.....@.......|
```

The hex is on the left, the ASCII on the right, and the bytes are what you
edit and save — the gtcaca `hexview` widget in an ordinary ccm buffer, so
`C-x o`, `C-x b` and the rest work there as anywhere else.

`0`–`9` and `a`–`f` type over the byte under the cursor, a nibble at a time;
`Tab` crosses to the ASCII side, where any character types that byte. `Insert`
switches to inserting rather than overwriting, `C-o` puts a `00` at the cursor
and `M-o` appends one at the end, `C-d` and `Backspace` take one out, and `C-_`
undoes. Bytes you have changed stay marked in red until the buffer is saved.

`C-s` searches, for bytes or for text — `de ad be ef`, `deadbeef` and
`0xDE,0xAD` all mean those bytes, `"GET /"` means those characters, and a word
that is not all hex digits means its characters too; the echo line says which it
read, and `C-s` again finds the next one while `M-s` asks for a different
pattern. `M-g` goes to an offset (hex, or `#1024` for decimal), `C-u N` repeats
a key N times, and `C-x (` … `C-x )` … `C-x e` records and replays a macro over
the bytes. `C-x C-s` writes the file back, `C-x C-w` writes the bytes out under
another name, and `?` lists every key.

Any file can be shown this way with `M-x hex-mode`, and `M-x hex-mode-exit`
goes back to text. A binary member of an archive, and a `.gz` holding
something that is not text, open as hex read-only — `C-x C-w` gets the bytes
out to a file of their own. ccm holds a hex buffer in memory, so a file over
256 MB is turned down rather than opened.

## Compressed files and archives

A `.gz` is opened as the text inside it and saved back compressed, the way
Emacs' `jka-compr` does — `ccm notes.txt.gz` edits `notes.txt` in place, and the
syntax highlighting comes from the name under the `.gz` (`prog.c.gz` colours as
C). `.bz2` and `.xz` open the same way but read-only, since ccm only writes
gzip; `C-x C-w` writes one out to a file you can edit.

A `.zip` or a `.tar` (plain, `.gz`, `.bz2` or `.xz`) is browsed as a directory
instead: `ccm src.zip`, `C-x C-f` onto one, or walking onto one in `C-x d` steps
*into* it. Enter descends into a folder, Enter on a file opens it read-only, and
`..` at the top comes back out. Nothing is unpacked to disk — a member goes from
the archive straight into the buffer — and ccm does not rewrite archives, so
`C-x C-w` is how you get a member out to work on.

Each format sits behind its own library, all three optional at build time: zlib
for `.gz` and `.zip`, libbz2 for `.bz2`, liblzma for `.xz`. A build missing one
says so when a file asks for that format.

## Markdown

`M-x md-` then Tab lists a set of helpers for writing prose — `md-title` and
`md-subtitle` underline a line with `===` / `---`, `md-h1`…`md-h6` make `#`
headings, `md-bold` / `md-italic` / `md-code` / `md-strike` / `md-link` mark up
the region or the word at point, and `md-list` / `md-ordered` / `md-task` /
`md-quote` / `md-hr` handle the block-level bits. Each is its own inverse. See
[docs/README-cacamacs.md](docs/README-cacamacs.md#markdown--m-x-md).

## Configuration

Per-user files live in `~/.cacamacs/` (`%APPDATA%\cacamacs\` on Windows).

- **Colours** — [themes/default](themes/default) (installed as
  `share/ccm/themes/default`) is cacamacs' theme, and `~/.cacamacs/theme` is
  applied over it, so a personal theme only names what it changes. Start from
  [docs/ccm-theme.example](docs/ccm-theme.example).

  A theme file carries two kinds of key, and is read twice — once by each half:
  `<widget>_bg` / `<widget>_fg` are the window chrome, which gtcaca layers over
  its own default; everything else (`background`, `comment`, `keyword`,
  `selection_bg`, …) is the editing surface and syntax colours, which cacamacs
  reads. Each parser ignores the other's keys, so both can live in one file.
  `ccm --warnings` reports where themes were looked for.
- **Editor config** — run `ccm --configure` to write `~/.cacamacs/config.json`
  with every setting at its default (tab size, indent, per-language overrides),
  annotated and ready to edit. It never overwrites an existing one.
- **File log** — `"logFiles": true` in that file appends one line per file
  opened (`O`) or written (`W`) to `~/.cacamacs/files.log`, so you can look back
  over what you worked on. Off by default; see
  [docs/README-cacamacs.md](docs/README-cacamacs.md#file-log--logfiles).
- **Language extensions** — the VSCode-style grammar/config bundles under
  [examples/extensions/](examples/extensions/) drive syntax colourization.

## Layout

```
src/            editor sources (cacamacs.c, ui.c, edit.c, search.c, …)
                diagram.c / diagram_model.c — M-x diagram (mode / document)
                lock.c — the "changed on disk" prompts (Emacs' userlock)
docs/           feature docs + the colour-theme sample
examples/       config sample and language extensions
packaging/      cross-platform bundles + pip wheels (build.sh → wheel.py)
scripts/        macOS packaging
CMakeLists.txt  standalone build (finds gtcaca via pkg-config)
run.sh          build-and-run helper
```

## License

Released into the public domain under [the Unlicense](LICENSE) — do whatever you
want with it.

# cacamacs

A small terminal text editor built on the gtcaca [`editor` widget](editor.md),
driven with Emacs key bindings. It demonstrates the widget's colourization,
folding, annotations, autocompletion, search/replace and the VSCode-style
language extension model.

```sh
cacamacs [file | directory] [language-configuration.json]
```

- A **file** opens in the editor.
- A **directory** opens the file browser (also `C-x d`). The browser is a
  read-only editor showing one entry per line, so it works like the editor:
  arrows/`C-n`/`C-p` move, `C-s` searches the listing, Enter descends into a
  folder (`../` goes up) or opens a file, and `q`/`Esc` closes it.
- An optional explicit `language-configuration.json` forces that config.

## Key bindings

```
Motion   C-f C-b C-n C-p  C-a C-e  C-v   arrows / Home / End / PageUp / PageDown
         M-< / M->  beginning / end of buffer
         (C-n / C-p step through wrapped rows when line wrap is on)
Help     M-x help  (a scrollable key-binding window; q or Esc closes it)
         M-x runs a command by name (help, diagram, undo, redo, goto-line, …; Tab completes)
Diagram  M-x diagram  drag-and-drop ASCII diagrams (see below)
Edit     C-d delete-fwd   C-k kill-line  Backspace   Tab indent / complete
Words    M-f / M-b move   M-d / M-DEL kill   M-u / M-l upcase / downcase
Region   C-Space mark     C-w kill   M-w (Esc w) copy   C-y yank   C-g cancel
Rect     C-x SPC rectangle mark, move to the opposite corner, then:
         C-x r t insert a string on each line   C-x r k kill   C-x r y yank
         C-x r d delete   C-x r c blank   (C-w / M-w also act on the box)
Comment  M-; comment / uncomment the current line or region
Lines    C-x C-t transpose lines   C-t transpose chars
Modes    C-x C-q read-only   C-x w show whitespace   Insert overtype
Search   C-s isearch fwd  C-r isearch back   (Enter accept, C-g cancel)
Replace  M-% (Esc %)   (prompts: Search: … then Replace with: …)
Windows  C-x 2 split the focused window below   C-x 3 split it right
         C-x 1 one window   C-x 0 close window   C-x o other   C-x b buffer
         (windows nest: split one pane without disturbing the others)
         (a split shows the same file at the same place in both windows —
          one document, two carets: editing either edits the file)
Macros   C-x ( start recording   C-x ) finish   C-x e replay
         (C-u N C-x e replays N times, e.g. C-u 100 C-x e)
Repeat   C-u N <command> runs the next command N times (bare C-u = 4):
         C-u 40 - draws 40 dashes, C-u 5 C-f moves five chars, …
Meta     Esc is the Meta prefix — press Esc, then a key, for M-<key>
         (Alt+<key> works too on terminals that send it as Esc-prefixed)
Undo     C-/   (also C-x u, or M-x undo)   Redo  C-x C-/  (or M-x redo)
Files    C-x C-f find file (Tab completes; a directory opens the browser)
         C-x C-s save (opens the save dialog if the buffer has no name)
         C-x C-w write to a different file (save dialog)
         C-x C-c quit   C-x d browser
         (save dialog: type the name, Enter saves via the default OK button;
          Tab to the buttons, Cancel or C-g aborts)
         M-x revert-buffer  throw the buffer away for the file on disk
View     C-x l line numbers   C-x f folding   C-x t toggle fold   C-x a annotate
         C-x C-l line wrap (on by default; turn off to scroll long lines)
JSON     C-x p pretty-print (.json/.jsonl open in JSON mode automatically)
Complete Tab (after a word)  — Up/Down choose, Enter/Tab accept, Esc cancel
```

## Markdown — `M-x md-…`

Helpers for writing prose. They all share the `md-` prefix, so `M-x md-` and
Tab lists the set. Each acts on the region when there is one and on the current
line or the word at point otherwise, and each is its own inverse — run it twice
and the markup comes back off.

| Command | Does |
|---|---|
| `md-title` | underlines the line with `=` (setext H1) |
| `md-subtitle` | underlines it with `-` (setext H2) |
| `md-h1` … `md-h6` | `#`-prefixed heading; the same level again removes it |
| `md-bold` / `md-italic` / `md-strike` | wrap in `**` / `*` / `~~` |
| `md-code` | inline `` ` `` — or a ``` fence when the region spans lines |
| `md-link` | `[text](url)`, leaving point where the URL goes |
| `md-list` / `md-ordered` / `md-task` | prefix the lines with `- `, `1. `, `- [ ] ` |
| `md-quote` | prefix the lines with `> ` |
| `md-hr` | a `---` rule on a line of its own |

So for the common case: type your title, then `M-x md-title`:

```
My Document          My Document
                ->   ===========
```

The underline is as long as the title *in characters*, so accented text lines
up. An underline already under the line is replaced rather than stacked, which
is what lets `md-title` and `md-subtitle` change a heading's level.

`md-ordered` renumbers the whole range from 1, so it also fixes a list whose
numbering has drifted.

## When the file changes on disk

A buffer remembers the modification time its file had when it was read or last
written. If something else — another editor, `git checkout`, a formatter —
rewrites that file underneath you, cacamacs asks before anything is lost, with
the same three questions Emacs asks:

- **You start typing** in a buffer that is otherwise unmodified:

  ```
  TIDAL.md changed on disk; really edit the buffer? (y, n, r or C-h)
  ```

  `y` goes ahead (your buffer wins when you save), `n` takes that change back
  out again, `r` rereads the file, and `C-h` explains the choice in a window.
  Only the *first* change asks; once you have said `y` the buffer is yours.

- **You save** (`C-x C-s`) a buffer whose file has moved on since:

  ```
  TIDAL.md has changed since visited or saved.  Save anyway? (yes or no)
  ```

- **You reopen** it — `C-x C-f`, or picking it out of the browser — while a
  buffer for it is already open:

  ```
  File TIDAL.md changed on disk.  Reread from disk? (yes or no)
  ```

  (or *Discard your edits?* if you have unsaved changes.)

The last two want a typed-out `yes` or `no`, as Emacs' `yes-or-no-p` does;
anything else gets `Please answer yes or no.` and the question again. A file
that has been *deleted* is not "changed" — there is nothing left to overwrite —
so cacamacs stays quiet about it, and saving simply writes the file back.

`M-x revert-buffer` throws a buffer away for the file on disk whenever you like;
it asks first, and point stays where it was.

## Configuration — `~/.cacamacs/config.json`

`ccm --configure` writes this file for you, with every setting at its built-in
default and a one-line description beside each, so there is something to edit
rather than a blank page. It creates `~/.cacamacs/` if needed and never
overwrites an existing config — move the old one aside first.

Controls indentation, with optional per-extension overrides:

```json
{
  "tabSize": 4,
  "insertSpaces": true,
  "indentSize": 4,
  "edgeColumn": 80,
  "logFiles": false,
  "languages": {
    ".py": { "tabSize": 4, "insertSpaces": true, "indentSize": 4 },
    ".c":  { "insertSpaces": false, "tabSize": 8 }
  }
}
```

- `tabSize` — display width of a tab.
- `insertSpaces` — Tab inserts spaces (true) or a real tab (false).
- `indentSize` — number of spaces inserted when `insertSpaces` is true.
- `edgeColumn` — column to draw the edge/ruler marker at (0 = off).
- `logFiles` — boolean, `false` unless set. See below.
- `languages` — overrides keyed by file extension (merged over the globals).

### File log — `logFiles`

With `"logFiles": true`, cacamacs appends a line to `~/.cacamacs/files.log`
every time it opens or writes a file, so you can look back over what you worked
on:

```
[2026-08-10 21:17:12] O /home/you/src/parser.c
[2026-08-10 21:19:44] W /home/you/src/parser.c
```

`O` is an open, `W` a write. Paths are absolute regardless of the directory ccm
was started in, and the timestamp is local time.

Every route in is covered — a file named on the command line, `C-x C-f`, and
picking one out of the browser (`C-x d`, or `ccm <directory>`) — as is every
route out: `C-x C-s` and `C-x C-w`. Reopening a file already held in a buffer
does not log a second time; nothing was read from disk.

The log is only ever appended to, and never rotated or pruned — it is yours to
trim. cacamacs never reports a failure to write it: if the file cannot be
opened, the line is dropped and editing carries on.

A ready-to-copy sample is in
[examples/cacamacs-config.json](../examples/cacamacs-config.json). (Indentation
is a user preference, so it lives here rather than inside an installed
extension.)

## Languages — VSCode-style extensions

cacamacs resolves a file's language the way VSCode does, from extension folders
containing a `package.json` that declares `contributes.languages` (a
`language-configuration.json` → comments/brackets/strings) and optionally
`contributes.grammars` (a `.tmLanguage.json` → full syntax colouring).

It scans these roots, in order, so **installed VSCode extensions work directly**:

```
~/.cacamacs/extensions (cacamacs' own)
~/.vscode/extensions        (VSCode)
~/.vscode-oss/extensions    (VSCodium)
~/.cursor/extensions        (Cursor)
```

For a file it picks the language whose `extensions` include the file's suffix,
loads its grammar (preferred for colour) and/or language-configuration, and
shows the language id in the modeline (`(c)`, `(python)`, …). If nothing
matches, the modeline says so. Two sample extensions ship in
[examples/cacamacs-extensions/](../examples/cacamacs-extensions/) (C and Python);
copy one into a root to try it:

```sh
mkdir -p ~/.cacamacs/extensions
cp -r examples/cacamacs-extensions/python-0.0.1 ~/.cacamacs/extensions/
./build/apps/ccm somefile.py
```

> Colour fidelity depends on the grammar. cacamacs maps TextMate scopes onto a
> small fixed palette (comment / string / number / keyword / bracket /
> operator), so a real VSCode grammar colours correctly but with fewer distinct
> hues than VSCode. See [editor.md](editor.md) for the grammar engine's limits.

## JSON mode

Files ending in `.json` / `.jsonl` open in JSON mode: JSON-aware colouring and
brace-depth folding (fold margin on). `C-x p` pretty-prints — the selection, the
whole buffer if it is one JSON value, or the current line (handy for a JSONL
record).

## Diagram mode — `M-x diagram`

Emacs' `artist-mode` draws *characters*. `M-x diagram` draws *objects*: a shape
is something you grab, drag, resize and label, and a link stays attached to the
two objects it joins, re-routing itself whenever either one moves — draw.io in a
terminal. The file it writes is nothing but the ASCII art, so a diagram drops
straight into a README, a comment block or an email.

```
                         +-------+              .-----------.
     O                  /         \             |'---------'|
    /|\        +------>+   ok?     +----------->|   store   |
    / \   -----+        \         /             |           |
   user                  +-------+              '-----------'
```

A diagram is a piece of a document, not a document of its own:

- **Draw a new one** — put the cursor where it belongs and run `M-x diagram`.
  The canvas starts empty; `q` drops the finished art into the buffer at that
  spot (breaking the line first if the cursor was mid-line).
- **Edit one you already have** — select it (`C-Space`, then move to the far
  corner) and run `M-x diagram`. The selection is read back into objects, and
  `q` puts the redrawn art in its place.

Then save the way you always do: `C-x C-s`. The mode never writes a file itself,
and `C-/` undoes the insertion like any other edit. `Q` leaves the drawing
behind and touches nothing.

### The shape pane

The pane down the left is the palette. Each entry can be used three ways: click
it and then click the canvas, drag it straight onto the canvas, or press its
key. `p` hides and shows the pane.

| | shape | good for |
| --- | --- | --- |
| `1` | Select | picking, moving and resizing what is already there |
| `2` | Box | a process, a component, a plain box |
| `3` | Rounded | a start or end — a terminator |
| `4` | Diamond | a decision |
| `5` | Circle | a round node: a junction, a hub |
| `6` | Ellipse | a state or an event |
| `7` | Cloud | the internet, or anything outside the picture |
| `8` | Cylinder | a database, a queue, any store |
| `9` | Hexagon | a preparation step or a gateway |
| `0` | Actor | a person: a user, a service owner (dropped small; it grows to fit its name, and resizes like anything else) |
| `t` | Text | a free-floating label, no outline |
| `l` | Link | a connector between any two objects |
| `e` | Eraser | rub out single characters |
| `f` | Freehand | draw characters yourself (`Space` picks which) |
| `-` | Line | a plain line, no arrowheads — an object like any other |

After you place something, the tool returns to **Select** and the label editor
opens, so the usual rhythm is: pick a shape, click, type, Enter. While the
canvas is empty a card in the middle spells that out.

`r` redraws whatever is selected as the next shape in that list, so a box you
have already labelled and linked can become a diamond without redrawing it.

### Linking

Any two objects can be linked — shapes of different types, and free text too.
Whichever of these feels natural works:

- **Click, click** — press `l` for the Link tool, click one object, click the
  other. No dragging.
- **Handles** — a selected object shows an `o` at the middle of each edge. Click
  one, then click the object to link to; or drag from it straight onto that
  object.
- **Keyboard** — select one (`Tab` cycles), press `c`, `Tab` to the other, press
  `Enter`.

A link keeps itself attached: move either end and it re-routes. Click its line
to select it, `a` cycles its arrowheads (to → both → from → none), `Del`
removes it. Deleting an object deletes the links hanging off it.

Two objects are either linked or they are not: asking for a link that already
exists — in either direction — selects the one that is there instead of stacking
a second, invisible one on the same route. Nothing links to itself.

### Lines, freehand and style

The **Line** tool (`-`) draws a plain line with no arrowheads. Drag from one end
to the other; the far end snaps to the nearest horizontal, vertical or 45° run,
which is what keeps it a clean row of characters. It is a real object — click it
to select, drag it to move, drag either `o` end to re-aim it, `Del` to remove it
— and reopening a file finds straight runs and hands them back as line objects.
Use `l` instead when you want the two ends to *stay* attached to objects.

The **Freehand** tool (`f`) is the opposite: it puts characters on the canvas
one cell at a time and nothing owns them. `Space` cycles the character it draws
with. Shapes are drawn on top of that background layer, so a stroke that runs
under a box is kept but hidden.

Anything selected can be restyled, and with nothing selected the same keys arm
the style that everything you draw next will get:

| key | what it changes |
| --- | --- |
| `C` | colour, from the toolkit's swatch dialog |
| `B` | the background behind it |
| `S` | solid → dashed → dotted |

Dashes and dots are drawn with the characters themselves, so a dashed line is
still dashed in the saved ASCII. Colour cannot survive plain text — see the
draw.io export below.

### The eraser

`e` picks the eraser, which rubs out **one character at a time** — `Del` is what
removes whole objects. Drag to rub out a run; the cells between two mouse
reports go too, so a quick sweep leaves no gaps.

A character inside a shape belongs to that shape rather than to the canvas, so
the first cell you rub out of one turns that shape back into plain characters —
the drawing does not change, only what is an object does — and then the
character goes. The status line says so when it happens, and `u` puts both the
shape and the characters back in one step. `x` does the same conversion on
demand for whatever is selected.

### Mouse

| gesture | what it does |
| --- | --- |
| drag anywhere on an object | move it |
| drag its `#` bottom-right corner | resize it |
| drag an `o` edge handle onto another object | link the two |
| double-click an object | type its label, centred inside it (the toolkit times the click; `gtcaca_set_double_click_time()` tunes it) |
| click a link or a line | select it |
| drag empty space | pan the canvas |
| wheel | scroll |

A selected object shows what each part does: `#` at the bottom right resizes,
an `o` at the middle of each edge starts a link, and everywhere else — sides
included — moves it.

### Keys

```
Shapes   1..0 t l e pick from the pane   p hide / show the pane   r change shape
Place    click the canvas, or press Ret to place at the cursor
Select   Tab next object    C-g deselect / cancel anything    click to pick
Edit     Ret edit the label (C-o adds a line, Ret commits, C-g cancels)
         Del / d delete     D duplicate            u undo
         x turn the selection into plain characters (the eraser's raw material)
Move     arrows move the selection — or the cursor, when nothing is selected
Size     < > narrower / wider          [ ] shorter / taller
Style    a cycle a link's arrowheads   s ASCII ↔ Unicode line drawing
Draw     f freehand (Space picks the character)   - a plain line, no arrows
Style    C colour   B background   S solid / dashed / dotted
Erase    e eraser: rub out single characters   x object → plain characters
Export   C-w write a .drawio copy that keeps the colours
Leaving  q insert the diagram at the cursor and leave   Q leave it behind
         (then C-x C-s saves the buffer, C-/ undoes the insert)   ? help
```

### Line drawing

A new diagram draws with box-drawing glyphs when the terminal can render them,
which is what makes a rounded box actually look rounded (`╭──╮`); otherwise it
falls back to `+--+`. `s` flips between the two at any time and the modeline
says which is in force. Art read from a file keeps whatever it was drawn with.

Four shapes are ASCII in either mode — diamond, circle, cloud, hexagon and the
actor are built from `/ \ < > ( ) ~`, which have no box-drawing equivalents, so
they draw the same way in both.

### Keeping the colours: a .drawio copy

`C-w` writes the same objects out as a **draw.io** file (`.drawio`, mxGraph XML)
next to the file you are editing. That copy keeps everything plain text cannot:
colours, backgrounds, dash patterns, and each shape as a real shape — a diamond
is a rhombus, a cylinder a cylinder, an actor a stick figure — with the links
still joining the right two. Open it at app.diagrams.net.

It is a *copy*, not a substitute: `q` still drops the ASCII into your document
the same way, and the buffer is still saved with `C-x C-s`.

### Reopening a file

There is no sidecar file and no metadata hidden in the art: reopening runs the
ASCII back through a recogniser that rebuilds the objects — each shape from its
outline, the text inside it as its label, and lines joining two objects as
links. Whatever it cannot account for (freehand scribbles, a table, prose) is
kept verbatim and written back out unchanged, so passing a hand-drawn file
through diagram mode never loses characters.

Two consequences worth knowing:

- A link is redrawn by the router, so a hand-drawn line that wandered comes back
  as a tidy elbow between the same two objects.
- Dashed and dotted lines come back as loose characters rather than line
  objects: the recogniser looks for unbroken runs. They still draw and save
  exactly as they were; they just are not draggable until redrawn.
- Shapes drawn inside other shapes are not recognised as nested; the inner one
  is read as part of the outer one's label.

Opening a buffer that is bigger than the 512x256 canvas is declined rather than
silently truncated, and leaving a diagram you did not change leaves the buffer
exactly as it was.

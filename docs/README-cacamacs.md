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
| drag its `#` bottom-right corner | resize it — the opposite corner stays put, so the box follows the pointer wherever it goes, including back past that corner |
| drag an `o` edge handle onto another object | link the two, leaving by that edge and arriving at whichever edge you let go nearest |
| drag the `l Link` row out of the pane onto an object | start a link from it, then click the object to link to |
| double-click an object | type its label, centred inside it (the toolkit times the click; `gtcaca_set_double_click_time()` tunes it) |
| click a link or a line | select it |
| click an object while several are selected | go back to just that one |
| Shift-click an object | add it to the selection, or take it back out |
| drag empty space | a rubber band: everything it touches is selected |
| middle- or right-drag | pan the canvas |
| wheel | scroll |

A selected object shows what each part does: `#` at the bottom right resizes,
an `o` at the middle of each edge starts a link, and everywhere else — sides
included — moves it. The markers are drawn from the same rule the mouse is
tested against, so a handle is exactly as big as it looks: on a roomy shape
they widen to a couple of cells rather than the single one a terminal pointer
has to hit dead on.

### Several objects at once

Drag a band round a group, or Shift-click them one by one, and everything that
follows acts on all of them: dragging any member moves the group, and the
arrows, `C`, `B`, `S`, `<` `>` `[` `]`, `r`, `x`, `D` and `Del` apply to every
one. A link comes along when both of its ends do, so a band drawn round a
cluster picks the cluster up whole rather than leaving its arrows behind.

**Clicking one member goes back to just that one**, so you are never stuck with
a group: press-and-drag moves them all, press-and-release picks the one under
the pointer. Resizing and linking always act on the single object you grabbed,
group or no group.

The modeline counts what is picked (`select 3 selected`). Only one of them —
the one picked last — carries the `#` and `o` handles, and is washed brighter
than the rest, so there is never any doubt about which object a drag from a
handle would resize or link.

Shift-click is a shortcut, not the only route: many terminals keep Shift-click
for their own text selection and never pass it to the program. The band and `m`
do the same job everywhere, which is why they exist alongside it.

| key | |
| --- | --- |
| `m` | add the next object to the selection |
| `G` | draw a frame around what is selected — a group |
| `Ret` on a link | type the text that rides on its arrow |
| `A` | select everything |
| `Tab` | start again from a single object |
| `C-g` | clear the selection |

### Keys

```
Shapes   1..0 t l e pick from the pane   p hide / show the pane   r change shape
Place    click the canvas, or press Ret to place at the cursor
Select   Tab next object    C-g deselect / cancel anything    click to pick
Edit     Ret edit the label (C-o adds a line, Ret commits, C-g cancels)
         Del / d delete     D duplicate            u undo
         x turn the selection into plain characters (the eraser's raw material)
Move     arrows move the selection — or the cursor, when nothing is selected
Size     < > or H L narrower / wider   [ ] or K J shorter / taller
Style    a cycle a link's arrowheads   s ASCII ↔ Unicode line drawing
Draw     f freehand (Space picks the character)   - a plain line, no arrows
Style    C colour   B background   S solid / dashed / dotted
Erase    e eraser: rub out single characters   x object → plain characters
Group    m add the next object to the selection   A select all
Export   C-w write a copy to a file: .drawio, or .mmd for Mermaid
Leaving  q insert at the cursor and leave — it asks for ASCII or Mermaid first
         Q leave the drawing behind          C-x q the same question as q
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

### Keeping the colours: a copy in a file of its own

`C-w` writes the same objects out to a file beside the one you are editing. The
name you give it picks the format: `.mmd` (or `.mermaid`) writes the Mermaid
graph, anything else a **draw.io** file (`.drawio`, mxGraph XML). That copy keeps everything plain text cannot:
colours, backgrounds, dash patterns, and each shape as a real shape — a diamond
is a rhombus, a cylinder a cylinder, an actor a stick figure — with the links
still joining the right two. Open it at app.diagrams.net.

It is a *copy*, not a substitute: `q` still drops the ASCII into your document
the same way, and the buffer is still saved with `C-x C-s`.

### Text on a link, and a second line in a label

Press `Ret` on a selected link to type the text that rides on its arrow. It is
drawn *beside* the line, never on it — written over the line it would break the
run of characters in two and the recogniser would no longer see one link — and
it becomes the edge label in the Mermaid output:

```
n3 -->|"feeds"| n4
```

To put a second line in any label, type `\n`. Shift-Enter would be the obvious
key, but most terminals send the same byte for it as for Enter, so there is
nothing to bind; `\n` is two ordinary characters that every terminal delivers,
and the backslash goes away with it. `C-o` does the same. For a literal
backslash followed by an n, type the backslash twice (`\\n`).

A box grows to fit what you type into it, so a second line always has somewhere
to go. It only ever grows: a box you sized by hand keeps that size.

One limit: a link's text is written into the art as plain characters beside the
line, so reopening the file reads it back as free text rather than as that
link's label. The line and the words are both still there and nothing is lost;
they are just no longer tied together.

### Which side a link leaves by

The handle you drag from decides where the link attaches, and where you let go
decides where it arrives. Drag out of a box's right-hand side and the arrow
stays on the right-hand side, going around both boxes if the other one is off
to the left. Starting a link by *clicking* instead — the `l` tool, or `c` from
the keyboard — leaves the choice to the router, which picks the facing edges
from where the two objects sit, exactly as it always did.

Because links can be told apart by their sides, two objects may be joined more
than once: a request and a reply want two arrows, not one. The only thing
refused is a second link along a route that already exists, since it would land
exactly on the first and be invisible. One handle can also feed as many links
as you like, out to as many different objects.

One limit worth knowing: the file is the ASCII art and nothing else, so two
routes that *touch* are a single run of characters by the time it is written
out. Reopening reads those back as one link, and the picture can shift. Leave a
little room between links running alongside each other and the round trip is
exact.

### Groups, and Mermaid subgraphs

There is no group *object*. A box drawn around other boxes **is** the group —
that is the only kind of grouping that can survive a file which is nothing but
the art. Select what belongs together and press `G`, and a frame is drawn round
it with room for a title, which you type straight away.

A frame behaves differently from an ordinary box in the two ways it has to:
it is never filled in, so what it encloses stays visible however the two were
drawn; and its label sits on its top row rather than the middle, where its
contents are. Frames nest.

The Mermaid export writes each frame as a `subgraph`, so this:

```
┌─ one ────────────────────────────┐
│ ┌────────────┐    ┌────────────┐ │
│ │     a1     │───>│     a2     │ │
│ └────────────┘    └────────────┘ │
└──────────────────────────────────┘
```

comes out as:

```
graph LR
  subgraph n3["one"]
    n0["a1"]
    n1["a2"]
  end
  n0 --> n1
```

Links that cross a frame's boundary are written normally, so a node in one
subgraph can point at a node in another.

### ASCII art or a Mermaid graph

`q` drops the diagram into the buffer. What it drops is your choice:

- **ASCII art** — the picture itself. It needs nothing to read it, so it goes
  into a README, a comment block or an email and simply looks right.
- **A Mermaid graph** — the graph *behind* the picture. GitHub, GitLab and most
  Markdown viewers lay it out and draw it for real, so it keeps the colours and
  the true shapes (a diamond is a rhombus, a cylinder a cylinder) that plain
  text cannot hold.

`q` asks which one before inserting — a small dialog with an **ascii** and a
**mermaid** button — and remembers the answer, so the modeline then says which
is in force (`q inserts mermaid at the cursor`). `C-x q` raises the same
question, and Escape cancels it and leaves you in the drawing. In a Markdown
buffer the Mermaid goes in a ```` ```mermaid ```` fence, which is what makes a
viewer draw it rather than print it.

The setting below decides which button starts highlighted, so if you always
want the same format, `q` then Enter gives it to you:

```json
{ "diagram-mermaid-default": true }
```

What Mermaid cannot carry, and the mode says so when it happens: the
hand-placed geometry (Mermaid does its own layout), free-standing lines — an
edge needs two ends to join — and any loose characters in the raw layer. When
those matter, the ASCII is the form that keeps them.

### Reopening a file

There is no sidecar file and no metadata hidden in the art: reopening runs the
ASCII back through a recogniser that rebuilds the objects — each shape from its
outline, the text inside it as its label, and lines joining two objects as
links. Whatever it cannot account for (freehand scribbles, a table, prose) is
kept verbatim and written back out unchanged, so passing a hand-drawn file
through diagram mode never loses characters.

Two consequences worth knowing:

- A link is redrawn by the router, so a hand-drawn line that wandered comes back
  as a tidy elbow between the same two objects — but which edges it attached to
  is read off the art, so a link drawn out of one side stays on that side.
- Dashed and dotted lines come back as loose characters rather than line
  objects: the recogniser looks for unbroken runs. They still draw and save
  exactly as they were; they just are not draggable until redrawn.
- Shapes drawn inside other shapes come back nested, as the group they look
  like: the outer one keeps its top row as its title and the inner ones are
  read as shapes in their own right, links between them included.

Opening a buffer that is bigger than the 512x256 canvas is declined rather than
silently truncated, and leaving a diagram you did not change leaves the buffer
exactly as it was.

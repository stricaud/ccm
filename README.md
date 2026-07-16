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

See [docs/README-cacamacs.md](docs/README-cacamacs.md) for the full key-binding
reference and feature tour.

> **History.** ccm started life as `apps/cacamacs` inside the gtcaca repo, where
> a copy remains as a worked example of building an app on the toolkit. Active
> development now happens here, against gtcaca as an ordinary external
> dependency.

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

Its postinstall seeds `~/.ccm/theme` from the bundled sample (never clobbering
an existing one).

## Configuration

- **Colour theme** — copy [docs/ccm-theme.example](docs/ccm-theme.example) to
  `~/.ccm/theme`. Comments in the file explain every key.
- **Editor config** — copy [examples/config.json](examples/config.json) to
  `~/.cacamacs/config.json` for tab size, indent, per-language overrides.
- **Language extensions** — the VSCode-style grammar/config bundles under
  [examples/extensions/](examples/extensions/) drive syntax colourization.

## Layout

```
src/            editor sources (cacamacs.c, ui.c, edit.c, search.c, …)
docs/           feature docs + the colour-theme sample
examples/       config sample and language extensions
scripts/        macOS packaging
CMakeLists.txt  standalone build (finds gtcaca via pkg-config)
run.sh          build-and-run helper
```

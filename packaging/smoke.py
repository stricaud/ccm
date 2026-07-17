#!/usr/bin/env python3
"""smoke.py — prove a packaged ccm actually loads.

    python3 packaging/smoke.py [command ...]      # default: ccm

Runs `<command> --version`, which forces the OS to resolve ccm's entire
dylib/DLL closure (gtcaca, libcaca, oniguruma) at process load — before main()
runs — then prints a version and exits 0. So a clean exit means every library
resolved; a missing one makes the process die in the loader instead (a dyld
`Library not loaded`, an ld.so `cannot open shared object`, or a Windows
STATUS_DLL_NOT_FOUND / 0xC0000135).

Using --version rather than driving the full-screen TUI keeps this reliable and
truly headless: no pseudo-terminal, no watchdog, and — crucially — nothing left
running to orphan a CI runner (an orphaned ccm.exe holding workspace handles was
enough to fail later steps on the Windows runner).

The command may be the binary itself or a console-script shim (`cacamacs` /
`ccm`); the shim forwards --version to the bundled binary unchanged.
"""
import subprocess
import sys

# Substrings (matched case-insensitively) that mean the loader, not the app,
# failed — the one outcome this test exists to catch.
LOADER_ERRORS = (
    "library not loaded",           # macOS dyld
    "image not found",              # macOS dyld
    "cannot open shared object",    # Linux ld.so
    "symbol not found",             # macOS/Linux
    "undefined symbol",             # Linux
    "cannot execute binary file",   # wrong-arch binary
    "dll was not found",            # Windows
    "status_dll_not_found",         # Windows
    "the code execution cannot proceed",   # Windows missing-DLL dialog text
)


def main() -> int:
    cmd = (sys.argv[1:] or ["ccm"]) + ["--version"]

    try:
        p = subprocess.run(cmd, capture_output=True, timeout=60)
    except FileNotFoundError:
        print(f"SMOKE FAIL: could not spawn {cmd[0]!r}", file=sys.stderr)
        return 1
    except subprocess.TimeoutExpired:
        # --version must return promptly; a hang means it did not take the
        # early-exit path (e.g. an old binary that opened the TUI instead).
        print("SMOKE FAIL: `--version` did not return within 60s", file=sys.stderr)
        return 1

    text = ((p.stdout or b"") + (p.stderr or b"")).decode("utf-8", "replace")
    # 0xC0000135 = STATUS_DLL_NOT_FOUND — a load-time DLL was missing on Windows.
    if p.returncode in (0xC0000135, -1073741515):
        text += "\nstatus_dll_not_found"

    low = text.lower()
    hit = next((e for e in LOADER_ERRORS if e in low), None)
    if hit:
        if text.strip():
            print(text, file=sys.stderr)
        print(f"SMOKE FAIL: dynamic-loader error ({hit!r})", file=sys.stderr)
        return 1
    if p.returncode != 0:
        if text.strip():
            print(text, file=sys.stderr)
        print(f"SMOKE FAIL: `--version` exited {p.returncode}", file=sys.stderr)
        return 1

    print(f"SMOKE OK: {text.strip() or 'started'} — libraries resolved")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""smoke.py — prove a packaged ccm actually starts: every shared library it
needs resolves, and it reaches the UI rather than dying in the dynamic loader.

    python3 packaging/smoke.py [command ...]      # default: ccm

ccm is a full-screen TUI with no headless/--dump mode (gtcaca_init opens a
libcaca display straight away), so it cannot be exercised like carcal's --dump
path. Instead we start it under a pseudo-terminal (a real one on POSIX, a pipe
on Windows), send Emacs quit (C-x C-c), and kill it after a short watchdog.

The test is deliberately strict about one thing and lenient about the rest: it
FAILS if the output carries a dynamic-loader error (a missing/undresolved dylib,
.so or .dll — exactly the `Library not loaded` crash `pip install` must never
ship) or if the binary cannot be spawned at all; it does NOT care about the exit
code, because a headless CI runner has no real terminal and libcaca legitimately
bails with "error opening terminal" *after* everything has linked.
"""
import os
import sys
import time

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
    "the code execution cannot proceed",   # Windows missing-DLL dialog text
)

QUIT = b"\x18\x03"          # C-x C-c
WATCHDOG = 8.0              # seconds


def _verdict(output: bytes, started: bool) -> int:
    text = output.decode("utf-8", "replace")
    low = text.lower()

    def dump():
        # Only on failure: a successful run is a full-screen TUI render, i.e.
        # kilobytes of terminal escapes that would bury the CI log.
        if text.strip():
            print(text if len(text) < 4000 else text[:4000] + "…", file=sys.stderr)

    if not started:
        dump()
        print("SMOKE FAIL: could not spawn the binary", file=sys.stderr)
        return 1
    hit = next((e for e in LOADER_ERRORS if e in low), None)
    if hit:
        dump()
        print(f"SMOKE FAIL: dynamic-loader error in output ({hit!r})", file=sys.stderr)
        return 1
    print("SMOKE OK: binary started and its libraries resolved")
    return 0


def run_posix(cmd: list[str]) -> int:
    import pty
    import select
    import signal

    pid, fd = pty.fork()
    if pid == 0:                                   # child
        os.environ.setdefault("TERM", "xterm")
        try:
            os.execvp(cmd[0], cmd)
        except OSError:
            os._exit(127)

    out = bytearray()
    deadline = time.time() + WATCHDOG
    sent = False
    started = True
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.3)
        if r:
            try:
                data = os.read(fd, 4096)
            except OSError:
                break                              # child closed the pty
            if not data:
                break
            out += data
        if not sent and time.time() > deadline - WATCHDOG + 1.5:
            try:
                os.write(fd, QUIT)
            except OSError:
                pass
            sent = True

    # reap; kill if the watchdog expired with the TUI still up
    _, status = os.waitpid(pid, os.WNOHANG)
    if _ == 0:
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass
        os.waitpid(pid, 0)
    else:
        # exec-failed sentinel from the child
        if os.WIFEXITED(status) and os.WEXITSTATUS(status) == 127:
            started = False
    os.close(fd)
    return _verdict(bytes(out), started)


def run_windows(cmd: list[str]) -> int:
    import subprocess
    import tempfile
    import time

    # Capture to a temp FILE, never a PIPE. The `cacamacs`/`ccm` console script
    # is a launcher that spawns ccm.exe as a grandchild; a TUI blocked on
    # console input (the piped C-x C-c never reaches it — libcaca reads the
    # console, not stdin) keeps the pipe's write end open, so a PIPE read
    # deadlocks even after the launcher is killed. A file is read at the end and
    # never blocks. This is the hang that stalled CI for hours.
    log = tempfile.TemporaryFile()
    flags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    try:
        p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                             stdout=log, stderr=subprocess.STDOUT,
                             creationflags=flags)
    except OSError as e:
        log.close()
        return _verdict(str(e).encode(), started=False)

    # A missing DLL exits almost immediately (STATUS_DLL_NOT_FOUND); a healthy
    # binary reaches the UI and blocks on console input that never arrives. So
    # wait a few seconds: still alive ⇒ it started and its libraries resolved.
    deadline = time.time() + WATCHDOG
    while time.time() < deadline and p.poll() is None:
        time.sleep(0.2)

    alive = p.poll() is None
    if alive:
        # Kill the whole tree (launcher → ccm.exe); killing just the launcher
        # would leak ccm.exe and hang the runner.
        try:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           timeout=10)
        except Exception:
            p.kill()
        try:
            p.wait(timeout=5)
        except Exception:
            pass

    log.seek(0)
    out = log.read()
    log.close()

    # 0xC0000135 = STATUS_DLL_NOT_FOUND — a load-time DLL was missing.
    if not alive and p.returncode in (0xC0000135, -1073741515):
        out += b"\nDLL was not found (STATUS_DLL_NOT_FOUND)"
    return _verdict(out, started=True)


def main() -> int:
    cmd = sys.argv[1:] or ["ccm"]
    return run_windows(cmd) if os.name == "nt" else run_posix(cmd)


if __name__ == "__main__":
    sys.exit(main())

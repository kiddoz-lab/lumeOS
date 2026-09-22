#!/usr/bin/env python3
"""Publish the tail of a CI log file as a GitHub Actions error annotation.

Why this exists: workflow logs are easy to read in the GitHub UI but are not
available to every consumer of the API (and some environments cannot reach the
log download host at all).  Annotations *are* reachable through the checks API,
so a failing CI step calls this script to make its real error message visible
wherever the run is inspected:

    python3 tools/ci_annotate.py build-kernel.log --title "make kernel"

The script never fails the build itself (a diagnostics tool must not turn one
failure into two): it always exits 0.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Lines that usually carry the real reason a build step failed.  make echoes
# every compiler command line, so a plain "last N lines" tail is mostly useless
# noise; these patterns pick out the diagnosis instead.
ERROR_PATTERNS = re.compile(
    r"(error:|Error \d|undefined reference|undefined symbol|cannot find|"
    r"no such file|collect2|FAILED|FAIL:|Traceback|AssertionError|"
    r"unrecognized|not found|Permission denied|"
    r"unimp|unassigned|no boot within|strategy '.*' failed|PANIC|"
    r"^DIAG|spinning in|never started executing)",
    re.IGNORECASE)


def escape(message: str) -> str:
    """Escape a message for the ::error:: workflow command."""
    return (message.replace("%", "%25")
                   .replace("\r", "%0D")
                   .replace("\n", "%0A"))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("logfile", help="log file to read")
    parser.add_argument("--lines", type=int, default=30,
                        help="how many trailing lines to publish when nothing "
                             "looks like an error")
    parser.add_argument("--error-lines", type=int, default=25,
                        help="how many matching error lines to publish")
    parser.add_argument("--title", default="", help="annotation title")
    # GitHub truncates an annotation message at 4096 characters and keeps the
    # beginning, so stay clearly below that and put the diagnosis first.
    parser.add_argument("--max-chars", type=int, default=3500)
    parser.add_argument("--tail", type=int, default=0,
                        help="publish the last N lines as-is, ignoring the error "
                             "patterns (use for test logs, where the interesting "
                             "lines - the kernel's own last words - do not look "
                             "like build errors)")
    args = parser.parse_args(argv)

    path = Path(args.logfile)
    if not path.is_file():
        print(f"::error title={args.title or 'ci_annotate'}::"
              f"log file {args.logfile} does not exist")
        return 0

    text = path.read_text(errors="replace")
    lines = [line for line in text.splitlines() if line.strip()]

    if args.tail > 0:
        selected = [line for line in lines if line.strip()][-args.tail:]
        header = f"last {len(selected)} line(s) of {args.logfile}\n"
        tail = header + "\n".join(selected)
        if len(tail) > args.max_chars:
            tail = tail[:args.max_chars] + "\n... (truncated)"
        print(f"::error title={escape(args.title or 'log tail')}::{escape(tail)}")
        return 0

    matches = [line for line in lines if ERROR_PATTERNS.search(line)]
    if matches:
        selected = matches[-args.error_lines:]
        header = (f"{len(matches)} line(s) matched the error patterns; "
                  f"showing the last {len(selected)}\n")
    else:
        selected = lines[-args.lines:]
        header = ""

    tail = header + "\n".join(selected)
    if not tail.strip() or not selected:
        tail = "(log file is empty or has no matching errors)"
    if len(tail) > args.max_chars:
        tail = "...(truncated)...\n" + tail[-args.max_chars:]

    title = escape(args.title) if args.title else ""
    print(f"::error title={title}::{escape(tail)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

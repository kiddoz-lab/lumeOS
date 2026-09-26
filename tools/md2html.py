#!/usr/bin/env python3
"""Render the Markdown in docs/ as standalone HTML.

Why this exists: the documents in `docs/` are the project's real record, and
they are written to be read - tables of what each test proves, code blocks with
exact boot output, cross-links.  Reading them needs a Markdown renderer and a
theme that happens to put dark text on a light background, and neither is
guaranteed on the machine someone is reading from.  This script turns them into
self-contained HTML with the colours spelled out (white page, near-black text),
so the result looks the same everywhere and cannot be made invisible by a
viewer's theme.

It deliberately depends on nothing but the Python standard library: no
`pip install`, no network.  That matters because the machine that needs it is
often the one where installing things is the problem.

Usage:
    python3 tools/md2html.py docs/testing.md build/docs-view/testing.html
    python3 tools/md2html.py --all build/docs-view        # every doc + index

Supported Markdown is what these documents actually use: ATX headings, fenced
code blocks, pipe tables, bullet and numbered lists (with nesting and wrapped
continuation lines), blockquotes, horizontal rules, and the inline forms (code
spans, bold, links).  Anything else is passed through as text rather than
silently dropped.
"""
from __future__ import annotations

import argparse
import html
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

CSS = """
:root { color-scheme: light; }
html, body { background: #ffffff; color: #16181d; margin: 0; padding: 0; }
body {
  font: 16px/1.62 -apple-system, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
  max-width: 52rem; margin: 0 auto; padding: 2.5rem 1.5rem 5rem;
}
h1, h2, h3, h4 { line-height: 1.25; font-weight: 650; }
h1 { font-size: 1.9rem; margin: 0 0 1.2rem; border-bottom: 2px solid #e6e8ec; padding-bottom: .5rem; }
h2 { font-size: 1.35rem; margin: 2.4rem 0 .8rem; border-bottom: 1px solid #e6e8ec; padding-bottom: .35rem; }
h3 { font-size: 1.1rem; margin: 1.8rem 0 .6rem; }
p, li { margin: .6rem 0; }
a { color: #0b5fff; }
code, pre {
  font-family: ui-monospace, SFMono-Regular, "SF Mono", Menlo, Consolas, monospace;
  font-size: .9em;
}
code { background: #f2f4f7; color: #24292f; padding: .12em .35em; border-radius: 3px; }
pre {
  background: #f6f8fa; color: #24292f; border: 1px solid #e6e8ec; border-radius: 6px;
  padding: .85rem 1rem; overflow-x: auto; line-height: 1.45;
}
pre code { background: none; padding: 0; }
table { border-collapse: collapse; margin: 1rem 0; width: 100%; }
th, td { border: 1px solid #dfe3e8; padding: .45rem .7rem; text-align: left; vertical-align: top; }
th { background: #f2f4f7; font-weight: 650; }
tr:nth-child(even) td { background: #fafbfc; }
blockquote {
  margin: 1rem 0; padding: .2rem 1rem; border-left: 4px solid #d6dae0;
  background: #fafbfc; color: #444a53;
}
hr { border: 0; border-top: 1px solid #e6e8ec; margin: 2rem 0; }
ul, ol { padding-left: 1.5rem; }
"""


def inline(text: str) -> str:
    """Escape, then apply inline markup.  Escaping first is load-bearing: a
    document that discusses `<lume/sched.h>` must render it, not swallow it."""
    t = html.escape(text, quote=False)
    t = re.sub(r"`([^`]+)`", r"<code>\1</code>", t)
    t = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", t)
    t = re.sub(r"(?<![\w*])\*([^*\n]+)\*(?![\w*])", r"<em>\1</em>", t)
    t = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", r'<a href="\2">\1</a>', t)
    return t


def is_table_sep(line: str) -> bool:
    cells = [c.strip() for c in line.strip().strip("|").split("|")]
    return bool(cells) and all(re.fullmatch(r":?-{2,}:?", c) for c in cells if c != "")


def split_row(line: str) -> list[str]:
    return [c.strip() for c in line.strip().strip("|").split("|")]


def convert(md: str) -> str:
    lines = md.splitlines()
    out: list[str] = []
    i, n = 0, len(lines)

    while i < n:
        line = lines[i]

        # fenced code
        if line.startswith("```"):
            i += 1
            buf = []
            while i < n and not lines[i].startswith("```"):
                buf.append(lines[i])
                i += 1
            i += 1
            out.append(f"<pre><code>{html.escape(chr(10).join(buf))}</code></pre>")
            continue

        # horizontal rule
        if re.fullmatch(r"\s*(-{3,}|\*{3,}|_{3,})\s*", line):
            out.append("<hr>")
            i += 1
            continue

        # heading
        m = re.match(r"^(#{1,6})\s+(.*?)\s*#*\s*$", line)
        if m:
            out.append(f"<h{len(m.group(1))}>{inline(m.group(2))}</h{len(m.group(1))}>")
            i += 1
            continue

        # table
        if line.lstrip().startswith("|") and i + 1 < n and is_table_sep(lines[i + 1]):
            head = split_row(line)
            i += 2
            body = []
            while i < n and lines[i].lstrip().startswith("|"):
                body.append(split_row(lines[i]))
                i += 1
            cells = "".join(f"<th>{inline(c)}</th>" for c in head)
            rows = "".join("<tr>" + "".join(f"<td>{inline(c)}</td>" for c in r) + "</tr>"
                           for r in body)
            out.append(f"<table><thead><tr>{cells}</tr></thead><tbody>{rows}</tbody></table>")
            continue

        # blockquote
        if line.lstrip().startswith(">"):
            buf = []
            while i < n and lines[i].lstrip().startswith(">"):
                buf.append(lines[i].lstrip()[1:].lstrip())
                i += 1
            out.append(f"<blockquote><p>{inline(' '.join(buf))}</p></blockquote>")
            continue

        # lists.  Three kinds of line belong to a list block: the items
        # themselves, items nested one level deeper, and *continuation* lines -
        # the wrapped rest of an item's sentence, which Markdown lets you indent
        # without repeating the marker.  Missing that third kind is how a
        # two-line bullet turns into a bullet plus a loose paragraph.
        if re.match(r"^\s*([-*+]|\d+\.)\s+", line):
            ordered = bool(re.match(r"^\s*\d+\.", line))
            tag = "ol" if ordered else "ul"
            items: list[str] = []
            nested: dict[int, list[str]] = {}
            target = -1
            while i < n and lines[i].strip():
                m = re.match(r"^(\s*)([-*+]|\d+\.)\s+(.*)$", lines[i])
                indent = len(lines[i]) - len(lines[i].lstrip())
                if m and indent <= 1:
                    items.append(m.group(3).strip())
                    target = len(items) - 1
                elif m and indent >= 2 and items:
                    nested.setdefault(target, []).append(m.group(3).strip())
                elif indent >= 2 and target >= 0:
                    if (nested.get(target) and not items[target].endswith(":")
                            and items[target].rstrip().endswith((".", "!", "?"))):
                        nested[target][-1] += " " + lines[i].strip()
                    else:
                        items[target] += " " + lines[i].strip()
                else:
                    break
                i += 1
            html_items = []
            for idx, text in enumerate(items):
                sub = nested.get(idx)
                inner = (f"<{tag}>" + "".join(f"<li>{inline(x)}</li>" for x in sub)
                         + f"</{tag}>") if sub else ""
                html_items.append(f"<li>{inline(text)}{inner}</li>")
            out.append(f"<{tag}>" + "".join(html_items) + f"</{tag}>")
            continue

        if not line.strip():
            i += 1
            continue

        # paragraph
        buf = []
        while (i < n and lines[i].strip()
               and not re.match(r"^(#{1,6}\s|```|\s*([-*+]|\d+\.)\s|>|\s*\|)", lines[i])
               and not re.fullmatch(r"\s*(-{3,}|\*{3,}|_{3,})\s*", lines[i])):
            buf.append(lines[i].strip())
            i += 1
        out.append(f"<p>{inline(' '.join(buf))}</p>")

    return "\n".join(out)


def page(title: str, body: str) -> str:
    return ("<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
            f"<title>{html.escape(title)} - LumeOS</title>\n<style>{CSS}</style>\n"
            f"</head>\n<body>\n{body}\n</body>\n</html>\n")


BLURBS = {
    "README": "the project in one page: what works, what is unproven",
    "testing": "the four levels of test, the guest-error gate, reading CI results",
    "userspace": "what runs in user mode, syscall by syscall",
    "architecture": "the memory map, the exception path, the scheduler",
    "roadmap": "milestones, what is done, and the order of the next commits",
    "building": "toolchains, make targets, and what CI does in what order",
    "hardware": "the Pi Zero W, config.txt, and what a real boot should print",
    "boot-pi": "the firmware chain and the memory layout it imposes",
    "research-notes": "what was read to decide the boot, timer and mailbox design",
}


def render_all(src_dir: Path, out_dir: Path) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    docs = sorted(src_dir.glob("*.md"))
    readme = REPO / "README.md"
    written = []
    for src in ([readme] if readme.is_file() else []) + docs:
        dst = out_dir / (src.stem + ".html")
        dst.write_text(page(src.stem.replace("-", " ").replace("_", " ").title(),
                            convert(src.read_text())))
        written.append((src.stem, dst))
        print(f"md2html: {src.relative_to(REPO)} -> {dst}")

    rows = "\n".join(
        f'<li><a href="{stem}.html">{stem}</a> - {BLURBS.get(stem, "a LumeOS document")}</li>'
        for stem, _ in written)
    index = ("<h1>LumeOS documentation</h1>\n"
             "<p>HTML copies of every document in the repository. The Markdown sources in "
             "<code>docs/</code> and <code>README.md</code> are the originals; these copies "
             "exist so the documents can be read without a Markdown renderer, with colours "
             "that do not depend on a viewer's theme.</p>\n"
             f"<ul>\n{rows}\n</ul>\n")
    (out_dir / "index.html").write_text(page("Documentation", index))
    print(f"md2html: wrote {out_dir / 'index.html'}")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", nargs="?", type=Path, help="a Markdown file")
    parser.add_argument("dest", nargs="?", type=Path, help="the HTML file to write")
    parser.add_argument("--all", type=Path, metavar="DIR",
                        help="render README.md and every docs/*.md into DIR, plus an index")
    args = parser.parse_args(argv)

    if args.all:
        return render_all(REPO / "docs", args.all)
    if not args.source or not args.dest:
        parser.error("give a source and a destination, or use --all DIR")
    args.dest.parent.mkdir(parents=True, exist_ok=True)
    args.dest.write_text(page(args.source.stem.replace("-", " ").title(),
                              convert(args.source.read_text())))
    print(f"md2html: {args.source} -> {args.dest} ({args.dest.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

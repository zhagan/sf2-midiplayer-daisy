#!/usr/bin/env python3
from __future__ import annotations

import html
import re
from pathlib import Path


DOCS_DIR = Path(__file__).resolve().parent
PROJECT_DIR = DOCS_DIR.parent
README = PROJECT_DIR / "README.md"
OUTPUT = DOCS_DIR / "index.html"


def slugify(text: str) -> str:
    text = text.lower()
    text = re.sub(r"[^a-z0-9]+", "-", text)
    return text.strip("-") or "section"


def inline_format(text: str) -> str:
    text = html.escape(text)
    text = re.sub(r"`([^`]+)`", r"<code>\1</code>", text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
    return text


def split_blocks(lines: list[str]) -> list[tuple[str, object]]:
    blocks: list[tuple[str, object]] = []
    i = 0
    while i < len(lines):
        line = lines[i].rstrip("\n")
        stripped = line.strip()
        if not stripped:
            i += 1
            continue

        if stripped.startswith("```"):
            lang = stripped[3:].strip()
            i += 1
            code_lines: list[str] = []
            while i < len(lines) and not lines[i].strip().startswith("```"):
                code_lines.append(lines[i].rstrip("\n"))
                i += 1
            if i < len(lines):
                i += 1
            blocks.append(("code", (lang, "\n".join(code_lines))))
            continue

        if re.match(r"^#{1,6}\s", stripped):
            level = len(stripped) - len(stripped.lstrip("#"))
            text = stripped[level:].strip()
            blocks.append(("heading", (level, text)))
            i += 1
            continue

        if re.match(r"^\d+\.\s+", stripped):
            items: list[str] = []
            while i < len(lines):
                candidate = lines[i].strip()
                if not re.match(r"^\d+\.\s+", candidate):
                    break
                items.append(re.sub(r"^\d+\.\s+", "", candidate))
                i += 1
            blocks.append(("olist", items))
            continue

        if stripped.startswith("- "):
            items: list[str] = []
            while i < len(lines):
                candidate = lines[i].strip()
                if not candidate.startswith("- "):
                    break
                items.append(candidate[2:].strip())
                i += 1
            blocks.append(("ulist", items))
            continue

        paragraph: list[str] = [stripped]
        i += 1
        while i < len(lines):
            candidate = lines[i].strip()
            if not candidate:
                break
            if candidate.startswith("```") or candidate.startswith("- "):
                break
            if re.match(r"^\d+\.\s+", candidate):
                break
            if re.match(r"^#{1,6}\s", candidate):
                break
            paragraph.append(candidate)
            i += 1
        blocks.append(("paragraph", " ".join(paragraph)))
    return blocks


def build_sections(blocks: list[tuple[str, object]]) -> tuple[str, list[tuple[str, str]]]:
    nav: list[tuple[str, str]] = []
    sections: list[str] = []
    current: list[str] = []
    current_id = "overview"
    current_title = "Overview"
    first_h1_seen = False

    def flush_section() -> None:
        nonlocal current, current_id, current_title
        if not current:
            return
        sections.append(
            f'<section class="section" id="{current_id}">\n'
            f"  <h2>{html.escape(current_title)}</h2>\n"
            + "\n".join(current)
            + "\n</section>"
        )
        current = []

    for kind, data in blocks:
        if kind == "heading":
            level, text = data  # type: ignore[misc]
            if level == 1 and not first_h1_seen:
                first_h1_seen = True
                continue
            if level == 2:
                flush_section()
                current_title = text
                current_id = slugify(text)
                nav.append((current_id, text))
                continue
            if level == 3:
                current.append(f"<h3>{html.escape(text)}</h3>")
                continue
            if level == 4:
                current.append(f"<h4>{html.escape(text)}</h4>")
                continue

        if kind == "paragraph":
            current.append(f"<p>{inline_format(data)}</p>")  # type: ignore[arg-type]
        elif kind == "ulist":
            items = "".join(f"<li>{inline_format(item)}</li>" for item in data)  # type: ignore[arg-type]
            current.append(f'<ul class="bullets">{items}</ul>')
        elif kind == "olist":
            items = "".join(f"<li>{inline_format(item)}</li>" for item in data)  # type: ignore[arg-type]
            current.append(f'<ol class="steps">{items}</ol>')
        elif kind == "code":
            lang, code = data  # type: ignore[misc]
            cls = f' class="language-{html.escape(lang)}"' if lang else ""
            current.append(f"<pre><code{cls}>{html.escape(code)}</code></pre>")

    flush_section()
    return "\n".join(sections), nav


def build_html(title: str, intro: str, sections_html: str, nav: list[tuple[str, str]]) -> str:
    nav_html = "\n".join(f'          <a href="#{slug}">{html.escape(text)}</a>' for slug, text in nav)
    return f"""<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>{html.escape(title)} Documentation</title>
    <meta
      name="description"
      content="Major MIDI documentation generated from the project README."
    />
    <link rel="stylesheet" href="./styles.css" />
  </head>
  <body>
    <div class="shell">
      <aside class="sidebar">
        <div class="brand">
          <p class="kicker">Major MIDI</p>
          <h1>Operator Guide</h1>
          <p>{html.escape(intro)}</p>
        </div>

        <nav class="nav">
{nav_html}
        </nav>
      </aside>

      <main class="main">
        <section class="hero" id="top">
          <div class="hero-grid">
            <div>
              <p class="kicker">Generated Docs</p>
              <h2>{html.escape(title)}</h2>
              <p>
                This site is generated from
                <code>README.md</code>.
              </p>
              <p>
                Edit the README, then rerun
                <code>python3 docs/generate_docs.py</code>.
              </p>
            </div>

            <div class="hero-card">
              <h3>Single Source</h3>
              <ul>
                <li>README is the source of truth</li>
                <li><code>docs/index.html</code> is generated</li>
                <li><code>docs/styles.css</code> controls presentation</li>
              </ul>
            </div>
          </div>
        </section>

{sections_html}

        <div class="footer">
          Generated from <code>README.md</code>.
        </div>
      </main>
    </div>
  </body>
</html>
"""


def main() -> None:
    text = README.read_text(encoding="utf-8")
    lines = text.splitlines()
    title = "Major MIDI"
    intro = "A practical front-panel guide for using Major MIDI on Daisy Patch SM."

    for line in lines:
        stripped = line.strip()
        if stripped.startswith("# "):
            title = stripped[2:].strip()
            break

    for line in lines:
        stripped = line.strip()
        if stripped and not stripped.startswith("#"):
            intro = stripped.replace("`", "")
            break

    blocks = split_blocks(lines)
    sections_html, nav = build_sections(blocks)
    output = build_html(title, intro, sections_html, nav)
    OUTPUT.write_text(output, encoding="utf-8")


if __name__ == "__main__":
    main()

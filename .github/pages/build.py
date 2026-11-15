#!/usr/bin/env python3
"""Builds the GitHub Pages site (samueljustice.github.io/plugins) from the root README.

    build.py <README rendered to HTML by GitHub's markdown API> <out dir> <owner/repo>

The README is rendered by GitHub itself (gh api markdown, mode markdown: how GitHub renders a README; gfm mode would turn every newline into a line break), so the page looks as it does on the
repository. This wraps it in a page and fixes what only works on github.com: relative image paths become
raw.githubusercontent.com URLs, relative links to other files point at the file on github.com, and heading
ids lose GitHub's "user-content-" prefix so the README's #anchors work.
"""
import html
import pathlib
import re
import sys

rendered, out, repo = sys.argv[1:4]
body = pathlib.Path(rendered).read_text(encoding="utf-8")
raw = f"https://raw.githubusercontent.com/{repo}/main/"
blob = f"https://github.com/{repo}/blob/main/"
tree = f"https://github.com/{repo}/tree/main/"


def relative(url: str) -> bool:
    return not re.match(r"^(?:[a-z]+:|#|/|data:)", url)


def fix_src(m):
    url = html.unescape(m.group(2))
    return f'{m.group(1)}"{raw + url if relative(url) else url}"' if relative(url) else m.group(0)


def fix_href(m):
    url = html.unescape(m.group(2))
    if not relative(url):
        return m.group(0)
    path, _, frag = url.partition("#")
    base = blob if "." in path.rsplit("/", 1)[-1] else tree
    return f'{m.group(1)}"{base}{path}{"#" + frag if frag else ""}"'


body = re.sub(r'(\bsrc=)"([^"]*)"', fix_src, body)
body = re.sub(r'(\bhref=)"([^"]*)"', fix_href, body)
# GitHub serves images through camo with data-canonical-src; keep the canonical one where it is relative.
body = re.sub(r'\sdata-canonical-src="[^"]*"', "", body)
body = body.replace('id="user-content-', 'id="')


# Heading ids, made the way GitHub makes them (lower case, punctuation dropped, spaces to hyphens), so links
# like #tonal-recall land on the right section. The API leaves them out; github.com adds them itself.
seen = {}


def heading_id(m):
    text = html.unescape(re.sub(r"<[^>]+>", "", m.group(3)))
    slug = re.sub(r"[^\w\- ]", "", text.strip().lower()).replace(" ", "-")
    n = seen.get(slug, 0)
    seen[slug] = n + 1
    slug = slug if n == 0 else f"{slug}-{n}"
    return f'<{m.group(1)}{m.group(2)} id="{slug}">{m.group(3)}</{m.group(1)}>'


body = re.sub(r"<(h[1-6])((?:\s+(?!id=)[\w-]+=\"[^\"]*\")*)>(.*?)</\1>", heading_id, body, flags=re.S)

page = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Sammy J's Plugins</title>
<meta name="description" content="Toys I've made for sound designers: audio plug-ins by Samuel Justice.">
<link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/github-markdown-css/5.8.1/github-markdown.min.css">
<style>
  :root {{ color-scheme: light dark; }}
  body {{ margin: 0; background: #ffffff; }}
  @media (prefers-color-scheme: dark) {{ body {{ background: #0d1117; }} }}
  .markdown-body {{ box-sizing: border-box; max-width: 980px; margin: 0 auto; padding: 32px 16px 64px; }}
  @media (min-width: 768px) {{ .markdown-body {{ padding: 48px 45px 80px; }} }}
  .markdown-body img {{ background: transparent; }}
  .markdown-body table {{ display: table; width: auto; margin-left: auto; margin-right: auto; }}
  .markdown-body .anchor {{ display: none; }}
  .repo-link {{ text-align: center; font: 14px -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; padding: 0 16px 40px; }}
  .repo-link a {{ color: #4493f8; text-decoration: none; }}
</style>
</head>
<body>
<article class="markdown-body">
{body}
</article>
<p class="repo-link"><a href="https://github.com/{repo}">Source code and every release on GitHub</a></p>
</body>
</html>
"""
outdir = pathlib.Path(out)
outdir.mkdir(parents=True, exist_ok=True)
(outdir / "index.html").write_text(page, encoding="utf-8")
(outdir / ".nojekyll").write_text("", encoding="utf-8")
print(f"Wrote {outdir / 'index.html'} ({len(page)} bytes)")

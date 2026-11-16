#!/usr/bin/env python3
"""Builds the website (samueljustice.github.io/plugins) from the root README.

    build.py <README rendered to HTML by GitHub's markdown API (mode markdown)> <out dir> <owner/repo>

The README stays the only place the words live. This reads its rendered HTML, takes each plug-in's section
(its "## Name" heading, main picture, paragraphs, extra pictures and formats line), the Get them all links
and the "Made by" footer, and lays them out as a designed page. A section it can't make sense of is shown as
it is, so a README edit can't break the site.
"""
import html
import math
import pathlib
import re
import sys

rendered, out, repo = sys.argv[1:4]
doc = pathlib.Path(rendered).read_text(encoding="utf-8")
RAW = f"https://raw.githubusercontent.com/{repo}/main/"
BLOB = f"https://github.com/{repo}/blob/main/"
TREE = f"https://github.com/{repo}/tree/main/"
SKIP = {"Get them all", "For developers", "Work in progress", "Made by Samuel Justice"}


def relative(url: str) -> bool:
    return not re.match(r"^(?:[a-z]+:|#|/|data:)", url)


def absolute(url: str, base_file: str = RAW) -> str:
    url = html.unescape(url)
    if not relative(url):
        return url
    if base_file is RAW:
        return RAW + url
    path, _, frag = url.partition("#")
    base = BLOB if "." in path.rsplit("/", 1)[-1] else TREE
    return base + path + ("#" + frag if frag else "")


def fix_links(fragment: str) -> str:
    fragment = re.sub(r'(\bsrc=)"([^"]*)"', lambda m: f'{m.group(1)}"{html.escape(absolute(m.group(2)))}"', fragment)
    fragment = re.sub(r'(\bhref=)"([^"]*)"', lambda m: f'{m.group(1)}"{html.escape(absolute(m.group(2), BLOB))}"', fragment)
    return re.sub(r'\s(?:data-canonical-src|target|rel)="[^"]*"', "", fragment)


def text_of(fragment: str) -> str:
    return html.unescape(re.sub(r"<[^>]+>", "", fragment)).strip()


def slug(name: str) -> str:
    return re.sub(r"[^\w\- ]", "", name.strip().lower()).replace(" ", "-")


# ---- split the README at its headings ---------------------------------------------------------------------
heads = [(m.start(), m.end(), int(m.group(1)), text_of(m.group(2))) for m in re.finditer(r"<h([23])[^>]*>(.*?)</h\1>", doc, re.S)]
chunks = []
for i, (start, end, level, name) in enumerate(heads):
    chunks.append((level, name, doc[end:heads[i + 1][0] if i + 1 < len(heads) else len(doc)]))

plugins = []
footer_html = ""
in_wip = False
for level, name, body in chunks:
    if level == 2:
        in_wip = name == "Work in progress"
    if name == "Made by Samuel Justice":
        footer_html = "".join(re.findall(r"<p[^>]*>.*?</p>", body, re.S))
        continue
    if name in SKIP:
        continue
    plugins.append((name, body, in_wip))


# What the installer holds, from the README's "every plug-in: Name 1.2.3 · ..." line (rewritten with each installer).
_inside = re.search(r"every plug-in: (.*?)\.\s*<br", doc, re.S)
VERSIONS = {}
for item in (html.unescape(_inside.group(1)).split("·") if _inside else []):
    m = re.match(r"\s*(.+?)\s+(\d+(?:\.\d+)+)\s*$", item)
    if m:
        VERSIONS[m.group(1)] = m.group(2)


def entry(number: int, name: str, body: str, group_wip: bool) -> str:
    wip = group_wip or "WORK_IN_PROGRESS" in body
    gallery = ""
    m = re.search(r"<details>.*?</details>", body, re.S)
    if m:
        shots = re.findall(r'<img[^>]*\bsrc="([^"]+)"[^>]*\balt="([^"]*)"', m.group(0))
        gallery = "".join(
            f'<a class="shot" href="{html.escape(absolute(src))}" data-caption="{alt}"><img loading="lazy" src="{html.escape(absolute(src))}" alt="{alt}"></a>'
            for src, alt in shots)
        body = body.replace(m.group(0), "")
    paragraphs = re.findall(r"<p([^>]*)>(.*?)</p>", body, re.S)
    hero = ""
    prose = []
    formats = ""
    links = ""
    for attrs, inner in paragraphs:
        if "WORK_IN_PROGRESS" in inner:
            continue
        img = re.search(r'<img[^>]*\bsrc="([^"]+)"', inner)
        if img and not hero and not text_of(inner):
            hero = absolute(img.group(1))
            continue
        if "Get it in the installer" in inner or "Read the manual" in inner:
            first, _, rest = inner.partition("<br>")
            formats = text_of(first)
            manual = re.search(r'<a[^>]*href="([^"]+)"[^>]*>Read the manual', rest or inner)
            if manual:
                links = f'<a href="{html.escape(absolute(manual.group(1), BLOB))}">Manual</a>'
            continue
        if text_of(inner):
            prose.append(f"<p>{fix_links(inner)}</p>")
    if not hero or not prose:
        # Not a shape we know: show the section as GitHub rendered it.
        return f'<section class="entry plain" id="{slug(name)}"><h2>{html.escape(name)}</h2>{fix_links(body)}</section>'
    tag = '<span class="wip">Work in progress</span>' if wip else ""
    version = VERSIONS.get(name)
    version_html = (f'<p class="version" data-plugin="{html.escape(name)}">Version <span>{html.escape(version)}</span> in the installer</p>'
                    if version else "")
    return f"""
<section class="entry" id="{slug(name)}">
  <header>
    <span class="no">{number:02d}</span>
    <h2>{html.escape(name)}</h2>
    {version_html}
    {tag}
    <p class="formats">{html.escape(formats)}</p>
    <p class="links"><a href="#get">Get it</a>{links}</p>
  </header>
  <div class="body">
    <a class="hero shot" href="{html.escape(hero)}" data-caption="{html.escape(name)}"><img src="{html.escape(hero)}" alt="{html.escape(name)}"></a>
    <div class="prose">{''.join(prose)}</div>
    {f'<div class="gallery">{gallery}</div>' if gallery else ''}
  </div>
</section>"""


entries = "".join(entry(i + 1, name, body, wip) for i, (name, body, wip) in enumerate(plugins))
index = "".join(
    f'<li><a href="#{slug(name)}"><span>{i + 1:02d}</span>{html.escape(name)}</a></li>'
    for i, (name, _body, _wip) in enumerate(plugins))

# ---- the top: downloads, what's in the installer, the donate link ------------------------------------------
def first(pattern: str, default: str = "") -> str:
    m = re.search(pattern, doc, re.S)
    return html.unescape(m.group(1)) if m else default


mac = first(r'href="([^"]+macOS\.dmg)"', f"https://github.com/{repo}/releases/latest")
win = first(r'href="([^"]+Windows-Installer\.exe)"', f"https://github.com/{repo}/releases/latest")
inside = [v.strip() for v in first(r"every plug-in: (.*?)\.\s*<br").split("·") if v.strip()]
built = first(r"Built ([^<]*?)\. Older")
donate = first(r'href="(https://www\.paypal\.com/donate/[^"]+)"')
# The bold line under the README's title, kept as HTML so its link survives; the meta description gets the text.
m = re.search(r"<p[^>]*>\s*<strong>(.*?)</strong>\s*</p>", doc, re.S)
tagline_html = m.group(1).strip() if m else "Some of the toys and utilities I've made for us at Sweet Justice Sound, given for free."
tagline_html = re.sub(r'<a ([^>]*)>', lambda a: '<a ' + re.sub(r'\s*(target|rel)="[^"]*"', '', a.group(1)) + ' target="_blank" rel="noopener">', tagline_html)
tagline = html.unescape(re.sub(r"<[^>]+>", "", tagline_html))
def _inside_item(v: str) -> str:
    m = re.match(r"(.+?)\s+(\d+(?:\.\d+)+)$", v)
    if not m:
        return f"<li>{html.escape(v)}</li>"
    return f'<li data-plugin="{html.escape(m.group(1))}">{html.escape(m.group(1))} <span>{html.escape(m.group(2))}</span></li>'


inside_html = "".join(_inside_item(v) for v in inside)

# A drawn waveform for the rule under the title: a few decaying bursts, the same every build.
points = []
for x in range(0, 1201, 4):
    t = x / 1200
    burst = sum(math.exp(-((t - c) ** 2) / w) * a for c, w, a in ((0.12, 0.002, 1.0), (0.38, 0.006, 0.7), (0.63, 0.0015, 0.9), (0.84, 0.004, 0.5)))
    y = 30 + math.sin(x * 0.21) * math.sin(x * 0.017 + 1.3) * (4 + 24 * burst)
    points.append(f"{x},{y:.1f}")
wave = f'<svg class="wave" viewBox="0 0 1200 60" preserveAspectRatio="none" aria-hidden="true"><polyline points="{" ".join(points)}"/></svg>'

page = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Sammy J's Plugins</title>
<meta name="description" content="{html.escape(tagline)} Audio plug-ins by Samuel Justice.">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Azeret+Mono:wght@400;500&family=Bricolage+Grotesque:opsz,wght@12..96,400;12..96,600;12..96,800&display=swap" rel="stylesheet">
<style>
  :root {{
    --ink: #0f0e0c; --paper: #ece7dc; --dim: #9a9488; --rule: #2e2b26; --red: #e0183f;
    --display: "Bricolage Grotesque", "Helvetica Neue", Arial, sans-serif;
    --mono: "Azeret Mono", ui-monospace, Menlo, monospace;
  }}
  * {{ box-sizing: border-box; }}
  html {{ scroll-behavior: smooth; }}
  body {{ margin: 0; background: var(--ink); color: var(--paper); font: 400 18px/1.6 var(--display); -webkit-font-smoothing: antialiased; }}
  a {{ color: inherit; }}
  .wrap {{ max-width: 1180px; margin: 0 auto; padding: 0 28px; }}
  .label, .formats, .links, .no, .wip, nav, .inside, .built, footer .small {{ font: 400 12px/1.5 var(--mono); letter-spacing: .08em; text-transform: uppercase; }}

  .top {{ padding: 72px 0 36px; }}
  .top .label {{ color: var(--dim); margin: 0 0 18px; }}
  .top .label a {{ color: var(--paper); text-decoration: none; border-bottom: 1px solid var(--red); }}
  .top .label a:hover {{ color: var(--red); }}
  h1 {{ font: 800 clamp(54px, 11vw, 148px)/.88 var(--display); letter-spacing: -.045em; margin: 0; }}
  h1 em {{ font-style: normal; color: var(--red); }}
  .wave {{ display: block; width: 100%; height: 60px; margin: 26px 0 22px; }}
  .wave polyline {{ fill: none; stroke: var(--paper); stroke-width: 1.4; vector-effect: non-scaling-stroke; }}
  .tagline {{ font-size: clamp(22px, 2.6vw, 30px); line-height: 1.25; margin: 0; max-width: 20em; }}
  .tagline a {{ color: inherit; text-decoration: none; border-bottom: 2px solid var(--red); }}
  .tagline a:hover {{ color: var(--red); }}

  .get {{ display: grid; grid-template-columns: auto 1fr; gap: 28px 56px; align-items: start; padding: 40px 0 48px; border-top: 1px solid var(--rule); margin-top: 44px; }}
  .buttons {{ display: flex; flex-wrap: wrap; gap: 12px; }}
  .button {{ display: inline-block; padding: 16px 24px; font: 600 17px/1 var(--display); text-decoration: none; border: 1.5px solid var(--paper); border-radius: 2px; }}
  .button.fill {{ background: var(--paper); color: var(--ink); }}
  .button:hover {{ background: var(--red); border-color: var(--red); color: #fff; }}
  .button small {{ display: block; font: 400 11px/1 var(--mono); letter-spacing: .08em; text-transform: uppercase; margin-bottom: 8px; opacity: .7; }}
  .inside {{ list-style: none; margin: 0; padding: 0; display: flex; flex-wrap: wrap; gap: 6px 22px; color: var(--paper); }}
  .get p {{ margin: 12px 0 0; color: var(--dim); font-size: 15px; line-height: 1.5; max-width: 46em; }}
  .get p a {{ color: var(--paper); }}
  .built {{ color: var(--dim); }}

  nav {{ border-top: 1px solid var(--rule); border-bottom: 1px solid var(--rule); position: sticky; top: 0; background: color-mix(in srgb, var(--ink) 92%, transparent); backdrop-filter: blur(8px); z-index: 5; }}
  nav ol {{ list-style: none; margin: 0; padding: 14px 0; display: flex; flex-wrap: wrap; gap: 8px 26px; }}
  nav a {{ text-decoration: none; color: var(--dim); }}
  nav a:hover {{ color: var(--paper); }}
  nav span {{ color: var(--red); margin-right: 8px; }}

  .entry {{ display: grid; grid-template-columns: 300px minmax(0, 1fr); gap: 0 48px; padding: 72px 0; border-bottom: 1px solid var(--rule); scroll-margin-top: 48px; }}
  .entry header {{ position: sticky; top: 76px; align-self: start; min-width: 0; }}
  .no {{ display: block; color: var(--red); margin-bottom: 14px; }}
  .version {{ font: 400 12px/1.5 var(--mono); letter-spacing: .08em; text-transform: uppercase; color: var(--paper); margin: 10px 0 0; }}
  .version span {{ color: var(--red); }}
  h2 {{ font: 800 clamp(32px, 3vw, 40px)/.98 var(--display); letter-spacing: -.03em; margin: 0 0 16px; overflow-wrap: anywhere; }}
  .wip {{ display: inline-block; color: var(--red); border: 1px solid var(--red); padding: 3px 8px 2px; margin-bottom: 16px; }}
  .formats {{ color: var(--dim); margin: 0 0 18px; }}
  .links {{ margin: 0; display: flex; gap: 18px; }}
  .links a {{ text-decoration: none; border-bottom: 1px solid var(--dim); padding-bottom: 2px; }}
  .links a:hover {{ color: var(--red); border-color: var(--red); }}
  .hero {{ display: block; }}
  .hero img {{ display: block; width: 100%; height: auto; border: 1px solid var(--rule); border-radius: 3px; }}
  .prose {{ max-width: 40em; margin-top: 28px; }}
  .prose p {{ margin: 0 0 1em; }}
  .prose a {{ text-decoration-color: var(--dim); text-underline-offset: 3px; }}
  .gallery {{ display: grid; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 10px; margin-top: 22px; }}
  .gallery img {{ display: block; width: 100%; height: 96px; object-fit: cover; object-position: top left; border: 1px solid var(--rule); border-radius: 2px; opacity: .8; transition: opacity .15s; }}
  .gallery a:hover img {{ opacity: 1; }}
  .entry.plain {{ display: block; }}
  .entry.plain img {{ max-width: 100%; height: auto; }}

  footer {{ padding: 64px 0 96px; display: grid; grid-template-columns: 300px minmax(0, 1fr); gap: 0 48px; }}
  footer h3 {{ font: 800 26px/1 var(--display); letter-spacing: -.02em; margin: 0; }}
  footer p {{ margin: 0 0 1em; max-width: 40em; }}
  footer sub {{ font-size: 15px; vertical-align: baseline; color: var(--dim); }}
  footer .small {{ color: var(--dim); margin-top: 28px; }}
  footer .small a {{ color: var(--paper); }}

  .shot {{ cursor: zoom-in; }}
  .viewer {{ position: fixed; inset: 0; z-index: 20; display: none; background: rgba(8, 8, 7, .94); }}
  .viewer.open {{ display: grid; grid-template-rows: 1fr auto; }}
  .viewer figure {{ margin: 0; min-height: 0; display: grid; place-items: center; padding: 56px 72px 12px; }}
  .viewer img {{ max-width: 100%; max-height: 100%; object-fit: contain; border: 1px solid var(--rule); border-radius: 3px; }}
  .viewer .bar {{ display: flex; justify-content: space-between; gap: 24px; padding: 14px 28px 22px; font: 400 12px/1.5 var(--mono); letter-spacing: .08em; text-transform: uppercase; color: var(--dim); }}
  .viewer .bar span:first-child {{ color: var(--paper); }}
  .viewer button {{ position: absolute; background: none; border: 0; color: var(--paper); font: 400 30px/1 var(--display); cursor: pointer; padding: 18px; opacity: .75; }}
  .viewer button:hover {{ opacity: 1; color: var(--red); }}
  .viewer .close {{ top: 6px; right: 12px; }}
  .viewer .prev, .viewer .next {{ top: 50%; transform: translateY(-50%); }}
  .viewer .prev {{ left: 8px; }}
  .viewer .next {{ right: 8px; }}
  .viewer.single .prev, .viewer.single .next {{ display: none; }}

  @media (max-width: 860px) {{
    .viewer figure {{ padding: 52px 12px 8px; }}
    .viewer .prev, .viewer .next {{ top: auto; bottom: 58px; transform: none; }}
    body {{ font-size: 17px; }}
    .wrap {{ padding: 0 20px; }}
    .top {{ padding-top: 44px; }}
    .get, .entry, footer {{ grid-template-columns: 1fr; }}
    .entry {{ padding: 48px 0; }}
    .entry header {{ position: static; margin-bottom: 22px; }}
    nav {{ position: static; }}
    footer h3 {{ margin-bottom: 18px; }}
  }}
</style>
</head>
<body>
<div class="wrap">
  <div class="top">
    <p class="label">Samuel Justice · audio plug-ins · <a href="https://www.sweetjusticesound.com" target="_blank" rel="noopener">www.sweetjusticesound.com</a></p>
    <h1>Sammy J's<br><em>Plugins</em></h1>
    {wave}
    <p class="tagline">{tagline_html}</p>
  </div>

  <div class="get" id="get">
    <div class="buttons">
      <a class="button fill" id="get-mac" href="{html.escape(mac)}"><small>Get them all</small>Download for macOS</a>
      <a class="button" id="get-win" href="{html.escape(win)}"><small>Get them all</small>Download for Windows</a>
    </div>
    <div>
      <ul class="inside">{inside_html}</ul>
      <p>One installer with the newest version of every plug-in: apps, VST3, AU and signed AAX for Pro Tools. It replaces older copies and comes with an uninstaller. Older versions are on the <a href="https://github.com/{repo}/releases">releases page</a>.</p>
      <p class="built">{f"Built {html.escape(built)}" if built else ""}</p>
      {f'<p>If these get some use in your work and you would like to chip in, you can <a href="{html.escape(donate)}">toss a coin in the jar</a> on PayPal.</p>' if donate else ''}
    </div>
  </div>
</div>

<nav><div class="wrap"><ol>{index}</ol></div></nav>

<div class="wrap">
{entries}
  <footer>
    <h3>Made by<br>Samuel Justice</h3>
    <div>
      {fix_links(footer_html)}
      <p class="small"><a href="https://github.com/{repo}">Source code, manuals and every release on GitHub</a></p>
    </div>
  </footer>
</div>

<div class="viewer" id="viewer" role="dialog" aria-modal="true" aria-label="Pictures">
  <button class="close" aria-label="Close">&times;</button>
  <button class="prev" aria-label="Previous picture">&larr;</button>
  <button class="next" aria-label="Next picture">&rarr;</button>
  <figure><img alt=""></figure>
  <div class="bar"><span class="caption"></span><span class="count"></span></div>
</div>
<script>
// The download buttons carry the installer that was newest when this page was built. A new installer replaces
// the old release, so ask GitHub for the newest one now and point the buttons at it (a page left open, or a
// cached copy, would otherwise link to files that are gone). If the lookup fails the built-in links stay.
(function () {{
  if (!window.fetch) return;
  fetch('https://api.github.com/repos/{repo}/releases/latest', {{ headers: {{ Accept: 'application/vnd.github+json' }} }})
    .then(function (r) {{ return r.ok ? r.json() : null; }})
    .then(function (release) {{
      if (!release || !/^bundle-v/.test(release.tag_name || '')) return;
      // The installer's notes list what it holds as "| [Name](...) | 1.2.3 |"; show those versions.
      var re = /^\\|\\s*\\[([^\\]]+)\\]\\([^)]*\\)\\s*\\|\\s*([0-9][0-9.]*)\\s*\\|/gm, row;
      while ((row = re.exec(release.body || '')) !== null) {{
        document.querySelectorAll('[data-plugin="' + row[1].replace(/"/g, '') + '"] span').forEach(function (el) {{ el.textContent = row[2]; }});
      }}
      (release.assets || []).forEach(function (a) {{
        if (/macOS\\.dmg$/.test(a.name)) document.getElementById('get-mac').href = a.browser_download_url;
        if (/Windows-Installer\\.exe$/.test(a.name)) document.getElementById('get-win').href = a.browser_download_url;
      }});
    }})
    .catch(function () {{}});
}})();
</script>
<script>
// Clicking a picture opens it here, with the rest of that plug-in's pictures to step through.
(function () {{
  var viewer = document.getElementById('viewer'), img = viewer.querySelector('img');
  var caption = viewer.querySelector('.caption'), count = viewer.querySelector('.count');
  var shots = [], at = 0, opener = null;
  function show(i) {{
    at = (i + shots.length) % shots.length;
    img.src = shots[at].href;
    img.alt = shots[at].dataset.caption || '';
    caption.textContent = shots[at].dataset.caption || '';
    count.textContent = shots.length > 1 ? (at + 1) + ' / ' + shots.length : '';
  }}
  function close() {{
    viewer.classList.remove('open');
    document.documentElement.style.overflow = '';
    img.removeAttribute('src');
    if (opener) opener.focus();
  }}
  document.addEventListener('click', function (e) {{
    var link = e.target.closest && e.target.closest('a.shot');
    if (!link || e.metaKey || e.ctrlKey || e.shiftKey || e.button) return;   // modified clicks still open the file
    e.preventDefault();
    opener = link;
    shots = Array.prototype.slice.call(link.closest('.entry').querySelectorAll('a.shot'));
    viewer.classList.toggle('single', shots.length < 2);
    viewer.classList.add('open');
    document.documentElement.style.overflow = 'hidden';
    show(shots.indexOf(link));
    viewer.querySelector('.close').focus();
  }});
  viewer.querySelector('.close').addEventListener('click', close);
  viewer.querySelector('.prev').addEventListener('click', function () {{ show(at - 1); }});
  viewer.querySelector('.next').addEventListener('click', function () {{ show(at + 1); }});
  viewer.addEventListener('click', function (e) {{ if (e.target === viewer || e.target.tagName === 'FIGURE') close(); }});
  document.addEventListener('keydown', function (e) {{
    if (!viewer.classList.contains('open')) return;
    if (e.key === 'Escape') close();
    else if (e.key === 'ArrowLeft') show(at - 1);
    else if (e.key === 'ArrowRight') show(at + 1);
  }});
  var x0 = null;
  viewer.addEventListener('touchstart', function (e) {{ x0 = e.touches[0].clientX; }}, {{ passive: true }});
  viewer.addEventListener('touchend', function (e) {{
    if (x0 === null) return;
    var dx = e.changedTouches[0].clientX - x0; x0 = null;
    if (Math.abs(dx) > 50) show(at + (dx < 0 ? 1 : -1));
  }});
}})();
</script>
</body>
</html>
"""
outdir = pathlib.Path(out)
outdir.mkdir(parents=True, exist_ok=True)
(outdir / "index.html").write_text(page, encoding="utf-8")
(outdir / ".nojekyll").write_text("", encoding="utf-8")
print(f"Wrote {outdir / 'index.html'}: {len(plugins)} plug-ins, {len(inside)} in the installer")

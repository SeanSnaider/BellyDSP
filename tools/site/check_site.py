#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Checks the website in site/ before it ships. Standard library only.

    check_site.py [--site site] [--base http://127.0.0.1:8000]

From the files on disk: every page parses, has a lang, a title, a viewport, and unique ids; every
internal link, script, stylesheet, image (src and srcset), and CSS url() resolves to a file; every image
has alt text and its width and height (no layout shift); no page loads anything from another origin
(links may go to github.com and bellydsp.com only); no em or en dashes anywhere in the text; and the
weight of each page's first load (the HTML, its CSS, scripts, fonts, and eagerly loaded images).

With --base, it also fetches every page and every file the pages use from a running server (python -m
http.server in site/) and checks each answers 200, plus data/releases.json and data/stats.json if built.

Exit status 0 if everything passes, 1 otherwise; every problem is printed.
"""

import argparse
import html.parser
import re
import sys
import urllib.parse
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
ALLOWED_LINK_HOSTS = ("github.com", "bellydsp.com")   # and their subdomains (docs.github.com)
DASHES = re.compile("[\u2014\u2013]")


class Page(html.parser.HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.links, self.resources, self.ids, self.images = [], [], [], []
        self.lang = self.title = self.viewport = None
        self.in_title = False
        self.text = []

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if "id" in a:
            self.ids.append(a["id"])
        if tag == "html":
            self.lang = a.get("lang")
        if tag == "title":
            self.in_title = True
        if tag == "meta" and a.get("name") == "viewport":
            self.viewport = a.get("content")
        if tag == "a" and "href" in a:
            self.links.append(a["href"])
        if tag == "link" and "href" in a:
            rel = a.get("rel", "")
            if rel in ("canonical",):
                self.links.append(a["href"])
            else:
                self.resources.append((a["href"], rel, a))
        if tag == "script" and "src" in a:
            self.resources.append((a["src"], "script", a))
        if tag == "img":
            self.images.append(a)
            if "src" in a:
                self.resources.append((a["src"], "img" if a.get("loading") != "lazy" else "img-lazy", a))
            for part in a.get("srcset", "").split(","):
                if part.strip():
                    self.resources.append((part.strip().split()[0], "srcset", a))
        if tag in ("video", "audio", "source", "iframe", "embed", "object"):
            src = a.get("src") or a.get("data")
            if src:
                self.resources.append((src, tag, a))
        if tag == "meta" and a.get("property") == "og:image":
            self.links.append(a.get("content", ""))

    def handle_endtag(self, tag):
        if tag == "title":
            self.in_title = False

    def handle_data(self, data):
        if self.in_title:
            self.title = (self.title or "") + data
        self.text.append(data)


def resolve(site, page_path, ref):
    """The file a same-site reference points at, or None for other origins."""
    u = urllib.parse.urlparse(ref)
    if u.scheme in ("http", "https"):
        if u.hostname == "bellydsp.com":
            ref = u.path or "/"
        else:
            return None
    elif u.scheme or ref.startswith("#"):
        return None
    path = urllib.parse.unquote(urllib.parse.urlparse(ref).path)
    if not path:
        return page_path
    target = (site / path.lstrip("/")) if path.startswith("/") else (page_path.parent / path)
    target = Path(*target.parts).resolve()
    if path.endswith("/") or target.is_dir():
        target = target / "index.html"
    return target


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--site", default=str(REPO_ROOT / "site"))
    p.add_argument("--base", help="also fetch everything from this server")
    args = p.parse_args(argv[1:])
    site = Path(args.site).resolve()
    problems = []
    used = set()

    pages = sorted(site.rglob("*.html"))
    for page in pages:
        rel = page.relative_to(site)
        doc = Page()
        source = page.read_text(encoding="utf-8")
        doc.feed(source)
        if not doc.lang:
            problems.append(f"{rel}: no lang on <html>")
        if not (doc.title or "").strip():
            problems.append(f"{rel}: no <title>")
        if not doc.viewport:
            problems.append(f"{rel}: no viewport meta")
        dupes = {i for i in doc.ids if doc.ids.count(i) > 1}
        if dupes:
            problems.append(f"{rel}: duplicate ids {sorted(dupes)}")
        for m in DASHES.finditer(source):
            problems.append(f"{rel}: an em or en dash at offset {m.start()}")
        for img in doc.images:
            if not (img.get("alt") or "").strip():
                problems.append(f"{rel}: image without alt text: {img.get('src')}")
            if not img.get("width") or not img.get("height"):
                problems.append(f"{rel}: image without width/height: {img.get('src')}")
        for href in doc.links:
            u = urllib.parse.urlparse(href)
            if u.scheme in ("http", "https"):
                host = u.hostname or ""
                if not any(host == h or host.endswith("." + h) for h in ALLOWED_LINK_HOSTS):
                    problems.append(f"{rel}: link to another site: {href}")
                if u.scheme != "https":
                    problems.append(f"{rel}: insecure link: {href}")
                if host != "bellydsp.com":
                    continue
            elif u.scheme == "mailto" or href.startswith("#"):
                continue
            target = resolve(site, page, href)
            if target is not None and not target.is_file() and rel.name != "404.html":
                problems.append(f"{rel}: broken link {href} -> {target}")
            if target is not None and u.fragment and target.is_file():
                t = Page()
                t.feed(target.read_text(encoding="utf-8"))
                if u.fragment not in t.ids:
                    problems.append(f"{rel}: link {href}: no id '{u.fragment}' in {target.relative_to(site)}")
        for ref, kind, _ in doc.resources:
            u = urllib.parse.urlparse(ref)
            if u.scheme in ("http", "https", "//") or ref.startswith("//"):
                problems.append(f"{rel}: loads {kind} from another origin: {ref}")
                continue
            if rel.name == "404.html":
                target = site / ref.lstrip("/")
            else:
                target = resolve(site, page, ref)
            if target is None or not target.is_file():
                problems.append(f"{rel}: missing {kind} {ref}")
            else:
                used.add(target)

    # Stylesheets: url() references and dashes. Scripts: dashes, and no absolute URLs fetched.
    for css in sorted(site.rglob("*.css")):
        text = css.read_text(encoding="utf-8")
        for m in DASHES.finditer(text):
            problems.append(f"{css.relative_to(site)}: an em or en dash at offset {m.start()}")
        for ref in re.findall(r"url\(\s*['\"]?([^'\")]+)", text):
            if re.match(r"^(https?:)?//", ref):
                problems.append(f"{css.relative_to(site)}: loads from another origin: {ref}")
                continue
            target = (css.parent / ref).resolve()
            if not target.is_file():
                problems.append(f"{css.relative_to(site)}: missing {ref}")
            else:
                used.add(target)
    for js in sorted(site.rglob("*.js")):
        text = js.read_text(encoding="utf-8")
        for m in DASHES.finditer(text):
            problems.append(f"{js.relative_to(site)}: an em or en dash at offset {m.start()}")
        for ref in re.findall(r"fetch\(\s*['\"]([^'\"]+)", text):
            if re.match(r"^(https?:)?//", ref):
                problems.append(f"{js.relative_to(site)}: fetches from another origin: {ref}")

    # Weight of each page's first load: the HTML, its stylesheet and the fonts it declares, its scripts,
    # and images that aren't lazy (with srcset, the src is what a 1x screen typically picks).
    css_files = sorted(site.rglob("*.css"))
    fonts = sorted((site / "assets" / "fonts").glob("*.woff2"))
    print("first-load weight per page (uncompressed; GitHub Pages also gzips the text files):")
    for page in pages:
        doc = Page()
        doc.feed(page.read_text(encoding="utf-8"))
        files = {page}
        for ref, kind, _ in doc.resources:
            if kind in ("stylesheet", "script", "img", "icon", "preload"):
                t = resolve(site, page, ref) if page.name != "404.html" else site / ref.lstrip("/")
                if t and t.is_file():
                    files.add(t)
        if any(f.suffix == ".css" for f in files):
            files.update(fonts)
        total = sum(f.stat().st_size for f in files)
        print(f"  {str(page.relative_to(site)):24s} {total / 1024:7.1f} KB  ({len(files)} files)")
    everything = [f for f in site.rglob("*") if f.is_file() and "data" not in f.relative_to(site).parts]
    print(f"whole site (without data/): {sum(f.stat().st_size for f in everything) / 1024:.1f} KB in {len(everything)} files")
    unused = [f for f in everything if f.suffix in (".webp", ".woff2", ".css", ".js") and f not in used and f.resolve() not in used]
    for f in unused:
        problems.append(f"unused file: {f.relative_to(site)}")
    print(f"checked {len(pages)} pages, {len(css_files)} stylesheets, {len(list(site.rglob('*.js')))} scripts, "
          f"{len(used)} referenced files")

    if args.base:
        base = args.base.rstrip("/") + "/"
        urls = set()
        for page in pages:
            if page.name == "404.html":
                continue
            rel = page.relative_to(site).as_posix()
            urls.add(rel[: -len("index.html")] if rel.endswith("index.html") else rel)
        urls.update(f.relative_to(site).as_posix() for f in used)
        urls.update(f"data/{n}" for n in ("releases.json", "stats.json") if (site / "data" / n).is_file())
        ok = 0
        for path in sorted(urls):
            try:
                with urllib.request.urlopen(base + path, timeout=10) as r:
                    r.read()
                    if r.status == 200:
                        ok += 1
                    else:
                        problems.append(f"GET /{path}: HTTP {r.status}")
            except Exception as e:  # noqa: BLE001 (report everything)
                problems.append(f"GET /{path}: {e}")
        print(f"fetched {ok} of {len(urls)} URLs from {base} with HTTP 200")

    for pr in problems:
        print("PROBLEM", pr)
    print("OK: no problems" if not problems else f"{len(problems)} problem(s)")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))

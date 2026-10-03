#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Writes an appcast: the small RSS file Sparkle (macOS) and WinSparkle (Windows) read to learn about
the newest version. Standard library only.

    make_appcast.py --platform mac --version 0.1.1 --file dist/AmpSim-0.1.1-mac.zip \
        --url https://github.com/OWNER/REPO/releases/download/v0.1.1/AmpSim-0.1.1-mac.zip \
        --signature <base64 Ed25519 signature of the file> --notes release-notes/0.1.1.md \
        --out dist/appcast.xml

Each release's appcast lists just that release. The apps read it from the latest GitHub release
(".../releases/latest/download/appcast.xml"), so the newest release's appcast is the only one anyone
ever sees, and an older item would only matter for delta updates or OS-version fallbacks, which we
don't use.

Fields that matter (Sparkle's appcast format, which WinSparkle shares):
  sparkle:version             compared with the installed CFBundleVersion (macOS) or the version WinSparkle
                              was given (Windows); an update is offered only if it's higher
  sparkle:shortVersionString  what the user sees
  sparkle:minimumSystemVersion  older systems don't get offered it
  enclosure url/length        the download (the .zip for Sparkle, the installer .exe for WinSparkle)
  sparkle:edSignature         Ed25519 signature of the download; the app refuses anything that doesn't verify
  sparkle:installerArguments  (Windows) how WinSparkle runs the installer: silently, then reopen the app
  description                 release notes (HTML), shown in Sparkle's and WinSparkle's update dialogs
"""

import argparse
import html
import os
import re
import sys
from email.utils import formatdate

WINDOWS_INSTALLER_ARGS = "/SILENT /SP- /NOCANCEL /SUPPRESSMSGBOXES /NORESTART /RELAUNCH=1"


def inline_markdown(text):
    text = html.escape(text, quote=False)
    text = re.sub(r"`([^`]+)`", r"<code>\1</code>", text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"(?<![*\w])\*([^*]+)\*(?!\w)", r"<em>\1</em>", text)
    text = re.sub(r"\[([^\]]+)\]\((https?://[^)\s]+)\)", r'<a href="\2">\1</a>', text)
    return text


def markdown_to_html(source):
    """The small subset release notes need: headings, bullet lists, paragraphs, and inline code, bold,
    italics, and links."""
    out, paragraph, in_list = [], [], False

    def flush_paragraph():
        if paragraph:
            out.append("<p>" + inline_markdown(" ".join(paragraph)) + "</p>")
            paragraph.clear()

    for raw in source.splitlines():
        line = raw.rstrip()
        heading = re.match(r"^(#{1,4})\s+(.*)$", line)
        bullet = re.match(r"^\s*[-*]\s+(.*)$", line)
        if heading:
            flush_paragraph()
            if in_list:
                out.append("</ul>")
                in_list = False
            level = len(heading.group(1)) + 1  # "# x" becomes <h2>: the dialog has its own title
            out.append(f"<h{level}>{inline_markdown(heading.group(2))}</h{level}>")
        elif bullet:
            flush_paragraph()
            if not in_list:
                out.append("<ul>")
                in_list = True
            out.append("<li>" + inline_markdown(bullet.group(1)) + "</li>")
        elif not line.strip():
            flush_paragraph()
            if in_list:
                out.append("</ul>")
                in_list = False
        else:
            if in_list:
                out.append("</ul>")
                in_list = False
            paragraph.append(line.strip())
    flush_paragraph()
    if in_list:
        out.append("</ul>")
    return "\n".join(out)


def notes_html(path, version):
    if not path:
        return f"<p>Amp Sim {html.escape(version)}.</p>"
    with open(path, encoding="utf-8") as f:
        text = f.read()
    if path.lower().endswith((".html", ".htm")):
        return text
    return markdown_to_html(text)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--platform", choices=["mac", "windows"], required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--file", required=True, help="the update file, for its length")
    parser.add_argument("--url", required=True, help="where the apps download it")
    parser.add_argument("--signature", required=True, help="base64 Ed25519 signature of the file")
    parser.add_argument("--notes", help="release notes, .md or .html")
    parser.add_argument("--min-os", help="minimum OS version (default 11.0 on mac, 10.0 on windows)")
    parser.add_argument("--repo", default="", help="owner/name of the releases repo, for the channel link")
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv[1:])

    if not re.fullmatch(r"\d+\.\d+\.\d+", args.version):
        sys.exit("make_appcast.py: the version must be major.minor.patch")
    length = os.path.getsize(args.file)
    min_os = args.min_os or ("11.0" if args.platform == "mac" else "10.0")
    notes = notes_html(args.notes, args.version).replace("]]>", "]]&gt;")
    link = f"https://github.com/{args.repo}/releases" if args.repo else "https://github.com"

    enclosure = [
        f'url="{html.escape(args.url)}"',
        f'length="{length}"',
        'type="application/octet-stream"',
        f'sparkle:edSignature="{html.escape(args.signature)}"',
    ]
    if args.platform == "windows":
        enclosure.insert(0, 'sparkle:os="windows"')
        enclosure.append(f'sparkle:installerArguments="{WINDOWS_INSTALLER_ARGS}"')

    xml = f"""<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle" xmlns:dc="http://purl.org/dc/elements/1.1/">
  <channel>
    <title>Amp Sim</title>
    <link>{html.escape(link)}</link>
    <description>Amp Sim updates ({'macOS' if args.platform == 'mac' else 'Windows'})</description>
    <language>en</language>
    <item>
      <title>Amp Sim {args.version}</title>
      <pubDate>{formatdate(usegmt=True)}</pubDate>
      <sparkle:version>{args.version}</sparkle:version>
      <sparkle:shortVersionString>{args.version}</sparkle:shortVersionString>
      <sparkle:minimumSystemVersion>{min_os}</sparkle:minimumSystemVersion>
      <description><![CDATA[
{notes}
      ]]></description>
      <enclosure {' '.join(enclosure)}/>
    </item>
  </channel>
</rss>
"""
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(xml)
    print(f"wrote {args.out}: {args.platform} {args.version}, {length} bytes at {args.url}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

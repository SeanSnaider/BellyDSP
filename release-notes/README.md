# Release notes

One Markdown file per version, named `<version>.md` (for example `0.1.1.md`). `tools/release/release.sh`
puts it in the update's appcast, where Sparkle and WinSparkle show it in their update windows (when a
friend checks for updates by hand; silent automatic updates on macOS show nothing), and on the GitHub
release page.

Supported Markdown: `#` headings, `-` bullet lists, paragraphs, `` `code` ``, `**bold**`, `*italics*`,
and `[links](https://...)`. Write for friends, not for developers: what's new, what's fixed.

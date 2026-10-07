// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider
//
// The download page: works out which computer the visitor is on, puts that button first and makes it
// the primary one (both stay visible), and points the buttons at the latest release's files from
// data/releases.json. If the JSON is missing, a button keeps its fallback link to the latest release on
// GitHub; if the JSON just doesn't list a file yet, the button links straight to that version's file.
"use strict";

(function () {
  var B = window.BellyDSP;

  // "mac", "windows", "linux", "mobile", or "unknown". User-Agent Client Hints first (Chromium browsers;
  // its platform is "macOS", "Windows", ...), then the classic user agent string (Safari, Firefox).
  // An iPad asking for the desktop site says "Macintosh", but it has a touch screen; real Macs don't.
  function detectOS() {
    var uad = navigator.userAgentData;
    var platform = (uad && uad.platform) || "";
    var ua = navigator.userAgent || "";
    if ((uad && uad.mobile) || /Android|iPhone|iPad|iPod/i.test(ua)) return "mobile";
    if (/^mac/i.test(platform) || /Macintosh|Mac OS X/.test(ua)) {
      return navigator.maxTouchPoints > 1 ? "mobile" : "mac";
    }
    if (/^win/i.test(platform) || /Windows/.test(ua)) return "windows";
    if (/linux|chrome ?os|cros/i.test(platform) || /Linux|CrOS|X11/.test(ua)) return "linux";
    return "unknown";
  }

  var NAMES = { mac: "macOS", windows: "Windows" };
  var os = detectOS();
  var note = document.getElementById("os-note");

  function setNote(text, on) {
    note.textContent = "";
    if (on) {
      var dot = document.createElement("span");
      dot.className = "on";
      dot.setAttribute("aria-hidden", "true");
      note.appendChild(dot);
    }
    note.appendChild(document.createTextNode(text));
  }

  var detectedButton = document.querySelector('[data-os-button="' + os + '"]');
  if (detectedButton) {
    detectedButton.classList.add("primary", "detected");
    setNote("You're on " + NAMES[os] + ". The other download is right next to it.", true);
  } else if (os === "mobile") {
    setNote("BellyDSP runs on Mac and Windows computers, not phones or tablets. Open this page on your computer to download it.");
  } else if (os === "linux") {
    setNote("There's no Linux version yet. The source is open, so it may build there, but that hasn't been tried.");
  } else {
    setNote("Pick the download for your computer.");
  }

  B.releases.then(function (data) {
    if (!data) return;
    document.querySelectorAll("[data-releases-link]").forEach(function (a) { a.href = data.releases_url; });
    var latest = data.latest;
    if (latest) {
      document.querySelectorAll("[data-source-link]").forEach(function (a) {
        a.href = latest.source_url;
        a.textContent = "BellyDSP " + latest.version + " on GitHub";
      });
    }
    ["mac", "windows"].forEach(function (key) {
      var button = document.querySelector('[data-os-button="' + key + '"]');
      if (!button) return;
      var meta = button.querySelector("[data-meta]");
      var file = latest && latest.downloads ? latest.downloads[key] : null;
      if (file && file.url) {
        button.href = file.url;
        meta.textContent = "Version " + latest.version + ", " + B.formatSize(file.size) + ", " + (key === "mac" ? ".dmg" : "installer");
      } else if (latest && latest.tag) {
        // Not in the data yet. The site's data is regenerated when a deploy runs, and the Windows installer
        // is attached to a release after the Mac files (once its CI build is green), so the data can briefly
        // miss it. The release's file names are fixed (tools/release/lib.sh), so link straight to the file
        // for this version: a direct download, not the releases page (Sean, 2026-10-06).
        var name = key === "mac" ? "BellyDSP-" + latest.version + ".dmg" : "BellyDSP-" + latest.version + "-windows-setup.exe";
        button.href = data.releases_url + "/download/" + latest.tag + "/" + name;
        meta.textContent = "Version " + latest.version + ", " + (key === "mac" ? ".dmg" : "installer");
      } else {
        button.href = data.latest_url;
        meta.textContent = latest ? "Not in version " + latest.version + " yet: see GitHub" : "No release yet: see GitHub";
      }
    });
    renderChangelog(data.changelog || []);
  });

  // The release notes, newest first. notes_html is generated at deploy time from the repo's own
  // release-notes/*.md (escaped Markdown, tools/release/make_appcast.py), never from visitor input.
  function renderChangelog(entries) {
    if (!entries.length) return;
    var box = document.getElementById("changelog");
    box.textContent = "";
    entries.forEach(function (e) {
      var article = document.createElement("article");
      var h = document.createElement("h3");
      var link = document.createElement("a");
      link.href = e.html_url;
      link.textContent = "BellyDSP " + e.version;
      h.appendChild(link);
      if (e.published_at) {
        var date = document.createElement("span");
        date.className = "dim";
        date.textContent = B.formatDate(e.published_at);
        h.appendChild(date);
      }
      var body = document.createElement("div");
      body.innerHTML = e.notes_html;
      article.appendChild(h);
      article.appendChild(body);
      box.appendChild(article);
    });
  }
})();

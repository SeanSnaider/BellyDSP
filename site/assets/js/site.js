// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider
//
// Shared by every page: loads data/releases.json (written at deploy time by tools/site/build_site_data.py
// from GitHub's release API; the browser never calls GitHub itself) and fills in the version line.
// Everything here is an enhancement: without it, or without the JSON, the pages still work and their
// links go to the latest release on GitHub.
"use strict";

(function () {
  var root = document.documentElement.getAttribute("data-root") || "./";

  function getJSON(name) {
    return fetch(root + "data/" + name, { cache: "no-cache" })
      .then(function (r) { if (!r.ok) throw new Error(name + ": HTTP " + r.status); return r.json(); });
  }

  function formatDate(iso) {
    if (!iso) return "";
    var d = new Date(iso);
    if (isNaN(d)) return "";
    return d.toLocaleDateString("en", { year: "numeric", month: "long", day: "numeric", timeZone: "UTC" });
  }

  function formatSize(bytes) {
    if (!bytes) return "";
    return (bytes / 1048576).toFixed(bytes >= 104857600 ? 0 : 1) + " MB";
  }

  var releases = getJSON("releases.json").catch(function () { return null; });

  releases.then(function (data) {
    if (!data) return;
    if (data.source === "fixture") {
      document.querySelectorAll("[data-fixture-notice]").forEach(function (el) { el.hidden = false; });
    }
    if (!data.latest) return;
    document.querySelectorAll("[data-version-line]").forEach(function (el) {
      el.textContent = "Version " + data.latest.version + (data.latest.published_at ? ", released " + formatDate(data.latest.published_at) : "");
      el.hidden = false;
    });
  });

  window.BellyDSP = { root: root, getJSON: getJSON, releases: releases, formatDate: formatDate, formatSize: formatSize };
})();

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider
//
// The overview's two enhancements. Without this script the page is complete: the shelf's links jump to each
// amp's band, and every download link goes to the download page.
//   1. The shelf follows the band you're in: the amp whose band crosses the reading line (a little below the
//      shelf) is marked current, and the emerald light glides under it.
//   2. The big download buttons go straight to the installer for this computer (from data/releases.json), and the
//      second hero button offers the other system. The top bar's button keeps going to the download page, where
//      the first-launch steps are (the app isn't signed with a paid certificate, so both systems ask once).
"use strict";

(function () {
  var B = window.BellyDSP;

  // ---- The shelf ----------------------------------------------------------------------------------------
  var shelf = document.querySelector(".shelf ol");
  var light = document.querySelector(".shelf-light");
  var links = Array.prototype.slice.call(document.querySelectorAll(".shelf a[data-amp]"));
  var bands = links.map(function (a) { return document.getElementById(a.getAttribute("data-amp")); }).filter(Boolean);
  var current = null;

  function place() {
    if (!current || !shelf || !light) return;
    var s = shelf.getBoundingClientRect(), r = current.querySelector(".mini").getBoundingClientRect();
    light.style.setProperty("--x", (r.left - s.left + shelf.scrollLeft) + "px");
    light.style.setProperty("--w", r.width + "px");
  }

  function setCurrent(id) {
    links.forEach(function (a) {
      var on = a.getAttribute("data-amp") === id;
      if (on) { a.setAttribute("aria-current", "true"); current = a; } else { a.removeAttribute("aria-current"); }
    });
    place();
    // On narrow screens the shelf scrolls sideways: keep the current amp in view without moving the page.
    if (current && shelf.scrollWidth > shelf.clientWidth) {
      var li = current.parentNode, target = li.offsetLeft - (shelf.clientWidth - li.offsetWidth) / 2;
      shelf.scrollTo({ left: Math.max(0, target), behavior: matchMedia("(prefers-reduced-motion: reduce)").matches ? "auto" : "smooth" });
    }
  }

  if (links.length && bands.length) {
    setCurrent(bands[0].id);
    // The reading line: the band under a point 30% of the way down the space below the chrome is current.
    // Between amp bands (the calm sections) the last amp passed stays lit.
    var onScroll = function () {
      var chrome = document.querySelector(".chrome").getBoundingClientRect().bottom;
      var line = chrome + (window.innerHeight - chrome) * 0.3, id = bands[0].id;
      for (var i = 0; i < bands.length; i++) {
        if (bands[i].getBoundingClientRect().top <= line) id = bands[i].id;
      }
      if (!current || current.getAttribute("data-amp") !== id) setCurrent(id);
    };
    var ticking = false;
    window.addEventListener("scroll", function () {
      if (ticking) return;
      ticking = true;
      window.requestAnimationFrame(function () { ticking = false; onScroll(); });
    }, { passive: true });
    window.addEventListener("resize", place);
    if (document.fonts && document.fonts.ready) document.fonts.ready.then(place);
    onScroll();
  }

  // ---- Downloads for this computer ---------------------------------------------------------------------
  function detectOS() {
    var uad = navigator.userAgentData, platform = (uad && uad.platform) || "", ua = navigator.userAgent || "";
    if ((uad && uad.mobile) || /Android|iPhone|iPad|iPod/i.test(ua)) return "mobile";
    if (/^mac/i.test(platform) || /Macintosh|Mac OS X/.test(ua)) return navigator.maxTouchPoints > 1 ? "mobile" : "mac";
    if (/^win/i.test(platform) || /Windows/.test(ua)) return "windows";
    return "other";
  }
  var os = detectOS(), NAMES = { mac: "macOS", windows: "Windows" };
  var primary = document.querySelectorAll('[data-dl="primary"]');
  var other = document.querySelector('[data-dl="other"]');
  var bar = document.querySelector('[data-dl="bar"]');

  if (os === "mac" || os === "windows") {
    var alt = os === "mac" ? "windows" : "mac";
    primary.forEach(function (a) { a.textContent = "Download for " + NAMES[os]; });
    if (other) { other.textContent = NAMES[alt]; other.href = "download/#" + alt; }
    if (bar) {
      var name = document.createElement("span");
      name.className = "os-name";
      name.textContent = " for " + NAMES[os];
      bar.textContent = "Download";
      bar.appendChild(name);
    }
    B.releases.then(function (data) {
      var latest = data && data.latest;
      if (!latest) return;
      var file = (latest.downloads || {})[os];
      var url = file && file.url ? file.url
        : data.releases_url + "/download/" + latest.tag + "/" + (os === "mac" ? "BellyDSP-" + latest.version + ".dmg" : "BellyDSP-" + latest.version + "-windows-setup.exe");
      primary.forEach(function (a) { a.href = url; });
    });
  }
})();

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider
//
// The stats page: renders data/stats.json (built at deploy time by tools/site/build_site_data.py from
// GitHub's release API and the stats branch's daily snapshots). Two small line charts in plain SVG, one
// per system on a shared scale (small multiples rather than two colours, because the one accent colour
// means "active", not "series 2"), each with a crosshair tooltip that also works from the keyboard,
// and the same numbers as a table.
"use strict";

(function () {
  var B = window.BellyDSP;
  var SVG = "http://www.w3.org/2000/svg";
  var fmt = new Intl.NumberFormat("en");

  function el(name, attrs, parent) {
    var e = document.createElementNS(SVG, name);
    for (var k in attrs) e.setAttribute(k, attrs[k]);
    if (parent) parent.appendChild(e);
    return e;
  }

  function cell(row, text, tag) {
    var c = document.createElement(tag || "td");
    if (tag === "th") c.setAttribute("scope", "row");
    c.textContent = text;
    row.appendChild(c);
  }

  function shortDate(iso) {
    var d = new Date(iso + "T00:00:00Z");
    return d.toLocaleDateString("en", { month: "short", day: "numeric", timeZone: "UTC" });
  }

  // A round number at or above max, so the top gridline gets a clean label.
  function niceCeil(max) {
    if (max <= 4) return 4;
    var p = Math.pow(10, Math.floor(Math.log10(max)));
    var steps = [1, 2, 2.5, 5, 10];
    for (var i = 0; i < steps.length; i++) if (steps[i] * p >= max) return steps[i] * p;
    return 10 * p;
  }

  function chart(container, title, points, key, yMax) {
    var W = 640, H = 220, L = 40, R = 12, T = 14, Bm = 28;
    var pw = W - L - R, ph = H - T - Bm;
    var n = points.length;
    var box = document.createElement("div");
    box.className = "chart";
    var h = document.createElement("h3");
    var last = points[n - 1];
    h.textContent = title + ": " + fmt.format(last[key]) + " on " + shortDate(last.date);
    box.appendChild(h);
    container.appendChild(box);

    var x = function (i) { return L + (n === 1 ? pw / 2 : (i / (n - 1)) * pw); };
    var y = function (v) { return T + ph - (v / yMax) * ph; };
    var svg = el("svg", { viewBox: "0 0 " + W + " " + H, role: "group", tabindex: "0",
      "aria-label": title + ", copies in use per day, " + n + " days from " + shortDate(points[0].date) + " to " + shortDate(last.date) +
        ". Use the left and right arrow keys to read each day." }, box);

    [0, yMax / 2, yMax].forEach(function (v) {
      el("line", { x1: L, x2: W - R, y1: y(v), y2: y(v), "class": "grid-line" }, svg);
      var t = el("text", { x: L - 8, y: y(v) + 4, "text-anchor": "end", "class": "axis-label" }, svg);
      t.textContent = fmt.format(v);
    });
    var labels = n > 2 ? [0, Math.floor((n - 1) / 2), n - 1] : (n === 2 ? [0, 1] : [0]);
    labels.forEach(function (i, j) {
      var anchor = n === 1 ? "middle" : (j === 0 ? "start" : (i === n - 1 ? "end" : "middle"));
      var t = el("text", { x: x(i), y: H - 8, "text-anchor": anchor, "class": "axis-label" }, svg);
      t.textContent = shortDate(points[i].date);
    });

    // Solid line, except where a snapshot was missed: that stretch (an average over the gap) is dotted.
    var solid = "", dotted = "";
    for (var i = 0; i < n; i++) {
      var px = x(i).toFixed(1), py = y(points[i][key]).toFixed(1);
      if (i === 0) { solid += "M" + px + " " + py; continue; }
      var prev = x(i - 1).toFixed(1) + " " + y(points[i - 1][key]).toFixed(1);
      if (points[i].days > 1) { dotted += "M" + prev + "L" + px + " " + py; solid += "M" + px + " " + py; }
      else solid += "L" + px + " " + py;
    }
    if (n === 1) el("circle", { cx: x(0), cy: y(points[0][key]), r: 4, "class": "dot" }, svg);
    el("path", { d: solid, "class": "series" }, svg);
    if (dotted) el("path", { d: dotted, "class": "gap-mark" }, svg);

    var cross = el("line", { y1: T, y2: T + ph, "class": "crosshair", visibility: "hidden" }, svg);
    var dot = el("circle", { r: 4, "class": "dot", visibility: "hidden" }, svg);
    var tip = document.createElement("div");
    tip.className = "tooltip";
    tip.hidden = true;
    tip.setAttribute("role", "status");
    box.appendChild(tip);

    var current = -1;
    function show(i) {
      current = i;
      var p = points[i];
      cross.setAttribute("x1", x(i)); cross.setAttribute("x2", x(i)); cross.setAttribute("visibility", "visible");
      dot.setAttribute("cx", x(i)); dot.setAttribute("cy", y(p[key])); dot.setAttribute("visibility", "visible");
      tip.textContent = "";
      var strong = document.createElement("strong");
      strong.textContent = fmt.format(p[key]);
      tip.appendChild(strong);
      tip.appendChild(document.createTextNode(shortDate(p.date) + (p.days > 1 ? ", averaged over " + p.days + " days" : "")));
      var r = svg.getBoundingClientRect(), b = box.getBoundingClientRect();
      var left = r.left - b.left + (x(i) / W) * r.width;
      var top = r.top - b.top + (y(p[key]) / H) * r.height - 10;
      tip.style.left = Math.max(60, Math.min(b.width - 60, left)) + "px";
      tip.style.top = top + "px";
      tip.hidden = false;
    }
    function hide() {
      cross.setAttribute("visibility", "hidden"); dot.setAttribute("visibility", "hidden"); tip.hidden = true;
    }
    svg.addEventListener("pointermove", function (e) {
      var r = svg.getBoundingClientRect();
      var vx = ((e.clientX - r.left) / r.width) * W;
      show(n === 1 ? 0 : Math.max(0, Math.min(n - 1, Math.round(((vx - L) / pw) * (n - 1)))));
    });
    svg.addEventListener("pointerleave", function () { if (document.activeElement !== svg) hide(); });
    svg.addEventListener("focus", function () { show(current < 0 ? n - 1 : current); });
    svg.addEventListener("blur", hide);
    svg.addEventListener("keydown", function (e) {
      if (e.key === "ArrowLeft" || e.key === "ArrowRight") {
        e.preventDefault();
        show(Math.max(0, Math.min(n - 1, (current < 0 ? n - 1 : current) + (e.key === "ArrowLeft" ? -1 : 1))));
      } else if (e.key === "Home") { e.preventDefault(); show(0); }
      else if (e.key === "End") { e.preventDefault(); show(n - 1); }
    });
  }

  function render(s) {
    var t = s.totals;
    var values = {
      downloads_total: s.downloads_total,
      mac_install: t.mac.install,
      windows_install: t.windows.install,
      updates: t.mac.update + t.windows.update
    };
    document.querySelectorAll("[data-stat]").forEach(function (e) {
      var v = values[e.getAttribute("data-stat")];
      if (v !== undefined) e.textContent = fmt.format(v);
    });
    var sub = document.querySelector('[data-stat-sub="updates"]');
    if (sub) sub.textContent = "macOS " + fmt.format(t.mac.update) + ", Windows " + fmt.format(t.windows.update);

    var updated = document.getElementById("updated");
    if (s.generated_at) {
      updated.textContent = "Counts as of " + B.formatDate(s.generated_at) + ". Daily snapshots: " +
        (s.snapshots.count ? s.snapshots.count + " since " + B.formatDate(s.snapshots.first) : "none yet") + ".";
    }

    var charts = document.getElementById("charts");
    var active = s.active || [];
    if (!active.length) {
      var p = document.createElement("p");
      p.className = "dim";
      p.textContent = "No estimates yet: they start once there are two daily snapshots to compare.";
      charts.appendChild(p);
    } else {
      var max = 0;
      active.forEach(function (a) { max = Math.max(max, a.mac, a.windows); });
      var yMax = niceCeil(max);
      chart(charts, "macOS", active, "mac", yMax);
      chart(charts, "Windows", active, "windows", yMax);
      var tbody = document.querySelector("#active-table tbody");
      active.slice().reverse().forEach(function (a) {
        var row = document.createElement("tr");
        cell(row, a.date + (a.days > 1 ? " (average of " + a.days + " days)" : ""), "th");
        cell(row, fmt.format(a.mac));
        cell(row, fmt.format(a.windows));
        tbody.appendChild(row);
      });
    }

    var rel = document.querySelector("#release-table tbody");
    (s.releases || []).forEach(function (r) {
      var row = document.createElement("tr");
      var th = document.createElement("th");
      th.setAttribute("scope", "row");
      var a = document.createElement("a");
      a.href = r.html_url;
      a.textContent = r.version;
      th.appendChild(a);
      row.appendChild(th);
      [r.mac_install, r.windows_install, r.mac_update, r.windows_update, r.mac_appcast + r.windows_appcast].forEach(function (v) {
        cell(row, fmt.format(v));
      });
      rel.appendChild(row);
    });
    if (!(s.releases || []).length) document.getElementById("no-data").hidden = false;
  }

  B.getJSON("stats.json").then(render).catch(function () {
    document.getElementById("no-data").hidden = false;
  });
})();

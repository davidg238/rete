// Copyright (c) 2026 Ekorau LLC
// fleet.js — helpers shared by /fleet and /node/{id}.
"use strict";

// Everything a node or porta wrote goes in through text nodes, never innerHTML:
// node ids, roles and reasons are data, never markup.
function el(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (k === "class") e.className = v; else e.setAttribute(k, v);
  }
  for (const k of kids) {
    if (k == null) continue;
    e.append(k instanceof Node ? k : document.createTextNode(String(k)));
  }
  return e;
}

function rows(answer) {
  return answer.rows.map(r => Object.fromEntries(answer.columns.map((c, i) => [c, r[i]])));
}

function age(ms, now) {
  if (ms == null) return "never";
  const s = Math.max(0, Math.round((now - ms) / 1000));
  if (s < 90) return s + " s ago";
  const m = Math.round(s / 60);
  if (m < 90) return m + " min ago";
  const h = Math.round(m / 60);
  if (h < 48) return h + " h ago";
  return Math.round(h / 24) + " d ago";
}

function clock(ms) {
  return new Date(ms).toLocaleString([], { dateStyle: "medium", timeStyle: "medium" });
}

// The same classification as serverstat.TransportOf: an IPv6 source is the
// Thread side of the dual-stack socket, IPv4 (incl. v4-mapped) is WiFi.
function transport(addr) {
  if (!addr) return "—";
  const host = addr.startsWith("[") ? addr.slice(1, addr.indexOf("]")) : addr.replace(/:\d+$/, "");
  if (/^(::ffff:)?\d+\.\d+\.\d+\.\d+$/i.test(host)) return "WiFi";
  return host.includes(":") ? "Thread" : "WiFi";
}

function pill(verdict) {
  const v = verdict == null ? "none" : verdict;
  return el("span", { class: "pill v-" + v }, verdict == null ? "not judged" : "#" + verdict);
}

async function getJSON(path) {
  const r = await fetch(path, { cache: "no-store" });
  const text = await r.text();
  if (!r.ok) throw new Error(path + " → " + r.status + ": " + text.trim());
  return JSON.parse(text);
}

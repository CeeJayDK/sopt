// Shared by the SweetOpt / GPU Blueprint pages: header, footer, helpers. Each page's <body> has data-root (the way
// back to ceejay.dk/sopt/, e.g. "../") and data-page (which nav entry is current).
"use strict";

const ROOT = document.body.dataset.root || "";
const VENDOR_COLOR = { NVIDIA: "var(--nvidia)", AMD: "var(--amd)", Intel: "var(--intel)", Qualcomm: "var(--qualcomm)", "Moore Threads": "var(--moore)" };

function el(tag, attrs = {}, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v == null) continue;
    if (k === "style") e.style.cssText = v; else if (k === "text") e.textContent = v; else e.setAttribute(k, v);
  }
  for (const k of kids) if (k != null && k !== false) e.append(k);
  return e;
}

function loadJSON(name) {
  return fetch(ROOT + "data/" + name + ".json").then((r) => r.json());
}

// Cost models count free modifiers as 1 quarter unit (so search levels stay well-founded); 4 quarters = one fma.
const fma = (q) => q / 4;
const isFree = (q) => q <= 1;
function num(v) {
  if (v >= 100) return v.toFixed(0);
  if (v >= 10) return v.toFixed(1).replace(/\.0$/, "");
  return v.toFixed(2).replace(/\.?0+$/, "");
}
const fmtCost = (q) => (isFree(q) ? "free" : num(fma(q)));

function barRow(name, value, scale, color, opts = {}) {
  const free = opts.free === true;
  const w = free ? 0 : Math.max(0, Math.min(100, (value / scale) * 100));
  return el("div", { class: opts.wide ? "row wide" : "row", title: opts.title },
    el("span", { class: "name", text: name }),
    el("div", { class: "track" }, el("div", { class: "bar" + (free ? " free" : ""), style: `width:${w}%;--c:${color}` })),
    el("span", { class: "val" + (free ? " free" : ""), text: opts.label ?? (free ? "free" : num(value)) }));
}

// Shader code in VS Code's dark colors (owner, 2026-10-10): types coral, keywords violet, functions yellow, numbers
// green, semantics blue, brackets by depth (gold, orchid, blue); names and operators as text.
const CODE_TOKEN = /(0x[0-9a-fA-F]+u?|\d+\.?\d*(?:e[+-]?\d+)?[uf]?|\.\d+(?:e[+-]?\d+)?)|([A-Za-z_]\w*)|([()[\]{}])|([\s\S])/g;
const CODE_TYPE = /^(?:void|bool|int|uint|half|float|double|min16float|min16int|min16uint)[1-4]?(?:x[1-4])?$|^(?:texture|sampler|storage)\w*$/;
const CODE_KEYWORD = new Set(["return", "if", "else", "for", "while", "do", "break", "continue", "switch", "case", "default",
  "discard", "const", "static", "uniform", "precise", "in", "out", "inout", "namespace", "struct", "technique", "pass",
  "true", "false"]);
function hl(text) {
  const out = document.createDocumentFragment();
  const open = "([{", close = ")]}";
  let depth = 0;
  for (const m of text.matchAll(CODE_TOKEN)) {
    let cls = null;
    if (m[1]) cls = "tk-n";
    else if (m[2]) {
      const after = text.slice(m.index + m[0].length).trimStart();
      cls = CODE_TYPE.test(m[2]) ? "tk-t" : CODE_KEYWORD.has(m[2]) ? "tk-k" : after.startsWith("(") ? "tk-f"
          : /^(SV_\w+|[A-Z][A-Z0-9_]*\d*)$/.test(m[2]) && /:\s*$/.test(text.slice(0, m.index)) ? "tk-s" : null;
    } else if (m[3]) {
      if (close.includes(m[3])) depth = Math.max(0, depth - 1);
      cls = "tk-b" + (depth % 3);
      if (open.includes(m[3])) depth++;
    }
    out.append(cls ? el("span", { class: cls, text: m[0] }) : m[0]);
  }
  return out;
}

// Heat colors: the warming stripes' 16 shades (Ed Hawkins; ColorBrewer Blues / Reds), dark blue = cheapest, dark
// red = most expensive, in steps on a log scale between lo and hi (owner, 2026-10-10). Twice the steps (owner,
// 2026-10-20): each shade, and between two neighbours the mix of both (31 shades).
const STRIPES = ["#08306b", "#08519c", "#2171b5", "#4292c6", "#6baed6", "#9ecae1", "#c6dbef", "#deebf7",
  "#fee0d2", "#fcbba1", "#fc9272", "#fb6a4a", "#ef3b2c", "#cb181d", "#a50f15", "#67000d"].flatMap((c, k, all) => {
  if (!k) return [c];
  const mid = [1, 3, 5].map((i) => Math.round((parseInt(all[k - 1].substr(i, 2), 16) + parseInt(c.substr(i, 2), 16)) / 2));
  return ["#" + mid.map((v) => v.toString(16).padStart(2, "0")).join(""), c];
});
function heatColor(v, lo, hi) {
  const t = hi > lo ? Math.max(0, Math.min(1, Math.log(v / lo) / Math.log(hi / lo))) : 0;
  const k = Math.min(STRIPES.length - 1, Math.floor(t * STRIPES.length));
  return { bg: STRIPES[k], fg: "#fff" };  // white with a drop shadow (td.hot) on every shade (owner, 2026-10-20)
}
// The scale under a heat table: each shade with the value where it starts.
function heatScale(lo, hi, unit) {
  return el("div", { class: "scale" }, el("span", { class: "lab", text: "cheaper" }),
    ...STRIPES.map((c, k) => el("span", { class: "sw", style: `background:${c}`, title: `from ${num(lo * Math.pow(hi / lo, k / STRIPES.length))}` })),
    el("span", { class: "lab", text: "more expensive" }),
    el("span", { class: "range", text: `${num(lo)} to ${num(hi)} ${unit}, log scale` }));
}

// Vendor names in their brand colors wherever they appear in the page content, also in what is drawn later
// (owner, 2026-10-10: NVIDIA green, AMD red, Intel blue). Not in <option> (no colors there) or code.
const VENDOR_WORD = /\b(NVIDIA|AMD|Intel|Qualcomm|Moore Threads)\b/;
function brand(root) {
  const skip = (n) => !n.parentElement || n.parentElement.closest(".vn, option, select, code, pre, script, style");
  const found = [];
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
  for (let n = walker.nextNode(); n; n = walker.nextNode()) if (VENDOR_WORD.test(n.data) && !skip(n)) found.push(n);
  for (const n of found) {
    const parts = n.data.split(new RegExp(VENDOR_WORD.source, "g"));  // odd indices are the vendor names
    n.replaceWith(...parts.map((p, k) => (k % 2 ? el("span", { class: "vn", style: `color:${VENDOR_COLOR[p]}`, text: p }) : p)));
  }
}
document.addEventListener("DOMContentLoaded", () => {
  const main = document.querySelector("main");
  if (!main) return;
  brand(main);
  new MutationObserver((records) => {
    for (const r of records)
      for (const n of r.addedNodes) if (n.nodeType === 1 ? !n.classList.contains("vn") : n.parentElement) brand(n.nodeType === 1 ? n : n.parentElement);
  }).observe(main, { childList: true, subtree: true });
});

(function frame() {
  const page = document.body.dataset.page;
  const link = (href, text, key) => el("a", { href: ROOT + href, class: page === key ? "on" : null, text });
  document.body.prepend(el("header", { class: "top" },
    el("a", { class: "logo", href: "https://ceejay.dk/" }, el("span", { class: "dot" }), "CeeJay.dk"),
    el("nav", { class: "site" },
      link("", "sweetopt", "sweetopt"),
      link("library/", "rewrite library", "library"),
      link("gpu-blueprint/", "gpu blueprint", "blueprint"),
      link("gpu-blueprint/texbench/", "texbench", "texbench"),
      link("gpu-blueprint/stagebench/", "stagebench", "stagebench"),
      el("a", { href: "https://github.com/CeeJayDK/sopt", text: "github" }))));
  document.body.append(el("footer", { class: "site" },
    el("p", { text: "SweetOpt and GPU Blueprint by CeeJay.dk. Free software (GPL-3.0): github.com/CeeJayDK/sopt" }),
    el("p", { text: "GPU Blueprint reports are published with the card, its driver version and the measurements only." })));
})();

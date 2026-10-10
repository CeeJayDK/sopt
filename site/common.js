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
      el("a", { href: "https://github.com/CeeJayDK/sopt", text: "github" }))));
  document.body.append(el("footer", { class: "site" },
    el("p", { text: "SweetOpt and GPU Blueprint by CeeJay.dk. Free software (GPL-3.0): github.com/CeeJayDK/sopt" }),
    el("p", { text: "GPU Blueprint reports are published with the card, its driver version and the measurements only." })));
})();

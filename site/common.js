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

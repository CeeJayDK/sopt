// TexBench findings (data/texbench.json from tools/site/build.py).
"use strict";

const FILTERS = ["Load", "point", "bilinear", "gather", "trilinear", "aniso 2x", "aniso 4x", "aniso 8x", "aniso 16x"];
const COMPARE = [
  ["RGBA8 bilinear", "RGBA8 bilinear"],
  ["RGBA16F bilinear", "RGBA16F bilinear"],
  ["RGBA32F bilinear", "RGBA32F bilinear"],
  ["RGBA8 trilinear", "RGBA8 trilinear"],
  ["RGBA8 aniso 16x", "RGBA8 aniso 16x"],
  ["RGBA8 random read", "RGBA8 random"],
  ["3D LUT 64³", "3D 64^3"],
  ["2D LUT 64 (2 reads)", "2D 64 (2 reads)"],
  ["ddx", "ddx"],
];

function vendorOf(name) {
  return Object.keys(VENDOR_COLOR).find((v) => name.toUpperCase().startsWith(v.toUpperCase())) || "";
}
function shortCard(name) { return name.replace(/^(NVIDIA GeForce|AMD Radeon|Intel) /, "").replace(/ (Graphics|GPU)$/, ""); }
function shortTitle(title) { return title.replace(/\s*\(.*\)\s*$/, "").replace(/:.*$/, ""); }
function rest(title) { const i = title.indexOf(":"); return i < 0 ? "" : title.slice(i + 1).trim(); }
function findRow(card, test) {
  for (const s of card.sections) for (const r of s.rows) if (r.test === test) return r;
  return null;
}
// Heat colour for a cost in fma units: cheap = cyan, expensive = red, on a log scale.
function heat(v, lo, hi) {
  const t = Math.max(0, Math.min(1, Math.log(v / lo) / Math.log(hi / lo)));
  const stops = [[94, 234, 212], [245, 166, 35], [255, 97, 102]];
  const i = t < 0.5 ? 0 : 1, f = t < 0.5 ? t * 2 : (t - 0.5) * 2;
  const c = stops[i].map((a, k) => Math.round(a + (stops[i + 1][k] - a) * f));
  return `rgb(${c.join(",")})`;
}
function orderSummary(card) {
  let best = null;
  for (const o of card.order) {
    const m = o.test.match(/^block (\d+)$/);
    if (m && parseFloat(o.value) >= 0.75) best = { n: +m[1], shape: (o.note.match(/shape (\d+ x \d+)/) || [])[1] };
  }
  return best ? `${best.n} pixels shaded together${best.shape ? ` (${best.shape} blocks)` : ""}` : "";
}

function matrix(sec) {
  const cells = {};
  const formats = [];
  for (const r of sec.rows) {
    const f = FILTERS.find((x) => r.test.endsWith(" " + x));
    if (!f || r.tput == null) continue;
    const fmt = r.test.slice(0, -(f.length + 1));
    if (!formats.includes(fmt)) formats.push(fmt);
    cells[fmt + "|" + f] = r;
  }
  const vals = Object.values(cells).map((r) => r.tput).filter((v) => v > 0);
  const lo = Math.min(...vals), hi = Math.max(...vals);
  return el("div", { class: "tablewrap" }, el("table", { class: "heat" },
    el("thead", {}, el("tr", {}, el("th", { text: "Format" }), ...FILTERS.map((f) => el("th", { class: "num", text: f })))),
    el("tbody", {}, ...formats.map((fmt) => el("tr", {}, el("td", { text: fmt }), ...FILTERS.map((f) => {
      const r = cells[fmt + "|" + f];
      if (!r) return el("td", { class: "num empty", text: "-" });
      return el("td", { class: "num", style: `background:${heat(Math.max(r.tput, lo), lo, hi)}`, text: num(r.tput),
        title: `${r.test}: ${num(r.tput)} fma` + (r.lat != null ? `, latency ${num(r.lat)}` : "") });
    }))))));
}

function sectionPanel(sec, color) {
  const isMatrix = /^Formats and filtering/.test(sec.title);
  const head = el("div", { class: "mhead" }, el("h3", { text: shortTitle(sec.title) }), el("span", { class: "basis", text: rest(sec.title) }));
  if (isMatrix) return el("div", { class: "panel" }, head, el("p", { class: "note", style: "margin-top:8px", text: "Cost per read in fma units, every format and filter (integer formats: Load and gather only)." }), matrix(sec));
  const kind = sec.kind;
  const key = kind === "cost" ? "tput" : "value";
  const rows = sec.rows.filter((r) => r[key] != null);
  const scale = Math.max(...rows.map((r) => r[key]), 1e-9);
  const unit = kind === "write" ? "GB/s, higher is better" : kind === "cost" ? "fma units, lower is better" : "ms per pass, lower is better";
  return el("div", { class: "panel" }, head,
    el("p", { class: "note", style: "margin-top:8px", text: unit }),
    ...rows.map((r) => barRow(r.test, r[key], scale, color, { wide: true,
      title: (r.note ? r.note + " · " : "") + (r.lat != null ? `latency ${num(r.lat)} fma` : "") })));
}

fetch(ROOT + "data/texbench-findings.html").then((r) => (r.ok ? r.text() : "")).then((h) => {
  document.getElementById("findings").innerHTML = h || '<p class="note">No findings yet.</p>';
});

loadJSON("texbench").then((data) => {
  const cards = data.cards;
  // Comparison table.
  document.getElementById("compare").replaceChildren(
    el("thead", {}, el("tr", {}, el("th", { text: "Card" }), ...COMPARE.map(([label]) => el("th", { class: "num", text: label })))),
    el("tbody", {}, ...cards.map((c) => el("tr", {},
      el("td", { title: c.name, style: "white-space:nowrap" }, el("span", { class: "dot", style: `--c:${VENDOR_COLOR[vendorOf(c.name)]}` }), shortCard(c.name)),
      ...COMPARE.map(([, test]) => {
        const r = findRow(c, test);
        return el("td", { class: "num", text: r && r.tput != null ? num(r.tput) : "-" });
      })))));

  // One card.
  const sel = document.getElementById("card");
  cards.forEach((c, i) => sel.append(el("option", { value: String(i), text: c.name })));
  const show = () => {
    const c = cards[+sel.value];
    const color = VENDOR_COLOR[vendorOf(c.name)] || "var(--cyan)";
    const scores = Object.entries(c.scores).map(([k, v]) => el("div", {}, el("b", { text: v }), el("span", { text: k })));
    document.getElementById("detail").replaceChildren(
      el("div", { class: "scores" }, ...scores,
        el("div", {}, el("b", { text: c.runTime || "-" }), el("span", { text: `run time · driver ${c.driver}` }))),
      c.orderImage ? el("div", { class: "panel" },
        el("div", { class: "mhead" }, el("h3", { text: "Pixel shader order" }), el("span", { class: "basis", text: orderSummary(c) })),
        el("figure", { style: "border:0;padding:0;margin-top:10px;background:none" }, el("div", { class: "pair" },
          el("img", { src: ROOT + c.orderImage, alt: `Pixel order on the ${c.name}`, width: "512", height: "512" }),
          c.orderZoom ? el("img", { src: ROOT + c.orderZoom, alt: `Close-up of the pixel blocks on the ${c.name}`, width: "512", height: "512" }) : null))) : null,
      ...c.sections.map((s) => sectionPanel(s, color)));
    history.replaceState(null, "", "#" + encodeURIComponent(c.name));
  };
  const want = decodeURIComponent(location.hash.slice(1));
  const idx = cards.findIndex((c) => c.name === want);
  sel.value = String(idx >= 0 ? idx : Math.max(0, cards.findIndex((c) => /1660$/.test(c.name))));
  sel.addEventListener("change", show);
  show();

  // Gallery.
  document.getElementById("gallery").replaceChildren(...cards.filter((c) => c.orderImage).map((c) => el("figure", {},
    el("div", { class: "pair" },
      el("img", { src: ROOT + c.orderImage, alt: `Pixel order on the ${c.name}`, loading: "lazy", width: "512", height: "512" }),
      c.orderZoom ? el("img", { src: ROOT + c.orderZoom, alt: `Close-up on the ${c.name}`, loading: "lazy", width: "512", height: "512" }) : null),
    el("figcaption", {}, c.name, el("small", { text: orderSummary(c) })))));
});

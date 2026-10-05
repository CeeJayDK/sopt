// GPU Blueprint web page: renders data.json (tools/site/build.py) without libraries.
"use strict";

const GROUPS = [
  ["Modifiers", ["neg", "abs", "saturate"]],
  ["Basic math", ["add", "sub", "mul", "mad"]],
  ["Min, max and compares", ["min", "max", "clamp", "compare", "select", "step", "lerp"]],
  ["Rounding and sign", ["floor", "ceil", "round", "frac", "sign"]],
  ["Special functions", ["rcp", "div", "sqrt", "rsqrt", "exp2", "log2", "exp", "log", "sin", "cos", "pow"]],
  ["Helpers (vectors: float3)", ["smoothstep", "dot3", "length3", "normalize3", "distance3"]],
];
const LABEL = { compare: "a < b", select: "c ? a : b", dot3: "dot", length3: "length", normalize3: "normalize", distance3: "distance" };
const VENDOR_COLOR = { NVIDIA: "var(--nvidia)", AMD: "var(--amd)", Intel: "var(--intel)" };

const el = (tag, attrs = {}, ...kids) => {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "style") e.style.cssText = v; else if (k === "text") e.textContent = v; else e.setAttribute(k, v);
  }
  for (const k of kids) if (k != null) e.append(k);
  return e;
};
// Cost models count modifiers as 1 quarter (so search levels stay well-founded); on the GPU they are free.
const fma = (q) => q / 4;
const isFree = (q) => q <= 1;
const fmt = (q) => (isFree(q) ? "free" : fma(q).toFixed(fma(q) < 10 ? 2 : 1).replace(/\.?0+$/, ""));

function barRow(name, q, scale, color, title, wide = false) {
  const free = isFree(q);
  const w = free ? 0 : Math.min(100, (fma(q) / scale) * 100);
  return el("div", { class: wide ? "row wide" : "row", title },
    el("span", { class: "name", text: name }),
    el("div", { class: "track" }, el("div", { class: "bar" + (free ? " free" : ""), style: `width:${w}%;--c:${color}` })),
    el("span", { class: "val" + (free ? " free" : ""), text: fmt(q) }));
}

function renderModel(data, m, scale) {
  const color = VENDOR_COLOR[m.vendor] || "var(--accent)";
  const cards = data.cards.filter((c) => c.model === m.name);
  const reports = cards.reduce((n, c) => n + c.reports, 0);
  const tag = (on, text) => (on ? el("span", { class: "tag", text }) : null);
  const panel = document.getElementById("model");
  panel.replaceChildren(
    el("div", { class: "mhead" },
      el("h3", { text: `${m.vendor} ${m.title}` }),
      el("span", { class: "basis", text: m.basis === "OpBench" ? `Measured with OpBench · model "${m.name}"` : `${m.basis} · model "${m.name}"` })),
    el("div", { class: "tags" },
      tag(m.contraction, "a * b + c becomes one fma"),
      tag(m.outputModifier, "×2, ×4, ×0.5 free (output modifier)"),
      tag(m.max3, "max(max(a, b), c) one instruction"),
      tag(m.minmax, "min(max(a, b), c) one instruction")),
    el("p", { class: "basedon", text: cards.length
      ? `Based on ${reports} report${reports === 1 ? "" : "s"} from ${cards.map((c) => c.name).join(", ")}.`
      : "No OpBench reports for this architecture yet." }),
    ...GROUPS.map(([title, ops]) => el("div", { class: "group" },
      el("h4", { text: title }),
      ...ops.filter((op) => op in m.costs).map((op) =>
        barRow(LABEL[op] || op, m.costs[op], scale, color, `${op}: ${m.costs[op]} quarter units`)))));
}

function renderOp(data, op, scale) {
  const rows = data.models.map((m) => ({ m, q: m.costs[op] })).sort((a, b) => a.q - b.q);
  document.getElementById("opbars").replaceChildren(...rows.map(({ m, q }) =>
    barRow(`${m.vendor} ${m.title}`, q, scale, VENDOR_COLOR[m.vendor] || "var(--accent)", `${m.name}: ${q} quarter units`, true)));
}

function renderCards(data) {
  const byName = Object.fromEntries(data.models.map((m) => [m.name, m]));
  const table = document.getElementById("cardtable");
  table.replaceChildren(
    el("thead", {}, el("tr", {}, el("th", { text: "Card" }), el("th", { text: "Architecture" }),
      el("th", { text: "Reports" }), el("th", { text: "Drivers" }))),
    el("tbody", {}, ...data.cards.map((c) => {
      const m = byName[c.model];
      return el("tr", {},
        el("td", { text: c.name }),
        el("td", {}, m ? el("span", { class: "dot", style: `--c:${VENDOR_COLOR[m.vendor]}` }) : null,
          m ? `${m.vendor} ${m.title}` : "not modelled yet"),
        el("td", { class: "num", text: String(c.reports) }),
        el("td", { text: c.drivers.join(", ") }));
    })));
}

function renderGallery(data) {
  const g = document.getElementById("gallery");
  if (!data.orderImages.length) { g.replaceChildren(el("p", { class: "note", text: "No pictures yet." })); return; }
  g.replaceChildren(...data.orderImages.map((i) => el("figure", {},
    el("div", { class: "pair" },
      el("img", { src: i.order, alt: `Pixel shading order on the ${i.card}`, loading: "lazy", width: "512", height: "512" }),
      i.zoom ? el("img", { src: i.zoom, alt: `Close-up of the pixel blocks on the ${i.card}`, loading: "lazy", width: "512", height: "512" }) : null),
    el("figcaption", { text: i.card }))));
}

fetch("data.json").then((r) => r.json()).then((data) => {
  // One scale for every chart, so bars compare across architectures.
  const scale = Math.max(...data.models.flatMap((m) => Object.values(m.costs).map(fma)));
  const picker = document.getElementById("picker");
  const vendors = [...new Set(data.models.map((m) => m.vendor))];
  let chips = [];
  const select = (m) => {
    chips.forEach((c) => c.setAttribute("aria-selected", String(c.dataset.name === m.name)));
    renderModel(data, m, scale);
    history.replaceState(null, "", "#" + m.name);
  };
  for (const v of vendors) {
    const group = el("div", { class: "vendor" }, el("span", { text: v }));
    for (const m of data.models.filter((x) => x.vendor === v)) {
      const chip = el("button", { class: "chip", role: "tab", "data-name": m.name, style: `--c:${VENDOR_COLOR[v]}`, text: m.title });
      chip.addEventListener("click", () => select(m));
      chips.push(chip);
      group.append(chip);
    }
    picker.append(group);
  }
  const start = data.models.find((m) => "#" + m.name === location.hash) || data.models[0];
  select(start);

  const opSel = document.getElementById("op");
  for (const [title, ops] of GROUPS) {
    const og = el("optgroup", { label: title });
    for (const op of ops) og.append(el("option", { value: op, text: LABEL[op] ? `${op} (${LABEL[op]})` : op }));
    opSel.append(og);
  }
  opSel.value = "sign";
  opSel.addEventListener("change", () => renderOp(data, opSel.value, scale));
  renderOp(data, opSel.value, scale);

  renderCards(data);
  renderGallery(data);
  document.getElementById("version").textContent = `cost models from SweetOpt ${data.version}`;
});

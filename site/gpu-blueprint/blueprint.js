// GPU Blueprint: cost models per architecture (data/models.json from tools/site/build.py).
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

loadJSON("models").then((data) => {
  const byName = Object.fromEntries(data.models.map((m) => [m.name, m]));
  const archs = data.coverage.flatMap((v) => v.architectures.map((a) => ({ ...a, vendor: v.vendor })));
  const scale = Math.max(...data.models.flatMap((m) => Object.values(m.costs).map(fma)));
  const archsOf = (model) => archs.filter((a) => a.model === model);

  function renderModel(a) {
    const panel = document.getElementById("model");
    const color = VENDOR_COLOR[a.vendor] || "var(--cyan)";
    if (!a.model) {
      panel.replaceChildren(el("div", { class: "panel missingbox" },
        el("h3", { text: `${a.vendor} ${a.name}` }),
        el("p", { text: `No cost model yet. Cards: ${a.examples}.` }),
        el("p", {}, "Have one? Run ", el("a", { href: "https://github.com/CeeJayDK/sopt/releases", text: "GPU Blueprint" }),
          " on it and send the reports from its menu: one report is enough to start a model.")));
      return;
    }
    const m = byName[a.model];
    const cards = data.cards.filter((c) => c.model === m.name);
    const reports = cards.reduce((n, c) => n + c.reports, 0);
    const shared = archsOf(m.name).filter((x) => x.name !== a.name).map((x) => x.name);
    const pill = (on, text) => (on ? el("span", { class: "pill", text }) : null);
    panel.replaceChildren(el("div", { class: "panel" },
      el("div", { class: "mhead" },
        el("h3", { text: `${a.vendor} ${a.name}` }),
        el("span", { class: "basis", text: (m.basis === "OpBench" ? "measured with OpBench" : m.basis) + ` · model ${m.name}` })),
      el("div", { class: "tags" },
        pill(m.contraction, "a * b + c becomes one fma"),
        pill(m.outputModifier, "×2 ×4 ×0.5 free (output modifier)"),
        pill(m.max3, "max(max(a, b), c) one instruction"),
        pill(m.minmax, "min(max(a, b), c) one instruction")),
      el("p", { class: "basedon", text:
        (cards.length ? `Based on ${reports} report${reports === 1 ? "" : "s"} from ${cards.map((c) => c.name).join(", ")}.`
          : "No OpBench reports for this architecture yet.") + (shared.length ? ` Shares its cost model with ${shared.join(", ")}.` : "") }),
      ...GROUPS.map(([title, ops]) => el("div", { class: "group" },
        el("h4", { text: title }),
        ...ops.filter((op) => op in m.costs).map((op) => {
          const q = m.costs[op];
          return barRow(LABEL[op] || op, fma(q), scale, color, { free: isFree(q), title: `${op}: ${q} quarter units` });
        })))));
  }

  // Picker: every DirectX 11 architecture; the ones without a model greyed.
  const picker = document.getElementById("picker");
  const chips = [];
  const select = (a) => {
    chips.forEach((c) => c.setAttribute("aria-selected", String(c.dataset.arch === a.name)));
    renderModel(a);
    history.replaceState(null, "", "#" + encodeURIComponent(a.name));
  };
  for (const v of data.coverage) {
    const group = el("div", { class: "vendor" }, el("span", { text: v.vendor }));
    for (const a0 of v.architectures) {
      const a = { ...a0, vendor: v.vendor };
      const m = a.model && byName[a.model];
      const cls = "chip" + (a.model ? "" : " missing") + (m && m.basis !== "OpBench" ? " estimated" : "");
      const chip = el("button", { class: cls, role: "tab", "data-arch": a.name, style: `--c:${VENDOR_COLOR[v.vendor]}`, text: a.name,
        title: a.model ? a.examples : `${a.examples}: not measured yet` });
      chip.addEventListener("click", () => select(a));
      chips.push(chip);
      group.append(chip);
    }
    picker.append(group);
  }
  const wanted = decodeURIComponent(location.hash.slice(1));
  select(archs.find((a) => a.name === wanted) || archs.find((a) => a.model === data.models[0].name));

  // One operation across the models.
  const opSel = document.getElementById("op");
  for (const [title, ops] of GROUPS) {
    const og = el("optgroup", { label: title });
    for (const op of ops) og.append(el("option", { value: op, text: LABEL[op] ? `${op} (${LABEL[op]})` : op }));
    opSel.append(og);
  }
  const renderOp = () => {
    const op = opSel.value;
    const rows = data.models.map((m) => ({ m, q: m.costs[op] })).sort((a, b) => a.q - b.q);
    document.getElementById("opbars").replaceChildren(...rows.map(({ m, q }) =>
      barRow(`${m.vendor} ${archsOf(m.name).map((a) => a.name).join(" / ") || m.title}`, fma(q), scale, VENDOR_COLOR[m.vendor],
        { free: isFree(q), wide: true, title: `${m.name}: ${q} quarter units` })));
  };
  opSel.value = "sign";
  opSel.addEventListener("change", renderOp);
  renderOp();

  // Coverage grid.
  const have = archs.filter((a) => a.model).length;
  document.getElementById("covnote").textContent =
    `${have} of ${archs.length} DirectX 11 architectures have a cost model. The rest need a report from one card each.`;
  document.getElementById("coverage").replaceChildren(
    el("div", { class: "cov" }, ...archs.map((a) => el("div", { class: a.model ? null : "missing", style: `--c:${VENDOR_COLOR[a.vendor]}` },
      el("b", { text: `${a.vendor} ${a.name}` }),
      el("small", { text: a.examples + (a.model ? "" : " · wanted") })))),
    el("div", { class: "legend" }, el("span", { text: "coloured edge: cost model" }), el("span", { text: "grey: not measured yet" }),
      el("span", { text: "RDNA 3: from AMD's compiler, not yet measured" })));

  // Cards.
  const table = document.getElementById("cardtable");
  table.replaceChildren(
    el("thead", {}, el("tr", {}, el("th", { text: "Card" }), el("th", { text: "Architecture" }), el("th", { class: "num", text: "Reports" }), el("th", { text: "Drivers" }))),
    el("tbody", {}, ...data.cards.map((c) => {
      const m = byName[c.model];
      const archName = m ? archsOf(m.name).map((a) => a.name).join(" / ") : "";
      return el("tr", {},
        el("td", { text: c.name }),
        el("td", {}, m ? el("span", { class: "dot", style: `--c:${VENDOR_COLOR[m.vendor]}` }) : null, m ? `${m.vendor} ${archName}` : "not modelled yet"),
        el("td", { class: "num", text: String(c.reports) }),
        el("td", { text: c.drivers.join(", ") }));
    })));
});

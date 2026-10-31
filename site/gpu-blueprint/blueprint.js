// GPU Blueprint: cost models per architecture (data/models.json and data/library.json from tools/site/build.py).
"use strict";

const GROUPS = [
  ["Modifiers and basic math", ["neg", "abs", "saturate", "add", "sub", "mul", "mad"]],
  ["Min, max and compares", ["min", "max", "clamp", "compare", "select", "step", "lerp"]],
  ["Rounding", ["floor", "ceil", "round", "trunc", "frac"]],
  ["Sign", ["sign"]],
  ["Special functions", ["rcp", "div", "sqrt", "rsqrt", "exp2", "log2", "exp", "log", "sin", "cos"]],
  ["Powers", ["pow"]],  // its own group: pow has many alternatives
  ["Helpers (vectors: float3)", ["smoothstep", "dot3", "length3", "normalize3", "distance3"]],
];
const LABEL = { compare: "a < b", select: "c ? a : b", dot3: "dot", length3: "length", normalize3: "normalize", distance3: "distance" };

Promise.all([loadJSON("models"), loadJSON("library").catch(() => ({ rules: [] }))]).then(([data, lib]) => {
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
        el("span", { class: "basis", text: (m.basis === "OpBench" || m.basis === "StageBench" ? `measured with ${m.basis}` : m.basis) + ` · model ${m.name}` })),
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
      const cls = "chip" + (a.model ? "" : " missing") + (m && m.basis !== "OpBench" && m.basis !== "StageBench" ? " estimated" : "");
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

  // One group across the models: architectures as rows, operations as columns (bars line up down a column), each
  // operation followed by the library's alternatives: rules whose pattern is the operation alone (frac(x) -> ...),
  // whose replacement is not just a value (saturate(x) -> x only holds in a range) and that win somewhere.
  // Its arguments may be constants (pow(x, 2.0) -> x * x, smoothstep(0.0, 1.0, x) -> ...): such a rule holds for those
  // values only, so the page shows the whole rule. The same rule twice (other conditions) is shown once.
  const altsOf = (op) => {
    const seen = new Set();
    return lib.rules.filter((r) => {
      const m = /^(\w+)\(\s*[\w.+-]+(\s*,\s*[\w.+-]+)*\s*\)$/.exec(r.lhs);
      const key = r.lhs + " -> " + r.rhs;
      if (!m || m[1] !== op || /^[\w.+-]+$/.test(r.rhs.trim()) || seen.has(key)) return false;
      seen.add(key);
      return data.models.some((md) => r.costs[md.name] && r.costs[md.name][1] < md.costs[op]);
    });
  };
  const grpSel = document.getElementById("grp");
  const viewSel = document.getElementById("view");
  GROUPS.forEach(([title], k) => grpSel.append(el("option", { value: String(k), text: title })));
  const renderGroup = () => {
    const ops = GROUPS[Number(grpSel.value)][1].filter((op) => data.models.some((m) => op in m.costs));
    // Columns: each operation, then its alternatives (→1, →2, ...).
    const cols = [];
    for (const op of ops) {
      cols.push({ op, label: LABEL[op] || op, cost: (m) => m.costs[op] });
      // A rule's name ([name] in the library), else a short replacement as it is, else the operation and a number.
      altsOf(op).forEach((r, k) => cols.push({ op, alt: r, label: r.name || (r.rhs.length <= 10 ? r.rhs : `${LABEL[op] || op}→${k + 1}`),
        cost: (m) => r.costs[m.name] && r.costs[m.name][1] }));
    }
    const all = cols.flatMap((c) => data.models.map((m) => c.cost(m))).filter((q) => q != null && !isFree(q));
    const lo = Math.min(...all), hi = Math.max(...all), max = fma(hi);
    const heatmap = viewSel.value === "heat";
    // Numbers line up on the decimal point: padded to 2 decimals, the padding hidden (it keeps its width, draws nothing).
    const aligned = (v) => {
      const [i, f = ""] = num(v).split(".");
      return el("span", {}, i + (f ? "." + f : ""), f.length < 2 ? el("span", { class: "pad", text: (f ? "" : ".") + "0".repeat(2 - f.length) }) : null);
    };
    const cell = (q, color, mark) => {  // mark: "better" / "worse" (an alternative faster / slower than its operation)
      if (q == null) return el("td", { class: "c free", text: "–" });
      if (isFree(q)) return el("td", { class: "c free" }, aligned(0));  // a free modifier: 0, easier to read than "free"
      const cls = "c" + (mark ? " " + mark : "");
      if (heatmap) {
        const h = heatColor(q, lo, hi);
        return el("td", { class: cls + " hot", style: `background:${h.bg}` }, aligned(fma(q)));
      }
      return el("td", { class: cls }, aligned(fma(q)),
        el("div", { class: "mini" }, el("div", { class: "bar", style: `width:${Math.min(100, (fma(q) / max) * 100)}%;--c:${color}` })));
    };
    const tip = (c) => (c.alt ? `${c.alt.lhs} → ${c.alt.rhs}${c.alt.where ? `  (where ${c.alt.where})` : ""}` : c.label);
    const head = el("tr", {}, el("th", { class: "arch", text: "Architecture" }),
      ...cols.map((c, k) => el("th", { class: "o" + (c.alt ? " alt" : "") + (!c.alt && k ? " gs" : ""), title: tip(c), text: c.label })));
    // Bold: the fastest way to write each operation on an architecture (the operation or one of its alternatives).
    const groupOf = [];
    cols.forEach((c, k) => groupOf.push(c.alt ? groupOf[k - 1] : k));
    const body = data.models.map((m) => el("tr", {},
      el("th", { class: "arch", style: `--c:${VENDOR_COLOR[m.vendor]}`, title: `${m.vendor} ${m.title}`, text: m.title }),
      ...cols.map((c, k) => {
        const q = c.cost(m);
        const best = q != null && cols.every((o, j) => groupOf[j] !== groupOf[k] || o.cost(m) == null || o.cost(m) >= q);
        const mark = !c.alt || q == null ? null : q < m.costs[c.op] ? "better" : q > m.costs[c.op] ? "worse" : null;
        const color = !c.alt ? VENDOR_COLOR[m.vendor] : mark === "better" ? "var(--cyan)" : mark === "worse" ? "var(--amber)" : "var(--text-faint)";
        const td = cell(q, color, mark);
        td.title = tip(c);
        if (best) td.classList.add("best");
        if (c.alt) td.classList.add("alt");
        else if (k) td.classList.add("gs");  // a new operation starts
        return td;
      })));
    document.getElementById("cmp").replaceChildren(el("thead", {}, head), el("tbody", {}, ...body));
    // The heatmap's scale.
    document.getElementById("scale").replaceChildren(...(heatmap && hi > lo ? [...heatScale(fma(lo), fma(hi), "fma units").children] : []));
    // The alternatives in full.
    const alts = cols.filter((c) => c.alt);
    document.getElementById("alts").replaceChildren(...(alts.length ? [
      el("h4", { text: "Faster ways to write them (rewrite library)" }),
      el("table", {}, el("tbody", {}, ...alts.map((c) => el("tr", {},
        el("td", { class: "name", text: `${LABEL[c.op] || c.op} ${c.label}` }),
        el("td", {}, el("code", {}, hl(c.alt.lhs))),
        el("td", { class: "to", text: "→" }),
        el("td", {}, el("code", {}, hl(c.alt.rhs))),
        el("td", { class: "cond", text: c.alt.where ? `where ${c.alt.where}` : "" })))))] : []));
  };
  grpSel.value = "2";  // rounding: the library's add-round forms
  grpSel.addEventListener("change", renderGroup);
  viewSel.addEventListener("change", renderGroup);
  renderGroup();

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
  const RUN_TOOLS = ["OpBench", "StageBench", "TexBench"];
  const runTime = (s) => (s < 120 ? `${s} s` : `${Math.floor(s / 60)} min ${s % 60} s`);
  const table = document.getElementById("cardtable");
  table.replaceChildren(
    el("thead", {}, el("tr", {}, el("th", { text: "Card" }), el("th", { text: "Architecture" }), el("th", { class: "num", text: "Reports" }), el("th", { text: "Drivers" }),
      ...RUN_TOOLS.map((t) => el("th", { class: "num", text: t })))),
    el("tbody", {}, ...data.cards.map((c) => {
      const m = byName[c.model];
      const archName = m ? archsOf(m.name).map((a) => a.name).join(" / ") : "";
      return el("tr", {},
        el("td", { text: c.name }),
        el("td", {}, m ? el("span", { class: "dot", style: `--c:${VENDOR_COLOR[m.vendor]}` }) : null, m ? `${m.vendor} ${archName}` : "not modelled yet"),
        el("td", { class: "num", text: String(c.reports) }),
        el("td", { text: c.drivers.join(", ") }),
        ...RUN_TOOLS.map((t) => el("td", { class: "num", text: c.times[t] ? runTime(c.times[t]) : "-" })));
    })));

  // Architecture cell as in the card table.
  const archCell = (model) => {
    const m = byName[model];
    return el("td", {}, m ? el("span", { class: "dot", style: `--c:${VENDOR_COLOR[m.vendor]}` }) : null,
      m ? `${m.vendor} ${archsOf(m.name).map((a) => a.name).join(" / ")}` : model);
  };
  const ops = (v) => v.toFixed(2);

  // Driver improvements (owner, 2026-10-12): one family's reports split by driver version. OpBench test names in plain
  // words; tests with the same name (the multiplies by 2, 4, 0.5 ...) are averaged into one entry. Costs below 0 are
  // free (0), except the fp16 tests, which are measured against the same work in fp32.
  const TEST_NAME = {
    mul: "x * constant merged into the next fma", mul16: "x * y (fp16)", radians: "radians merged into the next fma", mulxx: "x * y", step: "step", divxy: "x / y", fmod: "fmod", modf: "modf",
    imin: "integer min", iand: "bitwise and", ftoitof: "float → int → float", normalize: "normalize", sqrt: "sqrt",
    sqrt16: "sqrt (fp16)", mad16v3: "float3 fma in fp16, against fp32",
  };
  // omod*: the old scale tests after rcp (until OpBench 0.7.2), where the multiply could hide behind the rcp.
  const testName = (t) => (/^omod/.test(t) ? "x * 2, 4, 8, 0.5 … hidden behind rcp" : /^scale/.test(t) ? "x * 2, 4, 8, 0.5 …" : TEST_NAME[t] || t);
  const changes = (tests) => {
    const by = new Map();
    for (const t of tests) {
      const n = testName(t.test), relative = t.test === "mad16v3";
      const e = by.get(n) || { n, older: 0, newer: 0, k: 0 };
      e.older += relative ? t.older : Math.max(0, t.older);
      e.newer += relative ? t.newer : Math.max(0, t.newer);
      e.k++;
      by.set(n, e);
    }
    return [...by.values()].map((e) => ({ n: e.n, older: e.older / e.k, newer: e.newer / e.k }));
  };
  document.getElementById("drivertable").replaceChildren(
    el("thead", {}, el("tr", {}, el("th", { text: "Architecture" }), el("th", { text: "Older driver" }), el("th", { text: "Newer driver" }),
      el("th", { text: "What changed (cost in multiply-adds, older → newer)" }))),
    el("tbody", {}, ...data.drivers.map((r) => el("tr", {},
      archCell(r.model),
      el("td", { text: r.older.drivers.join(", "), title: r.older.cards.join(", ") }),
      el("td", { text: r.newer.drivers.join(", "), title: r.newer.cards.join(", ") }),
      el("td", {}, ...changes(r.tests).map((t, i) => el("span", {},
        `${i ? "; " : ""}${t.n} ${ops(t.older)} → ${ops(t.newer)}`, t.newer > t.older ? el("b", { text: " (slower)" }) : null)))))));

  // fp16 (owner, 2026-10-12: tells developers whether supporting it pays): min16float against the same work in fp32.
  const VERDICT = (f) => {
    if (!f.native) return "runs as fp32";
    const v = ["fma", "vec3", "add"].map((k) => f.speedup[k]).filter((x) => x).sort((a, b) => a - b);
    if (!v.length) return "";
    if (v[Math.floor(v.length / 2)] >= 1.5) return "yes";
    if (v[v.length - 1] >= 1.5) return "sometimes";
    return v[v.length - 1] >= 1.15 ? "a little" : "no gain";
  };
  const times = (x) => (x ? `${x.toFixed(1)}×` : "–");
  document.getElementById("fp16table").replaceChildren(
    el("thead", {}, el("tr", {}, el("th", { text: "Architecture" }), el("th", { class: "num", text: "Cards" }),
      el("th", { class: "num", text: "fma" }), el("th", { class: "num", text: "float3 fma" }), el("th", { class: "num", text: "add" }),
      el("th", { class: "num", text: "rcp" }), el("th", { text: "Worth it?" }))),
    el("tbody", {}, ...data.fp16.map((f) => el("tr", {},
      archCell(f.model),
      el("td", { class: "num", text: String(f.cards.length), title: f.cards.join(", ") }),
      ...["fma", "vec3", "add", "rcp"].map((k) => el("td", { class: "num", text: f.native ? times(f.speedup[k]) : "1.0×" })),
      el("td", { text: VERDICT(f) })))));

  // Every OpBench test (owner, 2026-10-16): tests as rows in OpBench's own order, architectures as columns.
  const at = data.alltests;
  const secSel = document.getElementById("allsec");
  at.sections.forEach((s, k) => secSel.append(el("option", { value: String(k), text: s.title })));
  const renderAll = () => {
    const sec = at.sections[Number(secSel.value)];
    const shown = sec.rows.flatMap((r) => Object.values(r.v)).filter((v) => v >= 0.25);  // below a quarter fma: noise, no colour
    const lo = Math.min(...shown), hi = Math.max(...shown);
    const head = el("tr", {}, el("th", { class: "arch", text: "Test" }), ...at.families.map((f) => {
      const m = byName[f.model];
      return el("th", { class: "o", style: m ? `color:${VENDOR_COLOR[m.vendor]}` : null, title: f.cards.join(", "), text: m ? m.title : f.model });
    }));
    const body = sec.rows.map((r) => el("tr", {}, el("th", { class: "arch", title: r.step, text: r.test }),
      ...at.families.map((f) => {
        const v = r.v[f.model];
        if (v == null) return el("td", { class: "c free", text: "–" });
        if (v < 0.25) return el("td", { class: "c free", text: num(Math.max(0, v)) });
        const h = heatColor(v, lo, hi);
        return el("td", { class: "c hot", style: `background:${h.bg}`, text: num(v) });
      })));
    document.getElementById("alltable").replaceChildren(el("thead", {}, head), el("tbody", {}, ...body));
    document.getElementById("allscale").replaceChildren(...(hi > lo ? [...heatScale(lo, hi, "fma units").children] : []));
  };
  secSel.addEventListener("change", renderAll);
  renderAll();
});

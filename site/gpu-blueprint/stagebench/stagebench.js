// StageBench findings (data/stagebench.json from tools/site/build.py).
"use strict";

const STAGES = ["ps", "vs"];
const ALL = ["cs", "ps", "vs"];
const STAGE_NAME = { cs: "compute", ps: "pixel", vs: "vertex" };
// A card's pixel or vertex numbers can be off as a whole (the median stage / compute ratio: HD 6450 vertex shaders 0.55):
// such cards stay out of the summary table, where every test would differ.
function shift(card, s) {
  if (!card.shift) {
    card.shift = {};
    for (const st of STAGES) {
      const r = card.tests.filter((t) => t.cs > 0.5 && t[st] != null).map((t) => t[st] / t.cs).sort((a, b) => a - b);
      card.shift[st] = r.length ? r[r.length >> 1] : 1;
    }
  }
  return card.shift[s];
}
// A stage differs from compute by more than 20% and 0.75 fma (the rule used for the cost models, history.md).
function differs(card, t, s) {
  const a = t.cs, b = t[s];
  return a != null && b != null && Math.abs(b - a) > 0.75 && Math.abs(b - a) > 0.2 * Math.max(a, b);
}
const shiftText = (card) => STAGES.filter((s) => Math.abs(shift(card, s) - 1) > 0.1).map((s) => `${s} ×${num(shift(card, s))}`).join(", ");
const steady = (card) => Object.values(card.drift).every((d) => !(parseFloat(d) > 30)) && STAGES.every((s) => Math.abs(shift(card, s) - 1) <= 0.15);
const show = (v) => (v == null ? "-" : num(v));
function shortCard(name) { return name.replace(/^(NVIDIA GeForce|AMD Radeon|Intel) /, "").replace(/ (Graphics|GPU)$/, ""); }
// The test's name with the code it times on hover (as on the OpBench page).
const testCell = (t) => el("th", { class: "arch", title: t.step || null, text: t.test });

// Three small bars per card (owner, 2026-10-20): compute, pixel, vertex, on one scale per test, so a stage that costs
// more or less stands out; the differing ones are drawn bright.
function trio(card, t, scale) {
  return el("td", { class: "trio", title: `${card.name}: compute ${show(t.cs)}, pixel ${show(t.ps)}, vertex ${show(t.vs)} fma` },
    ...ALL.map((s) => {
      const v = t[s];
      const hit = s !== "cs" && differs(card, t, s);
      return el("span", { class: `b ${s}${hit ? " hit" : ""}` },
        el("i", { style: `height:${v == null ? 0 : Math.max(1, Math.min(1, Math.max(0, v) / scale) * 28)}px` }),
        el("small", { text: show(v) }));
    }));
}

function differTable(cards) {
  const used = cards.filter(steady);
  const tests = [];
  for (const c of used) for (const t of c.tests) if (STAGES.some((s) => differs(c, t, s)) && !tests.some((x) => x.test === t.test)) tests.push(t);
  document.getElementById("differ").replaceChildren(
    el("thead", {}, el("tr", {}, el("th", { class: "arch", text: "Test" }), ...used.map((c) => el("th", { class: "o", title: c.name, text: shortCard(c.name) })))),
    el("tbody", {}, ...tests.map((test) => {
      const rows = used.map((c) => c.tests.find((x) => x.test === test.test));
      const scale = Math.max(1e-9, ...rows.flatMap((t) => (t ? ALL.map((s) => t[s] || 0) : [])));
      return el("tr", {}, testCell(test), ...used.map((c, k) => (rows[k] && rows[k].cs != null ? trio(c, rows[k], scale) : el("td", { class: "c free", text: "-" }))));
    })));
}

// Every test on every card in one stage, coloured like the OpBench page's heatmap.
function heatTable(cards, stage) {
  const tests = [];
  for (const c of cards) for (const t of c.tests) if (!tests.some((x) => x.test === t.test)) tests.push(t);
  const shown = cards.flatMap((c) => c.tests.map((t) => t[stage])).filter((v) => v != null && v >= 0.25);  // below a quarter fma: noise
  const lo = Math.min(...shown), hi = Math.max(...shown);
  document.getElementById("heat").replaceChildren(
    el("thead", {}, el("tr", {}, el("th", { class: "arch", text: "Test" }), ...cards.map((c) => el("th", { class: "o", title: c.name, text: shortCard(c.name) })))),
    el("tbody", {}, ...tests.map((test) => el("tr", {}, testCell(test), ...cards.map((c) => {
      const t = c.tests.find((x) => x.test === test.test);
      const v = t && t[stage];
      if (v == null) return el("td", { class: "c free", text: "–" });
      if (v < 0.25) return el("td", { class: "c free", text: num(Math.max(0, v)) });
      return el("td", { class: "c hot", style: `background:${heatColor(v, lo, hi).bg}`, text: num(v),
        title: `${c.name}, ${STAGE_NAME[stage]} shader: ${num(v)} fma` });
    })))));
  document.getElementById("heatscale").replaceChildren(...(hi > lo ? [...heatScale(lo, hi, "fma units").children] : []));
}

function detail(card) {
  const drift = Object.entries(card.drift).filter(([, d]) => d).map(([s, d]) => `${s} ${d}`).join(", ");
  document.getElementById("detail").replaceChildren(
    el("p", { class: "note", text: `StageBench ${card.version}, driver ${card.driver}. Clock drift during the runs: ${drift}.` +
      (Object.values(card.drift).every((d) => !(parseFloat(d) > 30)) ? "" : " Above 30%: the numbers may be off as a whole.") +
      (shiftText(card) ? ` Against compute as a whole (median): ${shiftText(card)}.` : "") }),
    el("div", { class: "tablewrap" }, el("table", {},
      el("thead", {}, el("tr", {}, ...["Test", "compute", "pixel", "vertex"].map((h, i) => el("th", { class: i ? "num" : null, text: h })))),
      el("tbody", {}, ...card.tests.map((t) => el("tr", {}, el("td", { title: t.step || null, text: t.test }), el("td", { class: "num", text: show(t.cs) }),
        ...STAGES.map((s) => el("td", { class: "num" + (differs(card, t, s) ? " hit" : ""), text: show(t[s]) }))))))));
}

loadJSON("stagebench").then((data) => {
  differTable(data.cards);
  const stageSel = document.getElementById("stage");
  const heat = () => heatTable(data.cards, stageSel.value);
  stageSel.addEventListener("change", heat);
  heat();
  const select = document.getElementById("card");
  for (const c of data.cards) select.append(el("option", { value: c.name, text: c.name }));
  const pick = () => detail(data.cards.find((c) => c.name === select.value));
  select.addEventListener("change", pick);
  if (data.cards.length) pick();
});

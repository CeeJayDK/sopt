// StageBench findings (data/stagebench.json from tools/site/build.py).
"use strict";

const STAGES = ["ps", "vs"];
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

function differTable(cards) {
  const used = cards.filter(steady);
  const tests = [];
  for (const c of used) for (const t of c.tests) if (STAGES.some((s) => differs(c, t, s)) && !tests.includes(t.test)) tests.push(t.test);
  const table = document.getElementById("differ");
  table.replaceChildren(
    el("thead", {}, el("tr", {}, el("th", { text: "Test" }), ...used.map((c) => el("th", { class: "num", text: shortCard(c.name) })))),
    el("tbody", {}, ...tests.map((name) => el("tr", {}, el("td", { text: name }), ...used.map((c) => {
      const t = c.tests.find((x) => x.test === name);
      if (!t || t.cs == null) return el("td", { class: "num empty", text: "-" });
      const hit = STAGES.some((s) => differs(c, t, s));
      return el("td", { class: "num" + (hit ? " hit" : ""), text: `${show(t.cs)} → ${show(t.ps)} / ${show(t.vs)}`,
        title: `${c.name}: compute ${show(t.cs)}, pixel ${show(t.ps)}, vertex ${show(t.vs)} fma` });
    })))));
}

function detail(card) {
  const drift = Object.entries(card.drift).filter(([, d]) => d).map(([s, d]) => `${s} ${d}`).join(", ");
  document.getElementById("detail").replaceChildren(
    el("p", { class: "note", text: `StageBench ${card.version}, driver ${card.driver}. Clock drift during the runs: ${drift}.` +
      (Object.values(card.drift).every((d) => !(parseFloat(d) > 30)) ? "" : " Above 30%: the numbers may be off as a whole.") +
      (shiftText(card) ? ` Against compute as a whole (median): ${shiftText(card)}.` : "") }),
    el("div", { class: "tablewrap" }, el("table", {},
      el("thead", {}, el("tr", {}, ...["Test", "compute", "pixel", "vertex"].map((h, i) => el("th", { class: i ? "num" : null, text: h })))),
      el("tbody", {}, ...card.tests.map((t) => el("tr", {}, el("td", { text: t.test }), el("td", { class: "num", text: show(t.cs) }),
        ...STAGES.map((s) => el("td", { class: "num" + (differs(card, t, s) ? " hit" : ""), text: show(t[s]) }))))))));
}

loadJSON("stagebench").then((data) => {
  differTable(data.cards);
  const select = document.getElementById("card");
  for (const c of data.cards) select.append(el("option", { value: c.name, text: c.name }));
  const pick = () => detail(data.cards.find((c) => c.name === select.value));
  select.addEventListener("change", pick);
  if (data.cards.length) pick();
});

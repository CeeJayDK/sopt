// The rewrite library (data/library.json + data/models.json from tools/site/build.py).
"use strict";

Promise.all([loadJSON("library"), loadJSON("models")]).then(([lib, models]) => {
  const archSel = document.getElementById("arch");
  const q = document.getElementById("q");
  const fasterOnly = document.getElementById("faster");
  const archName = (m) => {
    const names = models.coverage.flatMap((v) => v.architectures).filter((a) => a.model === m.name).map((a) => a.name);
    return `${m.vendor} ${names.join(" / ") || m.title}`;
  };
  archSel.append(el("option", { value: "", text: "best saving on any architecture" }));
  for (const m of models.models) archSel.append(el("option", { value: m.name, text: archName(m) }));

  const saving = (r, model) => {
    if (model) { const [a, b] = r.costs[model]; return { old: a, now: b, gain: a - b, model }; }
    let best = null;
    for (const [k, [a, b]] of Object.entries(r.costs)) if (!best || a - b > best.gain) best = { old: a, now: b, gain: a - b, model: k };
    return best;
  };
  const byName = Object.fromEntries(models.models.map((m) => [m.name, m]));

  function render() {
    const model = archSel.value;
    const text = q.value.trim().toLowerCase();
    let shown = 0;
    const box = document.getElementById("rules");
    box.replaceChildren();
    let section = null, panel = null;
    for (const r of lib.rules) {
      const s = saving(r, model);
      if (fasterOnly.checked && s.gain <= 0) continue;
      if (text && !(r.text + " " + r.comment + " " + r.section).toLowerCase().includes(text)) continue;
      if (r.section !== section) {
        section = r.section;
        panel = el("div", { class: "panel" }, el("h3", { text: section || "Other" }));
        box.append(panel);
      }
      const m = byName[s.model];
      panel.append(el("div", { class: "rule" },
        el("div", { class: "code" }, r.lhs, el("span", { class: "arrow", text: "→" }), r.rhs),
        el("div", { class: "save" }, s.gain > 0 ? `-${num(fma(s.gain))}` : s.gain < 0 ? `+${num(fma(-s.gain))}` : "same",
          el("small", { text: `${num(fma(s.old))} → ${num(fma(s.now))}${model ? "" : " · " + (m ? archName(m) : s.model)}` })),
        (r.where || r.comment) ? el("div", { class: "meta" },
          r.where ? el("span", {}, "where ", el("code", { text: r.where })) : null,
          r.where && r.comment ? " · " : null,
          r.comment || null) : null));
      shown++;
    }
    document.getElementById("count").textContent = `${shown} of ${lib.rules.length} rewrites shown.`;
  }
  archSel.addEventListener("change", render);
  q.addEventListener("input", render);
  fasterOnly.addEventListener("change", render);
  render();
});

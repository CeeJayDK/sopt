# ReShade Shader Superoptimizer: design og udviklingsplan

## 1. Mål og afgrænsning

**Mål:** Finde hurtigere, verificerede alternativer til rene aritmetiske udtryk i ReShade FX-shaders og præsentere dem som valgbare varianter i .fx-filen.

**Input:** `.fx`-filer (ReShade FX).
**Output:** En rapport og en `.fx`-fil, hvor varianterne ligger bag preprocessor-switches.

**Targets:** Alle ReShade-backends (DX9/SM3, DX10/11/12, OpenGL, Vulkan) på Windows og Linux. Optimeringen sker i FX, så resultatet gælder alle backends. Verifikation og målinger køres dog pr. backend.

**Uden for v1:**
- Control flow ud over `?:` samt loops.
- Integers og bitwise ops.
- Derivatives.
- Texture-fetches som søgbare operationer. De indgår kun som inputs (leaves).

## 2. Grundprincipper

1. **Forslag, ikke automatisk omskrivning.** Brugeren tester og vælger.
2. **Liste frem for én vinder.** Alle verificerede kandidater, der er billigere end originalen, rapporteres.
3. **Fejl måles i den enhed, der betyder noget.** Farveoutput måles i 8-bit kodeværdier. Texcoords, sammenligninger og temporal feedback behandles strengt.
4. **CPU-evaluering i float32 er referencen.** Den er bit-eksakt for `+ − * / sqrt`, da alle PC-GPU'er bruger IEEE 754. Transcendentale funktioner sammenlignes med tolerance.
5. **Tre spørgsmål holdes adskilt:** korrekthed, statisk cost og målt performance.
6. **Simpel søgning først.** Kompleksitet tilføjes kun, når benchmarks af optimizeren viser et behov.

## 3. Arkitektur

```
.fx ─► front end (reshadefx) ─► IR: SSA-DAG pr. pixel shader
    ─► region extraction + facts + fejlbudget
    ─► enumerator (cost-ordnet, OE, CEGIS)
    ─► verifikation (sampling → udtømmende gitter → interval)
    ─► backend-normalisering (fxc / SPIR-V / GLSL)
    ─► måling (GPU timestamps, A/B)
    ─► rapport + variant-.fx
```

**Moduler:**

| Modul | Indhold |
|---|---|
| `sopt-ir` | IR, typer, float32-evaluator |
| `sopt-search` | enumerator |
| `sopt-verify` | verifikation og fejlmetrikker |
| `sopt-fx` | front end, region extraction, variant-generering |
| `sopt-bench` | GPU-benchmark-harness |
| `sopt-cli` | kommandolinjeværktøj |

## 4. Komponenter

### 4.1 IR

- **Typer:** `float`, `float2–4`, `bool`, `bool2–4`. M1 bruger kun `float` og `bool`.
- **Noder:** `Input` (med facts), `Const` og `Op`. Noderne er hash-consed, så samme op med samme operander altid er én node.
- **Op-sæt i v1:**
  - Aritmetik: `+ − * /`, `neg`, `abs`, `min`, `max`, `saturate`, `clamp`, `lerp`, `mad`, `step`, `sign`, `floor`, `frac`, `sqrt`, `rsqrt`.
  - Sammenligninger (`< <= > >= == !=`) og select (`?:`).
  - Vektorer: `dot`, `length`, `normalize`, samt udtræk og konstruktion af komponenter.
  - `pow`, `exp`, `log`, `sin` og `cos` evalueres, men enumereres kun, hvis de optræder i originalen.
- Hver node har en source location (fra M3).

### 4.2 Semantik og fejlbudget

**Evaluering:** float32 på CPU. Toolet bygges uden FP-contraction og fast-math (GCC/Clang: `-ffp-contract=off`; MSVC: ikke `/fp:fast` eller `/fp:contract`). Ellers stemmer CPU-resultaterne ikke med GPU'en.

**Udefinerede input** (`pow` med negativ base, `normalize(0)`, `0/0`, NaN i `min`/`max`) er don't-care som default. Kandidater, der afhænger af målt vendor-adfærd, markeres separat (M5).

**Facts pr. input:**
- interval
- finite
- heltalsværdi
- diskret gitter (fx `k/255` for et 8-bit backbuffer)

**Fejlbudget pr. output, afledt af hvordan værdien bruges:**

| Brug | Default |
|---|---|
| Farve til 8-bit target | maks. afvigelse i kodeværdier efter kvantisering |
| Farve til 10-bit/HDR target | tilsvarende i 10-bit koder eller relativ fejl |
| Texcoord | maks. afvigelse i pixels ved 4K |
| Betingelse i sammenligning/select | bit-eksakt |
| Temporal feedback | bit-eksakt |
| Depth-linearisering | bit-eksakt |

**Klasser i rapporten:**
- bit-eksakt
- 8-bit-identisk
- inden for budget (med maks. fejl og andel ændrede pixels)
- afhænger af vendor-facts

Klassen "bit-eksakt" forudsætter, at compileren ikke contracter til FMA. Om det sker, afgøres af `precise` eller af vendor-facts.

### 4.3 Søgning

**Bottom-up enumeration efter stigende cost (heltal).** `Bank[c]` indeholder alle distinkte værdier med cost `c`.

**Fingerprint:** Kandidatens output på N testpunkter (N ≈ 32), gemt som et float32-array. En ny kandidat er én op anvendt elementvis på operandernes arrays. Den løkke kan auto-vektoriseres og er den vigtigste performance-detalje.

**Observational equivalence (OE):** I v1 dedup'es bit-eksakt på fingerprints. Det er sound og simpelt. Kvantiseret dedup kommer først i M7.

**Kanoniske regler:**
- Operander til kommutative ops sorteres.
- Rene konstant-udtryk enumereres ikke; de foldes.
- Typer skal passe.

**Leaves:**
- Inputs.
- Konstanter fra originalen og `0, 0.5, 1, 2, −1`.
- Afledte konstanter: `1/c`, `c²`, `−c`.

**Testpunkter** genereres ud fra facts:
- intervalgrænser, 0, 1 og midtpunkt
- uniformt tilfældige værdier
- log-uniformt tilfældige værdier for store intervaller
- par med næsten ens værdier (near-cancellation)
- gitterpunkter

**Målcheck:** Hver ny kandidat af den rigtige type sammenlignes med originalens fingerprint og klassificeres som bit-eksakt eller inden for ε. Hits med lavere cost end originalen gemmes.

**CEGIS:** Hits testes på et større sæt (ca. 4096 punkter). Fejlende punkter føjes til fingerprint-sættet ved næste genstart.

**Stop:** når originalens cost er nået, eller når tids- eller hukommelsesbudgettet er brugt.

**Kendt begrænsning i v1:** Bank-cost er tree-cost. Løsninger, der kræver en delt mellemværdi, kan derfor blive overset eller få for høj cost. Løses i M7 med *shared leaves*: vælg en billig bank-værdi `s`, brug den som ekstra leaf, og søg med budget − cost(s).

### 4.4 Verifikation (billigst først)

| Niveau | Metode | Milestone |
|---|---|---|
| V1 | Tæt sampling: ca. 1M punkter fra domænet, multitrådet | M1 |
| V2 | Udtømmende, hvor facts giver et lille endeligt domæne: 8-bit-gitre (256³ ≈ 16,7M for RGB) og unære float-inputs (alle float32 i intervallet) | M2 |
| V3 | Interval-subdivision for kontinuerte domæner med 2+ inputs, som giver en formel øvre fejlgrænse | M7 |

**Fejlmetrikker:**
- maks. absolut og relativ fejl
- maks. afvigelse i kodeværdier
- andel punkter med ændret kodeværdi

Transcendentale funktioner er ikke ens på CPU (libm) og GPU. De vurderes derfor kun med tolerance og klassificeres aldrig som bit-eksakte.

**Nøjagtighedsregel (ejerens beslutning):** Float32-originalen har selv afrundingsfejl, som kan være større end budgettet. En kandidat godkendes derfor også i et punkt, hvis den er mindst lige så tæt på den eksakte (matematiske) værdi som originalen, eller inden for budgettet af den. Eksakte værdier beregnes i double og bruges kun til målingen; float32-evalueringen er stadig referencen. Sådanne kandidater klassificeres "as accurate". Kandidater, der er mindre nøjagtige (inden for 100× budgettet eller originalens fejl, `--loose`), vises med deres fejl som "less accurate", så brugeren selv vurderer dem.

### 4.5 Cost (tre adskilte lag)

1. **Statisk cost:** heltalsvægte pr. op og pr. backend-profil (`sm3`, `dxbc`, `spirv`, `glsl`), gemt i JSON. Startværdierne er gæt, som kalibreres i M6.
2. **Backend-normalisering (M4):** Original og kandidat kompileres via reshadefx til hver backend og optimeres (fxc, spirv-opt). Er outputtet ens bagefter, markeres kandidaten "compileren gør det allerede". fxc kører kun på Windows.
3. **Måling (M4):**
   - Setup: fullscreen-pass på et fast 4K-inputbillede med GPU timestamp queries.
   - Kørsel: A/B interleaved, warmup, median over mange runs.
   - API'er: Vulkan (Windows og Linux) og DX11 (Windows).
   - En variant tæller kun som hurtigere, hvis ingen backend eller vendor bliver målbart langsommere.
   - Enkelte rewrites kan ligge under målestøjen, så alle accepterede varianter måles også samlet.

### 4.6 Output

**Rapport** (Markdown og JSON), pr. region:
- originalt udsnit med `fil:linje`
- kandidater sorteret efter cost, hver med klasse, fejlmetrikker, estimeret cost pr. backend og målt effekt, når den findes

**Variant-.fx:** Hver region får en switch, som kan skiftes i ReShade's UI via preprocessor definitions:

```hlsl
#ifndef SOPT_Curves_142
#define SOPT_Curves_142 0 // 0 = original, 1..n = varianter
#endif

#if SOPT_Curves_142 == 1
    color = x >= 0.5 ? 1.0 : color;              // bit-eksakt, cost 3 → 2
#else
    color = lerp(color, 1.0, step(0.5, x));      // original
#endif
```

### 4.7 Vendor-facts (M5)

- En probe-effect i FX rendrer et gitter af udefinerede input og edge cases til en texture.
- Input kommer fra en texture, ikke fra literals, så compileren ikke constant-folder udtrykkene.
- Proben tester også NaN-guards (`x != x`, `isnan(x)`, `x > 0 ? x : 0`) for at afsløre, om compiler eller driver folder dem væk.
- Resultatet gemmes i JSON pr. (vendor, API, driver) og kan genkøres efter driveropdateringer.

## 5. Udviklingsplan

### M0: Fundament
**Leverer:**
- Repo, CMake og CI på Windows og Linux.
- IR-typer.
- float32-evaluator for op-sættet.
- Unit tests af semantikken.

**Færdig når:** Evaluatoren giver identiske resultater med MSVC og GCC/Clang, bitvis for `+ − * / sqrt`.

### M1: Skalar enumerator på håndskrevne udtryk
**Leverer:**
- Et lille tekstformat til input.
- Bottom-up enumeration med OE og konstantpulje.
- Sammenligninger og select.
- Målcheck, simpel CEGIS og V1-verifikation.
- Tekstrapport.
- Første version af optimizer-benchmarken (afsnit 6).

Eksempel på inputformat:
```
input a : float in [0,1] grid 255
input b : float in [0,1] grid 255
output r = 1.0 - (1.0 - a) * (1.0 - b)
budget r : color8
```

**Færdig når:** Enumeratoren selv finder et testsæt af kendte rewrites:

| Original | Rewrite | Betingelse |
|---|---|---|
| `lerp(a, b, step(e, x))` | `x >= e ? b : a` | |
| `1 - (1 - a) * (1 - b)` (screen blend) | `lerp(a, 1.0, b)` | |
| `pow(x, 2.0)` | `x * x` | |
| `pow(x, -0.5)` | `rsqrt(x)` | |
| `saturate(x)` | `x` | x ∈ [0,1] |
| `x*a + x*b` | `x * (a + b)` | |

### M2: Vektorer og fejlklasser
**Leverer:**
- `float2–4`, `dot`, `length`, `normalize`, udtræk og konstruktion af komponenter.
- Fejlbudget-klasser.
- V2-verifikation.
- 8-bit fejlmetrikker.

**Færdig når:** Enumeratoren finder vektor-eksempler som:
- `c.r*0.2126 + c.g*0.7152 + c.b*0.0722` → `dot(c, float3(0.2126, 0.7152, 0.0722))`
- `sqrt(dot(v, v))` → `length(v)`
- `v / length(v)` → `normalize(v)`

### M3: FX front end og første rigtige kørsel
**Leverer:**
- reshadefx-codegen, der bygger IR.
- Region extraction: vinduer på ≤ k ops mellem texture-fetches og control flow. Vinduer er et statement med de single-use temporaries, det læser, og med kæder af statements på samme variabel (`d = 1.0 - d; d /= ...`). Vinduer hen over `#if`-linjer gælder kun under samme betingelse (`#if SOPT_x >= 1 && (COND)`).
- Facts:
  - BackBuffer-range afhængigt af farverum
  - `ui_min`/`ui_max`
  - texcoords
- Fejlbudget afledt af brug.
- Variant-.fx.

**Færdig når:**
- Toolet kører på hele reshade-shaders-repoet uden fejl.
- Variant-.fx-filer kompilerer og kører i ReShade på DX11 og Vulkan.
- Den første manuelle test af varianter i ReShade er gennemført.

### M4: Backend-normalisering og måling
**Leverer:**
- Automatisk kompilering af varianter til backends.
- Sammenligning af output efter compiler-optimering.
- GPU-benchmark-harness (Vulkan og DX11).

**Færdig når:** Rapporten viser "compileren gør det allerede" eller målt effekt pr. backend på NVIDIA, AMD og Intel for de første effekter.

### M5: Vendor-facts
**Leverer:** Probe-effect, facts-database og markering af fact-afhængige kandidater i rapporten.

En variant er interessant, hvis den er hurtigere for mindst én vendor; den kan være langsommere for en anden. Sådanne varianter kan vælges pr. vendor/device med `__VENDOR__` (0x1002 AMD, 0x10DE NVIDIA, 0x8086 Intel) og `__DEVICE__`. Første skridt er lavet: med målte costs får variant-filer `SOPT_AUTO` (standard 0); sat til 1 vælger hver switch den variant, der er målt hurtigst på GPU'ens vendor (AMD, NVIDIA; andre beholder originalen).

**Færdig når:**
- Matricen er udfyldt for 3 vendors × (DX11, Vulkan, OpenGL).
- Mindst én fact-afhængig rewrite klassificeres korrekt.

### M6: Kalibreret cost-model
**Leverer:** Micro-benchmarks pr. op, backend og vendor, som giver vægte i JSON.

**Færdig når:** Den statiske cost forudsiger rækkefølgen af de målte varianter bedre end et uvægtet op-count.

Idéer til data (ejeren):
- Intel Shader Analyzer (udgået, repo på GitHub) kan måske give Intel-tal.
- Et måleværktøj til rigtig hardware, som andre kan køre, evt. et ReShade-addon: installeret (opt-in) måler det shaders og rapporterer tallene tilbage, så cost pr. vendor og device kan udledes af målinger.

### M7: Skalering af søgningen
Kun de teknikker, som benchmarks viser behov for. Kandidater:
- shared leaves (DAG-sharing)
- symbolske konstanter
- kvantiseret OE
- billig pruning (dependency-bitmask, gradgrænser)
- søgning med et begrænset op-sæt først
- V3-verifikation
- større vinduer
- snitpunkter (dominatorer i dataflowet): værdier, som al senere beregning afhænger af, deler en stor funktion i stykker, der søges hver for sig. Afprøves og evalueres (ejerens idé).

### M8: Regel-mining og vertex-shader hoisting
- **Regel-mining:** Kør enumeratoren over hele korpusset og generalisér fundene til regler med preconditions. Fjern regler, som backends allerede anvender. Reglerne kan derefter anvendes hurtigt online uden søgning.
- **Hoisting:** Flyt udtryk, der kun afhænger af uniforms, til vertex shaderen. Det er en separat og simpel pass, som kan rykkes frem.

## 6. Benchmark af optimizeren selv

Startes i M1 og udvides løbende.

- **Planted problems:** Generér små tilfældige programmer, obfuskér dem med kendte rewrites, og mål om originalen findes igen.
- **Korpus:** vinduer udtrukket fra rigtige shaders (fra M3).
- **Tællere:**
  - kandidater pr. sekund
  - bankstørrelse pr. cost-niveau
  - hukommelsesforbrug
  - tid i enumeration og i verifikation
  - time-to-first og time-to-best
- **Regel for nye søgeteknikker:** Hver teknik ligger bag et flag og testes mod den forrige version på samme suite. Den beholdes kun, hvis den forbedrer time-to-best på korpusset.

## 7. Åbne beslutninger

1. **Sprog og build:** C++20 og CMake (passer til reshadefx), eller Rust.
2. **Front end:** eget reshadefx-codegen (bevarer `lerp`, `mad` og source locations), eller parsing af SPIR-V-output.
3. **Default fejlbudget for farveoutput:** 8-bit-identisk, eller ≤1 kodeværdi.
4. **Vinduesstørrelse k i første version:** forslag 6 ops.
5. **Placering:** selvstændigt repo, eller en del af ReshadeFX Tools.

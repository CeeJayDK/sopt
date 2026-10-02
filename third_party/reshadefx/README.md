# reshadefx (vendored)

The ReShade FX front end (lexer, preprocessor, parser, symbol table and the codegen
interface) from [ReShade](https://github.com/crosire/reshade) v6.8.0, `source/effect_*`,
unmodified except for these changes in the preprocessor and parser (marked "sopt"): `symbolic_macros`,
macros whose uses in code are replaced by the identifier `__sopt_<name>` (their value is
still used in `#if`; `symbolic_exclude` lists source lines where they expand to their value
anyway, and `sopt_macros()` returns the defined macros), the parser's
`sopt_named_expressions` (a global `static const` whose initializer is not a literal, only possible
with symbolic macros, is parsed again at each use instead of being an error), and `\` in `#include` names read as `/` on non-Windows platforms
(iMMERSE writes `".\MartysMods\x.fxh"`, which only works on Windows as is), and
includes not found as written are looked up ignoring letter case, as on Windows (OtisFX
includes `"Reshade.fxh"`). BSD-3-Clause, see LICENSE.md. sopt implements its own codegen
(`src/fx/codegen.cpp`) to read pixel shaders into its IR.

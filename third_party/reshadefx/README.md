# reshadefx (vendored)

The ReShade FX front end (lexer, preprocessor, parser, symbol table and the codegen
interface) from [ReShade](https://github.com/crosire/reshade) v6.8.0, `source/effect_*`,
unmodified except for two changes in the preprocessor (marked "sopt"): `symbolic_macros`,
macros whose uses in code are replaced by the identifier `__sopt_<name>` (their value is
still used in `#if`), and `\` in `#include` names read as `/` on non-Windows platforms
(iMMERSE writes `".\MartysMods\x.fxh"`, which only works on Windows as is). BSD-3-Clause, see LICENSE.md. sopt implements its own codegen
(`src/fx/codegen.cpp`) to read pixel shaders into its IR.

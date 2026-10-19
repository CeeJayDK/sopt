Two bugs in ReShade's FX compiler (6.8.0), found while running shaders from other projects (OBS Studio,
FFmpeg) through SweetOpt, which uses ReShade's preprocessor and parser. Each has a small repro effect next to
this file and a proposed fix (both in SweetOpt's copy of the compiler since 0.8.3, tests pass).

**1. A macro argument merges with the operator before it**

```hlsl
#define NEG(x) -x
float4 PS(float4 p : SV_Position) : SV_Target { return NEG(-1) * p.x; }
```

fails with `error X3025: l-value specifies const object`: the expansion is lexed again as text, so `-` followed
by the argument `-1` becomes `--1` (a decrement of a literal). A C preprocessor works on tokens and keeps them
apart (`- -1`). OBS Studio's YADIF deinterlacer hits this (`-level` with `level` = `-1 * 2`), in 27 of its shaders.
Repro: `sopt_MacroMinus.fx`.

Fix, in `preprocessor::expand_macro`, case `macro_replacement_argument`: remember where the argument starts, and
after appending it insert a space when the character before and the first character of the argument are both
operator characters. A space never changes the meaning (C keeps the tokens apart anyway):

```cpp
case macro_replacement_argument:
{
	const size_t argument_begin = input.size();
	push(arguments[index] + static_cast<char>(macro_replacement_argument));
	while (true) { /* unchanged */ }
	assert(_current_token_raw_data[0] == macro_replacement_argument);
	if (static const std::string ops = "+-*/%<>=&|^!~"; argument_begin > 0 && input.size() > argument_begin &&
		ops.find(input[argument_begin - 1]) != std::string::npos && ops.find(input[argument_begin]) != std::string::npos)
		input.insert(argument_begin, 1, ' ');
	break;
}
```

(The same can happen at the argument's end, `x-` with x = `a-`; not seen in practice, not covered.)

**2. Two pragmas in a row**

```hlsl
#pragma reshade showfps
#pragma reshade showclock
```

fails with `error X3000: syntax error: unexpected '_Pragma'` on the second line. The preprocessor turns each
pragma into `_Pragma("...")`; `parser::parse_top` handles one with `if (accept(tokenid::pragma))` and then expects
a declaration. Any two pragmas the preprocessor passes on fail this way (FFmpeg's shaders have
`#pragma shader_stage(compute)` and `#pragma use_vulkan_memory_model`). Repro: `sopt_PragmaTwice.fx`.

Fix: `while (accept(tokenid::pragma))` instead of `if` in `parse_top`.

**Tested:** SweetOpt's tests (MSVC, GCC, Clang) and the repros with the compiler copy; not yet in ReShade itself
(Test Host: load the two repro effects in an unpatched and a patched build).

# ReShade feature request ideas

Ideas collected while building SweetOpt and the SweetFX proposals, to bring up with crosire. Checked against
ReShade 6.8.0's source. Bugs found on the way are in [IEEE754.md](IEEE754.md) (NaN / infinity) and
[UPSTREAM.md](UPSTREAM.md) (internal shaders).

## 1. Set a saved uniform with the mouse

**Want:** place things on screen with the mouse (Layer.fx: drag the layer, rotate it, scroll to scale) and have
the result saved in the preset like any slider.

**Today:** a uniform with a `source` annotation (`mousepoint`, `mousebutton`, `mousedelta`, `mousewheel` ...) is
hidden from the UI and never saved (runtime_gui.cpp: `variable.special != special_uniform::none` skips it),
annotations such as `noedit` are fixed at compile time, and an effect cannot write a uniform. So an effect can
follow the mouse only in a texture, which is lost when the game restarts, and the user has to type the numbers
into the sliders (Layer.fx 1.0 shows them on screen for that).

**Possible forms:**
- An annotation that makes a normal (saved) slider follow the mouse while editing, e.g.
  `ui_mouse = "position"` on a float2: while the overlay is open and the slider is in "pick" mode, the value
  follows the mouse; a click stores it (owner's idea: the value follows the mouse until a button is pressed).
- Or a way for an effect to write a uniform back: a pass whose output (a 1 x 1 texture) the runtime copies into
  named uniforms after the frame and saves with the preset.

## 2. The effect's file name as an identifier

**Want:** copies of an effect (Layer2.fx, Layer3.fx ...) that need no editing.

**Today:** `__FILE_STEM__` / `__FILE_NAME__` are strings and `__FILE_STEM_HASH__` / `__FILE_NAME_HASH__` numbers.
Strings cannot become identifiers, so texture names have to use the hash (`Layer_Tex_123456789`), which is
unreadable in the texture list and statistics. Preprocessor definitions are already per effect file (set in the
effect's own list), which covers the settings.

**Possible form:** `__FILE_STEM_ID__`: the file stem with characters that are not allowed in identifiers replaced
by `_` (Layer2.fx -> `Layer2`), usable with `##` (`texture Layer_Tex_##__FILE_STEM_ID__` via a helper macro).

## 3. Show preprocessor names that are only tested in `#if`

**Today:** the effect's "Preprocessor definitions" list shows a name only when it is tested with `#ifdef` /
`#ifndef` (`_used_macros` in effect_preprocessor.cpp). A name used in `#if` (undefined = 0), or one made with
`##`, is not listed, so users must know to type it themselves.

**Possible form:** add identifiers evaluated in `#if` expressions to the used list too (with their default 0), or
an annotation-like pragma to declare user settings: `#pragma reshade definition LAYER_SIZE_X 1280`.

## 4. Copy the back buffer only when a pass reads it

**Today:** before a pass, the runtime copies the back buffer into the texture effects read through the `COLOR`
semantic (`ReShade::BackBuffer`): at the start of every technique and after every pass that drew to the back buffer
(`runtime::render_technique`, `needs_implicit_back_buffer_copy`). It does not look at whether the coming passes sample
that texture, so an effect that never reads the back buffer (an overlay drawn with blending, a debug view, compute
passes followed by a blended draw) still pays a full-screen copy per technique.

**Possible form:** make the copy when a pass that samples a `COLOR` texture is about to run, not before. Each pass
already has its sampler list (`pass_info.samplers`, used for its texture table), so a flag per pass ("samples the back
buffer") can be set when the effect is loaded:

```cpp
if (needs_implicit_back_buffer_copy && pass_data.samples_back_buffer)
{
	// barrier, copy_texture_region, barrier as today
	needs_implicit_back_buffer_copy = false;
}
```

with `needs_implicit_back_buffer_copy = true` after a pass that drew to the back buffer (as today) and left alone
after the others. What effects see does not change: a pass that samples the texture still gets the current image.

**How many effects:** few as they are written today. In a text scan of the 529 effects in the package list about 5
never read the back buffer (a solid colour, a checkerboard, a letterbox, a test pattern). It matters more for effects
whose final blend with the back buffer is moved to the blend stage, which then no longer sample it.

**Not measured:** what the copy costs. Add-ons that read the effect colour texture between passes
(`reshade_render_technique` events) would see an older image; they may need the copy kept.

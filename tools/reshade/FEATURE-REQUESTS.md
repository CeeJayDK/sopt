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

# SweetOpt: the super sweet shader optimizer

**SweetOpt makes your ReShade effects run faster, without changing how they look.**

Shaders (the little programs behind ReShade effects like SweetFX) do a lot of math for every
pixel on your screen. Often the same result can be computed with fewer steps. SweetOpt finds
those shortcuts automatically, double-checks that the picture stays the same, and writes new
copies of your effects that use them. Your original files are never changed.

## What you get

- **Faster effects.** Less work for your graphics card means more frames per second, or room
  for more effects.
- **The same picture.** Every shortcut is tested on millions of values before it is used. If a
  change could make even a tiny visible difference, SweetOpt tells you, or leaves it out.
- **Safe to try.** SweetOpt writes its results to a separate folder. If you don't like them,
  delete the folder and nothing has changed.

## How to use it (Windows)

1. Download `SweetOpt-<version>-windows-x64.zip` from the
   [Releases](https://github.com/CeeJayDK/sopt/releases) page and unzip it anywhere.
2. Double-click **SweetOpt.bat**. A simple menu opens.
3. Press **1** and pick your ReShade `Shaders` folder (the one inside `reshade-shaders`).
4. Press **5** to start. SweetOpt shows how far it has come and how long is left.
5. When it is done, press **7** to open the results. The `sopt-out` folder holds the faster
   copies of your effects.
6. In ReShade's settings, put the `sopt-out` folder **first** in the effect search paths, so
   ReShade uses the faster copies. (Or copy your game's `reshade-shaders` folder and try the
   results there.)

The menu starts in **easy mode**: you get ready-to-use files with only the changes we recommend.
Press **M** for **expert mode** if you want to choose between all the alternatives yourself.
Press **9** in the menu to open the full guide.

**Tip:** if the results say some shortcuts "need value ranges", press **8**. SweetOpt then asks
what values a setting can have (for example 0 to 1), which lets it use more shortcuts safely.

## The other tools in this project

- **GPU Blueprint** (`GPU-Blueprint-<version>.zip`, double-click `GPU-Blueprint.bat`):
  measures what your graphics card and driver really do. If you send us the results, SweetOpt
  learns to make better choices for cards like yours. Thank you!
- **Test Host** (`Test-Host-<version>.zip`, double-click `Test-Host.bat`): runs ReShade on
  every graphics API (Direct3D 9 to 12, Vulkan, OpenGL) without a game, to test effects and
  take screenshots. See [tools/windows/README.md](tools/windows/README.md).

All of them are on the [Releases](https://github.com/CeeJayDK/sopt/releases) page and are built
automatically from the code in this repository.

## For developers

SweetOpt also works on plain HLSL and GLSL shaders, and has a command line for scripts.

- How it works, all options and how to build it: [docs/technical.md](docs/technical.md)
- Which file does what (a map of the code): [docs/code-map.md](docs/code-map.md)
- Design and roadmap: [docs/design.md](docs/design.md) (Danish)

## Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by
[SignPath Foundation](https://signpath.org/) (once the application is approved; until then the
release files are unsigned and listed with SHA256 checksums).

- Committers and reviewers: [CeeJayDK](https://github.com/CeeJayDK)
- Approvers: [CeeJayDK](https://github.com/CeeJayDK)

Only programs built from this repository's source by its release workflow are signed
(OpBench.exe, sopt-host.exe, sopt-fxc.exe, sopt-timer.addon64 / .addon32). ReShade64.dll in the
tools zip is built unchanged from crosire's ReShade 6.8.0 source and is not signed by this project.

## Privacy

None of the programs use the internet. They only write files next to themselves (GPU Blueprint
into its `Reports` folder, Test Host into its run folder, SweetOpt into its output folder).
GPU Blueprint's "Send the reports" option, and only if you choose it, opens a Dropbox upload
page in your browser, where you can upload the zip yourself. It contains your graphics card's
name, driver version and the measurements, nothing else.

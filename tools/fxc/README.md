# sopt-fxc

Compiles one HLSL entry point with Microsoft's `D3DCompile` at `-O3` (what ReShade uses
for DX9-DX11) and prints the disassembly. `sopt-fx --backends` uses it to compare original
and variants after fxc's optimizer (`$SOPT_FXC` = path to `sopt-fxc.exe`).

**Windows:** built by CMake (`sopt-fxc`), uses the system `d3dcompiler_47.dll`.

**Linux:** Wine's own `d3dcompiler_47` is vkd3d-shader, which does not optimize like fxc,
so use Microsoft's DLL:

```sh
x86_64-w64-mingw32-gcc -O2 -o sopt-fxc.exe tools/fxc/sopt_fxc.c -ld3dcompiler_47
# Microsoft's d3dcompiler_47.dll (redistributable) next to the exe, e.g. from an
# Electron Windows release (electron-vXX-win32-x64.zip on GitHub)
export SOPT_FXC=$PWD/sopt-fxc.exe      # sopt runs it as: wine sopt-fxc.exe <file> <entry> <profile>
```

sopt sets `WINEDLLOVERRIDES=d3dcompiler_47=n` so the DLL next to the exe is used
(`$SOPT_WINE` overrides the `wine` command).

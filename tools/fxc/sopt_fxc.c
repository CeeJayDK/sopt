// sopt-fxc: compiles one HLSL entry point with Microsoft's D3DCompile (-O3, what ReShade
// uses for DX9-DX11) and prints the disassembly, for backend normalization (sopt --fxc).
// Windows: uses the system d3dcompiler_47.dll. Linux: run under Wine with Microsoft's
// d3dcompiler_47.dll next to the exe and WINEDLLOVERRIDES=d3dcompiler_47=n (Wine's own
// d3dcompiler is vkd3d-shader, which does not optimize like fxc).
// usage: sopt-fxc.exe <in.hlsl> <entry> <profile>
#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
	if (argc < 4) { fprintf(stderr, "usage: sopt-fxc <in.hlsl> <entry> <profile>\n"); return 2; }
	FILE *f = fopen(argv[1], "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *src = malloc(n + 1);
	if (fread(src, 1, n, f) != (size_t)n) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
	src[n] = 0;
	fclose(f);
	ID3DBlob *code = NULL, *err = NULL, *dis = NULL;
	HRESULT hr = D3DCompile(src, n, argv[1], NULL, NULL, argv[2], argv[3], D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
	if (err) fprintf(stderr, "%.*s\n", (int)err->lpVtbl->GetBufferSize(err), (char *)err->lpVtbl->GetBufferPointer(err));
	if (FAILED(hr)) { fprintf(stderr, "D3DCompile failed 0x%08lx\n", hr); return 1; }
	hr = D3DDisassemble(code->lpVtbl->GetBufferPointer(code), code->lpVtbl->GetBufferSize(code), 0, NULL, &dis);
	if (FAILED(hr)) { fprintf(stderr, "D3DDisassemble failed 0x%08lx\n", hr); return 1; }
	fwrite(dis->lpVtbl->GetBufferPointer(dis), 1, dis->lpVtbl->GetBufferSize(dis), stdout);
	return 0;
}

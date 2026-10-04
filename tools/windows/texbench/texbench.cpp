// TexBench: what texture reads, screen-space derivatives and render target writes cost on the GPU in
// this machine, next to math (owner, 2026-10-04: "ballpark figures on how expensive a texture operation
// is compared to math operations ... when to use math and when to use lookup tables"; and do not assume
// that R8, RG8, RGB10A2, RG11B10F and the other formats ReShade supports perform as expected: test them).
// A separate program from OpBench because it about doubles the run time.
//
//   TexBench [--adapter N] [--list] [--filter text] [--reps N] [--out file.csv] [--groups N]
//
// Method as in OpBench: every test is one HLSL step repeated in long dependent chains (constants from
// a constant buffer, different per unrolled step), timed with GPU timestamps against the reference step
// x = mad(x, c.x, c.y) measured right before it; costs are in sopt's units (4 = one fma) over the test's
// base. A texture step reads at a coordinate computed from x, so every read waits for the one before it
// in its chain:
//   coherent  each thread reads within one texel of its own pixel (8 x 8 thread tiles, like a
//             post-processing pass over a 1024 x 1024 texture): mostly cache hits
//   random    each read lands anywhere in the texture: cache misses on large textures, memory speed
//             (the next place from the value read and the place before it)
// Its base computes the same coordinate without reading (so the cost is the read alone).
//
// Configurations (compute shaders): tput (8 chains per thread, 1M threads), dep (1 chain, 1M threads:
// the GPU hides waiting by switching threads), lat (1 chain, one thread group: nothing hides it).
// Pixel shader tests (derivatives, sampling with automatic mip selection) draw a full-screen triangle
// into a 1024 x 1024 target (tput and dep). Render target writes: full-screen passes into a 3840 x 2160
// target of each format, in GB/s, with noise, a smooth gradient and a flat color (render target
// compression); blending by the hardware against the same math in a shader (ms per pass).
//
// Since 2026-10-04 (owner: "test everything ReShade and HLSL can do"): the remaining texture functions
// (offsets, gathers, grad, fetch from mips, 1D / 3D textures, size queries, address modes), coherent color
// LUTs (2D slices against 3D), compute (storage writes, groupshared memory, barriers, atomics, local arrays,
// branches) and pass states (render targets, clears, mipmaps, stencil, discard).
//
// Each test is read at least twice and more often (up to 6 times) when its readings disagree; a test's
// texture exists only while it is measured (large textures in 19 formats would not fit together).

#include "../benchkit.hpp"
#include <d3dcompiler.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <random>

namespace {

using namespace benchkit;

constexpr int kUnroll = 16;
constexpr UINT kGroupSize = 64;
constexpr UINT kGroupsFull = 16384;  // 1M threads = 128 x 128 tiles of 8 x 8 = 1024 x 1024 pixels
constexpr UINT kPsSize = 1024;       // pixel shader tests: 1024 x 1024 target
constexpr UINT kRtW = 3840, kRtH = 2160;

// ---------------------------------------------------------------------------
// Formats: every texture format ReShade FX supports (reshadefx::texture_format), plus the sRGB view
// of RGBA8 (SRGBTexture = true).

struct Format {
  const char* name;
  DXGI_FORMAT dxgi;
  int bytes;     // per texel
  char kind;     // 'u' unorm, 'h' float16, 'f' float32, 'i' integer, 'p' RGB10A2, 'r' RG11B10F
  int channels;
};

// Smallest to largest texel (owner), so formats of the same size stand together.
const Format kFormats[] = {
    {"R8", DXGI_FORMAT_R8_UNORM, 1, 'u', 1},
    {"RG8", DXGI_FORMAT_R8G8_UNORM, 2, 'u', 2},
    {"R16", DXGI_FORMAT_R16_UNORM, 2, 'u', 1},
    {"R16F", DXGI_FORMAT_R16_FLOAT, 2, 'h', 1},
    {"RGBA8", DXGI_FORMAT_R8G8B8A8_UNORM, 4, 'u', 4},
    {"RGBA8sRGB", DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4, 'u', 4},
    {"RGB10A2", DXGI_FORMAT_R10G10B10A2_UNORM, 4, 'p', 4},
    {"RG11B10F", DXGI_FORMAT_R11G11B10_FLOAT, 4, 'r', 3},
    {"RG16", DXGI_FORMAT_R16G16_UNORM, 4, 'u', 2},
    {"RG16F", DXGI_FORMAT_R16G16_FLOAT, 4, 'h', 2},
    {"R32F", DXGI_FORMAT_R32_FLOAT, 4, 'f', 1},
    {"R32U", DXGI_FORMAT_R32_UINT, 4, 'i', 1},
    {"R32I", DXGI_FORMAT_R32_SINT, 4, 'i', 1},
    {"RGBA16", DXGI_FORMAT_R16G16B16A16_UNORM, 8, 'u', 4},
    {"RGBA16F", DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 'h', 4},
    {"RG32F", DXGI_FORMAT_R32G32_FLOAT, 8, 'f', 2},
    {"RGBA32F", DXGI_FORMAT_R32G32B32A32_FLOAT, 16, 'f', 4},
    {"RGBA32U", DXGI_FORMAT_R32G32B32A32_UINT, 16, 'i', 4},
    {"RGBA32I", DXGI_FORMAT_R32G32B32A32_SINT, 16, 'i', 4},
};

const Format* formatByName(const char* name) {
  for (const Format& f : kFormats)
    if (std::strcmp(f.name, name) == 0) return &f;
  return nullptr;
}

// float -> half (for values in [0, 1)).
uint16_t toHalf(float v) {
  uint32_t b;
  std::memcpy(&b, &v, 4);
  const int e = int((b >> 23) & 0xFF) - 127 + 15;
  if (e <= 0) return 0;
  return uint16_t((e << 10) | ((b >> 13) & 0x3FF));
}

// Texel data: values in [0, 1) for float formats (no NaN / Inf), random bits otherwise.
std::vector<uint8_t> texelData(const Format& f, size_t texels, std::mt19937& rng) {
  std::vector<uint8_t> d(texels * size_t(f.bytes));
  std::uniform_real_distribution<float> u01(0.0f, 0.999f);
  switch (f.kind) {
    case 'h':
      for (size_t k = 0; k < d.size() / 2; ++k) {
        const uint16_t h = toHalf(u01(rng));
        std::memcpy(&d[k * 2], &h, 2);
      }
      break;
    case 'f':
      for (size_t k = 0; k < d.size() / 4; ++k) {
        const float v = u01(rng);
        std::memcpy(&d[k * 4], &v, 4);
      }
      break;
    case 'r':  // 11 / 11 / 10-bit floats below 1: exponent 0..14, random mantissa
      for (size_t k = 0; k < texels; ++k) {
        const uint32_t r = uint32_t(rng());
        auto f11 = [&](uint32_t bits, int mant) { return ((bits % 15u) << mant) | ((bits >> 4) & ((1u << mant) - 1)); };
        const uint32_t v = f11(r, 6) | (f11(r >> 10, 6) << 11) | (f11(r >> 20, 5) << 22);
        std::memcpy(&d[k * 4], &v, 4);
      }
      break;
    default:
      for (size_t k = 0; k < d.size(); k += 4) {
        const uint32_t r = uint32_t(rng());
        std::memcpy(&d[k], &r, std::min<size_t>(4, d.size() - k));
      }
  }
  return d;
}

// ---------------------------------------------------------------------------
// Tests

// Pass state tests (full-screen passes into 3840 x 2160, ms per pass).
enum { kPassTargets = 1, kPassClear, kPassMips, kPassHeavy, kPassStencil, kPassDiscardTiles, kPassDiscardPixels };

enum class Stage { Compute, Pixel, Write, Blend, Pass };
// Plain / Mipped: 2D size x size; Lut1D: 2D size x 1; Lut3D: 3D size^3; Tex1D: a real 1D texture;
// Vol: 3D size x size x depth; Lut2D: 2D (size * size) x size (a 3D LUT as slices side by side).
// Storage: a 2D size x size texture written by the compute shader (storage / RWTexture2D at u1).
enum class Tex { None, Plain, Mipped, Lut1D, Lut3D, Tex1D, Vol, Lut2D, Storage };
enum class Filter { Point, Linear, Trilinear, Aniso };

struct Test {
  std::string name;
  std::string section;
  Stage stage = Stage::Compute;
  std::string base;          // the test whose cost is subtracted ("" for the reference)
  std::string step;          // HLSL for one step of chain variable x (constants c)
  float cx = 0.5f, cy = 0.5f, cz = 0.0f, cw = 0.0f;
  const Format* format = nullptr;
  Tex tex = Tex::None;
  UINT size = 0;             // texture width (and height for 2D)
  Filter filter = Filter::Point;
  D3D11_TEXTURE_ADDRESS_MODE address = D3D11_TEXTURE_ADDRESS_CLAMP;
  UINT depth = 1;            // Vol: depth
  int uav = 0;               // compute: storage at u1, 1 float4 (W), 2 uint4 (WU), 3 uint for atomics (WA)
  bool groupshared = false;  // compute: groupshared float GS[2048] and uint GSI[64], filled before the loop
  bool localArray = false;   // compute: a local float A[16] filled before the loop (dynamic indexing)
  bool intTex = false;       // integer format: read through Texture2D<uint4>
  float scaleX = 1.0f, scaleY = 1.0f;  // pixel shader: texture coordinate scale (mip level, anisotropy)
  int write = 0;             // render target write: 0 smooth gradient, 1 noise (incompressible), 2 flat color
  UINT tileW = 8;            // compute: thread tile of a 64-thread group, tileW x (64 / tileW) pixels
  int blend = 0;             // blending test: 0 plain write, 1 add, 2 lerp (alpha), 3 multiply, 4 min
  bool shaderBlend = false;  // blending test: the shader reads the destination as a texture and does the math
  bool perByte = false;      // summary: show bytes per op (texel bytes / Ops)
  int pass = 0;              // pass state test: kPass* below
  int count = 1;             // pass state test: render targets
  std::string note;
};

// The coordinate of a read, from x.
const char* const kCoherent = "float2 uv = P + frac(x * c.x + c.y) * texel;";
const char* const kRandom2D = "float2 uv = frac(float2(x, x * 1.618034) * c.x + c.y);";
const char* const kRandom1D = "float2 uv = float2(frac(x * c.x + c.y), 0.5);";
const char* const kRandom3D = "float3 uv = frac(float3(x, x * 1.618034, x * 2.414214) * c.x + c.y);";
const char* const kUse = "x = (t.x + t.y) * c.z + c.w;";
// Random 2D reads: the next coordinate from the value read and the coordinate before it (uv.y). From
// the value alone, the chain would visit only as many places as the format has distinct values (R8:
// 256, all cached) and measure the data instead of the format.
const char* const kUseRandom = "x = (t.x + uv.y) * c.z + c.w;";
// Cache use: each thread reads at random within scale.x x scale.y texels around its own pixel (256
// texels in from the corner, so no read leaves the texture); the next place from the value read and
// the place before it.
const char* const kSpread = "float2 uv = P + ((frac(float2(x, x * 1.618034) * c.x + c.y) - 0.5) * scale + 256.0) * texel;";
// Address modes: coordinates that run a little past the texture's edges.
const char* const kCoherentWide = "float2 uv = P * 1.25 - 0.125 + frac(x * c.x + c.y) * texel;";
// Coherent LUT colors: smooth over the screen like an image (from the pixel position), with a small jitter
// from x so each read depends on the one before it.
const char* const kLutColor = "float3 rgb = saturate(float3(P * size / 1024.0, 0.5) + (frac(x * c.x + c.y) - 0.5) * 0.02);";
const char* const kUseLutColor = "x = (rgb.r + rgb.g) * c.z + c.w;";
// A size query that cannot be hoisted out of the loop: its mip level comes from x.
const char* const kSizeLevel = "uint lv = uint(x * c.x) & 1u;";
const char* const kUseSpread = "x = (t.x + uv.x + uv.y) * c.z + c.w;";
const char* const kUseSpreadBase = "x = (uv.x + uv.y + uv.x) * c.z + c.w;";
const char* const kUseBase = "x = (uv.x + uv.y) * c.z + c.w;";
// Integer textures: the low 10 bits as float, the same conversion in the base.
const char* const kUseInt = "float4 t = float4(tu & 1023u); x = (t.x + t.y) * (c.z * 0.0009765625) + c.w;";
const char* const kIntBase = "uint4 tu = asuint(uv.xyxy);";
const char* const kUseIntRandom = "float t = float(tu.x & 1023u); x = t * (c.z * 0.0009765625) + (uv.y * c.z + c.w);";

std::vector<Test> makeTests() {
  std::vector<Test> v;
  auto add = [&](Test t) { v.push_back(std::move(t)); };
  const float tx = 37.13f, ty = 0.1234f, tz = 0.7f, tw = 0.3f;  // texture steps: scatter, offset, scale, bias
  auto texTest = [&](std::string name, std::string section, std::string base, std::string coord, std::string read,
                     const Format* f, Tex tex, UINT size, Filter filter, bool intTex = false, std::string note = "",
                     std::string use = "") {
    Test t;
    t.name = std::move(name);
    t.section = std::move(section);
    t.base = std::move(base);
    t.step = coord + " " + read + " " + (!use.empty() ? use : intTex ? kUseInt : kUse);
    t.cx = tx, t.cy = ty, t.cz = tz, t.cw = tw;
    t.format = f;
    t.tex = tex;
    t.size = size;
    t.filter = filter;
    t.intTex = intTex;
    t.note = std::move(note);
    add(std::move(t));
  };
  auto baseTest = [&](std::string name, std::string coord, bool intTex, Stage stage = Stage::Compute,
                      std::string use = "") {
    Test t;
    t.name = std::move(name);
    t.section = "";
    t.stage = stage;
    t.base = "mad";
    t.step = coord + " " + (intTex ? std::string(kIntBase) + " " + (use.empty() ? kUseInt : use)
                                   : use.empty() ? std::string(kUseBase) : use);
    t.cx = tx, t.cy = ty, t.cz = tz, t.cw = tw;
    add(std::move(t));
  };

  Test mad;
  mad.name = "mad";
  mad.step = "x = mad(x, c.x, c.y);";
  mad.note = "reference: one fma per step";
  add(mad);
  baseTest("addr.coherent", kCoherent, false);
  baseTest("addr.random", kRandom2D, false);
  baseTest("addr.coherent.int", kCoherent, true);
  baseTest("addr.random.int", kRandom2D, true, Stage::Compute, kUseIntRandom);
  baseTest("addr.lut1d", kRandom1D, false);
  baseTest("addr.lut3d", kRandom3D, false);
  baseTest("addr.spread", kSpread, false, Stage::Compute, kUseSpreadBase);
  baseTest("addr.wide", kCoherentWide, false);
  baseTest("addr.lutcolor", kLutColor, false, Stage::Compute, kUseLutColor);
  baseTest("addr.size", kSizeLevel, false, Stage::Compute,
           "uint w = 1024u >> lv, h = 1024u >> lv, n = 11u - lv; x = mad(x, c.y, c.z + float(w + h + n) * 1e-4);");

  const std::string load = "float4 t = T.Load(int3(uv * size, 0));";
  const std::string loadInt = "uint4 tu = TU.Load(int3(uv * size, 0));";
  const std::string sample = "float4 t = T.SampleLevel(S, uv, 0.0);";
  for (const Format& f : kFormats) {
    const bool i = f.kind == 'i';
    texTest(std::string(f.name) + (i ? " load" : " bilinear"), "Formats: coherent reads (bilinear; integer formats: Load)",
            i ? "addr.coherent.int" : "addr.coherent", kCoherent, i ? loadInt : sample, &f, Tex::Plain, 1024,
            Filter::Linear, i);
    v.back().perByte = true;
  }
  for (const Format& f : kFormats) {
    const bool i = f.kind == 'i';
    texTest(std::string(f.name) + " random", "Formats: random reads from 4096 x 4096 (Load)",
            i ? "addr.random.int" : "addr.random", kRandom2D, i ? loadInt : load, &f, Tex::Plain, 4096, Filter::Point, i,
            "", i ? kUseIntRandom : kUseRandom);
    v.back().perByte = true;
  }
  const Format* rgba8 = formatByName("RGBA8");
  const char* acc = "Access and filtering: RGBA8 1024 x 1024 with mipmaps, coherent";
  texTest("Load", acc, "addr.coherent", kCoherent, load, rgba8, Tex::Mipped, 1024, Filter::Point);
  texTest("point", acc, "addr.coherent", kCoherent, sample, rgba8, Tex::Mipped, 1024, Filter::Point);
  texTest("bilinear", acc, "addr.coherent", kCoherent, sample, rgba8, Tex::Mipped, 1024, Filter::Linear);
  texTest("gather", acc, "addr.coherent", kCoherent, "float4 t = T.GatherRed(S, uv);", rgba8, Tex::Mipped, 1024,
          Filter::Point, false, "GatherRed: 4 texels of one channel");
  texTest("trilinear", acc, "addr.coherent", kCoherent, "float4 t = T.SampleLevel(S, uv, 0.5);", rgba8, Tex::Mipped,
          1024, Filter::Trilinear, false, "between mip 0 and 1");
  texTest("aniso 8:1", acc, "addr.coherent", kCoherent,
          "float4 t = T.SampleGrad(S, uv, float2(texel.x * 8.0, 0.0), float2(0.0, texel.y));", rgba8, Tex::Mipped,
          1024, Filter::Aniso, false, "SampleGrad, footprint 8 x 1 texels, 16x anisotropic filtering");
  // ReShade FX texture functions not covered above (coherent, RGBA8 1024 x 1024 with mipmaps).
  const char* fns = "Texture functions: RGBA8 1024 x 1024 with mipmaps, coherent";
  texTest("bilinear offset", fns, "addr.coherent", kCoherent, "float4 t = T.SampleLevel(S, uv, 0.0, int2(1, -1));", rgba8,
          Tex::Mipped, 1024, Filter::Linear, false, "tex2Dlod / tex2D with an offset");
  texTest("Load offset", fns, "addr.coherent", kCoherent, "float4 t = T.Load(int3(uv * size, 0), int2(1, -1));", rgba8,
          Tex::Mipped, 1024, Filter::Point, false, "tex2Dfetch with an offset");
  texTest("gather offset", fns, "addr.coherent", kCoherent, "float4 t = T.GatherRed(S, uv, int2(1, -1));", rgba8,
          Tex::Mipped, 1024, Filter::Point, false, "tex2DgatherR with an offset");
  for (const char* ch : {"Green", "Blue", "Alpha"})
    texTest(std::string("gather ") + char(std::tolower(ch[0])) + (ch + 1), fns, "addr.coherent", kCoherent,
            std::string("float4 t = T.Gather") + ch + "(S, uv);", rgba8, Tex::Mipped, 1024, Filter::Point, false,
            std::string("tex2Dgather") + ch[0]);
  texTest("Load mip 1", fns, "addr.coherent", kCoherent, "float4 t = T.Load(int3(uv * size * 0.5, 1));", rgba8, Tex::Mipped,
          1024, Filter::Point, false, "tex2Dfetch from mip level 1");
  texTest("grad 1:1", fns, "addr.coherent", kCoherent,
          "float4 t = T.SampleGrad(S, uv, float2(texel.x, 0.0), float2(0.0, texel.y));", rgba8, Tex::Mipped, 1024,
          Filter::Linear, false, "tex2Dgrad, gradients of one texel (mip 0)");
  for (int n : {2, 4, 16})
    texTest("aniso " + std::to_string(n) + ":1", fns, "addr.coherent", kCoherent,
            "float4 t = T.SampleGrad(S, uv, float2(texel.x * " + std::to_string(n) + ".0, 0.0), float2(0.0, texel.y));",
            rgba8, Tex::Mipped, 1024, Filter::Aniso, false,
            "SampleGrad, footprint " + std::to_string(n) + " x 1 texels, 16x anisotropic filtering");
  for (const char* fn : {"R8", "RGB10A2", "RG11B10F", "RGBA16F", "RGBA32F"})
    texTest(std::string("trilinear ") + fn, fns, "addr.coherent", kCoherent, "float4 t = T.SampleLevel(S, uv, 0.5);",
            formatByName(fn), Tex::Mipped, 1024, Filter::Trilinear, false, "between mip 0 and 1");
  texTest("tex1D bilinear", fns, "addr.coherent", kCoherent, "float4 t = T1.SampleLevel(S, uv.x, 0.0);", rgba8, Tex::Tex1D,
          1024, Filter::Linear, false, "a 1D texture of 1024 texels");
  texTest("tex1Dfetch", fns, "addr.coherent", kCoherent, "float4 t = T1.Load(int2(uv.x * size.x, 0));", rgba8, Tex::Tex1D,
          1024, Filter::Point, false, "a 1D texture of 1024 texels");
  texTest("tex3D linear", fns, "addr.coherent", kCoherent, "float4 t = T3.SampleLevel(S, float3(uv, 0.5), 0.0);", rgba8,
          Tex::Vol, 1024, Filter::Linear, false, "1024 x 1024 x 2, halfway between the slices: 8 texels");
  v.back().depth = 2;
  texTest("tex3Dfetch", fns, "addr.coherent", kCoherent, "float4 t = T3.Load(int4(uv * size, 0, 0));", rgba8, Tex::Vol,
          1024, Filter::Point, false, "1024 x 1024 x 2");
  v.back().depth = 2;
  // Size queries: the mip level comes from x (the plain form is constant per draw and would be hoisted).
  texTest("tex2Dsize", fns, "addr.size", kSizeLevel, "uint w, h, n; T.GetDimensions(lv, w, h, n);", rgba8, Tex::Mipped,
          1024, Filter::Point, false, "GetDimensions with a mip level", "x = mad(x, c.y, c.z + float(w + h + n) * 1e-4);");
  texTest("tex1Dsize", fns, "addr.size", kSizeLevel, "uint w, n; T1.GetDimensions(lv, w, n); uint h = w;", rgba8,
          Tex::Tex1D, 1024, Filter::Point, false, "GetDimensions with a mip level",
          "x = mad(x, c.y, c.z + float(w + h + n) * 1e-4);");
  texTest("tex3Dsize", fns, "addr.size", kSizeLevel, "uint w, h, d, n; T3.GetDimensions(lv, w, h, d, n);", rgba8, Tex::Vol,
          1024, Filter::Point, false, "GetDimensions with a mip level", "x = mad(x, c.y, c.z + float(w + h + n) * 1e-4);");
  v.back().depth = 2;
  // Address modes (bilinear, coordinates past the edges).
  const std::pair<const char*, D3D11_TEXTURE_ADDRESS_MODE> modes[] = {
      {"clamp", D3D11_TEXTURE_ADDRESS_CLAMP}, {"wrap", D3D11_TEXTURE_ADDRESS_WRAP},
      {"mirror", D3D11_TEXTURE_ADDRESS_MIRROR}, {"border", D3D11_TEXTURE_ADDRESS_BORDER}};
  for (const auto& m : modes) {
    texTest(std::string("address ") + m.first, fns, "addr.wide", kCoherentWide, sample, rgba8, Tex::Mipped, 1024,
            Filter::Linear, false, "bilinear, coordinates 12% past the edges");
    v.back().address = m.second;
  }

  // Color lookup tables as image effects use them: neighbouring pixels have similar colors. The old way (a
  // 2D texture of N slices side by side, two bilinear reads and a lerp, like ReShade's LUT.fx) against one
  // read from a 3D texture. Cost = the whole lookup (coordinates, reads, blend) over the color itself.
  const char* lutc = "Color lookup tables: coherent colors (like an image), RGBA8";
  const std::string lut2d =
      "float b = rgb.b * (size.x - 1.0); float s0 = floor(b); "
      "float2 uv = float2((rgb.r * (size.x - 1.0) + 0.5 + s0 * size.x) / (size.x * size.x), (rgb.g * (size.x - 1.0) + 0.5) / size.x); "
      "float4 t = lerp(T.SampleLevel(S, uv, 0.0), T.SampleLevel(S, uv + float2(1.0 / size.x, 0.0), 0.0), b - s0);";
  const std::string lut3d = "float4 t = T3.SampleLevel(S, rgb * ((size.x - 1.0) / size.x) + 0.5 / size.x, 0.0);";
  for (UINT n : {32u, 64u}) {
    const std::string ns = std::to_string(n);
    texTest("2D " + ns + " (2 reads)", lutc, "addr.lutcolor", kLutColor, lut2d, rgba8, Tex::Lut2D, n, Filter::Linear, false,
            ns + " slices of " + ns + " x " + ns + " side by side, 2 bilinear reads + lerp", kUse);
    texTest("3D " + ns + "^3", lutc, "addr.lutcolor", kLutColor, lut3d, rgba8, Tex::Lut3D, n, Filter::Linear, false,
            "one read from a " + ns + "^3 3D texture", kUse);
  }

  const char* lut = "Lookup tables and texture size: RGBA8, random reads, bilinear";
  texTest("LUT 256x1", lut, "addr.lut1d", kRandom1D, sample, rgba8, Tex::Lut1D, 256, Filter::Linear, false,
          "a 1D lookup table: stays in the cache");
  texTest("LUT 32^3", lut, "addr.lut3d", kRandom3D, "float4 t = T3.SampleLevel(S, uv, 0.0);", rgba8, Tex::Lut3D, 32,
          Filter::Linear, false, "a 3D color grading lookup table (trilinear between slices)");
  for (UINT size : {512u, 1024u, 2048u, 4096u, 8192u})
    texTest(std::to_string(size) + "^2", lut, "addr.random", kRandom2D, sample, rgba8, Tex::Plain, size, Filter::Linear,
            false, "", kUseRandom);

  // Cache sizes: random reads from growing RGBA8 textures, 4 KB to 256 MB in steps of 2x. Where the cost
  // jumps (clearest in lat), a cache level ran out.
  const char* cacheSizes = "Cache sizes: random reads (Load) from an RGBA8 texture of growing size";
  for (int k = 0; k <= 16; ++k) {
    const double bytes = 4096.0 * std::pow(2.0, k);
    const UINT size = UINT(std::lround(std::sqrt(bytes / 4.0)));
    const std::string name = bytes < 1048576.0 ? std::to_string(int(bytes / 1024.0)) + " KB"
                                               : std::to_string(int(bytes / 1048576.0)) + " MB";
    texTest(name, cacheSizes, "addr.random", kRandom2D, load, rgba8, Tex::Plain, size, Filter::Point, false,
            std::to_string(size) + " x " + std::to_string(size) + " texels", kUseRandom);
  }

  // Cache use: how far apart neighbouring threads may read, rows against columns, texel size and the
  // thread group's shape.
  const char* cacheUse = "Cache use: RGBA8 4096 x 4096 (Load), reads around each thread's own pixel";
  auto spreadTest = [&](std::string name, float sx, float sy, UINT tileW, std::string note) {
    texTest(std::move(name), cacheUse, "addr.spread", kSpread, load, rgba8, Tex::Plain, 4096, Filter::Point, false,
            std::move(note), kUseSpread);
    v.back().scaleX = sx, v.back().scaleY = sy, v.back().tileW = tileW;
  };
  for (int n : {1, 2, 4, 8, 16, 32, 64, 128, 256})
    spreadTest("spread " + std::to_string(n), float(n), float(n), 8,
               "random within " + std::to_string(n) + " x " + std::to_string(n) + " texels around the pixel");
  for (int n : {32, 256}) {
    spreadTest("row " + std::to_string(n), float(n), 0.0f, 8, "random along the row, " + std::to_string(n) + " texels");
    spreadTest("column " + std::to_string(n), 0.0f, float(n), 8,
               "random along the column, " + std::to_string(n) + " texels");
  }
  for (UINT w : {8u, 16u, 32u, 64u})
    spreadTest("group " + std::to_string(w) + "x" + std::to_string(64 / w), 8.0f, 8.0f, w,
               "64 threads as " + std::to_string(w) + " x " + std::to_string(64 / w) + " pixels, spread 8");
  const char* texelSize = "Cache use: texel size, random reads (Load) from 1024 x 1024";
  for (const char* fn : {"R8", "RGBA8", "RGBA16F", "RGBA32F"}) {
    const Format* f = formatByName(fn);
    texTest(std::string(fn) + " 1024^2", texelSize, "addr.random", kRandom2D, load, f, Tex::Plain, 1024,
            Filter::Point, false, std::to_string(f->bytes) + " MB", kUseRandom);
    v.back().perByte = true;
  }

  // Compute: storage writes (tex2Dstore / RWTexture2D) per format, coherent (near the thread's pixel) and
  // random; GB/s comparable with the render target writes below.
  const char* stores = "Compute: storage writes (tex2Dstore) into 4096 x 4096";
  for (const Format& f : kFormats) {
    if (f.dxgi == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) continue;  // not a storage format
    const bool i = f.kind == 'i';
    const std::string store = i ? "WU[int2(uv * size)] = uint4(asuint(uv), 1u, 2u);" : "W[int2(uv * size)] = float4(uv, x, 1.0);";
    for (bool random : {false, true}) {
      texTest(std::string(f.name) + (random ? " store random" : " store"), stores, random ? "addr.random" : "addr.coherent",
              random ? kRandom2D : kCoherent, store, &f, Tex::Storage, 4096, Filter::Point, false,
              random ? "random places" : "near the thread's own pixel", kUseBase);
      v.back().uav = i ? 2 : 1;
      v.back().perByte = true;
    }
  }

  // Compute: groupshared memory, barriers, atomics, local arrays and branches. Own constants: x stays near
  // 0.5; gi / ai index groupshared memory / a local array from x and the thread's lane l (lane-varying).
  auto cTest = [&](std::string name, std::string section, std::string base, std::string step, std::string note) {
    Test t;
    t.name = std::move(name);
    t.section = std::move(section);
    t.base = std::move(base);
    t.step = std::move(step);
    t.cx = 0.5f, t.cy = 0.25f, t.cz = 0.7f, t.cw = 0.3f;
    t.note = std::move(note);
    add(std::move(t));
    return &v.back();
  };
  const std::string gsIdx = "uint gi = (l + uint(x * 64.0)) & 2047u;";
  const std::string gsIdx32 = "uint gi = (l * 32u + uint(x * 64.0)) & 2047u;";
  const std::string gsUse = " x = mad(x, c.x, c.y + v * 0.01);";
  const std::string gsFake = " float v = asfloat((gi & 1023u) | 0x3f000000u);";
  cTest("addr.gs", "", "mad", gsIdx + gsFake + gsUse, "groupshared index, a stand-in value")->groupshared = true;
  cTest("addr.gs32", "", "mad", gsIdx32 + gsFake + gsUse, "groupshared index (stride 32), a stand-in value")->groupshared = true;
  const char* gs = "Compute: groupshared memory and barriers";
  cTest("gs read", gs, "addr.gs", gsIdx + " float v = GS[gi];" + gsUse, "neighbouring lanes, neighbouring words")->groupshared = true;
  cTest("gs read stride 32", gs, "addr.gs32", gsIdx32 + " float v = GS[gi];" + gsUse,
        "lanes 32 words apart: bank conflicts")->groupshared = true;
  cTest("gs write", gs, "addr.gs", gsIdx + " GS[gi] = x;" + gsFake + gsUse, "")->groupshared = true;
  cTest("gs write stride 32", gs, "addr.gs32", gsIdx32 + " GS[gi] = x;" + gsFake + gsUse, "bank conflicts")->groupshared = true;
  cTest("gs write + read", gs, "addr.gs", gsIdx + " GS[gi] = x; float v = GS[gi ^ 1u];" + gsUse, "the neighbour's word")
      ->groupshared = true;
  cTest("barrier", gs, "gs write", gsIdx + " GS[gi] = x; GroupMemoryBarrierWithGroupSync();" + gsFake + gsUse,
        "GroupMemoryBarrierWithGroupSync after a write")->groupshared = true;
  cTest("groupMemoryBarrier", gs, "gs write", gsIdx + " GS[gi] = x; GroupMemoryBarrier();" + gsFake + gsUse,
        "GroupMemoryBarrier after a write")->groupshared = true;
  cTest("memoryBarrier", gs, "gs write", gsIdx + " GS[gi] = x; AllMemoryBarrier();" + gsFake + gsUse,
        "AllMemoryBarrier after a write")->groupshared = true;

  // Atomics: on groupshared memory and on storage (R32U 1024 x 1024), each thread on its own address and
  // all threads (of the group / of the dispatch) on one address. The returned value feeds the chain.
  const std::string atUse = " x = mad(x, c.x, c.y + float(o & 255u) * 1e-6);";
  cTest("addr.atomic", "", "mad", "uint o = asuint(x) & 255u;" + atUse, "the atomic's stand-in");
  const char* atomics = "Compute: atomics (each thread its own address / (one): 64 threads on one address)";
  static const char* const kAtomics[] = {"Add", "And", "Or", "Xor", "Min", "Max", "Exchange", "CompareExchange"};
  for (bool storage : {false, true})
    for (const char* op : kAtomics)
      for (bool one : {false, true}) {
        // One address per group, not per dispatch: a million threads on one storage address could take
        // seconds and trip the driver's timeout (TDR).
        const std::string dest = storage ? (one ? "WA[int2(g % 1024u, g / 1024u)]" : "WA[int2(P * size)]") : (one ? "GSI[0]" : "GSI[l]");
        const std::string call = std::strcmp(op, "CompareExchange") == 0
                                     ? "InterlockedCompareExchange(" + dest + ", asuint(x) & 255u, 7u, o);"
                                     : std::string("Interlocked") + op + "(" + dest + ", asuint(x) & 255u, o);";
        const std::string shortOp = std::strcmp(op, "CompareExchange") == 0 ? "CmpXchg" : op;  // fits the column
        Test* t = cTest(std::string(storage ? "" : "gs ") + "atomic" + shortOp + (one ? " (one)" : ""), atomics, "addr.atomic",
                        "uint o; " + call + atUse,
                        std::string(storage ? "storage R32U" : "groupshared") +
                            (one ? ", the group's 64 threads on one address" : ", each thread its own address"));
        if (storage) {
          t->uav = 3;
          t->format = formatByName("R32U");
          t->tex = Tex::Storage;
          t->size = 1024;
        } else {
          t->groupshared = true;
        }
      }

  // Local arrays indexed at run time (fxc: indexable temps, often scratch memory), a constant array (fxc:
  // immediate constant buffer) and branches: uniform within a group (the condition from the group) against
  // divergent (from the lane), and both sides computed with a select.
  const std::string arIdx = "uint ai = (uint(x * 64.0) + l) & 15u;";
  const std::string arUse = " x = mad(x, c.x, c.y + v * 0.01);";
  cTest("addr.array", "", "mad", arIdx + " float v = asfloat(ai | 0x3f000000u);" + arUse, "array index, a stand-in value");
  const char* flow = "Compute: local arrays and branches";
  cTest("array read", flow, "addr.array", arIdx + " float v = A[ai];" + arUse, "float A[16], index differs per lane")->localArray = true;
  cTest("array write + read", flow, "addr.array", arIdx + " A[ai] = x; float v = A[ai ^ 1u];" + arUse, "")->localArray = true;
  cTest("const array read", flow, "addr.array", arIdx + " float v = K[ai];" + arUse, "static const float K[16]");
  const std::string brUse = " x = mad(v, c.x, c.y);";
  // Each side: 4 sin / 4 cos in a row (short sides get turned into selects by the driver).
  const std::string sideA = "sin(sin(sin(sin(x))))", sideB = "cos(cos(cos(cos(x))))";
  cTest("select (both)", flow, "mad", "bool sel = ((asuint(c.w) ^ g) & 1u) != 0; float v = sel ? " + sideA + " : " + sideB + ";" + brUse,
        "4 sin and 4 cos both computed, one result picked");
  cTest("branch uniform", flow, "mad",
        "bool sel = ((asuint(c.w) ^ g) & 1u) != 0; float v; [branch] if (sel) v = " + sideA + "; else v = " + sideB + ";" + brUse,
        "the same in a branch, one side per group");
  cTest("branch divergent", flow, "mad",
        "bool sel = ((asuint(c.w) ^ l) & 1u) != 0; float v; [branch] if (sel) v = " + sideA + "; else v = " + sideB + ";" + brUse,
        "the same in a branch, both sides in every group");

  // Pixel shader: derivatives (quad operations) and sampling with automatic mip selection.
  const char* ps = "Pixel shader: derivatives and automatic mip selection";
  Test pmad = mad;
  pmad.name = "ps.mad";
  pmad.stage = Stage::Pixel;
  pmad.note = "reference in the pixel shader";
  add(pmad);
  for (const char* d : {"ddx", "ddy", "ddx_fine", "ddy_fine", "ddx_coarse", "ddy_coarse", "fwidth"}) {
    Test t;
    t.name = d;
    t.section = ps;
    t.stage = Stage::Pixel;
    t.base = "ps.mad";
    t.step = std::string("x = mad(") + d + "(x), c.x, c.y);";
    add(t);
  }
  baseTest("ps.addr", kCoherent, false, Stage::Pixel);
  v.back().base = "ps.mad";
  auto psSample = [&](const char* name, Filter filter, float sx, float sy, const char* note) {
    Test t;
    t.name = name;
    t.section = ps;
    t.stage = Stage::Pixel;
    t.base = "ps.addr";
    t.step = std::string(kCoherent) + " float4 t = T.Sample(S, uv); " + kUse;
    t.cx = tx, t.cy = ty, t.cz = tz, t.cw = tw;
    t.format = rgba8;
    t.tex = Tex::Mipped;
    t.size = 1024;
    t.filter = filter;
    t.scaleX = sx, t.scaleY = sy;
    t.note = note;
    add(t);
  };
  psSample("Sample point", Filter::Point, 1.0f, 1.0f, "Sample with point filtering, RGBA8 1024 x 1024");
  psSample("Sample bilinear", Filter::Linear, 1.0f, 1.0f, "Sample (automatic mip level 0), RGBA8 1024 x 1024");
  psSample("Sample trilinear", Filter::Trilinear, 1.5f, 1.5f, "1.5 texels per pixel: between mip 0 and 1");
  psSample("Sample aniso 4:1", Filter::Aniso, 1.0f, 4.0f, "4 x 1 texel footprint, 16x anisotropic filtering");
  psSample("Sample offset", Filter::Linear, 1.0f, 1.0f, "tex2D with an offset, bilinear");
  v.back().step = std::string(kCoherent) + " float4 t = T.Sample(S, uv, int2(1, -1)); " + kUse;

  // Render target writes: full-screen passes per format, noise, a smooth gradient and one flat color (the
  // same shader, only the data differs): GPUs compress render targets, so compressible output can beat
  // the memory bandwidth.
  for (const Format& f : kFormats)
    for (int mode : {1, 0, 2}) {
      Test t;
      t.name = std::string(f.name) + (mode == 1 ? " write noise" : mode == 0 ? " write smooth" : " write flat");
      t.section = "Render target writes: full-screen passes into 3840 x 2160";
      t.stage = Stage::Write;
      t.format = &f;
      t.intTex = f.kind == 'i';
      t.write = mode;
      add(t);
    }

  // Blending (ReShade pass states BlendEnable / BlendOp / SrcBlend / DestBlend): the output blended into
  // the target by the hardware against a shader that reads the target's content as a texture and does the
  // same math. Each pass first restores the target's content (noise) by a copy, timed alone and subtracted.
  for (const char* fn : {"RGBA8", "RGB10A2", "RG11B10F", "RGBA16F", "RGBA32F"})
    for (int op = 0; op <= 4; ++op)
      for (bool shader : {false, true}) {
        if (op == 0 && shader) continue;
        static const char* const kOps[] = {"plain", "add", "lerp", "multiply", "min"};
        Test t;
        t.name = std::string(fn) + " " + kOps[op] + (op == 0 ? "" : shader ? " shader" : " blend");
        t.section = "Blending: full-screen passes into 3840 x 2160";
        t.stage = Stage::Blend;
        t.format = formatByName(fn);
        t.blend = op;
        t.shaderBlend = shader;
        add(t);
      }

  // Pass states: several render targets at once, clears, mipmap generation, and a heavy shader on all
  // pixels against half of them masked by the stencil test or by discard (8 x 8 tiles or single pixels).
  auto passTest = [&](std::string name, int kind, const char* fn, int count, std::string note) {
    Test t;
    t.name = std::move(name);
    t.section = "Pass states: full-screen passes into 3840 x 2160";
    t.stage = Stage::Pass;
    t.pass = kind;
    t.format = formatByName(fn);
    t.count = count;
    t.note = std::move(note);
    add(t);
  };
  for (int n : {1, 2, 4, 8})
    passTest(std::to_string(n) + (n == 1 ? " target" : " targets"), kPassTargets, "RGBA8", n,
             "RGBA8 render targets written by one pass (RenderTarget0..)");
  for (const char* fn : {"RGBA8", "RGBA16F"}) {
    passTest(std::string("clear ") + fn, kPassClear, fn, 1, "ClearRenderTargets");
    passTest(std::string("mipmaps ") + fn, kPassMips, fn, 1, "the mip chain of a 3840 x 2160 texture (MipLevels)");
  }
  passTest("heavy shader", kPassHeavy, "RGBA8", 1, "32 sin per pixel, every pixel");
  passTest("heavy, stencil 50%", kPassStencil, "RGBA8", 1, "half the pixels (8 x 8 tiles) fail the stencil test");
  passTest("heavy, discard 50% tiles", kPassDiscardTiles, "RGBA8", 1, "discard first in half the 8 x 8 tiles");
  passTest("heavy, discard 50% pixels", kPassDiscardPixels, "RGBA8", 1, "discard first in every other pixel");
  return v;
}

// ---------------------------------------------------------------------------
// Shaders

std::string chainStep(const std::string& step, const std::string& x) {
  // The chain variable x (a lone identifier) becomes xk; uv / t / tu get a per-chain scope.
  std::string out;
  for (size_t p = 0; p < step.size(); ++p) {
    const bool lone = step[p] == 'x' && (p == 0 || !(isalnum((unsigned char)step[p - 1]) || step[p - 1] == '_' || step[p - 1] == '.')) &&
                      (p + 1 == step.size() || !(isalnum((unsigned char)step[p + 1]) || step[p + 1] == '_'));
    out += lone ? x : std::string(1, step[p]);
  }
  return "{ " + out + " }";
}

const char* const kHeader =
    "cbuffer C : register(b0) { float4 U[16]; uint iters; float seed; float2 texel; float2 size; float2 scale; };\n"
    "Texture2D<float4> T : register(t0);\n"
    "Texture2D<uint4> TU : register(t1);\n"
    "Texture3D<float4> T3 : register(t2);\n"
    "Texture1D<float4> T1 : register(t3);\n"
    "SamplerState S : register(s0);\n";

std::string chainBody(const Test& t, int chains, const char* indent) {
  std::string s;
  s += std::string(indent) + "[loop] for (uint i = 0; i < iters; ++i)\n" + indent + "{\n";
  for (int r = 0; r < kUnroll; ++r) {
    s += std::string(indent) + "  {\n" + indent + "    const float4 c = U[" + std::to_string(r) + "];\n";
    for (int k = 0; k < chains; ++k) s += std::string(indent) + "    " + chainStep(t.step, "x" + std::to_string(k)) + "\n";
    s += std::string(indent) + "  }\n";
  }
  s += std::string(indent) + "}\n";
  return s;
}

std::string computeSource(const Test& t, int chains) {
  std::string s = kHeader;
  if (t.uav == 1) s += "RWTexture2D<float4> W : register(u1);\n";
  if (t.uav == 2) s += "RWTexture2D<uint4> WU : register(u1);\n";
  if (t.uav == 3) s += "RWTexture2D<uint> WA : register(u1);\n";
  if (t.groupshared) s += "groupshared float GS[2048];\ngroupshared uint GSI[64];\n";
  s += "static const float K[16] = {0.51, 0.52, 0.53, 0.54, 0.55, 0.56, 0.57, 0.58, 0.59, 0.60, 0.61, 0.62, 0.63, 0.64, 0.65, 0.66};\n";
  // Thread groups of 64 as tiles of tileW x (64 / tileW) pixels, laid out over 1024 x 1024 pixels.
  const std::string w = std::to_string(t.tileW), h = std::to_string(64 / t.tileW), row = std::to_string(1024 / t.tileW);
  s += "RWStructuredBuffer<float> O : register(u0);\n"
       "[numthreads(64, 1, 1)]\nvoid main(uint3 id : SV_DispatchThreadID)\n{\n"
       "  const uint g = id.x / 64u, l = id.x % 64u;\n"
       "  const float2 P = (float2((g % " + row + "u) * " + w + "u + l % " + w + "u, (g / " + row + "u) * " + h +
       "u + l / " + w + "u) + 0.5) * texel;\n";
  if (t.groupshared)
    s += "  for (uint k = l; k < 2048u; k += 64u) GS[k] = 0.5 + float(k) * 1e-4 + seed;\n"
         "  GSI[l] = l;\n  GroupMemoryBarrierWithGroupSync();\n";
  if (t.localArray) s += "  float A[16];\n  [unroll] for (uint k = 0; k < 16u; ++k) A[k] = 0.5 + float(k) * 0.01 + seed;\n";
  for (int k = 0; k < chains; ++k)
    s += "  float x" + std::to_string(k) + " = 0.3 + frac(id.x * 0.000123 + " + std::to_string(k) + " * 0.137) * seed;\n";
  s += chainBody(t, chains, "  ");
  s += "  O[id.x] = 0.0";
  for (int k = 0; k < chains; ++k) s += " + x" + std::to_string(k);
  // Read groupshared memory once at the end: fxc drops groupshared writes (and their barriers) that
  // nothing reads.
  if (t.groupshared) s += " + GS[(l * 7u) & 2047u] + float(GSI[l] & 1u)";
  s += ";\n}\n";
  return s;
}

const char* const kVertexShader =
    "float4 main(uint id : SV_VertexID) : SV_Position\n"
    "{\n  const float2 t = float2((id << 1) & 2, id & 2);\n  return float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n}\n";

std::string pixelSource(const Test& t, int chains) {
  std::string s = kHeader;
  s += "float4 main(float4 pos : SV_Position) : SV_Target\n{\n"
       "  const float2 P = pos.xy * texel * scale;\n";
  for (int k = 0; k < chains; ++k)
    s += "  float x" + std::to_string(k) + " = 0.3 + frac(pos.x * 0.000123 + pos.y * 0.0371 + " + std::to_string(k) +
         " * 0.137) * seed;\n";
  s += chainBody(t, chains, "  ");
  s += "  return float4(0.0";
  for (int k = 0; k < chains; ++k) s += " + x" + std::to_string(k);
  s += ", 0.0, 0.0, 1.0);\n}\n";
  return s;
}

// Render target writes: a smooth gradient, a hash of the pixel (noise) or a flat color, picked by U[0].x
// (0 / 1 / 2), so all three cost the same shader work.
std::string writeSource(bool integer) {
  std::string s = std::string(kHeader) +
                  "uint hash(uint v) { v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16; return v; }\n"
                  "uint4 noise4(float4 pos) {\n"
                  "  uint k = uint(pos.y) * 4096u + uint(pos.x);\n"
                  "  return uint4(hash(k), hash(k ^ 0x9e3779b9u), hash(k ^ 0x7f4a7c15u), hash(k ^ 0x94d049bbu));\n"
                  "}\n";
  if (integer)
    return s + "uint4 main(float4 pos : SV_Position) : SV_Target {\n"
               "  uint mask = U[0].x > 0.5 ? 0xffffffffu : 0u;\n"
               "  uint4 v = (noise4(pos) & mask) | (uint4(pos.xyxy) & ~mask);\n"
               "  return U[0].x > 1.5 ? uint4(1u, 2u, 3u, 4u) : v;\n"
               "}\n";
  return s + "float4 main(float4 pos : SV_Position) : SV_Target {\n"
             "  float4 noise = asfloat((noise4(pos) >> 9) | 0x3f800000u) - 1.0;\n"
             "  float4 smooth = frac(pos.xyxy * float4(0.0013, 0.0017, 0.0019, 0.0023));\n"
             "  return U[0].x > 1.5 ? float4(0.25, 0.5, 0.75, 1.0) : lerp(smooth, noise, U[0].x);\n"
             "}\n";
}

// Blending: the source color is noise (alpha too); the shader version reads the destination from T and
// does the blend's math itself.
std::string blendSource(int op, bool shader) {
  std::string s = std::string(kHeader) +
                  "uint hash(uint v) { v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16; return v; }\n"
                  "float4 main(float4 pos : SV_Position) : SV_Target {\n"
                  "  uint k = uint(pos.y) * 4096u + uint(pos.x);\n"
                  "  uint4 n = uint4(hash(k), hash(k ^ 0x9e3779b9u), hash(k ^ 0x7f4a7c15u), hash(k ^ 0x94d049bbu));\n"
                  "  float4 s = asfloat((n >> 9) | 0x3f800000u) - 1.0;\n";
  if (!shader || op == 0) return s + "  return s;\n}\n";
  s += "  float4 d = T.Load(int3(pos.xy, 0));\n";
  static const char* const kMath[] = {"", "s + d", "lerp(d, s, s.a)", "s * d", "min(s, d)"};
  return s + "  return " + kMath[op] + ";\n}\n";
}

// Pass state tests: noise into count targets, or the heavy shader (with discard for the discard tests).
std::string passSource(const Test& t) {
  std::string s =
      "uint hash(uint v) { v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16; return v; }\n"
      "float4 noise(float4 pos) {\n"
      "  uint k = uint(pos.y) * 4096u + uint(pos.x);\n"
      "  uint4 n = uint4(hash(k), hash(k ^ 0x9e3779b9u), hash(k ^ 0x7f4a7c15u), hash(k ^ 0x94d049bbu));\n"
      "  return asfloat((n >> 9) | 0x3f800000u) - 1.0;\n"
      "}\n";
  if (t.pass == kPassTargets) {
    s += "struct Out {";
    for (int k = 0; k < t.count; ++k) s += " float4 c" + std::to_string(k) + " : SV_Target" + std::to_string(k) + ";";
    s += " };\nOut main(float4 pos : SV_Position) {\n  float4 v = noise(pos);\n  Out o;\n";
    static const char* const kSw[] = {"xyzw", "yzwx", "zwxy", "wxyz", "wzyx", "zyxw", "yxwz", "xwzy"};
    for (int k = 0; k < t.count; ++k) s += std::string("  o.c") + std::to_string(k) + " = v." + kSw[k] + ";\n";
    return s + "  return o;\n}\n";
  }
  s = std::string("cbuffer C : register(b0) { float4 U[16]; };\n") + s + "float4 main(float4 pos : SV_Position) : SV_Target {\n";
  if (t.pass == kPassDiscardTiles) s += "  if (((uint(pos.x) >> 3) ^ (uint(pos.y) >> 3)) & 1u) discard;\n";
  if (t.pass == kPassDiscardPixels) s += "  if ((uint(pos.x) ^ uint(pos.y)) & 1u) discard;\n";
  // Constants from the constant buffer: with literals fxc folds the whole loop into one constant.
  return s + "  float4 v = noise(pos);\n  [unroll] for (int k = 0; k < 32; ++k) v = sin(v * U[k & 15].x + U[k & 15].y);\n"
             "  return v;\n}\n";
}

// Stencil test: marks half the pixels (8 x 8 tiles) in the stencil buffer.
const char* const kStencilMark =
    "void main(float4 pos : SV_Position) { if (((uint(pos.x) >> 3) ^ (uint(pos.y) >> 3)) & 1u) discard; }\n";

// ---------------------------------------------------------------------------
// GPU

struct CbData {
  float U[16][4];
  UINT iters;
  float seed;
  float texel[2];
  float size[2];
  float scale[2];
};

struct Gpu {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  ID3D11Buffer* cb = nullptr;
  ID3D11Buffer* out = nullptr;
  ID3D11UnorderedAccessView* uav = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11Texture2D* psTarget = nullptr;
  ID3D11RenderTargetView* psRtv = nullptr;
  Timer timer;
};

template <class T>
void release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

// Compiles with D3DCompile -O3; with dump set, writes the source and the DXBC disassembly there (so a
// test the compiler folded can be spotted).
ID3DBlob* compile(const std::string& src, const char* name, const char* profile, const std::filesystem::path& dump = {}) {
  ID3DBlob* code = nullptr;
  ID3DBlob* err = nullptr;
  if (FAILED(D3DCompile(src.data(), src.size(), name, nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                        &code, &err)))
    fail(std::string("cannot compile ") + name + ":\n" + (err ? (const char*)err->GetBufferPointer() : "") + "\n" + src);
  if (err) err->Release();
  ID3DBlob* dis = nullptr;
  if (!dump.empty() && SUCCEEDED(D3DDisassemble(code->GetBufferPointer(), code->GetBufferSize(), 0, nullptr, &dis))) {
    if (FILE* f = std::fopen(dump.string().c_str(), "wb")) {
      std::fwrite(src.data(), 1, src.size(), f);
      std::fputs("\n// ---- DXBC (D3DCompile -O3) ----\n", f);
      std::fwrite(dis->GetBufferPointer(), 1, dis->GetBufferSize() - 1, f);
      std::fclose(f);
    }
    dis->Release();
  }
  return code;
}

// A file name for a test ("RGBA8 bilinear" -> "RGBA8_bilinear.txt").
std::string fileName(const std::string& test) {
  std::string s;
  for (char ch : test) s += isalnum((unsigned char)ch) || ch == '.' ? ch : '_';
  return s + ".txt";
}

// The resources one test reads: a texture and its view, and a sampler.
struct Bound {
  ID3D11Resource* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  ID3D11UnorderedAccessView* uav = nullptr;
  ID3D11SamplerState* sampler = nullptr;
  void free() {
    release(uav);
    release(srv);
    release(tex);
    release(sampler);
  }
};

Bound bindResources(Gpu& g, const Test& t, std::mt19937& rng) {
  Bound b;
  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = t.filter == Filter::Point       ? D3D11_FILTER_MIN_MAG_MIP_POINT
              : t.filter == Filter::Linear    ? D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT
              : t.filter == Filter::Trilinear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR
                                              : D3D11_FILTER_ANISOTROPIC;
  sd.AddressU = sd.AddressV = sd.AddressW = t.address;
  sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 0.5f;
  sd.MaxAnisotropy = t.filter == Filter::Aniso ? 16 : 1;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
  if (FAILED(g.dev->CreateSamplerState(&sd, &b.sampler))) fail("cannot create a sampler for " + t.name);
  if (t.tex == Tex::None) return b;

  const Format& f = *t.format;
  if (t.tex == Tex::Storage) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = t.size;
    td.MipLevels = td.ArraySize = 1;
    td.Format = f.dxgi;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &tex))) fail("cannot create the storage texture for " + t.name);
    b.tex = tex;
    if (FAILED(g.dev->CreateUnorderedAccessView(b.tex, nullptr, &b.uav))) fail("cannot create the storage view for " + t.name);
    return b;
  }
  if (t.tex == Tex::Tex1D) {
    D3D11_TEXTURE1D_DESC td = {};
    td.Width = t.size;
    td.MipLevels = td.ArraySize = 1;
    td.Format = f.dxgi;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const auto data = texelData(f, t.size, rng);
    D3D11_SUBRESOURCE_DATA init = {data.data(), 0, 0};
    ID3D11Texture1D* tex = nullptr;
    if (FAILED(g.dev->CreateTexture1D(&td, &init, &tex))) fail("cannot create the texture for " + t.name);
    b.tex = tex;
  } else if (t.tex == Tex::Lut3D || t.tex == Tex::Vol) {
    D3D11_TEXTURE3D_DESC td = {};
    td.Width = td.Height = t.size;
    td.Depth = t.tex == Tex::Vol ? t.depth : t.size;
    td.MipLevels = 1;
    td.Format = f.dxgi;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const auto data = texelData(f, size_t(t.size) * t.size * td.Depth, rng);
    D3D11_SUBRESOURCE_DATA init = {data.data(), t.size * UINT(f.bytes), t.size * t.size * UINT(f.bytes)};
    ID3D11Texture3D* tex = nullptr;
    if (FAILED(g.dev->CreateTexture3D(&td, &init, &tex))) fail("cannot create the texture for " + t.name);
    b.tex = tex;
  } else {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = t.tex == Tex::Lut2D ? t.size * t.size : t.size;
    td.Height = t.tex == Tex::Lut1D ? 1 : t.size;
    td.MipLevels = t.tex == Tex::Mipped ? 0 : 1;
    td.ArraySize = 1;
    td.Format = f.dxgi;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (t.tex == Tex::Mipped ? D3D11_BIND_RENDER_TARGET : 0);
    td.MiscFlags = t.tex == Tex::Mipped ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &tex))) fail("cannot create the texture for " + t.name);
    // Upload in bands of rows (a 4096 x 4096 RGBA32F texture is 256 MB).
    const UINT band = std::max(1u, (16u << 20) / (td.Width * UINT(f.bytes)));
    for (UINT y = 0; y < td.Height; y += band) {
      const UINT rows = std::min(band, td.Height - y);
      const auto data = texelData(f, size_t(td.Width) * rows, rng);
      D3D11_BOX box = {0, y, 0, td.Width, y + rows, 1};
      g.ctx->UpdateSubresource(tex, 0, &box, data.data(), td.Width * UINT(f.bytes), 0);
    }
    b.tex = tex;
  }
  if (FAILED(g.dev->CreateShaderResourceView(b.tex, nullptr, &b.srv))) fail("cannot create the view for " + t.name);
  if (t.tex == Tex::Mipped) g.ctx->GenerateMips(b.srv);
  return b;
}

void setConstants(Gpu& g, const Test& t, UINT iters, UINT texSize) {
  CbData d = {};
  for (int r = 0; r < 16; ++r) {
    // Distinct constants per step (within 0.1%), so no two steps can be merged.
    const float e = 1.0f + 0.0001f * float(r);
    d.U[r][0] = t.cx * e;
    d.U[r][1] = t.cy * e;
    d.U[r][2] = t.cz * e;
    d.U[r][3] = t.cw * e;
  }
  d.iters = iters;
  d.seed = 0.1f;
  const float s = float(texSize ? texSize : 1024);
  d.texel[0] = d.texel[1] = 1.0f / s;
  d.size[0] = d.size[1] = s;
  if (t.tex == Tex::Lut1D) d.size[1] = 1.0f;
  d.scale[0] = t.scaleX;
  d.scale[1] = t.scaleY;
  g.ctx->UpdateSubresource(g.cb, 0, nullptr, &d, 0, 0);
}

struct Config {
  const char* name;
  int chains;
  UINT groups;
  const char* title;
};
Config kConfigs[] = {{"tput", 8, kGroupsFull, "Throughput"},
                     {"dep", 1, kGroupsFull, "Dependent chains"},
                     {"lat", 1, 1, "Latency"}};

// A compiled test for one configuration.
struct Kernel {
  ID3D11ComputeShader* cs = nullptr;
  ID3D11PixelShader* ps = nullptr;
};

void bind(Gpu& g, const Test& t, const Bound& b, const Kernel& k) {
  ID3D11ShaderResourceView* views[4] = {nullptr, nullptr, nullptr, nullptr};
  if (b.srv) views[t.tex == Tex::Lut3D || t.tex == Tex::Vol ? 2 : t.tex == Tex::Tex1D ? 3 : t.intTex ? 1 : 0] = b.srv;
  if (k.cs) {
    g.ctx->CSSetShader(k.cs, nullptr, 0);
    g.ctx->CSSetUnorderedAccessViews(1, 1, &b.uav, nullptr);
    g.ctx->CSSetShaderResources(0, 4, views);
    g.ctx->CSSetSamplers(0, 1, &b.sampler);
  } else {
    g.ctx->PSSetShader(k.ps, nullptr, 0);
    g.ctx->PSSetShaderResources(0, 4, views);
    g.ctx->PSSetSamplers(0, 1, &b.sampler);
  }
}

double run(Gpu& g, const Kernel& k, UINT groups) {
  if (k.cs) return g.timer.time(g.ctx, [&] { g.ctx->Dispatch(groups, 1, 1); });
  return g.timer.time(g.ctx, [&] { g.ctx->Draw(3, 0); });
}

struct Result {
  UINT iters = 0;
  double ms = 0.0;
  double nsPerStep = 0.0;
};

UINT calibrate(Gpu& g, const Test& t, const Kernel& k, UINT groups) {
  UINT iters = 2;
  for (;;) {
    setConstants(g, t, iters, t.size);
    if (run(g, k, groups) >= 2.0 || iters >= (1u << 20)) return iters;
    iters *= 2;
  }
}

Result measureAt(Gpu& g, const Test& t, const Kernel& k, const Config& c, UINT iters, int reps) {
  setConstants(g, t, iters, t.size);
  std::vector<double> runs;
  for (int n = 0; n < reps * 3 && int(runs.size()) < reps; ++n)
    if (const double m = run(g, k, c.groups); m > 0.0) runs.push_back(m);
  Result r;
  r.iters = iters;
  r.ms = median(runs);
  const double threads = k.cs ? double(c.groups) * kGroupSize : double(kPsSize) * kPsSize;
  r.nsPerStep = r.ms * 1e6 / (double(iters) * kUnroll * threads * c.chains);
  return r;
}

struct Measured {
  Result r;
  std::vector<double> readings;
  Consensus units;
  double vsBase = 0.0;
};

// Render target writes: the median time of one full-screen pass (in ms) at a draw count that
// takes >= 2 ms. The draws alternate between two targets: NVIDIA (and other tiling GPUs) keep back to
// back full-screen draws into one target in an on-chip tile cache, so only the last one reached memory
// (measured above the GTX 1660's memory bandwidth).
double writePass(Gpu& g, ID3D11RenderTargetView* const rtv[2], ID3D11PixelShader* ps, int reps, UINT& draws) {
  D3D11_VIEWPORT vp = {0.0f, 0.0f, float(kRtW), float(kRtH), 0.0f, 1.0f};
  g.ctx->RSSetViewports(1, &vp);
  g.ctx->PSSetShader(ps, nullptr, 0);
  auto timeDraws = [&](UINT n) {
    return g.timer.time(g.ctx, [&] {
      for (UINT k = 0; k < n; ++k) {
        g.ctx->OMSetRenderTargets(1, &rtv[k & 1], nullptr);
        g.ctx->Draw(3, 0);
      }
    });
  };
  if (draws == 0) {
    draws = 1;
    while (draws < 4096 && timeDraws(draws) < 2.0) draws *= 2;
  }
  std::vector<double> runs;
  for (int n = 0; n < reps * 3 && int(runs.size()) < reps; ++n)
    if (const double m = timeDraws(draws); m > 0.0) runs.push_back(m / draws);
  return median(runs);
}

// Blending: the median time of one pass (in ms). A unit restores both targets' content (noise, a plain
// draw each), then runs the test's pass on both; the time of the restores alone, measured right before,
// is subtracted. Two targets so no draw lands on the one right before it (tile cache, see writePass);
// draws, not copies, restore the content (copies gave two-valued results on the GTX 1660).
double blendPass(Gpu& g, ID3D11RenderTargetView* const rtv[2], ID3D11PixelShader* restore, ID3D11PixelShader* ps,
                 ID3D11ShaderResourceView* srv, ID3D11BlendState* bs, int reps, UINT& draws) {
  const float factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  D3D11_VIEWPORT vp = {0.0f, 0.0f, float(kRtW), float(kRtH), 0.0f, 1.0f};
  g.ctx->RSSetViewports(1, &vp);
  g.ctx->PSSetShaderResources(0, 1, &srv);
  auto timeUnits = [&](UINT n, bool pass) {
    return g.timer.time(g.ctx, [&] {
      for (UINT k = 0; k < n; ++k) {
        g.ctx->OMSetBlendState(nullptr, factor, 0xffffffffu);
        g.ctx->PSSetShader(restore, nullptr, 0);
        for (int r = 0; r < 2; ++r) {
          g.ctx->OMSetRenderTargets(1, &rtv[r], nullptr);
          g.ctx->Draw(3, 0);
        }
        if (!pass) continue;
        g.ctx->OMSetBlendState(bs, factor, 0xffffffffu);
        g.ctx->PSSetShader(ps, nullptr, 0);
        for (int r = 0; r < 2; ++r) {
          g.ctx->OMSetRenderTargets(1, &rtv[r], nullptr);
          g.ctx->Draw(3, 0);
        }
      }
    });
  };
  if (draws == 0) {
    draws = 1;
    while (draws < 4096 && timeUnits(draws, true) < 2.0) draws *= 2;
  }
  std::vector<double> runs;
  for (int n = 0; n < reps * 3 && int(runs.size()) < reps; ++n) {
    const double base = timeUnits(draws, false), unit = timeUnits(draws, true);
    if (base > 0.0 && unit > 0.0) runs.push_back((unit - base) / (2.0 * draws));
  }
  ID3D11ShaderResourceView* nullSrv = nullptr;
  ID3D11RenderTargetView* nullRtv = nullptr;
  g.ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
  g.ctx->OMSetBlendState(nullptr, factor, 0xffffffffu);
  g.ctx->PSSetShaderResources(0, 1, &nullSrv);
  return median(runs);
}

}  // namespace

int main(int argc, char** argv) {
  gProgram = "TexBench";
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const Style st = initConsole();
  int adapterIndex = -1;
  bool list = false;
  std::string filter, outPath;
  int reps = 7;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) fail("missing value for " + a);
      return argv[++i];
    };
    if (a == "--adapter") adapterIndex = std::atoi(next());
    else if (a == "--list") list = true;
    else if (a == "--filter") filter = next();
    else if (a == "--reps") reps = std::max(1, std::atoi(next()));
    else if (a == "--out") outPath = next();
    else if (a == "--groups") {
      const UINT n = UINT(std::max(1, std::min(int(kGroupsFull), std::atoi(next()))));
      kConfigs[0].groups = kConfigs[1].groups = n;
    } else {
      std::printf("TexBench %s\nusage: TexBench [--adapter N] [--list] [--filter text] [--reps N] [--out file.csv] [--groups N]\n",
                  SOPT_VERSION);
      return a == "-h" || a == "--help" ? 0 : 1;
    }
  }
  if (!list) {
    printBox(st, std::string("TexBench ") + SOPT_VERSION + "  -  by CeeJay.dk");
    std::printf("\n");
  }
  const Adapter ad = selectAdapter(st, list, adapterIndex);
  if (list) return 0;

  Gpu g;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(ad.adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &g.dev,
                               nullptr, &g.ctx)))
    fail("D3D11CreateDevice failed");
  {
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CbData);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &g.cb))) fail("cannot create the constant buffer");
    D3D11_BUFFER_DESC ob = {};
    ob.ByteWidth = kGroupsFull * kGroupSize * 4;
    ob.Usage = D3D11_USAGE_DEFAULT;
    ob.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ob.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    ob.StructureByteStride = 4;
    if (FAILED(g.dev->CreateBuffer(&ob, nullptr, &g.out))) fail("cannot create the output buffer");
    if (FAILED(g.dev->CreateUnorderedAccessView(g.out, nullptr, &g.uav))) fail("cannot create the output view");
    g.timer.create(g.dev);
    g.ctx->CSSetConstantBuffers(0, 1, &g.cb);
    g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
    g.ctx->CSSetUnorderedAccessViews(0, 1, &g.uav, nullptr);
    ID3DBlob* vsCode = compile(kVertexShader, "vs", "vs_5_0");
    if (FAILED(g.dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &g.vs)))
      fail("cannot create the vertex shader");
    vsCode->Release();
    g.ctx->VSSetShader(g.vs, nullptr, 0);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = kPsSize;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &g.psTarget)) ||
        FAILED(g.dev->CreateRenderTargetView(g.psTarget, nullptr, &g.psRtv)))
      fail("cannot create the pixel shader target");
  }

  char exePath[MAX_PATH];
  GetModuleFileNameA(nullptr, exePath, MAX_PATH);
  const std::filesystem::path here = std::filesystem::path(exePath).parent_path();
  std::string safeName = ad.name;
  for (char& ch : safeName)
    if (!isalnum((unsigned char)ch)) ch = '_';
  if (outPath.empty()) outPath = (here / ("texbench-" + safeName + ".csv")).string();
  const std::filesystem::path dxbcDir = here / "texbench-dxbc";
  std::filesystem::create_directories(dxbcDir);

  // The selected tests and the bases they need.
  const std::vector<Test> all = makeTests();
  std::vector<const Test*> tests;
  auto byName = [&](const std::string& n) -> const Test* {
    for (const Test& t : all)
      if (t.name == n) return &t;
    return nullptr;
  };
  // Storage formats this GPU cannot write from a compute shader (typed UAV stores beyond D3D11's required
  // formats are optional) are left out and listed.
  std::string noStorage;
  auto storageOk = [&](const Test& t) {
    if (t.tex != Tex::Storage) return true;
    UINT support = 0;
    if (SUCCEEDED(g.dev->CheckFormatSupport(t.format->dxgi, &support)) &&
        (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW))
      return true;
    if (noStorage.find(t.format->name) == std::string::npos)
      noStorage += std::string(noStorage.empty() ? "" : ", ") + t.format->name;
    return false;
  };
  for (const Test& t : all)
    if ((filter.empty() || t.name.find(filter) != std::string::npos || t.section.empty() || t.name == "mad" || t.name == "ps.mad") &&
        storageOk(t))
      tests.push_back(&t);
  if (!noStorage.empty()) std::printf("Not supported as storage (compute writes) on this GPU, left out: %s\n", noStorage.c_str());
  for (bool added = true; added;) {
    added = false;
    for (const Test* t : std::vector<const Test*>(tests))
      if (!t->base.empty() && std::find(tests.begin(), tests.end(), byName(t->base)) == tests.end()) {
        tests.push_back(byName(t->base));
        added = true;
      }
  }
  // Bases first (their results are subtracted), then in the list's order.
  std::stable_sort(tests.begin(), tests.end(), [](const Test* a, const Test* b) { return a->section.empty() && !b->section.empty(); });

  size_t planned = 0;
  for (const Test* t : tests) planned += t->stage == Stage::Compute ? 2 * 3 : t->stage == Stage::Pixel ? 2 * 2 : 2;
  std::printf("\n%zu tests, each read at least twice (more often when the readings disagree), each against the\n"
              "reference mad measured right before it. Warming up the GPU for 2 seconds ...",
              tests.size());
  std::mt19937 rng(12345);

  // Compile every compute / pixel test for its configurations.
  std::map<std::string, std::map<std::string, Kernel>> kernels;  // test -> config -> kernel
  for (const Test* t : tests) {
    if (t->stage == Stage::Write || t->stage == Stage::Blend || t->stage == Stage::Pass) continue;
    for (const Config& c : kConfigs) {
      if (t->stage == Stage::Pixel && std::strcmp(c.name, "lat") == 0) continue;
      Kernel k;
      if (t->stage == Stage::Compute) {
        ID3DBlob* code = compile(computeSource(*t, c.chains), t->name.c_str(), "cs_5_0",
                                 c.chains == 8 ? dxbcDir / fileName(t->name) : std::filesystem::path());
        if (FAILED(g.dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &k.cs)))
          fail("cannot create " + t->name);
        code->Release();
      } else {
        ID3DBlob* code = compile(pixelSource(*t, c.chains), t->name.c_str(), "ps_5_0",
                                 c.chains == 8 ? dxbcDir / fileName(t->name) : std::filesystem::path());
        if (FAILED(g.dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &k.ps)))
          fail("cannot create " + t->name);
        code->Release();
      }
      kernels[t->name][c.name] = k;
    }
  }
  // Warm up with the compute reference.
  {
    Bound none = bindResources(g, *byName("mad"), rng);
    const Kernel& k = kernels["mad"]["tput"];
    bind(g, *byName("mad"), none, k);
    setConstants(g, *byName("mad"), 256, 0);
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < 2000) run(g, k, kConfigs[0].groups);
    none.free();
  }
  std::printf(" done\n\n   %s%s%s\n   ", st.c("\x1b[90m"), progressScale(int(planned)).c_str(), st.reset());
  Progress progress{&st, int(planned)};

  std::map<std::string, std::map<std::string, Measured>> results;  // config -> test -> result
  std::map<std::string, std::map<std::string, UINT>> iters;        // config -> test -> iterations
  std::map<std::string, std::vector<double>> writeReadings;
  std::map<std::string, Consensus> writeUnits;
  std::map<std::string, double> writeMs;
  std::vector<double> refs;
  std::map<std::string, std::vector<double>> blendReadings;
  std::map<std::string, Consensus> blendMs;
  std::map<std::string, std::vector<double>> passReadings;
  std::map<std::string, Consensus> passMs;
  for (const Test* t : tests) {
    if (t->stage == Stage::Pass) {
      std::vector<ID3D11Texture2D*> rts;
      std::vector<ID3D11RenderTargetView*> rtvs;
      ID3D11ShaderResourceView* srv = nullptr;
      ID3D11Texture2D* ds = nullptr;
      ID3D11DepthStencilView* dsv = nullptr;
      ID3D11DepthStencilState *dsMark = nullptr, *dsTest = nullptr;
      ID3D11PixelShader *ps = nullptr, *psMark = nullptr;
      // Draw tests alternate between two sets of targets (the tile cache, see writePass).
      const int count = t->pass == kPassTargets ? t->count : 1;
      const bool draws2 = t->pass != kPassClear && t->pass != kPassMips;
      for (int k = 0; k < count * (draws2 ? 2 : 1); ++k) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = kRtW;
        td.Height = kRtH;
        td.MipLevels = t->pass == kPassMips ? 0 : 1;
        td.ArraySize = 1;
        td.Format = t->format->dxgi;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | (t->pass == kPassMips ? D3D11_BIND_SHADER_RESOURCE : 0);
        td.MiscFlags = t->pass == kPassMips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
        ID3D11Texture2D* rt = nullptr;
        ID3D11RenderTargetView* rtv = nullptr;
        if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &rt)) || FAILED(g.dev->CreateRenderTargetView(rt, nullptr, &rtv)))
          fail("cannot create the render target for " + t->name);
        rts.push_back(rt);
        rtvs.push_back(rtv);
      }
      if (t->pass == kPassMips && FAILED(g.dev->CreateShaderResourceView(rts[0], nullptr, &srv)))
        fail("cannot create the view for " + t->name);
      auto makePs = [&](const std::string& src, ID3D11PixelShader** out) {
        ID3DBlob* code = compile(src, t->name.c_str(), "ps_5_0");
        if (FAILED(g.dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, out)))
          fail("cannot create " + t->name);
        code->Release();
      };
      if (t->pass != kPassClear && t->pass != kPassMips) makePs(passSource(*t), &ps);
      D3D11_VIEWPORT vp = {0.0f, 0.0f, float(kRtW), float(kRtH), 0.0f, 1.0f};
      g.ctx->RSSetViewports(1, &vp);
      if (t->pass == kPassStencil) {
        // The stencil buffer: 1 in half the 8 x 8 tiles, drawn once; the heavy pass then runs where it is 1.
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = kRtW;
        td.Height = kRtH;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &ds)) || FAILED(g.dev->CreateDepthStencilView(ds, nullptr, &dsv)))
          fail("cannot create the stencil buffer for " + t->name);
        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable = FALSE;
        dd.StencilEnable = TRUE;
        dd.StencilReadMask = dd.StencilWriteMask = 0xFF;
        dd.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS};
        dd.BackFace = dd.FrontFace;
        if (FAILED(g.dev->CreateDepthStencilState(&dd, &dsMark))) fail("cannot create a stencil state");
        dd.StencilWriteMask = 0;
        dd.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_EQUAL};
        dd.BackFace = dd.FrontFace;
        if (FAILED(g.dev->CreateDepthStencilState(&dd, &dsTest))) fail("cannot create a stencil state");
        makePs(kStencilMark, &psMark);
        g.ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_STENCIL, 1.0f, 0);
        g.ctx->OMSetRenderTargets(0, nullptr, dsv);
        g.ctx->OMSetDepthStencilState(dsMark, 1);
        g.ctx->PSSetShader(psMark, nullptr, 0);
        g.ctx->Draw(3, 0);
        g.ctx->OMSetDepthStencilState(dsTest, 1);
      }
      if (ps) g.ctx->PSSetShader(ps, nullptr, 0);
      {
        // The heavy shader's constants (from the constant buffer, so fxc cannot fold the loop).
        CbData cd = {};
        for (int r = 0; r < 16; ++r) cd.U[r][0] = 1.7f + 0.001f * float(r), cd.U[r][1] = 0.3f;
        g.ctx->UpdateSubresource(g.cb, 0, nullptr, &cd, 0, 0);
      }
      const float clearColor[4] = {0.1f, 0.2f, 0.3f, 1.0f};
      auto unit = [&](UINT k) {
        if (t->pass == kPassClear) g.ctx->ClearRenderTargetView(rtvs[0], clearColor);
        else if (t->pass == kPassMips) g.ctx->GenerateMips(srv);
        else {
          g.ctx->OMSetRenderTargets(UINT(count), &rtvs[size_t((k & 1) * count)], dsv);
          g.ctx->Draw(3, 0);
        }
      };
      auto timeUnits = [&](UINT n) {
        return g.timer.time(g.ctx, [&] {
          for (UINT k = 0; k < n; ++k) unit(k);
        });
      };
      UINT n = 1;
      while (n < 4096 && timeUnits(n) < 2.0) n *= 2;
      std::vector<double>& rd = passReadings[t->name];
      for (int pass = 0; pass < kMaxPasses && (pass < 2 || !consensus(rd, 0.0).ok); ++pass) {
        std::vector<double> runs;
        for (int k = 0; k < reps * 3 && int(runs.size()) < reps; ++k)
          if (const double m = timeUnits(n); m > 0.0) runs.push_back(m / n);
        rd.push_back(median(runs));
        progress.step();
      }
      passMs[t->name] = consensus(rd, 0.0);
      g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
      g.ctx->OMSetDepthStencilState(nullptr, 0);
      release(ps);
      release(psMark);
      release(dsTest);
      release(dsMark);
      release(dsv);
      release(ds);
      release(srv);
      for (auto*& v : rtvs) release(v);
      for (auto*& r : rts) release(r);
      continue;
    }
    if (t->stage == Stage::Blend) {
      // Two 3840 x 2160 targets (see blendPass) and a noise texture the shader version reads.
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = kRtW;
      td.Height = kRtH;
      td.MipLevels = td.ArraySize = 1;
      td.Format = t->format->dxgi;
      td.SampleDesc.Count = 1;
      td.BindFlags = D3D11_BIND_RENDER_TARGET;
      ID3D11Texture2D* rt[2] = {nullptr, nullptr};
      ID3D11RenderTargetView* rtv[2] = {nullptr, nullptr};
      for (int r = 0; r < 2; ++r)
        if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &rt[r])) || FAILED(g.dev->CreateRenderTargetView(rt[r], nullptr, &rtv[r])))
          fail("cannot create the render target for " + t->name);
      td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      td.Usage = D3D11_USAGE_IMMUTABLE;
      const auto data = texelData(*t->format, size_t(kRtW) * kRtH, rng);
      D3D11_SUBRESOURCE_DATA init = {data.data(), kRtW * UINT(t->format->bytes), 0};
      ID3D11Texture2D* content = nullptr;
      ID3D11ShaderResourceView* srv = nullptr;
      if (FAILED(g.dev->CreateTexture2D(&td, &init, &content)) ||
          FAILED(g.dev->CreateShaderResourceView(content, nullptr, &srv)))
        fail("cannot create the content texture for " + t->name);
      ID3D11BlendState* bs = nullptr;
      if (t->blend != 0 && !t->shaderBlend) {
        D3D11_BLEND_DESC bd = {};
        D3D11_RENDER_TARGET_BLEND_DESC& b = bd.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        b.BlendOp = b.BlendOpAlpha = t->blend == 4 ? D3D11_BLEND_OP_MIN : D3D11_BLEND_OP_ADD;
        b.SrcBlend = b.SrcBlendAlpha = b.DestBlend = b.DestBlendAlpha = D3D11_BLEND_ONE;
        if (t->blend == 2) {
          b.SrcBlend = b.SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
          b.DestBlend = b.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        } else if (t->blend == 3) {
          b.SrcBlend = D3D11_BLEND_DEST_COLOR;
          b.SrcBlendAlpha = D3D11_BLEND_DEST_ALPHA;
          b.DestBlend = b.DestBlendAlpha = D3D11_BLEND_ZERO;
        }
        if (FAILED(g.dev->CreateBlendState(&bd, &bs))) fail("cannot create the blend state for " + t->name);
      }
      ID3DBlob* code = compile(blendSource(t->blend, t->shaderBlend), t->name.c_str(), "ps_5_0");
      ID3D11PixelShader* ps = nullptr;
      g.dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps);
      code->Release();
      code = compile(blendSource(0, false), "restore", "ps_5_0");
      ID3D11PixelShader* restore = nullptr;
      g.dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &restore);
      code->Release();
      UINT draws = 0;
      std::vector<double>& rd = blendReadings[t->name];
      for (int pass = 0; pass < kMaxPasses && (pass < 2 || !consensus(rd, 0.0).ok); ++pass) {
        rd.push_back(blendPass(g, rtv, restore, ps, srv, bs, reps, draws));
        progress.step();
      }
      blendMs[t->name] = consensus(rd, 0.0);
      ID3D11RenderTargetView* nullRtv = nullptr;
      g.ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
      release(ps);
      release(restore);
      release(bs);
      release(srv);
      release(content);
      for (int r = 0; r < 2; ++r) release(rtv[r]), release(rt[r]);
      continue;
    }
    if (t->stage == Stage::Write) {
      // Two 3840 x 2160 targets of the format (see writePass); GB/s from the pass time.
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = kRtW;
      td.Height = kRtH;
      td.MipLevels = td.ArraySize = 1;
      td.Format = t->format->dxgi;
      td.SampleDesc.Count = 1;
      td.BindFlags = D3D11_BIND_RENDER_TARGET;
      ID3D11Texture2D* rt[2] = {nullptr, nullptr};
      ID3D11RenderTargetView* rtv[2] = {nullptr, nullptr};
      for (int r = 0; r < 2; ++r)
        if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &rt[r])) || FAILED(g.dev->CreateRenderTargetView(rt[r], nullptr, &rtv[r])))
          fail("cannot create the render target for " + t->name);
      ID3DBlob* code = compile(writeSource(t->intTex), t->name.c_str(), "ps_5_0");
      ID3D11PixelShader* ps = nullptr;
      g.dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps);
      code->Release();
      CbData cd = {};
      cd.U[0][0] = float(t->write);
      g.ctx->UpdateSubresource(g.cb, 0, nullptr, &cd, 0, 0);
      UINT draws = 0;
      std::vector<double>& rd = writeReadings[t->name];
      for (int pass = 0; pass < kMaxPasses && (pass < 2 || !consensus(rd, 0.0).ok); ++pass) {
        const double ms = writePass(g, rtv, ps, reps, draws);
        rd.push_back(double(kRtW) * kRtH * t->format->bytes / (ms * 1e6));  // GB/s
        progress.step();
      }
      writeUnits[t->name] = consensus(rd, 0.0);
      writeMs[t->name] = double(kRtW) * kRtH * t->format->bytes / (writeUnits[t->name].value * 1e6);
      ID3D11RenderTargetView* nullRtv = nullptr;
      g.ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
      release(ps);
      for (int r = 0; r < 2; ++r) release(rtv[r]), release(rt[r]);
      continue;
    }
    Bound b = bindResources(g, *t, rng);
    Bound none = bindResources(g, *byName(t->stage == Stage::Pixel ? "ps.mad" : "mad"), rng);
    const Test& ref = *byName(t->stage == Stage::Pixel ? "ps.mad" : "mad");
    if (t->stage == Stage::Pixel) {
      g.ctx->OMSetRenderTargets(1, &g.psRtv, nullptr);
      D3D11_VIEWPORT vp = {0.0f, 0.0f, float(kPsSize), float(kPsSize), 0.0f, 1.0f};
      g.ctx->RSSetViewports(1, &vp);
    }
    for (const Config& c : kConfigs) {
      if (!kernels[t->name].count(c.name)) continue;
      const Kernel& k = kernels[t->name][c.name];
      const Kernel& rk = kernels[ref.name][c.name];
      if (!iters[c.name].count(ref.name)) {
        bind(g, ref, none, rk);
        iters[c.name][ref.name] = calibrate(g, ref, rk, c.groups);
      }
      bind(g, *t, b, k);
      iters[c.name][t->name] = calibrate(g, *t, k, c.groups);
      Measured& x = results[c.name][t->name];
      Result sum;
      for (int pass = 0; pass < kMaxPasses && (pass < 2 || !consensus(x.readings).ok); ++pass) {
        bind(g, ref, none, rk);
        const Result m = measureAt(g, ref, rk, c, iters[c.name][ref.name], reps);
        bind(g, *t, b, k);
        const Result r = t == &ref ? m : measureAt(g, *t, k, c, iters[c.name][t->name], reps);
        if (std::strcmp(c.name, "tput") == 0) refs.push_back(m.nsPerStep);
        x.readings.push_back(4.0 * r.nsPerStep / m.nsPerStep);
        sum.iters = r.iters;
        sum.ms += r.ms;
        sum.nsPerStep += r.nsPerStep;
        progress.step();
      }
      x.r = sum;
      x.r.ms /= double(x.readings.size());
      x.r.nsPerStep /= double(x.readings.size());
      x.units = consensus(x.readings);
    }
    ID3D11ShaderResourceView* nullViews[4] = {nullptr, nullptr, nullptr, nullptr};
    g.ctx->CSSetShaderResources(0, 4, nullViews);
    g.ctx->PSSetShaderResources(0, 4, nullViews);
    if (t->stage == Stage::Pixel) {
      ID3D11RenderTargetView* nullRtv = nullptr;
      g.ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
    }
    b.free();
    none.free();
  }
  // Costs over the bases (bases come first, so they are known).
  for (auto& [cname, byTest] : results)
    for (const Test* t : tests) {
      if (!byTest.count(t->name)) continue;
      Measured& x = byTest[t->name];
      x.vsBase = t->base.empty() || !byTest.count(t->base) ? x.units.value : x.units.value - byTest[t->base].units.value;
    }
  const auto [lo, hi] = std::minmax_element(refs.begin(), refs.end());
  const double drift = refs.empty() ? 0.0 : 100.0 * (*hi - *lo) / median(refs);
  std::printf("\n   reference drift %.1f%%%s\n", drift, drift > 5.0 ? " (the GPU clock moved)" : "");

  // CSV
  FILE* csv = std::fopen(outPath.c_str(), "wb");
  if (!csv) fail("cannot write " + outPath);
  std::fprintf(csv, "# TexBench %s\n# gpu: %s\n# vendor: 0x%04X\n# device: 0x%04X\n# driver: %s\n# reference drift: %.2f%%\n",
               SOPT_VERSION, ad.name.c_str(), ad.desc.VendorId, ad.desc.DeviceId, ad.driver.c_str(), drift);
  std::fprintf(csv, "config,test,section,base,iters,ms,ns_per_step,units,units_vs_base,passes,consensus,readings,step,note\n");
  auto readingsText = [](const std::vector<double>& v) {
    std::string s;
    for (double r : v) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%s%.3f", s.empty() ? "" : ";", r);
      s += buf;
    }
    return s;
  };
  for (const Config& c : kConfigs)
    for (const Test* t : tests) {
      if (!results[c.name].count(t->name)) continue;
      const Measured& x = results[c.name][t->name];
      std::fprintf(csv, "%s,\"%s\",\"%s\",%s,%u,%.4f,%.6f,%.3f,%.3f,%zu,%s,%s,\"%s\",\"%s\"\n", c.name, t->name.c_str(),
                   t->section.c_str(), t->base.c_str(), x.r.iters, x.r.ms, x.r.nsPerStep, x.units.value, x.vsBase,
                   x.readings.size(), x.units.ok ? "yes" : "no", readingsText(x.readings).c_str(), t->step.c_str(),
                   t->note.c_str());
    }
  for (const Test* t : tests)
    if (t->stage == Stage::Write)
      std::fprintf(csv, "write,\"%s\",\"%s\",,,%.4f,,%.3f,,%zu,%s,%s,\"GB/s, %u x %u\",\"\"\n", t->name.c_str(),
                   t->section.c_str(), writeMs[t->name], writeUnits[t->name].value, writeReadings[t->name].size(),
                   writeUnits[t->name].ok ? "yes" : "no", readingsText(writeReadings[t->name]).c_str(), kRtW, kRtH);
  for (const Test* t : tests)
    if (t->stage == Stage::Pass)
      std::fprintf(csv, "pass,\"%s\",\"%s\",,,%.4f,,%.4f,,%zu,%s,%s,\"ms per pass, %u x %u\",\"%s\"\n", t->name.c_str(),
                   t->section.c_str(), passMs[t->name].value, passMs[t->name].value, passReadings[t->name].size(),
                   passMs[t->name].ok ? "yes" : "no", readingsText(passReadings[t->name]).c_str(), kRtW, kRtH, t->note.c_str());
  for (const Test* t : tests)
    if (t->stage == Stage::Blend)
      std::fprintf(csv, "blend,\"%s\",\"%s\",,,%.4f,,%.4f,,%zu,%s,%s,\"ms per pass, %u x %u\",\"\"\n", t->name.c_str(),
                   t->section.c_str(), blendMs[t->name].value, blendMs[t->name].value, blendReadings[t->name].size(),
                   blendMs[t->name].ok ? "yes" : "no", readingsText(blendReadings[t->name]).c_str(), kRtW, kRtH);
  std::fclose(csv);

  // Summary
  std::printf("\n");
  printBox(st, std::string("TexBench ") + SOPT_VERSION + "  -  " + ad.name);
  std::printf("  driver %s, vendor 0x%04X, device 0x%04X\n", ad.driver.c_str(), ad.desc.VendorId, ad.desc.DeviceId);
  // Lines fit the console: names up to 24 characters (longer ones cut), the graphs shrink when the
  // window is narrow (one character spare, or the console wraps the line).
  const int cols = consoleColumns();
  std::string section;
  std::vector<std::string> unstable;
  double maxV = 0.0;
  auto sectionMax = [&](const std::string& sec) {
    double m = 0.0;
    for (const Test* t : tests)
      if (t->section == sec && results["tput"].count(t->name)) m = std::max(m, results["tput"][t->name].vsBase);
    return m;
  };
  // The name column fits the longest name shown, so the graphs line up; the notes start where they do.
  int nameW = 18;
  for (const Test* t : tests)
    if (!t->section.empty() && (t->stage == Stage::Compute || t->stage == Stage::Pixel))
      nameW = std::min(24, std::max(nameW, int(t->name.size())));
  // Fixed columns: indent, name, cost, Ops, dep, lat, GB/s.
  const int kBarWidth = std::clamp(cols - 1 - (2 + nameW + 1 + 7 + 2 + 2 + 6 + 1 + 6 + 1 + 6 + 8), 8, 24);
  auto shortName = [&](const std::string& n) { return int(n.size()) > nameW ? n.substr(0, size_t(nameW - 1)) + "~" : n; };
  const std::string graphIndent(size_t(2 + nameW + 1 + 7 + 2), ' ');
  bool rows = false;  // rows printed since the last "(shorter is better)" note
  auto betterNote = [&](const char* text) {
    if (rows) std::printf("%s%s(%s is better)%s\n", graphIndent.c_str(), st.c("\x1b[90m"), text, st.reset());
    rows = false;
  };
  for (const Test* t : tests) {
    if (t->section.empty() || t->stage == Stage::Write || t->stage == Stage::Blend || t->stage == Stage::Pass ||
        !results["tput"].count(t->name))
      continue;
    if (t->section != section) {
      betterNote("shorter");
      section = t->section;
      maxV = sectionMax(section);
      bool bytes = false;
      for (const Test* u : tests) bytes |= u->section == section && u->perByte;
      std::printf("\n  %s%s%s\n  %s%-*s %7s  %-*s  %6s %6s %6s%s%s\n", st.c("\x1b[1;96m"), section.c_str(), st.reset(),
                  st.c("\x1b[90m"), nameW, "Test", "Cost", kBarWidth, "(throughput)", "Ops", "dep", "lat",
                  bytes ? "    GB/s" : "", st.reset());
    }
    rows = true;
    const Measured& x = results["tput"][t->name];
    Shade shade;
    costComment(x.vsBase, &shade);
    const bool shaky = !x.units.ok;
    if (shaky) unstable.push_back(t->name);
    // A number in a fixed width: one decimal while it fits, none for large values (random writes reach
    // 10000+), so the columns never shift.
    auto num = [](double v, int width) {
      char buf[32];
      if (std::fabs(v) < 0.05) v = 0.0;  // no "-0.0"
      std::snprintf(buf, sizeof(buf), "%*.1f", width, v);
      if (int(std::strlen(buf)) > width) std::snprintf(buf, sizeof(buf), "%*.0f", width, v);
      return std::string(buf);
    };
    auto other = [&](const char* cfg) {
      if (!results[cfg].count(t->name)) return std::string("     -");
      return num(results[cfg][t->name].vsBase, 6);
    };
    // GB/s: the texel data the reads deliver per second at full throughput (the texel's bytes over the
    // read's own time: its cost in fma times the reference fma's time per step).
    char perByte[24] = "";
    const double refNs = x.units.value > 0.0 ? x.r.nsPerStep * 4.0 / x.units.value : 0.0;
    if (t->perByte && t->format && x.vsBase > 0.05 && refNs > 0.0)
      std::snprintf(perByte, sizeof(perByte), "  %6.0f", t->format->bytes / (x.vsBase / 4.0 * refNs));
    else if (t->perByte) std::snprintf(perByte, sizeof(perByte), "  %6s", "-");
    std::printf("  %-*s %s  %s  %s %s %s%s%s\n", nameW, shortName(t->name).c_str(), num(x.vsBase, 7).c_str(),
                bar(x.vsBase, maxV, kBarWidth, st, shade).c_str(), num(x.vsBase / 4.0, 6).c_str(), other("dep").c_str(),
                other("lat").c_str(), perByte, shaky ? (st.vt ? "  \x1b[93m! no consensus\x1b[0m" : "  ! no consensus") : "");
  }
  betterNote("shorter");
  bool anyWrite = false;
  double maxW = 0.0;
  for (const Test* t : tests)
    if (t->stage == Stage::Write) anyWrite = true, maxW = std::max(maxW, writeUnits[t->name].value);
  if (anyWrite) {
    // Noise cannot be compressed; Gain = how much faster the smooth gradient / the flat color writes.
    const int w = std::clamp((cols - 1 - (2 + 18 + 1 + 3 * 9 + 2 * 7 + 2)) / 3, 4, 8);
    std::printf("\n  %sRender target writes: full-screen passes into 3840 x 2160 (GB/s)%s\n"
                "  %s%-18s %7s  %-*s  %7s  %-*s  %7s  %-*s  %6s %6s%s\n",
                st.c("\x1b[1;96m"), st.reset(), st.c("\x1b[90m"), "Format", "Noise", w, "", "Smooth", w, "", "Flat", w, "",
                "Gain", "Gain", st.reset());
    for (const Format& f : kFormats) {
      auto get = [&](const char* kind) {
        const std::string name = std::string(f.name) + " write " + kind;
        return writeUnits.count(name) ? writeUnits[name].value : -1.0;
      };
      const double n = get("noise"), sm = get("smooth"), fl = get("flat");
      if (n < 0.0 && sm < 0.0 && fl < 0.0) continue;
      auto col = [&](double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%7.1f", std::max(v, 0.0));
        return std::string(buf) + "  " + bar(std::max(v, 0.0), maxW, w, st, kCyan);
      };
      auto gain = [&](double v) {
        char buf[16];
        if (n > 0.0 && v >= 0.0) std::snprintf(buf, sizeof(buf), "%5.2fx", v / n);
        else std::snprintf(buf, sizeof(buf), "%6s", "-");
        return std::string(buf);
      };
      std::printf("  %-18s %s  %s  %s  %s %s\n", f.name, col(n).c_str(), col(sm).c_str(), col(fl).c_str(),
                  gain(sm).c_str(), gain(fl).c_str());
    }
    std::printf("%*s%s(longer is better)%s\n", 2 + 18 + 1 + 7 + 2, "", st.c("\x1b[90m"), st.reset());
  }
  bool anyBlend = false;
  for (const Test* t : tests) anyBlend |= t->stage == Stage::Blend;
  if (anyBlend) {
    // ms per pass; of each blend / shader pair the faster one in green.
    std::printf("\n  %sBlending: full-screen passes into 3840 x 2160 (ms per pass)%s\n"
                "  %s%-10s %7s   %-15s   %-15s   %-15s   %-15s%s\n"
                "  %s%-10s %7s   %7s %7s   %7s %7s   %7s %7s   %7s %7s%s\n",
                st.c("\x1b[1;96m"), st.reset(), st.c("\x1b[90m"), "Format", "Plain", "Add", "Lerp (alpha)", "Multiply",
                "Min", st.reset(), st.c("\x1b[90m"), "", "", "blend", "shader", "blend", "shader", "blend", "shader",
                "blend", "shader", st.reset());
    for (const char* fn : {"RGBA8", "RGB10A2", "RG11B10F", "RGBA16F", "RGBA32F"}) {
      auto get = [&](const std::string& name) { return blendMs.count(name) ? blendMs[name].value : -1.0; };
      const double plain = get(std::string(fn) + " plain");
      std::string line;
      bool any = plain >= 0.0;
      for (const char* op : {"add", "lerp", "multiply", "min"}) {
        const double b = get(std::string(fn) + " " + op + " blend"), sh = get(std::string(fn) + " " + op + " shader");
        any |= b >= 0.0 || sh >= 0.0;
        auto cell = [&](double v, double other) {
          char buf[48];
          if (v < 0.0) return std::string("      -");
          const bool win = other >= 0.0 && v < other;
          std::snprintf(buf, sizeof(buf), "%s%7.3f%s", win ? st.c("\x1b[92m") : "", v, win ? st.reset() : "");
          return std::string(buf);
        };
        line += "   " + cell(b, sh) + " " + cell(sh, b);
      }
      if (!any) continue;
      char head[64];
      if (plain >= 0.0) std::snprintf(head, sizeof(head), "  %-10s %7.3f", fn, plain);
      else std::snprintf(head, sizeof(head), "  %-10s %7s", fn, "-");
      std::printf("%s%s\n", head, line.c_str());
    }
    std::printf("%*s%s(lower is better)%s\n", 2 + 10 + 1, "", st.c("\x1b[90m"), st.reset());
  }
  double maxP = 0.0;
  for (const Test* t : tests)
    if (t->stage == Stage::Pass) maxP = std::max(maxP, passMs[t->name].value);
  if (maxP > 0.0) {
    std::printf("\n  %sPass states: full-screen passes into 3840 x 2160%s\n  %s%-26s %8s  %-*s%s\n", st.c("\x1b[1;96m"),
                st.reset(), st.c("\x1b[90m"), "Test", "ms/pass", kBarWidth, "", st.reset());
    for (const Test* t : tests)
      if (t->stage == Stage::Pass)
        std::printf("  %-26s %8.3f  %s\n", t->name.c_str(), passMs[t->name].value,
                    bar(passMs[t->name].value, maxP, kBarWidth, st, kCyan).c_str());
    std::printf("%*s%s(shorter is better)%s\n", 2 + 26 + 1 + 8 + 2, "", st.c("\x1b[90m"), st.reset());
  }
  // How to read the summary, for people who are not programmers (owner's wording review, 2026-10-04).
  std::printf("\n  %sHow to read this%s\n"
              "  Cost     How long the operation takes, compared with the simplest thing a GPU does:\n"
              "           a multiply-add, which counts as 4. The rest of the test is already subtracted.\n"
              "  Ops      Operations: the cost counted in multiply-adds.\n"
              "           10.0 means \"takes as long as ten multiply-adds\".\n"
              "  dep      Dependent: the cost when every step has to wait for the result of the one\n"
              "           before it.\n"
              "  lat      Latency: the waiting time alone, one step at a time with nothing else to do\n"
              "           meanwhile.\n"
              "  GB/s     Gigabytes per second: how much data is read or written (more is better).\n"
              "  ms/pass  Milliseconds for one full-screen pass at 3840 x 2160 (less is better).\n"
              "\n"
              "  TESTS.txt, next to this program, explains every test in plain words.\n",
              st.c("\x1b[1;96m"), st.reset());
  if (drift > 5.0)
    std::printf("\n  %sWarning:%s the reference changed by %.0f%% during the run: the GPU clock moved.\n", st.c("\x1b[1;93m"),
                st.reset(), drift);
  if (!unstable.empty()) {
    std::printf("\n  %sWarning:%s %zu test(s) without agreeing readings after %d passes:", st.c("\x1b[1;93m"), st.reset(),
                unstable.size(), kMaxPasses);
    for (const std::string& n : unstable) std::printf(" %s", n.c_str());
    std::printf("\n");
  }
  std::printf("\n  CSV:  %s\n  DXBC: %s\n", outPath.c_str(), dxbcDir.string().c_str());
  return 0;
}

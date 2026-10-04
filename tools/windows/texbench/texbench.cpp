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

const Format kFormats[] = {
    {"R8", DXGI_FORMAT_R8_UNORM, 1, 'u', 1},
    {"R16", DXGI_FORMAT_R16_UNORM, 2, 'u', 1},
    {"R16F", DXGI_FORMAT_R16_FLOAT, 2, 'h', 1},
    {"R32F", DXGI_FORMAT_R32_FLOAT, 4, 'f', 1},
    {"R32U", DXGI_FORMAT_R32_UINT, 4, 'i', 1},
    {"R32I", DXGI_FORMAT_R32_SINT, 4, 'i', 1},
    {"RG8", DXGI_FORMAT_R8G8_UNORM, 2, 'u', 2},
    {"RG16", DXGI_FORMAT_R16G16_UNORM, 4, 'u', 2},
    {"RG16F", DXGI_FORMAT_R16G16_FLOAT, 4, 'h', 2},
    {"RG32F", DXGI_FORMAT_R32G32_FLOAT, 8, 'f', 2},
    {"RGBA8", DXGI_FORMAT_R8G8B8A8_UNORM, 4, 'u', 4},
    {"RGBA8sRGB", DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4, 'u', 4},
    {"RGBA16", DXGI_FORMAT_R16G16B16A16_UNORM, 8, 'u', 4},
    {"RGBA16F", DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 'h', 4},
    {"RGBA32F", DXGI_FORMAT_R32G32B32A32_FLOAT, 16, 'f', 4},
    {"RGBA32U", DXGI_FORMAT_R32G32B32A32_UINT, 16, 'i', 4},
    {"RGBA32I", DXGI_FORMAT_R32G32B32A32_SINT, 16, 'i', 4},
    {"RGB10A2", DXGI_FORMAT_R10G10B10A2_UNORM, 4, 'p', 4},
    {"RG11B10F", DXGI_FORMAT_R11G11B10_FLOAT, 4, 'r', 3},
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

enum class Stage { Compute, Pixel, Write, Blend };
enum class Tex { None, Plain, Mipped, Lut1D, Lut3D };
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
  bool intTex = false;       // integer format: read through Texture2D<uint4>
  float scaleX = 1.0f, scaleY = 1.0f;  // pixel shader: texture coordinate scale (mip level, anisotropy)
  int write = 0;             // render target write: 0 smooth gradient, 1 noise (incompressible), 2 flat color
  UINT tileW = 8;            // compute: thread tile of a 64-thread group, tileW x (64 / tileW) pixels
  int blend = 0;             // blending test: 0 plain write, 1 add, 2 lerp (alpha), 3 multiply, 4 min
  bool shaderBlend = false;  // blending test: the shader reads the destination as a texture and does the math
  bool perByte = false;      // summary: show bytes per op (texel bytes / Ops)
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

  // Pixel shader: derivatives (quad operations) and sampling with automatic mip selection.
  const char* ps = "Pixel shader: derivatives and automatic mip selection";
  Test pmad = mad;
  pmad.name = "ps.mad";
  pmad.stage = Stage::Pixel;
  pmad.note = "reference in the pixel shader";
  add(pmad);
  for (const char* d : {"ddx", "ddy", "ddx_fine", "ddy_fine", "ddx_coarse", "fwidth"}) {
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
  psSample("Sample bilinear", Filter::Linear, 1.0f, 1.0f, "Sample (automatic mip level 0), RGBA8 1024 x 1024");
  psSample("Sample trilinear", Filter::Trilinear, 1.5f, 1.5f, "1.5 texels per pixel: between mip 0 and 1");
  psSample("Sample aniso 4:1", Filter::Aniso, 1.0f, 4.0f, "4 x 1 texel footprint, 16x anisotropic filtering");

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
  // Thread groups of 64 as tiles of tileW x (64 / tileW) pixels, laid out over 1024 x 1024 pixels.
  const std::string w = std::to_string(t.tileW), h = std::to_string(64 / t.tileW), row = std::to_string(1024 / t.tileW);
  s += "RWStructuredBuffer<float> O : register(u0);\n"
       "[numthreads(64, 1, 1)]\nvoid main(uint3 id : SV_DispatchThreadID)\n{\n"
       "  const uint g = id.x / 64u, l = id.x % 64u;\n"
       "  const float2 P = (float2((g % " + row + "u) * " + w + "u + l % " + w + "u, (g / " + row + "u) * " + h +
       "u + l / " + w + "u) + 0.5) * texel;\n";
  for (int k = 0; k < chains; ++k)
    s += "  float x" + std::to_string(k) + " = 0.3 + frac(id.x * 0.000123 + " + std::to_string(k) + " * 0.137) * seed;\n";
  s += chainBody(t, chains, "  ");
  s += "  O[id.x] = 0.0";
  for (int k = 0; k < chains; ++k) s += " + x" + std::to_string(k);
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
  ID3D11SamplerState* sampler = nullptr;
  void free() {
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
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxAnisotropy = t.filter == Filter::Aniso ? 16 : 1;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
  if (FAILED(g.dev->CreateSamplerState(&sd, &b.sampler))) fail("cannot create a sampler for " + t.name);
  if (t.tex == Tex::None) return b;

  const Format& f = *t.format;
  if (t.tex == Tex::Lut3D) {
    D3D11_TEXTURE3D_DESC td = {};
    td.Width = td.Height = td.Depth = t.size;
    td.MipLevels = 1;
    td.Format = f.dxgi;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const auto data = texelData(f, size_t(t.size) * t.size * t.size, rng);
    D3D11_SUBRESOURCE_DATA init = {data.data(), t.size * UINT(f.bytes), t.size * t.size * UINT(f.bytes)};
    ID3D11Texture3D* tex = nullptr;
    if (FAILED(g.dev->CreateTexture3D(&td, &init, &tex))) fail("cannot create the texture for " + t.name);
    b.tex = tex;
  } else {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = t.size;
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
  ID3D11ShaderResourceView* views[3] = {nullptr, nullptr, nullptr};
  if (b.srv) views[t.tex == Tex::Lut3D ? 2 : t.intTex ? 1 : 0] = b.srv;
  if (k.cs) {
    g.ctx->CSSetShader(k.cs, nullptr, 0);
    g.ctx->CSSetShaderResources(0, 3, views);
    g.ctx->CSSetSamplers(0, 1, &b.sampler);
  } else {
    g.ctx->PSSetShader(k.ps, nullptr, 0);
    g.ctx->PSSetShaderResources(0, 3, views);
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
// takes >= 2 ms.
double writePass(Gpu& g, ID3D11RenderTargetView* rtv, ID3D11PixelShader* ps, int reps, UINT& draws) {
  g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
  D3D11_VIEWPORT vp = {0.0f, 0.0f, float(kRtW), float(kRtH), 0.0f, 1.0f};
  g.ctx->RSSetViewports(1, &vp);
  g.ctx->PSSetShader(ps, nullptr, 0);
  auto timeDraws = [&](UINT n) {
    return g.timer.time(g.ctx, [&] {
      for (UINT k = 0; k < n; ++k) g.ctx->Draw(3, 0);
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

// Blending: the median time of one pass (in ms) over reps readings; each reading times n x (copy + draw)
// and, right before, n copies alone, and takes the difference.
double blendPass(Gpu& g, ID3D11RenderTargetView* rtv, ID3D11Texture2D* rt, ID3D11Texture2D* content,
                 ID3D11ShaderResourceView* srv, ID3D11PixelShader* ps, ID3D11BlendState* bs, int reps, UINT& draws) {
  ID3D11ShaderResourceView* nullSrv = nullptr;
  g.ctx->PSSetShaderResources(0, 1, &nullSrv);
  g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
  const float factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  g.ctx->OMSetBlendState(bs, factor, 0xffffffffu);
  g.ctx->PSSetShaderResources(0, 1, &srv);
  D3D11_VIEWPORT vp = {0.0f, 0.0f, float(kRtW), float(kRtH), 0.0f, 1.0f};
  g.ctx->RSSetViewports(1, &vp);
  g.ctx->PSSetShader(ps, nullptr, 0);
  auto timeUnits = [&](UINT n, bool draw) {
    return g.timer.time(g.ctx, [&] {
      for (UINT k = 0; k < n; ++k) {
        g.ctx->CopyResource(rt, content);
        if (draw) g.ctx->Draw(3, 0);
      }
    });
  };
  if (draws == 0) {
    draws = 1;
    while (draws < 4096 && timeUnits(draws, true) < 2.0) draws *= 2;
  }
  std::vector<double> runs;
  for (int n = 0; n < reps * 3 && int(runs.size()) < reps; ++n) {
    const double copy = timeUnits(draws, false), unit = timeUnits(draws, true);
    if (copy > 0.0 && unit > 0.0) runs.push_back((unit - copy) / draws);
  }
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
  for (const Test& t : all)
    if (filter.empty() || t.name.find(filter) != std::string::npos || t.section.empty() || t.name == "mad" || t.name == "ps.mad")
      tests.push_back(&t);
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
    if (t->stage == Stage::Write || t->stage == Stage::Blend) continue;
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
  for (const Test* t : tests) {
    if (t->stage == Stage::Blend) {
      // A 3840 x 2160 target, its content (noise) in a second texture that restores it before every pass.
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = kRtW;
      td.Height = kRtH;
      td.MipLevels = td.ArraySize = 1;
      td.Format = t->format->dxgi;
      td.SampleDesc.Count = 1;
      td.BindFlags = D3D11_BIND_RENDER_TARGET;
      ID3D11Texture2D* rt = nullptr;
      ID3D11RenderTargetView* rtv = nullptr;
      if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &rt)) || FAILED(g.dev->CreateRenderTargetView(rt, nullptr, &rtv)))
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
      UINT draws = 0;
      std::vector<double>& rd = blendReadings[t->name];
      for (int pass = 0; pass < kMaxPasses && (pass < 2 || !consensus(rd, 0.0).ok); ++pass) {
        rd.push_back(blendPass(g, rtv, rt, content, srv, ps, bs, reps, draws));
        progress.step();
      }
      blendMs[t->name] = consensus(rd, 0.0);
      ID3D11RenderTargetView* nullRtv = nullptr;
      g.ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
      release(ps);
      release(bs);
      release(srv);
      release(content);
      release(rtv);
      release(rt);
      continue;
    }
    if (t->stage == Stage::Write) {
      // A 3840 x 2160 target of the format; GB/s from the pass time.
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = kRtW;
      td.Height = kRtH;
      td.MipLevels = td.ArraySize = 1;
      td.Format = t->format->dxgi;
      td.SampleDesc.Count = 1;
      td.BindFlags = D3D11_BIND_RENDER_TARGET;
      ID3D11Texture2D* rt = nullptr;
      ID3D11RenderTargetView* rtv = nullptr;
      if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &rt)) || FAILED(g.dev->CreateRenderTargetView(rt, nullptr, &rtv)))
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
      release(rtv);
      release(rt);
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
    ID3D11ShaderResourceView* nullViews[3] = {nullptr, nullptr, nullptr};
    g.ctx->CSSetShaderResources(0, 3, nullViews);
    g.ctx->PSSetShaderResources(0, 3, nullViews);
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
      std::fprintf(csv, "%s,%s,\"%s\",%s,%u,%.4f,%.6f,%.3f,%.3f,%zu,%s,%s,\"%s\",\"%s\"\n", c.name, t->name.c_str(),
                   t->section.c_str(), t->base.c_str(), x.r.iters, x.r.ms, x.r.nsPerStep, x.units.value, x.vsBase,
                   x.readings.size(), x.units.ok ? "yes" : "no", readingsText(x.readings).c_str(), t->step.c_str(),
                   t->note.c_str());
    }
  for (const Test* t : tests)
    if (t->stage == Stage::Write)
      std::fprintf(csv, "write,%s,\"%s\",,,%.4f,,%.3f,,%zu,%s,%s,\"GB/s, %u x %u\",\"\"\n", t->name.c_str(),
                   t->section.c_str(), writeMs[t->name], writeUnits[t->name].value, writeReadings[t->name].size(),
                   writeUnits[t->name].ok ? "yes" : "no", readingsText(writeReadings[t->name]).c_str(), kRtW, kRtH);
  for (const Test* t : tests)
    if (t->stage == Stage::Blend)
      std::fprintf(csv, "blend,%s,\"%s\",,,%.4f,,%.4f,,%zu,%s,%s,\"ms per pass, %u x %u\",\"\"\n", t->name.c_str(),
                   t->section.c_str(), blendMs[t->name].value, blendMs[t->name].value, blendReadings[t->name].size(),
                   blendMs[t->name].ok ? "yes" : "no", readingsText(blendReadings[t->name]).c_str(), kRtW, kRtH);
  std::fclose(csv);

  // Summary
  std::printf("\n");
  printBox(st, std::string("TexBench ") + SOPT_VERSION + "  -  " + ad.name);
  std::printf("  driver %s, vendor 0x%04X, device 0x%04X\n", ad.driver.c_str(), ad.desc.VendorId, ad.desc.DeviceId);
  constexpr int kBarWidth = 24;
  std::string section;
  std::vector<std::string> unstable;
  double maxV = 0.0;
  auto sectionMax = [&](const std::string& sec) {
    double m = 0.0;
    for (const Test* t : tests)
      if (t->section == sec && results["tput"].count(t->name)) m = std::max(m, results["tput"][t->name].vsBase);
    return m;
  };
  for (const Test* t : tests) {
    if (t->section.empty() || t->stage == Stage::Write || t->stage == Stage::Blend || !results["tput"].count(t->name))
      continue;
    if (t->section != section) {
      section = t->section;
      maxV = sectionMax(section);
      bool bytes = false;
      for (const Test* u : tests) bytes |= u->section == section && u->perByte;
      std::printf("\n  %s%s%s\n  %s%-18s %7s  %-*s  %6s %6s %6s%s%s\n", st.c("\x1b[1;96m"), section.c_str(), st.reset(),
                  st.c("\x1b[90m"), "Test", "Cost", kBarWidth, "(throughput)", "Ops", "dep", "lat",
                  bytes ? "   B/op" : "", st.reset());
    }
    const Measured& x = results["tput"][t->name];
    Shade shade;
    costComment(x.vsBase, &shade);
    const bool shaky = !x.units.ok;
    if (shaky) unstable.push_back(t->name);
    auto other = [&](const char* cfg) {
      char buf[16];
      if (!results[cfg].count(t->name)) return std::string("     -");
      std::snprintf(buf, sizeof(buf), "%6.1f", results[cfg][t->name].vsBase);
      return std::string(buf);
    };
    // Bytes per op: the texel's bytes over the read's cost in fma times (more = more data for the time).
    char perByte[16] = "";
    if (t->perByte && t->format && x.vsBase > 0.05) std::snprintf(perByte, sizeof(perByte), "  %5.2f", t->format->bytes / (x.vsBase / 4.0));
    else if (t->perByte) std::snprintf(perByte, sizeof(perByte), "  %5s", "-");
    std::printf("  %-18s %7.1f  %s  %6.1f %s %s%s%s\n", t->name.c_str(), std::fabs(x.vsBase) < 0.05 ? 0.0 : x.vsBase,
                bar(x.vsBase, maxV, kBarWidth, st, shade).c_str(), x.vsBase / 4.0, other("dep").c_str(),
                other("lat").c_str(), perByte, shaky ? (st.vt ? "  \x1b[93m! no consensus\x1b[0m" : "  ! no consensus") : "");
  }
  bool anyWrite = false;
  double maxW = 0.0;
  for (const Test* t : tests)
    if (t->stage == Stage::Write) anyWrite = true, maxW = std::max(maxW, writeUnits[t->name].value);
  if (anyWrite) {
    // Noise cannot be compressed; Gain = how much faster the smooth gradient / the flat color writes.
    constexpr int w = 8;
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
  }
  std::printf("\n  Cost = extra over the test's base in sopt units (4 = one fma, measured right before); Ops = Cost / 4;\n"
              "  dep / lat = the same cost with one chain per thread / one thread group. What each test measures:\n"
              "  TESTS.txt next to this program.\n");
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

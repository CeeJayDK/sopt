// sopt-opbench: measures what single GPU instructions and instruction patterns cost on the
// GPU in this machine (owner's idea, 2026-10-01), to calibrate sopt's cost models.
//
//   sopt-opbench [--adapter N] [--list] [--filter text] [--reps N] [--out file.csv] [--groups N]
//
// Every test is one HLSL step x = f(x, c) repeated in long dependent chains in a D3D11 compute
// shader (compiled at run time with Microsoft's D3DCompile -O3, as ReShade does on D3D9-12; the
// driver then compiles the DXBC for the GPU). The constants c come from a constant buffer and
// change with every unrolled step, so neither compiler can fold or reassociate the chain. Each
// step ends in mad(y, c.x, c.y), which keeps the values in range; the plain step
// x = mad(x, c.x, c.y) is the reference, and a test's cost is its time minus its base test's
// time per step, in sopt's units (4 = the reference mad, i.e. one fma).
//
// Three configurations:
//   tput  8 independent chains per thread, 1M threads: throughput (the cost model's question)
//   dep   1 chain per thread, 1M threads: dependent issue with full occupancy
//   lat   1 chain, one thread group: latency of a dependent step, relative to mad's
// Results go to the console and a CSV (default opbench-<gpu>.csv next to the exe); the DXBC
// disassembly of every test goes to opbench-dxbc\ so a folded test can be spotted.
//
// Version 2: every test is measured twice (forward, then backward through the list) with a fresh
// reference mad right before it, so a GPU clock change only affects the tests around it and shows
// as drift or as a disagreement between the two passes; the summary lists the throughput costs in
// a fixed order with a bar per test; the CSV has the GPU / driver once in '#' header lines.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// Laptops with switchable graphics: ask the NVIDIA (Optimus) and AMD (PowerXpress / Enduro)
// drivers for the discrete GPU instead of the integrated one.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

#ifndef SOPT_VERSION
#define SOPT_VERSION "dev"
#endif

namespace {

struct Test {
  const char* name;
  const char* step;  // HLSL expression of x (the chain value) and c (this step's float4 constants)
  float cx, cy, cz, cw;
  const char* base;  // the test whose time is subtracted
  const char* note;
};

// Constants keep every chain finite and away from denormals (x stays roughly in [0.3, 3]).
const Test kTests[] = {
    {"mad", "mad(x, c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, nullptr, "reference: one fma per step"},
    {"add", "mad(x + c.z, c.x, c.y)", 0.5f, 0.5f, 0.1f, 0.0f, "mad", ""},
    {"sub", "mad(c.z - x, c.x, c.y)", 0.5f, 1.0f, 2.0f, 0.0f, "mad", ""},
    {"mul", "mad(x * c.z, c.x, c.y)", 0.5f, 0.5f, 0.9f, 0.0f, "mad", ""},
    {"mad2", "mad(mad(x, c.z, c.w), c.x, c.y)", 0.5f, 0.5f, 0.9f, 0.1f, "mad", "a second mad"},
    {"min", "mad(min(x, c.z), c.x, c.y)", 0.5f, 0.5f, 1.2f, 0.0f, "mad", ""},
    {"max", "mad(max(x, c.z), c.x, c.y)", 0.5f, 0.5f, 0.8f, 0.0f, "mad", ""},
    {"neg", "mad(-x, c.x, c.y)", 0.5f, 1.5f, 0.0f, 0.0f, "mad", "source modifier"},
    {"abs", "mad(abs(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "source modifier"},
    {"negabs", "mad(-abs(x), c.x, c.y)", 0.5f, 1.5f, 0.0f, 0.0f, "mad", "source modifiers"},
    {"saturate", "mad(saturate(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", "output modifier of the previous mad"},
    {"clamp", "mad(clamp(x, c.z, c.w), c.x, c.y)", 0.5f, 0.5f, 0.8f, 1.2f, "mad", "AMD: v_med3"},
    {"floor", "mad(floor(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", ""},
    {"ceil", "mad(ceil(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", ""},
    {"round", "mad(round(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", ""},
    {"frac", "mad(frac(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", ""},
    {"sign", "mad(sign(x - c.z), c.x, c.y)", 0.5f, 1.0f, 1.1f, 0.0f, "sub", "sign of a difference"},
    {"step", "mad(step(c.z, x), c.x, c.y)", 0.5f, 1.0f, 1.2f, 0.0f, "mad", ""},
    {"select", "mad(x < c.z ? c.w : x, c.x, c.y)", 0.5f, 0.5f, 1.0f, 0.7f, "mad", "compare + cndmask"},
    {"lerp", "mad(lerp(c.z, x, c.w), c.x, c.y)", 0.5f, 0.5f, 0.9f, 0.6f, "mad", ""},
    {"rcp", "mad(rcp(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", ""},
    {"div", "mad(c.z / x, c.x, c.y)", 0.5f, 1.0f, 1.3f, 0.0f, "mad", "c / x"},
    {"divxy", "mad(x / (x + c.z), c.x, c.y)", 0.5f, 1.0f, 0.5f, 0.0f, "add", "x / y"},
    {"sqrt", "mad(sqrt(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", ""},
    {"rsqrt", "mad(rsqrt(x), c.x, c.y)", 0.5f, 0.5f, 0.0f, 0.0f, "mad", ""},
    {"exp2", "mad(exp2(x), c.x, c.y)", 0.25f, 0.0f, 0.0f, 0.0f, "mad", ""},
    {"log2", "mad(log2(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", ""},
    {"exp", "mad(exp(x), c.x, c.y)", 0.25f, 0.0f, 0.0f, 0.0f, "mad", "fxc: mul + exp2"},
    {"log", "mad(log(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", "fxc: log2 + mul"},
    {"pow", "mad(pow(x, c.z), c.x, c.y)", 0.5f, 0.5f, 1.3f, 0.0f, "mad", "fxc: log2, mul, exp2"},
    {"sin", "mad(sin(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", ""},
    {"cos", "mad(cos(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", ""},
    // Context effects (sopt's rdna3 model assumes them; compare with the plain forms above).
    {"rcpmax", "mad(max(rcp(x), c.z), c.x, c.y)", 0.5f, 0.5f, 0.1f, 0.0f, "mad", "base for the omod tests"},
    {"omod2", "mad(max(rcp(x) * 2.0, c.z), c.x, c.y)", 0.5f, 0.5f, 0.1f, 0.0f, "rcpmax", "AMD: output modifier, ~0"},
    {"omodhalf", "mad(max(rcp(x) * 0.5, c.z), c.x, c.y)", 0.5f, 1.0f, 0.1f, 0.0f, "rcpmax", "AMD: output modifier, ~0"},
    {"omod3", "mad(max(rcp(x) * 3.0, c.z), c.x, c.y)", 0.5f, 0.5f, 0.1f, 0.0f, "rcpmax", "control: not an omod scale"},
    {"max3", "mad(max(max(x, c.z), c.w), c.x, c.y)", 0.5f, 0.5f, 0.8f, 0.9f, "max", "AMD: v_max3, ~0"},
    {"minmax", "mad(min(max(x, c.z), c.w), c.x, c.y)", 0.5f, 0.5f, 0.8f, 1.2f, "max", "AMD: v_minmax / v_med3, ~0"},
    {"contract", "mad(x * c.z + c.w, c.x, c.y)", 0.5f, 0.5f, 0.9f, 0.1f, "mad", "mul + add: one fma like mad2?"},
    {"satmad", "mad(saturate(mad(x, c.z, c.w)), c.x, c.y)", 0.5f, 0.5f, 0.9f, 0.1f, "mad2", "saturate as output modifier, ~0"},
    // Fast forms of expensive ops (sign: no int -> float conversion; round: two precise adds).
    {"signsat", "mad(saturate((x - c.z) * 1e38) - saturate((x - c.z) * -1e38), c.x, c.y)", 0.5f, 1.0f, 1.1f, 0.0f, "sub",
     "exact sign: mul_sat + add"},
    {"signmad", "mad(mad(saturate(mad(x - c.z, 1e38, 0.5)), 2.0, -1.0), c.x, c.y)", 0.5f, 1.0f, 1.1f, 0.0f, "sub",
     "exact sign: mad_sat + mad"},
    {"signclamp", "mad(clamp((x - c.z) * 1e38, -1.0, 1.0), c.x, c.y)", 0.5f, 1.0f, 1.1f, 0.0f, "sub", "exact sign: mul, max, min"},
    {"signsel", "mad((x - c.z) > 0.0 ? 1.0 : ((x - c.z) < 0.0 ? -1.0 : 0.0), c.x, c.y)", 0.5f, 1.0f, 1.1f, 0.0f, "sub",
     "exact sign: lt, lt, and, movc"},
    {"signsel2", "mad((x - c.z) >= 0.0 ? 1.0 : -1.0, c.x, c.y)", 0.5f, 1.0f, 1.1f, 0.0f, "sub", "not sign (1 at 0): ge, movc"},
    {"roundadd", "mad(roundAdd(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", "round: (x + 1.5 * 2^23) - 1.5 * 2^23, precise"},
    {"flooradd", "mad(floorAdd(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", "floor: r - saturate((r - x) * 1e38), precise"},
    {"fracadd", "mad(fracAdd(x), c.x, c.y)", 0.5f, 1.0f, 0.0f, 0.0f, "mad", "frac: d + saturate(d * -1e38), d = x - r, precise"},
};

constexpr int kUnroll = 16;          // steps per loop iteration, each with its own constants
constexpr UINT kGroupSize = 64;
constexpr UINT kGroupsFull = 16384;  // 1M threads

struct Config {
  const char* name;
  int chains;
  UINT groups;
};
Config kConfigs[] = {{"tput", 8, kGroupsFull}, {"dep", 1, kGroupsFull}, {"lat", 1, 1}};

[[noreturn]] void fail(const std::string& what) {
  std::fprintf(stderr, "sopt-opbench: %s\n", what.c_str());
  std::exit(1);
}

std::string narrow(const wchar_t* w) {
  char buf[512];
  WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
  return buf;
}

std::string shaderSource(const Test& t, int chains) {
  std::string s =
      "cbuffer C : register(b0) { float4 U[16]; uint iters; float seed; float2 pad; };\n"
      "RWStructuredBuffer<float> O : register(u0);\n"
      // precise keeps fxc from folding (v + c) - c to v
      "float roundAdd(float v) { precise float t = v + 12582912.0; precise float r = t - 12582912.0; return r; }\n"
      "float floorAdd(float v) { precise float r = (v + 12582912.0) - 12582912.0; precise float f = r - saturate((r - v) * 1e38); return f; }\n"
      "float fracAdd(float v) { precise float d = v - ((v + 12582912.0) - 12582912.0); precise float f = d + saturate(d * -1e38); return f; }\n"
      "[numthreads(64, 1, 1)]\n"
      "void main(uint3 id : SV_DispatchThreadID)\n{\n";
  for (int k = 0; k < chains; ++k)
    s += "  float x" + std::to_string(k) + " = 1.0 + frac(id.x * 0.000123 + " + std::to_string(k) + " * 0.137) * seed;\n";
  s += "  [loop] for (uint i = 0; i < iters; ++i)\n  {\n";
  for (int r = 0; r < kUnroll; ++r) {
    s += "    {\n      const float4 c = U[" + std::to_string(r) + "];\n";
    for (int k = 0; k < chains; ++k) {
      const std::string x = "x" + std::to_string(k);
      std::string step = t.step;
      // Replace the chain variable x (a lone identifier) with xk.
      std::string out;
      for (size_t p = 0; p < step.size(); ++p) {
        const bool lone = step[p] == 'x' && (p == 0 || !(isalnum((unsigned char)step[p - 1]) || step[p - 1] == '_' || step[p - 1] == '.')) &&
                          (p + 1 == step.size() || !(isalnum((unsigned char)step[p + 1]) || step[p + 1] == '_'));
        out += lone ? x : std::string(1, step[p]);
      }
      s += "      " + x + " = " + out + ";\n";
    }
    s += "    }\n";
  }
  s += "  }\n  O[id.x] = 0.0";
  for (int k = 0; k < chains; ++k) s += " + x" + std::to_string(k);
  s += ";\n}\n";
  return s;
}

struct Gpu {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  ID3D11Buffer* cb = nullptr;
  ID3D11Buffer* out = nullptr;
  ID3D11UnorderedAccessView* uav = nullptr;
  ID3D11Query* disjoint = nullptr;
  ID3D11Query* t0 = nullptr;
  ID3D11Query* t1 = nullptr;
};

struct CbData {
  float U[16][4];
  UINT iters;
  float seed;
  float pad[2];
};

void setConstants(Gpu& g, const Test& t, UINT iters) {
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
  g.ctx->UpdateSubresource(g.cb, 0, nullptr, &d, 0, 0);
}

// One timed dispatch in milliseconds (negative if the timestamps were disjoint).
double timeDispatch(Gpu& g, UINT groups) {
  g.ctx->Begin(g.disjoint);
  g.ctx->End(g.t0);
  g.ctx->Dispatch(groups, 1, 1);
  g.ctx->End(g.t1);
  g.ctx->End(g.disjoint);
  D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
  while (g.ctx->GetData(g.disjoint, &dj, sizeof(dj), 0) != S_OK) Sleep(0);
  UINT64 a = 0, b = 0;
  while (g.ctx->GetData(g.t0, &a, sizeof(a), 0) != S_OK) Sleep(0);
  while (g.ctx->GetData(g.t1, &b, sizeof(b), 0) != S_OK) Sleep(0);
  if (dj.Disjoint || dj.Frequency == 0) return -1.0;
  return double(b - a) * 1000.0 / double(dj.Frequency);
}

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v.empty() ? 0.0 : v[v.size() / 2];
}

struct Result {
  UINT iters = 0;
  double ms = 0.0;
  double nsPerStep = 0.0;  // per step of one chain in one thread (tput: of all chains together / chains)
};

// Doubles the iteration count until a dispatch takes >= 2 ms.
UINT calibrate(Gpu& g, ID3D11ComputeShader* cs, const Test& t, const Config& c) {
  g.ctx->CSSetShader(cs, nullptr, 0);
  UINT iters = 2;
  for (;;) {
    setConstants(g, t, iters);
    if (timeDispatch(g, c.groups) >= 2.0 || iters >= (1u << 20)) return iters;
    iters *= 2;
  }
}

// The median of reps runs at a known iteration count.
Result measureAt(Gpu& g, ID3D11ComputeShader* cs, const Test& t, const Config& c, UINT iters, int reps) {
  g.ctx->CSSetShader(cs, nullptr, 0);
  setConstants(g, t, iters);
  std::vector<double> runs;
  for (int k = 0; k < reps * 3 && int(runs.size()) < reps; ++k)
    if (const double m = timeDispatch(g, c.groups); m > 0.0) runs.push_back(m);
  Result r;
  r.iters = iters;
  r.ms = median(runs);
  const double steps = double(iters) * kUnroll * double(c.groups) * kGroupSize * c.chains;
  r.nsPerStep = r.ms * 1e6 / steps;
  return r;
}

// One test in one configuration: both passes, each relative to the mad measured just before it.
struct Measured {
  Result r;                    // averaged over the passes
  double units[2] = {0, 0};    // 4 * time / reference mad's time, forward and backward pass
  double vsBase[2] = {0, 0};   // units minus the base test's units, per pass
  double unitsAvg() const { return 0.5 * (units[0] + units[1]); }
  double vsBaseAvg() const { return 0.5 * (vsBase[0] + vsBase[1]); }
};

// Console output: ANSI colors and UTF-8 box / bar characters where the console supports virtual
// terminal sequences (Windows 10+), plain ASCII otherwise.
struct Style {
  bool vt = false;
  const char* c(const char* code) const { return vt ? code : ""; }
  const char* reset() const { return c("\x1b[0m"); }
};

Style initConsole() {
  Style st;
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) &&
      SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
    st.vt = true;
    SetConsoleOutputCP(CP_UTF8);
  }
  return st;
}

// The summary's fixed order: cheapest to most expensive as most GPUs measure it, the same on every
// GPU so results can be compared line by line.
const char* const kDisplayOrder[] = {
    "neg",      "abs",       "negabs",  "saturate", "satmad",  "mul",      "omod2",   "omodhalf", "omod3",
    "min",      "max",       "step",    "add",      "sub",     "mad2",     "contract", "max3",    "minmax",
    "floor",    "ceil",      "round",   "frac",     "clamp",   "select",   "lerp",     "signsel2", "signsat",
    "signmad",  "signclamp", "signsel", "sign",     "roundadd", "flooradd", "fracadd", "divxy",   "rcp",
    "rsqrt",    "sqrt",      "div",     "exp2",     "log2",    "log",      "exp",      "cos",      "sin",
    "rcpmax",   "pow"};

// A comment and a color for a throughput cost (extra over the base, 4 = one fma).
const char* costComment(double v, char* buf, size_t n, const Style& st, const char** color) {
  if (v < 0.75) { *color = st.c("\x1b[92m"); return "free"; }
  if (v < 3.0) { *color = st.c("\x1b[96m"); return "cheap"; }
  if (v < 5.5) { *color = st.c("\x1b[97m"); return "one op"; }
  if (v < 9.0) { *color = st.c("\x1b[93m"); return "two ops"; }
  *color = st.c("\x1b[91m");
  std::snprintf(buf, n, "expensive (~%.0f mads)", v / 4.0);
  return buf;
}

std::string bar(double v, double maxV, int width, const Style& st) {
  if (maxV <= 0.0) return std::string();
  const double cells = std::max(0.0, v) / maxV * width;
  const int full = int(cells);
  std::string s;
  if (!st.vt) {
    s.assign(size_t(full), '#');
    return s;
  }
  static const char* const kPart[] = {"", "\u258F", "\u258E", "\u258D", "\u258C", "\u258B", "\u258A", "\u2589"};
  for (int k = 0; k < full; ++k) s += "\u2588";
  const int part = int((cells - full) * 8.0);
  if (part > 0 && full < width) s += kPart[part];
  if (s.empty()) s = kPart[1];
  return s;
}

// Display width of a UTF-8 string (one column per code point).
int columns(const std::string& s) {
  int n = 0;
  for (unsigned char ch : s) n += (ch & 0xC0) != 0x80;
  return n;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // progress shows while it runs
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
    else if (a == "--groups") {  // thread groups of 64 for tput / dep (default 16384; small for a quick check)
      const UINT n = UINT(std::max(1, std::min(int(kGroupsFull), std::atoi(next()))));
      kConfigs[0].groups = kConfigs[1].groups = n;
    } else {
      std::printf("sopt-opbench %s\nusage: sopt-opbench [--adapter N] [--list] [--filter text] [--reps N] [--out file.csv] [--groups N]\n",
                  SOPT_VERSION);
      return a == "-h" || a == "--help" ? 0 : 1;
    }
  }

  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) fail("CreateDXGIFactory1 failed");
  std::vector<IDXGIAdapter1*> adapters;
  for (UINT k = 0;; ++k) {
    IDXGIAdapter1* ad = nullptr;
    if (factory->EnumAdapters1(k, &ad) == DXGI_ERROR_NOT_FOUND) break;
    adapters.push_back(ad);
  }
  int pick = -1;
  SIZE_T bestMem = 0;
  std::vector<std::string> adapterNames;
  for (size_t k = 0; k < adapters.size(); ++k) {
    DXGI_ADAPTER_DESC1 d;
    adapters[k]->GetDesc1(&d);
    const bool software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    adapterNames.push_back(narrow(d.Description) + (software ? " [software]" : ""));
    if (list)
      std::printf("%zu: %s (%zu MB)%s\n", k, narrow(d.Description).c_str(), size_t(d.DedicatedVideoMemory >> 20),
                  software ? " [software]" : "");
    if (!software && (pick < 0 || d.DedicatedVideoMemory > bestMem)) {
      pick = int(k);
      bestMem = d.DedicatedVideoMemory;
    }
  }
  if (list) return 0;
  if (adapterIndex >= 0) pick = adapterIndex;
  if (pick < 0 || pick >= int(adapters.size())) fail("no GPU adapter (try --list)");

  DXGI_ADAPTER_DESC1 desc;
  adapters[pick]->GetDesc1(&desc);
  const std::string gpuName = narrow(desc.Description);
  LARGE_INTEGER umd = {};
  std::string driver = "unknown";
  if (SUCCEEDED(adapters[pick]->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", unsigned(HIWORD(umd.HighPart)), unsigned(LOWORD(umd.HighPart)),
                  unsigned(HIWORD(umd.LowPart)), unsigned(LOWORD(umd.LowPart)));
    driver = buf;
  }
  std::printf("GPU: %s (vendor 0x%04X, device 0x%04X), driver %s\n", gpuName.c_str(), desc.VendorId, desc.DeviceId,
              driver.c_str());

  Gpu g;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(adapters[pick], D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &g.dev,
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
    D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    g.dev->CreateQuery(&qd, &g.disjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    g.dev->CreateQuery(&qd, &g.t0);
    g.dev->CreateQuery(&qd, &g.t1);
    g.ctx->CSSetConstantBuffers(0, 1, &g.cb);
    g.ctx->CSSetUnorderedAccessViews(0, 1, &g.uav, nullptr);
  }

  char exePath[MAX_PATH];
  GetModuleFileNameA(nullptr, exePath, MAX_PATH);
  const std::filesystem::path here = std::filesystem::path(exePath).parent_path();
  std::string safeName = gpuName;
  for (char& ch : safeName)
    if (!isalnum((unsigned char)ch)) ch = '_';
  if (outPath.empty()) outPath = (here / ("opbench-" + safeName + ".csv")).string();
  const std::filesystem::path dxbcDir = here / "opbench-dxbc";
  std::filesystem::create_directories(dxbcDir);

  // Compile every test for every configuration (chains differ).
  std::vector<const Test*> tests;
  for (const Test& t : kTests)
    if (filter.empty() || std::string(t.name).find(filter) != std::string::npos || !t.base || std::strcmp(t.name, "mad") == 0)
      tests.push_back(&t);
  // Bases of the selected tests are needed too.
  for (bool added = true; added;) {
    added = false;
    for (const Test* t : std::vector<const Test*>(tests))
      if (t->base && std::none_of(tests.begin(), tests.end(), [&](const Test* u) { return std::strcmp(u->name, t->base) == 0; }))
        for (const Test& u : kTests)
          if (std::strcmp(u.name, t->base) == 0) {
            tests.push_back(&u);
            added = true;
          }
  }

  std::map<std::string, std::map<std::string, ID3D11ComputeShader*>> shaders;
  for (const Config& c : kConfigs)
    for (const Test* t : tests) {
      const std::string src = shaderSource(*t, c.chains);
      ID3DBlob* code = nullptr;
      ID3DBlob* err = nullptr;
      if (FAILED(D3DCompile(src.data(), src.size(), t->name, nullptr, nullptr, "main", "cs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
        fail(std::string("cannot compile test ") + t->name + ":\n" + (err ? (const char*)err->GetBufferPointer() : ""));
      if (c.chains == 8) {  // disassembly of the throughput version
        ID3DBlob* dis = nullptr;
        if (SUCCEEDED(D3DDisassemble(code->GetBufferPointer(), code->GetBufferSize(), 0, nullptr, &dis))) {
          if (FILE* f = std::fopen((dxbcDir / (std::string(t->name) + ".txt")).string().c_str(), "wb")) {
            std::fwrite(src.data(), 1, src.size(), f);
            std::fputs("\n// ---- DXBC (D3DCompile -O3) ----\n", f);
            std::fwrite(dis->GetBufferPointer(), 1, dis->GetBufferSize() - 1, f);
            std::fclose(f);
          }
          dis->Release();
        }
      }
      ID3D11ComputeShader* cs = nullptr;
      if (FAILED(g.dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs)))
        fail(std::string("cannot create test ") + t->name);
      code->Release();
      shaders[c.name][t->name] = cs;
    }

  const Test* madTest = nullptr;
  for (const Test* t : tests)
    if (std::strcmp(t->name, "mad") == 0) madTest = t;
  // Warm up (clocks ramp up): two seconds of the reference test.
  {
    g.ctx->CSSetShader(shaders["tput"]["mad"], nullptr, 0);
    setConstants(g, *madTest, 256);
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < 2000) timeDispatch(g, kConfigs[0].groups);
  }

  std::map<std::string, std::map<std::string, Measured>> results;  // config -> test -> result
  std::map<std::string, double> madDrift;                          // config -> spread of the reference
  std::map<std::string, double> madNs;                             // config -> mean reference time
  for (const Config& c : kConfigs) {
    std::printf("%s== %s%s (%d chain%s, %u threads): ", st.c("\x1b[1m"), c.name, st.reset(), c.chains,
                c.chains > 1 ? "s" : "", c.groups * kGroupSize);
    std::map<std::string, UINT> iters;
    for (const Test* t : tests) iters[t->name] = calibrate(g, shaders[c.name][t->name], *t, c);
    std::vector<double> mads;
    std::map<std::string, Result> sum;
    for (int pass = 0; pass < 2; ++pass) {
      std::vector<const Test*> order = tests;
      if (pass == 1) std::reverse(order.begin(), order.end());
      for (const Test* t : order) {
        // A fresh reference right before the test: a clock change moves both.
        const Result m = measureAt(g, shaders[c.name]["mad"], *madTest, c, iters["mad"], reps);
        const Result r = t == madTest ? m : measureAt(g, shaders[c.name][t->name], *t, c, iters[t->name], reps);
        mads.push_back(m.nsPerStep);
        Measured& x = results[c.name][t->name];
        x.units[pass] = 4.0 * r.nsPerStep / m.nsPerStep;
        Result& acc = sum[t->name];
        acc.iters = r.iters;
        acc.ms += 0.5 * r.ms;
        acc.nsPerStep += 0.5 * r.nsPerStep;
        std::printf(".");
      }
    }
    for (const Test* t : tests) {
      Measured& x = results[c.name][t->name];
      x.r = sum[t->name];
      for (int pass = 0; pass < 2; ++pass)
        x.vsBase[pass] = t->base ? x.units[pass] - results[c.name][t->base].units[pass] : x.units[pass];
    }
    const auto [lo, hi] = std::minmax_element(mads.begin(), mads.end());
    madDrift[c.name] = 100.0 * (*hi - *lo) / median(mads);
    double mean = 0.0;
    for (double v : mads) mean += v / double(mads.size());
    madNs[c.name] = mean;
    std::printf(" reference drift %.1f%%\n", madDrift[c.name]);
  }

  // CSV: the GPU once in header lines, then one row per configuration and test.
  FILE* csv = std::fopen(outPath.c_str(), "wb");
  if (!csv) fail("cannot write " + outPath);
  std::fprintf(csv, "# sopt-opbench %s\n# gpu: %s\n# vendor: 0x%04X\n# device: 0x%04X\n# driver: %s\n", SOPT_VERSION,
               gpuName.c_str(), desc.VendorId, desc.DeviceId, driver.c_str());
  for (const Config& c : kConfigs) std::fprintf(csv, "# reference drift %s: %.2f%%\n", c.name, madDrift[c.name]);
  std::fprintf(csv, "config,test,base,iters,ms,ns_per_step,units,units_vs_base,vs_base_fwd,vs_base_bwd,step,note\n");
  for (const Config& c : kConfigs)
    for (const Test* t : tests) {
      const Measured& x = results[c.name][t->name];
      std::fprintf(csv, "%s,%s,%s,%u,%.4f,%.6f,%.3f,%.3f,%.3f,%.3f,\"%s\",\"%s\"\n", c.name, t->name, t->base ? t->base : "",
                   x.r.iters, x.r.ms, x.r.nsPerStep, x.unitsAvg(), x.vsBaseAvg(), x.vsBase[0], x.vsBase[1], t->step,
                   t->note ? t->note : "");
    }
  std::fclose(csv);

  // Summary: banner, other adapters, throughput costs in the fixed order.
  const double fmaRate = 1.0 / madNs["tput"];  // per ns
  const std::string title = std::string("sopt-opbench ") + SOPT_VERSION + "  -  " + gpuName;
  const int w = columns(title) + 4;
  const std::string hz = st.vt ? "\u2550" : "=";
  std::string line;
  for (int k = 0; k < w; ++k) line += hz;
  std::printf("\n  %s%s%s%s%s\n", st.c("\x1b[1;96m"), st.vt ? "\u2554" : "+", line.c_str(), st.vt ? "\u2557" : "+", st.reset());
  std::printf("  %s%s%s  %s%s%s  %s%s%s\n", st.c("\x1b[1;96m"), st.vt ? "\u2551" : "|", st.reset(), st.c("\x1b[1;97m"),
              title.c_str(), st.reset(), st.c("\x1b[1;96m"), st.vt ? "\u2551" : "|", st.reset());
  std::printf("  %s%s%s%s%s\n", st.c("\x1b[1;96m"), st.vt ? "\u255A" : "+", line.c_str(), st.vt ? "\u255D" : "+", st.reset());
  std::printf("  driver %s, vendor 0x%04X, device 0x%04X, %.1f TFLOPS fp32 (measured)\n", driver.c_str(), desc.VendorId,
              desc.DeviceId, fmaRate * 2.0 / 1000.0);
  if (adapters.size() > 1) {
    std::printf("  Also detected in system:\n");
    size_t nameW = 0;
    for (size_t k = 0; k < adapters.size(); ++k)
      if (int(k) != pick) nameW = std::max(nameW, adapterNames[k].size());
    for (size_t k = 0; k < adapters.size(); ++k)
      if (int(k) != pick)
        std::printf("    %zu: %-*s   %sUse --adapter %zu to test this%s\n", k, int(nameW), adapterNames[k].c_str(),
                    st.c("\x1b[90m"), k, st.reset());
  }

  constexpr int kBarWidth = 28;
  double maxV = 0.0;
  for (const char* name : kDisplayOrder)
    if (results["tput"].count(name)) maxV = std::max(maxV, results["tput"][name].vsBaseAvg());
  std::printf("\n  %s%-10s %6s  %-*s  %s%s\n", st.c("\x1b[1m"), "test", "cost", kBarWidth, "graph", "comment", st.reset());
  std::vector<std::string> unstable;
  for (const char* name : kDisplayOrder) {
    if (!results["tput"].count(name)) continue;
    const Measured& x = results["tput"][name];
    const double v = x.vsBaseAvg();
    char buf[64];
    const char* color = "";
    const char* comment = costComment(v, buf, sizeof(buf), st, &color);
    const std::string b = bar(v, maxV, kBarWidth, st);
    const double diff = std::fabs(x.vsBase[0] - x.vsBase[1]);
    const bool shaky = diff > std::max(0.75, 0.15 * std::fabs(v));
    if (shaky) unstable.push_back(name);
    std::printf("  %-10s %6.1f  %s%s%s%*s  %s%s%s%s\n", name, v, color, b.c_str(), st.reset(),
                std::max(0, kBarWidth - columns(b)), "", color, comment, st.reset(),
                shaky ? (st.vt ? "  \x1b[93m! passes disagree\x1b[0m" : "  ! passes disagree") : "");
  }
  std::printf("\n  cost = extra over the test's base, in sopt units (4 = one fma); throughput, %d chains.\n",
              kConfigs[0].chains);

  bool warned = false;
  for (const Config& c : kConfigs)
    if (madDrift[c.name] > 5.0) {
      if (!warned) std::printf("\n");
      warned = true;
      std::printf("  %sWarning:%s the reference changed by %.0f%% during the %s tests: the GPU clock moved.\n",
                  st.c("\x1b[1;93m"), st.reset(), madDrift[c.name], c.name);
    }
  if (!unstable.empty()) {
    std::printf("%s  %sWarning:%s %zu test(s) differ between the forward and backward pass:", warned ? "" : "\n",
                st.c("\x1b[1;93m"), st.reset(), unstable.size());
    for (const std::string& n : unstable) std::printf(" %s", n.c_str());
    std::printf("\n");
    warned = true;
  }
  if (warned)
    std::printf("  For steadier numbers: close other programs, plug in a laptop, set \"Prefer maximum performance\"\n"
                "  (NVIDIA) or lock the clocks, and run it again.\n");
  std::printf("\n  CSV:  %s\n  DXBC: %s\n", outPath.c_str(), dxbcDir.string().c_str());
  return 0;
}

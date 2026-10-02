// sopt-timer: a ReShade add-on that times techniques on the GPU (M4 benchmark harness).
//
// Passive: a timestamp before the first and after every technique ReShade renders; per
// technique the median, 10th/90th percentile over the last frames and a 60-frame mean
// (what ReShade's own statistics show) in the add-on's settings.
// Bench: walks the presets (default: sopt-*.ini next to the current preset, the test
// bundle's one preset per effect and SOPT_ALL step). For every pair of enabled techniques
// X_orig / X_sopt it renders both itself on the same input (the frame before any effect),
// interleaved A B B A / B A A B, one timestamp pair per run. Per frame the difference of
// the means (sopt - orig) cancels clock drift; the medians go to sopt-timer.csv in
// ReShade's base path. The displayed frame is restored afterwards.
//
// D3D10/11 frames are dropped when the timestamp counter was disjoint (ReShade reads the
// frequency once and never checks this).

#define NOMINMAX
#include <imgui.h>
#include <reshade.hpp>

#include <d3d11.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace reshade::api;

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kRing = 4;       // frames in flight: results are read kRing frames later
constexpr uint32_t kSlots = 1024;   // timestamps per frame
constexpr size_t kKeep = 600;       // passive samples kept per technique

struct Run {
  int pair, side;   // side 0 = orig, 1 = sopt
  uint32_t t0, t1;
  bool rendered;
};

struct FrameRec {
  bool used = false;
  uint32_t next = 0;
  uint64_t gen = 0;                                        // bench generation (preset)
  std::vector<std::pair<std::string, uint32_t>> passive;   // first entry: begin
  std::vector<Run> runs;
  std::vector<uint64_t> chain;   // techniques ReShade rendered this frame (so they are created)
  ID3D11Query* disjoint = nullptr;
  bool disjointBegun = false;
};

struct Pair {
  std::string stem, effA, techA, effB, techB;
  effect_technique a = {0}, b = {0};
};

struct Config {
  int warmup = 120, frames = 300, repeats = 2;
  std::string pattern = "sopt-";
  bool autoRun = false, exitWhenDone = false;
  bool screenshots = false;  // one screenshot per preset once it has warmed up (ReShade's SavePath)
};

enum class Phase { Idle, Switch, Settle, Measure, Done };

struct State {
  effect_runtime* runtime = nullptr;
  query_heap heap = {0};
  uint64_t freq = 0;
  uint64_t frame = 0;
  FrameRec recs[kRing];
  FrameRec* cur = nullptr;
  bool inside = false;         // inside our own render_technique calls
  resource in = {0}, out = {0};
  resource_desc copyDesc;
  bool haveInput = false;
  std::map<std::string, std::deque<double>> passive;   // microseconds
  Config cfg;
  // bench
  Phase phase = Phase::Idle;
  std::vector<std::string> presets;
  size_t presetIdx = 0;
  std::string original;
  uint64_t gen = 0;
  int settle = 0;
  std::vector<Pair> pairs;
  std::vector<uint64_t> lastChain;   // techniques rendered in the last frame
  ULONGLONG tick = 0;
  std::vector<std::vector<double>> a, b, d;   // per pair: per frame mean orig, mean sopt, sopt - orig
  int dropped = 0;
  int runsSeen = 0, runsBad = 0, framesStale = 0, framesNoInput = 0;
  std::vector<std::string> rows;
  std::string status = "idle";
  std::string csvPath;
};

State g;

// UTF-8 text of a path (u8string() is std::u8string in C++20).
std::string utf8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

void logf(const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  reshade::log::message(reshade::log::level::info, buf);
}

std::string techniqueKey(effect_runtime* rt, effect_technique t, std::string* effect = nullptr,
                         std::string* name = nullptr) {
  char e[256] = "", n[256] = "";
  rt->get_technique_effect_name(t, e);
  rt->get_technique_name(t, n);
  if (effect) *effect = e;
  if (name) *name = n;
  return std::string(n) + "@" + e;
}

double quantile(std::vector<double> v, double q) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const double pos = q * static_cast<double>(v.size() - 1);
  const size_t i = static_cast<size_t>(pos);
  const double f = pos - static_cast<double>(i);
  return i + 1 < v.size() ? v[i] * (1.0 - f) + v[i + 1] * f : v[i];
}

std::string basePath() {
  char path[1024] = "";
  size_t n = sizeof(path);
  reshade::get_reshade_base_path(path, &n);
  return path;
}

void readConfig() {
  Config& c = g.cfg;
  reshade::get_config_value(nullptr, "SOPT_TIMER", "WarmupFrames", c.warmup);
  reshade::get_config_value(nullptr, "SOPT_TIMER", "Frames", c.frames);
  reshade::get_config_value(nullptr, "SOPT_TIMER", "Repeats", c.repeats);
  reshade::get_config_value(nullptr, "SOPT_TIMER", "AutoRun", c.autoRun);
  reshade::get_config_value(nullptr, "SOPT_TIMER", "ExitWhenDone", c.exitWhenDone);
  reshade::get_config_value(nullptr, "SOPT_TIMER", "Screenshots", c.screenshots);
  char pat[256] = "";
  size_t n = sizeof(pat);
  if (reshade::get_config_value(nullptr, "SOPT_TIMER", "PresetPattern", pat, &n) && pat[0]) c.pattern = pat;
  // The process environment (sopt-host sets these after start; the CRT's getenv copy is older).
  char env[16];
  if (GetEnvironmentVariableA("SOPT_TIMER_AUTO", env, sizeof(env))) c.autoRun = std::atoi(env) != 0;
  if (GetEnvironmentVariableA("SOPT_TIMER_EXIT", env, sizeof(env))) c.exitWhenDone = std::atoi(env) != 0;
  if (GetEnvironmentVariableA("SOPT_TIMER_SHOTS", env, sizeof(env))) c.screenshots = std::atoi(env) != 0;
  c.warmup = std::max(c.warmup, 8);
  c.frames = std::max(c.frames, 10);
  c.repeats = std::clamp(c.repeats, 1, 16);
}

// ---- resources -------------------------------------------------------------------------

void destroyCopies(device* dev) {
  if (g.in.handle) dev->destroy_resource(g.in);
  if (g.out.handle) dev->destroy_resource(g.out);
  g.in = g.out = {0};
  g.haveInput = false;
}

bool ensureCopies(device* dev, resource bb) {
  resource_desc desc = dev->get_resource_desc(bb);
  if (desc.texture.samples > 1) return false;
  if (g.in.handle && desc.texture.width == g.copyDesc.texture.width &&
      desc.texture.height == g.copyDesc.texture.height && desc.texture.format == g.copyDesc.texture.format)
    return true;
  destroyCopies(dev);
  desc.heap = memory_heap::default_;
  desc.usage = resource_usage::copy_source | resource_usage::copy_dest;
  desc.flags = resource_flags::none;
  desc.texture.levels = 1;
  if (!dev->create_resource(desc, nullptr, resource_usage::copy_source, &g.in) ||
      !dev->create_resource(desc, nullptr, resource_usage::copy_source, &g.out)) {
    destroyCopies(dev);
    return false;
  }
  g.copyDesc = desc;
  return true;
}

// Copies between the back buffer (render target state) and a copy (copy source state).
void fromBackBuffer(command_list* cmd, resource bb, resource dst) {
  cmd->barrier(bb, resource_usage::render_target, resource_usage::copy_source);
  cmd->barrier(dst, resource_usage::copy_source, resource_usage::copy_dest);
  cmd->copy_resource(bb, dst);
  cmd->barrier(dst, resource_usage::copy_dest, resource_usage::copy_source);
  cmd->barrier(bb, resource_usage::copy_source, resource_usage::render_target);
}
void toBackBuffer(command_list* cmd, resource src, resource bb) {
  cmd->barrier(bb, resource_usage::render_target, resource_usage::copy_dest);
  cmd->copy_resource(src, bb);
  cmd->barrier(bb, resource_usage::copy_dest, resource_usage::render_target);
}

uint32_t stamp(command_list* cmd) {
  if (!g.cur || g.cur->next >= kSlots) return UINT32_MAX;
  const uint32_t i = g.cur->next++;
  cmd->end_query(g.heap, query_type::timestamp, static_cast<uint32_t>(g.cur - g.recs) * kSlots + i);
  return i;
}

// ---- results ---------------------------------------------------------------------------

void collect(FrameRec& r, command_list* cmd) {
  if (!r.used || r.next == 0) return;
  device* dev = g.runtime->get_device();
  std::vector<uint64_t> ts(r.next);
  const uint32_t base = static_cast<uint32_t>(&r - g.recs) * kSlots;
  if (!dev->get_query_heap_results(g.heap, query_type::timestamp, base, r.next, ts.data(), sizeof(uint64_t))) {
    ++g.dropped;
    return;
  }
  uint64_t freq = g.freq;
  if (r.disjoint) {
    auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(cmd->get_native());
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
    if (!r.disjointBegun || ctx->GetData(r.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
        dj.Disjoint) {
      ++g.dropped;
      return;
    }
    freq = dj.Frequency;
  }
  if (freq == 0) return;
  const double us = 1e6 / static_cast<double>(freq);
  for (size_t i = 1; i < r.passive.size(); ++i) {
    auto& q = g.passive[r.passive[i].first];
    q.push_back(static_cast<double>(ts[r.passive[i].second] - ts[r.passive[i - 1].second]) * us);
    if (q.size() > kKeep) q.pop_front();
  }
  if (!r.runs.empty() && r.gen != g.gen) ++g.framesStale;
  if (r.runs.empty() || r.gen != g.gen || g.phase != Phase::Measure) return;
  std::vector<double> sum(g.pairs.size() * 2, 0.0);
  std::vector<int> cnt(g.pairs.size() * 2, 0), bad(g.pairs.size(), 0);
  for (const Run& run : r.runs) {
    if (run.pair >= static_cast<int>(g.pairs.size())) continue;
    ++g.runsSeen;
    if (!run.rendered || run.t0 == UINT32_MAX || run.t1 == UINT32_MAX) {
      ++g.runsBad;
      bad[run.pair] = 1;
      continue;
    }
    sum[run.pair * 2 + run.side] += static_cast<double>(ts[run.t1] - ts[run.t0]) * us;
    ++cnt[run.pair * 2 + run.side];
  }
  for (size_t p = 0; p < g.pairs.size(); ++p) {
    if (bad[p] || !cnt[p * 2] || !cnt[p * 2 + 1]) continue;
    const double ma = sum[p * 2] / cnt[p * 2], mb = sum[p * 2 + 1] / cnt[p * 2 + 1];
    g.a[p].push_back(ma);
    g.b[p].push_back(mb);
    g.d[p].push_back(mb - ma);
  }
}

// ---- bench -----------------------------------------------------------------------------

std::string apiName(device_api api) {
  switch (api) {
    case device_api::d3d9: return "d3d9";
    case device_api::d3d10: return "d3d10";
    case device_api::d3d11: return "d3d11";
    case device_api::d3d12: return "d3d12";
    case device_api::opengl: return "opengl";
    case device_api::vulkan: return "vulkan";
    default: return "unknown";
  }
}

void findPairs(effect_runtime* rt) {
  struct Tech { std::string key, effect, name; effect_technique h; };
  std::vector<Tech> all;
  rt->enumerate_techniques(nullptr, [](effect_runtime* rt, effect_technique t, void* user) {
    if (!rt->get_technique_state(t)) return;
    Tech x;
    x.key = techniqueKey(rt, t, &x.effect, &x.name);
    x.h = t;
    static_cast<std::vector<Tech>*>(user)->push_back(x);
  }, &all);
  g.pairs.clear();
  for (const Tech& x : all) {
    if (x.name.size() < 5 || x.name.compare(x.name.size() - 5, 5, "_orig") != 0) continue;
    const std::string stem = x.name.substr(0, x.name.size() - 5);
    for (const Tech& y : all)
      if (y.name == stem + "_sopt") {
        g.pairs.push_back({stem, x.effect, x.name, y.effect, y.name, x.h, y.h});
        break;
      }
  }
  g.a.assign(g.pairs.size(), {});
  g.b.assign(g.pairs.size(), {});
  g.d.assign(g.pairs.size(), {});
}

void refreshHandles(effect_runtime* rt) {
  for (Pair& p : g.pairs) {
    p.a = rt->find_technique(p.effA.c_str(), p.techA.c_str());
    p.b = rt->find_technique(p.effB.c_str(), p.techB.c_str());
  }
}

std::string csvField(const std::string& s) {
  if (s.find_first_of(",\"") == std::string::npos) return s;
  std::string o = "\"";
  for (char c : s) o += c == '"' ? std::string("\"\"") : std::string(1, c);
  return o + "\"";
}

void writeCsv() {
  device* dev = g.runtime->get_device();
  uint32_t vendor = 0, w = 0, h = 0;
  dev->get_property(device_properties::vendor_id, &vendor);
  char descr[256] = "";
  dev->get_property(device_properties::description, descr);
  g.runtime->get_screenshot_width_and_height(&w, &h);
  g.csvPath = utf8(fs::u8path(basePath()) / "sopt-timer.csv");
  FILE* f = std::fopen(g.csvPath.c_str(), "w");
  if (!f) {
    g.status = "cannot write " + g.csvPath;
    return;
  }
  std::fprintf(f, "# sopt-timer: api %s, vendor 0x%04x, %s, %ux%u, frames %d, repeats %d, dropped %d\n",
               apiName(dev->get_api()).c_str(), vendor, descr, w, h, g.cfg.frames, g.cfg.repeats, g.dropped);
  std::fprintf(f, "preset,technique,frames,orig_us,sopt_us,orig_p10,orig_p90,sopt_p10,sopt_p90,"
                  "diff_us,diff_p10,diff_p90,diff_pct,sopt_faster_frames_pct\n");
  for (const std::string& r : g.rows) std::fprintf(f, "%s\n", r.c_str());
  std::fclose(f);
}

void finishPreset() {
  const std::string preset = utf8(fs::u8path(g.presets[g.presetIdx]).filename());
  if (g.pairs.empty()) g.rows.push_back(csvField(preset) + ",(no _orig/_sopt pair),0,,,,,,,,,,,");
  for (size_t p = 0; p < g.pairs.size(); ++p) {
    const double ma = quantile(g.a[p], 0.5), mb = quantile(g.b[p], 0.5), md = quantile(g.d[p], 0.5);
    size_t faster = 0;
    for (double x : g.d[p]) faster += x < 0.0;
    char buf[512];
    std::snprintf(buf, sizeof(buf), ",%zu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%.1f", g.d[p].size(), ma,
                  mb, quantile(g.a[p], 0.1), quantile(g.a[p], 0.9), quantile(g.b[p], 0.1), quantile(g.b[p], 0.9), md,
                  quantile(g.d[p], 0.1), quantile(g.d[p], 0.9), ma > 0 ? 100.0 * md / ma : 0.0,
                  g.d[p].empty() ? 0.0 : 100.0 * static_cast<double>(faster) / static_cast<double>(g.d[p].size()));
    g.rows.push_back(csvField(preset) + "," + csvField(g.pairs[p].stem) + buf);
  }
}

void startBench(effect_runtime* rt) {
  readConfig();
  char cur[1024] = "";
  size_t n = sizeof(cur);
  rt->get_current_preset_path(cur, &n);
  g.original = cur;
  g.presets.clear();
  std::error_code ec;
  const fs::path dir = fs::u8path(g.original).parent_path();
  for (const auto& e : fs::directory_iterator(dir.empty() ? fs::u8path(basePath()) : dir, ec)) {
    const std::string name = utf8(e.path().filename());
    if (e.path().extension() == ".ini" && name.rfind(g.cfg.pattern, 0) == 0) g.presets.push_back(utf8(e.path()));
  }
  std::sort(g.presets.begin(), g.presets.end());
  g.rows.clear();
  g.dropped = 0;
  g.presetIdx = 0;
  if (g.presets.empty()) {
    // No bundle presets: time the pairs of the current preset.
    g.presets.push_back(g.original);
  }
  g.phase = Phase::Switch;
  logf("sopt-timer: bench over %zu preset(s), warmup %d, frames %d, repeats %d", g.presets.size(), g.cfg.warmup,
       g.cfg.frames, g.cfg.repeats);
}

std::string presetName() { return utf8(fs::u8path(g.presets[g.presetIdx]).filename()); }

bool chainRendered(effect_technique t) {
  return t.handle && std::find(g.lastChain.begin(), g.lastChain.end(), t.handle) != g.lastChain.end();
}

void nextPreset(effect_runtime* rt) {
  finishPreset();
  logf("sopt-timer: %s", g.rows.back().c_str());
  if (++g.presetIdx < g.presets.size()) {
    g.phase = Phase::Switch;
    return;
  }
  writeCsv();
  if (!g.original.empty()) rt->set_current_preset_path(g.original.c_str());
  g.phase = Phase::Done;
  g.status = "done: " + g.csvPath;
  logf("sopt-timer: wrote %s (%d frames dropped)", g.csvPath.c_str(), g.dropped);
  if (g.cfg.exitWhenDone) PostMessageW(static_cast<HWND>(rt->get_hwnd()), WM_CLOSE, 0, 0);
}

void stepBench(effect_runtime* rt) {
  switch (g.phase) {
    case Phase::Switch:
      ++g.gen;
      g.settle = 0;
      g.pairs.clear();
      g.lastChain.clear();
      rt->set_current_preset_path(g.presets[g.presetIdx].c_str());
      g.phase = Phase::Settle;
      g.tick = GetTickCount64();
      g.status = "loading " + presetName();
      break;
    case Phase::Settle: {
      // Warm-up counts frames in which ReShade rendered both techniques of every pair
      // (effects compile asynchronously after a preset switch).
      findPairs(rt);
      bool ready = !g.pairs.empty();
      for (const Pair& p : g.pairs) ready = ready && chainRendered(p.a) && chainRendered(p.b);
      g.settle = ready ? g.settle + 1 : 0;
      if (g.settle >= g.cfg.warmup) {
        // The preset's chain as displayed (for the bundle's presets: the Compare difference
        // image, black where orig and sopt agree), named after the preset.
        if (g.cfg.screenshots) rt->save_screenshot((" " + utf8(fs::u8path(g.presets[g.presetIdx]).stem())).c_str());
        ++g.gen;
        g.phase = Phase::Measure;
        g.tick = GetTickCount64();
        g.status = "measuring " + presetName() + " (" + std::to_string(g.presetIdx + 1) + "/" +
                   std::to_string(g.presets.size()) + ")";
        logf("sopt-timer: %s: %zu pair(s)", g.status.c_str(), g.pairs.size());
      } else if (GetTickCount64() - g.tick > 60000) {
        logf("sopt-timer: %s: no _orig/_sopt pair rendered within 60 s", presetName().c_str());
        g.pairs.clear();
        nextPreset(rt);
      }
      break;
    }
    case Phase::Measure: {
      size_t done = SIZE_MAX;
      for (const auto& d : g.d) done = std::min(done, d.size());
      if (++g.settle % 100 == 0)
        logf("sopt-timer: %s: %zu/%d frames, %d dropped, runs %d (%d not rendered), stale %d, no input %d",
             g.status.c_str(), done, g.cfg.frames, g.dropped, g.runsSeen, g.runsBad, g.framesStale,
             g.framesNoInput);
      // Give up after 2 minutes (e.g. a technique that stopped rendering).
      if (done >= static_cast<size_t>(g.cfg.frames) || GetTickCount64() - g.tick > 120000) nextPreset(rt);
      break;
    }
    default:
      break;
  }
}

// ---- events ----------------------------------------------------------------------------

void onInit(effect_runtime* rt) {
  if (g.runtime) return;   // one runtime (the first swap chain)
  g.runtime = rt;
  device* dev = rt->get_device();
  readConfig();
  if (!dev->create_query_heap(query_type::timestamp, kRing * kSlots, &g.heap)) {
    g.status = "no timestamp queries";
    g.heap = {0};
  }
  g.freq = rt->get_command_queue()->get_timestamp_frequency();
  if (dev->get_api() == device_api::d3d11) {
    auto* d3d = reinterpret_cast<ID3D11Device*>(dev->get_native());
    D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    for (FrameRec& r : g.recs) d3d->CreateQuery(&qd, &r.disjoint);
  }
  if (g.cfg.autoRun) g.phase = Phase::Idle, g.status = "auto run pending";
  logf("sopt-timer: timestamp frequency %llu Hz, auto run %d", static_cast<unsigned long long>(g.freq),
       int(g.cfg.autoRun));
}

void onDestroy(effect_runtime* rt) {
  if (rt != g.runtime) return;
  device* dev = rt->get_device();
  if (g.heap.handle) dev->destroy_query_heap(g.heap);
  for (FrameRec& r : g.recs) {
    if (r.disjoint) r.disjoint->Release();
    r = FrameRec();
  }
  destroyCopies(dev);
  g.runtime = nullptr;
  g.heap = {0};
}

void onBeginEffects(effect_runtime* rt, command_list* cmd, resource_view rtv, resource_view) {
  if (rt != g.runtime || g.inside || !g.heap.handle) return;
  ++g.frame;
  FrameRec& r = g.recs[g.frame % kRing];
  collect(r, cmd);
  ID3D11Query* dj = r.disjoint;
  r = FrameRec();
  r.disjoint = dj;
  r.used = true;
  r.gen = g.gen;
  g.cur = &r;
  if (r.disjoint) {
    reinterpret_cast<ID3D11DeviceContext*>(cmd->get_native())->Begin(r.disjoint);
    r.disjointBegun = true;
  }
  g.haveInput = false;
  if (g.phase == Phase::Measure && !g.pairs.empty()) {
    device* dev = rt->get_device();
    const resource bb = dev->get_resource_from_view(rtv);
    if (ensureCopies(dev, bb)) {
      fromBackBuffer(cmd, bb, g.in);
      g.haveInput = true;
    }
  }
  r.passive.emplace_back("", stamp(cmd));
}

void onRenderTechnique(effect_runtime* rt, effect_technique t, command_list* cmd, resource_view, resource_view) {
  if (rt != g.runtime) return;
  if (g.inside || !g.cur) return;   // ReShade does not send an add-on the events its own calls cause
  g.cur->chain.push_back(t.handle);
  const uint32_t i = stamp(cmd);
  if (i != UINT32_MAX) g.cur->passive.emplace_back(techniqueKey(rt, t), i);
}

void timeRun(effect_runtime* rt, command_list* cmd, resource_view rtv, resource_view rtv_srgb, resource bb,
             int pair, int side) {
  const effect_technique t = side ? g.pairs[pair].b : g.pairs[pair].a;
  toBackBuffer(cmd, g.in, bb);
  Run run{pair, side, 0, 0, false};
  run.t0 = stamp(cmd);
  // render_technique returns silently when the effect is not created yet; one ReShade
  // rendered in this frame's chain is.
  run.rendered = t.handle && std::find(g.cur->chain.begin(), g.cur->chain.end(), t.handle) != g.cur->chain.end();
  g.inside = true;
  if (run.rendered) rt->render_technique(t, cmd, rtv, rtv_srgb);
  g.inside = false;
  run.t1 = stamp(cmd);
  g.cur->runs.push_back(run);
}

void onFinishEffects(effect_runtime* rt, command_list* cmd, resource_view rtv, resource_view rtv_srgb) {
  if (rt != g.runtime || g.inside || !g.cur) return;
  if (g.phase == Phase::Measure && !g.haveInput) ++g.framesNoInput;
  if (g.phase == Phase::Measure && g.haveInput && g.cur->gen == g.gen) {
    device* dev = rt->get_device();
    const resource bb = dev->get_resource_from_view(rtv);
    fromBackBuffer(cmd, bb, g.out);
    const int parity = static_cast<int>(g.frame & 1);
    for (int p = 0; p < static_cast<int>(g.pairs.size()); ++p)
      for (int rep = 0; rep < g.cfg.repeats; ++rep) {
        const int first = (rep + parity) & 1;   // A B B A / B A A B
        timeRun(rt, cmd, rtv, rtv_srgb, bb, p, first);
        timeRun(rt, cmd, rtv, rtv_srgb, bb, p, first ^ 1);
      }
    toBackBuffer(cmd, g.out, bb);
  }
  g.lastChain = g.cur->chain;
  if (g.cur->disjoint && g.cur->disjointBegun)
    reinterpret_cast<ID3D11DeviceContext*>(cmd->get_native())->End(g.cur->disjoint);
  g.cur = nullptr;
}

void onReloaded(effect_runtime* rt) {
  if (rt != g.runtime) return;
  g.passive.clear();
  g.lastChain.clear();
  if (g.phase == Phase::Measure) {
    refreshHandles(rt);
    ++g.gen;   // drop frames in flight: their handles are gone
  }
}

void onPresent(effect_runtime* rt) {
  if (rt != g.runtime) return;
  if (g.phase == Phase::Idle && g.cfg.autoRun && g.frame > 30) {
    g.cfg.autoRun = false;
    startBench(rt);
  }
  if (g.phase != Phase::Idle && g.phase != Phase::Done) stepBench(rt);
}

void onOverlay(effect_runtime* rt) {
  if (rt != g.runtime) {
    ImGui::TextUnformatted("sopt-timer times the first effect runtime only.");
    return;
  }
  ImGui::Text("Status: %s", g.status.c_str());
  if (g.phase == Phase::Idle || g.phase == Phase::Done) {
    if (ImGui::Button("Run bench (sopt-*.ini presets)")) startBench(rt);
  } else if (ImGui::Button("Stop")) {
    g.phase = Phase::Idle;
    g.status = "stopped";
    if (!g.original.empty()) rt->set_current_preset_path(g.original.c_str());
  }
  ImGui::SameLine();
  if (ImGui::Button("Clear")) g.passive.clear();
  ImGui::Text("Dropped frames (results not ready or disjoint): %d", g.dropped);
  ImGui::Separator();
  ImGui::TextUnformatted("GPU time per technique, microseconds (last frames):");
  if (ImGui::BeginTable("sopt_timer", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("technique");
    ImGui::TableSetupColumn("median");
    ImGui::TableSetupColumn("p10 - p90");
    ImGui::TableSetupColumn("mean (60)");
    ImGui::TableSetupColumn("n");
    ImGui::TableHeadersRow();
    for (const auto& [name, q] : g.passive) {
      std::vector<double> v(q.begin(), q.end());
      double mean = 0.0;
      const size_t m = std::min<size_t>(60, v.size());
      for (size_t i = v.size() - m; i < v.size(); ++i) mean += v[i];
      mean = m ? mean / static_cast<double>(m) : 0.0;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(name.c_str());
      ImGui::TableNextColumn();
      ImGui::Text("%.1f", quantile(v, 0.5));
      ImGui::TableNextColumn();
      ImGui::Text("%.1f - %.1f", quantile(v, 0.1), quantile(v, 0.9));
      ImGui::TableNextColumn();
      ImGui::Text("%.1f", mean);
      ImGui::TableNextColumn();
      ImGui::Text("%zu", v.size());
    }
    ImGui::EndTable();
  }
  if (!g.rows.empty()) {
    ImGui::Separator();
    ImGui::TextUnformatted("Bench (preset, technique, frames, orig us, sopt us, ..., diff %):");
    for (const std::string& r : g.rows) ImGui::TextUnformatted(r.c_str());
  }
}

}  // namespace

extern "C" __declspec(dllexport) const char* NAME = "sopt-timer";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "GPU timings of techniques; A/B bench of sopt variant presets (_orig / _sopt pairs).";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(module)) return FALSE;
      reshade::register_event<reshade::addon_event::init_effect_runtime>(onInit);
      reshade::register_event<reshade::addon_event::destroy_effect_runtime>(onDestroy);
      reshade::register_event<reshade::addon_event::reshade_begin_effects>(onBeginEffects);
      reshade::register_event<reshade::addon_event::reshade_render_technique>(onRenderTechnique);
      reshade::register_event<reshade::addon_event::reshade_finish_effects>(onFinishEffects);
      reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(onReloaded);
      reshade::register_event<reshade::addon_event::reshade_present>(onPresent);
      reshade::register_overlay(nullptr, onOverlay);
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_addon(module);
      break;
  }
  return TRUE;
}

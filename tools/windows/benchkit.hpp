// Shared parts of OpBench and TexBench (owner, 2026-10-04: TexBench as its own program): console
// output (colors, title box, bars, progress bar with a percentage scale), the redundant-reading
// statistics, adapter selection and GPU timestamps. Include it in exactly one source file per
// program (it defines the switchable-graphics exports).
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <stdexcept>
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

namespace benchkit {

// The program's name in error messages ("OpBench", "TexBench").
inline const char* gProgram = "bench";

// The test being measured, named in error messages (a GPU that stops responding names its test), and the GPU
// (both in the window title).
inline std::string gCurrent;
inline std::string gGpu;

// The window title: "<program> - <GPU> - <state>" (owner: the GPU in the title; "done" at the end).
inline void setTitle(const std::string& state) {
  std::string t = gProgram;
  if (!gGpu.empty()) t += "  -  " + gGpu;
  if (!state.empty()) t += "  -  " + state;
  SetConsoleTitleA(t.c_str());
}

// Set on BackgroundJobs' worker thread: fail() there throws, and the job's error is reported by the main
// thread when it waits for that job.
inline thread_local bool gWorker = false;

[[noreturn]] inline void fail(const std::string& what) {
  if (gWorker) throw std::runtime_error(what);
  setTitle("stopped (error)");
  std::fprintf(stderr, "\n%s: %s\n", gProgram, what.c_str());
  if (!gCurrent.empty()) std::fprintf(stderr, "%s: while measuring \"%s\"\n", gProgram, gCurrent.c_str());
  std::exit(1);
}

inline std::string narrow(const wchar_t* w) {
  char buf[512];
  WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
  return buf;
}

// 1048576 -> "1,048,576".
inline std::string withCommas(unsigned long long v) {
  std::string s = std::to_string(v);
  for (int k = int(s.size()) - 3; k > 0; k -= 3) s.insert(size_t(k), ",");
  return s;
}

inline double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v.empty() ? 0.0 : v[v.size() / 2];
}

// ---------------------------------------------------------------------------
// Redundant readings (OpBench version 4): every test is read at least twice; readings that
// disagree call for more passes until more than half agree, like redundant sensors.

constexpr int kMaxPasses = 6;

// Two readings agree within absTol units or 15%.
inline bool agree(double a, double b, double absTol = 0.75) {
  return std::fabs(a - b) <= std::max(absTol, 0.15 * std::max(std::fabs(a), std::fabs(b)));
}

struct Consensus {
  bool ok = false;     // more than half of the readings agree
  double value = 0.0;  // their mean (without a majority: the median of all)
};

inline Consensus consensus(std::vector<double> v, double absTol = 0.75) {
  Consensus c;
  if (v.empty()) return c;
  std::sort(v.begin(), v.end());
  size_t best = 0, bestLo = 0;
  for (size_t lo = 0; lo < v.size(); ++lo)
    for (size_t hi = lo; hi < v.size() && agree(v[lo], v[hi], absTol); ++hi)
      if (hi - lo + 1 > best) {
        best = hi - lo + 1;
        bestLo = lo;
      }
  c.ok = 2 * best > v.size();
  if (c.ok) {
    for (size_t k = bestLo; k < bestLo + best; ++k) c.value += v[k] / double(best);
  } else {
    c.value = v[v.size() / 2];
  }
  return c;
}

// Where the programs write what testers send (owner, 2026-10-05: one folder instead of files next to
// the programs): Reports\ next to the exe, created on first use; shader dumps go to Reports\Shaders\.
inline std::filesystem::path reportsDir() {
  char exePath[MAX_PATH];
  GetModuleFileNameA(nullptr, exePath, MAX_PATH);
  // The release zip keeps the programs in bin\ (owner, 2026-10-05: only the menu and the guide in the main folder);
  // Reports\ goes next to GPU-Blueprint.bat then.
  std::filesystem::path base = std::filesystem::path(exePath).parent_path();
  if (_stricmp(base.filename().string().c_str(), "bin") == 0) base = base.parent_path();
  const std::filesystem::path dir = base / "Reports";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}

// ---------------------------------------------------------------------------
// Console output: ANSI colors and UTF-8 box / bar characters where the console supports virtual
// terminal sequences (Windows 10+), plain ASCII otherwise.

struct Style {
  bool vt = false;
  const char* c(const char* code) const { return vt ? code : ""; }
  const char* reset() const { return c("\x1b[0m"); }
};

inline Style initConsole() {
  Style st;
  // Keys pressed while the program ran are dropped at exit, so they do not reach the next prompt
  // (GPU-Blueprint.bat's choice beeped at them after the run).
  std::atexit([] { FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE)); });
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) &&
      SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
    st.vt = true;
    SetConsoleOutputCP(CP_UTF8);
    // The caret (text cursor) is hidden while the program runs (owner: it blinks at the progress bar) and
    // shown again at exit, after an error (fail exits) and on Ctrl+C / closing the window.
    std::fputs("\x1b[?25l", stdout);
    std::atexit([] { std::fputs("\x1b[?25h", stdout); std::fflush(stdout); });
    SetConsoleCtrlHandler(
        [](DWORD) -> BOOL {
          std::fputs("\x1b[?25h", stdout);
          std::fflush(stdout);
          return FALSE;  // then the default handling (the program ends)
        },
        TRUE);
  }
  return st;
}

// Block graphics use only the full block and the half blocks (code page 437, in every console font;
// the 1/8 blocks of OpBench version 3 showed as boxes). A color is a bright / dark pair of the 16
// console colors, and the dark one as a background gives 4 levels per cell (owner's design,
// 2026-10-03): space, left half dark, left half bright, left half bright on dark, full bright.
constexpr const char* kFull = "█";
constexpr const char* kLeft = "▌";
struct Shade {
  int bright, dark;  // foreground codes; the dark background is dark + 10
};
constexpr Shade kRed = {91, 31}, kYellow = {93, 33}, kWhite = {97, 90}, kCyan = {96, 36}, kGreen = {92, 32};

// A comment and a color for a throughput cost (extra over the base, 4 = one fma).
inline const char* costComment(double v, Shade* shade) {
  if (v < 0.75) { *shade = kGreen; return "free"; }
  if (v < 3.0) { *shade = kCyan; return "cheap"; }
  if (v < 5.5) { *shade = kWhite; return "one op"; }
  if (v < 9.0) { *shade = kYellow; return "two ops"; }
  *shade = kRed;
  return "expensive";
}

// One cell of a bar at level 0..4.
inline std::string barCell(int level, Shade c) {
  char buf[48];
  switch (level) {
    case 0: return " ";
    case 1: std::snprintf(buf, sizeof(buf), "\x1b[%dm%s\x1b[0m", c.dark, kLeft); break;
    case 2: std::snprintf(buf, sizeof(buf), "\x1b[%dm%s\x1b[0m", c.bright, kLeft); break;
    case 3: std::snprintf(buf, sizeof(buf), "\x1b[%d;%dm%s\x1b[0m", c.bright, (c.dark == 90 ? 100 : c.dark + 10), kLeft); break;
    default: std::snprintf(buf, sizeof(buf), "\x1b[%dm%s\x1b[0m", c.bright, kFull); break;
  }
  return buf;
}

// A bar of width cells for v out of maxV (at least one level when v > 0); returns its text, the
// display width is always `width`.
inline std::string bar(double v, double maxV, int width, const Style& st, Shade c) {
  const int levels = maxV <= 0.0 ? 0 : int(std::lround(std::max(0.0, v) / maxV * width * 4));
  const int n = std::min(width * 4, v > 0.0 ? std::max(1, levels) : 0);
  std::string s;
  for (int k = 0; k < width; ++k) {
    const int lv = std::min(4, std::max(0, n - 4 * k));
    s += st.vt ? barCell(lv, c) : std::string(lv >= 2 ? "#" : " ");
  }
  return s;
}

// The progress bar while measuring: one level per measurement, 6 levels per cell (with light grey).
// The planned measurements fill the scale from 0% to 100%; extra passes for tests whose readings
// disagree run on past 100% in yellow.
constexpr int kStepsPerCell = 6;

// Steps per cell of the progress bar: at least kStepsPerCell, more for long runs so the bar stays within
// kMaxCells (a console window's width).
constexpr int kMaxCells = 70;
inline int stepsPerCell(int planned) { return std::max(kStepsPerCell, (planned + kMaxCells - 1) / kMaxCells); }

inline std::string progressScale(int planned);

// Progress of the measurements. On a terminal the bar is drawn only when a section is printed (owner,
// 2026-10-04: every update made the console jump back to the bottom, so nothing above could be read while
// it ran); in between, the window title shows the percentage and the test being measured. Without a
// terminal (output to a file) the bar grows as before ('#' per cell).
struct Progress {
  const Style* st;
  int planned = 0;
  int steps = 0;
  bool live = true;  // until the first section is printed nothing is above to read: the bar updates every step
  void step() {
    ++steps;
    if (st->vt) {
      title();
      if (live) {
        std::printf("\r   ");
        draw();
      }
      return;
    }
    if (steps % stepsPerCell(planned) == 0) std::printf(steps > planned ? "+" : "#");
  }
  void title() const {
    const int pct = planned ? std::min(100, 100 * steps / planned) : 100;
    setTitle(std::to_string(pct) + "%" + (gCurrent.empty() ? "" : "  " + gCurrent));
  }
  // The bar for the steps so far: full cells, then the last cell's fill level (6 levels), yellow past 100%.
  void draw() const {
    if (!st->vt) return;
    static const char* const kCell[6] = {"\x1b[90m▌", "\x1b[37m▌", "\x1b[97m▌",
                                         "\x1b[97;100m▌", "\x1b[97;47m▌", "\x1b[97m█"};
    static const char* const kExtra[6] = {"\x1b[33m▌", "\x1b[33m▌", "\x1b[93m▌",
                                          "\x1b[93;43m▌", "\x1b[93;43m▌", "\x1b[93m█"};
    const int spc = stepsPerCell(planned);
    for (int cell = 0; cell * spc < steps; ++cell) {
      const int inCell = std::min(spc, steps - cell * spc);  // steps done in this cell, 1 .. spc
      const int sub = (inCell - 1) * kStepsPerCell / spc;
      std::printf("%s\x1b[0m", ((cell + 1) * spc > planned ? kExtra : kCell)[sub]);
    }
  }
  // The bar sits under a blank line and the percentage scale ("\n   scale\n   bar"). pause() takes the
  // three lines off a terminal (elsewhere it ends the bar's line), so a finished section can be printed
  // where they were; resume() draws them again below it, filled as far as the run got (owner: results
  // per section while the rest is measured).
  void start() const {
    std::printf("\n   %s%s%s\n   ", st->c("\x1b[90m"), progressScale(planned).c_str(), st->reset());
  }
  void pause() {
    live = false;
    std::printf(st->vt ? "\r\x1b[2K\x1b[1A\x1b[2K\x1b[1A\x1b[2K" : "\n");
  }
  void resume() const {
    start();
    if (st->vt) draw();
    else
      for (int k = stepsPerCell(planned); k <= steps; k += stepsPerCell(planned)) std::printf(k > planned ? "+" : "#");
  }
};

// The percentage scale above the progress bar: 0% at its first cell, 100% at its last cell (where the
// planned steps end). Each label's marking digit sits on its cell (owner): the 0 of 0%, the 5 of 25%,
// the 0 of 50%, the 5 of 75% and the first 0 of 100%.
inline std::string scaleLine(int cells, bool quarters) {
  std::string s(static_cast<size_t>(cells) + 4, ' ');
  auto put = [&](int q) {
    const std::string label = std::to_string(q) + "%";
    const int digit = q == 0 ? 0 : 1;
    const int at = ((cells - 1) * q + 50) / 100 - digit;
    if (at >= 0 && at + label.size() <= s.size()) s.replace(static_cast<size_t>(at), label.size(), label);
  };
  put(0);
  if (quarters)
    for (int q : {25, 50, 75}) put(q);
  put(100);
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

inline std::string progressScale(int planned) {
  const int spc = stepsPerCell(planned);
  const int cells = (planned + spc - 1) / spc;
  if (cells < 8) return {};
  return scaleLine(cells, cells >= 24);  // room for the quarters
}

// Display width of a UTF-8 string (one column per code point).
inline int columns(const std::string& s) {
  int n = 0;
  for (unsigned char ch : s) n += (ch & 0xC0) != 0x80;
  return n;
}


// Jobs (shader compiles) run in order on one worker thread below normal priority (owner, 2026-10-05: the
// next section's shaders compile while the current one is measured, so results start sooner); wait(i)
// returns once job i is done and fails with its error on the calling thread. Win32 threads: the mingw build
// has no std::thread. D3DCompile and the D3D11 device (not created single-threaded) are free-threaded.
class BackgroundJobs {
 public:
  explicit BackgroundJobs(std::vector<std::function<void()>> jobs) : jobs_(std::move(jobs)), errors_(jobs_.size()) {
    InitializeCriticalSection(&cs_);
    InitializeConditionVariable(&cv_);
    thread_ = CreateThread(nullptr, 0, &BackgroundJobs::threadMain, this, 0, nullptr);
    if (thread_) SetThreadPriority(thread_, THREAD_PRIORITY_BELOW_NORMAL);
    else run();  // no thread: everything now, on this thread
  }
  ~BackgroundJobs() {
    if (thread_) {
      EnterCriticalSection(&cs_);
      stop_ = true;
      LeaveCriticalSection(&cs_);
      WaitForSingleObject(thread_, INFINITE);
      CloseHandle(thread_);
    }
    DeleteCriticalSection(&cs_);
  }
  BackgroundJobs(const BackgroundJobs&) = delete;
  BackgroundJobs& operator=(const BackgroundJobs&) = delete;
  size_t size() const { return jobs_.size(); }
  void wait(size_t i) {
    if (i >= jobs_.size()) return;
    EnterCriticalSection(&cs_);
    while (done_ <= i) SleepConditionVariableCS(&cv_, &cs_, INFINITE);
    const std::string error = errors_[i];
    LeaveCriticalSection(&cs_);
    if (!error.empty()) fail(error);
  }

 private:
  static DWORD WINAPI threadMain(void* self) {
    gWorker = true;
    static_cast<BackgroundJobs*>(self)->run();
    return 0;
  }
  void run() {
    for (size_t i = 0; i < jobs_.size(); ++i) {
      EnterCriticalSection(&cs_);
      const bool stop = stop_;
      LeaveCriticalSection(&cs_);
      if (stop) break;
      std::string error;
      try {
        jobs_[i]();
      } catch (const std::exception& e) {
        error = e.what();
      }
      EnterCriticalSection(&cs_);
      errors_[i] = error;
      done_ = i + 1;
      WakeAllConditionVariable(&cv_);
      LeaveCriticalSection(&cs_);
    }
  }
  std::vector<std::function<void()>> jobs_;
  std::vector<std::string> errors_;
  CRITICAL_SECTION cs_;
  CONDITION_VARIABLE cv_;
  HANDLE thread_ = nullptr;
  size_t done_ = 0;
  bool stop_ = false;
};

// The console window's width in characters (120 when the output is not a console), so summary lines
// can be fitted to it instead of wrapping.
inline int consoleColumns() {
  CONSOLE_SCREEN_BUFFER_INFO info;
  const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (out && out != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(out, &info)) {
    const int w = info.srWindow.Right - info.srWindow.Left + 1;
    if (w >= 40) return w;
  }
  return 120;
}

// A double-line box around a title (bright cyan frame, bright white title; ASCII without VT).
inline void printBox(const Style& st, const std::string& title) {
  const std::string hz = st.vt ? "═" : "=";
  std::string line;
  for (int k = 0; k < columns(title) + 4; ++k) line += hz;
  std::printf("  %s%s%s%s%s\n", st.c("\x1b[1;96m"), st.vt ? "╔" : "+", line.c_str(), st.vt ? "╗" : "+", st.reset());
  std::printf("  %s%s%s  %s%s%s  %s%s%s\n", st.c("\x1b[1;96m"), st.vt ? "║" : "|", st.reset(), st.c("\x1b[1;97m"),
              title.c_str(), st.reset(), st.c("\x1b[1;96m"), st.vt ? "║" : "|", st.reset());
  std::printf("  %s%s%s%s%s\n", st.c("\x1b[1;96m"), st.vt ? "╚" : "+", line.c_str(), st.vt ? "╝" : "+", st.reset());
}

// Display width of a line that may hold color codes.
inline int visibleColumns(const std::string& s) {
  int n = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\x1b') {
      while (i < s.size() && !std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
      continue;
    }
    n += (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80;
  }
  return n;
}

// A number in large digits, 3 rows of full / half blocks (the characters the progress bars use).
inline std::vector<std::string> bigNumber(const std::string& text) {
  static const char* const kDigit[10][3] = {
      {"█▀█", "█ █", "█▄█"}, {"▄█ ", " █ ", "▄█▄"}, {"▀▀█", "█▀▀", "█▄▄"}, {"▀▀█", " ▀█", "▄▄█"}, {"█ █", "▀▀█", "  █"},
      {"█▀▀", "▀▀█", "▄▄█"}, {"█▀▀", "█▀█", "█▄█"}, {"▀▀█", "  █", "  █"}, {"█▀█", "█▀█", "█▄█"}, {"█▀█", "▀▀█", "▄▄█"}};
  std::vector<std::string> rows(3);
  for (char ch : text)
    for (int r = 0; r < 3; ++r) {
      if (ch >= '0' && ch <= '9') rows[r] += std::string(kDigit[ch - '0'][r]) + " ";
      else if (ch == '.') rows[r] += r == 2 ? "▄ " : "  ";
      else rows[r] += "  ";
    }
  return rows;
}

// A value with three significant digits ("5.03", "48.7", "157").
inline std::string threeDigits(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), v < 9.995 ? "%.2f" : v < 99.95 ? "%.1f" : "%.0f", v);
  return buf;
}

inline const char* vendorColor(const Style& st, UINT vendor);

// The score box at the end of a run (owner, 2026-10-04: a number users can show others, jazzed up):
// a title, the headline value in large yellow digits with its unit beside them, then more values.
inline void printScore(const Style& st, const std::string& title, const std::string& gpu, UINT vendor, double headline,
                       const std::string& unit, const std::string& what,
                       const std::vector<std::pair<std::string, std::string>>& more) {
  const std::string value = threeDigits(headline);
  if (!st.vt) {
    std::printf("\n  %s - %s\n  %s %s (%s)\n", title.c_str(), gpu.c_str(), value.c_str(), unit.c_str(), what.c_str());
    for (const auto& [k, v] : more) std::printf("  %-26s %s\n", k.c_str(), v.c_str());
    return;
  }
  std::vector<std::string> lines;
  lines.push_back(std::string("\x1b[1;97m") + title + "\x1b[0m   " + vendorColor(st, vendor) + gpu + "\x1b[0m");
  lines.emplace_back();
  const std::vector<std::string> big = bigNumber(value);
  lines.push_back("\x1b[1;93m" + big[0] + "\x1b[0m");
  lines.push_back("\x1b[1;93m" + big[1] + "\x1b[0m  \x1b[1;97m" + unit + "\x1b[0m");
  lines.push_back("\x1b[1;93m" + big[2] + "\x1b[0m  \x1b[90m" + what + "\x1b[0m");
  if (!more.empty()) lines.emplace_back();
  for (const auto& [k, v] : more) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%-26s \x1b[1;97m%s\x1b[0m", k.c_str(), v.c_str());
    lines.push_back(buf);
  }
  int width = 0;
  for (const std::string& l : lines) width = std::max(width, visibleColumns(l));
  std::string hz;
  for (int k = 0; k < width + 6; ++k) hz += "═";
  std::printf("\n  \x1b[1;96m╔%s╗\x1b[0m\n", hz.c_str());
  for (const std::string& l : lines)
    std::printf("  \x1b[1;96m║\x1b[0m   %s%*s   \x1b[1;96m║\x1b[0m\n", l.c_str(), width - visibleColumns(l), "");
  std::printf("  \x1b[1;96m╚%s╝\x1b[0m\n", hz.c_str());
}

// ---------------------------------------------------------------------------
// Adapter selection: the discrete GPU with the most memory unless --adapter N picks another.

struct Adapter {
  IDXGIAdapter1* adapter = nullptr;
  DXGI_ADAPTER_DESC1 desc = {};
  std::string name, driver = "unknown";
};

// A GPU name in its vendor's logo color (owner): NVIDIA bright green, AMD bright red, Intel bright blue.
inline const char* vendorColor(const Style& st, UINT vendor) {
  return st.c(vendor == 0x10DE ? "\x1b[1;92m" : vendor == 0x1002 || vendor == 0x1022 ? "\x1b[1;91m"
              : vendor == 0x8086                    ? "\x1b[1;94m"
                                                    : "\x1b[1m");
}

// --adapters: the index of every hardware GPU, one per line, each GPU once (a driver can list the same GPU
// twice, e.g. the NVIDIA card first because of NvOptimusEnablement and again at its own place: same LUID)
// and without software adapters (Microsoft Basic Render Driver). GPU-Blueprint.bat (every card) runs each index.
inline void printUniqueAdapters() {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) fail("CreateDXGIFactory1 failed");
  // The same GPU can be listed twice (the high-performance GPU again at the front, owner's GTX 1660: 0 1 2), not
  // always with the same LUID: also the same vendor, device, subsystem, revision and video memory count as one
  // (two identical cards in one PC are then measured once).
  std::vector<DXGI_ADAPTER_DESC1> seen;
  for (UINT k = 0;; ++k) {
    IDXGIAdapter1* ad = nullptr;
    if (factory->EnumAdapters1(k, &ad) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 d;
    ad->GetDesc1(&d);
    ad->Release();
    if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
    if (d.VendorId == 0x1414 && d.DeviceId == 0x8c) continue;  // Microsoft Basic Render Driver
    bool dup = false;
    for (const DXGI_ADAPTER_DESC1& e : seen)
      dup |= (e.AdapterLuid.LowPart == d.AdapterLuid.LowPart && e.AdapterLuid.HighPart == d.AdapterLuid.HighPart) ||
             (e.VendorId == d.VendorId && e.DeviceId == d.DeviceId && e.SubSysId == d.SubSysId && e.Revision == d.Revision &&
              e.DedicatedVideoMemory == d.DedicatedVideoMemory);
    if (dup) continue;
    seen.push_back(d);
    std::printf("%u\n", k);
  }
  factory->Release();
}

// With list set, prints the adapters and returns an empty Adapter. Otherwise prints the chosen GPU
// and the other adapters ("Use --adapter N to test this").
inline Adapter selectAdapter(const Style& st, bool list, int index) {
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
  std::vector<std::string> names;
  std::vector<UINT> vendors;
  for (size_t k = 0; k < adapters.size(); ++k) {
    DXGI_ADAPTER_DESC1 d;
    adapters[k]->GetDesc1(&d);
    vendors.push_back(d.VendorId);
    const bool software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    names.push_back(narrow(d.Description) + (software ? " [software]" : ""));
    if (list)
      std::printf("%zu: %s%s%s (%zu MB)%s\n", k, vendorColor(st, d.VendorId), narrow(d.Description).c_str(), st.reset(),
                  size_t(d.DedicatedVideoMemory >> 20), software ? " [software]" : "");
    if (!software && (pick < 0 || d.DedicatedVideoMemory > bestMem)) {
      pick = int(k);
      bestMem = d.DedicatedVideoMemory;
    }
  }
  if (list) return {};
  if (index >= 0) pick = index;
  if (pick < 0 || pick >= int(adapters.size())) fail("no GPU adapter (try --list)");

  Adapter a;
  a.adapter = adapters[size_t(pick)];
  a.adapter->GetDesc1(&a.desc);
  a.name = narrow(a.desc.Description);
  LARGE_INTEGER umd = {};
  if (SUCCEEDED(a.adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", unsigned(HIWORD(umd.HighPart)), unsigned(LOWORD(umd.HighPart)),
                  unsigned(HIWORD(umd.LowPart)), unsigned(LOWORD(umd.LowPart)));
    a.driver = buf;
  }
  gGpu = a.name;
  setTitle("");
  std::printf("%sGPU:%s %s%s%s (vendor 0x%04X, device 0x%04X), driver %s\n", st.c("\x1b[1m"), st.reset(),
              vendorColor(st, a.desc.VendorId), a.name.c_str(), st.reset(), a.desc.VendorId, a.desc.DeviceId,
              a.driver.c_str());
  if (adapters.size() > 1) {
    std::printf("Also detected in system:\n");
    size_t nameW = 0;
    for (size_t k = 0; k < adapters.size(); ++k)
      if (int(k) != pick) nameW = std::max(nameW, names[k].size());
    for (size_t k = 0; k < adapters.size(); ++k)
      if (int(k) != pick)
        std::printf("  %zu: %s%-*s%s   %sUse --adapter %zu to test this%s\n", k, vendorColor(st, vendors[k]), int(nameW),
                    names[k].c_str(), st.reset(), st.c("\x1b[90m"), k, st.reset());
  }
  return a;
}

// GPU timestamps around a piece of work: returns milliseconds, or a negative value when the
// timestamps were disjoint (the clock changed in between).
struct Timer {
  ID3D11Query* disjoint = nullptr;
  ID3D11Query* t0 = nullptr;
  ID3D11Query* t1 = nullptr;
  void create(ID3D11Device* dev) {
    D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    dev->CreateQuery(&qd, &disjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    dev->CreateQuery(&qd, &t0);
    dev->CreateQuery(&qd, &t1);
  }
  // The work's GPU time in ms. A disjoint reading (the GPU clock or power state changed during it: Intel
  // iGPUs do this) is run again, up to 4 times in all; -1 when every try was disjoint.
  template <class Work>
  double time(ID3D11DeviceContext* ctx, Work work) {
    for (int attempt = 0; attempt < 4; ++attempt)
      if (const double ms = timeOnce(ctx, work); ms >= 0.0) return ms;
    return -1.0;
  }
  template <class Work>
  double timeOnce(ID3D11DeviceContext* ctx, Work work) {
    ctx->Begin(disjoint);
    ctx->End(t0);
    work();
    ctx->End(t1);
    ctx->End(disjoint);
    // Waits for a query; a GPU that stopped responding (driver reset: the device is removed, every query fails)
    // or a measurement still unfinished after 60 seconds ends the program with a message instead of hanging.
    const ULONGLONG start = GetTickCount64();
    auto wait = [&](ID3D11Query* q, void* data, UINT size) {
      for (;;) {
        const HRESULT hr = ctx->GetData(q, data, size, 0);
        if (hr == S_OK) return;
        if (FAILED(hr)) {
          ID3D11Device* dev = nullptr;
          ctx->GetDevice(&dev);
          char buf[160];
          std::snprintf(buf, sizeof(buf), "the GPU stopped responding (0x%08lX, device removed reason 0x%08lX)",
                        static_cast<unsigned long>(hr), dev ? static_cast<unsigned long>(dev->GetDeviceRemovedReason()) : 0ul);
          fail(buf);
        }
        if (GetTickCount64() - start > 60000) fail("the GPU has not finished a measurement in 60 seconds");
        Sleep(0);
      }
    };
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    wait(disjoint, &dj, sizeof(dj));
    UINT64 a = 0, b = 0;
    wait(t0, &a, sizeof(a));
    wait(t1, &b, sizeof(b));
    if (dj.Disjoint || dj.Frequency == 0) return -1.0;
    return double(b - a) * 1000.0 / double(dj.Frequency);
  }
};

}  // namespace benchkit

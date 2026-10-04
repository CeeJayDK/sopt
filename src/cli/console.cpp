#include "cli/console.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace sopt::console {

namespace {

// UTF-8 bytes spelled out (MSVC without /utf-8 would turn \u escapes into '?').
const char* const kFull = "\xE2\x96\x88";   // U+2588 full block
const char* const kHalf = "\xE2\x96\x8C";   // U+258C left half block

int columns(const std::string& s) {
  int n = 0;
  for (unsigned char ch : s) n += (ch & 0xC0) != 0x80;
  return n;
}

std::string repeat(const char* s, int n) {
  std::string out;
  for (int k = 0; k < n; ++k) out += s;
  return out;
}

}  // namespace

Style init() {
  Style st;
  const char* noColor = std::getenv("NO_COLOR");
  if (noColor && *noColor) return st;
#ifdef _WIN32
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) &&
      SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
    st.vt = true;
    SetConsoleOutputCP(CP_UTF8);
  }
#else
  const char* term = std::getenv("TERM");
  st.vt = isatty(1) && !(term && std::strcmp(term, "dumb") == 0);
#endif
  return st;
}

void titleBox(const Style& st, const std::string& title) {
  const std::string hz = repeat(st.vt ? "\xE2\x95\x90" : "=", columns(title) + 4);  // U+2550
  std::printf("  %s%s%s%s%s\n", st.c("\x1b[1;96m"), st.vt ? "\xE2\x95\x94" : "+", hz.c_str(),
              st.vt ? "\xE2\x95\x97" : "+", st.reset());
  std::printf("  %s%s%s  %s%s%s  %s%s%s\n", st.c("\x1b[1;96m"), st.vt ? "\xE2\x95\x91" : "|", st.reset(),
              st.c("\x1b[1;97m"), title.c_str(), st.reset(), st.c("\x1b[1;96m"), st.vt ? "\xE2\x95\x91" : "|",
              st.reset());
  std::printf("  %s%s%s%s%s\n", st.c("\x1b[1;96m"), st.vt ? "\xE2\x95\x9A" : "+", hz.c_str(),
              st.vt ? "\xE2\x95\x9D" : "+", st.reset());
  std::fflush(stdout);
}

void section(const Style& st, const std::string& title) {
  std::printf("\n%s== %s%s\n", st.c("\x1b[1;96m"), title.c_str(), st.reset());
  std::fflush(stdout);
}

Progress::Progress(const Style& st, size_t total, int width)
    : st_(st), total_(total), width_(width), start_(std::chrono::steady_clock::now()) {
  if (!st_.vt || total_ == 0) return;
  // The scale: 0% at the bar's first cell, 100% at its last. Each label's marking digit sits on its cell
  // (owner): the 0 of 0%, the 5 of 25%, the 0 of 50%, the 5 of 75% and the first 0 of 100%.
  std::string s(static_cast<size_t>(width_) + 4, ' ');
  for (int q : {0, 25, 50, 75, 100}) {
    const std::string label = std::to_string(q) + "%";
    const int at = ((width_ - 1) * q + 50) / 100 - (q == 0 ? 0 : 1);
    if (at >= 0 && at + label.size() <= s.size()) s.replace(static_cast<size_t>(at), label.size(), label);
  }
  while (!s.empty() && s.back() == ' ') s.pop_back();
  std::printf("   %s%s%s\n", st_.c("\x1b[90m"), s.c_str(), st_.reset());
  open_ = true;
  draw();
}

void Progress::step() {
  if (!open_) return;
  std::lock_guard<std::mutex> lock(mu_);
  ++done_;
  draw();
}

void Progress::draw() {
  // Half-block resolution: 2 levels per cell.
  const size_t halves = total_ ? std::min<size_t>(done_ * 2 * width_ / total_, 2 * width_) : 0;
  const int full = static_cast<int>(halves / 2);
  const bool half = halves % 2 == 1;
  std::string bar = std::string("\x1b[96m") + repeat(kFull, full);
  if (half) bar += kHalf;
  bar += "\x1b[90m" + repeat(kFull, width_ - full - (half ? 1 : 0)) + "\x1b[0m";
  char info[96];
  const int pct = total_ ? static_cast<int>(100 * done_ / total_) : 100;
  int len = std::snprintf(info, sizeof(info), " %3d%%  %zu/%zu", pct, done_, total_);
  if (done_ > 0 && done_ < total_) {
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    const long left = static_cast<long>(sec / done_ * (total_ - done_) + 0.5);
    std::snprintf(info + len, sizeof(info) - len, "  ~%ld:%02ld left", left / 60, left % 60);
  }
  std::printf("\r   %s%s\x1b[K", bar.c_str(), info);
  std::fflush(stdout);
}

void Progress::print(const std::string& line) {
  if (!open_) {
    std::printf("%s\n", line.c_str());
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  std::printf("\r\x1b[K%s\n", line.c_str());
  draw();
}

void Progress::finish() {
  if (!open_) return;
  std::lock_guard<std::mutex> lock(mu_);
  done_ = total_;
  draw();
  std::printf("\n");
  std::fflush(stdout);
  open_ = false;
}

}  // namespace sopt::console

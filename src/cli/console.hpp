#pragma once
// Terminal output of sopt / sopt-fx (owner, 2026-10-04: a title box, colors and a progress bar like
// OpBench's). Colors and block graphics only on an interactive console (ANSI escape codes; on Windows
// with virtual terminal processing); redirected output stays plain text, so logs read as before.
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>

namespace sopt::console {

struct Style {
  bool vt = false;  // ANSI colors and UTF-8 block graphics
  const char* c(const char* code) const { return vt ? code : ""; }
  const char* reset() const { return c("\x1b[0m"); }
};

// Detects an interactive console (stdout a terminal, NO_COLOR unset, TERM not "dumb") and switches
// it to ANSI / UTF-8 output on Windows.
Style init();

// "  SweetOpt 0.6.0  -  the super sweet shader optimizer  -  by CeeJay.dk" in a double-line box (cyan frame; ASCII without colors).
void titleBox(const Style& st, const std::string& title);

// A section heading: "== Searching 318 regions" (bright cyan).
void section(const Style& st, const std::string& title);

// A progress bar for a known number of steps with a percentage scale above it and the time left
// (from the average time per step so far). Thread safe; does nothing without colors.
class Progress {
 public:
  Progress(const Style& st, size_t total, int width = 40);
  void step();    // one more step done
  void print(const std::string& line);  // a line of output above the bar (plain without colors)
  void finish();  // ends the bar's line
 private:
  void draw();
  Style st_;
  size_t total_, done_ = 0;
  int width_;
  bool open_ = false;
  std::chrono::steady_clock::time_point start_;
  std::mutex mu_;
};

}  // namespace sopt::console

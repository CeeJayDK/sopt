#include "measure/tools.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace sopt {

std::string runCommand(const std::string& cmd, int& status) {
#ifdef _WIN32
  const std::string full = "\"" + cmd + "\"";  // cmd /c strips one pair of outer quotes
#else
  const std::string& full = cmd;
#endif
  std::string out;
  FILE* p = popen(full.c_str(), "r");
  if (!p) {
    status = -1;
    return out;
  }
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
  status = pclose(p);
  return out;
}

std::string quote(const std::string& s) { return "\"" + s + "\""; }

std::filesystem::path makeWorkDir(const std::string& keepDir, const char* prefix,
                                  std::string& error) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path dir;
  if (!keepDir.empty()) {
    dir = keepDir;
  } else {
    std::random_device rd;
    dir = fs::temp_directory_path(ec) / (std::string(prefix) + std::to_string(rd()));
  }
  fs::create_directories(dir, ec);
  if (ec) {
    error = "cannot create " + dir.string() + ": " + ec.message();
    return {};
  }
  return dir;
}

void parallelFor(size_t n, unsigned threads, const std::function<void(size_t)>& fn) {
  if (n == 0) return;
  if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
  threads = static_cast<unsigned>(std::min<size_t>(threads, n));
  std::atomic<size_t> next{0};
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < threads; ++t)
    pool.emplace_back([&] {
      for (size_t i = next++; i < n; i = next++) fn(i);
    });
  for (auto& th : pool) th.join();
}

std::string firstLines(const std::string& s, int lines) {
  size_t pos = 0;
  for (int i = 0; i < lines && pos != std::string::npos; ++i) {
    pos = s.find('\n', pos);
    if (pos != std::string::npos) ++pos;
  }
  std::string out = s.substr(0, pos == std::string::npos ? s.size() : pos);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

}  // namespace sopt

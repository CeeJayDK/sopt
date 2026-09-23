#pragma once
#include <filesystem>
#include <functional>
#include <string>

namespace sopt {

// Runs a shell command and returns its stdout (the caller adds 2>&1 if needed).
std::string runCommand(const std::string& cmd, int& status);
std::string quote(const std::string& s);

// keepDir if given (created, kept), else a fresh temp directory the caller removes.
// Returns an empty path and sets error on failure.
std::filesystem::path makeWorkDir(const std::string& keepDir, const char* prefix,
                                  std::string& error);

// Calls fn(i) for i in [0, n) on up to `threads` threads (0 = hardware concurrency).
void parallelFor(size_t n, unsigned threads, const std::function<void(size_t)>& fn);

std::string firstLines(const std::string& s, int lines);

}  // namespace sopt

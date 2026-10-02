#include "search/diskstore.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#include "zstd.h"

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace sopt {

namespace {
std::atomic<uint64_t> fileCounter{0};

unsigned long processId() {
#if defined(_WIN32)
  return GetCurrentProcessId();
#else
  return static_cast<unsigned long>(getpid());
#endif
}

bool processAlive(unsigned long pid) {
#if defined(_WIN32)
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (!h) return false;
  DWORD code = 0;
  const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
  CloseHandle(h);
  return alive;
#else
  return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}
}  // namespace

DiskFpStore::DiskFpStore(const std::string& dir, size_t tileFloats, size_t blockFloats)
    : tileFloats_(tileFloats), blockFloats_(std::min(blockFloats, tileFloats)) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(dir, ec);
  // Files of processes that died (killed, crashed) are removed; ours carry our process id.
  const unsigned long pid = processId();
  for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    const std::string name = it->path().filename().string();
    unsigned long other = 0;
    if (name.rfind("sopt-bank-", 0) == 0 && std::sscanf(name.c_str(), "sopt-bank-%lu-", &other) == 1 &&
        other != pid && !processAlive(other))
      fs::remove(it->path(), ec);
  }
  ec.clear();
  path_ = (fs::path(dir) / ("sopt-bank-" + std::to_string(pid) + "-" + std::to_string(fileCounter++) + ".tmp")).string();
  file_ = std::fopen(path_.c_str(), "w+b");
  cctx_ = ZSTD_createCCtx();
  dctx_ = ZSTD_createDCtx();
  buf_.reserve(blockFloats_);
}

DiskFpStore::~DiskFpStore() {
  if (file_) std::fclose(file_);
  if (!path_.empty()) {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
  ZSTD_freeCCtx(static_cast<ZSTD_CCtx*>(cctx_));
  ZSTD_freeDCtx(static_cast<ZSTD_DCtx*>(dctx_));
}

void DiskFpStore::writeBlock() {
  if (buf_.empty()) return;
  const size_t src = buf_.size() * sizeof(float);
  cbuf_.resize(ZSTD_compressBound(src));
  const size_t n = ZSTD_compressCCtx(static_cast<ZSTD_CCtx*>(cctx_), cbuf_.data(), cbuf_.size(), buf_.data(), src, 1);
  if (ZSTD_isError(n)) throw std::runtime_error("zstd compression failed");
  std::fseek(file_, 0, SEEK_END);
  const uint64_t at = written_;
  if (std::fwrite(cbuf_.data(), 1, n, file_) != n) throw std::runtime_error("disk bank: write failed (disk full?)");
  open_.push_back({at, static_cast<uint32_t>(n), static_cast<uint32_t>(buf_.size())});
  written_ += n;
  raw_ += src;
  buf_.clear();
}

uint64_t DiskFpStore::append(const float* v, size_t len) {
  if (pos_ + len > tileStart_ + tileFloats_) closeTile();
  const uint64_t off = pos_;
  size_t done = 0;
  while (done < len) {
    const size_t k = std::min(len - done, blockFloats_ - buf_.size());
    buf_.insert(buf_.end(), v + done, v + done + k);
    done += k;
    if (buf_.size() == blockFloats_) writeBlock();
  }
  pos_ += len;
  return off;
}

void DiskFpStore::closeTile() {
  writeBlock();
  if (!open_.empty() || pos_ > tileStart_) {
    tiles_.push_back(std::move(open_));
    open_.clear();
    tileStart_ += tileFloats_;
    pos_ = tileStart_;
  }
  std::fflush(file_);
#if defined(__linux__)
  // Written pages count against a cgroup's memory until written back: write them out and
  // drop them from the page cache (tiles are read back with explicit reads).
  const int fd = fileno(file_);
  fdatasync(fd);
  posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
}

void DiskFpStore::readTile(uint64_t t, float* dst) {
  if (t >= tiles_.size()) throw std::runtime_error("disk bank: tile not written yet");
  size_t at = 0;
  std::vector<char> in;
  for (const Block& b : tiles_[t]) {
    in.resize(b.compressed);
#if defined(_WIN32)
    _fseeki64(file_, static_cast<long long>(b.fileOffset), SEEK_SET);
#else
    fseeko(file_, static_cast<off_t>(b.fileOffset), SEEK_SET);
#endif
    if (std::fread(in.data(), 1, b.compressed, file_) != b.compressed) throw std::runtime_error("disk bank: read failed");
    const size_t n = ZSTD_decompressDCtx(static_cast<ZSTD_DCtx*>(dctx_), dst + at, b.floats * sizeof(float),
                                         in.data(), b.compressed);
    if (ZSTD_isError(n) || n != b.floats * sizeof(float)) throw std::runtime_error("disk bank: corrupt block");
    at += b.floats;
  }
  ++tilesRead_;
}

}  // namespace sopt

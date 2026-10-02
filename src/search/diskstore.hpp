#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace sopt {

// Fingerprints on disk (disk-backed bank, SearchConfig::diskDir; owner 2026-09-28: an
// option for long single-region runs, zstd because the CPU decompresses faster than the
// disk reads). Records are appended as a stream of floats that is cut into tiles of
// tileFloats (a record never straddles two tiles); each tile is written as zstd blocks of
// blockFloats. A record is addressed by its offset in the stream; a whole tile is read
// and decompressed at once.
class DiskFpStore {
 public:
  DiskFpStore(const std::string& dir, size_t tileFloats, size_t blockFloats);
  ~DiskFpStore();
  DiskFpStore(const DiskFpStore&) = delete;
  DiskFpStore& operator=(const DiskFpStore&) = delete;
  bool ok() const { return file_ != nullptr; }

  // Appends a record; returns its stream offset.
  uint64_t append(const float* v, size_t len);
  // Writes what is buffered and starts the next record in a new tile, so that everything
  // appended so far can be read (called at the end of each cost level).
  void closeTile();

  size_t tileFloats() const { return tileFloats_; }
  uint64_t tileOf(uint64_t off) const { return off / tileFloats_; }
  size_t tiles() const { return tiles_.size(); }
  // Decompresses tile t (closed) into dst (tileFloats floats).
  void readTile(uint64_t t, float* dst);

  uint64_t bytesWritten() const { return written_; }
  uint64_t rawBytes() const { return raw_; }
  uint64_t tilesRead() const { return tilesRead_; }

 private:
  struct Block {
    uint64_t fileOffset;
    uint32_t compressed, floats;
  };
  void writeBlock();
  std::string path_;
  std::FILE* file_ = nullptr;
  size_t tileFloats_, blockFloats_;
  std::vector<std::vector<Block>> tiles_;  // closed tiles
  std::vector<Block> open_;                // blocks of the tile being written
  std::vector<float> buf_;                 // current block
  uint64_t pos_ = 0;                       // stream offset of the next float
  uint64_t tileStart_ = 0;                 // stream offset of the tile being written
  uint64_t written_ = 0, raw_ = 0, tilesRead_ = 0;
  std::vector<char> cbuf_;
  void* cctx_ = nullptr;
  void* dctx_ = nullptr;
};

}  // namespace sopt

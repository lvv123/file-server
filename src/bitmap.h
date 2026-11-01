#pragma once

#include <cstdint>
#include <algorithm>
#include <vector>

#include "error.h"

namespace fsrv {

// Compact bit set used for the "received chunks" accounting of an upload
// session. Serialised into the upload_sessions.received_bitmap BLOB column.
class ChunkBitmap {
 public:
  ChunkBitmap() = default;
  explicit ChunkBitmap(uint32_t chunks)
      : chunks_(chunks), bits_((static_cast<size_t>(chunks) + 7) / 8, 0) {}

  static ChunkBitmap from_blob(const std::vector<uint8_t>& blob,
                               uint32_t chunks) {
    ChunkBitmap bm(chunks);
    const size_t n = std::min(bm.bits_.size(), blob.size());
    for (size_t i = 0; i < n; ++i) bm.bits_[i] = blob[i];
    return bm;
  }

  uint32_t chunks() const { return chunks_; }

  const std::vector<uint8_t>& blob() const { return bits_; }

  bool test(uint32_t index) const {
    if (index >= chunks_) return false;
    return (bits_[index / 8] & (1u << (index % 8))) != 0;
  }

  void set(uint32_t index) {
    if (index >= chunks_) throw bad_request("chunk index out of range");
    bits_[index / 8] = static_cast<uint8_t>(bits_[index / 8] | (1u << (index % 8)));
  }

  void clear(uint32_t index) {
    if (index >= chunks_) return;
    bits_[index / 8] =
        static_cast<uint8_t>(bits_[index / 8] & ~(1u << (index % 8)));
  }

  void set_all() {
    for (uint32_t i = 0; i < chunks_; ++i) set(i);
  }

  uint32_t count() const {
    uint32_t n = 0;
    for (uint32_t i = 0; i < chunks_; ++i) {
      if (test(i)) ++n;
    }
    return n;
  }

  bool full() const { return count() == chunks_; }

  std::vector<uint32_t> indices() const {
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < chunks_; ++i) {
      if (test(i)) out.push_back(i);
    }
    return out;
  }

  std::vector<uint32_t> missing() const {
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < chunks_; ++i) {
      if (!test(i)) out.push_back(i);
    }
    return out;
  }

 private:
  uint32_t chunks_ = 0;
  std::vector<uint8_t> bits_;
};

}  // namespace fsrv

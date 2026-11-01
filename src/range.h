#pragma once

#include <cstdint>
#include <string_view>

namespace fsrv {

enum class RangeOutcome {
  Ignore,         // no usable Range header -> serve the whole entity (200)
  Satisfiable,    // serve a 206 for the returned span
  Unsatisfiable,  // syntactically valid but out of bounds -> 416
};

struct ByteRange {
  uint64_t start = 0;
  uint64_t end = 0;  // inclusive
  uint64_t length() const { return end >= start ? end - start + 1 : 0; }
};

// Parses a single-range HTTP Range header against a file of `file_size` bytes.
// Multi-range requests are reported as Unsatisfiable (the API only supports a
// single contiguous range).
RangeOutcome parse_range(std::string_view header, uint64_t file_size,
                         ByteRange& out);

}  // namespace fsrv

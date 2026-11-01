#include "range.h"

#include <cctype>
#include <limits>

#include "util.h"

namespace fsrv {

namespace {

bool parse_u64(std::string_view s, uint64_t& out) {
  if (s.empty()) return false;
  uint64_t value = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
    value = value * 10 + digit;
  }
  out = value;
  return true;
}

std::string_view trim_view(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

}  // namespace

RangeOutcome parse_range(std::string_view header, uint64_t file_size,
                         ByteRange& out) {
  const std::string_view h = trim_view(header);
  if (h.empty()) return RangeOutcome::Ignore;

  const size_t eq = h.find('=');
  if (eq == std::string_view::npos) return RangeOutcome::Ignore;
  if (!ascii_iequals(trim_view(h.substr(0, eq)), "bytes")) {
    return RangeOutcome::Ignore;
  }

  std::string_view spec = trim_view(h.substr(eq + 1));
  if (spec.empty() || spec == "*") return RangeOutcome::Ignore;

  // Only a single range is supported.
  if (spec.find(',') != std::string_view::npos) {
    return RangeOutcome::Unsatisfiable;
  }

  const size_t dash = spec.find('-');
  if (dash == std::string_view::npos) return RangeOutcome::Ignore;

  const std::string_view first = trim_view(spec.substr(0, dash));
  const std::string_view last = trim_view(spec.substr(dash + 1));

  if (first.empty()) {
    // Suffix range: bytes=-N (the final N bytes).
    uint64_t suffix = 0;
    if (!parse_u64(last, suffix)) return RangeOutcome::Ignore;
    if (suffix == 0) return RangeOutcome::Unsatisfiable;
    if (file_size == 0) return RangeOutcome::Unsatisfiable;
    out.start = suffix >= file_size ? 0 : file_size - suffix;
    out.end = file_size - 1;
    return RangeOutcome::Satisfiable;
  }

  uint64_t start = 0;
  if (!parse_u64(first, start)) return RangeOutcome::Ignore;

  if (file_size == 0) return RangeOutcome::Unsatisfiable;

  if (last.empty()) {
    if (start >= file_size) return RangeOutcome::Unsatisfiable;
    out.start = start;
    out.end = file_size - 1;
    return RangeOutcome::Satisfiable;
  }

  uint64_t end = 0;
  if (!parse_u64(last, end)) return RangeOutcome::Ignore;
  if (end < start) return RangeOutcome::Unsatisfiable;
  if (start >= file_size) return RangeOutcome::Unsatisfiable;

  out.start = start;
  out.end = end >= file_size ? file_size - 1 : end;
  return RangeOutcome::Satisfiable;
}

}  // namespace fsrv

#include "util.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <random>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fsrv {

namespace {

std::mutex& log_mutex() {
  static std::mutex m;
  return m;
}

std::string local_timestamp() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto t = system_clock::to_time_t(now);
  const auto ms =
      duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm tmv{};
#if defined(_WIN32)
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                tmv.tm_min, tmv.tm_sec, static_cast<int>(ms));
  return buf;
}

void log_line(const char* level, const std::string& msg) {
  std::lock_guard<std::mutex> lk(log_mutex());
  std::fprintf(stderr, "%s [%s] %s\n", local_timestamp().c_str(), level,
               msg.c_str());
  std::fflush(stderr);
}

uint64_t splitmix64(uint64_t& state) {
  uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

uint64_t& rng_state() {
  static thread_local uint64_t state = [] {
    std::random_device rd;
    uint64_t seed = (static_cast<uint64_t>(rd()) << 32) ^ rd();
#if defined(_WIN32)
    seed ^= static_cast<uint64_t>(GetTickCount64()) << 17;
#endif
    seed ^= static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    return seed ? seed : 0x5DEECE66DULL;
  }();
  return state;
}

}  // namespace

std::string uuid_v4() {
  uint64_t& st = rng_state();
  uint64_t hi = splitmix64(st);
  uint64_t lo = splitmix64(st);
  // Set version (4) and variant (10xx) bits.
  hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
  lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

  static const char* hex = "0123456789abcdef";
  char out[37];
  int pos = 0;
  auto put = [&](uint64_t v, int nibbles) {
    for (int i = nibbles - 1; i >= 0; --i) {
      out[pos++] = hex[(v >> (i * 4)) & 0xF];
    }
  };
  put(hi >> 32, 8);
  out[pos++] = '-';
  put((hi >> 16) & 0xFFFF, 4);
  out[pos++] = '-';
  put(hi & 0xFFFF, 4);
  out[pos++] = '-';
  put(lo >> 48, 4);
  out[pos++] = '-';
  put(lo & 0xFFFFFFFFFFFFULL, 12);
  return std::string(out, static_cast<size_t>(pos));
}

int64_t now_sec() {
  using namespace std::chrono;
  return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

std::string format_time_iso(int64_t unix_sec) {
  const auto t = static_cast<std::time_t>(unix_sec);
  std::tm tmv{};
#if defined(_WIN32)
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                tmv.tm_min, tmv.tm_sec);
  return buf;
}

namespace {

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

bool percent_decode(std::string_view in, std::string& out) {
  out.clear();
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (c == '%') {
      if (i + 2 >= in.size()) return false;
      const int hi = hex_value(in[i + 1]);
      const int lo = hex_value(in[i + 2]);
      if (hi < 0 || lo < 0) return false;
      out.push_back(static_cast<char>((hi << 4) | lo));
      i += 2;
    } else {
      out.push_back(c);
    }
  }
  return true;
}

std::string ascii_lower(std::string_view in) {
  std::string s(in);
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

bool ascii_iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char ca = a[i], cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
    if (ca != cb) return false;
  }
  return true;
}

bool ascii_istarts_with(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) return false;
  return ascii_iequals(s.substr(0, prefix.size()), prefix);
}

std::string trim(std::string_view in) {
  size_t b = 0, e = in.size();
  while (b < e && (in[b] == ' ' || in[b] == '\t' || in[b] == '\r' ||
                   in[b] == '\n')) {
    ++b;
  }
  while (e > b && (in[e - 1] == ' ' || in[e - 1] == '\t' || in[e - 1] == '\r' ||
                   in[e - 1] == '\n')) {
    --e;
  }
  return std::string(in.substr(b, e - b));
}

bool is_valid_utf8(std::string_view in) {
  size_t i = 0;
  const size_t n = in.size();
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    size_t extra = 0;
    unsigned int cp = 0;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      extra = 1;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      extra = 2;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      extra = 3;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + extra >= n) return false;
    for (size_t k = 1; k <= extra; ++k) {
      const unsigned char cc = static_cast<unsigned char>(in[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    // Reject overlong encodings and surrogates.
    if (extra == 1 && cp < 0x80) return false;
    if (extra == 2 && cp < 0x800) return false;
    if (extra == 3 && cp < 0x10000) return false;
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;
    if (cp > 0x10FFFF) return false;
    i += extra + 1;
  }
  return true;
}

void log_info(const std::string& msg) { log_line("info", msg); }
void log_warn(const std::string& msg) { log_line("warn", msg); }
void log_error(const std::string& msg) { log_line("error", msg); }

std::string human_size(uint64_t bytes) {
  static const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
  double v = static_cast<double>(bytes);
  int u = 0;
  while (v >= 1024.0 && u < 5) {
    v /= 1024.0;
    ++u;
  }
  char buf[64];
  if (u == 0) {
    std::snprintf(buf, sizeof(buf), "%.0f %s", v, units[u]);
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
  }
  return buf;
}

}  // namespace fsrv

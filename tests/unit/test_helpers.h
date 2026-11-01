#pragma once

// Shared helpers for the unit tests: throw-away directories, deterministic
// payloads and an exception -> ApiError adapter.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "error.h"
#include "test_framework.h"

namespace testutil {

namespace fs = std::filesystem;

inline std::string unique_name(std::string_view prefix) {
  static std::atomic<unsigned> counter{0};
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  std::random_device rd;
  return std::string(prefix) + "-" + std::to_string(ticks) + "-" +
         std::to_string(rd() % 1000000u) + "-" +
         std::to_string(counter.fetch_add(1));
}

// Creates a fresh directory under the system temp path and removes it again on
// destruction.
class TempDir {
 public:
  explicit TempDir(std::string_view prefix = "fsrv-test") {
    path_ = fs::temp_directory_path() / unique_name(prefix);
    fs::create_directories(path_);
  }

  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const fs::path& path() const { return path_; }

  fs::path operator/(std::string_view child) const {
    return path_ / std::string(child);
  }

 private:
  fs::path path_;
};

inline void write_bytes(const fs::path& p, std::string_view data) {
  if (p.has_parent_path()) fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

inline std::string read_bytes(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

// Deterministic, non-repeating-within-256 payload.
inline std::string pattern_bytes(size_t n) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i) {
    s[i] = static_cast<char>((i * 31 + 7) & 0xFF);
  }
  return s;
}

// Runs `fn` and returns the ApiError it threw. When nothing is thrown the
// returned error has status 0; a non-ApiError exception yields status -1.
inline fsrv::ApiError catch_error(const std::function<void()>& fn) {
  try {
    fn();
  } catch (const fsrv::ApiError& e) {
    return e;
  } catch (const std::exception& e) {
    return fsrv::ApiError(-1, "OTHER_EXCEPTION", e.what());
  } catch (...) {
    return fsrv::ApiError(-2, "UNKNOWN_EXCEPTION", "unknown exception");
  }
  return fsrv::ApiError(0, "NO_EXCEPTION", "no exception was thrown");
}

inline int catch_status(const std::function<void()>& fn) {
  return catch_error(fn).status;
}

}  // namespace testutil

// Verifies that `expr` throws an ApiError with the given HTTP status.
#define CHECK_STATUS(expr, expected_status)                                   \
  do {                                                                        \
    const int actual_status__ =                                               \
        ::testutil::catch_status([&] { (void)(expr); });                      \
    ++::testing::check_count();                                               \
    if (actual_status__ != (expected_status)) {                               \
      ::testing::record_failure(                                              \
          __FILE__, __LINE__,                                                 \
          std::string("CHECK_STATUS(" #expr ") -> status ") +                 \
              std::to_string(actual_status__) + " vs " +                      \
              std::to_string(expected_status));                               \
    }                                                                         \
  } while (0)

// Verifies that `expr` throws an ApiError with the given status *and* code.
#define CHECK_ERROR(expr, expected_status, expected_code)                     \
  do {                                                                        \
    const ::fsrv::ApiError actual_error__ =                                   \
        ::testutil::catch_error([&] { (void)(expr); });                       \
    ++::testing::check_count();                                               \
    if (actual_error__.status != (expected_status) ||                         \
        actual_error__.code != std::string(expected_code)) {                  \
      ::testing::record_failure(                                              \
          __FILE__, __LINE__,                                                 \
          std::string("CHECK_ERROR(" #expr ") -> ") +                         \
              std::to_string(actual_error__.status) + "/" +                   \
              actual_error__.code + " vs " + std::to_string(expected_status) + \
              "/" + expected_code);                                           \
    }                                                                         \
  } while (0)

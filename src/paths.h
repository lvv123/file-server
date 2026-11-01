#pragma once

#include <string>
#include <string_view>

#include "compat.h"

namespace fsrv {

// PathGuard is the only sanctioned way to turn a user supplied path into a
// filesystem path. It normalises the input, rejects dangerous components and
// then verifies that the result still lives under the configured root.
class PathGuard {
 public:
  // `root` must already exist and be a directory.
  static PathGuard make(const fs::path& root);

  const fs::path& root() const { return root_; }

  // Returns the normalised *relative* UTF-8 path ("" denotes the root itself).
  // Throws ApiError(400) for malformed input and ApiError(403) for anything
  // that tries to escape the root.
  std::string normalize(std::string_view user_path) const;

  // Normalised relative path -> absolute path, with a symlink aware
  // containment check.
  fs::path resolve(std::string_view user_path) const;

  // Absolute path -> relative UTF-8 path ("" for the root).
  std::string to_rel(const fs::path& abs) const;

  // True when `child` is `parent` or lives below it (case-insensitive on
  // Windows).
  static bool is_within(const fs::path& child, const fs::path& parent);

 private:
  fs::path root_;
  fs::path root_canon_;
};

// True when the component is a Windows reserved device name (CON, PRN, ...).
bool is_windows_reserved_name(std::string_view name);

}  // namespace fsrv

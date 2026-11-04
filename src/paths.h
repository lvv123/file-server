#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "compat.h"

namespace fsrv {

// PathGuard is the only sanctioned way to turn a user supplied path into a
// filesystem path. It normalises the input, rejects dangerous components and
// then verifies that the result still lives under the configured root.
class PathGuard {
 public:
  // `root` must already exist and be a directory. `reserved` lists paths (for
  // example the metadata directory) that must never be exposed through the
  // file API, even via a symlink or a differently-cased path.
  static PathGuard make(const fs::path& root,
                        std::vector<fs::path> reserved = {});

  const fs::path& root() const { return root_; }

  // Returns the normalised *relative* UTF-8 path ("" denotes the root itself).
  // Throws ApiError(400) for malformed input and ApiError(403) for anything
  // that tries to escape the root or reach a reserved path.
  std::string normalize(std::string_view user_path) const;

  // Normalised relative path -> absolute path, with a symlink aware
  // containment check.
  fs::path resolve(std::string_view user_path) const;

  // Like resolve(), but the input is already normalised (used for paths stored
  // in the database, which must not be percent-decoded a second time).
  fs::path resolve_normalized(std::string_view rel_path) const;

  // Absolute path -> relative UTF-8 path ("" for the root).
  std::string to_rel(const fs::path& abs) const;

  // True when `child` is `parent` or lives below it (case-insensitive on
  // Windows).
  static bool is_within(const fs::path& child, const fs::path& parent);

 private:
  void reject_reserved_rel(std::string_view rel) const;
  void reject_reserved_abs(const fs::path& abs) const;

  fs::path root_;
  fs::path root_canon_;
  std::vector<std::string> reserved_rel_;
  std::vector<fs::path> reserved_canon_;
};

// True when the component is a Windows reserved device name (CON, PRN, ...).
bool is_windows_reserved_name(std::string_view name);

}  // namespace fsrv

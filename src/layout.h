#pragma once

#include <string>
#include <string_view>

#include "compat.h"

namespace fsrv {

// Directory that keeps the database, upload staging area and configuration.
// It lives directly inside the served root and is never exposed through the
// file API.
inline constexpr const char* kMetaDirName = ".file-server";

struct RuntimeLayout {
  fs::path serve_root;
  fs::path meta_root;
  fs::path tmp_root;
  fs::path config_path;
  fs::path db_path;
};

// Resolves the served directory. An empty `dir` means the current working
// directory. The returned path is absolute, normalised and must already exist.
fs::path resolve_serve_root(const std::string& dir);

// Builds the derived paths for a served root. `serve_root` must be absolute.
RuntimeLayout make_layout(const fs::path& serve_root);

// True when `rel` is the metadata directory itself or a path below it.
bool is_reserved_rel_path(std::string_view rel);

// Checks that an existing metadata directory only contains entries this
// program owns. A user directory that merely happens to be called
// ".file-server" is rejected instead of being silently taken over.
void validate_meta_dir_if_exists(const fs::path& meta_root);

// Creates <root>/.file-server and its tmp subdirectory, then verifies that the
// metadata directory is a real directory inside the served root (not a symlink
// or junction pointing somewhere else).
void ensure_layout_directories(const RuntimeLayout& layout);

}  // namespace fsrv

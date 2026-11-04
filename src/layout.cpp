#include "layout.h"

#include <string>
#include <system_error>

#include "config.h"
#include "error.h"
#include "paths.h"
#include "util.h"

namespace fsrv {

namespace {

bool names_equal(std::string_view a, std::string_view b) {
#if defined(_WIN32)
  return ascii_iequals(a, b);
#else
  return a == b;
#endif
}

bool is_meta_name(std::string_view name) {
  return names_equal(name, kMetaDirName);
}

bool is_allowed_meta_entry(std::string_view name) {
  static constexpr const char* kAllowed[] = {
      "config.json",    "config.json.tmp", "meta.db",
      "meta.db-wal",    "meta.db-shm",     "meta.db-journal",
      "tmp",            "server.lock",
  };
  for (const char* allowed : kAllowed) {
    if (names_equal(name, allowed)) return true;
  }
  return false;
}

std::string first_segment(std::string_view rel) {
  const size_t slash = rel.find('/');
  return std::string(rel.substr(0, slash));
}

}  // namespace

fs::path resolve_serve_root(const std::string& dir) {
  std::error_code ec;
  fs::path p;
  if (dir.empty()) {
    p = fs::current_path(ec);
    if (ec) {
      throw ConfigError("cannot determine the current directory: " + ec.message());
    }
  } else {
    p = fs::u8path(dir);
    if (p.is_relative()) {
      p = fs::absolute(p, ec);
      if (ec) {
        throw ConfigError("cannot resolve the served directory '" + dir +
                          "': " + ec.message());
      }
    }
  }
  p = p.lexically_normal();

  if (!fs::exists(p, ec) || !fs::is_directory(p, ec)) {
    throw ConfigError("served directory does not exist or is not a directory: " +
                      p.u8string());
  }
  fs::path name = p.filename();
  if (name.empty()) name = p.parent_path().filename();
  if (is_meta_name(name.u8string())) {
    throw ConfigError(
        "the served directory cannot itself be named '" +
        std::string(kMetaDirName) + "'");
  }
  return p;
}

RuntimeLayout make_layout(const fs::path& serve_root) {
  fs::path root = serve_root;
  if (root.is_relative()) root = fs::absolute(root);
  root = root.lexically_normal();
  fs::path name = root.filename();
  if (name.empty()) name = root.parent_path().filename();
  if (is_meta_name(name.u8string())) {
    throw ConfigError(
        "the served directory cannot itself be named '" +
        std::string(kMetaDirName) + "'");
  }

  RuntimeLayout layout;
  layout.serve_root = std::move(root);
  layout.meta_root = layout.serve_root / fs::u8path(kMetaDirName);
  layout.tmp_root = layout.meta_root / "tmp";
  layout.config_path = layout.meta_root / "config.json";
  layout.db_path = layout.meta_root / "meta.db";
  return layout;
}

bool is_reserved_rel_path(std::string_view rel) {
  if (rel.empty()) return false;
  const std::string first = first_segment(rel);
  if (!names_equal(first, kMetaDirName)) return false;
  return rel.size() == first.size() || rel[first.size()] == '/';
}

void validate_meta_dir_if_exists(const fs::path& meta_root) {
  std::error_code ec;
  const fs::file_status st = fs::symlink_status(meta_root, ec);
  // On Windows symlink_status() reports a missing entry both as not_found and
  // as a system error; treat that combination as "does not exist yet".
  if (st.type() == fs::file_type::not_found) return;
  if (ec) {
    throw ConfigError("cannot inspect metadata path '" + meta_root.u8string() +
                      "': " + ec.message());
  }

  if (fs::is_symlink(st)) {
    throw ConfigError("metadata path is a symbolic link or junction, refusing to "
                      "use it: " +
                      meta_root.u8string());
  }
  if (!fs::is_directory(st)) {
    throw ConfigError("metadata path exists but is not a directory: " +
                      meta_root.u8string());
  }

  fs::directory_iterator it(meta_root, ec);
  const fs::directory_iterator end;
  for (; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().u8string();
    if (!is_allowed_meta_entry(name)) {
      throw ConfigError(
          "metadata directory contains an entry this program does not own: " +
          it->path().u8string() +
          " (move that data away or choose another served directory)");
    }
  }
  if (ec) {
    throw ConfigError("cannot inspect metadata directory '" +
                      meta_root.u8string() + "': " + ec.message());
  }
}

void ensure_layout_directories(const RuntimeLayout& layout) {
  validate_meta_dir_if_exists(layout.meta_root);

  std::error_code ec;
  fs::create_directories(layout.meta_root, ec);
  if (ec) {
    throw internal_error("cannot create metadata directory '" +
                             layout.meta_root.u8string() + "': " + ec.message(),
                         "DATA_ROOT_UNUSABLE");
  }
  fs::create_directories(layout.tmp_root, ec);
  if (ec) {
    throw internal_error("cannot create temporary directory '" +
                             layout.tmp_root.u8string() + "': " + ec.message(),
                         "DATA_ROOT_UNUSABLE");
  }

  // create_directories() happily follows a symlink. Re-check the final entries
  // so a ".file-server -> D:\\elsewhere" link cannot put the database or the
  // temporary files on another volume.
  for (const fs::path& p : {layout.meta_root, layout.tmp_root}) {
    const fs::file_status st = fs::symlink_status(p, ec);
    if (ec || fs::is_symlink(st) || !fs::is_directory(st)) {
      throw ConfigError("metadata path is not a real directory: " + p.u8string());
    }
  }

  fs::path root_canon = fs::weakly_canonical(layout.serve_root, ec);
  if (ec) root_canon = layout.serve_root;
  fs::path meta_canon = fs::weakly_canonical(layout.meta_root, ec);
  if (ec) meta_canon = layout.meta_root;
  if (!PathGuard::is_within(meta_canon, root_canon)) {
    throw ConfigError("metadata directory resolves outside the served root: " +
                      layout.meta_root.u8string());
  }
}

}  // namespace fsrv
